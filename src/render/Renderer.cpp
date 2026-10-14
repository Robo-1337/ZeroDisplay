#include "render/Renderer.h"
#include "core/Log.h"

#include "ps_bgra.h"
#include "ps_item.h"
#include "ps_nv12.h"
#include "ps_transition.h"
#include "ps_present.h"
#include "ps_filter.h"
#include "ps_bicubic.h"
#include "ps_sharpen.h"
#include "ps_upscale.h"
#include "ps_layer.h"
#include "ps_tohdr.h"
#include "ps_hdrpic.h"
#include "ps_layerhdr.h"
#include "ps_yuy2.h"
#include "vs_fullscreen.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cc {

struct ConvertCB {
    float row0[4];
    float row1[4];
    float row2[4];
    UINT size[2];
    UINT flags;
    UINT pad;
    INT xform[4];    // crop origin x, y and cropped width, height (source pixels)
    UINT xform2[4];  // quarter turns clockwise, mirror, output width, height
    float hdr[4];    // transfer (0 SDR, 1 PQ, 2 HLG), BT.2020 primaries (0 / 1), reference white nits, peak nits
};
static_assert(sizeof(ConvertCB) % 16 == 0);

// `tenBit`: 10-bit samples in 16-bit words (P010 / Y210), read as code * 64 / 65535: black is code 64, not 16 / 255.
static void ComputeMatrix(ColorMatrix m, ColorRange r, ConvertCB& cb, bool tenBit = false)
{
    float kr, kb;
    if (m == ColorMatrix::BT601) { kr = 0.299f; kb = 0.114f; }
    else if (m == ColorMatrix::BT2020) { kr = 0.2627f; kb = 0.0593f; }
    else { kr = 0.2126f; kb = 0.0722f; }
    const float kg = 1.0f - kr - kb;
    float ys, yo, cs, c0;
    if (tenBit) {
        const float u = 64.0f / 65535.0f;   // one 10-bit step
        if (r == ColorRange::Full) { ys = 1.0f / (1023.0f * u); yo = 0.0f; cs = ys; }
        else { ys = 1.0f / (876.0f * u); yo = 64.0f * u; cs = 1.0f / (896.0f * u); }
        c0 = 512.0f * u;
    } else {
        if (r == ColorRange::Full) { ys = 1.0f; yo = 0.0f; cs = 1.0f; }
        else { ys = 255.0f / 219.0f; yo = 16.0f / 255.0f; cs = 255.0f / 224.0f; }
        c0 = 128.0f / 255.0f;
    }
    const float rv = cs * (2.0f - 2.0f * kr);
    const float bu = cs * (2.0f - 2.0f * kb);
    const float gu = -cs * (2.0f - 2.0f * kb) * kb / kg;
    const float gv = -cs * (2.0f - 2.0f * kr) * kr / kg;
    const float row0[4] = {ys, 0.0f, rv, -ys * yo - rv * c0};
    const float row1[4] = {ys, gu, gv, -ys * yo - (gu + gv) * c0};
    const float row2[4] = {ys, bu, 0.0f, -ys * yo - bu * c0};
    memcpy(cb.row0, row0, sizeof(row0));
    memcpy(cb.row1, row1, sizeof(row1));
    memcpy(cb.row2, row2, sizeof(row2));
}

Renderer::Renderer() = default;

// Hardware adapters in a fixed order (integrated first), independent of the app's GPU preference, which
// the high-performance exports in main.cpp change. Indexes are what Settings > Graphics card stores.
static std::vector<ComPtr<IDXGIAdapter1>> HardwareAdapters()
{
    std::vector<ComPtr<IDXGIAdapter1>> list;
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return list;
    ComPtr<IDXGIFactory6> f6;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0;; ++i, a.Reset()) {
        const HRESULT hr = SUCCEEDED(f.As(&f6)) ? f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_MINIMUM_POWER, IID_PPV_ARGS(&a))
                                                : f->EnumAdapters1(i, &a);
        if (hr == DXGI_ERROR_NOT_FOUND || FAILED(hr)) break;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) list.push_back(a);
    }
    return list;
}

// The adapter that scans out `monitor` (the "display GPU"); null if unknown (e.g. remote session).
// Asked through the display configuration (CCD), because on switchable-graphics laptops DXGI lists the
// integrated GPU's outputs under the discrete GPU for apps that prefer it.
static ComPtr<IDXGIAdapter1> DisplayAdapter(HMONITOR monitor)
{
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) return nullptr;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return nullptr;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return nullptr;
    for (UINT32 i = 0; i < np; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof(sn);
        sn.header.adapterId = paths[i].sourceInfo.adapterId;
        sn.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS || _wcsicmp(sn.viewGdiDeviceName, mi.szDevice) != 0) continue;
        const LUID luid = paths[i].sourceInfo.adapterId;
        for (auto& a : HardwareAdapters()) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) return a;
        }
    }
    return nullptr;
}

std::vector<std::wstring> Renderer::EnumerateAdapters()
{
    std::vector<std::wstring> names;
    for (auto& a : HardwareAdapters()) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        names.push_back(d.Description);
    }
    return names;
}
Renderer::~Renderer() { Shutdown(); }

bool Renderer::Init(HWND hwnd, UINT width, UINT height)
{
    m_hwnd = hwnd;
    m_width = std::max(width, 1u);
    m_height = std::max(height, 1u);
    m_lost = false;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = E_FAIL;
    // Try with video support first (GPU MJPEG decode, video processor for encoding), then without.
    auto create = [&](IDXGIAdapter* adapter, D3D_DRIVER_TYPE type) {
        for (UINT flags : {UINT(D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT), UINT(D3D11_CREATE_DEVICE_BGRA_SUPPORT)}) {
            m_device.Reset();
            m_ctx.Reset();
            hr = D3D11CreateDevice(adapter, type, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &m_device, &fl, &m_ctx);
            if (SUCCEEDED(hr)) return true;
        }
        return false;
    };
    m_warp = false;
    bool ok = false;
    if (m_gpuChoice >= 0) {
        // A specific GPU chosen in Settings (index into EnumerateAdapters()).
        const auto list = HardwareAdapters();
        if (size_t(m_gpuChoice) < list.size()) ok = create(list[size_t(m_gpuChoice)].Get(), D3D_DRIVER_TYPE_UNKNOWN);
        if (!ok) LOGW(L"The chosen GPU could not be used; falling back to automatic");
    }
    if (!ok && m_gpuChoice != -2) {
        // Automatic: the GPU that drives the window's monitor, so presenting never crosses adapters.
        if (ComPtr<IDXGIAdapter1> display = DisplayAdapter(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY)))
            ok = create(display.Get(), D3D_DRIVER_TYPE_UNKNOWN);
        if (!ok) ok = create(nullptr, D3D_DRIVER_TYPE_HARDWARE);
    }
    if (!ok) {
        // No usable hardware GPU (very old GPU, remote session, VM, broken driver): Windows' software renderer.
        ok = create(nullptr, D3D_DRIVER_TYPE_WARP);
        m_warp = ok;
        if (ok && m_gpuChoice != -2) LOGW(L"No usable hardware GPU: using the software renderer (WARP)");
    }
    if (!ok) {
        LOGE(L"D3D11CreateDevice failed: %s", HrToString(hr).c_str());
        return false;
    }
    m_videoSupport = false;
    {
        ComPtr<ID3D11VideoDevice> vd;
        m_videoSupport = SUCCEEDED(m_device.As(&vd));
        UINT support = 0;
        m_yuy2Textures = SUCCEEDED(m_device->CheckFormatSupport(DXGI_FORMAT_YUY2, &support)) && (support & D3D11_FORMAT_SUPPORT_SHADER_LOAD);
        support = 0;
        m_nv12Textures = SUCCEEDED(m_device->CheckFormatSupport(DXGI_FORMAT_NV12, &support)) && (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE);
        m_p010Textures = SUCCEEDED(m_device->CheckFormatSupport(DXGI_FORMAT_P010, &support)) && (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE);
        m_y210Textures = SUCCEEDED(m_device->CheckFormatSupport(DXGI_FORMAT_Y210, &support)) && (support & D3D11_FORMAT_SUPPORT_SHADER_LOAD);
    }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(m_ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIDevice1> dxgiDevice;
    m_device.As(&dxgiDevice);
    dxgiDevice->SetMaximumFrameLatency(1);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);
    m_adapterName = ad.Description;
    m_adapterLuid = ad.AdapterLuid;
    m_vendorId = ad.VendorId;
    m_deviceId = ad.DeviceId;
    m_driverVersion = {};
    adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &m_driverVersion);
    m_probe = SuperResProbe::NotRun;   // a new device (or another graphics card): check again
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) m_tearing = allow != FALSE;
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = m_width;
    sd.Height = m_height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    ComPtr<IDXGISwapChain1> sc1;
    hr = factory->CreateSwapChainForHwnd(m_device.Get(), hwnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        LOGE(L"CreateSwapChainForHwnd failed: %s", HrToString(hr).c_str());
        return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&m_swap);
    m_swap->SetMaximumFrameLatency(1);
    m_waitable = m_swap->GetFrameLatencyWaitableObject();

    D2D1_FACTORY_OPTIONS fo{};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &fo, reinterpret_cast<void**>(m_d2dFactory.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = m_d2dFactory->CreateDevice(dxgiDevice.Get(), &m_d2dDevice);
    if (SUCCEEDED(hr)) hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_d2d);
    if (FAILED(hr)) {
        LOGE(L"Direct2D init failed: %s", HrToString(hr).c_str());
        return false;
    }
    m_d2d->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (!m_dwrite)
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwrite.GetAddressOf()));

    m_device->CreateVertexShader(g_vs_fullscreen, sizeof(g_vs_fullscreen), nullptr, &m_vs);
    m_device->CreatePixelShader(g_ps_yuy2, sizeof(g_ps_yuy2), nullptr, &m_psYuy2);
    m_device->CreatePixelShader(g_ps_nv12, sizeof(g_ps_nv12), nullptr, &m_psNv12);
    m_device->CreatePixelShader(g_ps_bgra, sizeof(g_ps_bgra), nullptr, &m_psBgra);
    m_device->CreatePixelShader(g_ps_present, sizeof(g_ps_present), nullptr, &m_psPresent);
    m_device->CreatePixelShader(g_ps_filter, sizeof(g_ps_filter), nullptr, &m_psFilter);
    m_device->CreatePixelShader(g_ps_bicubic, sizeof(g_ps_bicubic), nullptr, &m_psBicubic);
    m_device->CreatePixelShader(g_ps_upscale, sizeof(g_ps_upscale), nullptr, &m_psUpscale);
    m_device->CreatePixelShader(g_ps_sharpen, sizeof(g_ps_sharpen), nullptr, &m_psSharpen);
    m_device->CreatePixelShader(g_ps_layer, sizeof(g_ps_layer), nullptr, &m_psLayer);
    m_device->CreatePixelShader(g_ps_tohdr, sizeof(g_ps_tohdr), nullptr, &m_psToHdr);
    m_device->CreatePixelShader(g_ps_hdrpic, sizeof(g_ps_hdrpic), nullptr, &m_psHdrPic);
    m_device->CreatePixelShader(g_ps_layerhdr, sizeof(g_ps_layerhdr), nullptr, &m_psLayerHdr);
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 32;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        m_device->CreateBuffer(&bd, nullptr, &m_hdrCb);
    }
    m_device->CreatePixelShader(g_ps_item, sizeof(g_ps_item), nullptr, &m_psItem);
    m_device->CreatePixelShader(g_ps_transition, sizeof(g_ps_transition), nullptr, &m_psTransition);
    {
        D3D11_BLEND_DESC bl{};
        auto& rt = bl.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_ONE;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        m_device->CreateBlendState(&bl, &m_blendPremul);
        D3D11_BUFFER_DESC ib{};
        ib.ByteWidth = sizeof(float) * 28;   // ItemCB
        ib.Usage = D3D11_USAGE_DYNAMIC;
        ib.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ib.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        m_device->CreateBuffer(&ib, nullptr, &m_itemCb);
        ib.ByteWidth = sizeof(float) * 12;
        m_device->CreateBuffer(&ib, nullptr, &m_filterCb);
        ib.ByteWidth = sizeof(float) * 4;
        m_device->CreateBuffer(&ib, nullptr, &m_scaleCb);
        ib.ByteWidth = sizeof(float) * 12;
        m_device->CreateBuffer(&ib, nullptr, &m_sharpenCb);
    }

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    m_device->CreateSamplerState(&smp, &m_linear);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    m_device->CreateSamplerState(&smp, &m_point);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    m_device->CreateRasterizerState(&rd, &m_raster);

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(ConvertCB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    m_device->CreateBuffer(&bd, nullptr, &m_cb);

    if (!m_dxgiManager) MFCreateDXGIDeviceManager(&m_resetToken, &m_dxgiManager);
    if (m_dxgiManager) m_dxgiManager->ResetDevice(m_device.Get(), m_resetToken);

    if (!CreateTargets()) return false;
    LOGI(L"Renderer: %s, feature level %x, tearing %s", m_adapterName.c_str(), unsigned(fl), m_tearing ? L"yes" : L"no");
    return true;
}

void Renderer::Shutdown()
{
    ReleaseSuperRes();
    ReleaseProbe();
    m_vctx.Reset();
    m_vdev.Reset();
    ReleaseTargets();
    if (m_waitable) {
        CloseHandle(m_waitable);
        m_waitable = nullptr;
    }
    m_d2d.Reset();
    m_d2dDevice.Reset();
    m_d2dFactory.Reset();
    m_cb.Reset();
    m_raster.Reset();
    m_linear.Reset();
    m_point.Reset();
    m_vs.Reset();
    m_psYuy2.Reset();
    m_psNv12.Reset();
    m_psBgra.Reset();
    m_psPresent.Reset();
    m_psFilter.Reset();
    m_filterCb.Reset();
    m_psBicubic.Reset();
    m_psUpscale.Reset();
    m_psSharpen.Reset();
    m_psLayer.Reset();
    m_sharpenCb.Reset();
    m_enhanceRts.clear();
    m_scaleCb.Reset();
    m_scaleSrv = nullptr;
    m_psItem.Reset();
    m_psTransition.Reset();
    m_blendPremul.Reset();
    m_itemCb.Reset();
    m_swap.Reset();
    if (m_ctx) {
        m_ctx->ClearState();
        m_ctx->Flush();
    }
    m_ctx.Reset();
    m_device.Reset();
}

static ComPtr<ID3D11Texture2D> MakeTexture(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, bool dynamic, UINT bind);
static ComPtr<ID3D11ShaderResourceView> MakeSrv(ID3D11Device* dev, ID3D11Texture2D* t, DXGI_FORMAT fmt);

bool Renderer::CreateTargets()
{
    ComPtr<ID3D11Texture2D> back;
    HRESULT hr = m_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (FAILED(hr)) return false;
    ComPtr<IDXGISurface> surface;
    if (m_hdrOut) {
        // Everything is drawn into an SDR scene as usual; ComposeHdr brings it to the FP16 swap chain.
        m_device->CreateRenderTargetView(back.Get(), nullptr, &m_swapRtv);
        m_scene = {};
        m_scene.tex = MakeTexture(m_device.Get(), m_width, m_height, DXGI_FORMAT_B8G8R8A8_UNORM, false, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        if (!m_scene.tex || !m_swapRtv) return false;
        m_device->CreateRenderTargetView(m_scene.tex.Get(), nullptr, &m_scene.rtv);
        m_scene.srv = MakeSrv(m_device.Get(), m_scene.tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);
        m_scene.width = m_width;
        m_scene.height = m_height;
        m_backRtv = m_scene.rtv;
        m_scene.tex.As(&surface);
    } else {
        m_device->CreateRenderTargetView(back.Get(), nullptr, &m_backRtv);
        back.As(&surface);
    }
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    hr = m_d2d->CreateBitmapFromDxgiSurface(surface.Get(), &props, &m_d2dTarget);
    if (FAILED(hr)) return false;
    m_d2d->SetTarget(m_d2dTarget.Get());
    // The UI layer, same size as the window.
    m_uiValid = false;
    m_uiTex = MakeTexture(m_device.Get(), m_width, m_height, DXGI_FORMAT_B8G8R8A8_UNORM, false, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    if (m_uiTex) {
        m_uiSrv = MakeSrv(m_device.Get(), m_uiTex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);
        ComPtr<IDXGISurface> uiSurface;
        m_uiTex.As(&uiSurface);
        if (!uiSurface || FAILED(m_d2d->CreateBitmapFromDxgiSurface(uiSurface.Get(), &props, &m_uiBitmap))) {
            m_uiTex.Reset();
            m_uiSrv.Reset();
        }
    }
    return true;
}

void Renderer::ReleaseTargets()
{
    if (m_d2d) m_d2d->SetTarget(nullptr);
    m_d2dTarget.Reset();
    m_uiBitmap.Reset();
    m_uiSrv.Reset();
    m_uiTex.Reset();
    m_uiValid = false;
    m_backRtv.Reset();
    m_swapRtv.Reset();
    m_scene = {};
    m_hdrPics.clear();
    if (m_ctx) {
        m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        m_ctx->Flush();
    }
}

bool Renderer::Resize(UINT width, UINT height)
{
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    if (width == m_width && height == m_height) return true;
    ReleaseTargets();
    HRESULT hr = m_swap->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
                                       DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
    if (FAILED(hr)) {
        LOGE(L"ResizeBuffers failed: %s", HrToString(hr).c_str());
        m_lost = true;
        return false;
    }
    m_width = width;
    m_height = height;
    return CreateTargets();
}

static ComPtr<ID3D11Texture2D> MakeTexture(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, bool dynamic, UINT bind)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    td.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &t))) return nullptr;
    return t;
}

static ComPtr<ID3D11ShaderResourceView> MakeSrv(ID3D11Device* dev, ID3D11Texture2D* t, DXGI_FORMAT fmt)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = fmt;
    d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> v;
    if (FAILED(dev->CreateShaderResourceView(t, &d, &v))) return nullptr;
    return v;
}

bool Renderer::EnsureSurface(VideoSurface& s, PixelLayout layout, UINT w, UINT h, UINT outW, UINT outH, DXGI_FORMAT gpuFormat)
{
    const bool gpu = gpuFormat != DXGI_FORMAT_UNKNOWN;
    if (s.rgb && s.layout == layout && s.srcW == w && s.srcH == h && s.width == outW && s.height == outH && s.gpuFormat == gpuFormat) return true;
    // Keep the standby picture across format/size changes.
    auto lastGood = std::move(s.lastGood);
    auto lastGoodSrv = std::move(s.lastGoodSrv);
    const UINT lgW = s.lastGoodW, lgH = s.lastGoodH;
    const int64_t lgQpc = s.lastGoodQpc;
    s = VideoSurface{};
    s.lastGood = std::move(lastGood);
    s.lastGoodSrv = std::move(lastGoodSrv);
    s.lastGoodW = lgW;
    s.lastGoodH = lgH;
    s.lastGoodQpc = lgQpc;
    if (w == 0 || h == 0 || outW == 0 || outH == 0) return false;
    ID3D11Device* d = m_device.Get();
    s.rgb = MakeTexture(d, outW, outH, DXGI_FORMAT_B8G8R8A8_UNORM, false, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    if (!s.rgb) return false;
    d->CreateRenderTargetView(s.rgb.Get(), nullptr, &s.rgbRtv);
    s.rgbSrv = MakeSrv(d, s.rgb.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);

    switch (layout) {
    case PixelLayout::YUY2:
    case PixelLayout::UYVY:
        if (gpu) {
            // DXGI_FORMAT_YUY2 is viewed as RGBA8 at half width, exactly like the CPU upload texture.
            s.up0 = MakeTexture(d, w, h, gpuFormat, false, D3D11_BIND_SHADER_RESOURCE);
        } else {
            s.up0 = MakeTexture(d, w / 2, h, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D11_BIND_SHADER_RESOURCE);
        }
        if (s.up0) s.srv0 = MakeSrv(d, s.up0.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        break;
    case PixelLayout::BGRA: {
        DXGI_FORMAT f = gpu ? gpuFormat : DXGI_FORMAT_B8G8R8A8_UNORM;
        s.up0 = MakeTexture(d, w, h, f, !gpu, D3D11_BIND_SHADER_RESOURCE);
        if (s.up0) s.srv0 = MakeSrv(d, s.up0.Get(), f);
        break;
    }
    case PixelLayout::Y210:
        if (gpu) {
            s.up0 = MakeTexture(d, w, h, gpuFormat, false, D3D11_BIND_SHADER_RESOURCE);   // viewed as RGBA16 at half width
        } else {
            s.up0 = MakeTexture(d, w / 2, h, DXGI_FORMAT_R16G16B16A16_UNORM, true, D3D11_BIND_SHADER_RESOURCE);
        }
        if (s.up0) s.srv0 = MakeSrv(d, s.up0.Get(), DXGI_FORMAT_R16G16B16A16_UNORM);
        break;
    case PixelLayout::P010:
        if (gpu) {
            s.up0 = MakeTexture(d, w, h, DXGI_FORMAT_P010, false, D3D11_BIND_SHADER_RESOURCE);
            if (s.up0) {
                s.srv0 = MakeSrv(d, s.up0.Get(), DXGI_FORMAT_R16_UNORM);
                s.srv1 = MakeSrv(d, s.up0.Get(), DXGI_FORMAT_R16G16_UNORM);
            }
        } else {
            s.up0 = MakeTexture(d, w, h, DXGI_FORMAT_R16_UNORM, true, D3D11_BIND_SHADER_RESOURCE);
            s.up1 = MakeTexture(d, w / 2, h / 2, DXGI_FORMAT_R16G16_UNORM, true, D3D11_BIND_SHADER_RESOURCE);
            if (s.up0) s.srv0 = MakeSrv(d, s.up0.Get(), DXGI_FORMAT_R16_UNORM);
            if (s.up1) s.srv1 = MakeSrv(d, s.up1.Get(), DXGI_FORMAT_R16G16_UNORM);
        }
        break;
    case PixelLayout::NV12:
        if (gpu) {
            s.up0 = MakeTexture(d, w, h, DXGI_FORMAT_NV12, false, D3D11_BIND_SHADER_RESOURCE);
            if (s.up0) {
                s.srv0 = MakeSrv(d, s.up0.Get(), DXGI_FORMAT_R8_UNORM);
                s.srv1 = MakeSrv(d, s.up0.Get(), DXGI_FORMAT_R8G8_UNORM);
            }
        } else {
            s.up0 = MakeTexture(d, w, h, DXGI_FORMAT_R8_UNORM, true, D3D11_BIND_SHADER_RESOURCE);
            s.up1 = MakeTexture(d, w / 2, h / 2, DXGI_FORMAT_R8G8_UNORM, true, D3D11_BIND_SHADER_RESOURCE);
            if (s.up0) s.srv0 = MakeSrv(d, s.up0.Get(), DXGI_FORMAT_R8_UNORM);
            if (s.up1) s.srv1 = MakeSrv(d, s.up1.Get(), DXGI_FORMAT_R8G8_UNORM);
        }
        break;
    default: return false;
    }
    if (!s.srv0 || ((layout == PixelLayout::NV12 || layout == PixelLayout::P010) && !s.srv1)) {
        LOGE(L"Could not create textures for %ux%u", w, h);
        s = VideoSurface{};
        return false;
    }
    s.layout = layout;
    s.width = outW;
    s.height = outH;
    s.srcW = w;
    s.srcH = h;
    s.gpu = gpu;
    s.gpuFormat = gpuFormat;
    return true;
}

static void CopyRows(BYTE* dst, UINT dstPitch, const BYTE* src, LONG srcPitch, UINT rowBytes, UINT rows)
{
    if (LONG(dstPitch) == srcPitch && dstPitch == rowBytes) {
        memcpy(dst, src, size_t(rowBytes) * rows);
        return;
    }
    for (UINT r = 0; r < rows; ++r) memcpy(dst + size_t(r) * dstPitch, src + ptrdiff_t(r) * srcPitch, rowBytes);
}

bool Renderer::Upload(VideoSurface& s, const CapturedFrame& f, ColorMatrix matrixOverride, ColorRange rangeOverride, const PictureTransform& xf,
                      const HdrIn& hdr)
{
    if (!f.sample || f.layout == PixelLayout::None) return false;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(f.sample->GetBufferByIndex(0, &buf))) return false;

    const UINT w = f.width, h = f.height;

    // Frames that already live on the GPU (hardware MJPEG decode, or the camera frame server once a
    // D3D manager has been attached) are copied GPU-side; anything else is locked and uploaded.
    ComPtr<IMFDXGIBuffer> dxgiBuf;
    ComPtr<ID3D11Texture2D> srcTex;
    UINT srcSub = 0;
    DXGI_FORMAT gpuFormat = DXGI_FORMAT_UNKNOWN;
    if (SUCCEEDED(buf.As(&dxgiBuf)) && SUCCEEDED(dxgiBuf->GetResource(IID_PPV_ARGS(&srcTex))) &&
        SUCCEEDED(dxgiBuf->GetSubresourceIndex(&srcSub))) {
        D3D11_TEXTURE2D_DESC td{};
        srcTex->GetDesc(&td);
        ComPtr<ID3D11Device> srcDevice;
        srcTex->GetDevice(&srcDevice);
        const bool sameDevice = srcDevice.Get() == m_device.Get();
        const bool usable = (f.layout == PixelLayout::NV12 && td.Format == DXGI_FORMAT_NV12 && m_nv12Textures) ||
                            (f.layout == PixelLayout::YUY2 && td.Format == DXGI_FORMAT_YUY2 && m_yuy2Textures) ||
                            (f.layout == PixelLayout::P010 && td.Format == DXGI_FORMAT_P010 && m_p010Textures) ||
                            (f.layout == PixelLayout::Y210 && td.Format == DXGI_FORMAT_Y210 && m_y210Textures) ||
                            (f.layout == PixelLayout::BGRA && (td.Format == DXGI_FORMAT_B8G8R8A8_UNORM || td.Format == DXGI_FORMAT_B8G8R8X8_UNORM));
        if (sameDevice && usable && td.Width >= w && td.Height >= h) gpuFormat = td.Format;
    }
    // Crop in even pixels (encoders want even sizes), never below 64 pixels of picture.
    auto even = [](float part, UINT n) { return UINT(std::lround(std::clamp(part, 0.0f, 0.45f) * float(n) / 2.0f)) * 2; };
    UINT x0 = even(xf.crop[0], w), x1 = even(xf.crop[2], w), y0 = even(xf.crop[1], h), y1 = even(xf.crop[3], h);
    if (x0 + x1 + 64 > w) x0 = x1 = 0;
    if (y0 + y1 + 64 > h) y0 = y1 = 0;
    const UINT cw = w - x0 - x1, ch = h - y0 - y1;
    const int rot = ((xf.rotate % 4) + 4) % 4;
    const UINT outW = (rot & 1) ? ch : cw, outH = (rot & 1) ? cw : ch;
    if (!EnsureSurface(s, f.layout, w, h, outW, outH, gpuFormat)) return false;
    s.rotated = (rot & 1) != 0;
    s.xf = xf;

    if (gpuFormat != DXGI_FORMAT_UNKNOWN) {
        D3D11_BOX box{0, 0, 0, w & ~1u, h & ~1u, 1};
        m_ctx->CopySubresourceRegion(s.up0.Get(), 0, 0, 0, 0, srcTex.Get(), srcSub, &box);
    } else {
        ComPtr<IMF2DBuffer> buf2d;
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        DWORD length = 0;
        BYTE* base = nullptr;
        bool locked2d = false;
        if (SUCCEEDED(buf.As(&buf2d)) && SUCCEEDED(buf2d->Lock2D(&scan0, &pitch))) {
            locked2d = true;
            buf2d->GetContiguousLength(&length);
        } else {
            DWORD maxLen = 0;
            if (FAILED(buf->Lock(&base, &maxLen, &length))) return false;
            pitch = f.stride;
            if (pitch == 0)
                pitch = LONG((f.layout == PixelLayout::BGRA || f.layout == PixelLayout::Y210) ? w * 4
                             : f.layout == PixelLayout::NV12                                ? w
                                                                                            : w * 2);   // YUY2 / UYVY / P010
            scan0 = pitch < 0 ? base + size_t(-pitch) * (h - 1) : base;
        }

        D3D11_MAPPED_SUBRESOURCE m{};
        bool ok = true;
        switch (f.layout) {
        case PixelLayout::YUY2:
        case PixelLayout::UYVY:
        case PixelLayout::Y210:
        case PixelLayout::BGRA: {
            const UINT rowBytes = (f.layout == PixelLayout::BGRA || f.layout == PixelLayout::Y210) ? w * 4 : w * 2;
            if (length && size_t(std::abs(pitch)) * (h - 1) + rowBytes > length) { ok = false; break; }
            if (SUCCEEDED(m_ctx->Map(s.up0.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                CopyRows(static_cast<BYTE*>(m.pData), m.RowPitch, scan0, pitch, rowBytes, h);
                m_ctx->Unmap(s.up0.Get(), 0);
            }
            break;
        }
        case PixelLayout::NV12:
        case PixelLayout::P010: {
            const UINT bpp = f.layout == PixelLayout::P010 ? 2 : 1;   // bytes per sample
            UINT planeRows = h;
            if (length && pitch > 0) planeRows = std::max(h, UINT(size_t(length) * 2 / 3 / size_t(pitch)));
            const BYTE* uvPlane = scan0 + size_t(pitch) * planeRows;
            if (length && size_t(pitch) * planeRows + size_t(pitch) * (h / 2 - 1) + w * bpp > length) { ok = false; break; }
            if (SUCCEEDED(m_ctx->Map(s.up0.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                CopyRows(static_cast<BYTE*>(m.pData), m.RowPitch, scan0, pitch, w * bpp, h);
                m_ctx->Unmap(s.up0.Get(), 0);
            }
            if (SUCCEEDED(m_ctx->Map(s.up1.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                CopyRows(static_cast<BYTE*>(m.pData), m.RowPitch, uvPlane, pitch, w * bpp, h / 2);
                m_ctx->Unmap(s.up1.Get(), 0);
            }
            break;
        }
        default: ok = false; break;
        }
        if (locked2d) buf2d->Unlock2D();
        else buf->Unlock();
        if (!ok) return false;
    }

    // Resolve the color space: user override > what the pipeline reported > sensible defaults.
    // HDR: as reported, or as set for the device (cards that pass HDR in 8-bit formats send no flag).
    const Transfer tf = hdr.mode == 1 ? Transfer::Sdr : hdr.mode == 2 ? Transfer::Pq : hdr.mode == 3 ? Transfer::Hlg : f.transfer;
    const bool tenBit = f.layout == PixelLayout::P010 || f.layout == PixelLayout::Y210;
    ColorMatrix cm = matrixOverride != ColorMatrix::Auto ? matrixOverride
                   : hdr.mode >= 2                       ? ColorMatrix::BT2020   // set to HDR: the card's own (SDR) tag is wrong
                   : f.matrix != ColorMatrix::Auto       ? f.matrix
                   : tf != Transfer::Sdr                 ? ColorMatrix::BT2020   // HDR10 / HLG are BT.2020
                   : (h >= 720 ? ColorMatrix::BT709 : ColorMatrix::BT601);
    if (f.decoded && matrixOverride == ColorMatrix::Auto && f.matrix == ColorMatrix::Auto) cm = ColorMatrix::BT601;   // JPEG is BT.601
    ColorRange cr = rangeOverride != ColorRange::Auto ? rangeOverride
                  : f.range != ColorRange::Auto       ? f.range
                  : (f.decoded ? ColorRange::Full : ColorRange::Limited);
    s.matrix = cm;
    s.range = cr;

    ConvertCB cb{};
    ComputeMatrix(cm, cr, cb, tenBit);
    s.transfer = tf;
    cb.hdr[0] = tf == Transfer::Pq ? 1.0f : (tf == Transfer::Hlg ? 2.0f : 0.0f);
    cb.hdr[1] = tf != Transfer::Sdr && (f.bt2020 || cm == ColorMatrix::BT2020 || hdr.mode >= 2) ? 1.0f : 0.0f;
    cb.hdr[2] = std::clamp(hdr.whiteNits, 50.0f, 1000.0f);
    cb.hdr[3] = std::clamp(hdr.peakNits, cb.hdr[2] * 1.05f, 10000.0f);
    cb.size[0] = w;
    cb.size[1] = h;
    cb.flags = f.layout == PixelLayout::UYVY ? 1u : 0u;
    cb.xform[0] = INT(x0);
    cb.xform[1] = INT(y0);
    cb.xform[2] = INT(cw);
    cb.xform[3] = INT(ch);
    cb.xform2[0] = UINT(rot);
    cb.xform2[1] = xf.mirror ? 1u : 0u;
    cb.xform2[2] = outW;
    cb.xform2[3] = outH;
    D3D11_MAPPED_SUBRESOURCE cm2{};
    if (SUCCEEDED(m_ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cm2))) {
        memcpy(cm2.pData, &cb, sizeof(cb));
        m_ctx->Unmap(m_cb.Get(), 0);
    }

    // HDR screen + HDR signal: the linear copy is written in the same pass (second target).
    const bool wantLin = (m_hdrOut || s.keepLinear) && tf != Transfer::Sdr;
    if (wantLin) {
        D3D11_TEXTURE2D_DESC ld{};
        if (s.hdrLin) s.hdrLin->GetDesc(&ld);
        if (!s.hdrLin || ld.Width != outW || ld.Height != outH) {
            s.hdrLin = MakeTexture(m_device.Get(), outW, outH, DXGI_FORMAT_R16G16B16A16_FLOAT, false, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
            s.hdrLinRtv.Reset();
            s.hdrLinSrv.Reset();
            if (s.hdrLin) {
                m_device->CreateRenderTargetView(s.hdrLin.Get(), nullptr, &s.hdrLinRtv);
                s.hdrLinSrv = MakeSrv(m_device.Get(), s.hdrLin.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
            }
        }
    } else if (s.hdrLin) {   // light: nothing kept while not needed
        s.hdrLin.Reset();
        s.hdrLinRtv.Reset();
        s.hdrLinSrv.Reset();
    }
    s.hdrWhite = cb.hdr[2];
    s.hdrPeak = cb.hdr[3];
    ID3D11RenderTargetView* rtvs[2] = {s.rgbRtv.Get(), s.hdrLinRtv.Get()};
    m_ctx->OMSetRenderTargets(wantLin && s.hdrLinRtv ? 2 : 1, rtvs, nullptr);
    D3D11_VIEWPORT vp{0, 0, float(outW), float(outH), 0, 1};
    m_ctx->RSSetViewports(1, &vp);
    D3D11_RECT sc{0, 0, LONG(outW), LONG(outH)};
    m_ctx->RSSetScissorRects(1, &sc);
    m_ctx->RSSetState(m_raster.Get());
    m_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ID3D11PixelShader* ps = (f.layout == PixelLayout::NV12 || f.layout == PixelLayout::P010) ? m_psNv12.Get()
                          : f.layout == PixelLayout::BGRA                                    ? m_psBgra.Get()
                                                                                             : m_psYuy2.Get();   // YUY2 / UYVY / Y210
    m_ctx->PSSetShader(ps, nullptr, 0);
    ID3D11Buffer* cbs[] = {m_cb.Get()};
    m_ctx->PSSetConstantBuffers(0, 1, cbs);
    ID3D11ShaderResourceView* srvs[] = {s.srv0.Get(), s.srv1.Get()};
    m_ctx->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* samplers[] = {m_linear.Get()};
    m_ctx->PSSetSamplers(0, 1, samplers);
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[2] = {};
    m_ctx->PSSetShaderResources(0, 2, nulls);
    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);   // the linear copy is read as a texture next
    s.hasImage = true;
    return true;
}

void Renderer::BeginFrame()
{
    const float black[4] = {0, 0, 0, 1};
    m_hdrPics.clear();
    m_uiDeferred = false;
    m_ctx->ClearRenderTargetView(m_backRtv.Get(), black);
}

void Renderer::DrawSurface(const VideoSurface& s, const RectF& dest, const RectF& clip, bool smooth)
{
    if (!s.hasImage) return;
    DrawTexture(s.rgbSrv.Get(), dest, clip, smooth);
}

// ---------------------------------------------------------------------------------------------
// Offscreen composition

struct ItemCB {
    float uvRect[4];
    float solid[4];
    float keyColor[4];
    float keyParams[4];
    float adjust0[4];
    float adjust1[4];
    float misc[4];
};

bool Renderer::EnsureTarget(RenderTarget& t, UINT w, UINT h)
{
    if (t.tex && t.width == w && t.height == h) return true;
    t = RenderTarget{};
    t.tex = MakeTexture(m_device.Get(), w, h, DXGI_FORMAT_B8G8R8A8_UNORM, false, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    if (!t.tex) return false;
    m_device->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv);
    t.srv = MakeSrv(m_device.Get(), t.tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);
    t.width = w;
    t.height = h;
    return t.rtv && t.srv;
}

void Renderer::BeginTarget(RenderTarget& t, float r, float g, float b)
{
    const float c[4] = {r, g, b, 1};
    m_ctx->ClearRenderTargetView(t.rtv.Get(), c);
}

void Renderer::DrawItem(const RenderTarget& t, const ItemDraw& d)
{
    if (d.dest.w <= 0 || d.dest.h <= 0) return;
    ItemCB cb{};
    const float uv[4] = {d.u0, d.v0, d.u1, d.v1};
    memcpy(cb.uvRect, uv, sizeof(uv));
    memcpy(cb.solid, d.solid, sizeof(cb.solid));
    cb.keyParams[3] = d.premultiplied ? 1.0f : 0.0f;
    cb.adjust0[1] = 1.0f;
    cb.adjust0[2] = 1.0f;
    cb.adjust0[3] = 1.0f;
    if (const ItemFilters* f = d.filters) {
        if (f->key.enabled) {
            cb.keyColor[0] = float((f->key.color >> 16) & 0xFF) / 255.0f;
            cb.keyColor[1] = float((f->key.color >> 8) & 0xFF) / 255.0f;
            cb.keyColor[2] = float(f->key.color & 0xFF) / 255.0f;
            cb.keyColor[3] = 1.0f;
            cb.keyParams[0] = f->key.similarity;
            cb.keyParams[1] = f->key.smoothness;
            cb.keyParams[2] = f->key.spill;
        }
        if (f->color.enabled) {
            cb.adjust0[0] = f->color.brightness;
            cb.adjust0[1] = f->color.contrast;
            cb.adjust0[2] = f->color.saturation;
            cb.adjust0[3] = f->color.gamma;
            cb.adjust1[0] = f->color.hue * 3.14159265f / 180.0f;
            cb.adjust1[1] = 1.0f;
        }
        cb.adjust1[2] = f->sharpen;
    }
    cb.misc[0] = Clamp(d.opacity, 0.0f, 1.0f);
    cb.misc[1] = d.srv ? 0.0f : 1.0f;
    cb.misc[2] = d.srcW ? 1.0f / float(d.srcW) : 0.0f;
    cb.misc[3] = d.srcH ? 1.0f / float(d.srcH) : 0.0f;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(m_ctx->Map(m_itemCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, &cb, sizeof(cb));
    m_ctx->Unmap(m_itemCb.Get(), 0);

    ID3D11RenderTargetView* rtv = t.rtv.Get();
    m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{d.dest.x, d.dest.y, d.dest.w, d.dest.h, 0, 1};
    m_ctx->RSSetViewports(1, &vp);
    D3D11_RECT sc{0, 0, LONG(t.width), LONG(t.height)};
    m_ctx->RSSetScissorRects(1, &sc);
    m_ctx->RSSetState(m_raster.Get());
    m_ctx->OMSetBlendState(m_blendPremul.Get(), nullptr, 0xFFFFFFFF);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_psItem.Get(), nullptr, 0);
    ID3D11Buffer* cbs[2] = {nullptr, m_itemCb.Get()};
    m_ctx->PSSetConstantBuffers(0, 2, cbs);
    ID3D11ShaderResourceView* srv = d.srv;
    m_ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = m_linear.Get();
    m_ctx->PSSetSamplers(0, 1, &smp);
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    m_ctx->PSSetShaderResources(0, 1, &nullSrv);
    m_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
}

void Renderer::DrawTransition(RenderTarget& out, const RenderTarget& a, const RenderTarget& b, float progress, int mode)
{
    ItemCB cb{};
    cb.uvRect[0] = Clamp(progress, 0.0f, 1.0f);
    cb.uvRect[1] = float(mode);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(m_ctx->Map(m_itemCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, &cb, sizeof(cb));
    m_ctx->Unmap(m_itemCb.Get(), 0);
    ID3D11RenderTargetView* rtv = out.rtv.Get();
    m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, float(out.width), float(out.height), 0, 1};
    m_ctx->RSSetViewports(1, &vp);
    D3D11_RECT sc{0, 0, LONG(out.width), LONG(out.height)};
    m_ctx->RSSetScissorRects(1, &sc);
    m_ctx->RSSetState(m_raster.Get());
    m_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_psTransition.Get(), nullptr, 0);
    ID3D11Buffer* cbs[2] = {nullptr, m_itemCb.Get()};
    m_ctx->PSSetConstantBuffers(0, 2, cbs);
    ID3D11ShaderResourceView* srvs[2] = {a.srv.Get(), b.srv.Get()};
    m_ctx->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* smp = m_linear.Get();
    m_ctx->PSSetSamplers(0, 1, &smp);
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[2] = {};
    m_ctx->PSSetShaderResources(0, 2, nulls);
}

void Renderer::DrawTexture(ID3D11ShaderResourceView* srvIn, const RectF& dest, const RectF& clip, bool smooth, ID3D11PixelShader* ps)
{
    if (!srvIn || dest.w <= 0 || dest.h <= 0) return;
    ID3D11RenderTargetView* rtv = m_redirectRtv ? m_redirectRtv : m_backRtv.Get();
    const float tw = float(m_redirectRtv ? m_redirectW : m_width), th = float(m_redirectRtv ? m_redirectH : m_height);
    m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{dest.x, dest.y, dest.w, dest.h, 0, 1};
    m_ctx->RSSetViewports(1, &vp);
    D3D11_RECT sc{LONG(std::floor(std::max(0.0f, clip.x))), LONG(std::floor(std::max(0.0f, clip.y))),
                  LONG(std::ceil(std::min(tw, clip.Right()))), LONG(std::ceil(std::min(th, clip.Bottom())))};
    m_ctx->RSSetScissorRects(1, &sc);
    m_ctx->RSSetState(m_raster.Get());
    m_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    if (!ps && smooth && m_srWanted && !m_redirectRtv && DrawSuperRes(srvIn, dest, clip)) return;   // main window only
    if (!ps && smooth && m_enhanceMode > 0 && DrawEnhanced(srvIn, dest, clip, rtv, tw, th)) return;
    // Smooth scaling at a size other than 1:1 uses sharp bicubic; 1:1 and "pixel" scaling stay exact.
    // Software rendering (WARP) keeps plain bilinear: bicubic costs 9 fetches per pixel on the CPU.
    if (!ps && smooth && m_psBicubic && m_scaleCb && !m_warp) {
        if (srvIn != m_scaleSrv) {
            ComPtr<ID3D11Resource> res;
            srvIn->GetResource(&res);
            ComPtr<ID3D11Texture2D> t2;
            D3D11_TEXTURE2D_DESC td{};
            if (SUCCEEDED(res.As(&t2))) t2->GetDesc(&td);
            m_scaleW = float(td.Width);
            m_scaleH = float(td.Height);
            m_scaleSrv = srvIn;
        }
        if (m_scaleW > 0 && (std::fabs(dest.w - m_scaleW) > 0.5f || std::fabs(dest.h - m_scaleH) > 0.5f)) {
            const float cb[4] = {m_scaleW, m_scaleH, 0, 0};
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(m_ctx->Map(m_scaleCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                memcpy(m.pData, cb, sizeof(cb));
                m_ctx->Unmap(m_scaleCb.Get(), 0);
                ID3D11Buffer* b = m_scaleCb.Get();
                m_ctx->PSSetConstantBuffers(1, 1, &b);
                ps = m_psBicubic.Get();
            }
        }
    }
    m_ctx->PSSetShader(ps ? ps : m_psPresent.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = srvIn;
    m_ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = smooth ? m_linear.Get() : m_point.Get();
    m_ctx->PSSetSamplers(0, 1, &smp);
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    m_ctx->PSSetShaderResources(0, 1, &nullSrv);
}

void Renderer::SetEnhance(int mode, float strength, bool keepCache)
{
    m_enhanceMode = std::clamp(mode, 0, 2);
    m_enhanceStrength = std::clamp(strength, 0.0f, 1.0f);
    if (!m_enhanceMode && !keepCache) m_enhanceRts.clear();   // light: nothing kept while off
}

bool Renderer::TextureSize(ID3D11ShaderResourceView* srv, float& w, float& h)
{
    if (srv != m_scaleSrv) {
        ComPtr<ID3D11Resource> res;
        srv->GetResource(&res);
        ComPtr<ID3D11Texture2D> t2;
        D3D11_TEXTURE2D_DESC td{};
        if (SUCCEEDED(res.As(&t2))) t2->GetDesc(&td);
        m_scaleW = float(td.Width);
        m_scaleH = float(td.Height);
        m_scaleSrv = srv;
    }
    w = m_scaleW;
    h = m_scaleH;
    return w > 0 && h > 0;
}

// One 1:1 sharpening pass: output pixel (x, y) of the viewport reads source pixel origin + (x, y), with
// neighbours clamped to [lo, hi].
void Renderer::Sharpen(ID3D11ShaderResourceView* srv, const D3D11_VIEWPORT& vp, const float origin[2], const float lo[2], const float hi[2])
{
    const float cb[12] = {origin[0], origin[1], vp.Width, vp.Height, lo[0], lo[1], hi[0], hi[1], m_enhanceStrength, 0, 0, 0};
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(m_ctx->Map(m_sharpenCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, cb, sizeof(cb));
    m_ctx->Unmap(m_sharpenCb.Get(), 0);
    ID3D11Buffer* b = m_sharpenCb.Get();
    m_ctx->PSSetConstantBuffers(1, 1, &b);
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->PSSetShader(m_psSharpen.Get(), nullptr, 0);
    m_ctx->PSSetShaderResources(0, 1, &srv);
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    m_ctx->PSSetShaderResources(0, 1, &none);
}

// Enhance: 1:1 → sharpen straight onto the output. Larger → sharpen at the source size (4x fewer pixels than
// the output for a 2x upscale), then upscale that onto the output: edge-aware (mode 2) or bicubic (mode 1).
// Returns false to fall back to the normal draw (downscaling, or a resource missing).
bool Renderer::DrawEnhanced(ID3D11ShaderResourceView* srv, const RectF& dest, const RectF& clip, ID3D11RenderTargetView* rtv, float tw, float th)
{
    float sw = 0, sh = 0;
    if (!m_psSharpen || !m_psUpscale || !m_sharpenCb || !m_scaleCb || !TextureSize(srv, sw, sh)) return false;
    if (dest.w < sw - 0.5f || dest.h < sh - 0.5f) return false;
    if (std::max(0.0f, std::max(clip.x, dest.x)) >= std::min(tw, std::min(clip.Right(), dest.Right())) ||
        std::max(0.0f, std::max(clip.y, dest.y)) >= std::min(th, std::min(clip.Bottom(), dest.Bottom())))
        return true;   // nothing visible
    m_ctx->PSSetSamplers(0, 1, m_linear.GetAddressOf());
    const float origin[2] = {0, 0}, lo[2] = {0, 0}, hi[2] = {sw - 1, sh - 1};
    if (dest.w < sw + 0.5f && dest.h < sh + 0.5f) {
        Sharpen(srv, {std::round(dest.x), std::round(dest.y), sw, sh, 0, 1}, origin, lo, hi);
        return true;
    }
    // Sharpened copy at the source size (cached per size: multiview tiles may differ).
    const UINT iw = UINT(sw), ih = UINT(sh);
    RenderTarget* t = nullptr;
    for (auto& r : m_enhanceRts)
        if (r.width == iw && r.height == ih) t = &r;
    if (!t) {
        if (m_enhanceRts.size() >= 4) m_enhanceRts.erase(m_enhanceRts.begin());
        m_enhanceRts.emplace_back();
        t = &m_enhanceRts.back();
        if (!EnsureTarget(*t, iw, ih)) {
            m_enhanceRts.pop_back();
            return false;
        }
    }
    ID3D11RenderTargetView* scratch = t->rtv.Get();
    m_ctx->OMSetRenderTargets(1, &scratch, nullptr);
    const D3D11_RECT all{0, 0, LONG(iw), LONG(ih)};
    m_ctx->RSSetScissorRects(1, &all);
    Sharpen(srv, {0, 0, sw, sh, 0, 1}, origin, lo, hi);
    // Upscale onto the output with the caller's clip.
    const float cb[4] = {sw, sh, 0, 0};
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(m_ctx->Map(m_scaleCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, cb, sizeof(cb));
    m_ctx->Unmap(m_scaleCb.Get(), 0);
    ID3D11Buffer* b = m_scaleCb.Get();
    m_ctx->PSSetConstantBuffers(1, 1, &b);
    m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D11_RECT sc{LONG(std::floor(std::max(0.0f, clip.x))), LONG(std::floor(std::max(0.0f, clip.y))), LONG(std::ceil(std::min(tw, clip.Right()))),
                        LONG(std::ceil(std::min(th, clip.Bottom())))};
    m_ctx->RSSetScissorRects(1, &sc);
    const D3D11_VIEWPORT vp{dest.x, dest.y, dest.w, dest.h, 0, 1};
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->PSSetShader(m_enhanceMode == 2 ? m_psUpscale.Get() : m_psBicubic.Get(), nullptr, 0);
    ID3D11ShaderResourceView* sharpened = t->srv.Get();
    m_ctx->PSSetShaderResources(0, 1, &sharpened);
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    m_ctx->PSSetShaderResources(0, 1, &none);
    return true;
}

void Renderer::DrawTextureCropped(ID3D11ShaderResourceView* srv, const float uv[4], const RectF& dest, const RectF& clip, bool smooth,
                                  int filter, UINT srcW, UINT srcH, float virtLines, float strength)
{
    const float uw = uv[2] - uv[0], vh = uv[3] - uv[1];
    if (uw <= 0.0f || vh <= 0.0f) return;
    // Draw the whole texture into a larger rectangle that puts the uv part exactly on `dest`, then
    // scissor to `dest` (no extra shader needed).
    RectF full{dest.x - uv[0] * dest.w / uw, dest.y - uv[1] * dest.h / vh, dest.w / uw, dest.h / vh};
    RectF c{std::max(dest.x, clip.x), std::max(dest.y, clip.y), 0, 0};
    c.w = std::min(dest.Right(), clip.Right()) - c.x;
    c.h = std::min(dest.Bottom(), clip.Bottom()) - c.y;
    if (c.w <= 0 || c.h <= 0) return;
    if (filter <= 0 || !m_psFilter || !m_filterCb || srcW == 0 || srcH == 0) {
        DrawTexture(srv, full, c, smooth);
        return;
    }
    struct {
        float src[2], dst[2], virt[2];
        UINT mode;
        float strength;
        float pad[4];
    } cb{};
    cb.src[0] = float(srcW);
    cb.src[1] = float(srcH);
    cb.dst[0] = full.w;
    cb.dst[1] = full.h;
    const float lines = virtLines > 0 ? virtLines : float(srcH);
    cb.virt[0] = std::max(1.0f, std::round(lines * srcW / srcH));
    cb.virt[1] = lines;
    cb.mode = UINT(filter);
    cb.strength = strength;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(m_ctx->Map(m_filterCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, &cb, sizeof(cb));
    m_ctx->Unmap(m_filterCb.Get(), 0);
    ID3D11Buffer* b = m_filterCb.Get();
    m_ctx->PSSetConstantBuffers(1, 1, &b);
    DrawTexture(srv, full, c, true, m_psFilter.Get());
}

void Renderer::DetectBars(VideoSurface& s)
{
    if (!s.hasImage) return;
    constexpr UINT bw = 192, bh = 108;
    if (!s.bars) {
        s.bars = MakeTexture(m_device.Get(), bw, bh, DXGI_FORMAT_B8G8R8A8_UNORM, false, D3D11_BIND_RENDER_TARGET);
        if (!s.bars) return;
        m_device->CreateRenderTargetView(s.bars.Get(), nullptr, &s.barsRtv);
        D3D11_TEXTURE2D_DESC td{};
        s.bars->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& st : s.barsStaging) m_device->CreateTexture2D(&td, nullptr, &st);
    }
    const int read = s.barsIndex ^ 1;
    if (s.barsPending[read] && s.barsStaging[read]) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(m_ctx->Map(s.barsStaging[read].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
            // A row/column is "bar" when every sample in it is black.
            constexpr uint8_t thr = 13;   // ~5 %
            bool rowContent[bh] = {}, colContent[bw] = {};
            for (UINT y = 0; y < bh; ++y) {
                const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
                for (UINT x = 0; x < bw; ++x) {
                    const uint8_t* p = row + x * 4;
                    if (p[0] > thr || p[1] > thr || p[2] > thr) {
                        rowContent[y] = true;
                        colContent[x] = true;
                    }
                }
            }
            m_ctx->Unmap(s.barsStaging[read].Get(), 0);
            s.barsPending[read] = false;
            int top = 0, bottom = int(bh) - 1, left = 0, right = int(bw) - 1;
            while (top < int(bh) && !rowContent[top]) ++top;
            while (bottom >= 0 && !rowContent[bottom]) --bottom;
            while (left < int(bw) && !colContent[left]) ++left;
            while (right >= 0 && !colContent[right]) --right;
            s.barsValid = top <= bottom && left <= right;
            if (s.barsValid) {
                // Bars are symmetric in practice; using the smaller side keeps dark scenes from being
                // cropped. Half a sample of margin so content is never cut.
                const int v = std::min(top, int(bh) - 1 - bottom), h = std::min(left, int(bw) - 1 - right);
                const float vb = v >= 2 ? (v - 0.5f) / bh : 0.0f, hb = h >= 2 ? (h - 0.5f) / bw : 0.0f;
                s.barsRect[0] = hb;
                s.barsRect[1] = vb;
                s.barsRect[2] = 1.0f - hb;
                s.barsRect[3] = 1.0f - vb;
            }
            ++s.barsSeq;
        }
    }
    ID3D11RenderTargetView* rtv = s.barsRtv.Get();
    m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, float(bw), float(bh), 0, 1};
    m_ctx->RSSetViewports(1, &vp);
    D3D11_RECT sc{0, 0, LONG(bw), LONG(bh)};
    m_ctx->RSSetScissorRects(1, &sc);
    m_ctx->RSSetState(m_raster.Get());
    m_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_psPresent.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = s.rgbSrv.Get();
    m_ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = m_linear.Get();
    m_ctx->PSSetSamplers(0, 1, &smp);
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    m_ctx->PSSetShaderResources(0, 1, &nullSrv);
    if (s.barsStaging[s.barsIndex]) {
        m_ctx->CopyResource(s.barsStaging[s.barsIndex].Get(), s.bars.Get());
        s.barsPending[s.barsIndex] = true;
    }
    s.barsIndex ^= 1;
}

bool Renderer::CreateOutput(OutputWindow& o, HWND hwnd, UINT width, UINT height, bool composition)
{
    o = OutputWindow{};
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(m_device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = std::max(width, 1u);
    sd.Height = std::max(height, 1u);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    HRESULT hr = S_OK;
    if (composition) {
        sd.Scaling = DXGI_SCALING_STRETCH;
        hr = factory->CreateSwapChainForComposition(m_device.Get(), &sd, nullptr, &o.swap);
        if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&o.dcomp));
        if (SUCCEEDED(hr)) hr = o.dcomp->CreateTargetForHwnd(hwnd, TRUE, &o.target);
        if (SUCCEEDED(hr)) hr = o.dcomp->CreateVisual(&o.visual);
        if (SUCCEEDED(hr)) hr = o.dcomp->CreateEffectGroup(&o.effect);
        if (SUCCEEDED(hr)) hr = o.visual->SetContent(o.swap.Get());
        if (SUCCEEDED(hr)) hr = o.visual->SetEffect(o.effect.Get());
        if (SUCCEEDED(hr)) hr = o.target->SetRoot(o.visual.Get());
        if (SUCCEEDED(hr)) hr = o.dcomp->Commit();
        if (FAILED(hr)) {   // no composition (old system): an ordinary, always opaque window
            LOGW(L"Composition output unavailable (%s): opaque window", HrToString(hr).c_str());
            o = OutputWindow{};
            sd.Scaling = DXGI_SCALING_STRETCH;
            hr = factory->CreateSwapChainForHwnd(m_device.Get(), hwnd, &sd, nullptr, nullptr, &o.swap);
        }
    } else {
        hr = factory->CreateSwapChainForHwnd(m_device.Get(), hwnd, &sd, nullptr, nullptr, &o.swap);
    }
    if (FAILED(hr)) {
        LOGW(L"Output window swap chain: %s", HrToString(hr).c_str());
        return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    o.width = sd.Width;
    o.height = sd.Height;
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(o.swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    return SUCCEEDED(m_device->CreateRenderTargetView(back.Get(), nullptr, &o.rtv));
}

bool Renderer::ResizeOutput(OutputWindow& o, UINT width, UINT height)
{
    if (!o.swap) return false;
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    if (width == o.width && height == o.height) return true;
    o.rtv.Reset();
    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    m_ctx->Flush();
    if (FAILED(o.swap->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0))) return false;
    o.width = width;
    o.height = height;
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(o.swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    return SUCCEEDED(m_device->CreateRenderTargetView(back.Get(), nullptr, &o.rtv));
}

void Renderer::SetOutputOpacity(OutputWindow& o, float opacity)
{
    if (!o.effect || std::abs(o.opacity - opacity) < 0.001f) return;
    o.opacity = opacity;
    o.effect->SetOpacity(opacity);
    o.dcomp->Commit();
}

void Renderer::BeginOutput(OutputWindow& o)
{
    m_redirectRtv = o.rtv.Get();
    m_redirectW = o.width;
    m_redirectH = o.height;
    const float black[4] = {0, 0, 0, 1};
    if (m_redirectRtv) m_ctx->ClearRenderTargetView(m_redirectRtv, black);
}

void Renderer::EndOutput()
{
    m_redirectRtv = nullptr;
}

void Renderer::PresentOutput(OutputWindow& o)
{
    if (!o.swap) return;
    HRESULT hr = o.swap->Present(0, m_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) m_lost = true;
}

void Renderer::LagSample(VideoSurface& s, int64_t frameQpc)
{
    if (!s.hasImage) return;
    constexpr UINT n = 8;
    if (!s.lag) {
        s.lag = MakeTexture(m_device.Get(), n, n, DXGI_FORMAT_B8G8R8A8_UNORM, false, D3D11_BIND_RENDER_TARGET);
        if (!s.lag) return;
        m_device->CreateRenderTargetView(s.lag.Get(), nullptr, &s.lagRtv);
        D3D11_TEXTURE2D_DESC td{};
        s.lag->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& st : s.lagStaging) m_device->CreateTexture2D(&td, nullptr, &st);
    }
    const int k = s.lagIndex;
    if (s.lagPending[k]) return;   // ring full: skip this frame
    // Centre 50 % of the picture into 8x8.
    ID3D11RenderTargetView* rtv = s.lagRtv.Get();
    m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{-float(n) * 0.5f, -float(n) * 0.5f, float(n) * 2, float(n) * 2, 0, 1};
    m_ctx->RSSetViewports(1, &vp);
    D3D11_RECT sc{0, 0, LONG(n), LONG(n)};
    m_ctx->RSSetScissorRects(1, &sc);
    m_ctx->RSSetState(m_raster.Get());
    m_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_psPresent.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = s.rgbSrv.Get();
    m_ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = m_linear.Get();
    m_ctx->PSSetSamplers(0, 1, &smp);
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    m_ctx->PSSetShaderResources(0, 1, &nullSrv);
    m_ctx->CopyResource(s.lagStaging[k].Get(), s.lag.Get());
    s.lagQpc[k] = frameQpc;
    s.lagPending[k] = true;
    s.lagIndex = (k + 1) % 4;
}

bool Renderer::LagResult(VideoSurface& s, float& level, int64_t& frameQpc)
{
    // Oldest pending slot first.
    for (int i = 0; i < 4; ++i) {
        const int k = (s.lagIndex + i) % 4;
        if (!s.lagPending[k]) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(m_ctx->Map(s.lagStaging[k].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) return false;
        uint32_t sum = 0;
        for (UINT y = 0; y < 8; ++y) {
            const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
            for (UINT x = 0; x < 8; ++x) sum += (uint32_t(row[x * 4]) + row[x * 4 + 1] + row[x * 4 + 2]) / 3;
        }
        m_ctx->Unmap(s.lagStaging[k].Get(), 0);
        s.lagPending[k] = false;
        level = float(sum) / (64.0f * 255.0f);
        frameQpc = s.lagQpc[k];
        return true;
    }
    return false;
}

void Renderer::FillSurface(VideoSurface& s, float gray)
{
    if (!s.rgbRtv) return;
    const float c[4] = {gray, gray, gray, 1};
    m_ctx->ClearRenderTargetView(s.rgbRtv.Get(), c);
}

void Renderer::BeginUi()
{
    m_d2d->BeginDraw();
    m_d2d->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Renderer::EndUi()
{
    HRESULT hr = m_d2d->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) m_lost = true;
}

void Renderer::BeginUiLayer()
{
    if (!m_uiBitmap) {   // no layer (creation failed): draw straight onto the picture as before
        BeginUi();
        return;
    }
    m_d2d->SetTarget(m_uiBitmap.Get());
    m_d2d->BeginDraw();
    m_d2d->SetTransform(D2D1::Matrix3x2F::Identity());
    m_d2d->Clear(D2D1::ColorF(0, 0, 0, 0));
}

void Renderer::EndUiLayer()
{
    if (!m_uiBitmap) {
        EndUi();
        return;
    }
    HRESULT hr = m_d2d->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) m_lost = true;
    m_d2d->SetTarget(m_d2dTarget.Get());
    m_uiValid = SUCCEEDED(hr);
}

void Renderer::DrawUiLayer()
{
    if (m_hdrOut) {   // on top of the HDR pictures: drawn in ComposeHdr
        m_uiDeferred = true;
        return;
    }
    DrawUiLayerTo(m_backRtv.Get(), m_psLayer.Get());
}

void Renderer::DrawUiLayerTo(ID3D11RenderTargetView* rtv, ID3D11PixelShader* ps)
{
    if (!m_uiSrv || !m_uiValid || !ps || !rtv) return;
    m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, float(m_width), float(m_height), 0, 1};
    m_ctx->RSSetViewports(1, &vp);
    D3D11_RECT sc{0, 0, LONG(m_width), LONG(m_height)};
    m_ctx->RSSetScissorRects(1, &sc);
    m_ctx->RSSetState(m_raster.Get());
    m_ctx->OMSetBlendState(m_blendPremul.Get(), nullptr, 0xFFFFFFFF);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->PSSetShader(ps, nullptr, 0);
    ID3D11ShaderResourceView* srv = m_uiSrv.Get();
    m_ctx->PSSetShaderResources(0, 1, &srv);
    m_ctx->PSSetSamplers(0, 1, m_point.GetAddressOf());
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    m_ctx->PSSetShaderResources(0, 1, &none);
    m_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
}

bool Renderer::Visible()
{
    if (!m_occluded) return true;
    m_occluded = m_swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED;
    return !m_occluded;
}

bool Renderer::Present(bool vsync)
{
    if (m_hdrOut) ComposeHdr();
    HRESULT hr = vsync ? m_swap->Present(1, 0) : m_swap->Present(0, m_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    m_occluded = hr == DXGI_STATUS_OCCLUDED;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        LOGE(L"GPU device lost: %s", HrToString(m_device->GetDeviceRemovedReason()).c_str());
        m_lost = true;
        return false;
    }
    return true;
}

static bool ReadTexture(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, std::vector<uint8_t>& bgra)
{
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &staging))) return false;
    ctx->CopyResource(staging.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    bgra.resize(size_t(td.Width) * td.Height * 4);
    for (UINT y = 0; y < td.Height; ++y)
        memcpy(&bgra[size_t(y) * td.Width * 4], static_cast<BYTE*>(m.pData) + size_t(y) * m.RowPitch, size_t(td.Width) * 4);
    ctx->Unmap(staging.Get(), 0);
    return true;
}

bool Renderer::ReadbackBackBuffer(std::vector<uint8_t>& bgra)
{
    if (m_hdrOut) {   // the SDR scene with the UI on top (what an SDR screen would show)
        if (!m_scene.tex) return false;
        RenderTarget copy;
        copy.tex = MakeTexture(m_device.Get(), m_width, m_height, DXGI_FORMAT_B8G8R8A8_UNORM, false, D3D11_BIND_RENDER_TARGET);
        if (!copy.tex) return false;
        m_ctx->CopyResource(copy.tex.Get(), m_scene.tex.Get());
        m_device->CreateRenderTargetView(copy.tex.Get(), nullptr, &copy.rtv);
        if (m_uiDeferred) DrawUiLayerTo(copy.rtv.Get(), m_psLayer.Get());
        return ReadTexture(m_device.Get(), m_ctx.Get(), copy.tex.Get(), bgra);
    }
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(m_swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    return ReadTexture(m_device.Get(), m_ctx.Get(), back.Get(), bgra);
}

// ---------------------------------------------------------------------------------------------
// HDR screens

Renderer::HdrDisplay Renderer::QueryHdrDisplay(HMONITOR monitor, ComPtr<IDXGIFactory1>& factory)
{
    HdrDisplay d;
    if (!factory || !factory->IsCurrent()) {
        factory.Reset();
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return d;
    }
    // The monitor's output, on whichever adapter lists it.
    DXGI_OUTPUT_DESC1 desc{};
    bool found = false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; !found && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> out;
        for (UINT o = 0; !found && adapter->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; ++o) {
            ComPtr<IDXGIOutput6> out6;
            found = SUCCEEDED(out.As(&out6)) && SUCCEEDED(out6->GetDesc1(&desc)) && desc.Monitor == monitor;
            out.Reset();
        }
        adapter.Reset();
    }
    if (!found) return d;
    d.hdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    if (desc.MaxLuminance > 50.0f) d.peakNits = desc.MaxLuminance;
    // Windows' "SDR content brightness" for this monitor (what SDR white looks like while HDR is on).
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(desc.Monitor, &mi)) return d;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return d;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return d;
    for (UINT32 i = 0; i < np; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || wcscmp(src.viewGdiDeviceName, mi.szDevice) != 0) continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0)
            d.sdrWhiteNits = float(white.SDRWhiteLevel) / 1000.0f * 80.0f;
        break;
    }
    return d;
}

bool Renderer::SetHdrOutput(bool on, const HdrDisplay& d)
{
    m_hdrDisplay = d;
    if (on == m_hdrOut || !m_swap) return on == m_hdrOut;
    ReleaseTargets();
    const DXGI_FORMAT fmt = on ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    HRESULT hr = m_swap->ResizeBuffers(0, m_width, m_height, fmt,
                                       DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
    ComPtr<IDXGISwapChain3> sc3;
    if (SUCCEEDED(hr) && SUCCEEDED(m_swap.As(&sc3)))
        hr = sc3->SetColorSpace1(on ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    if (FAILED(hr) && on) {   // stay SDR
        LOGW(L"HDR output not possible: %s", HrToString(hr).c_str());
        m_swap->ResizeBuffers(0, m_width, m_height, DXGI_FORMAT_B8G8R8A8_UNORM,
                              DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
        if (SUCCEEDED(m_swap.As(&sc3))) sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        m_hdrOut = false;
        CreateTargets();
        return false;
    }
    m_hdrOut = on;
    LOGI(L"Screen output: %s (SDR white %.0f nits, display peak %.0f nits)", on ? L"HDR (scRGB)" : L"SDR", d.sdrWhiteNits, d.peakNits);
    return CreateTargets();
}

void Renderer::QueueHdrPicture(const VideoSurface& s, const float uv[4], const RectF& dest, const RectF& clip)
{
    if (!m_hdrOut || m_redirectRtv || !s.hdrLinSrv || s.transfer == Transfer::Sdr) return;
    HdrPic p;
    p.srv = s.hdrLinSrv;
    std::copy(uv, uv + 4, p.uv);
    p.dest = dest;
    p.clip = clip;
    p.white = s.hdrWhite;
    p.peak = s.hdrPeak;
    m_hdrPics.push_back(std::move(p));
}

void Renderer::ComposeHdr()
{
    if (!m_swapRtv || !m_scene.srv || !m_psToHdr || !m_psHdrPic || !m_hdrCb) return;
    auto setCb = [&](float a, float b, float c, float d, const float* uv) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(m_ctx->Map(m_hdrCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
        const float cb[8] = {a, b, c, d, uv ? uv[0] : 0, uv ? uv[1] : 0, uv ? uv[2] : 1, uv ? uv[3] : 1};
        memcpy(m.pData, cb, sizeof(cb));
        m_ctx->Unmap(m_hdrCb.Get(), 0);
    };
    const float sdrScale = m_hdrDisplay.sdrWhiteNits / 80.0f;
    ID3D11RenderTargetView* rtv = m_swapRtv.Get();
    m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    m_ctx->RSSetState(m_raster.Get());
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = {m_hdrCb.Get()};
    m_ctx->PSSetConstantBuffers(1, 1, cbs);
    // 1. The SDR scene at the screen's SDR brightness.
    setCb(sdrScale, m_hdrDisplay.peakNits, 203.0f, 1000.0f, nullptr);
    D3D11_VIEWPORT vp{0, 0, float(m_width), float(m_height), 0, 1};
    m_ctx->RSSetViewports(1, &vp);
    D3D11_RECT all{0, 0, LONG(m_width), LONG(m_height)};
    m_ctx->RSSetScissorRects(1, &all);
    m_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    m_ctx->PSSetShader(m_psToHdr.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = m_scene.srv.Get();
    m_ctx->PSSetShaderResources(0, 1, &srv);
    m_ctx->PSSetSamplers(0, 1, m_point.GetAddressOf());
    m_ctx->Draw(3, 0);
    // 2. HDR pictures at their real brightness, where their SDR versions were drawn.
    m_ctx->PSSetShader(m_psHdrPic.Get(), nullptr, 0);
    m_ctx->PSSetSamplers(0, 1, m_linear.GetAddressOf());
    for (const HdrPic& p : m_hdrPics) {
        setCb(sdrScale, m_hdrDisplay.peakNits, p.white, p.peak, p.uv);
        D3D11_VIEWPORT pv{p.dest.x, p.dest.y, p.dest.w, p.dest.h, 0, 1};
        m_ctx->RSSetViewports(1, &pv);
        D3D11_RECT sc{LONG(std::floor(std::max(0.0f, p.clip.x))), LONG(std::floor(std::max(0.0f, p.clip.y))),
                      LONG(std::ceil(std::min(float(m_width), p.clip.Right()))), LONG(std::ceil(std::min(float(m_height), p.clip.Bottom())))};
        m_ctx->RSSetScissorRects(1, &sc);
        srv = p.srv.Get();
        m_ctx->PSSetShaderResources(0, 1, &srv);
        m_ctx->Draw(3, 0);
    }
    // 3. The UI layer on top, at the SDR brightness.
    setCb(sdrScale, m_hdrDisplay.peakNits, 203.0f, 1000.0f, nullptr);
    if (m_uiDeferred) DrawUiLayerTo(m_swapRtv.Get(), m_psLayerHdr.Get());
    ID3D11ShaderResourceView* none = nullptr;
    m_ctx->PSSetShaderResources(0, 1, &none);
    m_uiDeferred = false;
    if (!m_sampleWant.empty() && !m_sampleReady) {   // testing: copy the requested pixels of this frame
        D3D11_TEXTURE2D_DESC sd{};
        if (m_hdrSample) m_hdrSample->GetDesc(&sd);
        if (!m_hdrSample || sd.Width != m_sampleWant.size()) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = UINT(m_sampleWant.size());
            td.Height = 1;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            m_hdrSample.Reset();
            m_device->CreateTexture2D(&td, nullptr, &m_hdrSample);
        }
        ComPtr<ID3D11Resource> back;
        m_swapRtv->GetResource(&back);
        if (m_hdrSample && back) {
            for (size_t i = 0; i < m_sampleWant.size(); ++i) {
                const POINT p = m_sampleWant[i];
                if (p.x < 0 || p.y < 0 || UINT(p.x) >= m_width || UINT(p.y) >= m_height) continue;
                D3D11_BOX box{UINT(p.x), UINT(p.y), 0, UINT(p.x) + 1, UINT(p.y) + 1, 1};
                m_ctx->CopySubresourceRegion(m_hdrSample.Get(), 0, UINT(i), 0, 0, back.Get(), 0, &box);
            }
            m_sampleReady = true;
        }
    }
}

bool Renderer::ReadPixelsFp16(ID3D11Texture2D* tex, const std::vector<POINT>& points, std::vector<float>& rgba)
{
    if (!tex || points.empty()) return false;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = UINT(points.size());
    sd.Height = 1;
    sd.MipLevels = sd.ArraySize = 1;
    sd.Format = td.Format;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(m_device->CreateTexture2D(&sd, nullptr, &st))) return false;
    for (size_t i = 0; i < points.size(); ++i) {
        D3D11_BOX box{UINT(points[i].x), UINT(points[i].y), 0, UINT(points[i].x) + 1, UINT(points[i].y) + 1, 1};
        m_ctx->CopySubresourceRegion(st.Get(), 0, UINT(i), 0, 0, tex, 0, &box);
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(m_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    const uint16_t* h = static_cast<const uint16_t*>(m.pData);
    rgba.resize(points.size() * 4);
    for (size_t i = 0; i < rgba.size(); ++i) {
        const uint32_t v = h[i], sign = (v >> 15) & 1, exp = (v >> 10) & 31, man = v & 1023;
        const float f = exp == 0 ? std::ldexp(float(man), -24) : exp == 31 ? INFINITY : std::ldexp(float(man | 1024), int(exp) - 25);
        rgba[i] = sign ? -f : f;
    }
    m_ctx->Unmap(st.Get(), 0);
    return true;
}

bool Renderer::HdrSamples(std::vector<float>& rgba)
{
    if (!m_sampleReady || !m_hdrSample) return false;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(m_ctx->Map(m_hdrSample.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    const uint16_t* h = static_cast<const uint16_t*>(m.pData);
    rgba.resize(m_sampleWant.size() * 4);
    for (size_t i = 0; i < rgba.size(); ++i) {   // half -> float
        const uint32_t v = h[i], sign = (v >> 15) & 1, exp = (v >> 10) & 31, man = v & 1023;
        const float f = exp == 0 ? std::ldexp(float(man), -24) : exp == 31 ? INFINITY : std::ldexp(float(man | 1024), int(exp) - 25);
        rgba[i] = sign ? -f : f;
    }
    m_ctx->Unmap(m_hdrSample.Get(), 0);
    return true;
}

bool Renderer::Readback(const VideoSurface& s, std::vector<uint8_t>& bgra, bool standby)
{
    if (standby && s.lastGood) return ReadTexture(m_device.Get(), m_ctx.Get(), s.lastGood.Get(), bgra);
    if (!s.hasImage) return false;
    return ReadTexture(m_device.Get(), m_ctx.Get(), s.rgb.Get(), bgra);
}

void Renderer::Probe(VideoSurface& s)
{
    if (!s.hasImage) return;
    constexpr UINT pw = 16, ph = 9;
    if (!s.probe) {
        s.probe = MakeTexture(m_device.Get(), pw, ph, DXGI_FORMAT_B8G8R8A8_UNORM, false, D3D11_BIND_RENDER_TARGET);
        if (!s.probe) return;
        m_device->CreateRenderTargetView(s.probe.Get(), nullptr, &s.probeRtv);
        D3D11_TEXTURE2D_DESC td{};
        s.probe->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& st : s.probeStaging) m_device->CreateTexture2D(&td, nullptr, &st);
    }
    // Collect the older sample if the GPU is done with it.
    const int read = s.probeIndex ^ 1;
    if (s.probePending[read] && s.probeStaging[read]) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(m_ctx->Map(s.probeStaging[read].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
            uint8_t maxV = 0;
            for (UINT y = 0; y < ph; ++y) {
                const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
                for (UINT x = 0; x < pw * 4; ++x)
                    if ((x & 3) != 3) maxV = std::max(maxV, row[x]);
            }
            m_ctx->Unmap(s.probeStaging[read].Get(), 0);
            s.brightest = maxV / 255.0f;
            s.probePending[read] = false;
            if (s.brightest >= kBlackLevel && s.probeCopy[read]) {
                // The copy taken with this probe shows a real picture: keep it (swap, no copy).
                std::swap(s.lastGood, s.probeCopy[read]);
                std::swap(s.lastGoodSrv, s.probeCopySrv[read]);
                std::swap(s.lastGoodW, s.probeCopyW[read]);
                std::swap(s.lastGoodH, s.probeCopyH[read]);
                s.lastGoodQpc = QpcNow();
            }
        }
    }
    // Take a new sample: draw the picture into 16x9 with filtering, then copy for readback.
    ID3D11RenderTargetView* rtv = s.probeRtv.Get();
    m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, float(pw), float(ph), 0, 1};
    m_ctx->RSSetViewports(1, &vp);
    D3D11_RECT sc{0, 0, LONG(pw), LONG(ph)};
    m_ctx->RSSetScissorRects(1, &sc);
    m_ctx->RSSetState(m_raster.Get());
    m_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_psPresent.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = s.rgbSrv.Get();
    m_ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = m_linear.Get();
    m_ctx->PSSetSamplers(0, 1, &smp);
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    m_ctx->PSSetShaderResources(0, 1, &nullSrv);
    if (s.probeStaging[s.probeIndex]) {
        m_ctx->CopyResource(s.probeStaging[s.probeIndex].Get(), s.probe.Get());
        s.probePending[s.probeIndex] = true;
        const int k = s.probeIndex;
        if (!s.probeCopy[k] || s.probeCopyW[k] != s.width || s.probeCopyH[k] != s.height) {
            s.probeCopy[k] = MakeTexture(m_device.Get(), s.width, s.height, DXGI_FORMAT_B8G8R8A8_UNORM, false,
                                          D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);   // RT: the encoder's video processor needs it
            s.probeCopySrv[k].Reset();
            if (s.probeCopy[k]) m_device->CreateShaderResourceView(s.probeCopy[k].Get(), nullptr, &s.probeCopySrv[k]);
            s.probeCopyW[k] = s.width;
            s.probeCopyH[k] = s.height;
        }
        if (s.probeCopy[k] && s.probeCopySrv[k]) m_ctx->CopyResource(s.probeCopy[k].Get(), s.rgb.Get());
        else s.probeCopy[k].Reset();
    }
    s.probeIndex ^= 1;
}

RectF Renderer::FitRect(UINT srcW, UINT srcH, const RectF& area, ScaleMode mode)
{
    if (srcW == 0 || srcH == 0 || area.w <= 0 || area.h <= 0) return area;
    const float sx = area.w / float(srcW), sy = area.h / float(srcH);
    float scale = 1.0f;
    switch (mode) {
    case ScaleMode::Stretch: return area;
    case ScaleMode::Fill: scale = std::max(sx, sy); break;
    case ScaleMode::Integer: {
        float fit = std::min(sx, sy);
        scale = fit >= 1.0f ? std::floor(fit) : fit;
        break;
    }
    default: scale = std::min(sx, sy); break;
    }
    const float w = std::round(srcW * scale), h = std::round(srcH * scale);
    return {std::round(area.x + (area.w - w) * 0.5f), std::round(area.y + (area.h - h) * 0.5f), w, h};
}

} // namespace cc
