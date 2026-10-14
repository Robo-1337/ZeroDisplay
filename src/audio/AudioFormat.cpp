#include "audio/AudioFormat.h"

#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace cc {

std::wstring PcmFormat::Describe() const
{
    const wchar_t* k = L"?";
    switch (kind) {
    case SampleKind::U8: k = L"int8"; break;
    case SampleKind::I16: k = L"int16"; break;
    case SampleKind::I24: k = L"int24"; break;
    case SampleKind::I32: k = L"int32"; break;
    case SampleKind::F32: k = L"float32"; break;
    case SampleKind::F64: k = L"float64"; break;
    default: break;
    }
    return Format(L"%u Hz %u ch %s", rate, channels, k);
}

// The usual speaker layout when a format names none (plain WAVEFORMATEX, or a zero mask).
static uint32_t DefaultMask(uint32_t channels)
{
    switch (channels) {
    case 1: return SPEAKER_FRONT_CENTER;
    case 2: return KSAUDIO_SPEAKER_STEREO;
    case 3: return KSAUDIO_SPEAKER_STEREO | SPEAKER_FRONT_CENTER;
    case 4: return KSAUDIO_SPEAKER_QUAD;
    case 5: return KSAUDIO_SPEAKER_QUAD | SPEAKER_FRONT_CENTER;
    case 6: return KSAUDIO_SPEAKER_5POINT1;
    case 7: return KSAUDIO_SPEAKER_5POINT1 | SPEAKER_BACK_CENTER;
    case 8: return KSAUDIO_SPEAKER_7POINT1_SURROUND;
    default: return 0;
    }
}

static void BuildMix(PcmFormat& f)
{
    f.toLeft.assign(f.channels, 0.0f);
    f.toRight.assign(f.channels, 0.0f);
    if (f.channels == 1) {
        f.toLeft[0] = f.toRight[0] = 1.0f;
        f.outLeft = f.outRight = -1;
        return;
    }
    // Channels appear in the order of their mask bits; channels beyond the mask have no position (left out).
    const float c = 0.7071f;
    uint32_t mask = f.channelMask;
    int left = -1, right = -1;
    for (uint32_t ch = 0; ch < f.channels && mask; ++ch) {
        const uint32_t bit = mask & (~mask + 1);   // lowest set bit
        mask &= mask - 1;
        float l = 0.0f, r = 0.0f;
        switch (bit) {
        case SPEAKER_FRONT_LEFT:
            l = 1.0f;
            if (left < 0) left = int(ch);
            break;
        case SPEAKER_FRONT_RIGHT:
            r = 1.0f;
            if (right < 0) right = int(ch);
            break;
        case SPEAKER_FRONT_CENTER: l = r = c; break;
        case SPEAKER_LOW_FREQUENCY: break;   // left out, as the ITU stereo downmix does
        case SPEAKER_BACK_LEFT:
        case SPEAKER_SIDE_LEFT:
        case SPEAKER_FRONT_LEFT_OF_CENTER:
        case SPEAKER_TOP_FRONT_LEFT:
        case SPEAKER_TOP_BACK_LEFT: l = c; break;
        case SPEAKER_BACK_RIGHT:
        case SPEAKER_SIDE_RIGHT:
        case SPEAKER_FRONT_RIGHT_OF_CENTER:
        case SPEAKER_TOP_FRONT_RIGHT:
        case SPEAKER_TOP_BACK_RIGHT: r = c; break;
        default: l = r = 0.5f; break;   // back / top centre
        }
        f.toLeft[ch] = l;
        f.toRight[ch] = r;
    }
    if (left < 0 || right < 0) {
        // No front pair named (an unusual mask): the first two channels are left and right.
        std::fill(f.toLeft.begin(), f.toLeft.end(), 0.0f);
        std::fill(f.toRight.begin(), f.toRight.end(), 0.0f);
        f.toLeft[0] = 1.0f;
        f.toRight[1] = 1.0f;
        left = 0;
        right = 1;
    }
    f.outLeft = left;
    f.outRight = right;
}

PcmFormat ParseFormat(const WAVEFORMATEX* wf)
{
    PcmFormat f;
    f.channels = wf->nChannels;
    f.rate = wf->nSamplesPerSec;
    f.blockAlign = wf->nBlockAlign;
    f.bits = wf->wBitsPerSample;
    WORD tag = wf->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        auto ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) tag = WAVE_FORMAT_PCM;
        f.channelMask = ext->dwChannelMask;
    }
    if (f.channels == 0 || f.rate == 0 || f.blockAlign < f.channels * (f.bits / 8)) return f;
    if (tag == WAVE_FORMAT_IEEE_FLOAT && f.bits == 32) f.kind = SampleKind::F32;
    else if (tag == WAVE_FORMAT_IEEE_FLOAT && f.bits == 64) f.kind = SampleKind::F64;
    else if (tag == WAVE_FORMAT_PCM && f.bits == 8) f.kind = SampleKind::U8;
    else if (tag == WAVE_FORMAT_PCM && f.bits == 16) f.kind = SampleKind::I16;
    else if (tag == WAVE_FORMAT_PCM && f.bits == 24) f.kind = SampleKind::I24;
    else if (tag == WAVE_FORMAT_PCM && f.bits == 32) f.kind = SampleKind::I32;   // also 20 / 24 valid bits (left-justified)
    if (std::popcount(f.channelMask) < int(std::min<uint32_t>(f.channels, 2))) f.channelMask = DefaultMask(f.channels);
    BuildMix(f);
    return f;
}

static inline float ReadSample(const BYTE* p, SampleKind k)
{
    switch (k) {
    case SampleKind::F32: {
        float v;
        memcpy(&v, p, 4);
        return v;
    }
    case SampleKind::F64: {
        double v;
        memcpy(&v, p, 8);
        return float(v);
    }
    case SampleKind::U8: return (float(p[0]) - 128.0f) * (1.0f / 128.0f);
    case SampleKind::I16: {
        int16_t v;
        memcpy(&v, p, 2);
        return float(v) * (1.0f / 32768.0f);
    }
    case SampleKind::I24: {
        int32_t v = int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 8;
        return float(v) * (1.0f / 8388608.0f);
    }
    case SampleKind::I32: {
        int32_t v;
        memcpy(&v, p, 4);
        return float(double(v) * (1.0 / 2147483648.0));
    }
    default: return 0.0f;
    }
}

static inline void WriteSample(BYTE* p, SampleKind k, float v)
{
    v = std::clamp(v, -1.0f, 1.0f);
    switch (k) {
    case SampleKind::F32: memcpy(p, &v, 4); break;
    case SampleKind::F64: {
        const double d = v;
        memcpy(p, &d, 8);
        break;
    }
    case SampleKind::U8: p[0] = BYTE(std::clamp(std::lround(v * 127.0f) + 128, 0L, 255L)); break;
    case SampleKind::I16: {
        int16_t s = int16_t(std::lround(v * 32767.0f));
        memcpy(p, &s, 2);
        break;
    }
    case SampleKind::I24: {
        int32_t s = int32_t(std::lround(v * 8388607.0f));
        p[0] = BYTE(s);
        p[1] = BYTE(s >> 8);
        p[2] = BYTE(s >> 16);
        break;
    }
    case SampleKind::I32: {
        int32_t s = int32_t(std::llround(double(v) * 2147483647.0));
        memcpy(p, &s, 4);
        break;
    }
    default: break;
    }
}

static inline uint32_t BytesPerSample(SampleKind k)
{
    switch (k) {
    case SampleKind::U8: return 1;
    case SampleKind::I16: return 2;
    case SampleKind::I24: return 3;
    case SampleKind::F64: return 8;
    default: return 4;
    }
}

void ToStereoFloat(const BYTE* src, uint32_t frames, const PcmFormat& f, float* dst)
{
    if (f.kind == SampleKind::F32 && f.channels == 2) {
        memcpy(dst, src, size_t(frames) * 8);
        return;
    }
    const uint32_t bps = BytesPerSample(f.kind);
    if (f.channels <= 2) {
        for (uint32_t i = 0; i < frames; ++i) {
            const BYTE* frame = src + size_t(i) * f.blockAlign;
            float l = ReadSample(frame, f.kind);
            float r = f.channels > 1 ? ReadSample(frame + bps, f.kind) : l;
            dst[2 * i] = l;
            dst[2 * i + 1] = r;
        }
        return;
    }
    for (uint32_t i = 0; i < frames; ++i) {
        const BYTE* frame = src + size_t(i) * f.blockAlign;
        float l = 0.0f, r = 0.0f;
        for (uint32_t ch = 0; ch < f.channels; ++ch) {
            if (f.toLeft[ch] == 0.0f && f.toRight[ch] == 0.0f) continue;
            const float v = ReadSample(frame + size_t(ch) * bps, f.kind);
            l += v * f.toLeft[ch];
            r += v * f.toRight[ch];
        }
        dst[2 * i] = l;
        dst[2 * i + 1] = r;
    }
}

void FromStereoFloat(const float* src, uint32_t frames, const PcmFormat& f, BYTE* dst)
{
    if (f.kind == SampleKind::F32 && f.channels == 2) {
        memcpy(dst, src, size_t(frames) * 8);
        return;
    }
    const uint32_t bps = BytesPerSample(f.kind);
    // Silence for the channels that get nothing (8-bit silence is 128).
    memset(dst, f.kind == SampleKind::U8 ? 128 : 0, size_t(frames) * f.blockAlign);
    for (uint32_t i = 0; i < frames; ++i) {
        BYTE* frame = dst + size_t(i) * f.blockAlign;
        if (f.outLeft < 0) {
            WriteSample(frame, f.kind, 0.5f * (src[2 * i] + src[2 * i + 1]));
        } else {
            WriteSample(frame + size_t(f.outLeft) * bps, f.kind, src[2 * i]);
            WriteSample(frame + size_t(f.outRight) * bps, f.kind, src[2 * i + 1]);
        }
    }
}

// ---------------------------------------------------------------------------------------------

void AudioRing::Init(uint32_t capacityFrames)
{
    uint32_t cap = 1;
    while (cap < capacityFrames) cap <<= 1;
    m_cap = cap;
    m_buf.assign(size_t(cap) * 2, 0.0f);
    m_write = 0;
    m_read = 0;
}

uint32_t AudioRing::Write(const float* src, uint32_t frames)
{
    uint64_t w = m_write.load(std::memory_order_relaxed);
    uint64_t r = m_read.load(std::memory_order_acquire);
    uint32_t free = m_cap - uint32_t(w - r);
    uint32_t n = std::min(frames, free);
    uint32_t pos = uint32_t(w & (m_cap - 1));
    uint32_t first = std::min(n, m_cap - pos);
    memcpy(&m_buf[size_t(pos) * 2], src, size_t(first) * 8);
    if (n > first) memcpy(&m_buf[0], src + size_t(first) * 2, size_t(n - first) * 8);
    m_write.store(w + n, std::memory_order_release);
    return n;
}

uint32_t AudioRing::WriteSilence(uint32_t frames)
{
    uint64_t w = m_write.load(std::memory_order_relaxed);
    uint64_t r = m_read.load(std::memory_order_acquire);
    uint32_t free = m_cap - uint32_t(w - r);
    uint32_t n = std::min(frames, free);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t pos = uint32_t((w + i) & (m_cap - 1));
        m_buf[size_t(pos) * 2] = 0.0f;
        m_buf[size_t(pos) * 2 + 1] = 0.0f;
    }
    m_write.store(w + n, std::memory_order_release);
    return n;
}

uint32_t AudioRing::Read(float* dst, uint32_t frames)
{
    uint64_t r = m_read.load(std::memory_order_relaxed);
    uint64_t w = m_write.load(std::memory_order_acquire);
    uint32_t n = std::min(frames, uint32_t(w - r));
    uint32_t pos = uint32_t(r & (m_cap - 1));
    uint32_t first = std::min(n, m_cap - pos);
    memcpy(dst, &m_buf[size_t(pos) * 2], size_t(first) * 8);
    if (n > first) memcpy(dst + size_t(first) * 2, &m_buf[0], size_t(n - first) * 8);
    m_read.store(r + n, std::memory_order_release);
    return n;
}

void AudioRing::Skip(uint32_t frames)
{
    uint64_t r = m_read.load(std::memory_order_relaxed);
    uint64_t w = m_write.load(std::memory_order_acquire);
    uint32_t n = std::min(frames, uint32_t(w - r));
    m_read.store(r + n, std::memory_order_release);
}

} // namespace cc
