// While recording, the taskbar button gets a small red dot (yellow while paused) and the window title says so: a
// minimized ZeroDisplay still shows that it records. Window thread (ITaskbarList3 needs the window's thread).
#include "App.h"
#include "core/Log.h"

#include <shobjidl.h>

namespace cc {

// A round dot icon (ARGB, anti-aliased edge), made once per colour.
static HICON DotIcon(COLORREF rgb)
{
    constexpr int n = 32;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = n;
    bi.bV5Height = -n;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return nullptr;
    auto* px = static_cast<uint32_t*>(bits);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const double dx = x + 0.5 - n / 2.0, dy = y + 0.5 - n / 2.0, d = std::sqrt(dx * dx + dy * dy);
            const double edge = std::clamp(n / 2.0 - 2.0 - d + 0.5, 0.0, 1.0);   // white ring outside, colour inside
            const double ring = std::clamp(n / 2.0 - d + 0.5, 0.0, 1.0);
            const uint32_t a = uint32_t(ring * 255.0);
            const uint32_t r = uint32_t((GetRValue(rgb) * edge + 255 * (1 - edge)) * ring);
            const uint32_t g = uint32_t((GetGValue(rgb) * edge + 255 * (1 - edge)) * ring);
            const uint32_t b = uint32_t((GetBValue(rgb) * edge + 255 * (1 - edge)) * ring);
            px[y * n + x] = (a << 24) | (r << 16) | (g << 8) | b;   // premultiplied
        }
    HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

// Window thread: 0 nothing, 1 recording, 2 paused.
void App::ShowRecordingOnTaskbar(int state)
{
    static HICON rec = DotIcon(RGB(232, 48, 48)), paused = DotIcon(RGB(240, 180, 40));
    if (!m_taskbar) {
        if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_taskbar))) || FAILED(m_taskbar->HrInit())) {
            m_taskbar.Reset();
        }
    }
    // No icon (nullptr) removes the overlay, as documented; the SDK's annotation doesn't say so.
#pragma warning(suppress : 6387)
    if (m_taskbar) m_taskbar->SetOverlayIcon(m_hwnd, state == 1 ? rec : (state == 2 ? paused : nullptr),
                                             state == 1 ? L"Recording" : (state == 2 ? L"Recording paused" : L""));
    SetWindowTextW(m_hwnd, state == 1 ? L"● REC · ZeroDisplay" : (state == 2 ? L"❚❚ Paused · ZeroDisplay" : L"ZeroDisplay"));
}

} // namespace cc
