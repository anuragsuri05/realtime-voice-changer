// Internal DSP building blocks. Not part of the public API.
#ifndef VS_DSP_H
#define VS_DSP_H

#include <Accelerate/Accelerate.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

static inline float vs_db2lin(float db) { return powf(10.f, db / 20.f); }
static inline float vs_lin2db(float x) { return 20.f * log10f(fmaxf(x, 1e-9f)); }
static inline float vs_clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

/// Flush denormals to zero on the calling thread (call at the top of each audio callback).
void vs_enable_ftz(void);

// MARK: Biquad (RBJ cookbook, transposed direct form II)

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} VSBiquad;

void vs_biquad_hpf(VSBiquad *b, double fs, double f0, double q);
void vs_biquad_lpf(VSBiquad *b, double fs, double f0, double q);
void vs_biquad_highshelf(VSBiquad *b, double fs, double f0, double gainDb);

static inline float vs_biquad_tick(VSBiquad *b, float x) {
    float y = b->b0 * x + b->z1;
    b->z1 = b->b1 * x - b->a1 * y + b->z2;
    b->z2 = b->b2 * x - b->a2 * y;
    return y;
}

// MARK: Spectral noise suppressor (STFT, decision-directed Wiener gain,
//       continuous minimum-tracking noise estimate). Latency = N samples.

#define VS_NS_SUBWIN 4

typedef struct {
    int N, hop, half, log2n;
    FFTSetup fft;
    float *win;      // sqrt-Hann analysis & synthesis window (N)
    float *inBuf;    // sliding analysis buffer (N)
    float *outAcc;   // overlap-add accumulator (N)
    float *outQ;     // finished output for the current hop (hop)
    float *frame;    // scratch (N)
    float *re, *im;  // split complex (N/2)
    float *P, *S, *noise, *gain, *gainPrev, *snrPrev, *gainTmp;  // per bin (half+1)
    int pos;         // samples consumed in the current hop
    int frames;      // non-silent frames processed (for initial noise learning)
    int learnFrames;
    float riseFactor;
    // Minimum statistics: per-bin minimum of S over VS_NS_SUBWIN sub-windows of subLen frames.
    // Lets the noise floor climb back from any state (e.g. after digital silence at start-up).
    float *curMin, *subMin;
    int subLen, subPos;
    float mix;       // 0 = gains forced to 1 (disabled), ramps per frame
    float attenDb;   // meter
} VSDenoise;

bool vs_denoise_init(VSDenoise *d, double fs, int N);
void vs_denoise_free(VSDenoise *d);
void vs_denoise_reset(VSDenoise *d);
/// In place. Always runs (so latency is constant); `enabled=false` fades gains to unity.
void vs_denoise_process(VSDenoise *d, float *buf, int n, bool enabled, float strength);

// MARK: YIN pitch tracker on a decimated, low-passed copy of the signal.

typedef struct {
    int M;                 // decimation factor
    double fd;             // decimated rate
    VSBiquad lp1, lp2;
    float *buf;            // decimated history ring
    int mask;
    int64_t count;
    int phase, hop, sinceHop;
    int tauMin, tauMax, W;
    float *win, *d, *dp;
    float period;          // full-rate samples (smoothed)
    bool voiced;
    float hist[3];
    int histN;
    int voicedRun, unvoicedRun;
    float silence;         // RMS below this is silence
    float aperiodicity;
} VSYin;

bool vs_yin_init(VSYin *y, double fs, float minF0, float maxF0);
void vs_yin_free(VSYin *y);
void vs_yin_reset(VSYin *y);
void vs_yin_push(VSYin *y, float s);

// MARK: Pitch-synchronous OLA pitch shifter with independent formant shift.
//
// Analysis marks are placed one detected period apart (refined by waveform
// correlation). Synthesis marks are placed one *target* period apart; at each
// synthesis mark the grain around the most recent analysis mark is
// Hann-windowed, resampled by the formant ratio (which scales the spectral
// envelope without touching the harmonic spacing) and overlap-added.
// Unvoiced sound keeps its timing (grains every Tu) and only gets the formant shift.

#define VS_RING 16384
#define VS_RMASK (VS_RING - 1)
#define VS_MAX_MARKS 256

typedef struct {
    int64_t pos;
    float period;
    bool voiced;
} VSMark;

typedef struct {
    double fs;
    float *x, *acc, *wsum;    // VS_RING each
    float *tmpA, *tmpB;       // correlation scratch
    int64_t nIn;
    int Pmax, Tu, D;
    VSYin yin;
    VSMark marks[VS_MAX_MARKS];
    int64_t markCount, synIdx;
    int64_t lastMarkPos;
    bool lastMarkVoiced;
    double nextTs;
    float logMean;            // running mean of log input F0
    float inF0, outF0;        // meters
    // parameters (written by the owner before each process call)
    float pitchRatio, formantRatio, intonation;
} VSPitch;

#define VS_FORMANT_MIN 0.84f
#define VS_FORMANT_MAX 1.50f

bool vs_pitch_init(VSPitch *p, double fs, float minF0);
void vs_pitch_free(VSPitch *p);
void vs_pitch_reset(VSPitch *p);
/// Writes the converted signal to `wet` and the identically delayed input to `dry`.
void vs_pitch_process(VSPitch *p, const float *in, float *wet, float *dry, int n);

#endif
