#include "net/Discovery.h"
#include "core/Log.h"

#include <algorithm>

namespace cc {

using namespace net;

void Discovery::Start()
{
    if (m_thread.joinable()) return;
    if (!m_sock.Open(0, false, true, 64 * 1024)) {
        LOGW(L"Discovery: no socket");
        return;
    }
    m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_stop || !m_wake) {
        LOGW(L"Discovery: no events");
        return;
    }
    m_thread = std::thread([this] { ThreadMain(); });
}

void Discovery::SetActive(bool active)
{
    if (m_active.exchange(active) != active && active) {
        m_activeSince = QpcNow();
        if (m_wake) SetEvent(m_wake);   // look right away
    }
}

void Discovery::SetPaused(bool paused)
{
    if (m_paused.exchange(paused) != paused && !paused && m_wake) SetEvent(m_wake);
}

void Discovery::Stop()
{
    if (!m_thread.joinable()) return;
    SetEvent(m_stop);
    m_thread.join();
    CloseHandle(m_stop);
    m_stop = nullptr;
    if (m_wake) CloseHandle(m_wake);
    m_wake = nullptr;
    m_sock.Close();
    std::lock_guard lock(m_mutex);   // stopped: nothing is known any more (wireless features off)
    if (!m_sources.empty()) m_changed = true;
    m_sources.clear();
}

std::vector<NetSource> Discovery::Sources() const
{
    std::lock_guard lock(m_mutex);
    return m_sources;
}

void Discovery::ThreadMain()
{
    SetCurrentThreadName(L"Discovery");
    SetThreadEfficiency(true);
    std::vector<uint8_t> buf(4096);
    int64_t lastProbe = 0;
    bool woken = false;
    for (;;) {
        const double interval = m_active ? 2000.0 : 30000.0;
        const double sinceProbe = lastProbe ? QpcToMs(QpcNow() - lastProbe) : interval;
        const DWORD wait = m_paused ? INFINITE : DWORD(std::clamp(interval - sinceProbe, 50.0, interval));
        HANDLE handles[3] = {m_stop, m_sock.Event(), m_wake};
        const DWORD w = WaitForMultipleObjects(3, handles, FALSE, wait);
        if (w == WAIT_OBJECT_0) break;
        woken = w == WAIT_OBJECT_0 + 2;
        RefreshThreadEfficiency();
        const int64_t now = QpcNow();
        if (!m_paused && (woken || !lastProbe || QpcToMs(now - lastProbe) >= interval - 1)) {
            lastProbe = now;
            Header probe{kMagic, Msg::Discover};
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(kDiscoveryPort);
            to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            m_sock.SendTo(to, &probe, sizeof(probe));
            to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // instances on this PC
            m_sock.SendTo(to, &probe, sizeof(probe));
        }
        WSAResetEvent(m_sock.Event());
        sockaddr_in from{};
        int n;
        while ((n = m_sock.RecvFrom(buf.data(), buf.size(), from)) > 0) {
            if (n < int(sizeof(AnnounceMsg))) continue;
            const auto* a = reinterpret_cast<const AnnounceMsg*>(buf.data());
            if (a->h.magic != kMagic || a->h.type != Msg::Announce || a->instance == InstanceId()) continue;
            NetSource s;
            sockaddr_in ep = from;
            ep.sin_port = htons(a->port);
            s.address = AddrToString(ep, true);
            s.server.assign(a->server, wcsnlen(a->server, 64));
            s.device.assign(a->device, wcsnlen(a->device, 64));
            s.width = a->width;
            s.height = a->height;
            s.fps = a->fpsDen ? double(a->fpsNum) / a->fpsDen : 0;
            s.viewers = a->viewers;
            s.instance = a->instance;
            s.lastSeenQpc = now;
            std::lock_guard lock(m_mutex);
            bool found = false;
            for (auto& e : m_sources) {
                // The same instance may answer on several interfaces (LAN + loopback): keep one entry.
                if (e.instance == s.instance) {
                    found = true;
                    if (e.device != s.device || e.width != s.width) m_changed = true;
                    bool keepLoopback = e.address.rfind(L"127.", 0) == 0;
                    std::wstring addr = keepLoopback ? e.address : s.address;
                    e = s;
                    e.address = addr;
                }
            }
            if (!found) {
                m_sources.push_back(s);
                m_changed = true;
                LOGI(L"Discovery: found %s sharing %s at %s", s.server.c_str(), s.device.c_str(), s.address.c_str());
            }
        }
        std::lock_guard lock(m_mutex);
        size_t before = m_sources.size();
        // 3.5 probe intervals; right after becoming active, entries from the slow probes stay until answers come in.
        const bool fast = m_active && QpcToMs(now - m_activeSince.load()) > 7000.0;
        const double expire = m_paused ? 1e12 : (fast ? 7000.0 : 105000.0);
        std::erase_if(m_sources, [&](const NetSource& e) { return QpcToMs(now - e.lastSeenQpc) > expire; });
        if (m_sources.size() != before) m_changed = true;
    }
}

} // namespace cc
