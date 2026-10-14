#include "wireless/CaptivePortal.h"
#include "core/Log.h"
#include "net/LandingPage.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <vector>

namespace cc {

CaptivePortal::~CaptivePortal() { Stop(); }

static SOCKET BindTo(int type, uint32_t ip, uint16_t port)
{
    SOCKET s = socket(AF_INET, type, type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = ip;
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || (type == SOCK_STREAM && listen(s, 16) != 0)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    return s;
}

bool CaptivePortal::Start(const std::wstring& address)
{
    Stop();
    IN_ADDR ip{};
    if (InetPtonW(AF_INET, address.c_str(), &ip) != 1) return false;
    m_address = address;
    m_ip = ip.s_addr;
    m_dns = BindTo(SOCK_DGRAM, m_ip, 53);
    const int dnsErr = m_dns == INVALID_SOCKET ? WSAGetLastError() : 0;
    m_http = BindTo(SOCK_STREAM, m_ip, 80);
    const int httpErr = m_http == INVALID_SOCKET ? WSAGetLastError() : 0;
    if (m_http == INVALID_SOCKET) {
        if (m_dns != INVALID_SOCKET) closesocket(m_dns);
        m_dns = INVALID_SOCKET;
        std::lock_guard lock(m_mutex);
        m_error = Format(L"port 80 on %s is in use (%d)", address.c_str(), httpErr);
        LOGW(L"Welcome page: %s", m_error.c_str());
        return false;
    }
    // Without DNS phones don't find the page by themselves, but the QR code still takes them there.
    if (m_dns == INVALID_SOCKET) LOGW(L"Welcome page: DNS port 53 on %s unavailable (%d)", address.c_str(), dnsErr);
    m_stop = false;
    m_thread = std::thread([this] { ThreadMain(); });
    LOGI(L"Welcome page on http://%s/ (DNS %s)", address.c_str(), m_dns != INVALID_SOCKET ? L"on" : L"off");
    return true;
}

void CaptivePortal::Stop()
{
    if (!m_thread.joinable()) return;
    m_stop = true;
    m_thread.join();
    if (m_dns != INVALID_SOCKET) closesocket(m_dns);
    if (m_http != INVALID_SOCKET) closesocket(m_http);
    m_dns = m_http = INVALID_SOCKET;
    LOGI(L"Welcome page stopped (%d visit(s))", m_visitors.load());
}

void CaptivePortal::SetInfo(const std::wstring& receiverName, const std::wstring& viewerUrl)
{
    std::lock_guard lock(m_mutex);
    m_receiver = Narrow(receiverName);
    m_viewer = Narrow(viewerUrl);
}

std::wstring CaptivePortal::Error() const
{
    std::lock_guard lock(m_mutex);
    return m_error;
}

// Every A query gets this PC's hotspot address; anything else gets an empty answer (no IPv6, so phones use IPv4).
void CaptivePortal::AnswerDns()
{
    for (;;) {
        uint8_t q[512];
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int n = recvfrom(m_dns, reinterpret_cast<char*>(q), sizeof(q), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n <= 0) return;
        Seen(from.sin_addr.s_addr);
        int qEnd = 0, qtype = 0;
        if (!ParseQuestion(q, n, qEnd, qtype)) continue;
        std::vector<uint8_t> r(q, q + qEnd);
        r[2] = uint8_t(0x80 | (q[2] & 0x79));   // response, same opcode, keep RD
        r[3] = 0x80;                            // recursion available, no error
        const bool answer = qtype == 1 || qtype == 255;
        r[6] = 0;
        r[7] = answer ? 1 : 0;
        r[8] = r[9] = r[10] = r[11] = 0;
        if (answer) {
            const uint8_t* ip = reinterpret_cast<const uint8_t*>(&m_ip);
            const uint8_t a[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 10, 0, 4, ip[0], ip[1], ip[2], ip[3]};
            r.insert(r.end(), std::begin(a), std::end(a));
        }
        sendto(m_dns, reinterpret_cast<const char*>(r.data()), int(r.size()), 0, reinterpret_cast<sockaddr*>(&from), fromLen);
    }
}

bool CaptivePortal::ParseQuestion(const uint8_t* q, int n, int& qEnd, int& qtype)
{
    if (!q || n < 12 || n > 512) return false;
    if ((q[2] & 0x80) || ((q[4] << 8) | q[5]) != 1) return false;   // a query with exactly one question
    int p = 12;
    for (;;) {
        if (p >= n) return false;
        const int len = q[p];
        if (len == 0) break;
        if (len & 0xC0) return false;   // compressed names don't occur in questions
        p += len + 1;
    }
    if (p + 5 > n) return false;        // terminator + type + class
    qEnd = p + 5;
    qtype = (q[p + 1] << 8) | q[p + 2];
    return true;
}

bool CaptivePortal::SelfTest()
{
    // A valid query for "a.b" type A.
    const uint8_t ok[] = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0, 1, 'a', 1, 'b', 0, 0, 1, 0, 1};
    int e = 0, t = 0;
    if (!ParseQuestion(ok, int(sizeof(ok)), e, t) || e != int(sizeof(ok)) || t != 1) return false;
    // Truncated everywhere: never accepted, never reads past n (the sanitizer build checks the reads).
    for (int n = 0; n < int(sizeof(ok)); ++n) {
        std::vector<uint8_t> cut(ok, ok + n);
        if (ParseQuestion(cut.data(), n, e, t)) return false;
    }
    // Labels pointing past the end, compression, random packets.
    uint32_t rng = 12345;
    for (int i = 0; i < 20000; ++i) {
        const int n = 12 + int(rng % 500);
        std::vector<uint8_t> b(static_cast<size_t>(n));
        for (auto& x : b) {
            rng = rng * 1664525u + 1013904223u;
            x = uint8_t(rng >> 24);
        }
        b[2] = 0x01;
        b[4] = 0;
        b[5] = 1;
        if (ParseQuestion(b.data(), n, e, t) && (e > n || e < 17)) return false;
    }
    return true;
}

std::string CaptivePortal::Respond(const std::string& request)
{
    const size_t sp1 = request.find(' '), sp2 = request.find(' ', sp1 + 1);
    const std::string path = sp1 != std::string::npos && sp2 != std::string::npos ? request.substr(sp1 + 1, sp2 - sp1 - 1) : "/";
    std::string lower = request;
    for (auto& ch : lower) ch = char(tolower(uint8_t(ch)));
    std::string ua;
    if (size_t at = lower.find("\r\nuser-agent:"); at != std::string::npos) ua = lower.substr(at + 13, lower.find("\r\n", at + 13) - at - 13);
    std::string receiver, viewer;
    {
        std::lock_guard lock(m_mutex);
        receiver = m_receiver;
        viewer = m_viewer;
    }
    std::string type = "text/html; charset=utf-8", body;
    if (path == "/status") {
        type = "application/json";
        body = "{\"viewer\":\"" + viewer + "\"}";
    } else if (path == "/favicon.ico") {
        return "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    } else {
        // Probes (Android generate_204, Apple hotspot-detect, Windows connecttest) get the page too: that is what
        // makes the phone show it. Count only real visits (the page's first request from a browser or sheet).
        const char* os = ua.find("android") != std::string::npos ? "android"
                         : (ua.find("iphone") != std::string::npos || ua.find("ipad") != std::string::npos || ua.find("captivenetworksupport") != std::string::npos) ? "ios"
                         : ua.find("windows") != std::string::npos ? "windows"
                                                                     : "other";
        body = web::kLandingPage;
        auto fill = [&](const std::string& key, const std::string& value) {
            for (size_t at = body.find(key); at != std::string::npos; at = body.find(key, at + value.size())) body.replace(at, key.size(), value);
        };
        fill("{{OS}}", os);
        fill("{{RECEIVER}}", receiver.empty() ? "ZeroDisplay" : receiver);
        if (ua.find("mozilla") != std::string::npos) ++m_visitors;
    }
    return "HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nCache-Control: no-store\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
}

void CaptivePortal::ThreadMain()
{
    SetCurrentThreadName(L"Welcome page");
    SetThreadEfficiency(true);
    std::vector<Client> clients;
    while (!m_stop) {
        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(m_http, &rd);
        if (m_dns != INVALID_SOCKET) FD_SET(m_dns, &rd);
        for (auto& c : clients) {
            if (c.out.empty()) FD_SET(c.sock, &rd);
            else FD_SET(c.sock, &wr);
        }
        timeval tv{0, 200000};
        if (select(0, &rd, &wr, nullptr, &tv) == SOCKET_ERROR) {
            Sleep(50);
            continue;
        }
        if (m_dns != INVALID_SOCKET && FD_ISSET(m_dns, &rd)) AnswerDns();
        if (FD_ISSET(m_http, &rd) && clients.size() < 32) {
            sockaddr_in peer{};
            int peerLen = sizeof(peer);
            SOCKET s = accept(m_http, reinterpret_cast<sockaddr*>(&peer), &peerLen);
            if (s != INVALID_SOCKET) {
                Seen(peer.sin_addr.s_addr);
                u_long nb = 1;
                ioctlsocket(s, FIONBIO, &nb);
                clients.push_back({s, {}, {}, QpcNow()});
            }
        }
        for (auto& c : clients) {
            if (FD_ISSET(c.sock, &rd)) {
                char buf[4096];
                const int n = recv(c.sock, buf, sizeof(buf), 0);
                if (n <= 0 || c.in.size() > 16384) {
                    closesocket(c.sock);
                    c.sock = INVALID_SOCKET;
                    continue;
                }
                c.in.append(buf, size_t(n));
                if (c.in.find("\r\n\r\n") != std::string::npos) c.out = Respond(c.in);
            } else if (FD_ISSET(c.sock, &wr)) {
                const int n = send(c.sock, c.out.data(), int(c.out.size()), 0);
                if (n > 0) c.out.erase(0, size_t(n));
                if (n <= 0 || c.out.empty()) {
                    shutdown(c.sock, SD_SEND);
                    closesocket(c.sock);
                    c.sock = INVALID_SOCKET;
                }
            } else if (QpcElapsedMs(c.opened) > 10000) {
                closesocket(c.sock);
                c.sock = INVALID_SOCKET;
            }
        }
        std::erase_if(clients, [](const Client& c) { return c.sock == INVALID_SOCKET; });
    }
    for (auto& c : clients) closesocket(c.sock);
}

void CaptivePortal::Seen(uint32_t ipv4)
{
    std::lock_guard lock(m_mutex);
    m_seen[ipv4] = QpcNow();
    while (m_seen.size() > 64) {   // forget the longest silent one (a flood of spoofed senders can't grow this)
        auto oldest = m_seen.begin();
        for (auto it = m_seen.begin(); it != m_seen.end(); ++it)
            if (it->second < oldest->second) oldest = it;
        m_seen.erase(oldest);
    }
}

int CaptivePortal::RecentDevices(double seconds) const
{
    std::lock_guard lock(m_mutex);
    int n = 0;
    for (const auto& [ip, at] : m_seen) n += QpcElapsedMs(at) <= seconds * 1000.0 ? 1 : 0;
    return n;
}

} // namespace cc
