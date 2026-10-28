import AVFoundation
import Combine
import CoreAudio
import Foundation
import VoiceCore

struct Meters: Equatable {
    var inputLevel: Float = 0
    var outputLevel: Float = 0
    var inputF0: Float = 0
    var outputF0: Float = 0
    var noiseReductionDb: Float = 0
    var cpu: Float = 0
    var latencyMs: Float = 0
    var sampleRate: Float = 0
    var underruns: Int = 0
}

/// Where the converted voice is currently going.
struct ActiveRoute: Equatable {
    enum Kind { case bluetooth, virtualMic, device, speakersFallback }
    let kind: Kind
    let output: AudioDevice
    let input: AudioDevice
    let note: String?

    var summary: String {
        switch kind {
        case .bluetooth: return "Bluetooth headset → \(output.name)"
        case .virtualMic: return "VoiceShift Virtual Mic"
        case .device: return output.name
        case .speakersFallback: return "\(output.name) (preview only)"
        }
    }
}

@MainActor
final class AppModel: ObservableObject {
    @Published private(set) var isOn = false
    @Published private(set) var running = false
    @Published var voice: VoiceSettings {
        didSet {
            guard voice != oldValue else { return }
            pushVoice()
            Persist.save("voice", voice)
        }
    }
    @Published var routing: RoutingSettings {
        didSet {
            guard routing != oldValue else { return }
            Persist.save("routing", routing)
            bluetooth.autoOpenAudioOnCall = routing.autoRouteCalls
            engine.set(.monitorGainDb, Float(routing.monitorGainDb))
            reconfigure()
        }
    }
    @Published private(set) var meters = Meters()
    @Published private(set) var route: ActiveRoute?
    @Published private(set) var errorMessage: String?
    @Published private(set) var micAuthorized = AVCaptureDevice.authorizationStatus(for: .audio) == .authorized
    @Published private(set) var installingDriver = false
    @Published private(set) var awaitingMicPermission = false

    let devices = AudioDeviceManager()
    let bluetooth = BluetoothHeadset()

    private let engine = VoiceEngine()
    /// Plays the caller's voice (phone → Mac) while acting as a Bluetooth headset.
    private let farEnd = VoiceEngine()
    private var activeConfig: EngineConfig?
    private var farEndConfig: [AudioObjectID]?
    private var meterTimer: Timer?
    private var subs = Set<AnyCancellable>()

    private struct EngineConfig: Equatable {
        let input: AudioObjectID, output: AudioObjectID, monitor: AudioObjectID?
        let buffer: Int, lowLatency: Bool
    }

    init() {
        voice = Persist.load("voice", default: VoiceSettings())
        routing = Persist.load("routing", default: RoutingSettings())
        bluetooth.devices = devices
        bluetooth.autoOpenAudioOnCall = routing.autoRouteCalls
        bluetooth.refresh()
        pushVoice()
        engine.set(.effectEnabled, false)
        engine.set(.monitorGainDb, Float(routing.monitorGainDb))
        farEnd.set(.passthrough, true)

        // Re-route when devices appear/disappear or Bluetooth call audio comes and goes.
        devices.$devices.dropFirst().receive(on: DispatchQueue.main)
            .sink { [weak self] _ in self?.reconfigure() }.store(in: &subs)
        bluetooth.$audioDevice.dropFirst().receive(on: DispatchQueue.main)
            .sink { [weak self] _ in self?.reconfigure() }.store(in: &subs)
        bluetooth.$fallbackReason.dropFirst().receive(on: DispatchQueue.main)
            .sink { [weak self] _ in self?.reconfigure() }.store(in: &subs)
        // Forward nested changes so views observing the model refresh.
        bluetooth.objectWillChange.sink { [weak self] _ in self?.objectWillChange.send() }.store(in: &subs)
        devices.objectWillChange.sink { [weak self] _ in self?.objectWillChange.send() }.store(in: &subs)

        applyLaunchArguments()

        if let addr = routing.phoneAddress, bluetooth.phones.contains(where: { $0.id == addr }),
           !BluetoothHeadset.isKnownUnsupported(addr) {
            // Re-attach to the remembered phone quietly; failures just leave the fallback in place.
            bluetooth.connect(address: addr)
        }
    }

    /// `--on` starts with the voice changer on (e.g. as a login item);
    /// `--input <device UID>` / `--output <device UID>` pick devices for this launch.
    private func applyLaunchArguments() {
        let args = CommandLine.arguments
        func value(after flag: String) -> String? {
            guard let i = args.firstIndex(of: flag), i + 1 < args.count else { return nil }
            return args[i + 1]
        }
        if let uid = value(after: "--input") { routing.inputUID = uid }
        if let uid = value(after: "--output") {
            routing.outputMode = .device
            routing.outputUID = uid
        }
        if let path = value(after: "--diagnostics") { startDiagnostics(path: path) }
        if args.contains("--on") {
            DispatchQueue.main.async { self.setOn(true) }
        }
    }

    private var diagnosticsTimer: Timer?

    /// Writes the live state as JSON twice a second (for troubleshooting / automated tests).
    private func startDiagnostics(path: String) {
        diagnosticsTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.writeDiagnostics(path: path) }
        }
    }

    private func writeDiagnostics(path: String) {
        let auth: String
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized: auth = "authorized"
        case .denied: auth = "denied"
        case .restricted: auth = "restricted"
        case .notDetermined: auth = "notDetermined"
        @unknown default: auth = "unknown"
        }
        let m = meters
        let info: [String: Any] = [
            "isOn": isOn, "running": running, "awaitingMicPermission": awaitingMicPermission, "engineRunning": engine.isRunning, "micPermission": auth,
            "input": route?.input.name ?? "", "output": route?.output.name ?? "", "route": route?.summary ?? "",
            "routeNote": route?.note ?? "", "error": errorMessage ?? "",
            "inputLevel": m.inputLevel, "outputLevel": m.outputLevel, "inputF0": m.inputF0, "outputF0": m.outputF0,
            "latencyMs": m.latencyMs, "cpu": m.cpu, "underruns": m.underruns,
            "bluetooth": bluetooth.state.label, "virtualMicInstalled": driverInstalled,
        ]
        if let data = try? JSONSerialization.data(withJSONObject: info, options: [.prettyPrinted, .sortedKeys]) {
            try? data.write(to: URL(fileURLWithPath: path), options: .atomic)
        }
    }

    // MARK: - ON/OFF

    /// ON = converted voice. OFF = your natural voice keeps flowing (calls never go silent).
    func setOn(_ on: Bool) {
        isOn = on
        engine.set(.effectEnabled, on)
        if on && !running { startAudio() }
    }

    func toggle() { setOn(!isOn) }

    func startAudio() {
        errorMessage = nil
        ensureMicPermission { [weak self] granted in
            guard let self else { return }
            guard granted else {
                self.errorMessage = "Microphone access is off. Enable VoiceShift in System Settings → " +
                    "Privacy & Security → Microphone."
                self.isOn = false
                return
            }
            self.running = true
            self.reconfigure(force: true)
            self.startMeters()
        }
    }

    func stopAudio() {
        engine.stop()
        farEnd.stop()
        activeConfig = nil
        farEndConfig = nil
        running = false
        isOn = false
        engine.set(.effectEnabled, false)
        route = nil
        meterTimer?.invalidate()
        meterTimer = nil
        meters = Meters()
    }

    func applyPreset(_ p: VoicePreset) { p.apply(to: &voice) }

    func setMuted(_ muted: Bool) { engine.set(.mute, muted) }

    // MARK: - Virtual mic driver

    var driverInstalled: Bool { devices.virtualMic != nil }

    func installDriver() {
        installingDriver = true
        errorMessage = nil
        let wasRunning = running
        if wasRunning { engine.stop(); farEnd.stop(); activeConfig = nil; farEndConfig = nil }
        DriverInstaller.install { [weak self] result in
            guard let self else { return }
            self.installingDriver = false
            if case .failure(let err) = result { self.errorMessage = err.localizedDescription }
            // coreaudiod restarts; give it a moment to republish devices.
            DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) {
                self.devices.refresh()
                if wasRunning { self.reconfigure(force: true) }
            }
        }
    }

    // MARK: - Routing

    func resolveInput() -> AudioDevice? {
        var dev = devices.device(uid: routing.inputUID) ?? devices.defaultInput
        // Capturing our own virtual mic would loop our output back into our input.
        if dev?.isVirtualMic == true { dev = devices.builtInMic }
        return dev
    }

    func resolveRoute(input: AudioDevice) -> ActiveRoute? {
        let vm = devices.virtualMic
        let bt = bluetooth.audioDevice
        let btNote = bluetooth.fallbackReason.map { "Bluetooth headset unavailable: \($0)" }
        switch routing.outputMode {
        case .automatic, .bluetoothHeadset:
            if let bt { return ActiveRoute(kind: .bluetooth, output: bt, input: input, note: nil) }
            if let vm {
                let note = routing.outputMode == .bluetoothHeadset
                    ? (btNote ?? "Waiting for phone call audio — using Virtual Mic meanwhile") : btNote
                return ActiveRoute(kind: .virtualMic, output: vm, input: input, note: note)
            }
            if let out = devices.defaultOutput {
                return ActiveRoute(kind: .speakersFallback, output: out, input: input,
                                   note: "Virtual Mic not installed — playing to \(out.name) so you can preview.")
            }
            return nil
        case .virtualMic:
            guard let vm else { return nil }
            return ActiveRoute(kind: .virtualMic, output: vm, input: input, note: nil)
        case .device:
            guard let d = devices.device(uid: routing.outputUID) else { return nil }
            return ActiveRoute(kind: d.isVirtualMic ? .virtualMic : .device, output: d, input: input, note: nil)
        }
    }

    private func monitorDevice(excluding output: AudioDevice) -> AudioDevice? {
        guard routing.monitorEnabled else { return nil }
        let m = devices.device(uid: routing.monitorUID) ?? devices.defaultOutput
        guard let m, m.id != output.id, !m.isVirtualMic else { return nil }
        return m
    }

    /// Recomputes devices and (re)starts the engines only when something actually changed.
    func reconfigure(force: Bool = false) {
        guard running else { return }
        guard let input = resolveInput() else {
            fail("No microphone found.")
            return
        }
        guard let r = resolveRoute(input: input) else {
            fail(routing.outputMode == .virtualMic
                 ? "VoiceShift Virtual Mic is not installed. Click “Install Virtual Mic”."
                 : "The selected output device is not available.")
            return
        }
        let monitor = monitorDevice(excluding: r.output)
        let cfg = EngineConfig(input: input.id, output: r.output.id, monitor: monitor?.id,
                               buffer: routing.bufferFrames, lowLatency: routing.lowLatency)
        route = r
        if force || cfg != activeConfig || !engine.isRunning {
            do {
                try engine.start(input: cfg.input, output: cfg.output, monitor: cfg.monitor,
                                 bufferFrames: cfg.buffer, lowLatency: cfg.lowLatency)
                activeConfig = cfg
                errorMessage = nil
            } catch {
                activeConfig = nil
                errorMessage = "Audio failed to start: \(error.localizedDescription)"
            }
        }
        updateFarEnd(route: r)
    }

    /// When the Mac is the phone's headset, the caller's voice arrives on the Bluetooth
    /// device's input; play it on the Mac's speakers/headphones.
    private func updateFarEnd(route r: ActiveRoute) {
        guard r.kind == .bluetooth, r.output.hasInput,
              let listen = devices.device(uid: routing.monitorUID) ?? devices.defaultOutput,
              listen.id != r.output.id else {
            if farEndConfig != nil { farEnd.stop(); farEndConfig = nil }
            return
        }
        let cfg = [r.output.id, listen.id]
        guard cfg != farEndConfig || !farEnd.isRunning else { return }
        do {
            try farEnd.start(input: r.output.id, output: listen.id, monitor: nil,
                             bufferFrames: routing.bufferFrames, lowLatency: true)
            farEndConfig = cfg
        } catch {
            farEndConfig = nil
            errorMessage = "Could not play the caller's audio: \(error.localizedDescription)"
        }
    }

    private func fail(_ msg: String) {
        engine.stop()
        farEnd.stop()
        activeConfig = nil
        farEndConfig = nil
        route = nil
        errorMessage = msg
    }

    // MARK: - Parameters & meters

    private func pushVoice() {
        let v = voice
        engine.set(.pitchSemitones, Float(v.pitch))
        engine.set(.formantSemitones, Float(v.formant))
        engine.set(.intonation, Float(v.intonation))
        engine.set(.breathiness, Float(v.breathiness))
        engine.set(.brightnessDb, Float(v.brightness))
        engine.set(.noiseSuppression, v.noiseSuppression)
        engine.set(.noiseStrength, Float(v.noiseStrength))
        engine.set(.gateThresholdDb, Float(v.gateDb))
        engine.set(.inputGainDb, Float(v.inputGainDb))
        engine.set(.outputGainDb, Float(v.outputGainDb))
    }

    private func startMeters() {
        meterTimer?.invalidate()
        meterTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 20.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.pollMeters() }
        }
    }

    private func pollMeters() {
        guard engine.isRunning else { return }
        let decay: Float = 0.75
        var m = meters
        m.inputLevel = max(engine.meter(.inputPeak), m.inputLevel * decay)
        m.outputLevel = max(engine.meter(.outputPeak), m.outputLevel * decay)
        m.inputF0 = engine.meter(.inputF0)
        m.outputF0 = engine.meter(.outputF0)
        m.noiseReductionDb = engine.meter(.noiseReductionDb)
        m.cpu = engine.meter(.cpuLoad)
        m.latencyMs = engine.meter(.latencyMs)
        m.sampleRate = engine.meter(.sampleRate)
        m.underruns = Int(engine.meter(.underruns))
        meters = m
    }

    private func ensureMicPermission(_ done: @escaping (Bool) -> Void) {
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized:
            micAuthorized = true
            done(true)
        case .notDetermined:
            awaitingMicPermission = true
            AVCaptureDevice.requestAccess(for: .audio) { ok in
                Task { @MainActor in
                    self.awaitingMicPermission = false
                    self.micAuthorized = ok
                    done(ok)
                }
            }
        default:
            micAuthorized = false
            done(false)
        }
    }
}
