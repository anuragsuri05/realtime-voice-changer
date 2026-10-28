// Offline verification of the VoiceCore DSP chain.
//   swift run -c release vscheck [output-dir]
// Synthesises a male-range voiced signal with formants + background noise, runs
// the chain and checks: output pitch, formant shift independence, latency,
// noise reduction and real-time CPU factor. Writes WAVs so you can listen.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "VoiceCore.h"

#define FS 48000.0
static int failures = 0;

static void check(int ok, const char *what, double got, double lo, double hi) {
    printf("  [%s] %-44s %10.3f   (expected %.3f … %.3f)\n", ok ? "PASS" : "FAIL", what, got, lo, hi);
    if (!ok) failures++;
}

static uint32_t rng = 12345;
static float frand(void) {
    rng = rng * 1664525u + 1013904223u;
    return (float)((int32_t)rng) / 2147483648.f;
}

typedef struct { double b0, b1, b2, a1, a2, z1, z2; } Res;
static void res_init(Res *r, double f, double bw) {
    double R = exp(-M_PI * bw / FS), th = 2 * M_PI * f / FS;
    r->a1 = -2 * R * cos(th);
    r->a2 = R * R;
    r->b0 = 1 - R;
    r->b1 = 0;
    r->b2 = 0;
    r->z1 = r->z2 = 0;
}
static double res_tick(Res *r, double x) {
    double y = r->b0 * x + r->z1;
    r->z1 = r->b1 * x - r->a1 * y + r->z2;
    r->z2 = r->b2 * x - r->a2 * y;
    return y;
}

/// Male-like vowel: glottal pulse train through three formant resonators (/a/).
static void synth_voice(float *out, int n, double f0, double glide, double noiseDb, int withGaps) {
    Res f1, f2, f3;
    res_init(&f1, 700, 90);
    res_init(&f2, 1220, 110);
    res_init(&f3, 2600, 160);
    double ph = 0, nz = pow(10, noiseDb / 20);
    for (int i = 0; i < n; i++) {
        double t = i / FS;
        double f = f0 * (1 + glide * sin(2 * M_PI * 0.7 * t)) * (1 + 0.004 * sin(2 * M_PI * 5.5 * t));
        ph += f / FS;
        if (ph >= 1) ph -= 1;
        // Rosenberg-ish glottal flow derivative
        double g = ph < 0.6 ? sin(M_PI * ph / 0.6) * sin(M_PI * ph / 0.6) : 0;
        double dg = ph < 0.6 ? (ph < 0.45 ? 1.0 : -3.0) * g : 0;
        double v = res_tick(&f1, dg) * 1.0 + res_tick(&f2, dg) * 0.6 + res_tick(&f3, dg) * 0.35;
        double env = 1;
        if (withGaps) {  // 0.6 s talk, 0.4 s pause
            double m = fmod(t, 1.0);
            env = m < 0.6 ? fmin(1, fmin(m, 0.6 - m) / 0.02) : 0;
        }
        out[i] = (float)(v * 6.0 * env + nz * frand());
    }
    float peak = 0;
    for (int i = 0; i < n; i++) peak = fmaxf(peak, fabsf(out[i]));
    float g = 0.5f / peak;
    for (int i = 0; i < n; i++) out[i] *= g;
}

/// Median F0 via normalised autocorrelation over voiced 40 ms frames.
static double estimate_f0(const float *x, int n, double fmin, double fmax) {
    int W = (int)(0.04 * FS), hop = W / 2, lagMin = (int)(FS / fmax), lagMax = (int)(FS / fmin);
    double vals[4096];
    int nv = 0;
    for (int s = 0; s + W + lagMax < n && nv < 4096; s += hop) {
        double e0 = 0;
        for (int i = 0; i < W; i++) e0 += x[s + i] * x[s + i];
        if (e0 / W < 1e-5) continue;
        static double rr[4096];
        double best = 0;
        int bl = 0;
        for (int L = lagMin; L <= lagMax; L++) {
            double c = 0, e1 = 0;
            for (int i = 0; i < W; i++) {
                c += x[s + i] * x[s + i + L];
                e1 += x[s + i + L] * x[s + i + L];
            }
            rr[L] = c / sqrt(e0 * e1 + 1e-12);
            if (rr[L] > best) best = rr[L];
        }
        // Shortest lag at a local peak within 90% of the best: avoids sub-octave picks.
        for (int L = lagMin + 1; L < lagMax; L++)
            if (rr[L] >= 0.9 * best && rr[L] >= rr[L - 1] && rr[L] >= rr[L + 1]) {
                bl = L;
                break;
            }
        if (best > 0.6 && bl > 0) vals[nv++] = FS / bl;
    }
    if (!nv) return 0;
    for (int i = 0; i < nv; i++)
        for (int j = i + 1; j < nv; j++)
            if (vals[j] < vals[i]) {
                double t = vals[i];
                vals[i] = vals[j];
                vals[j] = t;
            }
    return vals[nv / 2];
}

/// Spectral centroid of the smoothed magnitude spectrum (rough formant position).
static double centroid(const float *x, int n) {
    const int N = 2048;
    double num = 0, den = 0;
    for (int s = 0; s + N < n; s += N / 2) {
        for (int k = 2; k < 400; k++) {  // up to ~9.4 kHz
            double re = 0, im = 0, w = 2 * M_PI * k / N;
            for (int i = 0; i < N; i += 2) {  // cheap 2x-decimated DFT is enough here
                double h = 0.5 - 0.5 * cos(2 * M_PI * i / N);
                re += h * x[s + i] * cos(w * i);
                im -= h * x[s + i] * sin(w * i);
            }
            double mag = sqrt(re * re + im * im);
            num += mag * k * FS / N;
            den += mag;
        }
    }
    return den > 0 ? num / den : 0;
}

static double rms(const float *x, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += x[i] * x[i];
    return sqrt(s / (n > 0 ? n : 1));
}

static void write_wav(const char *dir, const char *name, const float *x, int n) {
    if (!dir) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t dataBytes = (uint32_t)n * 2, sr = (uint32_t)FS, byteRate = sr * 2, sz = 36 + dataBytes, fmtLen = 16;
    uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f);
    fwrite(&sz, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtLen, 4, 1, f);
    fwrite(&pcm, 2, 1, f);
    fwrite(&ch, 2, 1, f);
    fwrite(&sr, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&dataBytes, 4, 1, f);
    for (int i = 0; i < n; i++) {
        float v = x[i] > 1 ? 1 : (x[i] < -1 ? -1 : x[i]);
        int16_t s = (int16_t)lrintf(v * 32767);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    printf("  wrote %s\n", path);
}

static VSChain *make_chain(float pitch, float formant, int ns, int effect) {
    VSChain *c = vs_chain_create(FS, 512, false);
    vs_chain_set_param(c, VSParamPitchSemitones, pitch);
    vs_chain_set_param(c, VSParamFormantSemitones, formant);
    vs_chain_set_param(c, VSParamIntonation, 1.0f);
    vs_chain_set_param(c, VSParamBreathiness, 0.f);
    vs_chain_set_param(c, VSParamBrightnessDb, 0.f);
    vs_chain_set_param(c, VSParamNoiseSuppression, (float)ns);
    vs_chain_set_param(c, VSParamEffectEnabled, (float)effect);
    vs_chain_reset(c);
    return c;
}

static void run(VSChain *c, const float *in, float *out, int n) {
    for (int i = 0; i < n; i += 128) vs_chain_process(c, in + i, out + i, n - i < 128 ? n - i : 128);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : NULL;
    const int n = (int)(4 * FS);
    float *in = calloc(n, sizeof(float)), *out = calloc(n, sizeof(float)), *ref = calloc(n, sizeof(float));
    const int skip = (int)(0.5 * FS);  // ignore start-up

    printf("VoiceCore offline checks @ %.0f Hz\n", FS);

    // 1. Pitch: 115 Hz male vowel, +8 semitones → 182.6 Hz.
    synth_voice(in, n, 115, 0.0, -70, 0);
    VSChain *c = make_chain(8, 3, 0, 1);
    int lat = vs_chain_latency_samples(c);
    printf("\nAlgorithmic latency: %d samples = %.1f ms\n", lat, 1000.0 * lat / FS);
    run(c, in, out, n);
    double fin = estimate_f0(in + skip, n - skip, 60, 500), fout = estimate_f0(out + skip, n - skip, 60, 500);
    double want = fin * pow(2, 8 / 12.0);
    printf("\n1) Pitch shift (+8 st, formant +3 st)\n");
    check(fabs(fin - 115) < 3, "input F0 (Hz)", fin, 112, 118);
    check(fabs(fout - want) / want < 0.03, "output F0 (Hz)", fout, want * 0.97, want * 1.03);
    write_wav(dir, "1_male_input.wav", in, n);
    write_wav(dir, "1_female_output.wav", out, n);
    vs_chain_destroy(c);

    // 2. Formant shift is independent of pitch.
    printf("\n2) Formant control independent of pitch\n");
    c = make_chain(8, 0, 0, 1);
    run(c, in, ref, n);
    vs_chain_destroy(c);
    c = make_chain(8, 4, 0, 1);
    run(c, in, out, n);
    vs_chain_destroy(c);
    double c0 = centroid(ref + skip, n - skip), c1 = centroid(out + skip, n - skip);
    double f0a = estimate_f0(ref + skip, n - skip, 60, 500), f0b = estimate_f0(out + skip, n - skip, 60, 500);
    check(c1 / c0 > 1.12 && c1 / c0 < 1.45, "centroid ratio formant +4 st vs 0", c1 / c0, 1.12, 1.45);
    check(fabs(f0a - f0b) / f0a < 0.03, "F0 unchanged by formant (ratio)", f0b / f0a, 0.97, 1.03);

    // 3. Gliding intonation is tracked (pitch follows the contour).
    printf("\n3) Tracks intonation glide 95–135 Hz\n");
    synth_voice(in, n, 115, 0.17, -70, 0);
    c = make_chain(7, 2, 0, 1);
    run(c, in, out, n);
    vs_chain_destroy(c);
    double r = 0;
    int cnt = 0, W = (int)(0.2 * FS);
    for (int s = skip; s + W < n - 4000; s += W) {
        double a = estimate_f0(in + s, W, 60, 500), b = estimate_f0(out + s + lat, W, 60, 500);
        if (a > 0 && b > 0) {
            r += fabs(b / a - pow(2, 7 / 12.0));
            cnt++;
        }
    }
    double meanErr = cnt ? r / cnt : 1;
    check(meanErr < 0.06, "mean |ratio - 2^(7/12)| per 200 ms", meanErr, 0, 0.06);
    write_wav(dir, "3_glide_input.wav", in, n);
    write_wav(dir, "3_glide_output.wav", out, n);

    // 4. Bypass path is a pure delay of exactly the reported latency.
    printf("\n4) OFF = natural voice, latency-aligned\n");
    c = make_chain(8, 3, 0, 0);
    run(c, in, out, n);
    vs_chain_destroy(c);
    int bestLag = -1;
    double bestC = -1;
    for (int L = lat - 50; L <= lat + 50; L++) {
        double cc = 0;
        for (int i = skip; i < skip + 20000; i++) cc += in[i] * out[i + L];
        if (cc > bestC) {
            bestC = cc;
            bestLag = L;
        }
    }
    // ±4: the 70 Hz rumble filter's phase response shifts the correlation peak slightly
    check(abs(bestLag - lat) <= 4, "measured dry delay (samples)", bestLag, lat - 4, lat + 4);

    // 5. Noise suppression: speech with pauses in -40 dBFS noise.
    printf("\n5) Noise suppression\n");
    synth_voice(in, n, 115, 0.05, -40, 1);
    c = make_chain(8, 3, 0, 1);
    run(c, in, ref, n);
    vs_chain_destroy(c);
    c = make_chain(8, 3, 1, 1);
    vs_chain_set_param(c, VSParamNoiseStrength, 0.7f);
    run(c, in, out, n);
    vs_chain_destroy(c);
    // pause region of the 2nd second (t = 1.65 … 1.95 s), shifted by latency
    int ps = (int)(1.65 * FS) + lat, pn = (int)(0.3 * FS);
    int vs0 = (int)(1.15 * FS) + lat, vn = (int)(0.3 * FS);
    double noiseOff = rms(ref + ps, pn), noiseOn = rms(out + ps, pn);
    double voiceOff = rms(ref + vs0, vn), voiceOn = rms(out + vs0, vn);
    double nr = 20 * log10(noiseOff / noiseOn), vl = 20 * log10(voiceOff / voiceOn);
    check(nr > 12, "noise floor reduction in pauses (dB)", nr, 12, 60);
    check(vl < 3, "speech level loss (dB)", vl, -3, 3);
    write_wav(dir, "5_noisy_input.wav", in, n);
    write_wav(dir, "5_denoised_female.wav", out, n);

    // 5b. Regression: the mic delivers exact digital silence when it opens (Bluetooth,
    //     virtual devices, permission prompts). Noise suppression must still engage.
    printf("\n5b) Noise suppression after start-up silence\n");
    {
        int lead = (int)(0.5 * FS);
        float *in2 = calloc(n, sizeof(float));
        synth_voice(in2 + lead, n - lead, 115, 0.05, -40, 1);
        uint32_t r2 = 7;
        for (int i = lead; i < n; i++) {  // add audible room noise (-50 dBFS)
            r2 = r2 * 1664525u + 1013904223u;
            in2[i] += 0.005f * (float)((int32_t)r2) / 2147483648.f;
        }
        c = make_chain(8, 3, 0, 1);
        run(c, in2, ref, n);
        vs_chain_destroy(c);
        c = make_chain(8, 3, 1, 1);
        vs_chain_set_param(c, VSParamNoiseStrength, 0.7f);
        run(c, in2, out, n);
        vs_chain_destroy(c);
        // last pause in the file: t = lead + 2.65 … 2.95 s
        int ps2 = lead + (int)(2.65 * FS) + lat, pn2 = (int)(0.3 * FS);
        double nr2 = 20 * log10(rms(ref + ps2, pn2) / rms(out + ps2, pn2));
        check(nr2 > 12, "noise reduction despite 0.5 s digital silence (dB)", nr2, 12, 60);
        free(in2);
    }

    // 6. Real-time factor.
    printf("\n6) CPU\n");
    c = make_chain(8, 3, 1, 1);
    vs_chain_set_param(c, VSParamBreathiness, 0.3f);
    clock_t t0 = clock();
    for (int rep = 0; rep < 5; rep++) run(c, in, out, n);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    double rtf = secs / (5.0 * n / FS);
    vs_chain_destroy(c);
    check(rtf < 0.10, "CPU fraction of real time (1 core)", rtf, 0, 0.10);

    printf("\n%s (%d failure%s)\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", failures,
           failures == 1 ? "" : "s");
    free(in);
    free(out);
    free(ref);
    return failures ? 1 : 0;
}
