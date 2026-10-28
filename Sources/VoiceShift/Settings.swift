import Foundation

/// Voice parameters the user tunes. Persisted as JSON in UserDefaults.
struct VoiceSettings: Codable, Equatable {
    var pitch: Double = 8          // semitones
    var formant: Double = 3        // semitones
    var intonation: Double = 1.2
    var breathiness: Double = 0.25
    var brightness: Double = 2     // dB
    var noiseSuppression = true
    var noiseStrength: Double = 0.6
    var gateDb: Double = -90       // -90 = off
    var inputGainDb: Double = 0
    var outputGainDb: Double = 0
    var preset: String = VoicePreset.naturalFemale.name
}

/// Where audio comes from and goes to.
struct RoutingSettings: Codable, Equatable {
    enum OutputMode: String, Codable, CaseIterable, Identifiable {
        case automatic, bluetoothHeadset, virtualMic, device
        var id: String { rawValue }
        var label: String {
            switch self {
            case .automatic: return "Automatic"
            case .bluetoothHeadset: return "Bluetooth headset → phone"
            case .virtualMic: return "VoiceShift Virtual Mic"
            case .device: return "Specific device"
            }
        }
    }

    var inputUID: String?          // nil = system default (never our virtual mic)
    var outputMode: OutputMode = .automatic
    var outputUID: String?         // for .device
    var monitorEnabled = false
    var monitorUID: String?        // nil = system default output
    var monitorGainDb: Double = -6
    var bufferFrames = 128
    var lowLatency = false
    var phoneAddress: String?
    var autoRouteCalls = true
}

struct VoicePreset: Identifiable, Hashable {
    let name: String
    let pitch: Double, formant: Double, intonation: Double, breathiness: Double, brightness: Double
    var id: String { name }

    static let naturalFemale = VoicePreset(name: "Natural Female", pitch: 8, formant: 3, intonation: 1.2,
                                           breathiness: 0.25, brightness: 2)
    static let soft = VoicePreset(name: "Soft & Warm", pitch: 7, formant: 2.2, intonation: 1.1,
                                  breathiness: 0.4, brightness: 0.5)
    static let bright = VoicePreset(name: "Bright / Young", pitch: 10, formant: 4, intonation: 1.3,
                                    breathiness: 0.2, brightness: 4)
    static let alto = VoicePreset(name: "Alto (Deeper)", pitch: 5, formant: 2, intonation: 1.05,
                                  breathiness: 0.2, brightness: 1)
    static let all = [naturalFemale, soft, bright, alto]

    func apply(to s: inout VoiceSettings) {
        s.pitch = pitch
        s.formant = formant
        s.intonation = intonation
        s.breathiness = breathiness
        s.brightness = brightness
        s.preset = name
    }

    func matches(_ s: VoiceSettings) -> Bool {
        abs(s.pitch - pitch) < 0.05 && abs(s.formant - formant) < 0.05 && abs(s.intonation - intonation) < 0.01
            && abs(s.breathiness - breathiness) < 0.01 && abs(s.brightness - brightness) < 0.05
    }
}

enum Persist {
    static func load<T: Decodable>(_ key: String, default value: T) -> T {
        guard let data = UserDefaults.standard.data(forKey: key),
              let v = try? JSONDecoder().decode(T.self, from: data) else { return value }
        return v
    }

    static func save<T: Encodable>(_ key: String, _ value: T) {
        if let data = try? JSONEncoder().encode(value) { UserDefaults.standard.set(data, forKey: key) }
    }
}
