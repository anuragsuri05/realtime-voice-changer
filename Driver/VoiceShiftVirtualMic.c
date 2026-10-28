// VoiceShift Virtual Mic — a minimal CoreAudio AudioServerPlugIn loopback device.
//
// One device, 2 channels, Float32, 44.1/48 kHz. Whatever is played *to* the device
// (by the VoiceShift app) comes back out of the device's *input* (the "microphone"
// FaceTime, Zoom, WhatsApp, browsers, etc. see). The device refuses to become the
// default output so macOS never routes system sound into it by accident.
//
// Structure follows Apple's NullAudio sample driver.

#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// Overridable so tests can build a second, independent loopback device.
#ifndef kDevice_Name
#define kDevice_Name "VoiceShift Virtual Mic"
#endif
#ifndef kDevice_UID
#define kDevice_UID "VoiceShiftVirtualMic_UID"
#endif
#ifndef kDevice_ModelUID
#define kDevice_ModelUID "VoiceShiftVirtualMic_ModelUID"
#endif
#ifndef kFactoryName
#define kFactoryName VoiceShift_Create
#endif
#define kManufacturer "VoiceShift"

enum {
    kObjectID_PlugIn = kAudioObjectPlugInObject,
    kObjectID_Device = 2,
    kObjectID_Stream_Input = 3,
    kObjectID_Stream_Output = 4,
};

#define kChannels 2
#define kRingFrames 16384u  // also the zero-timestamp period

static const Float64 kRates[] = {44100.0, 48000.0};
#define kNumRates (sizeof(kRates) / sizeof(kRates[0]))

static pthread_mutex_t gStateMutex = PTHREAD_MUTEX_INITIALIZER;
static AudioServerPlugInHostRef gHost = NULL;
static UInt32 gRefCount = 0;

static Float64 gSampleRate = 48000.0;
static Float64 gPendingRate = 0;
static bool gInputActive = true, gOutputActive = true;

static UInt32 gIOCount = 0;
static Float64 gHostTicksPerFrame = 0;
static UInt64 gAnchorHostTime = 0;
static UInt64 gTimestampCount = 0;

static Float32 *gRing = NULL;
static Float64 gWriteEnd = -1;  // sample time just past the last WriteMix

// MARK: - forward declarations

static HRESULT VS_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface);
static ULONG VS_AddRef(void *inDriver);
static ULONG VS_Release(void *inDriver);
static OSStatus VS_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus VS_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription,
                                const AudioServerPlugInClientInfo *inClientInfo, AudioObjectID *outDeviceObjectID);
static OSStatus VS_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);
static OSStatus VS_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                   const AudioServerPlugInClientInfo *inClientInfo);
static OSStatus VS_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                      const AudioServerPlugInClientInfo *inClientInfo);
static OSStatus VS_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                    AudioObjectID inDeviceObjectID, UInt64 inChangeAction,
                                                    void *inChangeInfo);
static OSStatus VS_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                                  UInt64 inChangeAction, void *inChangeInfo);
static Boolean VS_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientPID,
                              const AudioObjectPropertyAddress *inAddress);
static OSStatus VS_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientPID,
                                      const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable);
static OSStatus VS_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                       pid_t inClientPID, const AudioObjectPropertyAddress *inAddress,
                                       UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize);
static OSStatus VS_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientPID,
                                   const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize,
                                   const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize,
                                   void *outData);
static OSStatus VS_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientPID,
                                   const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize,
                                   const void *inQualifierData, UInt32 inDataSize, const void *inData);
static OSStatus VS_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus VS_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus VS_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                    UInt32 inClientID, Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed);
static OSStatus VS_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                     UInt32 inClientID, UInt32 inOperationID, Boolean *outWillDo,
                                     Boolean *outWillDoInPlace);
static OSStatus VS_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                    UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                    const AudioServerPlugInIOCycleInfo *inIOCycleInfo);
static OSStatus VS_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                 AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID,
                                 UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo,
                                 void *ioMainBuffer, void *ioSecondaryBuffer);
static OSStatus VS_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                  UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                  const AudioServerPlugInIOCycleInfo *inIOCycleInfo);

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    VS_QueryInterface,
    VS_AddRef,
    VS_Release,
    VS_Initialize,
    VS_CreateDevice,
    VS_DestroyDevice,
    VS_AddDeviceClient,
    VS_RemoveDeviceClient,
    VS_PerformDeviceConfigurationChange,
    VS_AbortDeviceConfigurationChange,
    VS_HasProperty,
    VS_IsPropertySettable,
    VS_GetPropertyDataSize,
    VS_GetPropertyData,
    VS_SetPropertyData,
    VS_StartIO,
    VS_StopIO,
    VS_GetZeroTimeStamp,
    VS_WillDoIOOperation,
    VS_BeginIOOperation,
    VS_DoIOOperation,
    VS_EndIOOperation,
};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

// MARK: - factory & IUnknown

__attribute__((visibility("default"))) void *kFactoryName(CFAllocatorRef inAllocator,
                                                                CFUUIDRef inRequestedTypeUUID) {
    (void)inAllocator;
    if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) return gDriverRef;
    return NULL;
}

static HRESULT VS_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface) {
    if (inDriver != gDriverRef || !outInterface) return kAudioHardwareBadObjectError;
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    HRESULT result = E_NOINTERFACE;
    if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
        pthread_mutex_lock(&gStateMutex);
        gRefCount++;
        pthread_mutex_unlock(&gStateMutex);
        *outInterface = gDriverRef;
        result = S_OK;
    }
    CFRelease(req);
    return result;
}

static ULONG VS_AddRef(void *inDriver) {
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    ULONG r = ++gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return r;
}

static ULONG VS_Release(void *inDriver) {
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    if (gRefCount > 0) gRefCount--;
    ULONG r = gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return r;
}

// MARK: - basic operations

static void updateTicksPerFrame(void) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    Float64 ticksPerSecond = 1e9 * (Float64)tb.denom / (Float64)tb.numer;
    gHostTicksPerFrame = ticksPerSecond / gSampleRate;
}

static OSStatus VS_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    gHost = inHost;
    updateTicksPerFrame();
    return noErr;
}

static OSStatus VS_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc,
                                const AudioServerPlugInClientInfo *ci, AudioObjectID *out) {
    (void)d; (void)desc; (void)ci; (void)out;
    return kAudioHardwareUnsupportedOperationError;
}
static OSStatus VS_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID dev) {
    (void)d; (void)dev;
    return kAudioHardwareUnsupportedOperationError;
}
static OSStatus VS_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev,
                                   const AudioServerPlugInClientInfo *ci) {
    (void)ci;
    return (d == gDriverRef && dev == kObjectID_Device) ? noErr : kAudioHardwareBadObjectError;
}
static OSStatus VS_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev,
                                      const AudioServerPlugInClientInfo *ci) {
    (void)ci;
    return (d == gDriverRef && dev == kObjectID_Device) ? noErr : kAudioHardwareBadObjectError;
}

static OSStatus VS_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action,
                                                    void *info) {
    (void)action; (void)info;
    if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gStateMutex);
    if (gPendingRate > 0) {
        gSampleRate = gPendingRate;
        gPendingRate = 0;
        updateTicksPerFrame();
    }
    pthread_mutex_unlock(&gStateMutex);
    return noErr;
}

static OSStatus VS_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action,
                                                  void *info) {
    (void)d; (void)dev; (void)action; (void)info;
    pthread_mutex_lock(&gStateMutex);
    gPendingRate = 0;
    pthread_mutex_unlock(&gStateMutex);
    return noErr;
}

// MARK: - property helpers

static AudioStreamBasicDescription streamFormat(Float64 rate) {
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = sizeof(Float32) * kChannels;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = sizeof(Float32) * kChannels;
    f.mChannelsPerFrame = kChannels;
    f.mBitsPerChannel = 32;
    return f;
}

static bool isStream(AudioObjectID o) { return o == kObjectID_Stream_Input || o == kObjectID_Stream_Output; }

static bool knownObject(AudioObjectID o) { return o == kObjectID_PlugIn || o == kObjectID_Device || isStream(o); }

/// Streams visible for a scope on the device.
static UInt32 deviceStreams(AudioObjectPropertyScope scope, AudioObjectID out[2]) {
    UInt32 n = 0;
    if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeInput)
        out[n++] = kObjectID_Stream_Input;
    if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeOutput)
        out[n++] = kObjectID_Stream_Output;
    return n;
}

static Boolean VS_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                              const AudioObjectPropertyAddress *a) {
    (void)pid;
    if (d != gDriverRef || !a) return false;
    switch (obj) {
        case kObjectID_PlugIn:
            switch (a->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyManufacturer:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioPlugInPropertyDeviceList:
                case kAudioPlugInPropertyTranslateUIDToDevice:
                case kAudioPlugInPropertyResourceBundle:
                    return true;
            }
            return false;
        case kObjectID_Device:
            switch (a->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyName:
                case kAudioObjectPropertyManufacturer:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioObjectPropertyControlList:
                case kAudioDevicePropertyDeviceUID:
                case kAudioDevicePropertyModelUID:
                case kAudioDevicePropertyTransportType:
                case kAudioDevicePropertyRelatedDevices:
                case kAudioDevicePropertyClockDomain:
                case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyDeviceIsRunning:
                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                case kAudioDevicePropertyLatency:
                case kAudioDevicePropertyStreams:
                case kAudioDevicePropertySafetyOffset:
                case kAudioDevicePropertyNominalSampleRate:
                case kAudioDevicePropertyAvailableNominalSampleRates:
                case kAudioDevicePropertyIsHidden:
                case kAudioDevicePropertyPreferredChannelsForStereo:
                case kAudioDevicePropertyPreferredChannelLayout:
                case kAudioDevicePropertyZeroTimeStampPeriod:
                    return true;
            }
            return false;
        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output:
            switch (a->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioStreamPropertyIsActive:
                case kAudioStreamPropertyDirection:
                case kAudioStreamPropertyTerminalType:
                case kAudioStreamPropertyStartingChannel:
                case kAudioStreamPropertyLatency:
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat:
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats:
                    return true;
            }
            return false;
    }
    return false;
}

static OSStatus VS_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                      const AudioObjectPropertyAddress *a, Boolean *out) {
    if (!VS_HasProperty(d, obj, pid, a)) return kAudioHardwareUnknownPropertyError;
    if (!out) return kAudioHardwareIllegalOperationError;
    *out = false;
    if (obj == kObjectID_Device && a->mSelector == kAudioDevicePropertyNominalSampleRate) *out = true;
    if (isStream(obj) && (a->mSelector == kAudioStreamPropertyIsActive ||
                          a->mSelector == kAudioStreamPropertyVirtualFormat ||
                          a->mSelector == kAudioStreamPropertyPhysicalFormat))
        *out = true;
    return noErr;
}

/// Single implementation for size and data: when `out` is NULL only the size is computed.
static OSStatus getProperty(AudioObjectID obj, const AudioObjectPropertyAddress *a, UInt32 qSize, const void *qData,
                            UInt32 inSize, UInt32 *outSize, void *out) {
#define PUT(type, value)                                                     \
    do {                                                                     \
        if (out) {                                                           \
            if (inSize < sizeof(type)) return kAudioHardwareBadPropertySizeError; \
            *(type *)out = (value);                                          \
        }                                                                    \
        *outSize = sizeof(type);                                             \
        return noErr;                                                        \
    } while (0)
#define PUT_IDS(ids, count)                                                   \
    do {                                                                      \
        UInt32 _n = (count);                                                  \
        if (out) {                                                            \
            UInt32 _fit = inSize / sizeof(AudioObjectID);                     \
            if (_n > _fit) _n = _fit;                                         \
            memcpy(out, (ids), _n * sizeof(AudioObjectID));                   \
        }                                                                     \
        *outSize = _n * sizeof(AudioObjectID);                                \
        return noErr;                                                         \
    } while (0)

    switch (obj) {
        case kObjectID_PlugIn:
            switch (a->mSelector) {
                case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
                case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioPlugInClassID);
                case kAudioObjectPropertyOwner: PUT(AudioObjectID, kAudioObjectUnknown);
                case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR(kManufacturer));
                case kAudioObjectPropertyOwnedObjects:
                case kAudioPlugInPropertyDeviceList: {
                    AudioObjectID ids[1] = {kObjectID_Device};
                    PUT_IDS(ids, 1);
                }
                case kAudioPlugInPropertyTranslateUIDToDevice: {
                    AudioObjectID result = kAudioObjectUnknown;
                    if (qSize == sizeof(CFStringRef) && qData) {
                        CFStringRef uid = *(const CFStringRef *)qData;
                        if (uid && CFStringCompare(uid, CFSTR(kDevice_UID), 0) == kCFCompareEqualTo)
                            result = kObjectID_Device;
                    }
                    PUT(AudioObjectID, result);
                }
                case kAudioPlugInPropertyResourceBundle: PUT(CFStringRef, CFSTR(""));
            }
            break;

        case kObjectID_Device:
            switch (a->mSelector) {
                case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
                case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioDeviceClassID);
                case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjectID_PlugIn);
                case kAudioObjectPropertyName: PUT(CFStringRef, CFSTR(kDevice_Name));
                case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR(kManufacturer));
                case kAudioDevicePropertyDeviceUID: PUT(CFStringRef, CFSTR(kDevice_UID));
                case kAudioDevicePropertyModelUID: PUT(CFStringRef, CFSTR(kDevice_ModelUID));
                case kAudioDevicePropertyTransportType: PUT(UInt32, kAudioDeviceTransportTypeVirtual);
                case kAudioDevicePropertyRelatedDevices: {
                    AudioObjectID ids[1] = {kObjectID_Device};
                    PUT_IDS(ids, 1);
                }
                case kAudioDevicePropertyClockDomain: PUT(UInt32, 0);
                case kAudioDevicePropertyDeviceIsAlive: PUT(UInt32, 1);
                case kAudioDevicePropertyDeviceIsRunning: {
                    pthread_mutex_lock(&gStateMutex);
                    UInt32 running = gIOCount > 0;
                    pthread_mutex_unlock(&gStateMutex);
                    PUT(UInt32, running);
                }
                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                    // Usable as the default *input*; never steal the default output.
                    PUT(UInt32, a->mScope == kAudioObjectPropertyScopeInput ? 1 : 0);
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: PUT(UInt32, 0);
                case kAudioDevicePropertyLatency: PUT(UInt32, 0);
                case kAudioDevicePropertySafetyOffset: PUT(UInt32, 0);
                case kAudioDevicePropertyZeroTimeStampPeriod: PUT(UInt32, kRingFrames);
                case kAudioDevicePropertyIsHidden: PUT(UInt32, 0);
                case kAudioDevicePropertyNominalSampleRate: {
                    pthread_mutex_lock(&gStateMutex);
                    Float64 r = gSampleRate;
                    pthread_mutex_unlock(&gStateMutex);
                    PUT(Float64, r);
                }
                case kAudioObjectPropertyOwnedObjects:
                case kAudioDevicePropertyStreams: {
                    AudioObjectID ids[2];
                    UInt32 n = deviceStreams(a->mScope, ids);
                    PUT_IDS(ids, n);
                }
                case kAudioObjectPropertyControlList: {
                    *outSize = 0;
                    return noErr;
                }
                case kAudioDevicePropertyAvailableNominalSampleRates: {
                    UInt32 n = kNumRates;
                    if (out) {
                        UInt32 fit = inSize / sizeof(AudioValueRange);
                        if (n > fit) n = fit;
                        AudioValueRange *r = (AudioValueRange *)out;
                        for (UInt32 i = 0; i < n; i++) r[i].mMinimum = r[i].mMaximum = kRates[i];
                    }
                    *outSize = n * sizeof(AudioValueRange);
                    return noErr;
                }
                case kAudioDevicePropertyPreferredChannelsForStereo: {
                    if (out) {
                        if (inSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                        ((UInt32 *)out)[0] = 1;
                        ((UInt32 *)out)[1] = 2;
                    }
                    *outSize = 2 * sizeof(UInt32);
                    return noErr;
                }
                case kAudioDevicePropertyPreferredChannelLayout: {
                    UInt32 size = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) +
                                           kChannels * sizeof(AudioChannelDescription));
                    if (out) {
                        if (inSize < size) return kAudioHardwareBadPropertySizeError;
                        AudioChannelLayout *l = (AudioChannelLayout *)out;
                        memset(l, 0, size);
                        l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
                        l->mNumberChannelDescriptions = kChannels;
                        l->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
                        l->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
                    }
                    *outSize = size;
                    return noErr;
                }
            }
            break;

        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output: {
            bool input = obj == kObjectID_Stream_Input;
            switch (a->mSelector) {
                case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
                case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioStreamClassID);
                case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjectID_Device);
                case kAudioObjectPropertyOwnedObjects: {
                    *outSize = 0;
                    return noErr;
                }
                case kAudioStreamPropertyIsActive: {
                    pthread_mutex_lock(&gStateMutex);
                    UInt32 v = input ? gInputActive : gOutputActive;
                    pthread_mutex_unlock(&gStateMutex);
                    PUT(UInt32, v);
                }
                case kAudioStreamPropertyDirection: PUT(UInt32, input ? 1 : 0);
                case kAudioStreamPropertyTerminalType:
                    PUT(UInt32, input ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker);
                case kAudioStreamPropertyStartingChannel: PUT(UInt32, 1);
                case kAudioStreamPropertyLatency: PUT(UInt32, 0);
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat: {
                    pthread_mutex_lock(&gStateMutex);
                    AudioStreamBasicDescription f = streamFormat(gSampleRate);
                    pthread_mutex_unlock(&gStateMutex);
                    PUT(AudioStreamBasicDescription, f);
                }
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats: {
                    UInt32 n = kNumRates;
                    if (out) {
                        UInt32 fit = inSize / sizeof(AudioStreamRangedDescription);
                        if (n > fit) n = fit;
                        AudioStreamRangedDescription *r = (AudioStreamRangedDescription *)out;
                        for (UInt32 i = 0; i < n; i++) {
                            r[i].mFormat = streamFormat(kRates[i]);
                            r[i].mSampleRateRange.mMinimum = r[i].mSampleRateRange.mMaximum = kRates[i];
                        }
                    }
                    *outSize = n * sizeof(AudioStreamRangedDescription);
                    return noErr;
                }
            }
            break;
        }
    }
    return kAudioHardwareUnknownPropertyError;
#undef PUT
#undef PUT_IDS
}

static OSStatus VS_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                       const AudioObjectPropertyAddress *a, UInt32 qSize, const void *qData,
                                       UInt32 *outSize) {
    (void)pid;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (!a || !outSize) return kAudioHardwareIllegalOperationError;
    if (!knownObject(obj)) return kAudioHardwareBadObjectError;
    return getProperty(obj, a, qSize, qData, 0, outSize, NULL);
}

static OSStatus VS_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                   const AudioObjectPropertyAddress *a, UInt32 qSize, const void *qData,
                                   UInt32 inSize, UInt32 *outSize, void *out) {
    (void)pid;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (!a || !outSize || !out) return kAudioHardwareIllegalOperationError;
    if (!knownObject(obj)) return kAudioHardwareBadObjectError;
    return getProperty(obj, a, qSize, qData, inSize, outSize, out);
}

static bool supportedRate(Float64 r) {
    for (UInt32 i = 0; i < kNumRates; i++)
        if (r == kRates[i]) return true;
    return false;
}

static OSStatus requestRate(Float64 rate) {
    if (!supportedRate(rate)) return kAudioHardwareIllegalOperationError;
    pthread_mutex_lock(&gStateMutex);
    bool same = rate == gSampleRate;
    if (!same) gPendingRate = rate;
    pthread_mutex_unlock(&gStateMutex);
    if (!same && gHost) gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, 0, NULL);
    return noErr;
}

static OSStatus VS_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                   const AudioObjectPropertyAddress *a, UInt32 qSize, const void *qData,
                                   UInt32 inSize, const void *in) {
    (void)pid; (void)qSize; (void)qData;
    if (d != gDriverRef) return kAudioHardwareBadObjectError;
    if (!a || !in) return kAudioHardwareIllegalOperationError;
    if (obj == kObjectID_Device && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        return requestRate(*(const Float64 *)in);
    }
    if (isStream(obj)) {
        if (a->mSelector == kAudioStreamPropertyIsActive) {
            if (inSize != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            bool v = *(const UInt32 *)in != 0;
            pthread_mutex_lock(&gStateMutex);
            bool *slot = obj == kObjectID_Stream_Input ? &gInputActive : &gOutputActive;
            bool changed = *slot != v;
            *slot = v;
            pthread_mutex_unlock(&gStateMutex);
            if (changed && gHost) {
                AudioObjectPropertyAddress ch = {kAudioStreamPropertyIsActive, kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain};
                gHost->PropertiesChanged(gHost, obj, 1, &ch);
            }
            return noErr;
        }
        if (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat) {
            if (inSize != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
            const AudioStreamBasicDescription *f = (const AudioStreamBasicDescription *)in;
            AudioStreamBasicDescription want = streamFormat(f->mSampleRate);
            if (f->mFormatID != want.mFormatID || f->mChannelsPerFrame != want.mChannelsPerFrame ||
                f->mBitsPerChannel != want.mBitsPerChannel || f->mBytesPerFrame != want.mBytesPerFrame)
                return kAudioDeviceUnsupportedFormatError;
            return requestRate(f->mSampleRate);
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

// MARK: - IO

static OSStatus VS_StartIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client) {
    (void)client;
    if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
    OSStatus err = noErr;
    pthread_mutex_lock(&gStateMutex);
    if (gIOCount == 0) {
        if (!gRing) gRing = (Float32 *)calloc(kRingFrames * kChannels, sizeof(Float32));
        if (!gRing) {
            err = kAudioHardwareUnspecifiedError;
        } else {
            memset(gRing, 0, kRingFrames * kChannels * sizeof(Float32));
            gTimestampCount = 0;
            gAnchorHostTime = mach_absolute_time();
            gWriteEnd = -1;
        }
    }
    if (!err) gIOCount++;
    pthread_mutex_unlock(&gStateMutex);
    return err;
}

static OSStatus VS_StopIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client) {
    (void)client;
    if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gStateMutex);
    if (gIOCount > 0) gIOCount--;
    pthread_mutex_unlock(&gStateMutex);
    return noErr;
}

static OSStatus VS_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client,
                                    Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed) {
    (void)client;
    if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
    UInt64 now = mach_absolute_time();
    Float64 ticksPerRing = gHostTicksPerFrame * (Float64)kRingFrames;
    Float64 nextOffset = (Float64)(gTimestampCount + 1) * ticksPerRing;
    UInt64 nextTime = gAnchorHostTime + (UInt64)nextOffset;
    if (nextTime <= now) gTimestampCount++;
    *outSampleTime = (Float64)(gTimestampCount * kRingFrames);
    *outHostTime = gAnchorHostTime + (UInt64)((Float64)gTimestampCount * ticksPerRing);
    *outSeed = 1;
    return noErr;
}

static OSStatus VS_WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                                     Boolean *willDo, Boolean *inPlace) {
    (void)client;
    if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
    bool yes = op == kAudioServerPlugInIOOperationReadInput || op == kAudioServerPlugInIOOperationWriteMix;
    if (willDo) *willDo = yes;
    if (inPlace) *inPlace = true;
    return noErr;
}

static OSStatus VS_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                                    UInt32 frames, const AudioServerPlugInIOCycleInfo *info) {
    (void)client; (void)op; (void)frames; (void)info;
    return (d == gDriverRef && dev == kObjectID_Device) ? noErr : kAudioHardwareBadObjectError;
}

static OSStatus VS_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op,
                                  UInt32 frames, const AudioServerPlugInIOCycleInfo *info) {
    (void)client; (void)op; (void)frames; (void)info;
    return (d == gDriverRef && dev == kObjectID_Device) ? noErr : kAudioHardwareBadObjectError;
}

static OSStatus VS_DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, AudioObjectID stream,
                                 UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *info,
                                 void *main, void *secondary) {
    (void)client; (void)secondary;
    if (d != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (!gRing || !main) return noErr;
    Float32 *buf = (Float32 *)main;

    if (op == kAudioServerPlugInIOOperationWriteMix && stream == kObjectID_Stream_Output) {
        Float64 t = info->mOutputTime.mSampleTime;
        UInt64 start = (UInt64)t;
        for (UInt32 i = 0; i < frames; i++) {
            UInt32 slot = (UInt32)((start + i) % kRingFrames) * kChannels;
            for (UInt32 c = 0; c < kChannels; c++) gRing[slot + c] = buf[i * kChannels + c];
        }
        gWriteEnd = t + frames;
    } else if (op == kAudioServerPlugInIOOperationReadInput && stream == kObjectID_Stream_Input) {
        Float64 t = info->mInputTime.mSampleTime;
        UInt64 start = (UInt64)t;
        for (UInt32 i = 0; i < frames; i++) {
            Float64 st = t + i;
            UInt32 slot = (UInt32)((start + i) % kRingFrames) * kChannels;
            // Only hand out samples that were written in this ring lap; otherwise silence
            // (the app stopped, or hasn't written this far yet).
            bool fresh = gWriteEnd >= 0 && st < gWriteEnd && st >= gWriteEnd - kRingFrames;
            for (UInt32 c = 0; c < kChannels; c++) {
                buf[i * kChannels + c] = fresh ? gRing[slot + c] : 0.f;
                gRing[slot + c] = 0.f;  // never replay stale audio
            }
        }
    }
    return noErr;
}
