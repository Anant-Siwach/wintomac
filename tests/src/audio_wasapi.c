// WASAPI (Core Audio): MMDevice enumeration + property store, IAudioClient mix format / format negotiation / error
// codes, event-driven rendering of a sine for 30 periods (padding + clock), polling mode, a converted (16-bit 44.1 kHz)
// stream, capture, session/stream/endpoint volume, IAudioClient3, ActivateAudioInterfaceAsync.
// Only facts that hold on any Windows machine (and are independent of timing jitter) are printed; the expected output
// follows the documented behaviour (learn.microsoft.com, "Core Audio APIs").
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <math.h>
#include <stdio.h>

DEFINE_PROPERTYKEY(PKEY_AudioEngine_DeviceFormat_, 0xf19f064d, 0x082c, 0x4e27, 0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c, 0);
STDAPI ActivateAudioInterfaceAsync(LPCWSTR, REFIID, PROPVARIANT *, IActivateAudioInterfaceCompletionHandler *, IActivateAudioInterfaceAsyncOperation **);

#define YN(x) ((x) ? "yes" : "no")
static double now(void) { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart / f.QuadPart; }

static double phase;
static void sine(float *p, UINT32 frames, int ch, int rate) {
    for (UINT32 i = 0; i < frames; i++) {
        float s = 0.2f * sinf((float)phase); phase += 2 * 3.14159265 * 440.0 / rate;
        for (int c = 0; c < ch; c++) p[i * ch + c] = s;
    }
}

// ---- a minimal IActivateAudioInterfaceCompletionHandler ----
static HANDLE done_ev; static HRESULT async_hr = E_FAIL; static IUnknown *async_iface;
static HRESULT STDMETHODCALLTYPE h_qi(IActivateAudioInterfaceCompletionHandler *t, REFIID r, void **o) { (void)r; *o = t; return S_OK; }
static ULONG STDMETHODCALLTYPE h_ref(IActivateAudioInterfaceCompletionHandler *t) { (void)t; return 1; }
static HRESULT STDMETHODCALLTYPE h_done(IActivateAudioInterfaceCompletionHandler *t, IActivateAudioInterfaceAsyncOperation *op) {
    (void)t; HRESULT hr2; IActivateAudioInterfaceAsyncOperation_GetActivateResult(op, &hr2, &async_iface); async_hr = hr2; SetEvent(done_ev); return S_OK;
}
static IActivateAudioInterfaceCompletionHandlerVtbl hvt = { h_qi, h_ref, h_ref, h_done };
static IActivateAudioInterfaceCompletionHandler handler = { &hvt };

int main(void) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en);
    printf("CoCreateInstance(MMDeviceEnumerator): %s\n", SUCCEEDED(hr) ? "ok" : "FAILED"); if (FAILED(hr)) return 1;

    IMMDeviceCollection *coll; UINT cnt = 0;
    hr = IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &coll);
    IMMDeviceCollection_GetCount(coll, &cnt);
    IMMDevice *item = NULL; HRESULT hi = IMMDeviceCollection_Item(coll, cnt, &item);
    printf("render endpoints: %s, count>=1: %s, Item(count) fails: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED", YN(cnt >= 1), YN(hi == E_INVALIDARG && !item));
    IMMDeviceCollection_Release(coll);
    printf("GetDefaultAudioEndpoint(eAll): E_INVALIDARG: %s\n", YN(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eAll, eConsole, &item) == E_INVALIDARG));

    IMMDevice *dev; hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    DWORD state = 0; IMMDevice_GetState(dev, &state);
    LPWSTR id = NULL; IMMDevice_GetId(dev, &id);
    IMMDevice *again = NULL; HRESULT hg = IMMDeviceEnumerator_GetDevice(en, id, &again);
    printf("default render: %s, active: %s, id: %s, GetDevice(id): %s\n", SUCCEEDED(hr) ? "ok" : "FAILED", YN(state == DEVICE_STATE_ACTIVE),
           YN(id && id[0]), YN(SUCCEEDED(hg) && again));
    if (again) IMMDevice_Release(again);
    CoTaskMemFree(id);
    IMMEndpoint *ep; EDataFlow flow = eAll;
    if (SUCCEEDED(IMMDevice_QueryInterface(dev, &IID_IMMEndpoint, (void **)&ep))) { IMMEndpoint_GetDataFlow(ep, &flow); IMMEndpoint_Release(ep); }
    printf("IMMEndpoint data flow is eRender: %s\n", YN(flow == eRender));

    IPropertyStore *ps; hr = IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps);
    PROPVARIANT pv; PropVariantInit(&pv);
    IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &pv);
    printf("property store: %s, friendly name is a string: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED", YN(pv.vt == VT_LPWSTR && pv.pwszVal && pv.pwszVal[0]));
    PropVariantClear(&pv);
    IPropertyStore_GetValue(ps, &PKEY_AudioEngine_DeviceFormat_, &pv);
    printf("device format blob: %s\n", YN(pv.vt == VT_BLOB && pv.blob.cbSize >= sizeof(WAVEFORMATEX)));
    PropVariantClear(&pv);
    IPropertyStore_Release(ps);

    // ---- IAudioClient: formats and errors before Initialize ----
    IAudioClient *ac; hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    printf("Activate(IAudioClient): %s\n", SUCCEEDED(hr) ? "ok" : "FAILED"); if (FAILED(hr)) return 1;
    WAVEFORMATEX *mix = NULL; IAudioClient_GetMixFormat(ac, &mix);
    WAVEFORMATEXTENSIBLE *mx = (WAVEFORMATEXTENSIBLE *)mix;
    int ch = mix->nChannels, rate = (int)mix->nSamplesPerSec;
    printf("mix format: extensible=%s float=%s bits=%u align_ok=%s\n", YN(mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE),
           YN(mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(&mx->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)), mix->wBitsPerSample,
           YN(mix->nBlockAlign == ch * 4 && mix->nAvgBytesPerSec == (DWORD)rate * mix->nBlockAlign));
    WAVEFORMATEX *closest = (WAVEFORMATEX *)1;
    hr = IAudioClient_IsFormatSupported(ac, AUDCLNT_SHAREMODE_SHARED, mix, &closest);
    printf("IsFormatSupported(mix): S_OK=%s closest=NULL: %s\n", YN(hr == S_OK), YN(closest == NULL));
    WAVEFORMATEX pcm = { WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0 };
    hr = IAudioClient_IsFormatSupported(ac, AUDCLNT_SHAREMODE_SHARED, &pcm, &closest);
    printf("IsFormatSupported(16-bit 22 kHz mono): S_FALSE-with-closest or unsupported: %s\n",
           YN((hr == S_FALSE && closest) || (hr == AUDCLNT_E_UNSUPPORTED_FORMAT && !closest)));
    if (closest) CoTaskMemFree(closest);
    printf("IsFormatSupported(shared, no closest ptr): E_POINTER: %s\n", YN(IAudioClient_IsFormatSupported(ac, AUDCLNT_SHAREMODE_SHARED, mix, NULL) == E_POINTER));
    WAVEFORMATEX bad = { 0x55 /* MP3 */, 2, 44100, 16000, 1, 0, 0 };
    hr = IAudioClient_IsFormatSupported(ac, AUDCLNT_SHAREMODE_SHARED, &bad, &closest);
    printf("IsFormatSupported(MP3): unsupported: %s\n", YN(hr == AUDCLNT_E_UNSUPPORTED_FORMAT));
    UINT32 frames = 0; IAudioRenderClient *rc = NULL;
    printf("before Initialize: GetBufferSize=%s GetService=%s\n", IAudioClient_GetBufferSize(ac, &frames) == AUDCLNT_E_NOT_INITIALIZED ? "NOT_INITIALIZED" : "?",
           IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc) == AUDCLNT_E_NOT_INITIALIZED ? "NOT_INITIALIZED" : "?");
    REFERENCE_TIME defp = 0, minp = 0; IAudioClient_GetDevicePeriod(ac, &defp, &minp);
    printf("device period: default>0: %s min<=default: %s\n", YN(defp > 0), YN(minp > 0 && minp <= defp));

    // ---- event-driven shared stream ----
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, NULL);
    HRESULT hr2 = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, NULL);
    printf("Initialize(shared, event): %s, again: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED", hr2 == AUDCLNT_E_ALREADY_INITIALIZED ? "ALREADY_INITIALIZED" : "?");
    printf("Start without event handle: %s\n", IAudioClient_Start(ac) == AUDCLNT_E_EVENTHANDLE_NOT_SET ? "EVENTHANDLE_NOT_SET" : "?");
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    printf("SetEventHandle: %s\n", SUCCEEDED(IAudioClient_SetEventHandle(ac, ev)) ? "ok" : "FAILED");
    UINT32 bufsize = 0; IAudioClient_GetBufferSize(ac, &bufsize);
    REFERENCE_TIME lat = -1; IAudioClient_GetStreamLatency(ac, &lat);
    printf("buffer holds at least one period: %s, latency>=0: %s\n", YN((double)bufsize >= rate * (defp / 1e7) - 1), YN(lat >= 0));
    hr = IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc);
    IAudioClock *clk = NULL; HRESULT hc = IAudioClient_GetService(ac, &IID_IAudioClock, (void **)&clk);
    IAudioCaptureClient *wrong = NULL;
    printf("services: render=%s clock=%s capture-on-render=%s\n", SUCCEEDED(hr) ? "ok" : "FAILED", SUCCEEDED(hc) ? "ok" : "FAILED",
           IAudioClient_GetService(ac, &IID_IAudioCaptureClient, (void **)&wrong) == AUDCLNT_E_WRONG_ENDPOINT_TYPE ? "WRONG_ENDPOINT_TYPE" : "?");
    IAudioClock2 *clk2 = NULL; UINT64 dpos = 1;
    hr = IAudioClock_QueryInterface(clk, &IID_IAudioClock2, (void **)&clk2);
    if (SUCCEEDED(hr)) IAudioClock2_GetDevicePosition(clk2, &dpos, NULL);
    printf("IAudioClock2 before start: %s position=0: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED", YN(dpos == 0));
    UINT64 freq = 0, pos = 1, qpc = 0; IAudioClock_GetFrequency(clk, &freq); IAudioClock_GetPosition(clk, &pos, &qpc);
    printf("clock: frequency>0: %s position before start=0: %s\n", YN(freq > 0), YN(pos == 0));

    BYTE *data;
    printf("GetBuffer(buffer+1): %s\n", IAudioRenderClient_GetBuffer(rc, bufsize + 1, &data) == AUDCLNT_E_BUFFER_TOO_LARGE ? "BUFFER_TOO_LARGE" : "?");
    printf("ReleaseBuffer without GetBuffer: %s\n", IAudioRenderClient_ReleaseBuffer(rc, 16, 0) == AUDCLNT_E_OUT_OF_ORDER ? "OUT_OF_ORDER" : "?");
    hr = IAudioRenderClient_GetBuffer(rc, bufsize, &data);
    if (SUCCEEDED(hr)) { sine((float *)data, bufsize, ch, rate); IAudioRenderClient_ReleaseBuffer(rc, bufsize, 0); }
    UINT32 pad = 0; IAudioClient_GetCurrentPadding(ac, &pad);
    printf("prefill: %s, padding == buffer: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED", YN(pad == bufsize));

    double t0 = now();
    hr = IAudioClient_Start(ac); hr2 = IAudioClient_Start(ac);
    printf("Start: %s, again: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED", hr2 == AUDCLNT_E_NOT_STOPPED ? "NOT_STOPPED" : "?");
    int events = 0, pad_ok = 1, mono = 1; UINT64 last = 0, written = bufsize;
    for (int i = 0; i < 30; i++) {
        if (WaitForSingleObject(ev, 2000) != WAIT_OBJECT_0) break;
        events++;
        IAudioClient_GetCurrentPadding(ac, &pad);
        if (pad > bufsize) pad_ok = 0;
        UINT32 avail = bufsize - pad;
        if (avail && SUCCEEDED(IAudioRenderClient_GetBuffer(rc, avail, &data))) {
            sine((float *)data, avail, ch, rate);
            IAudioRenderClient_ReleaseBuffer(rc, avail, i == 10 ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
            written += avail;
        }
        IAudioClock_GetPosition(clk, &pos, &qpc);
        if (pos < last) mono = 0;
        last = pos;
    }
    double el = now() - t0;
    double played = (double)pos / freq;
    printf("event-driven: events=%d/30 padding<=buffer: %s clock monotonic: %s\n", events, YN(pad_ok), YN(mono));
    printf("clock advanced plausibly: %s, not beyond written data: %s\n", YN(played > el * 0.4 && played < el + 0.1),
           YN(pos <= written * mix->nBlockAlign));
    printf("Reset while running: %s\n", IAudioClient_Reset(ac) == AUDCLNT_E_NOT_STOPPED ? "NOT_STOPPED" : "?");
    HRESULT s1 = IAudioClient_Stop(ac), s2 = IAudioClient_Stop(ac);
    printf("Stop: %s, again: %s\n", s1 == S_OK ? "S_OK" : "?", s2 == S_FALSE ? "S_FALSE" : "?");
    hr = IAudioClient_Reset(ac);
    IAudioClient_GetCurrentPadding(ac, &pad); IAudioClock_GetPosition(clk, &pos, &qpc);
    printf("Reset: %s, padding=0: %s, position=0: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED", YN(pad == 0), YN(pos == 0));

    // ---- volumes on the stream / session ----
    IAudioStreamVolume *sv; UINT32 nch = 0; float lv = 0;
    hr = IAudioClient_GetService(ac, &IID_IAudioStreamVolume, (void **)&sv);
    IAudioStreamVolume_GetChannelCount(sv, &nch);
    IAudioStreamVolume_SetChannelVolume(sv, 0, 0.25f); IAudioStreamVolume_GetChannelVolume(sv, 0, &lv);
    printf("stream volume: %s channels=mix: %s set/get: %s out-of-range: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED", YN((int)nch == ch), YN(lv == 0.25f),
           YN(IAudioStreamVolume_SetChannelVolume(sv, nch, 0.5f) == E_INVALIDARG));
    IAudioStreamVolume_Release(sv);
    ISimpleAudioVolume *simple; hr = IAudioClient_GetService(ac, &IID_ISimpleAudioVolume, (void **)&simple);
    ISimpleAudioVolume_SetMasterVolume(simple, 0.5f, NULL); ISimpleAudioVolume_GetMasterVolume(simple, &lv);
    printf("session volume: %s set/get: %s >1 rejected: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED", YN(lv == 0.5f),
           YN(ISimpleAudioVolume_SetMasterVolume(simple, 1.5f, NULL) == E_INVALIDARG));
    ISimpleAudioVolume_SetMasterVolume(simple, 1.0f, NULL);
    ISimpleAudioVolume_Release(simple);
    IAudioSessionControl *sc; AudioSessionState sst = -1;
    hr = IAudioClient_GetService(ac, &IID_IAudioSessionControl, (void **)&sc);
    IAudioSessionControl_GetState(sc, &sst);
    printf("session control: %s state=inactive: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED", YN(sst == AudioSessionStateInactive));
    IAudioSessionControl_Release(sc);
    if (clk2) IAudioClock2_Release(clk2);
    IAudioClock_Release(clk); IAudioRenderClient_Release(rc); IAudioClient_Release(ac);

    // ---- polling stream, 100 ms buffer ----
    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, mix, NULL);
    IAudioClient_GetBufferSize(ac, &bufsize);
    printf("polling: Initialize %s, buffer>=100ms: %s, SetEventHandle: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED", YN(bufsize >= (UINT32)rate / 10),
           IAudioClient_SetEventHandle(ac, ev) == AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED ? "EVENTHANDLE_NOT_EXPECTED" : "?");
    IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc);
    IAudioRenderClient_GetBuffer(rc, bufsize, &data); sine((float *)data, bufsize, ch, rate); IAudioRenderClient_ReleaseBuffer(rc, bufsize, 0);
    IAudioClient_Start(ac);
    Sleep(60);
    IAudioClient_GetCurrentPadding(ac, &pad);
    printf("polling: padding drained while running: %s\n", YN(pad < bufsize));
    IAudioClient_Stop(ac);
    IAudioRenderClient_Release(rc); IAudioClient_Release(ac);

    // ---- converted format (AUTOCONVERTPCM): 16-bit 44.1 kHz stereo ----
    WAVEFORMATEX s16 = { WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0 };
    IAudioClient2 *ac2; hr = IMMDevice_Activate(dev, &IID_IAudioClient2, CLSCTX_ALL, NULL, (void **)&ac2);
    hr = IAudioClient2_Initialize(ac2, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                  0, 0, &s16, NULL);
    IAudioClient2_SetEventHandle(ac2, ev);
    IAudioClient2_GetBufferSize(ac2, &bufsize);
    IAudioClient2_GetService(ac2, &IID_IAudioRenderClient, (void **)&rc);
    IAudioClient2_Start(ac2);
    int ev2 = 0;
    for (int i = 0; i < 5; i++) {
        if (WaitForSingleObject(ev, 2000) != WAIT_OBJECT_0) break;
        ev2++;
        IAudioClient2_GetCurrentPadding(ac2, &pad);
        if (bufsize > pad && SUCCEEDED(IAudioRenderClient_GetBuffer(rc, bufsize - pad, &data))) {
            for (UINT32 k = 0; k < (bufsize - pad) * 2; k++) ((short *)data)[k] = (short)(3000 * sin(k * 0.03));
            IAudioRenderClient_ReleaseBuffer(rc, bufsize - pad, 0);
        }
    }
    IAudioClient2_Stop(ac2);
    printf("16-bit 44.1 kHz autoconvert: %s, events=%d/5\n", SUCCEEDED(hr) ? "ok" : "FAILED", ev2);
    IAudioRenderClient_Release(rc); IAudioClient2_Release(ac2);

    // ---- IAudioClient3 ----
    IAudioClient3 *ac3; hr = IMMDevice_Activate(dev, &IID_IAudioClient3, CLSCTX_ALL, NULL, (void **)&ac3);
    UINT32 pdef = 0, pfun = 0, pmin = 0, pmax = 0;
    if (SUCCEEDED(hr)) hr = IAudioClient3_GetSharedModeEnginePeriod(ac3, mix, &pdef, &pfun, &pmin, &pmax);
    printf("IAudioClient3 engine periods consistent: %s\n", YN(SUCCEEDED(hr) && pfun && pmin <= pdef && pdef <= pmax && pdef % pfun == 0));
    if (SUCCEEDED(hr)) {
        hr = IAudioClient3_InitializeSharedAudioStream(ac3, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, pdef, mix, NULL);
        printf("InitializeSharedAudioStream(default period): %s\n", SUCCEEDED(hr) ? "ok" : "FAILED");
        IAudioClient3_Release(ac3);
    }

    // ---- exclusive mode: accepted, or refused with a documented error ----
    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    hr = IAudioClient_IsFormatSupported(ac, AUDCLNT_SHAREMODE_EXCLUSIVE, mix, NULL);
    printf("exclusive IsFormatSupported: documented result: %s\n",
           YN(hr == S_OK || hr == AUDCLNT_E_UNSUPPORTED_FORMAT || hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED || hr == AUDCLNT_E_DEVICE_IN_USE));
    IAudioClient_Release(ac);

    // ---- endpoint volume + session manager ----
    IAudioEndpointVolume *epv; float mn = 0, mxv = 0, inc = 0, sc_ = -1;
    hr = IMMDevice_Activate(dev, &IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&epv);
    IAudioEndpointVolume_GetVolumeRange(epv, &mn, &mxv, &inc);
    IAudioEndpointVolume_GetMasterVolumeLevelScalar(epv, &sc_);
    printf("endpoint volume: %s range ok: %s scalar in [0,1]: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED", YN(mn < mxv && inc > 0), YN(sc_ >= 0 && sc_ <= 1));
    IAudioEndpointVolume_Release(epv);
    IAudioSessionManager2 *sm; hr = IMMDevice_Activate(dev, &IID_IAudioSessionManager2, CLSCTX_ALL, NULL, (void **)&sm);
    IAudioSessionEnumerator *se = NULL; int nsess = 0;
    if (SUCCEEDED(hr) && SUCCEEDED(IAudioSessionManager2_GetSessionEnumerator(sm, &se))) { IAudioSessionEnumerator_GetCount(se, &nsess); IAudioSessionEnumerator_Release(se); }
    printf("session manager: %s sessions>=1: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED", YN(nsess >= 1));
    IAudioSessionManager2_Release(sm);

    // ---- capture (no microphone needed: absent device or a stream that delivers packets) ----
    IMMDevice *cap = NULL; int cap_ok = 1;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eCapture, eCommunications, &cap);
    if (hr == S_OK) {
        IAudioClient *cc; WAVEFORMATEX *cmix;
        IMMDevice_Activate(cap, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&cc);
        IAudioClient_GetMixFormat(cc, &cmix);
        cap_ok = SUCCEEDED(IAudioClient_Initialize(cc, AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, cmix, NULL));
        IAudioCaptureClient *cpc = NULL; IAudioRenderClient *bad_rc = NULL;
        cap_ok &= IAudioClient_GetService(cc, &IID_IAudioRenderClient, (void **)&bad_rc) == AUDCLNT_E_WRONG_ENDPOINT_TYPE;
        cap_ok &= SUCCEEDED(IAudioClient_GetService(cc, &IID_IAudioCaptureClient, (void **)&cpc));
        IAudioClient_Start(cc);
        UINT32 got = 0; double tc = now();
        while (!got && now() - tc < 2.0) {
            UINT32 next = 0; IAudioCaptureClient_GetNextPacketSize(cpc, &next);
            if (!next) { Sleep(5); continue; }
            DWORD fl; UINT32 n; BYTE *d;
            if (IAudioCaptureClient_GetBuffer(cpc, &d, &n, &fl, NULL, NULL) == S_OK) { got = n; IAudioCaptureClient_ReleaseBuffer(cpc, n); }
        }
        cap_ok &= got > 0;
        IAudioClient_Stop(cc);
        IAudioCaptureClient_Release(cpc); IAudioClient_Release(cc); CoTaskMemFree(cmix); IMMDevice_Release(cap);
    } else cap_ok = hr == E_NOTFOUND;
    printf("capture: delivers packets or no device: %s\n", YN(cap_ok));

    // ---- ActivateAudioInterfaceAsync with the default render device interface ----
    done_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    IActivateAudioInterfaceAsyncOperation *op = NULL;
    hr = ActivateAudioInterfaceAsync(L"{E6327CAD-DCEC-4949-AE8A-991E976A79D2}", &IID_IAudioClient, NULL, &handler, &op);
    int fired = SUCCEEDED(hr) && WaitForSingleObject(done_ev, 2000) == WAIT_OBJECT_0;
    printf("ActivateAudioInterfaceAsync: %s, completed: %s, client: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED", YN(fired), YN(SUCCEEDED(async_hr) && async_iface));
    if (async_iface) IUnknown_Release(async_iface);
    if (op) IActivateAudioInterfaceAsyncOperation_Release(op);

    CoTaskMemFree(mix);
    IMMDevice_Release(dev); IMMDeviceEnumerator_Release(en);
    CoUninitialize();
    return 0;
}
