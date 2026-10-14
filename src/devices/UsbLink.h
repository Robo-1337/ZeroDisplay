#pragma once
#include "core/Common.h"

namespace cc {

// How a USB capture device is connected right now (asked from its hub, read-only, like Windows' USBView).
enum class UsbLink {
    Unknown,            // not a USB device, or Windows didn't say
    Usb2,               // High Speed (480 Mbit/s); the device can't do more
    Usb2OnUsb3Device,   // a USB 3 device running at USB 2 speed: wrong port / cable, or the plug isn't fully in
    Usb3,               // SuperSpeed or faster
};

// From the Media Foundation symbolic link of the capture device. Takes a few ms; call off the render thread.
UsbLink QueryUsbLink(const std::wstring& symlink);
const wchar_t* UsbLinkName(UsbLink link);

} // namespace cc
