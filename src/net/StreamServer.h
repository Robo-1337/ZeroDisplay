#pragma once
#include "audio/AudioSource.h"
#include "encode/VideoEncoder.h"
#include "net/Net.h"
#include "net/PinGate.h"

#include <deque>
#include <thread>

namespace cc {

// Shares the encoder's H.264 and raw PCM audio with ZeroDisplay viewers on the LAN over UDP.
// Answers discovery broadcasts so viewers find it automatically.
class StreamServer {
public:
    ~StreamServer();

    bool Start(uint16_t port = net::kStreamPort);
    void Stop();
    bool Running() const { return m_running.load(); }

    // Attach the stream source (may be switched while running, e.g. when the active device changes).
    void SetSource(VideoEncoder* encoder, const std::shared_ptr<AudioSource>& audio, const std::wstring& deviceName);
    void Detach() { SetSource(nullptr, nullptr, L""); }

    int Viewers() const { return m_viewers.load(); }
    // The sharing PIN (owned by the app, outlives the server). Set before Start.
    void SetPinGate(net::PinGate* gate) { m_gate = gate; }
    // Everyone has to say Hello again (the PIN changed): viewers with the old one are asked for the new one.
    void DropViewers();
    // Since the last call: fragments sent / resent after NACKs, frames viewers gave up on, keyframe requests.
    struct RateStats { uint32_t sent = 0, resent = 0, lost = 0, keyRequests = 0; };
    RateStats TakeRateStats();
    double Kbps() const { return m_kbps.load(); }
    uint16_t Port() const { return m_port; }
    std::wstring Error() const;

private:
    struct Client {
        sockaddr_in addr;
        int64_t lastSeenQpc;
        uint32_t lost = 0;
        int64_t lastWholeResend = 0;   // whole-frame resends are rate-limited (one small packet must not make us send megabits)
    };
    static constexpr size_t kMaxClients = 16;   // viewers at once: a flood of (possibly spoofed) Hellos can't grow this

    void ThreadMain();
    void OnPacket(const PacketPtr& p);
    void HandleStream(const uint8_t* data, int size, const sockaddr_in& from);
    void HandleDiscovery(const uint8_t* data, int size, const sockaddr_in& from);
    void SendConfig(const sockaddr_in& to);
    void SendAudio();
    void SendFragment(const sockaddr_in& to, uint32_t frameId, const PacketPtr& p, uint16_t index);   // m_mutex held
    void Broadcast(const void* data, size_t size);

    uint16_t m_port = net::kStreamPort;
    net::PinGate* m_gate = nullptr;
    net::UdpSocket m_stream, m_discovery;
    std::thread m_thread;
    HANDLE m_stop = nullptr;
    HANDLE m_audioEvent = nullptr;
    std::atomic<bool> m_running{false};

    mutable std::mutex m_mutex;   // source + clients
    VideoEncoder* m_encoder = nullptr;
    int m_sinkId = 0;
    std::shared_ptr<AudioSource> m_audioSource;
    std::shared_ptr<AudioTap> m_tap;
    std::wstring m_deviceName;
    std::vector<Client> m_clients;
    uint32_t m_frameId = 0;
    uint32_t m_audioSeq = 0;
    int64_t m_lastKeyRequest = 0;
    std::vector<uint8_t> m_sendBuf;
    std::deque<std::pair<uint32_t, PacketPtr>> m_history;   // recent frames, for resending lost fragments
    std::vector<int16_t> m_pcm;

    std::atomic<int> m_viewers{0};
    std::atomic<double> m_kbps{0};
    std::atomic<uint64_t> m_bytes{0};
    std::atomic<uint32_t> m_rateSent{0}, m_rateResent{0}, m_rateLost{0}, m_rateKeyReq{0};
    // health log (encoder thread)
    int64_t m_healthStart = 0;
    uint32_t m_healthCalls = 0, m_healthSent = 0, m_healthSendFail = 0, m_healthResent = 0;
    int64_t m_statStart = 0;
    uint64_t m_statBytes = 0;
    mutable std::mutex m_errorMutex;
    std::wstring m_error;
};

} // namespace cc
