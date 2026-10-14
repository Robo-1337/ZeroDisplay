#include "devices/UsbLink.h"
#include "core/Log.h"

#include <initguid.h>   // first: defines DEVPKEY_* and GUID_DEVINTERFACE_USB_HUB here (selectany)
#include <cfgmgr32.h>
#include <devpkey.h>
#include <winioctl.h>
#include <usbioctl.h>
#include <usbiodef.h>

namespace cc {

namespace {

std::wstring DevNodeId(DEVINST dn)
{
    wchar_t id[MAX_DEVICE_ID_LEN + 1] = {};
    return CM_Get_Device_IDW(dn, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS ? std::wstring(id) : std::wstring();
}

bool StartsWithNoCase(const std::wstring& s, const wchar_t* prefix) { return _wcsnicmp(s.c_str(), prefix, wcslen(prefix)) == 0; }

} // namespace

UsbLink QueryUsbLink(const std::wstring& symlink)
{
    if (!StartsWithNoCase(symlink, L"\\\\?\\usb#")) return UsbLink::Unknown;
    // Interface → its device node (for a composite device: the "&MI_xx" function) → the USB device on the hub port.
    wchar_t instance[MAX_DEVICE_ID_LEN + 1] = {};
    DEVPROPTYPE type = 0;
    ULONG size = sizeof(instance);
    if (CM_Get_Device_Interface_PropertyW(symlink.c_str(), &DEVPKEY_Device_InstanceId, &type, reinterpret_cast<PBYTE>(instance), &size, 0) != CR_SUCCESS)
        return UsbLink::Unknown;
    DEVINST dn = 0;
    if (CM_Locate_DevNodeW(&dn, instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return UsbLink::Unknown;
    for (int up = 0; up < 3; ++up) {
        const std::wstring id = DevNodeId(dn);
        if (StartsWithNoCase(id, L"USB\\") && id.find(L"&MI_") == std::wstring::npos) break;
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, dn, 0) != CR_SUCCESS) return UsbLink::Unknown;
        dn = parent;
    }
    UINT32 port = 0;
    size = sizeof(port);
    if (CM_Get_DevNode_PropertyW(dn, &DEVPKEY_Device_Address, &type, reinterpret_cast<PBYTE>(&port), &size, 0) != CR_SUCCESS || port == 0)
        return UsbLink::Unknown;
    DEVINST hubNode = 0;
    if (CM_Get_Parent(&hubNode, dn, 0) != CR_SUCCESS) return UsbLink::Unknown;
    const std::wstring hubId = DevNodeId(hubNode);
    ULONG listLen = 0;
    if (CM_Get_Device_Interface_List_SizeW(&listLen, const_cast<GUID*>(&GUID_DEVINTERFACE_USB_HUB), const_cast<wchar_t*>(hubId.c_str()),
                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || listLen <= 1)
        return UsbLink::Unknown;
    std::wstring list(listLen, L'\0');
    if (CM_Get_Device_Interface_ListW(const_cast<GUID*>(&GUID_DEVINTERFACE_USB_HUB), const_cast<wchar_t*>(hubId.c_str()), list.data(), listLen,
                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
        return UsbLink::Unknown;
    HANDLE hub = CreateFileW(list.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hub == INVALID_HANDLE_VALUE) return UsbLink::Unknown;
    USB_NODE_CONNECTION_INFORMATION_EX_V2 v2{};
    v2.ConnectionIndex = port;
    v2.Length = sizeof(v2);
    v2.SupportedUsbProtocols.Usb300 = 1;
    DWORD got = 0;
    const BOOL ok = DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2, &v2, sizeof(v2), &v2, sizeof(v2), &got, nullptr);
    CloseHandle(hub);
    if (!ok) return UsbLink::Unknown;
    if (v2.Flags.DeviceIsOperatingAtSuperSpeedOrHigher) return UsbLink::Usb3;
    return v2.Flags.DeviceIsSuperSpeedCapableOrHigher ? UsbLink::Usb2OnUsb3Device : UsbLink::Usb2;
}

const wchar_t* UsbLinkName(UsbLink link)
{
    switch (link) {
    case UsbLink::Usb3: return L"USB 3";
    case UsbLink::Usb2: return L"USB 2";
    case UsbLink::Usb2OnUsb3Device: return L"USB 2 (USB 3 device)";
    default: return L"";
    }
}

} // namespace cc
