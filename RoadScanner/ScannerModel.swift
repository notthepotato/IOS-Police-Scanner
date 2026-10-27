import CoreLocation
import Foundation

struct AlertEvent: Identifiable {
    let id = UUID()
    let date: Date
    let text: String
    let isPolice: Bool
}

struct NearbyCamera: Identifiable {
    let camera: Camera
    let distance: Double
    let direction: String
    var id: Int64 { camera.id }
}

/// Ties GPS, the camera list, the ESP32 sensor and the voice together.
@MainActor
final class ScannerModel: NSObject, ObservableObject {
    @Published private(set) var isRunning = false
    @Published private(set) var authorization: CLAuthorizationStatus = .notDetermined
    @Published private(set) var location: CLLocation?
    @Published private(set) var nearby: [NearbyCamera] = []
    @Published private(set) var events: [AlertEvent] = []

    @Published var alertRadius: Double {
        didSet { UserDefaults.standard.set(alertRadius, forKey: "alertRadius") }
    }
    @Published var voiceOn: Bool {
        didSet {
            UserDefaults.standard.set(voiceOn, forKey: "voiceOn")
            speaker.enabled = voiceOn
        }
    }
    @Published var sensorOn: Bool {
        didSet {
            UserDefaults.standard.set(sensorOn, forKey: "sensorOn")
            updateSensor()
        }
    }

    let cameras = CameraStore()
    let sensor = SensorLink()
    private let speaker = Speaker()
    private let manager = CLLocationManager()
    private var alerted = Set<Int64>()       // cameras you're currently inside the alert radius of
    private var policeLastSeen: [String: Date] = [:]

    override init() {
        let defaults = UserDefaults.standard
        let savedRadius = defaults.object(forKey: "alertRadius") as? Double
        if let savedRadius, Units.radiusOptions.contains(where: { $0.meters == savedRadius }) {
            alertRadius = savedRadius
        } else {
            alertRadius = Units.defaultRadius
        }
        voiceOn = defaults.object(forKey: "voiceOn") as? Bool ?? true
        sensorOn = defaults.object(forKey: "sensorOn") as? Bool ?? true
        super.init()

        authorization = manager.authorizationStatus
        manager.delegate = self
        manager.activityType = .automotiveNavigation
        manager.desiredAccuracy = kCLLocationAccuracyBest
        manager.distanceFilter = 5
        manager.pausesLocationUpdatesAutomatically = false
        speaker.enabled = voiceOn
        sensor.onHit = { [weak self] hit in self?.handlePolice(hit) }
        // GPS only reports when you move, so recalculate as soon as new cameras arrive;
        // otherwise "Nearest cameras" stays empty while you're parked.
        cameras.onChange = { [weak self] in self?.recheckCameras() }
    }

    private func recheckCameras() {
        if let location { handle(location) }
    }

    // MARK: - Start / stop

    func start() {
        isRunning = true
        switch manager.authorizationStatus {
        case .notDetermined:
            manager.requestWhenInUseAuthorization()  // continues in locationManagerDidChangeAuthorization
        case .authorizedWhenInUse, .authorizedAlways:
            beginLocationUpdates()
        default:
            addEvent("Location is off for Road Scanner. Turn it on in Settings > Privacy & Security > Location Services.",
                     police: false)
            isRunning = false
        }
        updateSensor()
    }

    func stop() {
        isRunning = false
        manager.stopUpdatingLocation()
        manager.allowsBackgroundLocationUpdates = false
        alerted.removeAll()
        updateSensor()
    }

    /// Best available location: live GPS while scanning, otherwise the last fix iOS remembers.
    var lastKnownLocation: CLLocation? { location ?? manager.location }

    /// Lets the map show your position even before you start scanning.
    func requestLocationPermissionIfNeeded() {
        if manager.authorizationStatus == .notDetermined {
            manager.requestWhenInUseAuthorization()
        }
    }

    func testVoice() {
        speaker.say("Road Scanner test. If you hear this through the car, alerts are working.", force: true)
    }

    private func beginLocationUpdates() {
        // Keeps running with the screen off or another app open (blue location pill),
        // as long as you don't swipe Road Scanner away.
        manager.allowsBackgroundLocationUpdates = true
        manager.showsBackgroundLocationIndicator = true
        manager.startUpdatingLocation()
    }

    private func updateSensor() {
        if isRunning && sensorOn { sensor.start() } else { sensor.stop() }
    }

    // MARK: - Cameras

    private func handle(_ fix: CLLocation) {
        guard fix.horizontalAccuracy >= 0, fix.horizontalAccuracy <= 100 else { return }
        location = fix
        cameras.refreshIfNeeded(at: fix.coordinate)

        let here = fix.coordinate
        let heading: Double? = (fix.course >= 0 && fix.speed > 3) ? fix.course : nil
        let resetDistance = alertRadius * 1.5  // hysteresis so GPS jitter doesn't re-trigger

        var distances: [(camera: Camera, distance: Double)] = []
        distances.reserveCapacity(cameras.cameras.count)

        for camera in cameras.cameras {
            let d = Geo.distance(here, camera.coordinate)
            distances.append((camera: camera, distance: d))

            if d <= alertRadius {
                guard !alerted.contains(camera.id) else { continue }
                let bearing = Geo.bearing(from: here, to: camera.coordinate)
                let relative = heading.map { Geo.relativeAngle(bearing: bearing, heading: $0) }
                // Already behind you when you got close: stay quiet, but alert if you turn back toward it.
                if let relative, abs(relative) > 120 { continue }
                alerted.insert(camera.id)
                announceCamera(camera, distance: d, relative: relative)
            } else if d > resetDistance {
                alerted.remove(camera.id)
            }
        }

        distances.sort { $0.distance < $1.distance }
        nearby = distances.prefix(5).map { item -> NearbyCamera in
            let bearing = Geo.bearing(from: here, to: item.camera.coordinate)
            let relative = heading.map { Geo.relativeAngle(bearing: bearing, heading: $0) }
            return NearbyCamera(camera: item.camera, distance: item.distance,
                                direction: Geo.describe(bearing: bearing, relative: relative))
        }
    }

    private func announceCamera(_ camera: Camera, distance: Double, relative: Double?) {
        let side = Geo.sideText(relative: relative) ?? "nearby"
        speaker.say("\(camera.spokenName) \(side), \(Units.spoken(distance))")
        addEvent("\(camera.displayName), \(Units.short(distance)) \(side)", police: false)
    }

    // MARK: - Police gear (from the ESP32)

    private func handlePolice(_ hit: PoliceHit) {
        let now = Date()
        let previous = policeLastSeen[hit.address]
        policeLastSeen[hit.address] = now
        if let previous, now.timeIntervalSince(previous) < 120 { return }

        let range = Geo.rssiLabel(hit.rssi)
        speaker.say("Police equipment \(range)")
        let name = hit.name.isEmpty ? "" : " \(hit.name)"
        addEvent("Police gear\(name) \(hit.address), \(hit.rssi) dBm (\(range))", police: true)
    }

    private func addEvent(_ text: String, police: Bool) {
        events.insert(AlertEvent(date: Date(), text: text, isPolice: police), at: 0)
        if events.count > 25 { events.removeLast(events.count - 25) }
    }
}

extension ScannerModel: CLLocationManagerDelegate {
    // CLLocationManager calls its delegate on the thread it was created on (main here).

    nonisolated func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        let status = manager.authorizationStatus
        MainActor.assumeIsolated {
            authorization = status
            guard isRunning else { return }
            switch status {
            case .authorizedWhenInUse, .authorizedAlways:
                beginLocationUpdates()
            case .denied, .restricted:
                addEvent("Location permission was denied. Turn it on in Settings to use Road Scanner.", police: false)
                stop()
            default:
                break
            }
        }
    }

    nonisolated func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        guard let latest = locations.last else { return }
        MainActor.assumeIsolated {
            handle(latest)
        }
    }

    nonisolated func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        // Temporary GPS errors are normal (tunnels, garages); Core Location keeps trying.
    }
}
