// Power & memory profiles: Battery saver (on battery by default), Memory saver (when Windows runs low by default)
// and Low-resource mode (manual, for weak PCs). One place decides; features ask (ReplayAllowed(), EncodeHw(), ...).
#include "App.h"
#include "core/Log.h"

#include <mmsystem.h>
#include <psapi.h>

namespace cc {

// Once a second: the screen stays on while a live picture is shown in a window that isn't minimized; the PC stays
// awake while it records, shares or keeps a replay buffer. Released the moment none of that is true.
void App::UpdateKeepAwake()
{
    Session* s = Active();
    const bool live = s && s->surface.hasImage && !s->standby && s->video->State() == CaptureState::Running;
    const bool display = m_keepAwake && !m_hidden && !IsIconic(m_hwnd) && (live || (m_advanced && m_program.srv));
    const bool system = m_keepAwake && (m_recorder || m_server || m_replay || !m_extraRecs.empty());
    if (display == m_awakeDisplay && system == m_awakeSystem) return;
    if (!m_powerRequest) {
        REASON_CONTEXT why{POWER_REQUEST_CONTEXT_VERSION, POWER_REQUEST_CONTEXT_SIMPLE_STRING};
        why.Reason.SimpleReasonString = const_cast<LPWSTR>(L"ZeroDisplay is showing or recording a live picture");
        m_powerRequest = PowerCreateRequest(&why);
        if (m_powerRequest == INVALID_HANDLE_VALUE) m_powerRequest = nullptr;
    }
    if (!m_powerRequest) return;
    auto set = [&](POWER_REQUEST_TYPE type, bool on, bool was) {
        if (on == was) return;
        if (on) PowerSetRequest(m_powerRequest, type);
        else PowerClearRequest(m_powerRequest, type);
    };
    set(PowerRequestDisplayRequired, display, m_awakeDisplay);
    set(PowerRequestSystemRequired, system, m_awakeSystem);
    set(PowerRequestExecutionRequired, system, m_awakeSystem);   // also on a laptop whose lid "sleep" is delayed
    LOGI(L"Keep awake: screen %s, PC %s", display ? L"on" : L"may turn off", system ? L"awake" : L"may sleep");
    m_awakeDisplay = display;
    m_awakeSystem = system;
}

void App::SetTimerResolution(bool fine)
{
    if (fine == m_timer1ms) return;
    m_timer1ms = fine;
    if (fine) timeBeginPeriod(1);
    else timeEndPeriod(1);
}

// Checked about once a second (cheap): power source, memory pressure, the settings; applies changes.
void App::UpdateProfile()
{
    const int64_t now = QpcNow();
    if (m_profilePolled && QpcToMs(now - m_profilePolled) < 1000) return;
    m_profilePolled = now;
    UpdateKeepAwake();

    SYSTEM_POWER_STATUS ps{};
    const bool battery = m_cmd.power >= 0 ? m_cmd.power == 1
                                          : (GetSystemPowerStatus(&ps) && ps.ACLineStatus == 0 && ps.BatteryFlag != 128);   // 128: no battery
    if (battery != m_onBattery) {
        m_onBattery = battery;
        m_srRecheck = true;   // NVIDIA (and Intel) switch video super resolution off on battery
        LOGI(L"Power: %s", battery ? L"on battery" : L"plugged in");
    }
    // Low memory: Windows' own signal, or 90 % of memory in use (back to normal below 85 %).
    if (!m_lowMemEvent) m_lowMemEvent = CreateMemoryResourceNotification(LowMemoryResourceNotification);
    BOOL lowSignal = FALSE;
    if (m_lowMemEvent) QueryMemoryResourceNotification(m_lowMemEvent, &lowSignal);
    MEMORYSTATUSEX ms{sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    const bool low = m_cmd.lowMemory || lowSignal || ms.dwMemoryLoad >= (m_lowMemory ? 85u : 90u);
    if (low != m_lowMemory) {
        m_lowMemory = low;
        LOGI(L"Memory: %s (%lu %% in use)", low ? L"low" : L"normal again", ms.dwMemoryLoad);
    }

    PowerProfile p;
    p.battery = m_batterySaver == 2 || (m_batterySaver == 1 && m_onBattery);
    p.memory = m_memorySaver == 2 || (m_memorySaver == 1 && m_lowMemory);
    p.low = m_lowResource || m_cmd.lowResource;
    const bool changed = p.battery != m_profile.battery || p.memory != m_profile.memory || p.low != m_profile.low || !m_profileApplied;
    m_profile = p;
    if (changed) ApplyProfile();
    else if (p.memory && m_advanced && !m_transitioning && ++m_keepOnlyTick % 10 == 0) m_compositor.KeepOnly(ScenesInUse());   // scenes left since

    // While hidden, keep only what's needed: 1 ms timer while something real-time runs.
    SetTimerResolution(!(p.battery || p.low) && !(m_hidden && !m_encoder && !m_shareEncoder));
    if (m_hidden && p.memory && !m_trimmedWhileHidden) {
        m_trimmedWhileHidden = true;
        SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));   // hand back memory Windows can reuse
    } else if (!m_hidden) {
        m_trimmedWhileHidden = false;
    }

    // The preview can't keep up (an older PC): suggest the 30 fps cap / Low-resource mode once.
    if (Session* s = Active(); s && !PreviewCapped() && !m_perfHintShown && !m_hidden) {
        const double capture = s->video->Stats().fps;
        if (capture >= 45 && s->shownFps > 0 && s->shownFps < capture * 0.9) ++m_slowSeconds;
        else m_slowSeconds = 0;
        if (m_slowSeconds >= 10) {
            m_perfHintShown = true;
            Toast(L"The preview can't keep up: try Settings ▸ Video ▸ Limit preview to 30 fps, or Low-resource mode");
            LOGW(L"Preview misses frames (%.1f of %.1f fps): suggested the 30 fps limit", s->shownFps, capture);
        }
    }
}

void App::ApplyProfile()
{
    const bool first = !m_profileApplied;
    m_profileApplied = true;
    const PowerProfile& p = m_profile;
    LOGI(L"Profile: battery saver %s, memory saver %s, low-resource %s", p.battery ? L"on" : L"off", p.memory ? L"on" : L"off", p.low ? L"on" : L"off");
    m_discovery.SetPaused(p.battery || p.low);
    m_uiRefreshMs = p.low ? 1000.0 : 250.0;
    if (!ReplayAllowed() && m_replay) {
        m_replay.reset();
        ReleaseEncoderIfIdle();
    }
    UpdateReplay();
    if (p.low) {
        if (m_multiview) SetMultiview(false);
        if (m_studio) {
            m_studio = false;
            SyncSceneSessions();
        }
    }
    if (p.memory) m_compositor.KeepOnly(ScenesInUse());
    InvalidateUi();
    if (!first || p.battery || p.memory || p.low) {
        const std::wstring s = ProfileSummary();
        if (!s.empty()) Toast(s);
        else if (!first) Toast(L"Power & memory: all features back on");
    }
}

std::wstring App::ProfileSummary() const
{
    const PowerProfile& p = m_profile;
    if (!p.battery && !p.memory && !p.low) return L"";
    std::wstring who = p.low ? L"Low-resource mode" : (p.battery && p.memory ? L"Battery + memory saver" : (p.battery ? L"Battery saver" : L"Memory saver"));
    std::wstring what = L"replay buffer off";
    if (p.battery || p.low) what += L", recording on the display GPU, Enhance off";
    if (p.low) what += L", preview 30 fps, filters / studio / multiview off, sharing 720p ≤ 6 Mbps";
    if (p.memory && !p.low) what += L", unused images freed";
    return who + L": " + what;
}

std::vector<uint32_t> App::ScenesInUse() const
{
    std::vector<uint32_t> ids;
    for (int idx : {m_scenes.program, m_studio ? m_scenes.preview : -1})
        if (idx >= 0 && idx < int(m_scenes.scenes.size()))
            for (const auto& it : m_scenes.scenes[size_t(idx)].items) ids.push_back(it.id);
    return ids;
}

// First start on a weak PC (little memory, few cores, no usable GPU): Low-resource mode, once, with a note.
void App::CheckWeakPc()
{
    if (m_settings.GetBool(L"General", L"WeakPcChecked", false) || m_cmd.selftestSec > 0) return;
    m_settings.SetBool(L"General", L"WeakPcChecked", true);
    m_settingsDirty = true;
    MEMORYSTATUSEX ms{sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    const double gb = ms.ullTotalPhys / 1073741824.0;
    const DWORD cores = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    const bool weak = gb < 3.5 || cores <= 2 || m_renderer.Warp();
    LOGI(L"This PC: %.1f GB, %lu logical processors, %s", gb, cores, m_renderer.Warp() ? L"software rendering" : L"GPU rendering");
    if (weak && !m_lowResource) {
        m_lowResource = true;
        Toast(L"This PC is light on resources: Low-resource mode is on (Settings ▸ General ▸ Power & memory)");
    }
}

// A crash report from last time (core/Crash.cpp writes Crashes\<date>\crash.dmp + log.txt, or crash-<date>.dmp next to
// the settings if that folder can't be made): say where it is, once.
void App::CheckCrashReport()
{
    const std::wstring dir = m_settings.Directory();
    std::wstring newest, newestDate;   // name as reported (folder or file), and its date part for comparing
    auto scan = [&](const std::wstring& pattern, bool folders, size_t datePos) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (bool(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != folders || fd.cFileName[0] == L'.') continue;
            const std::wstring date = std::wstring(fd.cFileName).substr(datePos, 19);
            if (newestDate.empty() || date > newestDate) {
                newestDate = date;
                newest = folders ? std::wstring(L"Crashes\\") + fd.cFileName : std::wstring(fd.cFileName);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    };
    scan(dir + L"\\Crashes\\*", true, 0);
    scan(dir + L"\\crash-*.dmp", false, 6);
    if (newest.empty() || newest == m_settings.GetString(L"General", L"CrashReported", L"")) return;
    m_settings.SetString(L"General", L"CrashReported", newest);
    m_settingsDirty = true;
    LOGW(L"Crash report from last time: %s\\%s", dir.c_str(), newest.c_str());
    if (newest.starts_with(L"Crashes\\")) {
        // The crash handler copies the log; if it couldn't, the crashed run's log is now log.txt.old (this start moved it).
        const std::wstring folder = dir + L"\\" + newest;
        const std::wstring old = dir + L"\\log.txt.old";
        if (GetFileAttributesW((folder + L"\\log.txt").c_str()) == INVALID_FILE_ATTRIBUTES && log::Path() == dir + L"\\log.txt")
            CopyFileW(old.c_str(), (folder + L"\\log.txt").c_str(), TRUE);
        ToastFile(L"ZeroDisplay closed unexpectedly last time. The crash report and its log are in " + newest, folder + L"\\crash.dmp");
    } else {
        ToastFile(L"ZeroDisplay closed unexpectedly last time. A crash report was saved: " + newest, dir + L"\\" + newest);
    }
}

} // namespace cc
