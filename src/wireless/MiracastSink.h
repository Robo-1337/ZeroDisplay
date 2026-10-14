#pragma once
#include "core/Common.h"

#include <memory>
#include <vector>

namespace cc {

// Miracast receiver ("cast to this PC") through Windows.Media.Miracast. Devices (Android Smart View /
// Cast, Windows Win+K) connect over Wi-Fi Direct; each connection delivers a media source, which the app
// plays through a CastSource. Events arrive on WinRT threads and are queued for the app thread.
class MiracastSink {
public:
    struct Event {
        enum class Type { Connected, Media, Disconnected } type = Type::Connected;
        std::wstring key;    // transmitter MAC address
        std::wstring name;   // "Galaxy S22"
        std::wstring pin;    // Connected, when PIN pairing is on
        ComPtr<IUnknown> source;   // Media: Windows.Media.Core.MediaSource
    };

    MiracastSink();
    ~MiracastSink();
    MiracastSink(const MiracastSink&) = delete;
    MiracastSink& operator=(const MiracastSink&) = delete;

    bool Start(const std::wstring& friendlyName, bool requirePin);
    void Stop();
    bool Running() const;
    std::wstring StatusText() const;   // listening / connected / Wi-Fi support
    bool Supported() const;            // the Wi-Fi adapter and driver can receive Miracast
    std::vector<Event> TakeEvents();
    void Disconnect(const std::wstring& key);

private:
    struct Impl;
    // Shared with the WinRT event handlers (as weak references): Windows may still deliver an event after Stop or
    // while this object is being destroyed, and such a late event must find the receiver gone, not freed memory.
    std::shared_ptr<Impl> m;
};

} // namespace cc
