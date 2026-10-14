// Crop by dragging on the picture (Settings ▸ Video ▸ Crop and turn ▸ Crop on the picture). While editing, the shown
// device's picture is converted without its crop (turn and mirror stay) and drawn whole; the crop is a frame whose
// edges are dragged. Done (Enter) keeps it, Cancel (Esc) puts the old one back. Recording locks it (one picture size).
#include "App.h"
#include "core/Log.h"

namespace cc {

// Which source edge (crop index: 0 left, 1 top, 2 right, 3 bottom) a screen edge (same order) shows, after the turn
// (quarter turns clockwise) and the mirror (left-right, after turning).
static int SourceEdge(int screenEdge, int rotate, bool mirror)
{
    if (mirror && (screenEdge == 0 || screenEdge == 2)) screenEdge = 2 - screenEdge;
    static const int map[4][4] = {{0, 1, 2, 3}, {3, 0, 1, 2}, {2, 3, 0, 1}, {1, 2, 3, 0}};
    return map[((rotate % 4) + 4) % 4][screenEdge];
}

bool App::CanCropOnPicture(std::wstring* why)
{
    Session* s = Active();
    const wchar_t* reason = nullptr;
    if (!s || m_advanced) reason = L"Needs a device shown in Simple mode.";
    else if (m_multiview && SimpleSessionCount() > 1) reason = L"Show one device (not all side by side) to crop on the picture.";
    else if (TransformLocked(*s)) reason = L"Stop recording first: the recording keeps one picture size.";
    else if (!s->surface.hasImage) reason = L"Needs a picture from the device.";
    if (why) *why = reason ? reason : L"";
    return !reason;
}

void App::BeginCropEdit()
{
    if (!CanCropOnPicture(nullptr)) return;
    m_cropBefore = m_xf;
    m_cropEdit = true;
    m_cropDrag = -1;
    m_settingsOpen = false;
    LOGI(L"Crop on the picture: editing");
    InvalidateUi();
}

void App::EndCropEdit(bool keep)
{
    if (!m_cropEdit) return;
    if (!keep) m_xf = m_cropBefore;
    m_cropEdit = false;
    m_cropDrag = -1;
    m_settingsDirty = true;
    LOGI(L"Crop on the picture: %s (%.3f %.3f %.3f %.3f)", keep ? L"kept" : L"cancelled", m_xf.crop[0], m_xf.crop[1], m_xf.crop[2], m_xf.crop[3]);
    InvalidateUi();
}

// The whole (uncropped, turned) picture as drawn while editing.
RectF App::CropEditRect(const Session& s) const
{
    const RectF area = TileRects()[0];
    const UINT w = s.surface.width, h = s.surface.height;
    if (!w || !h) return area;
    const UINT cw = UINT(std::lround(h * ShownAspect(s.surface, w, h, s.prof.aspect)));
    RectF r = Renderer::FitRect(cw, h, area, ScaleMode::Fit);
    const float m = m_ui.S(40);   // room around it for the handles, the top bar above and the line + buttons below
    if (r.w > area.w - 2 * m || r.h > area.h - 4 * m) {
        const RectF inner{area.x + m, area.y + m * 1.5f, area.w - 2 * m, area.h - 4 * m};
        r = Renderer::FitRect(cw, h, inner, ScaleMode::Fit);
    }
    return r;
}

void App::DrawCropEditor()
{
    Session* s = Active();
    if (!CanCropOnPicture(nullptr)) {   // the device went away, a recording started, ...
        EndCropEdit(true);
        return;
    }
    Ui& u = m_ui;
    const UiInput& in = u.Input();
    const RectF pic = CropEditRect(*s);
    const int rot = m_xf.rotate;
    const bool mir = m_xf.mirror;
    float f[4];
    for (int e = 0; e < 4; ++e) f[e] = m_xf.crop[SourceEdge(e, rot, mir)];
    const RectF c{pic.x + f[0] * pic.w, pic.y + f[1] * pic.h, pic.w * (1 - f[0] - f[2]), pic.h * (1 - f[1] - f[3])};

    // Dragging an edge (grabbed within a few pixels of it, along its length).
    const float grab = u.S(18);
    if (in.pressed && m_cropDrag < 0) {
        const bool inY = in.my > c.y - grab && in.my < c.Bottom() + grab, inX = in.mx > c.x - grab && in.mx < c.Right() + grab;
        float best = grab;
        const float d[4] = {std::abs(in.mx - c.x), std::abs(in.my - c.y), std::abs(in.mx - c.Right()), std::abs(in.my - c.Bottom())};
        for (int e = 0; e < 4; ++e)
            if (d[e] < best && ((e % 2 == 0) ? inY : inX)) {
                best = d[e];
                m_cropDrag = e;
            }
    }
    if (m_cropDrag >= 0 && in.down) {
        const int e = m_cropDrag;
        float v = e == 0 ? (in.mx - pic.x) / pic.w : e == 2 ? (pic.Right() - in.mx) / pic.w : e == 1 ? (in.my - pic.y) / pic.h : (pic.Bottom() - in.my) / pic.h;
        const float opposite = f[(e + 2) % 4];
        v = std::clamp(v, 0.0f, std::min(0.45f, 0.9f - opposite));
        if (v < 0.003f) v = 0.0f;
        float& target = m_xf.crop[SourceEdge(e, rot, mir)];
        if (std::abs(target - v) > 0.0005f) {
            target = v;
            m_settingsDirty = true;
        }
    }
    if (!in.down && m_cropDrag >= 0) {
        LOGI(L"Crop on the picture: edge %d now %.3f", m_cropDrag, m_xf.crop[SourceEdge(m_cropDrag, rot, mir)]);
        m_cropDrag = -1;
    }

    // Outside the crop dimmed, the frame and its four handles.
    const Color dim{0, 0, 0, 0.62f};
    u.Fill({pic.x, pic.y, pic.w, c.y - pic.y}, dim);
    u.Fill({pic.x, c.Bottom(), pic.w, pic.Bottom() - c.Bottom()}, dim);
    u.Fill({pic.x, c.y, c.x - pic.x, c.h}, dim);
    u.Fill({c.Right(), c.y, pic.Right() - c.Right(), c.h}, dim);
    u.Stroke(c, theme::Accent2, u.S(2));
    const float hl = u.S(36), ht = u.S(8);
    const RectF handles[4] = {{c.x - ht / 2, c.y + c.h / 2 - hl / 2, ht, hl}, {c.x + c.w / 2 - hl / 2, c.y - ht / 2, hl, ht},
                              {c.Right() - ht / 2, c.y + c.h / 2 - hl / 2, ht, hl}, {c.x + c.w / 2 - hl / 2, c.Bottom() - ht / 2, hl, ht}};
    for (int e = 0; e < 4; ++e) u.Fill(handles[e], m_cropDrag == e ? theme::Text : theme::Accent2, u.S(theme::Radius));

    // What the picture becomes (the same even-pixel rounding the conversion uses), and the buttons.
    const VideoSurface& sf = s->surface;
    auto even = [](float part, UINT n) { return UINT(std::lround(std::clamp(part, 0.0f, 0.45f) * float(n) / 2.0f)) * 2; };
    const UINT sw = sf.srcW ? sf.srcW : sf.width, sh = sf.srcH ? sf.srcH : sf.height;
    const UINT cw = sw - even(m_xf.crop[0], sw) - even(m_xf.crop[2], sw), ch = sh - even(m_xf.crop[1], sh) - even(m_xf.crop[3], sh);
    const UINT ow = (rot & 1) ? ch : cw, oh = (rot & 1) ? cw : ch;
    const float bw = u.S(110), bh = u.S(34), gap = u.S(8);
    const float total = 3 * bw + 2 * gap;
    const float by = std::min(pic.Bottom() + u.S(46), u.Height() - bh - u.S(12));
    const float bx = pic.x + pic.w / 2 - total / 2;
    const std::wstring info = Format(L"Drag the edges. Recorded and shared as %u×%u.", ow, oh);
    const float iw = u.Measure(info, u.S(13)) + u.S(24);
    const RectF infoR{pic.x + pic.w / 2 - iw / 2, by - u.S(34), iw, u.S(28)};   // just above the buttons (the top bar is above the picture)
    u.Fill(infoR, theme::Scrim, u.S(14));
    u.Text(info, infoR, u.S(13), theme::Text, Align::Center);
    if (u.Button("cropdone", {bx, by, bw, bh}, L"Done", 0, true)) EndCropEdit(true);
    if (u.Button("cropreset", {bx + bw + gap, by, bw, bh}, L"No crop")) {
        for (float& v : m_xf.crop) v = 0.0f;
        m_settingsDirty = true;
    }
    if (u.Button("cropcancel", {bx + 2 * (bw + gap), by, bw, bh}, L"Cancel")) EndCropEdit(false);
}

} // namespace cc
