#pragma once
#include <string>

namespace cc::log {

void Init(const std::wstring& path);
void Write(const wchar_t* level, const wchar_t* fmt, ...);
const std::wstring& Path();

} // namespace cc::log

#define LOGI(...) ::cc::log::Write(L"INFO", __VA_ARGS__)
#define LOGW(...) ::cc::log::Write(L"WARN", __VA_ARGS__)
#define LOGE(...) ::cc::log::Write(L"ERR ", __VA_ARGS__)
