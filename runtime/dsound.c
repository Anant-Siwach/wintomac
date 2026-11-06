// DirectSound 8 (dsound.dll): IDirectSound8, IDirectSoundBuffer8, IDirectSoundNotify, with a software mixer.
//
// Secondary buffers live in host memory the game writes directly (Lock returns plain pointers: memory is
// identity mapped). One mixer pulls 48 kHz float stereo for an SDL3 audio stream: per buffer it resamples
// (linear, honouring SetFrequency), applies DirectSound volume (hundredths of dB) and pan, and advances the
// play cursor; looping buffers wrap, one-shots stop. Position notifications are signalled from the mixer
// thread with rt_signal_event (no guest code runs there).
//
// No 3D (IDirectSound3DBuffer/Listener) or FX yet: QueryInterface for them fails cleanly, so games fall back.
// WINTOMAC_AUDIO_NULL=1 (or no audio device) mixes into a real-time null sink instead, keeping timing intact.
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "com.h"

#define MIX_RATE 48000
#define DS_OK 0u
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_INVALIDCALL 0x88780032u
#define DSERR_UNSUPPORTED E_NOTIMPL_
#define DSERR_BADFORMAT 0x88780064u
#define DSERR_NODRIVER 0x88780078u
#define DSBCAPS_PRIMARYBUFFER 0x1
#define DSBCAPS_CTRLFREQUENCY 0x20
#define DSBCAPS_CTRLPAN 0x40
#define DSBCAPS_CTRLVOLUME 0x80
#define DSBCAPS_CTRLPOSITIONNOTIFY 0x100
#define DSBCAPS_GETCURRENTPOSITION2 0x10000
#define DSBPLAY_LOOPING 1
#define DSBSTATUS_PLAYING 1
#define DSBSTATUS_LOOPING 4
#define DSBLOCK_FROMWRITECURSOR 1
#define DSBLOCK_ENTIREBUFFER 2
#define DSBPN_OFFSETSTOP 0xFFFFFFFFu

static const Guid IID_IDirectSound_ = { 0x279AFA83, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const Guid IID_IDirectSound8_ = { 0xC50A7E93, 0xF395, 0x4834, { 0x9E, 0xF6, 0x7F, 0xA9, 0x9D, 0xE5, 0x09, 0x66 } };
static const Guid IID_IDirectSoundBuffer_ = { 0x279AFA85, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const Guid IID_IDirectSoundBuffer8_ = { 0x6825A449, 0x7524, 0x4D82, { 0x92, 0x0F, 0x50, 0xE3, 0x6A, 0xB3, 0xAB, 0x1E } };
static const Guid IID_IDirectSoundNotify_ = { 0xB0210783, 0x89cd, 0x11d0, { 0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16 } };
static const Guid CLSID_DirectSound_ = { 0x47d4d946, 0x62e8, 0x11cf, { 0x93, 0xbc, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const Guid CLSID_DirectSound8_ = { 0x3901cc3f, 0x84b5, 0x4fa4, { 0xba, 0x35, 0xaa, 0x81, 0x72, 0xb8, 0xa0, 0x9b } };

typedef struct { u16 tag, ch; u32 rate, avg; u16 align, bits, cb; } Fmt;   // WAVEFORMATEX (packed 18 bytes)
typedef struct { u32 offset; u64 event; } Notify;

typedef struct Buf Buf;
typedef struct { u64 vtbl; Buf *b; } NotifyObj;
struct Buf {
    u64 vtbl; u32 refs; u32 flags;
    Fmt fmt; u8 *data; u32 size; int primary;
    u64 pos;                  // play position in frames, 32.32 fixed point
    int playing, looping;
    i32 volume, pan; u32 freq;
    Notify *notes; u32 nnotes;
    NotifyObj notify;
    Buf *next;
};
typedef struct { u64 vtbl; u32 refs; int v8; Buf *primary; } Dev;

static pthread_mutex_t mix_lock = PTHREAD_MUTEX_INITIALIZER;
static Buf *bufs;                 // playing-capable secondary buffers
static u64 *vt_dev, *vt_buf, *vt_notify;

static float mb_gain(i32 mb) { return mb <= -10000 ? 0.0f : powf(10.0f, (float)mb / 2000.0f); }

static float sample(const Buf *b, u32 frame, int ch) {
    u32 c = b->fmt.ch == 1 ? 0 : (u32)ch;
    const u8 *p = b->data + (u64)frame * b->fmt.align + c * (b->fmt.bits / 8);
    if (b->fmt.tag == 3) { float f; memcpy(&f, p, 4); return f; }              // WAVE_FORMAT_IEEE_FLOAT
    switch (b->fmt.bits) {
    case 8: return ((int)*p - 128) / 128.0f;
    case 16: { i16 v; memcpy(&v, p, 2); return v / 32768.0f; }
    case 24: { i32 v = (i32)((u32)p[0] << 8 | (u32)p[1] << 16 | (u32)p[2] << 24) >> 8; return v / 8388608.0f; }
    case 32: { i32 v; memcpy(&v, p, 4); return v / 2147483648.0f; }
    default: return 0;
    }
}

// Signal notifications whose offset lies in (from, to] bytes, handling one wrap when from > to.
static void notify_range(Buf *b, u32 from, u32 to, int wrapped) {
    for (u32 i = 0; i < b->nnotes; i++) {
        u32 o = b->notes[i].offset;
        if (o == DSBPN_OFFSETSTOP) continue;
        int hit = wrapped ? (o > from || o <= to) : (o > from && o <= to);
        if (hit) rt_signal_event(b->notes[i].event);
    }
}
static void notify_stop(Buf *b) {
    for (u32 i = 0; i < b->nnotes; i++) if (b->notes[i].offset == DSBPN_OFFSETSTOP) rt_signal_event(b->notes[i].event);
}

static void mix(float *out, int frames) {
    memset(out, 0, sizeof(float) * 2 * frames);
    pthread_mutex_lock(&mix_lock);
    for (Buf *b = bufs; b; b = b->next) {
        if (!b->playing || !b->data || !b->size) continue;
        u32 nframes = b->size / b->fmt.align;
        if (!nframes) continue;
        u64 step = ((u64)(b->freq ? b->freq : b->fmt.rate) << 32) / MIX_RATE;
        float g = mb_gain(b->volume);
        float gl = g * (b->pan > 0 ? mb_gain(-b->pan) : 1.0f), gr = g * (b->pan < 0 ? mb_gain(b->pan) : 1.0f);
        u32 start_byte = (u32)(b->pos >> 32) * b->fmt.align;
        int wrapped = 0, stopped = 0;
        for (int i = 0; i < frames; i++) {
            u32 f0 = (u32)(b->pos >> 32);
            if (f0 >= nframes) {
                if (b->looping) { b->pos -= (u64)nframes << 32; f0 -= nframes; wrapped = 1; }
                else { stopped = 1; break; }
            }
            u32 f1 = f0 + 1 < nframes ? f0 + 1 : (b->looping ? 0 : f0);
            float t = (float)(u32)b->pos / 4294967296.0f;
            for (int ch = 0; ch < 2; ch++) {
                float s = sample(b, f0, ch) * (1 - t) + sample(b, f1, ch) * t;
                out[2 * i + ch] += s * (ch ? gr : gl);
            }
            b->pos += step;
        }
        if (stopped) {
            notify_range(b, start_byte, b->size, 0);
            b->playing = 0; b->pos = 0;
            notify_stop(b);
        } else {
            u32 f = (u32)(b->pos >> 32);
            if (f >= nframes && b->looping) { b->pos -= (u64)nframes << 32; f -= nframes; wrapped = 1; }
            notify_range(b, start_byte, f * b->fmt.align, wrapped);
        }
    }
    pthread_mutex_unlock(&mix_lock);
}

// ---- output: SDL3 audio stream, or a real-time null sink --------------------------------------------
static void SDLCALL pull(void *ud, SDL_AudioStream *s, int additional, int total) {
    (void)ud; (void)total;
    static float tmp[4096 * 2];
    while (additional > 0) {
        int frames = additional / 8; if (frames > 4096) frames = 4096; if (frames <= 0) break;
        mix(tmp, frames);
        SDL_PutAudioStreamData(s, tmp, frames * 8);
        additional -= frames * 8;
    }
}
static void *null_sink(void *arg) {
    (void)arg;
    static float tmp[480 * 2];
    struct timespec ts = { 0, 10 * 1000 * 1000 };
    for (;;) { mix(tmp, 480); nanosleep(&ts, NULL); }   // 480 frames per 10 ms = 48 kHz
    return NULL;
}
static int start_output(void) {
    static int state;  // 0 untried, 1 ok
    if (state) return 1;
    if (!getenv("WINTOMAC_AUDIO_NULL")) {
        SDL_InitSubSystem(SDL_INIT_AUDIO);
        SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, MIX_RATE };
        SDL_AudioStream *s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, pull, NULL);
        if (s && SDL_ResumeAudioStreamDevice(s)) { state = 1; return 1; }
        rt_log("dsound: no audio device (%s); using a silent real-time sink", SDL_GetError());
    }
    pthread_t t; pthread_create(&t, NULL, null_sink, NULL); pthread_detach(t);
    state = 1;
    return 1;
}

static void init_vtables(void) {
    static int done; if (done) return; done = 1;
    static const char *const dev[] = { "QueryInterface", "AddRef", "Release", "CreateSoundBuffer", "GetCaps", "DuplicateSoundBuffer",
        "SetCooperativeLevel", "Compact", "GetSpeakerConfig", "SetSpeakerConfig", "Initialize", "VerifyCertification", NULL };
    static const char *const buf[] = { "QueryInterface", "AddRef", "Release", "GetCaps", "GetCurrentPosition", "GetFormat", "GetVolume",
        "GetPan", "GetFrequency", "GetStatus", "Initialize", "Lock", "Play", "SetCurrentPosition", "SetFormat", "SetVolume", "SetPan",
        "SetFrequency", "Stop", "Unlock", "Restore", "SetFX", "AcquireResources", "GetObjectInPath", NULL };
    static const char *const nt[] = { "QueryInterface", "AddRef", "Release", "SetNotificationPositions", NULL };
    vt_dev = rt_com_vtable("DS", dev); vt_buf = rt_com_vtable("DSB", buf); vt_notify = rt_com_vtable("DSN", nt);
}

static Dev *new_dev(int v8) {
    init_vtables();
    Dev *d = calloc(1, sizeof *d);
    d->vtbl = (u64)vt_dev; d->refs = 1; d->v8 = v8;
    return d;
}
static Buf *new_buf(u32 flags) {
    Buf *b = calloc(1, sizeof *b);
    b->vtbl = (u64)vt_buf; b->refs = 1; b->flags = flags;
    b->notify.vtbl = (u64)vt_notify; b->notify.b = b;
    return b;
}
static int valid_fmt(const Fmt *f) {
    if (f->ch < 1 || f->ch > 8 || f->rate < 100 || f->rate > 384000) return 0;
    if (f->tag == 3) return f->bits == 32;
    if (f->tag == 1 || f->tag == 0xFFFE) return f->bits == 8 || f->bits == 16 || f->bits == 24 || f->bits == 32;
    return 0;
}

// ---- IDirectSound8 ------------------------------------------------------------------------------
#define DEV ((Dev *)A1)
#define BUF ((Buf *)A1)
SHIM(DS_QueryInterface) {
    if (rt_guid_eq(A2, &IID_IUnknown_) || rt_guid_eq(A2, &IID_IDirectSound_) || (DEV->v8 && rt_guid_eq(A2, &IID_IDirectSound8_))) {
        COM_ADDREF(A1); ST64(A3, A1); RET(S_OK_);
    }
    ST64(A3, 0); RET(E_NOINTERFACE_);
}
SHIM(DS_AddRef) { RET(COM_ADDREF(A1)); }
SHIM(DS_Release) { u32 r = COM_RELEASE(A1); if (!r) free(DEV); RET(r); }
// CreateSoundBuffer(this, const DSBUFFERDESC*, **buffer, outer)
SHIM(DS_CreateSoundBuffer) {
    u64 desc = A2;
    if (!desc || !A3) RET(DSERR_INVALIDPARAM);
    u32 flags = LD32(desc + 4), bytes = LD32(desc + 8); u64 wf = LD64(desc + 16);
    if (flags & DSBCAPS_PRIMARYBUFFER) {
        if (!DEV->primary) { DEV->primary = new_buf(flags); DEV->primary->primary = 1;
                             DEV->primary->fmt = (Fmt){ 1, 2, 44100, 44100 * 4, 4, 16, 0 }; }
        else COM_ADDREF(DEV->primary);
        ST64(A3, (u64)DEV->primary);
        RET(DS_OK);
    }
    if (!wf || bytes < 4 || bytes > 0x0FFFFFFF) RET(DSERR_INVALIDPARAM);
    Fmt f; memcpy(&f, (void *)wf, 16); f.cb = 0;
    if (f.tag == 0xFFFE) f.tag = LD16(wf + 24) == 3 ? 3 : 1;   // WAVEFORMATEXTENSIBLE: SubFormat GUID's first word
    if (!f.align) f.align = (u16)(f.ch * f.bits / 8);
    if (!valid_fmt(&f)) { rt_log("dsound: unsupported buffer format tag=%u ch=%u bits=%u rate=%u", f.tag, f.ch, f.bits, f.rate); RET(DSERR_BADFORMAT); }
    Buf *b = new_buf(flags);
    b->fmt = f; b->size = bytes - bytes % f.align; b->data = calloc(1, b->size);
    pthread_mutex_lock(&mix_lock); b->next = bufs; bufs = b; pthread_mutex_unlock(&mix_lock);
    start_output();
    ST64(A3, (u64)b);
    RET(DS_OK);
}
SHIM(DS_GetCaps) {  // DSCAPS: dwSize first; report a generic hardware-mixing-free device
    u32 sz = LD32(A2); if (sz < 8) RET(DSERR_INVALIDPARAM);
    memset((void *)(A2 + 4), 0, sz - 4);
    ST32(A2 + 4, 0x00000400 | 0x00000800 | 0x00000004 | 0x00000008 | 0x00000010 | 0x00000020);  // PRIMARY/SECONDARY 16bit/8bit/mono/stereo
    if (sz >= 16) { ST32(A2 + 8, 100); ST32(A2 + 12, 200000); }  // dwMinSecondarySampleRate / dwMaxSecondarySampleRate
    RET(DS_OK);
}
SHIM(DS_DuplicateSoundBuffer) {  // shares the sample data, own cursor/volume
    Buf *src = (Buf *)A2;
    if (!src || src->primary || !A3) RET(DSERR_INVALIDPARAM);
    Buf *b = new_buf(src->flags);
    b->fmt = src->fmt; b->size = src->size; b->data = src->data; b->volume = src->volume; b->pan = src->pan; b->freq = src->freq;
    b->primary = -1;  // marks shared data: not freed on release
    pthread_mutex_lock(&mix_lock); b->next = bufs; bufs = b; pthread_mutex_unlock(&mix_lock);
    ST64(A3, (u64)b);
    RET(DS_OK);
}
SHIM(DS_SetCooperativeLevel) { RET(DS_OK); }
SHIM(DS_Compact) { RET(DS_OK); }
SHIM(DS_GetSpeakerConfig) { ST32(A2, 0x00040004 /* DSSPEAKER_STEREO, wide geometry */); RET(DS_OK); }
SHIM(DS_SetSpeakerConfig) { RET(DS_OK); }
SHIM(DS_Initialize) { RET(DS_OK); }
SHIM(DS_VerifyCertification) { if (A2) ST32(A2, 1); RET(DS_OK); }

// ---- IDirectSoundBuffer8 --------------------------------------------------------------------------
SHIM(DSB_QueryInterface) {
    if (rt_guid_eq(A2, &IID_IUnknown_) || rt_guid_eq(A2, &IID_IDirectSoundBuffer_) || (!BUF->primary && rt_guid_eq(A2, &IID_IDirectSoundBuffer8_))) {
        COM_ADDREF(A1); ST64(A3, A1); RET(S_OK_);
    }
    if (rt_guid_eq(A2, &IID_IDirectSoundNotify_) && (BUF->flags & DSBCAPS_CTRLPOSITIONNOTIFY)) {
        COM_ADDREF(A1); ST64(A3, (u64)&BUF->notify); RET(S_OK_);
    }
    if (getenv("WINTOMAC_VERBOSE")) { char g[64]; rt_guid_str(A2, g, sizeof g); rt_log("dsound: buffer interface %s not supported (3D/FX?)", g); }
    ST64(A3, 0); RET(E_NOINTERFACE_);
}
SHIM(DSB_AddRef) { RET(COM_ADDREF(A1)); }
SHIM(DSB_Release) {
    Buf *b = BUF; u32 r = COM_RELEASE(b);
    if (!r && b->primary != 1) {
        pthread_mutex_lock(&mix_lock);
        for (Buf **p = &bufs; *p; p = &(*p)->next) if (*p == b) { *p = b->next; break; }
        pthread_mutex_unlock(&mix_lock);
        if (b->primary != -1) free(b->data);
        free(b->notes); free(b);
    }
    RET(r);
}
SHIM(DSB_GetCaps) {  // DSBCAPS { dwSize, dwFlags, dwBufferBytes, dwUnlockTransferRate, dwPlayCpuOverhead }
    ST32(A2 + 4, BUF->flags); ST32(A2 + 8, BUF->size); ST32(A2 + 12, 0); ST32(A2 + 16, 0); RET(DS_OK);
}
SHIM(DSB_GetCurrentPosition) {  // (this, *play, *write)
    Buf *b = BUF;
    pthread_mutex_lock(&mix_lock);
    u32 play = b->primary == 1 ? 0 : (u32)(b->pos >> 32) * b->fmt.align;
    u32 lead = b->playing ? ((b->fmt.rate / 100) * b->fmt.align) : 0;     // ~10 ms of mixer lead
    u32 write = b->size ? (play + lead) % b->size : 0;
    pthread_mutex_unlock(&mix_lock);
    if (A2) ST32(A2, play);
    if (A3) ST32(A3, write);
    RET(DS_OK);
}
SHIM(DSB_GetFormat) {  // (this, WAVEFORMATEX*, size, *written)
    u32 need = 18;
    if (A4) ST32(A4, need);
    if (!A2) RET(DS_OK);
    if ((u32)A3 < need) RET(DSERR_INVALIDPARAM);
    memcpy((void *)A2, &BUF->fmt, 16); ST16(A2 + 16, 0);
    RET(DS_OK);
}
SHIM(DSB_GetVolume) { ST32(A2, (u32)BUF->volume); RET(DS_OK); }
SHIM(DSB_GetPan) { ST32(A2, (u32)BUF->pan); RET(DS_OK); }
SHIM(DSB_GetFrequency) { ST32(A2, BUF->freq ? BUF->freq : BUF->fmt.rate); RET(DS_OK); }
SHIM(DSB_GetStatus) { u32 s = 0; if (BUF->playing) s |= DSBSTATUS_PLAYING | (BUF->looping ? DSBSTATUS_LOOPING : 0); ST32(A2, s); RET(DS_OK); }
SHIM(DSB_Initialize) { RET(DS_OK); }
// Lock(this, offset, bytes, **p1, *n1, **p2, *n2, flags)
SHIM(DSB_Lock) {
    Buf *b = BUF;
    if (b->primary == 1) RET(DSERR_INVALIDCALL);
    u32 off = (u32)A2, n = (u32)A3, flags = (u32)AN(8);
    if (flags & DSBLOCK_FROMWRITECURSOR) {
        u32 lead = b->playing ? ((b->fmt.rate / 100) * b->fmt.align) : 0;
        off = ((u32)(b->pos >> 32) * b->fmt.align + lead) % b->size;
    }
    if (flags & DSBLOCK_ENTIREBUFFER) n = b->size;
    if (off >= b->size || n > b->size || !A4 || !AN(5)) RET(DSERR_INVALIDPARAM);
    u32 first = n < b->size - off ? n : b->size - off;
    ST64(A4, (u64)(b->data + off)); ST32(AN(5), first);
    if (AN(6)) ST64(AN(6), first < n ? (u64)b->data : 0);
    if (AN(7)) ST32(AN(7), n - first);
    RET(DS_OK);
}
SHIM(DSB_Play) {  // (this, reserved, priority, flags)
    Buf *b = BUF;
    pthread_mutex_lock(&mix_lock);
    b->looping = ((u32)A4 & DSBPLAY_LOOPING) != 0;
    b->playing = 1;
    pthread_mutex_unlock(&mix_lock);
    start_output();
    RET(DS_OK);
}
SHIM(DSB_SetCurrentPosition) {
    Buf *b = BUF;
    if (b->primary == 1) RET(DSERR_INVALIDCALL);
    if ((u32)A2 >= b->size) RET(DSERR_INVALIDPARAM);
    pthread_mutex_lock(&mix_lock); b->pos = (u64)((u32)A2 / b->fmt.align) << 32; pthread_mutex_unlock(&mix_lock);
    RET(DS_OK);
}
SHIM(DSB_SetFormat) {
    if (BUF->primary != 1) RET(DSERR_INVALIDCALL);
    Fmt f; memcpy(&f, (void *)A2, 16);
    if (!valid_fmt(&f)) RET(DSERR_BADFORMAT);
    BUF->fmt = f;   // our mixer output is fixed; the primary format is just remembered
    RET(DS_OK);
}
SHIM(DSB_SetVolume) { i32 v = (i32)A2; if (v < -10000 || v > 0) RET(DSERR_INVALIDPARAM); BUF->volume = v; RET(DS_OK); }
SHIM(DSB_SetPan) { i32 v = (i32)A2; if (v < -10000 || v > 10000) RET(DSERR_INVALIDPARAM); BUF->pan = v; RET(DS_OK); }
SHIM(DSB_SetFrequency) { u32 f = (u32)A2; if (f && (f < 100 || f > 200000)) RET(DSERR_INVALIDPARAM); BUF->freq = f; RET(DS_OK); }
SHIM(DSB_Stop) {
    Buf *b = BUF;
    pthread_mutex_lock(&mix_lock);
    int was = b->playing; b->playing = 0;
    if (was) notify_stop(b);
    pthread_mutex_unlock(&mix_lock);
    RET(DS_OK);
}
SHIM(DSB_Unlock) { RET(DS_OK); }   // data was written in place
SHIM(DSB_Restore) { RET(DS_OK); }
SHIM(DSB_SetFX) { if ((u32)A2) { static int w; if (!w++) rt_log("dsound: SetFX (effects) not supported yet"); RET(DSERR_UNSUPPORTED); } RET(DS_OK); }
SHIM(DSB_AcquireResources) { RET(DS_OK); }
SHIM(DSB_GetObjectInPath) { if (AN(5)) ST64(AN(5), 0); RET(DSERR_UNSUPPORTED); }

// ---- IDirectSoundNotify ------------------------------------------------------------------------------
#define NOTE ((NotifyObj *)A1)
SHIM(DSN_QueryInterface) { c->r[RCX] = (u64)NOTE->b; sh_DSB_QueryInterface(c); }   // same COM object as the buffer
SHIM(DSN_AddRef) { RET(COM_ADDREF(NOTE->b)); }
SHIM(DSN_Release) { c->r[RCX] = (u64)NOTE->b; sh_DSB_Release(c); }
SHIM(DSN_SetNotificationPositions) {  // (this, count, DSBPOSITIONNOTIFY* { DWORD offset; HANDLE event })
    Buf *b = NOTE->b; u32 n = (u32)A2;
    if (b->playing) RET(DSERR_INVALIDCALL);
    Notify *v = n ? calloc(n, sizeof *v) : NULL;
    for (u32 i = 0; i < n; i++) {
        v[i].offset = LD32(A3 + 16ull * i); v[i].event = LD64(A3 + 16ull * i + 8);
        if (v[i].offset != DSBPN_OFFSETSTOP && v[i].offset >= b->size) { free(v); RET(DSERR_INVALIDPARAM); }
    }
    pthread_mutex_lock(&mix_lock); free(b->notes); b->notes = v; b->nnotes = n; pthread_mutex_unlock(&mix_lock);
    RET(DS_OK);
}

// ---- entry points ---------------------------------------------------------------------------------------
SHIM(DirectSoundCreate) { if (!A2) RET(DSERR_INVALIDPARAM); ST64(A2, (u64)new_dev(0)); RET(DS_OK); }   // (guid, **ds, outer)
SHIM(DirectSoundCreate8) { if (!A2) RET(DSERR_INVALIDPARAM); ST64(A2, (u64)new_dev(1)); RET(DS_OK); }
// DirectSoundEnumerate(callback(guid, description, module, ctx), ctx): the default device, then one named device.
static void enumerate(Ctx *c, int wide) {
    static u16 wdesc1[] = { 'P','r','i','m','a','r','y',' ','S','o','u','n','d',' ','D','r','i','v','e','r',0 };
    static u16 wdesc2[] = { 'S','p','e','a','k','e','r','s',0 }, wmod[] = { 0 };
    static char adesc1[] = "Primary Sound Driver", adesc2[] = "Speakers", amod[] = "";
    static u8 dev_guid[16] = { 0x77, 0x69, 0x6e, 0x74, 0x6f, 0x6d, 0x61, 0x63, 0x64, 0x73, 0x6f, 0x75, 0x6e, 0x64, 0, 1 };
    u64 cb = A1, ctx = A2;
    u64 a1[4] = { 0, wide ? (u64)wdesc1 : (u64)adesc1, wide ? (u64)wmod : (u64)amod, ctx };
    if (!(u32)rt_guest_call(c, cb, 4, a1)) return;
    u64 a2[4] = { (u64)dev_guid, wide ? (u64)wdesc2 : (u64)adesc2, wide ? (u64)wmod : (u64)amod, ctx };
    rt_guest_call(c, cb, 4, a2);
}
SHIM(DirectSoundEnumerateA) { enumerate(c, 0); RET(DS_OK); }
SHIM(DirectSoundEnumerateW) { enumerate(c, 1); RET(DS_OK); }
SHIM(DirectSoundCaptureCreate) { if (A2) ST64(A2, 0); RET(DSERR_NODRIVER); }
SHIM(DirectSoundCaptureCreate8) { if (A2) ST64(A2, 0); RET(DSERR_NODRIVER); }
SHIM(DirectSoundCaptureEnumerateA) { RET(DS_OK); }
SHIM(DirectSoundCaptureEnumerateW) { RET(DS_OK); }
SHIM(GetDeviceID) { if (A2) memset((void *)A2, 0, 16); RET(DS_OK); }

static u32 create_ds(Ctx *c, u64 iid, u64 out, int v8) {
    (void)c;
    if (!rt_guid_eq(iid, &IID_IUnknown_) && !rt_guid_eq(iid, &IID_IDirectSound_) && !rt_guid_eq(iid, &IID_IDirectSound8_)) return E_NOINTERFACE_;
    ST64(out, (u64)new_dev(v8));
    return 0;
}
static u32 create_ds7(Ctx *c, u64 iid, u64 out) { return create_ds(c, iid, out, 0); }
static u32 create_ds8(Ctx *c, u64 iid, u64 out) { return create_ds(c, iid, out, 1); }
__attribute__((constructor)) static void register_ds(void) {
    rt_com_register(&CLSID_DirectSound_, create_ds7, "DirectSound");
    rt_com_register(&CLSID_DirectSound8_, create_ds8, "DirectSound8");
}

const Shim shims_dsound[] = {
#define E(n) { NULL, #n, sh_##n }
#define M(p, n) { NULL, "__wintomac_" #p "_" #n, sh_##p##_##n }
    E(DirectSoundCreate), E(DirectSoundCreate8), E(DirectSoundEnumerateA), E(DirectSoundEnumerateW), E(DirectSoundCaptureCreate),
    E(DirectSoundCaptureCreate8), E(DirectSoundCaptureEnumerateA), E(DirectSoundCaptureEnumerateW), E(GetDeviceID),
    M(DS, QueryInterface), M(DS, AddRef), M(DS, Release), M(DS, CreateSoundBuffer), M(DS, GetCaps), M(DS, DuplicateSoundBuffer),
    M(DS, SetCooperativeLevel), M(DS, Compact), M(DS, GetSpeakerConfig), M(DS, SetSpeakerConfig), M(DS, Initialize), M(DS, VerifyCertification),
    M(DSB, QueryInterface), M(DSB, AddRef), M(DSB, Release), M(DSB, GetCaps), M(DSB, GetCurrentPosition), M(DSB, GetFormat), M(DSB, GetVolume),
    M(DSB, GetPan), M(DSB, GetFrequency), M(DSB, GetStatus), M(DSB, Initialize), M(DSB, Lock), M(DSB, Play), M(DSB, SetCurrentPosition),
    M(DSB, SetFormat), M(DSB, SetVolume), M(DSB, SetPan), M(DSB, SetFrequency), M(DSB, Stop), M(DSB, Unlock), M(DSB, Restore),
    M(DSB, SetFX), M(DSB, AcquireResources), M(DSB, GetObjectInPath),
    M(DSN, QueryInterface), M(DSN, AddRef), M(DSN, Release), M(DSN, SetNotificationPositions),
#undef M
#undef E
    { 0, 0, 0 }
};
