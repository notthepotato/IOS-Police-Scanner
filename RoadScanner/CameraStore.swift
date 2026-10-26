import CoreLocation
import Foundation

struct Camera: Codable, Identifiable, Hashable {
    let id: Int64
    let lat: Double
    let lon: Double
    let kind: String   // "ALPR camera" or "Speed camera"
    let label: String  // manufacturer/operator if mapped, e.g. "Flock Safety"

    var coordinate: CLLocationCoordinate2D { CLLocationCoordinate2D(latitude: lat, longitude: lon) }

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
            return Camera(id: element.id, lat: lat, lon: lon, kind: kind, label: label)
        }
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
