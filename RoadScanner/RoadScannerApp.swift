import SwiftUI

@main
struct RoadScannerApp: App {
    @StateObject private var model = ScannerModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(model)
        }
    }
}
