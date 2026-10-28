import SwiftUI

@main
struct VoiceShiftApp: App {
    @StateObject private var model = AppModel()
    @State private var hotKey: HotKey?

    var body: some Scene {
        Window("VoiceShift", id: "main") {
            ContentView()
                .environmentObject(model)
                .onAppear {
                    if hotKey == nil { hotKey = HotKey { model.toggle() } }
                }
        }
        .windowResizability(.contentSize)

        MenuBarExtra {
            MenuBarContent().environmentObject(model)
        } label: {
            Image(systemName: model.isOn ? "waveform.circle.fill" : "waveform.circle")
        }
    }
}
