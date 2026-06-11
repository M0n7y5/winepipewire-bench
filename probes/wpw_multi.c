/* Multi-stream WASAPI drift/overhead probe for winepipewire.drv.
 *
 * Opens N shared-mode render streams on the default endpoint, keeps them all
 * topped up with silence (polled, no event), and samples each stream's
 * IAudioClock position over time.  For winepipewire.drv, shared-mode
 * GetPosition = clock_written - held_bytes, and held_bytes is drained by the
 * timer thread, so the inter-stream position delta exposes any divergence
 * between the streams' service cadence.
 *
 * With the shared per-(device, period) group timer ("winepipewire.drv:
 * Synchronize streams sharing a device and period."), all same-format streams
 * are drained in the same tick, so the inter-stream drift stays well under one
 * period (it was already rate-locked by the PipeWire graph clock pre-sync; the
 * group timer additionally removes per-tick phase skew).  See BASELINES.md.
 *
 * Args: [N streams = 2] [duration s = 30].  Built by build.sh.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <stdlib.h>

int _fltused = 0x9875;

#define MAXN 16

static const char *hrs(HRESULT hr)
{
    static char b[32];
    sprintf(b, "0x%08lx", (unsigned long)hr);
    return b;
}

int main(int argc, char **argv)
{
    int N   = argc > 1 ? atoi(argv[1]) : 2;
    int dur = argc > 2 ? atoi(argv[2]) : 30;
    HRESULT hr;
    IMMDeviceEnumerator *enumr = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *client[MAXN] = {0};
    IAudioRenderClient *rc[MAXN] = {0};
    IAudioClock *clk[MAXN] = {0};
    UINT32 bufFrames[MAXN] = {0};
    UINT32 frameSize = 0;
    REFERENCE_TIME bufdur = 2000000; /* 200 ms */
    LARGE_INTEGER qf, t0, t;
    double maxdrift = 0;
    UINT64 nextSample = 0, nextPrint = 0;
    double pos_sec[MAXN];
    double prev_pos[MAXN] = {0};
    int nonmono = 0;
    int i;

    if (N < 1) N = 1;
    if (N > MAXN) N = MAXN;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { printf("CoInitializeEx %s\n", hrs(hr)); return 1; }

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&enumr);
    if (FAILED(hr)) { printf("CoCreateInstance %s\n", hrs(hr)); return 1; }

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumr, eRender, eConsole, &dev);
    if (FAILED(hr)) { printf("GetDefaultAudioEndpoint %s\n", hrs(hr)); return 1; }

    for (i = 0; i < N; i++)
    {
        WAVEFORMATEX *wfx = NULL;

        hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client[i]);
        if (FAILED(hr)) { printf("Activate[%d] %s\n", i, hrs(hr)); return 1; }

        hr = IAudioClient_GetMixFormat(client[i], &wfx);
        if (FAILED(hr)) { printf("GetMixFormat[%d] %s\n", i, hrs(hr)); return 1; }
        if (i == 0)
        {
            frameSize = wfx->nBlockAlign;
            printf("MixFormat: tag=%u ch=%u rate=%lu bits=%u block=%u\n",
                   wfx->wFormatTag, wfx->nChannels, (unsigned long)wfx->nSamplesPerSec,
                   wfx->wBitsPerSample, wfx->nBlockAlign);
        }

        hr = IAudioClient_Initialize(client[i], AUDCLNT_SHAREMODE_SHARED, 0, bufdur, 0, wfx, NULL);
        CoTaskMemFree(wfx);
        if (FAILED(hr)) { printf("Initialize[%d] %s\n", i, hrs(hr)); return 1; }

        hr = IAudioClient_GetService(client[i], &IID_IAudioRenderClient, (void **)&rc[i]);
        if (FAILED(hr)) { printf("GetService RC[%d] %s\n", i, hrs(hr)); return 1; }
        hr = IAudioClient_GetService(client[i], &IID_IAudioClock, (void **)&clk[i]);
        if (FAILED(hr)) { printf("GetService Clock[%d] %s\n", i, hrs(hr)); return 1; }

        IAudioClient_GetBufferSize(client[i], &bufFrames[i]);
    }
    printf("Opened %d streams, frameSize=%u, buf[0]=%u frames\n", N, frameSize, bufFrames[0]);

    /* prefill every buffer with silence so none underruns at start */
    for (i = 0; i < N; i++)
    {
        BYTE *p = NULL;
        if (SUCCEEDED(IAudioRenderClient_GetBuffer(rc[i], bufFrames[i], &p)) && p)
        {
            memset(p, 0, (size_t)bufFrames[i] * frameSize);
            IAudioRenderClient_ReleaseBuffer(rc[i], bufFrames[i], 0);
        }
    }

    QueryPerformanceFrequency(&qf);
    for (i = 0; i < N; i++)
    {
        hr = IAudioClient_Start(client[i]);
        if (FAILED(hr)) printf("Start[%d] %s\n", i, hrs(hr));
    }
    QueryPerformanceCounter(&t0);
    printf("started %d streams; measuring %d s ...\n", N, dur);
    fflush(stdout);

    for (;;)
    {
        double elapsed;
        UINT64 ems;

        QueryPerformanceCounter(&t);
        elapsed = (double)(t.QuadPart - t0.QuadPart) / qf.QuadPart;
        if (elapsed >= dur) break;

        /* keep all streams topped up */
        for (i = 0; i < N; i++)
        {
            UINT32 pad = 0;
            if (SUCCEEDED(IAudioClient_GetCurrentPadding(client[i], &pad)))
            {
                UINT32 avail = bufFrames[i] > pad ? bufFrames[i] - pad : 0;
                if (avail)
                {
                    BYTE *p = NULL;
                    if (SUCCEEDED(IAudioRenderClient_GetBuffer(rc[i], avail, &p)) && p)
                    {
                        memset(p, 0, (size_t)avail * frameSize);
                        IAudioRenderClient_ReleaseBuffer(rc[i], avail, 0);
                    }
                }
            }
        }

        ems = (UINT64)(elapsed * 1000);
        if (ems >= nextSample)
        {
            double dmax = 0;
            nextSample = ems + 250;
            for (i = 0; i < N; i++)
            {
                UINT64 pos = 0, fr = 0;
                IAudioClock_GetFrequency(clk[i], &fr);
                IAudioClock_GetPosition(clk[i], &pos, NULL);
                pos_sec[i] = fr ? (double)pos / (double)fr : 0;
                if (pos_sec[i] < prev_pos[i]) nonmono++;
                prev_pos[i] = pos_sec[i];
            }
            for (i = 1; i < N; i++)
            {
                double d = pos_sec[i] - pos_sec[0];
                if (d < 0) d = -d;
                if (d > dmax) dmax = d;
            }
            if (dmax > maxdrift) maxdrift = dmax;
            if (ems >= nextPrint)
            {
                nextPrint = ems + 1000;
                printf("t=%6.2fs  pos0=%8.4f s  inter-stream drift=%+9.1f us  pos0-wall=%+9.1f us\n",
                       elapsed, pos_sec[0], dmax * 1e6, (pos_sec[0] - elapsed) * 1e6);
                fflush(stdout);
            }
        }
        Sleep(3);
    }

    QueryPerformanceCounter(&t);
    {
        double elapsed = (double)(t.QuadPart - t0.QuadPart) / qf.QuadPart;
        printf("\n=== SUMMARY  N=%d  dur=%ds ===\n", N, dur);
        for (i = 0; i < N; i++)
        {
            UINT64 pos = 0, fr = 0;
            double ps;
            IAudioClock_GetFrequency(clk[i], &fr);
            IAudioClock_GetPosition(clk[i], &pos, NULL);
            ps = fr ? (double)pos / (double)fr : 0;
            printf("stream%d  final pos=%.5f s   pos-wall=%+8.1f us  (%+7.1f ppm)\n",
                   i, ps, (ps - elapsed) * 1e6, elapsed > 0 ? (ps - elapsed) / elapsed * 1e6 : 0);
        }
        printf("MAX inter-stream drift over run = %.1f us\n", maxdrift * 1e6);
        printf("pos_nonmono=%d\n", nonmono);
    }

    for (i = 0; i < N; i++)
        IAudioClient_Stop(client[i]);
    return 0;
}
