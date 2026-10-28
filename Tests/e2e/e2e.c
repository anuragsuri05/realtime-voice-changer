// End-to-end checks against the *installed* VoiceShift Virtual Mic.
//   e2e loopback            play a reference signal into the virtual mic, record it back like a call app
//   e2e engine <wav>        play <wav> on the speakers, run VoiceCore mic → virtual mic, record & analyse
//   e2e record <sec> <wav>  record what call apps hear from the virtual mic (for testing the GUI app)
//   e2e analyze <in.wav> <out.wav>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "VoiceCore.h"

#define FS 48000.0
#include <mach/mach_time.h>
static double hostSec(UInt64 h) { static double k = 0; if (!k) { mach_timebase_info_data_t tb; mach_timebase_info(&tb); k = (double)tb.numer / tb.denom * 1e-9; } return h * k; }
static int fails = 0;
static void check(int ok, const char *what, double v, const char *unit) {
    printf("  [%s] %-52s %10.3f %s\n", ok ? "PASS" : "FAIL", what, v, unit);
    if (!ok) fails++;
}

// ---------- devices ----------
static AudioObjectID findDevice(const char *uid) {
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, 0};
    CFStringRef s = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
    AudioObjectID d = 0;
    UInt32 z = sizeof d;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof s, &s, &z, &d);
    CFRelease(s);
    return d;
}
static AudioObjectID dflt(AudioObjectPropertySelector sel) {
    AudioObjectPropertyAddress a = {sel, kAudioObjectPropertyScopeGlobal, 0};
    AudioObjectID d = 0;
    UInt32 z = 4;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, 0, &z, &d);
    return d;
}

// ---------- simple AUHAL player / recorder ----------
typedef struct { AudioUnit u; float *buf; long n, pos; double t0; int started; } Clip;  // t0 = host time (s) at which buf[0] was played/captured

static OSStatus playCB(void *ref, AudioUnitRenderActionFlags *f, const AudioTimeStamp *t, UInt32 b, UInt32 n,
                       AudioBufferList *io) {
    Clip *c = ref;
    if (!c->started && (t->mFlags & kAudioTimeStampHostTimeValid)) { c->started = 1; c->t0 = hostSec(t->mHostTime) - c->pos / FS; }
    for (UInt32 i = 0; i < n; i++) {
        float v = c->pos < c->n ? c->buf[c->pos] : 0;
        c->pos++;
        for (UInt32 k = 0; k < io->mNumberBuffers; k++) ((float *)io->mBuffers[k].mData)[i] = k == 0 ? v : -v;
    }
    return noErr;
}
static float recScratch[2][8192];
static OSStatus recCB(void *ref, AudioUnitRenderActionFlags *f, const AudioTimeStamp *t, UInt32 b, UInt32 n,
                      AudioBufferList *unused) {
    Clip *c = ref;
    struct { AudioBufferList l; AudioBuffer extra; } abl;
    abl.l.mNumberBuffers = 2;
    for (int k = 0; k < 2; k++) {
        abl.l.mBuffers[k].mNumberChannels = 1;
        abl.l.mBuffers[k].mDataByteSize = n * 4;
        abl.l.mBuffers[k].mData = recScratch[k];
    }
    OSStatus e = AudioUnitRender(c->u, f, t, b, n, &abl.l);
    if (e) return e;
    if (!c->started && (t->mFlags & kAudioTimeStampHostTimeValid)) { c->started = 1; c->t0 = hostSec(t->mHostTime) - c->pos / FS; }
    for (UInt32 i = 0; i < n && c->pos < c->n; i++) c->buf[c->pos++] = recScratch[0][i];
    return noErr;
}

static AudioStreamBasicDescription fmt(UInt32 ch) {
    AudioStreamBasicDescription f = {FS, kAudioFormatLinearPCM,
        kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved, 4, 1, 4, ch, 32, 0};
    return f;
}
static AudioUnit hal(void) {
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple};
    AudioUnit u;
    AudioComponentInstanceNew(AudioComponentFindNext(NULL, &d), &u);
    return u;
}
static int startPlayer(Clip *c, AudioObjectID dev) {
    c->u = hal();
    AudioUnitSetProperty(c->u, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, 4);
    AudioStreamBasicDescription f = fmt(2);
    AudioUnitSetProperty(c->u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    AURenderCallbackStruct cb = {playCB, c};
    AudioUnitSetProperty(c->u, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (AudioUnitInitialize(c->u)) return -1;
    return AudioOutputUnitStart(c->u);
}
static int startRecorder(Clip *c, AudioObjectID dev) {
    c->u = hal();
    UInt32 one = 1, zero = 0;
    AudioUnitSetProperty(c->u, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &one, 4);
    AudioUnitSetProperty(c->u, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &zero, 4);
    AudioUnitSetProperty(c->u, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, 4);
    AudioStreamBasicDescription f = fmt(2);
    AudioUnitSetProperty(c->u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &f, sizeof f);
    AURenderCallbackStruct cb = {recCB, c};
    AudioUnitSetProperty(c->u, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb, sizeof cb);
    if (AudioUnitInitialize(c->u)) return -1;
    return AudioOutputUnitStart(c->u);
}
static void stopUnit(Clip *c) { AudioOutputUnitStop(c->u); AudioUnitUninitialize(c->u); AudioComponentInstanceDispose(c->u); }

// ---------- wav ----------
static float *readWav(const char *path, long *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 44, SEEK_SET);
    long cap = 48000 * 60;
    float *x = calloc(cap, 4);
    int16_t s;
    *n = 0;
    while (*n < cap && fread(&s, 2, 1, f) == 1) x[(*n)++] = s / 32768.f;
    fclose(f);
    return x;
}
static void writeWav(const char *path, const float *x, long n) {
    FILE *f = fopen(path, "wb");
    uint32_t db = n * 2, sr = 48000, br = 96000, sz = 36 + db, fl = 16;
    uint16_t pcm = 1, ch = 1, al = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&sz, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fl, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&al, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&db, 4, 1, f);
    for (long i = 0; i < n; i++) { float v = fmaxf(-1, fminf(1, x[i])); int16_t s = (int16_t)lrintf(v * 32767); fwrite(&s, 2, 1, f); }
    fclose(f);
}

// ---------- analysis ----------
static double f0_median(const float *x, long n) {
    int W = 1920, lagMin = 96, lagMax = 640;
    static double vals[20000], rr[700];
    int nv = 0;
    for (long s = 0; s + W + lagMax < n && nv < 20000; s += W / 2) {
        double e0 = 0, best = 0;
        for (int i = 0; i < W; i++) e0 += x[s + i] * x[s + i];
        if (e0 / W < 1e-5) continue;
        for (int L = lagMin; L <= lagMax; L++) {
            double c = 0, e1 = 0;
            for (int i = 0; i < W; i++) { c += x[s + i] * x[s + i + L]; e1 += x[s + i + L] * x[s + i + L]; }
            rr[L] = c / sqrt(e0 * e1 + 1e-12);
            if (rr[L] > best) best = rr[L];
        }
        if (best < 0.6) continue;
        for (int L = lagMin + 1; L < lagMax; L++)
            if (rr[L] >= 0.9 * best && rr[L] >= rr[L - 1] && rr[L] >= rr[L + 1]) { vals[nv++] = FS / L; break; }
    }
    if (!nv) return 0;
    for (int i = 0; i < nv; i++) for (int j = i + 1; j < nv; j++) if (vals[j] < vals[i]) { double t = vals[i]; vals[i] = vals[j]; vals[j] = t; }
    return vals[nv / 2];
}
static double rmsdb(const float *x, long n) { double s = 0; for (long i = 0; i < n; i++) s += x[i] * x[i]; return 10 * log10(s / (n ? n : 1) + 1e-12); }
static long firstSound(const float *x, long n, float thr) { for (long i = 0; i < n; i++) if (fabsf(x[i]) > thr) return i; return -1; }

// ---------- tests ----------
static int loopback(void) {
    AudioObjectID vm = findDevice("VoiceShiftVirtualMic_UID");
    printf("Virtual mic device id: %u\n", vm);
    check(vm != 0, "VoiceShift Virtual Mic present in CoreAudio", vm, "id");
    if (!vm) return 1;
    long n = (long)(4 * FS);
    Clip play = {0}, rec = {0};
    play.buf = calloc(n, 4); play.n = n;
    // reference: 0.5 s silence then chirp 200 Hz → 4 kHz, then tone bursts
    for (long i = (long)(0.5 * FS); i < n - FS / 2; i++) {
        double t = (i - 0.5 * FS) / FS, f = 200 * pow(20, t / 3.0);
        play.buf[i] = 0.4 * sin(2 * M_PI * 200 * 3.0 / log(20) * (pow(20, t / 3.0) - 1));
        (void)f;
    }
    rec.buf = calloc(n + FS, 4); rec.n = n + (long)FS;
    if (startRecorder(&rec, vm)) { check(0, "recorder start", 0, ""); return 1; }
    if (startPlayer(&play, vm)) { check(0, "player start", 0, ""); return 1; }
    usleep((useconds_t)(5.2e6));
    stopUnit(&play); stopUnit(&rec);
    long on = firstSound(rec.buf, rec.pos, 0.05);
    check(rec.pos > n, "recorded samples (call-app side)", rec.pos, "samples");
    check(on > 0, "signal arrives at virtual mic input", on, "sample");
    long lag = on - (long)(0.5 * FS) - 1;
    // refine lag by correlation around coarse estimate
    double best = -1; long bl = lag;
    for (long L = lag - 200; L <= lag + 200; L++) {
        if (L < 0) continue;
        double c = 0;
        for (long i = (long)FS; i < (long)(2 * FS); i++) c += play.buf[i] * rec.buf[i + L];
        if (c > best) { best = c; bl = L; }
    }
    double err = 0, sig = 0;
    long glitches = 0;
    for (long i = (long)(0.6 * FS); i < n - FS; i++) {
        double d = rec.buf[i + bl] - play.buf[i];
        err += d * d; sig += play.buf[i] * play.buf[i];
        if (fabs(d) > 0.01) glitches++;
    }
    // bl is a buffer-index offset; convert to wall-clock using each client's host-time origin.
    double ms = 1000.0 * ((rec.t0 + bl / FS) - play.t0);
    // Timestamps are presentation/capture times, so a perfect loopback reads 0; the only real
    // delay is CoreAudio's own I/O buffering (~2 buffers), which every device has.
    check(fabs(ms) < 0.1, "delay added by the virtual mic driver", ms, "ms");
    double snr = 10 * log10(sig / (err + 1e-20));
    check(snr > 90, "loopback fidelity (signal / error)", snr, "dB");
    check(glitches == 0, "dropped/garbled samples", glitches, "samples");
    return 0;
}

static int engine(const char *wav, const char *outdir) {
    // Digital mode: E2E_SOURCE_UID names a loopback device we play the voice into and the
    // engine captures from (stands in for a microphone). Otherwise: speakers → room → default mic.
    const char *src = getenv("E2E_SOURCE_UID");
    AudioObjectID vm = findDevice("VoiceShiftVirtualMic_UID");
    AudioObjectID mic = src ? findDevice(src) : dflt(kAudioHardwarePropertyDefaultInputDevice);
    AudioObjectID spk = src ? mic : dflt(kAudioHardwarePropertyDefaultOutputDevice);
    printf("  source: %s\n", src ? src : "speakers → room → default microphone");
    if (!mic) { printf("source device not found\n"); return 1; }
    if (mic == vm) { printf("default input is the virtual mic; pick a real mic\n"); return 1; }
    long n;
    float *voice = readWav(wav, &n);
    if (!voice) { printf("cannot read %s\n", wav); return 1; }
    Clip play = {0}, rec = {0};
    play.buf = voice; play.n = n;
    rec.buf = calloc(n + 2 * FS, 4); rec.n = n + (long)(2 * FS);
    VSEngine *e = vs_engine_create();
    vs_engine_set_param(e, VSParamEffectEnabled, 1);
    vs_engine_set_param(e, VSParamIntonation, 1);  // keep the ratio exact for measurement
    if (getenv("E2E_NS")) vs_engine_set_param(e, VSParamNoiseSuppression, atof(getenv("E2E_NS")));
    if (getenv("E2E_EFFECT")) vs_engine_set_param(e, VSParamEffectEnabled, atof(getenv("E2E_EFFECT")));
    if (getenv("E2E_GAIN")) vs_engine_set_param(e, VSParamInputGainDb, atof(getenv("E2E_GAIN")));
    int verbose = getenv("E2E_VERBOSE") != NULL;
    int err = vs_engine_start(e, mic, vm, 0, 128, false);
    check(err == 0, "VoiceCore engine start (mic → virtual mic)", err, vs_engine_last_error(e));
    if (err) return 1;
    startRecorder(&rec, vm);   // "the call app"
    usleep(300000);
    startPlayer(&play, spk);   // a male voice in the room
    float cpuMax = 0, maxIn = 0;
    for (int i = 0; i < (int)((n / FS + 1.0) * 10); i++) {
        usleep(100000);
        cpuMax = fmaxf(cpuMax, vs_engine_meter(e, VSMeterCpuLoad));
        maxIn = fmaxf(maxIn, vs_engine_meter(e, VSMeterInputPeak));
        if (verbose && i % 3 == 0)
            printf("    t=%.1fs in-peak %.3f out-peak %.3f F0 %3.0f→%3.0f Hz NR %4.1f dB\n", i / 10.0,
                   vs_engine_meter(e, VSMeterInputPeak), vs_engine_meter(e, VSMeterOutputPeak),
                   vs_engine_meter(e, VSMeterInputF0), vs_engine_meter(e, VSMeterOutputF0),
                   vs_engine_meter(e, VSMeterNoiseReductionDb));
    }
    float lat = vs_engine_meter(e, VSMeterLatencyMs), und = vs_engine_meter(e, VSMeterUnderruns);
    stopUnit(&play); stopUnit(&rec); vs_engine_stop(e); vs_engine_destroy(e);

    char p[1024];
    snprintf(p, sizeof p, "%s/e2e_call_app_heard.wav", outdir);
    writeWav(p, rec.buf, rec.pos);
    printf("  wrote %s\n", p);
    double fin = f0_median(voice, n), fout = f0_median(rec.buf, rec.pos), want = fin * pow(2, 8 / 12.0);
    check(maxIn > 0.01, "mic picked up the played voice (peak)", maxIn, "");
    check(rmsdb(rec.buf, rec.pos) > -50, "call app receives audio (RMS)", rmsdb(rec.buf, rec.pos), "dBFS");
    printf("        input F0 %.1f Hz → call-app F0 %.1f Hz (target %.1f)\n", fin, fout, want);
    check(fout > 0 && fabs(fout - want) / want < 0.05, "pitch heard by call app vs +8 st target", fout, "Hz");
    check(und <= 2, "output underruns", und, "");
    check(cpuMax < 0.25, "peak DSP CPU load", cpuMax * 100, "%");
    printf("        reported end-to-end latency: %.1f ms\n", lat);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "loopback")) loopback();
    else if (argc >= 3 && !strcmp(argv[1], "engine")) engine(argv[2], argc > 3 ? argv[3] : ".");
    else if (argc >= 4 && !strcmp(argv[1], "record")) {
        AudioObjectID vm = findDevice("VoiceShiftVirtualMic_UID");
        Clip rec = {0};
        rec.n = (long)(atof(argv[2]) * FS); rec.buf = calloc(rec.n, 4);
        Clip play = {0};
        const char *src = getenv("E2E_SOURCE_UID"), *wav = getenv("E2E_PLAY_WAV");
        startRecorder(&rec, vm);
        if (src && wav) {
            play.buf = readWav(wav, &play.n);
            startPlayer(&play, findDevice(src));
        }
        usleep((useconds_t)(atof(argv[2]) * 1e6 + 300000));
        stopUnit(&rec);
        if (play.u) stopUnit(&play);
        writeWav(argv[3], rec.buf, rec.pos);
        printf("recorded %ld samples, RMS %.1f dBFS, F0 %.1f Hz\n", rec.pos, rmsdb(rec.buf, rec.pos), f0_median(rec.buf, rec.pos));
        return 0;
    } else if (argc >= 4 && !strcmp(argv[1], "analyze")) {
        long a, b; float *x = readWav(argv[2], &a), *y = readWav(argv[3], &b);
        printf("in F0 %.1f Hz  out F0 %.1f Hz  ratio %.3f  out RMS %.1f dBFS\n", f0_median(x, a), f0_median(y, b),
               f0_median(y, b) / f0_median(x, a), rmsdb(y, b));
        return 0;
    } else { fprintf(stderr, "usage: e2e loopback | engine <wav> [outdir] | record <sec> <wav> | analyze <in> <out>\n"); return 2; }
    printf("%s (%d failure%s)\n", fails ? "E2E: SOME CHECKS FAILED" : "E2E: ALL CHECKS PASSED", fails, fails == 1 ? "" : "s");
    return fails != 0;
}
