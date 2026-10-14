#include "encode/Mp4File.h"
#include "core/Log.h"
#include "encode/AacEncoder.h"
#include "encode/H264.h"
#include "encode/Mp4Fragmenter.h"

#include <mfapi.h>
#include <mferror.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace cc {

namespace {

class Box {
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
        const size_t at = b.size();
        u32(0);
        fourcc(type);
        return at;
    }
    size_t full(const char* type, uint8_t version, uint32_t flags)
    {
        const size_t at = begin(type);
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

void Matrix(Box& w)
{
    for (uint32_t v : {0x00010000u, 0u, 0u, 0u, 0x00010000u, 0u, 0u, 0u, 0x40000000u}) w.u32(v);
}

int HevcType(const h264::Nal& n) { return n.size ? (n.data[0] >> 1) & 0x3F : -1; }

// NAL payload without emulation-prevention bytes (00 00 03 -> 00 00).
std::vector<uint8_t> Rbsp(const uint8_t* p, size_t n)
{
    std::vector<uint8_t> r;
    r.reserve(n);
    int zeros = 0;
    for (size_t i = 0; i < n; ++i) {
        if (zeros >= 2 && p[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = p[i] == 0 ? zeros + 1 : 0;
        r.push_back(p[i]);
    }
    return r;
}

// HEVCDecoderConfigurationRecord (ISO/IEC 14496-15) from Annex B VPS/SPS/PPS.
void WriteHvcC(Box& w, const std::vector<uint8_t>& ps)
{
    std::vector<h264::Nal> arrays[3];   // VPS, SPS, PPS
    for (const auto& n : h264::SplitAnnexB(ps.data(), ps.size())) {
        const int t = HevcType(n);
        if (t >= 32 && t <= 34) arrays[t - 32].push_back(n);
    }
    uint8_t ptl[12] = {0x01, 0x60, 0, 0, 0, 0x90, 0, 0, 0, 0, 0, 0x5D};   // Main, level 3.1 (if the SPS can't be read)
    uint8_t subLayers = 1;
    bool nested = true;
    if (!arrays[1].empty()) {
        const std::vector<uint8_t> r = Rbsp(arrays[1][0].data + 2, arrays[1][0].size - 2);   // skip the 2-byte NAL header
        if (r.size() >= 13) {
            subLayers = uint8_t(((r[0] >> 1) & 7) + 1);
            nested = (r[0] & 1) != 0;
            memcpy(ptl, &r[1], 12);
        }
    }
    const size_t c = w.begin("hvcC");
    w.u8(1);
    w.bytes(ptl, 12);                    // profile space/tier/idc, compatibility, constraints, level
    w.u16(0xF000);                       // min_spatial_segmentation_idc = 0
    w.u8(0xFC);                          // parallelismType = 0
    w.u8(0xFC | 1);                      // chroma 4:2:0
    w.u8(0xF8);                          // 8-bit luma
    w.u8(0xF8);                          // 8-bit chroma
    w.u16(0);                            // avgFrameRate unknown
    w.u8((subLayers << 3) | (nested ? 4 : 0) | 3);   // 4-byte NAL lengths
    uint8_t count = 0;
    for (auto& a : arrays) count += a.empty() ? 0 : 1;
    w.u8(count);
    for (int i = 0; i < 3; ++i) {
        if (arrays[i].empty()) continue;
        w.u8(0x80 | uint8_t(32 + i));   // array_completeness = 1
        w.u16(uint32_t(arrays[i].size()));
        for (const auto& n : arrays[i]) {
            w.u16(uint32_t(n.size));
            w.bytes(n.data, n.size);
        }
    }
    w.end(c);
}

struct Sample {
    std::vector<uint8_t> data;
    uint64_t dts = 0;        // track timescale
    uint32_t duration = 0;
    bool key = true;
};

struct Stream {
    bool video = false;
    uint32_t timescale = 90000;
    uint32_t width = 0, height = 0;
    uint32_t rate = 48000, channels = 2;
    std::vector<uint8_t> ps;        // video: Annex B VPS/SPS/PPS
    bool hdr = false;               // video: HDR10 (BT.2020, PQ)
    bool pcmIn = false;             // audio: PCM in, AAC out
    std::unique_ptr<AacEncoder> aac;
    int64_t audioStart = -1;        // audio: decode time (samples) of the first AAC frame
    uint64_t audioFrames = 0;
    std::vector<Sample> pending;
    uint32_t nominal = 1500;        // video: default duration (track units)
};

class Mp4Writer : public IMFSinkWriter {
public:
    explicit Mp4Writer(const std::wstring& path) : m_path(path) {}
    virtual ~Mp4Writer()
    {
        if (m_file != INVALID_HANDLE_VALUE) CloseHandle(m_file);
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override
    {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSinkWriter)) {
            *ppv = static_cast<IMFSinkWriter*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }

    // IMFSinkWriter
    HRESULT STDMETHODCALLTYPE AddStream(IMFMediaType* type, DWORD* index) override
    {
        if (!type || !index || m_begun) return E_INVALIDARG;
        GUID major{}, sub{};
        type->GetGUID(MF_MT_MAJOR_TYPE, &major);
        type->GetGUID(MF_MT_SUBTYPE, &sub);
        Stream s;
        if (major == MFMediaType_Video) {
            if (sub != MFVideoFormat_HEVC) return MF_E_INVALIDMEDIATYPE;
            s.video = true;
            MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &s.width, &s.height);
            UINT32 n = 0, d = 0;
            if (SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &n, &d)) && n && d) s.nominal = uint32_t(90000ull * d / n);
            s.hdr = MFGetAttributeUINT32(type, MF_MT_TRANSFER_FUNCTION, 0) == MFVideoTransFunc_2084;
            UINT32 size = 0;
            if (SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size) {
                s.ps.resize(size);
                type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, s.ps.data(), size, nullptr);
            }
        } else if (major == MFMediaType_Audio) {
            if (sub != MFAudioFormat_AAC) return MF_E_INVALIDMEDIATYPE;
            s.rate = MFGetAttributeUINT32(type, MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            s.channels = MFGetAttributeUINT32(type, MF_MT_AUDIO_NUM_CHANNELS, 2);
            s.timescale = s.rate;
        } else {
            return MF_E_INVALIDMEDIATYPE;
        }
        *index = DWORD(m_streams.size());
        m_streams.push_back(std::move(s));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetInputMediaType(DWORD index, IMFMediaType* type, IMFAttributes*) override
    {
        if (index >= m_streams.size() || !type) return E_INVALIDARG;
        Stream& s = m_streams[index];
        if (s.video) return S_OK;   // passthrough
        GUID sub{};
        type->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub != MFAudioFormat_PCM || MFGetAttributeUINT32(type, MF_MT_AUDIO_BITS_PER_SAMPLE, 0) != 16 ||
            MFGetAttributeUINT32(type, MF_MT_AUDIO_NUM_CHANNELS, 0) != 2)
            return MF_E_INVALIDMEDIATYPE;
        s.pcmIn = true;
        s.aac = std::make_unique<AacEncoder>();
        if (!s.aac->Start(s.rate, 192000)) return MF_E_INVALIDMEDIATYPE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE BeginWriting() override
    {
        if (m_begun) return MF_E_INVALIDREQUEST;
        m_file = CreateFileW(m_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        m_begun = true;
        return Write(Header());
    }

    HRESULT STDMETHODCALLTYPE WriteSample(DWORD index, IMFSample* sample) override
    {
        if (!m_begun || index >= m_streams.size() || !sample) return E_INVALIDARG;
        Stream& s = m_streams[index];
        LONGLONG t = 0, d = 0;
        sample->GetSampleTime(&t);
        sample->GetSampleDuration(&d);
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return E_FAIL;
        BYTE* p = nullptr;
        DWORD len = 0;
        if (FAILED(buf->Lock(&p, nullptr, &len))) return E_FAIL;
        if (s.video) {
            Sample out;
            out.dts = uint64_t(std::max<LONGLONG>(0, t)) * 9 / 1000;
            out.duration = d > 0 ? uint32_t(d * 9 / 1000) : s.nominal;
            out.key = MFGetAttributeUINT32(sample, MFSampleExtension_CleanPoint, 0) != 0;
            for (const auto& n : h264::SplitAnnexB(p, len)) {
                if (HevcType(n) == 35) continue;   // access unit delimiters aren't needed
                const uint32_t sz = uint32_t(n.size);
                const uint8_t hdr[4] = {uint8_t(sz >> 24), uint8_t(sz >> 16), uint8_t(sz >> 8), uint8_t(sz)};
                out.data.insert(out.data.end(), hdr, hdr + 4);
                out.data.insert(out.data.end(), n.data, n.data + n.size);
            }
            buf->Unlock();
            // Fragment boundary: before a keyframe once a second of video is waiting.
            if (out.key && !s.pending.empty() && out.dts >= s.pending.front().dts + 90000) {
                if (HRESULT hr = Flush(false); FAILED(hr)) return hr;
            }
            s.pending.push_back(std::move(out));
            return S_OK;
        }
        // Audio: PCM -> AAC frames of 1024 samples.
        const int16_t* pcm = reinterpret_cast<const int16_t*>(p);
        const uint32_t frames = len / 4;
        std::vector<float> f(size_t(frames) * 2);
        for (size_t i = 0; i < f.size(); ++i) f[i] = pcm[i] / 32768.0f;
        buf->Unlock();
        if (s.audioStart < 0) s.audioStart = std::max<LONGLONG>(0, t) * s.rate / 10000000;
        s.aac->Encode(f.data(), frames, [&](const uint8_t* a, size_t n) {
            Sample out;
            out.data.assign(a, a + n);
            out.dts = uint64_t(s.audioStart) + s.audioFrames * 1024;
            out.duration = 1024;
            s.audioFrames++;
            s.pending.push_back(std::move(out));
        });
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SendStreamTick(DWORD, LONGLONG) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PlaceMarker(DWORD, LPVOID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE NotifyEndOfSegment(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Flush(DWORD) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE Finalize() override
    {
        if (!m_begun) return MF_E_INVALIDREQUEST;
        const HRESULT hr = Flush(true);
        CloseHandle(m_file);
        m_file = INVALID_HANDLE_VALUE;
        m_begun = false;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetServiceForStream(DWORD, REFGUID, REFIID, LPVOID* ppv) override
    {
        if (ppv) *ppv = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetStatistics(DWORD, MF_SINK_WRITER_STATISTICS*) override { return E_NOTIMPL; }

private:
    HRESULT Write(const std::vector<uint8_t>& data)
    {
        DWORD written = 0;
        if (!WriteFile(m_file, data.data(), DWORD(data.size()), &written, nullptr) || written != data.size())
            return HRESULT_FROM_WIN32(GetLastError());
        return S_OK;
    }

    std::vector<uint8_t> Header() const
    {
        Box w;
        const size_t ftyp = w.begin("ftyp");
        w.fourcc("iso6");
        w.u32(0);
        for (const char* b : {"iso6", "isom", "iso2", "mp41", "hvc1"}) w.fourcc(b);
        w.end(ftyp);

        const size_t moov = w.begin("moov");
        const size_t mvhd = w.full("mvhd", 0, 0);
        w.u32(0);
        w.u32(0);
        w.u32(1000);
        w.u32(0);   // duration: in the fragments
        w.u32(0x00010000);
        w.u16(0x0100);
        w.zeros(10);
        Matrix(w);
        w.zeros(24);
        w.u32(uint32_t(m_streams.size() + 1));
        w.end(mvhd);
        for (size_t i = 0; i < m_streams.size(); ++i) Track(w, m_streams[i], uint32_t(i + 1));
        const size_t mvex = w.begin("mvex");
        for (size_t i = 0; i < m_streams.size(); ++i) {
            const size_t trex = w.full("trex", 0, 0);
            w.u32(uint32_t(i + 1));
            w.u32(1);
            w.u32(0);
            w.u32(0);
            w.u32(0);
            w.end(trex);
        }
        w.end(mvex);
        w.end(moov);
        return std::move(w.b);
    }

    static void Track(Box& w, const Stream& s, uint32_t id)
    {
        const size_t trak = w.begin("trak");
        const size_t tkhd = w.full("tkhd", 0, 3);
        w.u32(0);
        w.u32(0);
        w.u32(id);
        w.u32(0);
        w.u32(0);
        w.zeros(8);
        w.u16(0);
        w.u16(s.video ? 0 : 1);   // audio tracks are alternatives (track 1 = the mix plays by default)
        w.u16(s.video ? 0 : 0x0100);
        w.u16(0);
        Matrix(w);
        w.u32(s.width << 16);
        w.u32(s.height << 16);
        w.end(tkhd);
        const size_t mdia = w.begin("mdia");
        const size_t mdhd = w.full("mdhd", 0, 0);
        w.u32(0);
        w.u32(0);
        w.u32(s.timescale);
        w.u32(0);
        w.u16(0x55C4);   // "und"
        w.u16(0);
        w.end(mdhd);
        const size_t hdlr = w.full("hdlr", 0, 0);
        w.u32(0);
        w.fourcc(s.video ? "vide" : "soun");
        w.zeros(12);
        const char* name = s.video ? "Video" : "Audio";
        w.bytes(reinterpret_cast<const uint8_t*>(name), strlen(name) + 1);
        w.end(hdlr);
        const size_t minf = w.begin("minf");
        if (s.video) {
            const size_t vmhd = w.full("vmhd", 0, 1);
            w.zeros(8);
            w.end(vmhd);
        } else {
            const size_t smhd = w.full("smhd", 0, 0);
            w.zeros(4);
            w.end(smhd);
        }
        const size_t dinf = w.begin("dinf");
        const size_t dref = w.full("dref", 0, 0);
        w.u32(1);
        const size_t url = w.full("url ", 0, 1);
        w.end(url);
        w.end(dref);
        w.end(dinf);
        const size_t stbl = w.begin("stbl");
        const size_t stsd = w.full("stsd", 0, 0);
        w.u32(1);
        if (s.video) {
            const size_t e = w.begin("hev1");   // parameter sets may also be in-band
            w.zeros(6);
            w.u16(1);
            w.zeros(16);
            w.u16(s.width);
            w.u16(s.height);
            w.u32(0x00480000);
            w.u32(0x00480000);
            w.u32(0);
            w.u16(1);
            w.zeros(32);
            w.u16(0x0018);
            w.u16(0xFFFF);
            WriteHvcC(w, s.ps);
            const size_t colr = w.begin("colr");   // BT.709, or HDR10: BT.2020 primaries, PQ, BT.2020 matrix; limited range
            w.fourcc("nclx");
            w.u16(s.hdr ? 9 : 1);
            w.u16(s.hdr ? 16 : 1);
            w.u16(s.hdr ? 9 : 1);
            w.u8(0);
            w.end(colr);
            if (s.hdr) {
                // HDR10 static metadata players use for their tone mapping: a BT.2020 / D65 mastering display of
                // 0.005 to 1000 nits, content light up to 1000 nits (400 average). (ISO/IEC 14496-12 mdcv / clli.)
                const size_t mdcv = w.begin("mdcv");
                const uint16_t prim[6] = {8500, 39850, 6550, 2300, 35400, 14600};   // G, B, R (x, y) in 0.00002
                for (uint16_t v : prim) w.u16(v);
                w.u16(15635);   // white point D65 x
                w.u16(16450);   //                 y
                w.u32(1000 * 10000);   // max luminance, 0.0001 nits
                w.u32(50);             // min luminance
                w.end(mdcv);
                const size_t clli = w.begin("clli");
                w.u16(1000);   // MaxCLL
                w.u16(400);    // MaxFALL
                w.end(clli);
            }
            const size_t pasp = w.begin("pasp");
            w.u32(1);
            w.u32(1);
            w.end(pasp);
            w.end(e);
        } else {
            const std::vector<uint8_t> asc = mp4::AacConfig(s.rate, s.channels);
            const size_t e = w.begin("mp4a");
            w.zeros(6);
            w.u16(1);
            w.zeros(8);
            w.u16(s.channels);
            w.u16(16);
            w.u16(0);
            w.u16(0);
            w.u32(s.rate << 16);
            const size_t esds = w.full("esds", 0, 0);
            const uint8_t dsi = uint8_t(asc.size()), dcd = uint8_t(13 + 2 + dsi), es = uint8_t(3 + 2 + dcd + 3);
            w.u8(0x03);
            w.u8(es);
            w.u16(id);
            w.u8(0);
            w.u8(0x04);
            w.u8(dcd);
            w.u8(0x40);
            w.u8(0x15);
            w.u24(0);
            w.u32(192000);
            w.u32(192000);
            w.u8(0x05);
            w.u8(dsi);
            w.bytes(asc.data(), asc.size());
            w.u8(0x06);
            w.u8(1);
            w.u8(0x02);
            w.end(esds);
            w.end(e);
        }
        w.end(stsd);
        for (const char* t : {"stts", "stsc", "stco"}) {
            const size_t x = w.full(t, 0, 0);
            w.u32(0);
            w.end(x);
        }
        const size_t stsz = w.full("stsz", 0, 0);
        w.u32(0);
        w.u32(0);
        w.end(stsz);
        w.end(stbl);
        w.end(minf);
        w.end(mdia);
        w.end(trak);
    }

    // Writes one fragment with everything pending. Video keeps its newest sample back (its duration is
    // only known when the next one arrives) unless this is the end of the file.
    HRESULT Flush(bool final)
    {
        struct Run {
            uint32_t track;
            std::vector<Sample> samples;
        };
        std::vector<Run> runs;
        for (size_t i = 0; i < m_streams.size(); ++i) {
            Stream& s = m_streams[i];
            if (s.pending.empty()) continue;
            if (s.video) {
                for (size_t k = 0; k + 1 < s.pending.size(); ++k) {
                    const uint64_t next = s.pending[k + 1].dts;
                    s.pending[k].duration = next > s.pending[k].dts ? uint32_t(next - s.pending[k].dts) : 1;
                }
            }
            Run r{uint32_t(i + 1), {}};
            const size_t take = (s.video && !final) ? s.pending.size() - 1 : s.pending.size();
            if (take == 0) continue;
            r.samples.assign(std::make_move_iterator(s.pending.begin()), std::make_move_iterator(s.pending.begin() + ptrdiff_t(take)));
            s.pending.erase(s.pending.begin(), s.pending.begin() + ptrdiff_t(take));
            runs.push_back(std::move(r));
        }
        if (runs.empty()) return S_OK;
        Box w;
        const size_t moof = w.begin("moof");
        const size_t mfhd = w.full("mfhd", 0, 0);
        w.u32(++m_sequence);
        w.end(mfhd);
        std::vector<size_t> offsetAt;
        for (const auto& r : runs) {
            const size_t traf = w.begin("traf");
            const size_t tfhd = w.full("tfhd", 0, 0x020000);   // default-base-is-moof
            w.u32(r.track);
            w.end(tfhd);
            const size_t tfdt = w.full("tfdt", 1, 0);
            w.u64(r.samples.front().dts);
            w.end(tfdt);
            const size_t trun = w.full("trun", 0, 0x000001 | 0x000100 | 0x000200 | 0x000400);
            w.u32(uint32_t(r.samples.size()));
            offsetAt.push_back(w.b.size());
            w.u32(0);
            for (const auto& s : r.samples) {
                w.u32(s.duration);
                w.u32(uint32_t(s.data.size()));
                w.u32(s.key ? 0x02000000 : 0x01010000);
            }
            w.end(trun);
            w.end(traf);
        }
        w.end(moof);
        uint32_t offset = uint32_t(w.b.size() + 8);   // first byte after the mdat header
        for (size_t i = 0; i < runs.size(); ++i) {
            w.patch32(offsetAt[i], offset);
            for (const auto& s : runs[i].samples) offset += uint32_t(s.data.size());
        }
        const size_t mdat = w.begin("mdat");
        for (const auto& r : runs)
            for (const auto& s : r.samples) w.bytes(s.data.data(), s.data.size());
        w.end(mdat);
        return Write(w.b);
    }

    std::atomic<ULONG> m_ref{1};
    std::wstring m_path;
    HANDLE m_file = INVALID_HANDLE_VALUE;
    bool m_begun = false;
    uint32_t m_sequence = 0;
    std::vector<Stream> m_streams;
};

} // namespace

HRESULT CreateFragmentedMp4Writer(const std::wstring& path, IMFSinkWriter** writer)
{
    if (!writer) return E_POINTER;
    *writer = new Mp4Writer(path);
    return S_OK;
}

} // namespace cc
