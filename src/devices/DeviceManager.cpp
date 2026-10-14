#include "core/Common.h"

#include <initguid.h>

#include "devices/DeviceManager.h"
#include "core/Log.h"

#include <cfgmgr32.h>
#include <dbt.h>
#include <devpkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cwctype>

namespace cc {

// KSCATEGORY_VIDEO_CAMERA and KSCATEGORY_CAPTURE
static const GUID kCategoryVideoCamera = {0xE5323777, 0xF976, 0x4F5B, {0x9B, 0x55, 0xB9, 0x46, 0x99, 0xC4, 0x6E, 0x44}};
static const GUID kCategoryCapture = {0x65E8773D, 0x8F56, 0x11D0, {0xA3, 0xB9, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96}};
// Container ID Windows assigns to devices built into the PC itself. Never auto-pair these
// (it would route the laptop microphone into the speakers).
static const GUID kLocalMachineContainer = {0x00000000, 0x0000, 0x0000, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

static GUID ContainerFromInterface(const std::wstring& path)
{
    wchar_t instance[MAX_DEVICE_ID_LEN] = {};
    ULONG size = sizeof(instance);
    DEVPROPTYPE type = 0;
    if (CM_Get_Device_Interface_PropertyW(path.c_str(), &DEVPKEY_Device_InstanceId, &type, reinterpret_cast<PBYTE>(instance), &size, 0) != CR_SUCCESS)
        return {};
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return {};
    GUID container{};
    size = sizeof(container);
    if (CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_ContainerId, &type, reinterpret_cast<PBYTE>(&container), &size, 0) != CR_SUCCESS)
        return {};
    return container;
}

std::wstring DeviceManager::EndpointName(IMMDevice* device)
{
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) return L"Audio device";
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring name = L"Audio device";
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = v.pwszVal;
    PropVariantClear(&v);
    return name;
}

static GUID EndpointContainer(IMMDevice* device)
{
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) return {};
    PROPVARIANT v;
    PropVariantInit(&v);
    GUID g{};
    PROPERTYKEY key;
    key.fmtid = DEVPKEY_Device_ContainerId.fmtid;
    key.pid = DEVPKEY_Device_ContainerId.pid;
    if (SUCCEEDED(props->GetValue(key, &v)) && v.vt == VT_CLSID && v.puuid) g = *v.puuid;
    PropVariantClear(&v);
    return g;
}

static void EnumerateAudio(IMMDeviceEnumerator* en, EDataFlow flow, std::vector<AudioDeviceInfo>& out)
{
    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll))) return;
    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, &dev))) continue;
        LPWSTR id = nullptr;
        if (FAILED(dev->GetId(&id))) continue;
        AudioDeviceInfo info;
        info.id = id;
        CoTaskMemFree(id);
        info.name = DeviceManager::EndpointName(dev.Get());
        info.container = EndpointContainer(dev.Get());
        out.push_back(std::move(info));
    }
}

DeviceList DeviceManager::Enumerate()
{
    DeviceList list;

    ComPtr<IMFAttributes> attr;
    if (SUCCEEDED(MFCreateAttributes(&attr, 1))) {
        attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        IMFActivate** devices = nullptr;
        UINT32 count = 0;
        if (SUCCEEDED(MFEnumDeviceSources(attr.Get(), &devices, &count))) {
            for (UINT32 i = 0; i < count; ++i) {
                VideoDeviceInfo info;
                WCHAR* s = nullptr;
                UINT32 len = 0;
                if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &s, &len))) {
                    info.name = s;
                    CoTaskMemFree(s);
                }
                if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &s, &len))) {
                    info.symlink = s;
                    CoTaskMemFree(s);
                }
                info.container = ContainerFromInterface(info.symlink);
                devices[i]->Release();
                if (!info.symlink.empty()) list.video.push_back(std::move(info));
            }
            CoTaskMemFree(devices);
        }
    }

    NumberIdentical(list.video);

    ComPtr<IMMDeviceEnumerator> en;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
        EnumerateAudio(en.Get(), eCapture, list.audioIn);
        EnumerateAudio(en.Get(), eRender, list.audioOut);
        ComPtr<IMMDevice> def;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def))) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(def->GetId(&id))) {
                list.defaultOutId = id;
                CoTaskMemFree(id);
            }
        }
    }
    return list;
}

// Identical cards share Windows' name: each gets its own settings key from its device path (stable for the USB port
// it's in, or its serial number) and a numbered name, "USB3.0 Capture", "USB3.0 Capture (2)". Entries that already
// have a key are left alone.
void DeviceManager::NumberIdentical(std::vector<VideoDeviceInfo>& video)
{
    auto portId = [](const std::wstring& symlink) {
        uint32_t h = 2166136261u;   // FNV-1a of the lower-case path
        for (wchar_t c : symlink) {
            h ^= uint32_t(towlower(c));
            h *= 16777619u;
        }
        wchar_t buf[12];
        swprintf_s(buf, L"%08x", h);
        return std::wstring(buf);
    };
    std::vector<VideoDeviceInfo*> fresh;
    for (auto& v : video)
        if (v.key.empty()) {
            v.key = v.autoName = v.name;
            fresh.push_back(&v);
        }
    for (size_t i = 0; i < fresh.size(); ++i) {
        std::vector<VideoDeviceInfo*> same;
        for (VideoDeviceInfo* v : fresh)
            if (v->name == fresh[i]->name && v->key == v->name) same.push_back(v);
        if (same.size() < 2) continue;
        std::sort(same.begin(), same.end(), [&](const VideoDeviceInfo* a, const VideoDeviceInfo* b) { return portId(a->symlink) < portId(b->symlink); });
        const std::wstring base = same[0]->name;
        for (size_t k = 0; k < same.size(); ++k) {
            same[k]->key = base + L" #" + portId(same[k]->symlink);
            same[k]->autoName = same[k]->name = k == 0 ? base : base + L" (" + std::to_wstring(k + 1) + L")";
        }
    }
}

bool DeviceManager::IsBluetoothEndpoint(const std::wstring& endpointId)
{
    if (endpointId.empty()) return false;
    const std::wstring instance = L"SWD\\MMDEVAPI\\" + endpointId;
    DEVINST node = 0, parent = 0;
    if (CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(instance.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return false;
    if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) return false;
    wchar_t id[MAX_DEVICE_ID_LEN] = {};
    if (CM_Get_Device_IDW(parent, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return false;
    return _wcsnicmp(id, L"BTH", 3) == 0;   // BTHENUM (A2DP), BTHHFENUM (hands-free), BTHLE... (LE Audio)
}

int DeviceManager::FindPairedAudio(const DeviceList& list, const VideoDeviceInfo& video)
{
    const GUID null{};
    if (video.container != null && video.container != kLocalMachineContainer) {
        for (size_t i = 0; i < list.audioIn.size(); ++i)
            if (list.audioIn[i].container == video.container) return int(i);
    }
    // Fallback: capture cards usually name their audio "Something (<video name>)", with Windows' name (not the user's).
    std::wstring winName = video.key.empty() ? video.name : video.key;
    if (video.key != video.autoName && winName.rfind(L" #") != std::wstring::npos) winName.resize(winName.rfind(L" #"));
    if (!winName.empty()) {
        for (size_t i = 0; i < list.audioIn.size(); ++i)
            if (list.audioIn[i].name.find(L"(" + winName + L")") != std::wstring::npos) return int(i);
    }
    return -1;
}

namespace {

class NotificationClient : public IMMNotificationClient {
public:
    explicit NotificationClient(std::function<void(bool)> cb) : m_cb(std::move(cb)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override
    {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { m_cb(false); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { m_cb(false); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { m_cb(false); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
    {
        if (role == eConsole) m_cb(flow == eRender);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    std::atomic<ULONG> m_ref{1};
    std::function<void(bool)> m_cb;
};

} // namespace

bool DeviceManager::StartWatching(HWND hwnd, std::function<void(bool)> onChange)
{
    m_onChange = std::move(onChange);
    for (const GUID& cat : {kCategoryVideoCamera, kCategoryCapture}) {
        DEV_BROADCAST_DEVICEINTERFACE_W filter{};
        filter.dbcc_size = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        filter.dbcc_classguid = cat;
        HDEVNOTIFY h = RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
        if (h) m_notifications.push_back(h);
    }
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&m_enumerator)))) {
        m_client.Attach(new NotificationClient(m_onChange));
        m_enumerator->RegisterEndpointNotificationCallback(m_client.Get());
    }
    return true;
}

void DeviceManager::StopWatching()
{
    for (void* h : m_notifications) UnregisterDeviceNotification(HDEVNOTIFY(h));
    m_notifications.clear();
    if (m_enumerator && m_client) m_enumerator->UnregisterEndpointNotificationCallback(m_client.Get());
    m_client.Reset();
    m_enumerator.Reset();
}

void DeviceManager::OnDeviceChange(WPARAM wp)
{
    if ((wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) && m_onChange) m_onChange(false);
}

} // namespace cc
