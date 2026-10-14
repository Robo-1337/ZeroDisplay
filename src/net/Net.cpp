#include "net/Net.h"

#include <iphlpapi.h>
#include <mswsock.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <random>

namespace cc::net {

uint64_t InstanceId()
{
    static const uint64_t id = [] {
        std::random_device rd;
        return (uint64_t(rd()) << 32) ^ rd() ^ uint64_t(QpcNow()) ^ GetCurrentProcessId();
    }();
    return id;
}

std::wstring ComputerName()
{
    wchar_t buf[256];
    DWORD n = DWORD(std::size(buf));
    if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, buf, &n)) return std::wstring(buf, n);
    return L"PC";
}

std::vector<std::wstring> LocalIPv4()
{
    std::vector<std::wstring> out;
    ULONG size = 16 * 1024;
    std::vector<BYTE> buf(size);
    auto addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    }
    flags |= GAA_FLAG_INCLUDE_GATEWAYS;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size) != NO_ERROR) return out;
    // Best address first: wired with a router, then Wi-Fi, then the rest (virtual switches, VPNs).
    std::vector<std::pair<int, std::wstring>> scored;
    for (auto a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        int score = a->IfType == IF_TYPE_ETHERNET_CSMACD ? 30 : (a->IfType == IF_TYPE_IEEE80211 ? 20 : 0);
        if (a->FirstGatewayAddress) score += 10;
        const std::wstring desc = a->Description ? a->Description : L"";
        if (desc.find(L"Virtual") != std::wstring::npos || desc.find(L"Hyper-V") != std::wstring::npos || desc.find(L"VPN") != std::wstring::npos)
            score -= 25;
        for (auto u = a->FirstUnicastAddress; u; u = u->Next) {
            auto sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            if (sin->sin_family != AF_INET) continue;
            if ((ntohl(sin->sin_addr.s_addr) >> 16) == 0xA9FE) continue;   // 169.254.x.x: no DHCP, unreachable
            scored.emplace_back(score, AddrToString(*sin));
        }
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    for (auto& s : scored) out.push_back(std::move(s.second));
    return out;
}

std::wstring AddrToString(const sockaddr_in& a, bool withPort)
{
    wchar_t buf[64] = {};
    InetNtopW(AF_INET, &a.sin_addr, buf, std::size(buf));
    std::wstring s = buf;
    if (withPort) s += L":" + std::to_wstring(ntohs(a.sin_port));
    return s;
}

bool ParseEndpoint(const std::wstring& text, sockaddr_in& out, uint16_t defaultPort)
{
    std::wstring host = text;
    uint16_t port = defaultPort;
    size_t colon = text.rfind(L':');
    if (colon != std::wstring::npos) {
        host = text.substr(0, colon);
        port = uint16_t(_wtoi(text.c_str() + colon + 1));
    }
    out = {};
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    if (InetPtonW(AF_INET, host.c_str(), &out.sin_addr) == 1) return true;
    // Host name
    ADDRINFOW hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    ADDRINFOW* res = nullptr;
    if (GetAddrInfoW(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    FreeAddrInfoW(res);
    return true;
}

void CopyName(wchar_t (&dst)[64], const std::wstring& src)
{
    size_t n = std::min<size_t>(src.size(), 63);
    memcpy(dst, src.data(), n * sizeof(wchar_t));
    dst[n] = 0;
}

bool UdpSocket::Open(uint16_t port, bool reuse, bool broadcast, int bufferBytes)
{
    Close();
    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) return false;
    BOOL on = TRUE;
    if (reuse) setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
    if (broadcast) setsockopt(m_sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
    setsockopt(m_sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufferBytes), sizeof(bufferBytes));
    setsockopt(m_sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufferBytes), sizeof(bufferBytes));
    // Don't let an ICMP "port unreachable" from a vanished peer break recvfrom.
    DWORD noReset = FALSE;
    DWORD ret = 0;
    WSAIoctl(m_sock, SIO_UDP_CONNRESET, &noReset, sizeof(noReset), nullptr, 0, &ret, nullptr, nullptr);
    // Low-latency DSCP class (best effort if not allowed).
    int tos = 0xB8;   // EF
    setsockopt(m_sock, IPPROTO_IP, IP_TOS, reinterpret_cast<const char*>(&tos), sizeof(tos));

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(m_sock, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        Close();
        return false;
    }
    m_event = WSACreateEvent();
    WSAEventSelect(m_sock, m_event, FD_READ);
    return true;
}

void UdpSocket::Close()
{
    if (m_sock != INVALID_SOCKET) closesocket(m_sock);
    m_sock = INVALID_SOCKET;
    if (m_event) WSACloseEvent(m_event);
    m_event = nullptr;
}

bool UdpSocket::SendTo(const sockaddr_in& to, const void* data, size_t size)
{
    int r = sendto(m_sock, static_cast<const char*>(data), int(size), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    return r == int(size);
}

int UdpSocket::RecvFrom(void* buf, size_t size, sockaddr_in& from)
{
    int len = sizeof(from);
    int r = recvfrom(m_sock, static_cast<char*>(buf), int(size), 0, reinterpret_cast<sockaddr*>(&from), &len);
    if (r == SOCKET_ERROR) {
        int e = WSAGetLastError();
        return (e == WSAEWOULDBLOCK || e == WSAEMSGSIZE) ? 0 : -1;
    }
    return r;
}

uint16_t UdpSocket::LocalPort() const
{
    sockaddr_in a{};
    int len = sizeof(a);
    if (getsockname(m_sock, reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
    return ntohs(a.sin_port);
}

} // namespace cc::net
