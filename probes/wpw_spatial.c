/* ISpatialAudioClient dynamic-object placement probe.
 *
 * Activates an ISpatialAudioObjectRenderStream on the default render
 * endpoint, drives one DYNAMIC audio object with a 3 kHz / 0.25-amplitude
 * sine (pump thread, event-driven 10 ms update windows), and places it hard
 * left (-1,0,0) for the first half of the run, hard right (+1,0,0) for the
 * second.  The main thread loopback-captures the same endpoint and computes
 * the interaural level difference (Goertzel at the tone frequency, L vs R)
 * for each half.
 *
 * Pass gates: first half ILD >= +6 dB, second half ILD <= -6 dB.  The
 * panning mixer yields very large magnitudes; the +-6 dB gate leaves
 * headroom for an HRTF backend, whose head-shadow ILD at 3 kHz is smaller.
 *
 * Requires spatial sound enabled: HKCU\Software\Wine\mmdevapi,
 * SpatialSound=1.  When the platform reports no dynamic objects the probe
 * prints spatial=off and exits 77 (skip).
 *
 * Prints: ild_left=X ild_right=Y discont=N errors=N
 * Args: [duration s = 4] [frequency Hz = 3000].  Built by build.sh.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <spatialaudioclient.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int _fltused = 0x9875;

static volatile LONG stop;
static volatile LONG perrs;
static double g_freq = 3000.0;
static int g_secs = 4;
static ISpatialAudioClient *g_sac;

static const char *hrs(HRESULT hr){ static char b[32]; sprintf(b,"0x%08lx",(unsigned long)hr); return b; }

static DWORD WINAPI pump_main(void *arg)
{
    ISpatialAudioObjectRenderStream *sas = NULL;
    ISpatialAudioObject *sao = NULL;
    SpatialAudioObjectRenderStreamActivationParams params;
    PROPVARIANT prop;
    IAudioFormatEnumerator *afe = NULL;
    WAVEFORMATEX *fmt = NULL;
    HANDLE evt;
    HRESULT hr;
    DWORD startms;
    double phase = 0;
    int side = 0;

    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    hr = ISpatialAudioClient_GetSupportedAudioObjectFormatEnumerator(g_sac, &afe);
    if (FAILED(hr)) { printf("GetFormatEnumerator %s\n", hrs(hr)); InterlockedIncrement(&perrs); return 1; }
    hr = IAudioFormatEnumerator_GetFormat(afe, 0, &fmt);
    if (FAILED(hr)) { printf("GetFormat %s\n", hrs(hr)); InterlockedIncrement(&perrs); return 1; }

    evt = CreateEventW(NULL, FALSE, FALSE, NULL);

    memset(&params, 0, sizeof(params));
    params.ObjectFormat = fmt;
    params.StaticObjectTypeMask = AudioObjectType_FrontLeft | AudioObjectType_FrontRight;
    params.MinDynamicObjectCount = 1;
    params.MaxDynamicObjectCount = 2;
    params.Category = AudioCategory_GameEffects;
    params.EventHandle = evt;
    params.NotifyObject = NULL;

    PropVariantInit(&prop);
    prop.vt = VT_BLOB;
    prop.blob.cbSize = sizeof(params);
    prop.blob.pBlobData = (BYTE *)&params;

    hr = ISpatialAudioClient_ActivateSpatialAudioStream(g_sac, &prop,
            &IID_ISpatialAudioObjectRenderStream, (void **)&sas);
    if (FAILED(hr)) { printf("ActivateStream %s\n", hrs(hr)); InterlockedIncrement(&perrs); return 1; }

    hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas,
            AudioObjectType_Dynamic, &sao);
    if (FAILED(hr)) { printf("ActivateObject %s\n", hrs(hr)); InterlockedIncrement(&perrs); return 1; }
    ISpatialAudioObject_SetPosition(sao, -1.0f, 0.0f, 0.0f);

    hr = ISpatialAudioObjectRenderStream_Start(sas);
    if (FAILED(hr)) { printf("Start %s\n", hrs(hr)); InterlockedIncrement(&perrs); return 1; }

    startms = GetTickCount();
    while (!stop)
    {
        UINT32 dyn, frames, f, len;
        BYTE *buf;
        int elapsed = (int)(GetTickCount() - startms);

        if (WaitForSingleObject(evt, 1000) != WAIT_OBJECT_0) { InterlockedIncrement(&perrs); continue; }

        if (!side && elapsed >= g_secs * 500)
        {
            side = 1;
            ISpatialAudioObject_SetPosition(sao, 1.0f, 0.0f, 0.0f);
        }

        hr = ISpatialAudioObjectRenderStream_BeginUpdatingAudioObjects(sas, &dyn, &frames);
        if (FAILED(hr)) { InterlockedIncrement(&perrs); continue; }
        hr = ISpatialAudioObject_GetBuffer(sao, &buf, &len);
        if (SUCCEEDED(hr) && buf)
        {
            for (f = 0; f < frames; f++)
            {
                ((float *)buf)[f] = (float)(0.25 * sin(phase));
                phase += 2.0 * 3.14159265358979 * g_freq / fmt->nSamplesPerSec;
                if (phase > 6.283185307179586) phase -= 6.283185307179586;
            }
        }
        else InterlockedIncrement(&perrs);
        hr = ISpatialAudioObjectRenderStream_EndUpdatingAudioObjects(sas);
        if (FAILED(hr)) InterlockedIncrement(&perrs);
    }

    ISpatialAudioObjectRenderStream_Stop(sas);
    ISpatialAudioObject_Release(sao);
    ISpatialAudioObjectRenderStream_Release(sas);
    IAudioFormatEnumerator_Release(afe);
    CloseHandle(evt);
    return 0;
}

/* Goertzel power at g_freq over buf[a..b) */
static double goertzel(const float *buf, int a, int b, double rate)
{
    double coeff = 2.0 * cos(2.0 * 3.14159265358979 * g_freq / rate);
    double s0, s1 = 0, s2 = 0;
    int k, n = b - a;

    if (n <= 0) return 0;
    for (k = a; k < b; k++)
    {
        s0 = buf[k] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return 2.0 * (s1 * s1 + s2 * s2 - coeff * s1 * s2) / ((double)n * n);
}

static double ild_db(const float *l, const float *r, int a, int b, double rate)
{
    double pl = goertzel(l, a, b, rate);
    double pr = goertzel(r, a, b, rate);
    if (pr <= 1e-12) return pl > 1e-12 ? 99.0 : 0.0;
    if (pl <= 1e-12) return -99.0;
    return 10.0 * log10(pl / pr);
}

int main(int argc, char **argv)
{
    HRESULT hr;
    UINT32 rate, maxdyn = 0;
    int ch, isFloat;
    int cerrs = 0, discont = 0, npkt = 0, count = 0, cap;
    int half, a1, b1, a2, b2;
    double ild1, ild2;
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *ac = NULL;
    IAudioCaptureClient *cc = NULL;
    WAVEFORMATEX *wfx = NULL;
    float *capL, *capR;
    HANDLE pt;
    DWORD startms;

    g_secs = argc > 1 ? atoi(argv[1]) : 4;
    if (g_secs < 2) g_secs = 2;
    if (g_secs > 30) g_secs = 30;
    g_freq = argc > 2 ? atof(argv[2]) : 3000.0;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr)) { printf("CoCreate %s\n", hrs(hr)); return 1; }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (FAILED(hr)) { printf("GetDefault %s\n", hrs(hr)); return 1; }

    hr = IMMDevice_Activate(dev, &IID_ISpatialAudioClient, CLSCTX_ALL, NULL, (void **)&g_sac);
    if (FAILED(hr)) { printf("ISpatialAudioClient %s\n", hrs(hr)); return 1; }
    hr = ISpatialAudioClient_GetMaxDynamicObjectCount(g_sac, &maxdyn);
    if (FAILED(hr)) { printf("GetMaxDynamicObjectCount %s\n", hrs(hr)); return 1; }
    if (!maxdyn) { printf("spatial=off\n"); return 77; }
    printf("max_dynamic_objects=%u\n", maxdyn);

    /* loopback capture on the same endpoint */
    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    if (FAILED(hr)) { printf("Activate %s\n", hrs(hr)); return 1; }
    hr = IAudioClient_GetMixFormat(ac, &wfx);
    if (FAILED(hr)) { printf("GetMixFormat %s\n", hrs(hr)); return 1; }
    ch = wfx->nChannels;
    rate = wfx->nSamplesPerSec;
    isFloat = (wfx->wBitsPerSample == 32);
    if (ch < 2) { printf("mono endpoint, cannot measure ILD\n"); return 77; }
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK, 5000000, 0, wfx, NULL);
    if (FAILED(hr)) { printf("Initialize %s\n", hrs(hr)); return 1; }
    hr = IAudioClient_GetService(ac, &IID_IAudioCaptureClient, (void **)&cc);
    if (FAILED(hr)) { printf("GetService %s\n", hrs(hr)); return 1; }

    cap = g_secs * (int)rate;
    capL = malloc(sizeof(float) * cap);
    capR = malloc(sizeof(float) * cap);
    if (!capL || !capR) { printf("malloc failed\n"); return 1; }

    IAudioClient_Start(ac);
    pt = CreateThread(NULL, 0, pump_main, NULL, 0, NULL);
    startms = GetTickCount();

    while ((int)(GetTickCount() - startms) < g_secs * 1000)
    {
        UINT32 pkt;

        Sleep(5);
        while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(cc, &pkt)) && pkt)
        {
            BYTE *data;
            UINT32 frames, f;
            DWORD flags;

            if (FAILED(IAudioCaptureClient_GetBuffer(cc, &data, &frames, &flags, NULL, NULL))) { cerrs++; break; }
            if (npkt > 0 && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)) discont++;
            npkt++;
            for (f = 0; f < frames && count < cap; f++)
            {
                if (isFloat)
                {
                    capL[count] = ((float *)data)[f * ch];
                    capR[count] = ((float *)data)[f * ch + 1];
                }
                else
                {
                    capL[count] = ((short *)data)[f * ch] / 32768.0f;
                    capR[count] = ((short *)data)[f * ch + 1] / 32768.0f;
                }
                count++;
            }
            IAudioCaptureClient_ReleaseBuffer(cc, frames);
        }
    }
    stop = 1;
    WaitForSingleObject(pt, 3000);
    IAudioClient_Stop(ac);

    /* halves with 0.5 s guard bands around start and the position flip */
    half = count / 2;
    a1 = (int)rate / 2;        b1 = half - (int)rate / 4;
    a2 = half + (int)rate / 2; b2 = count - (int)rate / 8;
    if (b1 - a1 < (int)rate / 4 || b2 - a2 < (int)rate / 4)
    {
        printf("ild_left=0.0 ild_right=0.0 discont=%d errors=%d\n", discont, cerrs + (int)perrs + 1);
        return 2;
    }
    ild1 = ild_db(capL, capR, a1, b1, rate);
    ild2 = ild_db(capL, capR, a2, b2, rate);

    printf("ild_left=%.1f ild_right=%.1f discont=%d errors=%d\n",
           ild1, ild2, discont, cerrs + (int)perrs);
    if (cerrs + perrs) return 2;
    if (!(ild1 >= 6.0 && ild2 <= -6.0)) return 2;
    return 0;
}
