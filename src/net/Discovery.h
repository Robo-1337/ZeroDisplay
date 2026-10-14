#pragma once
#include "net/Net.h"

#include <thread>

namespace cc {

struct NetSource {
    std::wstring address;   // "ip:port"
    std::wstring server;    // computer name
    std::wstring device;    // device being shared
    uint32_t width = 0, height = 0;
    double fps = 0;
    int viewers = 0;
    uint64_t instance = 0;
    int64_t lastSeenQpc = 0;
};

// Finds ZeroDisplay instances sharing on the LAN: a broadcast every 2 s while someone may be looking (device menu,
// Settings, the first seconds), every 30 s otherwise (light: few wake-ups when idle). Entries expire after
// 3.5 probe intervals. Paused: no probes at all (Battery saver). Stopped: socket closed, list emptied.
class Discovery {
public:
    ~Discovery() { Stop(); }
    void Start();
    void Stop();
    void SetActive(bool active);   // cheap; call whenever
    void SetPaused(bool paused);
    std::vector<NetSource> Sources() const;
    bool TakeChanged() { return m_changed.exchange(false); }

private:
    void ThreadMain();

    net::UdpSocket m_sock;
    std::thread m_thread;
    HANDLE m_stop = nullptr;
    HANDLE m_wake = nullptr;
    std::atomic<bool> m_active{true}, m_paused{false};
    std::atomic<int64_t> m_activeSince{0};
    mutable std::mutex m_mutex;
    std::vector<NetSource> m_sources;
    std::atomic<bool> m_changed{false};
};

} // namespace cc
