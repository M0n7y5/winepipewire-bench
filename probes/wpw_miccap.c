/* Microphone-capture integrity probe for winepipewire.drv.
 *
 * Opens the DEFAULT CAPTURE endpoint (plain eCapture, no LOOPBACK flag) and
 * polls IAudioCaptureClient for N seconds, then runs a Goertzel filter at the
 * given frequency over the tail.  Pair it with a deterministic source, e.g. a
 * remap-source on a null sink fed with a 0.25-amplitude tone:
 *
 *   pactl load-module module-null-sink sink_name=wpw_vmic_sink
 *   pactl load-module module-remap-source master=wpw_vmic_sink.monitor source_name=wpw_vmic
 *   pactl set-default-source wpw_vmic
 *   <play 0.25-amp sine into wpw_vmic_sink> ; wine wpw_miccap.exe 6 440
 *
 * Expected: cap_rms ~0.177 (= 0.25/sqrt(2)), tone_ratio ~1.0.  A driver that
 * mis-routes the capture stream (wrong target node, dead ring) fails the
 * tone_ratio gate; dropouts show up as DATA_DISCONTINUITY counts.
 *
 * Prints: cap_samples=N cap_rms=X tone_ratio=Y discont=N errors=N
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

static const char *hrs(HRESULT hr){ static char b[32]; sprintf(b,"0x%08lx",(unsigned long)hr); return b; }

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 4;
    double freq = argc > 2 ? atof(argv[2]) : 440.0;
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
    DWORD startms;

    if (secs < 2) secs = 2;
    if (secs > 30) secs = 30;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr)) { printf("CoCreate %s\n", hrs(hr)); return 1; }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eCapture, eConsole, &dev);
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
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, 0, 5000000, 0, wfx, NULL);
    if (FAILED(hr)) { printf("Initialize %s\n", hrs(hr)); return 1; }
    hr = IAudioClient_GetService(ac, &IID_IAudioCaptureClient, (void **)&cc);
    if (FAILED(hr)) { printf("GetService %s\n", hrs(hr)); return 1; }

    cap = secs * (int)rate;
    mono = malloc(sizeof(float) * cap);
    if (!mono) { printf("malloc failed\n"); return 1; }

    IAudioClient_Start(ac);
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
        double coeff = 2.0 * cos(2.0 * 3.14159265358979 * freq / rate);
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
           count, cap_rms, tone_ratio, discont, cerrs);
    return cerrs ? 2 : 0;
}
