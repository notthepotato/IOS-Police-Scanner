import CoreLocation
import Foundation

/// Distance, bearing and direction helpers.
enum Geo {
    static func distance(_ a: CLLocationCoordinate2D, _ b: CLLocationCoordinate2D) -> CLLocationDistance {
        CLLocation(latitude: a.latitude, longitude: a.longitude)
            .distance(from: CLLocation(latitude: b.latitude, longitude: b.longitude))
    }

    /// Compass bearing from `a` to `b`, 0...360 degrees.
    static func bearing(from a: CLLocationCoordinate2D, to b: CLLocationCoordinate2D) -> Double {
        let p1 = a.latitude * .pi / 180
        let p2 = b.latitude * .pi / 180
        let dl = (b.longitude - a.longitude) * .pi / 180
        let x = sin(dl) * cos(p2)
        let y = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl)
        let deg = atan2(x, y) * 180 / .pi
        return (deg + 360).truncatingRemainder(dividingBy: 360)
    }

    static func compass(_ degrees: Double) -> String {
        let names = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"]
        let index = Int((degrees + 22.5) / 45) % 8
        return names[index]
    }

    /// Angle of a target relative to your heading: 0 = dead ahead, + = right, - = left.
    static func relativeAngle(bearing: Double, heading: Double) -> Double {
        (bearing - heading + 540).truncatingRemainder(dividingBy: 360) - 180
    }

    /// "ahead", "on your right", "behind you"... or nil when there is no heading.
    static func sideText(relative: Double?) -> String? {
        guard let rel = relative else { return nil }
        if abs(rel) <= 45 { return "ahead" }
        if abs(rel) >= 135 { return "behind you" }
        return rel > 0 ? "on your right" : "on your left"
    }

    static func describe(bearing: Double, relative: Double?) -> String {
        var text = "to the \(compass(bearing))"
        if let side = sideText(relative: relative) { text += ", \(side)" }
        return text
    }

    /// Rough range from Bluetooth signal strength.
    static func rssiLabel(_ rssi: Int) -> String {
        if rssi >= -60 { return "very close" }
        if rssi >= -75 { return "close" }
        if rssi >= -88 { return "nearby" }
        return "in range"
    }
}

struct RadiusOption: Hashable {
    let meters: Double
    let label: String
}

/// Feet/miles for US phones, meters/km everywhere else.
enum Units {
    static var usesMetric: Bool { Locale.current.measurementSystem == .metric }

    static var radiusOptions: [RadiusOption] {
        usesMetric
            ? [RadiusOption(meters: 200, label: "200 m"),
               RadiusOption(meters: 400, label: "400 m"),
               RadiusOption(meters: 800, label: "800 m")]
            : [RadiusOption(meters: 160.9, label: "0.1 mi"),
               RadiusOption(meters: 402.3, label: "¼ mi"),
               RadiusOption(meters: 804.7, label: "½ mi")]
    }

    static var defaultRadius: Double { radiusOptions[1].meters }

    /// Short label for the screen, e.g. "650 ft", "1.2 mi", "400 m".
    static func short(_ meters: Double) -> String {
        if usesMetric {
            if meters >= 1000 { return String(format: "%.1f km", meters / 1000) }
            return "\(Int((meters / 10).rounded() * 10)) m"
        }
        let feet = meters * 3.28084
        if feet >= 1000 { return String(format: "%.1f mi", meters / 1609.344) }
        return "\(Int((feet / 10).rounded() * 10)) ft"
    }

    /// Words for the voice, e.g. "600 feet", "half a mile", "350 meters".
    static func spoken(_ meters: Double) -> String {
        if usesMetric {
            if meters >= 1000 { return String(format: "%.1f kilometers", meters / 1000) }
            return "\(Int(max(50, (meters / 50).rounded() * 50))) meters"
        }
        let feet = meters * 3.28084
        if feet >= 1200 {
            let miles = meters / 1609.344
            if abs(miles - 0.25) < 0.07 { return "a quarter mile" }
            if abs(miles - 0.5) < 0.1 { return "half a mile" }
            return String(format: "%.1f miles", miles)
        }
        return "\(Int(max(100, (feet / 100).rounded() * 100))) feet"
    }
}
