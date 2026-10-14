#include "wireless/CastAudioTap.h"
#include "audio/AudioSource.h"
#include "core/Common.h"
#include "core/Log.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Effects.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.Media.h>
#include <MemoryBuffer.h>
#include <winstring.h>
#include <roapi.h>
#include <activation.h>

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <span>
#include <thread>

namespace cc {

namespace {

struct Chunk {
    std::vector<float> stereo;
    int64_t media = -1;   // 100 ns media time of the first frame (-1 = unknown: release at once)
};
struct TapEntry {
    std::shared_ptr<AudioSource> target;
    uint64_t frames = 0;
    uint32_t rate = 0;
    std::deque<Chunk> queue;
    bool haveClock = false;
    int64_t clockMedia = 0, clockQpc = 0;
};
std::mutex g_mutex;
std::map<int, TapEntry> g_taps;
int g_nextId = 1;
std::thread g_release;
std::atomic<bool> g_releaseStop{false};

// Every few ms: hand the buffers that are due to their AudioSource, stamped with when they are heard.
void ReleaseLoop()
{
    SetCurrentThreadName(L"Cast audio release");
    MmcssScope mmcss(L"Pro Audio", 0);
    const int64_t f = QpcFrequency();
    while (!g_releaseStop) {
        Sleep(3);
        const int64_t now = QpcNow();
        std::vector<std::pair<std::shared_ptr<AudioSource>, Chunk>> due;
        {
            std::lock_guard lock(g_mutex);
            for (auto& [id, t] : g_taps) {
                while (!t.queue.empty()) {
                    Chunk& c = t.queue.front();
                    int64_t at = now;
                    if (c.media >= 0 && t.haveClock) at = t.clockQpc + (c.media - t.clockMedia) * f / 10000000;
                    else if (c.media >= 0 && t.queue.size() < 200) break;   // wait (up to ~2 s) for the first video frame's clock
                    if (at > now) break;
                    due.emplace_back(t.target, std::move(c));
                    t.queue.pop_front();
                }
            }
        }
        for (auto& [target, c] : due) {
            if (target) target->Push(c.stereo.data(), uint32_t(c.stereo.size() / 2), QpcTo100ns(QpcNow()));
        }
    }
}

namespace wme = winrt::Windows::Media::Effects;
namespace wmm = winrt::Windows::Media::MediaProperties;
namespace wfc = winrt::Windows::Foundation::Collections;

struct Tap : winrt::implements<Tap, wme::IBasicAudioEffect, winrt::Windows::Media::IMediaExtension> {
    int id = 0;
    uint32_t rate = 48000;
    std::atomic<uint32_t> channels{2};   // set by SetEncodingProperties, read per frame on the media thread
    std::vector<float> stereo;

    // IMediaExtension
    void SetProperties(const wfc::IPropertySet& config)
    {
        if (config && config.HasKey(L"tap")) id = winrt::unbox_value<int32_t>(config.Lookup(L"tap"));
    }
    // IBasicAudioEffect
    bool UseInputFrameForOutput() const { return true; }   // the sound passes through unchanged
    wfc::IVectorView<wmm::AudioEncodingProperties> SupportedEncodingProperties() const
    {
        auto v = winrt::single_threaded_vector<wmm::AudioEncodingProperties>();
        for (uint32_t r : {48000u, 44100u}) {
            for (uint32_t ch : {2u, 1u, 6u, 8u}) {
                wmm::AudioEncodingProperties p = wmm::AudioEncodingProperties::CreatePcm(r, ch, 32);
                p.Subtype(L"Float");
                v.Append(p);
            }
        }
        return v.GetView();
    }
    void SetEncodingProperties(const wmm::AudioEncodingProperties& p)
    {
        rate = p.SampleRate();
        channels = std::max(1u, p.ChannelCount());
        std::lock_guard lock(g_mutex);
        auto it = g_taps.find(id);
        if (it != g_taps.end() && it->second.target) {
            it->second.rate = rate;
            it->second.target->SetInputFormat(rate, rate / 100);
        }
    }
    void ProcessFrame(const wme::ProcessAudioFrameContext& ctx)
    {
        winrt::Windows::Media::AudioFrame frame = ctx.InputFrame();
        winrt::Windows::Media::AudioBuffer buf = frame.LockBuffer(winrt::Windows::Media::AudioBufferAccessMode::Read);
        winrt::Windows::Foundation::IMemoryBufferReference ref = buf.CreateReference();
        BYTE* data = nullptr;
        UINT32 capacity = 0;
        if (FAILED(ref.as<::Windows::Foundation::IMemoryBufferByteAccess>()->GetBuffer(&data, &capacity)) || !data) return;
        // Trust the buffer's capacity, not just the length it reports. One channel count for the whole frame (a format
        // change on another thread must not move the reads past the buffer): the last read is frames * ch - 1.
        const uint32_t ch = std::max(1u, channels.load());
        const uint32_t frames = std::min(buf.Length(), capacity) / uint32_t(sizeof(float) * ch);
        if (frames == 0) return;
        // Exactly the samples that fit in the buffer (frames * ch * 4 <= capacity, from the division above).
        const std::span<const float> f(reinterpret_cast<const float*>(data), size_t(frames) * ch);
        Chunk c;
        if (auto rt = frame.RelativeTime()) c.media = rt.Value().count();
        c.stereo.resize(size_t(frames) * 2);
        if (ch == 2) {
            std::copy(f.begin(), f.end(), c.stereo.begin());
        } else {   // mono: duplicate; surround: front left / right
            const size_t right = ch > 1 ? 1 : 0;
            for (size_t i = 0, k = 0; i < frames; ++i, k += ch) {
                c.stereo[2 * i] = f[k];
                c.stereo[2 * i + 1] = f[k + right];
            }
        }
        std::lock_guard lock(g_mutex);
        auto it = g_taps.find(id);
        if (it == g_taps.end()) return;
        it->second.frames += frames;
        it->second.queue.push_back(std::move(c));
        while (it->second.queue.size() > 400) it->second.queue.pop_front();   // no clock for a long time: keep ~4 s
    }
    void Close(wme::MediaEffectClosedReason) {}
    void DiscardQueuedFrames() {}
};

struct TapFactory : winrt::implements<TapFactory, winrt::Windows::Foundation::IActivationFactory> {
    winrt::Windows::Foundation::IInspectable ActivateInstance() const { return winrt::make<Tap>(); }
};

} // namespace

namespace casttap {

int Register(std::shared_ptr<AudioSource> target)
{
    std::lock_guard lock(g_mutex);
    const int id = g_nextId++;
    g_taps[id].target = std::move(target);
    if (!g_release.joinable()) {
        g_releaseStop = false;
        g_release = std::thread(ReleaseLoop);
    }
    return id;
}

void Unregister(int id)
{
    bool last = false;
    {
        std::lock_guard lock(g_mutex);
        g_taps.erase(id);
        last = g_taps.empty();
    }
    if (last && g_release.joinable()) {
        g_releaseStop = true;
        g_release.join();
    }
}

void SetClock(int id, int64_t media100ns, int64_t qpc)
{
    std::lock_guard lock(g_mutex);
    auto it = g_taps.find(id);
    if (it == g_taps.end()) return;
    it->second.haveClock = true;
    it->second.clockMedia = media100ns;
    it->second.clockQpc = qpc;
}

uint64_t Frames(int id)
{
    std::lock_guard lock(g_mutex);
    auto it = g_taps.find(id);
    return it == g_taps.end() ? 0 : it->second.frames;
}

} // namespace casttap
} // namespace cc

// Registration-free WinRT: combase loads the module named in app.manifest (this exe) and asks it for the
// factory of "CaptureCard.CastAudioTap".
extern "C" __declspec(dllexport) HRESULT __stdcall DllGetActivationFactory(HSTRING classId, void** factory)
{
    if (!factory) return E_POINTER;
    *factory = nullptr;
    const std::wstring_view name{WindowsGetStringRawBuffer(classId, nullptr)};
    if (name != cc::casttap::kClassId) return CLASS_E_CLASSNOTAVAILABLE;
    try {
        *factory = winrt::detach_abi(winrt::make<cc::TapFactory>().as<winrt::Windows::Foundation::IActivationFactory>());
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }
}
