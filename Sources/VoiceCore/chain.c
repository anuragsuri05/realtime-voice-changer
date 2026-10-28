// Full voice chain: input gain → HPF → noise suppression → gate →
// pitch/formant shift → brightness + breath → wet/dry crossfade → output gain → limiter.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "VoiceCore.h"
#include "dsp.h"

struct VSChain {
    double fs;
    int maxBlock;
    _Atomic float params[VSParamCount];
    _Atomic float meters[VSMeterCount];

    VSBiquad hpf;
    VSDenoise ns;
    VSPitch ps;
    VSBiquad shelf, breathHp, breathLp;
    float shelfDb;

    float gateEnv, gateGain;
    int gateHold;
    float breathEnv;
    float dryPow, wetPow, matchGain;  // keeps converted loudness equal to natural loudness
    uint32_t rng;
    float mix, inGain, outGain, muteGain, limEnv;
    float *tmp, *wet, *dry;
};

static const float kDefaults[VSParamCount] = {
    [VSParamEffectEnabled] = 1.f,
    [VSParamPitchSemitones] = 8.f,
    [VSParamFormantSemitones] = 3.f,
    [VSParamIntonation] = 1.2f,
    [VSParamBreathiness] = 0.25f,
    [VSParamBrightnessDb] = 2.f,
    [VSParamNoiseSuppression] = 1.f,
    [VSParamNoiseStrength] = 0.6f,
    [VSParamGateThresholdDb] = -90.f,
    [VSParamInputGainDb] = 0.f,
    [VSParamOutputGainDb] = 0.f,
    [VSParamMonitorGainDb] = -6.f,
    [VSParamMute] = 0.f,
    [VSParamPassthrough] = 0.f,
};

static inline float P(const VSChain *c, VSParam p) {
    return atomic_load_explicit(&((VSChain *)c)->params[p], memory_order_relaxed);
}
static inline void setMeter(VSChain *c, VSMeter m, float v) {
    atomic_store_explicit(&c->meters[m], v, memory_order_relaxed);
}

VSChain *vs_chain_create(double fs, int maxBlock, bool lowLatency) {
    VSChain *c = (VSChain *)calloc(1, sizeof(VSChain));
    if (!c) return NULL;
    c->fs = fs;
    c->maxBlock = maxBlock > 0 ? maxBlock : 1024;
    for (int i = 0; i < VSParamCount; i++) atomic_init(&c->params[i], kDefaults[i]);
    for (int i = 0; i < VSMeterCount; i++) atomic_init(&c->meters[i], 0.f);

    // ~10.7 ms analysis frames at 44.1/48 kHz (5.3 ms in low-latency mode).
    int N = fs >= 32000 ? 512 : 256;
    if (lowLatency) N /= 2;
    // Lowest input pitch we track; higher floor = shorter grains = less latency.
    float minF0 = lowLatency ? 90.f : 75.f;

    c->tmp = (float *)calloc((size_t)c->maxBlock, sizeof(float));
    c->wet = (float *)calloc((size_t)c->maxBlock, sizeof(float));
    c->dry = (float *)calloc((size_t)c->maxBlock, sizeof(float));
    if (!c->tmp || !c->wet || !c->dry || !vs_denoise_init(&c->ns, fs, N) || !vs_pitch_init(&c->ps, fs, minF0)) {
        vs_chain_destroy(c);
        return NULL;
    }
    vs_biquad_hpf(&c->hpf, fs, 70.0, 0.707);
    vs_biquad_hpf(&c->breathHp, fs, 2500.0, 0.707);
    vs_biquad_lpf(&c->breathLp, fs, fmin(9000.0, 0.45 * fs), 0.707);
    c->shelfDb = kDefaults[VSParamBrightnessDb];
    vs_biquad_highshelf(&c->shelf, fs, 3000.0, c->shelfDb);
    vs_chain_reset(c);
    setMeter(c, VSMeterSampleRate, (float)fs);
    setMeter(c, VSMeterLatencyMs, 1000.f * vs_chain_latency_samples(c) / (float)fs);
    return c;
}

void vs_chain_reset(VSChain *c) {
    vs_denoise_reset(&c->ns);
    vs_pitch_reset(&c->ps);
    VSBiquad *bq[] = {&c->hpf, &c->shelf, &c->breathHp, &c->breathLp};
    for (int i = 0; i < 4; i++) bq[i]->z1 = bq[i]->z2 = 0.f;
    c->gateEnv = 0.f;
    c->gateGain = 1.f;
    c->gateHold = 0;
    c->breathEnv = 0.f;
    c->dryPow = c->wetPow = 0.f;
    c->matchGain = 1.4f;  // typical compensation, refined on the fly
    c->rng = 0x9E3779B9u;
    c->mix = P(c, VSParamEffectEnabled) > 0.5f ? 1.f : 0.f;
    c->inGain = vs_db2lin(P(c, VSParamInputGainDb));
    c->outGain = vs_db2lin(P(c, VSParamOutputGainDb));
    c->muteGain = P(c, VSParamMute) > 0.5f ? 0.f : 1.f;
    c->limEnv = 0.f;
}

void vs_chain_destroy(VSChain *c) {
    if (!c) return;
    vs_denoise_free(&c->ns);
    vs_pitch_free(&c->ps);
    free(c->tmp);
    free(c->wet);
    free(c->dry);
    free(c);
}

void vs_chain_set_param(VSChain *c, VSParam p, float v) {
    if ((int)p < 0 || p >= VSParamCount) return;
    atomic_store_explicit(&c->params[p], v, memory_order_relaxed);
}

float vs_chain_get_param(const VSChain *c, VSParam p) {
    if ((int)p < 0 || p >= VSParamCount) return 0.f;
    return P(c, p);
}

int vs_chain_latency_samples(const VSChain *c) { return c->ns.N + c->ps.D; }

float vs_chain_meter(const VSChain *c, VSMeter m) {
    if ((int)m < 0 || m >= VSMeterCount) return 0.f;
    return atomic_load_explicit(&((VSChain *)c)->meters[m], memory_order_relaxed);
}

static inline float white(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return (float)(int32_t)x * (1.f / 2147483648.f);
}

static void process_block(VSChain *c, const float *in, float *out, int n) {
    const float fs = (float)c->fs;
    const float smooth = 1.f - expf(-1.f / (0.01f * fs));  // 10 ms parameter smoothing
    const float inGainT = vs_db2lin(vs_clampf(P(c, VSParamInputGainDb), -40.f, 30.f));
    const float outGainT = vs_db2lin(vs_clampf(P(c, VSParamOutputGainDb), -40.f, 20.f));
    const float muteT = P(c, VSParamMute) > 0.5f ? 0.f : 1.f;

    float inPeak = 0.f, outPeak = 0.f;

    if (P(c, VSParamPassthrough) > 0.5f) {
        for (int i = 0; i < n; i++) {
            c->outGain += smooth * (outGainT - c->outGain);
            c->muteGain += smooth * (muteT - c->muteGain);
            float y = in[i] * c->outGain * c->muteGain;
            inPeak = fmaxf(inPeak, fabsf(in[i]));
            outPeak = fmaxf(outPeak, fabsf(y));
            out[i] = y;
        }
        setMeter(c, VSMeterInputPeak, inPeak);
        setMeter(c, VSMeterOutputPeak, outPeak);
        return;
    }

    // 1. Input gain + rumble filter.
    for (int i = 0; i < n; i++) {
        c->inGain += smooth * (inGainT - c->inGain);
        float s = in[i] * c->inGain;
        inPeak = fmaxf(inPeak, fabsf(s));
        c->tmp[i] = vs_biquad_tick(&c->hpf, s);
    }

    // 2. Noise suppression.
    vs_denoise_process(&c->ns, c->tmp, n, P(c, VSParamNoiseSuppression) > 0.5f, P(c, VSParamNoiseStrength));

    // 3. Gate (downward expander with hold).
    float gateDb = P(c, VSParamGateThresholdDb);
    if (gateDb > -89.f) {
        const float envA = 1.f - expf(-1.f / (0.005f * fs));
        const float openA = 1.f - expf(-1.f / (0.002f * fs));
        const float closeA = 1.f - expf(-1.f / (0.080f * fs));
        const float thr = vs_db2lin(gateDb);
        const float thr2 = thr * thr;
        const int holdN = (int)(0.06f * fs);
        for (int i = 0; i < n; i++) {
            float s = c->tmp[i];
            c->gateEnv += envA * (s * s - c->gateEnv);
            float target;
            if (c->gateEnv > thr2) {
                c->gateHold = holdN;
                target = 1.f;
            } else if (c->gateHold > 0) {
                c->gateHold--;
                target = 1.f;
            } else {
                target = 0.03f;
            }
            c->gateGain += (target > c->gateGain ? openA : closeA) * (target - c->gateGain);
            c->tmp[i] = s * c->gateGain;
        }
    } else {
        c->gateGain = 1.f;
    }

    // 4. Pitch + formant.
    c->ps.pitchRatio = powf(2.f, vs_clampf(P(c, VSParamPitchSemitones), -6.f, 14.f) / 12.f);
    c->ps.formantRatio = powf(2.f, vs_clampf(P(c, VSParamFormantSemitones), -3.f, 7.f) / 12.f);
    c->ps.intonation = vs_clampf(P(c, VSParamIntonation), 0.3f, 2.f);
    vs_pitch_process(&c->ps, c->tmp, c->wet, c->dry, n);

    // 5. Timbre on the converted voice, crossfade, output stage.
    float bright = vs_clampf(P(c, VSParamBrightnessDb), -12.f, 12.f);
    if (fabsf(bright - c->shelfDb) > 0.01f) {
        c->shelfDb = bright;
        float z1 = c->shelf.z1, z2 = c->shelf.z2;
        vs_biquad_highshelf(&c->shelf, c->fs, 3000.0, bright);
        c->shelf.z1 = z1;
        c->shelf.z2 = z2;
    }
    const float breath = vs_clampf(P(c, VSParamBreathiness), 0.f, 1.f) * 0.30f;
    const float mixT = P(c, VSParamEffectEnabled) > 0.5f ? 1.f : 0.f;
    const float mixStep = 1.f / (0.03f * fs);  // 30 ms on/off crossfade
    const float envAtk = 1.f - expf(-1.f / (0.003f * fs));
    const float envRel = 1.f - expf(-1.f / (0.050f * fs));
    const float limRel = expf(-1.f / (0.120f * fs));
    const float limThr = 0.89f;  // -1 dBFS
    // Overlapping pitch-shifted grains add incoherently, so the converted voice
    // comes out ~3 dB quieter than the input. Track both loudnesses (60 ms) and
    // compensate; adapt only on real speech and hold the gain through pauses.
    const float powA = 1.f - expf(-1.f / (0.06f * fs));
    const float gainA = 1.f - expf(-1.f / (0.04f * fs));
    const float sigFloor = 1e-5f;  // -50 dBFS power

    for (int i = 0; i < n; i++) {
        float d0 = c->dry[i], w0 = c->wet[i];
        c->dryPow += powA * (d0 * d0 - c->dryPow);
        c->wetPow += powA * (w0 * w0 - c->wetPow);
        if (c->dryPow > sigFloor && c->wetPow > sigFloor * 0.1f) {
            float target = vs_clampf(sqrtf(c->dryPow / c->wetPow), 0.5f, 2.5f);
            c->matchGain += gainA * (target - c->matchGain);
        }
        float w = vs_biquad_tick(&c->shelf, w0 * c->matchGain);
        float a = fabsf(w);
        c->breathEnv += (a > c->breathEnv ? envAtk : envRel) * (a - c->breathEnv);
        if (breath > 0.f) {
            float nz = vs_biquad_tick(&c->breathLp, vs_biquad_tick(&c->breathHp, white(&c->rng)));
            w += nz * c->breathEnv * breath;
        }
        if (c->mix < mixT) c->mix = fminf(mixT, c->mix + mixStep);
        else if (c->mix > mixT) c->mix = fmaxf(mixT, c->mix - mixStep);
        // Equal-power crossfade (the two signals are uncorrelated in pitch).
        float gw = sinf(c->mix * (float)M_PI_2), gd = cosf(c->mix * (float)M_PI_2);
        float y = gw * w + gd * c->dry[i];

        c->outGain += smooth * (outGainT - c->outGain);
        c->muteGain += smooth * (muteT - c->muteGain);
        y *= c->outGain * c->muteGain;

        float ay = fabsf(y);
        c->limEnv = ay > c->limEnv ? ay : c->limEnv * limRel + ay * (1.f - limRel);
        if (c->limEnv > limThr) y *= limThr / c->limEnv;
        if (y > 0.98f) y = 0.98f + 0.02f * tanhf((y - 0.98f) * 50.f);
        else if (y < -0.98f) y = -0.98f + 0.02f * tanhf((y + 0.98f) * 50.f);
        outPeak = fmaxf(outPeak, fabsf(y));
        out[i] = y;
    }

    setMeter(c, VSMeterInputPeak, inPeak);
    setMeter(c, VSMeterOutputPeak, outPeak);
    bool v = c->ps.yin.voiced;
    setMeter(c, VSMeterInputF0, v ? (float)c->fs / c->ps.yin.period : 0.f);
    setMeter(c, VSMeterOutputF0, (v && c->mix > 0.5f) ? c->ps.outF0 : (v ? (float)c->fs / c->ps.yin.period : 0.f));
    setMeter(c, VSMeterNoiseReductionDb, c->ns.attenDb);
}

void vs_chain_process(VSChain *c, const float *in, float *out, int n) {
    while (n > 0) {
        int k = n < c->maxBlock ? n : c->maxBlock;
        process_block(c, in, out, k);
        in += k;
        out += k;
        n -= k;
    }
}
