#pragma once
#include "audio/AudioFormat.h"
#include "audio/Resampler.h"

#include <cmath>
#include <deque>
#include <memory>

namespace cc {

// A copy of everything a source captures, with capture timestamps (QPC, 100 ns). Used by recording
// and streaming; the playback path never waits on it.
struct AudioChunk {
    int64_t time = 0;
    uint32_t rate = 0;
    std::vector<float> stereo;
};

class AudioTap {
public:
    void Push(int64_t time, uint32_t rate, const float* stereo, uint32_t frames);
    bool Pop(AudioChunk& out);
    void Clear();
    void SetNotify(HANDLE ev) { m_notify = ev; }   // signalled on every Push (optional)

private:
    std::mutex m_mutex;
    std::deque<AudioChunk> m_chunks;
    HANDLE m_notify = nullptr;
};

// One captured audio stream feeding the mixer. The capture thread pushes; the output thread pulls
// through a band-limited resampler (any input rate to any output rate). Both devices' clocks are
// measured, and the ratio is steered on top of that to hold the buffer at a target fill, which
// absorbs any clock difference between the capture device and the output device.
// Taps (recording, replay, sharing) always get 48 kHz, whatever the device delivers.
class AudioSource {
public:
    AudioSource();

    // ---- producer (capture thread) ----
    void SetInputFormat(uint32_t rate, uint32_t periodFrames);
    // `time` = capture time of the first frame (QPC, 100 ns), or 0 if unknown.
    void Push(const float* stereo, uint32_t frames, int64_t time = 0);
    void PushSilence(uint32_t frames, int64_t time = 0);
    void AddTap(const std::shared_ptr<AudioTap>& tap);
    void RemoveTap(const AudioTap* tap);
    void Reset() { m_resetRequested = true; }

    // ---- consumer (output thread) ----
    // Adds `frames` resampled stereo frames into `out`, scaled by `gain`. `reserveMs` is the minimum to
    // keep buffered after each pull; `startMs` is how much to collect before (re)starting playback.
    void Pull(float* out, uint32_t frames, uint32_t outRate, double reserveMs, double startMs, double dtSec, float gain);
    // Called instead of Pull while the source is not being monitored; keeps the buffer from growing.
    void Idle();
    // The output device ran dry (missed audio): its frame count no longer tracks its clock, measure it again.
    void OutputGap() { m_outIdle = true; }

    // ---- shared ----
    std::atomic<float> gain{1.0f};
    std::atomic<bool> monitor{false};
    std::atomic<bool> tapsSilent{false};   // muted (microphone): recordings, replay and sharing get silence, timing kept
    static double s_testSkew;   // testing: pretend sources deliver this fraction fewer frames
    std::atomic<float> extraReserveMs{0.0f};   // e.g. network sources add room for packet jitter
    static constexpr uint32_t kTapRate = 48000;   // the rate taps receive
    uint32_t TapRate() const { return m_inRate.load() ? kTapRate : 0; }

    float TakePeak(int channel) { return m_peak[channel].exchange(0.0f); }
    uint32_t InputRate() const { return m_inRate.load(); }
    double InputPeriodMs() const;
    double FillMs() const { return m_statFillMs.load(); }        // average level
    double MinLeftMs() const { return m_statMinMs.load(); }      // lowest level after a pull (last window)
    double TargetMs() const { return m_statTargetMs.load(); }   // reserve in use, incl. LearnedReserveMs
    double LearnedReserveMs() const { return m_statExtraMs.load(); }   // learned from dips and underruns (bursty output)
    double DriftPpm() const { return m_statPpm.load(); }   // total rate correction
    // Measured clock errors of the capture and output device (ppm against their stated rates); NaN = not known yet.
    double InClockPpm() const { return m_statInPpm.load(); }
    double OutClockPpm() const { return m_statOutPpm.load(); }
    uint32_t Underruns() const { return m_underruns.load(); }
    uint32_t Overruns() const { return m_overruns.load(); }
    uint32_t Dropped() const { return m_dropped.load(); }
    // Largest gap between capture packets / largest packet since the last call (diagnostics).
    double TakeMaxGapMs() { return m_maxGapMs.exchange(0.0); }
    uint32_t TakeMaxPacket() { return m_maxPacket.exchange(0); }
    // When the capture last delivered anything (sound or silence packets), QPC; 0 = never. A device whose driver
    // stops delivering shows up here (App::CheckSound).
    int64_t LastPacketQpc() const { return m_lastPacketQpc.load(); }
    static std::atomic<bool> s_testStall;   // testing: captured packets are dropped, as if the driver stopped
    // Testing (--audio-sim): a simulated clock (QPC ticks) used instead of QpcNow(); 0 = real time.
    void SetTestNow(int64_t qpc) { m_testNow = qpc; }

private:
    double Fill() const { return double(m_ring.Available()) + double(m_chunkLen - m_chunkPos); }
    bool NextInput(float& l, float& r);
    bool Prime();
    void Skip(uint32_t frames);
    void UpdatePeak(const float* stereo, uint32_t frames);
    void NotePacket(uint32_t frames);

    void FeedTaps(const float* stereo, uint32_t frames, int64_t time);
    int64_t Now() const { return m_testNow ? m_testNow : QpcNow(); }
    int64_t m_testNow = 0;

    AudioRing m_ring;
    std::mutex m_tapMutex;
    std::vector<std::shared_ptr<AudioTap>> m_taps;
    std::atomic<bool> m_hasTaps{false};
    std::vector<float> m_silence;
    std::atomic<uint32_t> m_inRate{0};
    std::atomic<uint32_t> m_periodFrames{0};
    std::atomic<bool> m_resetRequested{false};
    std::atomic<float> m_peak[2] = {0.0f, 0.0f};

    // consumer state
    SincKernel m_kernel;
    SincHistory m_sinc;
    double m_frac = 0.0;
    bool m_prebuffering = true;
    double m_integral = 0.0;
    // Measured clock ratio (input against output) - 1, applied smoothly. Some devices deliver a few percent more or
    // less than their stated rate, far beyond what steering on the buffer level alone could follow.
    double m_rateTrim = 0.0;
    bool m_rateSet = false;   // m_rateTrim comes from a measurement
    ClockEstimator m_outClock;
    uint64_t m_outFrames = 0;
    bool m_outIdle = true;
    double m_extraMs = 0.0;      // learned extra reserve (bursty output devices)
    double m_playSec = 0.0;      // playing since the last (re)start
    double m_quietSec = 1e9;     // since the last underrun that counted towards the learned reserve
    double m_fillAvg = 0.0;
    double m_windowMin = 1e18;
    double m_windowTime = 0.0;
    double m_lastMin = -1.0;   // < 0: no complete window yet; else the lowest level over the last few windows
    double m_recentMins[4] = {};
    int m_windows = 0;
    double m_dip = 0.0;   // how far single windows dip below the typical level (learned, fades slowly), frames
    double TypicalMin() const;
    bool m_firstWindow = false;
    float m_chunk[512 * 2] = {};
    uint32_t m_chunkPos = 0, m_chunkLen = 0;

    // producer state
    double m_skewAcc = 0.0;
    ClockEstimator m_inClock;
    uint64_t m_inFrames = 0;
    int64_t m_inLastQpc = 0;
    std::atomic<bool> m_inReset{true};
    std::atomic<double> m_inRatio{0.0};
    uint32_t m_group = 0;                       // frames that arrived together (packets less than 2 ms apart)
    std::atomic<uint32_t> m_maxGroup{0};
    Resampler m_tapRs;
    std::vector<float> m_tapBuf;
    std::vector<float> m_tapSilence;   // capture thread: zeros for a muted source's taps
    bool m_tapsActive = false;

    // stats
    std::atomic<double> m_statFillMs{0}, m_statTargetMs{0}, m_statPpm{0}, m_statMinMs{0}, m_statExtraMs{0};
    std::atomic<double> m_statInPpm{std::nan("")}, m_statOutPpm{std::nan("")};
    std::atomic<uint32_t> m_underruns{0}, m_overruns{0}, m_dropped{0};
    int64_t m_lastPushQpc = 0;
    std::atomic<int64_t> m_lastPacketQpc{0};
    std::atomic<double> m_maxGapMs{0};
    std::atomic<uint32_t> m_maxPacket{0};
};

} // namespace cc
