#include "net/WebServer.h"
#include "core/Log.h"
#include "encode/H264.h"
#include "encode/Mp4Fragmenter.h"
#include "net/WebPage.h"

#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <cmath>

namespace cc {

static constexpr size_t kMaxQueued = 4 << 20;   // per viewer, absolute cap

WebServer::~WebServer() { Stop(); }

std::wstring WebServer::Error() const
{
    std::lock_guard lock(m_errorMutex);
    return m_error;
}

bool WebServer::Start(uint16_t port)
{
    Stop();
    m_port = port;
    m_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL on = TRUE;
    setsockopt(m_listen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (m_listen == INVALID_SOCKET || bind(m_listen, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(m_listen, SOMAXCONN) != 0) {
        if (m_listen != INVALID_SOCKET) closesocket(m_listen);
        m_listen = INVALID_SOCKET;
        std::lock_guard lock(m_errorMutex);
        m_error = Format(L"TCP port %u is already in use", port);
        LOGE(L"Web viewer: cannot listen on TCP %u", port);
        return false;
    }
    m_listenEvent = WSACreateEvent();
    WSAEventSelect(m_listen, m_listenEvent, FD_ACCEPT);
    m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_running = true;
    m_thread = std::thread([this] { ThreadMain(); });
    LOGI(L"Web viewer on http://*:%u/", port);
    return true;
}

void WebServer::Stop()
{
    if (!m_thread.joinable()) return;
    Detach();
    SetEvent(m_stop);
    m_thread.join();
    {
        std::lock_guard lock(m_mutex);
        for (auto& c : m_clients) {
            closesocket(c->sock);
            WSACloseEvent(c->event);
        }
        m_clients.clear();
    }
    closesocket(m_listen);
    m_listen = INVALID_SOCKET;
    WSACloseEvent(m_listenEvent);
    CloseHandle(m_stop);
    CloseHandle(m_wake);
    m_listenEvent = m_stop = m_wake = nullptr;
    m_aac.Stop();
    m_viewers = 0;
    m_running = false;
    LOGI(L"Web viewer stopped");
}

void WebServer::SetSource(VideoEncoder* encoder, const std::shared_ptr<AudioSource>& audio, const std::wstring& deviceName)
{
    VideoEncoder* oldEncoder;
    int oldSink;
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
        tap->SetNotify(m_wake);
        audio->AddTap(tap);
    }
    {
        std::lock_guard lock(m_mutex);
        m_encoder = encoder;
        m_audioSource = audio;
        m_tap = tap;
        m_deviceName = deviceName;
        for (auto& c : m_clients) {
            c->started = false;
            c->configSent = false;
        }
    }
    if (encoder) {
        m_sinkId = encoder->AddSink([this](const PacketPtr& p) { OnPacket(p); });
        encoder->RequestKeyframe();
    }
}

// ---------------------------------------------------------------------------------------------

WebServer::Bytes WebServer::WsFrame(uint8_t opcode, const uint8_t* data, size_t size, uint8_t prefix, bool hasPrefix)
{
    const size_t payload = size + (hasPrefix ? 1 : 0);
    auto f = std::make_shared<std::vector<uint8_t>>();
    f->reserve(payload + 10);
    f->push_back(uint8_t(0x80 | opcode));
    if (payload < 126) {
        f->push_back(uint8_t(payload));
    } else if (payload < 65536) {
        f->push_back(126);
        f->push_back(uint8_t(payload >> 8));
        f->push_back(uint8_t(payload));
    } else {
        f->push_back(127);
        for (int i = 7; i >= 0; --i) f->push_back(uint8_t(uint64_t(payload) >> (8 * i)));
    }
    if (hasPrefix) f->push_back(prefix);
    f->insert(f->end(), data, data + size);
    return f;
}

void WebServer::SendRaw(Client& c, const std::string& s)
{
    auto b = std::make_shared<std::vector<uint8_t>>(s.begin(), s.end());
    c.queued += b->size();
    c.out.push_back(std::move(b));
    c.outQpc.push_back(QpcNow());
}

void WebServer::Enqueue(Client& c, Bytes msg, bool video)
{
    // Latency first: if what is queued for this viewer has been waiting longer than ~250 ms, its connection
    // can't keep up (whatever its speed), so skip video to the next keyframe instead of letting the delay grow.
    const double waitedMs = c.outQpc.empty() ? 0.0 : QpcElapsedMs(c.outQpc.front());
    if (!video && (waitedMs > 600.0 || c.queued > kMaxQueued)) return;   // hopelessly behind: audio too
    if (video && (waitedMs > 250.0 || c.queued > kMaxQueued)) {
        // The viewer can't keep up (slow Wi-Fi): skip video until the next keyframe.
        if (c.started) ++m_rateSkips;
        c.started = false;
        if (m_encoder) m_encoder->RequestKeyframe();
        return;
    }
    c.queued += msg->size();
    c.out.push_back(std::move(msg));
    c.outQpc.push_back(QpcNow());
}

void WebServer::SendConfig(Client& c)
{
    if (m_codec.empty()) return;
    std::string name = Narrow(m_deviceName), esc;
    for (char ch : name) {
        if (ch == '"' || ch == '\\') esc += '\\';
        if (uint8_t(ch) >= 0x20) esc += ch;
    }
    char json[512];
    snprintf(json, sizeof(json), "{\"type\":\"config\",\"codec\":\"%s\",\"width\":%u,\"height\":%u,\"audioRate\":%u,\"name\":\"%s\"}", m_codec.c_str(),
             m_width, m_height, m_audioSource ? m_audioSource->TapRate() : 0, esc.c_str());
    Enqueue(c, WsFrame(1, reinterpret_cast<const uint8_t*>(json), strlen(json)), false);
    c.configSent = true;
}

void WebServer::OnPacket(const PacketPtr& p)
{
    // Encoder thread.
    std::lock_guard lock(m_mutex);
    if (p->keyframe) {
        std::vector<uint8_t> ps = h264::ExtractParameterSets(p->data.data(), p->data.size());
        std::vector<uint8_t> sps, pps;
        if (!ps.empty() && mp4::SplitParameterSets(ps, sps, pps) && (sps != m_sps || pps != m_pps)) {
            m_sps = sps;
            m_pps = pps;
            m_codec = h264::CodecString(ps);
            if (m_encoder) {
                m_width = m_encoder->Config().width;
                m_height = m_encoder->Config().height;
            }
            m_videoInit = std::make_shared<std::vector<uint8_t>>(mp4::VideoInit(m_width, m_height, m_sps, m_pps, 90000));
            for (auto& c : m_clients) {
                c->configSent = false;
                c->started = false;
            }
        }
    }
    bool any = false;
    Bytes wcFrame;
    std::vector<uint8_t> avcc;
    for (auto& cp : m_clients) {
        Client& c = *cp;
        if (c.mode != Mode::Mse && c.mode != Mode::WebCodecs) continue;
        if (!c.configSent) SendConfig(c);
        if (!c.configSent) continue;
        if (!c.started && !p->keyframe) continue;
        if (c.mode == Mode::WebCodecs) {
            if (!wcFrame) {
                std::vector<uint8_t> m(10 + p->data.size());
                m[0] = 5;
                m[1] = p->keyframe ? 1 : 0;
                const int64_t us = p->time / 10;
                memcpy(&m[2], &us, 8);
                memcpy(&m[10], p->data.data(), p->data.size());
                wcFrame = WsFrame(2, m.data(), m.size());
            }
            c.started = true;
            Enqueue(c, wcFrame, true);
        } else {
            if (!c.started) {
                if (!m_videoInit) continue;
                Enqueue(c, WsFrame(2, m_videoInit->data(), m_videoInit->size(), 1, true), false);
                if (c.base < 0) {
                    c.base = p->time;   // keep one timeline per viewer, even across restarts
                    // Media time 0 = this capture time (ms on the server clock): lets the page measure latency.
                    char json[96];
                    snprintf(json, sizeof(json), "{\"type\":\"base\",\"ms\":%.3f}", double(c.base) / 10000.0);
                    Enqueue(c, WsFrame(1, reinterpret_cast<const uint8_t*>(json), strlen(json)), false);
                }
                c.audioInit = false;
                c.started = true;
            }
            if (avcc.empty()) avcc = mp4::AnnexBToAvcc(p->data.data(), p->data.size());
            const uint64_t t = uint64_t(std::max<int64_t>(0, p->time - c.base)) * 9 / 1000;
            mp4::Sample s{avcc.data(), uint32_t(avcc.size()), uint32_t(std::max<int64_t>(1, p->duration * 9 / 1000)), p->keyframe};
            std::vector<uint8_t> frag = mp4::Fragment(++c.videoSeq, t, {s});
            Enqueue(c, WsFrame(2, frag.data(), frag.size(), 2, true), true);
        }
        any = true;
    }
    if (any) SetEvent(m_wake);
}

void WebServer::ProcessAudio()
{
    std::shared_ptr<AudioTap> tap;
    {
        std::lock_guard lock(m_mutex);
        tap = m_tap;
    }
    if (!tap) return;
    AudioChunk chunk;
    while (tap->Pop(chunk)) {
        std::lock_guard lock(m_mutex);
        const uint32_t frames = uint32_t(chunk.stereo.size() / 2);
        bool wantAac = false;
        Bytes pcmMsg;
        for (auto& cp : m_clients) {
            Client& c = *cp;
            if (!c.started) continue;
            if (c.mode == Mode::Mse) wantAac = true;
            if (c.mode == Mode::WebCodecs) {
                if (!pcmMsg) {
                    std::vector<uint8_t> m(10 + size_t(frames) * 4);
                    m[0] = 6;
                    const int64_t us = chunk.time / 10;
                    memcpy(&m[2], &us, 8);
                    int16_t* pcm = reinterpret_cast<int16_t*>(&m[10]);
                    for (uint32_t i = 0; i < frames * 2; ++i) pcm[i] = int16_t(std::lround(std::clamp(chunk.stereo[i], -1.0f, 1.0f) * 32767.0f));
                    pcmMsg = WsFrame(2, m.data(), m.size());
                }
                Enqueue(c, pcmMsg, false);
            }
        }
        if (!wantAac || (chunk.rate != 48000 && chunk.rate != 44100)) continue;
        if (!m_aac.Running() || m_audioRate != chunk.rate) {
            if (!m_aac.Start(chunk.rate)) continue;
            m_audioRate = chunk.rate;
            m_aacFed = m_aacOut = 0;
            m_audioInit = std::make_shared<std::vector<uint8_t>>(mp4::AudioInit(chunk.rate, 2, mp4::AacConfig(chunk.rate, 2)));
            for (auto& c : m_clients) c->audioInit = false;
        }
        // AAC frames get capture-clock timestamps: anchor sample positions to this chunk's capture time.
        m_anchorSample = m_aacFed;
        m_anchorTime = chunk.time;
        m_aacFed += frames;
        m_aac.Encode(chunk.stereo.data(), frames, [&](const uint8_t* data, size_t size) {
            const uint64_t sampleIndex = m_aacOut * 1024;
            ++m_aacOut;
            const int64_t t = m_anchorTime + (int64_t(sampleIndex) - int64_t(m_anchorSample)) * 10000000 / m_audioRate;
            for (auto& cp : m_clients) {
                Client& c = *cp;
                if (c.mode != Mode::Mse || !c.started || c.base < 0 || t < c.base) continue;
                if (!c.audioInit) {
                    Enqueue(c, WsFrame(2, m_audioInit->data(), m_audioInit->size(), 3, true), false);
                    c.audioInit = true;
                }
                const uint64_t tfdt = uint64_t(t - c.base) * m_audioRate / 10000000;
                mp4::Sample s{data, uint32_t(size), 1024, true};
                std::vector<uint8_t> frag = mp4::Fragment(++c.audioSeq, tfdt, {s});
                Enqueue(c, WsFrame(2, frag.data(), frag.size(), 4, true), false);
            }
        });

    }
}

// ---------------------------------------------------------------------------------------------

static std::string WebSocketAccept(const std::string& key)
{
    std::string s = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    UCHAR hash[20];
    if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(s.data()), ULONG(s.size()), hash, sizeof(hash))))
        return {};
    char out[64];
    DWORD n = sizeof(out);
    if (!CryptBinaryToStringA(hash, sizeof(hash), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out, &n)) return {};
    return std::string(out, n);
}

void WebServer::Accept()
{
    for (;;) {
        sockaddr_in from{};
        int len = sizeof(from);
        SOCKET s = accept(m_listen, reinterpret_cast<sockaddr*>(&from), &len);
        if (s == INVALID_SOCKET) return;
        std::lock_guard lock(m_mutex);
        if (m_clients.size() >= 60) {
            // Full: make room by dropping the oldest connection that never finished a request (someone holding
            // connections open would otherwise lock real viewers out); viewers who are watching are never dropped.
            Client* oldest = nullptr;
            for (auto& c : m_clients)
                if (c->mode == Mode::Http && !c->closeAfterSend && !c->dead && (!oldest || c->opened < oldest->opened)) oldest = c.get();
            if (!oldest || m_clients.size() >= 120) {   // (dropped ones leave the list on the next pass)
                closesocket(s);
                continue;
            }
            oldest->dead = true;
        }
        BOOL on = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
        int sndbuf = 128 << 10;   // small: backlog must stay in our queue, where it can be skipped (latency first)
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));
        auto c = std::make_unique<Client>();
        c->sock = s;
        c->event = WSACreateEvent();
        c->opened = QpcNow();
        if (c->event == WSA_INVALID_EVENT) {
            closesocket(s);
            continue;
        }
        c->addr = net::AddrToString(from, true);
        c->ip = from.sin_addr.s_addr;
        WSAEventSelect(s, c->event, FD_READ | FD_WRITE | FD_CLOSE);
        m_clients.push_back(std::move(c));
    }
}

void WebServer::HandleHttp(Client& c)
{
    size_t end = c.in.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (c.in.size() > 16384) c.dead = true;
        return;
    }
    std::string head = c.in.substr(0, end);
    c.in.erase(0, end + 4);
    std::string lower = head;
    for (auto& ch : lower) ch = char(tolower(uint8_t(ch)));
    size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
    std::string path = sp1 != std::string::npos && sp2 != std::string::npos ? head.substr(sp1 + 1, sp2 - sp1 - 1) : "/";
    auto header = [&](const char* name) -> std::string {
        std::string key = std::string("\r\n") + name + ":";
        size_t at = lower.find(key);
        if (at == std::string::npos) return {};
        size_t vs = at + key.size(), ve = head.find("\r\n", vs);
        std::string v = head.substr(vs, ve == std::string::npos ? std::string::npos : ve - vs);
        while (!v.empty() && v.front() == ' ') v.erase(0, 1);
        while (!v.empty() && v.back() == ' ') v.pop_back();
        return v;
    };

    if (path.rfind("/ws", 0) == 0 && lower.find("websocket") != std::string::npos) {
        // Only our own page may open the stream: a browser always says which site a WebSocket comes from, and another
        // website (open in a browser on this network) must not be able to pull the picture in the background.
        const std::string origin = header("origin"), host = header("host");
        if (!origin.empty()) {
            std::string o = origin;
            for (auto& ch : o) ch = char(tolower(uint8_t(ch)));
            std::string h = host;
            for (auto& ch : h) ch = char(tolower(uint8_t(ch)));
            if (h.empty() || (o != "http://" + h && o != "https://" + h)) {
                std::string shown = origin.substr(0, 80);
                for (char& ch : shown)
                    if (ch < 32 || ch > 126) ch = '?';
                LOGW(L"Web viewer: refused a connection from another website (%s)", Widen(shown).c_str());
                SendRaw(c, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                c.closeAfterSend = true;
                return;
            }
        }
        std::string accept = WebSocketAccept(header("sec-websocket-key"));
        SendRaw(c, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n");
        c.mode = Mode::Unknown;
        LOGI(L"Web viewer: %s connected", c.addr.c_str());
        return;
    }
    if (path == "/" || path.rfind("/?", 0) == 0 || path == "/index.html") {
        const size_t len = sizeof(web::kViewerPage) - 1;
        SendRaw(c, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nContent-Length: " + std::to_string(len) +
                       "\r\nConnection: close\r\n\r\n" + std::string(web::kViewerPage, len));
    } else {
        SendRaw(c, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    }
    c.closeAfterSend = true;
}

void WebServer::DropViewers()
{
    std::lock_guard lock(m_mutex);
    for (auto& c : m_clients)
        if (c->authorized) c->dead = true;   // the page reconnects and is asked for the new PIN
    SetEvent(m_wake);
}

void WebServer::OnText(Client& c, const std::string& text)
{
    auto reply = [&](const char* json) {
        Enqueue(c, WsFrame(1, reinterpret_cast<const uint8_t*>(json), strlen(json)), false);
        SetEvent(m_wake);
    };
    if (!c.authorized && (!m_gate || !m_gate->Required())) c.authorized = true;
    if (text.rfind("pin:", 0) == 0 && text.size() < 40) {
        // The page sends the PIN it has (maybe none) first; nothing is streamed before the right one.
        const net::PinGate::Result r = c.authorized ? net::PinGate::Result::Ok : m_gate->Check(c.ip, std::string_view(text).substr(4));
        if (r == net::PinGate::Result::Ok) {
            if (!c.authorized) LOGI(L"Web viewer: %s gave the PIN", c.addr.c_str());
            c.authorized = true;
            reply("{\"type\":\"pin\",\"ok\":true}");
        } else {
            reply(r == net::PinGate::Result::Needed ? "{\"type\":\"pin\",\"need\":\"needed\"}"
                  : r == net::PinGate::Result::Wrong ? "{\"type\":\"pin\",\"need\":\"wrong\"}"
                                                      : "{\"type\":\"pin\",\"need\":\"wait\"}");
        }
        return;
    }
    if (!c.authorized) {   // an old page, or one that skipped the PIN: ask for it, send nothing else
        if (text == "mse" || text == "webcodecs") reply("{\"type\":\"pin\",\"need\":\"needed\"}");
        return;
    }
    if (text == "mse" || text == "webcodecs") {
        c.mode = text == "mse" ? Mode::Mse : Mode::WebCodecs;
        c.started = false;
        c.configSent = false;
        SendConfig(c);
        if (m_encoder) m_encoder->RequestKeyframe();
        LOGI(L"Web viewer: %s uses %s", c.addr.c_str(), c.mode == Mode::Mse ? L"MSE" : L"WebCodecs");
    } else if (text == "key") {
        if (c.mode == Mode::Mse) c.started = false;   // restart cleanly at the next keyframe (with a fresh init segment)
        if (m_encoder) m_encoder->RequestKeyframe();
    } else if (text.rfind("ping ", 0) == 0) {
        // Clock sync: the page compares its clock with ours (ms on the QPC timeline the frames are stamped with).
        char json[128];
        snprintf(json, sizeof(json), "{\"type\":\"pong\",\"c\":%.3f,\"s\":%.3f}", strtod(text.c_str() + 5, nullptr), double(QpcTo100ns(QpcNow())) / 10000.0);
        Enqueue(c, WsFrame(1, reinterpret_cast<const uint8_t*>(json), strlen(json)), false);
        SetEvent(m_wake);
    } else if (text.rfind("stats ", 0) == 0 && text.size() < 600) {
        std::string clean = text.substr(6);   // from the browser: printable ASCII only in the log (no fake lines)
        for (char& ch : clean)
            if (ch < 32 || ch > 126) ch = '?';
        LOGI(L"Web viewer %s stats: %s", c.addr.c_str(), Widen(clean).c_str());
    }
}

void WebServer::HandleWsFrames(Client& c)
{
    for (;;) {
        if (c.in.size() < 2) return;
        const uint8_t* d = reinterpret_cast<const uint8_t*>(c.in.data());
        const uint8_t opcode = d[0] & 0x0F;
        const bool masked = (d[1] & 0x80) != 0;
        uint64_t len = d[1] & 0x7F;
        size_t pos = 2;
        if (len == 126) {
            if (c.in.size() < 4) return;
            len = (uint64_t(d[2]) << 8) | d[3];
            pos = 4;
        } else if (len == 127) {
            if (c.in.size() < 10) return;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | d[2 + i];
            pos = 10;
        }
        if (len > (1u << 20)) {
            c.dead = true;
            return;
        }
        uint8_t mask[4] = {};
        if (masked) {
            if (c.in.size() < pos + 4) return;
            memcpy(mask, d + pos, 4);
            pos += 4;
        }
        if (c.in.size() < pos + len) return;
        std::string payload(c.in.data() + pos, size_t(len));
        if (masked)
            for (size_t i = 0; i < payload.size(); ++i) payload[i] = char(payload[i] ^ mask[i & 3]);
        c.in.erase(0, pos + size_t(len));
        if (opcode == 1) OnText(c, payload);
        else if (opcode == 8) {
            Enqueue(c, WsFrame(8, nullptr, 0), false);
            c.closeAfterSend = true;
        } else if (opcode == 9) {
            Enqueue(c, WsFrame(10, reinterpret_cast<const uint8_t*>(payload.data()), payload.size()), false);
        }
    }
}

void WebServer::ReadFrom(Client& c)
{
    char buf[8192];
    for (;;) {
        int n = recv(c.sock, buf, sizeof(buf), 0);
        if (n > 0) {
            c.in.append(buf, size_t(n));
            continue;
        }
        if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) c.dead = true;
        break;
    }
    if (c.mode == Mode::Http) HandleHttp(c);
    if (c.mode != Mode::Http) HandleWsFrames(c);
}

void WebServer::Flush(Client& c)
{
    while (!c.out.empty() && !c.dead) {
        const auto& b = *c.out.front();
        int n = send(c.sock, reinterpret_cast<const char*>(b.data()) + c.outOffset, int(b.size() - c.outOffset), 0);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) c.dead = true;
            return;
        }
        c.outOffset += size_t(n);
        if (c.outOffset == b.size()) {
            c.queued -= b.size();
            c.out.pop_front();
            if (!c.outQpc.empty()) c.outQpc.pop_front();
            c.outOffset = 0;
        }
    }
    if (c.out.empty() && c.closeAfterSend) {
        shutdown(c.sock, SD_SEND);
        c.dead = true;
    }
}

void WebServer::ThreadMain()
{
    ComInit com;
    SetCurrentThreadName(L"Web viewer");
    MmcssScope mmcss(L"Playback", 1);
    for (;;) {
        std::vector<HANDLE> handles{m_stop, m_wake, m_listenEvent};
        {
            std::lock_guard lock(m_mutex);
            for (auto& c : m_clients) handles.push_back(c->event);
        }
        DWORD w = WaitForMultipleObjects(DWORD(handles.size()), handles.data(), FALSE, 100);
        if (w == WAIT_OBJECT_0) break;
        WSAResetEvent(m_listenEvent);
        Accept();
        ProcessAudio();
        std::lock_guard lock(m_mutex);
        for (auto& cp : m_clients) {
            Client& c = *cp;
            WSANETWORKEVENTS ne{};
            WSAEnumNetworkEvents(c.sock, c.event, &ne);
            if (ne.lNetworkEvents & FD_READ) ReadFrom(c);
            if (ne.lNetworkEvents & FD_CLOSE) {
                ReadFrom(c);
                c.dead = true;
            }
            Flush(c);
        }
        int viewers = 0;
        for (auto it = m_clients.begin(); it != m_clients.end();) {
            Client& c = **it;
            if (c.mode == Mode::Http && !c.closeAfterSend && QpcElapsedMs(c.opened) > 10000.0) c.dead = true;
            if (c.dead) {
                if (c.mode != Mode::Http) LOGI(L"Web viewer: %s left", c.addr.c_str());
                closesocket(c.sock);
                WSACloseEvent(c.event);
                it = m_clients.erase(it);
                continue;
            }
            if (c.mode == Mode::Mse || c.mode == Mode::WebCodecs) ++viewers;
            ++it;
        }
        m_viewers = viewers;
    }
}

} // namespace cc
