#include "core/Crash.h"

#include <dbghelp.h>

#include <cwchar>

namespace cc {

namespace {

wchar_t g_dir[MAX_PATH] = {};   // set ahead of time: a crash handler shouldn't allocate
wchar_t g_log[MAX_PATH] = {};   // this run's log file

// The log so far, next to the dump (plain reads and writes, a fixed buffer: no allocation, any sharing mode).
void CopyLog(const wchar_t* to)
{
    if (!g_log[0]) return;
    HANDLE in = CreateFileW(g_log, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (in == INVALID_HANDLE_VALUE) return;
    HANDLE out = CreateFileW(to, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out != INVALID_HANDLE_VALUE) {
        static char buf[64 * 1024];
        DWORD n = 0, written = 0;
        while (ReadFile(in, buf, sizeof(buf), &n, nullptr) && n > 0 && WriteFile(out, buf, n, &written, nullptr)) {}
        CloseHandle(out);
    }
    CloseHandle(in);
}

using WriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                  PMINIDUMP_CALLBACK_INFORMATION);

LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep)
{
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1) || !g_dir[0]) return EXCEPTION_CONTINUE_SEARCH;
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto write = dbghelp ? reinterpret_cast<WriteDumpFn>(reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump"))) : nullptr;
    if (!write) return EXCEPTION_CONTINUE_SEARCH;
    SYSTEMTIME st;
    GetLocalTime(&st);
    // Each crash in its own folder: Crashes\<date>\crash.dmp + log.txt (else crash-<date>.dmp next to the settings).
    static wchar_t folder[MAX_PATH + 64], path[MAX_PATH + 96];
    swprintf_s(folder, L"%s\\Crashes", g_dir);
    CreateDirectoryW(folder, nullptr);
    swprintf_s(folder, L"%s\\Crashes\\%04d-%02d-%02d_%02d-%02d-%02d", g_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const bool own = CreateDirectoryW(folder, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
    if (own) swprintf_s(path, L"%s\\crash.dmp", folder);
    else swprintf_s(path, L"%s\\crash-%04d-%02d-%02d_%02d-%02d-%02d.dmp", g_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), ep, FALSE};
    // Threads, stacks and module list (small); not the whole memory.
    // Stacks, threads and modules only: nothing the stacks merely point to (it could hold settings like the own
    // network's password, or picture data), so a report is safe to send along.
    write(GetCurrentProcess(), GetCurrentProcessId(), f, MINIDUMP_TYPE(MiniDumpWithThreadInfo), &info, nullptr,
          nullptr);
    CloseHandle(f);
    if (own) {   // the dump first (it matters most), then the log up to the crash
        swprintf_s(path, L"%s\\log.txt", folder);
        CopyLog(path);
    }
    return EXCEPTION_CONTINUE_SEARCH;   // then Windows' normal crash handling
}

} // namespace

void InstallCrashHandler() { SetUnhandledExceptionFilter(OnCrash); }

void SetCrashDirectory(const std::wstring& dir, const std::wstring& logPath)
{
    wcsncpy_s(g_dir, dir.c_str(), _TRUNCATE);
    wcsncpy_s(g_log, logPath.c_str(), _TRUNCATE);
}

} // namespace cc
