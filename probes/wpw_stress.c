/* Stress probe for winepipewire.drv: lifecycle churn, many-stream load, and
 * SetSampleRate churn.
 *
 * Three subcommands:
 *   churn [cycles = 300]     - full open/play-3-events/close cycle per
 *                              iteration; counts WASAPI errors and event
 *                              timeouts, and samples the process working set
 *                              after warmup and after the last cycle (leak
 *                              detector; run.sh gates ws growth).
 *   many [N = 24] [secs = 10] - N concurrent event-driven render streams,
 *                              one refill worker per stream; counts event
 *                              timeouts and refill errors (must both be 0).
 *   ratechurn [secs = 8]     - one RATEADJUST stream playing a continuous
 *                              440 Hz sine while IAudioClockAdjustment::
 *                              SetSampleRate alternates rate/2 and rate every
 *                              500 ms; the last >= 1.5 s plays at the original
 *                              rate so run.sh's monitor capture can verify the
 *                              tail frequency (a compounding-ratio regression
 *                              reads ~880 Hz instead of ~440).
 *
 * Exit codes: 0 pass, 1 setup failure, 2 runtime errors, 3 unsupported
 * (ratechurn only, SetSampleRate == E_NOTIMPL).  Built by build.sh.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <psapi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int _fltused = 0x9875;

#define MAXN 32

static IAudioClient *client[MAXN];
static IAudioRenderClient *rcl[MAXN];
static HANDLE evt[MAXN];
static UINT32 bufFrames[MAXN];
static UINT32 frameSize;
static volatile LONG stop_flag, timeouts, errors;

static const char *hrs(HRESULT hr){ static char b[32]; sprintf(b,"0x%08lx",(unsigned long)hr); return b; }

/* -------------------------------------------------------- churn ---------- */

static int churn_cycle(IMMDevice *dev)
{
    IAudioClient *ac = NULL;
    IAudioRenderClient *rend = NULL;
    WAVEFORMATEX *wfx = NULL;
    HANDLE e = NULL;
    BYTE *p = NULL;
    UINT32 buf = 0;
    HRESULT hr;
    int errs = 0, started = 0, k;

    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    if (FAILED(hr)) { errs++; goto done; }
    hr = IAudioClient_GetMixFormat(ac, &wfx);
    if (FAILED(hr)) { errs++; goto done; }
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, wfx, NULL);
    if (FAILED(hr)) { errs++; goto done; }
    hr = IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rend);
    if (FAILED(hr)) { errs++; goto done; }
    e = CreateEventW(NULL, FALSE, FALSE, NULL);
    hr = IAudioClient_SetEventHandle(ac, e);
    if (FAILED(hr)) { errs++; goto done; }
    IAudioClient_GetBufferSize(ac, &buf);

    hr = IAudioRenderClient_GetBuffer(rend, buf, &p);
    if (FAILED(hr) || !p) { errs++; goto done; }
    memset(p, 0, (size_t)buf * wfx->nBlockAlign);
    IAudioRenderClient_ReleaseBuffer(rend, buf, 0);

    hr = IAudioClient_Start(ac);
    if (FAILED(hr)) { errs++; goto done; }
    started = 1;

    for (k = 0; k < 3; k++)
    {
        UINT32 pad, avail;

        if (WaitForSingleObject(e, 2000) != WAIT_OBJECT_0) { errs++; continue; }
        hr = IAudioClient_GetCurrentPadding(ac, &pad);
        if (FAILED(hr)) { errs++; break; }
        avail = buf > pad ? buf - pad : 0;
        if (!avail) continue;
        hr = IAudioRenderClient_GetBuffer(rend, avail, &p);
        if (FAILED(hr) || !p) { errs++; break; }
        memset(p, 0, (size_t)avail * wfx->nBlockAlign);
        IAudioRenderClient_ReleaseBuffer(rend, avail, 0);
    }

done:
    if (started) IAudioClient_Stop(ac);
    if (rend) IAudioRenderClient_Release(rend);
    if (ac) IAudioClient_Release(ac);
    if (wfx) CoTaskMemFree(wfx);
    if (e) CloseHandle(e);
    return errs;
}

static int do_churn(IMMDevice *dev, int cycles)
{
    PROCESS_MEMORY_COUNTERS pmc;
    unsigned long ws_warmup = 0, ws_final = 0;
    int cyc, errs = 0, warmup_idx;

    if (cycles < 1) cycles = 1;
    warmup_idx = cycles < 50 ? 0 : 49;

    for (cyc = 0; cyc < cycles; cyc++)
    {
        errs += churn_cycle(dev);
        if (cyc == warmup_idx && GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
            ws_warmup = (unsigned long)(pmc.WorkingSetSize / 1024);
    }
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        ws_final = (unsigned long)(pmc.WorkingSetSize / 1024);

    printf("cycles=%d errors=%d ws_warmup_kb=%lu ws_final_kb=%lu\n",
           cycles, errs, ws_warmup, ws_final);
    return errs ? 2 : 0;
}

/* -------------------------------------------------------- many ----------- */

static DWORD WINAPI many_worker(void *arg)
{
    int i = (int)(INT_PTR)arg;
    for (;;)
    {
        DWORD w = WaitForSingleObject(evt[i], 2000);
        UINT32 pad, avail;
        BYTE *p = NULL;

        if (stop_flag) break;
        if (w != WAIT_OBJECT_0) { InterlockedIncrement(&timeouts); continue; }

        /* refill so the stream keeps running */
        if (FAILED(IAudioClient_GetCurrentPadding(client[i], &pad))) { InterlockedIncrement(&errors); continue; }
        avail = bufFrames[i] > pad ? bufFrames[i] - pad : 0;
        if (!avail) continue;
        if (FAILED(IAudioRenderClient_GetBuffer(rcl[i], avail, &p)) || !p) { InterlockedIncrement(&errors); continue; }
        memset(p, 0, (size_t)avail * frameSize);
        IAudioRenderClient_ReleaseBuffer(rcl[i], avail, 0);
    }
    return 0;
}

static int do_many(IMMDevice *dev, int N, int secs)
{
    HRESULT hr;
    int i;

    if (N < 1) N = 1;
    if (N > MAXN) N = MAXN;
    if (secs < 1) secs = 1;

    for (i = 0; i < N; i++)
    {
        WAVEFORMATEX *wfx = NULL;

        hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client[i]);
        if (FAILED(hr)) { printf("Activate[%d] %s\n", i, hrs(hr)); return 1; }
        hr = IAudioClient_GetMixFormat(client[i], &wfx);
        if (FAILED(hr)) { printf("GetMixFormat[%d] %s\n", i, hrs(hr)); return 1; }
        if (i == 0) frameSize = wfx->nBlockAlign;

        hr = IAudioClient_Initialize(client[i], AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, wfx, NULL);
        CoTaskMemFree(wfx);
        if (FAILED(hr)) { printf("Initialize[%d] %s\n", i, hrs(hr)); return 1; }

        hr = IAudioClient_GetService(client[i], &IID_IAudioRenderClient, (void **)&rcl[i]);
        if (FAILED(hr)) { printf("GetService[%d] %s\n", i, hrs(hr)); return 1; }

        evt[i] = CreateEventW(NULL, FALSE, FALSE, NULL);
        hr = IAudioClient_SetEventHandle(client[i], evt[i]);
        if (FAILED(hr)) { printf("SetEventHandle[%d] %s\n", i, hrs(hr)); return 1; }

        IAudioClient_GetBufferSize(client[i], &bufFrames[i]);
    }

    for (i = 0; i < N; i++)
    {
        BYTE *p = NULL;
        if (SUCCEEDED(IAudioRenderClient_GetBuffer(rcl[i], bufFrames[i], &p)) && p)
        {
            memset(p, 0, (size_t)bufFrames[i] * frameSize);
            IAudioRenderClient_ReleaseBuffer(rcl[i], bufFrames[i], 0);
        }
    }

    for (i = 0; i < N; i++)
        CreateThread(NULL, 0, many_worker, (void *)(INT_PTR)i, 0, NULL);
    for (i = 0; i < N; i++)
        IAudioClient_Start(client[i]);

    Sleep(secs * 1000);
    stop_flag = 1;
    Sleep(100);
    for (i = 0; i < N; i++)
        IAudioClient_Stop(client[i]);

    printf("streams=%d timeouts=%ld errors=%ld\n", N, (long)timeouts, (long)errors);
    return (timeouts || errors) ? 2 : 0;
}

/* -------------------------------------------------------- ratechurn ------ */

static int do_ratechurn(IMMDevice *dev, int secs)
{
    HRESULT hr;
    IAudioClient *ac = NULL;
    IAudioRenderClient *rend = NULL;
    IAudioClockAdjustment *aca = NULL;
    WAVEFORMATEX *wfx = NULL;
    HANDLE e;
    BYTE *p = NULL;
    UINT32 rate, buf;
    int ch, isFloat;
    int setrate_calls = 0, errs = 0, unsupported = 0;
    int at_orig = 1, tail_fixed = 0;
    DWORD start, next_toggle = 500, cutoff_ms;
    double phase = 0;

    if (secs < 2) secs = 2;

    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    if (FAILED(hr)) { printf("Activate %s\n", hrs(hr)); return 1; }
    hr = IAudioClient_GetMixFormat(ac, &wfx);
    if (FAILED(hr)) { printf("GetMixFormat %s\n", hrs(hr)); return 1; }
    ch = wfx->nChannels;
    rate = wfx->nSamplesPerSec;
    isFloat = (wfx->wBitsPerSample == 32);

    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_RATEADJUST,
                                 5000000, 0, wfx, NULL);
    if (FAILED(hr)) { printf("Initialize %s\n", hrs(hr)); return 1; }
    hr = IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rend);
    if (FAILED(hr)) { printf("GetService(RenderClient) %s\n", hrs(hr)); return 1; }
    hr = IAudioClient_GetService(ac, &IID_IAudioClockAdjustment, (void **)&aca);
    if (FAILED(hr)) { printf("GetService(ClockAdjustment) %s\n", hrs(hr)); return 1; }

    e = CreateEventW(NULL, FALSE, FALSE, NULL);
    hr = IAudioClient_SetEventHandle(ac, e);
    if (FAILED(hr)) { printf("SetEventHandle %s\n", hrs(hr)); return 1; }
    IAudioClient_GetBufferSize(ac, &buf);

    if (SUCCEEDED(IAudioRenderClient_GetBuffer(rend, buf, &p)) && p)
    {
        memset(p, 0, (size_t)buf * wfx->nBlockAlign);
        IAudioRenderClient_ReleaseBuffer(rend, buf, 0);
    }
    hr = IAudioClient_Start(ac);
    if (FAILED(hr)) { printf("Start %s\n", hrs(hr)); return 1; }

    cutoff_ms = (DWORD)(secs * 1000 - 1500);
    start = GetTickCount();

    for (;;)
    {
        DWORD elapsed = GetTickCount() - start;
        UINT32 pad, avail, f;
        int c;

        if ((int)elapsed >= secs * 1000) break;

        if (elapsed < cutoff_ms)
        {
            if (elapsed >= next_toggle)
            {
                float target = at_orig ? (float)rate / 2.0f : (float)rate;
                hr = IAudioClockAdjustment_SetSampleRate(aca, target);
                setrate_calls++;
                if (hr == E_NOTIMPL) { unsupported = 1; break; }
                if (FAILED(hr)) errs++;
                else at_orig = !at_orig;
                next_toggle += 500;
            }
        }
        else if (!tail_fixed)
        {
            tail_fixed = 1;
            if (!at_orig)
            {
                hr = IAudioClockAdjustment_SetSampleRate(aca, (float)rate);
                setrate_calls++;
                if (hr == E_NOTIMPL) { unsupported = 1; break; }
                if (FAILED(hr)) errs++;
                else at_orig = 1;
            }
        }

        if (WaitForSingleObject(e, 1000) != WAIT_OBJECT_0) { errs++; continue; }
        if (FAILED(IAudioClient_GetCurrentPadding(ac, &pad))) { errs++; continue; }
        avail = buf > pad ? buf - pad : 0;
        if (!avail) continue;
        if (FAILED(IAudioRenderClient_GetBuffer(rend, avail, &p)) || !p) { errs++; continue; }
        for (f = 0; f < avail; f++)
        {
            /* phase increment from the ORIGINAL rate: the nominal stream
             * format never changes, SetSampleRate only changes the
             * consumption rate (the effect under test) */
            double s = 0.25 * sin(phase);
            phase += 2.0 * 3.14159265358979 * 440.0 / rate;
            if (phase > 6.283185307179586) phase -= 6.283185307179586;
            for (c = 0; c < ch; c++)
            {
                if (isFloat) ((float *)p)[f * ch + c] = (float)s;
                else ((short *)p)[f * ch + c] = (short)(s * 32767);
            }
        }
        IAudioRenderClient_ReleaseBuffer(rend, avail, 0);
    }

    IAudioClient_Stop(ac);
    IAudioClockAdjustment_Release(aca);
    IAudioRenderClient_Release(rend);
    IAudioClient_Release(ac);
    CoTaskMemFree(wfx);
    CloseHandle(e);

    if (unsupported) { printf("setrate unsupported\n"); return 3; }
    printf("setrate_calls=%d errors=%d\n", setrate_calls, errs);
    return errs ? 2 : 0;
}

/* -------------------------------------------------------- main ----------- */

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : NULL;
    HRESULT hr;
    IMMDeviceEnumerator *enumr = NULL;
    IMMDevice *dev = NULL;

    if (!mode || (strcmp(mode, "churn") && strcmp(mode, "many") && strcmp(mode, "ratechurn")))
    {
        printf("usage: wpw_stress <churn [cycles=300] | many [N=24] [secs=10] | ratechurn [secs=8]>\n");
        return 1;
    }

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&enumr);
    if (FAILED(hr)) { printf("CoCreateInstance %s\n", hrs(hr)); return 1; }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumr, eRender, eConsole, &dev);
    if (FAILED(hr)) { printf("GetDefaultAudioEndpoint %s\n", hrs(hr)); return 1; }

    if (!strcmp(mode, "churn"))
        return do_churn(dev, argc > 2 ? atoi(argv[2]) : 300);
    if (!strcmp(mode, "many"))
        return do_many(dev, argc > 2 ? atoi(argv[2]) : 24, argc > 3 ? atoi(argv[3]) : 10);
    return do_ratechurn(dev, argc > 2 ? atoi(argv[2]) : 8);
}
