// XAudio2 (2.7 via COM, 2.8/2.9 via XAudio2Create) and X3DAudio on FAudio (Ethan Lee et al., zlib).
//
// FAudio reimplements the XAudio2 API natively, and its structs (WAVEFORMATEX, buffers, sends, voice state,
// performance data, X3DAudio listener/emitter/DSP settings) are layout-identical to Microsoft's. So each guest
// interface is a thin host COM object: { vtbl of thunks, FAudio object }. The parts that need translation:
//   * voice pointers the game passes back (send lists, output matrices) -> the host FAudioVoice they wrap;
//   * callbacks: FAudio calls host function pointers on its mixer thread; those bridge into the game's
//     IXAudio2VoiceCallback / IXAudio2EngineCallback vtables on a guest context for that host thread;
//   * version differences of 2.7 (device enumeration, Initialize, voice details/state structs).
#include <FAudio.h>
#include <F3DAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "com.h"

typedef struct { u64 vtbl; u32 refs; int ver; FAudio *a; } GEngine;
typedef struct { FAudioVoiceCallback cb; u64 guest; } VoiceBridge;     // FAudio sees &cb
typedef struct { FAudioEngineCallback cb; u64 guest; } EngineBridge;
typedef struct { u64 vtbl; FAudioVoice *v; GEngine *eng; VoiceBridge *bridge; int kind; } GVoice;
enum { K_SOURCE, K_SUBMIX, K_MASTER };

static int xa_trace(void) { static int t = -1; if (t < 0) t = getenv("WINTOMAC_AUDIO_TRACE") != NULL; return t; }
static float argf(u64 raw) { float f; u32 b = (u32)raw; memcpy(&f, &b, 4); return f; }   // float passed in a stack slot

// ---- callbacks into the game (run on FAudio's mixer thread, or synchronously on a game thread) ---------
static void call_cb(u64 obj, int slot, int nargs, u64 a1, u64 a2) {
    if (!obj) return;
    Ctx *c = rt_foreign_ctx();
    u64 args[3] = { obj, a1, a2 };
    rt_guest_call(c, LD64(LD64(obj) + 8ull * slot), nargs + 1, args);
}
#define VB(cb) ((VoiceBridge *)(cb))
static void FAUDIOCALL vcb_pass_start(FAudioVoiceCallback *cb, uint32_t bytes) { call_cb(VB(cb)->guest, 0, 1, bytes, 0); }
static void FAUDIOCALL vcb_pass_end(FAudioVoiceCallback *cb) { call_cb(VB(cb)->guest, 1, 0, 0, 0); }
static void FAUDIOCALL vcb_stream_end(FAudioVoiceCallback *cb) { call_cb(VB(cb)->guest, 2, 0, 0, 0); }
static void FAUDIOCALL vcb_buffer_start(FAudioVoiceCallback *cb, void *ctx) { call_cb(VB(cb)->guest, 3, 1, (u64)ctx, 0); }
static void FAUDIOCALL vcb_buffer_end(FAudioVoiceCallback *cb, void *ctx) { call_cb(VB(cb)->guest, 4, 1, (u64)ctx, 0); }
static void FAUDIOCALL vcb_loop_end(FAudioVoiceCallback *cb, void *ctx) { call_cb(VB(cb)->guest, 5, 1, (u64)ctx, 0); }
static void FAUDIOCALL vcb_error(FAudioVoiceCallback *cb, void *ctx, uint32_t err) { call_cb(VB(cb)->guest, 6, 2, (u64)ctx, err); }
#define EB(cb) ((EngineBridge *)(cb))
static void FAUDIOCALL ecb_pass_start(FAudioEngineCallback *cb) { call_cb(EB(cb)->guest, 0, 0, 0, 0); }
static void FAUDIOCALL ecb_pass_end(FAudioEngineCallback *cb) { call_cb(EB(cb)->guest, 1, 0, 0, 0); }
static void FAUDIOCALL ecb_error(FAudioEngineCallback *cb, uint32_t err) { call_cb(EB(cb)->guest, 2, 1, err, 0); }

// ---- vtables --------------------------------------------------------------------------------
static const char *const ENG28[] = { "QueryInterface", "AddRef", "Release", "RegisterForCallbacks", "UnregisterForCallbacks",
    "CreateSourceVoice", "CreateSubmixVoice", "CreateMasteringVoice", "StartEngine", "StopEngine", "CommitChanges",
    "GetPerformanceData", "SetDebugConfiguration", NULL };
static const char *const ENG27[] = { "QueryInterface", "AddRef", "Release", "GetDeviceCount", "GetDeviceDetails", "Initialize",
    "RegisterForCallbacks", "UnregisterForCallbacks", "CreateSourceVoice", "CreateSubmixVoice", "CreateMasteringVoice27",
    "StartEngine", "StopEngine", "CommitChanges", "GetPerformanceData", "SetDebugConfiguration", NULL };
#define VOICE_COMMON(details) details, "V_SetOutputVoices", "V_SetEffectChain", "V_EnableEffect", "V_DisableEffect", \
    "V_GetEffectState", "V_SetEffectParameters", "V_GetEffectParameters", "V_SetFilterParameters", "V_GetFilterParameters", \
    "V_SetOutputFilterParameters", "V_GetOutputFilterParameters", "V_SetVolume", "V_GetVolume", "V_SetChannelVolumes", \
    "V_GetChannelVolumes", "V_SetOutputMatrix", "V_GetOutputMatrix", "V_DestroyVoice"
#define SOURCE_COMMON(state) "S_Start", "S_Stop", "S_SubmitSourceBuffer", "S_FlushSourceBuffers", "S_Discontinuity", \
    "S_ExitLoop", state, "S_SetFrequencyRatio", "S_GetFrequencyRatio"
static const char *const SRC28[] = { VOICE_COMMON("V_GetVoiceDetails"), SOURCE_COMMON("S_GetState"), "S_SetSourceSampleRate", NULL };
static const char *const SRC27[] = { VOICE_COMMON("V_GetVoiceDetails27"), SOURCE_COMMON("S_GetState27"), NULL };
static const char *const SUB28[] = { VOICE_COMMON("V_GetVoiceDetails"), NULL };
static const char *const SUB27[] = { VOICE_COMMON("V_GetVoiceDetails27"), NULL };
static const char *const MAS28[] = { VOICE_COMMON("V_GetVoiceDetails"), "M_GetChannelMask", NULL };
static const char *const MAS27[] = { VOICE_COMMON("V_GetVoiceDetails27"), NULL };

static u64 *vt_eng[2], *vt_voice[2][3];  // [is28][kind]
static void init_vtables(void) {
    static int done; if (done) return; done = 1;
    vt_eng[0] = rt_com_vtable("XA2", ENG27); vt_eng[1] = rt_com_vtable("XA2", ENG28);
    vt_voice[0][K_SOURCE] = rt_com_vtable("XA2", SRC27); vt_voice[1][K_SOURCE] = rt_com_vtable("XA2", SRC28);
    vt_voice[0][K_SUBMIX] = rt_com_vtable("XA2", SUB27); vt_voice[1][K_SUBMIX] = rt_com_vtable("XA2", SUB28);
    vt_voice[0][K_MASTER] = rt_com_vtable("XA2", MAS27); vt_voice[1][K_MASTER] = rt_com_vtable("XA2", MAS28);
}

static GEngine *new_engine(int ver, FAudio *a) {
    init_vtables();
    GEngine *e = calloc(1, sizeof *e);
    e->vtbl = (u64)vt_eng[ver >= 8]; e->refs = 1; e->ver = ver; e->a = a;
    return e;
}
static GVoice *new_voice(GEngine *e, int kind, FAudioVoice *v, VoiceBridge *b) {
    GVoice *g = calloc(1, sizeof *g);
    g->vtbl = (u64)vt_voice[e->ver >= 8][kind]; g->v = v; g->eng = e; g->kind = kind; g->bridge = b;
    return g;
}
#define ENG ((GEngine *)A1)
#define VOICE ((GVoice *)A1)
static FAudioVoice *host_voice(u64 g) { return g ? ((GVoice *)g)->v : NULL; }

// Guest XAUDIO2_VOICE_SENDS -> FAudioVoiceSends (voice pointers translated). Returns NULL for "default".
typedef struct { FAudioVoiceSends s; FAudioSendDescriptor d[64]; } Sends;
static const FAudioVoiceSends *sends(u64 g, Sends *out) {
    if (!g) return NULL;
    u32 n = LD32(g); u64 arr = LD64(g + 8);
    if (n > 64) n = 64;
    for (u32 i = 0; i < n; i++) { out->d[i].Flags = LD32(arr + 16ull * i); out->d[i].pOutputVoice = host_voice(LD64(arr + 16ull * i + 8)); }
    out->s.SendCount = n; out->s.pSends = out->d;
    return &out->s;
}
static const FAudioEffectChain *effects(u64 chain) {
    if (chain && LD32(chain)) {
        static int warned; if (!warned++) rt_log("xaudio2: effect chains (XAPO reverb etc.) not supported yet; playing dry");
    }
    return NULL;
}

// ---- IXAudio2 ---------------------------------------------------------------------------------
static const Guid IID_IXAudio2_27 = { 0x8bcf1f58, 0x9fe7, 0x4583, { 0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb } };
static const Guid IID_IXAudio2_28 = { 0x60d8dac8, 0x5aa1, 0x4e8e, { 0xb5, 0x97, 0x2f, 0x5e, 0x28, 0x83, 0xd4, 0x84 } };
static const Guid IID_IXAudio2_29 = { 0x2b02e3cf, 0x2e0b, 0x4ec3, { 0xbe, 0x45, 0x1b, 0x2a, 0x3f, 0xe7, 0x21, 0x0d } };
SHIM(XA2_QueryInterface) {
    if (rt_guid_eq(A2, &IID_IUnknown_) || rt_guid_eq(A2, &IID_IXAudio2_27) || rt_guid_eq(A2, &IID_IXAudio2_28) || rt_guid_eq(A2, &IID_IXAudio2_29)) {
        COM_ADDREF(A1); ST64(A3, A1); RET(S_OK_);
    }
    ST64(A3, 0); RET(E_NOINTERFACE_);
}
SHIM(XA2_AddRef) { RET(COM_ADDREF(A1)); }
SHIM(XA2_Release) {
    u32 r = COM_RELEASE(A1);
    if (!r) { FAudio_StopEngine(ENG->a); FAudio_Release(ENG->a); free(ENG); }
    RET(r);
}
SHIM(XA2_RegisterForCallbacks) {
    EngineBridge *b = calloc(1, sizeof *b);
    b->cb.OnProcessingPassStart = ecb_pass_start; b->cb.OnProcessingPassEnd = ecb_pass_end; b->cb.OnCriticalError = ecb_error;
    b->guest = A2;
    RET(FAudio_RegisterForCallbacks(ENG->a, &b->cb));
}
SHIM(XA2_UnregisterForCallbacks) { RET(0); }  // bridge stays registered but the game stops caring; FAudio has no lookup by guest ptr

// CreateSourceVoice(this, **voice, fmt, flags, float maxFreqRatio, callback, sends, effects)
SHIM(XA2_CreateSourceVoice) {
    VoiceBridge *b = NULL;
    if (AN(6)) {
        b = calloc(1, sizeof *b); b->guest = AN(6);
        b->cb.OnVoiceProcessingPassStart = vcb_pass_start; b->cb.OnVoiceProcessingPassEnd = vcb_pass_end;
        b->cb.OnStreamEnd = vcb_stream_end; b->cb.OnBufferStart = vcb_buffer_start; b->cb.OnBufferEnd = vcb_buffer_end;
        b->cb.OnLoopEnd = vcb_loop_end; b->cb.OnVoiceError = vcb_error;
    }
    Sends sd; FAudioSourceVoice *v = NULL;
    u32 hr = FAudio_CreateSourceVoice(ENG->a, &v, (const FAudioWaveFormatEx *)A3, (u32)A4, argf(AN(5)), b ? &b->cb : NULL,
                                      sends(AN(7), &sd), effects(AN(8)));
    if (xa_trace()) rt_log("CreateSourceVoice(fmt tag=%u ch=%u rate=%u bits=%u) = 0x%08x", LD16(A3), LD16(A3 + 2), LD32(A3 + 4), LD16(A3 + 14), hr);
    if (hr) { free(b); ST64(A2, 0); RET(hr); }
    ST64(A2, (u64)new_voice(ENG, K_SOURCE, v, b));
    RET(0);
}
// CreateSubmixVoice(this, **voice, channels, rate, flags, stage, sends, effects)
SHIM(XA2_CreateSubmixVoice) {
    Sends sd; FAudioSubmixVoice *v = NULL;
    u32 hr = FAudio_CreateSubmixVoice(ENG->a, &v, (u32)A3, (u32)A4, (u32)AN(5), (u32)AN(6), sends(AN(7), &sd), effects(AN(8)));
    ST64(A2, hr ? 0 : (u64)new_voice(ENG, K_SUBMIX, v, NULL));
    RET(hr);
}
// 2.8+: CreateMasteringVoice(this, **voice, channels, rate, flags, deviceId, effects, category)
SHIM(XA2_CreateMasteringVoice) {
    FAudioMasteringVoice *v = NULL;
    u32 hr = FAudio_CreateMasteringVoice8(ENG->a, &v, (u32)A3, (u32)A4, (u32)AN(5), (uint16_t *)AN(6), effects(AN(7)), (FAudioStreamCategory)(u32)AN(8));
    if (xa_trace() || hr) rt_log("CreateMasteringVoice(ch=%u rate=%u) = 0x%08x", (u32)A3, (u32)A4, hr);
    ST64(A2, hr ? 0 : (u64)new_voice(ENG, K_MASTER, v, NULL));
    RET(hr);
}
// 2.7: CreateMasteringVoice(this, **voice, channels, rate, flags, deviceIndex, effects)
SHIM(XA2_CreateMasteringVoice27) {
    FAudioMasteringVoice *v = NULL;
    u32 hr = FAudio_CreateMasteringVoice(ENG->a, &v, (u32)A3, (u32)A4, (u32)AN(5), (u32)AN(6), effects(AN(7)));
    ST64(A2, hr ? 0 : (u64)new_voice(ENG, K_MASTER, v, NULL));
    RET(hr);
}
SHIM(XA2_StartEngine) { RET(FAudio_StartEngine(ENG->a)); }
SHIM(XA2_StopEngine) { FAudio_StopEngine(ENG->a); RET(0); }
SHIM(XA2_CommitChanges) { RET(FAudio_CommitOperationSet(ENG->a, (u32)A2)); }
SHIM(XA2_GetPerformanceData) { FAudio_GetPerformanceData(ENG->a, (FAudioPerformanceData *)A2); RET(0); }
SHIM(XA2_SetDebugConfiguration) { RET(0); }
// 2.7 only
SHIM(XA2_GetDeviceCount) { RET(FAudio_GetDeviceCount(ENG->a, (uint32_t *)A2)); }
SHIM(XA2_GetDeviceDetails) { RET(FAudio_GetDeviceDetails(ENG->a, (u32)A2, (FAudioDeviceDetails *)A3)); }
SHIM(XA2_Initialize) { RET(FAudio_Initialize(ENG->a, (u32)A2, (FAudioProcessor)(u32)A3)); }

// ---- IXAudio2Voice -----------------------------------------------------------------------------
SHIM(XA2_V_GetVoiceDetails) { FAudioVoice_GetVoiceDetails(VOICE->v, (FAudioVoiceDetails *)A2); RET(0); }
SHIM(XA2_V_GetVoiceDetails27) {  // 2.7 struct has no ActiveFlags
    FAudioVoiceDetails d; FAudioVoice_GetVoiceDetails(VOICE->v, &d);
    ST32(A2, d.CreationFlags); ST32(A2 + 4, d.InputChannels); ST32(A2 + 8, d.InputSampleRate); RET(0);
}
SHIM(XA2_V_SetOutputVoices) { Sends sd; RET(FAudioVoice_SetOutputVoices(VOICE->v, sends(A2, &sd))); }
SHIM(XA2_V_SetEffectChain) { (void)effects(A2); RET(0); }
SHIM(XA2_V_EnableEffect) { RET(0); }
SHIM(XA2_V_DisableEffect) { RET(0); }
SHIM(XA2_V_GetEffectState) { if (A3) ST32(A3, 0); RET(0); }
SHIM(XA2_V_SetEffectParameters) { RET(0); }
SHIM(XA2_V_GetEffectParameters) { if (A3 && (u32)A4) memset((void *)A3, 0, (u32)A4); RET(0); }
SHIM(XA2_V_SetFilterParameters) { RET(FAudioVoice_SetFilterParameters(VOICE->v, (const FAudioFilterParameters *)A2, (u32)A3)); }
SHIM(XA2_V_GetFilterParameters) { FAudioVoice_GetFilterParameters(VOICE->v, (FAudioFilterParameters *)A2); RET(0); }
SHIM(XA2_V_SetOutputFilterParameters) { RET(FAudioVoice_SetOutputFilterParameters(VOICE->v, host_voice(A2), (const FAudioFilterParameters *)A3, (u32)A4)); }
SHIM(XA2_V_GetOutputFilterParameters) { FAudioVoice_GetOutputFilterParameters(VOICE->v, host_voice(A2), (FAudioFilterParameters *)A3); RET(0); }
SHIM(XA2_V_SetVolume) { RET(FAudioVoice_SetVolume(VOICE->v, c->x[1].f[0], (u32)A3)); }
SHIM(XA2_V_GetVolume) { FAudioVoice_GetVolume(VOICE->v, (float *)A2); RET(0); }
SHIM(XA2_V_SetChannelVolumes) { RET(FAudioVoice_SetChannelVolumes(VOICE->v, (u32)A2, (const float *)A3, (u32)A4)); }
SHIM(XA2_V_GetChannelVolumes) { FAudioVoice_GetChannelVolumes(VOICE->v, (u32)A2, (float *)A3); RET(0); }
// SetOutputMatrix(this, dest, srcCh, dstCh, levels*, opset)
SHIM(XA2_V_SetOutputMatrix) { RET(FAudioVoice_SetOutputMatrix(VOICE->v, host_voice(A2), (u32)A3, (u32)A4, (const float *)AN(5), (u32)AN(6))); }
SHIM(XA2_V_GetOutputMatrix) { FAudioVoice_GetOutputMatrix(VOICE->v, host_voice(A2), (u32)A3, (u32)A4, (float *)AN(5)); RET(0); }
SHIM(XA2_V_DestroyVoice) {
    GVoice *g = VOICE;
    FAudioVoice_DestroyVoice(g->v);
    free(g->bridge); free(g);
    RET(0);
}

// ---- IXAudio2SourceVoice / IXAudio2MasteringVoice ------------------------------------------------
SHIM(XA2_S_Start) { RET(FAudioSourceVoice_Start(VOICE->v, (u32)A2, (u32)A3)); }
SHIM(XA2_S_Stop) { RET(FAudioSourceVoice_Stop(VOICE->v, (u32)A2, (u32)A3)); }
SHIM(XA2_S_SubmitSourceBuffer) { RET(FAudioSourceVoice_SubmitSourceBuffer(VOICE->v, (const FAudioBuffer *)A2, (const FAudioBufferWMA *)A3)); }
SHIM(XA2_S_FlushSourceBuffers) { RET(FAudioSourceVoice_FlushSourceBuffers(VOICE->v)); }
SHIM(XA2_S_Discontinuity) { RET(FAudioSourceVoice_Discontinuity(VOICE->v)); }
SHIM(XA2_S_ExitLoop) { RET(FAudioSourceVoice_ExitLoop(VOICE->v, (u32)A2)); }
SHIM(XA2_S_GetState) { FAudioSourceVoice_GetState(VOICE->v, (FAudioVoiceState *)A2, (u32)A3); RET(0); }
SHIM(XA2_S_GetState27) { FAudioSourceVoice_GetState(VOICE->v, (FAudioVoiceState *)A2, 0); RET(0); }
SHIM(XA2_S_SetFrequencyRatio) { RET(FAudioSourceVoice_SetFrequencyRatio(VOICE->v, c->x[1].f[0], (u32)A3)); }
SHIM(XA2_S_GetFrequencyRatio) { FAudioSourceVoice_GetFrequencyRatio(VOICE->v, (float *)A2); RET(0); }
SHIM(XA2_S_SetSourceSampleRate) { RET(FAudioSourceVoice_SetSourceSampleRate(VOICE->v, (u32)A2)); }
SHIM(XA2_M_GetChannelMask) { RET(FAudioMasteringVoice_GetChannelMask(VOICE->v, (uint32_t *)A2)); }

// ---- entry points ----------------------------------------------------------------------------------
// XAudio2Create(IXAudio2 **out, flags, processor) [xaudio2_8 / xaudio2_9]
SHIM(XAudio2Create) {
    FAudio *a = NULL;
    u32 hr = FAudioCreate(&a, (u32)A2, (FAudioProcessor)(u32)A3);
    if (xa_trace() || hr) rt_log("XAudio2Create = 0x%08x", hr);
    ST64(A1, hr ? 0 : (u64)new_engine(9, a));
    RET(hr);
}
SHIM(XAudio2CreateWithVersionInfo) { sh_XAudio2Create(c); }
SHIM(XAudio2CreateReverb) { rt_log("xaudio2: XAudio2CreateReverb not supported yet"); if (A1) ST64(A1, 0); RET(E_NOTIMPL_); }
SHIM(CreateAudioReverb) { sh_XAudio2CreateReverb(c); }
SHIM(CreateAudioVolumeMeter) { if (A1) ST64(A1, 0); RET(E_NOTIMPL_); }

// 2.7 is a COM class (CLSID_XAudio2 / CLSID_XAudio2_Debug in xaudio2_7.dll); the game calls Initialize.
static u32 create27(Ctx *c, u64 iid, u64 out) {
    (void)c;
    FAudio *a = NULL;
    u32 hr = FAudioCOMConstructEXT(&a, 7);
    if (hr) return hr;
    GEngine *e = new_engine(7, a);
    if (!rt_guid_eq(iid, &IID_IUnknown_) && !rt_guid_eq(iid, &IID_IXAudio2_27)) { FAudio_Release(a); free(e); return E_NOINTERFACE_; }
    ST64(out, (u64)e);
    return 0;
}
static const Guid CLSID_XAudio2_27 = { 0x5a508685, 0xa254, 0x4fba, { 0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf } };
static const Guid CLSID_XAudio2_27d = { 0xdb05ea35, 0x0329, 0x4d4b, { 0xa5, 0x3a, 0x6d, 0xea, 0xd0, 0x3d, 0x38, 0x52 } };
__attribute__((constructor)) static void register_classes(void) {
    rt_com_register(&CLSID_XAudio2_27, create27, "XAudio2 2.7");
    rt_com_register(&CLSID_XAudio2_27d, create27, "XAudio2 2.7 (debug)");
}

// ---- X3DAudio: F3DAudio has identical handles and structs ------------------------------------------------
SHIM(X3DAudioInitialize) { RET(F3DAudioInitialize8((u32)A1, c->x[1].f[0], (uint8_t *)A3)); }   // (mask, float speed, handle)
SHIM(X3DAudioCalculate) {   // (handle, listener*, emitter*, flags, dsp*)
    F3DAudioCalculate((uint8_t *)A1, (const F3DAUDIO_LISTENER *)A2, (const F3DAUDIO_EMITTER *)A3, (u32)A4, (F3DAUDIO_DSP_SETTINGS *)AN(5));
    RET(0);
}

const Shim shims_xaudio2[] = {
#define E(n) { NULL, #n, sh_##n }
#define M(n) { NULL, "__wintomac_XA2_" #n, sh_XA2_##n }
    E(XAudio2Create), E(XAudio2CreateWithVersionInfo), E(XAudio2CreateReverb), E(CreateAudioReverb), E(CreateAudioVolumeMeter),
    E(X3DAudioInitialize), E(X3DAudioCalculate),
    M(QueryInterface), M(AddRef), M(Release), M(RegisterForCallbacks), M(UnregisterForCallbacks), M(CreateSourceVoice),
    M(CreateSubmixVoice), M(CreateMasteringVoice), M(CreateMasteringVoice27), M(StartEngine), M(StopEngine), M(CommitChanges),
    M(GetPerformanceData), M(SetDebugConfiguration), M(GetDeviceCount), M(GetDeviceDetails), M(Initialize),
    M(V_GetVoiceDetails), M(V_GetVoiceDetails27), M(V_SetOutputVoices), M(V_SetEffectChain), M(V_EnableEffect), M(V_DisableEffect),
    M(V_GetEffectState), M(V_SetEffectParameters), M(V_GetEffectParameters), M(V_SetFilterParameters), M(V_GetFilterParameters),
    M(V_SetOutputFilterParameters), M(V_GetOutputFilterParameters), M(V_SetVolume), M(V_GetVolume), M(V_SetChannelVolumes),
    M(V_GetChannelVolumes), M(V_SetOutputMatrix), M(V_GetOutputMatrix), M(V_DestroyVoice),
    M(S_Start), M(S_Stop), M(S_SubmitSourceBuffer), M(S_FlushSourceBuffers), M(S_Discontinuity), M(S_ExitLoop), M(S_GetState),
    M(S_GetState27), M(S_SetFrequencyRatio), M(S_GetFrequencyRatio), M(S_SetSourceSampleRate), M(M_GetChannelMask),
#undef M
#undef E
    { 0, 0, 0 }
};
