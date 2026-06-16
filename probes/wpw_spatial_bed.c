/* ISpatialAudioClient static-bed HRTF virtualization probe.
 *
 * Activates an ISpatialAudioObjectRenderStream with a 7.1.4 static bed and
 * ZERO dynamic objects (the channel-bed model used by "Windows Sonic for
 * Headphones" titles such as Shadow of the Tomb Raider).  It drives two
 * static bed objects, SideLeft and SideRight, with a 3 kHz / 0.25-amplitude
 * sine (pump thread, event-driven 10 ms update windows): the tone is fed to
 * SideLeft for the first half of the run and to SideRight for the second,
 * the inactive channel carrying silence.  The main thread loopback-captures
 * the same endpoint and computes the interaural level difference (Goertzel
 * at the tone frequency, L vs R) for each half.
 *
 * This exercises bed HRTF virtualization: with the spatial toggle on and a
 * stereo endpoint the driver renders each bed channel through Steam Audio at
 * its canonical speaker position and sums to stereo.  True HRTF leaves real
 * energy in the contralateral ear, so the measured ILD lands at a moderate
 * magnitude (|ILD| < 90 dB, the +-99 dB sentinels mean one ear was silent,
 * i.e. the panning fallback engaged because libphonon was not loaded).
 *
 * Pass gates: first half (SideLeft) ILD in [+3, +90) dB, second half
 * (SideRight) ILD in (-90, -3] dB.  Requires spatial sound enabled
 * (WINE_SPATIAL_SOUND=1) AND WINE_SPATIAL_PHONON pointing at libphonon;
 * without the HRTF library the bed pans and the probe fails by design.  When
 * the platform reports no dynamic-object budget the probe prints spatial=off
 * and exits 77 (skip).
 *
 * Prints: bed_ild_left=X bed_ild_right=Y discont=N errors=N
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
#include <devpkey.h>
#include <string.h>

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
    ISpatialAudioObject *saoL = NULL, *saoR = NULL;
    SpatialAudioObjectRenderStreamActivationParams params;
    PROPVARIANT prop;
    IAudioFormatEnumerator *afe = NULL;
    WAVEFORMATEX *fmt = NULL;
    HANDLE evt;
    HRESULT hr;
    DWORD startms;
    double phase = 0;

    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    hr = ISpatialAudioClient_GetSupportedAudioObjectFormatEnumerator(g_sac, &afe);
    if (FAILED(hr)) { printf("GetFormatEnumerator %s\n", hrs(hr)); InterlockedIncrement(&perrs); return 1; }
    hr = IAudioFormatEnumerator_GetFormat(afe, 0, &fmt);
    if (FAILED(hr)) { printf("GetFormat %s\n", hrs(hr)); InterlockedIncrement(&perrs); return 1; }

    evt = CreateEventW(NULL, FALSE, FALSE, NULL);

    memset(&params, 0, sizeof(params));
    params.ObjectFormat = fmt;
    params.StaticObjectTypeMask = AudioObjectType_FrontLeft | AudioObjectType_FrontRight |
            AudioObjectType_FrontCenter | AudioObjectType_LowFrequency |
            AudioObjectType_SideLeft | AudioObjectType_SideRight |
            AudioObjectType_BackLeft | AudioObjectType_BackRight |
            AudioObjectType_TopFrontLeft | AudioObjectType_TopFrontRight |
            AudioObjectType_TopBackLeft | AudioObjectType_TopBackRight;
    params.MinDynamicObjectCount = 0;
    params.MaxDynamicObjectCount = 0;
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
            AudioObjectType_SideLeft, &saoL);
    if (FAILED(hr)) { printf("ActivateObject SL %s\n", hrs(hr)); InterlockedIncrement(&perrs); return 1; }
    hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas,
            AudioObjectType_SideRight, &saoR);
    if (FAILED(hr)) { printf("ActivateObject SR %s\n", hrs(hr)); InterlockedIncrement(&perrs); return 1; }

    hr = ISpatialAudioObjectRenderStream_Start(sas);
    if (FAILED(hr)) { printf("Start %s\n", hrs(hr)); InterlockedIncrement(&perrs); return 1; }

    startms = GetTickCount();
    while (!stop)
    {
        UINT32 dyn, frames, f, len;
        BYTE *bufL = NULL, *bufR = NULL;
        int elapsed = (int)(GetTickCount() - startms);
        int left_active = elapsed < g_secs * 500;

        if (WaitForSingleObject(evt, 1000) != WAIT_OBJECT_0) { InterlockedIncrement(&perrs); continue; }

        hr = ISpatialAudioObjectRenderStream_BeginUpdatingAudioObjects(sas, &dyn, &frames);
        if (FAILED(hr)) { InterlockedIncrement(&perrs); continue; }

        /* both objects must be fetched every cycle or they are invalidated */
        hr = ISpatialAudioObject_GetBuffer(saoL, &bufL, &len);
        if (FAILED(hr) || !bufL) { InterlockedIncrement(&perrs); bufL = NULL; }
        hr = ISpatialAudioObject_GetBuffer(saoR, &bufR, &len);
        if (FAILED(hr) || !bufR) { InterlockedIncrement(&perrs); bufR = NULL; }

        for (f = 0; f < frames; f++)
        {
            float s = (float)(0.25 * sin(phase));
            phase += 2.0 * 3.14159265358979 * g_freq / fmt->nSamplesPerSec;
            if (phase > 6.283185307179586) phase -= 6.283185307179586;
            if (bufL) ((float *)bufL)[f] = left_active ? s : 0.0f;
            if (bufR) ((float *)bufR)[f] = left_active ? 0.0f : s;
        }

        hr = ISpatialAudioObjectRenderStream_EndUpdatingAudioObjects(sas);
        if (FAILED(hr)) InterlockedIncrement(&perrs);
    }

    ISpatialAudioObjectRenderStream_Stop(sas);
    ISpatialAudioObject_Release(saoL);
    ISpatialAudioObject_Release(saoR);
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

/* Optional render-endpoint selection by friendly-name substring (env WPW_SINK),
 * so the probe can target a null sink without touching the system default. */
static IMMDevice *pick_device(IMMDeviceEnumerator *en, const char *want)
{
    IMMDeviceCollection *col = NULL;
    IMMDevice *d = NULL, *found = NULL;
    UINT n = 0, i;
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col)))
        return NULL;
    IMMDeviceCollection_GetCount(col, &n);
    for (i = 0; i < n; i++) {
        IPropertyStore *ps = NULL;
        PROPVARIANT pv;
        char name[256] = "";
        if (FAILED(IMMDeviceCollection_Item(col, i, &d))) continue;
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps))) {
            PropVariantInit(&pv);
            if (SUCCEEDED(IPropertyStore_GetValue(ps, (const PROPERTYKEY *)&DEVPKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, name, sizeof(name), NULL, NULL);
            PropVariantClear(&pv);
            IPropertyStore_Release(ps);
        }
        printf("  endpoint[%u]: %s\n", i, name);
        if (!found && want && strstr(name, want)) { found = d; continue; }
        IMMDevice_Release(d);
    }
    IMMDeviceCollection_Release(col);
    return found;
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
    {
        const char *want = getenv("WPW_SINK");
        if (want && want[0]) {
            dev = pick_device(en, want);
            if (!dev) { printf("sink '%s' not found\n", want); return 1; }
        } else {
            hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
            if (FAILED(hr)) { printf("GetDefault %s\n", hrs(hr)); return 1; }
        }
    }

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

    /* halves with 0.5 s guard bands around start and the channel flip */
    half = count / 2;
    a1 = (int)rate / 2;        b1 = half - (int)rate / 4;
    a2 = half + (int)rate / 2; b2 = count - (int)rate / 8;
    if (b1 - a1 < (int)rate / 4 || b2 - a2 < (int)rate / 4)
    {
        printf("bed_ild_left=0.0 bed_ild_right=0.0 discont=%d errors=%d\n", discont, cerrs + (int)perrs + 1);
        return 2;
    }
    ild1 = ild_db(capL, capR, a1, b1, rate);
    ild2 = ild_db(capL, capR, a2, b2, rate);

    printf("bed_ild_left=%.1f bed_ild_right=%.1f discont=%d errors=%d\n",
           ild1, ild2, discont, cerrs + (int)perrs);
    if (cerrs + perrs) return 2;
    if (!(ild1 >= 3.0 && ild1 < 90.0 && ild2 <= -3.0 && ild2 > -90.0)) return 2;
    return 0;
}
