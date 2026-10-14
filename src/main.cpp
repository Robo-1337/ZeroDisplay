#include <winsock2.h>

#include "App.h"
#include "core/Crash.h"
#include "core/Log.h"

#include <mfapi.h>
#include <shellapi.h>
#include <timeapi.h>

// Switchable-graphics laptops: the NVIDIA / AMD drivers only let a process use the discrete GPU's video
// encoder (Media Foundation MFT) when the exe asks for "high performance" with these documented exports.
// They also move that GPU to the front of the adapter list, so the renderer never relies on the order:
// "automatic" renders on the GPU that drives the window's monitor (Renderer::Init) and the GPU list is
// enumerated in a fixed (minimum power first) order. The discrete GPU only wakes up when it is used.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

// One ZeroDisplay per Windows session: opening it again brings the running window to the front (a second copy
// would fight the first for the capture card). Starts with options (tests, a second instance watching the network
// stream) are left alone. A restart after importing / resetting settings waits for the old process first.
static bool HandOverToRunningInstance()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool plain = true;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--wait-for") == 0 && i + 1 < argc) {
            if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, DWORD(_wtoi(argv[++i])))) {
                WaitForSingleObject(old, 15000);
                CloseHandle(old);
            }
        } else {
            plain = false;
        }
    }
    if (argv) LocalFree(argv);
    if (!plain) return false;
    static HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\CaptureCard.SingleInstance");   // held until exit
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    for (int tries = 0; tries < 20; ++tries) {   // the first copy may still be creating its window
        if (HWND w = FindWindowW(L"CaptureCardWindow", nullptr)) {
            if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
            SetForegroundWindow(w);
            return true;
        }
        Sleep(100);
    }
    return true;
}

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
    if (HandOverToRunningInstance()) return 0;
    cc::InstallCrashHandler();
    // Latency: no power throttling (EcoQoS), slightly elevated priority. (The 1 ms timer is App's: only while needed.)
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    throttling.StateMask = 0;
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling))) {
        // IGNORE_TIMER_RESOLUTION only exists on Windows 11 / Windows 10 21H2+; older builds reject the mask.
        throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
    }
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    // Media Foundation is delay-loaded: Windows "N"/"KN" editions ship without it unless the Media
    // Feature Pack is installed, and we want a readable message instead of a loader error. Loaded here from
    // System32 only, so the delay-load later finds these and never a same-named DLL next to the exe.
    HMODULE mfplat = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE mfrw = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE mf = LoadLibraryExW(L"mf.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mfplat || !mfrw || !mf) {
        // This Windows edition ships without its media components (capture devices don't work for any
        // app until they're added). They can't be bundled; Microsoft's free Media Feature Pack adds them.
        const int answer = MessageBoxW(nullptr,
                    L"This edition of Windows (an \"N\" edition) ships without its media components, so capture "
                    L"cards, cameras and video recording don't work in any app yet.\n\n"
                    L"Microsoft's free Media Feature Pack adds them. Open the Settings page to install it now?\n\n"
                    L"(Optional features > Add a feature > Media Feature Pack, then restart ZeroDisplay.)",
                    L"ZeroDisplay", MB_ICONINFORMATION | MB_YESNO);
        if (answer == IDYES) {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            ShellExecuteW(nullptr, L"open", L"ms-settings:optionalfeatures", nullptr, nullptr, SW_SHOWNORMAL);
            if (SUCCEEDED(com)) CoUninitialize();
        }
        return 1;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
        MessageBoxW(nullptr, L"Windows Media Foundation failed to start.", L"ZeroDisplay", MB_ICONERROR | MB_OK);
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }
    WSADATA wsa{};
    const bool winsock = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;   // if not, each network feature reports its own error

    int code = 0;
    {
        cc::App app;
        code = app.Run(instance, cc::CommandLine::Parse());
        // Everything is saved by now. If the final cleanup hangs (a Windows service not answering), don't leave a
        // process behind that keeps the capture card busy: end it after a few seconds.
        std::thread([] {
            Sleep(8000);
            LOGW(L"Exit took longer than 8 s (a system call didn't return): ending the process");
            TerminateProcess(GetCurrentProcess(), 0);
        }).detach();
    }

    if (winsock) WSACleanup();
    MFShutdown();
    if (SUCCEEDED(hr)) CoUninitialize();
    return code;
}
