// DirectSound 8: enumeration, primary buffer, secondary buffer Lock/Unlock (incl. wraparound), position
// notifications + stop event, volume/pan/frequency, real-time playback length, looping, duplicate buffers.
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <mmsystem.h>
#include <dsound.h>
#include <math.h>
#include <stdio.h>

static int devices;
static BOOL CALLBACK enum_cb(LPGUID g, LPCSTR desc, LPCSTR mod, LPVOID ctx) { (void)mod; (void)ctx; devices++; if (devices == 1) printf("first device: guid=%s desc=%s\n", g ? "set" : "NULL", desc); return TRUE; }
static double now(void) { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart / f.QuadPart; }

static double play_once(IDirectSoundBuffer8 *b, HANDLE mid, HANDLE stop, int *mid_hit, int *stopped) {
    ResetEvent(mid); ResetEvent(stop);
    IDirectSoundBuffer8_SetCurrentPosition(b, 0);
    double t0 = now();
    IDirectSoundBuffer8_Play(b, 0, 0, 0);
    *mid_hit = WaitForSingleObject(mid, 3000) == WAIT_OBJECT_0;
    *stopped = WaitForSingleObject(stop, 3000) == WAIT_OBJECT_0;
    return now() - t0;
}

int main(void) {
    DirectSoundEnumerateA(enum_cb, NULL);
    printf("devices enumerated: %d\n", devices);
    IDirectSound8 *ds; HRESULT hr = DirectSoundCreate8(NULL, &ds, NULL);
    printf("DirectSoundCreate8: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED"); if (FAILED(hr)) return 1;
    IDirectSound8_SetCooperativeLevel(ds, GetDesktopWindow(), DSSCL_PRIORITY);

    DSBUFFERDESC pd = { sizeof pd, DSBCAPS_PRIMARYBUFFER, 0, 0, NULL, { 0 } };
    IDirectSoundBuffer *prim; hr = IDirectSound8_CreateSoundBuffer(ds, &pd, &prim, NULL);
    WAVEFORMATEX pf = { WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0 }, got = { 0 }; DWORD w = 0;
    IDirectSoundBuffer_SetFormat(prim, &pf); IDirectSoundBuffer_GetFormat(prim, &got, sizeof got, &w);
    printf("primary: %s, format %lu Hz %u ch\n", SUCCEEDED(hr) ? "ok" : "FAILED", (unsigned long)got.nSamplesPerSec, got.nChannels);

    // 0.2 s of 16-bit mono at 22050 Hz
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0 };
    DSBUFFERDESC bd = { sizeof bd, DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY | DSBCAPS_GETCURRENTPOSITION2,
                        8820, 0, &wf, { 0 } };
    IDirectSoundBuffer *b1; hr = IDirectSound8_CreateSoundBuffer(ds, &bd, &b1, NULL);
    IDirectSoundBuffer8 *b; IDirectSoundBuffer_QueryInterface(b1, &IID_IDirectSoundBuffer8, (void **)&b); IDirectSoundBuffer_Release(b1);
    printf("secondary: %s\n", SUCCEEDED(hr) && b ? "ok" : "FAILED"); if (!b) return 1;

    void *p1, *p2; DWORD n1, n2;
    IDirectSoundBuffer8_Lock(b, 8000, 2000, &p1, &n1, &p2, &n2, 0);
    printf("wrapping lock: first=%lu second=%lu second_at_start=%s\n", (unsigned long)n1, (unsigned long)n2, p2 ? "yes" : "no");
    IDirectSoundBuffer8_Unlock(b, p1, n1, p2, n2);
    IDirectSoundBuffer8_Lock(b, 0, 0, &p1, &n1, &p2, &n2, DSBLOCK_ENTIREBUFFER);
    for (DWORD i = 0; i < n1 / 2; i++) ((short *)p1)[i] = (short)(6000 * sinf(6.2831853f * 660.0f * i / 22050));
    IDirectSoundBuffer8_Unlock(b, p1, n1, NULL, 0);

    IDirectSoundNotify *nt; hr = IDirectSoundBuffer8_QueryInterface(b, &IID_IDirectSoundNotify, (void **)&nt);
    HANDLE mid = CreateEventA(NULL, FALSE, FALSE, NULL), stop = CreateEventA(NULL, FALSE, FALSE, NULL);
    DSBPOSITIONNOTIFY pn[2] = { { 4410, mid }, { DSBPN_OFFSETSTOP, stop } };
    printf("notify interface: %s, set positions: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED", SUCCEEDED(IDirectSoundNotify_SetNotificationPositions(nt, 2, pn)) ? "ok" : "FAILED");

    LONG vol; IDirectSoundBuffer8_SetVolume(b, -600); IDirectSoundBuffer8_GetVolume(b, &vol);
    printf("volume=%ld, out-of-range rejected: %s\n", vol, IDirectSoundBuffer8_SetVolume(b, 5) == DSERR_INVALIDPARAM ? "yes" : "no");
    IDirectSoundBuffer8_SetPan(b, -2000);

    int m, s; double t1 = play_once(b, mid, stop, &m, &s);
    DWORD st, play, write; IDirectSoundBuffer8_GetStatus(b, &st); IDirectSoundBuffer8_GetCurrentPosition(b, &play, &write);
    printf("one-shot: midpoint=%s stopped=%s status_after=%lu play_cursor_after=%lu length_plausible=%s\n", m ? "yes" : "no", s ? "yes" : "no",
           (unsigned long)st, (unsigned long)play, t1 > 0.12 && t1 < 0.6 ? "yes" : "no");
    IDirectSoundBuffer8_SetFrequency(b, 44100);
    DWORD fq; IDirectSoundBuffer8_GetFrequency(b, &fq);
    double t2 = play_once(b, mid, stop, &m, &s);
    printf("double speed: freq=%lu stopped=%s faster=%s\n", (unsigned long)fq, s ? "yes" : "no", t2 < t1 * 0.75 ? "yes" : "no");
    IDirectSoundBuffer8_SetFrequency(b, DSBFREQUENCY_ORIGINAL);

    IDirectSoundBuffer8_Play(b, 0, 0, DSBPLAY_LOOPING);
    int loops = 0; double t0 = now();
    while (now() - t0 < 0.5) { if (WaitForSingleObject(mid, 600) == WAIT_OBJECT_0) loops++; }
    IDirectSoundBuffer8_GetStatus(b, &st);
    IDirectSoundBuffer8_Stop(b);
    printf("looping: status=%lu midpoint_hits>=2: %s stop_event_on_Stop: %s\n", (unsigned long)st, loops >= 2 ? "yes" : "no",
           WaitForSingleObject(stop, 1000) == WAIT_OBJECT_0 ? "yes" : "no");

    IDirectSoundBuffer *dup; hr = IDirectSound8_DuplicateSoundBuffer(ds, (IDirectSoundBuffer *)b, &dup);
    DWORD df = 0; if (SUCCEEDED(hr)) IDirectSoundBuffer_GetFrequency(dup, &df);
    printf("duplicate: %s freq=%lu\n", SUCCEEDED(hr) ? "ok" : "FAILED", (unsigned long)df);
    IDirectSound3DBuffer *b3d = NULL;
    printf("3D buffer interface: %s\n", IDirectSoundBuffer8_QueryInterface(b, &IID_IDirectSound3DBuffer, (void **)&b3d) == E_NOINTERFACE ? "not available" : "available");
    IDirectSoundBuffer_Release(dup); IDirectSoundNotify_Release(nt); IDirectSoundBuffer8_Release(b); IDirectSoundBuffer_Release(prim);
    IDirectSound8_Release(ds);
    return 0;
}
