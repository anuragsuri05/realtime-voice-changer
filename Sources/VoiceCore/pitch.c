#include "dsp.h"

#include <stdlib.h>
#include <string.h>

static float *zalloc(int n) { return (float *)calloc((size_t)n, sizeof(float)); }

// MARK: - YIN

bool vs_yin_init(VSYin *y, double fs, float minF0, float maxF0) {
    memset(y, 0, sizeof(*y));
    y->M = (int)lround(fs / 11025.0);
    if (y->M < 1) y->M = 1;
    y->fd = fs / y->M;
    double lpHz = fmin(1200.0, 0.4 * y->fd);
    vs_biquad_lpf(&y->lp1, fs, lpHz, 0.5412);  // 4th-order Butterworth
    vs_biquad_lpf(&y->lp2, fs, lpHz, 1.3066);
    y->tauMin = (int)floor(y->fd / maxF0);
    y->tauMax = (int)ceil(y->fd / minF0);
    y->W = y->tauMax;
    y->hop = (int)lround(y->fd * 0.005);  // re-estimate every 5 ms
    int len = 1;
    while (len < 2 * (y->W + y->tauMax)) len <<= 1;
    y->mask = len - 1;
    y->buf = zalloc(len);
    y->win = zalloc(y->W + y->tauMax + 1);
    y->d = zalloc(y->tauMax + 2);
    y->dp = zalloc(y->tauMax + 2);
    y->silence = vs_db2lin(-52.f);
    vs_yin_reset(y);
    return y->buf && y->win && y->d && y->dp;
}

void vs_yin_reset(VSYin *y) {
    memset(y->buf, 0, sizeof(float) * (y->mask + 1));
    y->lp1.z1 = y->lp1.z2 = y->lp2.z1 = y->lp2.z2 = 0;
    y->count = 0;
    y->phase = y->sinceHop = 0;
    y->period = (float)(y->M * y->fd / 120.0);
    y->voiced = false;
    y->histN = 0;
    y->voicedRun = y->unvoicedRun = 0;
    y->aperiodicity = 1.f;
}

void vs_yin_free(VSYin *y) {
    free(y->buf);
    free(y->win);
    free(y->d);
    free(y->dp);
    memset(y, 0, sizeof(*y));
}

static float median3(const float *h, int n) {
    if (n == 1) return h[0];
    if (n == 2) return 0.5f * (h[0] + h[1]);
    float a = h[0], b = h[1], c = h[2];
    if (a > b) { float t = a; a = b; b = t; }
    if (b > c) { b = c; }
    return a > b ? a : b;
}

static void yin_analyze(VSYin *y) {
    const int W = y->W, L = W + y->tauMax;
    for (int i = 0; i < L; i++) y->win[i] = y->buf[(y->count - L + i) & y->mask];

    float energy = 0.f;
    vDSP_svesq(y->win, 1, &energy, (vDSP_Length)L);
    float rms = sqrtf(energy / L);

    // Difference function and cumulative-mean-normalised difference.
    y->dp[0] = 1.f;
    float running = 0.f;
    for (int tau = 1; tau <= y->tauMax; tau++) {
        float s = 0.f;
        const float *a = y->win, *b = y->win + tau;
        for (int j = 0; j < W; j++) {
            float df = a[j] - b[j];
            s += df * df;
        }
        y->d[tau] = s;
        running += s;
        y->dp[tau] = running > 0.f ? s * tau / running : 1.f;
    }

    const float thresh = 0.15f;
    int tau = -1;
    for (int t = y->tauMin; t <= y->tauMax; t++) {
        if (y->dp[t] < thresh) {
            while (t + 1 <= y->tauMax && y->dp[t + 1] < y->dp[t]) t++;
            tau = t;
            break;
        }
    }
    if (tau < 0) {
        tau = y->tauMin;
        for (int t = y->tauMin + 1; t <= y->tauMax; t++)
            if (y->dp[t] < y->dp[tau]) tau = t;
    }
    float aper = y->dp[tau];
    float shift = 0.f;
    if (tau > y->tauMin && tau < y->tauMax) {
        float a = y->dp[tau - 1], b = y->dp[tau], c = y->dp[tau + 1];
        float den = a - 2.f * b + c;
        if (fabsf(den) > 1e-9f) shift = vs_clampf(0.5f * (a - c) / den, -1.f, 1.f);
    }
    float period = (tau + shift) * y->M;
    y->aperiodicity = aper;

    bool raw = aper < 0.22f && rms > y->silence;
    if (raw) {
        y->voicedRun++;
        y->unvoicedRun = 0;
        if (y->histN < 3) {
            y->hist[y->histN++] = period;
        } else {
            y->hist[0] = y->hist[1];
            y->hist[1] = y->hist[2];
            y->hist[2] = period;
        }
        y->period = median3(y->hist, y->histN);
    } else {
        y->unvoicedRun++;
        y->voicedRun = 0;
    }
    // Fast onset on a very clean detection (so syllable starts get shifted too),
    // otherwise wait for two in a row; release slowly.
    if (!y->voiced && (y->voicedRun >= 2 || (y->voicedRun == 1 && aper < 0.10f))) y->voiced = true;
    if (y->voiced && y->unvoicedRun >= 3) {
        y->voiced = false;
        y->histN = 0;
    }
}

void vs_yin_push(VSYin *y, float s) {
    float v = vs_biquad_tick(&y->lp2, vs_biquad_tick(&y->lp1, s));
    if (++y->phase < y->M) return;
    y->phase = 0;
    y->buf[y->count & y->mask] = v;
    y->count++;
    if (++y->sinceHop >= y->hop) {
        y->sinceHop = 0;
        if (y->count >= y->W + y->tauMax) yin_analyze(y);
    }
}

// MARK: - PSOLA pitch / formant shifter

bool vs_pitch_init(VSPitch *p, double fs, float minF0) {
    memset(p, 0, sizeof(*p));
    p->fs = fs;
    p->Pmax = (int)ceil(fs / minF0);
    p->Tu = (int)lround(fs * 0.005);
    // A grain placed at synthesis time ts needs input up to ts + Pmax, and its
    // left edge reaches back (Pmax / minimum formant ratio). See place_grains().
    p->D = p->Pmax + (int)ceil(p->Pmax / VS_FORMANT_MIN) + 2;
    p->x = zalloc(VS_RING);
    p->acc = zalloc(VS_RING);
    p->wsum = zalloc(VS_RING);
    p->tmpA = zalloc(p->Pmax + 8);
    p->tmpB = zalloc(2 * p->Pmax + 8);
    if (!vs_yin_init(&p->yin, fs, minF0, 500.f)) return false;
    p->pitchRatio = p->formantRatio = p->intonation = 1.f;
    vs_pitch_reset(p);
    return p->x && p->acc && p->wsum && p->tmpA && p->tmpB;
}

void vs_pitch_reset(VSPitch *p) {
    memset(p->x, 0, sizeof(float) * VS_RING);
    memset(p->acc, 0, sizeof(float) * VS_RING);
    memset(p->wsum, 0, sizeof(float) * VS_RING);
    vs_yin_reset(&p->yin);
    p->nIn = 0;
    p->markCount = p->synIdx = 0;
    p->lastMarkPos = 0;
    p->lastMarkVoiced = false;
    p->nextTs = p->Pmax;
    p->logMean = logf(120.f);
    p->inF0 = p->outF0 = 0.f;
}

void vs_pitch_free(VSPitch *p) {
    vs_yin_free(&p->yin);
    free(p->x);
    free(p->acc);
    free(p->wsum);
    free(p->tmpA);
    free(p->tmpB);
    memset(p, 0, sizeof(*p));
}

static inline float ring_cubic(const float *x, double pos) {
    double fl = floor(pos);
    int64_t i = (int64_t)fl;
    float t = (float)(pos - fl);
    float xm1 = x[(i - 1) & VS_RMASK], x0 = x[i & VS_RMASK];
    float x1 = x[(i + 1) & VS_RMASK], x2 = x[(i + 2) & VS_RMASK];
    float c1 = 0.5f * (x1 - xm1);
    float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
    float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

static void ring_copy(const float *x, int64_t start, int n, float *dst) {
    for (int i = 0; i < n; i++) dst[i] = x[(start + i) & VS_RMASK];
}

/// Picks the position within pred±search whose preceding period best matches
/// the period preceding the previous mark (keeps grains phase-consistent).
static int64_t align_mark(VSPitch *p, int64_t last, int64_t pred, int T, int search) {
    ring_copy(p->x, last - T, T, p->tmpA);
    int span = T + 2 * search;
    ring_copy(p->x, pred - search - T, span, p->tmpB);
    int64_t best = pred;
    float bestScore = -INFINITY;
    for (int off = 0; off <= 2 * search; off++) {
        float dot, en;
        vDSP_dotpr(p->tmpA, 1, p->tmpB + off, 1, &dot, (vDSP_Length)T);
        vDSP_svesq(p->tmpB + off, 1, &en, (vDSP_Length)T);
        float score = dot / sqrtf(en + 1e-9f);
        if (score > bestScore) {
            bestScore = score;
            best = pred - search + off;
        }
    }
    return best;
}

static void create_marks(VSPitch *p) {
    for (;;) {
        bool v = p->yin.voiced;
        float T = v ? p->yin.period : (float)p->Tu;
        T = vs_clampf(T, 16.f, (float)(p->Pmax - 4));
        int Ti = (int)lrintf(T);
        int search = (v && p->lastMarkVoiced) ? Ti / 4 : 0;
        int64_t pred = p->lastMarkPos + Ti;
        if (pred + search + 1 > p->nIn) break;
        int64_t pos = search > 0 ? align_mark(p, p->lastMarkPos, pred, Ti, search) : pred;
        VSMark *m = &p->marks[p->markCount % VS_MAX_MARKS];
        m->pos = pos;
        m->period = T;
        m->voiced = v;
        p->markCount++;
        p->lastMarkPos = pos;
        p->lastMarkVoiced = v;
    }
}

static void place_grains(VSPitch *p) {
    const double fs = p->fs;
    const float f = vs_clampf(p->formantRatio, VS_FORMANT_MIN, VS_FORMANT_MAX);
    while (p->markCount > 0 && p->nextTs <= (double)(p->nIn - p->Pmax)) {
        int64_t ts = llround(p->nextTs);
        while (p->synIdx + 1 < p->markCount && p->marks[(p->synIdx + 1) % VS_MAX_MARKS].pos <= ts)
            p->synIdx++;
        const VSMark m = p->marks[p->synIdx % VS_MAX_MARKS];
        if (m.pos > ts) {  // only before the first mark exists in the past
            p->nextTs += p->Tu;
            continue;
        }
        float T, Ts;
        if (m.voiced) {
            T = m.period;
            float lf0 = logf((float)fs / T);
            p->logMean += 0.004f * (lf0 - p->logMean);
            float lout = logf(p->pitchRatio) + p->logMean + p->intonation * (lf0 - p->logMean);
            float f0out = vs_clampf(expf(lout), 60.f, 1000.f);
            Ts = (float)fs / f0out;
            p->inF0 = (float)fs / T;
            p->outF0 = f0out;
        } else {
            T = (float)p->Tu;
            Ts = (float)p->Tu;
            p->inF0 = p->outF0 = 0.f;
        }
        float half = T / f;
        half = fmaxf(half, 0.75f * Ts);                 // no gaps when pitching down
        half = fminf(half, (float)(p->Pmax - 4) / f);   // stay inside available input
        int H = (int)half;
        float invHalf = 1.f / half;
        for (int k = -H; k <= H; k++) {
            float w = 0.5f + 0.5f * cosf((float)M_PI * k * invHalf);
            float v = ring_cubic(p->x, (double)m.pos + (double)k * f);
            int idx = (int)((ts + k) & VS_RMASK);
            p->acc[idx] += w * v;
            p->wsum[idx] += w;
        }
        p->nextTs += Ts;
    }
}

void vs_pitch_process(VSPitch *p, const float *in, float *wet, float *dry, int n) {
    for (int i = 0; i < n; i++) {
        float s = in[i];
        p->x[p->nIn & VS_RMASK] = s;
        p->nIn++;
        vs_yin_push(&p->yin, s);
        create_marks(p);
        place_grains(p);

        int64_t r = p->nIn - 1 - p->D;
        if (r < 0) {
            wet[i] = dry[i] = 0.f;
            continue;
        }
        int idx = (int)(r & VS_RMASK);
        float ws = p->wsum[idx];
        wet[i] = p->acc[idx] / fmaxf(ws, 0.2f);
        dry[i] = p->x[idx];
        p->acc[idx] = 0.f;
        p->wsum[idx] = 0.f;
    }
}
