// Real-time I/O: one AUHAL capturing the mic, processing in its callback, and
// lock-free SPSC rings feeding one AUHAL per output (main + optional monitor).
// Output AUHALs resample from the mic's rate to whatever the output device runs at.
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "VoiceCore.h"
#include "dsp.h"

#define RB_SIZE 16384u
#define RB_MASK (RB_SIZE - 1u)
#define MAX_FRAMES 4096u

typedef struct {
    float *data;
    _Atomic uint64_t w, r;
} VSRing;

static void rb_reset(VSRing *rb) {
    atomic_store(&rb->w, 0);
    atomic_store(&rb->r, 0);
    memset(rb->data, 0, sizeof(float) * RB_SIZE);
}

static void rb_write(VSRing *rb, const float *src, uint32_t n) {
    uint64_t w = atomic_load_explicit(&rb->w, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&rb->r, memory_order_acquire);
    uint64_t space = RB_SIZE - (w - r);
    if (n > space) n = (uint32_t)space;  // reader stalled: drop newest
    for (uint32_t i = 0; i < n; i++) rb->data[(w + i) & RB_MASK] = src[i];
    atomic_store_explicit(&rb->w, w + n, memory_order_release);
}

static uint64_t rb_avail(VSRing *rb) {
    return atomic_load_explicit(&rb->w, memory_order_acquire) - atomic_load_explicit(&rb->r, memory_order_relaxed);
}

static void rb_read(VSRing *rb, float *dst, uint32_t n) {
    uint64_t r = atomic_load_explicit(&rb->r, memory_order_relaxed);
    for (uint32_t i = 0; i < n; i++) dst[i] = rb->data[(r + i) & RB_MASK];
    atomic_store_explicit(&rb->r, r + n, memory_order_release);
}

static void rb_skip(VSRing *rb, uint64_t n) {
    atomic_fetch_add_explicit(&rb->r, n, memory_order_release);
}

typedef struct VSOutput {
    struct VSEngine *engine;
    AudioUnit unit;
    VSRing ring;
    bool primed;
    uint32_t target, slack;
    int gainParam;  // -1 = unity
    float gain;
    float *scratch;
    bool active;
} VSOutput;

struct VSEngine {
    AudioUnit inUnit;
    VSOutput main, monitor;
    VSChain *chain;
    float *inBuf, *procBuf;
    double fs;
    _Atomic float params[VSParamCount];
    _Atomic float cpu;
    _Atomic uint32_t underruns;
    bool running;
    double ticksToSec;
    float ioLatencyMs;
    char lastError[256];
};

// MARK: - helpers

static OSStatus getDeviceProp(AudioObjectID dev, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope,
                              void *out, UInt32 size) {
    AudioObjectPropertyAddress a = {sel, scope, kAudioObjectPropertyElementMain};
    return AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, out);
}

static void setBufferSize(AudioObjectID dev, uint32_t frames) {
    if (!dev) return;
    AudioValueRange range = {0, 0};
    if (getDeviceProp(dev, kAudioDevicePropertyBufferFrameSizeRange, kAudioObjectPropertyScopeGlobal, &range,
                      sizeof(range)) == noErr) {
        if (frames < range.mMinimum) frames = (uint32_t)range.mMinimum;
        if (frames > range.mMaximum) frames = (uint32_t)range.mMaximum;
    }
    UInt32 f = frames;
    AudioObjectPropertyAddress a = {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof(f), &f);
}

/// Device-side latency for one direction, in frames at the device's rate.
static double deviceLatencyMs(AudioObjectID dev, AudioObjectPropertyScope scope) {
    UInt32 lat = 0, safety = 0, buf = 0, streamLat = 0;
    Float64 sr = 48000;
    getDeviceProp(dev, kAudioDevicePropertyLatency, scope, &lat, sizeof(lat));
    getDeviceProp(dev, kAudioDevicePropertySafetyOffset, scope, &safety, sizeof(safety));
    getDeviceProp(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, &buf, sizeof(buf));
    getDeviceProp(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &sr, sizeof(sr));
    AudioObjectPropertyAddress a = {kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &size) == noErr && size >= sizeof(AudioStreamID)) {
        AudioStreamID sid = 0;
        UInt32 s1 = sizeof(sid);
        if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &s1, &sid) == noErr)
            getDeviceProp(sid, kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, &streamLat,
                          sizeof(streamLat));
    }
    if (sr <= 0) sr = 48000;
    return 1000.0 * (double)(lat + safety + buf + streamLat) / sr;
}

static void fail(VSEngine *e, const char *stage, OSStatus err) {
    snprintf(e->lastError, sizeof(e->lastError), "%s failed (OSStatus %d)", stage, (int)err);
}

static AudioStreamBasicDescription floatFormat(double sr, UInt32 channels) {
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = sr;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    f.mBytesPerPacket = sizeof(float);
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = sizeof(float);
    f.mChannelsPerFrame = channels;
    f.mBitsPerChannel = 32;
    return f;
}

static OSStatus newHAL(AudioUnit *u) {
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0,
                                   0};
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if (!comp) return kAudioUnitErr_InvalidElement;
    return AudioComponentInstanceNew(comp, u);
}

static void disposeUnit(AudioUnit *u) {
    if (!*u) return;
    AudioOutputUnitStop(*u);
    AudioUnitUninitialize(*u);
    AudioComponentInstanceDispose(*u);
    *u = NULL;
}

// MARK: - callbacks

static OSStatus inputProc(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                          UInt32 frames, AudioBufferList *unused) {
    (void)unused;
    VSEngine *e = (VSEngine *)ref;
    if (frames > MAX_FRAMES) return kAudioUnitErr_TooManyFramesToProcess;
    vs_enable_ftz();

    AudioBufferList abl;
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = 1;
    abl.mBuffers[0].mDataByteSize = frames * sizeof(float);
    abl.mBuffers[0].mData = e->inBuf;
    OSStatus err = AudioUnitRender(e->inUnit, flags, ts, bus, frames, &abl);
    if (err) return err;

    uint64_t t0 = mach_absolute_time();
    vs_chain_process(e->chain, e->inBuf, e->procBuf, (int)frames);
    rb_write(&e->main.ring, e->procBuf, frames);
    if (e->monitor.active) rb_write(&e->monitor.ring, e->procBuf, frames);
    double used = (double)(mach_absolute_time() - t0) * e->ticksToSec;
    float load = (float)(used / ((double)frames / e->fs));
    float prev = atomic_load_explicit(&e->cpu, memory_order_relaxed);
    atomic_store_explicit(&e->cpu, prev + 0.05f * (load - prev), memory_order_relaxed);
    return noErr;
}

static OSStatus outputProc(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                           UInt32 frames, AudioBufferList *io) {
    (void)flags;
    (void)ts;
    (void)bus;
    VSOutput *o = (VSOutput *)ref;
    VSEngine *e = o->engine;
    if (frames > MAX_FRAMES) frames = MAX_FRAMES;
    float *dst = o->scratch;

    uint64_t avail = rb_avail(&o->ring);
    if (!o->primed && avail >= o->target) o->primed = true;
    uint32_t got = 0;
    if (o->primed) {
        // Clock drift between input and output devices: keep the fill near target.
        if (avail > (uint64_t)o->target + o->slack + frames) {
            rb_skip(&o->ring, avail - o->target - frames);
            avail = o->target + frames;
        }
        got = avail < frames ? (uint32_t)avail : frames;
        rb_read(&o->ring, dst, got);
        if (got < frames) {
            o->primed = false;
            atomic_fetch_add_explicit(&e->underruns, 1, memory_order_relaxed);
        }
    }
    for (uint32_t i = got; i < frames; i++) dst[i] = 0.f;

    float g = 1.f;
    if (o->gainParam >= 0)
        g = vs_db2lin(atomic_load_explicit(&e->params[o->gainParam], memory_order_relaxed));
    for (uint32_t i = 0; i < frames; i++) {
        o->gain += 0.002f * (g - o->gain);
        dst[i] *= o->gain;
    }
    for (UInt32 b = 0; b < io->mNumberBuffers; b++) {
        float *ch = (float *)io->mBuffers[b].mData;
        UInt32 n = io->mBuffers[b].mDataByteSize / sizeof(float);
        if (n > frames) n = frames;
        memcpy(ch, dst, n * sizeof(float));
    }
    return noErr;
}

// MARK: - setup

static OSStatus setupInput(VSEngine *e, AudioObjectID dev) {
    OSStatus err;
    UInt32 one = 1, zero = 0;
    if ((err = newHAL(&e->inUnit))) return fail(e, "Create input unit", err), err;
    AudioUnit u = e->inUnit;
    if ((err = AudioUnitSetProperty(u, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &one,
                                    sizeof(one))))
        return fail(e, "Enable input", err), err;
    if ((err = AudioUnitSetProperty(u, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &zero,
                                    sizeof(zero))))
        return fail(e, "Disable input-unit output", err), err;
    if ((err = AudioUnitSetProperty(u, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev,
                                    sizeof(dev))))
        return fail(e, "Select input device", err), err;
    AudioStreamBasicDescription fmt = floatFormat(e->fs, 1);
    if ((err = AudioUnitSetProperty(u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &fmt,
                                    sizeof(fmt))))
        return fail(e, "Set input format", err), err;
    UInt32 maxF = MAX_FRAMES;
    AudioUnitSetProperty(u, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxF,
                         sizeof(maxF));
    AURenderCallbackStruct cb = {inputProc, e};
    if ((err = AudioUnitSetProperty(u, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb,
                                    sizeof(cb))))
        return fail(e, "Set input callback", err), err;
    if ((err = AudioUnitInitialize(u))) return fail(e, "Initialize input unit", err), err;
    return noErr;
}

static OSStatus setupOutput(VSEngine *e, VSOutput *o, AudioObjectID dev, uint32_t bufferFrames, int gainParam,
                            const char *label) {
    OSStatus err;
    char stage[96];
    o->engine = e;
    o->gainParam = gainParam;
    o->gain = gainParam >= 0 ? 0.f : 1.f;
    o->primed = false;
    o->target = bufferFrames + bufferFrames / 2 + 32;
    o->slack = bufferFrames + 256;
    rb_reset(&o->ring);
    if ((err = newHAL(&o->unit))) {
        snprintf(stage, sizeof(stage), "Create %s unit", label);
        return fail(e, stage, err), err;
    }
    UInt32 one = 1, zero = 0;
    AudioUnitSetProperty(o->unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &zero, sizeof(zero));
    AudioUnitSetProperty(o->unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &one, sizeof(one));
    if ((err = AudioUnitSetProperty(o->unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev,
                                    sizeof(dev)))) {
        snprintf(stage, sizeof(stage), "Select %s device", label);
        return fail(e, stage, err), err;
    }
    UInt32 devCh = 2;
    {
        AudioStreamBasicDescription dfmt;
        UInt32 sz = sizeof(dfmt);
        if (AudioUnitGetProperty(o->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &dfmt, &sz) ==
                noErr &&
            dfmt.mChannelsPerFrame > 0)
            devCh = dfmt.mChannelsPerFrame < 2 ? dfmt.mChannelsPerFrame : 2;
    }
    AudioStreamBasicDescription fmt = floatFormat(e->fs, devCh);
    if ((err = AudioUnitSetProperty(o->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt,
                                    sizeof(fmt)))) {
        snprintf(stage, sizeof(stage), "Set %s format", label);
        return fail(e, stage, err), err;
    }
    UInt32 maxF = MAX_FRAMES;
    AudioUnitSetProperty(o->unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxF,
                         sizeof(maxF));
    AURenderCallbackStruct cb = {outputProc, o};
    if ((err = AudioUnitSetProperty(o->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb,
                                    sizeof(cb)))) {
        snprintf(stage, sizeof(stage), "Set %s callback", label);
        return fail(e, stage, err), err;
    }
    if ((err = AudioUnitInitialize(o->unit))) {
        snprintf(stage, sizeof(stage), "Initialize %s unit", label);
        return fail(e, stage, err), err;
    }
    o->active = true;
    return noErr;
}

// MARK: - public

VSEngine *vs_engine_create(void) {
    VSEngine *e = (VSEngine *)calloc(1, sizeof(VSEngine));
    if (!e) return NULL;
    e->inBuf = (float *)calloc(MAX_FRAMES, sizeof(float));
    e->procBuf = (float *)calloc(MAX_FRAMES, sizeof(float));
    e->main.ring.data = (float *)calloc(RB_SIZE, sizeof(float));
    e->monitor.ring.data = (float *)calloc(RB_SIZE, sizeof(float));
    e->main.scratch = (float *)calloc(MAX_FRAMES, sizeof(float));
    e->monitor.scratch = (float *)calloc(MAX_FRAMES, sizeof(float));
    VSChain *defaults = vs_chain_create(48000, 64, false);
    for (int i = 0; i < VSParamCount; i++)
        atomic_init(&e->params[i], defaults ? vs_chain_get_param(defaults, (VSParam)i) : 0.f);
    vs_chain_destroy(defaults);
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    e->ticksToSec = (double)tb.numer / (double)tb.denom * 1e-9;
    return e;
}

void vs_engine_destroy(VSEngine *e) {
    if (!e) return;
    vs_engine_stop(e);
    free(e->inBuf);
    free(e->procBuf);
    free(e->main.ring.data);
    free(e->monitor.ring.data);
    free(e->main.scratch);
    free(e->monitor.scratch);
    free(e);
}

int32_t vs_engine_start(VSEngine *e, uint32_t inputDevice, uint32_t outputDevice, uint32_t monitorDevice,
                        uint32_t bufferFrames, bool lowLatency) {
    vs_engine_stop(e);
    e->lastError[0] = 0;
    if (!inputDevice || !outputDevice) {
        snprintf(e->lastError, sizeof(e->lastError), "No input or output device selected");
        return kAudioHardwareBadDeviceError;
    }
    if (bufferFrames < 32) bufferFrames = 32;
    if (bufferFrames > 2048) bufferFrames = 2048;

    Float64 sr = 0;
    OSStatus err = getDeviceProp(inputDevice, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                 &sr, sizeof(sr));
    if (err || sr < 8000) {
        fail(e, "Read input sample rate", err ? err : kAudioHardwareUnspecifiedError);
        return err ? err : kAudioHardwareUnspecifiedError;
    }
    e->fs = sr;

    setBufferSize(inputDevice, bufferFrames);
    setBufferSize(outputDevice, bufferFrames);
    if (monitorDevice) setBufferSize(monitorDevice, bufferFrames);

    e->chain = vs_chain_create(sr, (int)MAX_FRAMES, lowLatency);
    if (!e->chain) {
        snprintf(e->lastError, sizeof(e->lastError), "Out of memory creating DSP chain");
        return kAudioHardwareUnspecifiedError;
    }
    for (int i = 0; i < VSParamCount; i++)
        vs_chain_set_param(e->chain, (VSParam)i, atomic_load(&e->params[i]));
    vs_chain_reset(e->chain);
    atomic_store(&e->underruns, 0);
    atomic_store(&e->cpu, 0.f);

    if ((err = setupOutput(e, &e->main, outputDevice, bufferFrames, -1, "output"))) goto bail;
    if (monitorDevice && monitorDevice != outputDevice) {
        if ((err = setupOutput(e, &e->monitor, monitorDevice, bufferFrames, VSParamMonitorGainDb, "monitor")))
            goto bail;
    }
    if ((err = setupInput(e, inputDevice))) goto bail;

    if ((err = AudioOutputUnitStart(e->main.unit))) {
        fail(e, "Start output", err);
        goto bail;
    }
    if (e->monitor.active && (err = AudioOutputUnitStart(e->monitor.unit))) {
        fail(e, "Start monitor", err);
        goto bail;
    }
    if ((err = AudioOutputUnitStart(e->inUnit))) {
        fail(e, "Start input (microphone permission?)", err);
        goto bail;
    }

    e->ioLatencyMs = (float)(deviceLatencyMs(inputDevice, kAudioObjectPropertyScopeInput) +
                             deviceLatencyMs(outputDevice, kAudioObjectPropertyScopeOutput) +
                             1000.0 * e->main.target / sr);
    e->running = true;
    return noErr;

bail:
    vs_engine_stop(e);
    return err;
}

void vs_engine_stop(VSEngine *e) {
    disposeUnit(&e->inUnit);  // stop the producer first
    disposeUnit(&e->main.unit);
    disposeUnit(&e->monitor.unit);
    e->main.active = e->monitor.active = false;
    if (e->chain) {
        vs_chain_destroy(e->chain);
        e->chain = NULL;
    }
    e->running = false;
}

bool vs_engine_is_running(const VSEngine *e) { return e->running; }

void vs_engine_set_param(VSEngine *e, VSParam p, float v) {
    if ((int)p < 0 || p >= VSParamCount) return;
    atomic_store(&e->params[p], v);
    if (e->chain) vs_chain_set_param(e->chain, p, v);
}

float vs_engine_meter(const VSEngine *e, VSMeter m) {
    VSEngine *me = (VSEngine *)e;
    switch (m) {
        case VSMeterCpuLoad:
            return e->running ? atomic_load(&me->cpu) : 0.f;
        case VSMeterUnderruns:
            return (float)atomic_load(&me->underruns);
        case VSMeterLatencyMs:
            return e->chain ? vs_chain_meter(e->chain, m) + e->ioLatencyMs : 0.f;
        default:
            return e->chain ? vs_chain_meter(e->chain, m) : 0.f;
    }
}

const char *vs_engine_last_error(const VSEngine *e) { return e->lastError; }
