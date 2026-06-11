/* Stream-open / first-event / device-enumeration latency probe for
 * winepipewire.drv.
 *
 * Phase A repeatedly performs a full open sequence on the default render
 * endpoint (Activate -> GetMixFormat -> Initialize event-driven shared ->
 * SetEventHandle -> GetService -> prefill silence -> Start), timing
 * Activate-through-Start (t_open) and Start-to-first-event (t_firstevt)
 * with QPC.  Phase B times EnumAudioEndpoints + GetCount (t_enum).
 *
 * Prints p50/p99/max percentiles as one machine-parseable key=value line
 * consumed by run.sh.  Any FAILED hr aborts with the call name and exit 1.
 *
 * Args: [iters = 50].  Built by build.sh.
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

static const char *hrs(HRESULT hr){ static char b[32]; sprintf(b,"0x%08lx",(unsigned long)hr); return b; }

static int cmp_ull(const void *a, const void *b)
{
    ULONGLONG x = *(const ULONGLONG *)a, y = *(const ULONGLONG *)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 50;
    HRESULT hr;
    IMMDeviceEnumerator *enumr = NULL;
    IMMDevice *dev = NULL;
    LARGE_INTEGER qf;
    ULONGLONG *t_open, *t_firstevt, *t_enum;
    int i;

    if (iters < 1) iters = 1;
    if (iters > 1000) iters = 1000;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&enumr);
    if (FAILED(hr)) { printf("CoCreateInstance %s\n", hrs(hr)); return 1; }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumr, eRender, eConsole, &dev);
    if (FAILED(hr)) { printf("GetDefaultAudioEndpoint %s\n", hrs(hr)); return 1; }

    QueryPerformanceFrequency(&qf);
    t_open     = malloc(sizeof(ULONGLONG) * iters);
    t_firstevt = malloc(sizeof(ULONGLONG) * iters);
    t_enum     = malloc(sizeof(ULONGLONG) * iters);

    /* Phase A: full open sequence + first event */
    for (i = 0; i < iters; i++)
    {
        IAudioClient *ac = NULL;
        IAudioRenderClient *rc = NULL;
        WAVEFORMATEX *wfx = NULL;
        HANDLE evt;
        BYTE *p = NULL;
        UINT32 buf;
        LARGE_INTEGER t0, t1, t2;

        QueryPerformanceCounter(&t0);
        hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
        if (FAILED(hr)) { printf("Activate[%d] %s\n", i, hrs(hr)); return 1; }
        hr = IAudioClient_GetMixFormat(ac, &wfx);
        if (FAILED(hr)) { printf("GetMixFormat[%d] %s\n", i, hrs(hr)); return 1; }
        hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, wfx, NULL);
        if (FAILED(hr)) { printf("Initialize[%d] %s\n", i, hrs(hr)); return 1; }
        evt = CreateEventW(NULL, FALSE, FALSE, NULL);
        hr = IAudioClient_SetEventHandle(ac, evt);
        if (FAILED(hr)) { printf("SetEventHandle[%d] %s\n", i, hrs(hr)); return 1; }
        hr = IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc);
        if (FAILED(hr)) { printf("GetService[%d] %s\n", i, hrs(hr)); return 1; }
        hr = IAudioClient_GetBufferSize(ac, &buf);
        if (FAILED(hr)) { printf("GetBufferSize[%d] %s\n", i, hrs(hr)); return 1; }

        if (SUCCEEDED(IAudioRenderClient_GetBuffer(rc, buf, &p)) && p)
        {
            memset(p, 0, (size_t)buf * wfx->nBlockAlign);
            IAudioRenderClient_ReleaseBuffer(rc, buf, 0);
        }
        hr = IAudioClient_Start(ac);
        if (FAILED(hr)) { printf("Start[%d] %s\n", i, hrs(hr)); return 1; }
        QueryPerformanceCounter(&t1);
        t_open[i] = (ULONGLONG)((t1.QuadPart - t0.QuadPart) * 1000000 / qf.QuadPart);

        /* record even on timeout: ~1 s value blows the gate upstream */
        WaitForSingleObject(evt, 1000);
        QueryPerformanceCounter(&t2);
        t_firstevt[i] = (ULONGLONG)((t2.QuadPart - t1.QuadPart) * 1000000 / qf.QuadPart);

        IAudioClient_Stop(ac);
        IAudioRenderClient_Release(rc);
        IAudioClient_Release(ac);
        CoTaskMemFree(wfx);
        CloseHandle(evt);
        Sleep(50);
    }

    /* Phase B: endpoint enumeration */
    for (i = 0; i < iters; i++)
    {
        IMMDeviceCollection *col = NULL;
        UINT cnt;
        LARGE_INTEGER t0, t1;

        QueryPerformanceCounter(&t0);
        hr = IMMDeviceEnumerator_EnumAudioEndpoints(enumr, eRender, DEVICE_STATE_ACTIVE, &col);
        if (FAILED(hr)) { printf("EnumAudioEndpoints[%d] %s\n", i, hrs(hr)); return 1; }
        hr = IMMDeviceCollection_GetCount(col, &cnt);
        if (FAILED(hr)) { printf("GetCount[%d] %s\n", i, hrs(hr)); return 1; }
        IMMDeviceCollection_Release(col);
        QueryPerformanceCounter(&t1);
        t_enum[i] = (ULONGLONG)((t1.QuadPart - t0.QuadPart) * 1000000 / qf.QuadPart);
    }

    qsort(t_open, iters, sizeof(ULONGLONG), cmp_ull);
    qsort(t_firstevt, iters, sizeof(ULONGLONG), cmp_ull);
    qsort(t_enum, iters, sizeof(ULONGLONG), cmp_ull);

    printf("open/firstevt/enum over %d iters:\n", iters);
    printf("open_p50_us=%llu open_p99_us=%llu open_max_us=%llu "
           "firstevt_p50_us=%llu firstevt_p99_us=%llu "
           "enum_p50_us=%llu enum_p99_us=%llu\n",
           t_open[(iters - 1) / 2], t_open[(size_t)(0.99 * (iters - 1))], t_open[iters - 1],
           t_firstevt[(iters - 1) / 2], t_firstevt[(size_t)(0.99 * (iters - 1))],
           t_enum[(iters - 1) / 2], t_enum[(size_t)(0.99 * (iters - 1))]);
    return 0;
}
