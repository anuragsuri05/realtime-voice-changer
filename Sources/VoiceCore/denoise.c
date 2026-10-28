#include "dsp.h"

#include <stdlib.h>
#include <string.h>

static float *zalloc(int n) { return (float *)calloc((size_t)n, sizeof(float)); }

bool vs_denoise_init(VSDenoise *d, double fs, int N) {
    memset(d, 0, sizeof(*d));
    d->N = N;
    d->hop = N / 2;
    d->half = N / 2;
    d->log2n = (int)lrint(log2((double)N));
    d->fft = vDSP_create_fftsetup(d->log2n, kFFTRadix2);
    if (!d->fft) return false;
    int bins = d->half + 1;
    d->win = zalloc(N);
    d->inBuf = zalloc(N);
    d->outAcc = zalloc(N);
    d->outQ = zalloc(d->hop);
    d->frame = zalloc(N);
    d->re = zalloc(d->half);
    d->im = zalloc(d->half);
    d->P = zalloc(bins);
    d->S = zalloc(bins);
    d->noise = zalloc(bins);
    d->gain = zalloc(bins);
    d->gainPrev = zalloc(bins);
    d->snrPrev = zalloc(bins);
    d->gainTmp = zalloc(bins);
    d->curMin = zalloc(bins);
    d->subMin = zalloc(bins * VS_NS_SUBWIN);
    // Periodic sqrt-Hann: analysis * synthesis = Hann, which sums to 1 at 50% overlap.
    for (int i = 0; i < N; i++) d->win[i] = sqrtf(0.5f - 0.5f * cosf(2.f * (float)M_PI * i / N));
    double fps = fs / d->hop;
    d->learnFrames = (int)(0.25 * fps);
    d->riseFactor = powf(10.f, 3.f / 10.f / (float)fps);  // noise floor may rise 3 dB/s
    d->subLen = (int)lround(0.5 * fps);                     // 4 × 0.5 s = 2 s minimum window
    vs_denoise_reset(d);
    return true;
}

void vs_denoise_reset(VSDenoise *d) {
    int bins = d->half + 1;
    memset(d->inBuf, 0, sizeof(float) * d->N);
    memset(d->outAcc, 0, sizeof(float) * d->N);
    memset(d->outQ, 0, sizeof(float) * d->hop);
    memset(d->S, 0, sizeof(float) * bins);
    memset(d->noise, 0, sizeof(float) * bins);
    memset(d->snrPrev, 0, sizeof(float) * bins);
    for (int k = 0; k < bins; k++) d->gain[k] = d->gainPrev[k] = 1.f;
    for (int k = 0; k < bins; k++) d->curMin[k] = INFINITY;
    for (int k = 0; k < bins * VS_NS_SUBWIN; k++) d->subMin[k] = INFINITY;
    d->subPos = 0;
    d->pos = 0;
    d->frames = 0;
    d->mix = 0.f;
    d->attenDb = 0.f;
}

void vs_denoise_free(VSDenoise *d) {
    if (d->fft) vDSP_destroy_fftsetup(d->fft);
    float *bufs[] = {d->win, d->inBuf, d->outAcc, d->outQ, d->frame, d->re, d->im, d->P,
                     d->S, d->noise, d->gain, d->gainPrev, d->snrPrev, d->gainTmp, d->curMin, d->subMin};
    for (size_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); i++) free(bufs[i]);
    memset(d, 0, sizeof(*d));
}

static void process_frame(VSDenoise *d, bool enabled, float strength) {
    const int N = d->N, half = d->half, bins = half + 1;
    DSPSplitComplex sc = {d->re, d->im};

    vDSP_vmul(d->inBuf, 1, d->win, 1, d->frame, 1, (vDSP_Length)N);
    vDSP_ctoz((const DSPComplex *)d->frame, 2, &sc, 1, (vDSP_Length)half);
    vDSP_fft_zrip(d->fft, &sc, 1, (vDSP_Length)d->log2n, FFT_FORWARD);

    // Power spectrum. zrip packs DC in re[0] and Nyquist in im[0].
    float *P = d->P;
    P[0] = d->re[0] * d->re[0];
    P[half] = d->im[0] * d->im[0];
    for (int k = 1; k < half; k++) P[k] = d->re[k] * d->re[k] + d->im[k] * d->im[k];

    // Noise estimate. Smoothed power S per bin, then:
    //  - initial learning: average of the first non-silent frames;
    //  - continuous tracking: follow S down quickly, creep up slowly (3 dB/s);
    //  - minimum statistics: never sit below the bias-corrected minimum of S over the
    //    last ~2 s, so the estimate recovers from zero (digital silence at start-up)
    //    or from a sudden rise in background noise.
    const float tiny = 1e-12f;
    float framePow = 0.f;
    for (int k = 0; k < bins; k++) framePow += P[k];
    const bool silent = framePow < 1e-10f;  // exact/near digital silence: nothing to learn
    const float bias = 1.5f;
    for (int k = 0; k < bins; k++) {
        d->S[k] = d->frames == 0 ? P[k] : 0.8f * d->S[k] + 0.2f * P[k];
        if (d->S[k] < d->curMin[k]) d->curMin[k] = d->S[k];
    }
    if (++d->subPos >= d->subLen) {  // close the current sub-window
        d->subPos = 0;
        memmove(d->subMin, d->subMin + bins, sizeof(float) * bins * (VS_NS_SUBWIN - 1));
        memcpy(d->subMin + bins * (VS_NS_SUBWIN - 1), d->curMin, sizeof(float) * bins);
        for (int k = 0; k < bins; k++) d->curMin[k] = INFINITY;
    }
    if (!silent && d->frames < d->learnFrames) {
        float a = 1.f / (float)(d->frames + 1);
        for (int k = 0; k < bins; k++) d->noise[k] = (1.f - a) * d->noise[k] + a * P[k];
        d->frames++;
    } else if (!silent || d->frames >= d->learnFrames) {
        for (int k = 0; k < bins; k++) {
            float wmin = d->curMin[k];
            for (int u = 0; u < VS_NS_SUBWIN; u++) wmin = fminf(wmin, d->subMin[u * bins + k]);
            float msFloor = isfinite(wmin) ? wmin * bias : 0.f;
            if (d->S[k] < d->noise[k])
                d->noise[k] = 0.7f * d->noise[k] + 0.3f * d->S[k];
            else
                d->noise[k] *= d->riseFactor;
            if (d->noise[k] < msFloor) d->noise[k] += 0.1f * (msFloor - d->noise[k]);
        }
    }

    // Decision-directed a-priori SNR → Wiener gain with over-subtraction and a floor.
    strength = vs_clampf(strength, 0.f, 1.f);
    const float overSub = 1.f + 1.5f * strength;
    const float floorG = vs_db2lin(-(6.f + 24.f * strength));
    const float alphaDD = 0.96f;
    for (int k = 0; k < bins; k++) {
        float nk = fmaxf(d->noise[k] * overSub, tiny);
        float post = P[k] / nk;
        float prio = alphaDD * d->gainPrev[k] * d->gainPrev[k] * d->snrPrev[k] +
                     (1.f - alphaDD) * fmaxf(post - 1.f, 0.f);
        float g = prio / (1.f + prio);
        d->snrPrev[k] = post;
        d->gainPrev[k] = g;
        d->gainTmp[k] = fmaxf(g, floorG);
    }
    // Limit how fast gains may fall between frames (keeps word endings natural
    // and masks musical noise). No smoothing across frequency: with resolved
    // harmonics that would pull every harmonic peak down toward its gaps.
    float target = enabled ? 1.f : 0.f;
    d->mix += vs_clampf(target - d->mix, -0.08f, 0.08f);
    float inPow = 0.f, outPow = 0.f;
    for (int k = 0; k < bins; k++) {
        float g = fmaxf(d->gainTmp[k], d->gain[k] * 0.55f);
        d->gain[k] = g;
        float ge = 1.f - d->mix * (1.f - g);
        inPow += P[k];
        outPow += P[k] * ge * ge;
        P[k] = ge;  // reuse as the applied gain
    }
    d->attenDb = inPow > 1e-9f ? fmaxf(0.f, 10.f * log10f(inPow / fmaxf(outPow, 1e-12f))) : 0.f;

    d->re[0] *= P[0];
    d->im[0] *= P[half];
    for (int k = 1; k < half; k++) {
        d->re[k] *= P[k];
        d->im[k] *= P[k];
    }

    vDSP_fft_zrip(d->fft, &sc, 1, (vDSP_Length)d->log2n, FFT_INVERSE);
    vDSP_ztoc(&sc, 1, (DSPComplex *)d->frame, 2, (vDSP_Length)half);
    float scale = 1.f / (2.f * N);
    vDSP_vsmul(d->frame, 1, &scale, d->frame, 1, (vDSP_Length)N);
    vDSP_vmul(d->frame, 1, d->win, 1, d->frame, 1, (vDSP_Length)N);
    vDSP_vadd(d->outAcc, 1, d->frame, 1, d->outAcc, 1, (vDSP_Length)N);

    // Emit the finished hop and slide everything along.
    memcpy(d->outQ, d->outAcc, sizeof(float) * d->hop);
    memmove(d->outAcc, d->outAcc + d->hop, sizeof(float) * (N - d->hop));
    memset(d->outAcc + (N - d->hop), 0, sizeof(float) * d->hop);
    memmove(d->inBuf, d->inBuf + d->hop, sizeof(float) * (N - d->hop));
}

void vs_denoise_process(VSDenoise *d, float *buf, int n, bool enabled, float strength) {
    const int base = d->N - d->hop;
    for (int i = 0; i < n; i++) {
        d->inBuf[base + d->pos] = buf[i];
        buf[i] = d->outQ[d->pos];
        if (++d->pos == d->hop) {
            d->pos = 0;
            process_frame(d, enabled, strength);
        }
    }
}
