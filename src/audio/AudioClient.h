#pragma once
#include "audio/AudioFormat.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

namespace cc {

// A WASAPI shared-mode, event-driven stream opened with the smallest period the device allows.
struct WasapiStream {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    PcmFormat format;
    uint32_t periodFrames = 0;
    uint32_t bufferFrames = 0;
    bool lowLatency = false;   // IAudioClient3 minimum period in use
    bool exclusive = false;    // exclusive mode: the device's hardware period, no system mixer
    std::wstring name;
};

// endpointId empty = default device for the flow. Returns an HRESULT; on success the client is
// initialized with `eventHandle` as its event but not started. With `exclusive`, exclusive mode is
// tried first and shared mode is used if the device refuses.
HRESULT OpenWasapiStream(const std::wstring& endpointId, EDataFlow flow, HANDLE eventHandle, WasapiStream& out, bool exclusive = false);

} // namespace cc
