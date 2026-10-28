import Foundation
import IOBluetooth

/// Makes this Mac behave as a Bluetooth Hands-Free (HFP "HF") headset for a paired phone.
///
/// Flow: phone is paired with the Mac → we open the HFP service-level connection
/// (RFCOMM) to the phone's Audio Gateway → when a call is active we open the SCO
/// voice link → macOS exposes it as a CoreAudio device → the engine plays the
/// converted voice into it, and the caller's audio is played on the Mac.
///
/// Whether macOS exposes the SCO link as a usable CoreAudio device for a third-party
/// HF connection depends on the macOS version, the Bluetooth chipset and the phone.
/// Every stage is verified and, if one fails, `fallbackReason` says why so the app
/// can switch to the virtual-mic path.
@MainActor
final class BluetoothHeadset: NSObject, ObservableObject {
    enum LinkState: Equatable {
        case bluetoothOff
        case idle
        case connecting
        case connected        // HFP service level connection up
        case audioOpening
        case audioActive      // SCO up and CoreAudio device found
        case failed(String)

        var label: String {
            switch self {
            case .bluetoothOff: return "Bluetooth is off"
            case .idle: return "Not connected"
            case .connecting: return "Connecting to phone…"
            case .connected: return "Connected as headset"
            case .audioOpening: return "Opening call audio…"
            case .audioActive: return "Call audio routed"
            case .failed(let why): return why
            }
        }
    }

    struct Phone: Identifiable, Hashable {
        let id: String      // Bluetooth address
        let name: String
        let isPhone: Bool
        let advertisesHFP: Bool
    }

    @Published private(set) var phones: [Phone] = []
    @Published private(set) var state: LinkState = .idle
    @Published private(set) var callActive = false
    @Published private(set) var incomingCaller: String?
    /// CoreAudio device carrying the SCO voice link while audio is active.
    @Published private(set) var audioDevice: AudioDevice?
    /// Set when direct headset emulation turned out not to work on this system.
    @Published private(set) var fallbackReason: String?

    var autoOpenAudioOnCall = true
    weak var devices: AudioDeviceManager?

    private var hf: IOBluetoothHandsFreeDevice?
    private var phone: Phone?
    private var timeout: DispatchWorkItem?

    private static let hfpAudioGatewayUUID: UInt16 = 0x111F

    func refresh() {
        guard let host = IOBluetoothHostController.default(),
              host.powerState == kBluetoothHCIPowerStateON else {
            state = .bluetoothOff
            phones = []
            return
        }
        if state == .bluetoothOff { state = .idle }
        let paired = (IOBluetoothDevice.pairedDevices() as? [IOBluetoothDevice]) ?? []
        phones = paired.compactMap { d in
            guard let addr = d.addressString else { return nil }
            let isPhone = d.deviceClassMajor == UInt32(kBluetoothDeviceClassMajorPhone)
            let hfp = d.getServiceRecord(for: IOBluetoothSDPUUID(uuid16: Self.hfpAudioGatewayUUID)) != nil
            return Phone(id: addr, name: d.name ?? addr, isPhone: isPhone, advertisesHFP: hfp)
        }
        .sorted { ($0.isPhone ? 0 : 1, $0.name) < ($1.isPhone ? 0 : 1, $1.name) }
    }

    func connect(address: String) {
        disconnect()
        fallbackReason = nil
        guard let device = IOBluetoothDevice(addressString: address) else {
            fail("Phone \(address) is not paired with this Mac")
            return
        }
        let info = phones.first { $0.id == address }
            ?? Phone(id: address, name: device.name ?? address, isPhone: true, advertisesHFP: true)
        phone = info
        // Refresh the phone's SDP records so we know whether it offers an HFP Audio Gateway.
        device.performSDPQuery(nil)
        guard let hf = IOBluetoothHandsFreeDevice(device: device, delegate: self) else {
            fail("macOS refused to create a hands-free link (IOBluetoothHandsFreeDevice unavailable)")
            return
        }
        hf.supportedFeatures = IOBluetoothHandsFreeDeviceFeatures.cliPresentation.rawValue
            | IOBluetoothHandsFreeDeviceFeatures.remoteVolumeControl.rawValue
            | IOBluetoothHandsFreeDeviceFeatures.enhancedCallStatus.rawValue
            | IOBluetoothHandsFreeDeviceFeatures.ecAndOrNRFunction.rawValue  // we do our own NR
        self.hf = hf
        state = .connecting
        hf.connect()
        arm(seconds: 15) { [weak self] in
            self?.fail("The phone didn't accept the headset connection. Using the Virtual Mic instead.",
                       unsupported: true)
        }
    }

    func disconnect() {
        timeout?.cancel()
        hf?.disconnectSCO()
        hf?.disconnect()
        hf = nil
        audioDevice = nil
        callActive = false
        incomingCaller = nil
        if state != .bluetoothOff { state = .idle }
    }

    /// Opens the SCO voice channel (call audio) between the phone and the Mac.
    func openCallAudio() {
        guard let hf, hf.isConnected else { return }
        state = .audioOpening
        hf.connectSCO()
        hf.transferAudioToComputer()
        arm(seconds: 8) { [weak self] in
            self?.fail("The phone didn't open the call-audio channel. Using the Virtual Mic instead.")
        }
    }

    func closeCallAudio() {
        hf?.disconnectSCO()
        audioDevice = nil
        if hf?.isConnected == true { state = .connected }
    }

    func answer() { hf?.acceptCall() }
    func hangUp() { hf?.endCall() }

    // MARK: - private

    /// Ends the attempt *and* the link, so the phone never keeps treating this Mac as its
    /// headset (which could route a real call into a dead audio channel).
    private func fail(_ why: String, unsupported: Bool = false) {
        timeout?.cancel()
        if let hf {
            hf.transferAudioToPhone()
            hf.disconnectSCO()
            hf.disconnect()
        }
        hf = nil
        audioDevice = nil
        callActive = false
        incomingCaller = nil
        if unsupported, let id = phone?.id { Self.markUnsupported(id, true) }
        state = .failed(why)
        fallbackReason = why
    }

    // MARK: - Phones known not to support a Mac as hands-free headset

    private static let unsupportedKey = "hfpUnsupportedPhones"

    static func isKnownUnsupported(_ address: String) -> Bool {
        (UserDefaults.standard.stringArray(forKey: unsupportedKey) ?? []).contains(address)
    }

    private static func markUnsupported(_ address: String, _ value: Bool) {
        var set = Set(UserDefaults.standard.stringArray(forKey: unsupportedKey) ?? [])
        if value { set.insert(address) } else { set.remove(address) }
        UserDefaults.standard.set(Array(set), forKey: unsupportedKey)
    }

    /// Plain-language reason for an IOBluetooth status code.
    private static func explain(_ status: IOReturn) -> (text: String, unsupported: Bool) {
        switch UInt32(bitPattern: status) {
        case 0xE000_02C7:  // kIOReturnUnsupported
            return ("Your phone and macOS don't allow a Mac to carry call audio as a headset.", true)
        case 0xE000_02E2:  // kIOReturnNotPermitted
            return ("macOS didn't permit the hands-free connection (check Bluetooth permission for VoiceShift).", false)
        case 0xE000_02D6:  // kIOReturnTimeout
            return ("The phone didn't respond in time.", false)
        case 0xE000_02CD, 0xE000_02BC:  // kIOReturnNotOpen / kIOReturnError
            return ("The phone refused the hands-free connection.", true)
        default:
            return (String(format: "Bluetooth error 0x%08x.", status), false)
        }
    }

    private func arm(seconds: Double, _ action: @escaping () -> Void) {
        timeout?.cancel()
        let w = DispatchWorkItem(block: action)
        timeout = w
        DispatchQueue.main.asyncAfter(deadline: .now() + seconds, execute: w)
    }

    /// After SCO opens, wait for macOS to publish the matching CoreAudio device.
    private func awaitAudioDevice(attempt: Int = 0) {
        guard let phone, let devices else { return }
        devices.refresh()
        let matches = devices.bluetoothDevices(address: phone.id, name: phone.name)
        if let dev = matches.first(where: { $0.hasOutput }) {
            timeout?.cancel()
            audioDevice = dev
            state = .audioActive
            Self.markUnsupported(phone.id, false)
            return
        }
        if attempt >= 24 {  // ~6 s
            fail("Call audio connected, but macOS did not expose it as an audio device " +
                 "(not supported on this Mac/macOS). Using the Virtual Mic instead.", unsupported: true)
            return
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { [weak self] in
            self?.awaitAudioDevice(attempt: attempt + 1)
        }
    }

    fileprivate func handleConnected(_ status: IOReturn) {
        timeout?.cancel()
        if status == kIOReturnSuccess {
            state = .connected
        } else {
            let e = Self.explain(status)
            fail("Headset connection failed: \(e.text) Using the Virtual Mic instead.",
                 unsupported: e.unsupported || phone?.advertisesHFP == false)
        }
    }

    fileprivate func handleSCOOpened(_ status: IOReturn) {
        if status == kIOReturnSuccess {
            awaitAudioDevice()
        } else {
            let e = Self.explain(status)
            fail("Call audio can't be routed to this Mac: \(e.text) Using the Virtual Mic instead.",
                 unsupported: e.unsupported)
        }
    }

    fileprivate func handleSCOClosed() {
        audioDevice = nil
        if hf?.isConnected == true { state = .connected }
    }

    fileprivate func handleDisconnected() {
        hf = nil
        audioDevice = nil
        callActive = false
        if case .failed = state { return }
        state = .idle
    }

    fileprivate func handleCallActive(_ active: Bool) {
        callActive = active
        if active {
            incomingCaller = nil
            if autoOpenAudioOnCall, audioDevice == nil, state == .connected { openCallAudio() }
        } else {
            closeCallAudio()
        }
    }

    fileprivate func handleIncoming(_ number: String?) { incomingCaller = number ?? "Unknown caller" }
}

extension BluetoothHeadset: IOBluetoothHandsFreeDeviceDelegate {
    nonisolated func handsFree(_ device: IOBluetoothHandsFree!, connected status: NSNumber!) {
        let s = IOReturn(status?.int32Value ?? Int32(kIOReturnError))
        Task { @MainActor in self.handleConnected(s) }
    }

    nonisolated func handsFree(_ device: IOBluetoothHandsFree!, disconnected status: NSNumber!) {
        Task { @MainActor in self.handleDisconnected() }
    }

    nonisolated func handsFree(_ device: IOBluetoothHandsFree!, scoConnectionOpened status: NSNumber!) {
        let s = IOReturn(status?.int32Value ?? Int32(kIOReturnError))
        Task { @MainActor in self.handleSCOOpened(s) }
    }

    nonisolated func handsFree(_ device: IOBluetoothHandsFree!, scoConnectionClosed status: NSNumber!) {
        Task { @MainActor in self.handleSCOClosed() }
    }

    nonisolated func handsFree(_ device: IOBluetoothHandsFreeDevice!, isCallActive: NSNumber!) {
        let active = isCallActive?.boolValue ?? false
        Task { @MainActor in self.handleCallActive(active) }
    }

    nonisolated func handsFree(_ device: IOBluetoothHandsFreeDevice!, incomingCallFrom number: String!) {
        let n = number
        Task { @MainActor in self.handleIncoming(n) }
    }
}
