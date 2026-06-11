/* Sub-period event-phase probe for winepipewire.drv multi-stream sync.
 *
 * Opens N shared-mode EVENT-DRIVEN render streams on the default endpoint.
 * One worker thread per stream blocks on that stream's event and timestamps
 * (QPC) every firing.  The inter-stream spread of those timestamps is the true
 * phase (de)synchronization that IAudioClock GetPosition's period quantization
 * hides.
 *
 * With the shared per-(device, period) group timer ("winepipewire.drv:
 * Synchronize streams sharing a device and period."), all N same-format
 * streams are serviced and signalled in one tick, so the spread collapses to
 * scheduler-wake jitter (~microseconds, sd < 0.5 ms).  A regression to
 * per-stream timers shows the old ms-scale scatter (sd ~1.8 ms, range +/-5 ms).
 * See BASELINES.md.
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
#define MAXEV 200000

static IAudioClient *client[MAXN];
static IAudioRenderClient *rc[MAXN];
static HANDLE evt[MAXN];
static UINT32 bufFrames[MAXN];
static UINT32 frameSize;
static LARGE_INTEGER qf, t0;
static volatile LONG stop;

static double *ts[MAXN];      /* event timestamps, seconds since t0 */
static volatile LONG nev[MAXN];

static const char *hrs(HRESULT hr){ static char b[32]; sprintf(b,"0x%08lx",(unsigned long)hr); return b; }

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static DWORD WINAPI worker(void *arg)
{
    int i = (int)(INT_PTR)arg;
    for (;;)
    {
        DWORD w = WaitForSingleObject(evt[i], 2000);
        LARGE_INTEGER t;
        LONG n;
        UINT32 pad;

        if (stop) break;
        if (w != WAIT_OBJECT_0) continue;

        QueryPerformanceCounter(&t);
        n = nev[i];
        if (n < MAXEV) { ts[i][n] = (double)(t.QuadPart - t0.QuadPart) / qf.QuadPart; nev[i] = n + 1; }

        /* refill so the stream keeps running */
        if (SUCCEEDED(IAudioClient_GetCurrentPadding(client[i], &pad)))
        {
            UINT32 avail = bufFrames[i] > pad ? bufFrames[i] - pad : 0;
            BYTE *p = NULL;
            if (avail && SUCCEEDED(IAudioRenderClient_GetBuffer(rc[i], avail, &p)) && p)
            {
                memset(p, 0, (size_t)avail * frameSize);
                IAudioRenderClient_ReleaseBuffer(rc[i], avail, 0);
            }
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    int N   = argc > 1 ? atoi(argv[1]) : 2;
    int dur = argc > 2 ? atoi(argv[2]) : 30;
    HRESULT hr;
    IMMDeviceEnumerator *enumr = NULL;
    IMMDevice *dev = NULL;
    int i, k;

    if (N < 2) N = 2;
    if (N > MAXN) N = MAXN;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
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
        if (i == 0) frameSize = wfx->nBlockAlign;

        hr = IAudioClient_Initialize(client[i], AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, wfx, NULL);
        CoTaskMemFree(wfx);
        if (FAILED(hr)) { printf("Initialize[%d] %s\n", i, hrs(hr)); return 1; }

        hr = IAudioClient_GetService(client[i], &IID_IAudioRenderClient, (void **)&rc[i]);
        if (FAILED(hr)) { printf("GetService[%d] %s\n", i, hrs(hr)); return 1; }

        evt[i] = CreateEventW(NULL, FALSE, FALSE, NULL);
        hr = IAudioClient_SetEventHandle(client[i], evt[i]);
        if (FAILED(hr)) { printf("SetEventHandle[%d] %s\n", i, hrs(hr)); return 1; }

        IAudioClient_GetBufferSize(client[i], &bufFrames[i]);
        ts[i] = malloc(sizeof(double) * MAXEV);
    }
    printf("Opened %d event-driven streams, frameSize=%u, buf[0]=%u frames\n", N, frameSize, bufFrames[0]);

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
    QueryPerformanceCounter(&t0);
    for (i = 0; i < N; i++)
        CreateThread(NULL, 0, worker, (void *)(INT_PTR)i, 0, NULL);
    for (i = 0; i < N; i++)
        IAudioClient_Start(client[i]);

    printf("measuring event phase for %d s ...\n", dur);
    fflush(stdout);
    Sleep(dur * 1000);
    stop = 1;
    Sleep(50);

    /* analysis: align stream i events to stream 0 by index, report phase offset */
    printf("\n=== EVENT-PHASE SUMMARY  N=%d  dur=%ds ===\n", N, dur);
    for (i = 0; i < N; i++)
    {
        int n = nev[i];
        double per = n > 1 ? (ts[i][n - 1] - ts[i][0]) / (n - 1) * 1000.0 : 0;
        printf("stream%d: %d events, mean inter-event period = %.3f ms\n", i, n, per);
    }
    for (i = 1; i < N; i++)
    {
        int n = nev[i] < nev[0] ? nev[i] : nev[0];
        double sum = 0, sumsq = 0, mn = 1e9, mx = -1e9;
        int c = 0;
        /* skip first 100 events (startup transient) */
        for (k = 100; k < n; k++)
        {
            double d = (ts[i][k] - ts[0][k]) * 1000.0; /* ms */
            sum += d; sumsq += d * d;
            if (d < mn) mn = d;
            if (d > mx) mx = d;
            c++;
        }
        if (c > 0)
        {
            double mean = sum / c;
            double var = sumsq / c - mean * mean;
            double sd = var > 0 ? __builtin_sqrt(var) : 0;
            printf("stream%d vs stream0 phase: mean=%+.3f ms  sd=%.3f ms  min=%+.3f ms  max=%+.3f ms  (n=%d)\n",
                   i, mean, sd, mn, mx, c);
        }
    }

    /* per-stream wakeup-interval jitter: deltas between consecutive events,
     * same 100-event startup skip; worst stream's sd/p99/max in one line */
    {
        double wsd = 0, wp99 = 0, wmax = 0;
        for (i = 0; i < N; i++)
        {
            int n = nev[i];
            int c = n - 101;
            double *d, sum = 0, sumsq = 0, mean, var, sd, p99, mx;
            if (c < 2) continue;
            d = malloc(sizeof(double) * c);
            for (k = 0; k < c; k++)
            {
                d[k] = (ts[i][k + 101] - ts[i][k + 100]) * 1000.0; /* ms */
                sum += d[k]; sumsq += d[k] * d[k];
            }
            qsort(d, c, sizeof(double), cmp_double);
            mean = sum / c;
            var = sumsq / c - mean * mean;
            sd = var > 0 ? __builtin_sqrt(var) : 0;
            p99 = d[(int)(0.99 * (c - 1))];
            mx = d[c - 1];
            if (sd > wsd) wsd = sd;
            if (p99 > wp99) wp99 = p99;
            if (mx > wmax) wmax = mx;
            free(d);
        }
        printf("evt_interval worst: sd=%.3f ms p99=%.3f ms max=%.3f ms\n", wsd, wp99, wmax);
    }
    return 0;
}
