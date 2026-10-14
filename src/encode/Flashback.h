#pragma once
#include "audio/AudioSource.h"
#include "encode/VideoDecoder.h"
#include "encode/VideoEncoder.h"

#include <atomic>
#include <condition_variable>
#include <thread>

namespace cc {

// Flashback: plays back a snapshot of the replay buffer (decoded on a worker thread) while capture,
// recording and sharing carry on live. Times are seconds from the start of the snapshot.
class Flashback {
public:
    ~Flashback() { Stop(); }
    bool Start(IMFDXGIDeviceManager* dxgi, const EncoderConfig& cfg, std::vector<PacketPtr> video, std::vector<AudioChunk> audio);
    void Stop();

    double Duration() const { return m_duration; }
    double Position() const;
    void Seek(double seconds);
    void SetPlaying(bool on);
    bool Playing() const { return m_playing.load(); }
    bool AtEnd() const { return m_atEnd.load(); }
    bool TakeFrame(CapturedFrame& out);
    // Audio of the snapshot, paced with playback; the app monitors it instead of the live audio.
    const std::shared_ptr<AudioSource>& Audio() const { return m_audio; }
    // Copies the part between `in` and `out` (seconds) for Recorder::WriteClip. Starts at the keyframe at
    // or before `in` (passthrough: nothing is re-encoded).
    bool Clip(double in, double out, std::vector<PacketPtr>& video, std::vector<AudioChunk>& audio, EncoderConfig& cfg) const;

private:
    void ThreadMain();
    size_t FrameAt(int64_t rel) const;   // last frame whose time <= rel
    void PushAudioUntil(int64_t rel);

    EncoderConfig m_cfg;
    std::vector<PacketPtr> m_video;      // starts with a keyframe
    std::vector<AudioChunk> m_audioChunks;
    int64_t m_t0 = 0;                    // time of the first frame (100 ns)
    double m_duration = 0;
    IMFDXGIDeviceManager* m_dxgi = nullptr;
    std::shared_ptr<AudioSource> m_audio;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_thread;
    std::atomic<bool> m_quit{false};
    double m_basePos = 0;        // position when playback last started / seek target
    int64_t m_baseQpc = 0;       // QPC when playback last started
    bool m_seekPending = false;
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_atEnd{false};

    std::mutex m_frameMutex;
    CapturedFrame m_latest;
    bool m_hasFrame = false;
    uint64_t m_seq = 0;

    // worker state
    size_t m_decoded = SIZE_MAX;   // last frame index fed to the decoder
    size_t m_audioNext = 0;        // next audio chunk to push
};

} // namespace cc
