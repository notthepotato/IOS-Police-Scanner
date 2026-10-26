import CoreLocation
import Foundation

struct Camera: Codable, Identifiable, Hashable {
    let id: Int64
    let lat: Double
    let lon: Double
    let kind: String   // "ALPR camera" or "Speed camera"
    let label: String  // manufacturer/operator if mapped, e.g. "Flock Safety"
    /// Compass directions the camera faces (0 = north), when mappers recorded them.
    /// Optional so camera lists saved by older versions still load.
    var directions: [Double]? = nil

    var coordinate: CLLocationCoordinate2D { CLLocationCoordinate2D(latitude: lat, longitude: lon) }

    var isSpeedCamera: Bool { kind == "Speed camera" }

    var displayName: String { label.isEmpty ? kind : "\(kind) (\(label))" }

    var spokenName: String {
        if kind == "Speed camera" { return "Speed camera" }
        if label.localizedCaseInsensitiveContains("flock") { return "Flock camera" }
        return "Plate reader camera"
    }
}

private struct CameraCacheFile: Codable {
    var centerLat: Double
    var centerLon: Double
    var radius: Double
    var saved: Date
    var cameras: [Camera]
}

enum CameraDownloadError: LocalizedError {
    case badStatus(Int)
    var errorDescription: String? {
        switch self {
        case .badStatus(let code): return "server returned \(code)"
        }
    }
}

/// Downloads mapped speed and ALPR cameras from OpenStreetMap and keeps them on disk,
/// so alerts keep working when you lose signal.
@MainActor
final class CameraStore: ObservableObject {
    @Published private(set) var cameras: [Camera] = []
    @Published private(set) var status = "No cameras downloaded yet"
    @Published private(set) var isDownloading = false

    let downloadRadius: Double = 20_000  // meters
    private var center: CLLocationCoordinate2D?
    private var coveredRadius: Double = 20_000
    private var lastAttempt = Date.distantPast

    private let fileURL: URL = {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("cameras.json")
    }()

    init() {
        load()
    }

    /// True once you've driven about half the download radius away from where cameras were fetched.
    func needsRefresh(at coordinate: CLLocationCoordinate2D) -> Bool {
        guard let center else { return true }
        return Geo.distance(center, coordinate) > coveredRadius * 0.5
    }

    func refreshIfNeeded(at coordinate: CLLocationCoordinate2D) {
        guard needsRefresh(at: coordinate),
              !isDownloading,
              Date().timeIntervalSince(lastAttempt) > 60  // don't hammer the server while offline
        else { return }
        Task { await refresh(at: coordinate) }
    }

    func refresh(at coordinate: CLLocationCoordinate2D) async {
        guard !isDownloading else { return }
        isDownloading = true
        lastAttempt = Date()
        status = "Downloading cameras…"
        defer { isDownloading = false }
        do {
            let found = try await Self.download(around: coordinate, radius: downloadRadius)
            cameras = found
            center = coordinate
            coveredRadius = downloadRadius
            status = "\(found.count) cameras within \(Units.short(downloadRadius))"
            save(savedAt: Date())
        } catch {
            status = "Download failed (\(error.localizedDescription)). Using \(cameras.count) saved cameras."
        }
    }

    // MARK: - OpenStreetMap (Overpass API)

    nonisolated static func download(around c: CLLocationCoordinate2D, radius: Double) async throws -> [Camera] {
        let around = String(format: "around:%.0f,%.6f,%.6f", radius, c.latitude, c.longitude)
        let query = """
        [out:json][timeout:30];
        (
          node["highway"="speed_camera"](\(around));
          node["man_made"="surveillance"]["surveillance:type"="ALPR"](\(around));
        );
        out body;
        """
        var request = URLRequest(url: URL(string: "https://overpass-api.de/api/interpreter")!)
        request.httpMethod = "POST"
        request.timeoutInterval = 45
        request.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        request.setValue("RoadScanner-iOS/1.0 (personal hobby project)", forHTTPHeaderField: "User-Agent")
        let encoded = query.addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? ""
        request.httpBody = Data("data=\(encoded)".utf8)

        let (data, response) = try await URLSession.shared.data(for: request)
        if let http = response as? HTTPURLResponse, http.statusCode != 200 {
            throw CameraDownloadError.badStatus(http.statusCode)
        }
        return try parse(data)
    }

    nonisolated static func parse(_ data: Data) throws -> [Camera] {
        struct Response: Decodable { let elements: [Element] }
        struct Element: Decodable {
            let id: Int64
            let lat: Double?
            let lon: Double?
            let tags: [String: String]?
        }
        let response = try JSONDecoder().decode(Response.self, from: data)
        return response.elements.compactMap { element -> Camera? in
            guard let lat = element.lat, let lon = element.lon else { return nil }
            let tags = element.tags ?? [:]
            let kind = tags["highway"] == "speed_camera" ? "Speed camera" : "ALPR camera"
            let label = tags["manufacturer"] ?? tags["brand"] ?? tags["operator"] ?? ""
            let directions = parseDirections(tags["camera:direction"] ?? tags["direction"])
            return Camera(id: element.id, lat: lat, lon: lon, kind: kind, label: label, directions: directions)
        }
    }

    /// OpenStreetMap direction tags: "135", "SE", "90;270" (two cameras on one pole), or a range like "45-135".
    nonisolated static func parseDirections(_ raw: String?) -> [Double]? {
        guard let raw, !raw.isEmpty else { return nil }
        let cardinals: [String: Double] = [
            "N": 0, "NNE": 22.5, "NE": 45, "ENE": 67.5, "E": 90, "ESE": 112.5, "SE": 135, "SSE": 157.5,
            "S": 180, "SSW": 202.5, "SW": 225, "WSW": 247.5, "W": 270, "WNW": 292.5, "NW": 315, "NNW": 337.5,
        ]
        func normalized(_ degrees: Double) -> Double {
            let d = degrees.truncatingRemainder(dividingBy: 360)
            return d < 0 ? d + 360 : d
        }
        func value(_ token: String) -> Double? {
            if let number = Double(token), number.isFinite { return normalized(number) }
            return cardinals[token]
        }

        var result: [Double] = []
        for part in raw.split(separator: ";") {
            let token = part.trimmingCharacters(in: .whitespaces).uppercased()
            if let single = value(token) {
                result.append(single)
            } else {
                // "45-135" -> the middle of the range, handling wrap-around like "350-10"
                let ends = token.split(separator: "-").map { String($0).trimmingCharacters(in: .whitespaces) }
                if ends.count == 2, let from = value(ends[0]), let to = value(ends[1]) {
                    let span = normalized(to - from)
                    result.append(normalized(from + span / 2))
                }
            }
        }
        return result.isEmpty ? nil : Array(result.prefix(4))
    }

    // MARK: - Disk cache

    private func load() {
        guard let data = try? Data(contentsOf: fileURL),
              let file = try? JSONDecoder().decode(CameraCacheFile.self, from: data)
        else { return }
        cameras = file.cameras
        center = CLLocationCoordinate2D(latitude: file.centerLat, longitude: file.centerLon)
        coveredRadius = file.radius
        status = "\(file.cameras.count) saved cameras"
    }

    private func save(savedAt date: Date) {
        guard let center else { return }
        let file = CameraCacheFile(centerLat: center.latitude, centerLon: center.longitude,
                                   radius: coveredRadius, saved: date, cameras: cameras)
        do {
            try JSONEncoder().encode(file).write(to: fileURL, options: .atomic)
        } catch {
            status += " (couldn't save to disk)"
        }
    }
}
