// Where files go and what they're called: the recordings / screenshots folders (chosen in Settings ▸ Files), the
// file name template, screenshot extras (clipboard, flash) and the disk rules (stop below X GB free; keep the
// recordings under N GB by moving the oldest the app made to the Recycle Bin).
#include "App.h"
#include "core/Log.h"

#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <fstream>

namespace cc {

const wchar_t* const App::kDefaultNameTemplate = L"{type}_{date}_{time}";

// {type} {device} {date} {time} → a file name (without folder and extension); characters Windows doesn't allow in
// file names become '_'. Empty or nonsense templates fall back to the default.
std::wstring App::ExpandNameTemplate(const std::wstring& tmpl, const std::wstring& type, const std::wstring& device, const SYSTEMTIME& st)
{
    std::wstring t = tmpl.empty() ? kDefaultNameTemplate : tmpl;
    auto replace = [&](const wchar_t* key, const std::wstring& value) {
        for (size_t p = t.find(key); p != std::wstring::npos; p = t.find(key, p + value.size())) t.replace(p, wcslen(key), value);
    };
    replace(L"{type}", type);
    replace(L"{device}", device.empty() ? std::wstring(L"Canvas") : device);
    replace(L"{date}", Format(L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay));
    replace(L"{time}", Format(L"%02d-%02d-%02d", st.wHour, st.wMinute, st.wSecond));
    for (wchar_t& c : t)
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    // Text-direction controls (e.g. U+202E) could make a name show a different extension in Explorer.
    std::erase_if(t, [](wchar_t c) { return (c >= 0x200E && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069); });
    while (!t.empty() && (t.back() == L' ' || t.back() == L'.')) t.pop_back();   // Windows drops these anyway
    // Device names Windows reserves (a device renamed "NUL" with the template "{device}"): never a file name on their own.
    static const wchar_t* const reserved[] = {L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7",
                                              L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
    for (const wchar_t* r : reserved)
        if (_wcsicmp(t.c_str(), r) == 0 || (_wcsnicmp(t.c_str(), r, wcslen(r)) == 0 && t.size() > wcslen(r) && t[wcslen(r)] == L'.')) {
            t.insert(t.begin(), L'_');
            break;
        }
    return t.empty() ? ExpandNameTemplate(kDefaultNameTemplate, type, device, st) : t;
}

// A new file in `folder`: the template's name, " (2)", " (3)", ... when that exists already.
std::wstring App::NewFilePath(const std::wstring& folder, const wchar_t* type, const wchar_t* ext)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    Session* s = Active();
    const std::wstring device = m_advanced ? std::wstring() : (s ? s->info.name : std::wstring());
    const std::wstring base = folder + L"\\" + ExpandNameTemplate(m_nameTemplate, type, device, st);
    std::wstring path = base + ext;
    for (int n = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && n < 1000; ++n) path = base + Format(L" (%d)", n) + ext;
    return path;
}

void App::PickFolder(int which)
{
    const std::wstring start = which == 0 ? RecordFolder() : ScreenshotFolder();
    m_graveyard.push_back(std::async(std::launch::async, [this, which, start] {
        ComInit com(COINIT_APARTMENTTHREADED);
        ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        FILEOPENDIALOGOPTIONS opt = 0;
        dlg->GetOptions(&opt);
        dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(which == 0 ? L"Folder for recordings, replays and clips" : L"Folder for screenshots");
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder)))) dlg->SetFolder(folder.Get());
        if (FAILED(dlg->Show(m_hwnd))) return;
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
        {
            std::lock_guard lock(m_pickMutex);
            m_folderPicked = {which, path};
        }
        CoTaskMemFree(path);
        SetEvent(m_wake);
    }));
}

void App::ApplyPickedFolder()
{
    std::pair<int, std::wstring> picked;
    {
        std::lock_guard lock(m_pickMutex);
        picked.swap(m_folderPicked);
    }
    if (picked.second.empty()) return;
    if (!EnsureFolder(picked.second, picked.first == 0 ? L"recordings" : L"screenshots")) return;   // says why
    (picked.first == 0 ? m_recordFolderSetting : m_shotFolderSetting) = picked.second;
    m_settingsDirty = true;
    LOGI(L"%s folder: %s", picked.first == 0 ? L"Recordings" : L"Screenshots", picked.second.c_str());
    Toast((picked.first == 0 ? L"Recordings go to " : L"Screenshots go to ") + picked.second);
}

// The files ZeroDisplay wrote (recordings, replays, clips), one path per line (UTF-8) next to the settings: the "keep
// under N GB" rule only ever touches these, never other files in the folder.
std::wstring App::MadeFilesIndex() const { return m_settings.Directory() + L"\\recordings.txt"; }

void App::RememberMadeFile(const std::wstring& path)
{
    if (Settings::s_readOnly) return;   // self-tests leave the user's profile alone
    std::lock_guard lock(m_madeMutex);
    std::ofstream out(MadeFilesIndex(), std::ios::app | std::ios::binary);
    if (out) out << Narrow(path) << "\n";
}

// Worker thread, after a recording / replay / clip was saved.
void App::EnforceKeepLimit()
{
    const int keepGb = m_keepGb.load();
    if (keepGb <= 0 || Settings::s_readOnly) return;
    std::lock_guard lock(m_madeMutex);
    std::vector<std::wstring> paths;
    {
        std::ifstream in(MadeFilesIndex(), std::ios::binary);
        for (std::string line; std::getline(in, line);) {
            if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);   // a byte-order mark (edited in Notepad)
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;
            std::wstring p = Widen(line);
            // A path listed twice (a name used again) counts once: twice would move one recording too many.
            const bool seen = std::any_of(paths.begin(), paths.end(), [&](const std::wstring& q) { return _wcsicmp(q.c_str(), p.c_str()) == 0; });
            if (!seen) paths.push_back(std::move(p));
        }
    }
    struct File {
        std::wstring path;
        uint64_t size;
        FILETIME written;
    };
    std::vector<File> files;
    uint64_t total = 0;
    for (const auto& p : paths) {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) continue;   // gone: forgotten
        const uint64_t size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
        files.push_back({p, size, a.ftLastWriteTime});
        total += size;
    }
    std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return CompareFileTime(&a.written, &b.written) < 0; });
    const uint64_t limit = m_cmd.keepTestMb > 0 ? uint64_t(m_cmd.keepTestMb) << 20 : uint64_t(keepGb) << 30;
    size_t removed = 0;
    while (total > limit && files.size() > 1) {   // never the newest
        std::wstring from = files.front().path;
        from.push_back(L'\0');   // double-terminated list
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.hwnd = m_hwnd;
        // To the Recycle Bin. A file too big for it would be deleted for good: Windows asks first (and "No" stops here).
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_WANTNUKEWARNING;
        if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) {
            LOGW(L"Keep under %d GB: couldn't move %s to the Recycle Bin", keepGb, files.front().path.c_str());
            break;
        }
        LOGI(L"Keep under %d GB: moved %s to the Recycle Bin", keepGb, files.front().path.c_str());
        total -= files.front().size;
        files.erase(files.begin());
        ++removed;
    }
    std::ofstream out(MadeFilesIndex(), std::ios::trunc | std::ios::binary);
    for (const auto& f : files) out << Narrow(f.path) << "\n";
    if (removed) Toast(Format(L"Recordings over %d GB: %zu oldest moved to the Recycle Bin", keepGb, removed));
}

// Screenshot to the clipboard as a device-independent bitmap (bottom-up, 32-bit).
void App::CopyImageToClipboard(UINT w, UINT h, const std::vector<uint8_t>& bgra)
{
    const size_t row = size_t(w) * 4, bytes = sizeof(BITMAPINFOHEADER) + row * h;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) return;
    auto* p = static_cast<uint8_t*>(GlobalLock(mem));
    if (!p) {
        GlobalFree(mem);
        return;
    }
    BITMAPINFOHEADER bi{sizeof(bi), LONG(w), LONG(h), 1, 32, BI_RGB};
    memcpy(p, &bi, sizeof(bi));
    for (UINT y = 0; y < h; ++y) memcpy(p + sizeof(bi) + row * y, bgra.data() + row * (h - 1 - y), row);
    GlobalUnlock(mem);
    bool ok = false;
    for (int tries = 0; tries < 5 && !ok; ++tries) {   // another app may hold the clipboard for a moment
        if (OpenClipboard(m_hwnd)) {
            EmptyClipboard();
            ok = SetClipboardData(CF_DIB, mem) != nullptr;
            CloseClipboard();
        } else {
            Sleep(20);
        }
    }
    if (!ok) GlobalFree(mem);
}

} // namespace cc
