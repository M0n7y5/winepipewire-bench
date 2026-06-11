/* Single-stream 440 Hz tone player: functional + signal-fidelity check for
 * winepipewire.drv.  Exercises the render drain inside the per-(device, period)
 * group timer with one event-driven shared-mode stream.
 *
 * Plays a 0.25-amplitude sine; run.sh captures the default sink monitor with
 * parecord and verifies real signal (RMS ~= 0.177 = 0.25/sqrt(2)) at the
 * expected frequency with no harmonic distortion.  Prints the played frame
 * count and the number of WASAPI errors (must be 0).  See BASELINES.md.
 *
 * Args: [duration s = 3] [frequency Hz = 440].  Built by build.sh.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int _fltused = 0x9875;

int main(int argc, char **argv)
{
    int dur = argc > 1 ? atoi(argv[1]) : 3;
    double freq = argc > 2 ? atof(argv[2]) : 440.0;
    HRESULT hr;
    int errs = 0, ch, isFloat;
    UINT32 rate, buf;
    UINT64 frames = 0;
    double phase = 0;
    DWORD startms;
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *ac = NULL;
    IAudioRenderClient *rc = NULL;
    WAVEFORMATEX *wfx = NULL;
    HANDLE evt;
    BYTE *p = NULL;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr)) { printf("CoCreate %08lx\n", (unsigned long)hr); return 1; }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (FAILED(hr)) { printf("GetDefault %08lx\n", (unsigned long)hr); return 1; }
    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    if (FAILED(hr)) { printf("Activate %08lx\n", (unsigned long)hr); return 1; }
    hr = IAudioClient_GetMixFormat(ac, &wfx);
    if (FAILED(hr)) { printf("GetMixFormat %08lx\n", (unsigned long)hr); return 1; }
    ch = wfx->nChannels;
    rate = wfx->nSamplesPerSec;
    isFloat = (wfx->wBitsPerSample == 32);
    printf("format: %u ch, %lu Hz, %u bits (%s)\n", wfx->nChannels,
           (unsigned long)wfx->nSamplesPerSec, wfx->wBitsPerSample,
           isFloat ? "float32" : "int16");
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, wfx, NULL);
    if (FAILED(hr)) { printf("Initialize %08lx\n", (unsigned long)hr); return 1; }
    hr = IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc);
    if (FAILED(hr)) { printf("GetService %08lx\n", (unsigned long)hr); return 1; }
    evt = CreateEventW(NULL, FALSE, FALSE, NULL);
    IAudioClient_SetEventHandle(ac, evt);
    IAudioClient_GetBufferSize(ac, &buf);

    if (SUCCEEDED(IAudioRenderClient_GetBuffer(rc, buf, &p)) && p)
    {
        memset(p, 0, (size_t)buf * wfx->nBlockAlign);
        IAudioRenderClient_ReleaseBuffer(rc, buf, 0);
    }
    IAudioClient_Start(ac);
    startms = GetTickCount();

    while ((int)(GetTickCount() - startms) < dur * 1000)
    {
        UINT32 pad, avail, f;
        int c;

        if (WaitForSingleObject(evt, 1000) != WAIT_OBJECT_0) { errs++; continue; }
        if (FAILED(IAudioClient_GetCurrentPadding(ac, &pad))) { errs++; continue; }
        avail = buf > pad ? buf - pad : 0;
        if (!avail) continue;
        if (FAILED(IAudioRenderClient_GetBuffer(rc, avail, &p)) || !p) { errs++; continue; }
        for (f = 0; f < avail; f++)
        {
            double s = 0.25 * sin(phase);
            phase += 2.0 * 3.14159265358979 * freq / rate;
            if (phase > 6.283185307179586) phase -= 6.283185307179586;
            for (c = 0; c < ch; c++)
            {
                if (isFloat) ((float *)p)[f * ch + c] = (float)s;
                else ((short *)p)[f * ch + c] = (short)(s * 32767);
            }
        }
        IAudioRenderClient_ReleaseBuffer(rc, avail, 0);
        frames += avail;
    }
    IAudioClient_Stop(ac);
    printf("played %llu frames, errors=%d\n", (unsigned long long)frames, errs);
    return errs ? 2 : 0;
}
