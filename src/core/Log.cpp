#include "core/Log.h"
#include "core/Common.h"

#include <cstdarg>

namespace cc::log {

static std::mutex g_mutex;
static HANDLE g_file = INVALID_HANDLE_VALUE;
static std::wstring g_path;
static const int64_t g_start = QpcNow();

void Init(const std::wstring& path)
{
    std::lock_guard lock(g_mutex);
    g_path = path;
    MoveFileExW(path.c_str(), (path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) {
        // Another instance owns log.txt: use a per-process file next to it.
        size_t dot = path.rfind(L'.');
        g_path = path.substr(0, dot) + L"-" + std::to_wstring(GetCurrentProcessId()) + (dot == std::wstring::npos ? L"" : path.substr(dot));
        g_file = CreateFileW(g_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
}

const std::wstring& Path() { return g_path; }

void Write(const wchar_t* level, const wchar_t* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = _vscwprintf(fmt, args);
    std::wstring msg;
    if (n > 0) {
        msg.resize(size_t(n));
        _vsnwprintf_s(msg.data(), size_t(n) + 1, _TRUNCATE, fmt, args);
    }
    va_end(args);

    std::wstring line = Format(L"[%9.3f] %s %s\r\n", QpcElapsedMs(g_start) / 1000.0, level, msg.c_str());
    OutputDebugStringW(line.c_str());
    std::string utf8 = Narrow(line);
    std::lock_guard lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_file, utf8.data(), DWORD(utf8.size()), &written, nullptr);
    }
}

} // namespace cc::log
