#pragma once
#include "core/Common.h"

#include <functional>
#include <mmdeviceapi.h>

namespace cc {

struct VideoDeviceInfo {
    std::wstring name;      // shown everywhere: the user's name for it (Settings ▸ Devices), else Windows' name
    std::wstring symlink;   // unique per device/port
    GUID container{};       // physical device container, used to pair audio
    // Settings key ([Device.<key>], hidden list, last device). Windows' name, plus " #<port id>" when identical cards
    // share it; empty for network streams and casts (= the name).
    std::wstring key;
    std::wstring autoName;  // the name before the user's rename: Windows' name, numbered " (2)" for identical cards
    const std::wstring& Key() const { return key.empty() ? name : key; }
};

struct AudioDeviceInfo {
    std::wstring name;
    std::wstring id;
    GUID container{};
};

struct DeviceList {
    std::vector<VideoDeviceInfo> video;
    std::vector<AudioDeviceInfo> audioIn;
    std::vector<AudioDeviceInfo> audioOut;
    std::wstring defaultOutId;
};

class DeviceManager {
public:
    ~DeviceManager() { StopWatching(); }

    static DeviceList Enumerate();
    static void NumberIdentical(std::vector<VideoDeviceInfo>& video);   // keys + numbered names for identical cards
    // An audio endpoint on a Bluetooth device (its parent device node is enumerated by Bluetooth): its sound comes
    // later than Windows reports (the radio link and the headphones' own buffer).
    static bool IsBluetoothEndpoint(const std::wstring& endpointId);
    // Index into list.audioIn of the audio endpoint belonging to the same physical device, or -1.
    static int FindPairedAudio(const DeviceList& list, const VideoDeviceInfo& video);
    static std::wstring EndpointName(IMMDevice* device);

    // onChange(defaultOutputChanged) may be called from any thread.
    bool StartWatching(HWND hwnd, std::function<void(bool)> onChange);
    void StopWatching();
    // Forward WM_DEVICECHANGE here.
    void OnDeviceChange(WPARAM wp);

private:
    std::vector<void*> m_notifications;
    ComPtr<IMMDeviceEnumerator> m_enumerator;
    ComPtr<IMMNotificationClient> m_client;
    std::function<void(bool)> m_onChange;
};

} // namespace cc
