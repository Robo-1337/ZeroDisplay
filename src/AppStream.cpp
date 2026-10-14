// LAN sharing quality: sharing has its own H.264 encoder whose bitrate is either fixed or chosen by
// "Smart" mode, which follows the slowest viewer's connection.
#include "App.h"
#include "core/Log.h"

#include <algorithm>

namespace cc {

UINT App::StreamPresetKbps(int quality)
{
    static const UINT presets[] = {0, 4000, 8000, 15000, 25000};
    return presets[Clamp(quality, 1, 4)];
}

UINT App::StreamStartKbps() const
{
    if (StreamQuality() != 0) return StreamCapKbps(StreamPresetKbps(StreamQuality()));
    return StreamCapKbps(m_smartKbps ? m_smartKbps : 12000);   // smart: start mid-range, keep what it learned
}

bool App::EncodesLink(const std::wstring& link) const
{
    if ((m_encoder && m_encoderSymlink == link) || (m_shareEncoder && m_shareSymlink == link)) return true;
    for (const auto& x : m_extraRecs)
        if (x.encoder && x.link == link) return true;
    return false;
}

std::optional<EncoderConfig> App::ConfigForLink(const std::wstring& link) const
{
    if (m_encoder && m_encoderSymlink == link) return m_encoder->Config();
    if (m_shareEncoder && m_shareSymlink == link) return m_shareEncoder->Config();
    for (const auto& x : m_extraRecs)
        if (x.encoder && x.link == link) return x.encoder->Config();
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// Multiview recording: the active device is the main recording (mic, tracks, replay as usual); every other
// device records its own file "<main>_<device>.mp4" with its own sound. Their encoders start in the
// background; with a second GPU in use for the main recording they go to the display GPU (spreads the load).

void App::StartExtraRecordings(const std::wstring& mainPath, int splitSeconds)
{
    Session* active = Active();
    for (auto& sp : m_sessions) {
        Session& s = *sp;
        if (&s == active) continue;
        if (!s.surface.hasImage) {
            LOGW(L"Multiview recording: %s has no picture yet, not recorded", s.info.name.c_str());
            continue;
        }
        ExtraRecording x;
        x.link = s.info.symlink;
        x.name = s.info.name;
        if (s.audioCap || s.video->OwnAudio()) x.audio = s.audioSrc;
        std::wstring safe = s.info.name;
        for (auto& ch : safe)
            if (wcschr(L"\\/:*?\"<>|", ch)) ch = L'_';
        x.path = mainPath.substr(0, mainPath.size() - 4) + L"_" + safe + L".mp4";
        const VideoFormat f = s.video->Current();
        EncoderConfig cfg;
        cfg.width = s.surface.width;
        cfg.height = s.surface.height;
        cfg.fpsNum = f.fpsNum ? f.fpsNum : 60;
        cfg.fpsDen = f.fpsDen ? f.fpsDen : 1;
        cfg.bitrateKbps = BitrateKbps(m_quality);
        cfg.gopFrames = UINT(std::lround(f.Fps() > 0 ? f.Fps() * 2 : 120));
        cfg.hevc = WantHevc() || cfg.width > 4096 || cfg.height > 4096;
        ResolveTarget(cfg, false);
        if (cfg.target == EncodeTarget::OtherGpu && m_encoder && m_encoder->Target() == EncodeTarget::OtherGpu) {
            cfg.target = DisplayTarget(cfg.hevc);
            cfg.otherAdapter = {};
            cfg.otherAdapterName.clear();
        }
        cfg.qualityVsSpeed = QvsForPreset(DesiredPreset(cfg));
        x.starting = std::async(std::launch::async, [device = ComPtr<ID3D11Device>(m_renderer.Device()),
                                                     manager = ComPtr<IMFDXGIDeviceManager>(m_renderer.DxgiManager()), cfg]() mutable {
            ComInit com;
            auto e = std::make_unique<VideoEncoder>();
            if (e->Start(device.Get(), manager.Get(), cfg)) return e;
            if (cfg.target != EncodeTarget::SameGpu) {   // second GPU / software failed: the display GPU
                cfg.target = EncodeTarget::SameGpu;
                if (e->Start(device.Get(), manager.Get(), cfg)) return e;
            }
            if (cfg.hevc) {
                cfg.hevc = false;
                if (e->Start(device.Get(), manager.Get(), cfg)) return e;
            }
            return std::unique_ptr<VideoEncoder>();
        });
        (void)splitSeconds;
        m_extraRecs.push_back(std::move(x));
    }
    if (!m_extraRecs.empty()) LOGI(L"Multiview recording: %zu more device(s)", m_extraRecs.size());
}

void App::PollExtraRecordings()
{
    for (size_t i = 0; i < m_extraRecs.size();) {
        ExtraRecording& x = m_extraRecs[i];
        if (x.starting.valid() && x.starting.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            x.encoder = x.starting.get();
            if (!x.encoder) {
                LOGW(L"Multiview recording: no encoder for %s", x.name.c_str());
                Toast(L"Could not record " + x.name);
                m_extraRecs.erase(m_extraRecs.begin() + ptrdiff_t(i));
                continue;
            }
            RecordOptions opt;
            if (x.audio) opt.tracks.push_back(x.audio);
            opt.splitSeconds = m_recorder ? (m_cmd.splitSec > 0 ? m_cmd.splitSec : m_splitMinutes * 60) : 0;
            x.recorder = std::make_unique<Recorder>();
            x.recorder->Start(x.path, *x.encoder, opt);
            LOGI(L"Multiview recording: %s with %s (%s)", x.name.c_str(), x.encoder->Name().c_str(), x.encoder->TargetName().c_str());
        }
        ++i;
    }
}

void App::StopExtraRecordings(const std::wstring& onlyLink)
{
    for (size_t i = 0; i < m_extraRecs.size();) {
        ExtraRecording& x = m_extraRecs[i];
        if (!onlyLink.empty() && x.link != onlyLink) {
            ++i;
            continue;
        }
        if (x.recorder) {
            x.recorder->RequestStop();   // detaches from the encoder now
            m_graveyard.push_back(std::async(std::launch::async, [r = std::move(x.recorder)]() mutable {
                ComInit com;
                r->Stop();
                LOGI(L"Multiview recording saved: %s", r->Path().c_str());
            }));
        }
        if (x.starting.valid())
            m_graveyard.push_back(std::async(std::launch::async, [f = std::move(x.starting)]() mutable {
                ComInit com;
                if (auto e = f.get()) e->Stop();
            }));
        RetireEncoder(std::move(x.encoder));
        m_extraRecs.erase(m_extraRecs.begin() + ptrdiff_t(i));
    }
}

// Frames are handed to the encoders right after Present (FlushSubmits), so their colour conversion never
// queues on the GPU ahead of the picture on screen. The textures keep this frame until the next upload.
void App::SubmitFrame(const std::wstring& link, ID3D11Texture2D* tex, int64_t time100ns, int64_t qpc, ID3D11Texture2D* hdrTex)
{
    m_submits.push_back({link, tex, hdrTex, time100ns, qpc});
}

bool App::EncodesHdr(const std::wstring& link) const
{
    if (m_encoder && m_encoderSymlink == link && m_encoder->Config().hdr) return true;
    for (const auto& x : m_extraRecs)
        if (x.encoder && x.link == link && x.encoder->Config().hdr) return true;
    return false;
}

void App::FlushSubmits()
{
    for (const auto& p : m_submits) {
        // HDR encoders take the linear copy when there is one (else the SDR picture, at the same white).
        auto texFor = [&](VideoEncoder& e) { return p.hdrTex && e.Config().hdr ? p.hdrTex.Get() : p.tex.Get(); };
        if (m_encoder && m_encoderSymlink == p.link) m_encoder->Submit(texFor(*m_encoder), p.time, p.qpc);
        if (m_shareEncoder && m_shareSymlink == p.link) m_shareEncoder->Submit(p.tex.Get(), p.time, p.qpc);   // viewers: SDR
        for (auto& x : m_extraRecs)
            if (x.encoder && x.link == p.link) x.encoder->Submit(texFor(*x.encoder), p.time, p.qpc);
    }
    m_submits.clear();
    // Encoders fed from system memory (second GPU / software): hand over the frames the GPU has finished.
    if (m_encoder) m_encoder->Collect();
    if (m_shareEncoder) m_shareEncoder->Collect();
    for (auto& x : m_extraRecs)
        if (x.encoder) x.encoder->Collect();
}

void App::DestroyShareEncoder()
{
    if (m_shareNext.valid()) m_graveyard.push_back(std::async(std::launch::async, [f = std::move(m_shareNext)]() mutable {
        if (auto e = f.get()) e->Stop();
    }));
    if (m_shareStart.valid()) m_graveyard.push_back(std::async(std::launch::async, [f = std::move(m_shareStart)]() mutable {
        ComInit com;
        if (auto e = f.get()) e->Stop();
    }));
    DetachSharing();
    RetireEncoder(std::move(m_shareEncoder));
    m_shareSymlink.clear();
}

// Hardware encoders don't reliably honour a bitrate change while running (Intel's accepts it and keeps
// the old rate), so a new bitrate means a new encoder: created on a worker thread (MFT activation can take
// 100+ ms) and swapped in between two frames. Viewers just get a fresh keyframe.
void App::ChangeStreamBitrate(UINT kbps)
{
    if (!m_shareEncoder || m_shareNext.valid() || kbps == StreamKbps()) return;
    EncoderConfig cfg = m_shareEncoder->Config();
    cfg.bitrateKbps = kbps;
    ID3D11Device* device = m_renderer.Device();
    IMFDXGIDeviceManager* manager = m_renderer.DxgiManager();
    m_shareNextLink = m_shareSymlink;
    m_shareNext = std::async(std::launch::async, [cfg, device, manager] {
        ComInit com;
        auto e = std::make_unique<VideoEncoder>();
        if (!e->Start(device, manager, cfg)) e.reset();
        return e;
    });
}

// Smart mode. Congestion = a viewer had to give up on frames (lost / asked for a keyframe), a browser
// viewer fell behind, or more than 3 % of the fragments needed a resend. Then the bitrate drops to 60 %
// (at most once per 3 s, the time a change needs to show) and doesn't rise for 8 s; while everything is
// clean it climbs 25 % every 6 s, up to 25 Mbps.
void App::UpdateStreamRate()
{
    const int64_t now = QpcNow();
    // A replacement encoder is ready: swap it in.
    if (m_shareNext.valid() && m_shareNext.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::unique_ptr<VideoEncoder> next = m_shareNext.get();
        if (next && m_shareEncoder && m_shareSymlink == m_shareNextLink && next->Config().width == m_shareEncoder->Config().width &&
            next->Config().height == m_shareEncoder->Config().height) {
            DetachSharing();
            std::unique_ptr<VideoEncoder> old = std::move(m_shareEncoder);
            m_shareEncoder = std::move(next);
            ++m_shareEncGen;
            UpdateSharingSource();   // re-attach the viewers right away
            m_graveyard.push_back(std::async(std::launch::async, [e = std::move(old)]() mutable { e->Stop(); }));
            m_rateCooldown = now + QpcFrequency() * 2;   // ignore the hiccup of the switch itself
            if (m_server) m_server->TakeRateStats();
            if (m_web) m_web->TakeSkips();
            LOGI(L"Sharing now at %.1f Mbps", StreamKbps() / 1000.0);
        } else if (next) {
            m_graveyard.push_back(std::async(std::launch::async, [e = std::move(next)]() mutable { e->Stop(); }));
        }
    }
    if (QpcToMs(now - m_lastRateCheck) < 1000.0) return;
    m_lastRateCheck = now;
    if (!m_server || !m_shareEncoder) return;
    const StreamServer::RateStats udp = m_server->TakeRateStats();
    const uint32_t webSkips = m_web ? m_web->TakeSkips() : 0;
    const int quality = StreamQuality();
    if (quality != 0) {
        ChangeStreamBitrate(StreamCapKbps(StreamPresetKbps(quality)));
        return;
    }
    if (ShareViewers() == 0 || m_shareNext.valid() || now < m_rateCooldown) return;
    const UINT cur = StreamKbps();
    const UINT kMin = 1500, kMax = StreamCapKbps(25000);
    const double resentShare = udp.sent ? double(udp.resent) / udp.sent : 0.0;
    std::wstring why;
    if (udp.lost) why = Format(L"a viewer lost %u frame(s)", udp.lost);
    else if (udp.keyRequests) why = L"a viewer needed a fresh keyframe";
    else if (webSkips) why = L"a browser viewer fell behind";
    else if (resentShare > 0.03) why = Format(L"%.0f%% of the packets needed a resend", resentShare * 100.0);
    UINT next = cur;
    if (!why.empty()) {
        next = std::max(kMin, UINT(cur * 0.6) / 100 * 100);
        m_rateCooldown = now + QpcFrequency() * 3;
        m_rateHoldUntil = now + QpcFrequency() * 8;
        m_rateReason = why;
    } else if (now >= m_rateHoldUntil && cur < kMax) {
        next = std::min(kMax, std::max(cur + 1000, UINT(cur * 1.25)) / 100 * 100);
        m_rateHoldUntil = now + QpcFrequency() * 6;
    }
    if (next == cur) return;
    LOGI(L"Smart stream quality: %.1f -> %.1f Mbps%s%s", cur / 1000.0, next / 1000.0, why.empty() ? L"" : L" (", why.empty() ? L"" : (why + L")").c_str());
    m_smartKbps = next;
    ChangeStreamBitrate(next);
}

// ---------------------------------------------------------------------------------------------
// Recording encoder preset. Auto = Balanced unless this GPU was found too slow for it at this
// codec/format; the answer is learned while recording (or buffering replays) and remembered.

std::wstring App::AutoKey(const EncoderConfig& c) const
{
    // Learned per encoder that actually does the work.
    std::wstring gpu = c.target == EncodeTarget::OtherGpu ? c.otherAdapterName
                       : c.target == EncodeTarget::Cpu   ? std::wstring(L"Software")
                       : m_renderer.Warp()               ? std::wstring(L"WARP")
                                                         : m_renderer.AdapterName();
    for (auto& ch : gpu)
        if (ch == L'=' || ch == L'[' || ch == L']' || ch == L';') ch = L'_';
    const UINT fps = UINT(double(c.fpsNum) / std::max(1u, c.fpsDen) + 0.5);
    return Format(L"%s.%ux%u@%u.%s", c.hevc ? L"HEVC" : L"H264", c.width, c.height, fps, gpu.c_str());
}

int App::DesiredPreset(const EncoderConfig& c)
{
    if (EncoderSpeed() != 0) return EncoderSpeed();
    const std::wstring key = AutoKey(c);
    auto it = m_autoLearned.find(key);
    if (it == m_autoLearned.end()) {
        const int saved = m_settings.GetInt(L"EncoderAuto", key.c_str(), 0);
        if (saved < 1 || saved > 3) return 2;   // not known yet: try Balanced
        it = m_autoLearned.emplace(key, saved).first;
    }
    return it->second;
}

void App::LearnAuto(const std::wstring& key, int preset)
{
    m_autoLearned[key] = preset;
    if (m_cmd.selftestSec <= 0) m_settings.SetInt(L"EncoderAuto", key.c_str(), preset);   // tests never touch settings
    LOGI(L"Encoder Auto: %s -> %s", key.c_str(), preset == 1 ? L"Fast" : (preset == 2 ? L"Balanced" : L"Best"));
}

// ---------------------------------------------------------------------------------------------
// Encoding hardware. Auto = recording on a second GPU's hardware encoder when there is one (the display
// GPU keeps capture, display and sharing to itself, zero copy) unless that GPU was found too slow for
// this format; sharing stays on the display GPU. Display GPU / Software apply to both encoders.

bool App::OtherEncoderGpu(bool hevc, EncoderAdapter& out)
{
    if (m_hybridBroken) return false;
    return VideoEncoder::FindOtherAdapter(m_renderer.AdapterLuid(), hevc, out);
}

std::wstring App::HybridKey(const EncoderConfig& c) const { return AutoKey(c) + L".Hybrid"; }

EncodeTarget App::DisplayTarget(bool hevc) const
{
    // No GPU video processor (software renderer) or no hardware encoder on the display GPU: software.
    const bool encodes = !m_renderer.Warp() && m_renderer.VideoSupport() && !m_displayNoEncoder[hevc ? 1 : 0];
    return encodes ? EncodeTarget::SameGpu : EncodeTarget::Cpu;
}

bool App::ProbeOtherGpu(bool hevc)
{
    const LUID luid = m_renderer.AdapterLuid();
    if (m_hybridBroken || (VideoEncoder::OtherAdapterKnown(luid, hevc) && VideoEncoder::OtherAdapterKnown(luid, false))) return true;
    if (m_otherProbe.valid()) {
        if (m_otherProbe.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        m_otherProbe.get();
        return ProbeOtherGpu(hevc);
    }
    m_otherProbe = std::async(std::launch::async, [luid, hevc] {
        ComInit com;
        EncoderAdapter a;
        VideoEncoder::FindOtherAdapter(luid, hevc, a);
        if (hevc) VideoEncoder::FindOtherAdapter(luid, false, a);   // the H.264 fallback
    });
    return false;
}

void App::ResolveTarget(EncoderConfig& cfg, bool forSharing)
{
    const int mode = EncodeHw();
    cfg.target = EncodeTarget::SameGpu;
    cfg.otherAdapter = {};
    cfg.otherAdapterName.clear();
    const bool displayEncodes = DisplayTarget(cfg.hevc) == EncodeTarget::SameGpu;
    if (mode == 3 || (!displayEncodes && (forSharing || mode == 2))) {
        cfg.target = EncodeTarget::Cpu;
        return;
    }
    if (forSharing || mode == 2) return;
    EncoderAdapter other;
    if (!OtherEncoderGpu(cfg.hevc, other)) {
        if (mode == 1 && !m_hybridWarned) {
            m_hybridWarned = true;
            Toast(L"Hybrid encoding needs a second GPU with a hardware encoder, using the display GPU");
        }
        if (!displayEncodes) cfg.target = EncodeTarget::Cpu;
        return;
    }
    EncoderConfig probe = cfg;
    probe.target = EncodeTarget::OtherGpu;
    probe.otherAdapterName = other.name;
    if (mode == 0 && displayEncodes) {
        const std::wstring key = HybridKey(probe);
        auto it = m_autoLearned.find(key);
        const int ok = it != m_autoLearned.end() ? it->second : m_settings.GetInt(L"EncoderAuto", key.c_str(), 1);
        if (ok == 0) return;   // learned: the other GPU can't keep up with this format
    }
    cfg.target = EncodeTarget::OtherGpu;
    cfg.otherAdapter = other.luid;
    cfg.otherAdapterName = other.name;
}

App::EncoderRequest App::WantedEncoder(EncoderConfig c)
{
    c.hevc = WantHevc();
    ResolveTarget(c, false);
    return {c.hevc, c.target, QvsForPreset(DesiredPreset(c))};
}

bool App::EncoderUpToDate() { return m_encoder && m_encoderRequest == WantedEncoder(m_encoder->Config()); }

// Every 2 s (Maintain). "Behind" = fewer than 95 % of the frames that went in came out.
void App::MonitorEncoder()
{
    if (!m_encoder || !m_encoder->Running() || m_encoder->Restarting()) {   // none, or being replaced (counters start again from 0)
        m_encoderBehind = false;
        m_lastSubmitted = m_lastEmitted = 0;
        return;
    }
    const uint64_t sub = m_encoder->Submitted(), em = m_encoder->Emitted();
    if (sub < m_lastSubmitted || em < m_lastEmitted) {   // a new or restarted encoder
        m_lastSubmitted = sub;
        m_lastEmitted = em;
        return;
    }
    const uint64_t ds = sub - m_lastSubmitted, de = em - m_lastEmitted;
    m_lastSubmitted = sub;
    m_lastEmitted = em;
    if (ds < 30) return;   // hardly any frames (no picture / standby gaps): nothing to judge
    m_encoderBehind = de * 100 < ds * 95;
    // Act only on a sustained shortfall (two windows in a row), never on one caused by a screen mode change (the GPU
    // pauses briefly): restarting the encoder splits a running recording into parts.
    const bool displayChanged = QpcElapsedMs(m_displayChangedQpc.load()) < 4000.0;
    m_behindStreak = m_encoderBehind && !displayChanged ? m_behindStreak + 1 : 0;
    const bool behind = m_behindStreak >= 2;
    const EncoderConfig cfg = m_encoder->Config();
    const int cur = PresetForQvs(cfg.qualityVsSpeed);
    if (behind && cfg.target == EncodeTarget::OtherGpu && EncodeHw() == 0 && (cur == 1 || EncoderSpeed() != 0) && !m_hybridMovedBack) {
        // Auto hardware: the second GPU can't keep up even at its fastest setting -> the display GPU, in
        // place (a running recording continues in a new part). Remembered for this format only once the display GPU
        // has kept up for a minute (below): a short load spike must not send every later recording to a weaker GPU.
        const std::wstring key = HybridKey(cfg);
        m_autoLearned[key] = 0;
        m_hybridMoved = true;
        m_hybridFrom = cfg;
        m_hybridMovedQpc = QpcNow();
        EncoderConfig c2 = cfg;
        c2.target = EncodeTarget::SameGpu;
        c2.otherAdapter = {};
        c2.otherAdapterName.clear();
        c2.qualityVsSpeed = QvsForPreset(DesiredPreset(c2));
        LOGW(L"Encoder Auto: %s keeps up with only %.0f%% of the frames; moving the recording to the display GPU", cfg.otherAdapterName.c_str(),
             100.0 * double(de) / double(ds));
        m_encoder->Restart(&c2);
        m_encoderRequest = WantedEncoder(c2);
        m_lastSubmitted = m_lastEmitted = 0;
        m_autoCalibrating = EncoderSpeed() == 0 && !m_autoLearned.count(AutoKey(c2));
        m_autoSince = QpcNow();
        m_encoderBehind = false;
        Toast(L"Recording moved to the display GPU to keep up");
        return;
    }
    if (m_hybridMoved && cfg.target == EncodeTarget::SameGpu && EncodeHw() == 0) {
        if (behind && (cur == 1 || EncoderSpeed() != 0)) {
            // The display GPU can't keep up either (even on Fast): the move didn't help. Back to the second GPU, once per
            // recording, and the second GPU stays this format's choice.
            const std::wstring key = HybridKey(m_hybridFrom);
            m_autoLearned[key] = 1;
            if (m_cmd.selftestSec <= 0) m_settings.SetInt(L"EncoderAuto", key.c_str(), 1);   // tests never touch settings
            EncoderConfig c2 = m_hybridFrom;
            LOGW(L"Encoder Auto: the display GPU keeps up with only %.0f%% of the frames too; moving the recording back to %s",
                 100.0 * double(de) / double(ds), c2.otherAdapterName.c_str());
            m_encoder->Restart(&c2);
            m_encoderRequest = WantedEncoder(c2);
            m_lastSubmitted = m_lastEmitted = 0;
            m_hybridMoved = false;
            m_hybridMovedBack = true;
            m_autoCalibrating = false;
            m_autoSince = QpcNow();
            m_encoderBehind = false;
            Toast(L"Recording moved back to " + c2.otherAdapterName + L": the display GPU couldn't keep up either");
            return;
        }
        if (!behind && QpcToMs(QpcNow() - m_hybridMovedQpc) > 60000.0) {
            // A minute on the display GPU keeping up: the move was right; recordings of this format start there.
            m_hybridMoved = false;
            if (m_cmd.selftestSec <= 0) m_settings.SetInt(L"EncoderAuto", HybridKey(m_hybridFrom).c_str(), 0);
            LOGI(L"Encoder Auto: the display GPU keeps up; this format records there from now on");
        }
    }
    if (EncoderSpeed() == 0) {
        if (behind && cur > 1) {
            // Auto: one step faster, right away (the encoder restarts in place, so a running recording,
            // the replay buffer and viewers continue), and remember it for this format.
            const int next = cur - 1;
            LearnAuto(AutoKey(cfg), next);
            EncoderConfig c2 = cfg;
            c2.qualityVsSpeed = QvsForPreset(next);
            LOGW(L"Encoder Auto: %s keeps up with only %.0f%% of the frames; switching to %s", cur == 3 ? L"Best" : L"Balanced",
                 100.0 * double(de) / double(ds), next == 1 ? L"Fast" : L"Balanced");
            m_encoder->Restart(&c2);
            m_encoderRequest = WantedEncoder(c2);
            m_lastSubmitted = m_lastEmitted = 0;
            m_autoCalibrating = next > 1;
            m_autoSince = QpcNow();
            m_encoderBehind = false;
            Toast(next == 1 ? L"Encoder set to Fast automatically to keep up at 60 fps" : L"Encoder set to Balanced automatically to keep up");
            return;
        }
        if (!behind && m_autoCalibrating && QpcToMs(QpcNow() - m_autoSince) > 4000.0) {
            m_autoCalibrating = false;
            LearnAuto(AutoKey(cfg), cur);
        }
    }
    if (behind && m_recorder && !m_behindWarned && (EncoderSpeed() != 0 || cur == 1)) {
        m_behindWarned = true;
        LOGW(L"Recording encoder can't keep up (%llu of %llu frames)", (unsigned long long)de, (unsigned long long)ds);
        Toast(EncoderSpeed() == 0 ? L"This PC can't encode this format in real time even on Fast, try a lower resolution or frame rate"
                                  : L"The encoder can't keep up at this setting, Settings \u25B8 Recording \u25B8 Encoder: try Fast or Auto");
    }
}

} // namespace cc
