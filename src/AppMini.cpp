// Pop-out mini player: a small always-on-top borderless window showing the picture (or the program in
// Advanced mode) while you use other apps. Drag it anywhere, resize from the edges (keeps 16:9),
// double-click or right-click closes it. F6 or the top-bar button toggles it.
#include "App.h"
#include "core/Log.h"

#include <dwmapi.h>
#include <windowsx.h>

namespace cc {

void App::SetMini(bool on)
{
    if (on == m_miniOn) return;
    m_miniOn = on;
    if (!on) {
        if (HWND mini = m_miniHwnd.load()) {   // where it was: it opens there next time
            RECT r;
            if (GetWindowRect(mini, &r)) {
                m_miniPlacement = Format(L"%ld,%ld,%ld,%ld", r.left, r.top, r.right, r.bottom);
                m_settingsDirty = true;
            }
        }
        m_miniOut = OutputWindow{};   // release the swap chain before its window goes away
        m_renderer.EndOutput();
    }
    PostMessageW(m_hwnd, WM_APP + 4, on ? 1 : 0, 0);
    LOGI(L"Mini player %s", on ? L"on" : L"off");
}

void App::CreateMiniWindow()
{
    if (m_miniHwnd.load()) return;
    // Bottom-right of the work area of the main window's monitor, 1/4 of its width.
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT wa = mi.rcWork;
    const int w = std::max(320, int(wa.right - wa.left) / 4), h = int(w / m_miniAspect.load());
    const int margin = MulDiv(16, int(GetDpiForWindow(m_hwnd)), 96);
    RECT r{wa.right - w - margin, wa.bottom - h - margin, wa.right - margin, wa.bottom - margin};
    RECT saved{};
    if (swscanf_s(m_miniPlacement.c_str(), L"%ld,%ld,%ld,%ld", &saved.left, &saved.top, &saved.right, &saved.bottom) == 4 &&
        saved.right - saved.left >= 160 && MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) {
        r = saved;   // where it was last time, at the picture's shape
        r.bottom = r.top + LONG((r.right - r.left) / m_miniAspect.load());
    }
    // No redirection bitmap: the picture comes through DirectComposition, so the window can be see-through.
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, L"CaptureCardMini", L"ZeroDisplay mini player",
                                WS_POPUP | WS_THICKFRAME, r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                                GetModuleHandleW(nullptr), this);
    if (!hwnd) return;
    StyleFrame(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    m_miniHwnd = hwnd;
    m_miniResize = true;
    SetEvent(m_wake);
}

LRESULT CALLBACK App::MiniProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(cs->lpCreateParams));
    }
    auto self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->MiniProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::MiniProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) return 0;   // no visible frame; WS_THICKFRAME only for resizing
        break;
    case WM_NCHITTEST: {
        // Edges resize, everything else drags.
        RECT r;
        GetWindowRect(hwnd, &r);
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), b = MulDiv(8, int(GetDpiForWindow(hwnd)), 96);
        const bool l = x < r.left + b, rr = x >= r.right - b, t = y < r.top + b, bo = y >= r.bottom - b;
        if (t && l) return HTTOPLEFT;
        if (t && rr) return HTTOPRIGHT;
        if (bo && l) return HTBOTTOMLEFT;
        if (bo && rr) return HTBOTTOMRIGHT;
        if (l) return HTLEFT;
        if (rr) return HTRIGHT;
        if (t) return HTTOP;
        if (bo) return HTBOTTOM;
        return HTCAPTION;
    }
    case WM_SIZING: {
        // Keep the picture's shape while resizing.
        auto r = reinterpret_cast<RECT*>(lp);
        const int w = r->right - r->left, h = r->bottom - r->top;
        const float aspect = m_miniAspect.load();
        if (wp == WMSZ_TOP || wp == WMSZ_BOTTOM) {
            r->right = r->left + int(h * aspect);
        } else {
            const int nh = int(w / aspect);
            if (wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT || wp == WMSZ_TOP) r->top = r->bottom - nh;
            else r->bottom = r->top + nh;
        }
        return TRUE;
    }
    case WM_GETMINMAXINFO: {
        auto mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize = {192, 108};
        return 0;
    }
    case WM_SIZE:
        m_miniResize = true;
        SetEvent(m_wake);
        return 0;
    case WM_APP + 30: {   // the picture's shape changed: the height follows
        RECT r;
        GetWindowRect(hwnd, &r);
        SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, int((r.right - r.left) / m_miniAspect.load()), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_NCLBUTTONDBLCLK:
    case WM_NCRBUTTONUP:
    case WM_CLOSE:
        m_miniCloseRequest = true;   // the app thread releases the swap chain, then destroys the window
        SetEvent(m_wake);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// App thread, right after the main window's present.
void App::RenderMini()
{
    if (m_miniCloseRequest.exchange(false)) SetMini(false);
    if (!m_miniOn) return;
    HWND hwnd = m_miniHwnd.load();
    if (!hwnd) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    const UINT w = UINT(std::max<LONG>(1, rc.right - rc.left)), h = UINT(std::max<LONG>(1, rc.bottom - rc.top));
    // The shape of what it shows (device or Advanced canvas): the window follows it.
    float aspect = m_miniAspect;
    if (m_advanced) aspect = float(m_scenes.width) / float(std::max(1u, m_scenes.height));
    else if (Session* s = Active(); s && s->surface.width && s->surface.height) aspect = float(ShownAspect(s->surface, s->surface.width, s->surface.height, s->prof.aspect));
    if (std::abs(aspect - m_miniAspect) > 0.01f) {
        m_miniAspect = aspect;
        PostMessageW(hwnd, WM_APP + 30, 0, 0);
    }
    if (!m_miniOut.swap) {
        if (!m_renderer.CreateOutput(m_miniOut, hwnd, w, h, true)) {
            SetMini(false);
            Toast(L"The mini player couldn't start");
            return;
        }
        m_miniResize = false;
    } else if (m_miniResize.exchange(false)) {
        m_renderer.ResizeOutput(m_miniOut, w, h);
    }
    if (!m_miniOut.rtv) return;
    m_renderer.SetOutputOpacity(m_miniOut, m_miniOpacity);
    const RectF area{0, 0, float(m_miniOut.width), float(m_miniOut.height)};
    m_renderer.BeginOutput(m_miniOut);
    if (m_flash && m_flashSurface.hasImage) {
        m_renderer.DrawSurface(m_flashSurface, Renderer::FitRect(m_flashSurface.width, m_flashSurface.height, area, ScaleMode::Fit), area, true);
    } else if (m_advanced) {
        if (m_program.srv) m_renderer.DrawTexture(m_program.srv.Get(), Renderer::FitRect(m_scenes.width, m_scenes.height, area, ScaleMode::Fit), area, true);
    } else if (Session* s = Active()) {
        DrawSession(*s, area, true, int(ScaleMode::Fit));
    }
    m_renderer.EndOutput();
    m_renderer.PresentOutput(m_miniOut);
}

} // namespace cc
