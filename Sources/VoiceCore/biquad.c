#include "dsp.h"

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

void vs_enable_ftz(void) {
#if defined(__x86_64__) || defined(__i386__)
    _mm_setcsr(_mm_getcsr() | 0x8040);  // FTZ | DAZ
#elif defined(__aarch64__)
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24);  // FZ
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#endif
}

static void set_coeffs(VSBiquad *b, double b0, double b1, double b2, double a0, double a1, double a2) {
    b->b0 = (float)(b0 / a0);
    b->b1 = (float)(b1 / a0);
    b->b2 = (float)(b2 / a0);
    b->a1 = (float)(a1 / a0);
    b->a2 = (float)(a2 / a0);
}

void vs_biquad_hpf(VSBiquad *b, double fs, double f0, double q) {
    double w = 2.0 * M_PI * f0 / fs, cw = cos(w), alpha = sin(w) / (2.0 * q);
    set_coeffs(b, (1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
}

void vs_biquad_lpf(VSBiquad *b, double fs, double f0, double q) {
    double w = 2.0 * M_PI * f0 / fs, cw = cos(w), alpha = sin(w) / (2.0 * q);
    set_coeffs(b, (1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
}

void vs_biquad_highshelf(VSBiquad *b, double fs, double f0, double gainDb) {
    double A = pow(10.0, gainDb / 40.0);
    double w = 2.0 * M_PI * f0 / fs, cw = cos(w), sw = sin(w);
    double alpha = sw / 2.0 * sqrt(2.0);  // shelf slope S = 1
    double sa = 2.0 * sqrt(A) * alpha;
    set_coeffs(b,
               A * ((A + 1) + (A - 1) * cw + sa),
               -2 * A * ((A - 1) + (A + 1) * cw),
               A * ((A + 1) + (A - 1) * cw - sa),
               (A + 1) - (A - 1) * cw + sa,
               2 * ((A - 1) - (A + 1) * cw),
               (A + 1) - (A - 1) * cw - sa);
}
