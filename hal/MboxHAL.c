/*
 * Core Audio AudioServerPlugIn for the Mbox 2 family.
 *
 * Runs inside coreaudiod and publishes one device with a stereo input and
 * a stereo output stream. It does no USB work itself: audio and the device
 * clock come from mboxd through shared memory (see core/include/mbox/shm.h).
 *
 * Object tree:
 *   kAudioObjectPlugInObject (1)
 *     kObjectDevice (2)
 *       kObjectInputStream (3)
 *       kObjectOutputStream (4)
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "mbox/hosttime.h"
#include "mbox/ring.h"
#include "mbox/shm.h"
#include "transport_client.h"

enum {
    kObjectPlugIn = kAudioObjectPlugInObject,
    kObjectDevice = 2,
    kObjectInputStream = 3,
    kObjectOutputStream = 4,
};

#define kDeviceUID "com.rockgxyz.mbox2.device"
#define kModelUID "com.rockgxyz.mbox2.model"
#define kManufacturer "Digidesign (community driver)"
#define kChannels 2
#define kDefaultRate 48000.0
#define kDefaultZTSPeriod 512
#define kSafetyMarginFrames 48 /* 1 ms on top of the USB queue depth */
/* Anchors older than this mean mboxd stopped updating them. */
#define kStaleAnchorSeconds 0.5

/* ---- state ------------------------------------------------------------ */

static AudioServerPlugInHostRef g_host;
static _Atomic(void *) g_region; /* mapped mbox_shm_header, or NULL */
static atomic_uint g_refcount = 1;
static atomic_uint g_io_clients;
static dispatch_source_t g_reconnect_timer;

/* Synthetic clock used while no device is attached, so clients that keep
 * the device open do not stall. Only touched under g_clock_lock or from
 * the IO thread after StartIO. */
static pthread_mutex_t g_clock_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_synth_anchor_host;
static double g_host_ticks_per_frame;

static mbox_shm_header *region(void)
{
    return atomic_load_explicit(&g_region, memory_order_acquire);
}

static Float64 sample_rate(void)
{
    mbox_shm_header *h = region();
    return h && h->sample_rate ? (Float64)h->sample_rate : kDefaultRate;
}

static UInt32 zts_period(void)
{
    mbox_shm_header *h = region();
    return h ? h->zts_period : kDefaultZTSPeriod;
}

/* True if mboxd reports a running device and its clock is fresh. */
static bool device_running(mbox_anchor *anchor)
{
    mbox_shm_header *h = region();
    if (!h || atomic_load_explicit(&h->state, memory_order_acquire) != MBOX_STATE_RUNNING)
        return false;
    mbox_anchor a;
    if (!mbox_shm_read_anchor(h, &a))
        return false;
    double age = ((double)mbox_host_time_now() - (double)a.host_time) / mbox_host_ticks_per_second();
    if (age > kStaleAnchorSeconds)
        return false;
    if (anchor)
        *anchor = a;
    return true;
}

static void notify_device_changed(void)
{
    if (!g_host)
        return;
    AudioObjectPropertyAddress addrs[] = {
        {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
        {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
        {kAudioDevicePropertyAvailableNominalSampleRates, kAudioObjectPropertyScopeGlobal,
         kAudioObjectPropertyElementMain},
        {kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain},
        {kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain},
    };
    g_host->PropertiesChanged(g_host, kObjectDevice, sizeof addrs / sizeof addrs[0], addrs);
}

/* Background: (re)connect to mboxd whenever we have no live mapping. The
 * old mapping is intentionally leaked; the IO thread may still hold it. */
static void reconnect_tick(void *ctx)
{
    (void)ctx;
    if (region() && !mbox_client_lost())
        return;
    size_t size;
    void *base = mbox_client_map(&size);
    if (!base)
        return;
    atomic_store_explicit(&g_region, base, memory_order_release);
    notify_device_changed();
}

/* ---- formats ---------------------------------------------------------- */

static AudioStreamBasicDescription stream_format(void)
{
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = sample_rate();
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = kChannels * sizeof(Float32);
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = kChannels * sizeof(Float32);
    f.mChannelsPerFrame = kChannels;
    f.mBitsPerChannel = 32;
    return f;
}

static UInt32 safety_offset(bool input)
{
    mbox_shm_header *h = region();
    UInt32 queued = h ? (input ? h->in_latency : h->out_latency) : 0;
    return queued + kSafetyMarginFrames;
}

/* ---- property access -------------------------------------------------- *
 * One function answers both GetPropertyDataSize (out == NULL) and
 * GetPropertyData, so sizes and data can never disagree. */

#define RETURN_SCALAR(type, value)                                    \
    do {                                                              \
        if (out) {                                                    \
            if (cap < sizeof(type))                                   \
                return kAudioHardwareBadPropertySizeError;            \
            *(type *)out = (value);                                   \
        }                                                             \
        *size = sizeof(type);                                         \
        return kAudioHardwareNoError;                                 \
    } while (0)

/* CFStrings are returned retained; the caller releases them. */
#define RETURN_STRING(cstr) RETURN_SCALAR(CFStringRef, out ? CFStringCreateWithCString(NULL, (cstr), kCFStringEncodingUTF8) : NULL)

static OSStatus return_ids(const AudioObjectID *ids, UInt32 n, UInt32 cap, UInt32 *size, void *out)
{
    if (out) {
        UInt32 fit = cap / sizeof(AudioObjectID);
        if (fit < n)
            n = fit;
        memcpy(out, ids, n * sizeof(AudioObjectID));
    }
    *size = n * sizeof(AudioObjectID);
    return kAudioHardwareNoError;
}

static OSStatus plugin_property(const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata, UInt32 cap,
                                UInt32 *size, void *out)
{
    static const AudioObjectID devices[] = {kObjectDevice};
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:
        RETURN_SCALAR(AudioClassID, kAudioObjectClassID);
    case kAudioObjectPropertyClass:
        RETURN_SCALAR(AudioClassID, kAudioPlugInClassID);
    case kAudioObjectPropertyOwner:
        RETURN_SCALAR(AudioObjectID, kAudioObjectUnknown);
    case kAudioObjectPropertyManufacturer:
        RETURN_STRING(kManufacturer);
    case kAudioObjectPropertyOwnedObjects:
    case kAudioPlugInPropertyDeviceList:
        return return_ids(devices, 1, cap, size, out);
    case kAudioPlugInPropertyTranslateUIDToDevice: {
        AudioObjectID id = kAudioObjectUnknown;
        if (qsize == sizeof(CFStringRef) && qdata) {
            CFStringRef uid = *(const CFStringRef *)qdata;
            if (uid && CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo)
                id = kObjectDevice;
        }
        RETURN_SCALAR(AudioObjectID, id);
    }
    case kAudioPlugInPropertyResourceBundle:
        RETURN_SCALAR(CFStringRef, out ? CFSTR("") : NULL);
    default:
        return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus device_property(const AudioObjectPropertyAddress *a, UInt32 cap, UInt32 *size, void *out)
{
    bool input = a->mScope == kAudioObjectPropertyScopeInput;
    bool output = a->mScope == kAudioObjectPropertyScopeOutput;
    bool global = a->mScope == kAudioObjectPropertyScopeGlobal;

    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:
        RETURN_SCALAR(AudioClassID, kAudioObjectClassID);
    case kAudioObjectPropertyClass:
        RETURN_SCALAR(AudioClassID, kAudioDeviceClassID);
    case kAudioObjectPropertyOwner:
        RETURN_SCALAR(AudioObjectID, kObjectPlugIn);
    case kAudioObjectPropertyName: {
        mbox_shm_header *h = region();
        RETURN_STRING(h && h->model_name[0] ? h->model_name : "Mbox 2");
    }
    case kAudioObjectPropertyManufacturer:
        RETURN_STRING(kManufacturer);
    case kAudioObjectPropertyOwnedObjects:
    case kAudioDevicePropertyStreams: {
        static const AudioObjectID both[] = {kObjectInputStream, kObjectOutputStream};
        static const AudioObjectID in[] = {kObjectInputStream};
        static const AudioObjectID outs[] = {kObjectOutputStream};
        if (input)
            return return_ids(in, 1, cap, size, out);
        if (output)
            return return_ids(outs, 1, cap, size, out);
        return return_ids(both, global ? 2 : 0, cap, size, out);
    }
    case kAudioObjectPropertyControlList:
        return return_ids(NULL, 0, cap, size, out);
    case kAudioDevicePropertyDeviceUID:
        RETURN_STRING(kDeviceUID);
    case kAudioDevicePropertyModelUID:
        RETURN_STRING(kModelUID);
    case kAudioDevicePropertyTransportType:
        RETURN_SCALAR(UInt32, kAudioDeviceTransportTypeUSB);
    case kAudioDevicePropertyRelatedDevices: {
        static const AudioObjectID self[] = {kObjectDevice};
        return return_ids(self, 1, cap, size, out);
    }
    case kAudioDevicePropertyClockDomain:
        RETURN_SCALAR(UInt32, 0);
    case kAudioDevicePropertyDeviceIsAlive:
        RETURN_SCALAR(UInt32, 1);
    case kAudioDevicePropertyDeviceIsRunning:
        RETURN_SCALAR(UInt32, atomic_load(&g_io_clients) > 0);
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        RETURN_SCALAR(UInt32, 1);
    case kAudioDevicePropertyLatency:
        RETURN_SCALAR(UInt32, 0);
    case kAudioDevicePropertySafetyOffset:
        RETURN_SCALAR(UInt32, safety_offset(input));
    case kAudioDevicePropertyNominalSampleRate:
        RETURN_SCALAR(Float64, sample_rate());
    case kAudioDevicePropertyAvailableNominalSampleRates: {
        Float64 r = sample_rate();
        AudioValueRange range = {r, r};
        RETURN_SCALAR(AudioValueRange, range);
    }
    case kAudioDevicePropertyIsHidden:
        RETURN_SCALAR(UInt32, 0);
    case kAudioDevicePropertyZeroTimeStampPeriod:
        RETURN_SCALAR(UInt32, zts_period());
    case kAudioDevicePropertyPreferredChannelsForStereo: {
        if (out) {
            if (cap < 2 * sizeof(UInt32))
                return kAudioHardwareBadPropertySizeError;
            ((UInt32 *)out)[0] = 1;
            ((UInt32 *)out)[1] = 2;
        }
        *size = 2 * sizeof(UInt32);
        return kAudioHardwareNoError;
    }
    default:
        return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus stream_property(AudioObjectID id, const AudioObjectPropertyAddress *a, UInt32 cap, UInt32 *size,
                                void *out)
{
    bool input = id == kObjectInputStream;
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:
        RETURN_SCALAR(AudioClassID, kAudioObjectClassID);
    case kAudioObjectPropertyClass:
        RETURN_SCALAR(AudioClassID, kAudioStreamClassID);
    case kAudioObjectPropertyOwner:
        RETURN_SCALAR(AudioObjectID, kObjectDevice);
    case kAudioObjectPropertyOwnedObjects:
        return return_ids(NULL, 0, cap, size, out);
    case kAudioObjectPropertyName:
        RETURN_STRING(input ? "Mbox Input 1-2" : "Mbox Output 1-2");
    case kAudioStreamPropertyIsActive:
        RETURN_SCALAR(UInt32, 1);
    case kAudioStreamPropertyDirection:
        RETURN_SCALAR(UInt32, input ? 1 : 0);
    case kAudioStreamPropertyTerminalType:
        RETURN_SCALAR(UInt32, input ? kAudioStreamTerminalTypeLine : kAudioStreamTerminalTypeSpeaker);
    case kAudioStreamPropertyStartingChannel:
        RETURN_SCALAR(UInt32, 1);
    case kAudioStreamPropertyLatency:
        RETURN_SCALAR(UInt32, 0);
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
        RETURN_SCALAR(AudioStreamBasicDescription, stream_format());
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats: {
        AudioStreamRangedDescription d;
        d.mFormat = stream_format();
        d.mSampleRateRange.mMinimum = d.mSampleRateRange.mMaximum = d.mFormat.mSampleRate;
        RETURN_SCALAR(AudioStreamRangedDescription, d);
    }
    default:
        return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus property(AudioObjectID id, const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata,
                         UInt32 cap, UInt32 *size, void *out)
{
    switch (id) {
    case kObjectPlugIn:
        return plugin_property(a, qsize, qdata, cap, size, out);
    case kObjectDevice:
        return device_property(a, cap, size, out);
    case kObjectInputStream:
    case kObjectOutputStream:
        return stream_property(id, a, cap, size, out);
    default:
        return kAudioHardwareBadObjectError;
    }
}

/* ---- driver interface ------------------------------------------------- */

static AudioServerPlugInDriverInterface g_interface;
static AudioServerPlugInDriverInterface *g_interface_ptr = &g_interface;
static AudioServerPlugInDriverRef g_driver = &g_interface_ptr;

static HRESULT MboxHAL_QueryInterface(void *driver, REFIID uuid, LPVOID *out)
{
    if (driver != g_driver || !out)
        return kAudioHardwareBadObjectError;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
        atomic_fetch_add(&g_refcount, 1);
        *out = g_driver;
        r = S_OK;
    }
    CFRelease(requested);
    return r;
}

static ULONG MboxHAL_AddRef(void *driver)
{
    return atomic_fetch_add(&g_refcount, 1) + 1;
}

static ULONG MboxHAL_Release(void *driver)
{
    unsigned n = atomic_load(&g_refcount);
    while (n > 0 && !atomic_compare_exchange_weak(&g_refcount, &n, n - 1))
        ;
    return n > 0 ? n - 1 : 0;
}

static OSStatus MboxHAL_Initialize(AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef host)
{
    g_host = host;
    g_host_ticks_per_frame = mbox_host_ticks_per_second() / kDefaultRate;

    dispatch_queue_t q = dispatch_queue_create("com.rockgxyz.mbox2.reconnect", DISPATCH_QUEUE_SERIAL);
    g_reconnect_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
    dispatch_source_set_timer(g_reconnect_timer, dispatch_time(DISPATCH_TIME_NOW, 0), 2 * NSEC_PER_SEC,
                              NSEC_PER_SEC / 4);
    dispatch_source_set_event_handler_f(g_reconnect_timer, reconnect_tick);
    dispatch_resume(g_reconnect_timer);
    return kAudioHardwareNoError;
}

static OSStatus MboxHAL_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc,
                                     const AudioServerPlugInClientInfo *client, AudioObjectID *out)
{
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus MboxHAL_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id)
{
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus MboxHAL_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id,
                                        const AudioServerPlugInClientInfo *client)
{
    return id == kObjectDevice ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

static OSStatus MboxHAL_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id,
                                           const AudioServerPlugInClientInfo *client)
{
    return id == kObjectDevice ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

static OSStatus MboxHAL_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID id,
                                                         UInt64 action, void *info)
{
    return kAudioHardwareNoError;
}

static OSStatus MboxHAL_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID id,
                                                       UInt64 action, void *info)
{
    return kAudioHardwareNoError;
}

static Boolean MboxHAL_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid,
                                   const AudioObjectPropertyAddress *a)
{
    UInt32 size = 0;
    /* Qualified lookups (TranslateUIDToDevice) still report existence. */
    return property(id, a, 0, NULL, 0, &size, NULL) == kAudioHardwareNoError;
}

static OSStatus MboxHAL_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid,
                                           const AudioObjectPropertyAddress *a, Boolean *settable)
{
    UInt32 size = 0;
    OSStatus r = property(id, a, 0, NULL, 0, &size, NULL);
    if (r == kAudioHardwareNoError)
        *settable = false; /* rate and format follow the hardware */
    return r;
}

static OSStatus MboxHAL_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid,
                                            const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata,
                                            UInt32 *size)
{
    return property(id, a, qsize, qdata, 0, size, NULL);
}

static OSStatus MboxHAL_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid,
                                        const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata,
                                        UInt32 cap, UInt32 *size, void *out)
{
    return property(id, a, qsize, qdata, cap, size, out);
}

static OSStatus MboxHAL_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid,
                                        const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata,
                                        UInt32 size, const void *data)
{
    UInt32 unused;
    OSStatus r = property(id, a, qsize, qdata, 0, &unused, NULL);
    return r == kAudioHardwareNoError ? kAudioHardwareIllegalOperationError : r;
}

static OSStatus MboxHAL_StartIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client)
{
    if (id != kObjectDevice)
        return kAudioHardwareBadObjectError;
    if (atomic_fetch_add(&g_io_clients, 1) == 0) {
        pthread_mutex_lock(&g_clock_lock);
        g_synth_anchor_host = mbox_host_time_now();
        g_host_ticks_per_frame = mbox_host_ticks_per_second() / sample_rate();
        pthread_mutex_unlock(&g_clock_lock);
        mbox_shm_header *h = region();
        if (h)
            atomic_fetch_add(&h->client_io_count, 1);
    }
    return kAudioHardwareNoError;
}

static OSStatus MboxHAL_StopIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client)
{
    if (id != kObjectDevice)
        return kAudioHardwareBadObjectError;
    unsigned n = atomic_load(&g_io_clients);
    while (n > 0 && !atomic_compare_exchange_weak(&g_io_clients, &n, n - 1))
        ;
    if (n == 1) {
        mbox_shm_header *h = region();
        if (h && atomic_load(&h->client_io_count) > 0)
            atomic_fetch_sub(&h->client_io_count, 1);
    }
    return kAudioHardwareNoError;
}

static OSStatus MboxHAL_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client,
                                         Float64 *sample_time, UInt64 *host_time, UInt64 *seed)
{
    mbox_anchor a;
    if (device_running(&a)) {
        *sample_time = (Float64)a.sample_time;
        *host_time = a.host_time;
        *seed = a.generation + 1;
        return kAudioHardwareNoError;
    }
    /* No device: free-run on the host clock at the nominal rate. */
    UInt32 period = zts_period();
    double ticks_per_period = g_host_ticks_per_frame * period;
    uint64_t now = mbox_host_time_now();
    uint64_t n = (uint64_t)((double)(now - g_synth_anchor_host) / ticks_per_period);
    *sample_time = (Float64)(n * period);
    *host_time = g_synth_anchor_host + (uint64_t)((double)n * ticks_per_period);
    *seed = 0;
    return kAudioHardwareNoError;
}

static OSStatus MboxHAL_WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op,
                                          Boolean *will_do, Boolean *in_place)
{
    *will_do = op == kAudioServerPlugInIOOperationReadInput || op == kAudioServerPlugInIOOperationWriteMix;
    *in_place = true;
    return kAudioHardwareNoError;
}

static OSStatus MboxHAL_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op,
                                         UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle)
{
    return kAudioHardwareNoError;
}

static OSStatus MboxHAL_DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, AudioObjectID stream,
                                      UInt32 client, UInt32 op, UInt32 frames,
                                      const AudioServerPlugInIOCycleInfo *cycle, void *buffer, void *secondary)
{
    mbox_shm_header *h = region();
    bool live = h && device_running(NULL);

    if (op == kAudioServerPlugInIOOperationReadInput && stream == kObjectInputStream) {
        if (live && h->in_channels == kChannels && h->in_stride == kChannels) {
            mbox_tl_ring r = mbox_shm_input_ring(h);
            mbox_tl_read(&r, (int64_t)cycle->mInputTime.mSampleTime, buffer, frames);
        } else {
            memset(buffer, 0, (size_t)frames * kChannels * sizeof(Float32));
        }
    } else if (op == kAudioServerPlugInIOOperationWriteMix && stream == kObjectOutputStream) {
        if (live && h->out_channels == kChannels && h->out_stride == kChannels) {
            mbox_tl_ring r = mbox_shm_output_ring(h);
            mbox_tl_write(&r, (int64_t)cycle->mOutputTime.mSampleTime, buffer, frames);
        }
    }
    return kAudioHardwareNoError;
}

static OSStatus MboxHAL_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op,
                                       UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle)
{
    return kAudioHardwareNoError;
}

static AudioServerPlugInDriverInterface g_interface = {
    NULL,
    MboxHAL_QueryInterface,
    MboxHAL_AddRef,
    MboxHAL_Release,
    MboxHAL_Initialize,
    MboxHAL_CreateDevice,
    MboxHAL_DestroyDevice,
    MboxHAL_AddDeviceClient,
    MboxHAL_RemoveDeviceClient,
    MboxHAL_PerformDeviceConfigurationChange,
    MboxHAL_AbortDeviceConfigurationChange,
    MboxHAL_HasProperty,
    MboxHAL_IsPropertySettable,
    MboxHAL_GetPropertyDataSize,
    MboxHAL_GetPropertyData,
    MboxHAL_SetPropertyData,
    MboxHAL_StartIO,
    MboxHAL_StopIO,
    MboxHAL_GetZeroTimeStamp,
    MboxHAL_WillDoIOOperation,
    MboxHAL_BeginIOOperation,
    MboxHAL_DoIOOperation,
    MboxHAL_EndIOOperation,
};

/* Factory named in Info.plist (CFPlugInFactories). */
__attribute__((visibility("default"))) void *MboxHAL_Create(CFAllocatorRef allocator, CFUUIDRef type)
{
    if (!CFEqual(type, kAudioServerPlugInTypeUUID))
        return NULL;
    return g_driver;
}
