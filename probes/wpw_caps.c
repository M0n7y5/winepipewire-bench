/* Capability probe for winepipewire.drv: IAudioClient3 engine periods and
 * surround mix-format negotiation.
 *
 * Phase A queries the default render endpoint:
 *   GetMixFormat -> IAudioClient3::GetSharedModeEnginePeriod
 * and prints the engine period range in frames.  On winepipewire the
 * minimum derives from the graph's clock.min-quantum clamped to the
 * 128-frame Windows-style floor (128 at 48 kHz); winepulse stays at the
 * 3 ms transplant value (144 at 48 kHz).
 *
 * Phase B (with a name argument) finds the active render endpoint whose
 * FriendlyName contains the substring, prints its mix format (channel
 * count, decimal channel mask, rate), checks IsFormatSupported for a
 * 5.1 float WAVEFORMATEXTENSIBLE, then Initializes a shared stream on the
 * full mix format and renders ~300 ms of silence, counting errors.  This
 * exercises the SPA-position <-> WASAPI-mask mapping end to end on
 * surround sinks (the bench loads a 7.1 null sink for it).
 *
 * Prints machine-parseable key=value lines consumed by run.sh (decimal
 * integers only).  Any FAILED hr aborts with the call name and exit 1.
 *
 * Args: [device-name-substring]  (no argument: phase A only)
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propkeydef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DEFINE_PROPERTYKEY(PKEY_Device_FriendlyName, 0xa45c254e,0xdf1c,0x4efd,0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0, 14);

int _fltused = 0x9875;

static const char *hrs(HRESULT hr){ static char b[32]; sprintf(b,"0x%08lx",(unsigned long)hr); return b; }

static IMMDevice *find_render_device(IMMDeviceEnumerator *enumr, const WCHAR *substr)
{
    IMMDeviceCollection *col = NULL;
    IMMDevice *found = NULL;
    UINT count = 0, i;
    HRESULT hr;

    hr = IMMDeviceEnumerator_EnumAudioEndpoints(enumr, eRender, DEVICE_STATE_ACTIVE, &col);
    if (FAILED(hr)) { printf("EnumAudioEndpoints %s\n", hrs(hr)); return NULL; }
    IMMDeviceCollection_GetCount(col, &count);
    for (i = 0; i < count && !found; i++)
    {
        IMMDevice *dev = NULL;
        IPropertyStore *ps = NULL;
        PROPVARIANT pv;

        if (FAILED(IMMDeviceCollection_Item(col, i, &dev)))
            continue;
        PropVariantInit(&pv);
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps)) &&
            SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &pv)) &&
            pv.vt == VT_LPWSTR && pv.pwszVal && wcsstr(pv.pwszVal, substr))
        {
            found = dev;
            dev = NULL;
        }
        PropVariantClear(&pv);
        if (ps) IPropertyStore_Release(ps);
        if (dev) IMMDevice_Release(dev);
    }
    IMMDeviceCollection_Release(col);
    return found;
}

static int engine_period_caps(IMMDevice *dev)
{
    IAudioClient3 *ac3 = NULL;
    WAVEFORMATEX *mix = NULL;
    UINT32 def_frames = 0, unit_frames = 0, min_frames = 0, max_frames = 0;
    HRESULT hr;

    hr = IMMDevice_Activate(dev, &IID_IAudioClient3, CLSCTX_ALL, NULL, (void **)&ac3);
    if (FAILED(hr)) { printf("Activate(IAudioClient3) %s\n", hrs(hr)); return 1; }
    hr = IAudioClient3_GetMixFormat(ac3, &mix);
    if (FAILED(hr)) { printf("GetMixFormat %s\n", hrs(hr)); return 1; }
    hr = IAudioClient3_GetSharedModeEnginePeriod(ac3, mix, &def_frames, &unit_frames,
                                                 &min_frames, &max_frames);
    if (FAILED(hr)) { printf("GetSharedModeEnginePeriod %s\n", hrs(hr)); return 1; }

    printf("engine_def_frames=%u engine_min_frames=%u engine_max_frames=%u engine_rate=%lu\n",
           def_frames, min_frames, max_frames, (unsigned long)mix->nSamplesPerSec);

    CoTaskMemFree(mix);
    IAudioClient3_Release(ac3);
    return 0;
}

static void build_51(WAVEFORMATEXTENSIBLE *fmt, DWORD rate)
{
    memset(fmt, 0, sizeof(*fmt));
    fmt->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt->Format.nChannels = 6;
    fmt->Format.nSamplesPerSec = rate;
    fmt->Format.wBitsPerSample = 32;
    fmt->Format.nBlockAlign = 6 * 4;
    fmt->Format.nAvgBytesPerSec = rate * 6 * 4;
    fmt->Format.cbSize = sizeof(*fmt) - sizeof(fmt->Format);
    fmt->Samples.wValidBitsPerSample = 32;
    fmt->dwChannelMask = KSAUDIO_SPEAKER_5POINT1;
    fmt->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
}

static int surround_check(IMMDevice *dev)
{
    IAudioClient *ac = NULL;
    IAudioRenderClient *rc = NULL;
    WAVEFORMATEX *mix = NULL, *closest = NULL;
    WAVEFORMATEXTENSIBLE f51;
    DWORD mask = 0;
    UINT32 buf_frames = 0;
    BYTE *data = NULL;
    int errors = 0, t;
    HRESULT hr;

    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    if (FAILED(hr)) { printf("Activate(IAudioClient) %s\n", hrs(hr)); return 1; }
    hr = IAudioClient_GetMixFormat(ac, &mix);
    if (FAILED(hr)) { printf("GetMixFormat %s\n", hrs(hr)); return 1; }

    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        mask = ((WAVEFORMATEXTENSIBLE *)mix)->dwChannelMask;
    printf("mix_channels=%u mix_mask=%lu mix_rate=%lu\n",
           mix->nChannels, (unsigned long)mask, (unsigned long)mix->nSamplesPerSec);

    build_51(&f51, mix->nSamplesPerSec);
    hr = IAudioClient_IsFormatSupported(ac, AUDCLNT_SHAREMODE_SHARED, &f51.Format, &closest);
    printf("fmt51_supported=%d\n", hr == S_OK ? 1 : 0);
    if (closest) CoTaskMemFree(closest);

    /* Render ~300 ms of silence on the full mix format: end-to-end proof
     * that an 8-channel stream connects and progresses. */
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, 0, 5000000, 0, mix, NULL);
    if (FAILED(hr)) { printf("Initialize %s\n", hrs(hr)); return 1; }
    hr = IAudioClient_GetBufferSize(ac, &buf_frames);
    if (FAILED(hr)) { printf("GetBufferSize %s\n", hrs(hr)); return 1; }
    hr = IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc);
    if (FAILED(hr)) { printf("GetService %s\n", hrs(hr)); return 1; }

    hr = IAudioRenderClient_GetBuffer(rc, buf_frames, &data);
    if (SUCCEEDED(hr))
        hr = IAudioRenderClient_ReleaseBuffer(rc, buf_frames, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr)) errors++;
    hr = IAudioClient_Start(ac);
    if (FAILED(hr)) { printf("Start %s\n", hrs(hr)); return 1; }

    for (t = 0; t < 30; t++)
    {
        UINT32 pad = 0;
        Sleep(10);
        if (FAILED(IAudioClient_GetCurrentPadding(ac, &pad))) { errors++; continue; }
        if (pad < buf_frames / 2)
        {
            UINT32 want = buf_frames - pad;
            if (SUCCEEDED(IAudioRenderClient_GetBuffer(rc, want, &data)))
            {
                if (FAILED(IAudioRenderClient_ReleaseBuffer(rc, want, AUDCLNT_BUFFERFLAGS_SILENT)))
                    errors++;
            }
            else errors++;
        }
    }
    IAudioClient_Stop(ac);
    printf("render_errors=%d\n", errors);

    IAudioRenderClient_Release(rc);
    CoTaskMemFree(mix);
    IAudioClient_Release(ac);
    return 0;
}

int main(int argc, char **argv)
{
    IMMDeviceEnumerator *enumr = NULL;
    IMMDevice *dev = NULL;
    HRESULT hr;
    int rc = 0;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&enumr);
    if (FAILED(hr)) { printf("CoCreateInstance %s\n", hrs(hr)); return 1; }

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumr, eRender, eConsole, &dev);
    if (FAILED(hr)) { printf("GetDefaultAudioEndpoint %s\n", hrs(hr)); return 1; }
    rc = engine_period_caps(dev);
    IMMDevice_Release(dev);
    if (rc) return rc;

    if (argc > 1)
    {
        WCHAR wname[128];
        MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wname, 128);
        dev = find_render_device(enumr, wname);
        printf("dev_found=%d\n", dev ? 1 : 0);
        if (!dev) return 1;
        rc = surround_check(dev);
        IMMDevice_Release(dev);
    }
    IMMDeviceEnumerator_Release(enumr);
    return rc;
}
