// A capture device in its own window: a normal, resizable window per device (title = device name), drawn after
// the main window's present like the mini player. The device stays open while its window is; double-click or F11
// toggles full screen on the window's monitor, Esc leaves it. Position and "was open" are remembered per device.
#include "App.h"
#include "core/Log.h"

#include <dwmapi.h>

namespace cc {

bool App::HasDeviceWindow(const std::wstring& symlink) const
{
    for (const auto& w : m_deviceWindows)
        if (w->symlink == symlink) return true;
    return false;
}

void App::OpenDeviceWindow(const VideoDeviceInfo& d)
{
    if (HasDeviceWindow(d.symlink)) return;
    if (SessionIndex(d) < 0) OpenSession(d);
    auto w = std::make_unique<DeviceWindow>();
    w->app = this;
    w->symlink = d.symlink;
    w->name = d.name;
    w->key = d.Key();
    w->placement = m_settings.GetString(Settings::DeviceSection(d.Key()), L"Window", L"");
    m_settings.SetBool(Settings::DeviceSection(d.Key()), L"WindowOpen", true);
    PostMessageW(m_hwnd, WM_APP_DEVWIN_CREATE, 0, LPARAM(w.get()));
    LOGI(L"Device window: %s", d.name.c_str());
    m_deviceWindows.push_back(std::move(w));
}

void App::CloseDeviceWindow(size_t i, bool remember)
{
    if (i >= m_deviceWindows.size()) return;
    std::unique_ptr<DeviceWindow> w = std::move(m_deviceWindows[i]);
    m_deviceWindows.erase(m_deviceWindows.begin() + ptrdiff_t(i));
    w->out = OutputWindow{};   // release the swap chain before its window goes away
    if (HWND h = w->hwnd.exchange(nullptr)) {
        WINDOWPLACEMENT wp{sizeof(wp)};
        if (!w->fullscreen && GetWindowPlacement(h, &wp)) {
            const RECT& r = wp.rcNormalPosition;
            m_settings.SetString(Settings::DeviceSection(w->key), L"Window",
                                 Format(L"%ld,%ld,%ld,%ld,%d", r.left, r.top, r.right, r.bottom, wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0));
        }
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);   // the window procedure stops using this object
        PostMessageW(m_hwnd, WM_APP_DEVWIN_DESTROY, 0, LPARAM(h));
    }
    if (!remember) m_settings.SetBool(Settings::DeviceSection(w->key), L"WindowOpen", false);
    LOGI(L"Device window closed: %s", w->name.c_str());
    m_closedWindows.push_back(std::move(w));   // kept until exit: a message may still be on its way to it
    // Nothing else shows it: close the device (Simple keeps only what it shows).
    if (!m_advanced && !m_multiview)
        for (size_t s = 0; s < m_sessions.size(); ++s)
            if (m_sessions[s]->info.symlink == m_closedWindows.back()->symlink && s != m_active && !KeepOpen(*m_sessions[s])) {
                CloseSession(s);
                break;
            }
}

// Window thread.
void App::CreateDeviceWindow(DeviceWindow* w)
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    const int dpi = int(GetDpiForWindow(m_hwnd));
    const int cw = MulDiv(960, dpi, 96), ch = MulDiv(540, dpi, 96), step = MulDiv(32, dpi, 96) * int(m_deviceWindows.size() % 6);
    RECT r{mi.rcWork.left + MulDiv(80, dpi, 96) + step, mi.rcWork.top + MulDiv(80, dpi, 96) + step, 0, 0};
    r.right = r.left + cw;
    r.bottom = r.top + ch;
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, UINT(dpi));
    bool maximized = false;
    RECT saved{};
    int max = 0;
    if (swscanf_s(w->placement.c_str(), L"%ld,%ld,%ld,%ld,%d", &saved.left, &saved.top, &saved.right, &saved.bottom, &max) == 5 &&
        saved.right - saved.left >= 200 && saved.bottom - saved.top >= 120 && MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) {
        r = saved;   // where it was last time (only if that place is still on a screen)
        maximized = max != 0;
    }
    HWND hwnd = CreateWindowExW(0, L"CaptureCardDevice", w->name.c_str(), WS_OVERLAPPEDWINDOW, r.left, r.top, r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, GetModuleHandleW(nullptr), w);
    if (!hwnd) {
        w->closeRequest = true;
        SetEvent(m_wake);
        return;
    }
    StyleFrame(hwnd);
    ShowWindow(hwnd, maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    w->hwnd = hwnd;
    w->resize = true;
    SetEvent(m_wake);
}

LRESULT CALLBACK App::DeviceWindowProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto w = reinterpret_cast<DeviceWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (w && w->app) return w->app->DeviceWindowProc(*w, hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Window thread.
LRESULT App::DeviceWindowProc(DeviceWindow& w, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto fullscreen = [&](bool on) {
        if (on == w.fullscreen) return;
        w.fullscreen = on;
        if (on) {
            w.windowed.length = sizeof(w.windowed);
            GetWindowPlacement(hwnd, &w.windowed);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        } else {
            SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowPlacement(hwnd, &w.windowed);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        }
    };
    switch (msg) {
    case WM_SIZE:
        w.resize = true;
        SetEvent(m_wake);
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {320, 200};
        return 0;
    case WM_LBUTTONDBLCLK:
        fullscreen(!w.fullscreen);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_F11) fullscreen(!w.fullscreen);
        else if (wp == VK_ESCAPE) fullscreen(false);
        return 0;
    case WM_CLOSE:
        w.closeRequest = true;   // the app thread saves the place, releases the swap chain, then destroys the window
        SetEvent(m_wake);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// App thread, right after the main window's present (never delays it).
void App::RenderDeviceWindows()
{
    for (size_t i = m_deviceWindows.size(); i-- > 0;) {
        DeviceWindow& w = *m_deviceWindows[i];
        if (w.closeRequest.exchange(false)) {
            CloseDeviceWindow(i, false);
            continue;
        }
        HWND hwnd = w.hwnd.load();
        if (!hwnd || IsIconic(hwnd)) continue;
        Session* s = nullptr;
        for (auto& sp : m_sessions)
            if (sp->info.symlink == w.symlink) s = sp.get();
        if (!s) {   // closed meanwhile (mode change): open it again while its window is up
            const int di = DeviceIndex({w.name, w.symlink});
            if (di >= 0) s = OpenSession(m_devices.video[size_t(di)]);
            if (!s) continue;
        }
        RECT rc;
        GetClientRect(hwnd, &rc);
        const UINT cw = UINT(std::max<LONG>(1, rc.right - rc.left)), ch = UINT(std::max<LONG>(1, rc.bottom - rc.top));
        const bool resized = w.resize.exchange(false);
        if (!w.out.swap) {
            if (!m_renderer.CreateOutput(w.out, hwnd, cw, ch)) {
                Toast(L"Couldn't open a window for " + w.name);
                CloseDeviceWindow(i, false);
                continue;
            }
        } else if (resized) {
            m_renderer.ResizeOutput(w.out, cw, ch);
        }
        // Draw when the device has a new picture, after a resize, and a few times a second (standby, no signal).
        const bool fresh = s->fpsCount != w.lastFrames || resized || QpcElapsedMs(w.lastDrawn) > 250;
        if (!w.out.rtv || !fresh) continue;
        w.lastFrames = s->fpsCount;
        w.lastDrawn = QpcNow();
        const RectF area{0, 0, float(w.out.width), float(w.out.height)};
        m_renderer.BeginOutput(w.out);
        if (s->surface.hasImage) {
            DrawSession(*s, area, m_smooth, int(ScaleMode::Fit));
        }
        m_renderer.EndOutput();
        m_renderer.PresentOutput(w.out);
        if (!w.shown && s->surface.hasImage) {
            w.shown = true;
            LOGI(L"Device window %s: showing %ux%u in %ux%u", w.name.c_str(), s->surface.width, s->surface.height, w.out.width, w.out.height);
        }
    }
}

} // namespace cc
