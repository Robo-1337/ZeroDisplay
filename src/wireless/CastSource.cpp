#include "wireless/CastSource.h"
#include "audio/AudioSource.h"
#include "wireless/CastAudioTap.h"
#include "core/Log.h"

#include <mfapi.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Media.h>
#include <MemoryBuffer.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace cc {

namespace wmc = winrt::Windows::Media::Core;
namespace wmp = winrt::Windows::Media::Playback;

struct CastSource::Impl : std::enable_shared_from_this<CastSource::Impl> {
    HANDLE frameEvent = nullptr;
    std::shared_ptr<AudioSource> audio;
    ComPtr<ID3D11Device> device;
    wmc::MediaSource source{nullptr};
    std::wstring name;

    std::thread starter;
    std::mutex mutex;   // player, textures, latest frame, stats
    wmp::MediaPlayer player{nullptr};
    winrt::event_token onFrame{}, onOpened{}, onFailed{};

    static constexpr int kRing = 4;   // the renderer copies a frame before the ring comes round again
    struct Slot {
        ComPtr<ID3D11Texture2D> tex;
        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface surface{nullptr};
    };
    Slot ring[kRing];
    int next = 0;
    UINT w = 0, h = 0;
    CapturedFrame latest;
    bool hasNew = false;
    uint64_t seq = 0, frames = 0, replaced = 0;
    double fps = 0;
    int64_t statStart = 0;
    uint64_t statFrames = 0;
    std::atomic<int64_t> lastFrameQpc{0};
    std::atomic<CaptureState> state{CaptureState::Closed};
    std::wstring status = L"Waiting for the picture…";
    std::atomic<bool> closing{false};
    int tapId = 0;           // CastAudioTap feeding `audio`
    int64_t openedQpc = 0;   // stream opened (the tap must deliver within ~2 s, else Windows plays the sound)
    bool tapChecked = false;

    void Start();
    void OnFrame();
};

CastSource::CastSource(HANDLE frameEvent, std::shared_ptr<AudioSource> audio, ID3D11Device* device) : m(std::make_shared<Impl>())
{
    m->frameEvent = frameEvent;
    m->audio = std::move(audio);
    m->device = device;
}

CastSource::~CastSource() { Close(); }

void CastSource::SetMediaSource(IUnknown* mediaSource)
{
    if (!mediaSource) return;
    winrt::Windows::Foundation::IInspectable insp{nullptr};
    winrt::copy_from_abi(insp, mediaSource);
    m->source = insp.as<wmc::MediaSource>();
}

void CastSource::Reconnect(IUnknown* mediaSource, const VideoDeviceInfo& info)
{
    Close();
    {
        std::lock_guard lock(m->mutex);
        m->closing = false;
        m->tapChecked = false;
        m->openedQpc = 0;
        m->status = L"Waiting for the picture…";
    }
    SetMediaSource(mediaSource);
    Open(info, L"", false);
}

ComPtr<IUnknown> CastSource::MediaSourceFromFile(const std::wstring& path)
{
    try {
        wchar_t full[MAX_PATH] = {};
        GetFullPathNameW(path.c_str(), MAX_PATH, full, nullptr);
        std::wstring uri = L"file:///" + std::wstring(full);
        for (auto& c : uri)
            if (c == L'\\') c = L'/';
        wmc::MediaSource s = wmc::MediaSource::CreateFromUri(winrt::Windows::Foundation::Uri(uri));
        ComPtr<IUnknown> unk;
        unk.Attach(static_cast<IUnknown*>(winrt::detach_abi(s)));
        return unk;
    } catch (const winrt::hresult_error& e) {
        LOGE(L"Media test: %s: 0x%08X", path.c_str(), unsigned(e.code()));
        return nullptr;
    }
}

void CastSource::Open(const VideoDeviceInfo& info, const std::wstring&, bool)
{
    m->name = info.name;
    if (!m->source) {
        m->state = CaptureState::Error;
        m->status = L"No stream";
        return;
    }
    if (m->starter.joinable()) return;
    m->state = CaptureState::Opening;
    // Creating the player waits on async operations: never on the render thread.
    m->starter = std::thread([this] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        SetCurrentThreadName(L"Cast start");
        m->Start();
    });
}

void CastSource::Impl::Start()
{
    try {
        // A media source takes only one consumer (0xC00D36DA), so the sound can't be read separately (an
        // AudioGraph): the player decodes it and our audio effect taps it.
        wmp::MediaPlayer p;
        p.RealTimePlayback(true);
        p.AutoPlay(true);
        p.IsVideoFrameServerEnabled(true);
        p.CommandManager().IsEnabled(false);   // not a media app for the system transport controls
        // Sound: our audio effect copies what the player decodes into the mixer; the player itself is silent.
        if (audio) {
            tapId = casttap::Register(audio);
            try {
                winrt::Windows::Foundation::Collections::PropertySet config;
                config.Insert(L"tap", winrt::box_value(int32_t(tapId)));
                p.AddAudioEffect(casttap::kClassId, false, config);
                p.Volume(0.0);
            } catch (const winrt::hresult_error& e) {
                LOGW(L"Cast %s: audio tap unavailable (0x%08X); Windows plays the sound", name.c_str(), unsigned(e.code()));
                casttap::Unregister(tapId);
                tapId = 0;
            }
        }
        // The player's events arrive on its own threads, also while the cast is closing: the handlers hold this state
        // weakly, are removed in Close, and never let a WinRT error escape.
        const std::weak_ptr<Impl> weak = weak_from_this();
        onFrame = p.VideoFrameAvailable([weak](const wmp::MediaPlayer&, const winrt::Windows::Foundation::IInspectable&) {
            const auto s = weak.lock();
            if (!s || s->closing) return;
            try {
                s->OnFrame();
            } catch (const winrt::hresult_error&) {   // the player is going away
            }
        });
        onOpened = p.MediaOpened([weak](const wmp::MediaPlayer& mp, const winrt::Windows::Foundation::IInspectable&) {
            const auto s = weak.lock();
            if (!s || s->closing) return;
            try {
                LOGI(L"Cast %s: opened, %ux%u", s->name.c_str(), mp.PlaybackSession().NaturalVideoWidth(), mp.PlaybackSession().NaturalVideoHeight());
                s->openedQpc = QpcNow();
                mp.Play();   // a Play() before the stream opened is ignored
            } catch (const winrt::hresult_error& e) {
                LOGW(L"Cast %s: opened event: 0x%08X", s->name.c_str(), unsigned(e.code()));
            }
        });
        onFailed = p.MediaFailed([weak](const wmp::MediaPlayer&, const wmp::MediaPlayerFailedEventArgs& a) {
            const auto s = weak.lock();
            if (!s || s->closing) return;
            try {
                LOGE(L"Cast %s: playback failed: 0x%08X %s", s->name.c_str(), unsigned(a.ExtendedErrorCode()), a.ErrorMessage().c_str());
                std::lock_guard lock(s->mutex);
                s->status = a.ExtendedErrorCode() == HRESULT(0xC00D4278)
                                ? L"No picture arrives, Windows Firewall may be blocking casting (see the Wireless tab)"
                                : L"Playback failed";
                s->state = CaptureState::Error;
            } catch (const winrt::hresult_error& e) {
                LOGW(L"Cast %s: failed event: 0x%08X", s->name.c_str(), unsigned(e.code()));
            }
        });
        {
            std::lock_guard lock(mutex);
            if (closing) {
                p.VideoFrameAvailable(onFrame);
                p.MediaOpened(onOpened);
                p.MediaFailed(onFailed);
                return;
            }
            player = p;
        }
        p.Source(wmp::MediaPlaybackItem(source));
        p.Play();
        LOGI(L"Cast %s: playing (%s)", name.c_str(), tapId ? L"sound through the mixer" : L"sound played by Windows");
    } catch (const winrt::hresult_error& e) {
        LOGE(L"Cast %s: start failed: 0x%08X %s", name.c_str(), unsigned(e.code()), e.message().c_str());
        state = CaptureState::Error;
    }
}

void CastSource::Impl::OnFrame()
{
    std::lock_guard lock(mutex);
    if (!player || closing) return;
    const auto ps = player.PlaybackSession();
    const UINT nw = ps.NaturalVideoWidth(), nh = ps.NaturalVideoHeight();
    if (!nw || !nh) return;
    if (nw != w || nh != h) {
        for (auto& s : ring) s = Slot{};
        w = nw;
        h = nh;
        LOGI(L"Cast %s: video %ux%u", name.c_str(), w, h);
    }
    Slot& slot = ring[next];
    next = (next + 1) % kRing;
    if (!slot.tex) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &slot.tex))) return;
        ComPtr<IDXGISurface> dxgi;
        slot.tex.As(&dxgi);
        winrt::com_ptr<::IInspectable> insp;
        if (FAILED(CreateDirect3D11SurfaceFromDXGISurface(dxgi.Get(), insp.put()))) return;
        slot.surface = insp.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface>();
    }
    try {
        player.CopyFrameToVideoSurface(slot.surface);
    } catch (...) {
        return;
    }
    ComPtr<IMFMediaBuffer> mb;
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), slot.tex.Get(), 0, FALSE, &mb)) || FAILED(MFCreateSample(&sample))) return;
    sample->AddBuffer(mb.Get());
    const int64_t now = QpcNow();
    if (hasNew) ++replaced;
    latest = CapturedFrame{};
    latest.sample = sample;
    latest.layout = PixelLayout::BGRA;
    latest.width = w;
    latest.height = h;
    latest.arrivalQpc = now;
    latest.sampleTime = QpcTo100ns(now);
    latest.seq = ++seq;
    hasNew = true;
    ++frames;
    ++statFrames;
    if (!statStart) statStart = now;
    const double el = QpcToMs(now - statStart);
    if (el >= 1000.0) {
        fps = statFrames * 1000.0 / el;
        statFrames = 0;
        statStart = now;
    }
    lastFrameQpc = now;
    if (tapId) casttap::SetClock(tapId, ps.Position().count(), now);   // what is on screen now
    if (tapId && !tapChecked && openedQpc && QpcToMs(now - openedQpc) > 2000.0) {
        tapChecked = true;
        const uint64_t got = casttap::Frames(tapId);
        LOGI(L"Cast %s: audio tap delivered %llu frames", name.c_str(), (unsigned long long)got);
        if (got == 0) {   // the effect didn't load (e.g. the exe was renamed): let Windows play the sound
            player.Volume(1.0);
            LOGW(L"Cast %s: no sound through the tap; Windows plays it (not in recordings)", name.c_str());
        }
    }
    state = CaptureState::Running;
    status = Format(L"%ux%u", w, h);
    if (frameEvent) SetEvent(frameEvent);
}

void CastSource::Ended()
{
    std::lock_guard lock(m->mutex);
    m->state = CaptureState::Lost;
    m->status = L"Disconnected";
}

void CastSource::Close()
{
    m->closing = true;
    if (m->starter.joinable()) m->starter.join();
    wmp::MediaPlayer p{nullptr};
    {
        std::lock_guard lock(m->mutex);
        p = std::exchange(m->player, nullptr);
    }
    try {
        if (p) {
            p.VideoFrameAvailable(m->onFrame);
            p.MediaOpened(m->onOpened);
            p.MediaFailed(m->onFailed);
            p.Close();
        }
    } catch (...) {
    }
    if (m->tapId) casttap::Unregister(std::exchange(m->tapId, 0));
    m->state = CaptureState::Closed;
}

bool CastSource::TakeFrame(CapturedFrame& out)
{
    std::lock_guard lock(m->mutex);
    if (!m->hasNew) return false;
    out = m->latest;
    m->hasNew = false;
    return true;
}

CaptureState CastSource::State() const { return m->state.load(); }

std::wstring CastSource::StatusText() const
{
    std::lock_guard lock(m->mutex);
    return m->status;
}

VideoFormat CastSource::Current() const
{
    std::lock_guard lock(m->mutex);
    VideoFormat f;
    f.subtype = MFVideoFormat_ARGB32;
    f.width = m->w;
    f.height = m->h;
    f.fpsNum = UINT32(std::lround(m->fps > 1 ? m->fps : 60));
    f.fpsDen = 1;
    return f;
}

CaptureStats CastSource::Stats() const
{
    std::lock_guard lock(m->mutex);
    CaptureStats s;
    s.fps = m->fps;
    s.frames = m->frames;
    s.replaced = m->replaced;
    return s;
}

int64_t CastSource::LastFrameQpc() const { return m->lastFrameQpc.load(); }

} // namespace cc
