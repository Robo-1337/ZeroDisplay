#include "encode/FileTrimmer.h"
#include "core/Log.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <algorithm>
#include <deque>

namespace cc {

static double FileDuration(IMFSourceReader* r)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    double d = 0;
    if (SUCCEEDED(r->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &v)) && v.vt == VT_UI8)
        d = double(v.uhVal.QuadPart) / 1e7;
    PropVariantClear(&v);
    return d;
}

static void SeekTo(IMFSourceReader* r, double seconds)
{
    PROPVARIANT v;
    InitPropVariantFromInt64(int64_t(seconds * 1e7), &v);
    r->SetCurrentPosition(GUID_NULL, v);
    PropVariantClear(&v);
}

bool TrimFile(const std::wstring& in, const std::wstring& out, double start, double end, std::wstring& error, double* actualStart)
{
    ComInit com;
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(in.c_str(), nullptr, &reader);
    if (FAILED(hr)) {
        error = L"Can't open the file: " + HrToString(hr);
        return false;
    }
    ComPtr<IMFAttributes> wa;
    MFCreateAttributes(&wa, 2);
    wa->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    wa->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    ComPtr<IMFSinkWriter> writer;
    hr = MFCreateSinkWriterFromURL(out.c_str(), nullptr, wa.Get(), &writer);
    if (FAILED(hr)) {
        error = L"Can't create the output file: " + HrToString(hr);
        return false;
    }

    // Copy every video and audio stream as it is.
    struct Stream { DWORD in = 0, out = 0; bool video = false; bool done = false; };
    std::vector<Stream> streams;
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        hr = reader->GetNativeMediaType(i, 0, &t);
        if (hr == MF_E_INVALIDSTREAMNUMBER) break;
        if (FAILED(hr)) continue;
        GUID major{};
        t->GetGUID(MF_MT_MAJOR_TYPE, &major);
        if (major != MFMediaType_Video && major != MFMediaType_Audio) continue;
        Stream s;
        s.in = i;
        s.video = major == MFMediaType_Video;
        if (FAILED(reader->SetStreamSelection(i, TRUE)) || FAILED(reader->SetCurrentMediaType(i, nullptr, t.Get())) ||
            FAILED(writer->AddStream(t.Get(), &s.out)) || FAILED(writer->SetInputMediaType(s.out, t.Get(), nullptr))) {
            LOGW(L"Trim: stream %u can't be copied; skipped", i);
            reader->SetStreamSelection(i, FALSE);
            continue;
        }
        streams.push_back(s);
    }
    if (std::none_of(streams.begin(), streams.end(), [](const Stream& s) { return s.video; })) {
        error = L"The file has no video that can be copied";
        return false;
    }
    if (FAILED(writer->BeginWriting())) {
        error = L"Can't start writing";
        return false;
    }

    // Seek a few seconds early (more than a GOP), then keep everything since the last video keyframe until
    // the start is reached, so the cut begins on a keyframe. Seeking straight to `start` would put the
    // video on the keyframe before it but the audio exactly at `start` (audio missing at the beginning).
    SeekTo(reader.Get(), std::max(0.0, start - 5.0));
    const int64_t startT = int64_t(start * 1e7), endT = int64_t(end * 1e7);
    struct Pending { size_t stream; ComPtr<IMFSample> sample; int64_t t; };
    std::deque<Pending> held;
    int64_t base = -1;
    size_t written = 0;
    auto write = [&](size_t si, IMFSample* s, int64_t t) {
        s->SetSampleTime(t - base);
        if (SUCCEEDED(writer->WriteSample(streams[si].out, s))) ++written;
    };
    for (;;) {
        DWORD idx = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(DWORD(MF_SOURCE_READER_ANY_STREAM), 0, &idx, &flags, &ts, &sample);
        if (FAILED(hr)) {
            error = L"Reading failed: " + HrToString(hr);
            break;
        }
        size_t si = streams.size();
        for (size_t k = 0; k < streams.size(); ++k)
            if (streams[k].in == idx) si = k;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (si < streams.size()) streams[si].done = true;
        }
        if (sample && si < streams.size() && !streams[si].done) {
            if (ts > endT) {
                streams[si].done = true;
            } else if (base < 0) {
                UINT32 key = 0;
                sample->GetUINT32(MFSampleExtension_CleanPoint, &key);
                if (streams[si].video && key) std::erase_if(held, [&](const Pending& h) { return h.t < ts; });   // keep what belongs after the latest keyframe (audio can come first in the file)
                held.push_back({si, sample, ts});
                if (streams[si].video && ts >= startT) {
                    // Start reached: the cut begins at the first held video keyframe.
                    for (const auto& h : held)
                        if (streams[h.stream].video) {
                            base = h.t;
                            break;
                        }
                    for (auto& h : held)
                        if (h.t >= base) write(h.stream, h.sample.Get(), h.t);
                    held.clear();
                }
            } else if (ts >= base) {
                write(si, sample.Get(), ts);
            }
        }
        if (std::all_of(streams.begin(), streams.end(), [](const Stream& s) { return s.done; })) break;
    }
    hr = writer->Finalize();
    if (FAILED(hr) && error.empty()) error = L"Finalizing failed: " + HrToString(hr);
    if (actualStart) *actualStart = base >= 0 ? double(base) / 1e7 : start;
    LOGI(L"Trim %s [%.2f..%.2f] -> %s: %zu samples, cut starts at %.2f s %s", in.c_str(), start, end, out.c_str(), written,
         base >= 0 ? double(base) / 1e7 : -1.0, error.c_str());
    return error.empty() && written > 0;
}

// ---------------------------------------------------------------------------------------------

bool TrimPreview::Open(const std::wstring& path)
{
    Close();
    m_path = path;
    ComPtr<IMFSourceReader> r;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &r))) return false;
    m_duration = FileDuration(r.Get());
    m_quit = false;
    m_request = -1;
    m_ready = false;
    m_thread = std::thread([this] { ThreadMain(); });
    return m_duration > 0;
}

void TrimPreview::Close()
{
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void TrimPreview::Request(double seconds)
{
    {
        std::lock_guard lock(m_mutex);
        m_request = seconds;
    }
    m_cv.notify_all();
}

bool TrimPreview::Take(std::vector<uint8_t>& bgra, UINT& width, UINT& height)
{
    std::lock_guard lock(m_mutex);
    if (!m_ready) return false;
    bgra.swap(m_image);
    width = m_w;
    height = m_h;
    m_ready = false;
    return true;
}

void TrimPreview::ThreadMain()
{
    ComInit com;
    SetCurrentThreadName(L"Trim preview");
    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 1);
    a->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);   // decode + convert to RGB32 for us
    ComPtr<IMFSourceReader> r;
    if (FAILED(MFCreateSourceReaderFromURL(m_path.c_str(), a.Get(), &r))) return;
    r->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    r->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    ComPtr<IMFMediaType> rgb;
    MFCreateMediaType(&rgb);
    rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(r->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, rgb.Get()))) {
        LOGW(L"Trim preview: this file's video can't be decoded");
        return;
    }
    ComPtr<IMFMediaType> cur;
    r->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &cur);
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
    for (;;) {
        double want;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [&] { return m_quit || m_request >= 0; });
            if (m_quit) return;
            want = m_request;
            m_request = -1;
        }
        SeekTo(r.Get(), want);
        const int64_t target = int64_t(want * 1e7);
        ComPtr<IMFSample> last;
        for (int n = 0; n < 600; ++n) {   // decode forward from the keyframe to the requested time
            DWORD idx = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> s;
            if (FAILED(r->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &idx, &flags, &ts, &s))) break;
            if (s) last = s;
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) || ts >= target) break;
            std::lock_guard lock(m_mutex);
            if (m_request >= 0 || m_quit) break;   // a newer request: give up on this one
        }
        ComPtr<IMFMediaBuffer> buf;
        if (!last || FAILED(last->ConvertToContiguousBuffer(&buf))) continue;
        std::vector<uint8_t> img(size_t(w) * h * 4);
        ComPtr<IMF2DBuffer> b2;
        BYTE* p = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&p, &pitch))) {
            for (UINT y = 0; y < h; ++y) memcpy(&img[size_t(y) * w * 4], p + ptrdiff_t(y) * pitch, size_t(w) * 4);
            b2->Unlock2D();
        } else {
            DWORD len = 0;
            if (FAILED(buf->Lock(&p, nullptr, &len))) continue;
            memcpy(img.data(), p, std::min<size_t>(len, img.size()));
            buf->Unlock();
        }
        for (size_t i = 3; i < img.size(); i += 4) img[i] = 255;   // RGB32 has no alpha
        std::lock_guard lock(m_mutex);
        m_image.swap(img);
        m_w = w;
        m_h = h;
        m_ready = true;
    }
}

} // namespace cc
