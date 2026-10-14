#include "net/StreamServer.h"
#include "core/Log.h"

#include <algorithm>
#include <cmath>

namespace cc {

using namespace net;

StreamServer::~StreamServer() { Stop(); }

std::wstring StreamServer::Error() const
{
    std::lock_guard lock(m_errorMutex);
    return m_error;
}

bool StreamServer::Start(uint16_t port)
{
    Stop();
    m_port = port;
    if (!m_stream.Open(port)) {
        std::lock_guard lock(m_errorMutex);
        m_error = Format(L"Port %u is already in use", port);
        LOGE(L"Stream server: cannot open UDP port %u", port);
        return false;
    }
    // Several instances may listen for discovery on the same machine.
    if (!m_discovery.Open(kDiscoveryPort, true, true, 64 * 1024)) LOGW(L"Stream server: discovery port %u unavailable", kDiscoveryPort);
    m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_running = true;
    m_statStart = QpcNow();
    m_thread = std::thread([this] { ThreadMain(); });
    LOGI(L"Stream server listening on UDP %u", port);
    return true;
}

void StreamServer::Stop()
{
    if (!m_thread.joinable()) return;
    Detach();
    {
        // Tell viewers we're gone so they don't wait for a timeout.
        Header bye{kMagic, Msg::Bye};
        Broadcast(&bye, sizeof(bye));
    }
    SetEvent(m_stop);
    m_thread.join();
    CloseHandle(m_stop);
    CloseHandle(m_audioEvent);
    m_stop = m_audioEvent = nullptr;
    m_stream.Close();
    m_discovery.Close();
    {
        std::lock_guard lock(m_mutex);
        m_clients.clear();
    }
    m_viewers = 0;
    m_running = false;
    LOGI(L"Stream server stopped");
}

void StreamServer::SetSource(VideoEncoder* encoder, const std::shared_ptr<AudioSource>& audio, const std::wstring& deviceName)
{
    VideoEncoder* oldEncoder = nullptr;
    int oldSink = 0;
    std::shared_ptr<AudioSource> oldAudio;
    std::shared_ptr<AudioTap> oldTap;
    {
        std::lock_guard lock(m_mutex);
        oldEncoder = m_encoder;
        oldSink = m_sinkId;
        oldAudio = m_audioSource;
        oldTap = m_tap;
        m_encoder = nullptr;
        m_audioSource.reset();
        m_tap.reset();
    }
    if (oldEncoder) oldEncoder->RemoveSink(oldSink);
    if (oldAudio && oldTap) oldAudio->RemoveTap(oldTap.get());

    std::shared_ptr<AudioTap> tap;
    if (audio) {
        tap = std::make_shared<AudioTap>();
        tap->SetNotify(m_audioEvent);
        audio->AddTap(tap);
    }
    {
        std::lock_guard lock(m_mutex);
        m_encoder = encoder;
        m_audioSource = audio;
        m_tap = tap;
        m_deviceName = deviceName;
    }
    if (encoder) {
        m_sinkId = encoder->AddSink([this](const PacketPtr& p) { OnPacket(p); });
        encoder->RequestKeyframe();
        // New source: every viewer needs the new configuration.
        std::vector<sockaddr_in> addrs;
        {
            std::lock_guard lock(m_mutex);
            for (auto& c : m_clients) addrs.push_back(c.addr);
        }
        for (auto& a : addrs) SendConfig(a);
    }
}

void StreamServer::Broadcast(const void* data, size_t size)
{
    std::lock_guard lock(m_mutex);
    for (auto& c : m_clients) m_stream.SendTo(c.addr, data, size);
}

void StreamServer::SendConfig(const sockaddr_in& to)
{
    ConfigMsg m{};
    m.h = {kMagic, Msg::Config};
    {
        std::lock_guard lock(m_mutex);
        if (!m_encoder) return;
        const auto& c = m_encoder->Config();
        m.width = c.width;
        m.height = c.height;
        m.fpsNum = c.fpsNum;
        m.fpsDen = c.fpsDen;
        m.audioRate = m_audioSource ? m_audioSource->TapRate() : 0;
        CopyName(m.device, m_deviceName);
    }
    m.instance = InstanceId();
    CopyName(m.server, ComputerName());
    m_stream.SendTo(to, &m, sizeof(m));
}

void StreamServer::OnPacket(const PacketPtr& p)
{
    // Encoder thread: fragment the access unit and send it to every viewer right away.
    std::lock_guard lock(m_mutex);
    const int64_t now = QpcNow();
    if (!m_healthStart) m_healthStart = now;
    ++m_healthCalls;
    if (QpcToMs(now - m_healthStart) >= 5000) {
        const double sec = QpcToMs(now - m_healthStart) / 1000.0;
        if (!m_clients.empty())
            LOGI(L"Stream: %.1f fps from the encoder, %.1f fps sent, %u fragments resent, %u failed sends", m_healthCalls / sec,
                 m_healthSent / sec, m_healthResent, m_healthSendFail);
        m_healthStart = now;
        m_healthCalls = m_healthSent = m_healthSendFail = m_healthResent = 0;
    }
    if (m_clients.empty()) return;
    ++m_healthSent;
    const uint32_t id = ++m_frameId;
    const size_t total = p->data.size();
    const uint16_t count = uint16_t((total + kMaxPayload - 1) / kMaxPayload);
    for (uint16_t i = 0; i < count; ++i)
        for (auto& c : m_clients) SendFragment(c.addr, id, p, i);
    m_rateSent += uint32_t(count * m_clients.size());
    m_bytes += total * m_clients.size();
    // Keep ~2 s for resends (the packets are shared with the recorder / replay buffer anyway).
    m_history.emplace_back(id, p);
    while (m_history.size() > 120) m_history.pop_front();
}

StreamServer::RateStats StreamServer::TakeRateStats()
{
    RateStats r;
    r.sent = m_rateSent.exchange(0);
    r.resent = m_rateResent.exchange(0);
    r.lost = m_rateLost.exchange(0);
    r.keyRequests = m_rateKeyReq.exchange(0);
    return r;
}

void StreamServer::SendFragment(const sockaddr_in& to, uint32_t frameId, const PacketPtr& p, uint16_t index)
{
    const size_t total = p->data.size();
    const uint16_t count = uint16_t((total + kMaxPayload - 1) / kMaxPayload);
    if (index >= count) return;
    const size_t off = size_t(index) * kMaxPayload;
    const size_t n = std::min(kMaxPayload, total - off);
    m_sendBuf.resize(sizeof(VideoMsg) + kMaxPayload);
    VideoMsg h{};
    h.h = {kMagic, Msg::Video, uint8_t(p->keyframe ? 1 : 0), 0};
    h.frameId = frameId;
    h.fragIndex = index;
    h.fragCount = count;
    h.totalSize = uint32_t(total);
    h.time = p->time;
    memcpy(m_sendBuf.data(), &h, sizeof(h));
    memcpy(m_sendBuf.data() + sizeof(h), p->data.data() + off, n);
    if (!m_stream.SendTo(to, m_sendBuf.data(), sizeof(h) + n)) ++m_healthSendFail;
}

void StreamServer::SendAudio()
{
    std::shared_ptr<AudioTap> tap;
    {
        std::lock_guard lock(m_mutex);
        tap = m_tap;
    }
    if (!tap) return;
    AudioChunk c;
    while (tap->Pop(c)) {
        std::lock_guard lock(m_mutex);
        if (m_clients.empty()) continue;
        const uint32_t frames = uint32_t(c.stereo.size() / 2);
        for (uint32_t off = 0; off < frames; off += kAudioChunkFrames) {
            const uint32_t n = std::min(kAudioChunkFrames, frames - off);
            m_sendBuf.resize(sizeof(AudioMsg) + size_t(n) * 4);
            AudioMsg h{};
            h.h = {kMagic, Msg::Audio};
            h.seq = ++m_audioSeq;
            h.time = c.time + int64_t(off) * 10000000 / std::max(1u, c.rate);
            h.frames = uint16_t(n);
            memcpy(m_sendBuf.data(), &h, sizeof(h));
            int16_t* pcm = reinterpret_cast<int16_t*>(m_sendBuf.data() + sizeof(h));
            for (uint32_t i = 0; i < n * 2; ++i) pcm[i] = int16_t(std::lround(std::clamp(c.stereo[size_t(off) * 2 + i], -1.0f, 1.0f) * 32767.0f));
            for (auto& cl : m_clients) m_stream.SendTo(cl.addr, m_sendBuf.data(), m_sendBuf.size());
            m_bytes += m_sendBuf.size() * m_clients.size();
        }
    }
}

void StreamServer::HandleStream(const uint8_t* data, int size, const sockaddr_in& from)
{
    if (size < int(sizeof(Header))) return;
    const Header* h = reinterpret_cast<const Header*>(data);
    if (h->magic != kMagic) return;
    const int64_t now = QpcNow();
    auto findClient = [&]() -> Client* {
        for (auto& c : m_clients)
            if (c.addr.sin_addr.s_addr == from.sin_addr.s_addr && c.addr.sin_port == from.sin_port) return &c;
        return nullptr;
    };
    switch (h->type) {
    case Msg::Hello: {
        bool isNew = false;
        {
            std::lock_guard lock(m_mutex);
            if (Client* c = findClient()) {
                c->lastSeenQpc = now;
            } else {
                if (m_gate && m_gate->Required()) {   // a new viewer: only with the sharing PIN
                    std::string_view pin;
                    if (size >= int(sizeof(HelloMsg))) {
                        const char* p = reinterpret_cast<const HelloMsg*>(data)->pin;
                        pin = std::string_view(p, strnlen(p, sizeof(HelloMsg::pin)));
                    }
                    const net::PinGate::Result r = m_gate->Check(from.sin_addr.s_addr, pin);
                    if (r != net::PinGate::Result::Ok) {
                        DeniedMsg d;
                        d.h.flags = uint8_t(r == net::PinGate::Result::Needed ? DenyReason::PinNeeded
                                            : r == net::PinGate::Result::Wrong ? DenyReason::WrongPin
                                                                                : DenyReason::Wait);
                        m_stream.SendTo(from, &d, sizeof(d));
                        break;
                    }
                }
                if (m_clients.size() >= kMaxClients) break;   // full: no config, no stream for this one
                m_clients.push_back({from, now});
                isNew = true;
            }
            m_viewers = int(m_clients.size());
        }
        SendConfig(from);
        if (isNew) {
            LOGI(L"Stream: viewer connected from %s", AddrToString(from, true).c_str());
            std::lock_guard lock(m_mutex);
            if (m_encoder) m_encoder->RequestKeyframe();
        }
        break;
    }
    case Msg::Keepalive: {
        std::lock_guard lock(m_mutex);
        if (Client* c = findClient()) {
            c->lastSeenQpc = now;
            if (size >= int(sizeof(KeepaliveMsg))) {
                const uint32_t lost = reinterpret_cast<const KeepaliveMsg*>(data)->framesLost;
                if (lost > c->lost) m_rateLost += lost - c->lost;
                c->lost = lost;
            }
        }
        break;
    }
    case Msg::KeyRequest: {
        std::lock_guard lock(m_mutex);
        if (findClient()) ++m_rateKeyReq;
        if (findClient() && m_encoder && QpcToMs(now - m_lastKeyRequest) > 150) {
            m_lastKeyRequest = now;
            m_encoder->RequestKeyframe();
        }
        break;
    }
    case Msg::Nack: {
        // A viewer lost some fragments (Wi-Fi): resend just those, if the frame is still in the history.
        if (size < int(offsetof(NackMsg, frags))) break;
        const NackMsg* n = reinterpret_cast<const NackMsg*>(data);
        std::lock_guard lock(m_mutex);
        Client* c = findClient();
        if (!c) break;
        for (const auto& [id, p] : m_history) {
            if (id != n->frameId) continue;
            if (n->count == kNackWholeFrame) {
                if (c->lastWholeResend && QpcToMs(now - c->lastWholeResend) < 100.0) break;
                c->lastWholeResend = now;
                const uint16_t count = uint16_t((p->data.size() + kMaxPayload - 1) / kMaxPayload);
                for (uint16_t i = 0; i < count; ++i) SendFragment(c->addr, id, p, i);
                m_healthResent += count;
                m_rateResent += count;
            } else {
                const uint16_t k = uint16_t(std::min<size_t>(n->count, kMaxNackFrags));
                if (size < int(offsetof(NackMsg, frags) + k * sizeof(uint16_t))) break;
                for (uint16_t i = 0; i < k; ++i) SendFragment(c->addr, id, p, n->frags[i]);
                m_healthResent += k;
                m_rateResent += k;
            }
            break;
        }
        break;
    }
    case Msg::Bye: {
        std::lock_guard lock(m_mutex);
        const size_t gone =
            std::erase_if(m_clients, [&](const Client& c) { return c.addr.sin_addr.s_addr == from.sin_addr.s_addr && c.addr.sin_port == from.sin_port; });
        m_viewers = int(m_clients.size());
        if (gone) LOGI(L"Stream: viewer %s left", AddrToString(from, true).c_str());   // (not one that was never let in)
        break;
    }
    default: break;
    }
}

void StreamServer::DropViewers()
{
    std::lock_guard lock(m_mutex);
    if (!m_clients.empty()) LOGI(L"Stream: the PIN changed, %zu viewer(s) must enter it again", m_clients.size());
    m_clients.clear();
    m_viewers = 0;
}

void StreamServer::HandleDiscovery(const uint8_t* data, int size, const sockaddr_in& from)
{
    if (size < int(sizeof(Header))) return;
    const Header* h = reinterpret_cast<const Header*>(data);
    if (h->magic != kMagic || h->type != Msg::Discover) return;
    AnnounceMsg a{};
    a.h = {kMagic, Msg::Announce};
    a.instance = InstanceId();
    a.port = m_port;
    a.viewers = uint16_t(m_viewers.load());
    {
        std::lock_guard lock(m_mutex);
        if (!m_encoder) return;   // not actually sharing anything right now
        const auto& c = m_encoder->Config();
        a.width = c.width;
        a.height = c.height;
        a.fpsNum = c.fpsNum;
        a.fpsDen = c.fpsDen;
        CopyName(a.device, m_deviceName);
    }
    CopyName(a.server, ComputerName());
    m_discovery.SendTo(from, &a, sizeof(a));
}

void StreamServer::ThreadMain()
{
    SetCurrentThreadName(L"Stream server");
    MmcssScope mmcss(L"Pro Audio", 1);
    std::vector<uint8_t> buf(64 * 1024);
    int64_t lastConfig = QpcNow();
    for (;;) {
        HANDLE handles[4] = {m_stop, m_audioEvent, m_stream.Event(), m_discovery.Valid() ? m_discovery.Event() : m_stop};
        DWORD w = WaitForMultipleObjects(4, handles, FALSE, 250);
        if (w == WAIT_OBJECT_0) break;
        SendAudio();
        sockaddr_in from{};
        WSAResetEvent(m_stream.Event());
        int n;
        while ((n = m_stream.RecvFrom(buf.data(), buf.size(), from)) > 0) HandleStream(buf.data(), n, from);
        if (m_discovery.Valid()) {
            WSAResetEvent(m_discovery.Event());
            while ((n = m_discovery.RecvFrom(buf.data(), buf.size(), from)) > 0) HandleDiscovery(buf.data(), n, from);
        }

        const int64_t now = QpcNow();
        {
            std::lock_guard lock(m_mutex);
            size_t before = m_clients.size();
            std::erase_if(m_clients, [&](const Client& c) { return QpcToMs(now - c.lastSeenQpc) > 4000; });
            if (m_clients.size() != before) LOGI(L"Stream: %zu viewer(s) timed out", before - m_clients.size());
            m_viewers = int(m_clients.size());
        }
        // Repeat the configuration now and then (it's tiny) so a viewer recovers from any loss.
        if (QpcToMs(now - lastConfig) > 1000) {
            lastConfig = now;
            std::vector<sockaddr_in> addrs;
            {
                std::lock_guard lock(m_mutex);
                for (auto& c : m_clients) addrs.push_back(c.addr);
            }
            for (auto& a : addrs) SendConfig(a);
        }
        double el = QpcToMs(now - m_statStart);
        if (el >= 1000) {
            uint64_t b = m_bytes.load();
            m_kbps = (b - m_statBytes) * 8.0 / el;
            m_statBytes = b;
            m_statStart = now;
        }
    }
}

} // namespace cc
