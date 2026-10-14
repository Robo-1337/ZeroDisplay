#pragma once
#include "core/Common.h"

#include <condition_variable>
#include <thread>

namespace cc {

// Cuts [start, end] out of an MP4 without re-encoding (every video and audio track is copied as is).
// The cut starts at the keyframe at or before `start`. Returns the actual start used (seconds).
bool TrimFile(const std::wstring& in, const std::wstring& out, double start, double end, std::wstring& error, double* actualStart = nullptr);

// Decodes single preview frames of a file (BGRA) on a worker thread; the newest request wins.
class TrimPreview {
public:
    ~TrimPreview() { Close(); }
    bool Open(const std::wstring& path);   // reads the duration; frames are decoded on demand
    void Close();
    double Duration() const { return m_duration; }
    void Request(double seconds);
    // Latest decoded frame, if a new one is ready.
    bool Take(std::vector<uint8_t>& bgra, UINT& width, UINT& height);

private:
    void ThreadMain();

    std::wstring m_path;
    double m_duration = 0;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_quit = false;
    double m_request = -1;
    std::vector<uint8_t> m_image;
    UINT m_w = 0, m_h = 0;
    bool m_ready = false;
};

} // namespace cc
