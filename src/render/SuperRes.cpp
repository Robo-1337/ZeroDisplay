// AI upscaling (display only): the GPU vendor's video super resolution through the standard D3D11 video processor
// (ID3D11VideoContext::VideoProcessorSetStreamExtension). NVIDIA RTX Video Super Resolution and Intel Video Super
// Resolution are switched on with the vendors' published extension interfaces; nothing else of theirs is used.
// Drivers don't report whether the effect is applied, so the app checks: after a few frames it renders the same
// picture through a second processor without the extension and compares the two. No difference → the normal path.
#include "render/Renderer.h"
#include "core/Log.h"

#include <algorithm>
#include <cmath>

namespace cc {

namespace {

// NVIDIA: {version 1, method 2 = super resolution, enable}.
const GUID kNvidiaPpe = {0xd43ce1b3, 0x1f4b, 0x48ac, {0xba, 0xee, 0xc3, 0xc2, 0x53, 0x75, 0xe6, 0xf7}};
struct NvidiaStreamExtension {
    UINT version, method, enable;
};

// Intel (Arc / Xe): version 3, then pre-processing mode (output extension), then super-resolution scaling (stream).
const GUID kIntelVpe = {0xedd1d4b9, 0x8659, 0x4cbc, {0xa4, 0xd6, 0x98, 0x97, 0x2c, 0x87, 0x27, 0x89}};
struct IntelVpeExtension {
    UINT function;
    void* param;
};
constexpr UINT kIntelFnVersion = 0x01, kIntelFnMode = 0x20, kIntelFnScaling = 0x37;
constexpr UINT kIntelVersion3 = 0x0003, kIntelModeNone = 0x0, kIntelModePreproc = 0x01, kIntelScalingDefault = 0x0, kIntelScalingSuperRes = 0x2;

constexpr UINT kVendorNvidia = 0x10DE, kVendorIntel = 0x8086, kVendorAmd = 0x1002;
constexpr UINT kCheckAfterFrames = 30;   // let the driver settle before comparing
constexpr UINT kCrop = 256;              // compared area (centre)

void ConfigureProcessor(ID3D11VideoContext* vc, ID3D11VideoProcessor* vp, UINT inW, UINT inH, UINT outW, UINT outH)
{
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs{};
    cs.RGB_Range = 0;   // full-range RGB in and out
    vc->VideoProcessorSetStreamColorSpace(vp, 0, &cs);
    vc->VideoProcessorSetOutputColorSpace(vp, &cs);
    vc->VideoProcessorSetStreamFrameFormat(vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vc->VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE);
    const RECT src{0, 0, LONG(inW), LONG(inH)}, dst{0, 0, LONG(outW), LONG(outH)};
    vc->VideoProcessorSetStreamSourceRect(vp, 0, TRUE, &src);
    vc->VideoProcessorSetStreamDestRect(vp, 0, TRUE, &dst);
    vc->VideoProcessorSetOutputTargetRect(vp, TRUE, &dst);
}

void VersionParts(LARGE_INTEGER v, UINT p[4])
{
    const DWORD hi = DWORD(v.HighPart), lo = v.LowPart;
    p[0] = HIWORD(hi);
    p[1] = LOWORD(hi);
    p[2] = HIWORD(lo);
    p[3] = LOWORD(lo);
}

// Mean absolute RGB difference of two mapped crops, and how much detail the reference has (standard deviation).
double MeanDifference(const D3D11_MAPPED_SUBRESOURCE& ma, const D3D11_MAPPED_SUBRESOURCE& mb, UINT cw, UINT ch, double& detail)
{
    double diff = 0, sum = 0, sum2 = 0;
    for (UINT y = 0; y < ch; ++y) {
        const uint8_t* pa = static_cast<const uint8_t*>(ma.pData) + size_t(y) * ma.RowPitch;
        const uint8_t* pb = static_cast<const uint8_t*>(mb.pData) + size_t(y) * mb.RowPitch;
        for (UINT x = 0; x < cw * 4; ++x) {
            if ((x & 3) == 3) continue;   // alpha
            diff += std::abs(int(pa[x]) - int(pb[x]));
            sum += pb[x];
            sum2 += double(pb[x]) * pb[x];
        }
    }
    const double n = double(cw) * ch * 3, avg = sum / n;
    detail = std::sqrt(std::max(0.0, sum2 / n - avg * avg));
    return diff / n;
}

} // namespace

std::wstring Renderer::DriverVersion() const
{
    UINT p[4];
    VersionParts(m_simOn ? m_simDriver : m_driverVersion, p);
    return p[0] || p[1] || p[2] || p[3] ? Format(L"%u.%u.%u.%u", p[0], p[1], p[2], p[3]) : L"";
}

void Renderer::SetSuperResSim(const std::wstring& sim)
{
    if (sim.empty()) return;
    std::vector<std::wstring> f;
    for (size_t p = 0; p <= sim.size();) {
        size_t e = sim.find(L'|', p);
        if (e == std::wstring::npos) e = sim.size();
        f.push_back(sim.substr(p, e - p));
        p = e + 1;
    }
    if (f.size() < 4) return;
    m_simOn = true;
    m_simVendor = f[0] == L"nvidia" ? kVendorNvidia : (f[0] == L"intel" ? kVendorIntel : (f[0] == L"amd" ? kVendorAmd : 0));
    m_simName = f[1];
    UINT v[4] = {};
    swscanf_s(f[2].c_str(), L"%u.%u.%u.%u", &v[0], &v[1], &v[2], &v[3]);
    m_simDriver.HighPart = LONG(MAKELONG(v[1], v[0]));
    m_simDriver.LowPart = MAKELONG(v[3], v[2]);
    m_simProbe = f[3] == L"off" ? SuperResProbe::DriverOff : (f[3] == L"rejected" ? SuperResProbe::Rejected : SuperResProbe::DriverOn);
    LOGI(L"AI upscaling: simulating %s, driver %s, check result %s", m_simName.c_str(), f[2].c_str(), f[3].c_str());
}

Renderer::SuperResSupportInfo Renderer::SuperResSupport() const
{
    SuperResSupportInfo s;
    const UINT vendor = m_simOn ? m_simVendor : m_vendorId;
    const std::wstring& name = m_simOn ? m_simName : m_adapterName;
    std::wstring shortName = name.rfind(L"NVIDIA ", 0) == 0 ? name.substr(7) : name;
    UINT p[4];
    VersionParts(m_simOn ? m_simDriver : m_driverVersion, p);
    s.nvidia = vendor == kVendorNvidia;
    s.intel = vendor == kVendorIntel;
    if (m_warp && !m_simOn) {
        s.reason = L"Needs a graphics card: the picture is drawn in software now.";
        return s;
    }
    if (s.nvidia) {
        s.name = L"RTX Video Super Resolution";
        if (name.find(L"RTX") == std::wstring::npos) {
            s.reason = L"Needs an NVIDIA RTX graphics card (RTX 20 or newer); this is a " + shortName + L".";
            return s;
        }
        // NVIDIA's version is the last five digits: 32.0.15.8157 = 581.57. RTX 30 / 40 / 50: 531.18, RTX 20: 545.84.
        const UINT nv = (p[2] % 10) * 10000 + p[3], need = name.find(L"RTX 20") != std::wstring::npos ? 54584u : 53118u;
        if (p[2] && nv < need) {
            s.reason = Format(L"Needs NVIDIA driver %u.%02u or newer; this PC has %u.%02u.", need / 100, need % 100, nv / 100, nv % 100);
            return s;
        }
        s.ok = true;
        return s;
    }
    if (s.intel) {
        s.name = L"Intel Video Super Resolution";
        // 27.20.100.8681 or newer (the build is the last two numbers: 100.8681 < 101.xxxx).
        if (p[2] && (p[2] < 100 || (p[2] == 100 && p[3] < 8681))) {
            s.reason = Format(L"Needs Intel graphics driver 27.20.100.8681 or newer; this PC has %u.%u.%u.%u.", p[0], p[1], p[2], p[3]);
            return s;
        }
        s.ok = true;   // which Intel graphics really do it, only the driver knows: the check decides
        return s;
    }
    if (vendor == kVendorAmd) {
        s.reason = L"AMD offers its video upscaler only through its own developer kit, not to other apps. Enhance (above) works on AMD.";
        return s;
    }
    s.reason = L"Needs an NVIDIA RTX or Intel graphics card.";
    return s;
}

std::wstring Renderer::SuperResWhyNot() const
{
    const SuperResSupportInfo s = SuperResSupport();
    if (!s.ok) return s.reason;
    if (m_probe == SuperResProbe::Rejected)
        return s.nvidia ? L"The NVIDIA driver didn't accept the request: update the graphics driver."
                        : L"This Intel graphics doesn't offer video super resolution to apps (it needs Intel Xe or Arc graphics with a current driver).";
    return L"";
}

void Renderer::RecheckSuperRes()
{
    m_srNoEffectW = m_srNoEffectH = 0;
    if (m_srState == SuperRes::NoEffect) m_srState = SuperRes::Waiting;
}

void Renderer::ReleaseProbe()
{
    m_probeReadA.Reset();
    m_probeReadB.Reset();
    m_probeOutA.Reset();
    m_probeOutB.Reset();
    m_probeInView.Reset();
    m_probeVpA.Reset();
    m_probeVpB.Reset();
    m_probeEnum.Reset();
    m_probeIn = {};
    m_probeA = {};
    m_probeB = {};
    m_probeStep = 0;
}

void Renderer::ProbeSuperRes()
{
    if (m_probe == SuperResProbe::Running) return;
    if (m_simOn) {
        m_probe = m_simProbe;
        return;
    }
    if (!SuperResSupport().ok) return;
    ReleaseProbe();
    m_probe = SuperResProbe::Running;
}

void Renderer::StepSuperResProbe()
{
    if (m_probe != SuperResProbe::Running) return;
    constexpr UINT inW = 960, inH = 540, outW = 1920, outH = 1080, kBlits = 12;
    auto finish = [&](SuperResProbe result, const wchar_t* what) {
        LOGI(L"AI upscaling check (%s, driver %s): %s", m_adapterName.c_str(), DriverVersion().c_str(), what);
        ReleaseProbe();
        m_probe = result;
    };
    if (m_probeStep == 0) {
        if (!m_vdev && (FAILED(m_device.As(&m_vdev)) || FAILED(m_ctx.As(&m_vctx)))) {
            finish(SuperResProbe::Rejected, L"no video processor");
            return;
        }
        // A detailed still: coloured noise with fine diagonal lines (an upscaler changes edges and texture).
        std::vector<uint32_t> px(size_t(inW) * inH);
        uint32_t seed = 0x2545F491;
        for (UINT y = 0; y < inH; ++y)
            for (UINT x = 0; x < inW; ++x) {
                seed = seed * 1664525u + 1013904223u;
                const uint32_t n = seed >> 24, line = ((x + y) / 3) & 1 ? 230u : 20u;
                px[size_t(y) * inW + x] = 0xFF000000u | ((x * 255 / inW) << 16) | (((n + line) / 2) << 8) | (((y * 255 / inH) + n) / 2);
            }
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputFrameRate = cd.OutputFrameRate = {60, 1};
        cd.InputWidth = inW;
        cd.InputHeight = inH;
        cd.OutputWidth = outW;
        cd.OutputHeight = outH;
        cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        HRESULT hr = EnsureTarget(m_probeIn, inW, inH) && EnsureTarget(m_probeA, outW, outH) && EnsureTarget(m_probeB, outW, outH) ? S_OK : E_OUTOFMEMORY;
        if (SUCCEEDED(hr)) {
            m_ctx->UpdateSubresource(m_probeIn.tex.Get(), 0, nullptr, px.data(), inW * 4, 0);
            hr = m_vdev->CreateVideoProcessorEnumerator(&cd, &m_probeEnum);
        }
        if (SUCCEEDED(hr)) hr = m_vdev->CreateVideoProcessor(m_probeEnum.Get(), 0, &m_probeVpA);
        if (SUCCEEDED(hr)) hr = m_vdev->CreateVideoProcessor(m_probeEnum.Get(), 0, &m_probeVpB);
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (SUCCEEDED(hr)) hr = m_vdev->CreateVideoProcessorInputView(m_probeIn.tex.Get(), m_probeEnum.Get(), &ivd, &m_probeInView);
        if (SUCCEEDED(hr)) hr = m_vdev->CreateVideoProcessorOutputView(m_probeA.tex.Get(), m_probeEnum.Get(), &ovd, &m_probeOutA);
        if (SUCCEEDED(hr)) hr = m_vdev->CreateVideoProcessorOutputView(m_probeB.tex.Get(), m_probeEnum.Get(), &ovd, &m_probeOutB);
        if (FAILED(hr)) {
            LOGW(L"AI upscaling check: video processor failed: %s", HrToString(hr).c_str());
            finish(SuperResProbe::Rejected, L"the video processor couldn't be set up");
            return;
        }
        ConfigureProcessor(m_vctx.Get(), m_probeVpA.Get(), inW, inH, outW, outH);
        ConfigureProcessor(m_vctx.Get(), m_probeVpB.Get(), inW, inH, outW, outH);
        if (!SuperResExtension(m_probeVpA.Get(), true)) {
            finish(SuperResProbe::Rejected, L"the driver rejected the extension");
            return;
        }
        m_probeStep = 1;
        return;
    }
    if (m_probeStep <= kBlits) {   // a few blits per frame: the driver may need some frames before it engages
        D3D11_VIDEO_PROCESSOR_STREAM st{};
        st.Enable = TRUE;
        st.pInputSurface = m_probeInView.Get();
        for (int i = 0; i < 4 && m_probeStep <= kBlits; ++i, ++m_probeStep)
            if (FAILED(m_vctx->VideoProcessorBlt(m_probeVpA.Get(), m_probeOutA.Get(), m_probeStep, 1, &st)) ||
                FAILED(m_vctx->VideoProcessorBlt(m_probeVpB.Get(), m_probeOutB.Get(), m_probeStep, 1, &st))) {
                finish(SuperResProbe::Rejected, L"the video processor failed");
                return;
            }
        if (m_probeStep > kBlits) {
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width = kCrop;
            sd.Height = kCrop;
            sd.MipLevels = sd.ArraySize = 1;
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(m_device->CreateTexture2D(&sd, nullptr, &m_probeReadA)) || FAILED(m_device->CreateTexture2D(&sd, nullptr, &m_probeReadB))) {
                finish(SuperResProbe::Rejected, L"no memory for the read-back");
                return;
            }
            const D3D11_BOX box{(outW - kCrop) / 2, (outH - kCrop) / 2, 0, (outW - kCrop) / 2 + kCrop, (outH - kCrop) / 2 + kCrop, 1};
            m_ctx->CopySubresourceRegion(m_probeReadA.Get(), 0, 0, 0, 0, m_probeA.tex.Get(), 0, &box);
            m_ctx->CopySubresourceRegion(m_probeReadB.Get(), 0, 0, 0, 0, m_probeB.tex.Get(), 0, &box);
        }
        return;
    }
    // Read back without waiting (the picture keeps going; this finishes a frame or two later).
    D3D11_MAPPED_SUBRESOURCE ma{}, mb{};
    HRESULT hr = m_ctx->Map(m_probeReadA.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ma);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
    if (FAILED(hr)) {
        finish(SuperResProbe::Rejected, L"the read-back failed");
        return;
    }
    hr = m_ctx->Map(m_probeReadB.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mb);
    if (FAILED(hr)) {
        m_ctx->Unmap(m_probeReadA.Get(), 0);
        if (hr != DXGI_ERROR_WAS_STILL_DRAWING) finish(SuperResProbe::Rejected, L"the read-back failed");
        return;
    }
    double detail = 0;
    const double mean = MeanDifference(ma, mb, kCrop, kCrop, detail);
    m_ctx->Unmap(m_probeReadA.Get(), 0);
    m_ctx->Unmap(m_probeReadB.Get(), 0);
    const std::wstring what = Format(L"mean difference %.2f, %s", mean, mean > 0.25 ? L"the driver upscales" : L"the driver doesn't apply it");
    finish(mean > 0.25 ? SuperResProbe::DriverOn : SuperResProbe::DriverOff, what.c_str());
}

void Renderer::SetSuperRes(bool on)
{
    if (on == m_srWanted) return;
    m_srWanted = on;
    m_srNoEffectW = m_srNoEffectH = 0;
    ReleaseSuperRes();
    m_srState = !on ? SuperRes::Off : (SuperResWhyNot().empty() ? SuperRes::Waiting : SuperRes::Unavailable);
    LOGI(L"AI upscaling %s%s", on ? L"on" : L"off", m_srState == SuperRes::Unavailable ? L" (not available on this graphics card)" : L"");
}

std::wstring Renderer::SuperResText() const
{
    switch (m_srState) {
    case SuperRes::Unavailable: return SuperResWhyNot();
    case SuperRes::Waiting: return L"On: used when the picture is shown larger than its source.";
    case SuperRes::Checking: return L"Checking that the graphics driver upscales…";
    case SuperRes::Active: return Format(L"Active: %u×%u → %u×%u.", m_srInW, m_srInH, m_srOutW, m_srOutH);
    case SuperRes::NoEffect:
        return m_vendorId == kVendorNvidia ? L"Not active: the NVIDIA driver didn't upscale this picture (Super Resolution is off in the NVIDIA App, or on battery)."
                                           : L"Not active: this Intel graphics driver didn't upscale this picture.";
    case SuperRes::Failed: return L"Couldn't start the video processor (see log.txt).";
    default: return L"";
    }
}

void Renderer::ReleaseSuperRes()
{
    m_srOutView.Reset();
    m_srInView.Reset();
    m_srInTex.Reset();
    m_srVp.Reset();
    m_srEnum.Reset();
    m_srOut = {};
    m_srInW = m_srInH = m_srOutW = m_srOutH = m_srFrames = 0;
}

bool Renderer::SuperResExtension(ID3D11VideoProcessor* vp, bool enable)
{
    if (m_vendorId == kVendorNvidia) {
        NvidiaStreamExtension e{1, 2, enable ? 1u : 0u};
        return SUCCEEDED(m_vctx->VideoProcessorSetStreamExtension(vp, 0, &kNvidiaPpe, sizeof(e), &e));
    }
    if (m_vendorId == kVendorIntel) {
        UINT version = kIntelVersion3, mode = enable ? kIntelModePreproc : kIntelModeNone, scaling = enable ? kIntelScalingSuperRes : kIntelScalingDefault;
        IntelVpeExtension v{kIntelFnVersion, &version}, m{kIntelFnMode, &mode}, s{kIntelFnScaling, &scaling};
        return SUCCEEDED(m_vctx->VideoProcessorSetOutputExtension(vp, &kIntelVpe, sizeof(v), &v)) &&
               SUCCEEDED(m_vctx->VideoProcessorSetOutputExtension(vp, &kIntelVpe, sizeof(m), &m)) &&
               SUCCEEDED(m_vctx->VideoProcessorSetStreamExtension(vp, 0, &kIntelVpe, sizeof(s), &s));
    }
    return false;
}

bool Renderer::EnsureSuperRes(UINT inW, UINT inH, UINT outW, UINT outH)
{
    if (!m_vdev && (FAILED(m_device.As(&m_vdev)) || FAILED(m_ctx.As(&m_vctx)))) return false;
    if (m_srVp && inW == m_srInW && inH == m_srInH && outW == m_srOutW && outH == m_srOutH) return true;
    ReleaseSuperRes();
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {60, 1};
    cd.InputWidth = inW;
    cd.InputHeight = inH;
    cd.OutputFrameRate = {60, 1};
    cd.OutputWidth = outW;
    cd.OutputHeight = outH;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    HRESULT hr = m_vdev->CreateVideoProcessorEnumerator(&cd, &m_srEnum);
    UINT flags = 0;
    if (SUCCEEDED(hr)) hr = m_srEnum->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &flags);
    if (SUCCEEDED(hr) && (!(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) || !(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))) hr = E_NOTIMPL;
    if (SUCCEEDED(hr)) hr = m_vdev->CreateVideoProcessor(m_srEnum.Get(), 0, &m_srVp);
    if (SUCCEEDED(hr) && !EnsureTarget(m_srOut, outW, outH)) hr = E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        hr = m_vdev->CreateVideoProcessorOutputView(m_srOut.tex.Get(), m_srEnum.Get(), &ovd, &m_srOutView);
    }
    if (FAILED(hr)) {
        LOGW(L"AI upscaling: video processor %u×%u → %u×%u failed: %s", inW, inH, outW, outH, HrToString(hr).c_str());
        ReleaseSuperRes();
        return false;
    }
    ConfigureProcessor(m_vctx.Get(), m_srVp.Get(), inW, inH, outW, outH);
    const bool accepted = SuperResExtension(m_srVp.Get(), true);
    LOGI(L"AI upscaling: %u×%u → %u×%u, %s extension %s", inW, inH, outW, outH, m_vendorId == kVendorNvidia ? L"NVIDIA" : L"Intel",
         accepted ? L"accepted" : L"rejected by the driver");
    m_srInW = inW;
    m_srInH = inH;
    m_srOutW = outW;
    m_srOutH = outH;
    m_srState = SuperRes::Checking;
    return true;
}

// Same picture through a second processor without the extension; compare a centre crop. Runs once per size.
void Renderer::CheckSuperRes(ID3D11VideoProcessorInputView* in)
{
    const UINT cw = std::min(kCrop, m_srOutW), ch = std::min(kCrop, m_srOutH);
    ComPtr<ID3D11VideoProcessor> ref;
    ComPtr<ID3D11VideoProcessorOutputView> refView;
    RenderTarget refOut;
    HRESULT hr = m_vdev->CreateVideoProcessor(m_srEnum.Get(), 0, &ref);
    if (SUCCEEDED(hr) && !EnsureTarget(refOut, m_srOutW, m_srOutH)) hr = E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        hr = m_vdev->CreateVideoProcessorOutputView(refOut.tex.Get(), m_srEnum.Get(), &ovd, &refView);
    }
    if (SUCCEEDED(hr)) {
        ConfigureProcessor(m_vctx.Get(), ref.Get(), m_srInW, m_srInH, m_srOutW, m_srOutH);
        D3D11_VIDEO_PROCESSOR_STREAM st{};
        st.Enable = TRUE;
        st.pInputSurface = in;
        hr = m_vctx->VideoProcessorBlt(ref.Get(), refView.Get(), 0, 1, &st);
    }
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = cw;
    sd.Height = ch;
    sd.MipLevels = sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> a, b;
    if (SUCCEEDED(hr)) hr = m_device->CreateTexture2D(&sd, nullptr, &a);
    if (SUCCEEDED(hr)) hr = m_device->CreateTexture2D(&sd, nullptr, &b);
    if (FAILED(hr)) {
        LOGW(L"AI upscaling: check failed (%s); assuming it works", HrToString(hr).c_str());
        m_srState = SuperRes::Active;
        return;
    }
    const D3D11_BOX box{(m_srOutW - cw) / 2, (m_srOutH - ch) / 2, 0, (m_srOutW - cw) / 2 + cw, (m_srOutH - ch) / 2 + ch, 1};
    m_ctx->CopySubresourceRegion(a.Get(), 0, 0, 0, 0, m_srOut.tex.Get(), 0, &box);
    m_ctx->CopySubresourceRegion(b.Get(), 0, 0, 0, 0, refOut.tex.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE ma{}, mb{};
    if (FAILED(m_ctx->Map(a.Get(), 0, D3D11_MAP_READ, 0, &ma))) return;   // retried next frame
    if (FAILED(m_ctx->Map(b.Get(), 0, D3D11_MAP_READ, 0, &mb))) {
        m_ctx->Unmap(a.Get(), 0);
        return;
    }
    double detail = 0;
    const double mean = MeanDifference(ma, mb, cw, ch, detail);
    m_ctx->Unmap(a.Get(), 0);
    m_ctx->Unmap(b.Get(), 0);
    if (detail < 4.0) {   // a flat picture (no signal, black screen) shows nothing either way: look again later
        m_srFrames = 0;
        return;
    }
    const bool active = mean > 0.25;
    LOGI(L"AI upscaling %s: %u×%u → %u×%u, mean difference %.2f (picture detail %.1f)", active ? L"active" : L"has no effect", m_srInW,
         m_srInH, m_srOutW, m_srOutH, mean, detail);
    m_srState = active ? SuperRes::Active : SuperRes::NoEffect;
    if (!active) {
        m_srNoEffectW = m_srOutW;
        m_srNoEffectH = m_srOutH;
    }
}

bool Renderer::DrawSuperRes(ID3D11ShaderResourceView* srv, const RectF& dest, const RectF& clip)
{
    if (m_srState == SuperRes::Unavailable || m_srState == SuperRes::Failed || m_srState == SuperRes::Off) return false;
    ComPtr<ID3D11Resource> res;
    srv->GetResource(&res);
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(res.As(&tex))) return false;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    if (td.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return false;
    const UINT outW = UINT(std::lround(dest.w)), outH = UINT(std::lround(dest.h));
    if ((outW < td.Width + 2 && outH < td.Height + 2) || outW > 8192 || outH > 8192) {   // only when enlarging
        if (m_srVp) ReleaseSuperRes();
        if (m_srState != SuperRes::NoEffect) m_srState = SuperRes::Waiting;
        return false;
    }
    if (outW == m_srNoEffectW && outH == m_srNoEffectH) {
        if (m_srVp) ReleaseSuperRes();
        m_srState = SuperRes::NoEffect;
        return false;
    }
    if (!EnsureSuperRes(td.Width, td.Height, outW, outH)) {
        m_srState = SuperRes::Failed;
        return false;
    }
    if (tex.Get() != m_srInTex.Get()) {
        m_srInView.Reset();
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        const HRESULT hr = m_vdev->CreateVideoProcessorInputView(tex.Get(), m_srEnum.Get(), &ivd, &m_srInView);
        if (FAILED(hr)) {
            LOGW(L"AI upscaling: picture can't be used as video processor input: %s", HrToString(hr).c_str());
            m_srState = SuperRes::Failed;
            ReleaseSuperRes();
            return false;
        }
        m_srInTex = tex;
    }
    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable = TRUE;
    st.pInputSurface = m_srInView.Get();
    const HRESULT hr = m_vctx->VideoProcessorBlt(m_srVp.Get(), m_srOutView.Get(), m_srFrames, 1, &st);
    if (FAILED(hr)) {
        LOGW(L"AI upscaling: video processor failed: %s", HrToString(hr).c_str());
        m_srState = SuperRes::Failed;
        ReleaseSuperRes();
        return false;
    }
    ++m_srFrames;
    if (m_srState == SuperRes::Checking && m_srFrames >= kCheckAfterFrames) {
        CheckSuperRes(m_srInView.Get());
        if (m_srState == SuperRes::NoEffect) {
            ReleaseSuperRes();
            return false;   // this frame: the normal path
        }
    }
    // The upscaled picture, 1:1 (point sampling).
    DrawTexture(m_srOut.srv.Get(), {std::round(dest.x), std::round(dest.y), float(outW), float(outH)}, clip, false);
    return true;
}

} // namespace cc
