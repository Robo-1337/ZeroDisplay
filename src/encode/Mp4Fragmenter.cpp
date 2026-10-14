#include "encode/Mp4Fragmenter.h"
#include "encode/H264.h"

#include <cstring>

namespace cc::mp4 {

namespace {

class Writer {
public:
    std::vector<uint8_t> b;
    void u8(uint32_t v) { b.push_back(uint8_t(v)); }
    void u16(uint32_t v) { u8(v >> 8); u8(v); }
    void u24(uint32_t v) { u8(v >> 16); u16(v); }
    void u32(uint32_t v) { u16(v >> 16); u16(v); }
    void u64(uint64_t v) { u32(uint32_t(v >> 32)); u32(uint32_t(v)); }
    void zeros(size_t n) { b.insert(b.end(), n, 0); }
    void bytes(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
    void fourcc(const char* t) { bytes(reinterpret_cast<const uint8_t*>(t), 4); }
    size_t begin(const char* type)
    {
        size_t at = b.size();
        u32(0);
        fourcc(type);
        return at;
    }
    size_t beginFull(const char* type, uint8_t version, uint32_t flags)
    {
        size_t at = begin(type);
        u8(version);
        u24(flags);
        return at;
    }
    void end(size_t at) { patch32(at, uint32_t(b.size() - at)); }
    void patch32(size_t at, uint32_t v)
    {
        b[at] = uint8_t(v >> 24);
        b[at + 1] = uint8_t(v >> 16);
        b[at + 2] = uint8_t(v >> 8);
        b[at + 3] = uint8_t(v);
    }
};

void Ftyp(Writer& w)
{
    size_t f = w.begin("ftyp");
    w.fourcc("iso6");
    w.u32(0);
    w.fourcc("iso6");
    w.fourcc("iso5");
    w.fourcc("mp41");
    w.fourcc("cmfc");
    w.end(f);
}

void Matrix(Writer& w)
{
    const uint32_t m[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
    for (uint32_t v : m) w.u32(v);
}

// Everything around the sample description is identical for audio and video.
template <typename SampleEntry>
std::vector<uint8_t> Init(bool video, uint32_t timescale, uint32_t width, uint32_t height, SampleEntry entry)
{
    Writer w;
    Ftyp(w);
    size_t moov = w.begin("moov");
    {
        size_t mvhd = w.beginFull("mvhd", 0, 0);
        w.u32(0);   // creation
        w.u32(0);   // modification
        w.u32(1000);
        w.u32(0);   // duration unknown (live)
        w.u32(0x00010000);   // rate
        w.u16(0x0100);       // volume
        w.zeros(10);
        Matrix(w);
        w.zeros(24);
        w.u32(2);   // next track id
        w.end(mvhd);

        size_t trak = w.begin("trak");
        size_t tkhd = w.beginFull("tkhd", 0, 3);   // enabled | in movie
        w.u32(0);
        w.u32(0);
        w.u32(1);   // track id
        w.u32(0);
        w.u32(0);   // duration
        w.zeros(8);
        w.u16(0);   // layer
        w.u16(0);   // alternate group
        w.u16(video ? 0 : 0x0100);
        w.u16(0);
        Matrix(w);
        w.u32(width << 16);
        w.u32(height << 16);
        w.end(tkhd);

        size_t mdia = w.begin("mdia");
        size_t mdhd = w.beginFull("mdhd", 0, 0);
        w.u32(0);
        w.u32(0);
        w.u32(timescale);
        w.u32(0);
        w.u16(0x55C4);   // language "und"
        w.u16(0);
        w.end(mdhd);
        size_t hdlr = w.beginFull("hdlr", 0, 0);
        w.u32(0);
        w.fourcc(video ? "vide" : "soun");
        w.zeros(12);
        const char* name = video ? "Video" : "Audio";
        w.bytes(reinterpret_cast<const uint8_t*>(name), strlen(name) + 1);
        w.end(hdlr);

        size_t minf = w.begin("minf");
        if (video) {
            size_t vmhd = w.beginFull("vmhd", 0, 1);
            w.zeros(8);
            w.end(vmhd);
        } else {
            size_t smhd = w.beginFull("smhd", 0, 0);
            w.zeros(4);
            w.end(smhd);
        }
        size_t dinf = w.begin("dinf");
        size_t dref = w.beginFull("dref", 0, 0);
        w.u32(1);
        size_t url = w.beginFull("url ", 0, 1);
        w.end(url);
        w.end(dref);
        w.end(dinf);

        size_t stbl = w.begin("stbl");
        size_t stsd = w.beginFull("stsd", 0, 0);
        w.u32(1);
        entry(w);
        w.end(stsd);
        for (const char* t : {"stts", "stsc", "stco"}) {
            size_t x = w.beginFull(t, 0, 0);
            w.u32(0);
            w.end(x);
        }
        size_t stsz = w.beginFull("stsz", 0, 0);
        w.u32(0);
        w.u32(0);
        w.end(stsz);
        w.end(stbl);
        w.end(minf);
        w.end(mdia);
        w.end(trak);

        size_t mvex = w.begin("mvex");
        size_t trex = w.beginFull("trex", 0, 0);
        w.u32(1);   // track id
        w.u32(1);   // sample description index
        w.u32(0);
        w.u32(0);
        w.u32(0);
        w.end(trex);
        w.end(mvex);
    }
    w.end(moov);
    return std::move(w.b);
}

} // namespace

std::vector<uint8_t> VideoInit(uint32_t width, uint32_t height, const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps, uint32_t timescale)
{
    return Init(true, timescale, width, height, [&](Writer& w) {
        size_t avc1 = w.begin("avc1");
        w.zeros(6);
        w.u16(1);   // data reference index
        w.zeros(16);
        w.u16(width);
        w.u16(height);
        w.u32(0x00480000);
        w.u32(0x00480000);
        w.u32(0);
        w.u16(1);   // frame count
        w.zeros(32);   // compressor name
        w.u16(0x0018);
        w.u16(0xFFFF);
        size_t avcC = w.begin("avcC");
        w.u8(1);
        w.u8(sps.size() > 1 ? sps[1] : 0x64);   // profile
        w.u8(sps.size() > 2 ? sps[2] : 0);      // compatibility
        w.u8(sps.size() > 3 ? sps[3] : 0x28);   // level
        w.u8(0xFF);   // 4-byte lengths
        w.u8(0xE1);   // 1 SPS
        w.u16(uint32_t(sps.size()));
        w.bytes(sps.data(), sps.size());
        w.u8(1);      // 1 PPS
        w.u16(uint32_t(pps.size()));
        w.bytes(pps.data(), pps.size());
        const uint8_t profile = sps.size() > 1 ? sps[1] : 0;
        if (profile == 100 || profile == 110 || profile == 122 || profile == 144) {
            w.u8(0xFC | 1);   // 4:2:0
            w.u8(0xF8);       // 8-bit luma
            w.u8(0xF8);       // 8-bit chroma
            w.u8(0);
        }
        w.end(avcC);
        w.end(avc1);
    });
}

std::vector<uint8_t> AudioInit(uint32_t sampleRate, uint32_t channels, const std::vector<uint8_t>& asc)
{
    return Init(false, sampleRate, 0, 0, [&](Writer& w) {
        size_t mp4a = w.begin("mp4a");
        w.zeros(6);
        w.u16(1);
        w.zeros(8);
        w.u16(channels);
        w.u16(16);
        w.u16(0);
        w.u16(0);
        w.u32(sampleRate << 16);
        size_t esds = w.beginFull("esds", 0, 0);
        const uint8_t dsiLen = uint8_t(asc.size());
        const uint8_t dcdLen = uint8_t(13 + 2 + dsiLen);
        const uint8_t esLen = uint8_t(3 + 2 + dcdLen + 3);
        w.u8(0x03);
        w.u8(esLen);
        w.u16(1);   // ES id
        w.u8(0);
        w.u8(0x04);
        w.u8(dcdLen);
        w.u8(0x40);   // MPEG-4 audio
        w.u8(0x15);   // audio stream
        w.u24(0);
        w.u32(192000);
        w.u32(192000);
        w.u8(0x05);
        w.u8(dsiLen);
        w.bytes(asc.data(), asc.size());
        w.u8(0x06);
        w.u8(1);
        w.u8(0x02);
        w.end(esds);
        w.end(mp4a);
    });
}

std::vector<uint8_t> Fragment(uint32_t sequence, uint64_t baseDecodeTime, const std::vector<Sample>& samples)
{
    Writer w;
    size_t moof = w.begin("moof");
    size_t mfhd = w.beginFull("mfhd", 0, 0);
    w.u32(sequence);
    w.end(mfhd);
    size_t traf = w.begin("traf");
    size_t tfhd = w.beginFull("tfhd", 0, 0x020000);   // default-base-is-moof
    w.u32(1);
    w.end(tfhd);
    size_t tfdt = w.beginFull("tfdt", 1, 0);
    w.u64(baseDecodeTime);
    w.end(tfdt);
    size_t trun = w.beginFull("trun", 0, 0x000001 | 0x000100 | 0x000200 | 0x000400);
    w.u32(uint32_t(samples.size()));
    size_t dataOffsetAt = w.b.size();
    w.u32(0);
    for (const auto& s : samples) {
        w.u32(s.duration);
        w.u32(s.size);
        w.u32(s.keyframe ? 0x02000000 : 0x01010000);
    }
    w.end(trun);
    w.end(traf);
    w.end(moof);
    w.patch32(dataOffsetAt, uint32_t(w.b.size() + 8));   // first sample right after the mdat header
    size_t mdat = w.begin("mdat");
    for (const auto& s : samples) w.bytes(s.data, s.size);
    w.end(mdat);
    return std::move(w.b);
}

std::vector<uint8_t> AnnexBToAvcc(const uint8_t* data, size_t size)
{
    std::vector<uint8_t> out;
    out.reserve(size + 16);
    for (const auto& nal : h264::SplitAnnexB(data, size)) {
        const int t = nal.type();
        if (t == 7 || t == 8 || t == 9) continue;   // SPS/PPS live in avcC; AUDs aren't needed
        const uint32_t n = uint32_t(nal.size);
        out.push_back(uint8_t(n >> 24));
        out.push_back(uint8_t(n >> 16));
        out.push_back(uint8_t(n >> 8));
        out.push_back(uint8_t(n));
        out.insert(out.end(), nal.data, nal.data + nal.size);
    }
    return out;
}

bool SplitParameterSets(const std::vector<uint8_t>& annexB, std::vector<uint8_t>& sps, std::vector<uint8_t>& pps)
{
    sps.clear();
    pps.clear();
    for (const auto& nal : h264::SplitAnnexB(annexB.data(), annexB.size())) {
        if (nal.type() == 7 && sps.empty()) sps.assign(nal.data, nal.data + nal.size);
        if (nal.type() == 8 && pps.empty()) pps.assign(nal.data, nal.data + nal.size);
    }
    return !sps.empty() && !pps.empty();
}

std::vector<uint8_t> AacConfig(uint32_t sampleRate, uint32_t channels)
{
    static const uint32_t rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    uint32_t idx = 3;
    for (uint32_t i = 0; i < 13; ++i)
        if (rates[i] == sampleRate) idx = i;
    const uint32_t v = (2u << 11) | (idx << 7) | (channels << 3);   // AAC-LC
    return {uint8_t(v >> 8), uint8_t(v)};
}

} // namespace cc::mp4
