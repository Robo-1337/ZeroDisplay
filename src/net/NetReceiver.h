#pragma once
#include "audio/AudioSource.h"
#include "devices/VideoSource.h"
#include "net/Net.h"

#include <mftransform.h>

#include <map>
#include <thread>

namespace cc {

struct Pending {
    std::vector<uint8_t> data;
    std::vector<bool> got;
    uint16_t left = 0;
    bool known = false;     // a fragment arrived, so its size is known (else: noticed missing entirely)
    bool key = false;
    bool repaired = false;  // needed a resend
    int64_t time = 0;       // sender clock
    int64_t firstQpc = 0;   // first fragment arrived / first noticed missing
    int64_t lastNack = 0;
    int nacks = 0;
    bool Complete() const { return known && left == 0; }
};

// Receives a ZeroDisplay LAN stream (see net/Net.h) and behaves like a capture device: frames are
// decoded on the GPU (H.264 decoder MFT with our D3D device, low-latency mode) and audio is pushed
// into the session's AudioSource. The device "symlink" is "net:<ip>:<port>".
class NetReceiver : public VideoSource {
public:
    NetReceiver(IMFDXGIDeviceManager* dxgi, HANDLE frameEvent, std::shared_ptr<AudioSource> audio);
    ~NetReceiver() override;

    void Open(const VideoDeviceInfo& info, const std::wstring& preferredKey, bool preferUncompressed) override;
    void SetFormat(const VideoFormat&) override {}
    void Reopen() override;
    void Close() override;
    bool TakeFrame(CapturedFrame& out) override;

    CaptureState State() const override;
    std::wstring StatusText() const override;
    std::vector<VideoFormat> Formats() const override;
    VideoFormat Current() const override;
    CaptureStats Stats() const override;
    int64_t LastFrameQpc() const override { return m_lastFrameQpc.load(); }
    bool Busy() const override { return false; }
    bool IsNetwork() const override { return true; }

    static bool IsNetworkLink(const std::wstring& symlink) { return symlink.rfind(L"net:", 0) == 0; }
    // The sharing PIN to send (empty: none known). A new one makes a refused receiver try again. `typed`: just typed
    // by the user (a refusal then says "wrong"), else remembered (a refusal says the PC has a new PIN).
    void SetPin(const std::string& pin, bool typed = false);
    std::string Pin() const;
    static double s_testLoss;   // testing: drop this fraction of incoming video packets (simulated Wi-Fi loss)
    // Malformed / random packets (truncated, huge sizes, bad indexes, unterminated names): no crash, bounded memory.
    static bool SelfTest();

private:
    void ThreadMain(sockaddr_in server);
    void HandlePacket(const uint8_t* data, int size, const sockaddr_in& server);
    void OnConfig(const net::ConfigMsg& m);
    void OnVideo(const net::VideoMsg& h, const uint8_t* payload, size_t n, const sockaddr_in& server);
    void OnAudio(const net::AudioMsg& h, const uint8_t* payload, size_t n);
    void RequestKeyframe(const sockaddr_in& server);
    void Deliver();                                    // decode complete frames in order
    void Repair(int64_t now, const sockaddr_in& server);   // NACK missing fragments, give up on hopeless frames
    void SendNack(uint32_t id, const struct Pending& p, const sockaddr_in& server);
    void ResetFrames();
    bool InitDecoder();
    void Decode(const std::vector<uint8_t>& au, int64_t time, int64_t arrival);
    void SetState(CaptureState s, std::wstring text);

    ComPtr<IMFDXGIDeviceManager> m_dxgi;
    HANDLE m_frameEvent = nullptr;
    std::shared_ptr<AudioSource> m_audio;
    VideoDeviceInfo m_info;
    std::thread m_thread;
    HANDLE m_stop = nullptr;
    net::UdpSocket m_sock;

    // receive thread state
    net::ConfigMsg m_config{};
    bool m_haveConfig = false;
    ComPtr<IMFTransform> m_decoder;
    bool m_decoderProvidesSamples = false;
    // Frames being reassembled. Several can be in flight while a lost fragment is being resent; they are
    // decoded strictly in order.
    std::map<uint32_t, struct Pending> m_pending;
    uint32_t m_nextId = 0;      // next frame to decode (0 = nothing received yet)
    uint32_t m_highestId = 0;   // newest frame seen
    sockaddr_in m_server{};
    bool m_needKeyframe = true;
    int64_t m_lastKeyRequest = 0;
    int64_t m_lastPacket = 0;
    // Sender clock -> ours: the smallest (arrival - send time) of the last 5-10 s is the fastest path through the
    // network. Times mapped with it keep the sender's spacing, so recordings line up sound and picture by when they
    // were captured (a phone's sound reaches us ~80 ms after its picture).
    int64_t m_clockMin = INT64_MAX, m_clockPrevMin = INT64_MAX, m_clockBlock = 0, m_lastVideoTime = 0;
    void NoteClock(int64_t senderTime, int64_t arrival100ns);
    int64_t MapTime(int64_t senderTime) const;   // 0 while unknown (the caller then uses the arrival time)
    uint32_t m_framesReceived = 0, m_framesLost = 0, m_decodeRejected = 0;
    // health log (receiver thread)
    int64_t m_healthStart = 0;
    uint32_t m_healthIn = 0, m_healthOut = 0, m_healthLost = 0, m_healthComplete = 0, m_healthWaitKey = 0, m_healthNacks = 0, m_healthRepaired = 0;
    double m_healthDecodeMs = 0, m_healthDecodeMax = 0;
    std::vector<float> m_pcm;

    mutable std::mutex m_mutex;
    CaptureState m_state = CaptureState::Closed;
    std::wstring m_status;
    std::string m_pin;              // guarded by m_mutex
    bool m_pinTyped = false;        // guarded by m_mutex
    uint32_t m_pinGen = 0;          // bumped by SetPin
    uint32_t m_deniedGen = UINT32_MAX;   // refused with this PIN generation, so no more Hellos with it (m_mutex)
    bool m_deniedWait = false;           // receive thread: refused for too many tries (that ends by itself)
    CapturedFrame m_latest;
    uint64_t m_seq = 0;
    CaptureStats m_stats;
    int64_t m_fpsStart = 0;
    uint64_t m_fpsFrames = 0;
    std::atomic<int64_t> m_lastFrameQpc{0};
};

} // namespace cc
