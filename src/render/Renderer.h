#pragma once
#include "core/Common.h"
#include "devices/VideoCapture.h"
#include "scene/Scene.h"

#include <d2d1_1.h>
#include <dcomp.h>
#include <d3d11_1.h>
#include <dwrite.h>
#include <dxgi1_6.h>

namespace cc {

// Per-source GPU state: upload textures for the native layout plus a converted BGRA image.
// Crop, mirror and rotate, done by the colour conversion itself (an exact pixel remap, no extra pass), so the
// screen, recordings, sharing, screenshots and the replay buffer all get the same picture.
struct PictureTransform {
    float crop[4] = {0, 0, 0, 0};   // left, top, right, bottom: parts of the source width / height (up to 0.45 each)
    int rotate = 0;                 // quarter turns clockwise
    bool mirror = false;            // flipped left-right (after rotating)
    bool Identity() const { return !rotate && !mirror && !crop[0] && !crop[1] && !crop[2] && !crop[3]; }
    bool operator==(const PictureTransform&) const = default;
};

// How an HDR signal is shown (Settings ▸ Video ▸ HDR source, per device): mode 0 = as the device reports it, 1 = always
// SDR, 2 = HDR10 (PQ), 3 = HLG. HDR is tone-mapped for SDR screens: `whiteNits` of the signal become SDR white,
// highlights up to `peakNits` roll off smoothly.
struct HdrIn {
    int mode = 0;
    float whiteNits = 203.0f;   // BT.2408 reference white
    float peakNits = 1000.0f;   // typical HDR10 mastering / game peak
    bool operator==(const HdrIn&) const = default;
};

struct VideoSurface {
    PixelLayout layout = PixelLayout::None;
    UINT width = 0, height = 0;   // the picture after the transform (what is shown, recorded and shared)
    UINT srcW = 0, srcH = 0;      // the source frame
    bool rotated = false;         // turned a quarter: the source's pixel shape is swapped
    PictureTransform xf;          // applied
    Transfer transfer = Transfer::Sdr;   // the signal's brightness curve as converted (HDR: tone-mapped into rgb)
    // HDR screen: the same picture in linear light (FP16, 1.0 = the signal's white), written with rgb in one pass.
    ComPtr<ID3D11Texture2D> hdrLin;
    ComPtr<ID3D11RenderTargetView> hdrLinRtv;
    ComPtr<ID3D11ShaderResourceView> hdrLinSrv;
    float hdrWhite = 203.0f, hdrPeak = 1000.0f;   // as converted
    bool keepLinear = false;   // set by the app: an HDR recording encodes from the linear copy (kept also on SDR screens)
    bool gpu = false;   // frames arrive as D3D11 textures (hardware decode / frame server)
    DXGI_FORMAT gpuFormat = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11Texture2D> up0, up1;
    ComPtr<ID3D11ShaderResourceView> srv0, srv1;
    ComPtr<ID3D11Texture2D> rgb;
    ComPtr<ID3D11RenderTargetView> rgbRtv;
    ComPtr<ID3D11ShaderResourceView> rgbSrv;
    bool hasImage = false;
    ColorMatrix matrix = ColorMatrix::Auto;   // what was actually applied
    ColorRange range = ColorRange::Auto;

    // Picture probe: a 16x9 downsample read back asynchronously (never stalls the GPU).
    ComPtr<ID3D11Texture2D> probe;
    ComPtr<ID3D11RenderTargetView> probeRtv;
    ComPtr<ID3D11Texture2D> probeStaging[2];
    int probeIndex = 0;
    bool probePending[2] = {false, false};
    float brightest = -1.0f;   // brightest channel in the last probe, -1 = unknown

    // Standby: each probe also copies the picture; once the probe proves a copy isn't black it
    // becomes the "last good" picture shown (and encoded) while the signal is lost.
    ComPtr<ID3D11Texture2D> probeCopy[2];
    ComPtr<ID3D11ShaderResourceView> probeCopySrv[2];
    UINT probeCopyW[2] = {0, 0}, probeCopyH[2] = {0, 0};
    ComPtr<ID3D11Texture2D> lastGood;
    ComPtr<ID3D11ShaderResourceView> lastGoodSrv;
    UINT lastGoodW = 0, lastGoodH = 0;
    int64_t lastGoodQpc = 0;   // when the last good picture was taken

    // Black-bar detection (auto-crop): a 192x108 copy read back asynchronously.
    ComPtr<ID3D11Texture2D> bars;
    ComPtr<ID3D11RenderTargetView> barsRtv;
    ComPtr<ID3D11Texture2D> barsStaging[2];
    int barsIndex = 0;
    bool barsPending[2] = {false, false};
    float barsRect[4] = {0, 0, 1, 1};   // latest detection: content area (u0, v0, u1, v1)
    bool barsValid = false;             // false: picture fully black or nothing measured yet
    uint64_t barsSeq = 0;               // increments with every finished detection

    // Lag check: centre brightness of every frame, read back asynchronously (ring of 4).
    ComPtr<ID3D11Texture2D> lag;
    ComPtr<ID3D11RenderTargetView> lagRtv;
    ComPtr<ID3D11Texture2D> lagStaging[4];
    int64_t lagQpc[4] = {};
    bool lagPending[4] = {};
    int lagIndex = 0;
};

constexpr float kBlackLevel = 0.035f;   // brightest channel below this = the picture is black

enum class ScaleMode { Fit = 0, Fill = 1, Stretch = 2, Integer = 3 };

// A second window the renderer can draw into (pop-out mini player).
struct OutputWindow {
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11RenderTargetView> rtv;
    UINT width = 0, height = 0;
    // Composition output (see-through windows): the swap chain is a DirectComposition visual's content.
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDCompositionEffectGroup> effect;
    float opacity = 1.0f;
};

// An offscreen BGRA render target (scene canvas, preview/program, transitions).
struct RenderTarget {
    UINT width = 0, height = 0;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
};

// One layer to draw into a render target (see ps_item.hlsl).
struct ItemDraw {
    ID3D11ShaderResourceView* srv = nullptr;   // null = solid colour
    UINT srcW = 0, srcH = 0;
    bool premultiplied = false;
    RectF dest;                                 // in target pixels
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;       // crop
    float opacity = 1;
    float solid[4] = {0, 0, 0, 1};              // straight-alpha RGBA when srv == null
    const ItemFilters* filters = nullptr;
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    // gpuChoice: -1 automatic, -2 software (WARP), >= 0 index into EnumerateAdapters().
    bool Init(HWND hwnd, UINT width, UINT height);
    void SetGpuChoice(int choice) { m_gpuChoice = choice; }
    static std::vector<std::wstring> EnumerateAdapters();
    bool Warp() const { return m_warp; }
    bool VideoSupport() const { return m_videoSupport; }
    void Shutdown();
    bool Resize(UINT width, UINT height);

    bool Upload(VideoSurface& s, const CapturedFrame& f, ColorMatrix matrixOverride, ColorRange rangeOverride, const PictureTransform& xf = {},
                const HdrIn& hdr = {});
    void BeginFrame();
    void DrawSurface(const VideoSurface& s, const RectF& dest, const RectF& clip, bool smooth);
    void DrawTexture(ID3D11ShaderResourceView* srv, const RectF& dest, const RectF& clip, bool smooth, ID3D11PixelShader* ps = nullptr);   // to the back buffer

    // Offscreen composition
    bool EnsureTarget(RenderTarget& t, UINT w, UINT h);
    void BeginTarget(RenderTarget& t, float r = 0, float g = 0, float b = 0);
    void DrawItem(const RenderTarget& t, const ItemDraw& d);
    void DrawTransition(RenderTarget& out, const RenderTarget& a, const RenderTarget& b, float progress, int mode);
    void BeginUi();
    void EndUi();
    // The UI drawn into its own layer (redrawn only when it changes) and blended over the picture every frame.
    // Same result as drawing it straight onto the picture: premultiplied "over" is associative.
    void BeginUiLayer();
    void EndUiLayer();
    void DrawUiLayer();
    bool UiLayerValid() const { return m_uiValid; }
    bool Present(bool vsync);
    // The window can't be seen (minimized, or covered completely): Present said so. Visible() asks again cheaply.
    bool Occluded() const { return m_occluded; }
    bool Visible();
    bool Readback(const VideoSurface& s, std::vector<uint8_t>& bgra, bool standby = false);   // standby: the frozen picture
    bool ReadbackBackBuffer(std::vector<uint8_t>& bgra);   // call after EndUi, before Present
    // Samples the picture and updates s.brightest from an earlier sample (call a few times a second).
    void Probe(VideoSurface& s);
    // Secondary windows: Begin/EndOutput redirect DrawTexture* to the window's back buffer.
    // composition: drawn through DirectComposition so the window can be see-through (SetOutputOpacity); the window
    // needs WS_EX_NOREDIRECTIONBITMAP.
    bool CreateOutput(OutputWindow& o, HWND hwnd, UINT width, UINT height, bool composition = false);
    void SetOutputOpacity(OutputWindow& o, float opacity);
    bool ResizeOutput(OutputWindow& o, UINT width, UINT height);
    void BeginOutput(OutputWindow& o);
    void EndOutput();
    void PresentOutput(OutputWindow& o);
    void DetectBars(VideoSurface& s);   // updates barsRect / barsValid / barsSeq
    // Lag check: queue a brightness sample of the current picture (tagged with the frame's arrival time)
    // and collect finished ones (oldest first): level 0..1 of the centre area.
    void LagSample(VideoSurface& s, int64_t frameQpc);
    bool LagResult(VideoSurface& s, float& level, int64_t& frameQpc);
    void FillSurface(VideoSurface& s, float gray);   // testing: paint the picture
    // Draws only the `uv` part (u0, v0, u1, v1) of a texture into `dest`, clipped to `clip`.
    void DrawTextureCropped(ID3D11ShaderResourceView* srv, const float uv[4], const RectF& dest, const RectF& clip, bool smooth,
                            int filter = 0, UINT srcW = 0, UINT srcH = 0, float virtLines = 0, float strength = 1.0f);
    // Screen filters (ps_filter.hlsl): 0 none, 1 sharp pixels, 2 scanlines, 3 CRT, 4 LCD grid.
    // Enhance (display only): 0 off, 1 sharpen, 2 edge-aware upscale + sharpen; strength 0..1. Applies to smooth
    // draws shown at 1:1 or larger (downscaled pictures keep bicubic).
    void SetEnhance(int mode, float strength, bool keepCache = false);   // keepCache: another picture still uses it
    int Enhance() const { return m_enhanceMode; }
    // AI upscaling (display only): the GPU vendor's video super resolution (NVIDIA RTX Video Super Resolution, Intel
    // Arc / Xe VSR) through the D3D11 video processor, for smooth pictures shown larger than their source. The app
    // checks once per size that it really changed the picture; otherwise the normal path (Enhance / bicubic) draws.
    enum class SuperRes { Off, Unavailable, Waiting, Checking, Active, NoEffect, Failed };
    void SetSuperRes(bool on);
    SuperRes SuperResState() const { return m_srState; }
    std::wstring SuperResText() const;
    // Decided before the user turns it on: graphics card model, driver version and vendor. `reason` says why not.
    struct SuperResSupportInfo {
        bool ok = false;
        bool nvidia = false, intel = false;
        std::wstring name;     // "RTX Video Super Resolution" / "Intel Video Super Resolution"
        std::wstring reason;   // when !ok
    };
    SuperResSupportInfo SuperResSupport() const;
    // The driver's own switch can't be read, so a check renders a still picture off screen with and without the
    // extension (a few blits per frame after the present, read back without waiting) and compares the two.
    enum class SuperResProbe { NotRun, Running, DriverOn, DriverOff, Rejected };
    void ProbeSuperRes();             // starts the check (nothing while one runs)
    void StepSuperResProbe();         // once per frame, after the present
    SuperResProbe SuperResProbeResult() const { return m_probe; }
    std::wstring SuperResWhyNot() const;   // the reason the toggle is disabled ("" when it can be used)
    void RecheckSuperRes();           // the user may have changed the driver setting: check sizes found "no effect" again
    std::wstring DriverVersion() const;    // "32.0.15.8157"
    // Testing (--superres-sim "vendor|name|a.b.c.d|on|off|rejected"): pretend to be another graphics card.
    void SetSuperResSim(const std::wstring& sim);

    HANDLE Waitable() const { return m_waitable; }
    ID3D11Device* Device() const { return m_device.Get(); }
    ID3D11DeviceContext* Context() const { return m_ctx.Get(); }
    ID2D1Device* D2DDevice() const { return m_d2dDevice.Get(); }
    ID2D1DeviceContext* D2D() const { return m_d2d.Get(); }
    IDWriteFactory* DWrite() const { return m_dwrite.Get(); }
    IMFDXGIDeviceManager* DxgiManager() const { return m_dxgiManager.Get(); }
    UINT Width() const { return m_width; }
    UINT Height() const { return m_height; }
    bool Tearing() const { return m_tearing; }
    bool Lost() const { return m_lost; }
    const std::wstring& AdapterName() const { return m_adapterName; }
    LUID AdapterLuid() const { return m_adapterLuid; }

    static RectF FitRect(UINT srcW, UINT srcH, const RectF& area, ScaleMode mode);

    // ---- HDR screens ----
    // What Windows says about the monitor the window is on: HDR on, its peak, and its SDR brightness setting.
    struct HdrDisplay {
        bool hdr = false;
        float peakNits = 1000.0f;
        float sdrWhiteNits = 80.0f;
    };
    // Any thread (it doesn't touch the renderer): `factory` is the caller's cache, renewed when the adapters change.
    static HdrDisplay QueryHdrDisplay(HMONITOR monitor, ComPtr<IDXGIFactory1>& factory);
    // On: the window shows 16-bit HDR (scRGB); everything drawn as usual becomes the SDR part at `sdrWhiteNits`, and
    // HDR pictures queued with QueueHdrPicture are placed on top at their real brightness. Returns false if the
    // swap chain can't (then it stays SDR).
    bool SetHdrOutput(bool on, const HdrDisplay& d);
    bool HdrOutput() const { return m_hdrOut; }
    // An HDR picture (the surface's linear copy) for this frame, in the main window only (not mini / device windows).
    void QueueHdrPicture(const VideoSurface& s, const float uv[4], const RectF& dest, const RectF& clip);
    // Testing: scRGB values of some pixels of the HDR output, taken while the next frame is composed (before Present:
    // afterwards the flip-model buffer is another one). Request, then read after a presented frame.
    void RequestHdrSamples(const std::vector<POINT>& points) { m_sampleWant = points; m_sampleReady = false; }
    bool HdrSamples(std::vector<float>& rgba);
    // Testing: FP16 values of some pixels of a texture (waits for the GPU).
    bool ReadPixelsFp16(ID3D11Texture2D* tex, const std::vector<POINT>& points, std::vector<float>& rgba);

private:
    bool CreateTargets();
    void ReleaseTargets();
    void ComposeHdr();   // HDR output: scene + HDR pictures + UI layer into the swap chain (in Present)
    void DrawUiLayerTo(ID3D11RenderTargetView* rtv, ID3D11PixelShader* ps);
    bool EnsureSurface(VideoSurface& s, PixelLayout layout, UINT w, UINT h, UINT outW, UINT outH, DXGI_FORMAT gpuFormat);
    bool TextureSize(ID3D11ShaderResourceView* srv, float& w, float& h);
    bool DrawEnhanced(ID3D11ShaderResourceView* srv, const RectF& dest, const RectF& clip, ID3D11RenderTargetView* rtv, float tw, float th);
    bool DrawSuperRes(ID3D11ShaderResourceView* srv, const RectF& dest, const RectF& clip);
    bool EnsureSuperRes(UINT inW, UINT inH, UINT outW, UINT outH);
    bool SuperResExtension(ID3D11VideoProcessor* vp, bool enable);
    void CheckSuperRes(ID3D11VideoProcessorInputView* in);
    void ReleaseSuperRes();
    void ReleaseProbe();
    void Sharpen(ID3D11ShaderResourceView* srv, const D3D11_VIEWPORT& vp, const float origin[2], const float lo[2], const float hi[2]);

    HWND m_hwnd = nullptr;
    UINT m_width = 0, m_height = 0;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<IDXGISwapChain2> m_swap;
    HANDLE m_waitable = nullptr;
    bool m_tearing = false;
    bool m_lost = false;
    bool m_occluded = false;
    int m_gpuChoice = -1;
    bool m_warp = false;
    bool m_videoSupport = false;
    bool m_yuy2Textures = true;   // device can sample DXGI_FORMAT_YUY2 textures
    bool m_nv12Textures = true;
    bool m_p010Textures = true;   // DXGI_FORMAT_P010 / Y210 frames can be read on the GPU (else uploaded from memory)
    bool m_y210Textures = true;
    std::wstring m_adapterName;
    LUID m_adapterLuid{};
    ComPtr<ID3D11RenderTargetView> m_backRtv;   // where everything is drawn (the swap chain, or the SDR scene in HDR mode)
    // HDR output
    bool m_hdrOut = false;
    HdrDisplay m_hdrDisplay;
    ComPtr<ID3D11RenderTargetView> m_swapRtv;   // the FP16 swap chain buffer
    RenderTarget m_scene;                      // SDR scene, composed into m_swapRtv in Present
    bool m_uiDeferred = false;                 // the UI layer goes on top of the HDR pictures, in ComposeHdr
    struct HdrPic {
        ComPtr<ID3D11ShaderResourceView> srv;
        float uv[4];
        RectF dest, clip;
        float white, peak;
    };
    std::vector<HdrPic> m_hdrPics;
    ComPtr<ID3D11PixelShader> m_psToHdr, m_psHdrPic, m_psLayerHdr;
    ComPtr<ID3D11Buffer> m_hdrCb;
    ComPtr<ID3D11Texture2D> m_hdrSample;   // N x 1 staging (RequestHdrSamples)
    std::vector<POINT> m_sampleWant;
    bool m_sampleReady = false;

    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_psYuy2, m_psNv12, m_psBgra, m_psPresent, m_psItem, m_psTransition, m_psFilter, m_psBicubic;
    ComPtr<ID3D11BlendState> m_blendPremul;
    ComPtr<ID3D11Buffer> m_itemCb, m_filterCb, m_scaleCb, m_sharpenCb;
    ComPtr<ID3D11PixelShader> m_psUpscale, m_psSharpen, m_psLayer;
    int m_enhanceMode = 0;
    // AI upscaling
    UINT m_vendorId = 0, m_deviceId = 0;
    LARGE_INTEGER m_driverVersion{};   // user-mode driver version (IDXGIAdapter::CheckInterfaceSupport)
    SuperResProbe m_probe = SuperResProbe::NotRun;
    UINT m_probeStep = 0;
    RenderTarget m_probeIn, m_probeA, m_probeB;
    ComPtr<ID3D11VideoProcessorEnumerator> m_probeEnum;
    ComPtr<ID3D11VideoProcessor> m_probeVpA, m_probeVpB;
    ComPtr<ID3D11VideoProcessorInputView> m_probeInView;
    ComPtr<ID3D11VideoProcessorOutputView> m_probeOutA, m_probeOutB;
    ComPtr<ID3D11Texture2D> m_probeReadA, m_probeReadB;
    bool m_simOn = false;   // --superres-sim
    UINT m_simVendor = 0;
    std::wstring m_simName;
    LARGE_INTEGER m_simDriver{};
    SuperResProbe m_simProbe = SuperResProbe::DriverOn;
    bool m_srWanted = false;
    SuperRes m_srState = SuperRes::Off;
    UINT m_srInW = 0, m_srInH = 0, m_srOutW = 0, m_srOutH = 0, m_srFrames = 0;
    UINT m_srNoEffectW = 0, m_srNoEffectH = 0;   // checked: no effect at this output size (not retried)
    ComPtr<ID3D11VideoDevice> m_vdev;
    ComPtr<ID3D11VideoContext> m_vctx;
    ComPtr<ID3D11VideoProcessorEnumerator> m_srEnum;
    ComPtr<ID3D11VideoProcessor> m_srVp;
    ComPtr<ID3D11Texture2D> m_srInTex;
    ComPtr<ID3D11VideoProcessorInputView> m_srInView;
    ComPtr<ID3D11VideoProcessorOutputView> m_srOutView;
    RenderTarget m_srOut;
    float m_enhanceStrength = 0.5f;
    std::vector<RenderTarget> m_enhanceRts;   // sharpened pictures at source size (a few sizes; freed when off)
    ID3D11ShaderResourceView* m_scaleSrv = nullptr;   // texture whose size is in m_scaleCb (cache)
    float m_scaleW = 0, m_scaleH = 0;
    ID3D11RenderTargetView* m_redirectRtv = nullptr;   // BeginOutput
    UINT m_redirectW = 0, m_redirectH = 0;
    ComPtr<ID3D11SamplerState> m_linear, m_point;
    ComPtr<ID3D11RasterizerState> m_raster;
    ComPtr<ID3D11Buffer> m_cb;

    ComPtr<ID2D1Factory1> m_d2dFactory;
    ComPtr<ID2D1Device> m_d2dDevice;
    ComPtr<ID2D1DeviceContext> m_d2d;
    ComPtr<ID2D1Bitmap1> m_d2dTarget;
    ComPtr<ID3D11Texture2D> m_uiTex;
    ComPtr<ID3D11ShaderResourceView> m_uiSrv;
    ComPtr<ID2D1Bitmap1> m_uiBitmap;
    bool m_uiValid = false;
    ComPtr<IDWriteFactory> m_dwrite;

    ComPtr<IMFDXGIDeviceManager> m_dxgiManager;
    UINT m_resetToken = 0;
};

} // namespace cc
