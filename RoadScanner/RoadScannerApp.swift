import SwiftUI

@main
struct RoadScannerApp: App {
    @StateObject private var model = ScannerModel()

    var body: some Scene {
        WindowGroup {
            TabView {
                ContentView()
                    .tabItem { Label("Scanner", systemImage: "dot.radiowaves.left.and.right") }
                CameraMapView()
                    .tabItem { Label("Map", systemImage: "map") }
            }
            .environmentObject(model)
        }
    }
}
