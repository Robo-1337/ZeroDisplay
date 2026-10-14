#pragma once
#include "audio/AudioSource.h"
#include "encode/AacEncoder.h"
#include "encode/VideoEncoder.h"
#include "net/Net.h"
#include "net/PinGate.h"

#include <deque>
#include <thread>

namespace cc {

// Browser viewer: serves an embedded page over HTTP and streams to it over WebSocket.
// - Plain http (LAN): fragmented MP4 for Media Source Extensions (H.264 passthrough + AAC).
// - Secure contexts (localhost/https): raw H.264 for WebCodecs + PCM for an AudioWorklet (lowest latency).
class WebServer {
public:
    ~WebServer();

    bool Start(uint16_t port = 8080);
    void Stop();
    bool Running() const { return m_running.load(); }

    void SetSource(VideoEncoder* encoder, const std::shared_ptr<AudioSource>& audio, const std::wstring& deviceName);
    void Detach() { SetSource(nullptr, nullptr, L""); }

    int Viewers() const { return m_viewers.load(); }
    // The sharing PIN (owned by the app, outlives the server). Set before Start.
    void SetPinGate(net::PinGate* gate) { m_gate = gate; }
    // Viewers with the old PIN are disconnected (the page then asks for the new one).
    void DropViewers();
    uint16_t Port() const { return m_port; }
    std::wstring Error() const;

private:
    enum class Mode { Http, Unknown, Mse, WebCodecs };
    using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
public:
    uint32_t TakeSkips() { return m_rateSkips.exchange(0); }   // viewers that fell behind since the last call

private:
    std::atomic<uint32_t> m_rateSkips{0};
    struct Client {
        SOCKET sock = INVALID_SOCKET;
        HANDLE event = nullptr;
        std::wstring addr;
        uint32_t ip = 0;           // network byte order (for the PIN's wrong-guess limit)
        bool authorized = false;   // gave the sharing PIN (or none is needed)
        std::string in;
        Mode mode = Mode::Http;
        std::deque<Bytes> out;
        std::deque<int64_t> outQpc;   // when each queued message was queued (same order as `out`)
        size_t outOffset = 0;
        size_t queued = 0;
        bool closeAfterSend = false;
        bool dead = false;
        // stream state
        bool started = false;      // init sent and first keyframe delivered
        int64_t base = -1;         // 100 ns time of the first keyframe sent
        uint32_t videoSeq = 0, audioSeq = 0;
        bool audioInit = false;
        bool configSent = false;
        int64_t opened = 0;        // a request must arrive within 10 s (idle connections can't fill the 60 slots)
    };

    void ThreadMain();
    void Accept();
    void ReadFrom(Client& c);
    void HandleHttp(Client& c);
    void HandleWsFrames(Client& c);
    void OnText(Client& c, const std::string& text);
    void Flush(Client& c);
    void Enqueue(Client& c, Bytes msg, bool video);
    void SendRaw(Client& c, const std::string& s);
    void SendConfig(Client& c);
    void OnPacket(const PacketPtr& p);
    void ProcessAudio();
    static Bytes WsFrame(uint8_t opcode, const uint8_t* data, size_t size, uint8_t prefix = 0, bool hasPrefix = false);

    uint16_t m_port = 8080;
    net::PinGate* m_gate = nullptr;
    SOCKET m_listen = INVALID_SOCKET;
    HANDLE m_listenEvent = nullptr;
    HANDLE m_stop = nullptr;
    HANDLE m_wake = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_running{false};

    std::mutex m_mutex;   // clients + source + stream state
    std::vector<std::unique_ptr<Client>> m_clients;
    VideoEncoder* m_encoder = nullptr;
    int m_sinkId = 0;
    std::shared_ptr<AudioSource> m_audioSource;
    std::shared_ptr<AudioTap> m_tap;
    std::wstring m_deviceName;
    std::vector<uint8_t> m_sps, m_pps;
    std::string m_codec;
    uint32_t m_width = 0, m_height = 0;
    Bytes m_videoInit;

    // audio (network thread)
    AacEncoder m_aac;
    uint32_t m_audioRate = 0;
    uint64_t m_aacFed = 0, m_aacOut = 0;
    uint64_t m_anchorSample = 0;
    int64_t m_anchorTime = 0;
    Bytes m_audioInit;

    std::atomic<int> m_viewers{0};
    mutable std::mutex m_errorMutex;
    std::wstring m_error;
};

} // namespace cc
