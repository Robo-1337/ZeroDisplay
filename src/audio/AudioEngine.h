#pragma once
#include "audio/AudioSource.h"

#include <memory>
#include <thread>

namespace cc {

enum class AudioLatency { Ultra = 0, Low = 1, Safe = 2 };

// How much the output keeps queued in the system mixer (shared mode): about 1, 1.5 or 2 periods by preset. When the
// device runs dry twice within a minute (late wakeups: a busy system, a driver that takes audio in bursts) the queue
// grows by half a period, up to two periods more.
struct OutputQueue {
    uint32_t period = 0, buffer = 0, rate = 0;
    uint32_t learned = 0;   // frames added after the device ran dry
    uint32_t dry = 0;       // times it ran dry
    double quietSec = 1e9;  // since it last ran dry
    uint32_t Target(AudioLatency preset) const;
    // At every wakeup, before writing: `padding` = frames still queued, `wrote` = anything written before (0 = no),
    // `sec` = time since the last wakeup. Returns true when the device had run out of audio since then.
    bool Wake(uint32_t padding, uint32_t wrote, double sec);
};

// Low-latency WASAPI output that mixes all monitored AudioSources.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    void Start(const std::wstring& endpointId);   // empty = system default
    void Stop();
    bool Running() const { return m_running.load(); }
    bool Failed() const { return m_failed.load(); }
    // Exclusive mode was requested but the driver serviced it too irregularly, so shared mode is in use.
    bool ExclusiveRejected() const { return m_exclusiveRejected.load(); }
    bool ExclusiveUnavailable() const { return m_exclusiveUnavailable.load(); }   // asked for, but the device said no
    const std::wstring& EndpointSetting() const { return m_endpoint; }

    void AddSource(const std::shared_ptr<AudioSource>& src);
    void RemoveSource(const AudioSource* src);

    std::atomic<float> volume{1.0f};
    std::atomic<bool> muted{false};
    std::atomic<bool> passthrough{true};
    std::atomic<int> latency{int(AudioLatency::Low)};
    std::atomic<int> avOffsetMs{0};
    std::atomic<bool> exclusive{false};   // applied on the next Start()

    float TakePeak(int ch) { return m_peak[ch].exchange(0.0f); }
    std::wstring Description() const;
    double PeriodMs() const { return m_periodMs.load(); }
    double QueuedMs() const { return m_queuedMs.load(); }
    // Diagnostics: wakeups and the largest gap between them since the last call.
    uint32_t TakeWakeups() { return m_wakeups.exchange(0); }
    double TakeMaxWakeGapMs() { return m_maxWakeGapMs.exchange(0.0); }
    // Largest delay between output callbacks over the last ~2 s (driver scheduling jitter).
    double WakeJitterMs() const { return m_jitterMs.load(); }
    // Times the output device ran out of audio (late wakeups), and the extra queue learned from that.
    uint32_t DryCount() const { return m_dry.load(); }
    double LearnedQueueMs() const { return m_learnedMs.load(); }

    static double SafetyMs(AudioLatency l);

private:
    enum class RunResult { Stopped, Failed, FallbackShared };
    void ThreadMain();
    RunResult RunStream(bool exclusive);

    std::wstring m_endpoint;
    std::thread m_thread;
    HANDLE m_stopEvent = nullptr;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_failed{false};
    std::atomic<bool> m_exclusiveRejected{false};
    std::atomic<bool> m_exclusiveUnavailable{false};

    std::mutex m_sourcesMutex;
    std::vector<std::shared_ptr<AudioSource>> m_sources;

    std::atomic<float> m_peak[2] = {0.0f, 0.0f};
    std::atomic<double> m_periodMs{0}, m_queuedMs{0};
    std::atomic<uint32_t> m_wakeups{0};
    std::atomic<double> m_maxWakeGapMs{0};
    std::atomic<double> m_jitterMs{0};
    std::atomic<uint32_t> m_dry{0};
    std::atomic<double> m_learnedMs{0};
    mutable std::mutex m_descMutex;
    std::wstring m_desc;
};

} // namespace cc
