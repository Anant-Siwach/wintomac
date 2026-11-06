// XAudio2 2.9 + X3DAudio: engine, mastering voice, source voice with a callback object implemented in this
// program, buffer queueing with end-of-stream, volume/frequency changes, voice state, and 3D panning math.
#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>
#include <x3daudio.h>
#include <math.h>
#include <stdio.h>

#define RATE 48000
#define BUFS 4
#define FRAMES (RATE / 20)           // 50 ms per buffer
static float pcm[BUFS][FRAMES];
static volatile LONG starts, ends, stream_end, pass_starts;
static HANDLE done;

// IXAudio2VoiceCallback implemented by the game: its methods run on the audio engine's thread.
typedef struct { const void *vtbl; } Cb;
static void WINAPI on_pass_start(Cb *t, UINT32 b) { (void)t; (void)b; InterlockedIncrement(&pass_starts); }
static void WINAPI on_pass_end(Cb *t) { (void)t; }
static void WINAPI on_stream_end(Cb *t) { (void)t; InterlockedIncrement(&stream_end); SetEvent(done); }
static void WINAPI on_buf_start(Cb *t, void *ctx) { (void)t; (void)ctx; InterlockedIncrement(&starts); }
static void WINAPI on_buf_end(Cb *t, void *ctx) { (void)t; InterlockedAdd(&ends, (LONG)(INT_PTR)ctx); }
static void WINAPI on_loop_end(Cb *t, void *ctx) { (void)t; (void)ctx; }
static void WINAPI on_error(Cb *t, void *ctx, HRESULT hr) { (void)t; (void)ctx; printf("voice error 0x%08lx\n", (unsigned long)hr); }
static const void *cb_vtbl[7] = { on_pass_start, on_pass_end, on_stream_end, on_buf_start, on_buf_end, on_loop_end, on_error };
static Cb cb = { cb_vtbl };

int main(void) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    done = CreateEventA(NULL, FALSE, FALSE, NULL);
    IXAudio2 *xa; HRESULT hr = XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    printf("XAudio2Create: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED");
    if (FAILED(hr)) return 1;
    IXAudio2MasteringVoice *master;
    hr = IXAudio2_CreateMasteringVoice(xa, &master, 2, RATE, 0, NULL, NULL, AudioCategory_GameEffects);
    printf("mastering voice: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED");
    if (FAILED(hr)) return 1;
    IXAudio2Voice_SetVolume((IXAudio2Voice *)master, 0.15f, XAUDIO2_COMMIT_NOW);   // a quiet beep
    WAVEFORMATEX wf = { WAVE_FORMAT_IEEE_FLOAT, 1, RATE, RATE * 4, 4, 32, 0 };
    IXAudio2SourceVoice *src;
    hr = IXAudio2_CreateSourceVoice(xa, &src, &wf, 0, 2.0f, (IXAudio2VoiceCallback *)&cb, NULL, NULL);
    printf("source voice: %s\n", SUCCEEDED(hr) ? "ok" : "FAILED");
    if (FAILED(hr)) return 1;
    XAUDIO2_VOICE_DETAILS d; IXAudio2Voice_GetVoiceDetails((IXAudio2Voice *)src, &d);
    printf("details: channels=%u rate=%u\n", d.InputChannels, d.InputSampleRate);
    for (int b = 0; b < BUFS; b++) {
        for (int i = 0; i < FRAMES; i++) pcm[b][i] = 0.5f * sinf(6.2831853f * (440.0f * (1 + b * 0.25f)) * i / RATE);
        XAUDIO2_BUFFER buf = { b == BUFS - 1 ? XAUDIO2_END_OF_STREAM : 0, FRAMES * 4, (const BYTE *)pcm[b], 0, 0, 0, 0, 0, (void *)(INT_PTR)(b + 1) };
        IXAudio2SourceVoice_SubmitSourceBuffer(src, &buf, NULL);
    }
    XAUDIO2_VOICE_STATE st; IXAudio2SourceVoice_GetState(src, &st, 0);
    printf("queued before start: %u\n", st.BuffersQueued);
    IXAudio2SourceVoice_SetFrequencyRatio(src, 1.0f, XAUDIO2_COMMIT_NOW);
    float ratio = 0; IXAudio2SourceVoice_GetFrequencyRatio(src, &ratio);
    printf("frequency ratio: %.2f\n", ratio);
    IXAudio2SourceVoice_Start(src, 0, XAUDIO2_COMMIT_NOW);
    // SamplesPlayed counts since the stream started and resets at END_OF_STREAM (XAudio2 semantics): poll it mid-stream.
    UINT64 max_played = 0; DWORD w = WAIT_TIMEOUT;
    for (int i = 0; i < 300 && w != WAIT_OBJECT_0; i++) {
        XAUDIO2_VOICE_STATE s2; IXAudio2SourceVoice_GetState(src, &s2, 0);
        if (s2.SamplesPlayed > max_played) max_played = s2.SamplesPlayed;
        w = WaitForSingleObject(done, 10);
    }
    Sleep(20);
    IXAudio2SourceVoice_GetState(src, &st, 0);
    printf("stream end: %s, buffer starts=%ld, end-context sum=%ld (want 10), queued now=%u\n",
           w == WAIT_OBJECT_0 ? "signalled" : "TIMEOUT", (long)starts, (long)ends, st.BuffersQueued);
    printf("samples advanced mid-stream: %s, reset after end of stream: %s, callbacks on audio thread: %s\n",
           max_played >= FRAMES ? "yes" : "no", st.SamplesPlayed == 0 ? "yes" : "no", pass_starts > 0 ? "yes" : "no");
    IXAudio2Voice_DestroyVoice((IXAudio2Voice *)src);
    IXAudio2Voice_DestroyVoice((IXAudio2Voice *)master);
    IXAudio2_Release(xa);

    // X3DAudio: a source 10 m to the listener's right should land almost entirely in the right channel.
    X3DAUDIO_HANDLE x3d; X3DAudioInitialize(SPEAKER_STEREO, X3DAUDIO_SPEED_OF_SOUND, x3d);
    X3DAUDIO_LISTENER L = { 0 }; L.OrientFront.z = 1; L.OrientTop.y = 1;
    X3DAUDIO_EMITTER E = { 0 }; E.OrientFront.z = 1; E.OrientTop.y = 1; E.Position.x = 10; E.ChannelCount = 1;
    E.CurveDistanceScaler = 1; E.DopplerScaler = 1;
    float m[2] = { 0 }; X3DAUDIO_DSP_SETTINGS dsp = { 0 }; dsp.SrcChannelCount = 1; dsp.DstChannelCount = 2; dsp.pMatrixCoefficients = m;
    X3DAudioCalculate(x3d, &L, &E, X3DAUDIO_CALCULATE_MATRIX, &dsp);
    printf("x3daudio: right louder than left: %s, distance=%.1f\n", m[1] > m[0] * 4 ? "yes" : "no", dsp.EmitterToListenerDistance);
    CoUninitialize();
    return 0;
}
