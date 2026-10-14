#include "App.h"
#include "wireless/MiracastSink.h"
#include "core/Crash.h"
#include "core/ImageIo.h"
#include "core/Log.h"
#include "devices/TestPattern.h"
#include "wireless/CastSource.h"
#include "wireless/CaptivePortal.h"
#include "wireless/Hotspot.h"
#include "ui/QrCode.h"
#include "net/NetReceiver.h"
#include "audio/AudioSim.h"

#include <dwmapi.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <numeric>
#include <cmath>

namespace cc {

static constexpr UINT WM_APP_FULLSCREEN = WM_APP + 1;
static constexpr UINT WM_APP_CURSOR = WM_APP + 2;
static constexpr UINT KEY_ALT_ENTER = 0x10000 | VK_RETURN;
static const wchar_t* kWindowClass = L"CaptureCardWindow";

// ---------------------------------------------------------------------------------------------
// Command line

CommandLine CommandLine::Parse()
{
    CommandLine c;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (a == L"--selftest") c.selftestSec = _wtof(next().c_str());
        else if (a == L"--cycle") c.cycle = true;
        else if (a == L"--multiview") c.multiview = true;
        else if (a == L"--shot") c.shot = next();
        else if (a == L"--device") c.device = next();
        else if (a == L"--exclusive") c.exclusive = true;
        else if (a == L"--ui-shot") c.uiShot = next();
        else if (a == L"--record") c.record = true;
        else if (a == L"--share") c.share = true;
        else if (a == L"--connect") c.connect = next();
        else if (a == L"--pin") c.pin = next();
        else if (a == L"--pin-test") c.pinTest = true;
        else if (a == L"--advanced") c.advanced = true;
        else if (a == L"--demo-scene") c.demoScene = c.advanced = true;
        else if (a == L"--studio") c.studio = c.advanced = true;
        else if (a == L"--replay") c.replay = true;
        else if (a == L"--clean") c.clean = true;
        else if (a == L"--software-encoder") c.softwareEncoder = true;
        else if (a == L"--mic") c.mic = next();
        else if (a == L"--hevc") c.hevc = true;
        else if (a == L"--split") c.splitSec = _wtoi(next().c_str());
        else if (a == L"--limit") c.limitSec = _wtoi(next().c_str());
        else if (a == L"--tracks") c.tracks = true;
        else if (a == L"--standby-test") c.standbyTest = true;
        else if (a == L"--autocrop") c.autoCrop = true;
        else if (a == L"--demo-black") c.demoBlack = true;
        else if (a == L"--flashback-test") c.flashbackTest = true;
        else if (a == L"--trim") c.trim = next();
        else if (a == L"--mini") c.mini = true;
        else if (a == L"--lagcheck-sim") c.lagSim = _wtoi(next().c_str());
        else if (a == L"--mode-switch") c.modeSwitch = true;
        else if (a == L"--net-loss") c.netLoss = _wtof(next().c_str());
        else if (a == L"--stream-quality") c.streamQuality = std::clamp(_wtoi(next().c_str()), 0, 4);
        else if (a == L"--encoder-stall-test") c.encoderStallTest = true;
        else if (a == L"--encoder-speed") c.encoderSpeed = std::clamp(_wtoi(next().c_str()), 0, 3);
        else if (a == L"--encode-hw") {
            const std::wstring v = next();
            c.encodeHw = v == L"hybrid" ? 1 : (v == L"igpu" || v == L"display" ? 2 : (v == L"software" ? 3 : 0));
        } else if (a == L"--encode-align") c.encodeAlign = UINT(std::clamp(_wtoi(next().c_str()), 0, 64));
        else if (a == L"--hybrid-slow-test") c.hybridSlowTest = true;
        else if (a == L"--display-slow-test") c.displaySlowTest = true;
        else if (a == L"--settings-scroll") c.settingsScroll = float(_wtof(next().c_str()));
        else if (a == L"--wireless") c.wireless = true;
        else if (a == L"--hotspot") c.hotspot = true;
        else if (a == L"--record-cast") c.recordCast = true;
        else if (a == L"--flip-log") c.flipLog = true;
        else if (a == L"--portal-test") c.portalTest = true;
        else if (a == L"--audio-sim") c.audioSim = true;
        else if (a == L"--power") c.power = next() == L"battery" ? 1 : 0;
        else if (a == L"--low-memory") c.lowMemory = true;
        else if (a == L"--replay-resize-test") c.replayResizeTest = true;
        else if (a == L"--ai-upscale") c.aiUpscale = true;
        else if (a == L"--all-formats") c.allFormats = true;
        else if (a == L"--all-formats-from") {
            c.allFormats = true;
            c.allFormatsFrom = _wtoi(next().c_str());
        }
        else if (a == L"--test-modes") {   // a,b,c: the source changes mode every few seconds
            c.testPattern = true;
            std::wstring list = next(), item;
            for (wchar_t ch : list + L",") {
                if (ch == L',') {
                    if (!item.empty()) TestPattern::s_sequence.push_back(item);
                    item.clear();
                } else {
                    item += ch;
                }
            }
        }
        else if (a == L"--test-audio-rate") TestPattern::s_audioRate = Clamp(uint32_t(_wtoi(next().c_str())), 8000u, 768000u);
        else if (a == L"--test-switch-sec") TestPattern::s_switchSec = std::max(2.0, _wtof(next().c_str()));
        else if (a == L"--test-mode") {
            c.testPattern = true;
            if (!TestPattern::ParseMode(next())) MessageBoxW(nullptr, L"--test-mode wants WxH@fps, e.g. 3440x1440@100 or 1920x1080@59.94", L"ZeroDisplay", MB_OK);
        }
        else if (a == L"--low-resource") c.lowResource = true;
        else if (a == L"--preview-30") c.preview30 = true;
        else if (a == L"--hotspot-toggle-test") c.hotspot = c.hotspotToggleTest = true;
        else if (a == L"--no-wireless") c.noWireless = true;
        else if (a == L"--wireless-toggle-test") c.wirelessToggleTest = true;
        else if (a == L"--profile-test") c.profileTest = true;
        else if (a == L"--twin-test") c.twinTest = true;
        else if (a == L"--sound-stop-test") c.soundStopTest = true;
        else if (a == L"--rotate") c.rotate = _wtoi(next().c_str());
        else if (a == L"--mirror") c.mirror = true;
        else if (a == L"--sync") c.syncMs = _wtoi(next().c_str());
        else if (a == L"--hdr-mode") c.hdrMode = _wtoi(next().c_str());
        else if (a == L"--hdr-output-test") c.hdrOutputTest = true;
        else if (a == L"--hdr-on") c.hdrOn = true;
        else if (a == L"--test-hdr") {
            c.testPattern = c.testHdr = true;
            TestPattern::s_hdr = true;
        }
        else if (a == L"--crop") swscanf_s(next().c_str(), L"%f,%f,%f,%f", &c.crop[0], &c.crop[1], &c.crop[2], &c.crop[3]);
        else if (a == L"--test-pattern") c.testPattern = true;
        else if (a == L"--test-image") {
            c.testPattern = true;
            TestPattern::s_image = next();
        }
        else if (a == L"--encode-convert-vp") c.encodeConvertVp = true;
        else if (a == L"--undo-test") c.undoTest = true;
        else if (a == L"--studio-test") c.studioTest = c.studio = c.advanced = true;
        else if (a == L"--ui-select") c.uiSelect = _wtoi(next().c_str());
        else if (a == L"--demo-font") c.demoFont = next();
        else if (a == L"--multiview-devices") c.multiviewDevices = next();
        else if (a == L"--media-test") c.mediaTest = next();
        else if (a == L"--qr-test") c.qrTest = next();
        else if (a == L"--ui-steps") c.uiSteps = next();
        else if (a == L"--bar-test") c.barTest = true;
        else if (a == L"--welcome-test") c.welcomeTest = true;
        else if (a == L"--crash-test") c.crashTest = true;
        else if (a == L"--export-settings") c.exportSettings = next();
        else if (a == L"--import-settings") c.importSettings = next();
        else if (a == L"--reset-settings") c.resetSettings = true;
        else if (a == L"--keep-test-mb") c.keepTestMb = _wtoi(next().c_str());
        else if (a == L"--device-window") c.deviceWindow = next();
        else if (a == L"--superres-sim") c.superresSim = next();
        else if (a == L"--profile-dir") c.profileDir = next();
        else if (a == L"--portable-toggle-test") c.portableToggleTest = true;
        else if (a == L"--wireless-view") c.wirelessView = c.wireless = true;
        else if (a == L"--filter") c.filter = _wtoi(next().c_str());
        else if (a == L"--enhance") c.enhance = std::clamp(_wtoi(next().c_str()), 0, 2);
        else if (a == L"--scale") {
            const std::wstring v = next();
            c.scale = v == L"fill" ? 1 : (v == L"stretch" ? 2 : (v == L"integer" ? 3 : 0));
        }
        else if (a == L"--enhance-strength") c.enhanceStrength = std::clamp(float(_wtof(next().c_str())), 0.0f, 1.0f);
        else if (a == L"--filter-lines") c.filterLines = _wtoi(next().c_str());
        else if (a == L"--audio-skew") c.audioSkew = _wtof(next().c_str());
        else if (a == L"--gpu") {
            std::wstring v = next();
            c.gpu = v == L"warp" ? -2 : (v == L"auto" ? -1 : _wtoi(v.c_str()));
        }
    }
    if (argv) LocalFree(argv);
    return c;
}

// ---------------------------------------------------------------------------------------------

App::App()
{
    m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

App::~App()
{
    CloseHandle(m_wake);
    CloseHandle(m_frameEvent);
}

int App::Run(HINSTANCE instance, const CommandLine& cmd)
{
    m_instance = instance;
    m_cmd = cmd;
    Settings::s_profileDir = cmd.profileDir;
    m_settings.Load();
    // Self-tests never change the user's settings or scenes (only a --profile-dir test profile is written; the portable
    // toggle test runs only with one).
    Settings::s_readOnly = cmd.selftestSec > 0 && cmd.profileDir.empty();
    log::Init(m_settings.Directory() + L"\\log.txt");
    SetCrashDirectory(m_settings.Directory(), log::Path());
    LOGI(L"ZeroDisplay %s starting (settings: %s%s)", CC_VERSION, m_settings.Path().c_str(), m_settings.Portable() ? L", portable" : L"");
    LoadSettings();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    WNDCLASSEXW mc = wc;
    mc.lpfnWndProc = MiniProcThunk;
    WNDCLASSEXW dc = wc;
    dc.lpfnWndProc = DeviceWindowProcThunk;
    dc.lpszClassName = L"CaptureCardDevice";
    dc.hbrBackground = HBRUSH(GetStockObject(BLACK_BRUSH));
    RegisterClassExW(&dc);
    mc.lpszClassName = L"CaptureCardMini";
    mc.hbrBackground = HBRUSH(GetStockObject(BLACK_BRUSH));
    RegisterClassExW(&mc);
    WNDCLASSEXW fc = mc;
    fc.lpfnWndProc = FlashProcThunk;
    fc.lpszClassName = L"CaptureCardFlash";
    fc.hCursor = nullptr;
    RegisterClassExW(&fc);

    // Light or dark before the first frame and title bar.
    m_themeMode = std::clamp(m_settings.GetInt(L"General", L"Theme", 0), 0, 2);
    m_systemDark = theme::SystemDark();
    m_themeDark = m_themeMode == 0 ? m_systemDark.load() : m_themeMode == 2;
    theme::Apply(m_themeDark);

    // Initial placement: last saved, else 1280x720 client area centered on the primary monitor.
    UINT sysDpi = GetDpiForSystem();
    int w = m_settings.GetInt(L"Window", L"W", 0), h = m_settings.GetInt(L"Window", L"H", 0);
    int x = m_settings.GetInt(L"Window", L"X", INT_MIN), y = m_settings.GetInt(L"Window", L"Y", INT_MIN);
    if (w < 320 || h < 200 || x == INT_MIN) {
        RECT rc{0, 0, MulDiv(1280, int(sysDpi), 96), MulDiv(720, int(sysDpi), 96)};
        AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, sysDpi);
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        w = std::min(w, int(work.right - work.left));
        h = std::min(h, int(work.bottom - work.top));
        x = work.left + (work.right - work.left - w) / 2;
        y = work.top + (work.bottom - work.top - h) / 2;
    }
    m_hwnd = CreateWindowExW(0, kWindowClass, L"ZeroDisplay", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, instance, this);
    DragAcceptFiles(m_hwnd, TRUE);   // a video dropped on the window opens in the trimmer
    if (!m_hwnd) return 1;
    StyleFrame(m_hwnd);
    m_dpi = GetDpiForWindow(m_hwnd);

    ShowWindow(m_hwnd, m_settings.GetBool(L"Window", L"Maximized", false) ? SW_SHOWMAXIMIZED : SW_SHOW);
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    m_clientW = UINT(rc.right - rc.left);
    m_clientH = UINT(rc.bottom - rc.top);
    if (m_startFullscreen) ApplyFullscreen(true);

    m_deviceWatcher.StartWatching(m_hwnd, [this](bool defOut) { RequestRescan(defOut); });
    RegisterHotkeys(m_globalHotkeys);
    m_appThread = std::thread([this] { AppThread(); });

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    m_deviceWatcher.StopWatching();
    LOGI(L"Exit");
    if (m_restartAfterExit) {   // settings imported / reset: start again with the same command line
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmdLine = GetCommandLineW();
        if (const size_t at = cmdLine.find(L" --wait-for "); at != std::wstring::npos) cmdLine.resize(at);   // an earlier restart's
        cmdLine += Format(L" --wait-for %lu", GetCurrentProcessId());   // the new one waits until this one is gone
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(exe, cmdLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Window thread

LRESULT CALLBACK App::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(cs->lpCreateParams));
    }
    auto self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->WndProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // Scripted UI tests (--ui-steps) own the mouse: the real one, wherever it is, neither steers the test nor gets
    // hidden by it (the person at the PC keeps their cursor).
    if (!m_cmd.uiSteps.empty()) {
        switch (msg) {
        case WM_MOUSEMOVE:
        case WM_MOUSELEAVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MOUSEWHEEL: return 0;
        case WM_APP_CURSOR: return 0;
        case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
        default: break;
        }
    }
    auto pushKey = [&](UINT key) {
        {
            std::lock_guard lock(m_inputMutex);
            m_input.keys.push_back(key);
        }
        SetEvent(m_wake);
    };
    static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");
    if (msg == taskbarCreated) {   // Explorer (re)made the button: the overlay goes on again
        m_taskbar.Reset();
        ShowRecordingOnTaskbar(m_taskbarShown);
        return 0;
    }
    switch (msg) {
    case WM_ACTIVATEAPP:   // back from another app (maybe the graphics driver's settings): check AI upscaling again
        if (wp) {
            m_srRecheck = true;
            SetEvent(m_wake);
        }
        break;
    case WM_DISPLAYCHANGE:   // a game or the user changed a screen's mode: re-read the refresh rate, excuse encoder stalls
        m_displayChangedQpc = QpcNow();
        if (m_displayWake) SetEvent(m_displayWake);
        SetEvent(m_wake);
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            m_clientW = LOWORD(lp);
            m_clientH = HIWORD(lp);
            m_resizePending = true;
            SetEvent(m_wake);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {480, 300};
        return 0;
    }
    case WM_SETTINGCHANGE:
        // Windows switched between light and dark apps (Settings > Personalization > Colors).
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
            m_systemDark = theme::SystemDark();
            SetEvent(m_wake);
        }
        break;
    case WM_APP_THEME:
        // The render thread switched the colours: the title bars of this app's windows follow (the main window,
        // device windows, the mini player; not the other windows on this thread, such as the IME's).
        EnumThreadWindows(GetCurrentThreadId(), [](HWND w, LPARAM self) {
            wchar_t cls[64] = {};
            GetClassNameW(w, cls, 64);
            if (!wcscmp(cls, kWindowClass) || !wcscmp(cls, L"CaptureCardDevice") || !wcscmp(cls, L"CaptureCardMini"))
                reinterpret_cast<const App*>(self)->StyleFrame(w);
            return TRUE;
        }, reinterpret_cast<LPARAM>(this));
        return 0;
    case WM_DPICHANGED: {
        m_dpi = HIWORD(wp);
        auto r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!m_trackingMouse) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            m_trackingMouse = true;
        }
        std::lock_guard lock(m_inputMutex);
        float nx = float(GET_X_LPARAM(lp)), ny = float(GET_Y_LPARAM(lp));
        if (nx != m_input.mx || ny != m_input.my) m_input.moved = true;
        m_input.mx = nx;
        m_input.my = ny;
        m_input.mouseIn = true;
        SetEvent(m_wake);
        return 0;
    }
    case WM_MOUSELEAVE: {
        m_trackingMouse = false;
        std::lock_guard lock(m_inputMutex);
        if (!m_input.down) m_input.mouseIn = false;
        SetEvent(m_wake);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        SetCapture(hwnd);
        std::lock_guard lock(m_inputMutex);
        m_input.down = true;
        m_input.pressed = true;
        m_input.moved = true;
        m_input.mouseIn = true;
        m_input.mx = float(GET_X_LPARAM(lp));
        m_input.my = float(GET_Y_LPARAM(lp));
        if (msg == WM_LBUTTONDBLCLK) m_input.dbl = true;
        SetEvent(m_wake);
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        std::lock_guard lock(m_inputMutex);
        m_input.down = false;
        m_input.released = true;
        SetEvent(m_wake);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        std::lock_guard lock(m_inputMutex);
        m_input.wheel += float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        m_input.moved = true;
        SetEvent(m_wake);
        return 0;
    }
    case WM_KEYDOWN:
        // Ctrl / Shift travel with the key (KEY_CTRL / KEY_SHIFT) for shortcuts such as Ctrl+Z.
        pushKey(UINT(wp) | (GetKeyState(VK_CONTROL) < 0 ? KEY_CTRL : 0u) | (GetKeyState(VK_SHIFT) < 0 ? KEY_SHIFT : 0u) |
                (GetKeyState(VK_MENU) < 0 ? KEY_ALT : 0u));
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) {
            pushKey(KEY_ALT_ENTER);
            return 0;
        }
        if (wp == VK_F10) {
            pushKey(UINT(wp));
            return 0;
        }
        // Alt + key: a shortcut may use it (Windows still gets it: Alt+F4, Alt+Space).
        pushKey(UINT(wp) | KEY_ALT | (GetKeyState(VK_CONTROL) < 0 ? KEY_CTRL : 0u) | (GetKeyState(VK_SHIFT) < 0 ? KEY_SHIFT : 0u));
        if (m_hotkeyCapturing) return 0;   // being picked as a shortcut: no menu beep
        break;
    case WM_CHAR: {
        std::lock_guard lock(m_inputMutex);
        m_input.chars += wchar_t(wp);
        SetEvent(m_wake);
        return 0;
    }
    case WM_SYSCHAR:
        if (wp == VK_RETURN) return 0;   // suppress the beep for Alt+Enter
        break;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && m_hideCursor) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_APP_CURSOR:
        if (wp) SetCursor(nullptr);
        else SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return 0;
    case WM_APP_FULLSCREEN:
        ApplyFullscreen(wp != 0);
        return 0;
    case WM_HOTKEY:
        pushKey(0x20000u | UINT(wp));
        return 0;
    case WM_APP + 6:   // lag check: create (wp = 1, lp = monitor) / destroy the flash window
        if (wp) CreateFlashWindow(int(lp));
        else if (HWND f = m_flashHwnd.exchange(nullptr)) DestroyWindow(f);
        return 0;
    case WM_APP + 7:   // lag check: repaint the flash window (black / white) and note when it is on screen
        if (HWND f = m_flashHwnd.load()) {
            InvalidateRect(f, nullptr, FALSE);
            UpdateWindow(f);
            DwmFlush();   // returns when the composed frame with the change has been presented
            if (m_flashWhite.load()) m_flashShownQpc = QpcNow();
        }
        return 0;
    case WM_APP_DEVWIN_CREATE:
        CreateDeviceWindow(reinterpret_cast<DeviceWindow*>(lp));
        return 0;
    case WM_APP_DEVWIN_DESTROY:
        DestroyWindow(reinterpret_cast<HWND>(lp));
        return 0;
    case WM_APP + 4:   // pop-out mini player: create / destroy its window
        if (wp) CreateMiniWindow();
        else if (HWND mini = m_miniHwnd.exchange(nullptr)) DestroyWindow(mini);
        return 0;
    case WM_APP + 3:   // re-register hotkeys after the setting changed
        RegisterHotkeys(wp != 0);
        return 0;
    case WM_DEVICECHANGE:
        m_deviceWatcher.OnDeviceChange(wp);
        return TRUE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ValidateRect(hwnd, nullptr);
        SetEvent(m_wake);
        return 0;
    case WM_CLOSE:
        if (!ConfirmClose()) {   // a recording / viewers / unsaved replay: asked first
            m_restartAfterExit = false;
            m_onExit = OnExit::None;
            return 0;
        }
        SaveWindowPlacement();
        m_quit = true;
        SetEvent(m_wake);
        if (m_appThread.joinable()) m_appThread.join();
        for (auto& w : m_deviceWindows)
            if (HWND h = w->hwnd.exchange(nullptr)) DestroyWindow(h);
        DestroyWindow(hwnd);
        return 0;
    case WM_DROPFILES: {
        wchar_t path[MAX_PATH] = {};
        const UINT n = DragQueryFileW(HDROP(wp), 0xFFFFFFFF, nullptr, 0);
        DragQueryFileW(HDROP(wp), 0, path, MAX_PATH);
        DragFinish(HDROP(wp));
        const std::wstring file = path, ext = file.size() > 4 ? file.substr(file.size() - 4) : L"";
        if (n >= 1 && (_wcsicmp(ext.c_str(), L".mp4") == 0 || _wcsicmp(ext.c_str(), L".m4v") == 0 || _wcsicmp(ext.c_str(), L".mov") == 0)) {
            std::lock_guard lock(m_pickMutex);
            m_trimPicked = file;   // opened by the app thread (UpdateTrim)
        } else {
            Toast(L"Drop a video (.mp4) on the window to trim it");
        }
        SetEvent(m_wake);
        return 0;
    }
    case WM_APP + 22:   // recording state for the taskbar button and the title
        ShowRecordingOnTaskbar(int(wp));
        return 0;
    case WM_QUERYENDSESSION:   // Windows shuts down / signs out: fine, but say why it may take a moment
        if (m_closeRisk & 1) ShutdownBlockReasonCreate(hwnd, L"ZeroDisplay is saving the recording");
        return TRUE;
    case WM_ENDSESSION:   // it really ends: finish the recording and save the settings, no questions
        if (wp && !m_quit) {
            SaveWindowPlacement();
            m_quit = true;
            SetEvent(m_wake);
            if (m_appThread.joinable()) m_appThread.join();
            ShutdownBlockReasonDestroy(hwnd);
            LOGI(L"Windows is shutting down: recording and settings saved");
        }
        return 0;
    case WM_DESTROY:
        m_taskbar.Reset();
        RegisterHotkeys(false);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void App::ApplyFullscreen(bool on)
{
    if (on == m_fullscreen) return;
    if (on) {
        GetWindowPlacement(m_hwnd, &m_windowedPlacement);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(m_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(m_hwnd, &m_windowedPlacement);
        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    m_fullscreen = on;
    SetEvent(m_wake);
}

void App::SaveWindowPlacement()
{
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (m_fullscreen) wp = m_windowedPlacement;
    else GetWindowPlacement(m_hwnd, &wp);
    const RECT& r = wp.rcNormalPosition;
    if (r.right - r.left < 100) return;
    m_settings.SetInt(L"Window", L"X", r.left);
    m_settings.SetInt(L"Window", L"Y", r.top);
    m_settings.SetInt(L"Window", L"W", r.right - r.left);
    m_settings.SetInt(L"Window", L"H", r.bottom - r.top);
    m_settings.SetBool(L"Window", L"Maximized", wp.showCmd == SW_SHOWMAXIMIZED);
}

// ---------------------------------------------------------------------------------------------
// Settings

void App::LoadSettings()
{
    const std::wstring v = L"Video", a = L"Audio", g = L"General";
    // Shape, scaling, filters, Enhance and audio delay: the general profile until a device is shown (AppProfiles.cpp).
    m_generalProfile = LoadProfile(L"");
    UseProfile(m_generalProfile);
    m_profileKey.clear();
    m_smooth = m_settings.GetBool(v, L"Smooth", true);
    m_matrix = ColorMatrix(Clamp(m_settings.GetInt(v, L"Matrix", 0), 0, 2));
    m_range = ColorRange(Clamp(m_settings.GetInt(v, L"Range", 0), 0, 2));
    m_preferUncompressed = m_settings.GetBool(v, L"PreferUncompressed", true);
    m_vsync = m_settings.GetBool(v, L"VSync", false);
    m_standbyOn = m_settings.GetBool(v, L"Standby", true);
    m_standbyOnBlack = m_settings.GetBool(v, L"StandbyOnBlack", true);
    m_gpuChoice = m_settings.GetInt(v, L"Gpu", -1);
    m_outputSetting = m_settings.GetString(a, L"Output", L"");
    m_volume = Clamp(m_settings.GetFloat(a, L"Volume", 1.0f), 0.0f, 2.0f);   // up to 200 % (boost for quiet cards)
    m_muted = m_settings.GetBool(a, L"Muted", false);
    m_passthrough = m_settings.GetBool(a, L"Passthrough", true);
    m_exclusiveAudio = m_settings.GetBool(a, L"Exclusive", false);
    m_latency = Clamp(m_settings.GetInt(a, L"Latency", int(AudioLatency::Low)), 0, 2);
    m_multiview = m_settings.GetBool(g, L"Multiview", false) || m_cmd.multiview;
    m_autoSwitch = m_settings.GetBool(g, L"AutoSwitch", true);
    m_startFullscreen = m_settings.GetBool(g, L"StartFullscreen", false);
    m_showStats = m_settings.GetBool(g, L"Stats", false);
    m_quality = Clamp(m_settings.GetInt(L"Recording", L"Quality", 2), 0, 3);
    m_recordFolderSetting = m_settings.GetString(L"Recording", L"Folder", L"");
    m_shotFolderSetting = m_settings.GetString(L"Screenshots", L"Folder", L"");
    m_nameTemplate = m_settings.GetString(L"Recording", L"NameTemplate", kDefaultNameTemplate);
    m_shotFormat = Clamp(m_settings.GetInt(L"Screenshots", L"Format", 0), 0, 1);
    m_shotClipboard = m_settings.GetBool(L"Screenshots", L"Clipboard", false);
    m_shotFlash = m_settings.GetBool(L"Screenshots", L"Flash", true);
    m_minFreeGb = Clamp(m_settings.GetInt(L"Recording", L"MinFreeGb", 1), 1, 50);
    m_keepGb = Clamp(m_settings.GetInt(L"Recording", L"KeepGb", 0), 0, 4000);
    m_shareOnStart = m_settings.GetBool(L"Network", L"Share", false);
    m_streamQuality = Clamp(m_settings.GetInt(L"Network", L"StreamQuality", 0), 0, 4);
    m_viewerPinOn = m_settings.GetBool(L"Network", L"ViewerPinOn", true);
    {
        bool ok = false;
        m_viewerPin = Narrow(UnprotectString(m_settings.GetString(L"Network", L"ViewerPin", L""), ok));
        if (!ok || !net::PinGate::Valid(m_viewerPin)) {   // first start (or another Windows user's file): a new one
            m_viewerPin = net::PinGate::NewPin();
            m_settingsDirty = true;
        }
    }
    ApplyViewerPin();
    m_replayOn = m_settings.GetBool(L"Recording", L"Replay", false) || m_cmd.replay;
    m_replaySeconds = Clamp(m_settings.GetFloat(L"Recording", L"ReplaySeconds", 30.0f), 10.0f, 300.0f);
    m_hevc = m_settings.GetBool(L"Recording", L"Hevc", false);
    m_encoderSpeed = Clamp(m_settings.GetInt(L"Recording", L"EncoderPreset", 0), 0, 3);
    m_encodeHw = Clamp(m_settings.GetInt(L"Recording", L"EncodeHardware", 0), 0, 3);
    m_splitMinutes = Clamp(m_settings.GetInt(L"Recording", L"SplitMinutes", 0), 0, 600);
    m_limitMinutes = Clamp(m_settings.GetInt(L"Recording", L"LimitMinutes", 0), 0, 1440);
    m_stopNoSignalMinutes = Clamp(m_settings.GetInt(L"Recording", L"StopNoSignal", 0), 0, 60);
    m_separateTracks = m_settings.GetBool(L"Recording", L"SeparateTracks", false);
    m_recordHdr = m_settings.GetBool(L"Recording", L"Hdr", true);
    m_micId = m_settings.GetString(a, L"Mic", L"");
    m_micGain = Clamp(m_settings.GetFloat(a, L"MicGain", 1.0f), 0.0f, 3.0f);
    m_micMuted = m_settings.GetBool(a, L"MicMuted", false);
    m_micGate = m_settings.GetBool(a, L"MicGate", true);
    m_micComp = m_settings.GetBool(a, L"MicComp", true);
    m_globalHotkeys = m_settings.GetBool(g, L"GlobalHotkeys", true);
    m_askBeforeClose = m_settings.GetBool(g, L"AskBeforeClose", true);
    m_welcomeDone = m_settings.GetBool(g, L"WelcomeDone", false);
    m_miniOpacity = Clamp(m_settings.GetFloat(L"Mini", L"Opacity", 1.0f), 0.3f, 1.0f);
    m_miniPlacement = m_settings.GetString(L"Mini", L"Window", L"");
    m_keepAwake = m_settings.GetBool(g, L"KeepAwake", true);
    m_soundCue = m_settings.GetBool(L"Recording", L"SoundCue", false);
    LoadHotkeys();
    m_advanced = m_settings.GetBool(g, L"Advanced", false);
    m_wirelessView = !m_advanced && m_settings.GetBool(g, L"WirelessView", false);
    m_receiverOn = m_settings.GetBool(L"Wireless", L"Receiver", false);
    m_castPin = m_settings.GetBool(L"Wireless", L"Pin", false);
    m_hotspotOn = m_settings.GetBool(L"Wireless", L"Hotspot", false);
    m_wireless = m_settings.GetBool(g, L"Wireless", true);
    // Flags that need the network turn it on for this run; --no-wireless turns it off. Neither is saved.
    m_wirelessForced = m_cmd.noWireless || m_cmd.share || m_cmd.wireless || m_cmd.hotspot || !m_cmd.connect.empty();
    if (m_wirelessForced) m_wireless = !m_cmd.noWireless;
    m_batterySaver = Clamp(m_settings.GetInt(g, L"BatterySaver", 1), 0, 2);
    g_backgroundEco = m_settings.GetBool(g, L"EfficientBackground", true);
    m_memorySaver = Clamp(m_settings.GetInt(g, L"MemorySaver", 1), 0, 2);
    m_lowResource = m_settings.GetBool(g, L"LowResource", false);
    m_preview30 = m_settings.GetBool(v, L"Preview30", false);
    m_hdrEnabled = m_settings.GetBool(v, L"HdrEnabled", false);
    m_hdrScreen = m_settings.GetBool(v, L"HdrScreen", true);
    m_aiUpscale = m_settings.GetBool(v, L"AiUpscale", false);
    m_hiddenDevices.clear();
    {
        const std::wstring list = m_settings.GetString(L"Devices", L"Hidden", L"");
        for (size_t p = 0; p <= list.size();) {
            size_t e = list.find(L'|', p);
            if (e == std::wstring::npos) e = list.size();
            if (e > p) m_hiddenDevices.insert(list.substr(p, e - p));
            p = e + 1;
        }
    }
    m_collapsed.clear();
    {
        const std::wstring list = m_settings.GetString(L"Ui", L"Collapsed", L"");
        for (size_t p = 0; p <= list.size();) {
            size_t e = list.find(L',', p);
            if (e == std::wstring::npos) e = list.size();
            if (e > p) m_collapsed.insert(Narrow(list.substr(p, e - p)));
            p = e + 1;
        }
    }
    m_hotspotSsid = m_settings.GetString(L"Wireless", L"HotspotName", L"");
    {
        bool ok = true;
        m_hotspotPass = UnprotectString(m_settings.GetString(L"Wireless", L"HotspotPassword", L""), ok);
        if (!ok) LOGW(L"Own Wi-Fi: the saved password was encrypted on another PC / user; a new one is made when the network starts");
    }
    m_studio = m_settings.GetBool(g, L"Studio", false);
    m_transitionMode = Clamp(m_settings.GetInt(g, L"Transition", 1), 0, 3);
    m_transitionMs = Clamp(m_settings.GetFloat(g, L"TransitionMs", 300.0f), 100.0f, 2000.0f);
    m_sceneIni.Open(m_settings.Directory() + L"\\scenes.ini");
    m_scenes.Load(m_sceneIni);
}

void App::SaveSettings()
{
    if (m_cmd.selftestSec > 0 && m_cmd.profileDir.empty()) {   // test runs never touch the user's settings or scenes
        m_settingsDirty = false;
        m_scenesDirty = false;
        m_lastSave = QpcNow();
        return;
    }
    const std::wstring v = L"Video", a = L"Audio", g = L"General";
    SaveProfiles();   // shape, scaling, filters, Enhance, audio delay: general + per device
    m_settings.SetBool(v, L"Smooth", m_smooth);
    m_settings.SetInt(v, L"Matrix", int(m_matrix));
    m_settings.SetInt(v, L"Range", int(m_range));
    m_settings.SetBool(v, L"PreferUncompressed", m_preferUncompressed);
    m_settings.SetBool(v, L"VSync", m_vsync);
    m_settings.SetBool(v, L"Standby", m_standbyOn);
    m_settings.SetBool(v, L"StandbyOnBlack", m_standbyOnBlack);
    if (m_cmd.gpu == -3) m_settings.SetInt(v, L"Gpu", m_gpuChoice);
    m_settings.SetString(a, L"Output", m_outputSetting);
    m_settings.SetFloat(a, L"Volume", m_volume);
    m_settings.SetBool(a, L"Muted", m_muted);
    m_settings.SetBool(a, L"Passthrough", m_passthrough);
    m_settings.SetBool(a, L"Exclusive", m_exclusiveAudio);
    m_settings.SetInt(a, L"Latency", m_latency);
    if (!m_cmd.multiview) m_settings.SetBool(g, L"Multiview", m_multiview);   // command-line flags are not saved
    m_settings.SetBool(g, L"AutoSwitch", m_autoSwitch);
    m_settings.SetBool(g, L"StartFullscreen", m_startFullscreen);
    m_settings.SetBool(g, L"Stats", m_showStats);
    m_settings.SetInt(L"Recording", L"Quality", m_quality);
    m_settings.SetString(L"Recording", L"Folder", m_recordFolderSetting);
    m_settings.SetString(L"Screenshots", L"Folder", m_shotFolderSetting);
    m_settings.SetString(L"Recording", L"NameTemplate", m_nameTemplate);
    m_settings.SetInt(L"Screenshots", L"Format", m_shotFormat);
    m_settings.SetBool(L"Screenshots", L"Clipboard", m_shotClipboard);
    m_settings.SetBool(L"Screenshots", L"Flash", m_shotFlash);
    m_settings.SetInt(L"General", L"Theme", m_themeMode);
    m_settings.SetInt(L"Recording", L"MinFreeGb", m_minFreeGb);
    m_settings.SetInt(L"Recording", L"KeepGb", m_keepGb.load());
    if (!m_cmd.share) m_settings.SetBool(L"Network", L"Share", m_shareOnStart);
    m_settings.SetInt(L"Network", L"StreamQuality", m_streamQuality);
    m_settings.SetBool(L"Network", L"ViewerPinOn", m_viewerPinOn);
    m_settings.SetString(L"Network", L"ViewerPin", ProtectString(Widen(m_viewerPin)));   // encrypted for this Windows user
    if (!m_cmd.replay) m_settings.SetBool(L"Recording", L"Replay", m_replayOn);
    m_settings.SetFloat(L"Recording", L"ReplaySeconds", m_replaySeconds);
    m_settings.SetBool(L"Recording", L"Hevc", m_hevc);
    m_settings.SetInt(L"Recording", L"EncoderPreset", m_encoderSpeed);
    m_settings.SetInt(L"Recording", L"EncodeHardware", m_encodeHw);
    m_settings.SetInt(L"Recording", L"SplitMinutes", m_splitMinutes);
    m_settings.SetInt(L"Recording", L"LimitMinutes", m_limitMinutes);
    m_settings.SetInt(L"Recording", L"StopNoSignal", m_stopNoSignalMinutes);
    m_settings.SetBool(L"Recording", L"SeparateTracks", m_separateTracks);
    m_settings.SetBool(L"Recording", L"Hdr", m_recordHdr);
    m_settings.SetString(a, L"Mic", m_micId);
    m_settings.SetFloat(a, L"MicGain", m_micGain);
    m_settings.SetBool(a, L"MicMuted", m_micMuted);
    m_settings.SetBool(a, L"MicGate", m_micGate);
    m_settings.SetBool(a, L"MicComp", m_micComp);
    m_settings.SetBool(g, L"GlobalHotkeys", m_globalHotkeys);
    m_settings.SetBool(g, L"AskBeforeClose", m_askBeforeClose.load());
    m_settings.SetBool(g, L"WelcomeDone", m_welcomeDone);
    m_settings.SetFloat(L"Mini", L"Opacity", m_miniOpacity);
    m_settings.SetString(L"Mini", L"Window", m_miniPlacement);
    m_settings.SetBool(g, L"KeepAwake", m_keepAwake);
    m_settings.SetBool(L"Recording", L"SoundCue", m_soundCue);
    SaveHotkeys();
    if (!m_cmd.advanced) m_settings.SetBool(g, L"Advanced", m_advanced);
    if (!m_cmd.wirelessView) m_settings.SetBool(g, L"WirelessView", m_wirelessView);
    m_settings.SetBool(L"Wireless", L"Receiver", m_receiverOn);
    m_settings.SetBool(L"Wireless", L"Pin", m_castPin);
    if (!m_cmd.hotspot) m_settings.SetBool(L"Wireless", L"Hotspot", m_hotspotOn);
    if (!m_wirelessForced) m_settings.SetBool(g, L"Wireless", m_wireless);
    m_settings.SetInt(g, L"BatterySaver", m_batterySaver);
    m_settings.SetBool(g, L"EfficientBackground", g_backgroundEco);
    m_settings.SetInt(g, L"MemorySaver", m_memorySaver);
    m_settings.SetBool(g, L"LowResource", m_lowResource);
    m_settings.SetBool(v, L"Preview30", m_preview30);
    m_settings.SetBool(v, L"HdrEnabled", m_hdrEnabled);
    m_settings.SetBool(v, L"HdrScreen", m_hdrScreen);
    m_settings.SetBool(v, L"AiUpscale", m_aiUpscale);
    {
        std::wstring hidden;
        for (const std::wstring& n : m_hiddenDevices) hidden += (hidden.empty() ? L"" : L"|") + n;
        m_settings.SetString(L"Devices", L"Hidden", hidden);
    }
    {
        std::wstring collapsed;
        for (const std::string& id : m_collapsed) collapsed += (collapsed.empty() ? L"" : L",") + Widen(id);
        m_settings.SetString(L"Ui", L"Collapsed", collapsed);
    }
    m_settings.SetString(L"Wireless", L"HotspotName", m_hotspotSsid);
    m_settings.SetString(L"Wireless", L"HotspotPassword", ProtectString(m_hotspotPass));   // encrypted for this Windows user
    if (!m_cmd.studio) m_settings.SetBool(g, L"Studio", m_studio);
    m_settings.SetInt(g, L"Transition", m_transitionMode);
    m_settings.SetFloat(g, L"TransitionMs", m_transitionMs);
    if (m_scenesDirty && !m_cmd.demoScene) {
        m_scenes.Save(m_sceneIni);
        m_scenesDirty = false;
    }
    // The remembered device is the Simple-mode one (Advanced scenes may show other devices).
    if (Session* s = Active(); s && !m_advanced && m_cmd.device.empty() && m_cmd.connect.empty())
        m_settings.SetString(g, L"LastDevice", s->info.Key());
    m_settingsDirty = false;
    m_lastSave = QpcNow();
}

// ---------------------------------------------------------------------------------------------
// Devices and sessions (app thread)

void App::RequestRescan(bool defaultOutputChanged)
{
    if (defaultOutputChanged) m_defaultOutChanged = true;
    m_rescanAt = QpcNow() + QpcFrequency() * 4 / 10;   // debounce bursts of notifications
    SetEvent(m_wake);
}

void App::PollRescan()
{
    int64_t at = m_rescanAt.load();
    if (at && QpcNow() >= at && !m_enumFuture.valid()) {
        m_rescanAt = 0;
        m_enumFuture = std::async(std::launch::async, [] {
            ComInit com;
            return DeviceManager::Enumerate();
        });
    }
    if (m_enumFuture.valid() && m_enumFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        ApplyDevices(m_enumFuture.get(), false);
}

int App::DeviceIndex(const VideoDeviceInfo& info) const
{
    for (size_t i = 0; i < m_devices.video.size(); ++i)
        if (m_devices.video[i].symlink == info.symlink) return int(i);
    // Same device on a different USB port gets a new symlink; fall back to its settings key (Windows' name).
    for (size_t i = 0; i < m_devices.video.size(); ++i)
        if (m_devices.video[i].Key() == info.Key()) return int(i);
    return -1;
}

int App::SessionIndex(const VideoDeviceInfo& info) const
{
    for (size_t i = 0; i < m_sessions.size(); ++i)
        if (m_sessions[i]->info.symlink == info.symlink) return int(i);
    return -1;
}

Session* App::Active() { return m_active < m_sessions.size() ? m_sessions[m_active].get() : nullptr; }

size_t App::PickInitialDevice() const
{
    const auto& v = m_devices.video;
    if (!m_cmd.device.empty())
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i].name.find(m_cmd.device) != std::wstring::npos || v[i].autoName.find(m_cmd.device) != std::wstring::npos) return i;
    std::wstring last = m_settings.GetString(L"General", L"LastDevice", L"");
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].Key() == last) return i;
    // Prefer something that looks like a capture card over a webcam.
    auto score = [&](const VideoDeviceInfo& d) {
        std::wstring n = d.name;
        for (auto& c : n) c = wchar_t(towlower(c));
        int s = 0;
        if (NetReceiver::IsNetworkLink(d.symlink) || CastSource::IsCastLink(d.symlink)) return -10;   // never auto-open a stream or a cast
        if (DeviceManager::FindPairedAudio(m_devices, d) >= 0) s += 4;
        if (n.find(L"capture") != std::wstring::npos || n.find(L"hdmi") != std::wstring::npos || n.find(L"usb3") != std::wstring::npos) s += 2;
        if (n.find(L"webcam") != std::wstring::npos || n.find(L"integrated") != std::wstring::npos || n.find(L" ir ") != std::wstring::npos) s -= 3;
        return s;
    };
    size_t best = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (score(v[i]) > score(v[best])) best = i;
    return best;
}

Session* App::OpenSession(const VideoDeviceInfo& info)
{
    // Let any previous instance of a device finish closing before reopening it.
    for (auto& f : m_graveyard) f.wait();
    m_graveyard.clear();

    auto s = std::make_unique<Session>();
    s->info = info;
    s->audioSrc = std::make_shared<AudioSource>();
    if (NetReceiver::IsNetworkLink(info.symlink)) {
        auto rx = std::make_unique<NetReceiver>(m_renderer.DxgiManager(), m_frameEvent, s->audioSrc);
        rx->SetPin(NetPinFor(info));   // the sharing PC's PIN, if typed before
        s->video = std::move(rx);
    }
    else if (TestPattern::IsTestLink(info.symlink)) s->video = std::make_unique<TestPattern>(m_frameEvent, s->audioSrc);
    else if (CastSource::IsCastLink(info.symlink)) {
        auto cast = std::make_unique<CastSource>(m_frameEvent, s->audioSrc, m_renderer.Device());
        if (info.symlink == L"media:test") cast->SetMediaSource(CastSource::MediaSourceFromFile(m_cmd.mediaTest).Get());
        else cast->SetMediaSource(CastMediaSource(info.symlink).Get());
        s->video = std::move(cast);
    }
    else s->video = std::make_unique<VideoCapture>(m_renderer.DxgiManager(), m_frameEvent);
    m_audio.AddSource(s->audioSrc);
    const std::wstring sec = Settings::DeviceSection(info.Key());
    s->audioChoice = m_settings.GetString(sec, L"Audio", L"auto");
    s->prof = s->savedProf = LoadProfile(sec);
    s->openedQpc = QpcNow();
    s->fpsStart = s->openedQpc;
    s->video->Open(info, m_settings.GetString(sec, L"Format", L""), m_preferUncompressed);
    UpdateAudioRouting(*s);
    LOGI(L"Session opened: %s", info.name.c_str());
    m_sessions.push_back(std::move(s));
    return m_sessions.back().get();
}

void App::CloseSession(size_t index)
{
    if (index >= m_sessions.size()) return;
    // Sharing and the replay buffer re-attach to the new active device (UpdateSharingSource / UpdateReplay).
    if (m_sessions[index]->info.symlink == m_encoderSymlink) DestroyEncoder();
    if (m_sessions[index]->info.symlink == m_shareSymlink) DestroyShareEncoder();
    StopExtraRecordings(m_sessions[index]->info.symlink);
    std::unique_ptr<Session> s = std::move(m_sessions[index]);
    m_sessions.erase(m_sessions.begin() + ptrdiff_t(index));
    LOGI(L"Session closed: %s", s->info.name.c_str());
    if (s->info.Key() == m_profileKey) s->prof = CurrentProfile();   // Settings may have changed it this frame
    if (!(s->prof == s->savedProf) && !(m_cmd.selftestSec > 0 && m_cmd.profileDir.empty()))
        SaveProfile(Settings::DeviceSection(s->info.Key()), s->prof);
    m_audio.RemoveSource(s->audioSrc.get());   // first, so the output never pulls from a stopping capture
    s->audioCap.reset();
    // Closing a device can take a moment; do it off the render thread.
    m_graveyard.push_back(std::async(std::launch::async, [v = std::move(s->video)]() mutable { v.reset(); }));
    if (m_active >= m_sessions.size()) m_active = m_sessions.empty() ? 0 : m_sessions.size() - 1;
    else if (m_active > index) --m_active;
}

void App::SwitchTo(size_t deviceIndex)
{
    if (deviceIndex >= m_devices.video.size()) return;
    const VideoDeviceInfo& info = m_devices.video[deviceIndex];
    int idx = SessionIndex(info);
    if (!m_multiview) {
        // Single view: release everything else (frees USB bandwidth); connected casts stay open, unseen and unheard.
        for (size_t i = m_sessions.size(); i-- > 0;)
            if (int(i) != idx && !KeepOpen(*m_sessions[i])) CloseSession(i);
        idx = SessionIndex(info);
    }
    if (idx < 0) {
        OpenSession(info);
        idx = int(m_sessions.size()) - 1;
    }
    m_active = size_t(idx);
    m_settingsDirty = true;
}

void App::SetMultiview(bool on)
{
    if (on && m_profile.low) {
        Toast(L"Multiview is off in Low-resource mode");
        return;
    }
    m_multiview = on;
    m_settingsDirty = true;
    if (on) {
        auto wanted = [&](const VideoDeviceInfo& d) {
            if (m_cmd.multiviewDevices.empty()) return true;
            for (size_t a = 0, b; a <= m_cmd.multiviewDevices.size(); a = b + 1) {
                b = m_cmd.multiviewDevices.find(L'|', a);
                if (b == std::wstring::npos) b = m_cmd.multiviewDevices.size();
                if (b > a && d.name.find(m_cmd.multiviewDevices.substr(a, b - a)) != std::wstring::npos) return true;
            }
            return false;
        };
        for (const auto& d : m_devices.video)
            if (SessionIndex(d) < 0 && wanted(d)) OpenSession(d);
    } else {
        const Session* keep = Active();
        for (size_t i = m_sessions.size(); i-- > 0;)
            if (m_sessions[i].get() != keep && !KeepOpen(*m_sessions[i])) CloseSession(i);
        m_active = 0;
        for (size_t i = 0; i < m_sessions.size(); ++i)
            if (m_sessions[i].get() == keep) m_active = i;
    }
}

void App::UpdateAudioRouting(Session& s)
{
    if (s.video && s.video->OwnAudio()) return;   // network streams / test pattern / casts carry their own audio
    std::wstring want;
    if (s.audioChoice == L"none") {
        want.clear();
    } else if (s.audioChoice == L"auto" || s.audioChoice.empty()) {
        int i = DeviceManager::FindPairedAudio(m_devices, s.info);
        if (i >= 0) want = m_devices.audioIn[size_t(i)].id;
    } else {
        for (const auto& a : m_devices.audioIn)
            if (a.id == s.audioChoice) want = a.id;
    }
    bool restart = want != s.audioId || (s.audioCap && s.audioCap->Failed());
    if (!restart) return;
    s.audioCap.reset();
    s.audioId = want;
    s.audioSrc->Reset();
    if (!want.empty()) {
        s.audioCap = std::make_unique<AudioCapture>(want, s.audioSrc);
        s.audioCap->Start();
        s.audioStartQpc = QpcNow();
    }
}

void App::SetDeviceHidden(const std::wstring& key, bool hidden)
{
    if (hidden == m_hiddenDevices.contains(key)) return;
    if (hidden) m_hiddenDevices.insert(key);
    else m_hiddenDevices.erase(key);
    m_settingsDirty = true;
    const std::wstring name = NameForKey(key);
    LOGI(L"Device %s: %s", key.c_str(), hidden ? L"hidden" : L"shown again");
    bool closedActive = false;
    if (hidden)
        for (size_t i = m_deviceWindows.size(); i-- > 0;)
            if (m_deviceWindows[i]->key == key) CloseDeviceWindow(i, false);
    if (hidden)
        for (size_t i = m_sessions.size(); i-- > 0;)
            if (m_sessions[i]->info.Key() == key && m_sessions[i]->video->IsLocalDevice()) {
                closedActive |= i == m_active;
                CloseSession(i);
            }
    ApplyDevices(m_localDevices, false);   // shown again: it opens like a newly connected device
    if (m_advanced) SyncSceneSessions();
    else if (closedActive && !m_devices.video.empty()) SwitchTo(PickInitialDevice());
    Toast(hidden ? name + L" is hidden (Settings ▸ Devices shows it again)" : name + L" is shown again");
}

DeviceList App::WithNetworkSources(const DeviceList& local) const
{
    DeviceList all = local;
    auto add = [&](const VideoDeviceInfo& d) {
        for (const auto& e : all.video)
            if (e.symlink == d.symlink) return;
        all.video.push_back(d);
    };
    if (!m_wireless) {   // wireless features off: no network streams or casts, only the test sources
        if (m_cmd.testPattern) add({L"Test pattern", TestPattern::kLink});
        return all;
    }
    for (const auto& s : m_discovery.Sources()) {
        VideoDeviceInfo d;
        d.name = L"Network: " + s.server + L", " + s.device;
        d.symlink = L"net:" + s.address;
        add(d);
    }
    for (const auto& d : m_manualNet) add(d);
    if (m_cmd.testPattern) {
        VideoDeviceInfo d;
        d.name = L"Test pattern";
        d.symlink = TestPattern::kLink;
        add(d);
    }
    if (!m_cmd.mediaTest.empty()) {
        VideoDeviceInfo d;
        d.name = L"Wireless: test file";
        d.symlink = L"media:test";
        add(d);
    }
    for (const auto& c : m_casts) {
        VideoDeviceInfo d;
        d.name = L"Wireless: " + c.name;
        d.symlink = L"miracast:" + c.key;
        add(d);
    }
    // Keep network sessions that are open even if discovery lost them for a moment.
    for (const auto& s : m_sessions)
        if (s->video && !s->video->IsLocalDevice()) add(s->info);
    return all;
}

void App::ApplyDevices(DeviceList list, bool initial)
{
    std::vector<std::wstring> oldLinks;
    for (const auto& v : m_devices.video) oldLinks.push_back(v.symlink);
    m_localDevices = std::move(list);
    if (m_cmd.twinTest) {   // testing: two identical cards (test patterns under one name)
        m_localDevices.video.push_back({L"Twin card", L"test:twin-a"});
        m_localDevices.video.push_back({L"Twin card", L"test:twin-b"});
        DeviceManager::NumberIdentical(m_localDevices.video);
    }
    NameDevices(m_localDevices.video);   // the user's names; identical cards' settings under their new keys
    m_devices = WithNetworkSources(m_localDevices);
    std::erase_if(m_devices.video, [&](const VideoDeviceInfo& d) { return IsHidden(d); });   // hidden by the user
    LOGI(L"Devices: %zu video (%zu local), %zu audio in, %zu audio out", m_devices.video.size(), m_localDevices.video.size(),
         m_devices.audioIn.size(), m_devices.audioOut.size());

    for (auto& sp : m_sessions) {
        Session& s = *sp;
        if (!s.video->IsLocalDevice()) continue;   // network receivers reconnect on their own
        int idx = DeviceIndex(s.info);
        bool present = idx >= 0 && !s.testUnplugged;
        if (present) s.info = m_devices.video[size_t(idx)];
        CaptureState st = s.video->State();
        if (!present && s.present) {
            LOGI(L"%s disconnected", s.info.name.c_str());
            s.video->Close();
            s.surface.hasImage = false;
        } else if (present && (!s.present || st == CaptureState::Lost || st == CaptureState::Error || st == CaptureState::InUse)) {
            LOGI(L"%s available, reopening", s.info.name.c_str());
            s.video->Open(s.info, m_settings.GetString(Settings::DeviceSection(s.info.Key()), L"Format", L""), m_preferUncompressed);
            s.openedQpc = QpcNow();
        }
        s.present = present;
        UpdateAudioRouting(s);
    }

    if (initial) return;
    if (m_advanced) {
        SyncSceneSessions();   // scenes decide which devices are open
        return;
    }

    std::vector<size_t> added;
    for (size_t i = 0; i < m_devices.video.size(); ++i)
        if (std::find(oldLinks.begin(), oldLinks.end(), m_devices.video[i].symlink) == oldLinks.end()) added.push_back(i);
    for (size_t i : added) {
        const auto& d = m_devices.video[i];
        if (SessionIndex(d) >= 0) continue;
        if (NetReceiver::IsNetworkLink(d.symlink)) continue;   // network streams are only opened on request
        if (CastSource::IsCastLink(d.symlink)) continue;       // casts: UpdateWireless decides
        bool sameNameSession = false;
        for (auto& s : m_sessions)
            if (s->info.Key() == d.Key() && !s->present) sameNameSession = true;
        if (sameNameSession) continue;   // handled above as a reconnect
        if (m_multiview) {
            OpenSession(d);
            Toast(L"Connected: " + d.name);
        } else if ((m_autoSwitch && !m_recorder) || m_sessions.empty()) {
            SwitchTo(i);
            Toast(L"Switched to " + d.name);
        } else if (m_autoSwitch) {
            Toast(L"Connected: " + d.name + L" (not switching while recording)");   // switching would end the recording
        }
    }
    if (m_sessions.empty() && !m_devices.video.empty()) SwitchTo(PickInitialDevice());

    if (m_defaultOutChanged.exchange(false) && m_outputSetting.empty()) {
        LOGI(L"Default audio output changed");
        m_audio.Start(L"");
    }
}

void App::Maintain()
{
    const int64_t now = QpcNow();
    if (QpcToMs(now - m_lastMaintain) < 2000) return;
    m_lastMaintain = now;
    // Retry devices that failed while still present (e.g. in use by another app, transient errors).
    if (m_discovery.TakeChanged()) ApplyDevices(m_localDevices, false);
    SyncSceneSessions();
    for (VideoEncoder* e : {m_encoder.get(), m_shareEncoder.get()})
        if (e && e->Stalled()) e->Restart();
    MonitorEncoder();
    UpdateMic();
    // A device's crop / turn changed its picture size: the replay buffer's encoder starts again at the new size
    // (sharing follows on its own; a recording locks the size).
    if (!m_xfChangedLink.empty() && QpcElapsedMs(m_xfChangedQpc) > 700.0) {
        for (const auto& s : m_sessions)
            if (s->info.symlink == m_xfChangedLink && !m_recorder && m_encoder && m_encoderSymlink == m_xfChangedLink) {
                const EncoderConfig c = m_encoder->Config();
                if (c.width != s->surface.width || c.height != s->surface.height) {
                    LOGI(L"%s: picture now %ux%u, encoder restarts", s->info.name.c_str(), s->surface.width, s->surface.height);
                    DestroyEncoder();
                }
            }
        m_xfChangedLink.clear();
    }
    UpdateReplay();
    UpdateSharingSource();
    for (auto& s : m_sessions) {
        if (!s->video->IsLocalDevice()) continue;
        // A USB 3 card that came up at USB 2 speed (plug not fully in, USB 2 port / cable / hub) offers no fast
        // uncompressed formats (1080p60 YUY2 needs ~250 MB/s; USB 2 carries ~45): say so once per connection.
        const UsbLink link = s->video->Link();
        if (link == UsbLink::Usb3) s->usbWarned = false;
        if (link == UsbLink::Usb2OnUsb3Device && !s->usbWarned) {
            s->usbWarned = true;
            Toast(s->info.name + L" is connected at USB 2 speed, so 1080p60 isn't offered. Unplug it and plug it fully into a "
                                 L"USB 3 port (often blue), without USB 2 cables or hubs in between.");
        }
        CaptureState st = s->video->State();
        if (st == CaptureState::Running) s->errorRetries = 0;
        // Lost / busy devices are retried indefinitely; other errors a few times only.
        const bool retry = st == CaptureState::Lost || st == CaptureState::InUse || (st == CaptureState::Error && s->errorRetries < 3);
        if (s->present && !s->video->Busy() && retry && QpcToMs(now - s->openedQpc) > 3000) {
            if (st == CaptureState::Error) ++s->errorRetries;
            s->video->Open(s->info, m_settings.GetString(Settings::DeviceSection(s->info.Key()), L"Format", L""), m_preferUncompressed);
            s->openedQpc = now;
        }
        if (s->audioCap && s->audioCap->Failed()) UpdateAudioRouting(*s);
        CheckSound(*s, now);
    }
    OutputIsBluetooth();   // keeps F3's note current (cached per output device)
    if (m_audio.Failed()) {
        bool exists = m_outputSetting.empty();
        for (const auto& o : m_devices.audioOut)
            if (o.id == m_outputSetting) exists = true;
        m_audio.Start(exists ? m_outputSetting : L"");
    }
    std::erase_if(m_graveyard, [](std::future<void>& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
    if (m_settingsDirty && QpcToMs(now - m_lastSave) > 1000) SaveSettings();
}

// ---------------------------------------------------------------------------------------------
// Recording (the encoder is shared with streaming in later steps)

UINT App::BitrateKbps(int preset)
{
    static const UINT presets[] = {8000, 15000, 25000, 40000};
    return presets[Clamp(preset, 0, 3)];
}

std::wstring App::RecordFolder() const
{
    if (!m_recordFolderSetting.empty()) return m_recordFolderSetting;
    // Portable mode keeps everything next to the exe, so the folder can be carried around as a whole.
    if (m_settings.Portable()) return m_settings.Directory() + L"\\Videos";
    return KnownFolder(FOLDERID_Videos) + L"\\ZeroDisplay";
}

bool App::SetPortableMode(bool on)
{
    SaveSettings();   // current values go along
    std::wstring error;
    if (!m_settings.SetPortable(on, error)) {
        LOGW(L"Portable mode: %s", error.c_str());
        Toast(error);
        return false;
    }
    m_sceneIni.Open(m_settings.Directory() + L"\\scenes.ini");
    LOGI(L"Settings moved to %s (recordings: %s, screenshots: %s)", m_settings.Path().c_str(), RecordFolder().c_str(), ScreenshotFolder().c_str());
    Toast(on ? L"Portable: settings, videos and screenshots are now kept next to ZeroDisplay.exe" : L"Settings moved back to your user profile");
    return true;
}

std::wstring App::ScreenshotFolder() const
{
    if (!m_shotFolderSetting.empty()) return m_shotFolderSetting;
    if (m_settings.Portable()) return m_settings.Directory() + L"\\Screenshots";
    return KnownFolder(FOLDERID_Pictures) + L"\\ZeroDisplay";
}

void App::DisplayWatch()
{
    SetCurrentThreadName(L"Display watch");
    SetThreadEfficiency(true);
    ComPtr<IDXGIFactory1> factory;
    while (!m_displayQuit.load()) {
        DisplayInfo d;
        const HMONITOR monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (GetMonitorInfoW(monitor, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
            d.width = dm.dmPelsWidth;
            d.height = dm.dmPelsHeight;
            d.hz = dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 60;   // 0 / 1 = "hardware default"
        }
        d.hdr = Renderer::QueryHdrDisplay(monitor, factory);
        bool changed = false;
        {
            std::lock_guard lock(m_displayMutex);
            const DisplayInfo& o = m_displayInfo;
            changed = o.seq == 0 || o.width != d.width || o.height != d.height || o.hz != d.hz || o.hdr.hdr != d.hdr.hdr ||
                      o.hdr.peakNits != d.hdr.peakNits || o.hdr.sdrWhiteNits != d.hdr.sdrWhiteNits;
            if (changed) {
                d.seq = o.seq + 1;
                m_displayInfo = d;
            }
        }
        if (changed) SetEvent(m_wake);
        RefreshThreadEfficiency();
        WaitForSingleObject(m_displayWake, 1000);
    }
}

void App::UpdateDisplayHz()
{
    // Applies what the display-watch thread found (cheap); the HDR decision is re-made once a second too, because it
    // also follows the picture (an HDR source appearing).
    DisplayInfo d;
    bool fresh = false;
    {
        std::lock_guard lock(m_displayMutex);
        if (m_displayInfo.seq != m_displaySeen) {
            d = m_displayInfo;
            m_displaySeen = d.seq;
            fresh = true;
        }
    }
    const int64_t now = QpcNow();
    if (fresh && d.hz) {
        const bool modeChanged = m_displayChangedQpc.load() > m_displayHzChecked;
        if (d.hz != m_displayHz || modeChanged) LOGI(L"Display: %ux%u @ %u Hz%s", d.width, d.height, d.hz, modeChanged ? L" (changed)" : L"");
        m_displayHz = d.hz;
    }
    bool hdrChanged = false;
    if (fresh) {
        const Renderer::HdrDisplay& h = d.hdr;
        hdrChanged = h.hdr != m_hdrDisplayInfo.hdr || h.peakNits != m_hdrDisplayInfo.peakNits || h.sdrWhiteNits != m_hdrDisplayInfo.sdrWhiteNits;
        m_hdrDisplayInfo = h;
    }
    if (!fresh && m_displayHzChecked && QpcToMs(now - m_displayHzChecked) < 1000) return;
    m_displayHzChecked = now;
    UpdateHdrOutput(hdrChanged);
}

// HDR screens: the window switches to 16-bit HDR output while an HDR picture is shown on a monitor with Windows HDR on
// (and back to SDR otherwise: SDR output costs less). Checked once a second and when the display changes.
void App::UpdateHdrOutput(bool displayChanged)
{
    if (m_cmd.hdrOutputTest) m_hdrDisplayInfo = Renderer::HdrDisplay{true, 1000.0f, 200.0f};
    const Renderer::HdrDisplay d = m_hdrDisplayInfo;
    const Session* s = Active();
    const bool hdrPicture = s && !m_advanced && s->surface.hasImage && s->surface.transfer != Transfer::Sdr;
    const bool want = HdrOn() && d.hdr && m_hdrScreen && hdrPicture && !m_profile.low && !(m_multiview && SimpleSessionCount() > 1);
    if (want != m_renderer.HdrOutput() || (want && displayChanged)) {
        if (!m_renderer.SetHdrOutput(want, d) && want) m_hdrScreenFailed = true;
        InvalidateUi();
    }
}

bool App::SourceFasterThanScreen() const
{
    double fastest = 0;
    for (const auto& s : m_sessions)
        if (s->video) fastest = std::max(fastest, s->video->Stats().fps);
    return fastest > m_displayHz + 3.0;   // 120 / 144 fps on a 60 Hz screen; not 60 fps on 59.94 Hz
}

double App::DisplayAspect(UINT w, UINT h, int aspect)
{
    if (!w || !h) return 1.0;
    switch (aspect) {
    case 1: return 4.0 / 3.0;
    case 2: return 16.0 / 9.0;
    case 3: return double(w) / h;
    default: break;
    }
    const bool sd = (w == 720 || w == 704) && (h == 480 || h == 486 || h == 576 || h == 240 || h == 288);
    return sd ? 4.0 / 3.0 : double(w) / h;
}

bool App::EnsureEncoder(Session& s)
{
    if (m_encoder && m_encoder->Alive() && m_encoderSymlink == s.info.symlink &&
        (m_recorder || (EncoderUpToDate() && m_encoderHdrAsked == WantHdrRecording(s))))
        return true;
    if (m_recorder && m_encoder) return false;   // the recording owns the current encoder until it stops
    if (!s.surface.hasImage) {
        Toast(L"No picture to encode yet");
        return false;
    }
    VideoFormat f = s.video->Current();
    EncoderConfig cfg;
    cfg.width = s.surface.width;
    cfg.height = s.surface.height;
    cfg.fpsNum = f.fpsNum ? f.fpsNum : 60;
    cfg.fpsDen = f.fpsDen ? f.fpsDen : 1;
    cfg.bitrateKbps = BitrateKbps(m_quality);
    cfg.gopFrames = UINT(std::lround(f.Fps() > 0 ? f.Fps() * 2 : 120));   // a keyframe every 2 s
    cfg.hdr = WantHdrRecording(s);
    cfg.hdrWhiteNits = s.surface.hdrWhite;
    return StartEncoder(cfg, s.info.symlink);
}

bool App::StartEncoder(EncoderConfig cfg, const std::wstring& link)
{
    cfg.hevc = WantHevc() || cfg.width > 4096 || cfg.height > 4096;   // H.264 encoders stop at 4096: HEVC first, H.264 (software) after
    if (cfg.hdr) cfg.hevc = true;   // HDR10 is HEVC Main10
    if (m_encoderNext.valid()) {
        if (m_encoderNext.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;   // still starting
        EncoderJob job = m_encoderNext.get();
        const EncoderConfig& b = m_encoderNextBase;
        const bool same = m_encoderNextLink == link && b.width == cfg.width && b.height == cfg.height && b.fpsNum == cfg.fpsNum &&
                          b.fpsDen == cfg.fpsDen && b.bitrateKbps == cfg.bitrateKbps && b.gopFrames == cfg.gopFrames && b.hdr == cfg.hdr &&
                          m_encoderNextRequest == WantedEncoder(cfg);
        if (same && job.encoder) {
            const EncoderConfig& want = m_encoderNextTry.front();
            const EncoderConfig& got = m_encoderNextTry[job.index];
            m_replay.reset();   // never leave the replay buffer attached to an encoder that is about to go away
            RetireEncoder(std::move(m_encoder));
            m_encoder = std::move(job.encoder);
            if (want.target == EncodeTarget::OtherGpu && got.target != EncodeTarget::OtherGpu) m_hybridBroken = true;   // not again this session
            if (got.target == EncodeTarget::SameGpu && m_encoder->Target() == EncodeTarget::Cpu) {
                m_displayNoEncoder[got.hevc ? 1 : 0] = true;   // it fell back to software: plan for that from now on
                LOGW(L"The display GPU has no usable hardware %s encoder", got.hevc ? L"HEVC" : L"H.264");
            }
            m_encoderHdrAsked = want.hdr;
            if (want.hdr && !got.hdr)
                Toast(L"This PC can't encode 10-bit HDR (HEVC Main10): the HDR source is recorded tone-mapped, as SDR");
            else if (got.hevc != want.hevc) Toast(L"HEVC isn't available on this PC, recording H.264");
            else if (m_hybridBroken && want.target == EncodeTarget::OtherGpu) Toast(L"The second GPU's encoder didn't start, recording on the display GPU");
            // Auto without a learned answer for this format: watch the first seconds (MonitorEncoder).
            m_autoCalibrating = EncoderSpeed() == 0 && !m_autoLearned.count(AutoKey(m_encoder->Config()));
            m_autoSince = QpcNow();
            m_encoderRequest = WantedEncoder(cfg);
            m_encoderSymlink = link;
            ++m_encoderGen;
            return true;
        }
        RetireEncoder(std::move(job.encoder));
        if (same) {
            Toast(L"Could not start the video encoder");
            return false;
        }
        // The settings or the source changed while it started: start again.
    }
    // Whether a second GPU can encode: looked up once (~100 ms), in the background.
    if ((EncodeHw() == 0 || EncodeHw() == 1) && !ProbeOtherGpu(cfg.hevc)) return false;

    // Candidates in order: the chosen hardware, the display GPU (or software) when that was another GPU,
    // then the same again in H.264 when HEVC can't be encoded at all.
    std::vector<EncoderConfig> tries;
    // HDR: 10-bit HEVC on the chosen hardware / the display GPU first, then everything again as SDR (tone-mapped).
    std::vector<std::pair<bool, bool>> codecs;   // (hevc, hdr)
    if (cfg.hdr) {
        codecs.push_back({true, true});
        const bool sdrHevc = WantHevc() || cfg.width > 4096 || cfg.height > 4096;
        codecs.push_back({sdrHevc, false});
        if (sdrHevc) codecs.push_back({false, false});
    } else {
        codecs.push_back({cfg.hevc, false});
        if (cfg.hevc) codecs.push_back({false, false});
    }
    for (const auto& [hevc, hdr] : codecs) {
        EncoderConfig c = cfg;
        c.hevc = hevc;
        c.hdr = hdr;
        ResolveTarget(c, false);
        c.qualityVsSpeed = QvsForPreset(DesiredPreset(c));
        tries.push_back(c);
        if (c.target == EncodeTarget::OtherGpu) {
            c.target = DisplayTarget(hevc);
            c.otherAdapter = {};
            c.otherAdapterName.clear();
            c.qualityVsSpeed = QvsForPreset(DesiredPreset(c));
            tries.push_back(c);
        }
    }
    m_encoderNextLink = link;
    m_encoderNextBase = cfg;
    m_encoderNextTry = tries;
    m_encoderNextRequest = WantedEncoder(cfg);
    m_encoderNext = std::async(std::launch::async, [device = ComPtr<ID3D11Device>(m_renderer.Device()),
                                                    manager = ComPtr<IMFDXGIDeviceManager>(m_renderer.DxgiManager()), tries] {
        ComInit com;
        SetCurrentThreadName(L"Encoder start");
        EncoderJob job;
        for (size_t i = 0; i < tries.size() && !job.encoder; ++i) {
            auto e = std::make_unique<VideoEncoder>();
            if (e->Start(device.Get(), manager.Get(), tries[i])) {
                job.encoder = std::move(e);
                job.index = i;
            }
        }
        return job;
    });
    return false;
}

void App::DropEncoderJob()
{
    if (!m_encoderNext.valid()) return;
    m_graveyard.push_back(std::async(std::launch::async, [f = std::move(m_encoderNext)]() mutable {
        ComInit com;
        if (EncoderJob job = f.get(); job.encoder) job.encoder->Stop();
    }));
}

void App::RetireEncoder(std::unique_ptr<VideoEncoder> e)
{
    if (!e) return;
    m_graveyard.push_back(std::async(std::launch::async, [e = std::move(e)]() mutable {
        ComInit com;
        e->Stop();
    }));
}

void App::PollEncoderJob()
{
    auto ready = [](const auto& f) { return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; };
    if (ready(m_shareStart)) UpdateSharingSource();   // a sharing encoder is ready: viewers get it now
    if (ready(m_otherProbe)) {
        // The second-GPU lookup finished: the encoder can be chosen now.
        m_otherProbe.get();
        if (m_recordPending) StartRecording();
        else if (m_replayOn) UpdateReplay();
    }
    if (!ready(m_encoderNext)) return;
    if (m_recordPending) StartRecording();   // installs the encoder (or starts a new one if things changed)
    if (ready(m_encoderNext)) UpdateReplay();
    // Nobody wants it any more (recording cancelled, replay turned off): throw it away.
    if (ready(m_encoderNext)) DropEncoderJob();
}

void App::DestroyEncoder()
{
    StopRecording();
    DropEncoderJob();
    m_replay.reset();
    RetireEncoder(std::move(m_encoder));
    m_encoderSymlink.clear();
}

void App::ReleaseEncoderIfIdle()
{
    if (!m_encoder) return;
    if (m_recorder || m_replay) return;
    RetireEncoder(std::move(m_encoder));
    m_encoderSymlink.clear();
}

void App::StartRecording()
{
    if (m_recorder) return;
    m_recordPending = true;   // until the encoder is ready (PollEncoderJob calls again)
    Session* s = Active();
    const bool ready = m_advanced ? EnsureCanvasEncoder() : (s && EnsureEncoder(*s));
    if (!ready) {
        if (!m_encoderNext.valid() && !m_otherProbe.valid()) m_recordPending = false;   // failed (it said why), not just starting
        return;
    }
    m_recordPending = false;
    std::wstring folder = RecordFolder();
    if (!EnsureFolder(folder, L"the recording")) {
        ReleaseEncoderIfIdle();
        return;
    }
    ULARGE_INTEGER freeBytes{};
    if (GetDiskFreeSpaceExW(folder.c_str(), &freeBytes, nullptr, nullptr) && freeBytes.QuadPart < MinFreeBytes()) {
        Toast(Format(L"Not enough disk space to record (less than %d GB free; Settings ▸ Files)", m_minFreeGb));
        LOGW(L"Recording not started: %.1f GB free in %s, the limit is %d GB", double(freeBytes.QuadPart) / double(1ull << 30), folder.c_str(), m_minFreeGb);
        ReleaseEncoderIfIdle();
        return;
    }
    std::wstring path = NewFilePath(folder, L"ZeroDisplay", L".mp4");
    RecordOptions opt;
    opt.tracks = RecordTracks();
    opt.splitSeconds = m_cmd.splitSec > 0 ? m_cmd.splitSec : m_splitMinutes * 60;
    m_recUsesMixer = m_mixer.Running() && !opt.tracks.empty() && opt.tracks[0] == m_mixer.Output();
    m_recorder = std::make_unique<Recorder>();
    m_recorder->Start(path, *m_encoder, opt);
    m_headerPartsShown = 0;
    m_hybridMovedBack = false;
    if (m_multiview && !m_advanced && SimpleSessionCount() > 1) StartExtraRecordings(path, opt.splitSeconds);
    m_behindWarned = false;
    m_diskWarned = false;
    const double minutesLeft = DiskMinutesLeft(freeBytes.QuadPart);
    if (minutesLeft < 30.0) {
        m_diskWarned = minutesLeft < 5.0;
        Toast(Format(L"Recording started, disk space for only about %.0f minutes", std::max(1.0, minutesLeft)));
    } else {
        Toast(m_encoder->Config().hevc ? L"Recording started (HEVC)" : L"Recording started");
    }
    PlayCue(Cue::Start);
}

void App::StopRecording(const std::wstring& reason)
{
    m_recordPending = false;
    StopExtraRecordings();
    if (!m_recorder) return;
    PlayCue(Cue::Stop);
    std::wstring path = m_recorder->Path();
    m_recorder->RequestStop();
    m_recUsesMixer = false;
    // Finalizing the file can take a moment; do it off the render thread.
    const int parts = m_recorder->Parts();
    m_graveyard.push_back(std::async(std::launch::async, [this, r = std::move(m_recorder), path, reason, parts]() mutable {
        EfficiencyScope eco;
        r->Stop();
        std::wstring err = r->Error();
        // A file deleted while it was being written (e.g. in Explorer) is gone now: never say it was saved.
        int missing = 0;
        for (int part = 1; part <= parts; ++part)
            if (GetFileAttributesW(r->PartPath(part).c_str()) == INVALID_FILE_ATTRIBUTES) ++missing;
        if (missing) LOGW(L"Recording: %d of %d file(s) were deleted while recording", missing, parts);
        if (err.empty() && missing == parts) err = parts > 1 ? L"the files were deleted while recording" : L"the file was deleted while recording";
        if (err.empty()) {
            for (int part = 1; part <= parts; ++part) RememberMadeFile(r->PartPath(part));
            EnforceKeepLimit();
        }
        std::wstring msg = err.empty() ? (parts > 1 ? Format(L"Saved %d parts, last: ", parts) : std::wstring(L"Saved ")) + path
                                       : L"Recording failed: " + err;
        if (err.empty() && missing) msg += Format(L" (%d part(s) were deleted while recording)", missing);
        if (err.empty()) ToastFile(reason.empty() ? msg : reason + L". " + msg, path);
        else Toast(reason.empty() ? msg : reason + L". " + msg);
        r.reset();
    }));
    ReleaseEncoderIfIdle();
}

void App::ToggleRecording()
{
    if (m_recorder) StopRecording();
    else if (m_recordPending) {
        m_recordPending = false;   // pressed again while the encoder was starting
        Toast(L"Recording cancelled");
    } else StartRecording();
}

void App::UpdateReplay()
{
    if (!m_replayOn || !ReplayAllowed()) {   // off on battery, in Memory saver and in Low-resource mode
        if (m_replay) {
            m_replay.reset();
            ReleaseEncoderIfIdle();
        }
        return;
    }
    Session* s = Active();
    std::shared_ptr<AudioSource> audio;
    if (m_advanced) {
        if (!EnsureCanvasEncoder()) return;
        audio = ProgramAudioSource();
    } else {
        if (!s) return;
        // While recording, the replay buffer uses the recording's encoder (whatever device that is).
        const bool bound = m_encoder && m_encoder->Alive() && (m_encoderSymlink == s->info.symlink || m_recorder);
        if (!bound && (!s->surface.hasImage || !EnsureEncoder(*s))) return;
        audio = RecordAudioSource();
    }
    if (m_replay && m_replayGen == m_encoderGen) return;
    m_replay = std::make_unique<ReplayBuffer>();
    m_replay->Start(*m_encoder, audio, m_replaySeconds);
    m_replayGen = m_encoderGen;
}

void App::SaveReplay()
{
    if (!m_replay) {
        Toast(L"Turn on the replay buffer in Settings \u25B8 Recording first");
        return;
    }
    std::vector<PacketPtr> video;
    std::vector<AudioChunk> audio;
    EncoderConfig cfg;
    if (!m_replay->Snapshot(video, audio, cfg)) {
        Toast(L"Nothing in the replay buffer yet");
        return;
    }
    std::wstring folder = RecordFolder();
    if (!EnsureFolder(folder, L"the replay")) return;
    const std::wstring path = NewFilePath(folder, L"Replay", L".mp4");
    m_lastReplaySave = QpcNow();
    Toast(L"Saving replay\u2026");
    m_graveyard.push_back(std::async(std::launch::async, [this, path, cfg, video = std::move(video), audio = std::move(audio)] {
        EfficiencyScope eco;
        Recorder writer;
        const bool ok = writer.WriteClip(path, cfg, video, audio);
        if (ok) ToastFile(L"Replay saved: " + path, path);
        else Toast(L"The replay could not be saved");
        if (ok) {
            PlayCue(Cue::Saved);
            RememberMadeFile(path);
            EnforceKeepLimit();
        }
    }));
}

// ---------------------------------------------------------------------------------------------
// LAN sharing: the stream server runs while sharing is on and follows the active device.

void App::StartSharing()
{
    if (m_server) return;
    if (!m_wireless) {
        Toast(L"Sharing needs wireless features: Settings ▸ General ▸ Wireless features");
        return;
    }
    m_server = std::make_unique<StreamServer>();
    m_server->SetPinGate(&m_pinGate);
    if (!m_server->Start()) {
        Toast(L"Sharing failed: " + m_server->Error());
        m_server.reset();
        return;
    }
    m_web = std::make_unique<WebServer>();
    m_web->SetPinGate(&m_pinGate);
    if (!m_web->Start(8080)) {
        LOGW(L"Web viewer unavailable: %s", m_web->Error().c_str());
        m_web.reset();
    }
    UpdateSharingSource();
    {
        std::wstring ips;
        for (const auto& ip : net::LocalIPv4()) ips += (ips.empty() ? L"" : L", ") + ip;
        LOGI(L"Sharing on: %s (browser viewer on port 8080)", ips.c_str());
    }
    Toast(m_pinGate.Required() ? L"Sharing on the local network, PIN " + Widen(m_pinGate.Pin()) : std::wstring(L"Sharing on the local network, no PIN"));
}

// --- Sharing PIN ---

void App::ApplyViewerPin()
{
    const std::string runPin = Narrow(m_cmd.pin);
    m_pinGate.Set(m_viewerPinOn, net::PinGate::Valid(runPin) ? runPin : m_viewerPin);
}

void App::SetViewerPin(bool on, const std::string& pin)
{
    const bool stricter = (on && !m_viewerPinOn) || (on && pin != m_viewerPin);
    m_viewerPinOn = on;
    m_viewerPin = pin;
    m_settingsDirty = true;
    ApplyViewerPin();
    if (stricter) {   // whoever watches with no PIN or the old one is asked for the new one
        if (m_server) m_server->DropViewers();
        if (m_web) m_web->DropViewers();
    }
    LOGI(L"Sharing PIN %s", on ? (stricter ? L"changed" : L"on") : L"off");
}

std::wstring App::NetPinKey(const VideoDeviceInfo& info) const
{
    const std::wstring address = info.symlink.substr(4);   // "net:ip:port"
    std::wstring key;
    for (const auto& src : m_discovery.Sources())
        if (src.address == address && !src.server.empty()) key = L"pc." + src.server;
    if (key.empty()) key = L"ip." + address.substr(0, address.rfind(L':'));
    for (auto& ch : key)
        if (ch == L'=' || ch == L'[' || ch == L']' || ch == L';' || ch < 32) ch = L'_';
    return key;
}

std::string App::NetPinFor(const VideoDeviceInfo& info)
{
    if (!m_cmd.connect.empty() && info.symlink == L"net:" + m_cmd.connect && !m_cmd.pin.empty()) return Narrow(m_cmd.pin);
    bool ok = false;
    const std::string pin = Narrow(UnprotectString(m_settings.GetString(L"NetworkPins", NetPinKey(info).c_str(), L""), ok));
    return ok && net::PinGate::Valid(pin) ? pin : std::string();
}

void App::RememberNetPin(const VideoDeviceInfo& info, const std::string& pin)
{
    m_settings.SetString(L"NetworkPins", NetPinKey(info).c_str(), ProtectString(Widen(pin)));
}

void App::StopSharing()
{
    if (!m_server) return;
    m_server->Stop();
    m_server.reset();
    if (m_web) {
        m_web->Stop();
        m_web.reset();
    }
    DestroyShareEncoder();
}

void App::ToggleSharing()
{
    if (!m_server && !m_wireless) {
        StartSharing();   // says why it can't; the saved choice stays as it was
        return;
    }
    if (m_server) StopSharing();
    else StartSharing();
    if (!m_cmd.share) {
        m_shareOnStart = m_server != nullptr;
        m_settingsDirty = true;
    }
}

void App::DetachSharing()
{
    if (m_server) m_server->Detach();
    if (m_web) m_web->Detach();
    m_sharedGen = 0;
}

int App::ShareViewers() const
{
    return (m_server ? m_server->Viewers() : 0) + (m_web ? m_web->Viewers() : 0);
}

void App::UpdateSharingSource()
{
    if (!m_server) return;
    // What to share: the program in Advanced mode, the device on screen in Simple mode.
    std::wstring link, name;
    UINT w = 0, h = 0, fpsNum = 60, fpsDen = 1;
    std::shared_ptr<AudioSource> audio;
    if (m_advanced) {
        link = L"#canvas";
        name = L"Program";
        w = m_scenes.width;
        h = m_scenes.height;
        fpsNum = m_scenes.fpsNum;
        fpsDen = m_scenes.fpsDen;
        audio = ProgramAudioSource();
    } else {
        Session* s = Active();
        if (!s || s->video->IsNetwork() || !s->surface.hasImage) return;   // don't re-share a stream we receive
        link = s->info.symlink;
        name = s->info.name;
        w = s->surface.width;
        h = s->surface.height;
        const VideoFormat f = s->video->Current();
        if (f.fpsNum) {
            fpsNum = f.fpsNum;
            fpsDen = std::max(1u, f.fpsDen);
        }
        audio = RecordAudioSource();
    }
    // Low-resource mode shares at most 720p (1280x720, or 720x1280 for a turned picture): about half the encoding work
    // and a picture that suits its 6 Mbps cap. The encoder's colour conversion scales on the way.
    if (m_profile.low && w && h) {
        const double s = std::min({1.0, 1280.0 / std::max(w, h), 720.0 / std::min(w, h)});
        if (s < 1.0) {
            w = UINT(std::lround(w * s)) & ~1u;
            h = UINT(std::lround(h * s)) & ~1u;
        }
    }
    const bool bound = m_shareEncoder && m_shareEncoder->Alive() && m_shareSymlink == link && m_shareEncoder->Config().width == w &&
                       m_shareEncoder->Config().height == h;
    if (bound && m_sharedGen == m_shareEncGen) return;
    if (!bound) {
        // The encoder starts on a worker thread (0.3 s or more), so the picture never stops; the old one
        // keeps serving viewers until the new one is ready.
        if (m_shareStart.valid()) {
            if (m_shareStart.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            std::unique_ptr<VideoEncoder> enc = m_shareStart.get();
            if (enc && m_shareStartLink == link && enc->Config().width == w && enc->Config().height == h) {
                DestroyShareEncoder();
                m_shareEncoder = std::move(enc);
                m_shareSymlink = link;
                ++m_shareEncGen;
                LOGI(L"Sharing encoder: %s (%s) at %.1f Mbps (%s)", m_shareEncoder->Name().c_str(), m_shareEncoder->TargetName().c_str(),
                     m_shareEncoder->Config().bitrateKbps / 1000.0, StreamQuality() == 0 ? L"smart" : L"fixed");
                m_server->SetSource(m_shareEncoder.get(), audio, name);
                if (m_web) m_web->SetSource(m_shareEncoder.get(), audio, name);
                m_sharedGen = m_shareEncGen;
                return;
            }
            const bool failed = !enc && m_shareStartLink == link;
            RetireEncoder(std::move(enc));
            if (failed) {
                Toast(L"Could not start the encoder for sharing");
                return;   // tried again by Maintain in 2 s
            }
        }
        EncoderConfig cfg;
        cfg.width = w;
        cfg.height = h;
        cfg.fpsNum = fpsNum;
        cfg.fpsDen = fpsDen;
        cfg.bitrateKbps = StreamStartKbps();
        cfg.gopFrames = std::max(1u, fpsNum / fpsDen);   // a keyframe every second: viewers recover fast
        cfg.hevc = false;                                 // every viewer can decode H.264
        cfg.qualityVsSpeed = 0;   // fastest preset: lowest latency, and leaves the GPU room for the recording encoder
        ResolveTarget(cfg, true);   // display GPU (zero copy), or software when chosen
        m_shareStartLink = link;
        m_shareStart = std::async(std::launch::async, [device = ComPtr<ID3D11Device>(m_renderer.Device()),
                                                       manager = ComPtr<IMFDXGIDeviceManager>(m_renderer.DxgiManager()), cfg] {
            ComInit com;
            SetCurrentThreadName(L"Encoder start (sharing)");
            auto e = std::make_unique<VideoEncoder>();
            if (!e->Start(device.Get(), manager.Get(), cfg)) e.reset();
            return e;
        });
        return;
    }
    m_server->SetSource(m_shareEncoder.get(), audio, name);
    if (m_web) m_web->SetSource(m_shareEncoder.get(), audio, name);
    m_sharedGen = m_shareEncGen;
}

// ---------------------------------------------------------------------------------------------
// App thread main loop

void App::AppThread()
{
    ComInit com;
    SetCurrentThreadName(L"App/render");
    MmcssScope mmcss(L"Games", 1);
    m_startQpc = QpcNow();
    m_lastMoveQpc = m_startQpc;

    VideoEncoder::s_forceSoftware = m_cmd.softwareEncoder;
    VideoEncoder::s_testAlign = m_cmd.encodeAlign;
    VideoEncoder::s_testSlowOther = m_cmd.hybridSlowTest;
    VideoEncoder::s_testSlowSame = m_cmd.displaySlowTest;
    VideoEncoder::s_videoProcessorOnly = m_cmd.encodeConvertVp;
    AudioSource::s_testSkew = m_cmd.audioSkew / 100.0;
    NetReceiver::s_testLoss = m_cmd.netLoss / 100.0;
    VideoEncoder::s_testStall = m_cmd.encoderStallTest;
    m_renderer.SetGpuChoice(m_cmd.gpu != -3 ? m_cmd.gpu : m_gpuChoice);
    m_renderer.SetSuperResSim(m_cmd.superresSim);
    if (!m_renderer.Init(m_hwnd, m_clientW, m_clientH)) {
        MessageBoxW(m_hwnd, L"Could not initialize Direct3D 11. See log.txt for details.", L"ZeroDisplay", MB_ICONERROR);
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        return;
    }
    m_ui.Init(m_renderer.D2D(), m_renderer.DWrite());
    m_compositor.Init(m_renderer);
    m_gpuProf.Init(m_renderer.Device(), m_renderer.Context());
    CheckWeakPc();
    UpdateProfile();   // before anything starts: the replay buffer, the second GPU, ... follow it from the first frame
    CheckCrashReport();
    if (m_cmd.wirelessView) m_wirelessView = m_receiverOn = true;
    if (m_cmd.hotspot) m_hotspotOn = true;
    if (m_cmd.audioSim)   // about a minute of simulation: off the render thread, so the window draws meanwhile
        m_graveyard.push_back(std::async(std::launch::async, [] { AudioSelfTest(); }));
    if (m_cmd.portalTest) {
        LOGI(L"[selftest] DNS parser: %s", CaptivePortal::SelfTest() ? L"PASS" : L"FAIL");
        NetReceiver::SelfTest();
    }
    if (m_cmd.pinTest) net::PinGate::SelfTest();
    if (!m_cmd.qrTest.empty()) {
        const QrCode q = QrCode::Encode(Narrow(m_cmd.qrTest));
        LOGI(L"QRTEST size %d", q.size);
        for (int y = 0; y < q.size; ++y) {
            std::wstring row;
            for (int x = 0; x < q.size; ++x) row += q.Dark(x, y) ? L'1' : L'0';
            LOGI(L"QRROW %s", row.c_str());
        }
    }

    m_audio.volume = m_volume;
    m_audio.muted = m_muted;
    m_audio.passthrough = m_passthrough;
    m_audio.latency = m_latency;
    m_audio.avOffsetMs = std::max(0, m_delayMs);   // follows the shown device from now on (SyncProfile)
    m_audio.exclusive = m_exclusiveAudio || m_cmd.exclusive;   // the command-line flag is not saved
    m_audio.Start(m_outputSetting);
    m_displayWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_displayThread = std::thread([this] { DisplayWatch(); });

    if (m_wireless) m_discovery.Start();
    ApplyDevices(DeviceManager::Enumerate(), true);
    if (!m_cmd.connect.empty()) {
        VideoDeviceInfo d;
        d.symlink = L"net:" + m_cmd.connect;
        d.name = L"Network: " + m_cmd.connect;
        m_manualNet.push_back(d);
        ApplyDevices(m_localDevices, true);
        SwitchTo(size_t(DeviceIndex(d)));
    } else if (!m_devices.video.empty()) {
        SwitchTo(PickInitialDevice());
        if (m_multiview && !m_profile.low) SetMultiview(true);
    }
    if (m_advanced || m_cmd.advanced) {
        m_advanced = false;
        SetAdvanced(true);
        if (m_cmd.demoScene) BuildDemoScene();
        if (m_cmd.studio && !m_profile.low) {
            m_studio = true;
            m_scenes.preview = 0;
            SyncSceneSessions();
        }
    }
    if ((m_cmd.share || m_shareOnStart) && m_wireless) StartSharing();
    if (!m_cmd.exportSettings.empty()) {
        SaveSettings();
        LOGI(L"[test] export: %s", ExportSettings(m_cmd.exportSettings) ? L"written" : L"FAILED");
    }
    if (!m_cmd.importSettings.empty()) {
        m_importPath = m_cmd.importSettings;
        m_onExit = OnExit::Import;
    }
    if (m_cmd.resetSettings) m_onExit = OnExit::Reset;
    for (const auto& d : std::vector<VideoDeviceInfo>(m_devices.video))   // device windows that were open at the last exit
        if ((m_settings.GetBool(Settings::DeviceSection(d.Key()), L"WindowOpen", false) && ShownInSimple(d) && !Settings::s_readOnly) ||
            (!m_cmd.deviceWindow.empty() && d.name.find(m_cmd.deviceWindow) != std::wstring::npos))
            OpenDeviceWindow(d);
    if (m_cmd.clean) SetClean(true);
    if (m_cmd.mini) SetMini(true);

    while (!m_quit) {
        bool busy = (m_advanced && (m_encoder || m_transitioning)) || m_encoderNext.valid() || m_otherProbe.valid() || m_shareStart.valid() || (m_barAlpha > 0.0f && m_barAlpha < 1.0f) || m_showStats || m_ui.Animating() || m_ui.PopupOpen() || m_toastUntil > QpcNow() || m_rescanAt.load() != 0 ||
                    m_enumFuture.valid() || m_cmd.selftestSec > 0 || m_settingsOpen ||
                    m_renderer.SuperResProbeResult() == Renderer::SuperResProbe::Running;
        HANDLE handles[2] = {m_frameEvent, m_wake};
        DWORD waitMs = std::min<DWORD>(busy ? 16 : 250, MsUntilNextEncode());
        if (m_barCanHide && m_barAlpha >= 1.0f)   // wake when the bar is due to hide
            waitMs = std::min<DWORD>(waitMs, DWORD(std::clamp(QpcToMs(BarHideAt() - QpcNow()) + 1.0, 0.0, 250.0)));
        WaitForMultipleObjects(2, handles, FALSE, waitMs);
        if (m_quit) break;

        if (m_resizePending.exchange(false)) m_renderer.Resize(m_clientW, m_clientH);
        PollRescan();
        Maintain();
        CheckRecordingLimits();   // cheap; time limits stop on the second
        if (m_renderer.Lost() || m_recreateRenderer) {
            LOGW(m_renderer.Lost() ? L"Recreating renderer after GPU device loss" : L"Recreating renderer (graphics card changed in Settings)");
            m_recreateRenderer = false;
            HandleDeviceLost();
            m_swapSlot = false;
        }

        // Hidden (minimized / fully covered)? Checked every 250 ms; the swap chain doesn't pace frames meanwhile.
        if (IsIconic(m_hwnd)) {
            m_hidden = true;
        } else if (m_hidden || m_renderer.Occluded()) {
            const int64_t t = QpcNow();
            if (!m_hidden || QpcElapsedMs(m_hiddenCheck) >= 250) {
                m_hiddenCheck = t;
                m_hidden = !m_renderer.Visible();
                if (!m_hidden) InvalidateUi();
            }
        }
        UpdateProfile();
        // 30 fps limit: every other 60 fps frame is uploaded, recorded and shared but not shown (and the swap chain,
        // which only paces shown frames, isn't waited on for it).
        m_capSkip = PreviewCapped() && m_lastPresentQpc && QpcElapsedMs(m_lastPresentQpc) < 30.0;
        // Wait until the swap chain can take a frame *before* grabbing the newest capture frame.
        // A slot taken but not used (nothing new was drawn last time) is still ours: waiting again would stall 100 ms.
        // A source faster than the screen (120 / 144 fps on a 60 Hz display) is presented without waiting for the refresh
        // (as with VSync off), so every frame is taken, recorded and shared; the screen shows the newest.
        UpdateDisplayHz();
        m_fastSource = SourceFasterThanScreen();
        if (!m_hidden && !m_capSkip && !m_swapSlot) m_swapSlot = WaitForSingleObjectEx(m_renderer.Waitable(), 100, TRUE) == WAIT_OBJECT_0;
        Frame();
        SelftestTick();
    }

    LOGI(L"Shutting down");
    m_displayQuit = true;
    if (m_displayWake) SetEvent(m_displayWake);
    if (m_displayThread.joinable()) m_displayThread.join();
    if (m_displayWake) CloseHandle(m_displayWake);
    m_displayWake = nullptr;
    while (!m_deviceWindows.empty()) {   // remembered as open: they come back at the next start
        m_deviceWindows.back()->out = OutputWindow{};
        HWND h = m_deviceWindows.back()->hwnd.load();
        WINDOWPLACEMENT wp{sizeof(wp)};
        if (h && !m_deviceWindows.back()->fullscreen && GetWindowPlacement(h, &wp)) {
            const RECT& r = wp.rcNormalPosition;
            m_settings.SetString(Settings::DeviceSection(m_deviceWindows.back()->key), L"Window",
                                 Format(L"%ld,%ld,%ld,%ld,%d", r.left, r.top, r.right, r.bottom, wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0));
        }
        m_closedWindows.push_back(std::move(m_deviceWindows.back()));
        m_deviceWindows.pop_back();
    }
    SaveSettings();
    ApplySettingsChangeOnExit();
    StopSharing();
    StopRecording();
    StopFlashback();
    SetMini(false);
    StopLagCheck(false);
    m_replay.reset();
    m_mixer.Stop();
    m_micCap.reset();
    if (m_micSrc) m_audio.RemoveSource(m_micSrc.get());
    m_discovery.Stop();
    // Wireless steps, closing devices, stopping encoders, saving files: each gets 5 s; one that hangs (a Windows service
    // not answering) is named and left behind (its future would block in its destructor), the exit goes on and
    // main.cpp ends the process shortly after.
    static auto* leftBehind = new std::vector<std::future<void>>();   // never destroyed, on purpose
    auto bounded = [](const wchar_t* what, std::future<void> f) {
        if (f.valid() && f.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            LOGW(L"Shutdown: %s didn't finish within 5 s; left behind", what);
            leftBehind->push_back(std::move(f));
        }
    };
    // This PC's own Wi-Fi first: a cast receiver that couldn't start beside it (one Wi-Fi card can't do both) never
    // came back from being released while the network was still up (seen 2026-10-12: the app hung on exit).
    StopHotspot(false);
    bounded(L"stopping this PC's own Wi-Fi", std::move(m_hotspotStopping));
    bounded(L"stopping the cast receiver",
            std::async(std::launch::async, [start = std::move(m_receiverStart), r = std::move(m_miracast), stopping = std::move(m_receiverStopping)]() mutable {
                ComInit com;
                if (stopping.valid()) stopping.wait();
                if (start.valid()) r = start.get();
                r.reset();
            }));
    DestroyEncoder();
    while (!m_sessions.empty()) CloseSession(m_sessions.size() - 1);
    for (auto& f : m_graveyard)
        if (f.valid() && f.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            LOGW(L"Shutdown: a background job (closing a device, stopping an encoder or saving a file) is still running");
            leftBehind->push_back(std::move(f));
        }
    m_graveyard.clear();
    if (m_enumFuture.valid()) m_enumFuture.wait();
    // Background jobs still running (a firewall check, a GPU probe, an encoder start) would block the exit in their
    // futures' destructors: give each a moment and name the one that hangs (main.cpp ends the process if it does).
    auto settle = [](const wchar_t* name, auto& f) {
        if (f.valid() && f.wait_for(std::chrono::seconds(3)) != std::future_status::ready) LOGW(L"Shutdown: %s is still running", name);
    };
    settle(L"firewall check", m_fwCheck);
    settle(L"second-GPU probe", m_otherProbe);
    settle(L"sharing encoder start", m_shareStart);
    settle(L"sharing encoder change", m_shareNext);
    settle(L"encoder start", m_encoderNext);
    settle(L"own Wi-Fi start", m_hotspotStart);
    settle(L"own Wi-Fi stop", m_hotspotStopping);
    m_audio.Stop();
    m_renderer.Shutdown();
    SetTimerResolution(false);
    if (m_lowMemEvent) CloseHandle(m_lowMemEvent);
    m_lowMemEvent = nullptr;
    if (m_powerRequest) CloseHandle(m_powerRequest);   // ends every request it holds
    m_powerRequest = nullptr;
}

void App::HandleDeviceLost()
{
    StopFlashback();   // its frames live on the old device
    DestroyEncoder();
    DestroyShareEncoder();   // UpdateSharingSource recreates it on the new device
    m_renderer.Shutdown();
    m_gpuProf.Shutdown();
    if (!m_renderer.Init(m_hwnd, m_clientW, m_clientH)) {
        Sleep(500);
        return;
    }
    m_gpuProf.Init(m_renderer.Device(), m_renderer.Context());
    m_ui.Init(m_renderer.D2D(), m_renderer.DWrite());
    m_compositor.Init(m_renderer);
    m_program = RenderTarget{};
    m_miniOut = OutputWindow{};   // swap chains of the old device: made again on the new one
    for (auto& w : m_deviceWindows) w->out = OutputWindow{};
    for (auto& s : m_sessions) {
        s->surface = VideoSurface{};
        if (s->video->Current().IsCompressed()) s->video->Reopen();
    }
}

UiInput App::TakeInput()
{
    UiInput in;
    std::lock_guard lock(m_inputMutex);
    in.mx = m_input.mx;
    in.my = m_input.my;
    in.mouseIn = m_input.mouseIn;
    in.down = m_input.down;
    in.pressed = m_input.pressed;
    in.released = m_input.released;
    in.doubleClick = m_input.dbl;
    in.wheel = m_input.wheel;
    in.keys = std::move(m_input.keys);
    in.chars = std::move(m_input.chars);
    m_input.chars.clear();
    if (m_input.moved) m_lastMoveQpc = QpcNow();
    m_input.pressed = m_input.released = m_input.dbl = m_input.moved = false;
    m_input.wheel = 0;
    m_input.keys.clear();
    return in;
}

void App::SetClean(bool on)
{
    if (on == m_clean) return;
    m_clean = on;
    m_ui.ClosePopups();
    m_drag = Drag::None;
    if (on) {
        m_settingsOpen = false;
    } else {
        m_lastMoveQpc = QpcNow();   // bring the bar back right away
        m_barAlpha = 1.0f;
    }
    LOGI(L"Clean output %s", on ? L"on" : L"off");
    SetEvent(m_wake);
}

void App::UpdateTheme()
{
    const int mode = m_themeMode;
    const bool dark = mode == 0 ? m_systemDark.load() : mode == 2;
    if (dark == m_themeDark) return;
    m_themeDark = dark;
    theme::Apply(dark);
    PostMessageW(m_hwnd, WM_APP_THEME, 0, 0);
}

void App::StyleFrame(HWND hwnd) const
{
    BOOL dark = m_themeDark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    // Windows 11: a Mica title bar and the accent as the window border (Windows 10 ignores both). Only windows with
    // a title bar get Mica: it fills in behind see-through pixels, and the mini player can be see-through.
    if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION) == WS_CAPTION) {
        const int mica = 2 /*DWMSBT_MAINWINDOW*/;
        DwmSetWindowAttribute(hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &mica, sizeof(mica));
    }
    const COLORREF border = RGB(0x00, 0x78, 0xD7);
    DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
}

void App::ToggleFullscreen() { PostMessageW(m_hwnd, WM_APP_FULLSCREEN, m_fullscreen ? 0 : 1, 0); }

void App::ToggleMute()
{
    m_muted = !m_muted;
    m_audio.muted = m_muted;
    m_settingsDirty = true;
}

void App::Toast(const std::wstring& text) { Toast(text, text); }

void App::Toast(const std::wstring& text, const std::wstring& key)
{
    std::lock_guard lock(m_toastMutex);
    std::erase_if(m_toasts, [&](const ToastItem& t) { return t.key == key && t.file.empty(); });
    m_toasts.push_back({text, L"", key, QpcNow() + QpcFrequency() * 3});
    while (m_toasts.size() > 3) m_toasts.pop_front();
    m_toastUntil = std::max(m_toastUntil.load(), m_toasts.back().until);
    SetEvent(m_wake);
}

void App::ToastFile(const std::wstring& text, const std::wstring& file)
{
    std::lock_guard lock(m_toastMutex);
    m_toasts.push_back({text, file, file, QpcNow() + QpcFrequency() * 7});
    while (m_toasts.size() > 3) m_toasts.pop_front();
    m_toastUntil = std::max(m_toastUntil.load(), m_toasts.back().until);
    SetEvent(m_wake);
}

void App::HandleKeys(const UiInput& in)
{
    if (HandleHotkeyCapture(in.keys)) return;   // "Press a key…" in Settings ▸ Hotkeys
    for (UINT k : in.keys) {
        if (!(k & 0x20000u)) continue;
        const int id = int(k & 0xFFFF);
        if (id >= 1 && id <= kHotkeyCount) RunHotkey(id - 1);
        else if (id >= 11 && id <= 19 && m_advanced && id - 11 < int(m_scenes.scenes.size())) StartTransition(id - 11);
    }
    if (m_ui.TextFocused()) return;   // typing into a text box
    std::vector<UINT> keys;
    for (UINT k : in.keys) {
        if (k & 0x20000u) continue;   // global hotkeys: above
        // Advanced with a source selected: Delete removes it, arrows move it (Shift: 10 px).
        if (m_advanced && m_selectedItem && !(k & (KEY_CTRL | KEY_ALT)) && !m_flash) {
            const UINT vk = k & 0xFFFF;
            const float step = (k & KEY_SHIFT) ? 10.0f : 1.0f;
            if (vk == VK_DELETE) { DeleteSelectedItem(); continue; }
            if (vk == VK_LEFT) { NudgeSelectedItem(-step, 0); continue; }
            if (vk == VK_RIGHT) { NudgeSelectedItem(step, 0); continue; }
            if (vk == VK_UP) { NudgeSelectedItem(0, -step); continue; }
            if (vk == VK_DOWN) { NudgeSelectedItem(0, step); continue; }
        }
        if (!m_flash) {               // the user's shortcuts (Flashback keeps its own keys)
            const UINT combo = k & (0xFFFFu | KEY_CTRL | KEY_ALT | KEY_SHIFT);
            int action = -1;
            for (int i = 0; i < kHotkeyCount && action < 0; ++i)
                if (m_hotkeyLocal[i] == combo) action = i;
            if (action >= 0) {
                RunHotkey(action);
                continue;
            }
        }
        if (k & KEY_ALT) continue;   // other Alt combinations are Windows' own
        if (!(k & KEY_CTRL)) {
            keys.push_back(k & ~KEY_SHIFT);   // plain keys: Shift doesn't matter below
            continue;
        }
        const UINT key = k & 0xFFFF;   // Ctrl shortcuts
        if (m_advanced && key == 'Z') (k & KEY_SHIFT) ? Redo() : Undo();
        else if (m_advanced && key == 'Y') Redo();
        else if (m_advanced && key == 'D') DuplicateSelectedItem();
    }
    for (UINT k : keys) {
        if (m_ui.HeaderFocused() && (k == VK_RETURN || k == VK_SPACE)) continue;   // flips the focused section header
        if (m_lag && k == VK_ESCAPE) {
            StopLagCheck(false);
            continue;
        }
        if (m_trim) {   // the trim dialog is modal
            if (k == VK_ESCAPE) m_trim.reset();
            continue;
        }
        if (m_cropEdit) {   // cropping on the picture: Enter keeps, Esc puts the old crop back
            if (k == VK_ESCAPE || k == VK_RETURN) EndCropEdit(k == VK_RETURN);
            continue;
        }
        if (m_flash) {   // Flashback keys
            if (k == VK_LEFT) { FlashbackJump(-5.0); continue; }
            if (k == VK_RIGHT) { FlashbackJump(5.0); continue; }
            if (k == VK_SPACE) { m_flash->SetPlaying(!m_flash->Playing()); continue; }
            if (k == VK_ESCAPE || k == VK_END || k == 'L') { StopFlashback(); continue; }
            if (k == 'I') { m_clipIn = m_flash->Position(); continue; }
            if (k == 'O') { m_clipOut = m_flash->Position(); continue; }
            if (k == VK_RETURN) { SaveFlashbackClip(); continue; }
        } else if (k == VK_LEFT) {
            StartFlashback(10.0);   // the same as the top-bar button
            continue;
        }
        switch (k) {
        case KEY_ALT_ENTER: ToggleFullscreen(); break;
        case VK_F1:
            m_showHelp = !m_showHelp;
            break;
        case VK_ESCAPE:
            if (m_showHelp) {
                m_showHelp = false;
                break;
            }
            if (m_clean) {
                SetClean(false);
                break;
            }
            if (m_ui.PopupOpen()) break;
            if (m_settingsOpen) m_settingsOpen = false;
            else if (m_fullscreen) ToggleFullscreen();
            break;
        case VK_TAB:
            if (!m_advanced && m_devices.video.size() > 1) SetMultiview(!m_multiview);
            break;
        case VK_UP:
        case VK_DOWN:
            m_volume = Clamp(m_volume + (k == VK_UP ? 0.05f : -0.05f), 0.0f, 2.0f);
            m_audio.volume = m_volume;
            m_settingsDirty = true;
            Toast(Format(L"Volume %d%%", int(std::lround(m_volume * 100))), L"volume");
            break;
        default:
            if (k >= '1' && k <= '9') {
                size_t i = k - '1';
                if (m_advanced) {
                    if (i < m_scenes.scenes.size()) {
                        if (m_studio) {
                            m_scenes.preview = int(i);
                            ScenesChanged();
                            SyncSceneSessions();
                        } else {
                            StartTransition(int(i));
                        }
                    }
                } else {
                    // The i-th device / tile that Simple shows (casts are in the Wireless tab).
                    size_t n = 0;
                    if (m_multiview) {
                        for (size_t si = 0; si < m_sessions.size(); ++si)
                            if (ShownInSimple(m_sessions[si]->info) && n++ == i) m_active = si;
                    } else {
                        for (size_t di = 0; di < m_devices.video.size(); ++di)
                            if ((m_wirelessView || ShownInSimple(m_devices.video[di])) && n++ == i) {
                                SwitchTo(di);
                                break;
                            }
                    }
                }
            }
            break;
        }
    }
}

std::wstring App::PictureShapeHint()
{
    Session* s = Active();
    if (!s || !s->surface.hasImage) return L"";
    const VideoFormat f = s->video->Current();
    const UINT w = f.width ? f.width : s->surface.width, h = f.height ? f.height : s->surface.height;
    if (!w || !h) return L"";
    auto ratio = [](double a) -> std::wstring {
        const struct { double v; const wchar_t* n; } known[] = {{16.0 / 9, L"16:9"}, {4.0 / 3, L"4:3"}, {16.0 / 10, L"16:10"}, {21.0 / 9, L"21:9"},
                                                                {32.0 / 9, L"32:9"}, {5.0 / 4, L"5:4"}, {1.0, L"1:1"}, {9.0 / 16, L"9:16"}};
        for (const auto& k : known)
            if (std::abs(a - k.v) < 0.03) return k.n;
        return Format(L"%.2f:1", a);
    };
    const double source = double(w) / h, shown = DisplayAspect(w, h);
    if (std::abs(shown - source) < 0.01) return Format(L"%u×%u · square pixels (%s)", w, h, ratio(source).c_str());
    if (m_aspect == 0) return Format(L"%u×%u · shown %s (SD)", w, h, ratio(shown).c_str());
    return Format(L"%u×%u · stretched to %s", w, h, ratio(shown).c_str());
}

bool App::EnsureFolder(const std::wstring& folder, const wchar_t* what)
{
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    const DWORD attr = GetFileAttributesW(folder.c_str());
    bool ok = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
    if (ok) {   // writable? (a probe file that deletes itself)
        HANDLE h = CreateFileW((folder + L"\\.zerodisplay-write-test").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        ok = h != INVALID_HANDLE_VALUE;
        if (ok) CloseHandle(h);
    }
    if (!ok) {
        LOGW(L"Folder not writable: %s", folder.c_str());
        Toast(Format(L"Can't save %s: the folder %s can't be written. Choose another folder in Settings.", what, folder.c_str()));
    }
    return ok;
}

std::wstring App::DiskFreeHint() const
{
    if (!m_diskFreeAt || QpcElapsedMs(m_diskFreeAt) > 5000) {
        m_diskFreeAt = QpcNow();
        m_diskFreeGb = -1;
        wchar_t volume[MAX_PATH] = {};
        ULARGE_INTEGER avail{};
        if (GetVolumePathNameW(RecordFolder().c_str(), volume, MAX_PATH) && GetDiskFreeSpaceExW(volume, &avail, nullptr, nullptr))
            m_diskFreeGb = double(avail.QuadPart) / (1024.0 * 1024.0 * 1024.0);
    }
    if (m_diskFreeGb < 0) return L"";
    return m_diskFreeGb < 10 ? Format(L" (now %.1f GB free)", m_diskFreeGb) : Format(L" (now %.0f GB free)", m_diskFreeGb);
}

bool App::PanelSection(const char* id, const wchar_t* title, float& y, float cx, float cw)
{
    Ui& u = m_ui;
    bool open = !m_collapsed.contains(id);
    if (u.SectionHeader(id, {cx - u.S(4), y, cw + u.S(8), u.S(30)}, title, open)) {
        if (open) m_collapsed.erase(id);
        else m_collapsed.insert(id);
        m_settingsDirty = true;
    }
    y += u.S(open ? 40.0f : 36.0f);
    return open;
}

std::vector<RectF> App::TileRects() const
{
    std::vector<RectF> out;
    const float W = float(m_renderer.Width()), H = float(m_renderer.Height());
    if (!m_multiview || SimpleSessionCount() <= 1) {
        out.push_back({0, 0, W, H});
        return out;
    }
    const size_t n = SimpleSessionCount();
    const size_t cols = size_t(std::ceil(std::sqrt(double(n))));
    const size_t rows = (n + cols - 1) / cols;
    const float gap = 2;
    const float tw = (W - gap * float(cols - 1)) / float(cols), th = (H - gap * float(rows - 1)) / float(rows);
    size_t i = 0;
    for (const auto& s : m_sessions) {
        if (!ShownInSimple(s->info)) {
            out.push_back({});
            continue;
        }
        size_t c = i % cols, r = i / cols;
        out.push_back({std::round(float(c) * (tw + gap)), std::round(float(r) * (th + gap)), std::floor(tw), std::floor(th)});
        ++i;
    }
    return out;
}

bool App::ShownInSimple(const VideoDeviceInfo& d) { return !CastSource::IsCastLink(d.symlink); }

size_t App::SimpleSessionCount() const
{
    return size_t(std::count_if(m_sessions.begin(), m_sessions.end(), [](const auto& s) { return ShownInSimple(s->info); }));
}

void App::Frame()
{
    UpdateTheme();
    const int64_t now = QpcNow();
    m_profiling = m_showStats || m_cmd.selftestSec > 0;
    if (m_profiling) m_gpuProf.BeginFrame();
    UiInput in = TakeInput();
    HandleKeys(in);
    UpdateCloseRisk();
    SyncProfile();   // the members hold the shown device's shape, scaling, filters, Enhance, audio delay
    if (!m_reopenPending.empty()) {
        for (auto& s : m_sessions)
            if (s->info.symlink == m_reopenPending) {
                LOGI(L"Trying %s again (asked)", s->info.name.c_str());
                s->video->Close();
                s->video->Open(s->info, m_settings.GetString(Settings::DeviceSection(s->info.Key()), L"Format", L""), m_preferUncompressed);
                s->openedQpc = QpcNow();
            }
        m_reopenPending.clear();
    }
    if (!m_hidePending.empty()) {
        SetDeviceHidden(m_hidePending, true);
        m_hidePending.clear();
    }
    if (!m_windowPending.symlink.empty()) {
        OpenDeviceWindow(m_windowPending);
        m_windowPending = {};
    }
    for (UINT& k : in.keys) k &= ~(KEY_CTRL | KEY_SHIFT);   // widgets see plain keys

    // Newest frame from every device -> GPU. An HDR recording encodes from the picture's linear copy.
    for (auto& s : m_sessions) s->surface.keepLinear = EncodesHdr(s->info.symlink);
    bool newPicture = false;
    for (auto& s : m_sessions) {
        CapturedFrame f;
        if (s->video && s->video->TakeFrame(f)) {
            PictureTransform xf = s->prof.xf;
            if (m_cropEdit && s.get() == Active()) xf.crop[0] = xf.crop[1] = xf.crop[2] = xf.crop[3] = 0.0f;   // the whole picture while cropping on it
            if (m_renderer.Upload(s->surface, f, m_matrix, m_range, xf, EffectiveHdr(s->prof.hdr))) {
                m_lastCaptureQpc = now;
                if (!s->standby && EncodesLink(s->info.symlink)) {
                    // Device timestamps are QPC-based for most drivers; fall back to arrival time otherwise.
                    const int64_t arrival = QpcTo100ns(f.arrivalQpc);
                    const int64_t t = (f.sampleTime > 0 && std::llabs(arrival - f.sampleTime) < 10000000) ? f.sampleTime : arrival;
                    SubmitFrame(s->info.symlink, s->surface.rgb.Get(), t, f.arrivalQpc, s->surface.hdrLin.Get());
                }
                // Picture waiting for late sound (screen only): keep recent pictures on the GPU.
                if (s->prof.delayMs < 0)
                    s->pictureDelay.Push(m_renderer.Device(), m_renderer.Context(), s->surface.rgb.Get(), s->surface.width, s->surface.height,
                                         f.arrivalQpc, -s->prof.delayMs, s->shownFps > 1.0 ? s->shownFps : 60.0);
                if (m_lag && s.get() == Active()) UpdateLagCheck(s.get(), true, f.arrivalQpc);
                else if (m_cmd.flipLog && s.get() == Active()) m_renderer.LagSample(s->surface, f.arrivalQpc);
                s->hasPending = true;
                s->pendingArrival = f.arrivalQpc;
                s->pendingSampleTime = f.sampleTime;
                s->shownFrames++;
                s->fpsCount++;
                newPicture = true;
            } else {
                s->uploadFailures++;
            }
        }
        if (s->surface.hasImage && QpcToMs(now - s->lastProbe) > 250) {
            s->lastProbe = now;
            m_renderer.Probe(s->surface);
            const bool black = s->surface.brightest >= 0.0f && s->surface.brightest < kBlackLevel;
            if (!black) s->blackSince = 0;
            else if (!s->blackSince) s->blackSince = now;
        }
        double el = QpcToMs(now - s->fpsStart);
        if (el >= 1000.0) {
            s->shownFps = s->fpsCount * 1000.0 / el;
            s->fpsCount = 0;
            s->fpsStart = now;
        }
    }
    for (auto& s : m_sessions) {
        ApplySavedPicture(*s);
        UpdateCrop(*s, now);
        if (s->prof.delayMs >= 0 && !s->pictureDelay.Empty()) s->pictureDelay.Release();   // light: nothing kept while off
    }
    if (m_lag) UpdateLagCheck(Active(), false, 0);
    if (m_profiling) m_gpuProf.Mark(1);   // uploads / colour conversion

    // Standby: while a device's signal is lost, its last good picture is shown and keeps feeding the
    // encoder at the stream's frame rate, so recordings and viewers see a frozen picture, not a stall.
    for (auto& s : m_sessions) {
        const bool sb = InStandby(*s);
        if (sb != s->standby) {
            s->standby = sb;
            LOGI(L"%s: standby %s", s->info.name.c_str(), sb ? L"on (showing the last picture)" : L"off");
        }
        if (sb && EncodesLink(s->info.symlink)) {
            const std::optional<EncoderConfig> cfg = ConfigForLink(s->info.symlink);
            if (!cfg) continue;
            const EncoderConfig& c = *cfg;
            const double periodMs = 1000.0 * c.fpsDen / std::max(1u, c.fpsNum);
            const int64_t period = int64_t(periodMs * QpcFrequency() / 1000.0);
            if (now - s->lastStandbySubmit > period + period / 2) s->lastStandbySubmit = now - period;   // (re)start; no catch-up bursts
            if (now - s->lastStandbySubmit >= period - QpcFrequency() / 1000) {
                s->lastStandbySubmit += period;
                SubmitFrame(s->info.symlink, s->surface.lastGood.Get(), QpcTo100ns(now), now);
            }
        }
    }
    UpdateMix();
    UpdateStreamRate();
    PollEncoderJob();
    PollExtraRecordings();
    UpdateWireless();

    if (m_screenshotRequested) {
        m_screenshotRequested = false;
        SaveScreenshot(L"", false);
    }

    ApplyPickedFiles();
    ApplyPickedFolder();
    ApplyPickedSettingsFile();
    UpdateFlashback();
    UpdateTrim();
    const auto tiles = TileRects();
    // The interface never waits for the picture: input, animations and open menus are drawn at once, whatever the
    // source's frame rate or the 30 fps preview limit. Picture-only frames: skipped by the 30 fps limit, and when
    // nothing is new (no new picture) the last presented frame stays on screen (at least every 100 ms anyway).
    const bool uiDraw = !m_hidden && UiNeedsDraw(in, now);
    const bool pictureDue = !m_capSkip && (newPicture || m_advanced || m_flash || m_trim || !m_lastPresentQpc || QpcElapsedMs(m_lastPresentQpc) >= 100.0);
    const bool draw = !m_hidden && (uiDraw || pictureDue);
    if (!draw) ++(m_hidden || m_capSkip ? m_hiddenFrames : m_unchangedFrames);
    if (draw) m_renderer.BeginFrame();
    {
        // WARP: halves the frame rate. Simple-mode pictures set their own device's Enhance in DrawSession; the cache of
        // sharpened pictures stays while any open device uses it (multiview tiles, device windows).
        bool anyEnhance = false;
        for (const auto& s : m_sessions) anyEnhance |= EnhanceMode(s->prof) > 0;
        m_renderer.SetEnhance(EnhanceMode(CurrentProfile()), m_enhanceStrength, anyEnhance);
    }
    // AI upscaling: one picture per frame (not multiview tiles or studio's two canvases), like Enhance otherwise.
    m_renderer.SetSuperRes((m_aiUpscale || m_cmd.aiUpscale) && m_smooth && ScreenFilter() == 0 && EnhanceAllowed() && !(m_advanced && m_studio) &&
                           !(m_multiview && SimpleSessionCount() > 1 && !m_advanced));
    if (m_flash) {
        const RectF area = FlashbackArea();
        if (m_flashSurface.hasImage && draw)
            m_renderer.DrawSurface(m_flashSurface, Renderer::FitRect(m_flashSurface.width, m_flashSurface.height, area, m_advanced ? ScaleMode::Fit : m_scale),
                                   area, true);
        if (m_advanced) RenderProgram(now);   // keeps the canvas encoder (recording, sharing) running
    } else if (m_advanced) {
        RenderProgram(now);
        if (m_profiling) m_gpuProf.Mark(2);   // program (+ studio preview) composition
        if (!draw) {
        } else if (m_clean) {
            const RectF full{0, 0, float(m_renderer.Width()), float(m_renderer.Height())};
            m_renderer.DrawTexture(m_program.srv.Get(), Renderer::FitRect(m_scenes.width, m_scenes.height, full, m_scale), full,
                                   m_smooth || m_scale != ScaleMode::Integer);
        } else {
            m_renderer.DrawTexture(m_program.srv.Get(), CanvasRect(), CanvasArea(), true);
            if (m_studio) m_renderer.DrawTexture(m_preview.srv.Get(), EditRect(), CanvasArea(), true);
        }
    } else if (!draw) {
    } else if (m_wirelessView) {
        if (Session* s = ActiveCast()) DrawSession(*s, WirelessArea(), m_smooth || s->prof.scale != int(ScaleMode::Integer));
    } else if (m_multiview && SimpleSessionCount() > 1) {
        for (size_t i = 0; i < m_sessions.size() && i < tiles.size(); ++i)
            if (tiles[i].w > 0) DrawSession(*m_sessions[i], tiles[i], m_smooth);
    } else if (Session* s = Active()) {
        if (m_cropEdit && s->surface.hasImage) m_renderer.DrawTexture(s->surface.rgbSrv.Get(), CropEditRect(*s), tiles[0], true);
        else DrawSession(*s, tiles[0], m_smooth || s->prof.scale != int(ScaleMode::Integer));
    }

    if (draw) DrawTrimPreview();
    if (m_profiling) m_gpuProf.Mark(3);   // drawing the picture(s) on screen
    const int64_t uiStart = QpcNow();
    ++m_frames;
    if (draw && uiDraw) {
        m_renderer.BeginUiLayer();
        m_ui.Begin(in, float(m_dpi.load()) / 96.0f, float(m_renderer.Width()), float(m_renderer.Height()), QpcToMs(now - m_startQpc) / 1000.0);
        DrawUi(in);
        SyncProfile();   // a Settings change reaches the device's profile at once
        m_ui.End();
        m_renderer.EndUiLayer();
        const double drawMs = QpcElapsedMs(uiStart);
        m_uiDrawMsTotal += drawMs;
        m_uiDrawMsMax = std::max(m_uiDrawMsMax, drawMs);
        m_lastUiDraw = now;
        m_lastUiMx = in.mx;
        m_lastUiMy = in.my;
        m_lastUiMouseIn = in.mouseIn;
        ++m_uiDraws;
    }
    if (draw) m_renderer.DrawUiLayer();
    if (m_profiling) m_gpuProf.Mark(4);   // UI
    m_uiMs = m_uiMs * 0.9 + QpcElapsedMs(uiStart) * 0.1;

    if (!m_stepShot.empty() && draw) {
        std::vector<uint8_t> px;
        if (m_renderer.ReadbackBackBuffer(px)) {
            const HRESULT hr = SavePng(m_stepShot, m_renderer.Width(), m_renderer.Height(), px);
            LOGI(L"[selftest] ui step screenshot %s: %s", m_stepShot.c_str(), HrToString(hr).c_str());
        }
        m_stepShot.clear();
    }
    if (m_uiShotNow) {
        m_uiShotNow = false;
        m_uiShotTaken = true;
        std::vector<uint8_t> px;
        if (m_renderer.ReadbackBackBuffer(px)) {
            HRESULT hr = SavePng(m_cmd.uiShot, m_renderer.Width(), m_renderer.Height(), px);
            LOGI(L"UI screenshot %s: %s", m_cmd.uiShot.c_str(), HrToString(hr).c_str());
        }
    }

    const int64_t beforePresent = QpcNow();
    if (draw) {
        m_renderer.Present(m_vsync && !m_fastSource);
        m_swapSlot = false;
        m_lastPresentQpc = QpcNow();
        ++m_presentCount;
        if (!m_presentFpsStart) m_presentFpsStart = m_lastPresentQpc;
        if (const double el = QpcToMs(m_lastPresentQpc - m_presentFpsStart); el >= 1000.0) {
            m_presentFps = m_presentCount * 1000.0 / el;
            m_presentCount = 0;
            m_presentFpsStart = m_lastPresentQpc;
        }
    }
    const int64_t after = QpcNow();
    if (m_profiling) m_gpuProf.Mark(5);   // present
    // Encoder copies are recorded after the present: send them to the GPU now, not with the next present (frames
    // that aren't shown, fast sources, the 30 fps limit, a hidden window, would otherwise starve the encoders).
    const bool hadSubmits = !m_submits.empty();
    FlushSubmits();
    if (hadSubmits) m_renderer.Context()->Flush();
    m_renderer.StepSuperResProbe();   // a few video processor blits while an AI upscaling check runs
    if (m_profiling) {
        m_gpuProf.Mark(6);   // encoder colour conversion / read-back copies
        m_gpuProf.EndFrame();
    }
    RenderMini();   // after the main window's present: never delays it
    RenderDeviceWindows();
    m_presentMs = m_presentMs * 0.9 + QpcToMs(after - beforePresent) * 0.1;

    for (auto& s : m_sessions) {
        if (!s->hasPending) continue;
        s->hasPending = false;
        double lat = QpcToMs(after - s->pendingArrival);
        s->latArrivalMs = s->latArrivalMs == 0 ? lat : s->latArrivalMs * 0.9 + lat * 0.1;
        // Device timestamps share the QPC time base for most capture drivers; use them if plausible.
        int64_t d = QpcTo100ns(after) - s->pendingSampleTime;
        if (s->pendingSampleTime > 0 && d > 0 && d < 10000000) {
            double ms = double(d) / 10000.0;
            s->latCaptureMs = s->captureLatValid ? s->latCaptureMs * 0.9 + ms * 0.1 : ms;
            s->captureLatValid = true;
        }
    }
    m_lastFrameQpc = after;
}

// ---------------------------------------------------------------------------------------------
// UI

// The UI is immediate mode (drawing and input in one pass), so it runs when it can have changed: any input,
// something animating, the window resized, or 250 ms passed (fps, recording time and other live numbers).
bool App::UiNeedsDraw(const UiInput& in, int64_t now)
{
    if (!m_renderer.UiLayerValid() || m_uiShotNow || m_uiDirty.exchange(false)) return true;
    if (in.pressed || in.released || in.down || in.doubleClick || in.wheel != 0 || !in.keys.empty() || !in.chars.empty()) return true;
    if (in.mx != m_lastUiMx || in.my != m_lastUiMy || in.mouseIn != m_lastUiMouseIn) return true;
    if (m_ui.Animating() || m_ui.PopupOpen() || (m_barAlpha > 0.0f && m_barAlpha < 1.0f) || m_toastUntil > now || m_transitioning || m_lag)
        return true;
    if (m_barCanHide && m_barAlpha > 0.0f && now >= BarHideAt()) return true;   // time to hide the bar
    if (m_shotFlashQpc) return true;   // the screenshot flash fades
    return QpcToMs(now - m_lastUiDraw) >= m_uiRefreshMs;
}

void App::DrawUi(const UiInput& in)
{
    Ui& u = m_ui;
    if (m_clean) {
        // Clean output: only the picture. No bar, panels, pills, toasts, stats or cursor.
        m_barAlpha = 0.0f;
        const bool hideCursor = in.mouseIn;
        if (hideCursor != m_hideCursor.load()) {
            m_hideCursor = hideCursor;
            PostMessageW(m_hwnd, WM_APP_CURSOR, hideCursor ? 1 : 0, 0);
        }
        if (in.doubleClick && in.mouseIn) ToggleFullscreen();
        return;
    }
    if (m_trim) {   // modal: nothing else may draw over its preview
        DrawTrimDialog();
        DrawToast();
        return;
    }
    if (m_showHelp) {   // F1 page: on its own, so clicks don't reach the controls behind it
        DrawHelp(in);
        return;
    }
    const int64_t now = QpcNow();
    const float W = u.Width(), H = u.Height();
    const float barH = BarHeight();
    Session* active = Active();
    const bool activeHasImage = active && active->surface.hasImage;

    // Auto-hide the top bar after the mouse has been still for a while.
    const double idleMs = QpcToMs(now - m_lastMoveQpc);
    const bool overBar = in.mouseIn && in.my >= 0 && in.my < barH * 1.4f;
    const bool wantBar = idleMs < 2500 || overBar || u.PopupOpen() || m_settingsOpen || m_cropEdit || !activeHasImage || u.Dragging() ||
                         (m_cmd.selftestSec > 0 && !m_cmd.barTest) || m_advanced || m_wirelessView;
    m_barCanHide = !(overBar || u.PopupOpen() || m_settingsOpen || m_cropEdit || !activeHasImage || u.Dragging() || (m_cmd.selftestSec > 0 && !m_cmd.barTest) ||
                     m_advanced || m_wirelessView);
    const float barBefore = m_barAlpha;
    if (m_barAlpha == (wantBar ? 1.0f : 0.0f)) {
        m_barAnimQpc = now;   // at rest
    } else {
        // Real elapsed time: a fade out starts at the hide moment, a fade in at the mouse move, not at this drawing.
        const int64_t from = std::max(m_barAnimQpc, wantBar ? m_lastMoveQpc : BarHideAt());
        const float dt = float(std::clamp(QpcToMs(now - from) / 1000.0, 0.0, 0.1));
        m_barAlpha = Clamp(m_barAlpha + (wantBar ? dt * 8.0f : -dt * 4.0f), 0.0f, 1.0f);
        m_barAnimQpc = now;
    }
    if (m_cmd.barTest && barBefore != m_barAlpha && (barBefore == 1.0f || m_barAlpha == 0.0f || barBefore == 0.0f || m_barAlpha == 1.0f))
        LOGI(L"[selftest] bar %s at %.0f ms after the last mouse move (alpha %.2f)",
             barBefore == 1.0f ? L"fade starts" : (m_barAlpha == 0.0f ? L"hidden" : (barBefore == 0.0f ? L"shows" : L"fully shown")), idleMs, m_barAlpha);
    const bool hide = !wantBar && in.mouseIn && m_barAlpha <= 0.0f;
    if (hide != m_hideCursor.load()) {
        m_hideCursor = hide;
        PostMessageW(m_hwnd, WM_APP_CURSOR, hide ? 1 : 0, 0);
    }

    // Tiles: status overlays, labels, selection.
    const auto tiles = TileRects();
    const bool multi = m_multiview && SimpleSessionCount() > 1;
    const RectF panelRect = m_settingsOpen ? RectF{W - std::min(u.S(380), W - u.S(20)) - u.S(8), barH + u.S(8), std::min(u.S(380), W - u.S(20)),
                                                   H - barH - u.S(16)}
                                           : RectF{};
    auto overChrome = [&] {
        return (m_barAlpha > 0.5f && in.my < barH) || (m_settingsOpen && panelRect.Contains(in.mx, in.my));
    };
    if (m_advanced) {
        DrawAdvanced(in);
    } else if (m_wirelessView) {
        DrawWireless(in);
    } else if (m_sessions.empty()) {
        DrawStatus(nullptr, {0, 0, W, H}, false);
    } else if (multi) {
        for (size_t i = 0; i < m_sessions.size() && i < tiles.size(); ++i) {
            const Session& s = *m_sessions[i];
            const RectF& t = tiles[i];
            if (t.w <= 0) continue;   // a cast (Wireless tab)
            DrawStatus(&s, t, true);
            float lw = u.Measure(s.info.name, u.S(12)) + u.S(16);
            RectF label{t.x + u.S(8), t.Bottom() - u.S(32), std::min(lw, t.w - u.S(16)), u.S(24)};
            u.Fill(label, theme::Bar, u.S(5));
            u.Text(s.info.name, label.Inset(u.S(8), 0), u.S(12), i == m_active ? theme::Text : theme::TextDim);
            if (i == m_active) u.Stroke(t, theme::Accent2, u.S(2));
            if (u.Hot(t) && in.pressed && !overChrome()) {
                m_active = i;
                if (in.doubleClick) {
                    SetMultiview(false);
                    break;
                }
            }
        }
    } else {
        DrawStatus(active, tiles[0], false);
        if (in.doubleClick && u.Hot(tiles[0]) && !overChrome() && !m_cropEdit) ToggleFullscreen();
    }

    if (m_showStats) DrawStats();
    if (m_shotFlashQpc) {   // screenshot taken: a short white flash over the picture (fades in 0.2 s)
        const double ms = QpcElapsedMs(m_shotFlashQpc);
        if (ms < 200.0) u.Fill({0, 0, W, H}, Color{1, 1, 1, float(0.55 * (1.0 - ms / 200.0))});
        else m_shotFlashQpc = 0;
    }
    DrawTopBar(m_barAlpha);
    {
        // Status pills (recording, sharing): always visible, under the bar when the bar is shown.
        struct Pill {
            std::wstring label;
            Color dot;
            bool blink;
        };
        std::vector<Pill> pills;
        if (m_profile.battery || m_profile.memory || m_profile.low)
            pills.push_back({std::wstring(m_profile.low ? L"LOW RESOURCE" : (m_profile.battery ? L"BATTERY SAVER" : L"MEMORY SAVER")), theme::Warn, false});
        if (m_recordPending && !m_recorder) pills.push_back({std::wstring(L"REC  starting\u2026"), theme::Bad, true});
        if (m_recorder) {
            const int s = int(m_recorder->Seconds());
            const std::wstring files = m_extraRecs.empty() ? std::wstring() : Format(L"  \u00D7%zu", m_extraRecs.size() + 1);
            if (m_noSignalWarned) pills.push_back({std::wstring(L"NO SIGNAL"), theme::Warn, true});
            if (m_recorder->Paused())
                pills.push_back({Format(L"PAUSED  %02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60) + files, theme::Warn, false});
            else
                pills.push_back({m_recorder->Writing() ? Format(L"REC  %02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60) + files : std::wstring(L"REC  starting…"),
                                 theme::Bad, true});
        }
        if (Session* a = Active(); a && !m_advanced && a->soundWarned) pills.push_back({std::wstring(L"NO SOUND"), theme::Warn, true});
        // The shown card runs at USB 2 speed (1080p60 isn't offered): stays visible until it's replugged at USB 3.
        if (Session* a = Active(); a && !m_advanced && a->video && a->video->IsLocalDevice() && a->video->Link() == UsbLink::Usb2OnUsb3Device)
            pills.push_back({std::wstring(L"USB 2 SPEED"), theme::Warn, false});
        if (m_server) {
            const int v = ShareViewers();
            pills.push_back({v ? Format(L"LIVE  %d viewer%s", v, v == 1 ? L"" : L"s") : std::wstring(L"SHARING"), theme::Good, v > 0});
        }
        // Right-aligned over the picture, never over a side panel (Advanced, Wireless); left-aligned while Settings is open.
        const float right = m_advanced ? CanvasArea().Right() : (m_wirelessView ? WirelessArea().Right() : W);
        float px = m_settingsOpen ? u.S(12) : right - u.S(12);
        const float py = barH * m_barAlpha + u.S(10);
        const bool blinkOn = (QpcNow() / (QpcFrequency() / 2)) % 2 == 0;
        for (const auto& p : pills) {
            const float pw = u.Measure(p.label, u.S(13), true) + u.S(40);
            RectF pill{m_settingsOpen ? px : px - pw, py, pw, u.S(30)};
            px = m_settingsOpen ? px + pw + u.S(8) : px - pw - u.S(8);
            u.Fill(pill, theme::Scrim, u.S(15));
            u.Fill({pill.x + u.S(12), pill.y + u.S(10), u.S(10), u.S(10)}, (!p.blink || blinkOn) ? p.dot : p.dot.A(0.35f), u.S(5));
            u.Text(p.label, {pill.x + u.S(28), pill.y, pill.w - u.S(32), pill.h}, u.S(13), theme::Text, Align::Left, true);
        }
    }
    if (m_flash) DrawFlashbackBar(in);
    if (m_cropEdit) DrawCropEditor();
    if (m_settingsOpen) DrawSettings();
    DrawTrimDialog();
    DrawToast();
    DrawWelcome(in);
}

void App::DrawStatus(const Session* s, const RectF& area, bool compact)
{
    Ui& u = m_ui;
    wchar_t icon = glyph::Info;
    Color color = theme::TextDim;
    std::wstring title, sub;
    const int64_t now = QpcNow();
    if (!s) {
        icon = glyph::Plug;
        size_t hiddenHere = 0;
        for (const auto& d : m_localDevices.video) hiddenHere += IsHidden(d);
        title = m_devices.video.empty() ? (hiddenHere ? L"No capture device shown" : L"No capture device found") : L"No device selected";
        sub = !m_devices.video.empty() ? L"Pick a device from the menu above."
              : hiddenHere              ? Format(L"%zu connected device(s) hidden: Settings ▸ Devices shows them again.", hiddenHere)
                                        : L"Plug in your capture card, it will open automatically.";
    } else if (s->standby) {
        // Small badge only: the frozen picture stays visible (clean mode shows nothing at all).
        const bool gone = !s->present || s->video->State() != CaptureState::Running;
        const std::wstring text = gone ? L"Disconnected, showing the last picture" : L"No signal, showing the last picture";
        const float fs = u.S(compact ? 11.0f : 12.5f);
        const float w = std::min(area.w - u.S(16), u.Measure(text, fs) + u.S(34));
        const float bh = u.S(compact ? 24.0f : 28.0f);
        RectF badge{area.x + (area.w - w) * 0.5f, area.Bottom() - bh - u.S(compact ? 40.0f : 24.0f), w, bh};
        u.Fill(badge, theme::Bar, u.S(6));
        u.Icon(gone ? glyph::Plug : glyph::Video, {badge.x + u.S(6), badge.y, u.S(18), badge.h}, u.S(12), theme::Warn);
        u.Text(text, {badge.x + u.S(26), badge.y, badge.w - u.S(30), badge.h}, fs, theme::Text);
        return;
    }
    std::wstring tips;   // a checklist under the message (not on small multiview tiles)
    enum class Action { None, Retry, Privacy, Log, Pin } action = Action::None, action2 = Action::None;
    if (s) {
        CaptureState st = s->video->State();
        const bool stale = QpcToMs(now - s->video->LastFrameQpc()) > 1500;
        const bool local = s->video->IsLocalDevice();   // (the test pattern counts: it stands in for a card)
        if (!s->present || st == CaptureState::Lost) {
            icon = glyph::Plug;
            color = theme::Warn;
            title = L"Disconnected";
            sub = local ? L"Plug " + s->info.name + L" back in: it opens again by itself." : s->video->StatusText();
            if (local) tips = L"•  A USB 3 card belongs in a blue (USB 3) port, plugged in all the way, ideally without a hub.";
        } else if (st == CaptureState::Opening || st == CaptureState::Closed) {
            icon = glyph::Refresh;
            title = L"Opening " + s->info.name + L"…";
        } else if (st == CaptureState::Running) {
            const bool black = s->surface.hasImage && s->blackSince && QpcToMs(now - s->blackSince) > 3000;
            if (!stale && !black) return;   // showing a picture, or the first frame is on its way
            icon = glyph::Video;
            title = black ? L"The picture is black" : L"No signal";
            sub = !local ? s->video->StatusText()
                  : black ? L"The card gets a picture, but it's completely black."
                          : L"Nothing arrives from " + s->info.name + L".";
            if (local)
                tips = std::wstring(L"•  The console / PC is on and not asleep (press a button on the controller).\n") +
                       L"•  The HDMI cable sits firmly in both ends; try another cable or HDMI port.\n" +
                       L"•  PlayStation 4 / 5: turn off HDCP (Settings ▸ System ▸ HDMI ▸ Enable HDCP), or the card only gets black.\n" +
                       L"•  Still nothing: set the console to 1080p (some cards can't take 4K or 120 Hz).";
        } else if (st == CaptureState::NeedsPin) {
            icon = glyph::Info;
            color = theme::Warn;
            title = L"The sharing PC asks for a PIN";
            sub = compact ? L"Open it on its own to type the PIN." : s->video->StatusText();
            action = Action::Pin;
        } else {
            icon = glyph::Warning;
            color = theme::Bad;
            title = st == CaptureState::InUse ? L"Device busy" : (st == CaptureState::Blocked ? L"Camera access blocked" : L"Capture error");
            sub = s->video->StatusText();
            if (st == CaptureState::InUse) {
                tips = L"Close it in the other app (OBS, Discord, Teams, Zoom, the Camera app, or a browser tab with the camera on). "
                       L"ZeroDisplay keeps trying by itself.";
                action = Action::Retry;
            } else if (st == CaptureState::Blocked) {
                tips = L"Windows blocks camera access for desktop apps. Turn on \"Camera access\" and \"Let desktop apps access your camera\".";
                action = Action::Privacy;
                action2 = Action::Retry;
            } else {
                action = Action::Retry;
                action2 = Action::Log;
            }
        }
    }
    const float scale = compact ? 0.8f : 1.0f;
    const float cw = std::min(area.w - u.S(24), u.S(560) * scale);
    if (compact) tips.clear();
    const float subH = sub.empty() ? 0.0f : u.MeasureBlock(sub, cw - u.S(24), u.S(13) * scale) + u.S(6);
    const float tipsH = tips.empty() ? 0.0f : u.MeasureBlock(tips, cw - u.S(40), u.S(12) * scale) + u.S(12);
    const float btnH = action == Action::None || compact ? 0.0f : u.S(44);
    const float ch = u.S(88) * scale + subH + tipsH + btnH + u.S(8);
    RectF card{area.x + (area.w - cw) * 0.5f, area.y + (area.h - ch) * 0.5f, cw, ch};
    // On a picture, or on the black screen in the light theme (dark text can't sit on black): a card behind.
    if ((s && s->surface.hasImage) || !theme::Dark) u.Fill(card, theme::Bar, u.S(8));
    u.Icon(icon, {card.x, card.y + u.S(12) * scale, card.w, u.S(36) * scale}, u.S(28) * scale, color);
    u.Text(title, {card.x + u.S(12), card.y + u.S(52) * scale, card.w - u.S(24), u.S(28) * scale}, u.S(17) * scale, theme::Text, Align::Center, true);
    float y = card.y + u.S(84) * scale;
    if (!sub.empty()) {
        // Centred when it fits on one line, otherwise wrapped from the left.
        if (u.Measure(sub, u.S(13) * scale) <= cw - u.S(24)) u.Text(sub, {card.x + u.S(12), y, card.w - u.S(24), u.S(20) * scale}, u.S(13) * scale, theme::TextDim, Align::Center);
        else u.TextBlock(sub, {card.x + u.S(12), y, card.w - u.S(24), subH}, u.S(13) * scale, theme::TextDim);
        y += subH;
    }
    if (!tips.empty()) {
        const RectF box{card.x + u.S(12), y + u.S(4), card.w - u.S(24), tipsH - u.S(4)};
        u.Fill(box, theme::Control, u.S(theme::Radius + 2));
        u.TextBlock(tips, box.Inset(u.S(8), u.S(4)), u.S(12) * scale, theme::Text);
        y += tipsH;
    }
    if (btnH > 0 && s && action == Action::Pin) {
        // The sharing PC's PIN: 6 digits, remembered for that PC.
        const float bw = u.S(120), tw = u.S(130), gx = u.S(8);
        const float bx = card.x + (card.w - (tw + gx + bw)) * 0.5f;
        const bool entered = u.TextBox("netpin", {bx, y + u.S(6), tw, u.S(32)}, m_pinEntry, L"6 digits");
        std::erase_if(m_pinEntry, [](wchar_t c) { return c < L'0' || c > L'9'; });   // digits only, as they're typed
        if (m_pinEntry.size() > net::PinGate::kLength) m_pinEntry.resize(net::PinGate::kLength);
        const bool full = m_pinEntry.size() == net::PinGate::kLength;
        const bool clicked = u.Button("netpinok", {bx + tw + gx, y + u.S(6), bw, u.S(32)}, L"Connect", 0, true, !full);
        if ((clicked || entered) && full && s->video->IsNetwork()) {
            const std::string pin = Narrow(m_pinEntry);
            RememberNetPin(s->info, pin);
            static_cast<NetReceiver*>(s->video.get())->SetPin(pin, true);
            m_pinEntry.clear();
        }
        return;
    }
    if (btnH > 0 && s) {
        auto label = [](Action a) {
            return a == Action::Retry ? L"Try again" : (a == Action::Privacy ? L"Open privacy settings" : L"Open the log");
        };
        const int n = action2 == Action::None ? 1 : 2;
        const float bw = std::min(u.S(220), (card.w - u.S(24) - u.S(8) * (n - 1)) / n);
        float bx = card.x + (card.w - (bw * n + u.S(8) * (n - 1))) * 0.5f;
        for (Action a : {action, action2}) {
            if (a == Action::None) continue;
            const std::string id = "status" + std::to_string(int(a)) + Narrow(s->info.symlink);
            if (u.Button(id.c_str(), {bx, y + u.S(6), bw, u.S(32)}, label(a), 0, a == action)) {
                if (a == Action::Retry) m_reopenPending = s->info.symlink;
                else {
                    // This instance's own log (a second one writes log-<pid>.txt).
                    const std::wstring target = a == Action::Privacy ? std::wstring(L"ms-settings:privacy-webcam") : log::Path();
                    m_graveyard.push_back(std::async(std::launch::async, [target] {
                        ComInit com(COINIT_APARTMENTTHREADED);
                        ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }));
                }
            }
            bx += bw + u.S(8);
        }
    }
}

void App::DrawTopBar(float alpha)
{
    if (alpha < 0.02f) return;
    Ui& u = m_ui;
    const float W = u.Width();
    const float H = BarHeight();
    u.SetAlpha(alpha);
    u.Fill({0, 0, W, H}, theme::Bar);
    // The bottom edge: a hairline, or a red line while recording (visible from across the room).
    if (m_recorder) u.Fill({0, H - u.S(2), W, u.S(2)}, theme::Bad);
    else u.Fill({0, H - u.S(1), W, u.S(1)}, theme::Border);

    float x = u.S(14);
    u.Logo({x, (H - u.S(22)) * 0.5f, u.S(22), u.S(22)});
    x += u.S(30);
    if (W > u.S(1100)) x += u.Wordmark({x, 0, u.S(160), H}, u.S(15)) + u.S(18);
    {
        // Simple / Advanced / Wireless mode switch
        int mode = m_advanced ? 1 : (m_wirelessView ? 2 : 0);
        const float mw = W < u.S(1000) ? u.S(216) : u.S(264), mh = u.S(34);
        if (alpha > 0.5f && u.Segmented("mode", {x, (H - mh) * 0.5f, mw, mh}, {L"Simple", L"Advanced", L"Wireless"}, mode)) {
            if (mode == 2) SetWirelessView(true);
            else {
                SetWirelessView(false);
                SetAdvanced(mode == 1);
            }
        }
        x += mw + u.S(12);
    }

    const float bh = u.S(34), by = (H - bh) * 0.5f, gap = u.S(4);
    float rx = W - u.S(10);
    // A narrow window: the controls that also have a key or a place in Settings give way, least needed first, so
    // nothing overlaps the mode switch and Simple keeps room for its menus (device, then format, then the sound
    // source: they exist only here). Settings and Record always stay.
    const bool rec = m_recorder != nullptr;
    const float btn = bh + gap, menuGap = u.S(8);
    const float minDev = u.S(142), minFmt = u.S(176), minAud = u.S(130);   // narrowest useful menus ("1920×1080 60 fps")
    struct Fit { float w; int prio; bool on; };
    enum { kStats, kHelp, kMulti, kMini, kClean, kShare, kFlash, kReplay, kAudioRoom, kSliderWide, kSlider, kMute, kFormatRoom, kDeviceRoom,
           kFull, kShot, kFits };
    Fit fit[kFits] = {
        {btn, 13, true},                                         // statistics (F3)
        {btn, 13, true},                                         // help (F1)
        {btn, 12, !m_advanced && m_devices.video.size() > 1},    // multiview (Tab)
        {btn, 11, true},                                         // mini player (F6)
        {btn, 11, true},                                         // clean output (F10)
        {btn, 10, true},                                         // sharing (also in Settings)
        {btn, 10, true},                                         // Flashback (left arrow)
        {btn, 10, m_replay != nullptr},                          // save replay (F8)
        {minAud + menuGap, 9, !m_advanced},                      // room for the sound source menu
        {u.S(40), 8, true},                                      // the volume slider's full width
        {u.S(56) + u.S(4) + gap, 7, true},                       // the volume slider (Up / Down)
        {btn * (m_micId.empty() ? 1 : 2), 6, true},              // mute (M), microphone mute (N)
        {minFmt + menuGap, 5, !m_advanced},                      // room for the format menu
        {minDev + menuGap, 4, !m_advanced},                      // room for the device menu
        {btn, 3, true},                                          // full screen (F11)
        {btn, 2, true},                                          // screenshot (F12)
    };
    float need = btn * (rec ? 3 : 2);   // settings, record (+ pause)
    for (const Fit& f : fit) need += f.on ? f.w : 0.0f;
    const float room = rx - x;
    for (int p = 13; p >= 1 && need > room; --p)
        for (Fit& f : fit)
            if (f.on && f.prio == p && need > room) {
                f.on = false;
                need -= f.w;
            }
    auto rightButton = [&](const char* id, wchar_t g, const std::wstring& tip, bool active, bool disabled = false) {
        rx -= bh;
        bool clicked = alpha > 0.5f && u.IconButton(id, {rx, by, bh, bh}, g, tip, active, theme::Text, disabled);
        rx -= gap;
        return clicked;
    };
    if (fit[kFull].on && rightButton("fullscreen", m_fullscreen ? glyph::BackToWindow : glyph::FullScreen,
                                     (m_fullscreen ? L"Exit full screen" : L"Full screen") + KeyHint(5), false))
        ToggleFullscreen();
    if (rightButton("settings", glyph::Settings, L"Settings", m_settingsOpen)) m_settingsOpen = !m_settingsOpen;

    // volume + output meter
    const float sw = fit[kSliderWide].on ? u.S(96) : u.S(56);
    if (fit[kSlider].on) {
        rx -= sw + u.S(4);
        float vol = m_volume;
        u.Fill({rx + sw * 0.5f - u.S(1), by + bh * 0.5f - u.S(6), u.S(2), u.S(12)}, theme::TextDim.A(0.6f));   // 100 %
        if (u.Slider("volume", {rx, by, sw, bh}, vol, 0.0f, 2.0f, true)) {   // the top bar never scrolls: wheel = volume
            if (std::abs(vol - 1.0f) < 0.04f) vol = 1.0f;   // snaps to 100 %
            Toast(Format(L"Volume %d%%", int(std::lround(vol * 100))), L"volume");
            m_volume = vol;
            m_audio.volume = vol;
            if (m_muted && vol > 0) {
                m_muted = false;
                m_audio.muted = false;
            }
            m_settingsDirty = true;
        }
        for (int c = 0; c < 2; ++c) {
            float p = m_audio.TakePeak(c);
            m_meter[c] = std::max(p, m_meter[c] * 0.85f);
            float db = m_meter[c] > 1e-5f ? 20.0f * std::log10(m_meter[c]) : -100.0f;
            float level = Clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
            RectF m{rx, H - u.S(9) + float(c) * u.S(3), sw, u.S(2)};
            u.Fill(m, theme::Control);
            u.Fill({m.x, m.y, m.w * level, m.h}, db > -3.0f ? theme::Bad : (db > -12.0f ? theme::Warn : theme::Good));
        }
        rx -= gap;
    }
    if (fit[kMute].on && rightButton("mute", m_muted ? glyph::Mute : glyph::Volume, (m_muted ? L"Unmute" : L"Mute") + KeyHint(3), m_muted))
        ToggleMute();
    if (fit[kMute].on && !m_micId.empty() && rightButton("micmute", m_micMuted ? glyph::MicOff : glyph::Microphone,
                                        (m_micMuted ? L"Microphone muted: click to turn it on" : L"Mute the microphone") + KeyHint(9), m_micMuted))
        ToggleMicMute();
    if (fit[kShot].on && rightButton("screenshot", glyph::Camera, L"Screenshot" + KeyHint(2), false)) m_screenshotRequested = true;
    {
        rx -= bh;
        if (alpha > 0.5f && u.IconButton("record", {rx, by, bh, bh}, rec ? glyph::Stop : glyph::Record,
                                         (rec ? L"Stop recording" : L"Start recording") + KeyHint(0), false, theme::Bad))
            ToggleRecording();
        rx -= gap;
        if (rec) {   // pause / resume next to it while recording
            rx -= bh;
            const bool paused = m_recorder->Paused();
            if (alpha > 0.5f && u.IconButton("pauserec", {rx, by, bh, bh}, paused ? glyph::Play : glyph::Pause,
                                             (paused ? L"Resume recording" : L"Pause recording") + KeyHint(8), paused, paused ? theme::Warn : theme::Text))
                TogglePauseRecording();
            rx -= gap;
        }
    }
    if (fit[kClean].on && rightButton("clean", glyph::EyeOff, L"Clean output: hide all controls" + KeyHint(4), false)) SetClean(true);
    if (fit[kMini].on && rightButton("mini", glyph::PopOut, (m_miniOn ? L"Close the mini player" : L"Mini player: always-on-top window") + KeyHint(7), m_miniOn))
        SetMini(!m_miniOn);
    if (fit[kReplay].on && rightButton("savereplay", glyph::Refresh, L"Save replay" + KeyHint(1), false)) SaveReplay();
    if (fit[kFlash].on && rightButton("flashback", glyph::History, m_flash ? L"Back to live (Esc)" : L"Flashback: look back (\u2190)", m_flash != nullptr)) {
        if (m_flash) StopFlashback();
        else StartFlashback(10.0);
    }
    if (fit[kShare].on && rightButton("share", glyph::Share,
                    m_server ? L"Stop sharing on the network"
                             : (m_wireless ? L"Share on the local network" : L"Sharing is off: wireless features are off (Settings ▸ General)"),
                    m_server != nullptr, !m_wireless))
        ToggleSharing();
    if (fit[kMulti].on && rightButton("multiview", glyph::Grid, L"Show all devices (Tab)", m_multiview)) SetMultiview(!m_multiview);
    if (fit[kHelp].on && rightButton("help", glyph::Help, L"Keys and help (F1)", m_showHelp)) m_showHelp = !m_showHelp;
    if (fit[kStats].on && rightButton("stats", glyph::Stats, L"Statistics" + KeyHint(6), m_showStats)) {
        m_showStats = !m_showStats;
        m_settingsDirty = true;
    }

    if (m_advanced) {
        // Advanced mode: sources are managed in the side panel; show what's on air.
        if (Scene* sc = ProgramScene(); sc && rx - x - u.S(32) >= u.Measure(L"On air: ", u.S(13)) + u.S(24)) {   // not just an icon
            u.Icon(glyph::Video, {x, 0, u.S(18), H}, u.S(13), theme::Bad);
            u.Text(L"On air: " + sc->name, {x + u.S(24), 0, std::max(0.0f, rx - x - u.S(32)), H}, u.S(13), theme::Text);
        }
        u.SetAlpha(1.0f);
        return;
    }

    // Menus on the left, sized to the space that's left: device, format, sound source, each at least its narrowest
    // useful width (or not shown); spare room is shared out evenly up to their full widths.
    const float avail = rx - x - menuGap;
    const bool showFmt = avail >= minDev + menuGap + minFmt, showAud = showFmt && avail >= minDev + minFmt + minAud + 2 * menuGap;
    float devW = std::min(avail, minDev), fmtW = showFmt ? minFmt : 0.0f, audW = showAud ? minAud : 0.0f;
    float spare = avail - devW - (showFmt ? menuGap + fmtW : 0.0f) - (showAud ? menuGap + audW : 0.0f);
    std::pair<float*, float> menus[] = {{&devW, u.S(250)}, {&fmtW, showFmt ? u.S(230) : 0.0f}, {&audW, showAud ? u.S(260) : 0.0f}};
    for (int pass = 0; pass < 3 && spare > 0.5f; ++pass) {   // an even share each pass; what one can't take goes round again
        int growing = 0;
        for (const auto& [w, full] : menus) growing += *w < full ? 1 : 0;
        if (growing == 0) break;
        const float share = spare / float(growing);
        for (const auto& [w, full] : menus) {
            const float add = std::min(full - *w, share);
            if (add <= 0.0f) continue;
            *w += add;
            spare -= add;
        }
    }

    Session* active = Active();
    // device
    {
        std::vector<std::wstring> names;
        std::vector<size_t> index;   // menu row -> device
        int sel = -1;
        for (size_t i = 0; i < m_devices.video.size(); ++i) {
            if (!m_wirelessView && !ShownInSimple(m_devices.video[i])) continue;   // casts: the Wireless tab
            if (active && m_devices.video[i].symlink == active->info.symlink) sel = int(names.size());
            names.push_back(m_devices.video[i].name);
            index.push_back(i);
        }
        // Then: a device in its own window (each one not shown in a window yet), and hiding the device shown now
        // (a laptop's webcam, say; Settings ▸ Devices brings it back).
        std::vector<size_t> windowRows;
        if (!m_wirelessView)
            for (size_t i : index)
                if (m_devices.video[i].symlink.rfind(L"net:", 0) != 0 && !HasDeviceWindow(m_devices.video[i].symlink)) {
                    names.push_back(L"Open " + m_devices.video[i].name + L" in a new window");
                    windowRows.push_back(i);
                }
        const size_t hideRow = names.size();
        const bool canHide = active && active->video->IsLocalDevice() && !TestPattern::IsTestLink(active->info.symlink) && !m_wirelessView;
        if (canHide) names.push_back(L"Hide " + active->info.name + L" from this list");
        if (devW > u.S(60) && u.Dropdown("device", {x, by, devW, bh}, names, sel, glyph::Video, L"No capture devices")) {
            if (size_t(sel) >= hideRow) {
                m_hidePending = active->info.Key();   // not while the list is being drawn
            } else if (size_t(sel) >= index.size()) {
                m_windowPending = m_devices.video[windowRows[size_t(sel) - index.size()]];
            } else if (m_multiview) {
                int si = SessionIndex(m_devices.video[index[size_t(sel)]]);
                if (si >= 0) m_active = size_t(si);
            } else {
                SwitchTo(index[size_t(sel)]);
            }
        }
        x += devW + u.S(8);
    }
    active = Active();
    // format
    if (fmtW > 0 && active) {
        std::vector<VideoFormat> formats = active->video->Formats();
        std::vector<std::wstring> labels;
        VideoFormat cur = active->video->Current();
        int sel = -1;
        for (size_t i = 0; i < formats.size(); ++i) {
            labels.push_back(formats[i].Label());
            if (formats[i].Matches(cur)) sel = int(i);
        }
        if (u.Dropdown("format", {x, by, fmtW, bh}, labels, sel, 0, L"Format") && !active->video->IsNetwork()) {
            active->video->SetFormat(formats[size_t(sel)]);
            m_settings.SetString(Settings::DeviceSection(active->info.Key()), L"Format", formats[size_t(sel)].Key());
            active->surface.hasImage = false;
        }
        x += fmtW + u.S(8);
    }
    // audio input
    if (audW > 0 && active && active->video->OwnAudio()) {
        static const std::vector<std::wstring> netAudio{L"Audio from the network stream"};
        int sel = 0;
        u.Dropdown("audio", {x, by, audW, bh}, netAudio, sel, glyph::Microphone);
    } else if (audW > 0 && active) {
        int paired = DeviceManager::FindPairedAudio(m_devices, active->info);
        std::vector<std::wstring> items;
        items.push_back(paired >= 0 ? L"Auto: " + m_devices.audioIn[size_t(paired)].name : L"Auto: no matching audio");
        items.push_back(L"No audio");
        int sel = active->audioChoice == L"none" ? 1 : 0;
        for (size_t i = 0; i < m_devices.audioIn.size(); ++i) {
            items.push_back(m_devices.audioIn[i].name);
            if (m_devices.audioIn[i].id == active->audioChoice) sel = int(i) + 2;
        }
        if (u.Dropdown("audio", {x, by, audW, bh}, items, sel, glyph::Microphone)) {
            active->audioChoice = sel == 0 ? L"auto" : (sel == 1 ? L"none" : m_devices.audioIn[size_t(sel - 2)].id);
            m_settings.SetString(Settings::DeviceSection(active->info.Key()), L"Audio", active->audioChoice);
            UpdateAudioRouting(*active);
        }
    }
    u.SetAlpha(1.0f);
}

void App::DrawSettings()
{
    Ui& u = m_ui;
    const float W = u.Width(), H = u.Height(), barH = BarHeight();
    const float pw = std::min(u.S(380), W - u.S(20));
    RectF panel{W - pw - u.S(8), barH + u.S(8), pw, H - barH - u.S(16)};
    u.Fill(panel.Inset(-1), theme::Border, u.S(4));
    u.Fill(panel, theme::Panel, u.S(4));
    u.Fill({panel.x + u.S(1), panel.y, panel.w - u.S(2), u.S(2)}, theme::Accent);   // Windows 10 accent edge
    u.PushClip(panel);

    const float pad = u.S(18);
    const float cx = panel.x + pad, cw = panel.w - 2 * pad;
    float y = panel.y + pad - m_settingsScroll;
    const float rowH = u.S(34), labelH = u.S(22), gapS = u.S(10);

    // Title row: a big light "Settings" (Windows 10) and the light / dark button, which steps through
    // Follow Windows, Light and Dark (the same choice as General > Appearance).
    u.TextLight(L"Settings", {cx - u.S(2), y - u.S(4), cw - u.S(44), u.S(44)}, u.S(28), theme::Text);
    {
        static const wchar_t* const tips[] = {L"Appearance: follows Windows (click for light)", L"Appearance: light (click for dark)",
                                              L"Appearance: dark (click to follow Windows)"};
        const wchar_t icons[] = {glyph::System, glyph::Sun, glyph::Moon};
        const int mode = std::clamp(m_themeMode.load(), 0, 2);
        if (u.IconButton("themebtn", {cx + cw - u.S(34), y + u.S(1), u.S(34), u.S(34)}, icons[mode], tips[mode])) {
            m_themeMode = (mode + 1) % 3;
            m_settingsDirty = true;
            u.RequestFrame();
        }
    }
    y += u.S(50);

    // Each open section sits on a card (Windows 11 Settings). The card goes under the section's content, so it is
    // drawn first, as tall as the section was last frame; a change in height asks for one more frame.
    std::string cardId;
    float cardTop = 0;
    auto closeCard = [&](float end) {
        if (cardId.empty()) return;
        float& known = m_cardHeights[cardId];
        if (std::abs(known - (end - cardTop)) > 0.5f) {
            known = end - cardTop;
            u.RequestFrame();
        }
        cardId.clear();
    };
    auto section = [&](const char* id, const wchar_t* t) {
        closeCard(y);
        y += u.S(4);
        const bool open = PanelSection(id, t, y, cx, cw);
        if (open) {
            cardId = id;
            cardTop = y;
            if (auto it = m_cardHeights.find(id); it != m_cardHeights.end())
                u.Fill({cx - u.S(10), y - u.S(10), cw + u.S(20), it->second + u.S(6)}, theme::Card, u.S(theme::Radius + 2));
        }
        return open;
    };
    // `changed`: the setting differs from its default, so a small reset icon follows the label (returns true when clicked).
    auto label = [&](const std::wstring& t, const std::wstring& hint = L"", bool changed = false) {
        u.Text(t, {cx, y, cw, labelH}, u.S(13), theme::Text);
        if (!hint.empty()) u.Text(hint, {cx, y, cw, labelH}, u.S(12), theme::TextDim, Align::Right);
        bool reset = false;
        if (changed) {
            const std::string id = "reset:" + Narrow(t);
            reset = u.ResetIcon(id.c_str(), cx + u.Measure(t, u.S(13)) + u.S(4), y, labelH);
        }
        y += labelH + u.S(4);
        return reset;
    };
    // A one-line hint under a setting; wraps onto more lines when the panel is too narrow for it.
    auto note = [&](const std::wstring& t, Color c) {
        const float th = std::max(u.S(18), u.MeasureBlock(t, cw, u.S(11.5f)));
        u.TextBlock(t, {cx, y + u.S(1), cw, th + u.S(2)}, u.S(11.5f), c);
        y += th;
    };

    if (section("video", L"Video")) {
        if (const Session* s = Active(); s && !m_profileKey.empty()) {   // per-device profile (AppProfiles.cpp)
            note(L"Picture shape, scaling, Enhance, auto-crop, screen filter, sync on screen and crop / turn are saved for " + s->info.name + L".",
                 theme::TextDim);
            y += gapS;
        }
        if (label(L"Picture shape", PictureShapeHint(), m_aspect != 0)) {
            m_aspect = 0;
            m_settingsDirty = true;
        }
        if (u.Segmented("aspect", {cx, y, cw, rowH}, {L"Auto", L"4:3", L"16:9", L"Source pixels"}, m_aspect)) m_settingsDirty = true;
        y += rowH + gapS;
        DrawTransformSettings(y, cx, cw);
        if (label(L"Scaling", L"", m_scale != ScaleMode::Fit)) {
            m_scale = ScaleMode::Fit;
            m_settingsDirty = true;
        }
        int scale = int(m_scale);
        if (u.Segmented("scaling", {cx, y, cw, rowH}, {L"Fit", L"Fill", L"Stretch", L"Integer"}, scale)) {
            m_scale = ScaleMode(scale);
            m_settingsDirty = true;
        }
        y += rowH + gapS;
        if (u.Toggle("smooth", {cx, y, cw, rowH}, L"Smooth scaling", m_smooth, false, 1)) m_settingsDirty = true;
        y += rowH + gapS;
        if (u.Toggle("preview30", {cx, y, cw, rowH}, L"Limit preview to 30 fps", m_preview30, false, 0)) m_settingsDirty = true;
        y += rowH;
        note(L"For older PCs. Recordings and sharing keep the full frame rate.", theme::TextDim);
        y += gapS;
        if (label(L"Enhance", L"on screen only", m_enhance != 0)) {
            m_enhance = 0;
            m_settingsDirty = true;
        }
        if (u.Segmented("enhance", {cx, y, cw, rowH}, {L"Off", L"Sharpen", L"Upscale + Sharpen"}, m_enhance, m_renderer.Warp() ? 6u : 0u))
            m_settingsDirty = true;
        y += rowH;
        {
            const wchar_t* hint = m_renderer.Warp()      ? L"Needs a graphics card (software rendering would drop to half the frame rate)."
                                  : m_screenFilter > 0 ? L"Off while a screen filter is on."
                                  : !m_smooth       ? L"Needs Smooth scaling."
                                                    : L"Crisper picture for soft sources (Switch, 720p). Can't add real detail; recordings and streams keep the original.";
            note(hint, theme::TextDim);
            y += u.S(2) + gapS;
        }
        if (m_enhance > 0) {
            if (label(L"Enhance strength", Format(L"%.0f%%", m_enhanceStrength * 100.0f), std::abs(m_enhanceStrength - 0.5f) > 0.005f)) {
                m_enhanceStrength = 0.5f;
                m_settingsDirty = true;
            }
            if (u.Slider("enhancestr", {cx, y, cw, rowH}, m_enhanceStrength, 0.0f, 1.0f)) m_settingsDirty = true;
            y += rowH + gapS;
        }
        {
            // AI upscaling: NVIDIA RTX Video Super Resolution / Intel VSR. Offered only where it can work; the driver's
            // own switch is checked silently while this section is open (again after coming back from another app).
            using Probe = Renderer::SuperResProbe;
            const Renderer::SuperResSupportInfo sup = m_renderer.SuperResSupport();
            if (sup.ok && (m_renderer.SuperResProbeResult() == Probe::NotRun || m_srRecheck.exchange(false))) {
                m_renderer.ProbeSuperRes();
                m_renderer.RecheckSuperRes();
            }
            const Probe probe = m_renderer.SuperResProbeResult();
            const std::wstring whyNot = m_renderer.SuperResWhyNot();
            if (whyNot.empty() && probe == Probe::DriverOff) {   // what to switch on, before the user tries
                const std::wstring steps =
                    sup.nvidia ? (m_onBattery ? L"NVIDIA turns RTX Video Super Resolution off on battery: plug in the charger to use it."
                                              : L"Your NVIDIA driver has it switched off. Turn it on in the NVIDIA App ▸ Graphics ▸ Global settings ▸ RTX Video "
                                                L"Enhancement ▸ Super Resolution (or NVIDIA Control Panel ▸ Adjust video image settings ▸ RTX video enhancement). "
                                                L"ZeroDisplay checks again when you come back.")
                               : (m_onBattery ? L"Intel's driver may upscale only when plugged in: plug in the charger and come back."
                                              : L"This Intel graphics driver accepts the request but doesn't upscale. It needs Intel Xe or Arc graphics and a "
                                                L"current driver from Intel.");
                const float th = u.MeasureBlock(steps, cw, u.S(11.5f));
                u.TextBlock(steps, {cx, y + u.S(2), cw, th + u.S(2)}, u.S(11.5f), theme::Warn);
                y += th + u.S(8);
            }
            const bool usable = whyNot.empty();
            bool on = m_aiUpscale && usable;
            if (u.Toggle("aiupscale", {cx, y, cw, rowH}, sup.name.empty() ? std::wstring(L"AI upscaling") : L"AI upscaling (" + sup.name + L")", on,
                         !usable, 0)) {
                m_aiUpscale = on;
                m_settingsDirty = true;
            }
            y += rowH;
            std::wstring hint = m_renderer.SuperResText();
            if (!usable) hint = whyNot;
            else if (probe == Probe::Running) hint = L"Checking what the graphics driver does…";
            else if (hint.empty() || !m_aiUpscale)
                hint = L"Sharper enlarged picture from the graphics driver (e.g. 1080p on a 1440p / 4K screen). Screen only; uses more GPU.";
            else if (!m_smooth || ScreenFilter() != 0) hint = L"Needs Smooth scaling and no screen filter.";
            note(hint, m_renderer.SuperResState() == Renderer::SuperRes::Active ? theme::Good : theme::TextDim);
            y += u.S(2) + gapS;
        }
        if (label(L"Color matrix", L"", m_matrix != ColorMatrix(0))) {
            m_matrix = ColorMatrix(0);
            m_settingsDirty = true;
        }
        int matrix = int(m_matrix);
        if (u.Segmented("matrix", {cx, y, cw, rowH}, {L"Auto", L"BT.601", L"BT.709"}, matrix)) {
            m_matrix = ColorMatrix(matrix);
            m_settingsDirty = true;
        }
        y += rowH + gapS;
        if (label(L"Color range", L"", m_range != ColorRange(0))) {
            m_range = ColorRange(0);
            m_settingsDirty = true;
        }
        int range = int(m_range);
        if (u.Segmented("range", {cx, y, cw, rowH}, {L"Auto", L"Limited", L"Full"}, range)) {
            m_range = ColorRange(range);
            m_settingsDirty = true;
        }
        y += rowH + gapS;
        if (u.Toggle("uncompressed", {cx, y, cw, rowH}, L"Prefer uncompressed formats", m_preferUncompressed, false, 1)) m_settingsDirty = true;
        y += rowH;
        note(L"Uncompressed (YUY2) skips decoding, lowest latency.", theme::TextDim);
        y += gapS;
        if (u.Toggle("vsync", {cx, y, cw, rowH}, L"VSync", m_vsync, false, 0)) m_settingsDirty = true;
        y += rowH;
        note(L"Off = frames shown the instant they arrive (may tear).", theme::TextDim);
        y += gapS;
        if (u.Toggle("standby", {cx, y, cw, rowH}, L"Keep the last picture when the signal is lost", m_standbyOn, false, 1)) m_settingsDirty = true;
        y += rowH;
        note(L"Shown, recorded and shared until the signal returns.", theme::TextDim);
        y += gapS;
        if (m_standbyOn) {
            if (u.Toggle("standbyblack", {cx, y, cw, rowH}, L"Also when the picture stays black for 3 s", m_standbyOnBlack, false, 1)) m_settingsDirty = true;
            y += rowH + gapS;
        }
        if (u.Toggle("autocrop", {cx, y, cw, rowH}, L"Auto-crop black bars", m_autoCrop, false, 0)) m_settingsDirty = true;
        y += rowH;
        note(L"Zooms past letterbox / pillarbox borders on screen (recordings keep them).", theme::TextDim);
        y += gapS;
        if (label(L"Screen filter", L"retro consoles, on screen only", m_screenFilter != 0)) {
            m_screenFilter = 0;
            m_settingsDirty = true;
        }
        if (u.Dropdown("filter", {cx, y, cw, rowH}, {L"Off", L"Sharp pixels", L"Scanlines", L"CRT", L"LCD grid"}, m_screenFilter)) m_settingsDirty = true;
        y += rowH + gapS;
        if (m_screenFilter >= 2) {
            if (label(L"Original resolution", L"", m_filterLines != 0)) {
                m_filterLines = 0;
                m_settingsDirty = true;
            }
            static const int lines[] = {0, 240, 480, 720};
            int sel = 0;
            for (int i = 0; i < 4; ++i)
                if (lines[i] == m_filterLines) sel = i;
            if (u.Segmented("filterlines", {cx, y, cw, rowH}, {L"Source", L"240p", L"480p", L"720p"}, sel)) {
                m_filterLines = lines[sel];
                m_settingsDirty = true;
            }
            y += rowH;
            note(L"240p for SNES / Genesis / PS1-era games, 480p for PS2 / GameCube / Wii.", theme::TextDim);
            y += gapS;
            if (label(L"Filter strength", Format(L"%.0f%%", m_filterStrength * 100.0f), std::abs(m_filterStrength - 0.8f) > 0.005f)) {
                m_filterStrength = 0.8f;
                m_settingsDirty = true;
            }
            if (u.Slider("filterstr", {cx, y, cw, rowH}, m_filterStrength, 0.0f, 1.0f)) m_settingsDirty = true;
            y += rowH + gapS;
        }
        if (label(L"Graphics card", m_renderer.Warp() ? std::wstring(L"software") : m_renderer.AdapterName(), m_gpuChoice != -1)) {
            m_gpuChoice = -1;
            m_renderer.SetGpuChoice(m_gpuChoice);
            m_recreateRenderer = true;
            m_settingsDirty = true;
        }
        {
            if (m_adapters.empty()) m_adapters = Renderer::EnumerateAdapters();
            std::vector<std::wstring> items{L"Automatic"};
            for (const auto& a : m_adapters) items.push_back(a);
            items.push_back(L"Software (no GPU, slow)");
            int sel = m_gpuChoice == -1 ? 0 : (m_gpuChoice == -2 ? int(items.size()) - 1 : m_gpuChoice + 1);
            if (u.Dropdown("gpu", {cx, y, cw, rowH}, items, sel)) {
                m_gpuChoice = sel == 0 ? -1 : (sel == int(items.size()) - 1 ? -2 : sel - 1);
                m_renderer.SetGpuChoice(m_gpuChoice);
                m_recreateRenderer = true;
                m_settingsDirty = true;
            }
            y += rowH + gapS;
        }
        DrawPictureControls(y, cx, cw);
    }
    if (section("hdr", L"HDR (experimental)")) DrawHdrSettings(y, cx, cw);
    if (section("devices", L"Devices")) {
        // Every camera / capture card Windows reports, connected or remembered as hidden; off = left out everywhere.
        // The box renames it (menu, tiles, windows, file names); empty = Windows' name.
        struct Row {
            std::wstring key, autoName;
            bool connected;
        };
        std::vector<Row> rows;
        for (const auto& d : m_localDevices.video)
            if (!TestPattern::IsTestLink(d.symlink) && std::none_of(rows.begin(), rows.end(), [&](const Row& r) { return r.key == d.Key(); }))
                rows.push_back({d.Key(), d.autoName.empty() ? d.name : d.autoName, true});
        for (const std::wstring& k : m_hiddenDevices)
            if (std::none_of(rows.begin(), rows.end(), [&](const Row& r) { return r.key == k; })) rows.push_back({k, NameForKey(k), false});
        if (rows.empty()) {
            u.Text(L"No cameras or capture cards found.", {cx, y, cw, u.S(20)}, u.S(12), theme::TextDim);
            y += u.S(24);
        }
        const float swW = u.S(52);
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& r = rows[i];
            bool shown = !m_hiddenDevices.contains(r.key);
            const std::string tid = "devshow" + std::to_string(i), nid = "devname" + std::to_string(i);
            if (r.connected) {
                auto e = m_nameEdits.find(r.key);
                if (e == m_nameEdits.end()) e = m_nameEdits.emplace(r.key, m_settings.GetString(Settings::DeviceSection(r.key), L"Name", L"")).first;
                if (u.TextBox(nid.c_str(), {cx, y, cw - swW - u.S(8), rowH}, e->second, r.autoName)) RenameDevice(r.key, e->second);
            } else {
                u.Text(NameForKey(r.key) + L" (not connected)", {cx, y, cw - swW - u.S(8), rowH}, u.S(13), theme::TextDim);
            }
            if (u.Toggle(tid.c_str(), {cx + cw - swW, y, swW, rowH}, L"", shown)) SetDeviceHidden(r.key, !shown);
            y += rowH + u.S(6);
        }
        note(L"Type to rename a device (empty: Windows' name). Switched off: left out of the device menu, multiview and Advanced, "
             L"and never opened (a laptop's built-in camera, for example).",
             theme::TextDim);
        y += u.S(4) + gapS;
        // Copy the shown device's picture settings to another one (connected or hidden, not the test sources).
        if (const Session* shown = Active(); shown && !m_profileKey.empty()) {
            std::vector<std::wstring> keys, names;
            for (const Row& r : rows)
                if (r.key != m_profileKey) {
                    keys.push_back(r.key);
                    names.push_back(NameForKey(r.key));
                }
            int pick = -1;
            if (!keys.empty()) {
                if (u.Dropdown("copyprofile", {cx, y, cw, rowH}, names, pick, 0, L"Copy the settings of " + shown->info.name + L" to\u2026") &&
                    pick >= 0)
                    CopyProfileTo(keys[size_t(pick)]);
                y += rowH;
                note(L"Picture shape, scaling, filters, Enhance, sync on screen and crop / turn. The format and the sound source stay "
                     L"each device's own.",
                     theme::TextDim);
                y += u.S(4) + gapS;
            }
        }
    }
    if (section("audio", L"Audio")) {
        if (label(L"Output device", L"", !m_outputSetting.empty())) {
            m_outputSetting.clear();
            m_audio.Start(m_outputSetting);
            m_settingsDirty = true;
        }
        {
            std::vector<std::wstring> outs{L"System default"};
            int sel = 0;
            for (size_t i = 0; i < m_devices.audioOut.size(); ++i) {
                outs.push_back(m_devices.audioOut[i].name);
                if (m_devices.audioOut[i].id == m_outputSetting) sel = int(i) + 1;
            }
            if (u.Dropdown("output", {cx, y, cw, rowH}, outs, sel, glyph::Speaker)) {
                m_outputSetting = sel == 0 ? L"" : m_devices.audioOut[size_t(sel - 1)].id;
                m_audio.Start(m_outputSetting);
                m_settingsDirty = true;
            }
            y += rowH + gapS;
        }
        if (u.Toggle("passthrough", {cx, y, cw, rowH}, L"Play capture audio", m_passthrough, false, 1)) {
            m_audio.passthrough = m_passthrough;
            m_settingsDirty = true;
        }
        y += rowH + gapS;
        if (u.Toggle("exclusive", {cx, y, cw, rowH}, L"Exclusive output", m_exclusiveAudio, false, 0)) {
            m_audio.exclusive = m_exclusiveAudio;
            m_audio.Start(m_outputSetting);
            m_settingsDirty = true;
        }
        y += rowH;
        if (m_exclusiveAudio && m_audio.ExclusiveRejected())
            note(L"This audio driver can't do it reliably, using shared mode.", theme::Warn);
        else if (m_exclusiveAudio && m_audio.ExclusiveUnavailable())
            note(L"This output doesn't allow exclusive use right now (another app may have it), using shared mode.", theme::Warn);
        else
            note(L"Lowest audio latency; other apps can't play sound meanwhile.", theme::TextDim);
        y += gapS;
        if (label(L"Audio latency", Format(L"%.1f ms safety", AudioEngine::SafetyMs(AudioLatency(m_latency))), m_latency != int(AudioLatency::Low))) {
            m_latency = int(AudioLatency::Low);
            m_audio.latency = m_latency;
            m_settingsDirty = true;
        }
        if (u.Segmented("latency", {cx, y, cw, rowH}, {L"Ultra", L"Low", L"Safe"}, m_latency)) {
            m_audio.latency = m_latency;
            m_settingsDirty = true;
        }
        y += rowH + gapS;
        {
            // Sync on screen, one slider: left the sound waits (picture late), right the picture waits (sound late,
            // e.g. Bluetooth headphones). Saved per device; recordings and sharing are never shifted.
            const std::wstring now = m_delayMs > 0 ? Format(L"sound waits %d ms", m_delayMs)
                                   : m_delayMs < 0 ? Format(L"picture waits %d ms", -m_delayMs)
                                                   : std::wstring(L"off");
            if (label(L"Sync on screen", now, m_delayMs != 0)) {
                m_delayMs = 0;
                m_audio.avOffsetMs = 0;
                m_settingsDirty = true;
            }
            // Advanced mode: the canvas is recorded and shown as one, so only the sound can wait there.
            const float hi = m_advanced ? 0.0f : float(kMaxPictureDelayMs);
            float sync = std::min(float(-m_delayMs), hi);
            if (u.Slider("delay", {cx, y, cw, rowH}, sync, -300.0f, hi)) {
                int v = int(std::lround(sync));
                if (std::abs(v) < 8) v = 0;   // snaps to "off"
                m_delayMs = -v;
                m_audio.avOffsetMs = std::max(0, m_delayMs);
                m_settingsDirty = true;
            }
            if (!m_advanced) u.Fill({cx + cw * 0.5f - u.S(1), y + rowH * 0.5f - u.S(8), u.S(2), u.S(16)}, theme::TextDim);   // 0 in the middle
            y += rowH;
            u.Text(L"Picture earlier", {cx, y, cw, u.S(18)}, u.S(11.5f), theme::TextDim);
            u.Text(m_advanced ? L"Off" : L"Sound earlier", {cx, y, cw, u.S(18)}, u.S(11.5f), theme::TextDim, Align::Right);
            y += u.S(18);
            std::wstring hint;
            Color hintColor = theme::TextDim;
            if (m_advanced) {
                hint = L"Picture late? Move left: the sound waits (speakers only). In Advanced mode the picture can't wait for late sound; "
                       L"Simple mode can.";
            } else if (m_delayMs < 0) {
                // GPU memory for the waiting pictures: one copy per frame at the source's size and rate.
                const Session* s = Active();
                const double fps = s && s->shownFps > 1.0 ? s->shownFps : 60.0;
                const double frames = std::ceil(-m_delayMs * fps / 1000.0) + 2.0;
                const double mb = s && s->surface.width ? frames * s->surface.width * s->surface.height * 4.0 / (1024.0 * 1024.0) : 0.0;
                hint = Format(L"Adds %d ms to the picture: fine for watching, not for playing through ZeroDisplay. Recordings and sharing "
                              L"stay as they are.",
                              -m_delayMs);
                if (mb > 0) hint += Format(L" Uses about %.0f MB of graphics memory.", mb);
            } else if (OutputIsBluetooth() && m_delayMs >= 0) {
                hint = L"You're listening on Bluetooth: its sound usually arrives 100 to 300 ms late. Move right until speech matches the "
                       L"lips; the picture then waits for the sound. Saved for this device.";
                hintColor = theme::Warn;
            } else {
                hint = L"Sound late (Bluetooth headphones)? Move right: the picture waits. Picture late? Move left: the sound waits. "
                       L"Screen and speakers only, saved for this device.";
            }
            const float th = u.MeasureBlock(hint, cw, u.S(11.5f));
            u.TextBlock(hint, {cx, y + u.S(2), cw, th + u.S(2)}, u.S(11.5f), hintColor);
            y += th + u.S(6) + gapS;
        }
        if (label(L"Microphone (commentary)", m_micCap ? std::wstring(m_micCap->Failed() ? L"not working" : L"in recordings") : std::wstring(),
                  !m_micId.empty())) {
            m_micId.clear();
            m_settingsDirty = true;
            UpdateMic();
        }
        {
            std::vector<std::wstring> items{L"Off"};
            int sel = 0;
            for (size_t i = 0; i < m_devices.audioIn.size(); ++i) {
                items.push_back(m_devices.audioIn[i].name);
                if (m_devices.audioIn[i].id == m_micId) sel = int(i) + 1;
            }
            if (!m_micId.empty() && sel == 0) {
                items.push_back(L"(unplugged microphone)");
                sel = int(items.size()) - 1;
            }
            if (u.Dropdown("mic", {cx, y, cw, rowH}, items, sel, glyph::Microphone)) {
                if (sel == 0) m_micId.clear();
                else if (size_t(sel - 1) < m_devices.audioIn.size()) m_micId = m_devices.audioIn[size_t(sel - 1)].id;
                m_settingsDirty = true;
                UpdateMic();
            }
            y += rowH;
            if (m_micCap && m_micSrc) {   // what the mic hears now (it is never played back, so this shows it works)
                const float peak = std::max(m_micSrc->TakePeak(0), m_micSrc->TakePeak(1));
                m_micLevel = std::max(peak, m_micLevel * 0.8f);
                const float db = m_micLevel > 1e-5f ? 20.0f * std::log10(m_micLevel) : -100.0f;
                const float level = Clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
                const RectF bar{cx, y + u.S(6), cw - u.S(110), u.S(6)};
                u.Fill(bar, theme::Control, u.S(3));
                u.Fill({bar.x, bar.y, bar.w * level, bar.h}, m_micMuted ? theme::TextDim : (db > -3.0f ? theme::Bad : (db > -12.0f ? theme::Warn : theme::Good)), u.S(3));
                bool muted = m_micMuted;
                if (u.Toggle("micmuted", {cx + cw - u.S(100), y - u.S(6), u.S(100), u.S(30)}, L"Mute", muted, false, 0)) ToggleMicMute();
                y += u.S(22);
                InvalidateUi();   // the meter moves while it is shown
            }
            u.Text(m_micMuted ? std::wstring(L"Muted: left out of recordings, the replay buffer and sharing.")
                              : std::wstring(L"Mixed into recordings, the replay buffer and sharing."),
                   {cx, y, cw, u.S(18)}, u.S(11.5f), m_micMuted ? theme::Warn : theme::TextDim);
            y += u.S(18) + gapS;
            if (!m_micId.empty()) {
                if (label(L"Mic volume", Format(L"%.0f%%", m_micGain * 100.0f), std::abs(m_micGain - 1.0f) > 0.005f)) {
                    m_micGain = 1.0f;
                    m_settingsDirty = true;
                }
                if (u.Slider("micgain", {cx, y, cw, rowH}, m_micGain, 0.0f, 3.0f)) m_settingsDirty = true;
                y += rowH + gapS;
                if (u.Toggle("micgate", {cx, y, cw, rowH}, L"Noise gate (mute when quiet)", m_micGate, false, 1)) m_settingsDirty = true;
                y += rowH + gapS;
                if (u.Toggle("miccomp", {cx, y, cw, rowH}, L"Compressor (even out loud and quiet)", m_micComp, false, 1)) m_settingsDirty = true;
                y += rowH + gapS;
            }
        }
    }
    if (section("recording", L"Recording")) {
        if (label(L"Quality", Format(L"%u Mbps %s", BitrateKbps(m_quality) / 1000, m_hevc ? L"HEVC" : L"H.264"), m_quality != 2)) {
            m_quality = 2;
            m_settingsDirty = true;
        }
        if (u.Segmented("quality", {cx, y, cw, rowH}, {L"Low", L"Medium", L"High", L"Max"}, m_quality)) m_settingsDirty = true;
        y += rowH + gapS;
        if (label(L"Video format", L"", m_hevc)) {
            m_hevc = false;
            m_settingsDirty = true;
        }
        {
            int fmt = m_hevc ? 1 : 0;
            if (u.Segmented("codec", {cx, y, cw, rowH}, {L"H.264", L"HEVC (H.265)"}, fmt)) {
                m_hevc = fmt == 1;
                m_settingsDirty = true;
            }
            y += rowH;
            note(m_hevc ? L"Smaller files (viewers on the network always get H.264)." : L"Plays everywhere.", theme::TextDim);
            y += gapS;
        }
        {
            static const wchar_t* names[] = {L"Auto", L"Fast", L"Balanced", L"Best"};
            std::wstring now;
            if (m_encoder) now = std::wstring(L"now ") + names[PresetForQvs(m_encoder->Config().qualityVsSpeed)];
            if (m_encoderBehind) now = L"can't keep up!";
            if (label(L"Encoder", now, m_encoderSpeed != 0 && m_cmd.encoderSpeed < 0)) {
                m_encoderSpeed = 0;
                m_settingsDirty = true;
            }
            int sel = EncoderSpeed();
            if (u.Segmented("encspeed", {cx, y, cw, rowH}, {L"Auto", L"Fast", L"Balanced", L"Best"}, sel) && m_cmd.encoderSpeed < 0) {
                m_encoderSpeed = sel;
                m_settingsDirty = true;
            }
            y += rowH;
            static const wchar_t* hints[] = {L"Picks the best setting this PC runs in real time (checked once per format, remembered).",
                                             L"Quickest, least GPU load. A little less detail in fast motion at low bitrates.",
                                             L"The encoder's normal setting: good quality, real-time on most GPUs.",
                                             L"Most detail per bit. Needs a strong GPU, may drop frames."};
            std::wstring hint = m_encoderBehind && EncoderSpeed() != 0 ? L"The encoder can't keep up at this setting, choose a faster one."
                                                                       : hints[EncoderSpeed()];
            if (m_recorder && EncoderSpeed() != 0) hint += L" (Applies to the next recording.)";
            note(hint, m_encoderBehind && EncoderSpeed() != 0 ? theme::Warn : theme::TextDim);
            y += u.S(2) + gapS;
        }
        {
            // Encoding hardware. Looking for a second GPU takes ~100 ms: done in the background, once.
            const bool hevc = WantHevc();
            const bool known = ProbeOtherGpu(hevc);
            EncoderAdapter other;
            const bool hasOther = known && OtherEncoderGpu(hevc, other);
            std::wstring now;
            if (m_encoder) now = m_encoder->Target() == EncodeTarget::OtherGpu ? L"now " + m_encoder->Config().otherAdapterName
                                                                                : L"now " + m_encoder->TargetName();
            if (label(L"Encoding hardware", now, m_encodeHw != 0 && m_cmd.encodeHw < 0 && !m_cmd.softwareEncoder)) {
                m_encodeHw = 0;
                m_settingsDirty = true;
            }
            int sel = EncodeHw();
            const bool locked = m_cmd.encodeHw >= 0 || m_cmd.softwareEncoder;
            if (u.Segmented("enchw", {cx, y, cw, rowH}, {L"Auto", L"Hybrid", L"Display GPU", L"Software"}, sel, known && !hasOther && sel != 1 ? 2u : 0u) &&
                !locked) {
                m_encodeHw = sel;
                m_settingsDirty = true;
            }
            y += rowH;
            std::wstring hint;
            switch (EncodeHw()) {
            case 0:
                hint = !known    ? L"Looking for a second GPU\u2026"
                       : hasOther ? L"Records on " + other.name + L"; the display GPU keeps the picture and sharing. Moves back if it can't keep up."
                                  : L"Records on the display GPU (this PC has no second GPU with a hardware encoder).";
                break;
            case 1:
                hint = !known    ? L"Looking for a second GPU\u2026"
                       : hasOther ? L"Records on " + other.name + L" while the display GPU shows the picture. Sharing stays on the display GPU."
                                  : L"Needs a second GPU with a hardware encoder, this PC records on the display GPU.";
                break;
            case 2: hint = L"Records and shares on the GPU that shows the picture (no copies, lowest latency)."; break;
            default: hint = L"Recording and sharing are encoded by the CPU. Uses much more CPU; for PCs whose GPU encoder misbehaves."; break;
            }
            if (m_recorder && m_encoder && m_encoderRequest.target != WantedEncoder(m_encoder->Config()).target) hint += L" (Applies to the next recording.)";
            note(hint, EncodeHw() == 1 && known && !hasOther ? theme::Warn : theme::TextDim);
            y += u.S(2) + gapS;
        }
        if (label(L"Split into files every", L"", m_splitMinutes != 0)) {
            m_splitMinutes = 0;
            m_settingsDirty = true;
        }
        {
            static const int splits[] = {0, 15, 30, 60};
            int sel = 0;
            for (int i = 0; i < 4; ++i)
                if (splits[i] == m_splitMinutes) sel = i;
            if (u.Segmented("split", {cx, y, cw, rowH}, {L"Off", L"15 min", L"30 min", L"60 min"}, sel)) {
                m_splitMinutes = splits[sel];
                m_settingsDirty = true;
            }
            y += rowH + gapS;
        }
        if (label(L"Stop recording after", L"", m_limitMinutes != 0)) {
            m_limitMinutes = 0;
            m_settingsDirty = true;
        }
        {
            static const int limits[] = {0, 30, 60, 120};
            int sel = 0;
            for (int i = 0; i < 4; ++i)
                if (limits[i] == m_limitMinutes) sel = i;
            if (u.Segmented("limit", {cx, y, cw, rowH}, {L"Never", L"30 min", L"1 hour", L"2 hours"}, sel)) {
                m_limitMinutes = limits[sel];
                m_settingsDirty = true;
            }
            y += rowH + gapS;
        }
        if (label(L"Stop recording without a signal after", L"", m_stopNoSignalMinutes != 0)) {
            m_stopNoSignalMinutes = 0;
            m_settingsDirty = true;
        }
        {
            static const int mins[] = {0, 1, 5, 10};
            int sel = 0;
            for (int i = 0; i < 4; ++i)
                if (mins[i] == m_stopNoSignalMinutes) sel = i;
            if (u.Segmented("nosignal", {cx, y, cw, rowH}, {L"Never", L"1 min", L"5 min", L"10 min"}, sel)) {
                m_stopNoSignalMinutes = mins[sel];
                m_settingsDirty = true;
            }
            y += rowH;
            note(L"Console off or cable out: otherwise the last picture keeps being recorded (a warning shows after 5 s).", theme::TextDim);
            y += gapS;
        }
        if (u.Toggle("soundcue", {cx, y, cw, rowH}, L"Sound cues", m_soundCue, false, 0)) {
            m_settingsDirty = true;
            if (m_soundCue) PlayCue(Cue::Start);   // what it sounds like
        }
        y += rowH;
        note(L"A short sound when recording starts or stops and when a replay is saved.", theme::TextDim);
        y += gapS;
        if (u.Toggle("tracks", {cx, y, cw, rowH}, L"Separate audio tracks (mix + each source)", m_separateTracks, false, 0)) m_settingsDirty = true;
        y += rowH;
        if (m_separateTracks) {   // what the next recording really gets
            size_t inputs = 0;
            for (const auto& in : m_mixInputs) inputs += in.source != nullptr;
            const std::wstring tracks = m_mixer.Running() ? Format(L"Recordings get %zu tracks: the mix, then each source.", 1 + inputs)
                                                          : L"Only with a microphone on or in Advanced mode: now there's one sound, so one track.";
            note(tracks, theme::TextDim);
        }
        y += gapS;
        if (u.Toggle("replay", {cx, y, cw, rowH}, L"Replay buffer" + (KeyHint(1).empty() ? std::wstring() : KeyHint(1).substr(0, KeyHint(1).size() - 1) + L" saves)"), m_replayOn, false, 0)) {
            m_settingsDirty = true;
            UpdateReplay();
        }
        y += rowH;
        if (m_replayOn) {
            if (label(L"Replay length", Format(L"%.0f s  ·  ~%.0f MB of memory", m_replaySeconds, ReplayMegabytes()), m_replaySeconds != 30.0f)) {
                m_replaySeconds = 30.0f;
                m_settingsDirty = true;
                if (m_replay) m_replay->SetSeconds(m_replaySeconds);
            }
            if (u.Slider("replaysec", {cx, y, cw, rowH}, m_replaySeconds, 10.0f, 300.0f)) {
                m_replaySeconds = std::round(m_replaySeconds / 5.0f) * 5.0f;
                m_settingsDirty = true;
                if (m_replay) m_replay->SetSeconds(m_replaySeconds);   // keeps what's buffered
            }
            y += rowH;
        }
        y += gapS;
        if (u.Button("trimfile", {cx, y, u.S(200), rowH}, L"Trim a recording\u2026", glyph::Cut)) PickTrimFile();
        y += rowH + gapS;
    }
    if (section("files", L"Files")) {
        // Folders: where they are now, change / open / back to the default.
        auto folderRow = [&](int which, const wchar_t* title, const std::wstring& folder, std::wstring& setting) {
            label(title, setting.empty() ? L"default" : L"");
            const float th = u.MeasureBlock(folder, cw, u.S(11.5f));
            u.TextBlock(folder, {cx, y, cw, th + u.S(2)}, u.S(11.5f), theme::TextDim);
            y += th + u.S(6);
            const float bw = (cw - u.S(16)) / 3;
            const std::string idc = "fchange" + std::to_string(which), ido = "fopen" + std::to_string(which), idd = "fdefault" + std::to_string(which);
            if (u.Button(idc.c_str(), {cx, y, bw, rowH}, L"Change\u2026", glyph::Folder)) PickFolder(which);
            if (u.Button(ido.c_str(), {cx + bw + u.S(8), y, bw, rowH}, L"Open")) {
                m_graveyard.push_back(std::async(std::launch::async, [folder] {
                    ComInit com(COINIT_APARTMENTTHREADED);
                    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
                    ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }));
            }
            if (u.Button(idd.c_str(), {cx + 2 * (bw + u.S(8)), y, bw, rowH}, L"Default", 0, false, setting.empty())) {
                setting.clear();
                m_settingsDirty = true;
            }
            y += rowH + gapS;
        };
        folderRow(0, L"Recordings, replays and clips", RecordFolder(), m_recordFolderSetting);
        folderRow(1, L"Screenshots", ScreenshotFolder(), m_shotFolderSetting);

        if (label(L"File names", m_nameTemplate == kDefaultNameTemplate ? L"default" : L"", m_nameTemplate != kDefaultNameTemplate)) {
            m_nameTemplate = kDefaultNameTemplate;
            m_settingsDirty = true;
        }
        if (u.TextBox("nametmpl", {cx, y, cw, rowH}, m_nameTemplate, kDefaultNameTemplate)) {
            if (m_nameTemplate.empty()) m_nameTemplate = kDefaultNameTemplate;
            m_settingsDirty = true;
        }
        y += rowH + u.S(4);
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            Session* s = Active();
            const std::wstring next = ExpandNameTemplate(m_nameTemplate, L"ZeroDisplay", m_advanced || !s ? std::wstring() : s->info.name, st);
            const std::wstring help = L"Next recording: " + next + L".mp4\n{type} {device} {date} {time}; characters Windows doesn't allow become _.";
            const float th = u.MeasureBlock(help, cw, u.S(11.5f));
            u.TextBlock(help, {cx, y, cw, th + u.S(2)}, u.S(11.5f), theme::TextDim);
            y += th + u.S(4) + gapS;
        }

        if (label(L"Screenshot format", L"", m_shotFormat != 0)) {
            m_shotFormat = 0;
            m_settingsDirty = true;
        }
        if (u.Segmented("shotfmt", {cx, y, cw, rowH}, {L"PNG (exact)", L"JPG (smaller)"}, m_shotFormat)) m_settingsDirty = true;
        y += rowH + u.S(4);
        if (u.Toggle("shotclip", {cx, y, cw, rowH}, L"Also copy screenshots to the clipboard", m_shotClipboard, false, 0)) m_settingsDirty = true;
        y += rowH;
        if (u.Toggle("shotflash", {cx, y, cw, rowH}, L"Flash the screen when one is taken", m_shotFlash, false, 1)) m_settingsDirty = true;
        y += rowH + gapS;

        static const int freeGb[] = {1, 2, 5, 10, 20};
        int freeSel = 0;
        for (int i = 0; i < 5; ++i)
            if (freeGb[i] == m_minFreeGb) freeSel = i;
        if (label(L"Stop recording below this much free space", DiskFreeHint().empty() ? std::wstring() : DiskFreeHint().substr(2, DiskFreeHint().size() - 3),
                  m_minFreeGb != 1)) {
            m_minFreeGb = 1;
            m_settingsDirty = true;
        }
        if (u.Segmented("minfree", {cx, y, cw, rowH}, {L"1 GB", L"2", L"5", L"10", L"20"}, freeSel)) {
            m_minFreeGb = freeGb[freeSel];
            m_settingsDirty = true;
        }
        y += rowH + gapS;
        static const int keepGb[] = {0, 50, 100, 250, 500};
        int keepSel = 0;
        for (int i = 0; i < 5; ++i)
            if (keepGb[i] == m_keepGb) keepSel = i;
        if (label(L"Keep recordings under", L"", m_keepGb != 0)) {
            m_keepGb = 0;
            m_keepGbAsked = -1;
            m_settingsDirty = true;
        }
        if (u.Segmented("keepgb", {cx, y, cw, rowH}, {L"No limit", L"50 GB", L"100", L"250", L"500"}, keepSel)) {
            if (keepGb[keepSel] == 0) {
                m_keepGb = 0;
                m_keepGbAsked = -1;
                m_settingsDirty = true;
            } else {
                m_keepGbAsked = keepGb[keepSel];   // deletes files: asked below first
            }
        }
        y += rowH + u.S(4);
        if (m_keepGbAsked > 0) {
            note(Format(L"Above %d GB, the oldest recordings, replays and clips ZeroDisplay made go to the Recycle Bin (other files "
                        L"in the folder are never touched). Turn it on?", m_keepGbAsked),
                 theme::Warn);
            y += u.S(4);
            const float bw = (cw - u.S(8)) / 2;
            if (u.Button("keepyes", {cx, y, bw, rowH}, L"Turn on", 0, true)) {
                m_keepGb = m_keepGbAsked;
                m_keepGbAsked = -1;
                m_settingsDirty = true;
                m_graveyard.push_back(std::async(std::launch::async, [this] { EnforceKeepLimit(); }));
            }
            if (u.Button("keepno", {cx + bw + u.S(8), y, bw, rowH}, L"Cancel")) m_keepGbAsked = -1;
            y += rowH + gapS;
        } else {
            note(m_keepGb > 0 ? Format(L"On: above %d GB the oldest recordings ZeroDisplay made go to the Recycle Bin.", m_keepGb.load())
                              : std::wstring(L"Off: nothing is ever deleted."),
                 theme::TextDim);
            y += u.S(2) + gapS;
        }
    }
    if (section("network", L"Network")) {
        {
            bool sharing = m_server != nullptr;
            if (u.Toggle("sharing", {cx, y, cw, rowH}, L"Share on the local network", sharing, !m_wireless)) ToggleSharing();
            y += rowH;
            std::wstring info = L"Other PCs running ZeroDisplay will list this PC under “Network:” in the device menu.";
            if (!m_wireless) {
                info = L"Off: wireless features are off (General ▸ Wireless features). Nothing is shared or found on the network.";
            } else if (m_server) {
                static std::wstring ips;
                static int64_t ipsAt = 0;
                if (ips.empty() || QpcElapsedMs(ipsAt) > 5000) {
                    ipsAt = QpcNow();
                    ips.clear();
                    for (const auto& ip : net::LocalIPv4()) ips += (ips.empty() ? L"" : L", ") + ip;
                }
                info += Format(L"\nThis PC: %s (UDP %u), %d viewer(s), %.1f Mbps", ips.c_str(), m_server->Port(), m_server->Viewers(),
                               m_server->Kbps() / 1000.0);
                if (m_web) {
                    std::wstring first = ips.substr(0, ips.find(L','));
                    info += Format(L"\nBrowsers (phone, tablet, TV): http://%s:%u, %d watching", first.empty() ? L"localhost" : first.c_str(), m_web->Port(),
                                   m_web->Viewers());
                }
            } else {
                info += L"\nPhones and other devices can also watch in a browser.\nWindows Firewall may ask to allow ZeroDisplay the first time.";
            }
            const float ih = u.MeasureBlock(info, cw, u.S(11.5f));   // as tall as the text (it was a fixed 92 px)
            u.TextBlock(info, {cx, y + u.S(4), cw, ih + u.S(2)}, u.S(11.5f), theme::TextDim);
            y += ih + u.S(8) + gapS;
            // The sharing PIN: other PCs and browsers get the picture only with it.
            bool pinOn = m_viewerPinOn;
            if (u.Toggle("viewerpin", {cx, y, cw, rowH}, L"Viewers need a PIN", pinOn, !m_wireless)) SetViewerPin(pinOn, m_viewerPin);
            y += rowH;
            if (m_viewerPinOn) {
                const std::string pin = m_pinGate.Pin();   // this run's (--pin for tests)
                const float bw = u.S(110);
                u.Text(L"PIN", {cx, y, u.S(40), rowH}, u.S(13), theme::TextDim);
                u.Text(Widen(pin.substr(0, 3) + " " + pin.substr(3)), {cx + u.S(40), y, cw - bw - u.S(48), rowH}, u.S(20), theme::Text, Align::Left,
                       true);
                if (u.Button("newpin", {cx + cw - bw, y, bw, rowH}, L"New PIN", 0, false, !m_wireless)) {
                    SetViewerPin(true, net::PinGate::NewPin());
                    Toast(L"New sharing PIN: " + Widen(m_viewerPin) + L". Viewers type it once.");
                }
                y += rowH;
                note(L"Other PCs and browsers type it once and remember it; a new PIN asks them again. The QR code in Wireless "
                     L"contains it.",
                     theme::TextDim);
            } else {
                note(L"Off: anyone on this network can watch while sharing is on.", theme::Warn);
            }
            y += u.S(2) + gapS;
            const int q = StreamQuality();
            if (label(L"Stream quality", m_shareEncoder ? Format(L"now %.1f Mbps", StreamKbps() / 1000.0) : std::wstring(),
                      m_streamQuality != 0 && m_cmd.streamQuality < 0)) {
                m_streamQuality = 0;
                m_rateHoldUntil = 0;
                m_settingsDirty = true;
            }
            int sel = q;
            if (u.Segmented("streamq", {cx, y, cw, rowH}, {L"Smart", L"4 Mbps", L"8", L"15", L"25"}, sel) && m_cmd.streamQuality < 0) {
                m_streamQuality = sel;
                m_settingsDirty = true;
                if (sel == 0) m_rateHoldUntil = 0;
                else ChangeStreamBitrate(StreamPresetKbps(sel));
            }
            y += rowH;
            std::wstring hint = q == 0 ? L"Smart follows the slowest viewer's connection (drops fast when anyone falls behind, climbs back slowly)."
                                       : L"Fixed. Higher looks better but needs a fast connection for every viewer.";
            if (q == 0 && !m_rateReason.empty()) hint += L"\nLast lowered because " + m_rateReason + L".";
            y += u.S(3);
            note(hint, theme::TextDim);
            y += u.S(4) + gapS;
        }
    }
    if (section("power", L"Power & memory")) {
        if (label(L"Battery saver", m_onBattery ? L"on battery now" : L"plugged in", m_batterySaver != 1)) {
            m_batterySaver = 1;
            m_settingsDirty = true;
        }
        if (u.Segmented("battsaver", {cx, y, cw, rowH}, {L"Off", L"On battery", L"Always"}, m_batterySaver)) m_settingsDirty = true;
        y += rowH;
        note(L"Replay buffer and Enhance off, recording on the display GPU so the second GPU sleeps, fewer wake-ups. "
                    L"The preview stays smooth.", theme::TextDim);
        y += u.S(2) + gapS;
        if (label(L"Memory saver", m_lowMemory ? L"Windows is low on memory" : L"", m_memorySaver != 1)) {
            m_memorySaver = 1;
            m_settingsDirty = true;
        }
        if (u.Segmented("memsaver", {cx, y, cw, rowH}, {L"Off", L"When low", L"Always"}, m_memorySaver)) m_settingsDirty = true;
        y += rowH;
        note((m_replay ? Format(L"Replay buffer (~%.0f MB now)", ReplayMegabytes()) : std::wstring(L"Replay buffer")) +
                 L" and Flashback off, Enhance off, images of scenes not shown freed.",
             theme::TextDim);
        y += u.S(2) + gapS;
        if (u.Toggle("lowres", {cx, y, cw, rowH}, L"Low-resource mode", m_lowResource, false, 0)) m_settingsDirty = true;
        y += rowH;
        note(L"For older PCs: both savers, plus preview at 30 fps, no screen filters, studio or multiview, sharing at 720p and at most 6 Mbps.", theme::TextDim);
        y += u.S(2) + gapS;
        {
            bool eco = g_backgroundEco;
            if (u.Toggle("ecobg", {cx, y, cw, rowH}, L"Efficiency mode for background work", eco, false, 1)) {
                g_backgroundEco = eco;
                m_settingsDirty = true;
            }
            y += rowH;
            note(L"File saving, discovery and the welcome page run on efficient cores at a low clock. Picture, sound and "
                        L"encoding stay at full speed.", theme::TextDim);
            y += u.S(2) + gapS;
        }
        {
            const std::wstring active = ProfileSummary();
            if (!active.empty()) {
                note(L"Active: " + active, theme::Warn);
                y += u.S(4) + gapS;
            }
        }
    }
    if (section("hotkeys", L"Hotkeys")) {
        if (u.Toggle("hotkeys", {cx, y, cw, rowH}, L"Global hotkeys (work while another app is in front)", m_globalHotkeys, false, 1)) {
            m_settingsDirty = true;
            PostMessageW(m_hwnd, WM_APP + 3, m_globalHotkeys ? 1 : 0, 0);
        }
        y += rowH;
        if (m_globalHotkeys && m_hotkeysFailed > 0) {
            note(Format(L"%d global key(s) are already used by another app: marked \"taken\" below.", m_hotkeysFailed.load()), theme::Warn);
        }
        y += u.S(6);
        DrawHotkeyRows(y, cx, cw);
        y += gapS;
    }
    if (section("general", L"General")) {
        {
            int mode = std::clamp(m_themeMode.load(), 0, 2);
            const std::wstring hint = mode == 0 ? (m_systemDark ? L"Windows is dark now" : L"Windows is light now") : L"";
            if (label(L"Appearance", hint, mode != 0)) {
                m_themeMode = 0;
                m_settingsDirty = true;
                u.RequestFrame();
            }
            if (u.Segmented("theme", {cx, y, cw, rowH}, {L"Follow Windows", L"Light", L"Dark"}, mode)) {
                m_themeMode = mode;
                m_settingsDirty = true;
                u.RequestFrame();
            }
            y += rowH + gapS;
        }
        {
            bool wireless = m_wireless;
            if (u.Toggle("wireless", {cx, y, cw, rowH}, L"Wireless features", wireless, false, 1)) SetWireless(wireless);
            y += rowH;
            note(m_wireless ? L"Cast receiver, this PC's own Wi-Fi, sharing on the network and network sources. Off: nothing listens "
                                     L"on the network."
                                   : L"Off: no cast receiver, own Wi-Fi, sharing or network sources. Nothing listens on the network.", theme::TextDim);
            y += u.S(2) + gapS;
        }
        {
            bool ask = m_askBeforeClose;
            if (u.Toggle("askclose", {cx, y, cw, rowH}, L"Ask before closing while recording or sharing", ask, false, 1)) {
                m_askBeforeClose = ask;
                m_settingsDirty = true;
            }
            y += rowH + gapS;
        }
        if (u.Toggle("keepawake", {cx, y, cw, rowH}, L"Keep the screen on while a live picture shows", m_keepAwake, false, 1)) m_settingsDirty = true;
        y += rowH;
        u.Text(m_awakeDisplay || m_awakeSystem ? (m_awakeDisplay ? L"Now: the screen stays on." : L"Now: the PC stays awake while recording / sharing.")
                                               : L"Also keeps the PC awake while recording or sharing.",
               {cx, y, cw, u.S(18)}, u.S(11.5f), theme::TextDim);
        y += u.S(18) + gapS;
        if (label(L"Mini player opacity", Format(L"%.0f%%", m_miniOpacity * 100.0f), m_miniOpacity < 0.995f)) {
            m_miniOpacity = 1.0f;
            m_settingsDirty = true;
        }
        if (u.Slider("miniopacity", {cx, y, cw, rowH}, m_miniOpacity, 0.3f, 1.0f)) m_settingsDirty = true;
        y += rowH;
        note(L"Less than 100%: the mini player (F6) is see-through, over a game for example.", theme::TextDim);
        y += gapS;
        if (u.Toggle("autoswitch", {cx, y, cw, rowH}, L"Switch to newly connected devices", m_autoSwitch, false, 1)) m_settingsDirty = true;
        y += rowH + gapS;
        if (u.Toggle("startfs", {cx, y, cw, rowH}, L"Start in full screen", m_startFullscreen, false, 0)) m_settingsDirty = true;
        y += rowH + gapS;
        if (u.Toggle("stats", {cx, y, cw, rowH}, L"Show statistics", m_showStats, false, 0)) m_settingsDirty = true;
        y += rowH + gapS;
        if (u.Button("lagcheck", {cx, y, u.S(230), rowH}, m_lag ? L"Measuring\u2026" : L"Measure capture latency", glyph::Stats) && !m_lag)
            StartLagCheck();
        y += rowH;
        y += u.S(3);
        note(m_lagResult.empty() ? std::wstring(L"Flashes a screen that feeds the capture card (e.g. this PC's HDMI output into the card) and times "
                                                L"how long until the flash arrives here.")
                                 : m_lagResult,
             m_lagResult.empty() ? theme::TextDim : theme::Text);
        y += u.S(4) + gapS * 2;
        {
            bool portable = m_settings.Portable();
            if (u.Toggle("portable", {cx, y, cw, rowH}, L"Portable (keep everything next to ZeroDisplay.exe)", portable)) {
                if (m_cmd.selftestSec > 0 && m_cmd.profileDir.empty()) Toast(L"Not during a self-test");
                else SetPortableMode(portable);
            }
            y += rowH;
            note(m_settings.Portable() ? L"Settings here, recordings in Videos and screenshots in Screenshots next to the exe (unless you chose a "
                                                L"recording folder). The log moves after a restart."
                                              : L"Copy the ZeroDisplay folder to another PC and everything comes along.", theme::TextDim);
            y += u.S(2) + gapS;
        }
    }
    {   // the settings as a whole
        const float bw = (cw - u.S(16)) / 3;
        const bool locked = Settings::s_readOnly;   // self-tests on the user's profile
        if (u.Button("export", {cx, y, bw, rowH}, L"Export\u2026", 0, false, locked)) PickSettingsFile(true);
        if (u.Button("import", {cx + bw + u.S(8), y, bw, rowH}, L"Import\u2026", 0, false, locked)) PickSettingsFile(false);
        if (u.Button("resetall", {cx + 2 * (bw + u.S(8)), y, bw, rowH}, L"Reset all\u2026", 0, false, locked)) {
            m_resetAsked = true;
            m_importAsked.clear();
        }
        y += rowH + u.S(6);
        std::wstring ask;
        if (m_resetAsked) ask = L"Every setting goes back to the default (folders, hotkeys, hidden devices, per-device choices). Scenes are kept. ZeroDisplay restarts.";
        else if (!m_importAsked.empty()) ask = L"Settings and scenes from " + m_importAsked + L" replace the ones here. ZeroDisplay restarts.";
        if (!ask.empty()) {
            const float th = u.MeasureBlock(ask, cw, u.S(11.5f));
            u.TextBlock(ask, {cx, y, cw, th + u.S(2)}, u.S(11.5f), theme::Warn);
            y += th + u.S(6);
            const float hb = (cw - u.S(8)) / 2;
            if (u.Button("confirmall", {cx, y, hb, rowH}, m_resetAsked ? L"Reset and restart" : L"Import and restart", 0, true)) {
                if (m_resetAsked) RequestRestart(OnExit::Reset);
                else {
                    m_importPath = m_importAsked;
                    RequestRestart(OnExit::Import);
                }
                m_resetAsked = false;
                m_importAsked.clear();
            }
            if (u.Button("cancelall", {cx + hb + u.S(8), y, hb, rowH}, L"Cancel")) {
                m_resetAsked = false;
                m_importAsked.clear();
            }
            y += rowH + u.S(6);
        } else {
            note(L"Export saves settings and scenes to a file (for another PC); Import reads one back.", theme::TextDim);
            y += u.S(4);
        }
        y += gapS;
    }
    closeCard(y);
    y += u.S(6);
    {
        // The whole path, however long (it was cut off after two lines).
        const std::wstring where = (m_settings.Portable() ? L"Portable mode: " : L"Settings: ") + m_settings.Path();
        const float th = u.MeasureBlock(where, cw, u.S(11));
        u.TextBlock(where, {cx, y, cw, th + u.S(2)}, u.S(11), theme::TextDim);
        y += th + u.S(6);
    }
    u.Logo({cx, y + u.S(2), u.S(14), u.S(14)});
    u.Text(std::wstring(L"ZeroDisplay ") + CC_VERSION, {cx + u.S(20), y, cw - u.S(20), u.S(18)}, u.S(11), theme::TextDim);
    y += u.S(18) + pad;

    u.PopClip();
    const float content = y + m_settingsScroll - panel.y;
    const float maxScroll = std::max(0.0f, content - panel.h);
    if (u.Hot(panel) && u.Input().wheel != 0 && !u.PopupOpen()) m_settingsScroll -= u.Input().wheel * u.S(60);
    m_settingsScroll = Clamp(m_settingsScroll, 0.0f, maxScroll);
}

std::wstring App::StatsText(const Session& s) const
{
    std::wstring t;
    VideoFormat f = s.video->Current();
    CaptureStats cs = s.video->Stats();
    const wchar_t* path = s.video->IsNetwork() ? (s.surface.gpu ? L"network, GPU decode" : L"network, CPU decode")
                        : s.surface.gpu         ? (f.IsCompressed() ? L"GPU decode" : L"GPU copy")
                                                : (f.IsCompressed() ? L"CPU decode" : L"CPU upload");
    t += s.info.name + L"\n";
    const UsbLink link = s.video->Link();
    t += Format(L"  %s  (%s%s%s)\n", f.Label().c_str(), path, link != UsbLink::Unknown ? L", " : L"", UsbLinkName(link));
    if (m_renderer.SuperResState() != Renderer::SuperRes::Off) t += L"  AI upscaling: " + m_renderer.SuperResText() + L"\n";
    t += Format(L"  capture %.1f fps   shown %.1f fps   skipped %llu   gaps %llu   on screen %.1f fps%s\n", cs.fps, s.shownFps, cs.replaced, cs.ticks,
                m_presentFps, PreviewCapped() ? L" (30 fps limit)" : L"");
    t += Format(L"  latency  arrival→present %.2f ms", s.latArrivalMs);
    if (s.captureLatValid) t += Format(L"   capture→present %.2f ms", s.latCaptureMs);
    t += L"\n";
    if (s.prof.delayMs < 0 && !s.pictureDelay.Empty())   // Sync on screen: the picture waits for late sound
        t += Format(L"  picture waits %d ms   shown picture %.0f ms old   %.0f MB\n", -s.prof.delayMs, s.pictureDelay.ShownAgeMs(),
                    double(s.pictureDelay.Bytes()) / (1024.0 * 1024.0));
    t += Format(L"  color %s %s%s\n",
                s.surface.matrix == ColorMatrix::BT601 ? L"BT.601" : (s.surface.matrix == ColorMatrix::BT2020 ? L"BT.2020" : L"BT.709"),
                s.surface.range == ColorRange::Full ? L"full" : L"limited",
                s.surface.transfer != Transfer::Sdr ? (std::wstring(L"   ") + TransferName(s.surface.transfer) + L", tone-mapped for this screen").c_str() : L"");
    if (s.audioCap || s.video->OwnAudio()) {
        t += L"  audio in  " + (s.audioCap ? s.audioCap->Description() : Format(L"network stream, %u Hz", s.audioSrc->InputRate())) + L"\n";
        if (const double silent = SoundSilentMs(s, QpcNow()); silent > 1000.0)   // the driver stopped delivering sound
            t += Format(L"  NO SOUND PACKETS for %.0f s%s\n", silent / 1000.0, s.soundWarned ? L" (replug the device)" : L"");
        t += Format(L"  buffer avg %.1f ms   min %.1f ms (reserve %.1f%s)   drift %+.0f ppm   underruns %u   resyncs %u\n", s.audioSrc->FillMs(),
                    s.audioSrc->MinLeftMs(), s.audioSrc->TargetMs(),
                    s.audioSrc->LearnedReserveMs() >= 0.05 ? Format(L", %.1f learned", s.audioSrc->LearnedReserveMs()).c_str() : L"",
                    s.audioSrc->DriftPpm(), s.audioSrc->Underruns(), s.audioSrc->Overruns());
        // Measured clocks: how far each device runs from its stated rate (what the drift correction follows).
        auto ppm = [](double v) { return std::isnan(v) ? std::wstring(L"measuring") : Format(L"%+.0f ppm", v); };
        t += Format(L"  clocks: capture %s, output %s", ppm(s.audioSrc->InClockPpm()).c_str(), ppm(s.audioSrc->OutClockPpm()).c_str());
        if (m_audio.DryCount())
            t += Format(L"   output ran dry %u times (queue +%.1f ms)", m_audio.DryCount(), m_audio.LearnedQueueMs());
        t += L"\n";
        t += Format(L"  audio path ≈ %.0f ms (capture period + buffer + output queue)\n",
                    s.audioSrc->InputPeriodMs() + s.audioSrc->FillMs() + m_audio.QueuedMs());
        if (m_btOutput) t += L"  + the Bluetooth output's own delay on top (Windows doesn't report it; often 100 to 300 ms)\n";
    } else {
        t += L"  audio in  none\n";
    }
    return t;
}

void App::DrawStats()
{
    Ui& u = m_ui;
    // Rebuilt 4x per second: readable, and cheap (text layouts are cached while the text is unchanged).
    if (QpcElapsedMs(m_statsAt) > 250 || m_statsText.empty()) {
        m_statsAt = QpcNow();
        std::wstring t;
        if (m_multiview) {
            for (const auto& s : m_sessions) t += StatsText(*s);
        } else if (m_active < m_sessions.size()) {
            t += StatsText(*m_sessions[m_active]);
        }
        t += L"audio out " + m_audio.Description() + Format(L"   queued %.1f ms   callback jitter %.1f ms\n", m_audio.QueuedMs(), m_audio.WakeJitterMs());
        if (m_encoder) {
            const auto& c = m_encoder->Config();
            t += Format(L"encoder %s (%s)  %ux%u  %.1f fps  %.0f kbps  frame→packet %.2f ms  dropped %llu\n", m_encoder->Name().c_str(),
                        m_encoder->TargetName().c_str(), c.width, c.height, m_encoder->OutputFps(), m_encoder->BitrateKbps(), m_encoder->LatencyMs(),
                        m_encoder->Dropped());
        }
        if (m_shareEncoder) {
            t += Format(L"sharing %s  %.1f fps  target %.1f Mbps (%s)  sent %.1f Mbps  frame→packet %.2f ms\n", m_shareEncoder->Name().c_str(),
                        m_shareEncoder->OutputFps(), StreamKbps() / 1000.0, StreamQuality() == 0 ? L"smart" : L"fixed",
                        m_shareEncoder->BitrateKbps() / 1000.0, m_shareEncoder->LatencyMs());
        }
        if (m_server) t += Format(L"sharing  UDP %u  %d viewer(s)  %.1f Mbps sent", m_server->Port(), m_server->Viewers(), m_server->Kbps() / 1000.0);
        if (m_web) t += Format(L"   web :%u  %d viewer(s)", m_web->Port(), m_web->Viewers());
        if (m_server) t += L"\n";
        if (m_gpuProf.Valid())
            t += Format(L"gpu time %.2f ms   upload %.2f   program %.2f   draw %.2f   ui %.2f   present %.2f   encode %.2f\n", m_gpuProf.TotalMs(),
                        m_gpuProf.SectionMs(0), m_gpuProf.SectionMs(1), m_gpuProf.SectionMs(2), m_gpuProf.SectionMs(3), m_gpuProf.SectionMs(4),
                        m_gpuProf.SectionMs(5));
        t += Format(L"gpu %s%s   tearing %s   vsync %s   ui %.2f ms   present %.2f ms", m_renderer.AdapterName().c_str(),
                    m_renderer.Warp() ? L" (software)" : L"", m_renderer.Tearing() ? L"yes" : L"no", m_vsync ? L"on" : L"off", m_uiMs, m_presentMs);
        m_statsText = std::move(t);
    }
    const std::wstring& t = m_statsText;
    size_t lines = 1;
    for (wchar_t c : t)
        if (c == L'\n') ++lines;
    const float lh = u.S(16.5f);
    const float maxW = m_settingsOpen ? u.Width() - std::min(u.S(380), u.Width() - u.S(20)) - u.S(36) : u.Width() - u.S(20);
    RectF r{u.S(10), BarHeight() + u.S(10), std::min(maxW, u.S(1100)), lh * float(lines) + u.S(16)};
    u.Fill(r, theme::Scrim, u.S(4));
    u.TextBlock(t, r.Inset(u.S(10), u.S(8)), u.S(12), theme::Text, true);
}

void App::DrawToast()
{
    std::vector<ToastItem> toasts;
    const int64_t now = QpcNow();
    {
        std::lock_guard lock(m_toastMutex);
        std::erase_if(m_toasts, [&](const ToastItem& t) { return t.until <= now; });
        toasts.assign(m_toasts.begin(), m_toasts.end());
    }
    if (toasts.empty()) return;
    Ui& u = m_ui;
    // Newest at the bottom; long text wraps; the mouse over a toast keeps it.
    float y = u.Height() - u.S(36);
    const float maxW = std::min(u.Width() - u.S(40), u.S(720));
    for (size_t i = toasts.size(); i-- > 0;) {
        const ToastItem& t = toasts[i];
        const bool hasFile = !t.file.empty();
        const float textW = std::min(u.Measure(t.text, u.S(13)), maxW - u.S(32));
        const float textH = std::max(u.S(18), u.MeasureBlock(t.text, textW + u.S(2), u.S(13)));
        const bool video = hasFile && t.file.size() > 4 && _wcsicmp(t.file.c_str() + t.file.size() - 4, L".mp4") == 0;
        const float w = std::max(textW + u.S(32), hasFile ? u.S(video ? 400.0f : 300.0f) : 0.0f);
        const float h = textH + u.S(18) + (hasFile ? u.S(38) : 0.0f);
        const RectF r{(u.Width() - w) * 0.5f, y - h, w, h};
        y -= h + u.S(8);
        const bool hot = u.Hot(r);
        if (hot) {   // still being read: stays a moment longer
            std::lock_guard lock(m_toastMutex);
            for (auto& q : m_toasts)
                if (q.key == t.key && q.text == t.text) q.until = std::max(q.until, now + QpcFrequency() * 2);
            m_toastUntil = std::max(m_toastUntil.load(), now + QpcFrequency() * 2);
        }
        u.SetAlpha(float(std::min(1.0, QpcToMs(t.until - now) / 400.0)));
        u.Fill(r, theme::Popup, u.S(14));
        u.TextBlock(t.text, {r.x + u.S(16), r.y + u.S(9), r.w - u.S(32), textH + u.S(2)}, u.S(13), theme::Text);
        if (hasFile) {
            const int buttons = video ? 3 : 2;
            const float bw = (r.w - u.S(32) - u.S(8) * float(buttons - 1)) / float(buttons), by = r.Bottom() - u.S(40);
            const std::string ids = "toastdir" + std::to_string(i), ido = "toastopen" + std::to_string(i);
            const std::wstring file = t.file;
            if (u.Button(ids.c_str(), {r.x + u.S(16), by, bw, u.S(30)}, L"Show in folder", glyph::Folder))
                m_graveyard.push_back(std::async(std::launch::async, [file] {
                    ComInit com(COINIT_APARTMENTTHREADED);
                    const std::wstring args = L"/select,\"" + file + L"\"";
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                }));
            if (u.Button(ido.c_str(), {r.x + u.S(24) + bw, by, bw, u.S(30)}, L"Open"))
                m_graveyard.push_back(std::async(std::launch::async, [file] {
                    ComInit com(COINIT_APARTMENTTHREADED);
                    ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }));
            const std::string idt = "toasttrim" + std::to_string(i);
            if (video && u.Button(idt.c_str(), {r.x + u.S(32) + 2 * bw, by, bw, u.S(30)}, L"Trim", glyph::Cut)) {
                std::lock_guard lock(m_pickMutex);
                m_trimPicked = file;
            }
        }
    }
    u.SetAlpha(1);
}

// ---------------------------------------------------------------------------------------------

void App::SaveScreenshot(const std::wstring& pathIn, bool wait)
{
    Session* s = Active();
    std::vector<uint8_t> pixels;
    if (!s || !m_renderer.Readback(s->surface, pixels, s->standby)) {
        Toast(L"Nothing to capture yet");
        return;
    }
    const UINT w = s->standby ? s->surface.lastGoodW : s->surface.width, h = s->standby ? s->surface.lastGoodH : s->surface.height;
    std::wstring path = pathIn;
    if (path.empty()) {
        std::wstring dir = ScreenshotFolder();
        if (!EnsureFolder(dir, L"the screenshot")) return;
        path = NewFilePath(dir, L"ZeroDisplay", m_shotFormat == 1 ? L".jpg" : L".png");
    }
    if (m_shotFlash && pathIn.empty()) {
        m_shotFlashQpc = QpcNow();   // a short white flash says it was taken
        InvalidateUi();
    }
    const bool jpeg = m_shotFormat == 1 && pathIn.empty(), clipboard = m_shotClipboard && pathIn.empty();
    auto job = [this, path, w, h, px = std::move(pixels), wait, jpeg, clipboard] {
        std::optional<EfficiencyScope> eco;
        if (!wait) eco.emplace();
        ComInit com;
        HRESULT hr = jpeg ? SaveJpeg(path, w, h, px) : SavePng(path, w, h, px);
        if (clipboard) CopyImageToClipboard(w, h, px);
        if (SUCCEEDED(hr)) {
            LOGI(L"Screenshot saved: %s", path.c_str());
            ToastFile((clipboard ? L"Screenshot saved and copied: " : L"Screenshot saved: ") + path, path);
        } else {
            LOGE(L"Screenshot failed: %s", HrToString(hr).c_str());
            Toast(L"Screenshot failed");
        }
    };
    if (wait) job();
    else m_graveyard.push_back(std::async(std::launch::async, std::move(job)));
}

void App::SelftestTick()
{
    if (m_cmd.selftestSec <= 0 || m_selftestDone) return;
    const double t = QpcToMs(QpcNow() - m_startQpc) / 1000.0;
    // --all-formats: each format of the active device in turn; it must deliver its size at (nearly) its rate.
    if (m_cmd.allFormats) {
        static std::vector<VideoFormat> fmts;
        static size_t idx = 0;
        static double since = -1;
        static int passed = 0, failed = 0;
        Session* s = Active();
        if (s && fmts.empty() && t > 2.0) {
            fmts = s->video->Formats();
            idx = std::min<size_t>(size_t(std::max(0, m_cmd.allFormatsFrom)), fmts.size());
        }
        if (s && idx < fmts.size()) {
            const VideoFormat& want = fmts[idx];
            if (since < 0) {
                s->video->SetFormat(want);
                since = t;
            } else if (t - since >= m_cmd.allFormatsDwell) {
                const VideoFormat cur = s->video->Current();
                const CaptureStats cs = s->video->Stats();
                const bool sizeOk = s->surface.width == want.width && s->surface.height == want.height;
                const bool ok = cur.Matches(want) && sizeOk && cs.fps >= want.Fps() * 0.85 && s->surface.hasImage;
                ++(ok ? passed : failed);
                LOGI(L"[formats] %s %s: capture %.1f fps, picture %ux%u", ok ? L"PASS" : L"FAIL", want.Label().c_str(), cs.fps, s->surface.width, s->surface.height);
                ++idx;
                since = -1;
                if (idx == fmts.size()) LOGI(L"[formats] done: %d passed, %d failed of %zu", passed, failed, fmts.size());
            }
        }
    }
    auto logAll = [&](const wchar_t* tag) {
        for (const auto& s : m_sessions) {
            std::wstring st = StatsText(*s);
            LOGI(L"[selftest %s t=%.1fs] state=%d status='%s' uploadFailures=%llu\n%s", tag, t, int(s->video->State()),
                 s->video->StatusText().c_str(), s->uploadFailures, st.c_str());
        }
        LOGI(L"[selftest %s] audio out: %s queued %.1f ms", tag, m_audio.Description().c_str(), m_audio.QueuedMs());
        if (m_gpuProf.Valid())
            LOGI(L"[selftest %s] gpu time %.2f ms: upload %.2f, program %.2f, draw %.2f, ui %.2f, present %.2f, encode %.2f", tag, m_gpuProf.TotalMs(),
                 m_gpuProf.SectionMs(0), m_gpuProf.SectionMs(1), m_gpuProf.SectionMs(2), m_gpuProf.SectionMs(3), m_gpuProf.SectionMs(4),
                 m_gpuProf.SectionMs(5));
        if (m_encoder)
            LOGI(L"[selftest %s] encoder %s (%s): %.1f fps, %.0f kbps, frame->packet %.2f ms, dropped %llu", tag, m_encoder->Name().c_str(),
                 m_encoder->TargetName().c_str(), m_encoder->OutputFps(), m_encoder->BitrateKbps(), m_encoder->LatencyMs(), m_encoder->Dropped());
        if (m_shareEncoder)
            LOGI(L"[selftest %s] sharing encoder %s: %.1f fps, target %.1f Mbps, actual %.1f Mbps, dropped %llu", tag, m_shareEncoder->Name().c_str(),
                 m_shareEncoder->OutputFps(), StreamKbps() / 1000.0, m_shareEncoder->BitrateKbps() / 1000.0, m_shareEncoder->Dropped());
        if (m_recorder)
            LOGI(L"[selftest %s] recorder: %s, %.1f s, %.2f MB %s", tag, m_recorder->Writing() ? L"writing" : L"waiting", m_recorder->Seconds(),
                 m_recorder->Bytes() / 1048576.0, m_recorder->Error().c_str());
        if (m_server) LOGI(L"[selftest %s] sharing: %d viewer(s), %.0f kbps", tag, m_server->Viewers(), m_server->Kbps());
        LOGI(L"[selftest %s] %s", tag, m_probe.Report(m_renderer.Device()).c_str());
        LOGI(L"[selftest %s] UI drawn in %llu of %llu frames, %.2f ms per drawing (max %.1f); %llu frames not drawn (window hidden or 30 fps limit), %llu (nothing new)", tag,
             (unsigned long long)m_uiDraws, (unsigned long long)m_frames, m_uiDraws ? m_uiDrawMsTotal / double(m_uiDraws) : 0.0, m_uiDrawMsMax,
             (unsigned long long)m_hiddenFrames, (unsigned long long)m_unchangedFrames);
        LOGI(L"[selftest %s] cpu per frame (ms): input + upload %.2f, program %.2f, draw %.2f, ui %.2f, present %.2f, encode %.2f", tag,
             m_gpuProf.CpuSectionMs(0), m_gpuProf.CpuSectionMs(1), m_gpuProf.CpuSectionMs(2), m_gpuProf.CpuSectionMs(3), m_gpuProf.CpuSectionMs(4),
             m_gpuProf.CpuSectionMs(5));
    };
    static bool probeMarked = false;   // measure the second half of the run (startup excluded)
    if (!probeMarked && t >= m_cmd.selftestSec * 0.4) {
        probeMarked = true;
        m_probe.Mark();
    }

    static bool replayResized = false;
    if (m_cmd.replayResizeTest && m_replay && !replayResized && t >= m_cmd.selftestSec * 0.6) {
        replayResized = true;
        const double before = m_replay->BufferedSeconds();
        const double to = m_replaySeconds > 15.0f ? 10.0 : 30.0;   // not saved: the setting keeps its value
        m_replay->SetSeconds(to);                                  // what the Settings slider does
        LOGI(L"[selftest] replay length %.0f -> %.0f s: buffer %.1f s -> %.1f s (kept)", double(m_replaySeconds), to, before, m_replay->BufferedSeconds());
    }
    static bool replaySaved = false;
    if (m_cmd.replay && !replaySaved && t >= m_cmd.selftestSec - 0.5) {
        replaySaved = true;
        if (m_replay) LOGI(L"[selftest] replay buffer holds %.1f s", m_replay->BufferedSeconds());
        SaveReplay();
    }

    static bool transitionStarted = false;
    if (m_cmd.studio && !m_cmd.studioTest && !transitionStarted && t >= m_cmd.selftestSec - 1.0) {
        transitionStarted = true;
        m_transitionMode = 1;
        m_transitionMs = 2000.0f;
        LOGI(L"[selftest] transition %d -> %d", m_scenes.program, m_scenes.preview);
        StartTransition(m_scenes.preview);
    }

    static bool flashOpened = false, flashStepped = false;
    if (m_cmd.flashbackTest && !flashOpened && t >= m_cmd.selftestSec * 0.6) {
        flashOpened = true;
        StartFlashback(4.0);
        if (m_flash) LOGI(L"[selftest] flashback open: %.1f s buffered, at %.2f", m_flash->Duration(), m_flash->Position());
    }
    if (m_cmd.flashbackTest && flashOpened && !flashStepped && t >= m_cmd.selftestSec * 0.6 + 1.5 && m_flash) {
        flashStepped = true;
        LOGI(L"[selftest] flashback after 1.5 s of playback: at %.2f (playing %d)", m_flash->Position(), int(m_flash->Playing()));
        FlashbackJump(-3.0);
        m_flash->SetPlaying(false);
        LOGI(L"[selftest] flashback paused after -3 s: at %.2f", m_flash->Position());
        m_clipIn = 0.5;
        m_clipOut = 2.5;
        SaveFlashbackClip();
    }

    static int modeSwitches = 0;
    if (m_cmd.modeSwitch && modeSwitches < 2 && t >= m_cmd.selftestSec * (modeSwitches == 0 ? 0.35 : 0.65)) {
        ++modeSwitches;
        LOGI(L"[selftest] switching to %s (replay %s, recording %s)", m_advanced ? L"Simple" : L"Advanced", m_replay ? L"on" : L"off",
             m_recorder ? L"on" : L"off");
        SetAdvanced(!m_advanced);
    }

    static bool lagStarted = false;
    if (m_cmd.lagSim >= 0 && !lagStarted && t >= 1.5) {
        lagStarted = true;
        StartLagCheck();
    }

    static bool trimOpened = false, trimSaved = false;
    if (!m_cmd.trim.empty() && !trimOpened && t >= 1.0) {
        trimOpened = true;
        OpenTrim(m_cmd.trim);
        if (m_trim) {
            m_trim->start = float(m_trim->duration * 0.3);
            m_trim->end = float(m_trim->duration * 0.7);
            m_trim->wantPreview = m_trim->start;
        }
    }
    if (m_trim && !trimSaved && t >= m_cmd.selftestSec - 1.5) {
        trimSaved = true;
        std::wstring out = m_cmd.trim.substr(0, m_cmd.trim.find_last_of(L'.')) + L"_selftest_trim.mp4";
        std::wstring err;
        double actual = 0;
        const bool ok = TrimFile(m_cmd.trim, out, m_trim->start, m_trim->end, err, &actual);
        LOGI(L"[selftest] trim %.2f..%.2f -> %s: %s (starts at %.2f)", m_trim->start, m_trim->end, out.c_str(), ok ? L"ok" : err.c_str(), actual);
    }

    static bool unplugged = false;
    if (m_cmd.standbyTest && !unplugged && t >= m_cmd.selftestSec * 0.5) {
        unplugged = true;
        if (Session* s = Active()) {
            LOGI(L"[selftest] simulating unplug of %s (last good picture %s)", s->info.name.c_str(), s->surface.lastGood ? L"available" : L"missing");
            s->testUnplugged = true;
            s->present = false;
            s->video->Close();
            s->surface.hasImage = false;
        }
    }

    if (m_cmd.undoTest && m_advanced) UndoSelftest(t);
    if (m_cmd.studioTest && m_advanced) StudioSelftest(t);
    if (m_cmd.crashTest && t >= m_cmd.selftestSec * 0.5) {
        LOGI(L"[selftest] crash test: crashing now");
        SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);   // no Windows "stopped working" box for the test
        RaiseException(0xE0C0FFEE, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    }
    if (m_cmd.portableToggleTest && !m_cmd.profileDir.empty()) {
        static int step = 0;
        if (step == 0 && t >= 2.0) {
            step = 1;
            LOGI(L"[selftest] portable on: %s", SetPortableMode(true) ? L"ok" : L"FAILED");
        } else if (step == 1 && t >= 4.0) {
            step = 2;
            LOGI(L"[selftest] portable off: %s", SetPortableMode(false) ? L"ok" : L"FAILED");
        }
    }
    if (m_cmd.flipLog && !m_lag)
        if (Session* a = Active()) {
            // The source may fill only part of the frame (a portrait phone): threshold halfway between the
            // darkest and brightest levels seen.
            float level = 0;
            int64_t fq = 0;
            while (m_renderer.LagResult(a->surface, level, fq)) {
                m_flipLo = std::min(m_flipLo, level);
                m_flipHi = std::max(m_flipHi, level);
                if (m_flipHi - m_flipLo < 0.05f) continue;
                const bool bright = level > (m_flipLo + m_flipHi) * 0.5f;
                if (bright != m_flipBright) {
                    m_flipBright = bright;
                    LOGI(L"[flip] %s at %.3f", bright ? L"bright" : L"dark", QpcToMs(fq));
                }
            }
        }
    // --ui-steps: clicks, moves and keys fed straight into the input state (posted mouse messages lose their position
    // to the real cursor when the window captures the mouse). A click is held for one tick, then released.
    static std::vector<std::wstring> uiSteps;
    static size_t uiStep = 0;
    static bool uiRelease = false, uiParsed = false;
    if (!uiParsed) {
        uiParsed = true;
        for (size_t p = 0; p < m_cmd.uiSteps.size();) {
            size_t e = m_cmd.uiSteps.find(L';', p);
            if (e == std::wstring::npos) e = m_cmd.uiSteps.size();
            if (e > p) uiSteps.push_back(m_cmd.uiSteps.substr(p, e - p));
            p = e + 1;
        }
    }
    if (uiRelease) {
        std::lock_guard lock(m_inputMutex);
        m_input.down = false;
        m_input.released = true;
        uiRelease = false;
        SetEvent(m_wake);
    } else if (uiStep < uiSteps.size()) {
        double at = 0;
        wchar_t kind[16] = {};
        int x = 0, y = 0;
        if (swscanf_s(uiSteps[uiStep].c_str(), L"%lf:%15[a-z]:%d,%d", &at, kind, unsigned(std::size(kind)), &x, &y) >= 3 && t >= at) {
            const std::wstring k = kind;
            std::lock_guard lock(m_inputMutex);
            if (k == L"key") {
                m_input.keys.push_back(UINT(x));
            } else if (k == L"char") {   // typed text, one character (code)
                m_input.chars += wchar_t(x);
            } else if (k == L"wheel") {   // x = notches (negative: down)
                m_input.wheel += float(x);
            } else if (k == L"shot") {   // x = number: <--ui-shot without .png>_N.png
                std::wstring base = m_cmd.uiShot;
                if (base.size() > 4) base.resize(base.size() - 4);
                m_stepShot = base + Format(L"_%02d.png", x);
                m_uiDirty = true;
            } else {
                m_input.mx = float(x);
                m_input.my = float(y);
                m_input.mouseIn = m_input.moved = true;
                if (k == L"click") {
                    m_input.down = m_input.pressed = true;
                    uiRelease = true;
                } else if (k == L"down") {   // press and hold (drags: "down", "move"s, "up")
                    m_input.down = m_input.pressed = true;
                } else if (k == L"up") {
                    m_input.down = false;
                    m_input.released = true;
                }
            }
            LOGI(L"[selftest] ui step %s", uiSteps[uiStep].c_str());
            ++uiStep;
            SetEvent(m_wake);
        }
    }
    static int wirelessToggles = 0;   // --wireless-toggle-test: master switch off at 35 %, on again at 65 %
    if (m_cmd.wirelessToggleTest && wirelessToggles < 2 && t >= m_cmd.selftestSec * (wirelessToggles == 0 ? 0.35 : 0.65)) {
        ++wirelessToggles;
        LOGI(L"[selftest] wireless features %s", wirelessToggles == 2 ? L"on" : L"off");
        SetWireless(wirelessToggles == 2);
    }
    if (m_cmd.profileTest) ProfileSelftest(t);
    static bool hdrSampled = false;   // --hdr-output-test: sample the grey steps (and one point of the top bar) a second before the end
    if (m_cmd.hdrOutputTest && t >= m_cmd.selftestSec - 1.5) m_showStats = false;   // nothing over the measured picture
    if (m_cmd.hdrOutputTest && !hdrSampled && t >= m_cmd.selftestSec - 1.0) {
        hdrSampled = true;
        const RectF pic = Renderer::FitRect(1920, 1080, {0, 0, float(m_renderer.Width()), float(m_renderer.Height())}, ScaleMode::Fit);
        std::vector<POINT> pts;
        for (int i = 0; i < 11; ++i) pts.push_back({LONG(pic.x + (i + 0.5f) * pic.w / 11), LONG(pic.y + pic.h * 0.2f)});
        m_renderer.RequestHdrSamples(pts);
    }
    if (m_cmd.twinTest) TwinSelftest(t);
    static int stallStep = 0;   // --sound-stop-test
    if (m_cmd.soundStopTest && stallStep < 2 && t >= m_cmd.selftestSec * (stallStep == 0 ? 0.3 : 0.75)) {
        AudioSource::s_testStall = stallStep++ == 0;
        LOGI(L"[selftest] device sound %s", AudioSource::s_testStall.load() ? L"stops arriving" : L"arrives again");
    }
    static int hotspotToggles = 0;   // --hotspot-toggle-test: network off at 35 %, on again at 65 %
    if (m_cmd.hotspotToggleTest && hotspotToggles < 2 && t >= m_cmd.selftestSec * (hotspotToggles == 0 ? 0.35 : 0.65)) {
        ++hotspotToggles;
        m_hotspotOn = hotspotToggles == 2;
        LOGI(L"[selftest] own Wi-Fi network %s", m_hotspotOn ? L"on" : L"off");
    }
    static double lastWirelessLog = 0;
    if (m_miracast && t - lastWirelessLog >= 5.0) {
        lastWirelessLog = t;
        LOGI(L"[selftest] Miracast: %s", m_miracast->StatusText().c_str());
    }
    static bool castRecordStarted = false;   // --record-cast: record once a cast shows a picture
    if (m_cmd.recordCast && !castRecordStarted && !m_recorder)
        if (Session* c = ActiveCast(); c && c->video->LastFrameQpc() != 0) {
            castRecordStarted = true;
            LOGI(L"[selftest] recording the cast");
            StartRecording();
        }
    static bool recordStarted = false;
    if (m_cmd.record && !recordStarted && t >= 2.0) {
        recordStarted = true;
        StartRecording();
    }

    static double lastAudioLog = 0;
    if (t - lastAudioLog >= 1.0) {
        lastAudioLog = t;
        for (const auto& s : m_sessions)
            LOGI(L"[selftest audio t=%.1f] avg %.1f ms min %.1f ms reserve %.1f ms drift %+.0f ppm underruns %u resyncs %u dropped %u | max gap %.1f ms max packet %u"
                 L" | clocks in %+.0f out %+.0f ppm",
                 t, s->audioSrc->FillMs(), s->audioSrc->MinLeftMs(), s->audioSrc->TargetMs(), s->audioSrc->DriftPpm(), s->audioSrc->Underruns(), s->audioSrc->Overruns(),
                 s->audioSrc->Dropped(), s->audioSrc->TakeMaxGapMs(), s->audioSrc->TakeMaxPacket(), s->audioSrc->InClockPpm(), s->audioSrc->OutClockPpm());
        LOGI(L"[selftest audio t=%.1f] output wakeups %u max gap %.1f ms ran dry %u", t, m_audio.TakeWakeups(), m_audio.TakeMaxWakeGapMs(), m_audio.DryCount());
    }

    if (m_cmd.cycle) {
        struct Want { UINT w, h; double fps; GUID sub; };
        static const Want wants[] = {
            {1920, 1080, 60, MFVideoFormat_MJPG}, {1280, 720, 60, MFVideoFormat_YUY2}, {1600, 1200, 60, MFVideoFormat_YUY2},
            {1920, 1080, 30, MFVideoFormat_YUY2}, {640, 480, 60, MFVideoFormat_MJPG},  {1920, 1080, 60, MFVideoFormat_YUY2},
        };
        const double stepSec = 4.0;
        if (m_cycleStep < std::size(wants) && t >= stepSec * double(m_cycleStep + 1)) {
            logAll(L"step");
            if (Session* s = Active()) {
                const Want& w = wants[m_cycleStep];
                for (const auto& f : s->video->Formats()) {
                    if (f.width == w.w && f.height == w.h && f.subtype == w.sub && std::fabs(f.Fps() - w.fps) < 1.0) {
                        LOGI(L"[selftest] -> %s", f.Label().c_str());
                        s->video->SetFormat(f);
                        break;
                    }
                }
            }
            ++m_cycleStep;
        }
    }
    if (!m_cmd.uiShot.empty() && t >= m_cmd.selftestSec - 1.0 && !m_settingsOpen && !m_uiShotTaken) {
        // Show every panel for the UI screenshot (restored before exit so settings aren't changed).
        m_statsBeforeShot = m_showStats;
        m_settingsOpen = !m_advanced && !m_wirelessView;   // Advanced / Wireless show their own panel instead
        m_showStats = !m_advanced && !m_wirelessView && m_cmd.settingsScroll <= 0;
        m_settingsScroll = m_cmd.settingsScroll;
        if (m_advanced && m_selectedItem == 0)
            if (Scene* sc = ProgramScene())
                for (const auto& it : sc->items)
                    if (int(it.kind) == (m_cmd.uiSelect >= 0 ? m_cmd.uiSelect : int(SourceKind::Device))) m_selectedItem = it.id;
        if (m_advanced) m_panelScroll = m_cmd.settingsScroll;
        if (m_wirelessView) m_wirelessScroll = m_cmd.settingsScroll;
    }
    if (t >= m_cmd.selftestSec) {
        if (!m_cmd.uiShot.empty() && !m_uiShotTaken) {
            m_uiShotNow = true;   // captured during the next frame
            return;
        }
        if (!m_cmd.uiShot.empty()) m_showStats = m_statsBeforeShot;
        m_selftestDone = true;
        logAll(L"final");
        if (m_cmd.hdrOutputTest) {   // the HDR output's values over the test pattern's grey steps (scRGB: nits / 80)
            std::vector<float> v;
            std::wstring line = !m_renderer.HdrOutput() ? L"[selftest] HDR output is OFF" : L"[selftest] HDR output scRGB:";
            if (m_renderer.HdrOutput() && m_renderer.HdrSamples(v))
                for (size_t i = 0; i * 4 < v.size(); ++i) line += Format(L" %g nits=%.3f", i < 11 ? TestPattern::kHdrSteps[i] : -1.0f, v[i * 4]);
            LOGI(L"%s", line.c_str());
            if (Session* hs = Active(); hs && hs->surface.hdrLin) {   // the picture's linear copy (1.0 = 203 nits)
                std::vector<POINT> pts;
                for (int i = 0; i < 11; ++i) pts.push_back({LONG((i + 0.5f) * hs->surface.width / 11), LONG(hs->surface.height / 5)});
                std::vector<float> lin;
                std::wstring l2 = L"[selftest] HDR linear copy (x white):";
                if (m_renderer.ReadPixelsFp16(hs->surface.hdrLin.Get(), pts, lin))
                    for (size_t i = 0; i < 11; ++i) l2 += Format(L" %g=%.3f", TestPattern::kHdrSteps[i], lin[i * 4]);
                LOGI(L"%s", l2.c_str());
            }
        }
        StopRecording();
        if (!m_cmd.shot.empty()) SaveScreenshot(m_cmd.shot, true);
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    }
}

} // namespace cc
