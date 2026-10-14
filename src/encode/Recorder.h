#pragma once
#include "audio/AudioSource.h"
#include "encode/VideoEncoder.h"

#include <mfreadwrite.h>

#include <condition_variable>
#include <deque>
#include <thread>

namespace cc {

struct RecordOptions {
    // Audio tracks in file order. Track 1 is what players use by default (the mix); further tracks
    // (e.g. game and microphone on their own) are for editing. Null entries are skipped.
    std::vector<std::shared_ptr<AudioSource>> tracks;
    int splitSeconds = 0;   // > 0: continue in a new file (…_part2.mp4) at the first keyframe after this long
};

// Writes the shared encoder's H.264/HEVC (passthrough) plus AAC audio to a fragmented MP4, so a crash
// or power loss still leaves a playable file. Starts at the next keyframe.
class Recorder {
public:
    ~Recorder();

    bool Start(const std::wstring& path, VideoEncoder& encoder, const RecordOptions& options);
    void RequestStop();   // detaches from the encoder immediately; the file is finalized by Stop()
    // Pause: nothing is written meanwhile (the encoder keeps running for sharing / replay). Resume continues the same
    // file at the next keyframe, with the paused time cut out of the timeline.
    void SetPaused(bool paused);
    bool Paused() const { return m_pauseWanted.load(); }
    // Writes an already-captured clip (replay buffer) in one go. Starts at the first keyframe.
    bool WriteClip(const std::wstring& path, const EncoderConfig& cfg, const std::vector<PacketPtr>& video,
                   const std::vector<AudioChunk>& audio);
    void Stop();          // finalizes the file; may block briefly

    bool Active() const { return m_active.load(); }
    bool Writing() const { return m_writing.load(); }   // first keyframe arrived, file open
    double Seconds() const { return m_seconds.load(); }   // all parts together
    uint64_t Bytes() const { return m_bytes.load(); }
    std::wstring Path() const;   // the file being written (the latest part)
    int Parts() const { return m_part.load(); }
    int HeaderParts() const { return m_headerParts.load(); }   // new parts because the encoder's stream headers changed
    std::wstring PartPath(int part) const;   // part 1 = the base path, then "<base>_part2.mp4", ...
    int AudioTracks() const { return int(m_tracks.size()); }
    std::wstring Error() const;

private:
    struct Track {
        std::shared_ptr<AudioSource> source;
        std::shared_ptr<AudioTap> tap;
        DWORD stream = 0;
        bool has = false;           // stream exists in the current file
        uint32_t rate = 48000;
        int64_t next = -1;          // 100 ns, file time of the next audio frame
        std::deque<AudioChunk> pending;   // captured before the file opened
        std::deque<AudioChunk> recent;    // last ~1 s, replayed into the next part after a split
    };

    void ThreadMain();
    bool OpenSink(const EncodedPacket& first);
    void FinishFile();
    void WriteVideo(const PacketPtr& p);   // held back one frame: its duration is the time to the next one
    void WriteVideoSample(const EncodedPacket& p, int64_t duration);
    void FlushVideo(int64_t nextTime = -1);
    void WriteAudio(Track& t, const AudioChunk& c);
    void WritePcm(Track& t, const float* stereo, uint32_t frames, int64_t time);
    void Fail(const std::wstring& what, HRESULT hr);

    std::wstring m_basePath;
    std::wstring m_path;   // guarded by m_errorMutex for readers on other threads
    VideoEncoder* m_encoder = nullptr;
    int m_sinkId = 0;
    std::vector<Track> m_tracks;
    int m_splitSeconds = 0;
    std::atomic<int> m_part{1};
    std::atomic<int> m_headerParts{0};
    double m_doneSeconds = 0;   // finished parts
    EncoderConfig m_cfg;

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<PacketPtr> m_packets;
    bool m_stop = false;
    VideoEncoder* m_kfEncoder = nullptr;   // guarded by m_mutex: asks for a keyframe at split points
    std::vector<uint8_t> m_filePs;          // parameter sets (SPS/PPS/VPS) the current file was opened with
    bool m_kfRequested = false;

    // writer thread state
    ComPtr<IMFSinkWriter> m_writer;
    DWORD m_videoStream = 0;
    int64_t m_t0 = 0;
    PacketPtr m_heldVideo;
    int64_t m_videoEnd = 0;   // file time where the last video frame ends
    // Pauses in the current file: [start, end) in encoder time (end < 0 while still paused). Times after a pause
    // move back by its length; anything inside one is dropped.
    std::atomic<bool> m_pauseWanted{false};
    std::vector<std::pair<int64_t, int64_t>> m_pauses;
    bool Dropped(int64_t time) const;     // inside a pause
    int64_t Shift(int64_t time) const;    // total pause length before `time`

    std::atomic<bool> m_active{false}, m_writing{false};
    std::atomic<double> m_seconds{0};
    std::atomic<uint64_t> m_bytes{0};
    mutable std::mutex m_errorMutex;
    std::wstring m_error;
};

} // namespace cc
