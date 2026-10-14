#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace cc {

// ---- Time (QueryPerformanceCounter based) ----
int64_t QpcNow();
int64_t QpcFrequency();
double QpcToMs(int64_t ticks);
inline double QpcElapsedMs(int64_t since) { return QpcToMs(QpcNow() - since); }
// Converts a QPC value to 100ns units (same time base as MFGetSystemTime).
int64_t QpcTo100ns(int64_t qpc);

// ---- Strings ----
std::wstring Widen(std::string_view s);
std::string Narrow(std::wstring_view s);
std::wstring Format(const wchar_t* fmt, ...);
std::wstring HrToString(HRESULT hr);
std::wstring ExeDirectory();
std::wstring KnownFolder(const GUID& id);

void SetCurrentThreadName(const wchar_t* name);

// Secrets in settings files (the own network's password): encrypted for this Windows user (DPAPI) as "dpapi:<base64>".
// Unprotect returns plain text unchanged (older settings) and sets ok = false when it can't decrypt (another PC /
// user, e.g. a portable copy), the caller then makes a new secret.
std::wstring ProtectString(const std::wstring& plain);
std::wstring UnprotectString(const std::wstring& stored, bool& ok);

// Windows' efficiency mode (EcoQoS) for background work that nobody waits on frame by frame (saving files, network
// discovery, the welcome page): Windows may run it on efficient cores at a low clock. Picture, sound, capture and
// encoders never use it. Off for everything when g_backgroundEco is false (Settings ▸ General ▸ Power & memory).
extern std::atomic<bool> g_backgroundEco;
void SetThreadEfficiency(bool eco);   // for this thread (follows g_backgroundEco)
void RefreshThreadEfficiency();       // long-lived threads: pick up a changed setting (cheap)
// For pooled threads (std::async): efficient for the scope, then back to the process default.
class EfficiencyScope {
public:
    EfficiencyScope() { SetThreadEfficiency(true); }
    ~EfficiencyScope() { SetThreadEfficiency(false); }
    EfficiencyScope(const EfficiencyScope&) = delete;
    EfficiencyScope& operator=(const EfficiencyScope&) = delete;
};

// Registers the calling thread with MMCSS for the lifetime of the scope.
class MmcssScope {
public:
    explicit MmcssScope(const wchar_t* task, int priority = 0);
    ~MmcssScope();
    MmcssScope(const MmcssScope&) = delete;
    MmcssScope& operator=(const MmcssScope&) = delete;

private:
    HANDLE m_handle = nullptr;
};

class ComInit {
public:
    explicit ComInit(DWORD model = COINIT_MULTITHREADED) { m_ok = SUCCEEDED(CoInitializeEx(nullptr, model)); }
    ~ComInit() { if (m_ok) CoUninitialize(); }
    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;

private:
    bool m_ok = false;
};

struct RectF {
    float x = 0, y = 0, w = 0, h = 0;
    float Right() const { return x + w; }
    float Bottom() const { return y + h; }
    bool Contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    RectF Inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    RectF Inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    bool operator==(const RectF&) const = default;
};

template <typename T> T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace cc
