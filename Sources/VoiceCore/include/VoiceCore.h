#ifndef VOICECORE_H
#define VOICECORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VS_CLOSED_ENUM(name) \
    typedef enum __attribute__((enum_extensibility(closed))) name : int32_t name; \
    enum name : int32_t

/// Tunable parameters. Every parameter may be set from any thread at any time;
/// the audio thread picks the new value up on its next block.
VS_CLOSED_ENUM(VSParam) {
    VSParamEffectEnabled = 0,  ///< 0 = natural voice passthrough, 1 = converted voice (crossfaded)
    VSParamPitchSemitones,     ///< -6 … +14
    VSParamFormantSemitones,   ///< -3 … +7  (vocal-tract length; + = shorter/feminine)
    VSParamIntonation,         ///< 0.5 … 1.8 pitch-contour expressiveness (1 = unchanged)
    VSParamBreathiness,        ///< 0 … 1
    VSParamBrightnessDb,       ///< -6 … +9  high shelf at 3 kHz on the converted voice
    VSParamNoiseSuppression,   ///< 0 / 1
    VSParamNoiseStrength,      ///< 0 … 1
    VSParamGateThresholdDb,    ///< -90 (off) … -30
    VSParamInputGainDb,        ///< -24 … +24
    VSParamOutputGainDb,       ///< -24 … +12
    VSParamMonitorGainDb,      ///< -60 … +6
    VSParamMute,               ///< 0 / 1
    VSParamPassthrough,        ///< 0 / 1 raw copy, no processing (far-end call audio)
    VSParamCount
};

VS_CLOSED_ENUM(VSMeter) {
    VSMeterInputPeak = 0,     ///< linear
    VSMeterOutputPeak,        ///< linear
    VSMeterInputF0,           ///< Hz, 0 when unvoiced
    VSMeterOutputF0,          ///< Hz, 0 when unvoiced / bypassed
    VSMeterNoiseReductionDb,  ///< >= 0
    VSMeterCpuLoad,           ///< fraction of the real-time budget used by DSP
    VSMeterLatencyMs,         ///< estimated end-to-end (DSP + device + buffering)
    VSMeterSampleRate,
    VSMeterUnderruns,
    VSMeterCount
};

// MARK: - DSP chain (offline or real-time; process() is real-time safe)

typedef struct VSChain VSChain;

VSChain *vs_chain_create(double sampleRate, int maxBlock, bool lowLatency);
void vs_chain_destroy(VSChain *c);
void vs_chain_reset(VSChain *c);
void vs_chain_set_param(VSChain *c, VSParam p, float value);
float vs_chain_get_param(const VSChain *c, VSParam p);
/// Mono in → mono out, any n (internally split into maxBlock chunks). In-place allowed.
void vs_chain_process(VSChain *c, const float *in, float *out, int n);
/// Algorithmic latency of the chain in samples (constant for the chain's lifetime).
int vs_chain_latency_samples(const VSChain *c);
float vs_chain_meter(const VSChain *c, VSMeter m);

// MARK: - Real-time engine (CoreAudio AUHAL input → chain → output [+ monitor])

typedef struct VSEngine VSEngine;

VSEngine *vs_engine_create(void);
void vs_engine_destroy(VSEngine *e);
/// Starts capture from `inputDevice`, processing, and playback to `outputDevice`.
/// `monitorDevice` may be 0 (kAudioObjectUnknown) for no monitoring.
/// Returns 0 (noErr) or an OSStatus; see vs_engine_last_error().
int32_t vs_engine_start(VSEngine *e, uint32_t inputDevice, uint32_t outputDevice,
                        uint32_t monitorDevice, uint32_t bufferFrames, bool lowLatency);
void vs_engine_stop(VSEngine *e);
bool vs_engine_is_running(const VSEngine *e);
void vs_engine_set_param(VSEngine *e, VSParam p, float value);
float vs_engine_meter(const VSEngine *e, VSMeter m);
const char *vs_engine_last_error(const VSEngine *e);

#ifdef __cplusplus
}
#endif

#endif
