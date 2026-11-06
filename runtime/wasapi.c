// WASAPI / Windows Core Audio (mmdevapi.dll + audioses): IMMDeviceEnumerator, IMMDevice(+IMMEndpoint), IMMDeviceCollection,
// IPropertyStore, IAudioClient/2/3 with IAudioRenderClient, IAudioCaptureClient, IAudioClock/2, ISimpleAudioVolume,
// IAudioStreamVolume, IChannelAudioVolume, IAudioSessionControl/2, IAudioSessionManager/2 (+enumerator),
// IAudioEndpointVolume/Ex, IAudioMeterInformation, and ActivateAudioInterfaceAsync.
//
// Design (semantics follow the Microsoft Core Audio docs; Wine's mmdevapi/winecoreaudio was the behavioural reference):
//   * Two endpoints: "Speakers (wintomac)" (render) and "Microphone (wintomac)" (capture, always silent: the Mac microphone
//     is never opened, so a packaged app needs no microphone permission).
//   * The shared-mode mix format is WAVEFORMATEXTENSIBLE float32, 48 kHz stereo (WINTOMAC_WASAPI_RATE / _CHANNELS override).
//     Initialize accepts any PCM (8/16/24/32-bit) or float32 format with 1-8 channels and converts it (SDL3 audio streams
//     do resampling/channel mapping), as if AUTOCONVERTPCM were always set. IsFormatSupported is strict like Windows
//     (S_OK only for the mix format, S_FALSE + closest match otherwise).
//   * Each render client owns a ring buffer of its requested size in guest memory (identity mapped: GetBuffer returns a
//     pointer into it; a wrapped request gets a bounce buffer copied in by ReleaseBuffer). One host engine thread ticks
//     every 10 ms (the device period): it moves one period from each started client's ring into that client's SDL audio
//     stream (bound to the default output device), advances the clock, and signals the client's event (event-driven mode).
//     The SDL queue depth paces it (skip a period when >3 periods are queued, take two when it is nearly dry), so the
//     10 ms timer cannot drift away from the real device clock.
//   * Exclusive mode is reported as disabled by the user (AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED), which every engine
//     (FMOD, Wwise, SDL, Unity, Unreal) handles by falling back to shared mode. WINTOMAC_WASAPI_EXCLUSIVE=1 instead
//     accepts exclusive streams and runs them as shared ones.
//   * WINTOMAC_AUDIO_NULL=1 (or no audio device) keeps the engine clock running without output.
//   * No guest code runs on the engine thread: it only calls rt_signal_event. ActivateAudioInterfaceAsync calls the
//     completion handler synchronously on the calling thread.
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "com.h"

#define G(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } }
static const Guid CLSID_MMDeviceEnumerator_ = G(0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
static const Guid IID_IMMDeviceEnumerator_ = G(0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
static const Guid IID_IMMDevice_ = G(0xD666063F, 0x1587, 0x4E43, 0x81, 0xF1, 0xB9, 0x48, 0xE8, 0x07, 0x36, 0x3F);
static const Guid IID_IMMEndpoint_ = G(0x1BE09788, 0x6894, 0x4089, 0x85, 0x86, 0x9A, 0x2A, 0x6C, 0x26, 0x5A, 0xC5);
static const Guid IID_IMMDeviceCollection_ = G(0x0BD7A1BE, 0x7A1A, 0x44DB, 0x83, 0x97, 0xCC, 0x53, 0x92, 0x38, 0x7B, 0x5E);
static const Guid IID_IPropertyStore_ = G(0x886D8EEB, 0x8CF2, 0x4446, 0x8D, 0x02, 0xCD, 0xBA, 0x1D, 0xBD, 0xCF, 0x99);
static const Guid IID_IAudioClient_ = G(0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
static const Guid IID_IAudioClient2_ = G(0x726778CD, 0xF60A, 0x4EDA, 0x82, 0xDE, 0xE4, 0x76, 0x10, 0xCD, 0x78, 0xAA);
static const Guid IID_IAudioClient3_ = G(0x7ED4EE07, 0x8E67, 0x4CD4, 0x8C, 0x1A, 0x2B, 0x7A, 0x59, 0x87, 0xAD, 0x42);
static const Guid IID_IAudioRenderClient_ = G(0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);
static const Guid IID_IAudioCaptureClient_ = G(0xC8ADBD64, 0xE71E, 0x48A0, 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);
static const Guid IID_IAudioClock_ = G(0xCD63314F, 0x3FBA, 0x4A1B, 0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7);
static const Guid IID_IAudioClock2_ = G(0x6F49FF73, 0x6727, 0x49AC, 0xA0, 0x08, 0xD9, 0x8C, 0xF5, 0xE7, 0x00, 0x48);
static const Guid IID_ISimpleAudioVolume_ = G(0x87CE5498, 0x68D6, 0x44E5, 0x92, 0x15, 0x6D, 0xA4, 0x7E, 0xF8, 0x83, 0xD8);
static const Guid IID_IAudioStreamVolume_ = G(0x93014887, 0x242D, 0x4068, 0x8A, 0x15, 0xCF, 0x5E, 0x93, 0xB9, 0x0F, 0xE3);
static const Guid IID_IChannelAudioVolume_ = G(0x1C158861, 0xB533, 0x4B30, 0xB1, 0xCF, 0xE8, 0x53, 0xE5, 0x1C, 0x59, 0xB8);
static const Guid IID_IAudioSessionControl_ = G(0xF4B1A599, 0x7266, 0x4319, 0xA8, 0xCA, 0xE7, 0x0A, 0xCB, 0x11, 0xE8, 0xCD);
static const Guid IID_IAudioSessionControl2_ = G(0xBFB7FF88, 0x7239, 0x4FC9, 0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D);
static const Guid IID_IAudioSessionManager_ = G(0xBFA971F1, 0x4D5E, 0x40BB, 0x93, 0x5E, 0x96, 0x70, 0x39, 0xBF, 0xBE, 0xE4);
static const Guid IID_IAudioSessionManager2_ = G(0x77AA99A0, 0x1BD6, 0x484F, 0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F);
static const Guid IID_IAudioSessionEnumerator_ = G(0xE2F5BB11, 0x0570, 0x40CA, 0xAC, 0xDD, 0x3A, 0xA0, 0x12, 0x77, 0xDE, 0xE8);
static const Guid IID_IAudioEndpointVolume_ = G(0x5CDF2C82, 0x841E, 0x4546, 0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A);
static const Guid IID_IAudioEndpointVolumeEx_ = G(0x66E11784, 0xF695, 0x4F28, 0xA5, 0x05, 0xA7, 0x08, 0x00, 0x81, 0xA7, 0x8F);
static const Guid IID_IAudioMeterInformation_ = G(0xC02216F6, 0x8C67, 0x4B5B, 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64);
static const Guid IID_IActivateAudioInterfaceAsyncOperation_ = G(0x72A22D78, 0xCDE4, 0x431D, 0xB8, 0xCC, 0x84, 0x3A, 0x71, 0x19, 0x9B, 0x6D);
static const Guid IID_IAgileObject_ = G(0x94EA2B94, 0xE9CC, 0x49E0, 0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90);
static const Guid KSDATAFORMAT_SUBTYPE_PCM_ = G(0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
static const Guid KSDATAFORMAT_SUBTYPE_FLOAT_ = G(0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
// property keys
static const Guid FMTID_Device_ = G(0xA45C254E, 0xDF1C, 0x4EFD, 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0);          // 14 FriendlyName, 2 DeviceDesc
static const Guid FMTID_DevIface_ = G(0x026E516E, 0xB814, 0x414B, 0x83, 0xCD, 0x85, 0x6D, 0x6F, 0xEF, 0x48, 0x22);       // 2 DeviceInterface_FriendlyName
static const Guid FMTID_EngineDevFmt_ = G(0xF19F064D, 0x082C, 0x4E27, 0xBC, 0x73, 0x68, 0x82, 0xA1, 0xBB, 0x8E, 0x4C);   // 0 AudioEngine_DeviceFormat
static const Guid FMTID_EngineOEMFmt_ = G(0xE4870E26, 0x3CC5, 0x4CD2, 0xBA, 0x46, 0xCA, 0x0A, 0x9A, 0x70, 0xED, 0x04);   // 3 AudioEngine_OEMFormat
static const Guid FMTID_Endpoint_ = G(0x1DA5D803, 0xD492, 0x4EDD, 0x8C, 0x23, 0xE0, 0xC0, 0xFF, 0xEE, 0x7F, 0x0E);       // 0 FormFactor, 3 PhysicalSpeakers, 4 GUID

#define AUDCLNT_E(n) (0x88890000u | (n))
#define E_NOT_INITIALIZED AUDCLNT_E(0x01)
#define E_ALREADY_INITIALIZED AUDCLNT_E(0x02)
#define E_WRONG_ENDPOINT_TYPE AUDCLNT_E(0x03)
#define E_NOT_STOPPED AUDCLNT_E(0x05)
#define E_BUFFER_TOO_LARGE AUDCLNT_E(0x06)
#define E_OUT_OF_ORDER AUDCLNT_E(0x07)
#define E_UNSUPPORTED_FORMAT AUDCLNT_E(0x08)
#define E_INVALID_SIZE AUDCLNT_E(0x09)
#define E_BUFFER_OPERATION_PENDING AUDCLNT_E(0x0B)
#define E_EXCLUSIVE_MODE_NOT_ALLOWED AUDCLNT_E(0x0E)
#define E_EVENTHANDLE_NOT_EXPECTED AUDCLNT_E(0x11)
#define E_EVENTHANDLE_NOT_SET AUDCLNT_E(0x14)
#define E_INVALID_DEVICE_PERIOD AUDCLNT_E(0x20)
#define S_BUFFER_EMPTY 0x08890001u
#define E_NOTFOUND_ 0x80070490u
#define S_FALSE_ 1u
#define STG_E_ACCESSDENIED_ 0x80030005u

#define STREAMFLAGS_LOOPBACK 0x00020000u
#define STREAMFLAGS_EVENTCALLBACK 0x00040000u
#define BUFFERFLAGS_DATA_DISCONTINUITY 0x1u
#define BUFFERFLAGS_SILENT 0x2u
#define DEVICE_STATE_ACTIVE 0x1u
#define PERIOD_HNS 100000ll                    // 10 ms shared-mode engine period
#define MIN_PERIOD_HNS 30000ll                 // reported minimum (exclusive) period
#define MIN_SHARED_BUFFER_HNS 220000ll         // Windows' smallest shared buffer is ~22 ms (1056 frames at 48 kHz)

static int trace(void) { static int t = -1; if (t < 0) t = getenv("WINTOMAC_AUDIO_TRACE") != NULL; return t; }
static u64 now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }   // same timebase as QueryPerformanceCounter
static float argf(V128 x) { return x.f[0]; }

// ---- guest-visible objects ---------------------------------------------------------------------
// Every object starts with Hdr. Refcounted objects have destroy != NULL; sub-objects (services of a client, the
// IMMEndpoint of a device) forward their reference count to `owner`. Statics (devices, sessions) are never freed.
typedef struct Hdr Hdr;
struct Hdr {
    u64 vtbl;
    const Guid *const *iids;      // interfaces this object answers QueryInterface for (NULL-terminated)
    Hdr *owner, *alt;             // refcount owner (sub-objects) / a second interface object reachable by QI
    u32 refs;
    void (*destroy)(Hdr *);
};

typedef struct { u16 tag, ch; u32 rate, avg; u16 align, bits; } Fmt;   // parsed + normalised client format
enum { CONV_NONE, CONV_S24 };

typedef struct Device Device;
typedef struct Client Client;
typedef struct { Hdr h; Client *cl; } Sub;
struct Client {
    Hdr h;
    Device *dev;
    int capture;                  // capture semantics (capture endpoint or loopback)
    int inited, started, event_mode;
    u32 flags;
    u8 wfx[64];                   // the format as the guest gave it (GetMixFormat/IsFormatSupported don't need it)
    Fmt f; int is_float, conv; SDL_AudioFormat sdlfmt;
    u32 period, bufframes;        // in client frames
    u8 *ring, *bounce, *scratch;
    u64 wpos, rpos;               // frames written by the producer / consumed by the consumer (monotonic)
    u64 clock;                    // frames played (render) / captured (capture)
    u64 clock_qpc;                // QPC (100 ns) of the last clock advance
    int get_out; u32 get_frames; u8 *get_ptr; u64 get_pos; int discont;
    u64 event;
    float svol[8], cvol[8];
    float gain_set;
    SDL_AudioStream *stream;
    Sub render, capclient, clk, clk2, strvol, chvol;
    Client *next;
};
struct Device { Hdr h; Hdr endpoint; Hdr props; int flow; const char *name, *desc; const char *id; };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static Client *clients;
static SDL_AudioDeviceID out_dev; static int out_state;   // 0 untried, 1 device, 2 null
static Fmt mixf;
// session-wide volume state (one default session per process, like a typical game)
static float sess_vol = 1.0f; static int sess_mute;
static float ep_vol = 1.0f; static int ep_mute; static float ep_ch[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
static u16 *sess_name;

static u64 *vt_enum, *vt_dev, *vt_endp, *vt_coll, *vt_props, *vt_client, *vt_render, *vt_capture, *vt_clock, *vt_clock2, *vt_svol, *vt_strvol,
    *vt_chvol, *vt_sessctl, *vt_sessmgr, *vt_sessenum, *vt_epvol, *vt_meter, *vt_asyncop;

static const Guid *const IIDS_ENUM[] = { &IID_IMMDeviceEnumerator_, NULL };
static const Guid *const IIDS_DEV[] = { &IID_IMMDevice_, NULL };
static const Guid *const IIDS_ENDP[] = { &IID_IMMEndpoint_, NULL };
static const Guid *const IIDS_COLL[] = { &IID_IMMDeviceCollection_, NULL };
static const Guid *const IIDS_PROPS[] = { &IID_IPropertyStore_, NULL };
static const Guid *const IIDS_CLIENT[] = { &IID_IAudioClient_, &IID_IAudioClient2_, &IID_IAudioClient3_, NULL };
static const Guid *const IIDS_RENDER[] = { &IID_IAudioRenderClient_, NULL };
static const Guid *const IIDS_CAPTURE[] = { &IID_IAudioCaptureClient_, NULL };
static const Guid *const IIDS_CLOCK[] = { &IID_IAudioClock_, NULL };
static const Guid *const IIDS_CLOCK2[] = { &IID_IAudioClock2_, NULL };
static const Guid *const IIDS_SVOL[] = { &IID_ISimpleAudioVolume_, NULL };
static const Guid *const IIDS_STRVOL[] = { &IID_IAudioStreamVolume_, NULL };
static const Guid *const IIDS_CHVOL[] = { &IID_IChannelAudioVolume_, NULL };
static const Guid *const IIDS_SESSCTL[] = { &IID_IAudioSessionControl_, &IID_IAudioSessionControl2_, NULL };
static const Guid *const IIDS_SESSMGR[] = { &IID_IAudioSessionManager_, &IID_IAudioSessionManager2_, NULL };
static const Guid *const IIDS_SESSENUM[] = { &IID_IAudioSessionEnumerator_, NULL };
static const Guid *const IIDS_EPVOL[] = { &IID_IAudioEndpointVolume_, &IID_IAudioEndpointVolumeEx_, NULL };
static const Guid *const IIDS_METER[] = { &IID_IAudioMeterInformation_, NULL };
static const Guid *const IIDS_ASYNCOP[] = { &IID_IActivateAudioInterfaceAsyncOperation_, &IID_IAgileObject_, NULL };

static Hdr *root(Hdr *o) { return o->owner ? o->owner : o; }
static u32 addref(Hdr *o) { o = root(o); return o->destroy ? __atomic_add_fetch(&o->refs, 1, __ATOMIC_SEQ_CST) : 1; }
static u32 release(Hdr *o) {
    o = root(o);
    if (!o->destroy) return 1;
    u32 r = __atomic_sub_fetch(&o->refs, 1, __ATOMIC_SEQ_CST);
    if (!r) o->destroy(o);
    return r;
}
static int has_iid(Hdr *o, u64 iid) { if (!o || !o->iids) return 0; for (int i = 0; o->iids[i]; i++) if (rt_guid_eq(iid, o->iids[i])) return 1; return 0; }
static void hdr_init(Hdr *o, u64 *vt, const Guid *const *iids, Hdr *owner, void (*destroy)(Hdr *)) {
    o->vtbl = (u64)vt; o->iids = iids; o->owner = owner; o->alt = NULL; o->refs = 1; o->destroy = destroy;
}

SHIM(OBJ_QueryInterface) {
    Hdr *o = (Hdr *)A1;
    if (!A3) RET(E_POINTER_);
    Hdr *cand[4] = { o, o->owner, o->alt, o->owner ? o->owner->alt : NULL };
    if (rt_guid_eq(A2, &IID_IUnknown_)) { addref(o); ST64(A3, (u64)root(o)); RET(S_OK_); }
    for (int i = 0; i < 4; i++) if (has_iid(cand[i], A2)) { addref(cand[i]); ST64(A3, (u64)cand[i]); RET(S_OK_); }
    if (trace()) { char g[64]; rt_guid_str(A2, g, sizeof g); rt_log("wasapi: QueryInterface %s not supported", g); }
    ST64(A3, 0); RET(E_NOINTERFACE_);
}
SHIM(OBJ_AddRef) { RET(addref((Hdr *)A1)); }
SHIM(OBJ_Release) { RET(release((Hdr *)A1)); }

static u16 *wdup(const char *s) {   // CoTaskMemAlloc'd UTF-16 copy (ASCII input)
    size_t n = strlen(s); u16 *w = malloc(2 * (n + 1));
    for (size_t i = 0; i <= n; i++) w[i] = (u8)s[i];
    return w;
}
static int wmatch(u64 w, const char *s) {   // case-insensitive guest UTF-16 vs ASCII
    if (!w) return 0;
    for (size_t i = 0;; i++) {
        u16 a = LD16(w + 2 * i); u8 b = (u8)s[i];
        if ((a < 128 ? (a | 32) : a) != (b | 32) && !(a == 0 && b == 0)) return 0;
        if (!a) return 1;
    }
}
static int wcontains(u64 w, const char *s) {   // case-insensitive substring of a guest UTF-16 string
    if (!w) return 0;
    char buf[512]; size_t n = 0;
    for (; n + 1 < sizeof buf; n++) { u16 ch = LD16(w + 2 * n); if (!ch) break; buf[n] = ch < 128 ? (char)(ch | (ch >= 'A' && ch <= 'Z' ? 32 : 0)) : '?'; }
    buf[n] = 0;
    char t[128]; size_t k = 0; for (; s[k] && k + 1 < sizeof t; k++) t[k] = (char)(s[k] >= 'A' && s[k] <= 'Z' ? s[k] | 32 : s[k]); t[k] = 0;
    return strstr(buf, t) != NULL;
}

// ---- formats ---------------------------------------------------------------------------------------
static u32 mask_for(u16 ch) {
    switch (ch) { case 1: return 0x4; case 2: return 0x3; case 3: return 0x7; case 4: return 0x33; case 5: return 0x37;
                  case 6: return 0x3F; case 7: return 0x13F; case 8: return 0x63F; default: return 0; }
}
static void init_mixf(void) {
    if (mixf.ch) return;
    const char *r = getenv("WINTOMAC_WASAPI_RATE"), *ch = getenv("WINTOMAC_WASAPI_CHANNELS");
    u32 rate = r ? (u32)atoi(r) : 48000; u16 n = ch ? (u16)atoi(ch) : 2;
    if (rate < 8000 || rate > 384000) rate = 48000;
    if (n < 1 || n > 8) n = 2;
    mixf = (Fmt){ 3, n, rate, rate * n * 4, (u16)(n * 4), 32 };
}
// WAVEFORMATEXTENSIBLE (40 bytes) for the mix format, in CoTaskMemAlloc'd (malloc) memory or into `out`.
static void write_mix(u8 *w) {
    init_mixf();
    memset(w, 0, 40);
    u16 tag = 0xFFFE, cb = 22, valid = 32; u32 mask = mask_for(mixf.ch);
    memcpy(w, &tag, 2); memcpy(w + 2, &mixf.ch, 2); memcpy(w + 4, &mixf.rate, 4); memcpy(w + 8, &mixf.avg, 4);
    memcpy(w + 12, &mixf.align, 2); memcpy(w + 14, &mixf.bits, 2); memcpy(w + 16, &cb, 2); memcpy(w + 18, &valid, 2);
    memcpy(w + 20, &mask, 4); memcpy(w + 24, &KSDATAFORMAT_SUBTYPE_FLOAT_, 16);
}
static u64 alloc_mix(void) { u8 *w = malloc(40); write_mix(w); return (u64)w; }

// Parse a guest WAVEFORMATEX(TENSIBLE): 1 if we can play it.
static int parse_fmt(u64 p, Fmt *f, int *is_float) {
    if (!p) return 0;
    f->tag = LD16(p); f->ch = LD16(p + 2); f->rate = LD32(p + 4); f->avg = LD32(p + 8); f->align = LD16(p + 12); f->bits = LD16(p + 14);
    int flt;
    if (f->tag == 1) flt = 0;
    else if (f->tag == 3) flt = 1;
    else if (f->tag == 0xFFFE) {
        if (LD16(p + 16) < 22) return 0;
        if (rt_guid_eq(p + 24, &KSDATAFORMAT_SUBTYPE_PCM_)) flt = 0;
        else if (rt_guid_eq(p + 24, &KSDATAFORMAT_SUBTYPE_FLOAT_)) flt = 1;
        else return 0;
    } else return 0;
    if (f->ch < 1 || f->ch > 8 || f->rate < 8000 || f->rate > 384000) return 0;
    if (flt ? f->bits != 32 : (f->bits != 8 && f->bits != 16 && f->bits != 24 && f->bits != 32)) return 0;
    if (f->align != f->ch * f->bits / 8) return 0;
    *is_float = flt;
    return 1;
}
static int is_mix(const Fmt *f, int flt) { init_mixf(); return flt && f->ch == mixf.ch && f->rate == mixf.rate && f->bits == 32; }

// ---- output device + engine thread ------------------------------------------------------------------
static int open_output(void) {
    if (out_state) return out_state == 1;
    out_state = 2;
    if (getenv("WINTOMAC_AUDIO_NULL")) return 0;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { rt_log("wasapi: SDL audio init failed (%s); silent output", SDL_GetError()); return 0; }
    out_dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
    if (!out_dev) { rt_log("wasapi: no audio device (%s); silent output", SDL_GetError()); return 0; }
    out_state = 1;
    return 1;
}

static float client_gain(void) { return (sess_mute || ep_mute) ? 0.0f : sess_vol * ep_vol; }

// Copy n frames starting at ring frame `pos` into the client's SDL stream (24-bit expansion, per-channel volume).
static void push_frames(Client *cl, u64 pos, u32 n) {
    if (!cl->stream || !n) return;
    u32 align = cl->f.align, ch = cl->f.ch;
    float chg[8]; int unity = 1;
    for (u32 i = 0; i < ch; i++) { chg[i] = cl->svol[i] * cl->cvol[i] * ep_ch[i < 8 ? i : 0]; if (chg[i] != 1.0f) unity = 0; }
    while (n) {
        u32 off = (u32)(pos % cl->bufframes), run = cl->bufframes - off; if (run > n) run = n;
        const u8 *src = cl->ring + (u64)off * align;
        if (cl->conv == CONV_NONE && unity) SDL_PutAudioStreamData(cl->stream, src, (int)(run * align));
        else {
            u32 outsz = cl->conv == CONV_S24 ? 4 * ch : align;
            u8 *dst = cl->scratch;   // sized for 2 periods of 32-bit samples; chunk the run accordingly
            u32 cap = 2 * cl->period, done = 0;
            while (done < run) {
                u32 k = run - done < cap ? run - done : cap;
                for (u32 i = 0; i < k; i++) {
                    const u8 *s = src + (u64)(done + i) * align; u8 *d = dst + (u64)i * outsz;
                    for (u32 c = 0; c < ch; c++) {
                        if (cl->conv == CONV_S24) {
                            const u8 *p = s + 3 * c; i32 v = (i32)((u32)p[0] << 8 | (u32)p[1] << 16 | (u32)p[2] << 24);
                            if (!unity) v = (i32)fmaxf(fminf((float)v * chg[c], 2147483520.0f), -2147483648.0f);
                            memcpy(d + 4 * c, &v, 4);
                        } else if (cl->is_float) {
                            float v; memcpy(&v, s + 4 * c, 4); v *= chg[c]; memcpy(d + 4 * c, &v, 4);
                        } else if (cl->f.bits == 16) {
                            i16 v; memcpy(&v, s + 2 * c, 2); float x = v * chg[c]; v = (i16)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); memcpy(d + 2 * c, &v, 2);
                        } else if (cl->f.bits == 32) {
                            i32 v; memcpy(&v, s + 4 * c, 4); double x = (double)v * chg[c]; v = (i32)(x > 2147483647.0 ? 2147483647.0 : x < -2147483648.0 ? -2147483648.0 : x); memcpy(d + 4 * c, &v, 4);
                        } else {   // 8-bit unsigned
                            float x = ((int)s[c] - 128) * chg[c] + 128; d[c] = (u8)(x > 255 ? 255 : x < 0 ? 0 : x);
                        }
                    }
                }
                SDL_PutAudioStreamData(cl->stream, dst, (int)(k * outsz));
                done += k;
            }
        }
        pos += run; n -= run;
    }
}

static void tick_client(Client *cl, u64 qpc) {
    if (cl->capture) {   // silence arrives one period per tick
        cl->wpos += cl->period; cl->clock += cl->period;
        if (cl->wpos - cl->rpos > cl->bufframes) {   // overrun: drop the oldest whole packets
            u64 keep = cl->bufframes - cl->bufframes % cl->period;
            if (!cl->get_out) { cl->rpos = cl->wpos - keep; cl->discont = 1; }
        }
    } else {
        u64 held = cl->wpos - cl->rpos;
        u32 want = cl->period;
        if (cl->stream) {
            u64 queued = (u64)SDL_GetAudioStreamQueued(cl->stream) / (cl->conv == CONV_S24 ? 4u * cl->f.ch : cl->f.align);
            if (queued > 3ull * cl->period) want = 0;
            else if (queued < cl->period / 2) want = 2 * cl->period;
            float g = client_gain();
            if (g != cl->gain_set) { SDL_SetAudioStreamGain(cl->stream, g); cl->gain_set = g; }
        }
        u32 n = held < want ? (u32)held : want;
        push_frames(cl, cl->rpos, n);
        cl->rpos += n; cl->clock += n;
    }
    cl->clock_qpc = qpc;
    if (cl->event_mode && cl->event) rt_signal_event(cl->event);
}

static void *engine(void *arg) {
    (void)arg;
    u64 next = now_ns();
    pthread_mutex_lock(&lock);
    for (;;) {
        int any = 0;
        for (Client *cl = clients; cl; cl = cl->next) if (cl->started) any = 1;
        if (!any) { pthread_cond_wait(&wake, &lock); next = now_ns(); continue; }
        u64 qpc = now_ns() / 100;
        for (Client *cl = clients; cl; cl = cl->next) if (cl->started) tick_client(cl, qpc);
        pthread_mutex_unlock(&lock);
        next += (u64)PERIOD_HNS * 100;
        u64 t = now_ns();
        if (t > next + 50000000ull) next = t;            // fell far behind (suspended?): resynchronise
        else if (next > t) { struct timespec ts = { 0, (long)(next - t) }; nanosleep(&ts, NULL); }
        pthread_mutex_lock(&lock);
    }
    return NULL;
}
static void spawn_engine(void) { pthread_t t; pthread_create(&t, NULL, engine, NULL); pthread_detach(t); }
static void start_engine(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, spawn_engine); }

// ---- vtables --------------------------------------------------------------------------------------
#define QAR "QueryInterface", "AddRef", "Release"
static void init_vtables(void) {
    static int done; if (done) return; done = 1;
    static const char *const en[] = { QAR, "EnumAudioEndpoints", "GetDefaultAudioEndpoint", "GetDevice", "RegisterEndpointNotificationCallback",
        "UnregisterEndpointNotificationCallback", NULL };
    static const char *const dv[] = { QAR, "Activate", "OpenPropertyStore", "GetId", "GetState", NULL };
    static const char *const ep[] = { QAR, "GetDataFlow", NULL };
    static const char *const co[] = { QAR, "GetCount", "Item", NULL };
    static const char *const ps[] = { QAR, "GetCount", "GetAt", "GetValue", "SetValue", "Commit", NULL };
    static const char *const cl[] = { QAR, "Initialize", "GetBufferSize", "GetStreamLatency", "GetCurrentPadding", "IsFormatSupported",
        "GetMixFormat", "GetDevicePeriod", "Start", "Stop", "Reset", "SetEventHandle", "GetService",
        "IsOffloadCapable", "SetClientProperties", "GetBufferSizeLimits",
        "GetSharedModeEnginePeriod", "GetCurrentSharedModeEnginePeriod", "InitializeSharedAudioStream", NULL };
    static const char *const rc[] = { QAR, "GetBuffer", "ReleaseBuffer", NULL };
    static const char *const cc[] = { QAR, "GetBuffer", "ReleaseBuffer", "GetNextPacketSize", NULL };
    static const char *const ck[] = { QAR, "GetFrequency", "GetPosition", "GetCharacteristics", NULL };
    static const char *const ck2[] = { QAR, "GetDevicePosition", NULL };
    static const char *const sv[] = { QAR, "SetMasterVolume", "GetMasterVolume", "SetMute", "GetMute", NULL };
    static const char *const st[] = { QAR, "GetChannelCount", "SetChannelVolume", "GetChannelVolume", "SetAllVolumes", "GetAllVolumes", NULL };
    static const char *const sc[] = { QAR, "GetState", "GetDisplayName", "SetDisplayName", "GetIconPath", "SetIconPath", "GetGroupingParam",
        "SetGroupingParam", "RegisterAudioSessionNotification", "UnregisterAudioSessionNotification", "GetSessionIdentifier",
        "GetSessionInstanceIdentifier", "GetProcessId", "IsSystemSoundsSession", "SetDuckingPreference", NULL };
    static const char *const sm[] = { QAR, "GetAudioSessionControl", "GetSimpleAudioVolume", "GetSessionEnumerator", "RegisterSessionNotification",
        "UnregisterSessionNotification", "RegisterDuckNotification", "UnregisterDuckNotification", NULL };
    static const char *const se[] = { QAR, "GetCount", "GetSession", NULL };
    static const char *const ev[] = { QAR, "RegisterControlChangeNotify", "UnregisterControlChangeNotify", "GetChannelCount", "SetMasterVolumeLevel",
        "SetMasterVolumeLevelScalar", "GetMasterVolumeLevel", "GetMasterVolumeLevelScalar", "SetChannelVolumeLevel",
        "SetChannelVolumeLevelScalar", "GetChannelVolumeLevel", "GetChannelVolumeLevelScalar", "SetMute", "GetMute", "GetVolumeStepInfo",
        "VolumeStepUp", "VolumeStepDown", "QueryHardwareSupport", "GetVolumeRange", "GetVolumeRangeChannel", NULL };
    static const char *const mi[] = { QAR, "GetPeakValue", "GetMeteringChannelCount", "GetChannelsPeakValues", "QueryHardwareSupport", NULL };
    static const char *const ao[] = { QAR, "GetActivateResult", NULL };
    vt_enum = rt_com_vtable("WAEN", en); vt_dev = rt_com_vtable("WADV", dv); vt_endp = rt_com_vtable("WAEP", ep); vt_coll = rt_com_vtable("WACO", co);
    vt_props = rt_com_vtable("WAPS", ps); vt_client = rt_com_vtable("WACL", cl); vt_render = rt_com_vtable("WARC", rc);
    vt_capture = rt_com_vtable("WACC", cc); vt_clock = rt_com_vtable("WACK", ck); vt_clock2 = rt_com_vtable("WAK2", ck2); vt_svol = rt_com_vtable("WASV", sv);
    vt_strvol = rt_com_vtable("WAST", st); vt_chvol = rt_com_vtable("WACH", st); vt_sessctl = rt_com_vtable("WASC", sc);
    vt_sessmgr = rt_com_vtable("WASM", sm); vt_sessenum = rt_com_vtable("WASE", se); vt_epvol = rt_com_vtable("WAEV", ev);
    vt_meter = rt_com_vtable("WAMI", mi); vt_asyncop = rt_com_vtable("WAAO", ao);
}

// ---- statics: devices, session, endpoint volume, meter ---------------------------------------------
static Device devs[2];
static Hdr sessctl, sessmgr, sessenum, simplevol, epvol[2], meter[2], enumerator;
static void init_statics(void) {
    static int done; if (done) return; done = 1;
    init_vtables(); init_mixf();
    static const char *names[2] = { "Speakers (wintomac)", "Microphone (wintomac)" }, *descs[2] = { "Speakers", "Microphone" };
    static const char *ids[2] = { "{0.0.0.00000000}.{77696e74-6f6d-6163-0000-000000000001}", "{0.0.1.00000000}.{77696e74-6f6d-6163-0000-000000000002}" };
    for (int i = 0; i < 2; i++) {
        Device *d = &devs[i];
        hdr_init(&d->h, vt_dev, IIDS_DEV, NULL, NULL);
        hdr_init(&d->endpoint, vt_endp, IIDS_ENDP, &d->h, NULL);
        hdr_init(&d->props, vt_props, IIDS_PROPS, NULL, NULL);
        d->h.alt = &d->endpoint;
        d->flow = i; d->name = names[i]; d->desc = descs[i]; d->id = ids[i];
        hdr_init(&epvol[i], vt_epvol, IIDS_EPVOL, NULL, NULL);
        hdr_init(&meter[i], vt_meter, IIDS_METER, NULL, NULL);
    }
    hdr_init(&sessctl, vt_sessctl, IIDS_SESSCTL, NULL, NULL);
    hdr_init(&sessmgr, vt_sessmgr, IIDS_SESSMGR, NULL, NULL);
    hdr_init(&sessenum, vt_sessenum, IIDS_SESSENUM, NULL, NULL);
    hdr_init(&simplevol, vt_svol, IIDS_SVOL, NULL, NULL);
    hdr_init(&enumerator, vt_enum, IIDS_ENUM, NULL, NULL);
    sess_name = wdup("");
}
static Device *dev_of(Hdr *o) { return (Device *)o; }

// ---- IMMDeviceEnumerator ------------------------------------------------------------------------------
typedef struct { Hdr h; u32 n; Device *d[2]; } Coll;
static void coll_destroy(Hdr *o) { free(o); }
SHIM(WAEN_EnumAudioEndpoints) {   // (this, flow, stateMask, **coll)
    u32 flow = (u32)A2, mask = (u32)A3;
    if (!A4) RET(E_POINTER_);
    ST64(A4, 0);
    if (flow > 2 || mask > 0xF) RET(E_INVALIDARG_);
    Coll *co = calloc(1, sizeof *co);
    hdr_init(&co->h, vt_coll, IIDS_COLL, NULL, coll_destroy);
    if (mask & DEVICE_STATE_ACTIVE)
        for (int i = 0; i < 2; i++) if (flow == 2 || flow == (u32)i) co->d[co->n++] = &devs[i];
    ST64(A4, (u64)co);
    RET(S_OK_);
}
SHIM(WAEN_GetDefaultAudioEndpoint) {   // (this, flow, role, **dev)
    if (!A4) RET(E_POINTER_);
    ST64(A4, 0);
    if ((u32)A2 > 1 || (u32)A3 > 2) RET(E_INVALIDARG_);
    ST64(A4, (u64)&devs[(u32)A2]);
    RET(S_OK_);
}
SHIM(WAEN_GetDevice) {   // (this, LPCWSTR id, **dev)
    if (!A2 || !A3) RET(E_POINTER_);
    ST64(A3, 0);
    for (int i = 0; i < 2; i++) if (wmatch(A2, devs[i].id)) { ST64(A3, (u64)&devs[i]); RET(S_OK_); }
    RET(E_NOTFOUND_);
}
SHIM(WAEN_RegisterEndpointNotificationCallback) { RET(A2 ? S_OK_ : E_POINTER_); }    // our endpoints never change
SHIM(WAEN_UnregisterEndpointNotificationCallback) { RET(A2 ? S_OK_ : E_POINTER_); }

// ---- IMMDeviceCollection ------------------------------------------------------------------------------
SHIM(WACO_GetCount) { if (!A2) RET(E_POINTER_); ST32(A2, ((Coll *)A1)->n); RET(S_OK_); }
SHIM(WACO_Item) {
    Coll *co = (Coll *)A1;
    if (!A3) RET(E_POINTER_);
    if ((u32)A2 >= co->n) { ST64(A3, 0); RET(E_INVALIDARG_); }
    ST64(A3, (u64)co->d[(u32)A2]);
    RET(S_OK_);
}

// ---- IMMDevice / IMMEndpoint -------------------------------------------------------------------------
static void client_destroy(Hdr *o);
static Client *new_client(Device *d) {
    Client *cl = calloc(1, sizeof *cl);
    hdr_init(&cl->h, vt_client, IIDS_CLIENT, NULL, client_destroy);
    cl->dev = d; cl->capture = d->flow == 1; cl->gain_set = -1;
    for (int i = 0; i < 8; i++) cl->svol[i] = cl->cvol[i] = 1.0f;
    hdr_init(&cl->render.h, vt_render, IIDS_RENDER, &cl->h, NULL); cl->render.cl = cl;
    hdr_init(&cl->capclient.h, vt_capture, IIDS_CAPTURE, &cl->h, NULL); cl->capclient.cl = cl;
    hdr_init(&cl->clk.h, vt_clock, IIDS_CLOCK, &cl->h, NULL); cl->clk.cl = cl;
    hdr_init(&cl->clk2.h, vt_clock2, IIDS_CLOCK2, &cl->h, NULL); cl->clk2.cl = cl;
    cl->clk.h.alt = &cl->clk2.h; cl->clk2.h.alt = &cl->clk.h;
    hdr_init(&cl->strvol.h, vt_strvol, IIDS_STRVOL, &cl->h, NULL); cl->strvol.cl = cl;
    hdr_init(&cl->chvol.h, vt_chvol, IIDS_CHVOL, &cl->h, NULL); cl->chvol.cl = cl;
    return cl;
}
static u32 activate(Device *d, u64 iid, u64 out) {
    if (!out) return E_POINTER_;
    ST64(out, 0);
    if (has_iid(&(Hdr){ .iids = IIDS_CLIENT }, iid)) { ST64(out, (u64)new_client(d)); return S_OK_; }
    if (has_iid(&epvol[d->flow], iid)) { ST64(out, (u64)&epvol[d->flow]); return S_OK_; }
    if (has_iid(&sessmgr, iid)) { ST64(out, (u64)&sessmgr); return S_OK_; }
    if (has_iid(&meter[d->flow], iid)) { ST64(out, (u64)&meter[d->flow]); return S_OK_; }
    char g[64]; rt_guid_str(iid, g, sizeof g);
    rt_log("wasapi: IMMDevice::Activate(%s) not supported", g);
    return E_NOINTERFACE_;
}
SHIM(WADV_Activate) { RET(activate(dev_of((Hdr *)A1), A2, AN(5))); }   // (this, iid, clsctx, params, **out)
SHIM(WADV_OpenPropertyStore) { if (!A3) RET(E_POINTER_); ST64(A3, (u64)&dev_of((Hdr *)A1)->props); RET(S_OK_); }
SHIM(WADV_GetId) { if (!A2) RET(E_POINTER_); ST64(A2, (u64)wdup(dev_of((Hdr *)A1)->id)); RET(S_OK_); }
SHIM(WADV_GetState) { if (!A2) RET(E_POINTER_); ST32(A2, DEVICE_STATE_ACTIVE); RET(S_OK_); }
SHIM(WAEP_GetDataFlow) { if (!A2) RET(E_POINTER_); ST32(A2, (u32)dev_of(((Hdr *)A1)->owner)->flow); RET(S_OK_); }

// ---- IPropertyStore ------------------------------------------------------------------------------------
#define PROPS_DEV(o) ((Device *)((u8 *)(o) - offsetof(Device, props)))
typedef struct { const Guid *fmtid; u32 pid; } PKey;
static const PKey pkeys[] = { { &FMTID_Device_, 14 }, { &FMTID_Device_, 2 }, { &FMTID_DevIface_, 2 }, { &FMTID_EngineDevFmt_, 0 },
                              { &FMTID_EngineOEMFmt_, 3 }, { &FMTID_Endpoint_, 0 }, { &FMTID_Endpoint_, 3 }, { &FMTID_Endpoint_, 4 } };
#define NPKEYS (sizeof pkeys / sizeof *pkeys)
SHIM(WAPS_GetCount) { if (!A2) RET(E_POINTER_); ST32(A2, NPKEYS); RET(S_OK_); }
SHIM(WAPS_GetAt) {
    if (!A3) RET(E_POINTER_);
    if ((u32)A2 >= NPKEYS) RET(E_INVALIDARG_);
    memcpy((void *)A3, pkeys[(u32)A2].fmtid, 16); ST32(A3 + 16, pkeys[(u32)A2].pid);
    RET(S_OK_);
}
SHIM(WAPS_GetValue) {   // (this, REFPROPERTYKEY, PROPVARIANT*)
    Device *d = PROPS_DEV(A1);
    u64 key = A2, pv = A3;
    if (!key || !pv) RET(E_POINTER_);
    memset((void *)pv, 0, 24);   // VT_EMPTY for keys we don't know: S_OK, like Windows
    u32 pid = LD32(key + 16);
    if (rt_guid_eq(key, &FMTID_Device_) && (pid == 14 || pid == 2)) { ST16(pv, 31); ST64(pv + 8, (u64)wdup(pid == 14 ? d->name : d->desc)); }
    else if (rt_guid_eq(key, &FMTID_DevIface_) && pid == 2) { ST16(pv, 31); ST64(pv + 8, (u64)wdup("wintomac")); }
    else if ((rt_guid_eq(key, &FMTID_EngineDevFmt_) && pid == 0) || (rt_guid_eq(key, &FMTID_EngineOEMFmt_) && pid == 3)) {
        ST16(pv, 65);   // VT_BLOB { ULONG cbSize; BYTE *pBlobData }
        ST32(pv + 8, 40); ST64(pv + 16, alloc_mix());
    }
    else if (rt_guid_eq(key, &FMTID_Endpoint_) && pid == 0) { ST16(pv, 19); ST32(pv + 8, d->flow ? 4 /* Microphone */ : 1 /* Speakers */); }
    else if (rt_guid_eq(key, &FMTID_Endpoint_) && pid == 3) { ST16(pv, 19); ST32(pv + 8, mask_for(mixf.ch)); }
    else if (rt_guid_eq(key, &FMTID_Endpoint_) && pid == 4) { ST16(pv, 31); ST64(pv + 8, (u64)wdup(strchr(d->id, '}') + 2)); }
    RET(S_OK_);
}
SHIM(WAPS_SetValue) { RET(STG_E_ACCESSDENIED_); }
SHIM(WAPS_Commit) { RET(STG_E_ACCESSDENIED_); }

// ---- IAudioClient / 2 / 3 ----------------------------------------------------------------------------
#define CL ((Client *)A1)
static void client_destroy(Hdr *o) {
    Client *cl = (Client *)o;
    pthread_mutex_lock(&lock);
    for (Client **p = &clients; *p; p = &(*p)->next) if (*p == cl) { *p = cl->next; break; }
    pthread_mutex_unlock(&lock);
    if (cl->stream) SDL_DestroyAudioStream(cl->stream);
    free(cl->ring); free(cl->bounce); free(cl->scratch); free(cl);
}
static int exclusive_ok(void) { return getenv("WINTOMAC_WASAPI_EXCLUSIVE") != NULL; }

static u32 do_initialize(Client *cl, u32 mode, u32 flags, i64 duration, i64 periodicity, u64 fmt) {
    if (cl->inited) return E_ALREADY_INITIALIZED;
    if (!fmt) return E_POINTER_;
    if (mode > 1) return E_INVALIDARG_;
    if (mode == 1 && !exclusive_ok()) return E_EXCLUSIVE_MODE_NOT_ALLOWED;
    if ((flags & STREAMFLAGS_LOOPBACK) && cl->dev->flow != 0) return E_WRONG_ENDPOINT_TYPE;
    if (mode == 1 && periodicity && periodicity < MIN_PERIOD_HNS) return E_INVALID_DEVICE_PERIOD;
    if (duration < 0 || periodicity < 0) return E_INVALIDARG_;
    Fmt f; int flt;
    if (!parse_fmt(fmt, &f, &flt)) {
        rt_log("wasapi: unsupported format tag=0x%x ch=%u rate=%u bits=%u align=%u", LD16(fmt), LD16(fmt + 2), LD32(fmt + 4), LD16(fmt + 14), LD16(fmt + 12));
        return E_UNSUPPORTED_FORMAT;
    }
    if (duration < MIN_SHARED_BUFFER_HNS) duration = MIN_SHARED_BUFFER_HNS;
    if (duration > 20000000) duration = 20000000;   // 2 s
    cl->f = f; cl->is_float = flt; cl->flags = flags;
    memcpy(cl->wfx, (void *)fmt, LD16(fmt) == 0xFFFE ? 40 : 18);
    cl->event_mode = (flags & STREAMFLAGS_EVENTCALLBACK) != 0;
    if (flags & STREAMFLAGS_LOOPBACK) cl->capture = 1;
    cl->period = f.rate / 100;
    cl->bufframes = (u32)((duration * (i64)f.rate + 9999999) / 10000000);
    if (cl->capture) cl->bufframes = (cl->bufframes + cl->period - 1) / cl->period * cl->period;   // whole packets
    cl->ring = calloc(cl->bufframes, f.align);
    cl->bounce = calloc(cl->bufframes, f.align);
    cl->scratch = calloc(2ull * cl->period, 4ull * f.ch);
    if (cl->capture || f.bits == 8) memset(cl->bounce, f.bits == 8 ? 0x80 : 0, (size_t)cl->bufframes * f.align);   // capture packets: silence
    cl->conv = (!flt && f.bits == 24) ? CONV_S24 : CONV_NONE;
    cl->sdlfmt = flt ? SDL_AUDIO_F32 : f.bits == 8 ? SDL_AUDIO_U8 : f.bits == 16 ? SDL_AUDIO_S16 : SDL_AUDIO_S32;
    if (!cl->capture && open_output()) {
        SDL_AudioSpec spec = { cl->sdlfmt, f.ch, (int)f.rate };
        cl->stream = SDL_CreateAudioStream(&spec, NULL);
        if (cl->stream && !SDL_BindAudioStream(out_dev, cl->stream)) {
            rt_log("wasapi: SDL_BindAudioStream: %s", SDL_GetError());
            SDL_DestroyAudioStream(cl->stream); cl->stream = NULL;
        }
    }
    pthread_mutex_lock(&lock);
    cl->next = clients; clients = cl; cl->inited = 1;
    pthread_mutex_unlock(&lock);
    start_engine();
    if (trace()) rt_log("wasapi: Initialize %s mode=%u flags=0x%x fmt=%s%u ch=%u rate=%u buffer=%u frames", cl->capture ? "capture" : "render",
                        mode, flags, flt ? "f" : "s", f.bits, f.ch, f.rate, cl->bufframes);
    return S_OK_;
}
// Initialize(this, mode, flags, REFERENCE_TIME duration, REFERENCE_TIME periodicity, fmt, session guid)
SHIM(WACL_Initialize) { RET(do_initialize(CL, (u32)A2, (u32)A3, (i64)A4, (i64)AN(5), AN(6))); }
SHIM(WACL_GetBufferSize) { if (!A2) RET(E_POINTER_); if (!CL->inited) RET(E_NOT_INITIALIZED); ST32(A2, CL->bufframes); RET(S_OK_); }
SHIM(WACL_GetStreamLatency) { if (!A2) RET(E_POINTER_); if (!CL->inited) RET(E_NOT_INITIALIZED); ST64(A2, (u64)PERIOD_HNS); RET(S_OK_); }
SHIM(WACL_GetCurrentPadding) {
    Client *cl = CL;
    if (!A2) RET(E_POINTER_);
    if (!cl->inited) RET(E_NOT_INITIALIZED);
    pthread_mutex_lock(&lock);
    u64 p = cl->wpos - cl->rpos;
    pthread_mutex_unlock(&lock);
    ST32(A2, (u32)p);
    RET(S_OK_);
}
SHIM(WACL_IsFormatSupported) {   // (this, mode, fmt, **closest)
    u32 mode = (u32)A2; u64 fmt = A3, closest = A4;
    if (!fmt) RET(E_POINTER_);
    if (mode > 1) RET(E_INVALIDARG_);
    if (mode == 0 && !closest) RET(E_POINTER_);
    if (closest) ST64(closest, 0);
    Fmt f; int flt;
    int ok = parse_fmt(fmt, &f, &flt);
    if (mode == 1) RET(!exclusive_ok() ? E_EXCLUSIVE_MODE_NOT_ALLOWED : ok ? S_OK_ : E_UNSUPPORTED_FORMAT);
    if (!ok) RET(E_UNSUPPORTED_FORMAT);
    if (is_mix(&f, flt)) RET(S_OK_);
    ST64(closest, alloc_mix());
    RET(S_FALSE_);
}
SHIM(WACL_GetMixFormat) { if (!A2) RET(E_POINTER_); ST64(A2, alloc_mix()); RET(S_OK_); }
SHIM(WACL_GetDevicePeriod) {
    if (!A2 && !A3) RET(E_POINTER_);
    if (A2) ST64(A2, (u64)PERIOD_HNS);
    if (A3) ST64(A3, (u64)MIN_PERIOD_HNS);
    RET(S_OK_);
}
SHIM(WACL_Start) {
    Client *cl = CL;
    if (!cl->inited) RET(E_NOT_INITIALIZED);
    if (cl->event_mode && !cl->event) RET(E_EVENTHANDLE_NOT_SET);
    pthread_mutex_lock(&lock);
    if (cl->started) { pthread_mutex_unlock(&lock); RET(E_NOT_STOPPED); }
    cl->started = 1;
    pthread_cond_broadcast(&wake);
    pthread_mutex_unlock(&lock);
    RET(S_OK_);
}
SHIM(WACL_Stop) {
    Client *cl = CL;
    if (!cl->inited) RET(E_NOT_INITIALIZED);
    pthread_mutex_lock(&lock);
    int was = cl->started; cl->started = 0;
    pthread_mutex_unlock(&lock);
    RET(was ? S_OK_ : S_FALSE_);
}
SHIM(WACL_Reset) {
    Client *cl = CL;
    if (!cl->inited) RET(E_NOT_INITIALIZED);
    pthread_mutex_lock(&lock);
    if (cl->started) { pthread_mutex_unlock(&lock); RET(E_NOT_STOPPED); }
    if (cl->get_out) { pthread_mutex_unlock(&lock); RET(E_BUFFER_OPERATION_PENDING); }
    cl->wpos = cl->rpos = cl->clock = 0; cl->discont = 0;
    if (cl->stream) SDL_ClearAudioStream(cl->stream);
    pthread_mutex_unlock(&lock);
    RET(S_OK_);
}
SHIM(WACL_SetEventHandle) {
    Client *cl = CL;
    if (!A2) RET(E_INVALIDARG_);
    if (!cl->inited) RET(E_NOT_INITIALIZED);
    if (!cl->event_mode) RET(E_EVENTHANDLE_NOT_EXPECTED);
    cl->event = A2;
    RET(S_OK_);
}
SHIM(WACL_GetService) {   // (this, iid, **out)
    Client *cl = CL;
    if (!A3) RET(E_POINTER_);
    ST64(A3, 0);
    if (!cl->inited) RET(E_NOT_INITIALIZED);
    Hdr *o = NULL;
    if (rt_guid_eq(A2, &IID_IAudioRenderClient_)) { if (cl->capture) RET(E_WRONG_ENDPOINT_TYPE); o = &cl->render.h; }
    else if (rt_guid_eq(A2, &IID_IAudioCaptureClient_)) { if (!cl->capture) RET(E_WRONG_ENDPOINT_TYPE); o = &cl->capclient.h; }
    else if (rt_guid_eq(A2, &IID_IAudioClock_)) o = &cl->clk.h;
    else if (rt_guid_eq(A2, &IID_IAudioClock2_)) o = &cl->clk2.h;
    else if (rt_guid_eq(A2, &IID_IAudioStreamVolume_)) o = &cl->strvol.h;
    else if (rt_guid_eq(A2, &IID_IChannelAudioVolume_)) o = &cl->chvol.h;
    else if (rt_guid_eq(A2, &IID_ISimpleAudioVolume_)) o = &simplevol;
    else if (has_iid(&sessctl, A2)) o = &sessctl;
    else {
        char g[64]; rt_guid_str(A2, g, sizeof g);
        if (trace()) rt_log("wasapi: GetService(%s) not supported", g);
        RET(E_NOINTERFACE_);
    }
    addref(o); ST64(A3, (u64)o);
    RET(S_OK_);
}
SHIM(WACL_IsOffloadCapable) { if (!A3) RET(E_INVALIDARG_); ST32(A3, 0); RET(S_OK_); }
SHIM(WACL_SetClientProperties) { if (!A2) RET(E_POINTER_); RET(CL->inited ? E_ALREADY_INITIALIZED : S_OK_); }
SHIM(WACL_GetBufferSizeLimits) {   // (this, fmt, eventDriven, *min, *max)
    if (!A2 || !A4 || !AN(5)) RET(E_POINTER_);
    ST64(A4, (u64)MIN_SHARED_BUFFER_HNS); ST64(AN(5), 20000000);
    RET(S_OK_);
}
SHIM(WACL_GetSharedModeEnginePeriod) {   // (this, fmt, *default, *fundamental, *min, *max) in frames
    if (!A2 || !A3 || !A4 || !AN(5) || !AN(6)) RET(E_POINTER_);
    u32 p = LD32(A2 + 4) / 100;
    ST32(A3, p); ST32(A4, p); ST32(AN(5), p); ST32(AN(6), p);
    RET(S_OK_);
}
SHIM(WACL_GetCurrentSharedModeEnginePeriod) {   // (this, **fmt, *period)
    if (!A2 || !A3) RET(E_POINTER_);
    init_mixf();
    ST64(A2, alloc_mix()); ST32(A3, mixf.rate / 100);
    RET(S_OK_);
}
SHIM(WACL_InitializeSharedAudioStream) {   // (this, flags, periodInFrames, fmt, session)
    u64 fmt = A4;
    if (fmt && (u32)A3 && (u32)A3 != LD32(fmt + 4) / 100) RET(E_INVALIDARG_);
    RET(do_initialize(CL, 0, (u32)A2, 0, 0, fmt));
}

// ---- IAudioRenderClient -----------------------------------------------------------------------------
#define SUBCL (((Sub *)A1)->cl)
SHIM(WARC_GetBuffer) {   // (this, frames, **data)
    Client *cl = SUBCL; u32 n = (u32)A2;
    if (!A3) RET(E_POINTER_);
    ST64(A3, 0);
    if (cl->get_out) RET(E_OUT_OF_ORDER);
    if (!n) RET(S_OK_);
    pthread_mutex_lock(&lock);
    u64 held = cl->wpos - cl->rpos, w = cl->wpos;
    pthread_mutex_unlock(&lock);
    if (n > cl->bufframes - held) RET(E_BUFFER_TOO_LARGE);
    u32 off = (u32)(w % cl->bufframes);
    cl->get_ptr = off + n <= cl->bufframes ? cl->ring + (u64)off * cl->f.align : cl->bounce;
    cl->get_out = 1; cl->get_frames = n; cl->get_pos = w;
    ST64(A3, (u64)cl->get_ptr);
    RET(S_OK_);
}
SHIM(WARC_ReleaseBuffer) {   // (this, frames, flags)
    Client *cl = SUBCL; u32 n = (u32)A2, flags = (u32)A3;
    if (!cl->get_out) RET(n ? E_OUT_OF_ORDER : S_OK_);
    if (n > cl->get_frames) RET(E_INVALID_SIZE);
    if (flags & ~BUFFERFLAGS_SILENT) RET(E_INVALIDARG_);
    u32 a = cl->f.align;
    if (flags & BUFFERFLAGS_SILENT) memset(cl->get_ptr, cl->f.bits == 8 ? 0x80 : 0, (size_t)n * a);
    if (cl->get_ptr == cl->bounce) {
        u32 off = (u32)(cl->get_pos % cl->bufframes), first = cl->bufframes - off; if (first > n) first = n;
        memcpy(cl->ring + (u64)off * a, cl->bounce, (size_t)first * a);
        memcpy(cl->ring, cl->bounce + (u64)first * a, (size_t)(n - first) * a);
    }
    pthread_mutex_lock(&lock);
    cl->wpos += n; cl->get_out = 0;
    pthread_mutex_unlock(&lock);
    RET(S_OK_);
}

// ---- IAudioCaptureClient (silence) ----------------------------------------------------------------
// GetBuffer(this, **data, *frames, *flags, *devpos, *qpcpos)
SHIM(WACC_GetBuffer) {
    Client *cl = SUBCL;
    if (!A2 || !A3 || !A4) RET(E_POINTER_);
    ST64(A2, 0); ST32(A3, 0); ST32(A4, 0);
    if (cl->get_out) RET(E_OUT_OF_ORDER);
    pthread_mutex_lock(&lock);
    u64 avail = cl->wpos - cl->rpos, pos = cl->rpos; int disc = cl->discont; cl->discont = 0;
    u64 qpc = cl->clock_qpc;
    if (avail >= cl->period) { cl->get_out = 1; cl->get_frames = cl->period; }
    pthread_mutex_unlock(&lock);
    if (avail < cl->period) RET(S_BUFFER_EMPTY);
    ST64(A2, (u64)cl->bounce); ST32(A3, cl->period);
    ST32(A4, BUFFERFLAGS_SILENT | (disc ? BUFFERFLAGS_DATA_DISCONTINUITY : 0));
    if (AN(5)) ST64(AN(5), pos);
    if (AN(6)) ST64(AN(6), qpc - (u64)((avail - cl->period) * 10000000ull / cl->f.rate));
    RET(S_OK_);
}
SHIM(WACC_ReleaseBuffer) {
    Client *cl = SUBCL; u32 n = (u32)A2;
    if (!cl->get_out) RET(n ? E_OUT_OF_ORDER : S_OK_);
    if (n && n != cl->get_frames) RET(E_INVALID_SIZE);
    pthread_mutex_lock(&lock); cl->rpos += n; cl->get_out = 0; pthread_mutex_unlock(&lock);
    RET(S_OK_);
}
SHIM(WACC_GetNextPacketSize) {
    Client *cl = SUBCL;
    if (!A2) RET(E_POINTER_);
    pthread_mutex_lock(&lock); u64 avail = cl->wpos - cl->rpos; pthread_mutex_unlock(&lock);
    ST32(A2, avail >= cl->period ? cl->period : 0);
    RET(S_OK_);
}

// ---- IAudioClock / IAudioClock2 ------------------------------------------------------------------------
SHIM(WACK_GetFrequency) {   // shared mode: bytes per second (positions are byte offsets)
    Client *cl = SUBCL;
    if (!A2) RET(E_POINTER_);
    ST64(A2, (u64)cl->f.rate * cl->f.align);
    RET(S_OK_);
}
SHIM(WACK_GetPosition) {   // (this, *pos, *qpcpos)
    Client *cl = SUBCL;
    if (!A2) RET(E_POINTER_);
    pthread_mutex_lock(&lock); u64 frames = cl->clock, qpc = cl->clock_qpc; pthread_mutex_unlock(&lock);
    ST64(A2, frames * cl->f.align);
    if (A3) ST64(A3, qpc ? qpc : now_ns() / 100);
    RET(S_OK_);
}
SHIM(WAK2_GetDevicePosition) {   // (this, *frames, *qpcpos): device frames
    Client *cl = SUBCL;
    if (!A2) RET(E_POINTER_);
    pthread_mutex_lock(&lock); u64 frames = cl->clock, qpc = cl->clock_qpc; pthread_mutex_unlock(&lock);
    ST64(A2, frames);
    if (A3) ST64(A3, qpc ? qpc : now_ns() / 100);
    RET(S_OK_);
}
SHIM(WACK_GetCharacteristics) { if (!A2) RET(E_POINTER_); ST32(A2, 1 /* AUDIOCLOCK_CHARACTERISTIC_FIXED_FREQ */); RET(S_OK_); }

// ---- ISimpleAudioVolume (the process' default session) --------------------------------------------
SHIM(WASV_SetMasterVolume) { float v = argf(c->x[1]); if (!(v >= 0.0f && v <= 1.0f)) RET(E_INVALIDARG_); sess_vol = v; RET(S_OK_); }
SHIM(WASV_GetMasterVolume) { if (!A2) RET(E_POINTER_); float v = sess_vol; memcpy((void *)A2, &v, 4); RET(S_OK_); }
SHIM(WASV_SetMute) { sess_mute = (u32)A2 != 0; RET(S_OK_); }
SHIM(WASV_GetMute) { if (!A2) RET(E_POINTER_); ST32(A2, (u32)sess_mute); RET(S_OK_); }

// ---- IAudioStreamVolume / IChannelAudioVolume (per stream) ------------------------------------------
static float *vols(Ctx *c) { Sub *s = (Sub *)A1; return &s->h == &s->cl->strvol.h ? s->cl->svol : s->cl->cvol; }
SHIM(WAST_GetChannelCount) { if (!A2) RET(E_POINTER_); ST32(A2, SUBCL->f.ch); RET(S_OK_); }
SHIM(WAST_SetChannelVolume) {   // (this, index, float level[, ctx])
    float v = argf(c->x[2]);
    if ((u32)A2 >= SUBCL->f.ch || !(v >= 0.0f && v <= 1.0f)) RET(E_INVALIDARG_);
    vols(c)[(u32)A2] = v;
    RET(S_OK_);
}
SHIM(WAST_GetChannelVolume) {
    if (!A3) RET(E_POINTER_);
    if ((u32)A2 >= SUBCL->f.ch) RET(E_INVALIDARG_);
    memcpy((void *)A3, &vols(c)[(u32)A2], 4);
    RET(S_OK_);
}
SHIM(WAST_SetAllVolumes) {   // (this, count, const float *[, ctx])
    if (!A3) RET(E_POINTER_);
    if ((u32)A2 != SUBCL->f.ch) RET(E_INVALIDARG_);
    float *v = vols(c);
    for (u32 i = 0; i < (u32)A2; i++) { float x; memcpy(&x, (void *)(A3 + 4 * i), 4); if (!(x >= 0.0f && x <= 1.0f)) RET(E_INVALIDARG_); }
    for (u32 i = 0; i < (u32)A2; i++) memcpy(&v[i], (void *)(A3 + 4 * i), 4);
    RET(S_OK_);
}
SHIM(WAST_GetAllVolumes) {
    if (!A3) RET(E_POINTER_);
    if ((u32)A2 != SUBCL->f.ch) RET(E_INVALIDARG_);
    memcpy((void *)A3, vols(c), 4ull * (u32)A2);
    RET(S_OK_);
}

// ---- IAudioSessionControl2 -----------------------------------------------------------------------------
SHIM(WASC_GetState) {
    if (!A2) RET(E_POINTER_);
    int active = 0;
    pthread_mutex_lock(&lock); for (Client *cl = clients; cl; cl = cl->next) if (cl->started) active = 1; pthread_mutex_unlock(&lock);
    ST32(A2, active ? 1 /* AudioSessionStateActive */ : 0);
    RET(S_OK_);
}
static u64 wcopy(const u16 *w) { size_t n = 0; while (w[n]) n++; u16 *r = malloc(2 * (n + 1)); memcpy(r, w, 2 * (n + 1)); return (u64)r; }
SHIM(WASC_GetDisplayName) { if (!A2) RET(E_POINTER_); ST64(A2, wcopy(sess_name)); RET(S_OK_); }
SHIM(WASC_SetDisplayName) {
    if (!A2) RET(E_POINTER_);
    size_t n = 0; while (LD16(A2 + 2 * n)) n++;
    u16 *w = malloc(2 * (n + 1)); memcpy(w, (void *)A2, 2 * (n + 1));
    free(sess_name); sess_name = w;
    RET(S_OK_);
}
SHIM(WASC_GetIconPath) { if (!A2) RET(E_POINTER_); ST64(A2, (u64)wdup("")); RET(S_OK_); }
SHIM(WASC_SetIconPath) { RET(A2 ? S_OK_ : E_POINTER_); }
SHIM(WASC_GetGroupingParam) { if (!A2) RET(E_POINTER_); memset((void *)A2, 0, 16); RET(S_OK_); }
SHIM(WASC_SetGroupingParam) { RET(A2 ? S_OK_ : E_POINTER_); }
SHIM(WASC_RegisterAudioSessionNotification) { RET(A2 ? S_OK_ : E_POINTER_); }   // no session events are ever raised
SHIM(WASC_UnregisterAudioSessionNotification) { RET(A2 ? S_OK_ : E_POINTER_); }
SHIM(WASC_GetSessionIdentifier) { if (!A2) RET(E_POINTER_); ST64(A2, (u64)wdup("{0.0.0.00000000}.{77696e74-6f6d-6163-0000-000000000001}|wintomac%b{00000000-0000-0000-0000-000000000000}")); RET(S_OK_); }
SHIM(WASC_GetSessionInstanceIdentifier) {
    if (!A2) RET(E_POINTER_);
    char s[160]; snprintf(s, sizeof s, "{0.0.0.00000000}.{77696e74-6f6d-6163-0000-000000000001}|wintomac%%b{00000000-0000-0000-0000-000000000000}|1%%b%d", (int)getpid());
    ST64(A2, (u64)wdup(s)); RET(S_OK_);
}
SHIM(WASC_GetProcessId) { if (!A2) RET(E_POINTER_); ST32(A2, (u32)getpid()); RET(S_OK_); }
SHIM(WASC_IsSystemSoundsSession) { RET(S_FALSE_); }
SHIM(WASC_SetDuckingPreference) { RET(S_OK_); }

// ---- IAudioSessionManager2 / IAudioSessionEnumerator -----------------------------------------------
SHIM(WASM_GetAudioSessionControl) { if (!A4) RET(E_POINTER_); addref(&sessctl); ST64(A4, (u64)&sessctl); RET(S_OK_); }  // (this, guid, flags, **ctl)
SHIM(WASM_GetSimpleAudioVolume) { if (!A4) RET(E_POINTER_); ST64(A4, (u64)&simplevol); RET(S_OK_); }
SHIM(WASM_GetSessionEnumerator) { if (!A2) RET(E_POINTER_); ST64(A2, (u64)&sessenum); RET(S_OK_); }
SHIM(WASM_RegisterSessionNotification) { RET(A2 ? S_OK_ : E_POINTER_); }
SHIM(WASM_UnregisterSessionNotification) { RET(A2 ? S_OK_ : E_POINTER_); }
SHIM(WASM_RegisterDuckNotification) { RET(A3 ? S_OK_ : E_POINTER_); }
SHIM(WASM_UnregisterDuckNotification) { RET(A2 ? S_OK_ : E_POINTER_); }
SHIM(WASE_GetCount) { if (!A2) RET(E_POINTER_); ST32(A2, 1); RET(S_OK_); }
SHIM(WASE_GetSession) { if (!A3) RET(E_POINTER_); if ((u32)A2 != 0) RET(E_INVALIDARG_); ST64(A3, (u64)&sessctl); RET(S_OK_); }

// ---- IAudioEndpointVolume(Ex): scales this process' output only (never the Mac's system volume) -------
#define EP_MIN_DB -65.25f
static float db_of(float s) { return s <= 0.0f ? EP_MIN_DB : fmaxf(20.0f * log10f(s), EP_MIN_DB); }
static float scalar_of(float db) { return db <= EP_MIN_DB ? 0.0f : powf(10.0f, fminf(db, 0.0f) / 20.0f); }
static u32 ep_channels(Ctx *c) { (void)c; init_mixf(); return mixf.ch; }
SHIM(WAEV_RegisterControlChangeNotify) { RET(A2 ? S_OK_ : E_POINTER_); }
SHIM(WAEV_UnregisterControlChangeNotify) { RET(A2 ? S_OK_ : E_POINTER_); }
SHIM(WAEV_GetChannelCount) { if (!A2) RET(E_POINTER_); ST32(A2, ep_channels(c)); RET(S_OK_); }
SHIM(WAEV_SetMasterVolumeLevel) { float db = argf(c->x[1]); if (db < EP_MIN_DB || db > 0.0f) RET(E_INVALIDARG_); ep_vol = scalar_of(db); RET(S_OK_); }
SHIM(WAEV_SetMasterVolumeLevelScalar) { float v = argf(c->x[1]); if (!(v >= 0.0f && v <= 1.0f)) RET(E_INVALIDARG_); ep_vol = v; RET(S_OK_); }
SHIM(WAEV_GetMasterVolumeLevel) { if (!A2) RET(E_POINTER_); float v = db_of(ep_vol); memcpy((void *)A2, &v, 4); RET(S_OK_); }
SHIM(WAEV_GetMasterVolumeLevelScalar) { if (!A2) RET(E_POINTER_); float v = ep_vol; memcpy((void *)A2, &v, 4); RET(S_OK_); }
SHIM(WAEV_SetChannelVolumeLevel) {
    float db = argf(c->x[2]);
    if ((u32)A2 >= ep_channels(c) || db < EP_MIN_DB || db > 0.0f) RET(E_INVALIDARG_);
    ep_ch[(u32)A2] = scalar_of(db); RET(S_OK_);
}
SHIM(WAEV_SetChannelVolumeLevelScalar) {
    float v = argf(c->x[2]);
    if ((u32)A2 >= ep_channels(c) || !(v >= 0.0f && v <= 1.0f)) RET(E_INVALIDARG_);
    ep_ch[(u32)A2] = v; RET(S_OK_);
}
SHIM(WAEV_GetChannelVolumeLevel) { if (!A3) RET(E_POINTER_); if ((u32)A2 >= ep_channels(c)) RET(E_INVALIDARG_); float v = db_of(ep_ch[(u32)A2]); memcpy((void *)A3, &v, 4); RET(S_OK_); }
SHIM(WAEV_GetChannelVolumeLevelScalar) { if (!A3) RET(E_POINTER_); if ((u32)A2 >= ep_channels(c)) RET(E_INVALIDARG_); memcpy((void *)A3, &ep_ch[(u32)A2], 4); RET(S_OK_); }
SHIM(WAEV_SetMute) { ep_mute = (u32)A2 != 0; RET(S_OK_); }
SHIM(WAEV_GetMute) { if (!A2) RET(E_POINTER_); ST32(A2, (u32)ep_mute); RET(S_OK_); }
SHIM(WAEV_GetVolumeStepInfo) { if (!A2 || !A3) RET(E_POINTER_); ST32(A2, (u32)lroundf(ep_vol * 100)); ST32(A3, 101); RET(S_OK_); }
SHIM(WAEV_VolumeStepUp) { ep_vol = fminf(1.0f, ep_vol + 0.01f); RET(S_OK_); }
SHIM(WAEV_VolumeStepDown) { ep_vol = fmaxf(0.0f, ep_vol - 0.01f); RET(S_OK_); }
SHIM(WAEV_QueryHardwareSupport) { if (!A2) RET(E_POINTER_); ST32(A2, 0); RET(S_OK_); }
static u32 vol_range(u64 mn, u64 mx, u64 inc) {
    if (!mn || !mx || !inc) return E_POINTER_;
    float a = EP_MIN_DB, b = 0.0f, d = 0.03125f;
    memcpy((void *)mn, &a, 4); memcpy((void *)mx, &b, 4); memcpy((void *)inc, &d, 4);
    return S_OK_;
}
SHIM(WAEV_GetVolumeRange) { RET(vol_range(A2, A3, A4)); }
SHIM(WAEV_GetVolumeRangeChannel) { if ((u32)A2 >= ep_channels(c)) RET(E_INVALIDARG_); RET(vol_range(A3, A4, AN(5))); }

// ---- IAudioMeterInformation (always silent) ------------------------------------------------------------
SHIM(WAMI_GetPeakValue) { if (!A2) RET(E_POINTER_); ST32(A2, 0); RET(S_OK_); }
SHIM(WAMI_GetMeteringChannelCount) { if (!A2) RET(E_POINTER_); ST32(A2, ep_channels(c)); RET(S_OK_); }
SHIM(WAMI_GetChannelsPeakValues) { if (!A3) RET(E_POINTER_); memset((void *)A3, 0, 4ull * (u32)A2); RET(S_OK_); }
SHIM(WAMI_QueryHardwareSupport) { if (!A2) RET(E_POINTER_); ST32(A2, 0); RET(S_OK_); }

// ---- ActivateAudioInterfaceAsync (mmdevapi.dll; used by UWP-style code, SDL's WinRT path, newer engines) ------
typedef struct { Hdr h; u32 hr; u64 iface; } AsyncOp;
static void asyncop_destroy(Hdr *o) { free(o); }
SHIM(WAAO_GetActivateResult) {   // (this, HRESULT *hr, IUnknown **iface)
    AsyncOp *op = (AsyncOp *)A1;
    if (!A2 || !A3) RET(E_POINTER_);
    ST32(A2, op->hr); ST64(A3, op->iface); op->iface = 0;   // ownership moves to the caller
    RET(S_OK_);
}
// (LPCWSTR path, REFIID, PROPVARIANT *params, IActivateAudioInterfaceCompletionHandler *, IActivateAudioInterfaceAsyncOperation **)
SHIM(ActivateAudioInterfaceAsync) {
    init_statics();
    u64 path = A1, iid = A2, handler = A4, opout = AN(5);
    if (!path || !iid || !handler || !opout) RET(E_POINTER_);
    int capture = wcontains(path, "2EEF81BE-33FA-4800-9670-1CD474972C3F") || wcontains(path, devs[1].id);   // DEVINTERFACE_AUDIO_CAPTURE
    AsyncOp *op = calloc(1, sizeof *op);
    hdr_init(&op->h, vt_asyncop, IIDS_ASYNCOP, NULL, asyncop_destroy);
    op->hr = activate(&devs[capture], iid, (u64)&op->iface);
    op->h.refs = 2;   // the caller's reference + ours across the callback
    ST64(opout, (u64)op);
    u64 args[2] = { handler, (u64)op };
    rt_guest_call(c, LD64(LD64(handler) + 24), 2, args);   // IActivateAudioInterfaceCompletionHandler::ActivateCompleted
    release(&op->h);
    RET(S_OK_);
}

// ---- class factory -------------------------------------------------------------------------------------
static u32 create_enumerator(Ctx *c, u64 iid, u64 out) {
    (void)c;
    init_statics();
    if (!rt_guid_eq(iid, &IID_IUnknown_) && !rt_guid_eq(iid, &IID_IMMDeviceEnumerator_)) return E_NOINTERFACE_;
    ST64(out, (u64)&enumerator);
    return S_OK_;
}
__attribute__((constructor)) static void register_mmdevapi(void) {
    rt_com_register(&CLSID_MMDeviceEnumerator_, create_enumerator, "MMDeviceEnumerator");
}

const Shim shims_wasapi[] = {
#define E(n) { NULL, #n, sh_##n }
#define M(p, n) { NULL, "__wintomac_" #p "_" #n, sh_##p##_##n }
#define U(p) { NULL, "__wintomac_" #p "_QueryInterface", sh_OBJ_QueryInterface }, { NULL, "__wintomac_" #p "_AddRef", sh_OBJ_AddRef }, \
             { NULL, "__wintomac_" #p "_Release", sh_OBJ_Release }
#define S(p, n) { NULL, "__wintomac_" #p "_" #n, sh_WAST_##n }
    E(ActivateAudioInterfaceAsync),
    U(WAEN), M(WAEN, EnumAudioEndpoints), M(WAEN, GetDefaultAudioEndpoint), M(WAEN, GetDevice), M(WAEN, RegisterEndpointNotificationCallback),
    M(WAEN, UnregisterEndpointNotificationCallback),
    U(WADV), M(WADV, Activate), M(WADV, OpenPropertyStore), M(WADV, GetId), M(WADV, GetState),
    U(WAEP), M(WAEP, GetDataFlow),
    U(WACO), M(WACO, GetCount), M(WACO, Item),
    U(WAPS), M(WAPS, GetCount), M(WAPS, GetAt), M(WAPS, GetValue), M(WAPS, SetValue), M(WAPS, Commit),
    U(WACL), M(WACL, Initialize), M(WACL, GetBufferSize), M(WACL, GetStreamLatency), M(WACL, GetCurrentPadding), M(WACL, IsFormatSupported),
    M(WACL, GetMixFormat), M(WACL, GetDevicePeriod), M(WACL, Start), M(WACL, Stop), M(WACL, Reset), M(WACL, SetEventHandle), M(WACL, GetService),
    M(WACL, IsOffloadCapable), M(WACL, SetClientProperties), M(WACL, GetBufferSizeLimits), M(WACL, GetSharedModeEnginePeriod),
    M(WACL, GetCurrentSharedModeEnginePeriod), M(WACL, InitializeSharedAudioStream),
    U(WARC), M(WARC, GetBuffer), M(WARC, ReleaseBuffer),
    U(WACC), M(WACC, GetBuffer), M(WACC, ReleaseBuffer), M(WACC, GetNextPacketSize),
    U(WACK), M(WACK, GetFrequency), M(WACK, GetPosition), M(WACK, GetCharacteristics), U(WAK2), M(WAK2, GetDevicePosition),
    U(WASV), M(WASV, SetMasterVolume), M(WASV, GetMasterVolume), M(WASV, SetMute), M(WASV, GetMute),
    U(WAST), M(WAST, GetChannelCount), M(WAST, SetChannelVolume), M(WAST, GetChannelVolume), M(WAST, SetAllVolumes), M(WAST, GetAllVolumes),
    U(WACH), S(WACH, GetChannelCount), S(WACH, SetChannelVolume), S(WACH, GetChannelVolume), S(WACH, SetAllVolumes), S(WACH, GetAllVolumes),
    U(WASC), M(WASC, GetState), M(WASC, GetDisplayName), M(WASC, SetDisplayName), M(WASC, GetIconPath), M(WASC, SetIconPath),
    M(WASC, GetGroupingParam), M(WASC, SetGroupingParam), M(WASC, RegisterAudioSessionNotification), M(WASC, UnregisterAudioSessionNotification),
    M(WASC, GetSessionIdentifier), M(WASC, GetSessionInstanceIdentifier), M(WASC, GetProcessId), M(WASC, IsSystemSoundsSession),
    M(WASC, SetDuckingPreference),
    U(WASM), M(WASM, GetAudioSessionControl), M(WASM, GetSimpleAudioVolume), M(WASM, GetSessionEnumerator), M(WASM, RegisterSessionNotification),
    M(WASM, UnregisterSessionNotification), M(WASM, RegisterDuckNotification), M(WASM, UnregisterDuckNotification),
    U(WASE), M(WASE, GetCount), M(WASE, GetSession),
    U(WAEV), M(WAEV, RegisterControlChangeNotify), M(WAEV, UnregisterControlChangeNotify), M(WAEV, GetChannelCount), M(WAEV, SetMasterVolumeLevel),
    M(WAEV, SetMasterVolumeLevelScalar), M(WAEV, GetMasterVolumeLevel), M(WAEV, GetMasterVolumeLevelScalar), M(WAEV, SetChannelVolumeLevel),
    M(WAEV, SetChannelVolumeLevelScalar), M(WAEV, GetChannelVolumeLevel), M(WAEV, GetChannelVolumeLevelScalar), M(WAEV, SetMute), M(WAEV, GetMute),
    M(WAEV, GetVolumeStepInfo), M(WAEV, VolumeStepUp), M(WAEV, VolumeStepDown), M(WAEV, QueryHardwareSupport), M(WAEV, GetVolumeRange),
    M(WAEV, GetVolumeRangeChannel),
    U(WAMI), M(WAMI, GetPeakValue), M(WAMI, GetMeteringChannelCount), M(WAMI, GetChannelsPeakValues), M(WAMI, QueryHardwareSupport),
    U(WAAO), M(WAAO, GetActivateResult),
#undef S
#undef U
#undef M
#undef E
    { 0, 0, 0 }
};
