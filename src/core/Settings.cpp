#include "core/Settings.h"
#include "core/Common.h"

#include <knownfolders.h>
#include <shlobj.h>

#include <cmath>
#include <iterator>
#include <vector>

namespace cc {

std::wstring Settings::s_profileDir;

std::wstring Settings::ProfileDirectory()
{
    if (!s_profileDir.empty()) return s_profileDir;
    const std::wstring roaming = KnownFolder(FOLDERID_RoamingAppData);
    const std::wstring dir = roaming + L"\\ZeroDisplay", old = roaming + L"\\CaptureCard";
    // The app was called CaptureCard before the rename: its folder (settings, scenes, the list of files it made, crash
    // reports) moves across once. If it can't be moved (open elsewhere), the old folder stays in use.
    static const bool useOld = [&] {
        if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES || GetFileAttributesW(old.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
        return !MoveFileExW(old.c_str(), dir.c_str(), 0);
    }();
    return useOld ? old : dir;
}

void Settings::Load()
{
    std::wstring local = ExeDirectory() + L"\\settings.ini";
    // An explicit --profile-dir (tests) wins over portable settings next to the exe, which tests never touch.
    if (s_profileDir.empty() && GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) {
        m_portable = true;
        m_dir = ExeDirectory();
        m_path = local;
    } else {
        m_dir = ProfileDirectory();
        SHCreateDirectoryExW(nullptr, m_dir.c_str(), nullptr);
        m_path = m_dir + L"\\settings.ini";
    }
    Open(m_path);
}

bool Settings::SetPortable(bool portable, std::wstring& error)
{
    if (portable == m_portable) return true;
    const std::wstring dir = portable ? ExeDirectory() : ProfileDirectory();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    const std::wstring oldDir = m_dir;
    std::vector<std::wstring> moved;
    for (const wchar_t* name : {L"settings.ini", L"scenes.ini"}) {
        const std::wstring from = oldDir + L"\\" + name, to = dir + L"\\" + name;
        if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        // Never overwrite settings / scenes already there (an older install, another profile): keep them as .bak.
        if (GetFileAttributesW(to.c_str()) != INVALID_FILE_ATTRIBUTES && !MoveFileExW(to.c_str(), (to + L".bak").c_str(), MOVEFILE_REPLACE_EXISTING)) {
            error = L"Can't make room in " + dir + L" (" + HrToString(HRESULT_FROM_WIN32(GetLastError())) + L")";
            for (const auto& n : moved) MoveFileExW((dir + L"\\" + n).c_str(), (oldDir + L"\\" + n).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
            return false;
        }
        if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED)) {
            error = L"Can't write to " + dir + L" (" + HrToString(HRESULT_FROM_WIN32(GetLastError())) + L")";
            for (const auto& n : moved) MoveFileExW((dir + L"\\" + n).c_str(), (oldDir + L"\\" + n).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
            return false;
        }
        moved.push_back(name);
    }
    m_dir = dir;
    m_path = dir + L"\\settings.ini";
    m_portable = portable;
    Open(m_path);   // creates it if there was nothing to move
    return true;
}

bool Settings::s_readOnly = false;

void Settings::ClearSection(const std::wstring& section)
{
    if (s_readOnly) return;
    WritePrivateProfileStringW(section.c_str(), nullptr, nullptr, m_path.c_str());
}

bool Settings::HasSection(const std::wstring& section) const
{
    wchar_t buf[4] = {};
    return GetPrivateProfileSectionW(section.c_str(), buf, DWORD(std::size(buf)), m_path.c_str()) > 0;
}

void Settings::CopySection(const std::wstring& from, const std::wstring& to)
{
    if (s_readOnly) return;
    std::wstring buf(32768, L'\0');
    const DWORD n = GetPrivateProfileSectionW(from.c_str(), buf.data(), DWORD(buf.size()), m_path.c_str());
    if (n == 0 || n >= buf.size() - 2) return;   // empty, or too large to be a device section
    WritePrivateProfileSectionW(to.c_str(), buf.c_str(), m_path.c_str());
}

void Settings::Open(const std::wstring& path)
{
    m_path = path;
    if (m_dir.empty()) {
        size_t slash = path.find_last_of(L"\\/");
        m_dir = slash == std::wstring::npos ? L"." : path.substr(0, slash);
    }
    // A UTF-16LE BOM makes the profile API read and write Unicode (device names can be non-ASCII).
    if (GetFileAttributesW(m_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(m_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            const BYTE bom[2] = {0xFF, 0xFE};
            DWORD written = 0;
            WriteFile(f, bom, 2, &written, nullptr);
            CloseHandle(f);
        }
    }
}

std::wstring Settings::GetString(const std::wstring& section, const wchar_t* key, const wchar_t* def) const
{
    wchar_t buf[2048];
    DWORD n = GetPrivateProfileStringW(section.c_str(), key, def, buf, DWORD(std::size(buf)), m_path.c_str());
    return std::wstring(buf, n);
}

int Settings::GetInt(const std::wstring& section, const wchar_t* key, int def) const
{
    std::wstring s = GetString(section, key, L"");
    if (s.empty()) return def;
    return _wtoi(s.c_str());
}

float Settings::GetFloat(const std::wstring& section, const wchar_t* key, float def) const
{
    std::wstring s = GetString(section, key, L"");
    if (s.empty()) return def;
    const double v = _wtof(s.c_str());
    return std::isfinite(v) && std::abs(v) < 1e9 ? float(v) : def;   // "nan", "inf", 1e309 from a damaged file: the default
}

void Settings::SetString(const std::wstring& section, const wchar_t* key, const std::wstring& value)
{
    if (s_readOnly) return;
    WritePrivateProfileStringW(section.c_str(), key, value.c_str(), m_path.c_str());
}

void Settings::SetInt(const std::wstring& section, const wchar_t* key, int value)
{
    SetString(section, key, std::to_wstring(value));
}

void Settings::SetFloat(const std::wstring& section, const wchar_t* key, float value)
{
    SetString(section, key, Format(L"%.4f", value));
}

std::wstring Settings::DeviceSection(const std::wstring& deviceName)
{
    std::wstring s = L"Device." + deviceName;
    for (auto& c : s)
        if (c == L'[' || c == L']' || c == L'=' || c == L';') c = L'_';
    return s;
}

} // namespace cc
