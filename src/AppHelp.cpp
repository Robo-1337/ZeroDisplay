// F1: every key on one page (the user's own hotkeys included), and a short welcome on the very first start.
#include "App.h"
#include "core/Log.h"

namespace cc {

void App::DrawHelp(const UiInput& in)
{
    Ui& u = m_ui;
    const float W = u.Width(), H = u.Height();
    u.Fill({0, 0, W, H}, Color{0, 0, 0, 0.55f});
    struct Row {
        std::wstring key, what;
    };
    struct Group {
        const wchar_t* title;
        std::vector<Row> rows;
    };
    auto key = [&](int action) {
        const UINT k = m_hotkeyLocal[action];
        return k ? KeyName(k) : std::wstring(L"(none)");
    };
    std::vector<Group> groups = {
        {L"Picture and window",
         {{key(5) + L" / Alt+Enter / double-click", L"Full screen"},
          {L"Esc", L"Close a panel, leave full screen"},
          {key(4), L"Clean output (only the picture)"},
          {key(7), L"Mini player (always on top)"},
          {key(6), L"Statistics"},
          {L"Tab", L"All devices side by side"},
          {L"1 - 9", L"Switch device (Advanced: scene)"}}},
        {L"Recording and sound",
         {{key(0), L"Start / stop recording"},
          {key(8), L"Pause / resume recording"},
          {key(1), L"Save the replay buffer"},
          {key(2), L"Screenshot"},
          {key(3) + L", Up / Down", L"Mute, volume"},
          {key(9), L"Mute the microphone"}}},
        {L"Flashback (look back while it keeps running)",
         {{L"←", L"Open Flashback 10 s back"},
          {L"← / →, Space", L"5 s back / forward, pause"},
          {L"I / O, Enter", L"Mark a clip, save it"},
          {L"Esc", L"Back to live"}}},
        {L"Advanced",
         {{L"Ctrl+Z / Ctrl+Y", L"Undo / redo"},
          {L"Delete, arrows (Shift: 10 px)", L"Remove / move the selected source"},
          {L"Ctrl+D", L"Duplicate the selected source"}}},
    };
    std::vector<Row> global;
    if (m_globalHotkeys)
        for (int i = 0; i < kHotkeyCount; ++i)
            if (m_hotkeyGlobal[i]) global.push_back({KeyName(m_hotkeyGlobal[i]), kHotkeyActions[i].name});
    if (m_globalHotkeys) global.push_back({L"Ctrl+Alt+1 - 9", L"Scene (Advanced)"});
    if (!global.empty()) groups.push_back({L"Anywhere (global hotkeys)", global});

    const float pw = std::min(W - u.S(40), u.S(860)), colW = (pw - u.S(72)) / 2, rowH = u.S(24);
    // Two columns: groups fill the left one, then the right one.
    float heights[2] = {0, 0};
    std::vector<int> column(groups.size());
    float total = 0;
    for (const auto& g : groups) total += u.S(34) + rowH * float(g.rows.size());
    for (size_t i = 0; i < groups.size(); ++i) {
        const float h = u.S(34) + rowH * float(groups[i].rows.size());
        column[i] = heights[0] + h <= total / 2 + u.S(40) ? 0 : 1;
        heights[column[i]] += h;
    }
    const float ph = std::min(H - u.S(40), std::max(heights[0], heights[1]) + u.S(110));
    const RectF panel{(W - pw) * 0.5f, (H - ph) * 0.5f, pw, ph};
    u.Fill(panel, theme::Panel, u.S(8));
    u.Stroke(panel, theme::Border, 1, u.S(8));
    u.Text(L"Keys", {panel.x + u.S(24), panel.y + u.S(16), pw, u.S(30)}, u.S(20), theme::Text, Align::Left, true);
    u.Text(L"Change them in Settings ▸ Hotkeys", {panel.x, panel.y + u.S(20), pw - u.S(24), u.S(24)}, u.S(12), theme::TextDim, Align::Right);
    float y[2] = {panel.y + u.S(64), panel.y + u.S(64)};
    for (size_t i = 0; i < groups.size(); ++i) {
        const int c = column[i];
        const float x = panel.x + u.S(24) + c * (colW + u.S(24));
        u.Text(groups[i].title, {x, y[c], colW, u.S(24)}, u.S(12), theme::Accent2, Align::Left, true);
        y[c] += u.S(30);
        for (const auto& r : groups[i].rows) {
            const float kw = colW * 0.44f;
            u.Text(r.key, {x, y[c], kw - u.S(6), rowH}, u.S(12.5f), theme::Text, Align::Left, true);
            u.Text(r.what, {x + kw, y[c], colW - kw, rowH}, u.S(12.5f), theme::TextDim);
            y[c] += rowH;
        }
        y[c] += u.S(4);
    }
    u.Text(L"F1 or Esc closes this", {panel.x, panel.Bottom() - u.S(34), pw, u.S(24)}, u.S(12), theme::TextDim, Align::Center);
    if (in.pressed && !panel.Contains(in.mx, in.my)) m_showHelp = false;
}

// The first start: where things are, once.
void App::DrawWelcome(const UiInput& in)
{
    (void)in;
    if (m_welcomeDone || (m_cmd.selftestSec > 0 && !m_cmd.welcomeTest)) return;
    if (!m_welcomeQpc) m_welcomeQpc = QpcNow();
    Ui& u = m_ui;
    const std::wstring text = L"Move the mouse to show the top bar: device, format, record, settings. Press F1 for every key. "
                              L"Everything else is in Settings (the gear).";
    const float areaW = m_settingsOpen ? u.Width() - std::min(u.S(380), u.Width() - u.S(20)) - u.S(16) : u.Width();   // left of Settings
    const float w = std::min(areaW - u.S(40), u.S(560));
    const float th = u.MeasureBlock(text, w - u.S(40), u.S(13));
    const RectF r{(areaW - w) * 0.5f, u.Height() - u.S(150) - th, w, th + u.S(96)};
    u.Fill(r, theme::Popup, u.S(8));
    u.Stroke(r, theme::Accent2.A(0.6f), 1, u.S(8));
    u.Logo({r.x + u.S(20), r.y + u.S(15), u.S(20), u.S(20)});
    u.Text(L"Welcome to ZeroDisplay", {r.x + u.S(48), r.y + u.S(12), r.w - u.S(68), u.S(26)}, u.S(15), theme::Text, Align::Left, true);
    u.TextBlock(text, {r.x + u.S(20), r.y + u.S(42), r.w - u.S(40), th + u.S(2)}, u.S(13), theme::TextDim);
    const bool help = u.Button("welcomehelp", {r.x + u.S(20), r.Bottom() - u.S(44), u.S(140), u.S(32)}, L"Show keys (F1)");
    const bool done = u.Button("welcomeok", {r.Right() - u.S(140), r.Bottom() - u.S(44), u.S(120), u.S(32)}, L"Got it", 0, true);
    if (help) m_showHelp = true;
    if (help || done || QpcElapsedMs(m_welcomeQpc) > 25000) {   // shown once; it goes away by itself too
        m_welcomeDone = true;
        m_settingsDirty = true;
    }
}

} // namespace cc
