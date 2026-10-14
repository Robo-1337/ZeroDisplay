#include "wireless/MiracastSink.h"
#include "core/Log.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Miracast.h>

#include <map>
#include <mutex>

namespace cc {

using namespace winrt::Windows::Media::Miracast;

static const wchar_t* ListeningName(MiracastReceiverListeningStatus s)
{
    switch (s) {
    case MiracastReceiverListeningStatus::NotListening: return L"not listening";
    case MiracastReceiverListeningStatus::Listening: return L"ready";
    case MiracastReceiverListeningStatus::ConnectionPending: return L"connecting";
    case MiracastReceiverListeningStatus::Connected: return L"connected";
    case MiracastReceiverListeningStatus::DisabledByPolicy: return L"disabled by policy";
    case MiracastReceiverListeningStatus::TemporarilyDisabled: return L"temporarily disabled";
    }
    return L"?";
}

struct MiracastSink::Impl {
    MiracastReceiver receiver{nullptr};
    MiracastReceiverSession session{nullptr};
    mutable std::mutex mutex;
    std::vector<Event> events;
    std::map<std::wstring, MiracastReceiverConnection> connections;
    std::atomic<bool> live{false};   // false: events are ignored (stopping / stopped)
    winrt::event_token onCreated{}, onMedia{}, onDisconnected{};
};

MiracastSink::MiracastSink() : m(std::make_shared<Impl>()) {}
MiracastSink::~MiracastSink() { Stop(); }

bool MiracastSink::Start(const std::wstring& friendlyName, bool requirePin)
{
    try {
        m->receiver = MiracastReceiver();
        MiracastReceiverSettings settings = m->receiver.GetDefaultSettings();
        settings.FriendlyName(friendlyName);
        settings.ModelName(L"ZeroDisplay");
        settings.ModelNumber(L"1");
        settings.AuthorizationMethod(requirePin ? MiracastReceiverAuthorizationMethod::PinDisplayRequired : MiracastReceiverAuthorizationMethod::None);
        settings.RequireAuthorizationFromKnownTransmitters(requirePin);
        const MiracastReceiverApplySettingsResult applied = m->receiver.DisconnectAllAndApplySettings(settings);
        if (applied.Status() != MiracastReceiverApplySettingsStatus::Success)
            LOGW(L"Miracast: settings not applied: %d (0x%08X)", int(applied.Status()), unsigned(applied.ExtendedError()));

        m->session = m->receiver.CreateSession(nullptr);
        m->session.AllowConnectionTakeover(true);
        m->session.MaxSimultaneousConnections(4);
        m->live = true;
        // The handlers run on WinRT threads and may still fire after Stop (a crash on the RTX PC, 2026-10-12: a
        // "disconnected" event reached a receiver that had been destroyed). They hold the state weakly, ignore events
        // once stopping, and never let a WinRT error escape (that would end the app).
        const std::weak_ptr<Impl> weak = m;
        m->onCreated = m->session.ConnectionCreated([weak](const MiracastReceiverSession&, const MiracastReceiverConnectionCreatedEventArgs& a) {
            const auto s = weak.lock();
            if (!s || !s->live) return;
            try {
                const auto t = a.Connection().Transmitter();
                Event e;
                e.type = Event::Type::Connected;
                e.key = std::wstring(t.MacAddress());
                e.name = std::wstring(t.Name());
                e.pin = std::wstring(a.Pin());
                LOGI(L"Miracast: connection from \"%s\" (%s)", e.name.c_str(), e.key.c_str());
                std::lock_guard lock(s->mutex);
                s->connections.insert_or_assign(e.key, a.Connection());
                s->events.push_back(std::move(e));
            } catch (const winrt::hresult_error& x) {
                LOGW(L"Miracast: connection event: 0x%08X", unsigned(x.code()));
            }
        });
        m->onMedia = m->session.MediaSourceCreated([weak](const MiracastReceiverSession&, const MiracastReceiverMediaSourceCreatedEventArgs& a) {
            // Hand the source over and return at once: the stream only starts after this event has returned.
            const auto s = weak.lock();
            if (!s || !s->live) return;
            try {
                const auto t = a.Connection().Transmitter();
                Event e;
                e.type = Event::Type::Media;
                e.key = std::wstring(t.MacAddress());
                e.name = std::wstring(t.Name());
                winrt::Windows::Media::Core::MediaSource src = a.MediaSource();
                e.source.Attach(static_cast<IUnknown*>(winrt::detach_abi(src)));
                LOGI(L"Miracast: stream from \"%s\"", e.name.c_str());
                std::lock_guard lock(s->mutex);
                s->events.push_back(std::move(e));
            } catch (const winrt::hresult_error& x) {
                LOGW(L"Miracast: stream event: 0x%08X", unsigned(x.code()));
            }
        });
        m->onDisconnected = m->session.Disconnected([weak](const MiracastReceiverSession&, const MiracastReceiverDisconnectedEventArgs& a) {
            const auto s = weak.lock();
            if (!s || !s->live) return;
            try {
                Event e;
                e.type = Event::Type::Disconnected;
                if (const auto conn = a.Connection()) {
                    e.key = std::wstring(conn.Transmitter().MacAddress());
                    e.name = std::wstring(conn.Transmitter().Name());
                }
                LOGI(L"Miracast: %s disconnected", e.name.c_str());
                std::lock_guard lock(s->mutex);
                s->connections.erase(e.key);
                s->events.push_back(std::move(e));
            } catch (const winrt::hresult_error& x) {
                LOGW(L"Miracast: disconnect event: 0x%08X", unsigned(x.code()));
            }
        });
        const MiracastReceiverSessionStartResult started = m->session.StartAsync().get();
        const bool ok = started.Status() == MiracastReceiverSessionStartStatus::Success;
        LOGI(L"Miracast: receiver \"%s\" %s; %s", friendlyName.c_str(), ok ? L"started" : L"failed to start", StatusText().c_str());
        // A receiver that couldn't start (e.g. beside this PC's own Wi-Fi network: one Wi-Fi card can't do both) is let
        // go at once: released later, after a phone had joined that network, it never came back (the app hung on exit).
        if (!ok) Stop();
        return ok;
    } catch (const winrt::hresult_error& e) {
        LOGE(L"Miracast: start failed: 0x%08X %s", unsigned(e.code()), e.message().c_str());
        m->live = false;
        m->session = nullptr;
        m->receiver = nullptr;
        return false;
    }
}

void MiracastSink::Stop()
{
    if (!m->receiver && !m->session) return;
    m->live = false;   // from here on, events still on their way are dropped
    try {
        if (m->session) {
            m->session.ConnectionCreated(m->onCreated);
            m->session.MediaSourceCreated(m->onMedia);
            m->session.Disconnected(m->onDisconnected);
        }
    } catch (const winrt::hresult_error& e) {
        LOGW(L"Miracast: removing event handlers: 0x%08X", unsigned(e.code()));
    }
    try {
        std::map<std::wstring, MiracastReceiverConnection> conns;
        {
            std::lock_guard lock(m->mutex);
            conns.swap(m->connections);
        }
        for (auto& [k, c] : conns) c.Disconnect(MiracastReceiverDisconnectReason::Finished);
        if (m->session) m->session.Close();
    } catch (const winrt::hresult_error& e) {
        LOGW(L"Miracast: stop: 0x%08X", unsigned(e.code()));
    }
    m->session = nullptr;
    m->receiver = nullptr;
    LOGI(L"Miracast: receiver stopped");
}

bool MiracastSink::Running() const { return m->session != nullptr; }

std::wstring MiracastSink::StatusText() const
{
    try {
        if (!m->receiver) return L"off";
        return ListeningName(m->receiver.GetStatus().ListeningStatus());
    } catch (const winrt::hresult_error&) {
        return L"unknown";
    }
}

bool MiracastSink::Supported() const
{
    try {
        MiracastReceiver r = m->receiver ? m->receiver : MiracastReceiver();
        const auto wifi = r.GetStatus().WiFiStatus();
        return wifi == MiracastReceiverWiFiStatus::MiracastSupported || wifi == MiracastReceiverWiFiStatus::MiracastSupportNotOptimized;
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

std::vector<MiracastSink::Event> MiracastSink::TakeEvents()
{
    std::lock_guard lock(m->mutex);
    return std::exchange(m->events, {});
}

void MiracastSink::Disconnect(const std::wstring& key)
{
    MiracastReceiverConnection c{nullptr};
    {
        std::lock_guard lock(m->mutex);
        auto it = m->connections.find(key);
        if (it == m->connections.end()) return;
        c = it->second;
    }
    try {
        c.Disconnect(MiracastReceiverDisconnectReason::Finished);
    } catch (...) {
    }
}

} // namespace cc
