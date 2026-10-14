#pragma once
#include "core/Common.h"

#include <memory>

namespace cc {

// The app's own Wi-Fi network ("ZeroDisplay-XXXX"), so phones can join without the home network:
// Wi-Fi Direct in legacy mode (a normal access point that exists only while ZeroDisplay runs; nothing is
// stored in Windows). Fallback when that can't run next to the Miracast receiver: Windows Mobile Hotspot,
// whose previous name/password are put back when the app turns it off.
class Hotspot {
public:
    enum class Kind { None, WiFiDirect, MobileHotspot };
    Hotspot();
    ~Hotspot();
    Hotspot(const Hotspot&) = delete;
    Hotspot& operator=(const Hotspot&) = delete;

    // Blocks up to a few seconds: call off the render thread.
    bool Start(const std::wstring& ssid, const std::wstring& passphrase, bool allowMobileHotspot);
    void Stop();
    Kind Active() const;
    std::wstring Ssid() const;
    std::wstring Passphrase() const;
    std::wstring Address() const;   // the PC's IPv4 address on the hotspot ("" until known)
    int Clients() const;            // devices seen on the hotspot network
    std::wstring Error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace cc
