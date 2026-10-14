#include "encode/Flashback.h"
#include "core/Log.h"

#include <algorithm>

namespace cc {

bool Flashback::Start(IMFDXGIDeviceManager* dxgi, const EncoderConfig& cfg, std::vector<PacketPtr> video, std::vector<AudioChunk> audio)
{
    Stop();
    size_t first = 0;
    while (first < video.size() && !video[first]->keyframe) ++first;
    if (video.size() - first < 2) return false;
    m_video.assign(video.begin() + ptrdiff_t(first), video.end());
    m_audioChunks = std::move(audio);
    m_cfg = cfg;
    m_dxgi = dxgi;
    m_t0 = m_video.front()->time;
    m_duration = double(m_video.back()->time + m_video.back()->duration - m_t0) / 1e7;
    m_audio = std::make_shared<AudioSource>();
    m_decoded = SIZE_MAX;
    m_audioNext = 0;
    m_basePos = 0;
    m_baseQpc = QpcNow();
    m_seekPending = true;
    m_playing = false;
    m_atEnd = false;
    m_quit = false;
    m_hasFrame = false;
    m_thread = std::thread([this] { ThreadMain(); });
    LOGI(L"Flashback: %.1f s, %zu frames, %zu audio chunks", m_duration, m_video.size(), m_audioChunks.size());
    return true;
}

void Flashback::Stop()
{
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    m_video.clear();
    m_audioChunks.clear();
}

double Flashback::Position() const
{
    std::lock_guard lock(m_mutex);
    if (!m_playing) return m_basePos;
    return std::min(m_duration, m_basePos + QpcToMs(QpcNow() - m_baseQpc) / 1000.0);
}

void Flashback::Seek(double seconds)
{
    {
        std::lock_guard lock(m_mutex);
        m_basePos = std::clamp(seconds, 0.0, m_duration);
        m_baseQpc = QpcNow();
        m_seekPending = true;
        m_atEnd = false;
    }
    m_cv.notify_all();
}

void Flashback::SetPlaying(bool on)
{
    {
        std::lock_guard lock(m_mutex);
        if (on == m_playing.load()) return;
        if (!on) m_basePos = std::min(m_duration, m_basePos + QpcToMs(QpcNow() - m_baseQpc) / 1000.0);
        else if (m_basePos >= m_duration - 0.05) m_basePos = 0;   // play again from the start
        m_baseQpc = QpcNow();
        m_playing = on;
        m_atEnd = false;
        m_seekPending = true;   // re-sync audio
    }
    m_cv.notify_all();
}

bool Flashback::TakeFrame(CapturedFrame& out)
{
    std::lock_guard lock(m_frameMutex);
    if (!m_hasFrame) return false;
    out = std::move(m_latest);
    m_hasFrame = false;
    return true;
}

bool Flashback::Clip(double in, double out, std::vector<PacketPtr>& video, std::vector<AudioChunk>& audio, EncoderConfig& cfg) const
{
    if (m_video.empty() || out <= in) return false;
    size_t a = FrameAt(int64_t(in * 1e7));
    while (a > 0 && !m_video[a]->keyframe) --a;
    const size_t b = FrameAt(int64_t(out * 1e7));
    if (b <= a) return false;
    video.assign(m_video.begin() + ptrdiff_t(a), m_video.begin() + ptrdiff_t(b) + 1);
    const int64_t from = m_video[a]->time, to = m_video[b]->time + m_video[b]->duration;
    audio.clear();
    for (const auto& c : m_audioChunks)
        if (c.time + 200000 >= from && c.time <= to) audio.push_back(c);
    cfg = m_cfg;
    return true;
}

size_t Flashback::FrameAt(int64_t rel) const
{
    // Binary search on time.
    const int64_t abs = m_t0 + rel;
    auto it = std::upper_bound(m_video.begin(), m_video.end(), abs, [](int64_t t, const PacketPtr& p) { return t < p->time; });
    return it == m_video.begin() ? 0 : size_t(it - m_video.begin()) - 1;
}

void Flashback::PushAudioUntil(int64_t rel)
{
    const int64_t abs = m_t0 + rel;
    while (m_audioNext < m_audioChunks.size() && m_audioChunks[m_audioNext].time <= abs) {
        const AudioChunk& c = m_audioChunks[m_audioNext++];
        if (c.rate == 0 || c.stereo.empty()) continue;
        if (m_audio->InputRate() != c.rate) m_audio->SetInputFormat(c.rate, uint32_t(c.stereo.size() / 2));
        m_audio->Push(c.stereo.data(), uint32_t(c.stereo.size() / 2), c.time);
    }
}

void Flashback::ThreadMain()
{
    ComInit com;
    SetCurrentThreadName(L"Flashback");
    VideoDecoder dec;
    if (!dec.Open(m_dxgi, m_cfg.width, m_cfg.height, m_cfg.fpsNum, m_cfg.fpsDen, m_cfg.hevc, m_cfg.hdr)) {
        LOGW(L"Flashback: no decoder");
        return;
    }
    size_t shown = SIZE_MAX;
    for (;;) {
        double pos;
        bool seek, playing;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(4), [&] { return m_quit || m_seekPending; });
            if (m_quit) break;
            playing = m_playing.load();
            pos = playing ? std::min(m_duration, m_basePos + QpcToMs(QpcNow() - m_baseQpc) / 1000.0) : m_basePos;
            seek = m_seekPending;
            m_seekPending = false;
            if (playing && pos >= m_duration) {
                m_basePos = m_duration;
                m_playing = false;
                m_atEnd = true;
                playing = false;
            }
        }
        const int64_t rel = int64_t(pos * 1e7);
        const size_t target = FrameAt(rel);

        if (seek) {
            // Audio restarts at the new position.
            m_audio->Reset();
            m_audioNext = 0;
            const int64_t abs = m_t0 + rel;
            while (m_audioNext < m_audioChunks.size() && m_audioChunks[m_audioNext].time < abs) ++m_audioNext;
        }
        if (playing) PushAudioUntil(rel + 200000);   // 20 ms ahead; the source's buffer holds it

        if (target == shown) continue;
        // Decode forward from where we are, or from the keyframe before the target.
        size_t from;
        if (m_decoded != SIZE_MAX && target > m_decoded && target - m_decoded <= size_t(m_cfg.gopFrames) + 1) {
            from = m_decoded + 1;
        } else {
            from = target;
            while (from > 0 && !m_video[from]->keyframe) --from;
            dec.Flush();
        }
        CapturedFrame last;
        bool got = false;
        bool failed = false;
        for (size_t i = from; i <= target && !m_quit.load() && !failed; ++i) {   // closing never waits for a long decode
            failed = !dec.Decode(m_video[i]->data, m_video[i]->time, [&](CapturedFrame&& f) {
                last = std::move(f);
                got = true;
            });
            m_decoded = i;
        }
        if (m_quit.load()) break;
        if (failed && !got) {   // this decoder can't show these frames: say so once and stop trying
            LOGW(L"Flashback: the decoder can't show this video");
            break;
        }
        if (!got) {
            // The decoder is holding the frame back: drain it (decoding continues normally afterwards).
            dec.Drain([&](CapturedFrame&& f) {
                last = std::move(f);
                got = true;
            });
        }
        shown = target;
        if (got) {
            std::lock_guard lock(m_frameMutex);
            last.seq = ++m_seq;
            m_latest = std::move(last);
            m_hasFrame = true;
        }
    }
    dec.Close();
}

} // namespace cc
