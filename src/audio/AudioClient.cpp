#include "audio/AudioClient.h"
#include "core/Log.h"
#include "devices/DeviceManager.h"

#include <ks.h>
#include <ksmedia.h>

namespace cc {

static WAVEFORMATEXTENSIBLE MakeFormat(uint32_t rate, uint16_t channels, uint16_t container, uint16_t valid, bool isFloat, DWORD mask = 0)
{
    WAVEFORMATEXTENSIBLE w{};
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = channels;
    w.Format.nSamplesPerSec = rate;
    w.Format.wBitsPerSample = container;
    w.Format.nBlockAlign = WORD(channels * container / 8);
    w.Format.nAvgBytesPerSec = rate * w.Format.nBlockAlign;
    w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w.Samples.wValidBitsPerSample = valid;
    w.dwChannelMask = mask ? mask : channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : SPEAKER_FRONT_CENTER;
    w.SubFormat = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    return w;
}

// Exclusive mode at the device's minimum period. Returns false if the device can't do it. Tries the rate and channel
// layout Windows uses for the device first, then common rates in stereo and in the device's own layout.
static bool TryExclusive(WasapiStream& out, HANDLE eventHandle, uint32_t preferredRate, uint16_t mixChannels, DWORD mixMask)
{
    ComPtr<IAudioClient> client;
    if (FAILED(out.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return false;
    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    client->GetDevicePeriod(&defPeriod, &minPeriod);

    struct Candidate { uint16_t container, valid; bool isFloat; };
    const Candidate candidates[] = {{32, 32, true}, {32, 24, false}, {24, 24, false}, {16, 16, false}, {32, 32, false}};
    const uint32_t rates[] = {preferredRate, 48000, 44100, 96000, 88200, 192000, 176400, 32000};
    struct Layout { uint16_t channels; DWORD mask; };
    const Layout layouts[] = {{2, 0}, {mixChannels, mixMask}};
    WAVEFORMATEXTENSIBLE chosen{};
    bool found = false;
    for (uint32_t rate : rates) {
        for (const auto& lay : layouts) {
            if (lay.channels == 0 || (&lay != layouts && lay.channels == 2)) continue;
            for (const auto& c : candidates) {
                WAVEFORMATEXTENSIBLE w = MakeFormat(rate, lay.channels, c.container, c.valid, c.isFloat, lay.mask);
                if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.Format, nullptr) == S_OK) {
                    chosen = w;
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
        if (found) break;
    }
    if (!found) return false;

    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minPeriod, minPeriod, &chosen.Format, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 frames = 0;
        client->GetBufferSize(&frames);
        REFERENCE_TIME aligned = REFERENCE_TIME(10000000.0 * frames / chosen.Format.nSamplesPerSec + 0.5);
        client.Reset();
        if (FAILED(out.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return false;
        hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, aligned, aligned, &chosen.Format, nullptr);
    }
    if (FAILED(hr)) {
        LOGW(L"Exclusive mode unavailable on %s: %s", out.name.c_str(), HrToString(hr).c_str());
        return false;
    }
    if (FAILED(client->SetEventHandle(eventHandle))) return false;
    out.client = client;
    out.format = ParseFormat(&chosen.Format);
    if (out.format.kind == SampleKind::Unknown) {
        out.client.Reset();
        return false;
    }
    client->GetBufferSize(&out.bufferFrames);
    out.periodFrames = out.bufferFrames;
    out.exclusive = true;
    return true;
}

HRESULT OpenWasapiStream(const std::wstring& endpointId, EDataFlow flow, HANDLE eventHandle, WasapiStream& out, bool exclusive)
{
    out = {};
    ComPtr<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (FAILED(hr)) return hr;
    if (endpointId.empty()) hr = en->GetDefaultAudioEndpoint(flow, eConsole, &out.device);
    else hr = en->GetDevice(endpointId.c_str(), &out.device);
    if (FAILED(hr)) return hr;
    out.name = DeviceManager::EndpointName(out.device.Get());

    WAVEFORMATEX* mix = nullptr;
    if (exclusive) {
        uint32_t rate = 48000;
        uint16_t channels = 2;
        DWORD mask = 0;
        ComPtr<IAudioClient> probe;
        if (SUCCEEDED(out.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &probe)) && SUCCEEDED(probe->GetMixFormat(&mix))) {
            rate = mix->nSamplesPerSec;
            channels = mix->nChannels;
            if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) mask = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
            CoTaskMemFree(mix);
            mix = nullptr;
        }
        probe.Reset();
        if (TryExclusive(out, eventHandle, rate, channels, mask)) return S_OK;
    }
    // Preferred: IAudioClient3 with the minimum shared-mode engine period (for a mix format we can read directly).
    ComPtr<IAudioClient3> client3;
    hr = out.device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &client3);
    if (SUCCEEDED(hr) && SUCCEEDED(client3->GetMixFormat(&mix)) && ParseFormat(mix).kind != SampleKind::Unknown) {
        UINT32 def = 0, fund = 0, minP = 0, maxP = 0;
        if (SUCCEEDED(client3->GetSharedModeEnginePeriod(mix, &def, &fund, &minP, &maxP)) && minP > 0 &&
            SUCCEEDED(client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minP, mix, nullptr))) {
            out.client = client3;
            out.periodFrames = minP;
            out.lowLatency = minP < def;
        }
    }
    if (!out.client) {
        if (mix) {
            CoTaskMemFree(mix);
            mix = nullptr;
        }
        ComPtr<IAudioClient> client;
        hr = out.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
        if (FAILED(hr)) return hr;
        hr = client->GetMixFormat(&mix);
        if (FAILED(hr)) return hr;
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        if (ParseFormat(mix).kind == SampleKind::Unknown) {
            // A mix format we can't read (unusual driver): let Windows convert to 32-bit float at the same rate.
            WAVEFORMATEXTENSIBLE w = MakeFormat(mix->nSamplesPerSec, mix->nChannels == 1 ? 1 : 2, 32, 32, true);
            CoTaskMemFree(mix);
            mix = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(w)));
            if (!mix) return E_OUTOFMEMORY;
            memcpy(mix, &w, sizeof(w));
            flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            LOGI(L"Audio: %s uses a mix format that needs converting; Windows converts it to float", out.name.c_str());
        }
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, mix, nullptr);
        if (FAILED(hr)) {
            CoTaskMemFree(mix);
            return hr;
        }
        REFERENCE_TIME defPeriod = 0, minPeriod = 0;
        client->GetDevicePeriod(&defPeriod, &minPeriod);
        out.client = client;
        out.periodFrames = UINT32(double(defPeriod) * mix->nSamplesPerSec / 1e7 + 0.5);
    }

    out.format = ParseFormat(mix);
    CoTaskMemFree(mix);
    if (out.format.kind == SampleKind::Unknown) return AUDCLNT_E_UNSUPPORTED_FORMAT;

    hr = out.client->SetEventHandle(eventHandle);
    if (FAILED(hr)) return hr;
    out.client->GetBufferSize(&out.bufferFrames);
    return S_OK;
}

} // namespace cc
