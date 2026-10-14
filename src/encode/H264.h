#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cc::h264 {

struct Nal {
    const uint8_t* data;   // NAL header byte onwards (start code stripped)
    size_t size;
    int type() const { return size ? (data[0] & 0x1F) : -1; }
};

// Splits an Annex B byte stream (00 00 01 / 00 00 00 01 start codes) into NAL units.
std::vector<Nal> SplitAnnexB(const uint8_t* data, size_t size);

// Returns SPS+PPS (Annex B, with start codes) found in an access unit, or empty.
std::vector<uint8_t> ExtractParameterSets(const uint8_t* data, size_t size);

bool IsKeyframe(const uint8_t* data, size_t size);   // contains an IDR slice

// HEVC (H.265): NAL type = (byte0 >> 1) & 0x3F. VPS/SPS/PPS = 32/33/34, IRAP slices = 16..21.
std::vector<uint8_t> ExtractParameterSetsHevc(const uint8_t* data, size_t size);
bool IsKeyframeHevc(const uint8_t* data, size_t size);

// "avc1.PPCCLL" codec string from the SPS (for WebCodecs / MSE), or empty.
std::string CodecString(const std::vector<uint8_t>& parameterSets);

} // namespace cc::h264
