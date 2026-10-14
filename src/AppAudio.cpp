// Microphone commentary, the recording mix and recording limits.
#include "App.h"
#include "core/Log.h"

#include <algorithm>
#include <cwctype>

namespace cc {

static bool ContainsNoCase(const std::wstring& text, const std::wstring& part)
{
    auto it = std::search(text.begin(), text.end(), part.begin(), part.end(),
                          [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
    return it != text.end();
}

// The commentary microphone is captured into its own AudioSource. It is never played back locally
// (hearing yourself with delay is distracting); it only feeds the program mixer.
void App::UpdateMic()
{
    std::wstring want;
    for (const auto& a : m_devices.audioIn) {
        if (!m_cmd.mic.empty() ? ContainsNoCase(a.name, m_cmd.mic) : a.id == m_micId) {
            want = a.id;
            break;
        }
    }
    const bool restart = want != m_micRunningId || (m_micCap && m_micCap->Failed());
    if (!restart) return;
    m_micCap.reset();
    m_micRunningId = want;
    if (!m_micSrc) {
        m_micSrc = std::make_shared<AudioSource>();
        m_audio.AddSource(m_micSrc);
    }
    m_micSrc->monitor = false;
    m_micSrc->Reset();
    if (want.empty()) return;
    m_micCap = std::make_unique<AudioCapture>(want, m_micSrc);
    m_micCap->Start();
    LOGI(L"Microphone: %s", want.c_str());
    if (m_recorder && !m_recUsesMixer) Toast(L"The microphone will be in the next recording");
}

// Every frame: decides who is heard and what the program mixer combines. Simple mode only runs the
// mixer when a microphone is added (otherwise recordings take the device audio directly).
void App::UpdateMix()
{
    std::vector<MixInput> mix;
    const bool mic = m_micCap != nullptr;
    if (m_advanced) {
        // Advanced mode: every device that is visible (and not muted) in the program scene is heard.
        for (auto& s : m_sessions) {
            s->audioSrc->monitor = false;
            s->audioSrc->gain = 1.0f;
        }
        if (Scene* sc = ProgramScene())
            for (const auto& it : sc->items)
                if (it.kind == SourceKind::Device && it.visible && !it.muted)
                    if (Session* s = SessionForItem(it)) {
                        s->audioSrc->monitor = true;
                        s->audioSrc->gain = it.volume;
                        if (s->audioCap || s->video->OwnAudio())
                            mix.push_back({s->audioSrc, it.volume, it.gateOn, it.gateDb, it.compOn, it.compDb, it.compRatio});
                    }
    } else {
        for (size_t i = 0; i < m_sessions.size(); ++i) m_sessions[i]->audioSrc->monitor = (i == m_active);
        if (mic || m_recUsesMixer) {
            Session* s = Active();
            if (m_encoder)
                for (auto& ss : m_sessions)
                    if (ss->info.symlink == m_encoderSymlink) s = ss.get();
            if (s && (s->audioCap || s->video->OwnAudio())) mix.push_back({s->audioSrc});
        }
    }
    if (mic) mix.push_back({m_micSrc, m_micMuted ? 0.0f : m_micGain, m_micGate, -45.0f, m_micComp, -20.0f, 3.0f});
    if (m_micSrc) m_micSrc->tapsSilent = m_micMuted;   // muted: out of its own recording track too, as Settings says

    if (m_flash) {
        // Flashback: hear the past, not the live input (which is still recorded and shared).
        for (auto& s : m_sessions) s->audioSrc->monitor = false;
        m_flash->Audio()->monitor = true;
    }

    const bool want = m_advanced || mic || m_recUsesMixer;
    if (want != m_mixer.Running()) {
        if (want) m_mixer.Start();
        else m_mixer.Stop();
        // Replay and sharing follow the new audio source.
        m_replay.reset();
        m_sharedGen = 0;   // re-attach sharing with the new audio source
    }
    m_mixer.SetInputs(mix);
    m_mixInputs = std::move(mix);
}

bool App::OutputIsBluetooth()
{
    const std::wstring id = m_outputSetting.empty() ? m_devices.defaultOutId : m_outputSetting;
    if (id != m_btCheckedId) {
        m_btCheckedId = id;
        m_btOutput = DeviceManager::IsBluetoothEndpoint(id);
        if (m_btOutput) LOGI(L"Sound output is a Bluetooth device: its own delay isn't reported by Windows");
    }
    return m_btOutput;
}

// Sound packets come every ~10 ms while a device's sound is captured, even in silence. None for 3 s means the
// driver stopped delivering (seen with this card until it was replugged): restart the capture once, then say so.
double App::SoundSilentMs(const Session& s, int64_t now) const
{
    if (!s.audioCap || !s.audioCap->Running()) return 0;
    const int64_t last = std::max(s.audioSrc->LastPacketQpc(), s.audioStartQpc);
    return last ? QpcToMs(now - last) : 0;
}

void App::CheckSound(Session& s, int64_t now)
{
    if (!s.audioCap) {
        s.soundRestarts = 0;
        s.soundWarned = false;
        return;
    }
    const int64_t lastPacket = s.audioSrc->LastPacketQpc();
    if (lastPacket > s.audioStartQpc && QpcToMs(now - lastPacket) < 3000.0) {   // arriving (since the capture started)
        if (s.soundWarned) {
            Toast(L"Sound from " + s.info.name + L" is back", L"nosound:" + s.info.Key());
            LOGI(L"%s: sound arrives again", s.info.name.c_str());
        }
        s.soundRestarts = 0;
        s.soundWarned = false;
        return;
    }
    const double silent = SoundSilentMs(s, now);
    if (silent < 3000.0) return;
    if (s.soundRestarts == 0) {
        LOGW(L"%s: no sound packets for %.1f s, restarting the sound capture", s.info.name.c_str(), silent / 1000.0);
        s.soundRestarts = 1;
        s.soundRestartQpc = now;
        s.audioId.clear();   // forces UpdateAudioRouting to open it again
        UpdateAudioRouting(s);
        return;
    }
    if (!s.soundWarned && QpcToMs(now - s.soundRestartQpc) >= 5000.0) {
        s.soundWarned = true;
        LOGW(L"%s: still no sound after restarting the capture", s.info.name.c_str());
        Toast(L"No sound arrives from " + s.info.name + L" (Windows stopped delivering it). Unplug the device and plug it in again.",
              L"nosound:" + s.info.Key());
    }
}

std::shared_ptr<AudioSource> App::RecordAudioSource()
{
    if (m_mixer.Running()) return m_mixer.Output();
    if (m_advanced) return ProgramAudioSource();
    Session* s = Active();
    if (m_encoder)
        for (auto& ss : m_sessions)
            if (ss->info.symlink == m_encoderSymlink) s = ss.get();
    return s && (s->audioCap || s->video->OwnAudio()) ? s->audioSrc : nullptr;
}

// Track 1 is the mix; with separate tracks on, every mixer input follows on its own track
// (unprocessed, for editing).
std::vector<std::shared_ptr<AudioSource>> App::RecordTracks()
{
    std::vector<std::shared_ptr<AudioSource>> tracks;
    if (auto main = RecordAudioSource()) tracks.push_back(main);
    if ((m_separateTracks || m_cmd.tracks) && m_mixer.Running())
        for (const auto& in : m_mixInputs)
            if (in.source) tracks.push_back(in.source);
    return tracks;
}

void App::TogglePauseRecording()
{
    if (!m_recorder) {
        Toast(L"Nothing is being recorded");
        return;
    }
    const bool pause = !m_recorder->Paused();
    m_recorder->SetPaused(pause);
    for (auto& x : m_extraRecs)
        if (x.recorder) x.recorder->SetPaused(pause);
    PlayCue(pause ? Cue::Stop : Cue::Start);
    Toast(pause ? L"Recording paused: nothing is written until you resume" + KeyHint(8) : std::wstring(L"Recording resumed (same file)"));
}

void App::ToggleMicMute()
{
    if (m_micId.empty()) {
        Toast(L"No microphone is on (Settings ▸ Audio ▸ Microphone)");
        return;
    }
    m_micMuted = !m_micMuted;
    m_settingsDirty = true;
    Toast(m_micMuted ? L"Microphone muted" : L"Microphone on");
}

void App::CheckRecordingLimits()
{
    if (!m_recorder) return;
    if (!m_recorder->Error().empty()) {   // the file can't be written (folder, disk): say so now, not at the stop
        StopRecording();
        return;
    }
    // The encoder was restarted with other settings (Encoder Auto chose a faster preset or moved the recording to the
    // display GPU): the file continues seamlessly in a new part; say why there are several files.
    if (const int hp = m_recorder->HeaderParts(); hp > m_headerPartsShown) {
        m_headerPartsShown = hp;
        Toast(Format(L"The encoder changed its settings to keep up, so the recording continues in part %d", m_recorder->Parts()));
    }
    const int limit = m_cmd.limitSec > 0 ? m_cmd.limitSec : m_limitMinutes * 60;
    if (limit > 0 && m_recorder->Seconds() >= limit) {
        StopRecording(L"Time limit reached");
        return;
    }
    // The recorded source lost its signal (console off, cable out): the last picture keeps being recorded. Say so
    // after a few seconds, and stop after the chosen time without a signal.
    Session* rec = nullptr;
    if (!m_advanced)
        for (auto& s : m_sessions)
            if (s->info.symlink == m_encoderSymlink) rec = s.get();
    const bool lost = rec && (rec->standby || !rec->surface.hasImage) && !m_recorder->Paused();
    if (!lost) {
        if (m_noSignalSince && m_noSignalWarned) Toast(L"The signal is back", L"nosignal");
        m_noSignalSince = 0;
        m_noSignalWarned = false;
    } else {
        if (!m_noSignalSince) m_noSignalSince = QpcNow();
        const double secs = QpcElapsedMs(m_noSignalSince) / 1000.0;
        if (!m_noSignalWarned && secs >= 5.0) {
            m_noSignalWarned = true;
            LOGW(L"Recording without a signal from %s", rec->info.name.c_str());
            Toast(m_stopNoSignalMinutes > 0 ? Format(L"No signal: recording the last picture; stops in %d min without one", m_stopNoSignalMinutes)
                                            : std::wstring(L"No signal: still recording the last picture (Settings ▸ Recording can stop it)"),
                  L"nosignal");
        }
        if (m_stopNoSignalMinutes > 0 && secs >= m_stopNoSignalMinutes * 60.0) {
            StopRecording(Format(L"No signal for %d min", m_stopNoSignalMinutes));
            return;
        }
    }
    if (QpcElapsedMs(m_lastDiskCheck) < 5000) return;
    m_lastDiskCheck = QpcNow();
    std::wstring dir = m_recorder->Path();
    dir.resize(dir.find_last_of(L"\\/") == std::wstring::npos ? 0 : dir.find_last_of(L"\\/") + 1);
    ULARGE_INTEGER freeBytes{};
    if (!dir.empty() && GetDiskFreeSpaceExW(dir.c_str(), &freeBytes, nullptr, nullptr) && freeBytes.QuadPart < MinFreeBytes()) {
        LOGW(L"Stopping the recording: %.2f GB free", freeBytes.QuadPart / 1073741824.0);
        StopRecording(L"The disk is almost full");
        return;
    }
    if (!m_diskWarned && freeBytes.QuadPart && DiskMinutesLeft(freeBytes.QuadPart) < 5.0) {
        m_diskWarned = true;
        LOGW(L"Disk space for about %.1f more minutes", DiskMinutesLeft(freeBytes.QuadPart));
        Toast(L"Disk almost full: about 5 minutes of recording left");
    }
}

double App::DiskMinutesLeft(uint64_t freeBytes) const
{
    if (freeBytes <= MinFreeBytes()) return 0.0;
    double kbps = m_encoder ? double(m_encoder->Config().bitrateKbps) : 25000.0;
    for (const auto& x : m_extraRecs)
        if (x.encoder) kbps += x.encoder->Config().bitrateKbps;
    kbps = kbps * 1.1 + 400.0;   // container + sound
    return double(freeBytes - MinFreeBytes()) * 8.0 / 1000.0 / kbps / 60.0;
}

} // namespace cc
