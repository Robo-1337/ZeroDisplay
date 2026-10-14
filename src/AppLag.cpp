// Lag check: flashes a full-screen window white once a second on a monitor whose output feeds the
// capture card (e.g. this PC's HDMI port into the card's input) and measures how long it takes until
// a captured frame shows the flash. Result = display scan-out + HDMI + capture device + USB + driver,
// i.e. everything before ZeroDisplay gets the frame (our own path after that is ~2-5 ms, see stats).
#include "App.h"
#include "core/Log.h"

#include <algorithm>
#include <dwmapi.h>

namespace cc {

static constexpr int kFlashes = 10;

struct MonitorPick {
    HMONITOR avoid;
    std::vector<HMONITOR> all;
};

static BOOL CALLBACK CollectMonitor(HMONITOR m, HDC, LPRECT, LPARAM p)
{
    reinterpret_cast<MonitorPick*>(p)->all.push_back(m);
    return TRUE;
}

void App::StartLagCheck()
{
    if (m_lag) return;
    if (!Active() && m_cmd.lagSim < 0) {
        Toast(L"Open the capture device first");
        return;
    }
    m_lag = std::make_unique<LagCheck>();
    m_lag->phaseStart = QpcNow();
    m_lagResult.clear();
    m_settingsOpen = false;
    // Prefer a monitor other than the one ZeroDisplay is on (usually the one connected to the card).
    MonitorPick pick{MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), {}};
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, LPARAM(&pick));
    int index = 0;
    for (size_t i = 0; i < pick.all.size(); ++i)
        if (pick.all[i] != pick.avoid) index = int(i);
    if (m_cmd.lagSim < 0) PostMessageW(m_hwnd, WM_APP + 6, 1, LPARAM(index));
    LOGI(L"Lag check started (%zu monitor(s), flashing #%d)", pick.all.size(), index);
    Toast(pick.all.size() > 1 ? L"Measuring: watch for the flashes on your other screen (Esc stops)"
                              : L"Measuring: the screen will flash 10 times (Esc stops)");
}

void App::StopLagCheck(bool showResult)
{
    if (!m_lag) return;
    PostMessageW(m_hwnd, WM_APP + 6, 0, 0);
    if (showResult) {
        auto& r = m_lag->results;
        if (r.empty()) {
            m_lagResult = L"No flash reached the capture card. Make sure the flashing screen is the one connected to its HDMI input.";
        } else {
            std::sort(r.begin(), r.end());
            double sum = 0;
            for (double v : r) sum += v;
            const double avg = sum / double(r.size());
            const double median = r[r.size() / 2];
            m_lagResult = Format(L"Capture latency: %.0f ms (median %.0f, min %.0f, max %.0f; %zu of %d flashes seen). "
                                 L"Measured from the flash reaching the screen to the frame arriving here.",
                                 avg, median, r.front(), r.back(), r.size(), kFlashes);
        }
        LOGI(L"Lag check: %s", m_lagResult.c_str());
        Toast(m_lagResult);
    }
    m_lag.reset();
    m_flashWhite = false;
}

// Called for every new frame of the active device (newFrame) and once per app frame.
void App::UpdateLagCheck(Session* s, bool newFrame, int64_t frameQpc)
{
    if (!m_lag) return;
    LagCheck& L = *m_lag;
    const int64_t now = QpcNow();
    if (newFrame && s) {
        if (m_cmd.lagSim >= 0) {
            // Self-test: pretend the chain shows the flash `lagSim` ms after it went up.
            const int64_t shown = m_flashShownQpc.load();
            const bool lit = m_flashWhite.load() && shown && QpcToMs(frameQpc - shown) >= m_cmd.lagSim;
            m_renderer.FillSurface(s->surface, lit ? 1.0f : 0.0f);
        }
        m_renderer.LagSample(s->surface, frameQpc);
        return;
    }
    // Collect brightness samples.
    if (s) {
        float level = 0;
        int64_t fq = 0;
        while (m_renderer.LagResult(s->surface, level, fq)) {
            const int64_t shown = m_flashShownQpc.load();
            if (L.white && !L.measured && shown && fq > shown && level > 0.5f) {
                L.measured = true;
                L.results.push_back(QpcToMs(fq - shown));
                LOGI(L"Lag check: flash %d seen after %.1f ms", L.flashes, L.results.back());
            }
        }
    }
    // Flash timing: 1 s per flash, white for the first 400 ms.
    const double t = QpcToMs(now - L.phaseStart);
    if (!L.white && t >= 600.0) {
        if (L.flashes >= kFlashes) {
            StopLagCheck(true);
            return;
        }
        L.white = true;
        L.measured = false;
        ++L.flashes;
        L.phaseStart = now;
        m_flashShownQpc = 0;
        m_flashWhite = true;
        if (m_cmd.lagSim >= 0) m_flashShownQpc = now;   // no real window in the simulation
        else PostMessageW(m_hwnd, WM_APP + 7, 0, 0);
    } else if (L.white && t >= 400.0) {
        if (!L.measured) ++L.missed;
        L.white = false;
        L.phaseStart = now;
        m_flashWhite = false;
        if (m_cmd.lagSim < 0) PostMessageW(m_hwnd, WM_APP + 7, 0, 0);
    }
}

// ---------------------------------------------------------------------------------------------
// Flash window (window thread)

void App::CreateFlashWindow(int monitor)
{
    if (m_flashHwnd.load()) return;
    MonitorPick pick{nullptr, {}};
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, LPARAM(&pick));
    if (pick.all.empty()) return;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(pick.all[size_t(std::clamp(monitor, 0, int(pick.all.size()) - 1))], &mi);
    const RECT r = mi.rcMonitor;
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"CaptureCardFlash", L"ZeroDisplay lag check", WS_POPUP, r.left, r.top,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd) return;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    m_flashHwnd = hwnd;
}

LRESULT CALLBACK App::FlashProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(cs->lpCreateParams));
    }
    auto self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, HBRUSH(GetStockObject(self && self->m_flashWhite.load() ? WHITE_BRUSH : BLACK_BRUSH)));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        SetCursor(nullptr);
        return TRUE;
    case WM_KEYDOWN:
    case WM_LBUTTONDOWN:
        if (self) {
            {
                std::lock_guard lock(self->m_inputMutex);
                self->m_input.keys.push_back(VK_ESCAPE);   // any key / click cancels
            }
            SetEvent(self->m_wake);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace cc
