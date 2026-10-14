// Small confirmations: a short sound when recording starts / stops or a replay / clip is saved (optional), and
// "ask before closing" while something would be lost (a recording, viewers, unsaved replay footage).
#include "App.h"
#include "core/Log.h"

#include <mmsystem.h>

#include <cmath>

namespace cc {

namespace {

// A tiny WAV in memory: tones one after another (Hz, ms), 44.1 kHz 16-bit mono, soft edges.
std::vector<uint8_t> MakeTones(std::initializer_list<std::pair<double, int>> tones)
{
    constexpr int rate = 44100;
    std::vector<int16_t> pcm;
    for (const auto& [hz, ms] : tones) {
        const int n = rate * ms / 1000, fade = rate / 200;   // 5 ms in / out
        for (int i = 0; i < n; ++i) {
            const double env = std::min({1.0, double(i) / fade, double(n - i) / fade});
            pcm.push_back(int16_t(std::sin(2.0 * 3.14159265358979 * hz * i / rate) * env * 9000.0));
        }
    }
    const uint32_t data = uint32_t(pcm.size() * 2);
    std::vector<uint8_t> wav(44 + data);
    auto put32 = [&](size_t at, uint32_t v) { memcpy(&wav[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { memcpy(&wav[at], &v, 2); };
    memcpy(&wav[0], "RIFF", 4);
    put32(4, 36 + data);
    memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);   // PCM
    put16(22, 1);   // mono
    put32(24, rate);
    put32(28, rate * 2);
    put16(32, 2);
    put16(34, 16);
    memcpy(&wav[36], "data", 4);
    put32(40, data);
    memcpy(&wav[44], pcm.data(), data);
    return wav;
}

} // namespace

void App::PlayCue(Cue cue)
{
    if (!m_soundCue || m_cmd.selftestSec > 0) return;
    static const std::vector<uint8_t> start = MakeTones({{880, 70}, {1320, 90}});
    static const std::vector<uint8_t> stop = MakeTones({{1320, 70}, {880, 90}});
    static const std::vector<uint8_t> saved = MakeTones({{1175, 60}, {0, 40}, {1175, 60}});
    const std::vector<uint8_t>& w = cue == Cue::Start ? start : (cue == Cue::Stop ? stop : saved);
    PlaySoundW(reinterpret_cast<LPCWSTR>(w.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// App thread, every frame: what closing now would lose (read by the window thread's WM_CLOSE).
void App::UpdateCloseRisk()
{
    uint32_t risk = 0;
    if (m_recorder || m_recordPending) risk |= 1;
    if (ShareViewers() > 0) risk |= 2;
    if (m_replay) {
        const double buffered = m_replay->BufferedSeconds();
        const double sinceSave = m_lastReplaySave ? QpcElapsedMs(m_lastReplaySave) / 1000.0 : 1e9;
        if (std::min(buffered, sinceSave) >= 10.0) risk |= 4;
    }
    m_closeRisk = risk;
    m_closeViewers = ShareViewers();
    const int taskbar = m_recorder ? (m_recorder->Paused() ? 2 : 1) : 0;
    if (taskbar != m_taskbarState) {
        m_taskbarState = taskbar;
        m_taskbarShown = taskbar;
        PostMessageW(m_hwnd, WM_APP + 22, WPARAM(taskbar), 0);
    }
}

// Window thread (WM_CLOSE): true = go ahead and close.
bool App::ConfirmClose()
{
    const uint32_t risk = m_closeRisk;
    if (!m_askBeforeClose || !risk || m_cmd.selftestSec > 0) return true;
    std::wstring text = L"Close ZeroDisplay?\n";
    if (risk & 1) text += L"\n•  A recording is running (it is saved before closing).";
    if (risk & 2) text += Format(L"\n•  %d viewer(s) are watching what you share.", m_closeViewers.load());
    if (risk & 4) text += L"\n•  The replay buffer holds footage that wasn't saved (Save replay" + KeyHint(1) + L").";
    text += L"\n\n(Settings ▸ General ▸ Ask before closing)";
    return MessageBoxW(m_hwnd, text.c_str(), L"ZeroDisplay", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK;
}

} // namespace cc
