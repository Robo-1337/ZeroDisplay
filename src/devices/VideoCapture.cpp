#include "devices/VideoCapture.h"
#include "core/Log.h"

#include <mferror.h>
#include <strmif.h>   // IAMVideoProcAmp

#include <algorithm>
#include <cmath>
#include <tuple>

namespace cc {

// ---------------------------------------------------------------------------------------------
// VideoFormat

static bool IsSupportedSubtype(const GUID& g)
{
    return g == MFVideoFormat_YUY2 || g == MFVideoFormat_UYVY || g == MFVideoFormat_NV12 || g == MFVideoFormat_MJPG ||
           g == MFVideoFormat_RGB32 || g == MFVideoFormat_ARGB32 || g == MFVideoFormat_P010 || g == MFVideoFormat_Y210;
}

bool VideoFormat::IsCompressed() const { return subtype == MFVideoFormat_MJPG; }

std::wstring VideoFormat::SubtypeName() const
{
    if (subtype == MFVideoFormat_YUY2) return L"YUY2";
    if (subtype == MFVideoFormat_UYVY) return L"UYVY";
    if (subtype == MFVideoFormat_NV12) return L"NV12";
    if (subtype == MFVideoFormat_MJPG) return L"MJPEG";
    if (subtype == MFVideoFormat_RGB32) return L"RGB32";
    if (subtype == MFVideoFormat_ARGB32) return L"ARGB32";
    if (subtype == MFVideoFormat_P010) return transfer == Transfer::Pq ? L"P010 HDR10" : (transfer == Transfer::Hlg ? L"P010 HLG" : L"P010");
    if (subtype == MFVideoFormat_Y210) return transfer == Transfer::Pq ? L"Y210 HDR10" : (transfer == Transfer::Hlg ? L"Y210 HLG" : L"Y210");
    wchar_t s[5] = {wchar_t(subtype.Data1 & 0xFF), wchar_t((subtype.Data1 >> 8) & 0xFF), wchar_t((subtype.Data1 >> 16) & 0xFF),
                    wchar_t((subtype.Data1 >> 24) & 0xFF), 0};
    return s;
}

std::wstring VideoFormat::Label() const
{
    double fps = Fps();
    double rounded = std::round(fps);
    std::wstring f = std::fabs(fps - rounded) < 0.01 ? Format(L"%.0f", rounded) : Format(L"%.2f", fps);
    return Format(L"%u×%u  %s fps  %s", width, height, f.c_str(), SubtypeName().c_str());
}

std::wstring VideoFormat::Key() const
{
    return Format(L"%s_%ux%u_%.2f", SubtypeName().c_str(), width, height, Fps());
}

bool VideoFormat::Matches(const VideoFormat& o) const
{
    return subtype == o.subtype && width == o.width && height == o.height && std::fabs(Fps() - o.Fps()) < 0.01;
}

static bool ParseType(IMFMediaType* type, VideoFormat& f)
{
    GUID major{};
    if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video) return false;
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &f.subtype))) return false;
    if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &f.width, &f.height))) return false;
    if (FAILED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &f.fpsNum, &f.fpsDen)) || f.fpsDen == 0) {
        f.fpsNum = 30;
        f.fpsDen = 1;
    }
    UINT32 range = 0;
    if (SUCCEEDED(type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range)))
        f.nativeRange = range == MFNominalRange_0_255 ? ColorRange::Full : (range == MFNominalRange_16_235 ? ColorRange::Limited : ColorRange::Auto);
    UINT32 tf = 0;
    if (SUCCEEDED(type->GetUINT32(MF_MT_TRANSFER_FUNCTION, &tf)))
        f.transfer = tf == MFVideoTransFunc_2084 ? Transfer::Pq : (tf == MFVideoTransFunc_HLG ? Transfer::Hlg : Transfer::Sdr);
    return true;
}

static VideoFormat ChooseDefault(const std::vector<VideoFormat>& formats, bool preferUncompressed)
{
    // Prefer >= 50 fps, then the largest frame, then the highest rate, then the preferred encoding, then packed YUV
    // (YUY2/UYVY: what UVC cards send uncompressed) over NV12, which, on cards that also offer MJPEG, is usually
    // Windows decoding the card's MJPEG (compressed quality, extra latency).
    auto score = [&](const VideoFormat& f) {
        bool fast = f.Fps() >= 49.0;
        bool pref = preferUncompressed ? !f.IsCompressed() : f.IsCompressed();
        bool packed = f.subtype == MFVideoFormat_YUY2 || f.subtype == MFVideoFormat_UYVY;
        return std::make_tuple(fast, uint64_t(f.width) * f.height, int(std::round(f.Fps() * 100)), pref, packed);
    };
    VideoFormat best = formats.front();
    for (const auto& f : formats)
        if (score(f) > score(best)) best = f;
    return best;
}

// ---------------------------------------------------------------------------------------------
// Source reader callback

class VideoCapture::Callback : public IMFSourceReaderCallback {
public:
    explicit Callback(VideoCapture* owner) : m_owner(owner) {}

    void Detach()
    {
        std::lock_guard lock(m_lock);
        m_owner = nullptr;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override
    {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback)) {
            *ppv = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE OnReadSample(HRESULT hr, DWORD, DWORD flags, LONGLONG ts, IMFSample* sample) override
    {
        std::lock_guard lock(m_lock);
        if (m_owner) m_owner->OnSample(hr, flags, ts, sample);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFlush(DWORD) override
    {
        std::lock_guard lock(m_lock);
        if (m_owner) m_owner->OnFlushDone();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    std::atomic<ULONG> m_ref{1};
    std::mutex m_lock;
    VideoCapture* m_owner;
};

// ---------------------------------------------------------------------------------------------

VideoCapture::VideoCapture(IMFDXGIDeviceManager* dxgi, HANDLE frameEvent) : m_dxgi(dxgi), m_frameEvent(frameEvent)
{
    m_flushEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_worker = std::thread([this] { WorkerLoop(); });
}

VideoCapture::~VideoCapture()
{
    Post([this] { DoClose(); });
    {
        std::lock_guard lock(m_queueMutex);
        m_quit = true;
    }
    m_queueCv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    CloseHandle(m_flushEvent);
}

void VideoCapture::Post(std::function<void()> fn)
{
    {
        std::lock_guard lock(m_queueMutex);
        m_queue.push_back(std::move(fn));
    }
    m_queueCv.notify_one();
}

void VideoCapture::WorkerLoop()
{
    ComInit com;
    SetCurrentThreadName(L"VideoCapture worker");
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock lock(m_queueMutex);
            m_queueCv.wait(lock, [&] { return m_quit || !m_queue.empty(); });
            if (m_queue.empty()) return;
            fn = std::move(m_queue.front());
            m_queue.pop_front();
        }
        m_busy = true;
        fn();
        m_busy = false;
    }
}

void VideoCapture::Open(const VideoDeviceInfo& info, const std::wstring& preferredKey, bool preferUncompressed)
{
    SetState(CaptureState::Opening, L"Opening " + info.name + L"…");
    Post([this, info, preferredKey, preferUncompressed] { DoOpen(info, preferredKey, preferUncompressed); });
}

void VideoCapture::SetFormat(const VideoFormat& format)
{
    Post([this, format] {
        if (!m_source) return;
        StopReader();
        LOGI(L"%s: switching to %s", m_info.name.c_str(), format.Label().c_str());
        if (!StartReader(format)) {
            LOGW(L"%s: format %s failed, reverting", m_info.name.c_str(), format.Label().c_str());
            VideoFormat prev;
            {
                std::lock_guard lock(m_infoMutex);
                prev = m_current;
            }
            if (!prev.Matches(format)) StartReader(prev);
        }
    });
}

void VideoCapture::Reopen()
{
    Post([this] {
        VideoFormat cur;
        {
            std::lock_guard lock(m_infoMutex);
            cur = m_current;
        }
        if (!m_source) return;
        StopReader();
        StartReader(cur);
    });
}

void VideoCapture::Close()
{
    Post([this] { DoClose(); });
}

void VideoCapture::SetState(CaptureState s, std::wstring text)
{
    std::lock_guard lock(m_infoMutex);
    m_state = s;
    m_status = std::move(text);
}

CaptureState VideoCapture::State() const
{
    std::lock_guard lock(m_infoMutex);
    return m_state;
}

std::wstring VideoCapture::StatusText() const
{
    std::lock_guard lock(m_infoMutex);
    return m_status;
}

std::vector<VideoFormat> VideoCapture::Formats() const
{
    std::lock_guard lock(m_infoMutex);
    return m_formats;
}

VideoFormat VideoCapture::Current() const
{
    std::lock_guard lock(m_infoMutex);
    return m_current;
}

CaptureStats VideoCapture::Stats() const
{
    std::lock_guard lock(m_infoMutex);
    return m_stats;
}

bool VideoCapture::TakeFrame(CapturedFrame& out)
{
    std::lock_guard lock(m_frameMutex);
    if (!m_latest.sample) return false;
    out = std::move(m_latest);
    m_latest = {};
    return true;
}

void VideoCapture::HandleError(HRESULT hr, const wchar_t* where)
{
    LOGE(L"%s: %s failed: %s", m_info.name.c_str(), where, HrToString(hr).c_str());
    switch (hr) {
    case E_ACCESSDENIED:
        SetState(CaptureState::Blocked, L"Camera access is blocked. Turn on Settings ▸ Privacy ▸ Camera ▸ Let desktop apps access your camera");
        break;
    case MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED:
    case HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION):
    case HRESULT_FROM_WIN32(ERROR_BUSY):
        SetState(CaptureState::InUse, L"Device is in use by another app");
        break;
    // A card another app streams from fails to start like this (an unplugged one is reported by Windows'
    // device-removal notice instead, which shows "Disconnected"): busy, not gone.
    case MF_E_HW_MFT_FAILED_START_STREAMING:
        SetState(CaptureState::InUse, L"Another app is using it (or the USB port can't carry another video stream)");
        break;
    case MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED:
    case HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED):
    case HRESULT_FROM_WIN32(ERROR_GEN_FAILURE):
    case HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND):
    case MF_E_SHUTDOWN:
        SetState(CaptureState::Lost, L"Device disconnected, waiting for it to come back");
        break;
    default:
        SetState(CaptureState::Error, std::wstring(where) + L" failed: " + HrToString(hr));
        break;
    }
}

void VideoCapture::DoOpen(VideoDeviceInfo info, std::wstring preferredKey, bool preferUncompressed)
{
    DoClose();
    m_info = info;
    SetState(CaptureState::Opening, L"Opening " + info.name + L"…");
    const int64_t t0 = QpcNow();

    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 2);
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, info.symlink.c_str());
    HRESULT hr = MFCreateDeviceSource(attr.Get(), &m_source);
    if (FAILED(hr)) {
        m_source.Reset();
        HandleError(hr, L"Open device");
        return;
    }
    if (!EnumerateFormats()) {
        SetState(CaptureState::Error, L"The device offers no supported video formats");
        return;
    }

    std::vector<VideoFormat> formats = Formats();
    VideoFormat pick = ChooseDefault(formats, preferUncompressed);
    if (!preferredKey.empty()) {
        for (const auto& f : formats)
            if (f.Key() == preferredKey) {
                pick = f;
                break;
            }
    }
    if (!StartReader(pick)) {
        VideoFormat fallback = ChooseDefault(formats, preferUncompressed);
        if (fallback.Matches(pick) || !StartReader(fallback)) {
            if (State() == CaptureState::Opening) SetState(CaptureState::Error, L"Could not start capture");
            return;
        }
    }
    LOGI(L"%s: opened in %.1f ms", info.name.c_str(), QpcElapsedMs(t0));
    m_link = QueryUsbLink(info.symlink);
    if (m_link != UsbLink::Unknown) LOGI(L"%s: connected over %s", info.name.c_str(), UsbLinkName(m_link));
    if (m_link == UsbLink::Usb2OnUsb3Device)
        LOGW(L"%s is a USB 3 device running at USB 2 speed: the fast uncompressed formats aren't offered", info.name.c_str());
    QueryPictureControls();
}

// Device picture adjustments (worker thread). The MF device source exposes the UVC processing unit
// through the DirectShow IAMVideoProcAmp interface.
void VideoCapture::QueryPictureControls()
{
    std::vector<PictureControl> list;
    ComPtr<IAMVideoProcAmp> amp;
    if (m_source && SUCCEEDED(m_source.As(&amp))) {
        for (int id = VideoProcAmp_Brightness; id <= VideoProcAmp_Gain; ++id) {
            PictureControl c;
            c.id = id;
            long flags = 0, cur = 0, curFlags = 0;
            if (FAILED(amp->GetRange(id, &c.min, &c.max, &c.step, &c.def, &flags)) || c.max <= c.min) continue;
            if (SUCCEEDED(amp->Get(id, &cur, &curFlags))) c.value = cur;
            else c.value = c.def;
            c.step = std::max(1L, c.step);
            c.canAuto = (flags & VideoProcAmp_Flags_Auto) != 0;
            c.autoOn = (curFlags & VideoProcAmp_Flags_Auto) != 0;
            list.push_back(c);
        }
    }
    std::wstring names;
    for (const auto& c : list) names += Format(L" %d[%ld..%ld=%ld]", c.id, c.min, c.max, c.value);
    LOGI(L"%s: %zu picture control(s)%s", m_info.name.c_str(), list.size(), names.c_str());
    std::lock_guard lock(m_infoMutex);
    m_controls = std::move(list);
    ++m_controlsGen;
}

std::vector<PictureControl> VideoCapture::PictureControls(uint64_t* generation) const
{
    std::lock_guard lock(m_infoMutex);
    if (generation) *generation = m_controlsGen;
    return m_controls;
}

void VideoCapture::SetPictureControl(int id, long value, bool autoOn)
{
    {
        std::lock_guard lock(m_infoMutex);
        m_pendingControls[id] = {value, autoOn};
        for (auto& c : m_controls)
            if (c.id == id) {
                c.value = value;
                c.autoOn = autoOn;
            }
        if (m_controlsTaskQueued) return;   // slider drags: one task applies the latest values
        m_controlsTaskQueued = true;
    }
    Post([this] { ApplyPendingControls(); });
}

void VideoCapture::ApplyPendingControls()
{
    std::map<int, std::pair<long, bool>> pending;
    {
        std::lock_guard lock(m_infoMutex);
        pending.swap(m_pendingControls);
        m_controlsTaskQueued = false;
    }
    ComPtr<IAMVideoProcAmp> amp;
    if (!m_source || FAILED(m_source.As(&amp))) return;
    for (const auto& [id, v] : pending) {
        HRESULT hr = amp->Set(id, v.first, v.second ? VideoProcAmp_Flags_Auto : VideoProcAmp_Flags_Manual);
        if (FAILED(hr)) LOGW(L"%s: picture control %d = %ld failed: %s", m_info.name.c_str(), id, v.first, HrToString(hr).c_str());
    }
}

void VideoCapture::DoClose()
{
    StopReader();
    m_handler.Reset();
    m_nativeTypes.clear();
    if (m_source) {
        m_source->Shutdown();
        m_source.Reset();
    }
    SetState(CaptureState::Closed, L"Closed");
}

bool VideoCapture::EnumerateFormats()
{
    ComPtr<IMFPresentationDescriptor> pd;
    if (FAILED(m_source->CreatePresentationDescriptor(&pd))) return false;
    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> sd;
    if (FAILED(pd->GetStreamDescriptorByIndex(0, &selected, &sd))) return false;
    if (FAILED(sd->GetMediaTypeHandler(&m_handler))) return false;

    DWORD count = 0;
    m_handler->GetMediaTypeCount(&count);
    std::vector<VideoFormat> formats;
    m_nativeTypes.clear();
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IMFMediaType> type;
        if (FAILED(m_handler->GetMediaTypeByIndex(i, &type))) continue;
        VideoFormat f;
        if (!ParseType(type.Get(), f) || !IsSupportedSubtype(f.subtype)) continue;
        bool dup = false;
        for (const auto& e : formats)
            if (e.Matches(f)) dup = true;
        if (dup) continue;
        formats.push_back(f);
        m_nativeTypes.push_back(type);
    }

    // Sort for the UI: largest first, then fps, uncompressed before MJPEG. Keep types aligned.
    std::vector<size_t> order(formats.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const auto& x = formats[a];
        const auto& y = formats[b];
        uint64_t ax = uint64_t(x.width) * x.height, ay = uint64_t(y.width) * y.height;
        if (ax != ay) return ax > ay;
        if (std::fabs(x.Fps() - y.Fps()) > 0.01) return x.Fps() > y.Fps();
        return !x.IsCompressed() && y.IsCompressed();
    });
    std::vector<VideoFormat> sortedFormats;
    std::vector<ComPtr<IMFMediaType>> sortedTypes;
    for (size_t i : order) {
        sortedFormats.push_back(formats[i]);
        sortedTypes.push_back(m_nativeTypes[i]);
    }
    m_nativeTypes = std::move(sortedTypes);

    LOGI(L"%s: %zu formats", m_info.name.c_str(), sortedFormats.size());
    std::lock_guard lock(m_infoMutex);
    m_formats = std::move(sortedFormats);
    return !m_formats.empty();
}

bool VideoCapture::StartReader(const VideoFormat& format)
{
    std::vector<VideoFormat> formats = Formats();
    ComPtr<IMFMediaType> native;
    for (size_t i = 0; i < formats.size(); ++i)
        if (formats[i].Matches(format)) native = m_nativeTypes[i];
    if (!native) return false;

    HRESULT hr = m_handler->SetCurrentMediaType(native.Get());
    if (FAILED(hr)) {
        HandleError(hr, L"Set device format");
        return false;
    }

    const bool compressed = format.IsCompressed();
    // Attempt 0 hands the source our D3D device: MJPEG is decoded on the GPU and uncompressed frames
    // arrive as textures (no CPU copy). Then CPU fallbacks: 1 = plain, 2 = plus video processor (MJPEG only).
    const int attempts = compressed ? 3 : 2;
    ComPtr<IMFSourceReader> reader;
    ComPtr<Callback> callback;
    for (int attempt = 0; attempt < attempts && !reader; ++attempt) {
        if (attempt == 0 && !m_dxgi) continue;
        callback.Attach(new Callback(this));
        ComPtr<IMFAttributes> a;
        MFCreateAttributes(&a, 6);
        a->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback.Get());
        a->SetUINT32(MF_LOW_LATENCY, TRUE);
        a->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE);
        if (!compressed) a->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
        if (attempt == 0) {
            a->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, m_dxgi.Get());
            if (compressed) a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        } else if (attempt == 2) {
            a->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        }

        ComPtr<IMFSourceReader> r;
        hr = MFCreateSourceReaderFromMediaSource(m_source.Get(), a.Get(), &r);
        if (FAILED(hr)) {
            LOGW(L"%s: create reader (attempt %d) failed: %s", m_info.name.c_str(), attempt, HrToString(hr).c_str());
            callback->Detach();
            continue;
        }
        r->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        r->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

        if (!compressed) {
            hr = r->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, native.Get());
        } else {
            hr = E_FAIL;
            for (const GUID& sub : {MFVideoFormat_NV12, MFVideoFormat_YUY2, MFVideoFormat_RGB32}) {
                ComPtr<IMFMediaType> out;
                MFCreateMediaType(&out);
                out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                out->SetGUID(MF_MT_SUBTYPE, sub);
                MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, format.width, format.height);
                hr = r->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, out.Get());
                if (SUCCEEDED(hr)) break;
            }
        }
        if (FAILED(hr)) {
            LOGW(L"%s: set output type (attempt %d) failed: %s", m_info.name.c_str(), attempt, HrToString(hr).c_str());
            callback->Detach();
            continue;
        }
        reader = r;
        LOGI(L"%s: %s path %d (%s)", m_info.name.c_str(), compressed ? L"MJPEG decode" : L"frame", attempt, attempt == 0 ? L"GPU" : L"CPU");
    }
    if (!reader) {
        SetState(CaptureState::Error, L"Could not configure " + format.Label());
        return false;
    }

    {
        std::lock_guard lock(m_readerMutex);
        m_reader = reader;
        m_callback = callback;
        m_stopping = false;
    }
    {
        std::lock_guard lock(m_infoMutex);
        m_current = format;
        m_stats = {};
        m_fpsWindowStart = QpcNow();
        m_fpsWindowFrames = 0;
    }
    UpdateOutputType();
    m_lastFrameQpc = QpcNow();

    {
        std::lock_guard lock(m_readerMutex);
        hr = m_reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, nullptr, nullptr, nullptr);
    }
    if (FAILED(hr)) {
        HandleError(hr, L"Start streaming");
        StopReader();
        return false;
    }
    SetState(CaptureState::Running, L"");
    LOGI(L"%s: streaming %s", m_info.name.c_str(), format.Label().c_str());
    return true;
}

void VideoCapture::StopReader()
{
    ComPtr<IMFSourceReader> reader;
    {
        std::lock_guard lock(m_readerMutex);
        reader = m_reader;
    }
    if (!reader) return;

    HRESULT hr;
    {
        std::lock_guard lock(m_readerMutex);
        m_stopping = true;
        ResetEvent(m_flushEvent);
        hr = reader->Flush(DWORD(MF_SOURCE_READER_ALL_STREAMS));
    }
    if (SUCCEEDED(hr)) WaitForSingleObject(m_flushEvent, 2000);
    if (m_callback) m_callback->Detach();
    {
        std::lock_guard lock(m_readerMutex);
        m_reader.Reset();
    }
    reader.Reset();
    m_callback.Reset();
    std::lock_guard lock(m_frameMutex);
    m_latest = {};
}

void VideoCapture::UpdateOutputType()
{
    ComPtr<IMFSourceReader> reader;
    {
        std::lock_guard lock(m_readerMutex);
        reader = m_reader;
    }
    if (!reader) return;
    ComPtr<IMFMediaType> type;
    if (FAILED(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type))) return;

    CapturedFrame t;
    GUID sub{};
    type->GetGUID(MF_MT_SUBTYPE, &sub);
    if (sub == MFVideoFormat_YUY2) t.layout = PixelLayout::YUY2;
    else if (sub == MFVideoFormat_UYVY) t.layout = PixelLayout::UYVY;
    else if (sub == MFVideoFormat_NV12) t.layout = PixelLayout::NV12;
    else if (sub == MFVideoFormat_RGB32 || sub == MFVideoFormat_ARGB32) t.layout = PixelLayout::BGRA;
    else if (sub == MFVideoFormat_P010) t.layout = PixelLayout::P010;
    else if (sub == MFVideoFormat_Y210) t.layout = PixelLayout::Y210;
    MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &t.width, &t.height);

    UINT32 stride = 0;
    if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
        t.stride = LONG(stride);
    } else {
        LONG s = 0;
        if (SUCCEEDED(MFGetStrideForBitmapInfoHeader(sub.Data1, t.width, &s))) t.stride = s;
    }
    UINT32 v = 0;
    if (SUCCEEDED(type->GetUINT32(MF_MT_YUV_MATRIX, &v)))
        t.matrix = v == MFVideoTransferMatrix_BT709                                                    ? ColorMatrix::BT709
                   : v == MFVideoTransferMatrix_BT601                                                  ? ColorMatrix::BT601
                   : (v == MFVideoTransferMatrix_BT2020_10 || v == MFVideoTransferMatrix_BT2020_12) ? ColorMatrix::BT2020
                                                                                                       : ColorMatrix::Auto;
    if (SUCCEEDED(type->GetUINT32(MF_MT_TRANSFER_FUNCTION, &v)))
        t.transfer = v == MFVideoTransFunc_2084 ? Transfer::Pq : (v == MFVideoTransFunc_HLG ? Transfer::Hlg : Transfer::Sdr);
    if (SUCCEEDED(type->GetUINT32(MF_MT_VIDEO_PRIMARIES, &v))) t.bt2020 = v == MFVideoPrimaries_BT2020;
    if (SUCCEEDED(type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &v)))
        t.range = v == MFNominalRange_0_255 ? ColorRange::Full : (v == MFNominalRange_16_235 ? ColorRange::Limited : ColorRange::Auto);

    VideoFormat cur = Current();
    t.decoded = cur.IsCompressed();
    if (t.range == ColorRange::Auto) t.range = cur.nativeRange;

    std::lock_guard lock(m_frameMutex);
    m_out = t;
}

void VideoCapture::OnFlushDone() { SetEvent(m_flushEvent); }

void VideoCapture::OnSample(HRESULT hr, DWORD flags, LONGLONG timestamp, IMFSample* sample)
{
    if (FAILED(hr)) {
        HandleError(hr, L"Read frame");
        return;
    }
    if (flags & MF_SOURCE_READERF_ERROR) {
        HandleError(MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED, L"Read frame");
        return;
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        SetState(CaptureState::Lost, L"Device stopped streaming");
        return;
    }
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) UpdateOutputType();

    const int64_t now = QpcNow();
    if (flags & MF_SOURCE_READERF_STREAMTICK) {
        std::lock_guard lock(m_infoMutex);
        m_stats.ticks++;
    }
    if (sample) {
        bool replaced = false;
        {
            std::lock_guard lock(m_frameMutex);
            CapturedFrame f = m_out;
            f.sample = sample;
            f.arrivalQpc = now;
            f.sampleTime = timestamp;
            f.seq = ++m_seq;
            replaced = m_latest.sample != nullptr;
            m_latest = std::move(f);
        }
        SetEvent(m_frameEvent);
        m_lastFrameQpc = now;

        std::lock_guard lock(m_infoMutex);
        m_stats.frames++;
        if (replaced) m_stats.replaced++;
        m_fpsWindowFrames++;
        double elapsed = QpcToMs(now - m_fpsWindowStart);
        if (elapsed >= 1000.0) {
            m_stats.fps = m_fpsWindowFrames * 1000.0 / elapsed;
            m_fpsWindowFrames = 0;
            m_fpsWindowStart = now;
        }
    }

    std::lock_guard lock(m_readerMutex);
    if (!m_stopping && m_reader) {
        HRESULT h2 = m_reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, nullptr, nullptr, nullptr);
        if (FAILED(h2)) HandleError(h2, L"Request frame");
    }
}

} // namespace cc
