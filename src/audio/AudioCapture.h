#pragma once
#include "audio/AudioSource.h"

#include <memory>
#include <thread>

namespace cc {

// Captures one audio endpoint (e.g. the capture card's HDMI audio) into an AudioSource.
class AudioCapture {
public:
    AudioCapture(std::wstring endpointId, std::shared_ptr<AudioSource> sink);
    ~AudioCapture();
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    void Start();
    void Stop();

    bool Failed() const { return m_failed.load(); }
    bool Running() const { return m_running.load(); }
    std::wstring Description() const;
    const std::wstring& EndpointId() const { return m_id; }

private:
    void ThreadMain();

    std::wstring m_id;
    std::shared_ptr<AudioSource> m_sink;
    std::thread m_thread;
    HANDLE m_stopEvent = nullptr;
    std::atomic<bool> m_failed{false};
    std::atomic<bool> m_running{false};
    mutable std::mutex m_descMutex;
    std::wstring m_desc;
};

} // namespace cc
