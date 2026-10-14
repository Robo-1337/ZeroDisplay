#pragma once
#include <cstdint>
#include <vector>

namespace cc::mp4 {

// Minimal fragmented-MP4 (ISO BMFF / CMAF-style) writer for Media Source Extensions: one track per
// init segment, one moof+mdat per fragment.

struct Sample {
    const uint8_t* data;
    uint32_t size;
    uint32_t duration;   // in the track timescale
    bool keyframe;
};

// H.264 init segment. `sps`/`pps` are NAL units without start codes.
std::vector<uint8_t> VideoInit(uint32_t width, uint32_t height, const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps,
                               uint32_t timescale);
// AAC-LC init segment. `asc` = AudioSpecificConfig.
std::vector<uint8_t> AudioInit(uint32_t sampleRate, uint32_t channels, const std::vector<uint8_t>& asc);
std::vector<uint8_t> Fragment(uint32_t sequence, uint64_t baseDecodeTime, const std::vector<Sample>& samples);

// Annex B access unit -> length-prefixed (AVCC) sample, dropping SPS/PPS/AUD NAL units.
std::vector<uint8_t> AnnexBToAvcc(const uint8_t* data, size_t size);
// Splits Annex B parameter sets into SPS and PPS (no start codes).
bool SplitParameterSets(const std::vector<uint8_t>& annexB, std::vector<uint8_t>& sps, std::vector<uint8_t>& pps);
std::vector<uint8_t> AacConfig(uint32_t sampleRate, uint32_t channels);   // AAC-LC AudioSpecificConfig

} // namespace cc::mp4
