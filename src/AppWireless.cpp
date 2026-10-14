// Wireless streaming: devices cast their screen to this PC (Miracast receiver), shown in the Wireless mode
// tab and usable as a source everywhere (Simple, multiview, Advanced scenes, recording, sharing).
#include "App.h"
#include "core/Log.h"
#include "net/NetReceiver.h"
#include "wireless/CastSource.h"
#include "wireless/CaptivePortal.h"
#include "wireless/Hotspot.h"
#include "wireless/MiracastSink.h"

#include <bcrypt.h>
#include <netfw.h>
#include <shellapi.h>

namespace cc {

static float WirelessPanelWidth(const Ui& u) { return std::min(u.S(380), u.Width() * 0.42f); }

RectF App::WirelessArea() const
{
    const float W = m_ui.Width(), H = m_ui.Height(), barH = BarHeight();
    return {0, barH, std::max(1.0f, W - WirelessPanelWidth(m_ui)), std::max(1.0f, H - barH)};
}

std::wstring App::ReceiverName() const
{
    wchar_t pc[256] = L"PC";
    DWORD n = DWORD(std::size(pc));
    GetComputerNameExW(ComputerNamePhysicalDnsHostname, pc, &n);
    return std::wstring(L"ZeroDisplay (") + pc + L")";
}

Session* App::ActiveCast()
{
    Session* s = Active();
    return s && CastSource::IsCastLink(s->info.symlink) ? s : nullptr;
}

bool App::KeepOpen(const Session& s) const
{
    for (const auto& c : m_casts)
        if (L"miracast:" + c.key == s.info.symlink) return true;
    return HasDeviceWindow(s.info.symlink);   // shown in its own window
}

void App::SetWirelessView(bool on)
{
    if (on == m_wirelessView) return;
    if (on && m_advanced) SetAdvanced(false);
    if (on && m_multiview) SetMultiview(false);
    m_wirelessView = on;
    m_settingsDirty = true;
    if (!m_recorder) {   // switching sources would end a recording
        if (on && !ActiveCast()) {
            // Show a connected cast right away (Simple's device is remembered for the way back).
            for (size_t i = 0; i < m_devices.video.size(); ++i)
                if (!ShownInSimple(m_devices.video[i]) && SessionIndex(m_devices.video[i]) >= 0) {
                    if (Session* a = Active()) m_beforeCast = a->info;
                    SwitchTo(i);
                    break;
                }
        } else if (!on && ActiveCast()) {
            // Back in Simple: the device it showed before (the cast keeps running in the background).
            size_t back = m_devices.video.size();
            for (size_t i = 0; i < m_devices.video.size(); ++i)
                if (m_devices.video[i].symlink == m_beforeCast.symlink) back = i;
            if (back == m_devices.video.size() && !m_devices.video.empty()) back = PickInitialDevice();
            if (back < m_devices.video.size() && ShownInSimple(m_devices.video[back])) SwitchTo(back);
        }
    }
    if (on && !m_receiverOn && m_wireless) {
        m_receiverOn = true;   // opening the tab is asking for it; it stays on until turned off
        LOGI(L"Wireless: receiver turned on");
    }
}

// The master switch. Off stops everything that uses the network right away (the receiver and the own Wi-Fi
// follow in UpdateWireless, in the background); on brings back what the settings ask for.
void App::SetWireless(bool on)
{
    if (on == m_wireless) return;
    m_wireless = on;
    m_settingsDirty = true;
    LOGI(L"Wireless features %s", on ? L"on" : L"off");
    if (on) {
        m_discovery.Start();
        if (m_cmd.share || m_shareOnStart) StartSharing();
        ApplyDevices(m_localDevices, false);
        Toast(L"Wireless features on");
        return;
    }
    // Say exactly what stopped.
    std::vector<std::wstring> stopped;
    if (!m_casts.empty()) stopped.push_back(L"casting");
    else if (m_miracast || m_receiverStart.valid()) stopped.push_back(L"the cast receiver");
    if (m_server) stopped.push_back(L"sharing");
    if (m_hotspot) stopped.push_back(L"this PC's Wi-Fi network");
    std::wstring what;
    for (size_t i = 0; i < stopped.size(); ++i) what += (i == 0 ? L"" : (i + 1 == stopped.size() ? L" and " : L", ")) + stopped[i];
    StopSharing();
    m_discovery.Stop();
    // Network streams and casts close now; Simple mode goes back to a device on this PC.
    bool closedActive = false, closed = false;
    for (size_t i = m_sessions.size(); i-- > 0;) {
        const std::wstring& link = m_sessions[i]->info.symlink;
        if (!NetReceiver::IsNetworkLink(link) && !CastSource::IsCastLink(link)) continue;
        closedActive |= i == m_active;
        closed = true;
        CloseSession(i);
    }
    m_manualNet.clear();
    ApplyDevices(m_localDevices, false);
    if (m_advanced) SyncSceneSessions();
    else if (closedActive && !m_multiview && !m_devices.video.empty()) SwitchTo(PickInitialDevice());
    if (what.empty() && closed) what = L"the network source";
    Toast(what.empty() ? std::wstring(L"Wireless features off") : L"Wireless features off: " + what + L" stopped");
}

// Every frame: the receiver follows the setting (started in the background, it takes about a second),
// and connection events become sources.
void App::UpdateWireless()
{
    // Discovery looks often only while the device list may be looked at (menu or Settings open, first 10 s).
    m_discovery.SetActive(m_ui.PopupOpen() || m_settingsOpen || QpcElapsedMs(m_startQpc) < 10000.0);
    UpdateHotspot();
    const bool want = (m_receiverOn || m_cmd.wireless) && m_wireless;
    const bool hsWanted = m_hotspotOn && m_wireless;
    if (m_receiverStart.valid() && m_receiverStart.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        m_miracast = m_receiverStart.get();
        m_receiverPinUsed = m_receiverStartPin;
        m_receiverUnderHotspot = m_receiverStartHotspot;
    }
    if (m_receiverStopping.valid() && m_receiverStopping.wait_for(std::chrono::seconds(0)) == std::future_status::ready) m_receiverStopping.get();
    const bool hotspotBusy = m_hotspotStopping.valid() || (hsWanted && (!m_hotspot || m_hotspotStart.valid()));
    if (want && !m_miracast && !m_receiverStart.valid() && !m_receiverStopping.valid() && !hotspotBusy) {
        const std::wstring name = ReceiverName();
        const bool pin = m_castPin;
        m_receiverStartPin = pin;
        m_receiverStartHotspot = hsWanted;
        m_receiverStart = std::async(std::launch::async, [name, pin] {
            ComInit com;
            auto r = std::make_unique<MiracastSink>();
            r->Start(name, pin);
            return r;
        });
        return;
    }
    // Off, the PIN setting changed, or the app's own network comes or goes: restart.
    if (m_miracast && (!want || m_receiverPinUsed != m_castPin || m_receiverUnderHotspot != hsWanted || m_receiverRestart)) {
        m_receiverRestart = false;
        m_receiverStopping = std::async(std::launch::async, [r = std::move(m_miracast)]() mutable {
            ComInit com;
            r->Stop();
        });
        for (const auto& c : m_casts) CastEnded(c.key, c.name, false);
        m_casts.clear();
        ApplyDevices(m_localDevices, false);
        return;
    }
    for (const auto& s : m_sessions) {
        if (!CastSource::IsCastLink(s->info.symlink)) continue;
        const CaptureState st = s->video->State();
        if (st == CaptureState::Running) m_castFirewallBlocked = false;
        else if (st == CaptureState::Error && s->video->StatusText().find(L"Firewall") != std::wstring::npos) m_castFirewallBlocked = true;
    }
    if (!m_miracast) return;
    for (auto& e : m_miracast->TakeEvents()) {
        const std::wstring link = L"miracast:" + e.key;
        switch (e.type) {
        case MiracastSink::Event::Type::Connected:
            if (!e.pin.empty()) {
                m_castPinShown = e.pin;
                Toast(L"PIN for " + e.name + L": " + e.pin);
            }
            break;
        case MiracastSink::Event::Type::Media: {
            auto it = std::find_if(m_casts.begin(), m_casts.end(), [&](const Cast& c) { return c.key == e.key; });
            if (it == m_casts.end()) m_casts.push_back({e.key, e.name, e.source});
            else it->source = e.source;
            ApplyDevices(m_localDevices, false);
            // A reconnect brings a new stream: the same source plays it.
            for (auto& sp : m_sessions)
                if (sp->info.symlink == link)
                    if (auto* c = dynamic_cast<CastSource*>(sp->video.get())) c->Reconnect(e.source.Get(), sp->info);
            // The stream is read from now on (an unread cast is dropped by the device after ~30 s). The Wireless tab
            // shows it right away, unless that would end a recording (switching sources stops it); Simple and
            // Advanced keep what they show (casts live in the Wireless tab and in Advanced scenes).
            const bool takeOver = m_wirelessView && !m_recorder;
            if (takeOver)
                if (Session* a = Active(); a && !CastSource::IsCastLink(a->info.symlink)) m_beforeCast = a->info;
            for (size_t i = 0; i < m_devices.video.size(); ++i)
                if (m_devices.video[i].symlink == link) {
                    if (takeOver) SwitchTo(i);
                    else if (SessionIndex(m_devices.video[i]) < 0) OpenSession(m_devices.video[i]);
                }
            m_castPinShown.clear();
            Toast(e.name + (takeOver || m_advanced ? L" is casting"
                            : m_wirelessView ? L" is casting, Show switches to it (ends the recording)"
                                             : L" is casting: open the Wireless tab to watch it"));
            break;
        }
        case MiracastSink::Event::Type::Disconnected:
            CastEnded(e.key, e.name, true);
            break;
        }
    }
}

void App::CastEnded(const std::wstring& key, const std::wstring& name, bool toast)
{
    const std::wstring link = L"miracast:" + key;
    for (size_t i = 0; i < m_sessions.size(); ++i) {
        Session& s = *m_sessions[i];
        if (s.info.symlink != link) continue;
        if (!m_advanced && !m_multiview && Active() != &s) {
            CloseSession(i);   // was open in the background only
        } else if (s.video->LastFrameQpc() == 0) {
            // Never showed a picture: nothing to keep on screen; go back to what was shown before.
            const bool wasActive = Active() == &s;
            CloseSession(i);
            if (wasActive && !m_advanced && !m_beforeCast.symlink.empty())
                for (size_t d = 0; d < m_devices.video.size(); ++d)
                    if (m_devices.video[d].symlink == m_beforeCast.symlink) SwitchTo(d);
        } else if (auto* c = dynamic_cast<CastSource*>(s.video.get())) {
            c->Ended();   // standby keeps the last picture
        }
        break;
    }
    m_casts.erase(std::remove_if(m_casts.begin(), m_casts.end(), [&](const Cast& c) { return c.key == key; }), m_casts.end());
    ApplyDevices(m_localDevices, false);
    if (toast && !name.empty()) Toast(name + L" stopped casting");
}

ComPtr<IUnknown> App::CastMediaSource(const std::wstring& link) const
{
    for (const auto& c : m_casts)
        if (L"miracast:" + c.key == link) return c.source;
    return nullptr;
}

// Windows Firewall lets the cast's video in only on networks where ZeroDisplay is allowed; Wi-Fi Direct
// links count as public networks. The user approves this rule in Windows' own administrator prompt.
void App::AllowCastingThroughFirewall()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // One administrator prompt for both: the rule from before the rename (it names the old CaptureCard.exe) goes,
    // the new one comes. cmd /s keeps the inner quotes; its exit code is the "add" one.
    const std::wstring args = std::wstring(L"/s /c \"netsh advfirewall firewall delete rule name=\"CaptureCard - casting\" >nul 2>&1 & "
                                           L"netsh advfirewall firewall add rule name=\"ZeroDisplay - casting\" dir=in action=allow program=\"") +
                              exe + L"\" profile=public interfacetype=wireless enable=yes\"";
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = m_hwnd;
    sei.lpVerb = L"runas";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (ShellExecuteExW(&sei)) {
        if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 15000);
            DWORD code = 1;
            GetExitCodeProcess(sei.hProcess, &code);
            CloseHandle(sei.hProcess);
            Toast(code == 0 ? L"Casting allowed through Windows Firewall, cast again from your device" : L"Windows Firewall wasn't changed");
            LOGI(L"Wireless: firewall rule for casting: exit code %lu", code);
        }
    } else {
        Toast(L"Windows Firewall wasn't changed");
    }
}

bool App::CastBlockedByFirewall() const { return m_castFirewallBlocked; }

// Read-only look at Windows Firewall: is there an enabled inbound rule allowing this exe on public networks
// (Wi-Fi Direct links - casts and the app's own network - count as public)? Off the render thread.
void App::CheckFirewall()
{
    if (m_fwCheck.valid()) return;
    m_fwCheck = std::async(std::launch::async, [] {
        EfficiencyScope eco;
        ComInit com;
        ComPtr<INetFwPolicy2> policy;
        if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&policy)))) return true;
        VARIANT_BOOL enabled = VARIANT_TRUE;
        if (SUCCEEDED(policy->get_FirewallEnabled(NET_FW_PROFILE2_PUBLIC, &enabled)) && !enabled) return true;
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        ComPtr<INetFwRules> rules;
        ComPtr<IUnknown> unk;
        ComPtr<IEnumVARIANT> it;
        if (FAILED(policy->get_Rules(&rules)) || FAILED(rules->get__NewEnum(&unk)) || FAILED(unk.As(&it))) return true;
        bool allowed = false, blocked = false;
        VARIANT v;
        VariantInit(&v);
        ULONG got = 0;
        while (it->Next(1, &v, &got) == S_OK && got == 1) {
            ComPtr<INetFwRule> rule;
            if (v.vt == VT_DISPATCH && v.pdispVal) v.pdispVal->QueryInterface(IID_PPV_ARGS(&rule));
            VariantClear(&v);
            if (!rule) continue;
            BSTR app = nullptr;
            VARIANT_BOOL on = VARIANT_FALSE;
            long profiles = 0;
            NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_OUT;
            NET_FW_ACTION action = NET_FW_ACTION_BLOCK;
            rule->get_ApplicationName(&app);
            const bool ours = app && _wcsicmp(app, exe) == 0;
            SysFreeString(app);
            if (!ours) continue;
            rule->get_Enabled(&on);
            rule->get_Profiles(&profiles);
            rule->get_Direction(&dir);
            rule->get_Action(&action);
            if (!on || dir != NET_FW_RULE_DIR_IN || !(profiles & NET_FW_PROFILE2_PUBLIC)) continue;
            (action == NET_FW_ACTION_ALLOW ? allowed : blocked) = true;
        }
        return allowed && !blocked;
    });
}

std::wstring App::ViewerUrl(bool hotspot) const
{
    if (!m_web) return L"";
    std::wstring ip;
    if (hotspot) {
        ip = m_hotspotAddr;
    } else {
        if (m_lanIp.empty() || QpcElapsedMs(m_lanIpAt) > 5000) {
            m_lanIpAt = QpcNow();
            m_lanIp.clear();
            for (const auto& a : net::LocalIPv4())
                if (a != m_hotspotAddr) {
                    m_lanIp = a;
                    break;
                }
        }
        ip = m_lanIp;
    }
    return ip.empty() ? L"" : Format(L"http://%s:%u/", ip.c_str(), m_web->Port());
}

const QrCode& App::QrCache::For(const std::string& t)
{
    if (t != text) {
        text = t;
        code = QrCode::Encode(t);
    }
    return code;
}

static std::wstring RandomText(const wchar_t* alphabet, size_t length)
{
    const size_t n = wcslen(alphabet);
    uint8_t rnd[64] = {};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, rnd, sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        // Should never happen; still never hand out a predictable all-zero password.
        LOGW(L"Random numbers unavailable: using a time-based fallback");
        uint64_t x = uint64_t(QpcNow()) ^ (uint64_t(GetTickCount64()) << 21) ^ uint64_t(GetCurrentProcessId());
        for (auto& b : rnd) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            b = uint8_t(x);
        }
    }
    std::wstring s;
    for (size_t i = 0; i < length && i < sizeof(rnd); ++i) s += alphabet[rnd[i] % n];
    return s;
}

// The app's own Wi-Fi network follows the setting; started and stopped in the background (a few seconds).
void App::UpdateHotspot()
{
    if (m_hotspotSsid.empty() || m_hotspotPass.empty()) {
        if (m_hotspotSsid.empty()) m_hotspotSsid = L"ZeroDisplay-" + RandomText(L"0123456789ABCDEF", 4);
        if (m_hotspotPass.empty()) m_hotspotPass = RandomText(L"abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789", 12);
        m_settingsDirty = true;
    }
    if (m_fwCheck.valid() && m_fwCheck.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        m_fwPublic = m_fwCheck.get() ? 1 : 0;
        LOGI(L"Wireless: Windows Firewall %s ZeroDisplay on public networks", m_fwPublic ? L"allows" : L"doesn't allow");
    }
    if ((m_wirelessView || m_hotspotOn) && m_wireless && m_fwPublic < 0) CheckFirewall();

    const bool want = m_hotspotOn && m_wireless;
    if (m_hotspotStart.valid()) {
        if (m_hotspotStart.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        if (m_hotspotStart.get()) {
            Toast(L"Wi-Fi network “" + m_hotspot->Ssid() + L"” is on");
            CheckFirewall();
        } else {
            Toast(m_hotspot->Error());
        }
        m_hotspotPolled = 0;
    }
    if (m_hotspotStopping.valid()) {
        if (m_hotspotStopping.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        m_hotspotStopping.get();
    }
    // The receiver steps aside first (UpdateWireless), then comes back next to the network.
    if (want && !m_hotspot && (m_miracast || m_receiverStart.valid() || m_receiverStopping.valid())) return;
    if (want && !m_hotspot) {
        m_hotspot = std::make_unique<Hotspot>();
        m_portalTried.clear();
        m_hotspotStart = std::async(std::launch::async, [h = m_hotspot.get(), ssid = m_hotspotSsid, pass = m_hotspotPass] {
            ComInit com;
            return h->Start(ssid, pass, true);
        });
        return;
    }
    if (!want && m_hotspot) {
        StopHotspot(false);
        return;
    }
    if (!m_hotspot || m_hotspot->Active() == Hotspot::Kind::None) return;
    if (m_hotspotPolled && QpcElapsedMs(m_hotspotPolled) < 1000) return;
    m_hotspotPolled = QpcNow();
    m_hotspotAddr = m_hotspot->Address();
    m_hotspotClients = m_hotspot->Clients();
    if (m_portal && m_portal->Address() != m_hotspotAddr) m_portal.reset();
    // The welcome page needs its own DNS answers; Mobile Hotspot has Windows' own DNS, so it gets only the QR codes.
    if (!m_portal && !m_hotspotAddr.empty() && m_hotspotAddr != m_portalTried && m_hotspot->Active() == Hotspot::Kind::WiFiDirect) {
        m_portalTried = m_hotspotAddr;
        m_portal = std::make_unique<CaptivePortal>();
        if (!m_portal->Start(m_hotspotAddr)) m_portal.reset();
    }
    if (m_portal) m_portal->SetInfo(ReceiverName(), ViewerUrl(true));
}

void App::StopHotspot(bool wait)
{
    m_portal.reset();
    m_hotspotAddr.clear();
    m_hotspotClients = 0;
    if (wait && m_hotspotStopping.valid()) m_hotspotStopping.wait();
    if (!m_hotspot) return;
    if (m_miracast) m_receiverRestart = true;   // it may have been paused by the network
    auto job = [h = std::move(m_hotspot), f = std::move(m_hotspotStart)]() mutable {
        if (f.valid()) f.wait();
        ComInit com;
        h->Stop();
        h.reset();
    };
    if (wait) {
        if (m_hotspotStopping.valid()) m_hotspotStopping.wait();
        job();
    } else {
        m_hotspotStopping = std::async(std::launch::async, std::move(job));
    }
}

void App::DrawWireless(const UiInput& in)
{
    Ui& u = m_ui;
    const RectF area = WirelessArea();
    const float W = u.Width(), H = u.Height(), barH = BarHeight(), pw = WirelessPanelWidth(u);

    // Left: the cast, or how to cast.
    if (Session* cast = ActiveCast()) {
        DrawStatus(cast, area, true);
        const std::wstring label = cast->info.name.substr(cast->info.name.find(L": ") != std::wstring::npos ? cast->info.name.find(L": ") + 2 : 0);
        const float lw = u.Measure(label, u.S(12)) + u.S(16);
        const RectF l{area.x + u.S(10), area.Bottom() - u.S(34), std::min(lw, area.w - u.S(20)), u.S(24)};
        u.Fill(l, theme::Bar, u.S(5));
        u.Text(label, l.Inset(u.S(8), 0), u.S(12), theme::Text);
        if (in.doubleClick && u.Hot(area)) ToggleFullscreen();
    } else {
        u.Fill(area, theme::Panel.A(1.0f));   // an info page, not a picture: it follows light / dark
        const float cw = std::min(area.w - u.S(48), u.S(620));
        float y = area.y + std::max(u.S(40), area.h * 0.16f);
        const float x = area.x + (area.w - cw) * 0.5f;
        u.Text(L"Cast to this PC", {x, y, cw, u.S(40)}, u.S(26), theme::Text, Align::Left, true);
        y += u.S(48);
        const std::wstring state = m_miracast && m_miracast->Running() ? m_miracast->StatusText() : L"";
        const bool ready = state == L"ready" || state == L"connected";
        const wchar_t* why = !m_wireless                                               ? L"Wireless features are off: turn them on at the top of the panel on the right."
                             : ready                                                   ? L"Your phone, tablet or laptop shows this PC as:"
                             : (m_receiverStart.valid() || m_receiverStopping.valid()) ? L"Starting the receiver…"
                             : (m_miracast && m_receiverUnderHotspot)                  ? L"Screen mirroring (Smart View / Win+K) is paused while this PC's own Wi-Fi network is on. The ZeroDisplay app for Android still casts over it."
                             : m_miracast                                              ? L"The receiver couldn't start, see the panel on the right."
                                                                                       : L"The receiver is off, turn it on in the panel on the right.";
        u.TextBlock(why, {x, y, cw, u.S(24)}, u.S(14), theme::TextDim);
        y += u.S(ready ? 28.0f : 48.0f);
        if (ready) {
            u.Text(ReceiverName(), {x, y, cw, u.S(34)}, u.S(20), theme::Accent2, Align::Left, true);
            y += u.S(50);
        }
        struct Step {
            const wchar_t* who;
            const wchar_t* how;
        };
        static const Step steps[] = {
            {L"Android (Samsung, Xiaomi, OnePlus, Motorola, …)", L"Open Quick Settings ▸ Smart View / Cast / Screen mirroring ▸ pick this PC."},
            {L"Windows PC or laptop", L"Press Win+K ▸ pick this PC."},
            {L"iPhone, iPad, Google Pixel", L"They only cast with AirPlay / Google Cast, which Windows can't receive: use a USB-C or Lightning → HDMI adapter into the capture card."},
        };
        for (const auto& s : steps) {
            u.Text(s.who, {x, y, cw, u.S(22)}, u.S(13.5f), theme::Text, Align::Left, true);
            y += u.S(24);
            u.TextBlock(s.how, {x, y, cw, u.S(40)}, u.S(13), theme::TextDim);
            y += u.S(48);
        }
        if (!m_castPinShown.empty()) {
            u.Text(L"PIN: " + m_castPinShown, {x, y, cw, u.S(44)}, u.S(30), theme::Accent2, Align::Left, true);
            y += u.S(50);
        }
    }

    // Right: the panel.
    RectF panel{W - pw, barH, pw, H - barH};
    u.Fill(panel, theme::Panel);
    u.Fill({panel.x, panel.y, 1, panel.h}, theme::Border);
    u.PushClip(panel);
    const float cx = panel.x + u.S(16), cw = pw - u.S(32), rowH = u.S(32);
    float y = panel.y + u.S(14) - m_wirelessScroll;
    auto header = [&](const char* id, const wchar_t* t) { return PanelSection(id, t, y, cx, cw); };
    const float qrSide = std::min(cw, u.S(176));
    {   // the master switch (the same as Settings ▸ General ▸ Wireless features)
        bool wl = m_wireless;
        if (u.Toggle("wlmaster", {cx, y, cw, rowH}, L"Wireless features", wl)) SetWireless(wl);
        y += rowH;
        if (!m_wireless) {
            u.TextBlock(L"Off: nothing below runs and nothing listens on the network. Turn it on to receive casts, share or create a "
                        L"Wi-Fi network.",
                        {cx, y + u.S(2), cw, u.S(36)}, u.S(11.5f), theme::Warn);
            y += u.S(40);
        }
        y += u.S(10);
    }
    const bool off = !m_wireless;
    if (header("w.receive", L"Receive casts")) {
        // While everything wireless is off, the toggles show what runs (nothing); the saved choices come back with it.
        bool receiverShown = m_receiverOn && !off;
        if (u.Toggle("receiver", {cx, y, cw, rowH}, L"Let devices cast to this PC", receiverShown, off)) {
            m_receiverOn = receiverShown;
            m_settingsDirty = true;
        }
        y += rowH;
        {
            std::wstring st;
            if (off) st = L"Off: wireless features are off.";
            else if (!m_receiverOn && !m_cmd.wireless) st = L"Off. Nothing is advertised over Wi-Fi.";
            else if (m_receiverStart.valid()) st = L"Starting…";
            else if (m_receiverStopping.valid() || (!m_miracast && m_hotspotStart.valid())) st = L"Waiting for this PC's own Wi-Fi to start…";
            else if (m_miracast && !m_miracast->Running() && m_receiverUnderHotspot)   // it couldn't start beside the network
                st = L"Paused while this PC's own Wi-Fi network is on: this PC's Wi-Fi can't do both at once.";
            else if (m_miracast && m_miracast->Running()) {
                std::wstring state = m_miracast->StatusText();
                if (m_receiverUnderHotspot && state != L"ready" && state != L"connected")
                    st = L"Paused while this PC's own Wi-Fi network is on: this PC's Wi-Fi can't do both at once.";
                else {
                    if (!state.empty()) state[0] = wchar_t(towupper(state[0]));
                    st = state + L", devices see this PC as " + ReceiverName();
                }
            }
            else st = L"Couldn't start: this PC's Wi-Fi may not support receiving casts (Miracast).";
            u.TextBlock(st, {cx, y + u.S(2), cw, u.S(36)}, u.S(11.5f), theme::TextDim);
            y += u.S(40);
        }
        if (u.Toggle("castpin", {cx, y, cw, rowH}, L"Ask for a PIN", m_castPin, off)) m_settingsDirty = true;
        y += rowH + u.S(10);

        // Casts and phones on the app's own network arrive over Wi-Fi Direct, which Windows treats as a public network.
        const bool fwNeeded = !off && m_fwPublic == 0 && (m_receiverOn || m_cmd.wireless || m_hotspot);
        if (CastBlockedByFirewall() || fwNeeded) {
            u.TextBlock(CastBlockedByFirewall() ? L"A device connected but no picture arrived: Windows Firewall blocks it on the Wi-Fi Direct link. Allow it once "
                                                  L"(Windows asks for permission):"
                                                : L"Windows Firewall will block casts and phones on this PC's own Wi-Fi until you allow ZeroDisplay "
                                                  L"once (Windows asks for permission):",
                        {cx, y, cw, u.S(54)}, u.S(12), theme::Warn);
            y += u.S(58);
            if (u.Button("fwallow", {cx, y, cw, rowH}, L"Allow through Windows Firewall", 0, true)) {
                AllowCastingThroughFirewall();
                m_fwPublic = -1;
                CheckFirewall();
            }
            y += rowH + u.S(12);
        }
    }
    if (header("w.connected", L"Connected")) {
        if (m_casts.empty()) {
            u.Text(L"Nothing is casting.", {cx, y, cw, u.S(22)}, u.S(12.5f), theme::TextDim);
            y += u.S(30);
        }
        for (const auto& c : m_casts) {
            const std::wstring link = L"miracast:" + c.key;
            Session* s = nullptr;
            for (auto& sp : m_sessions)
                if (sp->info.symlink == link) s = sp.get();
            u.Text(c.name, {cx, y, cw, u.S(22)}, u.S(13.5f), theme::Text, Align::Left, true);
            y += u.S(22);
            std::wstring info = s ? s->video->StatusText() : L"";
            if (s && s->video->State() == CaptureState::Running) info += Format(L" · %.0f fps", s->video->Stats().fps);
            u.Text(info, {cx, y, cw, u.S(20)}, u.S(11.5f), theme::TextDim);
            y += u.S(24);
            const float bw = (cw - u.S(8)) / 2;
            const std::string idShow = "castshow" + Narrow(c.key), idDrop = "castdrop" + Narrow(c.key);
            if (u.Button(idShow.c_str(), {cx, y, bw, rowH}, L"Show"))
                for (size_t i = 0; i < m_devices.video.size(); ++i)
                    if (m_devices.video[i].symlink == link) SwitchTo(i);
            if (u.Button(idDrop.c_str(), {cx + bw + u.S(8), y, bw, rowH}, L"Disconnect") && m_miracast) m_miracast->Disconnect(c.key);
            y += rowH + u.S(12);
        }
        u.TextBlock(L"Casts are sources like the capture card: record them" + KeyHint(0) + L", share them, or add them to Advanced scenes.",
                    {cx, y, cw, u.S(36)}, u.S(11.5f), theme::TextDim);
        y += u.S(46);
    }
    if (header("w.watch", L"Watch on phones and tablets")) {
        {
            bool sharing = m_server != nullptr;
            if (u.Toggle("wsharing", {cx, y, cw, rowH}, L"Share on the local network", sharing, off)) ToggleSharing();
            y += rowH;
            const std::wstring lan = ViewerUrl(false), own = m_hotspot && m_hotspot->Active() != Hotspot::Kind::None ? ViewerUrl(true) : L"";
            if (!m_web) {
                u.TextBlock(off ? L"Off: wireless features are off." : L"Turn sharing on, then scan the code with a phone's camera to watch in its browser.", {cx, y + u.S(4), cw, u.S(36)}, u.S(11.5f),
                            theme::TextDim);
                y += u.S(44);
            } else {
                const std::wstring url = !lan.empty() ? lan : own;
                // The code carries the sharing PIN (whoever scans it stands at this PC); the address shown doesn't.
                const std::string pin = m_pinGate.Required() ? m_pinGate.Pin() : std::string();
                u.Qr(m_qrViewer.For(Narrow(url) + (pin.empty() ? "" : "#pin=" + pin)), {cx, y + u.S(6), qrSide, qrSide});
                y += qrSide + u.S(12);
                u.Text(url, {cx, y, cw, u.S(20)}, u.S(12.5f), theme::Text, Align::Left, true);
                y += u.S(22);
                std::wstring info = Format(L"%d watching. Scan with the phone's camera (same Wi-Fi as this PC).", m_web->Viewers());
                if (!pin.empty()) info += L"\nTyping the address instead? The PIN is " + Widen(pin) + L".";
                if (!own.empty() && own != url) info += L"\nOn " + m_hotspot->Ssid() + L": " + own;
                const float ih = u.MeasureBlock(info, cw, u.S(11.5f));
                u.TextBlock(info, {cx, y, cw, ih + u.S(2)}, u.S(11.5f), theme::TextDim);
                y += ih + u.S(10);
            }
        }
    }
    if (header("w.ownwifi", L"This PC's own Wi-Fi")) {
        {
            bool hotspotShown = m_hotspotOn && !off;
            if (u.Toggle("hotspot", {cx, y, cw, rowH}, L"Create a Wi-Fi network for phones", hotspotShown, off)) {
                m_hotspotOn = hotspotShown;
                m_settingsDirty = true;
            }
            y += rowH;
            const Hotspot::Kind kind = m_hotspot ? m_hotspot->Active() : Hotspot::Kind::None;
            const bool busy = m_hotspotStart.valid() || m_hotspotStopping.valid();
            std::wstring st;
            if (off) st = L"Off: wireless features are off.";
            else if (!m_hotspotOn) st = L"For places without Wi-Fi: phones join this PC directly. Off: nothing is broadcast.";
            else if (busy) st = L"Starting…";
            else if (kind == Hotspot::Kind::None) st = m_hotspot ? m_hotspot->Error() : L"";
            else {
                // Devices the PC has heard lately: its address table, and (own network) who used the DNS / welcome page.
                const int seen = std::max(m_hotspotClients, m_portal ? m_portal->RecentDevices(120.0) : 0);
                st = Format(L"On%s, %d device(s) seen in the last 2 minutes",
                            kind == Hotspot::Kind::MobileHotspot ? L" (Windows Mobile Hotspot, shares this PC's internet)" : L"", seen);
                if (m_portal) st += Format(L", welcome page opened %d time(s)", m_portal->Visitors());
            }
            u.TextBlock(st, {cx, y + u.S(2), cw, u.S(36)}, u.S(11.5f), kind == Hotspot::Kind::None && m_hotspot && !busy ? theme::Warn : theme::TextDim);
            y += u.S(40);
            // Name and password: applied when the box is left (restarts the network if it is on).
            if (m_hsEditSsid.empty()) m_hsEditSsid = m_hotspotSsid;
            if (m_hsEditPass.empty()) m_hsEditPass = m_hotspotPass;
            const float lw = u.S(76);
            u.Text(L"Name", {cx, y, lw, rowH}, u.S(12.5f), theme::TextDim);
            const bool ssidDone = u.TextBox("hsssid", {cx + lw, y, cw - lw, rowH}, m_hsEditSsid, L"Network name");
            y += rowH + u.S(6);
            u.Text(L"Password", {cx, y, lw, rowH}, u.S(12.5f), theme::TextDim);
            // Hidden by default (the screen may be streamed or recorded): dots, and the join code below, until "Show".
            const float showW = u.S(64);
            const bool passDone = u.TextBox("hspass", {cx + lw, y, cw - lw - showW - u.S(6), rowH}, m_hsEditPass, L"8-63 characters", !m_showHsPass);
            if (u.Button("hsshow", {cx + cw - showW, y, showW, rowH}, m_showHsPass ? L"Hide" : L"Show")) m_showHsPass = !m_showHsPass;
            y += rowH + u.S(8);
            if (ssidDone || passDone) {
                const std::string ns = Narrow(m_hsEditSsid);
                bool asciiPass = true;
                for (wchar_t ch : m_hsEditPass) asciiPass &= ch >= 32 && ch < 127;
                if (ns.empty() || ns.size() > 32) {
                    Toast(L"The network name must be 1-32 characters");
                    m_hsEditSsid = m_hotspotSsid;
                } else if (m_hsEditPass.size() < 8 || m_hsEditPass.size() > 63 || !asciiPass) {
                    Toast(L"The password must be 8-63 letters, digits or symbols");
                    m_hsEditPass = m_hotspotPass;
                } else if (m_hsEditSsid != m_hotspotSsid || m_hsEditPass != m_hotspotPass) {
                    m_hotspotSsid = m_hsEditSsid;
                    m_hotspotPass = m_hsEditPass;
                    m_settingsDirty = true;
                    if (m_hotspot) StopHotspot(false);   // comes back with the new name / password
                }
            }
            if (kind != Hotspot::Kind::None) {
                // Wi-Fi join code (phone cameras offer "Join network"); special characters escaped as the format asks.
                auto esc = [](const std::wstring& v) {
                    std::wstring o;
                    for (wchar_t ch : v) {
                        if (ch == L'\\' || ch == L';' || ch == L',' || ch == L':' || ch == L'"') o += L'\\';
                        o += ch;
                    }
                    return o;
                };
                const std::wstring join = L"WIFI:S:" + esc(m_hotspot->Ssid()) + L";T:WPA;P:" + esc(m_hotspot->Passphrase()) + L";;";
                // 1: join the network, 2: open the welcome page there (Wi-Fi Direct gives phones no DNS server, so
                // they can't find it by themselves). Without the page (Mobile Hotspot), 2 is the viewer itself.
                const std::wstring open = m_portal ? L"http://" + m_hotspotAddr + L"/" : ViewerUrl(true);
                const float side = open.empty() ? qrSide : std::min(qrSide, (cw - u.S(12)) / 2);
                u.Text(L"1  Join", {cx, y, side, u.S(20)}, u.S(12), theme::Text, Align::Left, true);
                if (!open.empty()) u.Text(L"2  Open", {cx + side + u.S(12), y, side, u.S(20)}, u.S(12), theme::Text, Align::Left, true);
                y += u.S(22);
                if (m_showHsPass) {
                    u.Qr(m_qrWifi.For(Narrow(join)), {cx, y, side, side});
                } else {   // the join code contains the password: shown with it
                    const RectF box{cx, y, side, side};
                    if (u.Button("hsshowqr", box, L"")) m_showHsPass = true;
                    u.TextBlock(L"Hidden: it contains the password. Click to show.", box.Inset(u.S(12)), u.S(12), theme::TextDim);
                }
                if (!open.empty()) u.Qr(m_qrWelcome.For(Narrow(open)), {cx + side + u.S(12), y, side, side});
                y += side + u.S(8);
                u.TextBlock(L"Scan 1 with the phone's camera to join, then scan 2" + (open.empty() ? std::wstring() : L" (" + open + L")") +
                                L". Phones on this network have no internet.",
                            {cx, y, cw, u.S(52)}, u.S(11.5f), theme::TextDim);
                y += u.S(56);
            }
        }
    }
    const float content = y + m_wirelessScroll - panel.y;
    const float maxScroll = std::max(0.0f, content - panel.h + u.S(20));
    if (u.Hot(panel) && in.wheel != 0 && !u.PopupOpen()) m_wirelessScroll -= in.wheel * u.S(60);
    m_wirelessScroll = Clamp(m_wirelessScroll, 0.0f, maxScroll);
    u.PopClip();
}

} // namespace cc
