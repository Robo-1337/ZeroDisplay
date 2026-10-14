// Settings as a whole: export to one file (settings + scenes), import from it, and reset everything to the
// defaults. Import and reset happen while ZeroDisplay closes (after its last save) and it starts again.
#include "App.h"
#include "core/Log.h"

#include <shobjidl.h>

namespace cc {

namespace {

constexpr wchar_t kScenesPrefix[] = L"Scenes|";

std::vector<std::wstring> SectionNames(const std::wstring& path)
{
    std::vector<wchar_t> buf(1 << 16);
    const DWORD n = GetPrivateProfileSectionNamesW(buf.data(), DWORD(buf.size()), path.c_str());
    std::vector<std::wstring> out;
    for (DWORD i = 0; i < n;) {
        std::wstring s(&buf[i]);
        if (s.empty()) break;
        i += DWORD(s.size()) + 1;
        out.push_back(std::move(s));
    }
    return out;
}

// "key=value\0key=value\0\0", as the profile API reads and writes whole sections.
std::vector<wchar_t> SectionData(const std::wstring& path, const std::wstring& section)
{
    std::vector<wchar_t> buf(1 << 16);
    const DWORD n = GetPrivateProfileSectionW(section.c_str(), buf.data(), DWORD(buf.size()), path.c_str());
    buf.resize(n + 2);
    buf[n] = buf[n + 1] = 0;
    return buf;
}

// Without the keys that belong to this PC / this Windows user (and can't be used anywhere else).
std::vector<wchar_t> Filtered(const std::vector<wchar_t>& data, std::initializer_list<const wchar_t*> dropKeys)
{
    std::vector<wchar_t> out;
    for (size_t i = 0; i < data.size() && data[i];) {
        std::wstring line(&data[i]);
        i += line.size() + 1;
        bool drop = false;
        for (const wchar_t* k : dropKeys) drop |= line.rfind(std::wstring(k) + L"=", 0) == 0;
        if (drop) continue;
        out.insert(out.end(), line.begin(), line.end());
        out.push_back(0);
    }
    out.push_back(0);
    if (out.size() == 1) out.push_back(0);
    return out;
}

bool NewUnicodeIni(const std::wstring& path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const uint8_t bom[2] = {0xFF, 0xFE};   // UTF-16: the profile API then keeps every character
    DWORD w = 0;
    WriteFile(f, bom, 2, &w, nullptr);
    CloseHandle(f);
    return w == 2;
}

// Sections that belong to this PC only: never exported, kept as they are by import and reset.
// NetworkPins: the PINs of PCs this one watched, encrypted for this Windows user.
bool LocalOnly(const std::wstring& section)
{
    return section == L"Window" || section == L"EncoderAuto" || section == L"CaptureCardExport" || section == L"NetworkPins";
}

} // namespace

void App::PickSettingsFile(bool exportIt)
{
    m_graveyard.push_back(std::async(std::launch::async, [this, exportIt] {
        ComInit com(COINIT_APARTMENTTHREADED);
        ComPtr<IFileDialog> dlg;
        if (FAILED(CoCreateInstance(exportIt ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        COMDLG_FILTERSPEC spec[1] = {{L"ZeroDisplay settings", L"*.ini"}};
        dlg->SetFileTypes(1, spec);
        dlg->SetDefaultExtension(L"ini");
        if (exportIt) dlg->SetFileName(L"ZeroDisplay settings.ini");
        dlg->SetTitle(exportIt ? L"Export settings and scenes" : L"Import settings and scenes");
        if (FAILED(dlg->Show(m_hwnd))) return;
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
        {
            std::lock_guard lock(m_pickMutex);
            m_settingsFilePicked = {exportIt ? 1 : 2, path};
        }
        CoTaskMemFree(path);
        SetEvent(m_wake);
    }));
}

void App::ApplyPickedSettingsFile()
{
    std::pair<int, std::wstring> picked;
    {
        std::lock_guard lock(m_pickMutex);
        picked.swap(m_settingsFilePicked);
    }
    if (picked.second.empty()) return;
    if (picked.first == 1) {
        SaveSettings();   // what's set now goes into the file
        if (ExportSettings(picked.second)) ToastFile(L"Settings and scenes exported: " + picked.second, picked.second);
        else Toast(L"Couldn't write " + picked.second);
        return;
    }
    if (GetPrivateProfileIntW(L"CaptureCardExport", L"Version", 0, picked.second.c_str()) < 1) {
        Toast(L"That file isn't a ZeroDisplay settings export");
        return;
    }
    m_importAsked = picked.second;   // confirmed in Settings ▸ General (it restarts the app)
}

bool App::ExportSettings(const std::wstring& path)
{
    if (!NewUnicodeIni(path)) return false;
    WritePrivateProfileStringW(L"CaptureCardExport", L"Version", L"1", path.c_str());
    WritePrivateProfileStringW(L"CaptureCardExport", L"App", CC_VERSION, path.c_str());
    const std::wstring settings = m_settings.Path(), scenes = m_settings.Directory() + L"\\scenes.ini";
    for (const std::wstring& s : SectionNames(settings)) {
        if (LocalOnly(s)) continue;
        // The own Wi-Fi password and the sharing PIN are encrypted for this Windows user; bookkeeping keys are this PC's.
        const auto data =
            Filtered(SectionData(settings, s), {L"HotspotPassword", L"ViewerPin", L"WeakPcChecked", L"CrashReported", L"WindowOpen", L"Window"});
        WritePrivateProfileSectionW(s.c_str(), data.data(), path.c_str());
    }
    for (const std::wstring& s : SectionNames(scenes))
        WritePrivateProfileSectionW((kScenesPrefix + s).c_str(), SectionData(scenes, s).data(), path.c_str());
    LOGI(L"Settings exported: %s", path.c_str());
    return true;
}

// App thread, while closing, after the last SaveSettings: import / reset, then the next start applies them.
void App::ApplySettingsChangeOnExit()
{
    if (m_onExit == OnExit::None || Settings::s_readOnly) return;
    const std::wstring settings = m_settings.Path(), scenes = m_settings.Directory() + L"\\scenes.ini";
    const std::wstring password = m_settings.GetString(L"Wireless", L"HotspotPassword", L"");
    const std::wstring viewerPin = m_settings.GetString(L"Network", L"ViewerPin", L"");
    for (const std::wstring& s : SectionNames(settings))
        if (!LocalOnly(s)) WritePrivateProfileStringW(s.c_str(), nullptr, nullptr, settings.c_str());
    if (m_onExit == OnExit::Import) {
        for (const std::wstring& s : SectionNames(scenes)) WritePrivateProfileStringW(s.c_str(), nullptr, nullptr, scenes.c_str());
        for (const std::wstring& s : SectionNames(m_importPath)) {
            if (s == L"CaptureCardExport") continue;
            const auto data = SectionData(m_importPath, s);
            if (s.rfind(kScenesPrefix, 0) == 0) WritePrivateProfileSectionW(s.substr(wcslen(kScenesPrefix)).c_str(), data.data(), scenes.c_str());
            else if (!LocalOnly(s)) WritePrivateProfileSectionW(s.c_str(), data.data(), settings.c_str());
        }
        LOGI(L"Settings imported from %s", m_importPath.c_str());
    } else {
        LOGI(L"Settings reset to the defaults (scenes kept)");
    }
    if (!password.empty()) WritePrivateProfileStringW(L"Wireless", L"HotspotPassword", password.c_str(), settings.c_str());   // stays this PC's
    if (!viewerPin.empty()) WritePrivateProfileStringW(L"Network", L"ViewerPin", viewerPin.c_str(), settings.c_str());   // so does the PIN
}

void App::RequestRestart(OnExit what)
{
    m_onExit = what;
    m_restartAfterExit = true;
    PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
}

} // namespace cc
