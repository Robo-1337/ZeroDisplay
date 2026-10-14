#pragma once
#include "core/Common.h"

#include <mmreg.h>

namespace cc {

// Every PCM layout Windows devices use: 8-bit (unsigned), 16, 24 (packed), 32-bit integer (also 20 / 24 valid bits in a
// 32-bit container), 32 and 64-bit float.
enum class SampleKind { Unknown, U8, I16, I24, I32, F32, F64 };

struct PcmFormat {
    SampleKind kind = SampleKind::Unknown;
    uint32_t channels = 0;
    uint32_t rate = 0;
    uint32_t blockAlign = 0;
    uint32_t bits = 0;         // container bits per sample
    uint32_t channelMask = 0;  // speaker positions (WAVEFORMATEXTENSIBLE), or the usual layout for the channel count
    // Stereo downmix weights per channel (left, right): centre and surrounds folded in, LFE left out.
    std::vector<float> toLeft, toRight;
    int outLeft = 0, outRight = 1;   // where stereo goes on output (front left / right; mono: -1 = the average)
    std::wstring Describe() const;
};

PcmFormat ParseFormat(const WAVEFORMATEX* wf);
// Converts interleaved device samples to interleaved stereo float (mono duplicated, more channels mixed down).
void ToStereoFloat(const BYTE* src, uint32_t frames, const PcmFormat& f, float* dst);
// Converts interleaved stereo float to the device format (front left / right; other channels silent; mono = average).
void FromStereoFloat(const float* src, uint32_t frames, const PcmFormat& f, BYTE* dst);

// Single-producer / single-consumer ring of stereo float frames.
class AudioRing {
public:
    void Init(uint32_t capacityFrames);
    uint32_t Available() const { return uint32_t(m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_relaxed)); }
    uint32_t Write(const float* src, uint32_t frames);
    uint32_t WriteSilence(uint32_t frames);
    uint32_t Read(float* dst, uint32_t frames);
    void Skip(uint32_t frames);

private:
    std::vector<float> m_buf;
    uint32_t m_cap = 0;   // power of two
    std::atomic<uint64_t> m_write{0};
    std::atomic<uint64_t> m_read{0};
};

} // namespace cc
