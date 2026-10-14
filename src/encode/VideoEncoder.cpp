#include "encode/VideoEncoder.h"
#include "core/Log.h"
#include "encode/H264.h"

#include <strmif.h>   // ICodecAPI

#include <codecapi.h>
#include <mferror.h>

#include <algorithm>
#include <cstring>

#include "ps_rgb2uv.h"
#include "ps_rgb2uv10.h"
#include "ps_rgb2y.h"
#include "ps_rgb2y10.h"
#include "vs_fullscreen.h"

namespace cc {

// ---------------------------------------------------------------------------------------------
// Async MFT event pump

class EncoderCore::EventCallback : public IMFAsyncCallback {
public:
    explicit EventCallback(EncoderCore* owner) : m_owner(owner) {}
    void Detach()
    {
        std::lock_guard lock(m_lock);
        m_owner = nullptr;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override
    {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFAsyncCallback)) {
            *ppv = static_cast<IMFAsyncCallback*>(this);
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
    HRESULT STDMETHODCALLTYPE GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult* result) override
    {
        std::lock_guard lock(m_lock);
        if (!m_owner) return S_OK;
        ComPtr<IMFMediaEvent> ev;
        HRESULT hr = m_owner->m_events->EndGetEvent(result, &ev);
        if (FAILED(hr) || m_owner->m_stopping) {
            SetEvent(m_owner->m_eventsDone);
            return S_OK;
        }
        m_owner->OnTransformEvent(ev.Get());
        if (FAILED(m_owner->m_events->BeginGetEvent(this, nullptr))) SetEvent(m_owner->m_eventsDone);
        return S_OK;
    }

private:
    std::atomic<ULONG> m_ref{1};
    std::mutex m_lock;
    EncoderCore* m_owner;
};

// ---------------------------------------------------------------------------------------------

EncoderCore::EncoderCore() { m_eventsDone = CreateEventW(nullptr, TRUE, FALSE, nullptr); }

EncoderCore::~EncoderCore()
{
    Stop();
    CloseHandle(m_eventsDone);
}

#define SetCodecValue(api, key, value) SetCodecValueNamed(api, key, value, L## #key)
static void SetCodecValueNamed(ICodecAPI* api, const GUID& key, UINT32 value, const wchar_t* name)
{
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    HRESULT hr = api->SetValue(&key, &v);
    if (FAILED(hr)) {
        // e.g. AMD: only logged. NVIDIA refuses the B-frame count but makes none in low-latency mode (checked with
        // ffprobe), so that one is not a warning.
        if (key == CODECAPI_AVEncMPVDefaultBPictureCount) LOGI(L"Encoder: %s not accepted (%s)", name, HrToString(hr).c_str());
        else LOGW(L"Encoder: %s not accepted (%s)", name, HrToString(hr).c_str());
    }
}

bool EncoderCore::Configure(IMFTransform* t, bool async, bool gpuInput)
{
    if (async) {
        ComPtr<IMFAttributes> attrs;
        if (FAILED(t->GetAttributes(&attrs))) return false;
        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        // Without a D3D manager a hardware MFT takes frames in system memory (another GPU's encoder).
        if (gpuInput && FAILED(t->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, ULONG_PTR(m_manager.Get())))) {
            LOGW(L"Encoder: MFT refused our D3D device");
            return false;
        }
    }

    ComPtr<ICodecAPI> api;
    if (SUCCEEDED(t->QueryInterface(IID_PPV_ARGS(&api)))) {
        SetCodecValue(api.Get(), CODECAPI_AVLowLatencyMode, TRUE);
        SetCodecValue(api.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        SetCodecValue(api.Get(), CODECAPI_AVEncCommonMeanBitRate, m_cfg.bitrateKbps * 1000);
        SetCodecValue(api.Get(), CODECAPI_AVEncMPVGOPSize, m_cfg.gopFrames);
        SetCodecValue(api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        // Speed preset chosen by the user (Settings > Recording > Encoder) or by the app for sharing.
        // Note: HEVC is much heavier; on an HD 630 only the fastest preset holds 1080p60.
        if (m_cfg.qualityVsSpeed >= 0) SetCodecValue(api.Get(), CODECAPI_AVEncCommonQualityVsSpeed, UINT32(m_cfg.qualityVsSpeed));
    }

    // The exact frame size first; some encoders (e.g. AMD's) only take sizes aligned to 16 or 32 lines.
    // Then the picture fills the top-left of a larger frame and the display aperture crops it back.
    UINT lastW = 0, lastH = 0;
    for (UINT a : {s_testAlign ? s_testAlign : 2u, 16u, 32u}) {
        const UINT w = (m_cfg.width + a - 1) / a * a, h = (m_cfg.height + a - 1) / a * a;
        if (w == lastW && h == lastH) continue;
        lastW = w;
        lastH = h;
        if (!SetTypes(t, w, h)) continue;
        m_codedW = w;
        m_codedH = h;
        if (w != m_cfg.width || h != m_cfg.height) LOGI(L"Encoder: coding %ux%u frames for a %ux%u picture", w, h, m_cfg.width, m_cfg.height);
        MFT_OUTPUT_STREAM_INFO info{};
        t->GetOutputStreamInfo(0, &info);
        m_providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        m_outBufferSize = std::max<DWORD>(info.cbSize, w * h * 3 / 2);
        return true;
    }
    return false;
}

bool EncoderCore::SetTypes(IMFTransform* t, UINT codedW, UINT codedH)
{
    const bool cropped = codedW != m_cfg.width || codedH != m_cfg.height;
    MFVideoArea aperture{};
    aperture.Area = {LONG(m_cfg.width), LONG(m_cfg.height)};
    ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, m_cfg.hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, m_cfg.bitrateKbps * 1000);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, codedW, codedH);
    if (cropped) out->SetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<const UINT8*>(&aperture), sizeof(aperture));
    // Say what the pixels are (BT.709, or BT.2020 PQ for HDR10; limited range) so players and editors don't have to guess.
    out->SetUINT32(MF_MT_VIDEO_PRIMARIES, m_cfg.hdr ? MFVideoPrimaries_BT2020 : MFVideoPrimaries_BT709);
    out->SetUINT32(MF_MT_TRANSFER_FUNCTION, m_cfg.hdr ? MFVideoTransFunc_2084 : MFVideoTransFunc_709);
    out->SetUINT32(MF_MT_YUV_MATRIX, m_cfg.hdr ? MFVideoTransferMatrix_BT2020_10 : MFVideoTransferMatrix_BT709);
    out->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, m_cfg.fpsNum, m_cfg.fpsDen);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);   // non-square is refused by hardware encoders (Quick Sync)
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, m_cfg.hdr    ? UINT32(eAVEncH265VProfile_Main_420_10)
                                        : m_cfg.hevc ? UINT32(eAVEncH265VProfile_Main_420_8)
                                                     : UINT32(eAVEncH264VProfile_High));
    HRESULT hr = t->SetOutputType(0, out.Get(), 0);
    if (FAILED(hr) && !m_cfg.hevc) {
        out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        hr = t->SetOutputType(0, out.Get(), 0);
    }
    if (FAILED(hr)) {
        LOGW(L"Encoder: output type %ux%u rejected: %s", codedW, codedH, HrToString(hr).c_str());
        return false;
    }

    ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, m_cfg.hdr ? MFVideoFormat_P010 : MFVideoFormat_NV12);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, codedW, codedH);
    if (cropped) in->SetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<const UINT8*>(&aperture), sizeof(aperture));
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, m_cfg.fpsNum, m_cfg.fpsDen);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    in->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    in->SetUINT32(MF_MT_YUV_MATRIX, m_cfg.hdr ? MFVideoTransferMatrix_BT2020_10 : MFVideoTransferMatrix_BT709);
    if (m_cfg.hdr) {
        in->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT2020);
        in->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_2084);
    }
    hr = t->SetInputType(0, in.Get(), 0);
    if (FAILED(hr) && cropped) {   // some encoders only take the aperture on the output type
        in->DeleteItem(MF_MT_MINIMUM_DISPLAY_APERTURE);
        hr = t->SetInputType(0, in.Get(), 0);
    }
    if (FAILED(hr)) {
        LOGW(L"Encoder: input type %ux%u rejected: %s", codedW, codedH, HrToString(hr).c_str());
        return false;
    }
    return true;
}

bool EncoderCore::s_forceSoftware = false;
UINT EncoderCore::s_testAlign = 0;
bool EncoderCore::s_videoProcessorOnly = false;
std::atomic<bool> EncoderCore::s_testSlowOther{false};
std::atomic<bool> EncoderCore::s_testSlowSame{false};

namespace {
std::mutex g_otherMutex;
std::map<std::pair<uint64_t, bool>, std::pair<bool, EncoderAdapter>> g_other;   // (render LUID, hevc) -> answer
uint64_t LuidKey(LUID l) { return (uint64_t(uint32_t(l.HighPart)) << 32) | l.LowPart; }
} // namespace

bool EncoderCore::OtherAdapterKnown(LUID renderAdapter, bool hevc)
{
    std::lock_guard lock(g_otherMutex);
    return g_other.count({LuidKey(renderAdapter), hevc}) != 0;
}

bool EncoderCore::FindOtherAdapter(LUID renderAdapter, bool hevc, EncoderAdapter& out)
{
    const auto key = std::make_pair(LuidKey(renderAdapter), hevc);
    {
        std::lock_guard lock(g_otherMutex);
        auto it = g_other.find(key);
        if (it != g_other.end()) {
            out = it->second.second;
            return it->second.first;
        }
    }
    // By capability, never by vendor: the first other hardware adapter with an encoder MFT for the codec.
    bool found = false;
    EncoderAdapter result;
    ComPtr<IDXGIFactory1> f;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; !found && f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset()) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || LuidKey(d.AdapterLuid) == key.first) continue;
            MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12};
            MFT_REGISTER_TYPE_INFO outType{MFMediaType_Video, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264};
            ComPtr<IMFAttributes> ea;
            MFCreateAttributes(&ea, 1);
            ea->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&d.AdapterLuid), sizeof(d.AdapterLuid));
            IMFActivate** acts = nullptr;
            UINT32 count = 0;
            if (SUCCEEDED(MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in, &outType, ea.Get(), &acts, &count))) {
                for (UINT32 k = 0; k < count; ++k) acts[k]->Release();
                CoTaskMemFree(acts);
            }
            if (count > 0) {
                found = true;
                result.luid = d.AdapterLuid;
                result.name = d.Description;
            }
        }
    }
    LOGI(L"Encoder: second GPU with a hardware %s encoder: %s", hevc ? L"HEVC" : L"H.264", found ? result.name.c_str() : L"none");
    std::lock_guard lock(g_otherMutex);
    g_other[key] = {found, result};
    out = result;
    return found;
}

std::wstring EncoderCore::TargetName() const
{
    switch (m_cfg.target) {
    case EncodeTarget::OtherGpu: return L"hybrid: " + m_cfg.otherAdapterName;
    case EncodeTarget::SameGpu: return L"display GPU";
    default: return L"software";
    }
}

bool EncoderCore::CreateTransform(LUID adapter, bool allowHardware, bool allowSoftware, bool gpuInput)
{
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, m_cfg.hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264};

    // Hardware encoders on the chosen adapter first.
    ComPtr<IMFAttributes> ea;
    MFCreateAttributes(&ea, 1);
    ea->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&adapter), sizeof(adapter));
    for (int pass = allowHardware ? 0 : 1; pass < (allowSoftware ? 2 : 1) && !m_transform; ++pass) {
        const bool hw = pass == 0;
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        HRESULT hr = hw ? MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, ea.Get(), &acts, &count)
                        : MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &acts, &count);
        if (FAILED(hr)) continue;
        for (UINT32 i = 0; i < count; ++i) {
            if (!m_transform) {
                WCHAR* name = nullptr;
                UINT32 len = 0;
                std::wstring n = L"H.264 encoder";
                if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len)) && name) {
                    n = name;
                    CoTaskMemFree(name);
                }
                ComPtr<IMFTransform> t;
                const HRESULT ahr = acts[i]->ActivateObject(IID_PPV_ARGS(&t));
                if (FAILED(ahr)) LOGW(L"Encoder: %s could not be created: %s", n.c_str(), HrToString(ahr).c_str());
                if (SUCCEEDED(ahr)) {
                    if (Configure(t.Get(), hw, hw && gpuInput)) {
                        m_transform = t;
                        m_async = hw;
                        m_gpuInput = hw && gpuInput;
                        std::lock_guard lock(m_cfgMutex);
                        m_name = n;
                    } else {
                        LOGW(L"Encoder: %s could not be configured", n.c_str());
                        acts[i]->ShutdownObject();
                    }
                }
            }
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
    }
    return m_transform != nullptr;
}

EncoderConfig EncoderCore::Config() const
{
    std::lock_guard lock(m_cfgMutex);
    return m_cfg;
}

std::wstring EncoderCore::Name() const
{
    std::lock_guard lock(m_cfgMutex);
    return m_name;
}

bool EncoderCore::Start(ID3D11Device* device, IMFDXGIDeviceManager* manager, const EncoderConfig& cfg)
{
    Stop();
    m_submitted = 0;
    m_emitted = 0;
    m_testStall = s_testStall.exchange(false);
    {
        std::lock_guard lock(m_cfgMutex);
        m_cfg = cfg;
        m_cfg.width &= ~1u;
        m_cfg.height &= ~1u;
    }
    m_device = device;
    device->GetImmediateContext(&m_ctx);
    m_manager = manager;
    // Without a GPU video processor (e.g. the software renderer) frames are converted on the CPU and
    // encoded in software.
    m_cpuConvert = FAILED(m_device.As(&m_videoDevice)) || FAILED(m_ctx.As(&m_videoContext));
    if (m_cpuConvert) LOGW(L"Encoder: no GPU video processor, converting on the CPU");
    if (m_cfg.hdr && (m_cpuConvert || !m_cfg.hevc)) {
        LOGW(L"Encoder: HDR needs HEVC and the GPU conversion");
        return false;
    }

    ComPtr<IDXGIDevice> dxgi;
    m_device.As(&dxgi);
    ComPtr<IDXGIAdapter> adapter;
    dxgi->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);

    // Same GPU: its hardware encoder reads our textures, else the software encoder. Other GPU: only its
    // hardware encoder (the caller falls back to the same GPU), fed from system memory. Cpu: software only.
    const EncodeTarget want = s_forceSoftware ? EncodeTarget::Cpu : m_cfg.target;
    const bool other = want == EncodeTarget::OtherGpu;
    const bool created = other ? CreateTransform(m_cfg.otherAdapter, true, false, false)
                               : CreateTransform(ad.AdapterLuid, want == EncodeTarget::SameGpu && !m_cpuConvert, true, true);
    if (!created) {
        if (other) LOGW(L"Encoder: the %s encoder of %s could not be used", m_cfg.hevc ? L"HEVC" : L"H.264", m_cfg.otherAdapterName.c_str());
        else LOGE(L"Encoder: no usable %s encoder", m_cfg.hevc ? L"HEVC" : L"H.264");
        Stop();
        return false;
    }
    {
        std::lock_guard lock(m_cfgMutex);
        m_cfg.target = !m_async ? EncodeTarget::Cpu : (other ? EncodeTarget::OtherGpu : EncodeTarget::SameGpu);
    }
    m_testSlow = (other && s_testSlowOther.load()) || (!other && m_async && s_testSlowSame.load());   // testing: slow encoders
    m_testCount = 0;

    if (m_gpuInput) {
        // Frames for the hardware encoder come from a pool of NV12 textures it can read directly.
        ComPtr<IMFMediaType> nv12;
        MFCreateMediaType(&nv12);
        nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        nv12->SetGUID(MF_MT_SUBTYPE, m_cfg.hdr ? MFVideoFormat_P010 : MFVideoFormat_NV12);
        MFSetAttributeSize(nv12.Get(), MF_MT_FRAME_SIZE, m_codedW, m_codedH);
        ComPtr<IMFAttributes> aa;
        MFCreateAttributes(&aa, 2);
        aa->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER);
        aa->SetUINT32(MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT);
        HRESULT hr = MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&m_allocator));
        if (SUCCEEDED(hr)) hr = m_allocator->SetDirectXManager(m_manager.Get());
        if (SUCCEEDED(hr)) hr = m_allocator->InitializeSampleAllocatorEx(3, m_cfg.hevc ? 12 : 6, aa.Get(), nv12.Get());   // HEVC MFTs keep more frames in flight
        if (FAILED(hr)) {
            LOGE(L"Encoder: sample pool failed: %s", HrToString(hr).c_str());
            Stop();
            return false;
        }
    }
    if (!m_cpuConvert) InitShaderConvert();
    if (m_cfg.hdr && !m_shaderConvert) {   // the video processor can't make PQ from our linear frames
        LOGW(L"Encoder: HDR needs the shader conversion (10-bit render targets)");
        Stop();
        return false;
    }
    if (!m_gpuInput && !m_cpuConvert) {
        if (!CreateReadback()) {
            LOGE(L"Encoder: read-back textures failed");
            Stop();
            return false;
        }
        m_rbStop = false;
        m_rbThread = std::thread([this] { ReadbackWorker(); });
    }
    if (m_cpuConvert) {
        m_cpuStop = false;
        m_cpuThread = std::thread([this] { CpuWorker(); });
    }
    if (m_async) {
        m_transform.As(&m_events);
        m_callback.Attach(new EventCallback(this));
        ResetEvent(m_eventsDone);
        m_events->BeginGetEvent(m_callback.Get(), nullptr);
    } else {
        m_syncThread = std::thread([this] { SyncWorker(); });
    }

    m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    m_t0 = -1;
    m_needInput = 0;
    m_statStart = QpcNow();
    m_statFrames = m_statBytes = 0;
    m_dropped = 0;
    m_forceKeyframe = true;
    m_stopping = false;
    m_running = true;
    LOGI(L"Encoder: %s (%s%s), %ux%u @ %.2f fps, %u kbps, GOP %u, %s", m_name.c_str(), TargetName().c_str(),
         m_gpuInput ? L", zero copy" : (m_cpuConvert ? L", CPU conversion" : L", read-back"), m_cfg.width, m_cfg.height,
         double(m_cfg.fpsNum) / m_cfg.fpsDen, m_cfg.bitrateKbps, m_cfg.gopFrames,
         m_cfg.qualityVsSpeed >= 0 ? Format(L"quality vs speed %d", m_cfg.qualityVsSpeed).c_str() : L"encoder's own speed");
    return true;
}

bool EncoderCore::CreateReadback()
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = m_codedW;
    td.Height = m_codedH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = m_cfg.hdr ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_swNv12))) return false;
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& rb : m_rb) {
        rb = {};
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &rb.tex))) return false;
    }
    m_rbSeq = 0;
    return true;
}

void EncoderCore::Collect()
{
    // The read-back worker collects finished copies by itself; nothing to do on the render thread.
}

void EncoderCore::ReadbackWorker()
{
    SetCurrentThreadName(L"Encoder (read-back)");
    for (;;) {
        Readback* rb = nullptr;
        {
            std::unique_lock lock(m_rbMutex);
            m_rbCv.wait(lock, [&] {
                if (m_rbStop) return true;
                for (auto& r : m_rb)
                    if (r.state == 2) return true;
                return false;
            });
            if (m_rbStop) return;
            for (auto& r : m_rb)
                if (r.state == 2 && (!rb || r.seq < rb->seq)) rb = &r;
            rb->state = 3;
        }
        // The device is multithread protected: Map/Unmap are serialized with the render thread; the copy isn't.
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hr;
        int tries = 0;
        while ((hr = m_ctx->Map(rb->tex.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)) == DXGI_ERROR_WAS_STILL_DRAWING && ++tries < 200) {
            Sleep(1);   // the GPU hasn't finished the copy yet
            std::lock_guard lock(m_rbMutex);
            if (m_rbStop) {
                rb->state = 0;
                return;
            }
        }
        if (FAILED(hr) || hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            ++m_dropped;
            std::lock_guard lock(m_rbMutex);
            rb->state = 0;
            continue;
        }
        const DWORD rowBytes = m_codedW * (m_cfg.hdr ? 2 : 1), h = m_codedH, size = rowBytes * h * 3 / 2;   // NV12 / P010
        ComPtr<IMFMediaBuffer> mb;
        ComPtr<IMFSample> sample;
        BYTE* dst = nullptr;
        if (SUCCEEDED(MFCreateMemoryBuffer(size, &mb)) && SUCCEEDED(mb->Lock(&dst, nullptr, nullptr))) {
            const BYTE* src = static_cast<const BYTE*>(m.pData);
            for (DWORD y = 0; y < h; ++y) memcpy(dst + size_t(y) * rowBytes, src + size_t(y) * m.RowPitch, rowBytes);
            const BYTE* uv = src + size_t(m.RowPitch) * h;
            for (DWORD y = 0; y < h / 2; ++y) memcpy(dst + size_t(rowBytes) * h + size_t(y) * rowBytes, uv + size_t(y) * m.RowPitch, rowBytes);
            mb->Unlock();
            mb->SetCurrentLength(size);
            MFCreateSample(&sample);
            sample->AddBuffer(mb.Get());
        }
        m_ctx->Unmap(rb->tex.Get(), 0);
        int64_t time = 0, qpc = 0;
        {
            std::lock_guard lock(m_rbMutex);
            time = rb->time;
            qpc = rb->qpc;
            rb->state = 0;
        }
        if (sample) Enqueue(sample, time, qpc);
        else ++m_dropped;
    }
}

void EncoderCore::Stop()
{
    if (!m_transform) return;
    m_stopping = true;
    m_running = false;
    m_cv.notify_all();
    if (m_syncThread.joinable()) m_syncThread.join();
    {
        std::lock_guard lock(m_cpuMutex);
        m_cpuStop = true;
    }
    m_cpuCv.notify_all();
    if (m_cpuThread.joinable()) m_cpuThread.join();
    {
        std::lock_guard lock(m_rbMutex);
        m_rbStop = true;
    }
    m_rbCv.notify_all();
    if (m_rbThread.joinable()) m_rbThread.join();
    m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    if (m_async) {
        ComPtr<IMFShutdown> shutdown;
        if (SUCCEEDED(m_transform.As(&shutdown))) shutdown->Shutdown();
        WaitForSingleObject(m_eventsDone, 1000);
        if (m_callback) m_callback->Detach();
    }
    {
        std::lock_guard lock(m_mutex);
        m_queue.clear();
        m_fed.clear();
        m_fedCount = 0;
    }
    m_callback.Reset();
    m_events.Reset();
    m_transform.Reset();
    m_outViews.clear();
    m_inView.Reset();
    m_inTex = nullptr;
    m_vp.Reset();
    m_vpEnum.Reset();
    m_vpInW = m_vpInH = 0;
    if (m_allocator) m_allocator->UninitializeSampleAllocator();
    m_allocator.Reset();
    m_swNv12.Reset();
    for (auto& rb : m_rb) rb = {};
    m_srvs.clear();
    m_rtvs.clear();
    m_convState.Reset();
    m_ctx1.Reset();
    m_vs.Reset();
    m_psY.Reset();
    m_psUV.Reset();
    m_linear.Reset();
    m_raster.Reset();
    m_shaderConvert = false;
    m_gpuInput = false;
    for (auto& c : m_cpuSlots) c = CpuSlot{};
    m_videoContext.Reset();
    m_videoDevice.Reset();
    m_ctx.Reset();
    m_device.Reset();
    m_manager.Reset();
    std::lock_guard lock(m_psMutex);
    m_parameterSets.clear();
    LOGI(L"Encoder stopped");
}

void EncoderCore::SetOutput(std::function<void(const PacketPtr&)> out)
{
    std::lock_guard lock(m_outMutex);   // also waits for a hand-over in progress
    m_output = std::move(out);
}

std::vector<uint8_t> EncoderCore::ParameterSets() const
{
    std::lock_guard lock(m_psMutex);
    return m_parameterSets;
}

bool EncoderCore::EnsureProcessor(UINT inW, UINT inH)
{
    if (m_vp && inW == m_vpInW && inH == m_vpInH) return true;
    m_vp.Reset();
    m_vpEnum.Reset();
    m_inView.Reset();
    m_inTex = nullptr;
    m_outViews.clear();
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {m_cfg.fpsNum, m_cfg.fpsDen};
    cd.InputWidth = inW;
    cd.InputHeight = inH;
    cd.OutputFrameRate = {m_cfg.fpsNum, m_cfg.fpsDen};
    cd.OutputWidth = m_codedW;
    cd.OutputHeight = m_codedH;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    HRESULT hr = m_videoDevice->CreateVideoProcessorEnumerator(&cd, &m_vpEnum);
    if (SUCCEEDED(hr)) hr = m_videoDevice->CreateVideoProcessor(m_vpEnum.Get(), 0, &m_vp);
    if (FAILED(hr)) {
        LOGE(L"Encoder: video processor failed: %s", HrToString(hr).c_str());
        return false;
    }
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
    in.RGB_Range = 0;   // full-range RGB
    m_videoContext->VideoProcessorSetStreamColorSpace(m_vp.Get(), 0, &in);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs{};
    outCs.YCbCr_Matrix = 1;   // BT.709
    outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    m_videoContext->VideoProcessorSetOutputColorSpace(m_vp.Get(), &outCs);
    m_videoContext->VideoProcessorSetStreamFrameFormat(m_vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    m_videoContext->VideoProcessorSetStreamAutoProcessingMode(m_vp.Get(), 0, FALSE);
    D3D11_VIDEO_COLOR black{};
    black.YCbCr = {16.0f / 255.0f, 0.5f, 0.5f, 1.0f};
    m_videoContext->VideoProcessorSetOutputBackgroundColor(m_vp.Get(), TRUE, &black);
    // Keep the aspect ratio: letterbox the source inside the picture (the top-left m_cfg-sized part of
    // the coded frame; the rest is background).
    float s = std::min(float(m_cfg.width) / inW, float(m_cfg.height) / inH);
    LONG w = LONG(inW * s + 0.5f) & ~1, h = LONG(inH * s + 0.5f) & ~1;
    LONG x = (LONG(m_cfg.width) - w) / 2 & ~1, y = (LONG(m_cfg.height) - h) / 2 & ~1;
    RECT dst{x, y, x + w, y + h};
    RECT src{0, 0, LONG(inW), LONG(inH)};
    m_videoContext->VideoProcessorSetStreamSourceRect(m_vp.Get(), 0, TRUE, &src);
    m_videoContext->VideoProcessorSetStreamDestRect(m_vp.Get(), 0, TRUE, &dst);
    m_vpInW = inW;
    m_vpInH = inH;
    return true;
}

void EncoderCore::QueueCpuCopy(ID3D11Texture2D* bgra, int64_t time100ns, int64_t sourceQpc)
{
    D3D11_TEXTURE2D_DESC sd{};
    bgra->GetDesc(&sd);
    CpuSlot* slot = nullptr;
    {
        std::lock_guard lock(m_cpuMutex);
        for (auto& c : m_cpuSlots)
            if (c.state == 0) {
                slot = &c;
                break;
            }
        if (!slot) {   // the worker is behind: give up this frame (never wait)
            ++m_dropped;
            return;
        }
        slot->state = 2;
    }
    if (!slot->staging || slot->w != sd.Width || slot->h != sd.Height) {
        D3D11_TEXTURE2D_DESC td = sd;
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        slot->staging.Reset();
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &slot->staging))) {
            std::lock_guard lock(m_cpuMutex);
            slot->state = 0;
            return;
        }
        slot->w = sd.Width;
        slot->h = sd.Height;
    }
    m_ctx->CopyResource(slot->staging.Get(), bgra);
    {
        std::lock_guard lock(m_cpuMutex);
        slot->state = 1;
        slot->seq = ++m_cpuSeq;
        slot->time = time100ns;
        slot->qpc = sourceQpc;
    }
    m_cpuCv.notify_one();
}

void EncoderCore::CpuWorker()
{
    ComInit com;
    SetCurrentThreadName(L"Encoder (CPU conversion)");
    for (;;) {
        CpuSlot* s = nullptr;
        {
            std::unique_lock lock(m_cpuMutex);
            m_cpuCv.wait(lock, [&] {
                if (m_cpuStop) return true;
                for (auto& c : m_cpuSlots)
                    if (c.state == 1) return true;
                return false;
            });
            if (m_cpuStop) return;
            for (auto& c : m_cpuSlots)
                if (c.state == 1 && (!s || c.seq < s->seq)) s = &c;
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = m_ctx->Map(s->staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            Sleep(1);   // the GPU (or WARP) hasn't finished the copy yet
            continue;
        }
        ComPtr<IMFSample> sample;
        if (SUCCEEDED(hr)) {
            sample = ConvertBgra(static_cast<const BYTE*>(m.pData), m.RowPitch, s->w, s->h);
            m_ctx->Unmap(s->staging.Get(), 0);
        }
        int64_t time = 0, qpc = 0;
        {
            std::lock_guard lock(m_cpuMutex);
            time = s->time;
            qpc = s->qpc;
            s->state = 0;
        }
        if (sample) Enqueue(sample, time, qpc);
        else ++m_dropped;
    }
}

// BGRA -> NV12, BT.709 limited range, letterboxed into the picture (fixed point; nearest-neighbour scaling).
ComPtr<IMFSample> EncoderCore::ConvertBgra(const BYTE* src, UINT pitch, UINT srcW, UINT srcH)
{
    const UINT W = m_codedW, H = m_codedH;   // coded frame; the picture is its top-left m_cfg-sized part
    const UINT PW = m_cfg.width, PH = m_cfg.height;
    const DWORD size = W * H * 3 / 2;
    ComPtr<IMFMediaBuffer> mb;
    BYTE* dst = nullptr;
    if (FAILED(MFCreateMemoryBuffer(size, &mb)) || FAILED(mb->Lock(&dst, nullptr, nullptr))) return nullptr;
    const float s = std::min(float(PW) / srcW, float(PH) / srcH);
    const UINT dw = UINT(srcW * s) & ~1u, dh = UINT(srcH * s) & ~1u;
    const UINT dx = ((PW - dw) / 2) & ~1u, dy = ((PH - dh) / 2) & ~1u;
    BYTE* yPlane = dst;
    BYTE* uvPlane = dst + size_t(W) * H;
    if (dw != W || dh != H) {
        memset(yPlane, 16, size_t(W) * H);
        memset(uvPlane, 128, size_t(W) * H / 2);
    }
    std::vector<UINT> sx(dw);
    for (UINT x = 0; x < dw; ++x) sx[x] = std::min(UINT(x / s), srcW - 1) * 4;
    for (UINT y = 0; y < dh; y += 2) {
        const BYTE* r0 = src + size_t(std::min(UINT(y / s), srcH - 1)) * pitch;
        const BYTE* r1 = src + size_t(std::min(UINT((y + 1) / s), srcH - 1)) * pitch;
        BYTE* y0 = yPlane + size_t(dy + y) * W + dx;
        BYTE* y1 = y0 + W;
        BYTE* uv = uvPlane + size_t((dy + y) / 2) * W + dx;
        for (UINT x = 0; x < dw; x += 2) {
            const BYTE* p[4] = {r0 + sx[x], r0 + sx[x + 1], r1 + sx[x], r1 + sx[x + 1]};
            int rs = 0, gs = 0, bs = 0;
            for (int k = 0; k < 4; ++k) {
                const int b = p[k][0], g = p[k][1], r = p[k][2];
                const BYTE yv = BYTE(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16);
                ((k >> 1) ? y1 : y0)[x + (k & 1)] = yv;
                rs += r;
                gs += g;
                bs += b;
            }
            uv[x] = BYTE(std::clamp((-26 * rs - 87 * gs + 112 * bs + 512 + (128 << 10)) >> 10, 0, 255));
            uv[x + 1] = BYTE(std::clamp((112 * rs - 102 * gs - 10 * bs + 512 + (128 << 10)) >> 10, 0, 255));
        }
    }
    mb->Unlock();
    mb->SetCurrentLength(size);
    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(mb.Get());
    return sample;
}

void EncoderCore::Submit(ID3D11Texture2D* bgra, int64_t time100ns, int64_t sourceQpc)
{
    if (!m_running || !bgra) return;
    ++m_submitted;
    if (m_cpuConvert) {
        QueueCpuCopy(bgra, time100ns, sourceQpc);
        return;
    }
    D3D11_TEXTURE2D_DESC sd{};
    bgra->GetDesc(&sd);

    ComPtr<IMFSample> sample;
    ID3D11Texture2D* target = nullptr;
    ComPtr<ID3D11Texture2D> poolTex;
    UINT sub = 0;
    if (m_gpuInput) {
        if (FAILED(m_allocator->AllocateSample(&sample))) {
            ++m_dropped;   // encoder is behind; never block the render thread
            return;
        }
        ComPtr<IMFMediaBuffer> buf;
        ComPtr<IMFDXGIBuffer> dx;
        if (FAILED(sample->GetBufferByIndex(0, &buf)) || FAILED(buf.As(&dx)) || FAILED(dx->GetResource(IID_PPV_ARGS(&poolTex)))) return;
        dx->GetSubresourceIndex(&sub);
        target = poolTex.Get();
    } else {
        target = m_swNv12.Get();
    }

    if (!(m_shaderConvert && ShaderConvert(bgra, sd, target, sub)) && !ProcessorConvert(bgra, sd, target, sub)) {
        ++m_dropped;
        return;
    }

    if (!m_gpuInput) {
        // Other GPU / software encoder: queue a copy to system memory, collected by the read-back worker once
        // the GPU has finished it. If every copy is still waiting, the oldest waiting frame is given up.
        Readback* rb = nullptr;
        {
            std::lock_guard lock(m_rbMutex);
            for (auto& r : m_rb)
                if (r.state == 0) rb = &r;
            if (!rb) {
                for (auto& r : m_rb)
                    if (r.state == 2 && (!rb || r.seq < rb->seq)) rb = &r;
                ++m_dropped;
            }
            if (!rb) return;   // all being read: skip this frame (never wait)
            rb->state = 1;
        }
        m_ctx->CopyResource(rb->tex.Get(), m_swNv12.Get());
        {
            std::lock_guard lock(m_rbMutex);
            rb->state = 2;
            rb->seq = ++m_rbSeq;
            rb->time = time100ns;
            rb->qpc = sourceQpc;
        }
        m_rbCv.notify_one();
        return;
    }

    Enqueue(sample, time100ns, sourceQpc);
}

bool EncoderCore::ProcessorConvert(ID3D11Texture2D* bgra, const D3D11_TEXTURE2D_DESC& sd, ID3D11Texture2D* target, UINT sub)
{
    if (!EnsureProcessor(sd.Width, sd.Height)) return false;
    if (bgra != m_inTex) {
        m_inView.Reset();
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        const HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(bgra, m_vpEnum.Get(), &ivd, &m_inView);
        if (FAILED(hr)) {
            static bool logged = false;
            if (!logged) LOGW(L"Encoder: texture can't be used as video processor input: %s", HrToString(hr).c_str());
            logged = true;
            return false;
        }
        m_inTex = bgra;
    }
    auto& view = m_outViews[{target, sub}];
    if (!view) {
        D3D11_TEXTURE2D_DESC td{};
        target->GetDesc(&td);
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        if (td.ArraySize > 1) {
            ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
            ovd.Texture2DArray.FirstArraySlice = sub;
            ovd.Texture2DArray.ArraySize = 1;
        } else {
            ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        }
        if (FAILED(m_videoDevice->CreateVideoProcessorOutputView(target, m_vpEnum.Get(), &ovd, &view))) {
            m_outViews.erase({target, sub});
            return false;
        }
    }
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = m_inView.Get();
    return SUCCEEDED(m_videoContext->VideoProcessorBlt(m_vp.Get(), view.Get(), 0, 1, &stream));
}

void EncoderCore::InitShaderConvert()
{
    m_shaderConvert = false;
    if (s_videoProcessorOnly) return;
    UINT support = 0;
    const DXGI_FORMAT planar = m_cfg.hdr ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    if (FAILED(m_device->CheckFormatSupport(planar, &support)) || !(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET)) {
        LOGI(L"Encoder: %s render targets not supported, using the video processor", m_cfg.hdr ? L"P010" : L"NV12");
        return;
    }
    ComPtr<ID3D11Device1> dev1;
    if (FAILED(m_device.As(&dev1)) || FAILED(m_ctx.As(&m_ctx1))) return;
    const D3D_FEATURE_LEVEL fl = m_device->GetFeatureLevel();
    if (FAILED(dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &m_convState))) return;
    if (FAILED(m_device->CreateVertexShader(g_vs_fullscreen, sizeof(g_vs_fullscreen), nullptr, &m_vs)) ||
        FAILED(m_cfg.hdr ? m_device->CreatePixelShader(g_ps_rgb2y10, sizeof(g_ps_rgb2y10), nullptr, &m_psY)
                         : m_device->CreatePixelShader(g_ps_rgb2y, sizeof(g_ps_rgb2y), nullptr, &m_psY)) ||
        FAILED(m_cfg.hdr ? m_device->CreatePixelShader(g_ps_rgb2uv10, sizeof(g_ps_rgb2uv10), nullptr, &m_psUV)
                         : m_device->CreatePixelShader(g_ps_rgb2uv, sizeof(g_ps_rgb2uv), nullptr, &m_psUV)))
        return;
    if (m_cfg.hdr) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m_device->CreateBuffer(&bd, nullptr, &m_encCb))) return;
    }
    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(m_device->CreateSamplerState(&smp, &m_linear)) || FAILED(m_device->CreateRasterizerState(&rd, &m_raster))) return;
    m_shaderConvert = true;
}

bool EncoderCore::ShaderConvert(ID3D11Texture2D* bgra, const D3D11_TEXTURE2D_DESC& sd, ID3D11Texture2D* target, UINT sub)
{
    if (!(sd.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return false;
    // Views, cached (a few input textures, one pair of plane views per pool texture/slice).
    ID3D11ShaderResourceView* srv = nullptr;
    for (auto& e : m_srvs)
        if (e.tex.Get() == bgra) srv = e.srv.Get();
    if (!srv) {
        SrvEntry e;
        e.tex = bgra;
        if (FAILED(m_device->CreateShaderResourceView(bgra, nullptr, &e.srv))) return false;
        if (m_srvs.size() >= 8) m_srvs.erase(m_srvs.begin());
        srv = e.srv.Get();
        m_srvs.push_back(std::move(e));
    }
    RtvEntry* rt = nullptr;
    for (auto& e : m_rtvs)
        if (e.tex.Get() == target && e.sub == sub) rt = &e;
    if (!rt) {
        D3D11_TEXTURE2D_DESC td{};
        target->GetDesc(&td);
        RtvEntry e;
        e.tex = target;
        e.sub = sub;
        D3D11_RENDER_TARGET_VIEW_DESC rv{};
        if (td.ArraySize > 1) {
            rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rv.Texture2DArray.FirstArraySlice = sub;
            rv.Texture2DArray.ArraySize = 1;
        } else {
            rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        }
        rv.Format = m_cfg.hdr ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;   // the format picks the plane: R8 / R16 = Y, R8G8 / R16G16 = UV
        HRESULT hr = m_device->CreateRenderTargetView(target, &rv, &e.y);
        rv.Format = m_cfg.hdr ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
        if (SUCCEEDED(hr)) hr = m_device->CreateRenderTargetView(target, &rv, &e.uv);
        if (FAILED(hr)) {
            LOGW(L"Encoder: NV12 plane views failed (%s), using the video processor", HrToString(hr).c_str());
            m_shaderConvert = false;
            return false;
        }
        if (m_rtvs.size() >= 16) m_rtvs.erase(m_rtvs.begin());
        m_rtvs.push_back(std::move(e));
        rt = &m_rtvs.back();
    }

    // Letterbox exactly like EnsureProcessor: the picture is the top-left m_cfg-sized part of the coded frame.
    const float s = std::min(float(m_cfg.width) / sd.Width, float(m_cfg.height) / sd.Height);
    const LONG w = LONG(sd.Width * s + 0.5f) & ~1, h = LONG(sd.Height * s + 0.5f) & ~1;
    const LONG x = (LONG(m_cfg.width) - w) / 2 & ~1, y = (LONG(m_cfg.height) - h) / 2 & ~1;
    const bool full = x == 0 && y == 0 && w == LONG(m_codedW) && h == LONG(m_codedH);

    ComPtr<ID3DDeviceContextState> previous;
    m_ctx1->SwapDeviceContextState(m_convState.Get(), &previous);
    if (!full) {
        const float k = m_cfg.hdr ? 64.0f * 64.0f / 65535.0f : 16.0f / 255.0f, c = m_cfg.hdr ? 512.0f * 64.0f / 65535.0f : 128.0f / 255.0f;
        const float black[4] = {k, 0, 0, 0}, grey[4] = {c, c, 0, 0};
        m_ctx->ClearRenderTargetView(rt->y.Get(), black);
        m_ctx->ClearRenderTargetView(rt->uv.Get(), grey);
    }
    if (m_cfg.hdr && m_encCb) {   // linear FP16 frames, or an SDR picture (the standby picture) shown at the same white
        D3D11_MAPPED_SUBRESOURCE mp{};
        if (SUCCEEDED(m_ctx->Map(m_encCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) {
            const float cb[4] = {m_cfg.hdrWhiteNits, sd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 0.0f : 1.0f, 0, 0};
            memcpy(mp.pData, cb, sizeof(cb));
            m_ctx->Unmap(m_encCb.Get(), 0);
        }
        ID3D11Buffer* b = m_encCb.Get();
        m_ctx->PSSetConstantBuffers(2, 1, &b);
    }
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->RSSetState(m_raster.Get());
    m_ctx->PSSetSamplers(0, 1, m_linear.GetAddressOf());
    m_ctx->PSSetShaderResources(0, 1, &srv);
    D3D11_VIEWPORT vp{float(x), float(y), float(w), float(h), 0, 1};
    m_ctx->OMSetRenderTargets(1, rt->y.GetAddressOf(), nullptr);
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->PSSetShader(m_psY.Get(), nullptr, 0);
    m_ctx->Draw(3, 0);
    vp = {float(x / 2), float(y / 2), float(w / 2), float(h / 2), 0, 1};
    m_ctx->OMSetRenderTargets(1, rt->uv.GetAddressOf(), nullptr);
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->PSSetShader(m_psUV.Get(), nullptr, 0);
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    m_ctx->PSSetShaderResources(0, 1, &none);
    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    m_ctx1->SwapDeviceContextState(previous.Get(), nullptr);
    return true;
}

void EncoderCore::Enqueue(const ComPtr<IMFSample>& sample, int64_t time100ns, int64_t sourceQpc)
{
    if (m_testSlow && (++m_testCount & 1)) {   // testing: an encoder that can't keep up
        ++m_dropped;
        return;
    }
    if (m_t0 < 0) m_t0 = time100ns;
    const int64_t rel = std::max<int64_t>(0, time100ns - m_t0);
    sample->SetSampleTime(rel);
    sample->SetSampleDuration(int64_t(10000000.0 * m_cfg.fpsDen / m_cfg.fpsNum));
    {
        std::lock_guard lock(m_mutex);
        m_fed.push_back({rel, sourceQpc, m_fedCount++});
        while (m_fed.size() > 128) m_fed.pop_front();
        // Only the newest frames matter: if the encoder hasn't asked for input yet, replace old ones.
        while (m_queue.size() >= 2) {
            m_queue.pop_front();
            ++m_dropped;
        }
        m_queue.push_back(sample);
    }
    if (m_async) TryFeed();
    else m_cv.notify_one();
}

void EncoderCore::TryFeed()
{
    std::lock_guard lock(m_mutex);
    while (m_needInput > 0 && !m_queue.empty() && !m_stopping) {
        ComPtr<IMFSample> s = m_queue.front();
        m_queue.pop_front();
        if (m_forceKeyframe.exchange(false)) {
            ComPtr<ICodecAPI> api;
            if (SUCCEEDED(m_transform.As(&api))) SetCodecValue(api.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
        }
        HRESULT hr = m_transform->ProcessInput(0, s.Get(), 0);
        if (FAILED(hr)) {
            LOGW(L"Encoder: ProcessInput failed: %s", HrToString(hr).c_str());
            break;
        }
        --m_needInput;
    }
}

void EncoderCore::OnTransformEvent(IMFMediaEvent* ev)
{
    MediaEventType type = MEUnknown;
    ev->GetType(&type);
    if (type == METransformNeedInput) {
        {
            std::lock_guard lock(m_mutex);
            ++m_needInput;
        }
        TryFeed();
    } else if (type == METransformHaveOutput) {
        DrainOutput();
    }
}

void EncoderCore::DrainOutput()
{
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        ComPtr<IMFSample> own;
        if (!m_providesSamples) {
            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(MFCreateMemoryBuffer(m_outBufferSize, &buf))) return;
            MFCreateSample(&own);
            own->AddBuffer(buf.Get());
            ob.pSample = own.Get();
        }
        DWORD status = 0;
        HRESULT hr = m_transform->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> t;
            if (SUCCEEDED(m_transform->GetOutputAvailableType(0, 0, &t))) m_transform->SetOutputType(0, t.Get(), 0);
            continue;
        }
        if (FAILED(hr)) return;   // MF_E_TRANSFORM_NEED_MORE_INPUT: drained
        Emit(ob.pSample);
        if (m_providesSamples && ob.pSample) ob.pSample->Release();
        if (m_async) return;   // async MFTs deliver one sample per METransformHaveOutput
    }
}

std::atomic<bool> EncoderCore::s_testStall{false};

void EncoderCore::Emit(IMFSample* sample)
{
    if (!sample || m_testStall) return;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return;
    BYTE* p = nullptr;
    DWORD len = 0;
    if (FAILED(buf->Lock(&p, nullptr, &len))) return;
    auto pkt = std::make_shared<EncodedPacket>();
    pkt->data.assign(p, p + len);
    buf->Unlock();

    LONGLONG t = 0, d = 0;
    sample->GetSampleTime(&t);
    sample->GetSampleDuration(&d);
    UINT32 clean = 0;
    pkt->keyframe = (SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean) ||
                    (m_cfg.hevc ? h264::IsKeyframeHevc(pkt->data.data(), pkt->data.size()) : h264::IsKeyframe(pkt->data.data(), pkt->data.size()));
    {
        // Some hardware encoders (NVIDIA's) stamp their output as frame number x frame duration instead of
        // passing our times through. Exact for a steady 60 fps card, but casts and network sources have a
        // variable frame rate: map the output back to the input's real time.
        std::lock_guard lock(m_mutex);
        const Fed* match = nullptr;
        for (const auto& f : m_fed)
            if (f.time == t) match = &f;
        if (!match && m_cfg.fpsNum) {
            const int64_t n = std::llround(double(t) * m_cfg.fpsNum / (1e7 * m_cfg.fpsDen));
            for (const auto& f : m_fed)
                if (f.index == n) match = &f;
            if (match && !m_restampLogged && std::llabs(match->time - t) > 20000) {
                m_restampLogged = true;
                LOGI(L"Encoder: output stamped by frame number (%.1f ms off at frame %lld): using the real frame times", (t - match->time) / 1e4,
                     (long long)n);
            }
        }
        if (match) {
            t = match->time;
            pkt->sourceQpc = match->qpc;
        }
    }
    pkt->time = t + m_t0;
    pkt->duration = d;
    if (pkt->keyframe) {
        auto ps = m_cfg.hevc ? h264::ExtractParameterSetsHevc(pkt->data.data(), pkt->data.size())
                             : h264::ExtractParameterSets(pkt->data.data(), pkt->data.size());
        if (!ps.empty()) {
            std::lock_guard lock(m_psMutex);
            m_parameterSets = std::move(ps);
        }
    }

    const int64_t now = QpcNow();
    if (pkt->sourceQpc) {
        double ms = QpcToMs(now - pkt->sourceQpc);
        double prev = m_latencyMs.load();
        m_latencyMs = prev == 0 ? ms : prev * 0.9 + ms * 0.1;
    }
    m_statFrames++;
    m_statBytes += len;
    ++m_emitted;
    double el = QpcToMs(now - m_statStart);
    if (el >= 1000.0) {
        m_outFps = m_statFrames * 1000.0 / el;
        m_kbps = m_statBytes * 8.0 / el;
        m_statFrames = m_statBytes = 0;
        m_statStart = now;
    }

    std::lock_guard out(m_outMutex);
    if (m_output) m_output(pkt);
}

void EncoderCore::SyncWorker()
{
    ComInit com;
    SetCurrentThreadName(L"Encoder (software)");
    for (;;) {
        ComPtr<IMFSample> s;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [&] { return m_stopping || !m_queue.empty(); });
            if (m_stopping) return;
            s = m_queue.front();
            m_queue.pop_front();
        }
        if (m_forceKeyframe.exchange(false)) {
            ComPtr<ICodecAPI> api;
            if (SUCCEEDED(m_transform.As(&api))) SetCodecValue(api.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
        }
        if (SUCCEEDED(m_transform->ProcessInput(0, s.Get(), 0))) DrainOutput();
    }
}

} // namespace cc

namespace cc {

// ---------------------------------------------------------------------------------------------
// VideoEncoder: stable front for consumers; replaces its core without a gap.

VideoEncoder::~VideoEncoder() { Stop(); }

std::shared_ptr<EncoderCore> VideoEncoder::Core() const
{
    std::lock_guard lock(m_coreMutex);
    return m_core;
}

void VideoEncoder::Dispatch(const PacketPtr& p)
{
    std::lock_guard emit(m_emitMutex);
    std::vector<std::function<void(const PacketPtr&)>> sinks;
    {
        std::lock_guard lock(m_sinkMutex);
        for (auto& [id, s] : m_sinks) sinks.push_back(s);
    }
    for (auto& s : sinks) s(p);
}

int VideoEncoder::AddSink(std::function<void(const PacketPtr&)> sink)
{
    std::lock_guard lock(m_sinkMutex);
    m_sinks[m_nextSink] = std::move(sink);
    return m_nextSink++;
}

void VideoEncoder::RemoveSink(int id)
{
    {
        std::lock_guard lock(m_sinkMutex);
        m_sinks.erase(id);
    }
    std::lock_guard emit(m_emitMutex);   // a dispatch that may still be calling the removed sink has finished
}

bool VideoEncoder::Start(ID3D11Device* device, IMFDXGIDeviceManager* manager, const EncoderConfig& cfg)
{
    Stop();
    m_device = device;
    m_manager = manager;
    auto core = std::make_shared<EncoderCore>();
    core->SetOutput([this](const PacketPtr& p) { Dispatch(p); });
    if (!core->Start(device, manager, cfg)) {
        core->SetOutput(nullptr);
        return false;
    }
    m_lastCfg = core->Config();
    std::lock_guard lock(m_coreMutex);
    m_core = std::move(core);
    return true;
}

void VideoEncoder::Stop()
{
    if (m_restartThread.joinable()) m_restartThread.join();
    std::shared_ptr<EncoderCore> core, next;
    {
        std::lock_guard lock(m_coreMutex);
        core = std::move(m_core);
        next = std::move(m_next);
    }
    m_nextReady = false;
    for (auto& c : {core, next})
        if (c) {
            c->SetOutput(nullptr);
            c->Stop();
        }
    JoinRetired(true);
}

void VideoEncoder::JoinRetired(bool all)
{
    if (!all) return;   // retired cores are joined when the encoder stops
    for (auto& t : m_retired)
        if (t.joinable()) t.join();
    m_retired.clear();
}

bool VideoEncoder::Restart(const EncoderConfig* newConfig)
{
    if (m_restarting || m_nextReady) return false;   // one is already under way
    if (m_restartThread.joinable()) m_restartThread.join();
    std::shared_ptr<EncoderCore> cur = Core();
    if (!cur || !m_device) return false;
    const EncoderConfig cfg = newConfig ? *newConfig : cur->Config();
    if (!newConfig)
        LOGW(L"Encoder: %s produced nothing for %llu frames, replacing it", cur->Name().c_str(), (unsigned long long)cur->Submitted());
    m_restarting = true;
    m_restartThread = std::thread([this, cfg] {
        ComInit com;
        SetCurrentThreadName(L"Encoder restart");
        auto core = std::make_shared<EncoderCore>();
        core->SetOutput([this](const PacketPtr& p) { Dispatch(p); });
        if (core->Start(m_device.Get(), m_manager.Get(), cfg)) {
            std::lock_guard lock(m_coreMutex);
            m_next = std::move(core);
            m_nextReady = true;
        } else {
            LOGE(L"Encoder: the replacement didn't start; keeping the current one");
            core->SetOutput(nullptr);
            core->Stop();
        }
        m_restarting = false;
    });
    return true;
}

void VideoEncoder::SwapIn()
{
    std::shared_ptr<EncoderCore> old;
    {
        std::lock_guard lock(m_coreMutex);
        if (!m_next) return;
        old = std::move(m_core);
        m_core = std::move(m_next);
        m_lastCfg = m_core->Config();
    }
    LOGI(L"Encoder: switched to %s (%s)", m_core->Name().c_str(), m_core->TargetName().c_str());
    if (old) {
        old->SetOutput(nullptr);   // its last frames in flight would carry the old stream headers
        m_retired.emplace_back([old]() mutable {
            ComInit com;
            old->Stop();
            old.reset();
        });
    }
}

void VideoEncoder::Submit(ID3D11Texture2D* bgra, int64_t time100ns, int64_t sourceQpc)
{
    // At most the configured rate: a source that switches to a faster mode mid-recording (60 -> 120 fps) would
    // otherwise overrun an encoder set up for 60 (it falls behind, restarts, the file splits).
    const EncoderConfig c = Config();
    const int64_t period = c.fpsNum ? int64_t(10000000.0 * c.fpsDen / c.fpsNum) : 0;
    if (period && m_lastAccepted != INT64_MIN && time100ns > m_lastAccepted && time100ns - m_lastAccepted < period * 3 / 4) return;
    m_lastAccepted = time100ns;
    if (m_nextReady.exchange(false)) SwapIn();
    if (m_core) m_core->Submit(bgra, time100ns, sourceQpc);   // m_core changes only on this thread
}

void VideoEncoder::Collect()
{
    if (m_core) m_core->Collect();
}

void VideoEncoder::RequestKeyframe()
{
    if (auto c = Core()) c->RequestKeyframe();
}

bool VideoEncoder::Running() const
{
    auto c = Core();
    return c && c->Running();
}

EncoderConfig VideoEncoder::Config() const
{
    auto c = Core();
    return c ? c->Config() : m_lastCfg;
}

std::wstring VideoEncoder::Name() const
{
    auto c = Core();
    return c ? c->Name() : std::wstring();
}

bool VideoEncoder::Hardware() const
{
    auto c = Core();
    return c && c->Hardware();
}

std::wstring VideoEncoder::TargetName() const
{
    auto c = Core();
    return c ? c->TargetName() : std::wstring();
}

std::vector<uint8_t> VideoEncoder::ParameterSets() const
{
    auto c = Core();
    return c ? c->ParameterSets() : std::vector<uint8_t>();
}

double VideoEncoder::LatencyMs() const { auto c = Core(); return c ? c->LatencyMs() : 0.0; }
double VideoEncoder::OutputFps() const { auto c = Core(); return c ? c->OutputFps() : 0.0; }
double VideoEncoder::BitrateKbps() const { auto c = Core(); return c ? c->BitrateKbps() : 0.0; }
uint64_t VideoEncoder::Dropped() const { auto c = Core(); return c ? c->Dropped() : 0; }
bool VideoEncoder::Stalled() const { auto c = Core(); return c && !Restarting() && c->Stalled(); }
uint64_t VideoEncoder::Submitted() const { auto c = Core(); return c ? c->Submitted() : 0; }
uint64_t VideoEncoder::Emitted() const { auto c = Core(); return c ? c->Emitted() : 0; }

} // namespace cc
