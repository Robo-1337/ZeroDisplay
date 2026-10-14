#pragma once
#include "core/Common.h"

#include <d2d1_1.h>
#include <dwrite.h>

#include <map>
#include <unordered_map>

namespace cc {

struct Color {
    float r, g, b, a;
    Color A(float na) const { return {r, g, b, na}; }
};

namespace theme {
// Hybrid of Windows 10 and 11: solid #0078D7 for what is on (Win10), a lighter blue for markers (Win11).
// Dark values here; Apply() switches the set to light or dark (it follows Windows unless the user picks one).
inline bool Dark = true;
inline Color Bar{0.106f, 0.122f, 0.153f, 0.92f};
inline Color Panel{0.106f, 0.114f, 0.133f, 0.97f};
inline Color Popup{0.145f, 0.157f, 0.180f, 0.99f};
inline Color Control{1, 1, 1, 0.05f};
inline Color ControlHover{1, 1, 1, 0.10f};
inline Color ControlActive{1, 1, 1, 0.17f};
inline Color Border{1, 1, 1, 0.10f};
inline Color Text{0.94f, 0.95f, 0.97f, 1};
inline Color TextDim{0.706f, 0.729f, 0.769f, 1};
inline Color Accent{0.0f, 0.471f, 0.843f, 1};    // #0078D7: switched on, primary (white text on it)
inline Color Accent2{0.376f, 0.804f, 1.0f, 1};   // markers and accent text: #60CDFF dark, #0067C0 light
inline Color Card{1, 1, 1, 0.04f};               // a setting's card in the panel
inline Color Scrim{0, 0, 0, 0.72f};              // behind text drawn over the picture (statistics, status pills)
inline Color Good{0.30f, 0.82f, 0.50f, 1};
inline Color Warn{1.0f, 0.74f, 0.28f, 1};
inline Color Bad{1.0f, 0.38f, 0.38f, 1};
inline constexpr Color OnAccent{1, 1, 1, 1};     // text and icons on Accent
// Text straight on the black canvas around the pictures (studio labels): the dark theme's colours in both themes.
inline constexpr Color OnBlack{0.94f, 0.95f, 0.97f, 1};
inline constexpr Color OnBlackDim{0.706f, 0.729f, 0.769f, 1};
inline constexpr Color OnBlackAccent{0.376f, 0.804f, 1.0f, 1};
inline constexpr Color OnBlackBad{1.0f, 0.38f, 0.38f, 1};
inline constexpr float Radius = 2;   // control corners (unscaled px)
void Apply(bool dark);
// Windows' app mode (Settings > Personalization > Colors): true = dark.
bool SystemDark();
} // namespace theme

// Segoe Fluent Icons / Segoe MDL2 Assets code points
namespace glyph {
inline constexpr wchar_t Video = 0xE714;
inline constexpr wchar_t Camera = 0xE722;
inline constexpr wchar_t Volume = 0xE767;
inline constexpr wchar_t Mute = 0xE74F;
inline constexpr wchar_t Settings = 0xE713;
inline constexpr wchar_t FullScreen = 0xE740;
inline constexpr wchar_t BackToWindow = 0xE73F;
inline constexpr wchar_t Grid = 0xECA5;
inline constexpr wchar_t Microphone = 0xE720;
inline constexpr wchar_t Speaker = 0xE7F5;
inline constexpr wchar_t ChevronDown = 0xE70D;
inline constexpr wchar_t ChevronRight = 0xE76C;
inline constexpr wchar_t Check = 0xE73E;
inline constexpr wchar_t Close = 0xE711;
inline constexpr wchar_t Info = 0xE946;
inline constexpr wchar_t Warning = 0xE7BA;
inline constexpr wchar_t Plug = 0xE88E;
inline constexpr wchar_t Stats = 0xE9D9;
inline constexpr wchar_t Refresh = 0xE72C;
inline constexpr wchar_t Record = 0xE7C8;
inline constexpr wchar_t Stop = 0xE71A;
inline constexpr wchar_t Folder = 0xE838;
inline constexpr wchar_t Share = 0xE72D;
inline constexpr wchar_t Eye = 0xE890;
inline constexpr wchar_t EyeOff = 0xED1A;
inline constexpr wchar_t Add = 0xE710;
inline constexpr wchar_t Delete = 0xE74D;
inline constexpr wchar_t ChevronUp = 0xE70E;
inline constexpr wchar_t Picture = 0xE8B9;
inline constexpr wchar_t Font = 0xE8D2;
inline constexpr wchar_t Color = 0xE790;
inline constexpr wchar_t History = 0xE81C;
inline constexpr wchar_t Play = 0xE768;
inline constexpr wchar_t Pause = 0xE769;
inline constexpr wchar_t Back = 0xEB9E;      // rewind
inline constexpr wchar_t Forward = 0xEB9D;   // fast forward
inline constexpr wchar_t Cut = 0xE8C6;
inline constexpr wchar_t PopOut = 0xE8A7;   // open in new window
inline constexpr wchar_t Undo = 0xE7A7;     // reset to default
inline constexpr wchar_t MicOff = 0xEC54;
inline constexpr wchar_t Help = 0xE897;
inline constexpr wchar_t Sun = 0xE706;      // light theme
inline constexpr wchar_t Moon = 0xE708;     // dark theme
inline constexpr wchar_t System = 0xE770;   // follow Windows
} // namespace glyph

struct UiInput {
    float mx = -1, my = -1;
    bool mouseIn = false;
    bool down = false;       // left button held
    bool pressed = false;    // went down this frame
    bool released = false;
    bool doubleClick = false;
    float wheel = 0;
    std::vector<UINT> keys;
    std::wstring chars;      // typed characters (WM_CHAR)
};

enum class Align { Left, Center, Right };

// Minimal immediate-mode UI drawn with Direct2D. Coordinates are physical pixels.
class Ui {
public:
    bool Init(ID2D1DeviceContext* dc, IDWriteFactory* dw);
    void Begin(const UiInput& in, float scale, float width, float height, double timeSec);
    void End();

    float S(float v) const { return v * m_scale; }
    float Width() const { return m_w; }
    float Height() const { return m_h; }
    const UiInput& Input() const { return m_in; }
    void SetAlpha(float a) { m_alpha = a; }

    // primitives
    void Fill(const RectF& r, Color c, float radius = 0);
    void Stroke(const RectF& r, Color c, float width, float radius = 0);
    void Text(std::wstring_view s, const RectF& r, float size, Color c, Align align = Align::Left, bool bold = false, bool mono = false);
    void TextBlock(std::wstring_view s, const RectF& r, float size, Color c, bool mono = false);   // wraps, top aligned
    float Measure(std::wstring_view s, float size, bool bold = false, bool mono = false);
    float MeasureBlock(std::wstring_view s, float width, float size);   // height of TextBlock's wrapped text
    void Icon(wchar_t g, const RectF& r, float size, Color c);
    // Light weight (Segoe UI Variable Display when installed): big titles and the "Display" half of the wordmark.
    void TextLight(std::wstring_view s, const RectF& r, float size, Color c, Align align = Align::Left);
    float MeasureLight(std::wstring_view s, float size);
    // The Raster Z mark (a display's scan path: across, diagonal retrace, across), fitted into `r` (square).
    void Logo(const RectF& r);
    // "ZeroDisplay" ("Zero" semibold, "Display" light), left aligned and centred vertically in `r`. Returns its width.
    float Wordmark(const RectF& r, float size);
    float MeasureWordmark(float size);
    // Hover light that follows the mouse (Windows 10 "Reveal"): a soft glow and a lit edge while `r` is hot.
    void Reveal(const RectF& r, float radius = theme::Radius) { RevealIf(r, Hot(r), radius); }
    void PushClip(const RectF& r);
    void PopClip();

    // widgets (return true when the value changed / button clicked)
    // `disabled`: drawn greyed out and ignores the mouse (say why next to it, never offer what can't work).
    bool Button(const char* id, const RectF& r, std::wstring_view label, wchar_t icon = 0, bool primary = false, bool disabled = false);
    bool IconButton(const char* id, const RectF& r, wchar_t icon, std::wstring_view tip, bool active = false, Color iconColor = theme::Text,
                    bool disabled = false);
    // `resetTo` (0 / 1): the default; when the value differs, a small reset icon after the label puts it back.
    bool Toggle(const char* id, const RectF& r, std::wstring_view label, bool& value, bool disabled = false, int resetTo = -1);
    // A small "back to the default" icon (shown next to a changed setting). Returns true when clicked.
    bool ResetIcon(const char* id, float x, float y, float h);
    // Collapsible section header (accent title + chevron). Click, or Enter / Space while it is focused (clicked
    // last) or under the mouse, flips `open`. Returns true when it was flipped.
    bool SectionHeader(const char* id, const RectF& r, std::wstring_view title, bool& open);
    bool HeaderFocused() const { return m_headerFocus != 0; }
    // `wheel`: the mouse wheel adjusts it too. Off by default: sliders in scrolling panels must not change
    // while the user scrolls past them.
    bool Slider(const char* id, const RectF& r, float& value, float lo, float hi, bool wheel = false);
    bool Dropdown(const char* id, const RectF& r, const std::vector<std::wstring>& items, int& selected, wchar_t icon = 0,
                  std::wstring_view placeholder = L"");
    bool Segmented(const char* id, const RectF& r, const std::vector<std::wstring>& items, int& selected, uint32_t disabled = 0);   // disabled: bit i = item i greyed out
    // Single-line text field. Returns true when the edit is committed (Enter or clicking elsewhere).
    bool TextBox(const char* id, const RectF& r, std::wstring& text, std::wstring_view placeholder = L"", bool masked = false);   // masked: dots (passwords)
    bool TextFocused() const { return m_focusId != 0; }
    // Colour picker (ARGB, alpha kept): preset swatches, saturation/brightness square, hue bar, hex box.
    bool ColorPicker(const char* id, const RectF& r, uint32_t& argb);
    float ColorPickerHeight() const { return S(22 + 8 + 110 + 8 + 14 + 10 + 30); }
    // QR code on white with a 4-module quiet zone (phones need the light border), fitted into `r` (square).
    void Qr(const struct QrCode& code, const RectF& r);

    bool Hot(const RectF& r) const;
    bool PopupOpen() const { return m_popupId != 0; }
    void ClosePopups()
    {
        m_popupId = 0;
        m_focusId = 0;
        m_activeId = 0;
    }
    bool Animating() const { return m_wantsFrames; }
    void RequestFrame() { m_wantsFrames = true; }   // something drawn this frame changes the next one
    bool Dragging() const { return m_activeId != 0; }

private:
    static uint64_t Id(const char* s);
    bool Pressed(const RectF& r) const { return Hot(r) && m_in.pressed; }
    ID2D1SolidColorBrush* Brush(Color c);
    IDWriteTextFormat* Fmt(float size, bool bold, bool mono, bool icon = false, bool light = false);
    // Text layouts are cached across frames (creating them is the most expensive part of the UI).
    IDWriteTextLayout* Layout(std::wstring_view s, float size, bool bold, bool mono, bool icon, float w, float h, Align align, bool wrap,
                              bool light = false);
    void RevealIf(const RectF& r, bool hot, float radius);
    void TrackHover(uint64_t id, const RectF& r, std::wstring_view tip);
    void DrawPopup();

    ComPtr<ID2D1DeviceContext> m_dc;
    ComPtr<IDWriteFactory> m_dw;
    ComPtr<ID2D1SolidColorBrush> m_brush;
    ComPtr<ID2D1RadialGradientBrush> m_reveal[2];   // [0] white (dark theme), [1] black (light): fading out from the mouse
    ComPtr<ID2D1LinearGradientBrush> m_logo[2];     // the logo's light-to-deep blue, [0] dark theme, [1] light
    ComPtr<ID2D1StrokeStyle> m_roundCaps;
    std::wstring m_lightFont = L"Segoe UI";
    std::map<uint32_t, ComPtr<IDWriteTextFormat>> m_formats;
    struct CachedLayout {
        ComPtr<IDWriteTextLayout> layout;
        uint64_t lastFrame = 0;
    };
    std::unordered_map<std::wstring, CachedLayout> m_layouts;
    uint64_t m_frame = 0;
    std::vector<ComPtr<IDWriteInlineObject>> m_ellipsis;
    std::wstring m_iconFont = L"Segoe MDL2 Assets";
    std::wstring m_monoFont = L"Consolas";

    UiInput m_in;
    float m_scale = 1, m_w = 0, m_h = 0, m_alpha = 1;
    double m_time = 0;
    bool m_blocked = false;   // a popup was open at frame start: regular widgets ignore the mouse
    bool m_wantsFrames = false;

    uint64_t m_activeId = 0;  // slider being dragged
    uint64_t m_focusId = 0;   // text box with keyboard focus
    bool m_focusSeen = false;
    uint64_t m_headerFocus = 0;   // section header that Enter / Space flips
    bool m_headerSeen = false, m_headerClicked = false;

    std::unordered_map<uint64_t, float> m_pickerHue;          // hue survives grey/black colours
    std::unordered_map<uint64_t, std::wstring> m_pickerHex;   // hex text while it is being typed

    // dropdown popup
    uint64_t m_popupId = 0;
    bool m_popupJustOpened = false;
    RectF m_popupAnchor;
    std::vector<std::wstring> m_popupItems;
    int m_popupSel = -1;
    float m_popupScroll = 0;
    uint64_t m_resultFor = 0;
    int m_result = -1;

    // tooltip
    uint64_t m_hoverId = 0;
    double m_hoverStart = 0;
    bool m_hoverSeen = false;
    std::wstring m_tip;
    RectF m_tipAnchor;
};

} // namespace cc
