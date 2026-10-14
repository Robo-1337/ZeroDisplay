#include "audio/AudioEngine.h"
#include "audio/AudioClient.h"
#include "core/Log.h"

#include <algorithm>
#include <cmath>

namespace cc {

AudioEngine::AudioEngine() { m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); }

AudioEngine::~AudioEngine()
{
    Stop();
    CloseHandle(m_stopEvent);
}

double AudioEngine::SafetyMs(AudioLatency l)
{
    switch (l) {
    case AudioLatency::Ultra: return 1.5;
    case AudioLatency::Low: return 4.0;
    default: return 15.0;
    }
}

uint32_t OutputQueue::Target(AudioLatency preset) const
{
    const uint32_t extra = preset == AudioLatency::Ultra ? rate / 1000 : (preset == AudioLatency::Low ? period / 2 : period);
    return std::min(buffer, period + extra + learned);
}

bool OutputQueue::Wake(uint32_t padding, uint32_t wrote, double sec)
{
    quietSec += sec;
    if (wrote == 0 || padding > 0) return false;
    ++dry;
    if (quietSec < 60.0 && learned < 2 * period) learned = std::min(2 * period, learned + std::max(1u, period / 2));
    quietSec = 0.0;
    return true;
}

void AudioEngine::Start(const std::wstring& endpointId)
{
    Stop();
    m_endpoint = endpointId;
    ResetEvent(m_stopEvent);
    m_failed = false;
    m_exclusiveRejected = false;
    m_exclusiveUnavailable = false;
    m_thread = std::thread([this] { ThreadMain(); });
}

void AudioEngine::Stop()
{
    SetEvent(m_stopEvent);
    if (m_thread.joinable()) m_thread.join();
    m_running = false;
}

void AudioEngine::AddSource(const std::shared_ptr<AudioSource>& src)
{
    std::lock_guard lock(m_sourcesMutex);
    m_sources.push_back(src);
}

void AudioEngine::RemoveSource(const AudioSource* src)
{
    std::lock_guard lock(m_sourcesMutex);
    std::erase_if(m_sources, [&](const auto& s) { return s.get() == src; });
}

std::wstring AudioEngine::Description() const
{
    std::lock_guard lock(m_descMutex);
    return m_desc;
}

void AudioEngine::ThreadMain()
{
    ComInit com;
    SetCurrentThreadName(L"Audio output");
    MmcssScope mmcss(L"Pro Audio", 2);

    bool tryExclusive = exclusive.load();
    for (;;) {
        RunResult r = RunStream(tryExclusive);
        if (r == RunResult::FallbackShared) {
            tryExclusive = false;
            continue;
        }
        if (r == RunResult::Failed) m_failed = true;
        break;
    }
    m_running = false;
}

AudioEngine::RunResult AudioEngine::RunStream(bool wantExclusive)
{
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    struct EventGuard {
        HANDLE h;
        ~EventGuard() { CloseHandle(h); }
    } guard{ready};

    WasapiStream s;
    HRESULT hr = OpenWasapiStream(m_endpoint, eRender, ready, s, wantExclusive);
    ComPtr<IAudioRenderClient> render;
    if (SUCCEEDED(hr)) hr = s.client->GetService(IID_PPV_ARGS(&render));
    if (FAILED(hr)) {
        LOGE(L"Audio output open failed: %s", HrToString(hr).c_str());
        return RunResult::Failed;
    }
    const uint32_t outRate = s.format.rate;
    const double periodMs = s.periodFrames * 1000.0 / outRate;
    m_periodMs = periodMs;
    m_exclusiveUnavailable = wantExclusive && !s.exclusive && !m_exclusiveRejected.load();
    {
        std::lock_guard lock(m_descMutex);
        const wchar_t* note = s.exclusive                ? L" (exclusive)"
                              : m_exclusiveRejected.load() ? L" (shared; driver too bursty for exclusive)"
                              : wantExclusive               ? L" (shared; exclusive unavailable)"
                              : s.lowLatency                ? L" (low-latency)"
                                                            : L"";
        m_desc = Format(L"%s: %s, period %.2f ms, buffer %.2f ms%s", s.name.c_str(), s.format.Describe().c_str(), periodMs,
                        s.bufferFrames * 1000.0 / outRate, note);
    }
    LOGI(L"Audio output: %s", Description().c_str());

    if (s.exclusive) {
        // Exclusive mode must be primed with one buffer before starting.
        BYTE* data = nullptr;
        if (SUCCEEDED(render->GetBuffer(s.bufferFrames, &data))) render->ReleaseBuffer(s.bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    hr = s.client->Start();
    if (FAILED(hr)) {
        LOGE(L"Audio output start failed: %s", HrToString(hr).c_str());
        return RunResult::Failed;
    }
    m_running = true;

    std::vector<float> mix;
    std::vector<std::shared_ptr<AudioSource>> sources;
    OutputQueue queue;
    queue.period = s.periodFrames;
    queue.buffer = s.bufferFrames;
    queue.rate = outRate;
    uint32_t wrote = 0;   // anything written yet (a stream that never had audio can't run dry)
    m_learnedMs = 0;
    {
        // New output clock: start every source's buffer and drift controller from scratch.
        std::lock_guard lock(m_sourcesMutex);
        for (auto& src : m_sources) src->Reset();
    }
    float lastGain = 0.0f;
    const int64_t startQpc = QpcNow();
    int64_t lastWake = 0;
    int64_t jitterWindowStart = startQpc;
    double jitterWindowMax = 0;
    double startupMaxGap = 0;
    RunResult result = RunResult::Stopped;

    HANDLE handles[2] = {m_stopEvent, ready};
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, handles, FALSE, 200);
        if (w == WAIT_OBJECT_0) break;

        // Callback timing: how regularly does the driver actually ask for audio?
        const int64_t nowQ = QpcNow();
        const double lastGapSec = lastWake ? QpcToMs(nowQ - lastWake) / 1000.0 : 0.0;
        if (lastWake) {
            const double gap = QpcToMs(nowQ - lastWake);
            if (gap > m_maxWakeGapMs.load()) m_maxWakeGapMs = gap;
            jitterWindowMax = std::max(jitterWindowMax, gap);
            if (QpcToMs(nowQ - jitterWindowStart) > 2000) {
                m_jitterMs = jitterWindowMax;
                jitterWindowMax = 0;
                jitterWindowStart = nowQ;
            }
            if (QpcToMs(nowQ - startQpc) > 300) startupMaxGap = std::max(startupMaxGap, gap);
        }
        lastWake = nowQ;
        ++m_wakeups;

        // Some drivers (e.g. Intel SST) accept a tiny exclusive period but service it in large
        // bursts, which is worse than shared mode. Detect that and fall back.
        if (s.exclusive && QpcToMs(nowQ - startQpc) > 800 && startupMaxGap > std::max(4.0 * periodMs, 20.0)) {
            LOGW(L"Exclusive output is bursty (callbacks up to %.1f ms apart for a %.1f ms period); using shared mode", startupMaxGap, periodMs);
            m_exclusiveRejected = true;
            result = RunResult::FallbackShared;
            break;
        }

        {
            std::lock_guard lock(m_sourcesMutex);
            sources = m_sources;
        }
        uint32_t frames = 0;
        if (s.exclusive) {
            // Event-driven exclusive mode: each event asks for exactly one full buffer.
            frames = s.bufferFrames;
            m_queuedMs = periodMs;
        } else {
            UINT32 padding = 0;
            hr = s.client->GetCurrentPadding(&padding);
            if (FAILED(hr)) break;
            m_queuedMs = padding * 1000.0 / outRate;
            if (queue.Wake(padding, wrote, lastGapSec)) {
                // The device ran out of audio: sources must measure its clock again, and the queue may grow.
                for (auto& src : sources) src->OutputGap();
                m_dry = queue.dry;
                m_learnedMs = queue.learned * 1000.0 / outRate;
                static int64_t lastLog = 0;
                if (QpcElapsedMs(lastLog) > 5000) {
                    lastLog = QpcNow();
                    LOGW(L"Audio output ran dry (%u times); queue now %.1f ms", queue.dry, queue.Target(AudioLatency(latency.load())) * 1000.0 / outRate);
                }
            }
            const uint32_t queueTarget = queue.Target(AudioLatency(latency.load()));
            if (padding >= queueTarget) continue;
            frames = queueTarget - padding;
            wrote += frames;
        }

        mix.assign(size_t(frames) * 2, 0.0f);
        const double dt = double(frames) / outRate;
        const double safety = SafetyMs(AudioLatency(latency.load()));
        const bool pass = passthrough.load();
        for (auto& src : sources) {
            if (pass && src->monitor.load()) {
                const double delay = std::max(0, avOffsetMs.load());
                const double reserve = safety + delay + src->extraReserveMs.load();
                // Start with just enough for one pull plus the reserve; the controller fine-tunes from there.
                const double start = reserve + dt * 1000.0 + 1.0;
                src->Pull(mix.data(), frames, outRate, reserve, start, dt, 1.0f);
            } else {
                src->Idle();
            }
        }

        // Master gain with a per-buffer ramp so volume/mute changes never click.
        const float targetGain = muted.load() ? 0.0f : volume.load();
        float pl = 0, pr = 0;
        for (uint32_t i = 0; i < frames; ++i) {
            float g = lastGain + (targetGain - lastGain) * float(i + 1) / float(frames);
            // Above 100 % (quiet cards) peaks would clip: they are rounded off softly instead.
            auto limit = [](float x) {
                const float a = std::fabs(x);
                if (a <= 0.9f) return x;
                return std::copysign(0.9f + 0.1f * std::tanh((a - 0.9f) / 0.1f), x);
            };
            float l = limit(mix[2 * i] * g);
            float r = limit(mix[2 * i + 1] * g);
            mix[2 * i] = l;
            mix[2 * i + 1] = r;
            pl = std::max(pl, std::fabs(l));
            pr = std::max(pr, std::fabs(r));
        }
        lastGain = targetGain;
        float cur = m_peak[0].load();
        while (pl > cur && !m_peak[0].compare_exchange_weak(cur, pl)) {}
        cur = m_peak[1].load();
        while (pr > cur && !m_peak[1].compare_exchange_weak(cur, pr)) {}

        BYTE* data = nullptr;
        hr = render->GetBuffer(frames, &data);
        if (FAILED(hr)) break;
        FromStereoFloat(mix.data(), frames, s.format, data);
        render->ReleaseBuffer(frames, 0);
    }
    if (FAILED(hr)) {
        LOGW(L"Audio output stopped: %s", HrToString(hr).c_str());
        result = RunResult::Failed;
    }
    s.client->Stop();
    m_running = false;
    return result;
}

} // namespace cc
