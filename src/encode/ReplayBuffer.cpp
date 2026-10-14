#include "encode/ReplayBuffer.h"
#include "core/Log.h"
#include "encode/H264.h"

namespace cc {

ReplayBuffer::~ReplayBuffer() { Stop(); }

void ReplayBuffer::Start(VideoEncoder& encoder, const std::shared_ptr<AudioSource>& audio, double seconds)
{
    Stop();
    m_seconds = std::max(5.0, seconds);
    m_cfg = encoder.Config();
    m_audioSource = audio;
    if (audio) {
        m_tap = std::make_shared<AudioTap>();
        audio->AddTap(m_tap);
    }
    m_encoder = &encoder;
    m_sinkId = encoder.AddSink([this](const PacketPtr& p) { OnPacket(p); });
    LOGI(L"Replay buffer: keeping the last %.0f s", m_seconds);
}

void ReplayBuffer::Stop()
{
    if (m_encoder) m_encoder->RemoveSink(m_sinkId);
    m_encoder = nullptr;
    if (m_audioSource && m_tap) m_audioSource->RemoveTap(m_tap.get());
    m_tap.reset();
    m_audioSource.reset();
    std::lock_guard lock(m_mutex);
    m_video.clear();
    m_audio.clear();
}

void ReplayBuffer::OnPacket(const PacketPtr& p)
{
    std::lock_guard lock(m_mutex);
    if (p->keyframe) {
        // Video from before an encoder restart has other stream headers and can't share a clip with
        // what follows: start over from this keyframe.
        std::vector<uint8_t> ps = m_cfg.hevc ? h264::ExtractParameterSetsHevc(p->data.data(), p->data.size())
                                             : h264::ExtractParameterSets(p->data.data(), p->data.size());
        if (!ps.empty() && ps != m_ps) {
            if (!m_ps.empty()) m_video.clear();
            m_ps = std::move(ps);
        }
    }
    m_video.push_back(p);
    AudioChunk c;
    while (m_tap && m_tap->Pop(c)) m_audio.push_back(std::move(c));
    Trim();
}

void ReplayBuffer::SetSeconds(double seconds)
{
    std::lock_guard lock(m_mutex);
    if (std::max(5.0, seconds) == m_seconds) return;   // the slider repeats while dragging
    m_seconds = std::max(5.0, seconds);
    Trim();
    LOGI(L"Replay buffer: keeping the last %.0f s (length changed, buffer kept)", m_seconds);
}

void ReplayBuffer::Trim()
{
    if (m_video.empty()) return;
    const int64_t newest = m_video.back()->time;
    const int64_t keep = int64_t(m_seconds * 1e7);
    // Drop whole GOPs: only remove up to a keyframe that is still old enough.
    for (;;) {
        size_t nextKey = 0;
        for (size_t i = 1; i < m_video.size(); ++i)
            if (m_video[i]->keyframe) {
                nextKey = i;
                break;
            }
        if (nextKey == 0 || newest - m_video[nextKey]->time < keep) break;
        m_video.erase(m_video.begin(), m_video.begin() + ptrdiff_t(nextKey));
    }
    const int64_t oldest = m_video.front()->time;
    while (!m_audio.empty() && m_audio.front().time < oldest - 2000000) m_audio.pop_front();
}

double ReplayBuffer::BufferedSeconds() const
{
    std::lock_guard lock(m_mutex);
    if (m_video.size() < 2) return 0;
    return double(m_video.back()->time - m_video.front()->time) / 1e7;
}

bool ReplayBuffer::Snapshot(std::vector<PacketPtr>& video, std::vector<AudioChunk>& audio, EncoderConfig& cfg) const
{
    std::lock_guard lock(m_mutex);
    if (m_video.empty()) return false;
    video.assign(m_video.begin(), m_video.end());
    cfg = m_cfg;
    if (!m_audioSource) {   // started without sound (no audio device): a clip without a sound track
        audio.assign(m_audio.begin(), m_audio.end());
        return true;
    }
    // Where the device delivered no sound (its driver stopped, or a gap), silence fills in, so a replay always has
    // its sound track over the whole picture, like a recording.
    const uint32_t rate = m_audio.empty() ? 48000u : std::max(8000u, m_audio.front().rate);
    const uint32_t frames = rate / 100;                    // 10 ms chunks
    const int64_t step = int64_t(frames) * 10000000 / rate;
    const int64_t gap = 1000000;                           // gaps from 100 ms on
    auto silence = [&](int64_t from, int64_t to) {
        for (int64_t t = from; t + step <= to; t += step) audio.push_back({t, rate, std::vector<float>(size_t(frames) * 2, 0.0f)});
    };
    audio.clear();
    int64_t covered = m_video.front()->time;
    for (const AudioChunk& c : m_audio) {
        if (c.time - covered > gap) silence(covered, c.time);
        audio.push_back(c);
        if (c.rate) covered = std::max(covered, c.time + int64_t(c.stereo.size() / 2) * 10000000 / int64_t(c.rate));
    }
    const int64_t end = m_video.back()->time + std::max<int64_t>(0, m_video.back()->duration);
    if (end - covered > gap) silence(covered, end);
    return true;
}
} // namespace cc
