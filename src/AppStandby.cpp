// Standby: when a device's signal is lost (unplugged, HDMI source off, capture error), the last good
// picture stays on screen and keeps feeding recordings and viewers until the signal returns.
#include "App.h"
#include "core/Log.h"

#include <climits>
#include <cmath>
#include <strmif.h>

namespace cc {

bool App::InStandby(const Session& s) const
{
    if (!m_standbyOn || !s.surface.lastGood || !s.surface.lastGoodSrv) return false;
    if (!s.present) return true;
    const int64_t now = QpcNow();
    const CaptureState st = s.video->State();
    if (st != CaptureState::Running) return true;   // lost, reopening, busy or failed
    // Frames stopped arriving (some cards stop sending without a signal) ...
    if (QpcToMs(now - s.video->LastFrameQpc()) > 1000 && QpcToMs(now - s.openedQpc) > 1500) return true;
    // ... or this card's "no HDMI signal" output: a completely black picture. A short fade to black in a
    // game is left alone; only a picture that stays black for 3 s counts.
    return m_standbyOnBlack && s.blackSince && QpcToMs(now - s.blackSince) > 3000;
}

DWORD App::MsUntilNextEncode() const
{
    if (!m_encoder && !m_shareEncoder) return INFINITE;
    const int64_t now = QpcNow(), f = QpcFrequency();
    int64_t due = INT64_MAX;
    if (m_advanced && EncodesLink(L"#canvas")) {
        const int64_t period = f * m_scenes.fpsDen / std::max(1u, m_scenes.fpsNum);
        due = m_lastCanvasSubmit + period;
        // While capture frames keep coming, the canvas is encoded on those frames (they are rendered
        // anyway). Waking up for the deadline as well would render and present twice per frame and make
        // the next capture frame wait behind it, so only wake when capture is half a period late.
        if (now - m_lastCaptureQpc < 2 * period) due += period / 2;
    } else {
        for (const auto& s : m_sessions)
            if (s->standby && EncodesLink(s->info.symlink)) {
                if (const std::optional<EncoderConfig> c = ConfigForLink(s->info.symlink))
                    due = std::min(due, s->lastStandbySubmit + f * c->fpsDen / std::max(1u, c->fpsNum));
            }
    }
    if (due == INT64_MAX) return INFINITE;
    return due <= now ? 0 : DWORD((due - now) * 1000 / f);
}

// ---------------------------------------------------------------------------------------------
// Auto-crop: black bars are measured twice a second; a new crop is only used after the same result
// came back three times in a row (~1.5 s), so dark scenes and fades don't make the picture jump.
// A completely black picture never changes the crop.

void App::UpdateCrop(Session& s, int64_t now)
{
    if (!s.prof.autoCrop) {
        s.crop[0] = s.crop[1] = 0.0f;
        s.crop[2] = s.crop[3] = 1.0f;
        s.cropVotes = 0;
        return;
    }
    if (s.surface.hasImage && !s.standby && QpcToMs(now - s.lastBars) >= 500) {
        s.lastBars = now;
        m_renderer.DetectBars(s.surface);
    }
    if (s.surface.barsSeq == s.barsSeen) return;
    s.barsSeen = s.surface.barsSeq;
    if (!s.surface.barsValid) return;
    const float* r = s.surface.barsRect;
    if (r[2] - r[0] < 0.4f || r[3] - r[1] < 0.4f) return;   // implausible: more bar than picture
    bool same = true;
    for (int i = 0; i < 4; ++i) same &= std::fabs(r[i] - s.cropCand[i]) < 0.012f;
    if (!same) {
        for (int i = 0; i < 4; ++i) s.cropCand[i] = r[i];
        s.cropVotes = 1;
        return;
    }
    if (++s.cropVotes < 3) return;
    bool changed = false;
    for (int i = 0; i < 4; ++i) {
        changed |= std::fabs(s.crop[i] - s.cropCand[i]) > 0.001f;
        s.crop[i] = s.cropCand[i];
    }
    if (changed)
        LOGI(L"%s: auto-crop %.1f%% left/right, %.1f%% top/bottom", s.info.name.c_str(), s.crop[0] * 100.0f, s.crop[1] * 100.0f);
}

void App::DrawSession(const Session& s, const RectF& area, bool smooth, int forceScale)
{
    ID3D11ShaderResourceView* srv = s.standby ? s.surface.lastGoodSrv.Get() : (s.surface.hasImage ? s.surface.rgbSrv.Get() : nullptr);
    if (!srv) return;
    UINT w = s.standby ? s.surface.lastGoodW : s.surface.width, h = s.standby ? s.surface.lastGoodH : s.surface.height;
    if (!s.standby && s.prof.delayMs < 0)   // the picture waits for late sound (screen only)
        if (ID3D11ShaderResourceView* old = s.pictureDelay.Pick(QpcNow(), -s.prof.delayMs, w, h)) srv = old;
    // Shown width from the picture shape (SD frames are 4:3 / 16:9 TV pictures), of the cropped part.
    const DeviceProfile& p = s.prof;
    const UINT cw = UINT(std::lround(h * ShownAspect(s.surface, w, h, p.aspect) * (s.crop[2] - s.crop[0]))), ch = UINT(std::lround(h * (s.crop[3] - s.crop[1])));
    m_renderer.SetEnhance(EnhanceMode(p), p.enhanceStrength, true);
    const RectF dest = Renderer::FitRect(cw, ch, area, ScaleMode(forceScale >= 0 ? forceScale : p.scale));
    m_renderer.DrawTextureCropped(srv, s.crop, dest, area, smooth, ScreenFilter(p), w, h, float(p.filterLines), p.filterStrength);
    // HDR screen: the HDR picture goes on top at its real brightness (main window; not while the picture waits for
    // late sound, which keeps SDR copies, or while showing the last picture).
    if (m_renderer.HdrOutput() && !s.standby && p.delayMs >= 0) m_renderer.QueueHdrPicture(s.surface, s.crop, dest, area);
}

// ---------------------------------------------------------------------------------------------
// Device picture adjustments (UVC processing unit). Only values the user changed are saved, per
// device, and re-applied whenever the device is opened again.

static const wchar_t* PictureControlName(int id)
{
    switch (id) {
    case VideoProcAmp_Brightness: return L"Brightness";
    case VideoProcAmp_Contrast: return L"Contrast";
    case VideoProcAmp_Hue: return L"Hue";
    case VideoProcAmp_Saturation: return L"Saturation";
    case VideoProcAmp_Sharpness: return L"Sharpness";
    case VideoProcAmp_Gamma: return L"Gamma";
    case VideoProcAmp_ColorEnable: return L"Color";
    case VideoProcAmp_WhiteBalance: return L"White balance";
    case VideoProcAmp_BacklightCompensation: return L"Backlight compensation";
    case VideoProcAmp_Gain: return L"Gain";
    default: return L"Control";
    }
}

void App::ApplySavedPicture(Session& s)
{
    uint64_t gen = 0;
    const auto controls = s.video->PictureControls(&gen);
    if (gen == s.pictureGen) return;
    s.pictureGen = gen;
    const std::wstring sec = Settings::DeviceSection(s.info.Key());
    for (const auto& c : controls) {
        const int v = m_settings.GetInt(sec, Format(L"Pic%d", c.id).c_str(), INT_MIN);
        if (v == INT_MIN) continue;
        const bool autoOn = m_settings.GetBool(sec, Format(L"Pic%dAuto", c.id).c_str(), false);
        s.video->SetPictureControl(c.id, Clamp(long(v), c.min, c.max), autoOn && c.canAuto);
        LOGI(L"%s: restored %s = %d%s", s.info.name.c_str(), PictureControlName(c.id), v, autoOn ? L" (auto)" : L"");
    }
}

void App::DrawPictureControls(float& y, float cx, float cw)
{
    Session* s = Active();
    if (!s || s->video->IsNetwork()) return;
    const auto controls = s->video->PictureControls();
    if (controls.empty()) return;
    Ui& u = m_ui;
    const float rowH = u.S(34), labelH = u.S(22), gapS = u.S(10);
    const std::wstring sec = Settings::DeviceSection(s->info.Key());
    u.Text(L"Device picture: " + s->info.name, {cx, y, cw, labelH}, u.S(13), theme::Text, Align::Left, true);
    y += labelH + u.S(6);
    for (const auto& c : controls) {
        const bool boolean = c.max - c.min == 1;
        if (boolean) {
            bool on = c.value != c.min;
            if (u.Toggle(("pic" + std::to_string(c.id)).c_str(), {cx, y, cw, rowH}, PictureControlName(c.id), on)) {
                s->video->SetPictureControl(c.id, on ? c.max : c.min, false);
                m_settings.SetInt(sec, Format(L"Pic%d", c.id).c_str(), int(on ? c.max : c.min));
            }
            y += rowH + gapS;
            continue;
        }
        u.Text(PictureControlName(c.id), {cx, y, cw, labelH}, u.S(12.5f), theme::Text);
        u.Text(c.autoOn ? std::wstring(L"auto") : Format(L"%ld", c.value), {cx, y, cw, labelH}, u.S(12), theme::TextDim, Align::Right);
        y += labelH + u.S(2);
        float sw = cw;
        if (c.canAuto) {
            sw = cw - u.S(80);
            bool a = c.autoOn;
            if (u.Toggle(("picauto" + std::to_string(c.id)).c_str(), {cx + sw + u.S(8), y, u.S(72), rowH}, L"Auto", a)) {
                s->video->SetPictureControl(c.id, c.value, a);
                m_settings.SetBool(sec, Format(L"Pic%dAuto", c.id).c_str(), a);
                m_settings.SetInt(sec, Format(L"Pic%d", c.id).c_str(), int(c.value));
            }
        }
        if (c.autoOn) u.SetAlpha(0.4f);
        float v = float(c.value);
        if (u.Slider(("pic" + std::to_string(c.id)).c_str(), {cx, y, sw, rowH}, v, float(c.min), float(c.max)) && !c.autoOn) {
            long nv = c.min + long(std::lround((v - c.min) / float(c.step))) * c.step;
            nv = Clamp(nv, c.min, c.max);
            if (nv != c.value) {
                s->video->SetPictureControl(c.id, nv, false);
                m_settings.SetInt(sec, Format(L"Pic%d", c.id).c_str(), int(nv));
            }
        }
        u.SetAlpha(1.0f);
        y += rowH + gapS;
    }
    if (u.Button("picreset", {cx, y, u.S(170), rowH}, L"Reset picture", glyph::Refresh)) {
        for (const auto& c : controls) {
            s->video->SetPictureControl(c.id, c.def, c.canAuto && c.autoOn);
            m_settings.SetString(sec, Format(L"Pic%d", c.id).c_str(), L"");
            m_settings.SetString(sec, Format(L"Pic%dAuto", c.id).c_str(), L"");
        }
        LOGI(L"%s: picture reset to device defaults", s->info.name.c_str());
    }
    y += rowH + gapS;
}

} // namespace cc
