#pragma once
#include <string>

namespace cc {

// INI-backed settings using the Win32 profile API. Portable mode when settings.ini sits next to the exe.
class Settings {
public:
    void Load();
    // Use an arbitrary INI file (created as UTF-16 if missing), e.g. scenes.ini next to settings.ini.
    void Open(const std::wstring& path);
    // Removes a whole section (used when rewriting lists such as scenes).
    void ClearSection(const std::wstring& section);
    bool HasSection(const std::wstring& section) const;
    // Copies every key of `from` into `to` (a device that got a new key keeps its old settings).
    void CopySection(const std::wstring& from, const std::wstring& to);

    const std::wstring& Path() const { return m_path; }
    const std::wstring& Directory() const { return m_dir; }
    bool Portable() const { return m_portable; }
    // Moves settings.ini and scenes.ini next to the exe (portable) or back to %APPDATA%\ZeroDisplay.
    // Fails (with a reason) when the folder can't be written, e.g. an exe in Program Files.
    bool SetPortable(bool portable, std::wstring& error);
    // Testing: use this folder instead of %APPDATA%\ZeroDisplay (set before Load()).
    static std::wstring s_profileDir;
    static std::wstring ProfileDirectory();

    std::wstring GetString(const std::wstring& section, const wchar_t* key, const wchar_t* def = L"") const;
    int GetInt(const std::wstring& section, const wchar_t* key, int def) const;
    float GetFloat(const std::wstring& section, const wchar_t* key, float def) const;
    bool GetBool(const std::wstring& section, const wchar_t* key, bool def) const { return GetInt(section, key, def ? 1 : 0) != 0; }

    void SetString(const std::wstring& section, const wchar_t* key, const std::wstring& value);
    void SetInt(const std::wstring& section, const wchar_t* key, int value);
    void SetFloat(const std::wstring& section, const wchar_t* key, float value);
    void SetBool(const std::wstring& section, const wchar_t* key, bool value) { SetInt(section, key, value ? 1 : 0); }

    // Self-tests on the user's own profile: every write is dropped (the INI files are written immediately otherwise).
    static bool s_readOnly;

    // Section name for per-device settings, sanitized for INI syntax.
    static std::wstring DeviceSection(const std::wstring& deviceName);

private:
    std::wstring m_path;
    std::wstring m_dir;
    bool m_portable = false;
};

} // namespace cc
