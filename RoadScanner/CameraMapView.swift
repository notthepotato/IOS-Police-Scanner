import CoreLocation
import MapKit
import SwiftUI

/// Map of the downloaded cameras around you. Tap a camera for details.
struct CameraMapView: View {
    @EnvironmentObject private var model: ScannerModel

    var body: some View {
        CameraMap(model: model, store: model.cameras)
    }
}

private struct CameraMap: View {
    @ObservedObject var model: ScannerModel
    @ObservedObject var store: CameraStore

    @State private var position: MapCameraPosition = .userLocation(fallback: .automatic)
    @State private var region: MKCoordinateRegion?
    @State private var mapHeading: Double = 0
    @State private var selectedID: Int64?

    /// Drawing thousands of pins makes the map sluggish, so show the nearest ones to the view.
    private let maxPins = 300

    var body: some View {
        let shown = visibleCameras
        Map(position: $position) {
            UserAnnotation()

            if model.isRunning, let here = model.location?.coordinate {
                MapCircle(center: here, radius: model.alertRadius)
                    .foregroundStyle(Color.blue.opacity(0.12))
                    .stroke(Color.blue.opacity(0.6), lineWidth: 1)
            }

            ForEach(shown.cameras) { camera in
                Annotation(camera.displayName, coordinate: camera.coordinate, anchor: .center) {
                    Button {
                        selectedID = camera.id
                    } label: {
                        CameraPin(camera: camera, mapHeading: mapHeading, isSelected: camera.id == selectedID)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(camera.displayName)
                }
                .annotationTitles(.hidden)
            }
        }
        .mapStyle(.standard(pointsOfInterest: .excludingAll))
        .mapControls {
            MapUserLocationButton()
            MapCompass()
            MapScaleView()
        }
        .onMapCameraChange(frequency: .onEnd) { context in
            region = context.region
            mapHeading = context.camera.heading
        }
        .safeAreaInset(edge: .top) {
            statusBar(truncated: shown.truncated)
        }
        .safeAreaInset(edge: .bottom) {
            if let camera = selectedCamera {
                CameraCard(camera: camera, from: model.lastKnownLocation) {
                    selectedID = nil
                }
            }
        }
        .onAppear { model.requestLocationPermissionIfNeeded() }
    }

    // MARK: - Which cameras to draw

    private var visibleCameras: (cameras: [Camera], truncated: Bool) {
        let all = store.cameras
        var inView = all
        var center = model.lastKnownLocation?.coordinate

        if let region {
            center = region.center
            // Half the visible span plus a margin, so pins don't pop in at the edges.
            let latLimit = region.span.latitudeDelta * 0.6
            let lonLimit = region.span.longitudeDelta * 0.6
            inView = all.filter {
                abs($0.lat - region.center.latitude) <= latLimit && abs($0.lon - region.center.longitude) <= lonLimit
            }
        }

        guard inView.count > maxPins else { return (inView, false) }
        guard let center else { return (Array(inView.prefix(maxPins)), true) }
        let middle = CLLocation(latitude: center.latitude, longitude: center.longitude)
        let ranked: [(Camera, CLLocationDistance)] = inView.map { camera in
            (camera, middle.distance(from: CLLocation(latitude: camera.lat, longitude: camera.lon)))
        }
        var nearest: [Camera] = ranked.sorted { $0.1 < $1.1 }.prefix(maxPins).map { $0.0 }
        if let selected = selectedCamera, !nearest.contains(selected) {
            nearest.append(selected)  // keep the tapped camera on screen
        }
        return (nearest, true)
    }

    private var selectedCamera: Camera? {
        guard let selectedID else { return nil }
        return store.cameras.first { $0.id == selectedID }
    }

    // MARK: - Top bar

    /// "Search here" loads cameras for wherever you've scrolled the map. While scanning, the
    /// list follows your GPS automatically, so the button is hidden then.
    private var searchCenter: CLLocationCoordinate2D? {
        region?.center ?? model.lastKnownLocation?.coordinate
    }

    private var canSearchHere: Bool {
        guard !model.isRunning, !store.isDownloading, let center = searchCenter else { return false }
        return store.cameras.isEmpty || store.needsRefresh(at: center)
    }

    private func statusBar(truncated: Bool) -> some View {
        HStack(spacing: 12) {
            if store.isDownloading {
                ProgressView()
            }
            VStack(alignment: .leading, spacing: 4) {
                Text(store.status)
                    .font(.subheadline.weight(.semibold))
                    .lineLimit(2)
                HStack(spacing: 14) {
                    LegendItem(color: .orange, text: "Plate reader")
                    LegendItem(color: .red, text: "Speed")
                    if truncated {
                        Text("Nearest \(maxPins) shown")
                            .foregroundStyle(.secondary)
                    }
                }
                .font(.caption)
            }
            Spacer(minLength: 0)
            if canSearchHere, let center = searchCenter {
                Button("Search here") {
                    Task { await store.refresh(at: center) }
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.small)
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
        .padding(.horizontal)
        .padding(.top, 4)
    }
}

// MARK: - Pieces

private extension Camera {
    var tint: Color { isSpeedCamera ? .red : .orange }
    var symbol: String { isSpeedCamera ? "speedometer" : "camera.fill" }
}

private struct LegendItem: View {
    let color: Color
    let text: String

    var body: some View {
        HStack(spacing: 4) {
            Circle().fill(color).frame(width: 8, height: 8)
            Text(text)
        }
    }
}

/// Round camera icon, with a shaded wedge for each direction the camera faces (when mapped).
private struct CameraPin: View {
    let camera: Camera
    let mapHeading: Double
    let isSelected: Bool

    var body: some View {
        let size: CGFloat = isSelected ? 30 : 24
        ZStack {
            ForEach(Array((camera.directions ?? []).enumerated()), id: \.offset) { _, direction in
                ViewCone()
                    .fill(camera.tint.opacity(0.35))
                    .frame(width: 64, height: 64)
                    // Pins don't rotate with the map, so subtract the map's own rotation.
                    .rotationEffect(.degrees(direction - mapHeading))
            }
            Circle()
                .fill(camera.tint)
                .frame(width: size, height: size)
                .overlay(
                    Image(systemName: camera.symbol)
                        .font(.system(size: size * 0.45, weight: .bold))
                        .foregroundStyle(.white)
                )
                .overlay(Circle().stroke(Color.white, lineWidth: isSelected ? 3 : 2))
                .shadow(color: .black.opacity(0.3), radius: 2, y: 1)
        }
        .frame(width: 64, height: 64)
        .contentShape(Circle().inset(by: 18))  // only the icon is tappable, not the wedge
    }
}

/// A 60° wedge pointing straight up from the center; rotated to the camera's direction.
private struct ViewCone: Shape {
    func path(in rect: CGRect) -> Path {
        let center = CGPoint(x: rect.midX, y: rect.midY)
        let radius = min(rect.width, rect.height) / 2
        var points = [center]
        let steps = 12
        for i in 0...steps {
            // -90° is straight up on screen; sweep from -120° to -60°.
            let degrees = -120.0 + 60.0 * Double(i) / Double(steps)
            let radians = degrees * Double.pi / 180
            points.append(CGPoint(x: center.x + radius * CGFloat(cos(radians)),
                                  y: center.y + radius * CGFloat(sin(radians))))
        }
        var path = Path()
        path.addLines(points)
        path.closeSubpath()
        return path
    }
}

/// Details for the tapped camera.
private struct CameraCard: View {
    let camera: Camera
    let from: CLLocation?
    let onClose: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 10) {
                Image(systemName: camera.symbol)
                    .foregroundStyle(camera.tint)
                Text(camera.isSpeedCamera ? "Speed camera" : "License plate reader")
                    .font(.headline)
                Spacer()
                Button(action: onClose) {
                    Image(systemName: "xmark.circle.fill")
                        .font(.title2)
                        .foregroundStyle(.secondary)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Close")
            }
            if !camera.label.isEmpty {
                Text(camera.label)
                    .foregroundStyle(.secondary)
            }
            if let from {
                let distance = Geo.distance(from.coordinate, camera.coordinate)
                let bearing = Geo.bearing(from: from.coordinate, to: camera.coordinate)
                Label("\(Units.short(distance)) \(Geo.describe(bearing: bearing, relative: nil)) of you",
                      systemImage: "location")
            }
            if let directions = camera.directions {
                Label("Faces " + directions.map { Geo.compass($0) }.joined(separator: " and "),
                      systemImage: "arrow.up.forward")
            }
            if let url = URL(string: "https://www.openstreetmap.org/node/\(camera.id)") {
                Link(destination: url) {
                    Label("View on OpenStreetMap", systemImage: "arrow.up.right.square")
                }
            }
        }
        .font(.subheadline)
        .padding()
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
        .padding(.horizontal)
        .padding(.bottom, 8)
    }
}
