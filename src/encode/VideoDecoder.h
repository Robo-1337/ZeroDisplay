#pragma once
#include "core/Common.h"
#include "devices/VideoSource.h"

#include <mfapi.h>
#include <mftransform.h>

#include <functional>

namespace cc {

// H.264 / HEVC decoder MFT producing NV12 frames (GPU textures when the decoder supports our D3D
// device). Used by Flashback to play back the replay buffer.
class VideoDecoder {
public:
    ~VideoDecoder() { Close(); }
    // `hdr`: the stream is our HDR10 recording (10-bit HEVC, BT.2020, PQ): frames come out as P010 tagged that way.
    bool Open(IMFDXGIDeviceManager* dxgi, UINT width, UINT height, UINT fpsNum, UINT fpsDen, bool hevc, bool hdr = false);
    void Close();
    bool IsOpen() const { return m_mft != nullptr; }
    // Feeds one access unit (Annex B). Every frame that comes out is passed to `out`.
    bool Decode(const std::vector<uint8_t>& au, int64_t time, const std::function<void(CapturedFrame&&)>& out);
    void Flush();   // drop everything in flight (before a seek)
    void Drain(const std::function<void(CapturedFrame&&)>& out);   // get frames the decoder still holds
    bool Gpu() const { return m_gpu; }

private:
    bool SelectOutput();   // NV12, or P010 for 10-bit streams
    bool m_hdr = false;
    bool m_p010 = false;   // current output is P010
    bool Pull(const std::function<void(CapturedFrame&&)>& out);   // every finished frame

    ComPtr<IMFTransform> m_mft;
    UINT m_width = 0, m_height = 0;
    bool m_providesSamples = false;
    bool m_gpu = false;
};

} // namespace cc
