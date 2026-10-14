#pragma once
#include "core/Common.h"

#include <mftransform.h>

namespace cc {

// Windows' AAC-LC encoder MFT (synchronous): float stereo in, raw AAC frames (1024 samples) out.
class AacEncoder {
public:
    bool Start(uint32_t sampleRate, uint32_t bitrate = 128000);
    void Stop();
    bool Running() const { return m_mft != nullptr; }
    uint32_t SampleRate() const { return m_rate; }

    // Feeds PCM; calls `out(frame, size)` for every finished AAC frame.
    template <typename F> void Encode(const float* stereo, uint32_t frames, F&& out)
    {
        std::vector<uint8_t> f;
        while (Drain(f)) out(f.data(), f.size());   // make room first: the MFT holds little input
        Feed(stereo, frames);
        while (Drain(f)) out(f.data(), f.size());
    }

private:
    void Feed(const float* stereo, uint32_t frames);
    bool Drain(std::vector<uint8_t>& frame);

    ComPtr<IMFTransform> m_mft;
    uint32_t m_rate = 48000;
    DWORD m_outSize = 0;
    uint64_t m_fed = 0;   // samples fed so far (the MFT requires timestamps)
    std::vector<int16_t> m_pcm;
};

} // namespace cc
