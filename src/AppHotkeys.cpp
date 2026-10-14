// Keyboard shortcuts the user can change (Settings ▸ Hotkeys): a key for the window and a global one that works
// while another app has focus. Scene keys (1-9, Ctrl+Alt+1-9), Flashback's arrows, Esc and Tab stay fixed.
#include "App.h"
#include "core/Log.h"

namespace cc {

const App::HotkeyAction App::kHotkeyActions[App::kHotkeyCount] = {
    {L"Record", L"Start / stop recording", VK_F9, KEY_CTRL | KEY_ALT | 'R'},
    {L"SaveReplay", L"Save the replay", VK_F8, KEY_CTRL | KEY_ALT | 'S'},
    {L"Screenshot", L"Screenshot", VK_F12, KEY_CTRL | KEY_ALT | 'P'},
    {L"Mute", L"Mute / unmute", 'M', KEY_CTRL | KEY_ALT | 'M'},
    {L"Clean", L"Clean output", VK_F10, KEY_CTRL | KEY_ALT | 'H'},
    {L"Fullscreen", L"Full screen", VK_F11, 0},
    {L"Stats", L"Statistics", VK_F3, 0},
    {L"Mini", L"Mini player", VK_F6, 0},
    {L"Pause", L"Pause recording", VK_F7, 0},
    {L"MicMute", L"Mute the microphone", 'N', KEY_CTRL | KEY_ALT | 'N'},
};

std::wstring App::KeyName(UINT combo)
{
    if (!combo) return L"";
    std::wstring s;
    if (combo & KEY_CTRL) s += L"Ctrl+";
    if (combo & KEY_ALT) s += L"Alt+";
    if (combo & KEY_SHIFT) s += L"Shift+";
    const UINT vk = combo & 0xFFFF;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) s += wchar_t(vk);
    else if (vk >= VK_F1 && vk <= VK_F24) s += Format(L"F%u", vk - VK_F1 + 1);
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) s += Format(L"Num %u", vk - VK_NUMPAD0);
    else {
        static const std::pair<UINT, const wchar_t*> named[] = {{VK_SPACE, L"Space"}, {VK_HOME, L"Home"}, {VK_END, L"End"}, {VK_INSERT, L"Insert"},
                                                               {VK_DELETE, L"Delete"}, {VK_PRIOR, L"Page Up"}, {VK_NEXT, L"Page Down"},
                                                               {VK_PAUSE, L"Pause"}, {VK_SCROLL, L"Scroll Lock"}};
        for (const auto& [k, n] : named)
            if (k == vk) return s + n;
        wchar_t name[64] = {};
        LONG scan = LONG(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16;
        if (GetKeyNameTextW(scan, name, 64) > 0) s += name;
        else s += Format(L"Key %u", vk);
    }
    return s;
}

std::wstring App::KeyHint(int action) const
{
    const UINT k = m_hotkeyLocal[action] ? m_hotkeyLocal[action].load() : (m_globalHotkeys ? m_hotkeyGlobal[action].load() : 0u);
    return k ? L" (" + KeyName(k) + L")" : L"";
}

void App::LoadHotkeys()
{
    for (int i = 0; i < kHotkeyCount; ++i) {
        const HotkeyAction& a = kHotkeyActions[i];
        m_hotkeyLocal[i] = UINT(m_settings.GetInt(L"Hotkeys", a.key, int(a.local)));
        m_hotkeyGlobal[i] = UINT(m_settings.GetInt(L"Hotkeys", (std::wstring(a.key) + L"Global").c_str(), int(a.global)));
    }
}

void App::SaveHotkeys()
{
    for (int i = 0; i < kHotkeyCount; ++i) {
        const HotkeyAction& a = kHotkeyActions[i];
        m_settings.SetInt(L"Hotkeys", a.key, int(m_hotkeyLocal[i].load()));
        m_settings.SetInt(L"Hotkeys", (std::wstring(a.key) + L"Global").c_str(), int(m_hotkeyGlobal[i].load()));
    }
}

void App::RunHotkey(int action)
{
    switch (action) {
    case 0: ToggleRecording(); break;
    case 1: SaveReplay(); break;
    case 2: m_screenshotRequested = true; break;
    case 3: ToggleMute(); break;
    case 4: SetClean(!m_clean); break;
    case 5: ToggleFullscreen(); break;
    case 6:
        m_showStats = !m_showStats;
        m_settingsDirty = true;
        break;
    case 7: SetMini(!m_miniOn); break;
    case 8: TogglePauseRecording(); break;
    case 9: ToggleMicMute(); break;
    default: break;
    }
}

// Window thread. Global hotkey ids: 1..kHotkeyCount the actions, 11..19 the scenes (Ctrl+Alt+1..9).
void App::RegisterHotkeys(bool on)
{
    if (m_hotkeysRegistered) {
        for (int id = 1; id <= kHotkeyCount; ++id) UnregisterHotKey(m_hwnd, id);
        for (int id = 11; id <= 19; ++id) UnregisterHotKey(m_hwnd, id);
        m_hotkeysRegistered = false;
    }
    m_hotkeyTaken = 0;
    m_hotkeysFailed = 0;
    if (!on || m_hotkeyCapturing) return;   // while a key is being picked, the old ones mustn't fire
    int failed = 0;
    for (int i = 0; i < kHotkeyCount; ++i) {
        const UINT k = m_hotkeyGlobal[i];
        if (!k) continue;
        const UINT mods = MOD_NOREPEAT | ((k & KEY_CTRL) ? MOD_CONTROL : 0u) | ((k & KEY_ALT) ? MOD_ALT : 0u) | ((k & KEY_SHIFT) ? MOD_SHIFT : 0u);
        if (!RegisterHotKey(m_hwnd, 1 + i, mods, k & 0xFFFF)) {
            ++failed;
            m_hotkeyTaken |= 1u << i;
        }
    }
    for (int i = 0; i < 9; ++i)
        if (!RegisterHotKey(m_hwnd, 11 + i, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, '1' + i)) ++failed;
    m_hotkeysRegistered = true;
    m_hotkeysFailed = failed;
    if (failed) LOGW(L"%d global hotkey(s) are already used by another app", failed);
}

// Keys ZeroDisplay uses itself: never given to an action.
static bool Reserved(UINT combo)
{
    const UINT vk = combo & 0xFFFF;
    if (combo & (KEY_CTRL | KEY_ALT)) return (combo & KEY_CTRL) && !(combo & KEY_ALT) && (vk == 'Z' || vk == 'Y');   // undo / redo
    return vk == VK_ESCAPE || vk == VK_TAB || vk == VK_F1 || vk == VK_RETURN || vk == VK_SPACE || vk == VK_BACK || vk == VK_LEFT || vk == VK_RIGHT ||
           vk == VK_UP || vk == VK_DOWN || (vk >= '1' && vk <= '9') || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN ||
           vk == VK_RWIN;
}

// App thread: the next key after "Press a key…" becomes the shortcut. Esc cancels, Backspace / Delete clears.
bool App::HandleHotkeyCapture(const std::vector<UINT>& keys)
{
    if (m_hotkeyCapture < 0) return false;
    for (UINT k : keys) {
        if (k & 0x20000u) continue;   // a global hotkey message
        const int action = m_hotkeyCapture % kHotkeyCount;
        const bool global = m_hotkeyCapture >= kHotkeyCount;
        const UINT vk = k & 0xFFFF;
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;   // a modifier alone: wait for the key
        std::atomic<UINT>& slot = global ? m_hotkeyGlobal[action] : m_hotkeyLocal[action];
        const UINT combo = k & (0xFFFF | KEY_CTRL | KEY_ALT | KEY_SHIFT);
        if (vk == VK_ESCAPE) {
            // nothing changes
        } else if (vk == VK_BACK || vk == VK_DELETE) {
            slot = 0;
            Toast(std::wstring(kHotkeyActions[action].name) + (global ? L": no global hotkey" : L": no key"));
        } else if (Reserved(combo)) {
            Toast(KeyName(combo) + L" is used by ZeroDisplay itself: pick another key");
            return true;   // still waiting
        } else if (global && !(combo & (KEY_CTRL | KEY_ALT))) {
            Toast(L"A global hotkey needs Ctrl or Alt (it works in every app)");
            return true;
        } else {
            // A key belongs to one action: taken from the other one if needed.
            for (int i = 0; i < kHotkeyCount; ++i)
                for (std::atomic<UINT>* other : {&m_hotkeyLocal[i], &m_hotkeyGlobal[i]})
                    if (other != &slot && *other == combo) {
                        *other = 0;
                        Toast(KeyName(combo) + L" moved from " + kHotkeyActions[i].name + L" to " + kHotkeyActions[action].name);
                    }
            slot = combo;
        }
        m_hotkeyCapture = -1;
        m_hotkeyCapturing = false;
        m_settingsDirty = true;
        PostMessageW(m_hwnd, WM_APP + 3, m_globalHotkeys ? 1 : 0, 0);   // register what's set now
        return true;
    }
    return true;   // waiting: keys go nowhere else meanwhile
}

void App::DrawHotkeyRows(float& y, float cx, float cw)
{
    Ui& u = m_ui;
    const float rowH = u.S(34), bw = (cw * 0.62f - u.S(8)) / 2, x0 = cx + cw - 2 * bw - u.S(8);
    u.Text(L"In this window", {x0, y, bw, u.S(18)}, u.S(11), theme::TextDim, Align::Center);
    u.Text(L"Anywhere (global)", {x0 + bw + u.S(8), y, bw, u.S(18)}, u.S(11), theme::TextDim, Align::Center);
    y += u.S(20);
    for (int i = 0; i < kHotkeyCount; ++i) {
        const HotkeyAction& a = kHotkeyActions[i];
        u.Text(a.name, {cx, y, x0 - cx - u.S(6), rowH}, u.S(12.5f), theme::Text);
        for (int g = 0; g < 2; ++g) {
            const int slotId = i + g * kHotkeyCount;
            const UINT k = g ? m_hotkeyGlobal[i].load() : m_hotkeyLocal[i].load();
            const bool waiting = m_hotkeyCapture == slotId;
            const bool taken = g && (m_hotkeyTaken.load() >> i & 1u) && m_globalHotkeys;
            const std::string id = "hk" + std::to_string(slotId);
            const std::wstring text = waiting ? L"Press a key…" : (k ? KeyName(k) : L"none") + (taken ? L" (taken)" : L"");
            if (u.Button(id.c_str(), {x0 + g * (bw + u.S(8)), y + u.S(2), bw, rowH - u.S(4)}, text, 0, waiting, g && !m_globalHotkeys)) {
                m_hotkeyCapture = waiting ? -1 : slotId;
                m_hotkeyCapturing = m_hotkeyCapture >= 0;
                PostMessageW(m_hwnd, WM_APP + 3, m_globalHotkeys && !m_hotkeyCapturing ? 1 : 0, 0);
            }
        }
        y += rowH;
    }
    const std::wstring help = L"Click a key, then press the new one (Esc cancels, Backspace removes it). Global hotkeys need Ctrl or Alt. "
                              L"Scenes: 1-9 here, Ctrl+Alt+1-9 anywhere.";
    const float th = u.MeasureBlock(help, cw, u.S(11.5f));
    u.TextBlock(help, {cx, y + u.S(4), cw, th + u.S(2)}, u.S(11.5f), theme::TextDim);
    y += th + u.S(8);
    bool changed = false;
    for (int i = 0; i < kHotkeyCount; ++i) changed |= m_hotkeyLocal[i] != kHotkeyActions[i].local || m_hotkeyGlobal[i] != kHotkeyActions[i].global;
    if (u.Button("hkdefault", {cx, y, u.S(170), rowH - u.S(4)}, L"Default keys", glyph::Undo, false, !changed)) {
        for (int i = 0; i < kHotkeyCount; ++i) {
            m_hotkeyLocal[i] = kHotkeyActions[i].local;
            m_hotkeyGlobal[i] = kHotkeyActions[i].global;
        }
        m_hotkeyCapture = -1;
        m_hotkeyCapturing = false;
        m_settingsDirty = true;
        PostMessageW(m_hwnd, WM_APP + 3, m_globalHotkeys ? 1 : 0, 0);
    }
    y += rowH;
}

} // namespace cc
