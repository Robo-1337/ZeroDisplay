#pragma once
#include "core/Common.h"

namespace cc {

// On a crash, saves a minidump and the run's log so far in their own folder next to the settings
// (Crashes\<date>\crash.dmp + log.txt) so problems on other PCs can be diagnosed.
// dbghelp.dll (part of Windows) is loaded only when a crash happens.
void InstallCrashHandler();
void SetCrashDirectory(const std::wstring& dir, const std::wstring& logPath);

} // namespace cc
