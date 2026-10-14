#include "net/NetReceiver.h"
#include "core/Log.h"

#include <strmif.h>   // ICodecAPI

#include <codecapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>

#include <algorithm>

namespace cc {

using namespace net;

NetReceiver::NetReceiver(IMFDXGIDeviceManager* dxgi, HANDLE frameEvent, std::shared_ptr<AudioSource> audio)
    : m_dxgi(dxgi), m_frameEvent(frameEvent), m_audio(std::move(audio))
{
    m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_audio) m_audio->extraReserveMs = 8.0f;   // absorb network jitter
}

NetReceiver::~NetReceiver()
{
    Close();
    CloseHandle(m_stop);
}

void NetReceiver::SetState(CaptureState s, std::wstring text)
{
    std::lock_guard lock(m_mutex);
    m_state = s;
    m_status = std::move(text);
}

CaptureState NetReceiver::State() const
{
    std::lock_guard lock(m_mutex);
    return m_state;
}

void NetReceiver::SetPin(const std::string& pin, bool typed)
{
    std::lock_guard lock(m_mutex);
    m_pin = pin;
    m_pinTyped = typed;
    ++m_pinGen;
    if (m_state == CaptureState::NeedsPin) {
        m_state = CaptureState::Opening;
        m_status = L"Checking the PIN…";
    }
}

std::string NetReceiver::Pin() const
{
    std::lock_guard lock(m_mutex);
    return m_pin;
}

std::wstring NetReceiver::StatusText() const
{
    std::lock_guard lock(m_mutex);
    return m_status;
}

std::vector<VideoFormat> NetReceiver::Formats() const
{
    VideoFormat f = Current();
    if (f.width == 0) return {};
    return {f};
}

VideoFormat NetReceiver::Current() const
{
    std::lock_guard lock(m_mutex);
    VideoFormat f;
    f.subtype = MFVideoFormat_H264;
    f.width = m_config.width;
    f.height = m_config.height;
    f.fpsNum = m_config.fpsNum;
    f.fpsDen = m_config.fpsDen ? m_config.fpsDen : 1;
    return f;
}

CaptureStats NetReceiver::Stats() const
{
    std::lock_guard lock(m_mutex);
    return m_stats;
}

bool NetReceiver::TakeFrame(CapturedFrame& out)
{
    std::lock_guard lock(m_mutex);
    if (!m_latest.sample) return false;
    out = std::move(m_latest);
    m_latest = {};
    return true;
}

void NetReceiver::Open(const VideoDeviceInfo& info, const std::wstring&, bool)
{
    Close();
    m_info = info;
    sockaddr_in server{};
    std::wstring endpoint = info.symlink.substr(4);   // strip "net:"
    if (!ParseEndpoint(endpoint, server, kStreamPort)) {
        SetState(CaptureState::Error, L"Invalid network address " + endpoint);
        return;
    }
    ResetEvent(m_stop);
    SetState(CaptureState::Opening, L"Connecting to " + endpoint + L"…");
    m_thread = std::thread([this, server] { ThreadMain(server); });
}

void NetReceiver::Reopen()
{
    VideoDeviceInfo info = m_info;
    Open(info, L"", false);
}

void NetReceiver::Close()
{
    if (m_thread.joinable()) {
        SetEvent(m_stop);
        m_thread.join();
    }
    SetState(CaptureState::Closed, L"Closed");
}

void NetReceiver::RequestKeyframe(const sockaddr_in& server)
{
    const int64_t now = QpcNow();
    if (QpcToMs(now - m_lastKeyRequest) < 100) return;
    m_lastKeyRequest = now;
    Header h{kMagic, Msg::KeyRequest};
    m_sock.SendTo(server, &h, sizeof(h));
}

bool NetReceiver::InitDecoder()
{
    m_decoder.Reset();
    HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_decoder));
    if (FAILED(hr)) {
        LOGE(L"Network: H.264 decoder unavailable: %s", HrToString(hr).c_str());
        return false;
    }
    ComPtr<IMFAttributes> attrs;
    m_decoder->GetAttributes(&attrs);
    UINT32 d3dAware = 0;
    if (attrs) {
        attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware);
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    if (d3dAware && m_dxgi) {
        if (FAILED(m_decoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, ULONG_PTR(m_dxgi.Get())))) LOGW(L"Network: decoding on the CPU");
    }
    ComPtr<ICodecAPI> api;
    if (SUCCEEDED(m_decoder.As(&api))) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = TRUE;
        api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    }
    ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, m_config.width, m_config.height);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, m_config.fpsNum, std::max(1u, m_config.fpsDen));
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = m_decoder->SetInputType(0, in.Get(), 0);
    if (FAILED(hr)) {
        LOGE(L"Network: decoder input type: %s", HrToString(hr).c_str());
        m_decoder.Reset();
        return false;
    }
    bool ok = false;
    for (DWORD i = 0; !ok; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(m_decoder->GetOutputAvailableType(0, i, &t))) break;
        GUID sub{};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12) ok = SUCCEEDED(m_decoder->SetOutputType(0, t.Get(), 0));
    }
    if (!ok) {
        LOGE(L"Network: decoder has no NV12 output");
        m_decoder.Reset();
        return false;
    }
    MFT_OUTPUT_STREAM_INFO info{};
    m_decoder->GetOutputStreamInfo(0, &info);
    m_decoderProvidesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    LOGI(L"Network: decoder ready (%ux%u, %s)", m_config.width, m_config.height, d3dAware ? L"GPU" : L"CPU");
    return true;
}

void NetReceiver::Decode(const std::vector<uint8_t>& au, int64_t time, int64_t arrival)
{
    if (!m_decoder) return;
    ComPtr<IMFMediaBuffer> buf;
    BYTE* dst = nullptr;
    if (FAILED(MFCreateMemoryBuffer(DWORD(au.size()), &buf)) || FAILED(buf->Lock(&dst, nullptr, nullptr))) return;
    memcpy(dst, au.data(), au.size());
    buf->Unlock();
    buf->SetCurrentLength(DWORD(au.size()));
    ComPtr<IMFSample> in;
    MFCreateSample(&in);
    in->AddBuffer(buf.Get());
    in->SetSampleTime(time);

    // Collects every frame the decoder has finished.
    auto pull = [&] {
    HRESULT hr = S_OK;
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        ComPtr<IMFSample> own;
        if (!m_decoderProvidesSamples) {
            ComPtr<IMFMediaBuffer> ob2;
            MFCreateMemoryBuffer(DWORD(m_config.width) * ((m_config.height + 31) & ~31u) * 3 / 2, &ob2);
            MFCreateSample(&own);
            own->AddBuffer(ob2.Get());
            ob.pSample = own.Get();
        }
        DWORD status = 0;
        hr = m_decoder->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            for (DWORD i = 0;; ++i) {
                ComPtr<IMFMediaType> t;
                if (FAILED(m_decoder->GetOutputAvailableType(0, i, &t))) break;
                GUID sub{};
                t->GetGUID(MF_MT_SUBTYPE, &sub);
                if (sub == MFVideoFormat_NV12 && SUCCEEDED(m_decoder->SetOutputType(0, t.Get(), 0))) break;
            }
            MFT_OUTPUT_STREAM_INFO info{};
            m_decoder->GetOutputStreamInfo(0, &info);
            m_decoderProvidesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            continue;
        }
        if (FAILED(hr)) break;   // needs more input
        CapturedFrame f;
        f.sample = ob.pSample;
        if (m_decoderProvidesSamples && ob.pSample) ob.pSample->Release();   // f.sample keeps its own reference
        f.layout = PixelLayout::NV12;
        f.width = m_config.width;
        f.height = m_config.height;
        f.stride = LONG(m_config.width);
        f.matrix = ColorMatrix::BT709;   // what the encoder's video processor produces
        f.range = ColorRange::Limited;
        f.arrivalQpc = arrival;
        LONGLONG outTime = time;
        if (f.sample) f.sample->GetSampleTime(&outTime);   // may belong to an earlier access unit
        f.sampleTime = outTime;
        const int64_t now = QpcNow();
        {
            std::lock_guard lock(m_mutex);
            f.seq = ++m_seq;
            if (m_latest.sample) m_stats.replaced++;
            m_latest = std::move(f);
            m_stats.frames++;
            m_fpsFrames++;
            double el = QpcToMs(now - m_fpsStart);
            if (el >= 1000) {
                m_stats.fps = m_fpsFrames * 1000.0 / el;
                m_fpsFrames = 0;
                m_fpsStart = now;
            }
            m_stats.ticks = m_framesLost;
            if (m_state != CaptureState::Running) {
                m_state = CaptureState::Running;
                m_status.clear();
            }
        }
        m_lastFrameQpc = now;
        ++m_healthOut;
        SetEvent(m_frameEvent);
    }
    };

    // A GPU decoder works ahead on several frames and refuses input while its queue is full. Every access
    // unit must reach it (dropping one corrupts the frames that reference it), so collect finished frames
    // and retry until it takes this one.
    HRESULT hr = m_decoder->ProcessInput(0, in.Get(), 0);
    for (int tries = 0; hr == MF_E_NOTACCEPTING && tries < 200; ++tries) {
        pull();
        hr = m_decoder->ProcessInput(0, in.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) Sleep(tries < 2 ? 0 : 1);
    }
    if (FAILED(hr)) {
        ++m_decodeRejected;
        LOGW(L"Network: decoder refused a frame: %s", HrToString(hr).c_str());
        m_needKeyframe = true;   // the picture can't be trusted until the next keyframe
        return;
    }
    pull();
}

namespace {
// What a sender may make us hold: one access unit at most this big (an 8K keyframe at a very high bitrate), and at
// most this many frames in flight. Anything beyond is malformed or hostile and ignored.
constexpr uint32_t kMaxFrameBytes = 32u << 20;
constexpr size_t kMaxPendingFrames = 64;
constexpr uint32_t kMaxDimension = 16384;
} // namespace

void NetReceiver::OnConfig(const ConfigMsg& in)
{
    ConfigMsg m = in;
    m.server[63] = m.device[63] = L'\0';   // names from the network: always terminated
    if (!m.width || !m.height || m.width > kMaxDimension || m.height > kMaxDimension || !m.fpsDen || m.fpsNum > 1000 * m.fpsDen ||
        m.audioRate > 192000)
        return;
    const bool changed = !m_haveConfig || m.width != m_config.width || m.height != m_config.height || m.instance != m_config.instance;
    const bool audioChanged = !m_haveConfig || m.audioRate != m_config.audioRate;
    {
        std::lock_guard lock(m_mutex);
        m_config = m;
    }
    m_haveConfig = true;
    if (changed) {
        LOGI(L"Network: stream from %s (%s) %ux%u @ %.2f fps, audio %u Hz", m.server, m.device, m.width, m.height,
             double(m.fpsNum) / std::max(1u, m.fpsDen), m.audioRate);
        ResetFrames();
        InitDecoder();
    }
    if (audioChanged && m_audio && m.audioRate) m_audio->SetInputFormat(m.audioRate, kAudioChunkFrames);
}

void NetReceiver::ResetFrames()
{
    m_pending.clear();
    m_nextId = 0;
    m_highestId = 0;
    m_needKeyframe = true;
}

void NetReceiver::OnVideo(const VideoMsg& h, const uint8_t* payload, size_t n, const sockaddr_in& server)
{
    if (!m_haveConfig) return;
    const uint32_t id = h.frameId;
    // A big jump either way means the sender restarted (or switched sources).
    if (m_nextId && (id + 1000 < m_nextId || id > m_nextId + 100000)) ResetFrames();
    if (m_nextId && id < m_nextId) return;   // already decoded, or given up on
    if (!m_nextId) m_nextId = id;
    // The sizes come from the network: consistent, and bounded, before anything is allocated.
    if (!h.totalSize || h.totalSize > kMaxFrameBytes || h.fragCount != (h.totalSize + kMaxPayload - 1) / kMaxPayload || n > kMaxPayload) return;
    if (!m_pending.count(id) && m_pending.size() >= kMaxPendingFrames) return;
    Pending& p = m_pending[id];
    if (p.known && p.data.size() != h.totalSize) return;   // contradicts the first fragment
    if (!p.known) {
        p.known = true;
        p.data.assign(h.totalSize, 0);
        p.got.assign(h.fragCount, false);
        p.left = h.fragCount;
        p.key = (h.h.flags & 1) != 0;
        p.time = h.time;
        if (!p.firstQpc) {
            p.firstQpc = QpcNow();
            NoteClock(h.time, QpcTo100ns(p.firstQpc));
        }
    }
    if (h.fragIndex >= p.got.size() || p.got[h.fragIndex]) return;   // duplicate (e.g. a resend that crossed)
    const size_t off = size_t(h.fragIndex) * kMaxPayload;
    if (off + n > p.data.size()) return;
    memcpy(p.data.data() + off, payload, n);
    p.got[h.fragIndex] = true;
    if (p.nacks) p.repaired = true;
    if (id > m_highestId) m_highestId = id;
    if (--p.left == 0) {
        ++m_framesReceived;
        ++m_healthComplete;
        if (p.repaired) ++m_healthRepaired;
    }
    Deliver();
}

void NetReceiver::Deliver()
{
    for (;;) {
        if (m_needKeyframe) {
            // Nothing before a keyframe can be decoded: jump to the first one we have (complete or not).
            auto key = std::find_if(m_pending.begin(), m_pending.end(), [](const auto& e) { return e.second.known && e.second.key; });
            if (key == m_pending.end()) {
                // Everything so far is useless without a keyframe: drop it and keep asking for one.
                m_healthWaitKey += uint32_t(std::count_if(m_pending.begin(), m_pending.end(), [](const auto& e) { return e.second.Complete(); }));
                m_pending.clear();
                if (m_highestId) m_nextId = m_highestId + 1;
                RequestKeyframe(m_server);   // rate-limited
                return;
            }
            m_healthWaitKey += uint32_t(std::count_if(m_pending.begin(), key, [](const auto& e) { return e.second.Complete(); }));
            m_pending.erase(m_pending.begin(), key);
            m_nextId = key->first;
        }
        auto it = m_pending.find(m_nextId);
        if (it == m_pending.end() || !it->second.Complete()) return;   // waiting (Repair resends or gives up)
        Pending f = std::move(it->second);
        m_pending.erase(it);
        ++m_nextId;
        if (f.key) m_needKeyframe = false;
        const int64_t t0 = QpcNow();
        int64_t t = MapTime(f.time);
        if (t && t <= m_lastVideoTime) t = m_lastVideoTime + 1;   // a new fastest path moved the mapping back a little
        if (t) m_lastVideoTime = t;
        Decode(f.data, t, f.firstQpc);
        const double ms = QpcElapsedMs(t0);
        ++m_healthIn;
        m_healthDecodeMs += ms;
        m_healthDecodeMax = std::max(m_healthDecodeMax, ms);
    }
}

void NetReceiver::SendNack(uint32_t id, const Pending& p, const sockaddr_in& server)
{
    NackMsg m{};
    m.frameId = id;
    if (!p.known) {
        m.count = kNackWholeFrame;
        m_sock.SendTo(server, &m, offsetof(NackMsg, frags));
        return;
    }
    uint16_t k = 0;
    for (size_t i = 0; i < p.got.size(); ++i) {
        if (p.got[i]) continue;
        m.frags[k++] = uint16_t(i);
        if (k == kMaxNackFrags) {
            m.count = k;
            m_sock.SendTo(server, &m, offsetof(NackMsg, frags) + k * sizeof(uint16_t));
            k = 0;
        }
    }
    if (k) {
        m.count = k;
        m_sock.SendTo(server, &m, offsetof(NackMsg, frags) + k * sizeof(uint16_t));
    }
}

// Wi-Fi loses packets now and then. Instead of throwing the frame away (and waiting for a keyframe, which
// is hundreds of packets and likely to lose one too), ask for exactly the missing fragments. A frame
// that can't be repaired within ~150 ms is given up and a keyframe requested.
void NetReceiver::Repair(int64_t now, const sockaddr_in& server)
{
    if (!m_haveConfig || !m_nextId) return;
    // Frames we haven't seen a single fragment of (newer ones did arrive).
    for (uint32_t id = m_nextId; id < m_highestId && id < m_nextId + 240; ++id)
        if (!m_pending.count(id)) m_pending[id].firstQpc = now;
    for (auto& [id, p] : m_pending) {
        if (p.Complete()) continue;
        const bool newerSeen = id < m_highestId;
        if (!newerSeen && QpcToMs(now - p.firstQpc) < 12.0) continue;   // its last fragments may still be on the way
        if (p.nacks < 4 && (p.nacks == 0 || QpcToMs(now - p.lastNack) > 30.0)) {
            SendNack(id, p, server);
            ++p.nacks;
            p.lastNack = now;
            ++m_healthNacks;
        }
    }
    auto head = m_pending.find(m_nextId);
    if (head != m_pending.end() && !head->second.Complete() && QpcToMs(now - head->second.firstQpc) > 150.0) {
        ++m_framesLost;
        m_needKeyframe = true;
        RequestKeyframe(server);
        m_pending.erase(head);
        ++m_nextId;
        Deliver();
    }
}

void NetReceiver::NoteClock(int64_t senderTime, int64_t arrival100ns)
{
    if (senderTime <= 0) return;
    if (!m_clockBlock || arrival100ns - m_clockBlock > 50000000) {   // 5 s blocks: follows clock drift
        m_clockPrevMin = m_clockMin;
        m_clockMin = INT64_MAX;
        m_clockBlock = arrival100ns;
    }
    m_clockMin = std::min(m_clockMin, arrival100ns - senderTime);
}

int64_t NetReceiver::MapTime(int64_t senderTime) const
{
    const int64_t off = std::min(m_clockMin, m_clockPrevMin);
    return senderTime > 0 && off != INT64_MAX ? senderTime + off : 0;
}

void NetReceiver::OnAudio(const AudioMsg& h, const uint8_t* payload, size_t n)
{
    if (!m_audio || !m_config.audioRate) return;
    const uint32_t frames = std::min<uint32_t>(h.frames, uint32_t(n / 4));
    m_pcm.resize(size_t(frames) * 2);
    const int16_t* s = reinterpret_cast<const int16_t*>(payload);
    for (uint32_t i = 0; i < frames * 2; ++i) m_pcm[i] = s[i] * (1.0f / 32768.0f);
    m_audio->Push(m_pcm.data(), frames, MapTime(h.time));
}

double NetReceiver::s_testLoss = 0.0;

bool NetReceiver::SelfTest()
{
    NetReceiver r(nullptr, nullptr, nullptr);
    sockaddr_in from{};
    std::vector<uint8_t> pkt(2048);
    uint32_t x = 0x9E3779B9u;
    auto rnd = [&] {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return x;
    };
    // A valid stream description (with unterminated names) so video and audio are accepted.
    ConfigMsg cfg{};
    cfg.width = 1280;
    cfg.height = 720;
    cfg.fpsNum = 60;
    cfg.fpsDen = 1;
    for (auto& ch : cfg.server) ch = L'A';
    for (auto& ch : cfg.device) ch = L'B';
    r.HandlePacket(reinterpret_cast<const uint8_t*>(&cfg), int(sizeof(cfg)), from);
    const bool configOk = r.m_haveConfig && r.m_config.server[63] == 0;
    size_t maxPending = 0, maxBytes = 0;
    for (int i = 0; i < 20000; ++i) {
        const int size = int(rnd() % pkt.size());
        for (int k = 0; k < size; ++k) pkt[size_t(k)] = uint8_t(rnd());
        if (size >= int(sizeof(Header))) {
            auto* h = reinterpret_cast<Header*>(pkt.data());
            h->magic = kMagic;
            const Msg types[] = {Msg::Video, Msg::Video, Msg::Video, Msg::Audio, Msg::Config, Msg::Bye, Msg(rnd())};
            h->type = types[rnd() % 7];
            if (h->type == Msg::Config && rnd() % 20) h->type = Msg::Video;   // few configs (each may restart the decoder)
            if (h->type == Msg::Video && size >= int(sizeof(VideoMsg))) {
                auto* v = reinterpret_cast<VideoMsg*>(pkt.data());
                if (rnd() % 2) v->frameId = r.m_nextId + rnd() % 200;   // near the stream, sometimes far away
                if (rnd() % 2) {                                         // plausible sizes half of the time
                    v->totalSize = 1 + rnd() % 200000;
                    v->fragCount = uint16_t((v->totalSize + kMaxPayload - 1) / kMaxPayload);
                    v->fragIndex = uint16_t(rnd() % (v->fragCount + 2));
                }
            }
        }
        if (rnd() % 50 == 0) r.HandlePacket(reinterpret_cast<const uint8_t*>(&cfg), int(sizeof(cfg)), from);   // back to a valid stream
        r.HandlePacket(pkt.data(), size, from);
        size_t bytes = 0;
        for (const auto& [id, p] : r.m_pending) bytes += p.data.size();
        maxPending = std::max(maxPending, r.m_pending.size());
        maxBytes = std::max(maxBytes, bytes);
    }
    const bool ok = configOk && maxPending <= kMaxPendingFrames && maxBytes <= kMaxPendingFrames * size_t(kMaxFrameBytes);
    LOGI(L"[selftest] network receiver: %s (names terminated %s, at most %zu frames / %.1f MB held)", ok ? L"PASS" : L"FAIL", configOk ? L"yes" : L"no",
         maxPending, maxBytes / 1048576.0);
    return ok;
}


void NetReceiver::HandlePacket(const uint8_t* data, int size, const sockaddr_in& server)
{
    if (size < int(sizeof(Header))) return;
    const Header* h = reinterpret_cast<const Header*>(data);
    if (h->magic != kMagic) return;
    if (s_testLoss > 0 && h->type == Msg::Video) {
        static uint32_t rng = 12345;
        rng = rng * 1664525u + 1013904223u;
        if ((rng >> 8) * (1.0 / 16777216.0) < s_testLoss) return;
    }
    m_lastPacket = QpcNow();
    switch (h->type) {
    case Msg::Config:
        if (size >= int(sizeof(ConfigMsg))) OnConfig(*reinterpret_cast<const ConfigMsg*>(data));
        break;
    case Msg::Video:
        if (size > int(sizeof(VideoMsg)))
            OnVideo(*reinterpret_cast<const VideoMsg*>(data), data + sizeof(VideoMsg), size_t(size) - sizeof(VideoMsg), server);
        break;
    case Msg::Audio:
        if (size > int(sizeof(AudioMsg))) OnAudio(*reinterpret_cast<const AudioMsg*>(data), data + sizeof(AudioMsg), size_t(size) - sizeof(AudioMsg));
        break;
    case Msg::Bye:
        m_haveConfig = false;
        SetState(CaptureState::Lost, L"The sender stopped sharing, waiting for it to come back");
        break;
    case Msg::Denied: {
        // No stream without the sharing PIN: ask for it, and stop knocking with this one until a new one is typed.
        m_haveConfig = false;
        const DenyReason why = DenyReason(h->flags);
        bool typed;
        {
            std::lock_guard lock(m_mutex);
            m_deniedGen = m_pinGen;
            typed = m_pinTyped;
        }
        m_deniedWait = why == DenyReason::Wait;
        const std::wstring ask = L"Type the PIN shown in Settings ▸ Network on the sharing PC.";
        SetState(CaptureState::NeedsPin, why == DenyReason::Wait       ? std::wstring(L"Too many wrong PINs: wait a minute, then try again.")
                                         : why == DenyReason::WrongPin ? (typed ? L"Wrong PIN. " : L"The sharing PC has a new PIN. ") + ask
                                                                       : ask);
        LOGI(L"Network: %s refused the stream (%s)", AddrToString(server, true).c_str(),
             why == DenyReason::Wait ? L"too many tries" : why == DenyReason::WrongPin ? L"wrong PIN" : L"PIN needed");
        break;
    }
    default: break;
    }
}

void NetReceiver::ThreadMain(sockaddr_in server)
{
    ComInit com;
    SetCurrentThreadName(L"Network receiver");
    MmcssScope mmcss(L"Playback", 1);
    if (!m_sock.Open(0, false, false, 8 << 20)) {
        SetState(CaptureState::Error, L"Could not open a network socket");
        return;
    }
    m_haveConfig = false;
    ResetFrames();
    m_server = server;
    {
        std::lock_guard lock(m_mutex);
        m_deniedGen = UINT32_MAX;   // a reopen asks again with the PIN it has
    }
    m_deniedWait = false;
    m_lastPacket = 0;
    m_framesLost = m_framesReceived = 0;
    m_fpsStart = QpcNow();
    const std::wstring endpoint = AddrToString(server, true);
    std::vector<uint8_t> buf(64 * 1024);
    int64_t lastHello = 0, lastKeepalive = 0;
    for (;;) {
        HANDLE handles[2] = {m_stop, m_sock.Event()};
        if (WaitForMultipleObjects(2, handles, FALSE, m_pending.empty() ? 100 : 4) == WAIT_OBJECT_0) break;
        WSAResetEvent(m_sock.Event());
        sockaddr_in from{};
        int n;
        while ((n = m_sock.RecvFrom(buf.data(), buf.size(), from)) > 0)
            if (from.sin_addr.s_addr == server.sin_addr.s_addr && from.sin_port == server.sin_port) HandlePacket(buf.data(), n, server);

        const int64_t now = QpcNow();
        Repair(now, server);
        if (!m_healthStart) m_healthStart = now;
        if (QpcToMs(now - m_healthStart) >= 5000) {
            // Once every 5 s: where frames go (complete frames in, decoded out, lost on the network).
            const double sec = QpcToMs(now - m_healthStart) / 1000.0;
            if (m_healthIn || m_framesLost != m_healthLost)
                LOGI(L"Network: %.1f fps complete, %.1f fps decoded in / %.1f out, %u repaired (%u NACKs), %u lost, %u skipped waiting for a "
                     L"keyframe, decode avg %.1f ms max %.1f ms",
                     m_healthComplete / sec, m_healthIn / sec, m_healthOut / sec, m_healthRepaired, m_healthNacks, m_framesLost - m_healthLost,
                     m_healthWaitKey, m_healthIn ? m_healthDecodeMs / m_healthIn : 0.0, m_healthDecodeMax);
            m_healthStart = now;
            m_healthIn = m_healthOut = m_healthComplete = m_healthWaitKey = m_healthNacks = m_healthRepaired = 0;
            m_healthLost = m_framesLost;
            m_healthDecodeMs = m_healthDecodeMax = 0;
        }
        const bool silent = !m_lastPacket || QpcToMs(now - m_lastPacket) > 1500;
        HelloMsg hello;
        bool refused;
        {
            std::lock_guard lock(m_mutex);
            refused = m_deniedGen == m_pinGen;
            memcpy(hello.pin, m_pin.data(), std::min(m_pin.size(), sizeof(hello.pin)));
        }
        // Refused: a "too many tries" wait ends by itself (one Hello every 10 s), a wrong / missing PIN waits for a new one.
        const double helloEvery = refused ? (m_deniedWait ? 10000.0 : -1.0) : 500.0;
        if ((!m_haveConfig || silent) && helloEvery > 0 && QpcToMs(now - lastHello) > helloEvery) {
            lastHello = now;
            m_sock.SendTo(server, &hello, sizeof(hello));
        }
        if (m_haveConfig && QpcToMs(now - lastKeepalive) > 1000) {
            lastKeepalive = now;
            KeepaliveMsg k{};
            k.h = {kMagic, Msg::Keepalive};
            k.framesReceived = m_framesReceived;
            k.framesLost = m_framesLost;
            m_sock.SendTo(server, &k, sizeof(k));
        }
        if (m_lastPacket && QpcToMs(now - m_lastPacket) > 2500 && State() == CaptureState::Running) {
            SetState(CaptureState::Lost, L"Connection to " + endpoint + L" lost, reconnecting…");
            m_needKeyframe = true;
        }
    }
    Header bye{kMagic, Msg::Bye};
    m_sock.SendTo(server, &bye, sizeof(bye));
    if (m_decoder) {
        m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        m_decoder.Reset();
    }
    m_sock.Close();
    std::lock_guard lock(m_mutex);
    m_latest = {};
}

} // namespace cc
