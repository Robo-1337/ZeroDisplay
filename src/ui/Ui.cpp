#include "ui/Ui.h"
#include "ui/QrCode.h"

#include <d2d1_1helper.h>

#include <algorithm>
#include <cmath>

namespace cc {

static D2D1_RECT_F ToD2D(const RectF& r) { return D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h); }

namespace theme {
void Apply(bool dark)
{
    Dark = dark;
    if (dark) {
        Bar = {0.106f, 0.122f, 0.153f, 0.92f};
        Panel = {0.106f, 0.114f, 0.133f, 0.97f};
        Popup = {0.145f, 0.157f, 0.180f, 0.99f};
        Control = {1, 1, 1, 0.05f};
        ControlHover = {1, 1, 1, 0.10f};
        ControlActive = {1, 1, 1, 0.17f};
        Border = {1, 1, 1, 0.10f};
        Text = {0.94f, 0.95f, 0.97f, 1};
        TextDim = {0.706f, 0.729f, 0.769f, 1};
        Accent2 = {0.376f, 0.804f, 1.0f, 1};
        Card = {1, 1, 1, 0.04f};
        Scrim = {0, 0, 0, 0.72f};
        Good = {0.30f, 0.82f, 0.50f, 1};
        Warn = {1.0f, 0.74f, 0.28f, 1};
        Bad = {1.0f, 0.38f, 0.38f, 1};
    } else {
        Bar = {0.953f, 0.957f, 0.965f, 0.93f};
        Panel = {0.945f, 0.949f, 0.957f, 0.98f};
        Popup = {0.988f, 0.988f, 0.992f, 0.99f};
        Control = {0, 0, 0, 0.045f};
        ControlHover = {0, 0, 0, 0.08f};
        ControlActive = {0, 0, 0, 0.13f};
        Border = {0, 0, 0, 0.13f};
        Text = {0.10f, 0.11f, 0.12f, 1};
        TextDim = {0.36f, 0.38f, 0.42f, 1};
        Accent2 = {0.0f, 0.404f, 0.753f, 1};
        Card = {1, 1, 1, 0.80f};
        Scrim = {0.976f, 0.976f, 0.984f, 0.88f};
        Good = {0.05f, 0.52f, 0.24f, 1};
        Warn = {0.70f, 0.40f, 0.0f, 1};
        Bad = {0.80f, 0.16f, 0.16f, 1};
    }
    Accent = {0.0f, 0.471f, 0.843f, 1};
}

bool SystemDark()
{
    DWORD light = 0, size = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS)
        return true;   // no setting (older Windows): dark, as before
    return light == 0;
}
} // namespace theme

// A hover shade of a fill colour (towards white in the dark theme, towards black in the light one).
static Color Hover(Color c)
{
    const float t = 0.12f, to = theme::Dark ? 1.0f : 0.0f;
    return {c.r + (to - c.r) * t, c.g + (to - c.g) * t, c.b + (to - c.b) * t, c.a};
}
static bool SameColor(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

static bool FontExists(IDWriteFactory* dw, const wchar_t* family)
{
    ComPtr<IDWriteFontCollection> fonts;
    if (FAILED(dw->GetSystemFontCollection(&fonts))) return false;
    UINT32 index = 0;
    BOOL exists = FALSE;
    fonts->FindFamilyName(family, &index, &exists);
    return exists != FALSE;
}

bool Ui::Init(ID2D1DeviceContext* dc, IDWriteFactory* dw)
{
    m_dc = dc;
    m_dw = dw;
    m_formats.clear();
    m_ellipsis.clear();
    m_layouts.clear();
    m_brush.Reset();
    if (FAILED(m_dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &m_brush))) return false;
    ComPtr<ID2D1Factory> factory;
    m_dc->GetFactory(&factory);
    m_roundCaps.Reset();
    factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0,
                               &m_roundCaps);
    for (int i = 0; i < 2; ++i) {
        const float v = i ? 0.0f : 1.0f;
        D2D1_GRADIENT_STOP glow[2] = {{0, D2D1::ColorF(v, v, v, 1)}, {1, D2D1::ColorF(v, v, v, 0)}};
        ComPtr<ID2D1GradientStopCollection> stops;
        m_reveal[i].Reset();
        if (SUCCEEDED(m_dc->CreateGradientStopCollection(glow, 2, &stops)))
            m_dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({0, 0}, {0, 0}, 1, 1), stops.Get(), &m_reveal[i]);
        // The logo: light to deep blue on dark backgrounds, a touch deeper on light ones so the top stays visible.
        D2D1_GRADIENT_STOP blue[2] = {{0, D2D1::ColorF(i ? 0x2E9BF0 : 0x8BE1FF)}, {1, D2D1::ColorF(i ? 0x0058A8 : 0x0063B8)}};
        ComPtr<ID2D1GradientStopCollection> logoStops;
        m_logo[i].Reset();
        if (SUCCEEDED(m_dc->CreateGradientStopCollection(blue, 2, &logoStops)))
            m_dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {1, 1}), logoStops.Get(), &m_logo[i]);
    }
    if (FontExists(dw, L"Segoe Fluent Icons")) m_iconFont = L"Segoe Fluent Icons";
    if (FontExists(dw, L"Segoe UI Variable Display")) m_lightFont = L"Segoe UI Variable Display";
    if (FontExists(dw, L"Cascadia Mono")) m_monoFont = L"Cascadia Mono";
    return true;
}

uint64_t Ui::Id(const char* s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *s; ++s) h = (h ^ uint8_t(*s)) * 1099511628211ULL;
    return h | 1;
}

void Ui::Begin(const UiInput& in, float scale, float width, float height, double timeSec)
{
    m_in = in;
    m_scale = scale;
    m_w = width;
    m_h = height;
    m_time = timeSec;
    m_alpha = 1;
    ++m_frame;
    m_blocked = m_popupId != 0;
    m_wantsFrames = false;
    m_hoverSeen = false;
    m_focusSeen = false;
    m_headerSeen = m_headerClicked = false;
    m_tip.clear();
    if (m_activeId && !m_in.down) m_activeId = 0;
}

ID2D1SolidColorBrush* Ui::Brush(Color c)
{
    m_brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a * m_alpha));
    return m_brush.Get();
}

IDWriteTextFormat* Ui::Fmt(float size, bool bold, bool mono, bool icon, bool light)
{
    uint32_t key = uint32_t(std::lround(size * 4.0f)) << 4 | (bold ? 1u : 0u) | (mono ? 2u : 0u) | (icon ? 4u : 0u) | (light ? 8u : 0u);
    auto it = m_formats.find(key);
    if (it != m_formats.end()) return it->second.Get();
    const wchar_t* family = icon ? m_iconFont.c_str() : (mono ? m_monoFont.c_str() : (light ? m_lightFont.c_str() : L"Segoe UI"));
    ComPtr<IDWriteTextFormat> f;
    if (FAILED(m_dw->CreateTextFormat(family, nullptr,
                                      bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : (light ? DWRITE_FONT_WEIGHT_LIGHT : DWRITE_FONT_WEIGHT_NORMAL),
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f)))
        return nullptr;
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (!icon) {
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(m_dw->CreateEllipsisTrimmingSign(f.Get(), &sign))) {
            DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            f->SetTrimming(&t, sign.Get());
            m_ellipsis.push_back(sign);
        }
    }
    m_formats[key] = f;
    return f.Get();
}

void Ui::Fill(const RectF& r, Color c, float radius)
{
    if (radius > 0) m_dc->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(r), radius, radius), Brush(c));
    else m_dc->FillRectangle(ToD2D(r), Brush(c));
}

void Ui::Stroke(const RectF& r, Color c, float width, float radius)
{
    RectF i = r.Inset(width * 0.5f);
    if (radius > 0) m_dc->DrawRoundedRectangle(D2D1::RoundedRect(ToD2D(i), radius, radius), Brush(c), width);
    else m_dc->DrawRectangle(ToD2D(i), Brush(c), width);
}

IDWriteTextLayout* Ui::Layout(std::wstring_view s, float size, bool bold, bool mono, bool icon, float w, float h, Align align, bool wrap,
                              bool light)
{
    IDWriteTextFormat* f = Fmt(size, bold, mono, icon, light);
    if (!f) return nullptr;
    std::wstring key;
    key.reserve(s.size() + 48);
    key.append(s);
    key += Format(L"\x1F%d|%d|%d|%d|%d|%d|%d|%d|%d", int(std::lround(size * 4)), int(bold), int(mono), int(icon), int(std::lround(w)),
                  int(std::lround(h)), int(align), int(wrap), int(light));
    auto it = m_layouts.find(key);
    if (it != m_layouts.end()) {
        it->second.lastFrame = m_frame;
        return it->second.layout.Get();
    }
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(m_dw->CreateTextLayout(s.data(), UINT32(s.size()), f, w, h, &layout))) return nullptr;
    layout->SetTextAlignment(align == Align::Left ? DWRITE_TEXT_ALIGNMENT_LEADING
                             : align == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                      : DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (wrap) {
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    IDWriteTextLayout* raw = layout.Get();
    m_layouts[std::move(key)] = {layout, m_frame};
    return raw;
}

void Ui::Text(std::wstring_view s, const RectF& r, float size, Color c, Align align, bool bold, bool mono)
{
    if (s.empty() || r.w <= 0) return;
    if (IDWriteTextLayout* l = Layout(s, size, bold, mono, false, r.w, r.h, align, false))
        m_dc->DrawTextLayout(D2D1::Point2F(r.x, r.y), l, Brush(c), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Ui::TextBlock(std::wstring_view s, const RectF& r, float size, Color c, bool mono)
{
    if (s.empty()) return;
    if (IDWriteTextLayout* l = Layout(s, size, false, mono, false, r.w, r.h, Align::Left, true))
        m_dc->DrawTextLayout(D2D1::Point2F(r.x, r.y), l, Brush(c), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

float Ui::Measure(std::wstring_view s, float size, bool bold, bool mono)
{
    if (s.empty()) return 0;
    IDWriteTextLayout* l = Layout(s, size, bold, mono, false, 100000.0f, 1000.0f, Align::Left, false);
    if (!l) return 0;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

float Ui::MeasureBlock(std::wstring_view s, float width, float size)
{
    if (s.empty()) return 0;
    IDWriteTextLayout* l = Layout(s, size, false, false, false, width, 100000.0f, Align::Left, true);
    if (!l) return 0;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return std::ceil(m.height);
}

void Ui::Icon(wchar_t g, const RectF& r, float size, Color c)
{
    if (IDWriteTextLayout* l = Layout(std::wstring_view(&g, 1), size, false, false, true, r.w, r.h, Align::Center, false))
        m_dc->DrawTextLayout(D2D1::Point2F(r.x, r.y), l, Brush(c), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Ui::TextLight(std::wstring_view s, const RectF& r, float size, Color c, Align align)
{
    if (s.empty() || r.w <= 0) return;
    if (IDWriteTextLayout* l = Layout(s, size, false, false, false, r.w, r.h, align, false, true))
        m_dc->DrawTextLayout(D2D1::Point2F(r.x, r.y), l, Brush(c), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

float Ui::MeasureLight(std::wstring_view s, float size)
{
    if (s.empty()) return 0;
    IDWriteTextLayout* l = Layout(s, size, false, false, false, 100000.0f, 1000.0f, Align::Left, false, true);
    if (!l) return 0;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void Ui::Logo(const RectF& r)
{
    ID2D1LinearGradientBrush* blue = m_logo[theme::Dark ? 0 : 1].Get();
    if (!blue) return;
    const float s = std::min(r.w, r.h), u = s / 96.0f;
    const float x0 = r.x + (r.w - s) * 0.5f, y0 = r.y + (r.h - s) * 0.5f;
    auto at = [&](float x, float y) { return D2D1::Point2F(x0 + x * u, y0 + y * u); };
    blue->SetStartPoint(at(12, 12));
    blue->SetEndPoint(at(84, 84));
    blue->SetOpacity(m_alpha);
    // Small sizes get relatively thicker strokes so the retrace survives at 16 px.
    const float bar = std::max(13.0f * u, 2.2f), retrace = std::max(6.0f * u, 1.4f);
    m_dc->DrawLine(at(18, 22), at(78, 22), blue, bar, m_roundCaps.Get());
    m_dc->DrawLine(at(74, 27), at(22, 69), Brush(theme::Accent2.A(0.92f)), retrace, m_roundCaps.Get());
    m_dc->DrawLine(at(18, 74), at(78, 74), blue, bar, m_roundCaps.Get());
}

float Ui::MeasureWordmark(float size) { return Measure(L"Zero", size, true) + MeasureLight(L"Display", size); }

float Ui::Wordmark(const RectF& r, float size)
{
    const float zero = Measure(L"Zero", size, true);
    Text(L"Zero", {r.x, r.y, zero + S(2), r.h}, size, theme::Text, Align::Left, true);
    const float display = MeasureLight(L"Display", size);
    TextLight(L"Display", {r.x + zero, r.y, display + S(2), r.h}, size, theme::Text);
    return zero + display;
}

void Ui::RevealIf(const RectF& r, bool hot, float radius)
{
    ID2D1RadialGradientBrush* glow = m_reveal[theme::Dark ? 0 : 1].Get();
    if (!hot || !glow || r.w <= 0) return;
    const float reach = S(70), cr = S(radius);
    glow->SetCenter(D2D1::Point2F(m_in.mx, m_in.my));
    glow->SetRadiusX(reach);
    glow->SetRadiusY(reach);
    glow->SetOpacity((theme::Dark ? 0.13f : 0.06f) * m_alpha);
    if (cr > 0) m_dc->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(r), cr, cr), glow);
    else m_dc->FillRectangle(ToD2D(r), glow);
    glow->SetOpacity((theme::Dark ? 0.50f : 0.32f) * m_alpha);
    const RectF edge = r.Inset(S(0.5f));
    if (cr > 0) m_dc->DrawRoundedRectangle(D2D1::RoundedRect(ToD2D(edge), cr, cr), glow, S(1));
    else m_dc->DrawRectangle(ToD2D(edge), glow, S(1));
}

void Ui::PushClip(const RectF& r) { m_dc->PushAxisAlignedClip(ToD2D(r), D2D1_ANTIALIAS_MODE_ALIASED); }
void Ui::PopClip() { m_dc->PopAxisAlignedClip(); }

bool Ui::Hot(const RectF& r) const { return !m_blocked && m_in.mouseIn && r.Contains(m_in.mx, m_in.my) && (m_activeId == 0); }

void Ui::TrackHover(uint64_t id, const RectF& r, std::wstring_view tip)
{
    if (!Hot(r)) return;
    m_hoverSeen = true;
    if (m_hoverId != id) {
        m_hoverId = id;
        m_hoverStart = m_time;
    }
    if (!tip.empty()) {
        if (m_time - m_hoverStart > 0.5) {
            m_tip = tip;
            m_tipAnchor = r;
        } else {
            m_wantsFrames = true;
        }
    }
}

// Greys out what is drawn while it lives (disabled widgets).
struct AlphaScope {
    float& a;
    float saved;
    AlphaScope(float& alpha, bool dim) : a(alpha), saved(alpha) { if (dim) a *= 0.4f; }
    ~AlphaScope() { a = saved; }
};

bool Ui::Button(const char* idStr, const RectF& r, std::wstring_view label, wchar_t icon, bool primary, bool disabled)
{
    AlphaScope dim(m_alpha, disabled);
    uint64_t id = Id(idStr);
    if (!disabled) TrackHover(id, r, L"");
    bool hot = !disabled && Hot(r);
    const float rad = S(theme::Radius);
    if (primary) {
        Fill(r, hot ? Hover(theme::Accent) : theme::Accent, rad);
    } else {
        Fill(r, hot ? theme::ControlHover : theme::Control, rad);
        Stroke(r, theme::Border, S(1), rad);
    }
    RevealIf(r, hot, theme::Radius);
    const Color fg = primary ? theme::OnAccent : theme::Text;
    float x = r.x;
    float total = Measure(label, S(13)) + (icon ? S(22) : 0);
    x = r.x + std::max(S(10), (r.w - total) * 0.5f);
    if (icon) {
        Icon(icon, {x, r.y, S(18), r.h}, S(14), fg);
        x += S(22);
    }
    Text(label, {x, r.y, r.Right() - x - S(6), r.h}, S(13), fg);
    return !disabled && Pressed(r);
}

bool Ui::IconButton(const char* idStr, const RectF& r, wchar_t icon, std::wstring_view tip, bool active, Color iconColor, bool disabled)
{
    uint64_t id = Id(idStr);
    TrackHover(id, r, tip);   // the tip still shows: it says why the button is disabled
    AlphaScope dim(m_alpha, disabled);
    bool hot = !disabled && Hot(r);
    const float rad = S(theme::Radius);
    // On: solid accent with a white icon, like Windows 10. A button with its own status colour (paused: amber) keeps
    // it, on a tint of that colour, so the state reads at a glance.
    const bool status = !SameColor(iconColor, theme::Text);
    if (active && status) Fill(r, iconColor.A(hot ? 0.32f : 0.22f), rad);
    else if (active) Fill(r, hot ? Hover(theme::Accent) : theme::Accent, rad);
    else if (hot) Fill(r, theme::ControlHover, rad);
    RevealIf(r, hot, theme::Radius);
    Icon(icon, r, S(16), active && !status ? theme::OnAccent : iconColor);
    return !disabled && Pressed(r);
}

bool Ui::ResetIcon(const char* idStr, float x, float y, float h)
{
    const RectF r{x, y + (h - S(20)) * 0.5f, S(20), S(20)};
    const uint64_t id = Id(idStr) ^ 0x5EEDu;
    TrackHover(id, r, L"Back to the default");
    const bool hot = Hot(r);
    if (hot) Fill(r, theme::ControlHover, S(theme::Radius));
    Icon(glyph::Undo, r, S(11), hot ? theme::Text : theme::TextDim);
    return Pressed(r);
}

bool Ui::Toggle(const char* idStr, const RectF& r, std::wstring_view label, bool& value, bool disabled, int resetTo)
{
    AlphaScope dim(m_alpha, disabled);
    uint64_t id = Id(idStr);
    if (!disabled && resetTo >= 0 && int(value) != resetTo) {   // changed from the default: the reset icon after the label
        const float x = std::min(r.x + Measure(label, S(13)) + S(4), r.Right() - S(40) - S(30));
        std::string rid = std::string(idStr) + "#reset";
        if (ResetIcon(rid.c_str(), x, r.y, r.h)) {
            value = resetTo != 0;
            return true;
        }
    }
    if (!disabled) TrackHover(id, r, L"");
    bool hot = !disabled && Hot(r);
    const float sw = S(40), sh = S(20);
    RectF track{r.Right() - sw, r.y + (r.h - sh) * 0.5f, sw, sh};
    Text(label, {r.x, r.y, r.w - sw - S(8), r.h}, S(13), theme::Text);
    // Windows 10 switch: an outlined pill when off, solid accent when on, a small round knob.
    if (value) {
        Fill(track, hot ? Hover(theme::Accent) : theme::Accent, sh * 0.5f);
    } else {
        if (hot) Fill(track, theme::ControlHover, sh * 0.5f);
        Stroke(track, theme::Text.A(0.85f), S(2), sh * 0.5f);
    }
    const float k = S(10);
    const float kx = value ? track.Right() - S(5) - k : track.x + S(5);
    Fill({kx, track.y + (sh - k) * 0.5f, k, k}, value ? theme::OnAccent : theme::Text, k * 0.5f);
    if (!disabled && Pressed(r)) {
        value = !value;
        return true;
    }
    return false;
}

bool Ui::SectionHeader(const char* idStr, const RectF& r, std::wstring_view title, bool& open)
{
    const uint64_t id = Id(idStr);
    m_headerSeen |= m_headerFocus == id;
    const bool hot = Hot(r);
    bool flip = Pressed(r);
    if (flip) {
        m_headerFocus = id;
        m_headerClicked = m_headerSeen = true;
    }
    if (!flip && m_focusId == 0 && !m_blocked && (m_headerFocus == id || (m_headerFocus == 0 && hot)))
        for (UINT k : m_in.keys)
            if (k == VK_RETURN || k == VK_SPACE) flip = !flip;
    const float rad = S(theme::Radius);
    if (hot) Fill(r, theme::ControlHover.A(theme::ControlHover.a * 0.5f), rad);
    RevealIf(r, hot, theme::Radius);
    if (m_headerFocus == id) Stroke(r, theme::Accent2.A(0.6f), 1, rad);
    // A short marker bar, then the title in plain sentence case (Windows Settings style); the chevron at the right end.
    Fill({r.x + S(4), r.y + (r.h - S(14)) * 0.5f, S(3), S(14)}, theme::Accent2, S(1.5f));
    Text(title, {r.x + S(14), r.y, r.w - S(38), r.h}, S(15), theme::Text);
    Icon(open ? glyph::ChevronDown : glyph::ChevronRight, {r.Right() - S(22), r.y, S(16), r.h}, S(10), hot ? theme::Text : theme::TextDim);
    if (flip) open = !open;
    return flip;
}

bool Ui::Slider(const char* idStr, const RectF& r, float& value, float lo, float hi, bool wheel)
{
    uint64_t id = Id(idStr);
    bool changed = false;
    if (Pressed(r)) m_activeId = id;
    bool hot = Hot(r) || m_activeId == id;
    if (m_activeId == id && m_in.down) {
        float t = Clamp((m_in.mx - r.x) / std::max(1.0f, r.w), 0.0f, 1.0f);
        float nv = lo + t * (hi - lo);
        if (nv != value) {
            value = nv;
            changed = true;
        }
        m_wantsFrames = true;
    }
    if (wheel && Hot(r) && m_in.wheel != 0) {
        value = Clamp(value + m_in.wheel * (hi - lo) * 0.05f, lo, hi);
        changed = true;
    }
    const float th = S(4);
    RectF track{r.x, r.y + (r.h - th) * 0.5f, r.w, th};
    Fill(track, theme::ControlActive, th * 0.5f);
    float t = (hi > lo) ? Clamp((value - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
    Fill({track.x, track.y, track.w * t, th}, theme::Accent2, th * 0.5f);
    // Windows 10 thumb: a tall rounded bar.
    const float hw = S(8), hh = std::min(r.h, hot ? S(20) : S(18));
    Fill({r.x + r.w * t - hw * 0.5f, r.y + (r.h - hh) * 0.5f, hw, hh}, hot ? Hover(theme::Accent2) : theme::Accent2, S(4));
    return changed;
}

bool Ui::Segmented(const char* idStr, const RectF& r, const std::vector<std::wstring>& items, int& selected, uint32_t disabled)
{
    (void)idStr;
    if (items.empty()) return false;
    const float rad = S(theme::Radius);
    Fill(r, theme::Control.A(theme::Control.a * 0.6f), rad);
    Stroke(r, theme::Border, S(1), rad);
    const float w = r.w / float(items.size());
    bool changed = false;
    for (size_t i = 0; i < items.size(); ++i) {
        RectF cell{r.x + w * float(i), r.y, w, r.h};
        bool sel = int(i) == selected;
        const bool off = i < 32 && (disabled >> i & 1u);   // shown greyed out, can't be picked
        const bool hot = !off && Hot(cell);
        // Selected: a light fill and an underline across the whole option.
        if (sel) {
            Fill(cell.Inset(S(1)), theme::ControlActive.A(theme::ControlActive.a * 0.6f));
            Fill({cell.x + S(1), cell.Bottom() - S(3), cell.w - S(2), S(2)}, theme::Accent2);
        } else if (hot) {
            Fill(cell.Inset(S(1)), theme::ControlHover);
        }
        RevealIf(cell.Inset(S(1)), hot, 0);
        Text(items[i], cell, S(12.5f), sel ? theme::Text : (off ? theme::TextDim.A(0.4f) : theme::TextDim), Align::Center, sel);
        if (!off && Pressed(cell) && !sel) {
            selected = int(i);
            changed = true;
        }
    }
    return changed;
}

static void RgbToHsv(uint32_t argb, float& h, float& s, float& v)
{
    const float r = ((argb >> 16) & 0xFF) / 255.0f, g = ((argb >> 8) & 0xFF) / 255.0f, b = (argb & 0xFF) / 255.0f;
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    v = mx;
    s = mx > 0 ? d / mx : 0;
    if (d <= 0) h = 0;
    else if (mx == r) h = 60.0f * std::fmod((g - b) / d + 6.0f, 6.0f);
    else if (mx == g) h = 60.0f * ((b - r) / d + 2.0f);
    else h = 60.0f * ((r - g) / d + 4.0f);
}

static uint32_t HsvToRgb(float h, float s, float v)
{
    const float c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1)), m = v - c;
    float r = 0, g = 0, b = 0;
    if (h < 60) r = c, g = x;
    else if (h < 120) r = x, g = c;
    else if (h < 180) g = c, b = x;
    else if (h < 240) g = x, b = c;
    else if (h < 300) r = x, b = c;
    else r = c, b = x;
    auto q = [](float f) { return uint32_t(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f)); };
    return (q(r + m) << 16) | (q(g + m) << 8) | q(b + m);
}

static Color ColorOf(uint32_t argb) { return {((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f, 1.0f}; }

bool Ui::ColorPicker(const char* idStr, const RectF& r, uint32_t& argb)
{
    const uint64_t id = Id(idStr), idSv = id ^ 0x53560000ull, idHue = id ^ 0x48550000ull;
    float h = 0, s = 0, v = 0;
    RgbToHsv(argb, h, s, v);
    auto known = m_pickerHue.find(id);
    if (known != m_pickerHue.end() && (s < 0.004f || v < 0.004f)) h = known->second;   // grey/black: keep the hue
    const uint32_t alpha = argb & 0xFF000000u;
    bool changed = false;
    float y = r.y;

    static const uint32_t presets[] = {0xFFFFFF, 0x101010, 0x1E2430, 0xFF5A5A, 0xFF9F1C, 0xFFD84A, 0x00B140, 0x4AD8FF, 0x0047BB, 0xC04CFF};
    const int n = int(std::size(presets));
    const float gap = S(4), sw = (r.w - gap * float(n - 1)) / float(n);
    for (int i = 0; i < n; ++i) {
        const RectF c{r.x + float(i) * (sw + gap), y, sw, S(22)};
        Fill(c, ColorOf(presets[i]), S(4));
        if ((argb & 0xFFFFFF) == presets[i]) Stroke(c.Inset(-S(2)), theme::Accent2, S(2), S(5));
        else if (Hot(c)) Stroke(c, theme::Text.A(0.5f), S(1), S(4));
        if (Pressed(c) && (argb & 0xFFFFFF) != presets[i]) {
            argb = alpha | presets[i];
            RgbToHsv(argb, h, s, v);
            changed = true;
        }
    }
    y += S(22 + 8);

    // Saturation (left -> right) / brightness (top -> bottom) for the current hue.
    const RectF sv{r.x, y, r.w, S(110)};
    if (!m_blocked && Pressed(sv)) m_activeId = idSv;
    if (m_activeId == idSv && m_in.down) {
        s = std::clamp((m_in.mx - sv.x) / std::max(1.0f, sv.w), 0.0f, 1.0f);
        v = 1.0f - std::clamp((m_in.my - sv.y) / std::max(1.0f, sv.h), 0.0f, 1.0f);
        argb = alpha | HsvToRgb(h, s, v);
        changed = true;
        m_wantsFrames = true;
    }
    Fill(sv, ColorOf(HsvToRgb(h, 1, 1)), S(4));
    auto gradient = [&](const RectF& g, std::initializer_list<D2D1_GRADIENT_STOP> stops, bool vertical) {
        ComPtr<ID2D1GradientStopCollection> col;
        ComPtr<ID2D1LinearGradientBrush> br;
        if (FAILED(m_dc->CreateGradientStopCollection(stops.begin(), UINT32(stops.size()), &col))) return;
        const D2D1_POINT_2F a = D2D1::Point2F(g.x, g.y), b = vertical ? D2D1::Point2F(g.x, g.y + g.h) : D2D1::Point2F(g.x + g.w, g.y);
        if (FAILED(m_dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), col.Get(), &br))) return;
        br->SetOpacity(m_alpha);
        m_dc->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(g), S(4), S(4)), br.Get());
    };
    gradient(sv, {{0, D2D1::ColorF(1, 1, 1, 1)}, {1, D2D1::ColorF(1, 1, 1, 0)}}, false);
    gradient(sv, {{0, D2D1::ColorF(0, 0, 0, 0)}, {1, D2D1::ColorF(0, 0, 0, 1)}}, true);
    {
        const float k = S(12), mx = sv.x + s * sv.w, my = sv.y + (1 - v) * sv.h;
        Stroke({mx - k / 2, my - k / 2, k, k}, v > 0.5f ? Color{0, 0, 0, 0.8f} : Color{1, 1, 1, 0.9f}, S(2), k / 2);
    }
    y += S(110 + 8);

    const RectF hb{r.x, y, r.w, S(14)};
    if (!m_blocked && Pressed(hb)) m_activeId = idHue;
    if (m_activeId == idHue && m_in.down) {
        h = std::clamp((m_in.mx - hb.x) / std::max(1.0f, hb.w), 0.0f, 0.9999f) * 360.0f;
        if (s < 0.004f && v < 0.004f) s = v = 1.0f;   // picking a hue from black means "show me the colour"
        argb = alpha | HsvToRgb(h, s, v);
        changed = true;
        m_wantsFrames = true;
    }
    gradient(hb, {{0.0f, D2D1::ColorF(1, 0, 0)}, {1 / 6.0f, D2D1::ColorF(1, 1, 0)}, {2 / 6.0f, D2D1::ColorF(0, 1, 0)}, {3 / 6.0f, D2D1::ColorF(0, 1, 1)},
                  {4 / 6.0f, D2D1::ColorF(0, 0, 1)}, {5 / 6.0f, D2D1::ColorF(1, 0, 1)}, {1.0f, D2D1::ColorF(1, 0, 0)}},
             false);
    {
        const float mx = hb.x + h / 360.0f * hb.w;
        Stroke({mx - S(3), hb.y - S(2), S(6), hb.h + S(4)}, theme::Text, S(2), S(2));
    }
    y += S(14 + 10);

    // #RRGGBB text box + preview.
    const std::string hexIdStr = std::string(idStr) + "#hex";
    const uint64_t hexId = Id(hexIdStr.c_str());
    std::wstring& hex = m_pickerHex[id];
    if (m_focusId != hexId) hex = Format(L"#%06X", argb & 0xFFFFFF);
    if (TextBox(hexIdStr.c_str(), {r.x, y, r.w * 0.55f, S(30)}, hex, L"#RRGGBB")) {
        std::wstring t = hex;
        if (!t.empty() && t[0] == L'#') t.erase(0, 1);
        if (t.size() == 6 && t.find_first_not_of(L"0123456789abcdefABCDEF") == std::wstring::npos) {
            argb = alpha | uint32_t(wcstoul(t.c_str(), nullptr, 16));
            RgbToHsv(argb, h, s, v);
            changed = true;
        }
    }
    Fill({r.x + r.w * 0.6f, y, r.w * 0.4f, S(30)}, ColorOf(argb), S(6));
    Stroke({r.x + r.w * 0.6f, y, r.w * 0.4f, S(30)}, theme::Border, S(1), S(6));
    m_pickerHue[id] = h;
    return changed;
}

void Ui::Qr(const QrCode& code, const RectF& r)
{
    if (code.size <= 0) return;
    const float side = std::min(r.w, r.h);
    const int modules = code.size + 8;
    const float m = std::floor(side / float(modules));   // whole pixels per module: crisp edges
    if (m < 1) return;
    const float total = m * float(modules), ox = std::floor(r.x + (r.w - total) / 2), oy = std::floor(r.y + (r.h - total) / 2);
    Fill({ox, oy, total, total}, Color{1, 1, 1, 1}, S(4));
    for (int y = 0; y < code.size; ++y)
        for (int x = 0; x < code.size;) {
            if (!code.Dark(x, y)) {
                ++x;
                continue;
            }
            int run = 1;   // one rectangle per horizontal run of dark modules
            while (x + run < code.size && code.Dark(x + run, y)) ++run;
            Fill({ox + float(x + 4) * m, oy + float(y + 4) * m, float(run) * m, m}, Color{0, 0, 0, 1});
            x += run;
        }
}

bool Ui::TextBox(const char* idStr, const RectF& r, std::wstring& text, std::wstring_view placeholder, bool masked)
{
    const uint64_t id = Id(idStr);
    bool focused = m_focusId == id;
    bool committed = false;
    if (!focused && Pressed(r)) {
        m_focusId = id;
        focused = true;
    } else if (focused && m_in.pressed && !r.Contains(m_in.mx, m_in.my)) {
        m_focusId = 0;   // clicked elsewhere: keep the text
        focused = false;
        committed = true;
    }
    if (focused) {
        m_focusSeen = true;
        for (wchar_t ch : m_in.chars)
            if (ch >= 32 && ch != 127 && text.size() < 256) text += ch;
        for (UINT k : m_in.keys) {
            if (k == VK_BACK && !text.empty()) text.pop_back();
            if (k == VK_RETURN || k == VK_ESCAPE || k == VK_TAB) {
                m_focusId = 0;
                focused = false;
                committed = true;
            }
        }
        m_wantsFrames = true;
    }
    const float rad = S(theme::Radius);
    Fill(r, focused ? theme::ControlActive : (Hot(r) ? theme::ControlHover : theme::Control), rad);
    Stroke(r, theme::Border, S(1), rad);
    if (focused) Fill({r.x, r.Bottom() - S(2), r.w, S(2)}, theme::Accent2);   // typing: an accent underline
    else Fill({r.x + S(1), r.Bottom() - S(1), r.w - S(2), S(1)}, theme::Text.A(0.22f));
    RectF tr{r.x + S(10), r.y, r.w - S(20), r.h};
    if (text.empty() && !focused) {
        Text(placeholder, tr, S(13), theme::TextDim);
    } else {
        // Show the end of long text while typing.
        const std::wstring display = masked ? std::wstring(text.size(), L'•') : text;
        float tw = Measure(display, S(13));
        PushClip(tr);
        RectF shown{tw > tr.w && focused ? tr.Right() - tw - S(4) : tr.x, r.y, std::max(tw + S(8), tr.w), r.h};
        Text(display, shown, S(13), theme::Text);
        if (focused && int(m_time * 2) % 2 == 0) Fill({std::min(shown.x + tw + S(1), tr.Right() - S(1)), r.y + r.h * 0.22f, S(1.5f), r.h * 0.56f}, theme::Text);
        PopClip();
    }
    return committed;
}

bool Ui::Dropdown(const char* idStr, const RectF& r, const std::vector<std::wstring>& items, int& selected, wchar_t icon,
                  std::wstring_view placeholder)
{
    uint64_t id = Id(idStr);
    TrackHover(id, r, L"");
    bool open = m_popupId == id;
    bool hot = Hot(r);
    const float rad = S(theme::Radius);
    Fill(r, open ? theme::ControlActive : (hot ? theme::ControlHover : theme::Control), rad);
    Stroke(r, theme::Border, S(1), rad);
    if (open) Fill({r.x, r.Bottom() - S(2), r.w, S(2)}, theme::Accent2);   // open: an accent underline
    else Fill({r.x + S(1), r.Bottom() - S(1), r.w - S(2), S(1)}, theme::Text.A(0.22f));
    RevealIf(r, hot, theme::Radius);
    float x = r.x + S(10);
    if (icon) {
        Icon(icon, {x, r.y, S(16), r.h}, S(14), theme::TextDim);
        x += S(22);
    }
    std::wstring_view label = (selected >= 0 && selected < int(items.size())) ? std::wstring_view(items[size_t(selected)]) : placeholder;
    Text(label, {x, r.y, r.Right() - x - S(26), r.h}, S(13), theme::Text);
    Icon(glyph::ChevronDown, {r.Right() - S(24), r.y, S(16), r.h}, S(10), theme::TextDim);

    bool changed = false;
    if (m_resultFor == id) {
        if (m_result >= 0 && m_result != selected) {
            selected = m_result;
            changed = true;
        }
        m_resultFor = 0;
        m_result = -1;
    }
    if (!open && Pressed(r) && !items.empty()) {
        m_popupId = id;
        m_popupJustOpened = true;
        m_popupAnchor = r;
        m_popupItems = items;
        m_popupSel = selected;
        // Long lists (fonts) open with the current choice in view.
        m_popupScroll = selected > 3 ? float(selected - 3) * S(32) : 0.0f;
    }
    if (open) m_popupItems = items;   // keep in sync if the list changes while open
    return changed;
}

void Ui::DrawPopup()
{
    if (!m_popupId) return;
    const float itemH = S(32);
    float maxText = 0;
    for (const auto& it : m_popupItems) maxText = std::max(maxText, Measure(it, S(13)));
    float w = std::min(std::max(m_popupAnchor.w, maxText + S(48)), m_w - S(16));
    float x = Clamp(m_popupAnchor.x, S(8), m_w - w - S(8));
    float full = itemH * float(m_popupItems.size()) + S(8);
    float below = m_h - m_popupAnchor.Bottom() - S(12);
    float above = m_popupAnchor.y - S(12);
    float h, y;
    if (full <= below || below >= above) {
        h = std::min(full, below);
        y = m_popupAnchor.Bottom() + S(4);
    } else {
        h = std::min(full, above);
        y = m_popupAnchor.y - S(4) - h;
    }
    RectF rect{x, y, w, std::max(h, itemH)};
    const float maxScroll = std::max(0.0f, full - rect.h);

    bool inside = m_in.mouseIn && rect.Contains(m_in.mx, m_in.my);
    if (inside && m_in.wheel != 0) m_popupScroll = Clamp(m_popupScroll - m_in.wheel * itemH * 2, 0.0f, maxScroll);

    Fill(rect.Inset(-S(1)), theme::Border, S(4));
    Fill(rect, theme::Popup, S(4));
    PushClip(rect);
    for (size_t i = 0; i < m_popupItems.size(); ++i) {
        RectF item{rect.x + S(4), rect.y + S(4) + itemH * float(i) - m_popupScroll, rect.w - S(8), itemH};
        if (item.Bottom() < rect.y || item.y > rect.Bottom()) continue;
        bool hot = m_in.mouseIn && item.Contains(m_in.mx, m_in.my) && inside;
        const float rad = S(theme::Radius);
        if (int(i) == m_popupSel) {   // the current choice: a light fill and a short accent pill (Windows 11 menus)
            Fill(item, theme::Control, rad);
            Fill({item.x + S(1), item.y + (item.h - S(16)) * 0.5f, S(3), S(16)}, theme::Accent2, S(1.5f));
        }
        if (hot) Fill(item, theme::ControlHover, rad);
        RevealIf(item, hot, theme::Radius);
        Text(m_popupItems[i], {item.x + S(14), item.y, item.w - S(20), item.h}, S(13), theme::Text);
        if (hot && m_in.pressed) {
            m_resultFor = m_popupId;
            m_result = int(i);
        }
    }
    PopClip();

    bool close = false;
    if (m_resultFor == m_popupId) close = true;
    else if (m_in.pressed && !inside && !m_popupJustOpened) close = true;
    for (UINT k : m_in.keys)
        if (k == VK_ESCAPE) close = true;
    if (close) m_popupId = 0;
    m_popupJustOpened = false;
    m_wantsFrames = true;
}

void Ui::End()
{
    m_alpha = 1;
    DrawPopup();
    if (!m_tip.empty()) {
        float tw = Measure(m_tip, S(12)) + S(16);
        float tx = Clamp(m_tipAnchor.x + m_tipAnchor.w * 0.5f - tw * 0.5f, S(4), m_w - tw - S(4));
        float ty = m_tipAnchor.Bottom() + S(6);
        if (ty + S(26) > m_h) ty = m_tipAnchor.y - S(32);
        RectF r{tx, ty, tw, S(26)};
        Fill(r, theme::Popup, S(theme::Radius + 1));
        Stroke(r, theme::Border, 1, S(theme::Radius + 1));
        Text(m_tip, r, S(12), theme::Text, Align::Center);
    }
    if (!m_hoverSeen) m_hoverId = 0;
    if (!m_focusSeen) m_focusId = 0;   // the focused text box wasn't drawn this frame
    // A click anywhere else, or the focused header not being drawn (panel closed / collapsed away), ends its focus.
    if (!m_headerSeen || (m_in.pressed && !m_headerClicked)) m_headerFocus = 0;
    if (m_resultFor) m_wantsFrames = true;
    // Drop layouts that haven't been drawn for a couple of seconds (changing numbers, old labels).
    if (m_frame % 120 == 0) std::erase_if(m_layouts, [&](const auto& kv) { return m_frame - kv.second.lastFrame > 120; });
}

} // namespace cc
