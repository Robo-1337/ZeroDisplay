#pragma once
#include "audio/AudioSource.h"
#include "encode/VideoEncoder.h"

#include <deque>

namespace cc {

// Keeps the last N seconds of encoded video (and audio) in memory; Snapshot() + Recorder::WriteClip save them.
class ReplayBuffer {
public:
    ~ReplayBuffer();

    void Start(VideoEncoder& encoder, const std::shared_ptr<AudioSource>& audio, double seconds);
    void Stop();
    // New length without losing what's buffered: longer keeps everything (and fills up), shorter drops the oldest.
    void SetSeconds(double seconds);
    bool Running() const { return m_encoder != nullptr; }
    double BufferedSeconds() const;
    // Copies what is buffered (for writing with Recorder::WriteClip on another thread).
    bool Snapshot(std::vector<PacketPtr>& video, std::vector<AudioChunk>& audio, EncoderConfig& cfg) const;

private:
    void OnPacket(const PacketPtr& p);
    void Trim();

    VideoEncoder* m_encoder = nullptr;
    int m_sinkId = 0;
    EncoderConfig m_cfg;
    std::shared_ptr<AudioSource> m_audioSource;
    std::shared_ptr<AudioTap> m_tap;
    double m_seconds = 30;
    mutable std::mutex m_mutex;
    std::deque<PacketPtr> m_video;
    std::vector<uint8_t> m_ps;   // parameter sets of the buffered video (a change = the encoder restarted)
    std::deque<AudioChunk> m_audio;
};

} // namespace cc
