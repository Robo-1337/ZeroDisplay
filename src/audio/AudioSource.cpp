#include "audio/AudioSource.h"
#include "core/Log.h"

#include <algorithm>
#include <cmath>

namespace cc {

// ~5.5 s at 48 kHz, 0.68 s at 384 kHz: room for the largest sync delay at any rate.
AudioSource::AudioSource() { m_ring.Init(1 << 18); }

void AudioSource::SetInputFormat(uint32_t rate, uint32_t periodFrames)
{
    m_inRate = rate;
    m_periodFrames = periodFrames;
    m_inReset = true;
    m_resetRequested = true;
}

double AudioSource::InputPeriodMs() const
{
    uint32_t rate = m_inRate.load();
    return rate ? m_periodFrames.load() * 1000.0 / rate : 0.0;
}

void AudioSource::UpdatePeak(const float* stereo, uint32_t frames)
{
    float pl = 0, pr = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        pl = std::max(pl, std::fabs(stereo[2 * i]));
        pr = std::max(pr, std::fabs(stereo[2 * i + 1]));
    }
    float cur = m_peak[0].load();
    while (pl > cur && !m_peak[0].compare_exchange_weak(cur, pl)) {}
    cur = m_peak[1].load();
    while (pr > cur && !m_peak[1].compare_exchange_weak(cur, pr)) {}
}

void AudioTap::Push(int64_t time, uint32_t rate, const float* stereo, uint32_t frames)
{
    AudioChunk c;
    c.time = time;
    c.rate = rate;
    c.stereo.assign(stereo, stereo + size_t(frames) * 2);
    {
        std::lock_guard lock(m_mutex);
        m_chunks.push_back(std::move(c));
        while (m_chunks.size() > 1000) m_chunks.pop_front();   // consumer gone for ~10 s: forget old audio
    }
    if (m_notify) SetEvent(m_notify);
}

bool AudioTap::Pop(AudioChunk& out)
{
    std::lock_guard lock(m_mutex);
    if (m_chunks.empty()) return false;
    out = std::move(m_chunks.front());
    m_chunks.pop_front();
    return true;
}

void AudioTap::Clear()
{
    std::lock_guard lock(m_mutex);
    m_chunks.clear();
}

void AudioSource::AddTap(const std::shared_ptr<AudioTap>& tap)
{
    std::lock_guard lock(m_tapMutex);
    m_taps.push_back(tap);
    m_hasTaps = true;
}

void AudioSource::RemoveTap(const AudioTap* tap)
{
    std::lock_guard lock(m_tapMutex);
    std::erase_if(m_taps, [&](const auto& t) { return t.get() == tap; });
    m_hasTaps = !m_taps.empty();
}

void AudioSource::FeedTaps(const float* stereo, uint32_t frames, int64_t time)
{
    if (!m_hasTaps.load()) {
        m_tapsActive = false;
        return;
    }
    const uint32_t rate = m_inRate.load();
    if (rate == 0) return;
    if (time == 0) time = QpcTo100ns(QpcNow());
    const float* data = stereo;
    uint32_t n = frames;
    if (rate != kTapRate) {
        // Recording (AAC), the browser viewer and the program mixer all work at 48 kHz: convert here, once.
        if (!m_tapRs.Matches(rate, kTapRate)) m_tapRs.Configure(rate, kTapRate);
        else if (!m_tapsActive) m_tapRs.Reset();   // old history from before the taps were attached
        m_tapBuf.clear();
        double first = 0.0;
        n = m_tapRs.Process(stereo, frames, m_tapBuf, &first);
        m_tapsActive = true;
        if (n == 0) return;
        time += int64_t(first * 1e7 / rate);
        data = m_tapBuf.data();
    }
    m_tapsActive = true;
    if (tapsSilent.load()) {   // the same timing, no sound (a separate track stays continuous)
        m_tapSilence.assign(size_t(n) * 2, 0.0f);
        data = m_tapSilence.data();
    }
    std::lock_guard lock(m_tapMutex);
    for (auto& t : m_taps) t->Push(time, kTapRate, data, n);
}

double AudioSource::s_testSkew = 0.0;
std::atomic<bool> AudioSource::s_testStall{false};

void AudioSource::Push(const float* stereo, uint32_t frames, int64_t time)
{
    if (s_testStall.load()) return;
    if (s_testSkew > 0.0) {
        m_skewAcc += frames * s_testSkew;
        const uint32_t cut = std::min(frames, uint32_t(m_skewAcc));
        m_skewAcc -= cut;
        frames -= cut;
    }
    NotePacket(frames);
    UpdatePeak(stereo, frames);
    uint32_t n = m_ring.Write(stereo, frames);
    if (n < frames) m_dropped += frames - n;
    FeedTaps(stereo, frames, time);
}

void AudioSource::NotePacket(uint32_t frames)
{
    const int64_t now = Now();
    if (m_lastPushQpc) {
        double gap = QpcToMs(now - m_lastPushQpc);
        if (gap > m_maxGapMs.load()) m_maxGapMs = gap;
        // Packets that arrive together (bursts): playback must not start with less than one such group buffered.
        m_group = gap < 2.0 ? m_group + frames : frames;
    } else {
        m_group = frames;
    }
    if (m_group > m_maxGroup.load()) m_maxGroup = m_group;
    m_lastPushQpc = now;
    m_lastPacketQpc = now;
    if (frames > m_maxPacket.load()) m_maxPacket = frames;

    // Measure the capture clock. A pause in delivery (the source stopped, a device change) starts over: frames that
    // never arrived would look like a slow clock.
    const uint32_t rate = m_inRate.load();
    const double pauseMs = std::max(250.0, 3.0 * InputPeriodMs());
    if (m_inReset.exchange(false) || (m_inLastQpc && QpcToMs(now - m_inLastQpc) > pauseMs)) {
        m_inClock.Reset();
        m_inFrames = 0;
        m_maxGroup = m_group = frames;
    }
    m_inLastQpc = now;
    m_inFrames += frames;
    if (rate) {
        m_inClock.Add(double(now) / double(QpcFrequency()), double(m_inFrames) / rate);
        m_inRatio = m_inClock.Ratio();
    }
}

void AudioSource::PushSilence(uint32_t frames, int64_t time)
{
    if (s_testStall.load()) return;
    NotePacket(frames);
    uint32_t n = m_ring.WriteSilence(frames);
    if (n < frames) m_dropped += frames - n;
    if (m_hasTaps.load()) {
        if (m_silence.size() < size_t(frames) * 2) m_silence.assign(size_t(frames) * 2, 0.0f);
        FeedTaps(m_silence.data(), frames, time);
    } else {
        m_tapsActive = false;
    }
}

bool AudioSource::NextInput(float& l, float& r)
{
    if (m_chunkPos >= m_chunkLen) {
        m_chunkLen = m_ring.Read(m_chunk, 512);
        m_chunkPos = 0;
        if (m_chunkLen == 0) return false;
    }
    l = m_chunk[2 * m_chunkPos];
    r = m_chunk[2 * m_chunkPos + 1];
    ++m_chunkPos;
    return true;
}

bool AudioSource::Prime()
{
    // Silence before the first frame, so that the first output is exactly the first input frame.
    m_sinc.Clear();
    for (uint32_t i = 0; i < m_kernel.Half() - 1; ++i) m_sinc.Push(0.0f, 0.0f);
    float l, r;
    for (uint32_t i = 0; i <= m_kernel.Half(); ++i) {
        if (!NextInput(l, r)) return false;
        m_sinc.Push(l, r);
    }
    m_frac = 0.0;
    return true;
}

void AudioSource::Idle()
{
    if (m_resetRequested.exchange(false)) {
        m_rateTrim = 0.0;
        m_rateSet = false;
        m_integral = 0.0;
        m_extraMs = 0.0;
        m_dip = 0.0;
        m_quietSec = 1e9;
    }
    m_ring.Skip(m_ring.Available());
    m_chunkPos = m_chunkLen = 0;
    m_prebuffering = true;
    m_outIdle = true;
    m_statFillMs = 0;
    m_fillAvg = 0;
}

void AudioSource::Skip(uint32_t frames)
{
    uint32_t fromChunk = std::min(frames, m_chunkLen - m_chunkPos);
    m_chunkPos += fromChunk;
    m_ring.Skip(frames - fromChunk);
}

// The median of the last few windows' lowest levels (frames), 0 before the first window.
double AudioSource::TypicalMin() const
{
    const int count = std::min(m_windows, 4);
    if (count == 0) return 0.0;
    double sorted[4];
    std::copy(m_recentMins, m_recentMins + count, sorted);
    std::sort(sorted, sorted + count);
    return (count % 2) != 0 ? sorted[count / 2] : 0.5 * (sorted[count / 2 - 1] + sorted[count / 2]);
}

void AudioSource::Pull(float* out, uint32_t frames, uint32_t outRate, double reserveMs, double startMs, double dtSec, float gainMul)
{
    if (m_resetRequested.exchange(false)) {
        m_ring.Skip(m_ring.Available());
        m_chunkPos = m_chunkLen = 0;
        m_prebuffering = true;
        m_rateTrim = 0.0;   // possibly a different device or format now
        m_rateSet = false;
        m_integral = 0.0;
        m_extraMs = 0.0;
        m_dip = 0.0;
        m_quietSec = 1e9;
        m_outIdle = true;
    }
    const uint32_t inRate = m_inRate.load();
    if (inRate == 0 || outRate == 0) return;
    if (!m_kernel.Matches(inRate, outRate)) {
        m_kernel.Build(inRate, outRate);
        m_sinc.Init(m_kernel.Taps());
        m_prebuffering = true;
    }

    // Measure the output clock from what it asks for (every call counts, also while this source is refilling).
    const double nowSec = double(Now()) / double(QpcFrequency());
    if (m_outIdle) {
        m_outIdle = false;
        m_outClock.Reset();
        m_outFrames = 0;
    }
    m_outFrames += frames;
    m_outClock.Add(nowSec, double(m_outFrames) / outRate);

    // Capture packets and output pulls are both periodic, so the fill level seen at any one moment
    // depends on their relative phase. What matters is the lowest level reached right after a pull:
    // that is steered to `reserve`, which makes the result independent of phase and absorbs jitter.
    const double effReserveMs = reserveMs + m_extraMs;
    const double reserve = effReserveMs * inRate / 1000.0;
    // Start with the reserve, this pull and one whole delivery from the capture side (a packet, or a burst of them)
    // buffered; otherwise the buffer runs dry before the next delivery arrives and playback stutters on restart.
    const double start = std::max(startMs + m_extraMs, effReserveMs) * inRate / 1000.0 + std::min(double(m_maxGroup.load()), inRate * 0.1);
    double fill = Fill();
    m_statTargetMs = effReserveMs + m_dip * 1000.0 / inRate;
    m_statExtraMs = m_extraMs + m_dip * 1000.0 / inRate;

    if (m_prebuffering) {
        if (fill < start || fill < m_kernel.Half() + 1) return;   // stay silent until enough audio has arrived
        if (!Prime()) return;
        fill = Fill();
        m_prebuffering = false;
        m_integral = 0.0;
        m_windowMin = 1e18;
        m_windowTime = 0.0;
        m_lastMin = -1.0;
        m_windows = 0;
        m_firstWindow = true;
        m_playSec = 0.0;
        m_fillAvg = fill - frames * double(inRate) / outRate;
    }

    double adjust = 0.0;
    if (m_lastMin >= 0.0) {
        // The first measured window after (re)starting tells us exactly how much spare audio there
        // is; drop it in one step instead of resampling it away over several seconds.
        // (Repeated until it's gone: this pull may come when the buffer is low and can only give up part of it.)
        if (m_firstWindow) {
            const double spare = m_lastMin - reserve - inRate * 0.0005;
            m_firstWindow = false;
            if (spare > inRate * 0.001) {
                double canDrop = fill - (frames * double(inRate) / outRate + reserve);
                uint32_t drop = uint32_t(std::max(0.0, std::min(spare, canDrop)));
                Skip(drop);
                m_firstWindow = spare - drop > inRate * 0.001;
                m_lastMin = -1.0;   // wait for a fresh measurement
                m_windows = 0;
            }
        }
    }
    if (m_lastMin >= 0.0) {
        if (m_lastMin > reserve + inRate * 0.040) {
            // Persistently far too much buffered (e.g. the output stalled for a while): drop the excess
            // at once instead of slowly resampling it away. Never drop into the reserve of this pull.
            double canDrop = fill - (frames * double(inRate) / outRate + reserve);
            uint32_t drop = uint32_t(std::max(0.0, std::min(m_lastMin - reserve, canDrop)));
            if (drop > 0) {
                Skip(drop);
                ++m_overruns;
            }
            m_lastMin = -1.0;
            m_windows = 0;
            m_integral = 0.0;
        } else {
            // PI controller on the windowed minimum (seconds of audio above the reserve). It reacts
            // faster to a shortfall than to an excess: an underrun is audible, extra latency isn't.
            // The measured clock ratio does the bulk of the work; this only trims what's left.
            const double errSec = (m_lastMin - reserve) / inRate;
            m_integral = Clamp(m_integral + errSec * dtSec, -0.01, 0.01);
            const double kp = errSec < 0 ? 1.5 : 0.25;
            adjust = Clamp(kp * errSec + 0.05 * m_integral, -0.0025, 0.0025);
        }
    }
    const double step = double(inRate) / double(outRate) * (1.0 + m_rateTrim) * (1.0 + adjust);
    m_statPpm = ((1.0 + m_rateTrim) * (1.0 + adjust) - 1.0) * 1e6;

    const float g = gain.load() * gainMul;
    for (uint32_t i = 0; i < frames; ++i) {
        float l, r;
        m_kernel.Interpolate(m_sinc.Window(), m_frac, l, r);
        out[2 * i] += l * g;
        out[2 * i + 1] += r * g;
        m_frac += step;
        while (m_frac >= 1.0) {
            m_frac -= 1.0;
            if (!NextInput(l, r)) {
                ++m_underruns;
                m_prebuffering = true;
                // Underruns while both clocks are known come from irregular timing, not from drift: an output that
                // takes audio in bursts, a busy system, or capture packets and output pulls drifting into step so
                // that jitter puts a pull before a packet (that comes back every few minutes). Each one is learned
                // as a dip (how far the level fell, which fades slowly). A dip still remembered gets deeper (by half a pull or more); one
                // that comes back within ten minutes after its memory faded means this pairing needs more room for
                // good: the reserve grows by one pull (at most 30 ms in all).
                const bool rateKnown = m_inRatio.load() > 0.0 && m_outClock.Ratio() > 0.0;
                if (rateKnown) {
                    const double shortfall = (frames - i) * step;
                    const double depth = TypicalMin() + shortfall;
                    const bool remembered = m_dip >= 0.5 * depth;
                    m_dip = std::min(inRate * 0.03, remembered ? std::max(depth, m_dip + std::max(shortfall, 0.5 * frames * step)) : std::max(m_dip, depth));
                    if (!remembered && m_quietSec < 600.0 && m_extraMs < 30.0) {
                        m_extraMs = std::min(30.0, m_extraMs + std::max(2.0, dtSec * 1000.0));
                        LOGW(L"Audio: the output takes sound irregularly; reserve raised by %.1f ms", m_extraMs);
                    }
                    m_quietSec = 0.0;
                }
                static int64_t lastLog = 0;
                if (QpcElapsedMs(lastLog) > 1000) {
                    lastLog = QpcNow();
                    LOGW(L"Audio underrun: pull %u frames, ran dry at %u (fill was %.0f frames, reserve %.0f)", frames, i, fill, reserve);
                }
                return;
            }
            m_sinc.Push(l, r);
        }
    }
    m_playSec += dtSec;
    m_quietSec += dtSec;

    // Track the level left after each pull: its minimum over 0.5 s windows (for control) and its
    // average (how long audio actually waits in this buffer).
    const double left = Fill();
    m_fillAvg += (left - m_fillAvg) * 0.05;
    m_statFillMs = m_fillAvg * 1000.0 / inRate;
    m_windowMin = std::min(m_windowMin, left);
    m_windowTime += dtSec;
    if (m_windowTime >= 0.5) {
        // Control on the typical lowest level (median of the last 2 s), less the depth of the dips seen lately: some
        // outputs take an extra period now and then (Bluetooth), and when capture packets and output wakeups drift
        // into step, jitter puts a pull before a packet instead of after it. The buffer then sits that much higher,
        // steadily, instead of chasing each dip. The learned depth fades by 0.05 ms per second.
        m_recentMins[m_windows % 4] = m_windowMin;
        ++m_windows;
        const double typical = TypicalMin();
        m_dip = std::max(0.0, std::max(typical - m_windowMin, m_dip - inRate * 0.00005 * m_windowTime));
        m_dip = std::min(m_dip, inRate * 0.03);
        m_lastMin = typical - m_dip;
        m_statMinMs = m_windowMin * 1000.0 / inRate;
        m_windowMin = 1e18;
        m_windowTime = 0.0;
        // Follow the measured clocks (input frames per output frame, against the stated rates).
        const double inRatio = m_inRatio.load(), outRatio = m_outClock.Ratio();
        m_statInPpm = inRatio > 0.0 ? (inRatio - 1.0) * 1e6 : std::nan("");
        m_statOutPpm = outRatio > 0.0 ? (outRatio - 1.0) * 1e6 : std::nan("");
        if (inRatio > 0.0 || outRatio > 0.0) {
            const double want = (inRatio > 0.0 ? inRatio : 1.0) / (outRatio > 0.0 ? outRatio : 1.0) - 1.0;
            m_rateTrim = m_rateSet ? m_rateTrim + (want - m_rateTrim) * 0.5 : want;   // the first measurement at once
            m_rateSet = true;
        }
    }
}

} // namespace cc
