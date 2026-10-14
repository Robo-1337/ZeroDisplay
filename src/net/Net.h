#pragma once
#include "core/Common.h"

#include <winsock2.h>
#include <ws2tcpip.h>

namespace cc::net {

constexpr uint16_t kDiscoveryPort = 47800;
constexpr uint16_t kStreamPort = 47801;
constexpr uint32_t kMagic = 0x31534343;   // "CCS1"
constexpr size_t kMaxPayload = 1200;      // stays under a 1500-byte MTU with IP/UDP headers
constexpr uint32_t kAudioChunkFrames = 240;   // 5 ms at 48 kHz

enum class Msg : uint8_t {
    Hello = 1,        // viewer -> server: please stream to me
    Config = 2,       // server -> viewer: stream description
    Video = 3,        // server -> viewer: fragment of an H.264 access unit
    Audio = 4,        // server -> viewer: PCM s16 stereo
    Keepalive = 5,    // viewer -> server, once a second
    KeyRequest = 6,   // viewer -> server: lost data, send a keyframe
    Bye = 7,          // either way
    Discover = 8,     // viewer broadcast
    Announce = 9,     // server -> viewer: "I'm sharing"
    Nack = 10,        // viewer -> server: resend these fragments of a frame (Wi-Fi loss repair)
    Denied = 11,      // server -> viewer: no stream without the right PIN (h.flags: DenyReason)
};

enum class DenyReason : uint8_t { PinNeeded = 1, WrongPin = 2, Wait = 3 };

#pragma pack(push, 1)
struct Header {
    uint32_t magic = kMagic;
    Msg type;
    uint8_t flags = 0;
    uint16_t reserved = 0;
};

struct HelloMsg {   // a Hello may be just the header (no PIN)
    Header h{kMagic, Msg::Hello};
    char pin[8] = {};   // the sharing PIN as typed, not NUL-terminated when 8 long
};

struct DeniedMsg {   // never bigger than a Hello: a spoofed Hello can't be used to flood someone
    Header h{kMagic, Msg::Denied};
};

struct ConfigMsg {
    Header h{kMagic, Msg::Config};
    uint32_t width, height, fpsNum, fpsDen;
    uint32_t audioRate;   // 0 = no audio
    uint64_t instance;
    wchar_t server[64];
    wchar_t device[64];
};

struct VideoMsg {   // followed by payload; h.flags bit 0 = keyframe
    Header h{kMagic, Msg::Video};
    uint32_t frameId;
    uint16_t fragIndex, fragCount;
    uint32_t totalSize;
    int64_t time;   // 100 ns, sender's QPC clock
};

struct AudioMsg {   // followed by frames * 4 bytes of s16 stereo
    Header h{kMagic, Msg::Audio};
    uint32_t seq;
    int64_t time;
    uint16_t frames;
};

struct KeepaliveMsg {
    Header h{kMagic, Msg::Keepalive};
    uint32_t framesReceived;
    uint32_t framesLost;
};

constexpr uint16_t kNackWholeFrame = 0xFFFF;
constexpr size_t kMaxNackFrags = 64;

struct NackMsg {
    Header h{kMagic, Msg::Nack};
    uint32_t frameId;
    uint16_t count;   // number of entries in frags, or kNackWholeFrame (nothing of it arrived)
    uint16_t frags[kMaxNackFrags];
};

struct AnnounceMsg {
    Header h{kMagic, Msg::Announce};
    uint64_t instance;
    uint16_t port;
    uint16_t viewers;
    uint32_t width, height, fpsNum, fpsDen;
    wchar_t server[64];
    wchar_t device[64];
};
#pragma pack(pop)

// Random per-process id so an instance can ignore its own announcements.
uint64_t InstanceId();
std::wstring ComputerName();
std::vector<std::wstring> LocalIPv4();
std::wstring AddrToString(const sockaddr_in& a, bool withPort = false);
bool ParseEndpoint(const std::wstring& text, sockaddr_in& out, uint16_t defaultPort);
void CopyName(wchar_t (&dst)[64], const std::wstring& src);

// Non-blocking UDP socket with an event that is signalled when data arrives.
class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket() { Close(); }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool Open(uint16_t port, bool reuse = false, bool broadcast = false, int bufferBytes = 4 << 20);
    void Close();
    bool SendTo(const sockaddr_in& to, const void* data, size_t size);
    // Returns bytes received, 0 when nothing is pending, -1 on error.
    int RecvFrom(void* buf, size_t size, sockaddr_in& from);
    HANDLE Event() const { return m_event; }
    bool Valid() const { return m_sock != INVALID_SOCKET; }
    uint16_t LocalPort() const;

private:
    SOCKET m_sock = INVALID_SOCKET;
    HANDLE m_event = nullptr;
};

} // namespace cc::net
