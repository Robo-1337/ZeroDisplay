#pragma once
#include "core/Common.h"

#include <winsock2.h>

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

namespace cc {

// Welcome page for phones joining the app's own Wi-Fi network: a DNS responder that answers every name with
// this PC's hotspot address, and an HTTP server on port 80 of that address. Phones check for internet after
// joining; getting our page instead makes them open it ("Sign in to network"). Both sockets are bound to the
// hotspot address only, never to the home network.
class CaptivePortal {
public:
    ~CaptivePortal();
    bool Start(const std::wstring& address);
    void Stop();
    bool Running() const { return m_thread.joinable(); }
    std::wstring Address() const { return m_address; }

    // What the page shows: the receiver's name and the live viewer ("" while the PC isn't sharing).
    void SetInfo(const std::wstring& receiverName, const std::wstring& viewerUrl);
    int Visitors() const { return m_visitors.load(); }   // page views (probes don't count)
    // Devices that used this network's DNS or welcome page within the last `seconds` (every phone asks the DNS when
    // it joins and while it's in use; the PC's address table doesn't see idle phones on a Wi-Fi Direct network).
    int RecentDevices(double seconds) const;
    std::wstring Error() const;

private:
    struct Client {
        SOCKET sock = INVALID_SOCKET;
        std::string in;
        std::string out;
        int64_t opened = 0;
    };
    void ThreadMain();
    void AnswerDns();

public:
    // The single question of a DNS query: returns false for anything malformed. qEnd = bytes up to the end of the
    // question, qtype = its type. Never reads outside q[0, n).
    static bool ParseQuestion(const uint8_t* q, int n, int& qEnd, int& qtype);
    static bool SelfTest();   // malformed / random packets (run under the sanitizer build too)

private:
    std::string Respond(const std::string& request);

    std::wstring m_address;
    uint32_t m_ip = 0;   // network order
    SOCKET m_dns = INVALID_SOCKET, m_http = INVALID_SOCKET;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_visitors{0};
    std::map<uint32_t, int64_t> m_seen;   // client IPv4 -> last request (QPC), guarded by m_mutex; at most 64
    void Seen(uint32_t ipv4);
    mutable std::mutex m_mutex;
    std::string m_receiver, m_viewer;
    std::wstring m_error;
};

} // namespace cc
