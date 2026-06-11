/* Loopback-capture integrity probe for winepipewire.drv.
 *
 * Renders a 0.25-amplitude sine on the default render endpoint (own thread,
 * wpw_tone-style event-driven stream) while the main thread opens a second
 * IAudioClient on the SAME endpoint with AUDCLNT_STREAMFLAGS_LOOPBACK and
 * polls IAudioCaptureClient.  Verifies the loopback actually returns the
 * sink's signal: a driver that wires loopback to the microphone instead of
 * the sink monitor (missing PW_KEY_STREAM_CAPTURE_SINK) fails the Goertzel
 * tone_ratio gate; dropouts show up as DATA_DISCONTINUITY counts.
 *
 * Prints: cap_samples=N cap_rms=X tone_ratio=Y discont=N errors=N
 * (RMS of the captured tail should be ~0.177 = 0.25/sqrt(2), tone_ratio is
 * the fraction of captured power in the rendered frequency bin.)
 *
 * Args: [duration s = 4] [frequency Hz = 440].  Built by build.sh.
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

static volatile LONG stop;
static volatile LONG rerrs;
static double g_freq = 440.0;

static const char *hrs(HRESULT hr){ static char b[32]; sprintf(b,"0x%08lx",(unsigned long)hr); return b; }

static DWORD WINAPI render_main(void *arg)
{
    HRESULT hr;
    int ch, isFloat;
    UINT32 rate, buf;
    double phase = 0;
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *ac = NULL;
    IAudioRenderClient *rc = NULL;
    WAVEFORMATEX *wfx = NULL;
    HANDLE evt;
    BYTE *p = NULL;

    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr)) { printf("render CoCreate %s\n", hrs(hr)); InterlockedIncrement(&rerrs); return 1; }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (FAILED(hr)) { printf("render GetDefault %s\n", hrs(hr)); InterlockedIncrement(&rerrs); return 1; }
    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    if (FAILED(hr)) { printf("render Activate %s\n", hrs(hr)); InterlockedIncrement(&rerrs); return 1; }
    hr = IAudioClient_GetMixFormat(ac, &wfx);
    if (FAILED(hr)) { printf("render GetMixFormat %s\n", hrs(hr)); InterlockedIncrement(&rerrs); return 1; }
    ch = wfx->nChannels;
    rate = wfx->nSamplesPerSec;
    isFloat = (wfx->wBitsPerSample == 32);
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, wfx, NULL);
    if (FAILED(hr)) { printf("render Initialize %s\n", hrs(hr)); InterlockedIncrement(&rerrs); return 1; }
    hr = IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc);
    if (FAILED(hr)) { printf("render GetService %s\n", hrs(hr)); InterlockedIncrement(&rerrs); return 1; }
    evt = CreateEventW(NULL, FALSE, FALSE, NULL);
    IAudioClient_SetEventHandle(ac, evt);
    IAudioClient_GetBufferSize(ac, &buf);

    if (SUCCEEDED(IAudioRenderClient_GetBuffer(rc, buf, &p)) && p)
    {
        memset(p, 0, (size_t)buf * wfx->nBlockAlign);
        IAudioRenderClient_ReleaseBuffer(rc, buf, 0);
    }
    IAudioClient_Start(ac);

    while (!stop)
    {
        UINT32 pad, avail, f;
        int c;

        if (WaitForSingleObject(evt, 1000) != WAIT_OBJECT_0) { InterlockedIncrement(&rerrs); continue; }
        if (FAILED(IAudioClient_GetCurrentPadding(ac, &pad))) { InterlockedIncrement(&rerrs); continue; }
        avail = buf > pad ? buf - pad : 0;
        if (!avail) continue;
        if (FAILED(IAudioRenderClient_GetBuffer(rc, avail, &p)) || !p) { InterlockedIncrement(&rerrs); continue; }
        for (f = 0; f < avail; f++)
        {
            double s = 0.25 * sin(phase);
            phase += 2.0 * 3.14159265358979 * g_freq / rate;
            if (phase > 6.283185307179586) phase -= 6.283185307179586;
            for (c = 0; c < ch; c++)
            {
                if (isFloat) ((float *)p)[f * ch + c] = (float)s;
                else ((short *)p)[f * ch + c] = (short)(s * 32767);
            }
        }
        IAudioRenderClient_ReleaseBuffer(rc, avail, 0);
    }
    IAudioClient_Stop(ac);
    IAudioRenderClient_Release(rc);
    IAudioClient_Release(ac);
    CloseHandle(evt);
    CoTaskMemFree(wfx);
    IMMDevice_Release(dev);
    IMMDeviceEnumerator_Release(en);
    return 0;
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 4;
    HRESULT hr;
    int ch, isFloat;
    UINT32 rate;
    int cerrs = 0, discont = 0, npkt = 0, count = 0, cap;
    int a0, n;
    double cap_rms = 0, tone_ratio = 0;
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *ac = NULL;
    IAudioCaptureClient *cc = NULL;
    WAVEFORMATEX *wfx = NULL;
    float *mono;
    HANDLE rt;
    DWORD startms;

    if (secs < 2) secs = 2;
    if (secs > 30) secs = 30;
    g_freq = argc > 2 ? atof(argv[2]) : 440.0;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr)) { printf("CoCreate %s\n", hrs(hr)); return 1; }
    /* loopback capture opens the RENDER endpoint */
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (FAILED(hr)) { printf("GetDefault %s\n", hrs(hr)); return 1; }
    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    if (FAILED(hr)) { printf("Activate %s\n", hrs(hr)); return 1; }
    hr = IAudioClient_GetMixFormat(ac, &wfx);
    if (FAILED(hr)) { printf("GetMixFormat %s\n", hrs(hr)); return 1; }
    ch = wfx->nChannels;
    rate = wfx->nSamplesPerSec;
    isFloat = (wfx->wBitsPerSample == 32);
    printf("format: %u ch, %lu Hz, %u bits (%s)\n", wfx->nChannels,
           (unsigned long)wfx->nSamplesPerSec, wfx->wBitsPerSample,
           isFloat ? "float32" : "int16");
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK, 5000000, 0, wfx, NULL);
    if (FAILED(hr)) { printf("Initialize %s\n", hrs(hr)); return 1; }
    hr = IAudioClient_GetService(ac, &IID_IAudioCaptureClient, (void **)&cc);
    if (FAILED(hr)) { printf("GetService %s\n", hrs(hr)); return 1; }

    cap = secs * (int)rate;
    mono = malloc(sizeof(float) * cap);
    if (!mono) { printf("malloc failed\n"); return 1; }

    IAudioClient_Start(ac);
    rt = CreateThread(NULL, 0, render_main, NULL, 0, NULL);
    startms = GetTickCount();

    while ((int)(GetTickCount() - startms) < secs * 1000)
    {
        UINT32 pkt;

        Sleep(5);
        while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(cc, &pkt)) && pkt)
        {
            BYTE *data;
            UINT32 frames, f;
            DWORD flags;
            int c;

            if (FAILED(IAudioCaptureClient_GetBuffer(cc, &data, &frames, &flags, NULL, NULL))) { cerrs++; break; }
            if (npkt > 0 && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)) discont++;
            npkt++;
            for (f = 0; f < frames && count < cap; f++)
            {
                double sum = 0;
                for (c = 0; c < ch; c++)
                {
                    if (isFloat) sum += ((float *)data)[f * ch + c];
                    else sum += ((short *)data)[f * ch + c] / 32768.0f;
                }
                mono[count++] = (float)(sum / ch);
            }
            IAudioCaptureClient_ReleaseBuffer(cc, frames);
        }
    }
    stop = 1;
    WaitForSingleObject(rt, 3000);
    IAudioClient_Stop(ac);

    /* analyze the tail: skip the first second (startup transient) */
    a0 = (int)rate;
    n = count - a0;
    if (n < (int)rate / 2)
    {
        cerrs++;
    }
    else
    {
        double total_ms = 0, tone_ms, power;
        double coeff = 2.0 * cos(2.0 * 3.14159265358979 * g_freq / rate);
        double s0, s1 = 0, s2 = 0;
        int k;

        for (k = 0; k < n; k++)
        {
            double x = mono[a0 + k];
            total_ms += x * x;
            s0 = x + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        total_ms /= n;
        cap_rms = sqrt(total_ms);
        power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        tone_ms = 2.0 * power / ((double)n * n);
        tone_ratio = total_ms > 0 ? tone_ms / total_ms : 0;
    }

    printf("cap_samples=%d cap_rms=%.4f tone_ratio=%.3f discont=%d errors=%d\n",
           count, cap_rms, tone_ratio, discont, cerrs + (int)rerrs);
    return (cerrs + rerrs) ? 2 : 0;
}
