import CoreLocation
import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var model: ScannerModel

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Button {
                        if model.isRunning { model.stop() } else { model.start() }
                    } label: {
                        Label(model.isRunning ? "Stop scanning" : "Start scanning",
                              systemImage: model.isRunning ? "stop.circle.fill" : "play.circle.fill")
                            .font(.headline)
                            .foregroundStyle(model.isRunning ? Color.red : Color.green)
                    }

                    StatusRow(title: "GPS", value: gpsText, ok: model.isRunning && model.location != nil)
                    CameraStatusRow(store: model.cameras, location: model.location)
                    SensorStatusRow(sensor: model.sensor, enabled: model.sensorOn, running: model.isRunning)
                } footer: {
                    Text("Leave Road Scanner open in the background while you drive (don't swipe it away). "
                         + "Alerts play through CarPlay or your car's Bluetooth like navigation directions.")
                }

                Section("Alerts") {
                    Picker("Warn me within", selection: $model.alertRadius) {
                        ForEach(Units.radiusOptions, id: \.self) { option in
                            Text(option.label).tag(option.meters)
                        }
                    }
                    Toggle("Voice alerts", isOn: $model.voiceOn)
                    Toggle("ESP32 police sensor", isOn: $model.sensorOn)
                    Button("Test voice") { model.testVoice() }
                }

                Section("Nearest cameras") {
                    if model.nearby.isEmpty {
                        Text(model.isRunning ? "None mapped near you yet" : "Start scanning to see cameras near you")
                            .foregroundStyle(.secondary)
                    }
                    ForEach(model.nearby) { item in
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(item.camera.displayName)
                                Text(item.direction)
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                            Spacer()
                            Text(Units.short(item.distance))
                                .monospacedDigit()
                                .foregroundStyle(.secondary)
                        }
                    }
                }

                Section {
                    if model.events.isEmpty {
                        Text("Nothing yet").foregroundStyle(.secondary)
                    }
                    ForEach(model.events) { event in
                        HStack(alignment: .top, spacing: 10) {
                            Image(systemName: event.isPolice ? "exclamationmark.triangle.fill" : "camera.fill")
                                .foregroundStyle(event.isPolice ? Color.red : Color.orange)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(event.text)
                                Text(event.date, style: .time)
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }
                } header: {
                    Text("Recent alerts")
                } footer: {
                    Text(Self.buildText)
                        .padding(.top, 8)
                }
            }
            .navigationTitle("Road Scanner")
        }
    }

    /// "Version 1.0 (build 7, 5c778c1)": the build number is the GitHub Actions run number.
    private static var buildText: String {
        let info = Bundle.main.infoDictionary ?? [:]
        let version = info["CFBundleShortVersionString"] as? String ?? "?"
        let build = info["CFBundleVersion"] as? String ?? "?"
        let commit = info["RSBuildCommit"] as? String ?? ""
        return commit.isEmpty ? "Version \(version) (build \(build))" : "Version \(version) (build \(build), \(commit))"
    }

    private var gpsText: String {
        guard model.isRunning else { return "Off" }
        switch model.authorization {
        case .denied, .restricted: return "Permission off"
        case .notDetermined: return "Waiting for permission"
        default: break
        }
        guard let fix = model.location else { return "Waiting for a fix…" }
        let accuracy = Units.short(fix.horizontalAccuracy)
        guard fix.speed >= 0 else { return "±\(accuracy)" }
        let speed = Units.usesMetric
            ? String(format: "%.0f km/h", fix.speed * 3.6)
            : String(format: "%.0f mph", fix.speed * 2.23694)
        return "\(speed), ±\(accuracy)"
    }
}

private struct StatusRow: View {
    let title: String
    let value: String
    let ok: Bool

    var body: some View {
        HStack {
            Image(systemName: ok ? "checkmark.circle.fill" : "circle.dashed")
                .foregroundStyle(ok ? Color.green : Color.secondary)
            Text(title)
            Spacer()
            Text(value)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.trailing)
        }
    }
}

private struct CameraStatusRow: View {
    @ObservedObject var store: CameraStore
    let location: CLLocation?

    var body: some View {
        HStack {
            Image(systemName: store.cameras.isEmpty ? "circle.dashed" : "checkmark.circle.fill")
                .foregroundStyle(store.cameras.isEmpty ? Color.secondary : Color.green)
            VStack(alignment: .leading, spacing: 2) {
                Text("Cameras")
                Text(store.status)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Button("Refresh") {
                guard let coordinate = location?.coordinate else { return }
                Task { await store.refresh(at: coordinate) }
            }
            .buttonStyle(.bordered)
            .disabled(location == nil || store.isDownloading)
        }
    }
}

private struct SensorStatusRow: View {
    @ObservedObject var sensor: SensorLink
    let enabled: Bool
    let running: Bool

    var body: some View {
        StatusRow(title: "ESP32 sensor", value: text, ok: sensor.status == .connected)
    }

    private var text: String {
        guard enabled else { return "Turned off" }
        guard running else { return "Off" }
        switch sensor.status {
        case .off: return "Off"
        case .unavailable(let reason): return reason
        case .searching: return "Looking for sensor…"
        case .connecting: return "Connecting…"
        case .connected: return "Connected"
        }
    }
}
