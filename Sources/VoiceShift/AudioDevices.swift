import CoreAudio
import Foundation

/// A CoreAudio device as the UI needs to see it.
struct AudioDevice: Identifiable, Hashable {
    let id: AudioObjectID
    let uid: String
    let name: String
    let transport: UInt32
    let inputChannels: Int
    let outputChannels: Int

    var isBluetooth: Bool {
        transport == kAudioDeviceTransportTypeBluetooth || transport == kAudioDeviceTransportTypeBluetoothLE
    }
    var isVirtualMic: Bool { uid == AudioDeviceManager.virtualMicUID }
    var hasInput: Bool { inputChannels > 0 }
    var hasOutput: Bool { outputChannels > 0 }
}

/// Enumerates devices and publishes changes (plug/unplug, Bluetooth connect, driver install).
final class AudioDeviceManager: ObservableObject {
    static let virtualMicUID = "VoiceShiftVirtualMic_UID"

    @Published private(set) var devices: [AudioDevice] = []

    var inputs: [AudioDevice] { devices.filter { $0.hasInput && !$0.isVirtualMic } }
    var outputs: [AudioDevice] { devices.filter(\.hasOutput) }
    var virtualMic: AudioDevice? { devices.first(where: \.isVirtualMic) }

    private var listener: AudioObjectPropertyListenerBlock?

    init() {
        refresh()
        var addr = Self.address(kAudioHardwarePropertyDevices)
        let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in self?.refresh() }
        listener = block
        AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &addr, .main, block)
    }

    deinit {
        if let listener {
            var addr = Self.address(kAudioHardwarePropertyDevices)
            AudioObjectRemovePropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &addr, .main, listener)
        }
    }

    func refresh() {
        let list = Self.allDeviceIDs().compactMap(Self.describe)
        if Thread.isMainThread { devices = list } else { DispatchQueue.main.async { self.devices = list } }
    }

    func device(uid: String?) -> AudioDevice? {
        guard let uid else { return nil }
        return devices.first { $0.uid == uid }
    }

    func device(id: AudioObjectID) -> AudioDevice? { devices.first { $0.id == id } }

    var defaultInput: AudioDevice? { device(id: Self.defaultDevice(kAudioHardwarePropertyDefaultInputDevice)) }
    var defaultOutput: AudioDevice? { device(id: Self.defaultDevice(kAudioHardwarePropertyDefaultOutputDevice)) }

    /// The built-in microphone, used when the system default input is our own virtual mic
    /// (capturing it would feed our output straight back into our input).
    var builtInMic: AudioDevice? {
        inputs.first { $0.transport == kAudioDeviceTransportTypeBuiltIn } ?? inputs.first
    }

    /// CoreAudio device(s) macOS creates for a connected Bluetooth peer, matched by address or name.
    func bluetoothDevices(address: String, name: String) -> [AudioDevice] {
        let norm = { (s: String) in s.lowercased().filter(\.isHexDigit) }
        let addr = norm(address)
        return devices.filter { d in
            d.isBluetooth && ((!addr.isEmpty && norm(d.uid).contains(addr)) || d.name == name)
        }
    }

    // MARK: CoreAudio plumbing

    static func address(_ sel: AudioObjectPropertySelector,
                        _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal)
        -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(mSelector: sel, mScope: scope, mElement: kAudioObjectPropertyElementMain)
    }

    private static func allDeviceIDs() -> [AudioObjectID] {
        var addr = address(kAudioHardwarePropertyDevices)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size) == noErr
        else { return [] }
        var ids = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &ids) == noErr
        else { return [] }
        return ids
    }

    private static func defaultDevice(_ sel: AudioObjectPropertySelector) -> AudioObjectID {
        var addr = address(sel)
        var id = AudioObjectID(0)
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &id)
        return id
    }

    private static func string(_ id: AudioObjectID, _ sel: AudioObjectPropertySelector) -> String? {
        var addr = address(sel)
        var value: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(id, &addr, 0, nil, &size, &value) == noErr, let value else { return nil }
        return value.takeRetainedValue() as String
    }

    private static func uint32(_ id: AudioObjectID, _ sel: AudioObjectPropertySelector) -> UInt32 {
        var addr = address(sel)
        var v: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        AudioObjectGetPropertyData(id, &addr, 0, nil, &size, &v)
        return v
    }

    private static func channels(_ id: AudioObjectID, _ scope: AudioObjectPropertyScope) -> Int {
        var addr = address(kAudioDevicePropertyStreamConfiguration, scope)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(id, &addr, 0, nil, &size) == noErr, size > 0 else { return 0 }
        let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: 16)
        defer { raw.deallocate() }
        guard AudioObjectGetPropertyData(id, &addr, 0, nil, &size, raw) == noErr else { return 0 }
        let list = UnsafeMutableAudioBufferListPointer(raw.assumingMemoryBound(to: AudioBufferList.self))
        return list.reduce(0) { $0 + Int($1.mNumberChannels) }
    }

    private static func describe(_ id: AudioObjectID) -> AudioDevice? {
        guard let uid = string(id, kAudioDevicePropertyDeviceUID) else { return nil }
        let inCh = channels(id, kAudioObjectPropertyScopeInput)
        let outCh = channels(id, kAudioObjectPropertyScopeOutput)
        guard inCh + outCh > 0 else { return nil }
        // Skip aggregate/private helper devices other apps create.
        let transport = uint32(id, kAudioDevicePropertyTransportType)
        return AudioDevice(id: id, uid: uid, name: string(id, kAudioObjectPropertyName) ?? uid,
                           transport: transport, inputChannels: inCh, outputChannels: outCh)
    }
}
