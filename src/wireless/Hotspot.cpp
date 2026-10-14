#include "wireless/Hotspot.h"
#include "core/Log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <winrt/Windows.Devices.WiFiDirect.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.NetworkOperators.h>
#include <winrt/Windows.Security.Credentials.h>

#include <mutex>
#include <vector>

namespace cc {

namespace wfd = winrt::Windows::Devices::WiFiDirect;
namespace wno = winrt::Windows::Networking::NetworkOperators;

struct Hotspot::Impl {
    mutable std::mutex mutex;
    Kind kind = Kind::None;
    std::wstring ssid, pass, error;
    wfd::WiFiDirectAdvertisementPublisher publisher{nullptr};
    wno::NetworkOperatorTetheringManager tethering{nullptr};
    wno::NetworkOperatorTetheringAccessPointConfiguration previous{nullptr};   // Mobile Hotspot settings to restore
    winrt::guid tetherAdapter{};
};

Hotspot::Hotspot() : m(std::make_unique<Impl>()) {}
Hotspot::~Hotspot() { Stop(); }

bool Hotspot::Start(const std::wstring& ssid, const std::wstring& passphrase, bool allowMobileHotspot)
{
    Stop();
    {
        std::lock_guard lock(m->mutex);
        m->ssid = ssid;
        m->pass = passphrase;
        m->error.clear();
    }
    // 1) Wi-Fi Direct, legacy mode: phones see a normal WPA2 network.
    try {
        wfd::WiFiDirectAdvertisementPublisher pub;
        auto adv = pub.Advertisement();
        adv.IsAutonomousGroupOwnerEnabled(true);
        auto legacy = adv.LegacySettings();
        legacy.IsEnabled(true);
        legacy.Ssid(ssid);
        winrt::Windows::Security::Credentials::PasswordCredential cred;
        cred.Password(passphrase);
        legacy.Passphrase(cred);
        HANDLE started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!started) throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()), L"no event");
        auto status = std::make_shared<std::atomic<int>>(-1);
        auto err = std::make_shared<std::atomic<int>>(0);
        pub.StatusChanged([started, status, err](const wfd::WiFiDirectAdvertisementPublisher&,
                                                  const wfd::WiFiDirectAdvertisementPublisherStatusChangedEventArgs& a) {
            *status = int(a.Status());
            *err = int(a.Error());
            if (a.Status() == wfd::WiFiDirectAdvertisementPublisherStatus::Started ||
                a.Status() == wfd::WiFiDirectAdvertisementPublisherStatus::Aborted)
                SetEvent(started);
        });
        pub.Start();
        WaitForSingleObject(started, 5000);
        CloseHandle(started);
        if (*status == int(wfd::WiFiDirectAdvertisementPublisherStatus::Started)) {
            std::lock_guard lock(m->mutex);
            m->publisher = pub;
            m->kind = Kind::WiFiDirect;
            LOGI(L"Hotspot: Wi-Fi Direct network \"%s\" started", ssid.c_str());
            return true;
        }
        LOGW(L"Hotspot: Wi-Fi Direct didn't start (status %d, error %d)", status->load(), err->load());
        pub.Stop();
    } catch (const winrt::hresult_error& e) {
        LOGW(L"Hotspot: Wi-Fi Direct failed: 0x%08X %s", unsigned(e.code()), e.message().c_str());
    }
    if (!allowMobileHotspot) {
        std::lock_guard lock(m->mutex);
        m->error = L"This PC's Wi-Fi can't create the network next to the cast receiver.";
        return false;
    }
    // 2) Windows Mobile Hotspot, sharing the current internet connection (its settings are restored on Stop).
    try {
        auto profile = winrt::Windows::Networking::Connectivity::NetworkInformation::GetInternetConnectionProfile();
        if (!profile) throw winrt::hresult_error(E_FAIL, L"no internet connection to share");
        auto mgr = wno::NetworkOperatorTetheringManager::CreateFromConnectionProfile(profile);
        auto prev = mgr.GetCurrentAccessPointConfiguration();
        wno::NetworkOperatorTetheringAccessPointConfiguration cfg;
        cfg.Ssid(ssid);
        cfg.Passphrase(passphrase);
        mgr.ConfigureAccessPointAsync(cfg).get();
        const auto result = mgr.StartTetheringAsync().get();
        if (result.Status() != wno::TetheringOperationStatus::Success) {
            mgr.ConfigureAccessPointAsync(prev).get();
            throw winrt::hresult_error(E_FAIL, L"Mobile Hotspot didn't start (" + winrt::to_hstring(int(result.Status())) + L")");
        }
        std::lock_guard lock(m->mutex);
        m->tethering = mgr;
        m->previous = prev;
        m->kind = Kind::MobileHotspot;
        LOGI(L"Hotspot: Windows Mobile Hotspot \"%s\" started", ssid.c_str());
        return true;
    } catch (const winrt::hresult_error& e) {
        LOGW(L"Hotspot: Mobile Hotspot failed: 0x%08X %s", unsigned(e.code()), e.message().c_str());
        std::lock_guard lock(m->mutex);
        m->error = L"Couldn't create the network: " + std::wstring(e.message());
        return false;
    }
}

void Hotspot::Stop()
{
    wfd::WiFiDirectAdvertisementPublisher pub{nullptr};
    wno::NetworkOperatorTetheringManager mgr{nullptr};
    wno::NetworkOperatorTetheringAccessPointConfiguration prev{nullptr};
    {
        std::lock_guard lock(m->mutex);
        pub = std::exchange(m->publisher, nullptr);
        mgr = std::exchange(m->tethering, nullptr);
        prev = std::exchange(m->previous, nullptr);
        m->kind = Kind::None;
    }
    try {
        if (pub) {
            pub.Stop();
            LOGI(L"Hotspot: Wi-Fi Direct network stopped");
        }
        if (mgr) {
            mgr.StopTetheringAsync().get();
            if (prev) mgr.ConfigureAccessPointAsync(prev).get();   // put the user's own hotspot name/password back
            LOGI(L"Hotspot: Mobile Hotspot stopped, previous settings restored");
        }
    } catch (const winrt::hresult_error& e) {
        LOGW(L"Hotspot: stop: 0x%08X", unsigned(e.code()));
    }
}

Hotspot::Kind Hotspot::Active() const
{
    std::lock_guard lock(m->mutex);
    return m->kind;
}

std::wstring Hotspot::Ssid() const
{
    std::lock_guard lock(m->mutex);
    return m->ssid;
}

std::wstring Hotspot::Passphrase() const
{
    std::lock_guard lock(m->mutex);
    return m->pass;
}

std::wstring Hotspot::Error() const
{
    std::lock_guard lock(m->mutex);
    return m->error;
}

// The hotspot's adapter: "Wi-Fi Direct Virtual Adapter" (Wi-Fi Direct) or the hosted Mobile Hotspot adapter
// ("Microsoft Wi-Fi Direct Virtual Adapter" too on current Windows); the one that is up with a private IPv4.
static bool HotspotAdapter(NET_IFINDEX& index, std::wstring& address)
{
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, aa, &size) ==
        ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, aa, &size) != NO_ERROR)
            return false;
    }
    for (auto* a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || !a->Description || !wcsstr(a->Description, L"Wi-Fi Direct")) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            if (sin->sin_family != AF_INET || (sin->sin_addr.S_un.S_un_b.s_b1 == 169 && sin->sin_addr.S_un.S_un_b.s_b2 == 254)) continue;
            wchar_t s[32] = {};
            InetNtopW(AF_INET, &sin->sin_addr, s, 32);
            index = a->IfIndex;
            address = s;
            return true;
        }
    }
    return false;
}

std::wstring Hotspot::Address() const
{
    if (Active() == Kind::None) return L"";
    NET_IFINDEX idx = 0;
    std::wstring addr;
    return HotspotAdapter(idx, addr) ? addr : L"";
}

int Hotspot::Clients() const
{
    if (Active() == Kind::None) return 0;
    NET_IFINDEX idx = 0;
    std::wstring addr;
    if (!HotspotAdapter(idx, addr)) return 0;
    PMIB_IPNET_TABLE2 table = nullptr;
    if (GetIpNetTable2(AF_INET, &table) != NO_ERROR || !table) return 0;
    int n = 0;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const auto& r = table->Table[i];
        if (r.InterfaceIndex != idx) continue;
        if (r.State != NlnsReachable && r.State != NlnsStale && r.State != NlnsDelay && r.State != NlnsProbe) continue;
        const auto& a = r.Address.Ipv4.sin_addr.S_un.S_un_b;
        if (a.s_b4 == 255 || a.s_b1 >= 224) continue;   // broadcast / multicast
        ++n;
    }
    FreeMibTable(table);
    return n;
}

} // namespace cc
