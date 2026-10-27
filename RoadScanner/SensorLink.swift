import CoreBluetooth
import Foundation

/// UUIDs shared with the ESP32 firmware (esp32/RScanSensor/include/config.h). Change both together.
enum SensorUUIDs {
    static let service = CBUUID(string: "6E3A0001-5C1B-4B3D-9E1E-7A2F4C8D0B11")
    /// Board -> phone: "AXON,<mac>,<rssi>,<name>" notifications.
    static let alert = CBUUID(string: "6E3A0002-5C1B-4B3D-9E1E-7A2F4C8D0B11")
    /// Phone -> board: the current time, so the board's SD log has real dates.
    static let time = CBUUID(string: "6E3A0003-5C1B-4B3D-9E1E-7A2F4C8D0B11")
    /// Phone -> board: commands such as "TEST,SPEAKER".
    static let command = CBUUID(string: "6E3A0004-5C1B-4B3D-9E1E-7A2F4C8D0B11")
}

struct PoliceHit {
    let address: String
    let rssi: Int
    let name: String
}

/// Connects to the ESP32 sensor and passes along its Axon detections.
/// iPhones can't read other devices' Bluetooth MAC addresses, so the ESP32 does the
/// actual scanning and sends "AXON,<mac>,<rssi>,<name>" notifications here.
@MainActor
final class SensorLink: NSObject, ObservableObject {
    enum Status: Equatable {
        case off
        case unavailable(String)
        case searching
        case connecting
        case connected
    }

    @Published private(set) var status: Status = .off
    /// True when the connected sensor's firmware supports commands (the speaker test).
    @Published private(set) var canSendCommands = false
    var onHit: (@MainActor (PoliceHit) -> Void)?

    private var central: CBCentralManager?
    private var sensor: CBPeripheral?
    private var commandCharacteristic: CBCharacteristic?
    private var wantsRunning = false

    /// Asks the board to play its test tones (works even when the board is muted).
    func testSpeaker() {
        guard status == .connected, let sensor, let commandCharacteristic else { return }
        sensor.writeValue(Data("TEST,SPEAKER".utf8), for: commandCharacteristic, type: .withResponse)
    }

    private func forgetCharacteristics() {
        commandCharacteristic = nil
        canSendCommands = false
    }

    func start() {
        wantsRunning = true
        if central == nil {
            // queue: nil delivers every delegate callback on the main thread.
            central = CBCentralManager(delegate: self, queue: nil)
        } else {
            connectOrScan()
        }
    }

    func stop() {
        wantsRunning = false
        guard let central else { status = .off; return }
        if central.state == .poweredOn {
            central.stopScan()
            if let sensor { central.cancelPeripheralConnection(sensor) }
        }
        forgetCharacteristics()
        status = .off
    }

    private func connectOrScan() {
        guard wantsRunning, let central, central.state == .poweredOn else { return }
        if let sensor {
            // A pending connect never times out on iOS, so this also reconnects when you
            // get back in range.
            status = .connecting
            central.connect(sensor, options: nil)
        } else {
            status = .searching
            central.scanForPeripherals(withServices: [SensorUUIDs.service], options: nil)
        }
    }

    private func handle(_ data: Data) {
        guard let text = String(data: data, encoding: .utf8) else { return }
        let parts = text.split(separator: ",", omittingEmptySubsequences: false).map(String.init)
        guard parts.count >= 3, parts[0] == "AXON" else { return }
        let hit = PoliceHit(address: parts[1],
                            rssi: Int(parts[2]) ?? -100,
                            name: parts.count > 3 ? parts[3] : "")
        onHit?(hit)
    }
}

extension SensorLink: CBCentralManagerDelegate {
    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        let state = central.state
        MainActor.assumeIsolated {
            switch state {
            case .poweredOn: connectOrScan()
            case .poweredOff: status = .unavailable("Bluetooth is off")
            case .unauthorized: status = .unavailable("Bluetooth permission is off")
            case .unsupported: status = .unavailable("Bluetooth not supported")
            default: break
            }
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                                    advertisementData: [String: Any], rssi RSSI: NSNumber) {
        MainActor.assumeIsolated {
            central.stopScan()
            sensor = peripheral
            peripheral.delegate = self
            status = .connecting
            central.connect(peripheral, options: nil)
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        MainActor.assumeIsolated {
            status = .connected
            peripheral.discoverServices([SensorUUIDs.service])
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral,
                                    error: Error?) {
        MainActor.assumeIsolated {
            sensor = nil
            forgetCharacteristics()
            connectOrScan()
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral,
                                    error: Error?) {
        MainActor.assumeIsolated {
            forgetCharacteristics()
            connectOrScan()
        }
    }
}

extension SensorLink: CBPeripheralDelegate {
    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == SensorUUIDs.service }) else { return }
        peripheral.discoverCharacteristics([SensorUUIDs.alert, SensorUUIDs.time, SensorUUIDs.command], for: service)
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService,
                                error: Error?) {
        let characteristics = service.characteristics ?? []
        if let alert = characteristics.first(where: { $0.uuid == SensorUUIDs.alert }) {
            peripheral.setNotifyValue(true, for: alert)
        }
        // Older firmware has no time characteristic; then the board just logs uptime instead.
        if let time = characteristics.first(where: { $0.uuid == SensorUUIDs.time }) {
            let now = Int(Date().timeIntervalSince1970)
            let offset = TimeZone.current.secondsFromGMT()
            peripheral.writeValue(Data("TIME,\(now),\(offset)".utf8), for: time, type: .withResponse)
        }
        // Older firmware has no command characteristic; the speaker test button stays disabled.
        let command = characteristics.first(where: { $0.uuid == SensorUUIDs.command })
        MainActor.assumeIsolated {
            commandCharacteristic = command
            canSendCommands = command != nil
        }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic,
                                error: Error?) {
        guard error == nil, let data = characteristic.value else { return }
        MainActor.assumeIsolated {
            handle(data)
        }
    }
}
