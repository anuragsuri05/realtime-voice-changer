import SwiftUI

struct ContentView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        ScrollView {
            VStack(spacing: 14) {
                PowerHeader()
                if model.awaitingMicPermission {
                    Banner(text: "Waiting for microphone permission — click “Allow” in the macOS dialog.",
                           color: .blue, icon: "mic.badge.plus")
                }
                if let err = model.errorMessage { Banner(text: err, color: .red, icon: "exclamationmark.triangle.fill") }
                if let note = model.route?.note { Banner(text: note, color: .orange, icon: "arrow.triangle.branch") }
                MetersCard()
                VoiceCard()
                NoiseCard()
                RoutingCard()
                BluetoothCard()
                Text("Shortcut: ⌥⌘V toggles the voice changer from any app.")
                    .font(.footnote).foregroundStyle(.secondary)
            }
            .padding(16)
        }
        .frame(minWidth: 460, idealWidth: 480, minHeight: 600, idealHeight: 820)
    }
}

// MARK: - Header

struct PowerHeader: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        HStack(spacing: 16) {
            Button { model.toggle() } label: {
                ZStack {
                    Circle()
                        .fill(model.isOn ? Color.pink.gradient : Color.gray.opacity(0.25).gradient)
                        .frame(width: 72, height: 72)
                        .shadow(color: model.isOn ? .pink.opacity(0.5) : .clear, radius: 10)
                    Image(systemName: "power").font(.system(size: 30, weight: .bold))
                        .foregroundStyle(model.isOn ? .white : .secondary)
                }
            }
            .buttonStyle(.plain)
            .keyboardShortcut(.space, modifiers: [])
            .help("Turn the voice changer on or off (Space, or ⌥⌘V from anywhere)")

            VStack(alignment: .leading, spacing: 4) {
                Text(model.isOn ? "Voice changer ON" : "Voice changer OFF")
                    .font(.title2.weight(.semibold))
                Text(subtitle).font(.callout).foregroundStyle(.secondary).lineLimit(2)
                if model.running {
                    Button("Stop audio", role: .destructive) { model.stopAudio() }
                        .controlSize(.small)
                }
            }
            Spacer()
        }
        .padding(14)
        .background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: 14))
    }

    private var subtitle: String {
        if model.awaitingMicPermission { return "Waiting for microphone permission…" }
        guard model.running, let r = model.route else { return "Press the button to start." }
        return (model.isOn ? "Female voice" : "Natural voice (pass-through)") + " → " + r.summary
    }
}

// MARK: - Meters

struct MetersCard: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        Card(title: "Live", icon: "waveform") {
            LevelBar(label: "Mic", level: model.meters.inputLevel)
            LevelBar(label: "Out", level: model.meters.outputLevel)
            HStack {
                Stat(label: "Pitch", value: pitchText)
                Stat(label: "Noise cut", value: String(format: "%.0f dB", model.meters.noiseReductionDb))
                Stat(label: "Latency", value: model.running ? String(format: "%.0f ms", model.meters.latencyMs) : "—")
                Stat(label: "CPU", value: model.running ? String(format: "%.0f%%", model.meters.cpu * 100) : "—")
            }
            .padding(.top, 4)
        }
    }

    private var pitchText: String {
        let m = model.meters
        guard m.inputF0 > 0 else { return "—" }
        return String(format: "%.0f → %.0f Hz", m.inputF0, m.outputF0)
    }
}

struct LevelBar: View {
    let label: String
    let level: Float

    var body: some View {
        HStack(spacing: 8) {
            Text(label).font(.caption.monospaced()).frame(width: 28, alignment: .leading)
            GeometryReader { geo in
                let db = 20 * log10(max(Double(level), 1e-5))
                let frac = max(0, min(1, (db + 60) / 60))
                ZStack(alignment: .leading) {
                    Capsule().fill(.quaternary)
                    Capsule().fill(LinearGradient(colors: [.green, .yellow, .red], startPoint: .leading,
                                                  endPoint: .trailing))
                        .frame(width: geo.size.width * frac)
                        .animation(.linear(duration: 0.05), value: frac)
                }
            }
            .frame(height: 8)
        }
    }
}

struct Stat: View {
    let label: String
    let value: String

    var body: some View {
        VStack(spacing: 2) {
            Text(value).font(.callout.monospacedDigit().weight(.medium))
            Text(label).font(.caption2).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity)
    }
}

// MARK: - Voice

struct VoiceCard: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        Card(title: "Voice", icon: "person.wave.2") {
            Picker("Preset", selection: presetBinding) {
                ForEach(VoicePreset.all) { Text($0.name).tag($0.name) }
                Text("Custom").tag("Custom")
            }
            .pickerStyle(.menu)

            ParamSlider(label: "Pitch", value: $model.voice.pitch, range: -6...14, step: 0.5,
                        format: { String(format: "%+.1f st", $0) },
                        help: "How much higher the voice is. +7 to +10 semitones is typical for male → female.")
            ParamSlider(label: "Formant", value: $model.voice.formant, range: -3...7, step: 0.1,
                        format: { String(format: "%+.1f st", $0) },
                        help: "Vocal-tract size. Higher sounds like a smaller, more feminine throat. Too high sounds childlike.")
            ParamSlider(label: "Intonation", value: $model.voice.intonation, range: 0.5...1.8, step: 0.05,
                        format: { String(format: "%.0f%%", $0 * 100) },
                        help: "Expressiveness of the pitch contour. Female speech typically uses a wider range.")
            ParamSlider(label: "Breathiness", value: $model.voice.breathiness, range: 0...1, step: 0.05,
                        format: { String(format: "%.0f%%", $0 * 100) },
                        help: "Adds soft aspiration that follows your voice.")
            ParamSlider(label: "Brightness", value: $model.voice.brightness, range: -6...9, step: 0.5,
                        format: { String(format: "%+.1f dB", $0) },
                        help: "High-frequency presence of the converted voice.")
        }
    }

    private var presetBinding: Binding<String> {
        Binding(
            get: { VoicePreset.all.first { $0.matches(model.voice) }?.name ?? "Custom" },
            set: { name in if let p = VoicePreset.all.first(where: { $0.name == name }) { model.applyPreset(p) } }
        )
    }
}

// MARK: - Noise

struct NoiseCard: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        Card(title: "Noise", icon: "wind") {
            Toggle("Noise suppression", isOn: $model.voice.noiseSuppression)
            ParamSlider(label: "Strength", value: $model.voice.noiseStrength, range: 0...1, step: 0.05,
                        format: { String(format: "%.0f%%", $0 * 100) },
                        help: "Maximum attenuation of background noise (6–30 dB).")
                .disabled(!model.voice.noiseSuppression)
            ParamSlider(label: "Gate", value: $model.voice.gateDb, range: -90 ... -30, step: 1,
                        format: { $0 <= -89.5 ? "Off" : String(format: "%.0f dBFS", $0) },
                        help: "Silences everything below this level between words.")
            ParamSlider(label: "Mic gain", value: $model.voice.inputGainDb, range: -24...24, step: 0.5,
                        format: { String(format: "%+.1f dB", $0) }, help: nil)
            ParamSlider(label: "Out gain", value: $model.voice.outputGainDb, range: -24...12, step: 0.5,
                        format: { String(format: "%+.1f dB", $0) }, help: nil)
        }
    }
}

// MARK: - Routing

struct RoutingCard: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        Card(title: "Audio routing", icon: "arrow.left.arrow.right") {
            Picker("Microphone", selection: $model.routing.inputUID) {
                Text("System default").tag(String?.none)
                ForEach(model.devices.inputs) { Text($0.name).tag(Optional($0.uid)) }
            }
            Picker("Send voice to", selection: $model.routing.outputMode) {
                ForEach(RoutingSettings.OutputMode.allCases) { Text($0.label).tag($0) }
            }
            if model.routing.outputMode == .device {
                Picker("Device", selection: $model.routing.outputUID) {
                    Text("Choose…").tag(String?.none)
                    ForEach(model.devices.outputs) { Text($0.name).tag(Optional($0.uid)) }
                }
            }

            HStack {
                Image(systemName: model.driverInstalled ? "checkmark.circle.fill" : "xmark.circle")
                    .foregroundStyle(model.driverInstalled ? .green : .secondary)
                Text(model.driverInstalled
                     ? "Virtual Mic installed — pick “VoiceShift Virtual Mic” as the microphone in FaceTime, Zoom, WhatsApp…"
                     : "Virtual Mic not installed (needed for the fallback route).")
                    .font(.caption).foregroundStyle(.secondary)
                Spacer()
                if !model.driverInstalled {
                    Button(model.installingDriver ? "Installing…" : "Install Virtual Mic") { model.installDriver() }
                        .disabled(model.installingDriver)
                }
            }

            Divider()
            Toggle("Hear myself (monitor)", isOn: $model.routing.monitorEnabled)
            if model.routing.monitorEnabled {
                Picker("Monitor on", selection: $model.routing.monitorUID) {
                    Text("System default output").tag(String?.none)
                    ForEach(model.devices.outputs.filter { !$0.isVirtualMic }) { Text($0.name).tag(Optional($0.uid)) }
                }
                ParamSlider(label: "Level", value: $model.routing.monitorGainDb, range: -40...6, step: 1,
                            format: { String(format: "%+.0f dB", $0) }, help: "Use headphones to avoid feedback.")
            }

            Divider()
            Picker("Buffer", selection: $model.routing.bufferFrames) {
                ForEach([64, 128, 256, 512], id: \.self) { Text("\($0) samples").tag($0) }
            }
            Toggle("Lowest latency (−10 ms, slightly rougher on very deep voices)", isOn: $model.routing.lowLatency)
        }
    }
}

// MARK: - Bluetooth

struct BluetoothCard: View {
    @EnvironmentObject var model: AppModel
    @State private var selected: String?

    var body: some View {
        let bt = model.bluetooth
        Card(title: "Bluetooth headset mode", icon: "headphones") {
            Text("Makes this Mac act as a hands-free headset for your phone, so the converted voice goes "
                 + "straight into phone calls. If macOS or your phone doesn't allow it, VoiceShift "
                 + "automatically falls back to the Virtual Mic.")
                .font(.caption).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            HStack {
                Picker("Phone", selection: Binding(get: { selected ?? model.routing.phoneAddress },
                                                    set: { selected = $0 })) {
                    Text("Choose a paired phone…").tag(String?.none)
                    ForEach(bt.phones) { p in
                        Text(p.name + (p.advertisesHFP && !BluetoothHeadset.isKnownUnsupported(p.id)
                                       ? "" : " — headset mode unavailable")).tag(Optional(p.id))
                    }
                }
                Button { bt.refresh() } label: { Image(systemName: "arrow.clockwise") }
                    .help("Refresh paired devices")
            }

            HStack(alignment: .top) {
                StatusDot(state: bt.state).padding(.top, 5)
                Text(bt.state.label).font(.callout).fixedSize(horizontal: false, vertical: true)
                Spacer()
            }

            if showAlternative(bt) {
                CallRelayHelp(isIPhone: selectedPhone(bt)?.name.localizedCaseInsensitiveContains("iphone") ?? false)
            }

            HStack {
                switch bt.state {
                case .idle, .failed, .bluetoothOff:
                    Button("Connect as headset") {
                        guard let addr = selected ?? model.routing.phoneAddress else { return }
                        model.routing.phoneAddress = addr
                        bt.connect(address: addr)
                    }
                    .disabled((selected ?? model.routing.phoneAddress) == nil || bt.state == .bluetoothOff)
                default:
                    Button("Disconnect") { bt.disconnect() }
                    if bt.state == .connected { Button("Route call audio") { bt.openCallAudio() } }
                    if bt.state == .audioActive { Button("Return audio to phone") { bt.closeCallAudio() } }
                }
                Spacer()
                if bt.incomingCaller != nil {
                    Button("Answer") { bt.answer() }.tint(.green)
                }
                if bt.callActive || bt.incomingCaller != nil {
                    Button("Hang up", role: .destructive) { bt.hangUp() }
                }
            }
            if let caller = bt.incomingCaller {
                Label("Incoming call: \(caller)", systemImage: "phone.arrow.down.left").foregroundStyle(.green)
            }
            Toggle("Route call audio automatically when a call starts", isOn: $model.routing.autoRouteCalls)
        }
        .onAppear { bt.refresh() }
    }
}

extension BluetoothCard {
    func selectedPhone(_ bt: BluetoothHeadset) -> BluetoothHeadset.Phone? {
        let id = selected ?? model.routing.phoneAddress
        return bt.phones.first { $0.id == id }
    }

    /// Show the working alternative when headset mode failed or is known not to work for this phone.
    func showAlternative(_ bt: BluetoothHeadset) -> Bool {
        if case .failed = bt.state { return true }
        guard let p = selectedPhone(bt) else { return false }
        return !p.advertisesHFP || BluetoothHeadset.isKnownUnsupported(p.id)
    }
}

/// The route that does work for phone calls when the Mac can't be the phone's headset.
struct CallRelayHelp: View {
    let isIPhone: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Label(isIPhone ? "Use iPhone call relay instead (works with normal phone calls)"
                           : "Use one of these instead",
                  systemImage: "lightbulb").font(.callout.weight(.semibold))
            if isIPhone {
                Step(n: 1, text: "On the iPhone: Settings → Phone → Calls on Other Devices → turn on this Mac.")
                Step(n: 2, text: "Make or answer the call on this Mac (FaceTime app, or the call notification).")
                Step(n: 3, text: "In FaceTime’s menu bar: Video → Microphone → VoiceShift Virtual Mic.")
                Text("The caller then hears your converted voice. Both devices need the same Apple Account and Wi-Fi.")
                    .font(.caption).foregroundStyle(.secondary)
            } else {
                Step(n: 1, text: "Call from the Mac with WhatsApp, Zoom, Meet, Teams, etc., and pick "
                                 + "“VoiceShift Virtual Mic” as the microphone.")
                Step(n: 2, text: "Or connect the Mac’s audio output to the phone’s mic input with a TRRS cable "
                                 + "and choose “Send voice to → Specific device”.")
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.accentColor.opacity(0.08), in: RoundedRectangle(cornerRadius: 8))
    }

    private struct Step: View {
        let n: Int
        let text: String
        var body: some View {
            HStack(alignment: .top, spacing: 6) {
                Text("\(n).").font(.callout.monospacedDigit()).foregroundStyle(.secondary)
                Text(text).font(.callout).fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}

struct StatusDot: View {
    let state: BluetoothHeadset.LinkState
    var body: some View {
        Circle().fill(color).frame(width: 9, height: 9)
    }
    private var color: Color {
        switch state {
        case .audioActive: return .green
        case .connected: return .blue
        case .connecting, .audioOpening: return .yellow
        case .failed: return .orange
        case .idle, .bluetoothOff: return .gray
        }
    }
}

// MARK: - Building blocks

struct Card<Content: View>: View {
    let title: String
    let icon: String
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Label(title, systemImage: icon).font(.headline)
            content
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color(nsColor: .controlBackgroundColor), in: RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(.quaternary))
    }
}

struct ParamSlider: View {
    let label: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    let step: Double
    let format: (Double) -> String
    let help: String?

    var body: some View {
        HStack {
            Text(label).frame(width: 84, alignment: .leading)
            Slider(value: $value, in: range, step: step)
            Text(format(value)).font(.callout.monospacedDigit()).frame(width: 76, alignment: .trailing)
        }
        .help(help ?? "")
    }
}

struct Banner: View {
    let text: String
    let color: Color
    let icon: String

    var body: some View {
        Label(text, systemImage: icon)
            .font(.callout)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(10)
            .background(color.opacity(0.12), in: RoundedRectangle(cornerRadius: 10))
            .foregroundStyle(color)
    }
}

// MARK: - Menu bar

struct MenuBarContent: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        Toggle("Voice changer", isOn: Binding(get: { model.isOn }, set: { model.setOn($0) }))
            .keyboardShortcut("v", modifiers: [.command, .option])
        Menu("Preset") {
            ForEach(VoicePreset.all) { p in Button(p.name) { model.applyPreset(p) } }
        }
        Toggle("Noise suppression", isOn: $model.voice.noiseSuppression)
        Divider()
        if let r = model.route { Text("→ " + r.summary) }
        if model.running { Button("Stop audio") { model.stopAudio() } }
        Button("Open VoiceShift…") {
            openWindow(id: "main")
            NSApp.activate(ignoringOtherApps: true)
        }
        Divider()
        Button("Quit") { NSApp.terminate(nil) }.keyboardShortcut("q")
    }
}
