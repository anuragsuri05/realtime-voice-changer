import CoreAudio
import Foundation
import VoiceCore

/// Thin Swift owner of a C `VSEngine`. All calls happen on the main thread;
/// the C side handles the hand-off to the real-time audio thread.
final class VoiceEngine {
    private let ref: OpaquePointer

    init() {
        guard let e = vs_engine_create() else { fatalError("Out of memory creating audio engine") }
        ref = e
    }

    deinit { vs_engine_destroy(ref) }

    var isRunning: Bool { vs_engine_is_running(ref) }

    /// Throws with a readable message if any CoreAudio stage fails.
    func start(input: AudioObjectID, output: AudioObjectID, monitor: AudioObjectID?,
               bufferFrames: Int, lowLatency: Bool) throws {
        let err = vs_engine_start(ref, input, output, monitor ?? 0, UInt32(bufferFrames), lowLatency)
        if err != noErr {
            throw EngineError(message: String(cString: vs_engine_last_error(ref)))
        }
    }

    func stop() { vs_engine_stop(ref) }

    func set(_ p: VSParam, _ v: Float) { vs_engine_set_param(ref, p, v) }
    func set(_ p: VSParam, _ on: Bool) { vs_engine_set_param(ref, p, on ? 1 : 0) }

    func meter(_ m: VSMeter) -> Float { vs_engine_meter(ref, m) }

    struct EngineError: LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }
}
