#include "core/Common.h"
#include "core/Log.h"

#include <avrt.h>
#include <wincrypt.h>
#include <shlobj.h>

#include <cstdarg>
#include <cwchar>

namespace cc {

static const int64_t g_qpcFreq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}();

int64_t QpcNow()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

int64_t QpcFrequency() { return g_qpcFreq; }

double QpcToMs(int64_t ticks) { return double(ticks) * 1000.0 / double(g_qpcFreq); }

int64_t QpcTo100ns(int64_t qpc)
{
    const int64_t whole = qpc / g_qpcFreq;
    const int64_t rem = qpc % g_qpcFreq;
    return whole * 10000000LL + rem * 10000000LL / g_qpcFreq;
}

std::wstring Widen(std::string_view s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::string Narrow(std::wstring_view s)
{
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Format(const wchar_t* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int n = _vscwprintf(fmt, copy);
    va_end(copy);
    std::wstring out;
    if (n > 0) {
        out.resize(size_t(n));
        _vsnwprintf_s(out.data(), size_t(n) + 1, _TRUNCATE, fmt, args);
    }
    va_end(args);
    return out;
}

std::wstring HrToString(HRESULT hr)
{
    wchar_t* msg = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, DWORD(hr), 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    if (len == 0) {
        // Media Foundation errors live in mferror.dll
        HMODULE mf = GetModuleHandleW(L"mfplat.dll");
        if (mf)
            len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 mf, DWORD(hr), 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    }
    std::wstring text = Format(L"0x%08X", unsigned(hr));
    if (len && msg) {
        std::wstring m(msg, len);
        while (!m.empty() && (m.back() == L'\n' || m.back() == L'\r' || m.back() == L' ' || m.back() == L'.')) m.pop_back();
        text += L" " + m;
    }
    if (msg) LocalFree(msg);
    return text;
}

std::wstring ExeDirectory()
{
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

std::wstring KnownFolder(const GUID& id)
{
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path))) out = path;
    if (path) CoTaskMemFree(path);
    return out;
}

void SetCurrentThreadName(const wchar_t* name) { SetThreadDescription(GetCurrentThread(), name); }

std::atomic<bool> g_backgroundEco{true};

std::wstring ProtectString(const std::wstring& plain)
{
    if (plain.empty()) return plain;
    DATA_BLOB in{DWORD(plain.size() * sizeof(wchar_t)), reinterpret_cast<BYTE*>(const_cast<wchar_t*>(plain.data()))}, out{};
    if (!CryptProtectData(&in, L"CaptureCard", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return L"";
    DWORD chars = 0;
    std::wstring b64;
    if (CryptBinaryToStringW(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &chars)) {
        b64.resize(chars);
        if (!CryptBinaryToStringW(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &chars)) b64.clear();
        b64.resize(wcslen(b64.c_str()));
    }
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return b64.empty() ? L"" : L"dpapi:" + b64;
}

std::wstring UnprotectString(const std::wstring& stored, bool& ok)
{
    ok = true;
    if (stored.rfind(L"dpapi:", 0) != 0) return stored;   // plain (older settings): encrypted on the next save
    ok = false;
    const std::wstring b64 = stored.substr(6);
    DWORD bytes = 0;
    if (!CryptStringToBinaryW(b64.c_str(), DWORD(b64.size()), CRYPT_STRING_BASE64, nullptr, &bytes, nullptr, nullptr)) return L"";
    std::vector<BYTE> blob(bytes);
    if (!CryptStringToBinaryW(b64.c_str(), DWORD(b64.size()), CRYPT_STRING_BASE64, blob.data(), &bytes, nullptr, nullptr)) return L"";
    DATA_BLOB in{bytes, blob.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return L"";
    std::wstring plain(reinterpret_cast<const wchar_t*>(out.pbData), out.cbData / sizeof(wchar_t));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    ok = true;
    return plain;
}

namespace {
thread_local bool t_wantEco = false, t_appliedEco = false;

void ApplyThreadEfficiency(bool eco)
{
    t_appliedEco = eco;
    // ControlMask 0 = no thread-level choice: the process setting (no throttling, main.cpp) applies again.
    THREAD_POWER_THROTTLING_STATE s{};
    s.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = eco ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0;
    s.StateMask = eco ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0;
    static std::atomic<bool> s_logged{false};
    if (SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &s, sizeof(s))) {
        if (eco && !s_logged.exchange(true)) LOGI(L"Efficiency mode: background work runs with Windows' EcoQoS");
    } else if (!s_logged.exchange(true)) {
        LOGW(L"Efficiency mode not available on this Windows (error %lu)", GetLastError());
    }
}
} // namespace

void SetThreadEfficiency(bool eco)
{
    t_wantEco = eco;
    const bool on = eco && g_backgroundEco.load(std::memory_order_relaxed);
    if (on != t_appliedEco) ApplyThreadEfficiency(on);
}

void RefreshThreadEfficiency()
{
    const bool on = t_wantEco && g_backgroundEco.load(std::memory_order_relaxed);
    if (on != t_appliedEco) ApplyThreadEfficiency(on);
}

MmcssScope::MmcssScope(const wchar_t* task, int priority)
{
    DWORD index = 0;
    m_handle = AvSetMmThreadCharacteristicsW(task, &index);
    if (m_handle && priority != 0) AvSetMmThreadPriority(m_handle, AVRT_PRIORITY(priority));
}

MmcssScope::~MmcssScope()
{
    if (m_handle) AvRevertMmThreadCharacteristics(m_handle);
}

} // namespace cc
