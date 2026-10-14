// Flashback: look back through the replay buffer (scrub, pause, jump ±5 s) while capture, recording
// and sharing continue live. ← opens it 5 s back; Esc / End / L returns to live.
#include "App.h"
#include "core/Log.h"

#include <shlobj.h>

namespace cc {

void App::StartFlashback(double secondsBack)
{
    if (!ReplayAllowed()) {
        Toast(L"Flashback needs the replay buffer, which is off: " + ProfileSummary());
        return;
    }
    if (!m_replay) {
        m_replayOn = true;   // flashback needs the buffer: start it so the next try has something
        m_settingsDirty = true;
        UpdateReplay();
        Toast(L"Flashback needs the replay buffer, it's on now; try again in a few seconds");
        return;
    }
    std::vector<PacketPtr> video;
    std::vector<AudioChunk> audio;
    EncoderConfig cfg;
    if (!m_replay->Snapshot(video, audio, cfg)) {
        Toast(L"Nothing buffered yet");
        return;
    }
    auto fb = std::make_unique<Flashback>();
    if (!fb->Start(m_renderer.DxgiManager(), cfg, std::move(video), std::move(audio))) {
        Toast(L"Nothing buffered yet");
        return;
    }
    m_flash = std::move(fb);
    m_clipIn = m_clipOut = -1;
    m_audio.AddSource(m_flash->Audio());
    m_flash->Seek(std::max(0.0, m_flash->Duration() - secondsBack));
    m_flash->SetPlaying(true);
    LOGI(L"Flashback opened (%.1f s buffered)", m_flash->Duration());
}

void App::StopFlashback()
{
    if (!m_flash) return;
    m_audio.RemoveSource(m_flash->Audio().get());
    m_flash.reset();
    m_flashSurface = VideoSurface{};
    LOGI(L"Flashback closed (live)");
}

void App::FlashbackJump(double delta)
{
    if (!m_flash) return;
    const double to = m_flash->Position() + delta;
    if (to >= m_flash->Duration()) {
        StopFlashback();   // jumped past the end: back to live
        return;
    }
    m_flash->Seek(std::max(0.0, to));
}

void App::UpdateFlashback()
{
    if (!m_flash) return;
    CapturedFrame f;
    if (m_flash->TakeFrame(f)) m_renderer.Upload(m_flashSurface, f, m_matrix, m_range);
    if (m_flash->AtEnd()) {
        StopFlashback();
        Toast(L"Back to live");
    }
}

RectF App::FlashbackArea() const
{
    if (m_advanced && !m_clean) return CanvasArea();
    const auto tiles = TileRects();
    return tiles.empty() ? RectF{0, 0, float(m_renderer.Width()), float(m_renderer.Height())} : tiles[0];
}

void App::DrawFlashbackBar(const UiInput& in)
{
    (void)in;
    Ui& u = m_ui;
    const RectF area = FlashbackArea();
    const float h = u.S(56), pad = u.S(12);
    RectF bar{area.x + pad, area.Bottom() - h - pad, area.w - 2 * pad, h};
    u.Fill(bar, theme::Scrim, u.S(4));
    const float bh = u.S(36), by = bar.y + (h - bh) * 0.5f;
    float x = bar.x + u.S(10);
    if (u.IconButton("fb_back", {x, by, bh, bh}, glyph::Back, L"Back 5 s (\u2190)", false)) FlashbackJump(-5.0);
    x += bh + u.S(4);
    if (!m_flash) return;
    const bool playing = m_flash->Playing();
    if (u.IconButton("fb_play", {x, by, bh, bh}, playing ? glyph::Pause : glyph::Play, playing ? L"Pause (Space)" : L"Play (Space)", false))
        m_flash->SetPlaying(!playing);
    x += bh + u.S(4);
    if (u.IconButton("fb_fwd", {x, by, bh, bh}, glyph::Forward, L"Forward 5 s (\u2192)", false)) FlashbackJump(5.0);
    if (!m_flash) return;
    x += bh + u.S(12);

    const double dur = m_flash->Duration(), pos = m_flash->Position();
    const std::wstring label = Format(L"\u2212%.1f s", std::max(0.0, dur - pos));
    const float lw = u.S(70);
    u.Text(label, {x, bar.y, lw, h}, u.S(13), theme::Text, Align::Left, true);
    x += lw;
    const float liveW = u.S(84), markW = u.S(84), clipW = markW * 2 + u.S(4) + u.S(116);
    RectF track{x, by, bar.Right() - x - liveW - clipW - u.S(28), bh};
    // Clip range under the timeline.
    if (m_clipIn >= 0 || m_clipOut >= 0) {
        const double a = m_clipIn >= 0 ? m_clipIn : 0.0, b = m_clipOut >= 0 ? m_clipOut : dur;
        const float x0 = track.x + track.w * float(a / dur), x1 = track.x + track.w * float(b / dur);
        if (x1 > x0) u.Fill({x0, track.y + track.h * 0.5f - u.S(7), x1 - x0, u.S(14)}, theme::Accent2.A(0.35f), u.S(3));
    }
    float v = float(pos);
    if (u.Slider("fb_pos", track, v, 0.0f, float(dur))) m_flash->Seek(v);
    float cx = track.Right() + u.S(12);
    // Clip marks: words, not arrows (the arrows above jump 5 s); blue once set.
    if (u.Button("fb_in", {cx, by, markW, bh}, L"Start (I)", 0, m_clipIn >= 0)) m_clipIn = m_flash->Position();
    cx += markW + u.S(4);
    if (u.Button("fb_out", {cx, by, markW, bh}, L"End (O)", 0, m_clipOut >= 0)) m_clipOut = m_flash->Position();
    cx += markW + u.S(6);
    if (u.Button("fb_save", {cx, by, u.S(110), bh}, L"Save clip", glyph::Cut)) SaveFlashbackClip();
    if (u.Button("fb_live", {bar.Right() - liveW - u.S(8), by, liveW, bh}, L"LIVE", 0, true)) StopFlashback();
    u.Text(L"FLASHBACK", {bar.x, bar.y - u.S(22), u.S(200), u.S(20)}, u.S(11.5f), theme::Warn, Align::Left, true);
}

// Saves the marked part (default: everything) without re-encoding video.
void App::SaveFlashbackClip()
{
    if (!m_flash) return;
    double a = m_clipIn >= 0 ? m_clipIn : 0.0, b = m_clipOut >= 0 ? m_clipOut : m_flash->Duration();
    if (b < a) std::swap(a, b);
    std::vector<PacketPtr> video;
    std::vector<AudioChunk> audio;
    EncoderConfig cfg;
    if (b - a < 0.2 || !m_flash->Clip(a, b, video, audio, cfg)) {
        Toast(L"Mark a longer part first (I = start, O = end)");
        return;
    }
    std::wstring folder = RecordFolder();
    if (!EnsureFolder(folder, L"the clip")) return;
    const std::wstring path = NewFilePath(folder, L"Clip", L".mp4");
    LOGI(L"Flashback clip %.2f..%.2f s -> %s", a, b, path.c_str());
    Toast(L"Saving clip\u2026");
    m_graveyard.push_back(std::async(std::launch::async, [this, path, cfg, video = std::move(video), audio = std::move(audio)] {
        EfficiencyScope eco;
        Recorder writer;
        const bool ok = writer.WriteClip(path, cfg, video, audio);
        if (ok) ToastFile(L"Clip saved: " + path, path);
        else Toast(L"The clip could not be saved");
        if (ok) {
            PlayCue(Cue::Saved);
            RememberMadeFile(path);
            EnforceKeepLimit();
        }
    }));
}

} // namespace cc
