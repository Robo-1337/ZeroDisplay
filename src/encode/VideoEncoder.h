#pragma once
#include "core/Common.h"

#include <d3d11_1.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <thread>

namespace cc {

struct EncodedPacket {
    std::vector<uint8_t> data;   // one H.264 access unit, Annex B
    int64_t time = 0;            // 100 ns, QPC time base (same as MFGetSystemTime / QpcTo100ns)
    int64_t duration = 0;
    bool keyframe = false;
    int64_t sourceQpc = 0;       // when the source frame arrived (for latency statistics)
};
using PacketPtr = std::shared_ptr<const EncodedPacket>;

// Where the frames are encoded.
enum class EncodeTarget {
    SameGpu,    // hardware encoder of the render GPU, fed with its textures (zero copy); software MFT if it has none
    OtherGpu,   // hardware encoder of another GPU ("hybrid"), fed with NV12 frames read back to system memory
    Cpu,        // Microsoft's software encoder
};

struct EncoderConfig {
    UINT width = 1920, height = 1080;
    UINT fpsNum = 60, fpsDen = 1;
    UINT bitrateKbps = 20000;
    UINT gopFrames = 120;   // keyframe interval
    bool hevc = false;      // H.265 instead of H.264 (recording only; viewers expect H.264)
    // HDR10: HEVC Main10 from 10-bit P010 (BT.2020, PQ). Input frames are linear light (FP16, 1.0 = hdrWhiteNits), or
    // SDR pictures (BGRA, their white = hdrWhiteNits). Needs a hardware HEVC encoder and the shader conversion.
    bool hdr = false;
    float hdrWhiteNits = 203.0f;
    int qualityVsSpeed = -1;   // CODECAPI_AVEncCommonQualityVsSpeed 0 (fastest) .. 100 (best); -1 = encoder default
    EncodeTarget target = EncodeTarget::SameGpu;
    LUID otherAdapter{};           // OtherGpu: the adapter whose encoder is used
    std::wstring otherAdapterName;
};

// A GPU (other than the one we render on) with a hardware encoder for the codec.
struct EncoderAdapter {
    LUID luid{};
    std::wstring name;
};

// One encoder pipeline: frames are converted BGRA -> NV12 (with scaling) on the GPU (our shader, or the
// video processor) and either handed to a hardware MFT on the same adapter as textures, or read back to
// system memory (without ever waiting on the GPU) for another GPU's encoder or a software MFT.
// Used through VideoEncoder, which can replace it without a gap.
class EncoderCore {
public:
    EncoderCore();
    ~EncoderCore();
    EncoderCore(const EncoderCore&) = delete;
    EncoderCore& operator=(const EncoderCore&) = delete;

    bool Start(ID3D11Device* device, IMFDXGIDeviceManager* manager, const EncoderConfig& cfg);
    void Stop();
    bool Running() const { return m_running.load(); }
    void Submit(ID3D11Texture2D* bgra, int64_t time100ns, int64_t sourceQpc);
    void Collect();
    void RequestKeyframe() { m_forceKeyframe = true; }
    // Where encoded packets go (any thread). nullptr mutes the pipeline (an old one being replaced).
    void SetOutput(std::function<void(const PacketPtr&)> out);

    static bool FindOtherAdapter(LUID renderAdapter, bool hevc, EncoderAdapter& out);
    static bool OtherAdapterKnown(LUID renderAdapter, bool hevc);

    EncoderConfig Config() const;
    std::wstring Name() const;
    bool Hardware() const { return m_async; }
    EncodeTarget Target() const { return Config().target; }
    std::wstring TargetName() const;
    static bool s_forceSoftware;
    static std::atomic<bool> s_testStall;
    static UINT s_testAlign;
    static std::atomic<bool> s_testSlowOther;
    static std::atomic<bool> s_testSlowSame;   // testing: display-GPU hardware encoders take every 2nd frame
    static bool s_videoProcessorOnly;
    std::vector<uint8_t> ParameterSets() const;
    double LatencyMs() const { return m_latencyMs.load(); }
    double OutputFps() const { return m_outFps.load(); }
    double BitrateKbps() const { return m_kbps.load(); }
    uint64_t Dropped() const { return m_dropped.load(); }
    bool Stalled() const { return m_running.load() && m_submitted.load() > 90 && m_emitted.load() == 0; }
    uint64_t Submitted() const { return m_submitted.load(); }
    uint64_t Emitted() const { return m_emitted.load(); }

private:
    class EventCallback;

    bool CreateTransform(LUID adapter, bool allowHardware, bool allowSoftware, bool gpuInput);
    void QueueCpuCopy(ID3D11Texture2D* bgra, int64_t time100ns, int64_t sourceQpc);
    ComPtr<IMFSample> ConvertBgra(const BYTE* src, UINT pitch, UINT srcW, UINT srcH);
    void CpuWorker();
    void Enqueue(const ComPtr<IMFSample>& sample, int64_t time100ns, int64_t sourceQpc);
    bool Configure(IMFTransform* t, bool async, bool gpuInput);
    bool SetTypes(IMFTransform* t, UINT codedW, UINT codedH);
    bool CreateReadback();
    void InitShaderConvert();
    bool ShaderConvert(ID3D11Texture2D* bgra, const D3D11_TEXTURE2D_DESC& sd, ID3D11Texture2D* target, UINT sub);
    bool ProcessorConvert(ID3D11Texture2D* bgra, const D3D11_TEXTURE2D_DESC& sd, ID3D11Texture2D* target, UINT sub);
   // oldest finished read-back -> encoder; false if none was ready
    bool EnsureProcessor(UINT inW, UINT inH);
    void TryFeed();
    void OnTransformEvent(IMFMediaEvent* ev);
    void DrainOutput();
    void Emit(IMFSample* sample);
    void SyncWorker();

    EncoderConfig m_cfg;               // written under m_cfgMutex (read freely by the thread that starts the encoder)
    mutable std::mutex m_cfgMutex;
    std::wstring m_name;
    bool m_async = false;
    bool m_gpuInput = false;          // the MFT reads our NV12 textures directly (same GPU, zero copy)
    UINT m_codedW = 0, m_codedH = 0;  // frame size given to the encoder (>= m_cfg size when it needs aligned sizes)
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_forceKeyframe{false};

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<IMFDXGIDeviceManager> m_manager;
    ComPtr<IMFTransform> m_transform;
    ComPtr<IMFMediaEventGenerator> m_events;
    ComPtr<EventCallback> m_callback;
    HANDLE m_eventsDone = nullptr;
    bool m_providesSamples = false;
    DWORD m_outBufferSize = 0;

    // GPU colour conversion
    ComPtr<ID3D11VideoDevice> m_videoDevice;
    ComPtr<ID3D11VideoContext> m_videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> m_vpEnum;
    ComPtr<ID3D11VideoProcessor> m_vp;
    UINT m_vpInW = 0, m_vpInH = 0;
    ID3D11Texture2D* m_inTex = nullptr;   // identity only (cache key)
    ComPtr<ID3D11VideoProcessorInputView> m_inView;
    std::map<std::pair<ID3D11Texture2D*, UINT>, ComPtr<ID3D11VideoProcessorOutputView>> m_outViews;   // (texture, array slice)
    // Shader colour conversion (BGRA -> NV12 planes), usually cheaper than the video processor. Runs in its
    // own device-context state so the renderer's pipeline state is never disturbed.
    bool m_shaderConvert = false;
    ComPtr<ID3D11DeviceContext1> m_ctx1;
    ComPtr<ID3DDeviceContextState> m_convState;
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_psY, m_psUV;
    ComPtr<ID3D11Buffer> m_encCb;   // HDR: white nits, input is SDR
    ComPtr<ID3D11SamplerState> m_linear;
    ComPtr<ID3D11RasterizerState> m_raster;
    struct SrvEntry {
        ComPtr<ID3D11Texture2D> tex;   // keeps the key alive (no reused addresses)
        ComPtr<ID3D11ShaderResourceView> srv;
    };
    std::vector<SrvEntry> m_srvs;
    struct RtvEntry {
        ComPtr<ID3D11Texture2D> tex;
        UINT sub = 0;
        ComPtr<ID3D11RenderTargetView> y, uv;
    };
    std::vector<RtvEntry> m_rtvs;
    ComPtr<IMFVideoSampleAllocatorEx> m_allocator;
    // read-back path (other GPU / software encoder): NV12 render target -> ring of staging copies; a worker
    // thread maps each one once the GPU has finished it (D3D11_MAP_FLAG_DO_NOT_WAIT) and copies it out, so the
    // render thread only issues the GPU copy. States: 0 free, 1 being written (render thread), 2 waiting, 3 being
    // read (worker). Guarded by m_rbMutex.
    struct Readback {
        ComPtr<ID3D11Texture2D> tex;
        int state = 0;
        uint64_t seq = 0;
        int64_t time = 0, qpc = 0;
    };
    static constexpr int kReadbacks = 4;
    Readback m_rb[kReadbacks];
    uint64_t m_rbSeq = 0;
    std::mutex m_rbMutex;
    std::condition_variable m_rbCv;
    std::thread m_rbThread;
    bool m_rbStop = false;
    void ReadbackWorker();
    ComPtr<ID3D11Texture2D> m_swNv12;
    bool m_cpuConvert = false;   // no video processor (software renderer): BGRA -> NV12 on the CPU
    // CPU path: the render thread only queues copies into staging textures; a worker maps each one once
    // the GPU is done (never waiting) and converts it, so the render thread never stalls.
    struct CpuSlot {
        ComPtr<ID3D11Texture2D> staging;
        UINT w = 0, h = 0;
        int state = 0;   // 0 free, 1 copied (waiting for the GPU / worker), 2 being filled or converted
        uint64_t seq = 0;
        int64_t time = 0, qpc = 0;
    };
    CpuSlot m_cpuSlots[3];
    uint64_t m_cpuSeq = 0;
    std::mutex m_cpuMutex;
    std::condition_variable m_cpuCv;
    std::thread m_cpuThread;
    bool m_cpuStop = false;

    // input queue (both paths)
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<ComPtr<IMFSample>> m_queue;
    int m_needInput = 0;
    int64_t m_t0 = -1;
    struct Fed {
        int64_t time;   // relative sample time we gave the encoder
        int64_t qpc;    // source capture QPC
        int64_t index;  // frame number
    };
    std::deque<Fed> m_fed;   // recent inputs: output time -> real time and source QPC
    int64_t m_fedCount = 0;
    bool m_restampLogged = false;
    std::thread m_syncThread;

    // output
    std::mutex m_outMutex;   // held while a packet is handed over
    std::function<void(const PacketPtr&)> m_output;
    mutable std::mutex m_psMutex;
    std::vector<uint8_t> m_parameterSets;

    // stats
    std::atomic<double> m_latencyMs{0}, m_outFps{0}, m_kbps{0};
    std::atomic<uint64_t> m_dropped{0};
    std::atomic<uint64_t> m_submitted{0}, m_emitted{0};
    bool m_testStall = false;
    bool m_testSlow = false;
    uint64_t m_testCount = 0;
    int64_t m_statStart = 0;
    uint64_t m_statFrames = 0, m_statBytes = 0;
};


// The encoder the app uses (recording, LAN sharing): a stable object that consumers (recorder, replay
// buffer, viewers) attach to, around an EncoderCore that Restart() can replace without a gap, the new core
// is built on a worker thread while the old one keeps encoding, and swapped in between two frames.
class VideoEncoder {
public:
    VideoEncoder() = default;
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    bool Start(ID3D11Device* device, IMFDXGIDeviceManager* manager, const EncoderConfig& cfg);
    void Stop();
    bool Running() const;
    bool Alive() const { return Running() || m_restarting.load(); }   // running, or a replacement is being built
    bool Restarting() const { return m_restarting.load() || m_nextReady.load(); }

    // Render thread. `bgra` may be any size; it is letterboxed into the encoder size.
    void Submit(ID3D11Texture2D* bgra, int64_t time100ns, int64_t sourceQpc);
    // Render thread, after Present: hands finished read-backs (OtherGpu / Cpu targets) to the encoder.
    void Collect();
    void RequestKeyframe();

    // Finds a GPU other than `renderAdapter` with a hardware encoder for the codec (by capability, any
    // vendor). Cached; the first call enumerates encoders (~100 ms), so call it off the render thread when you can.
    static bool FindOtherAdapter(LUID renderAdapter, bool hevc, EncoderAdapter& out) { return EncoderCore::FindOtherAdapter(renderAdapter, hevc, out); }
    static bool OtherAdapterKnown(LUID renderAdapter, bool hevc) { return EncoderCore::OtherAdapterKnown(renderAdapter, hevc); }

    int AddSink(std::function<void(const PacketPtr&)> sink);
    void RemoveSink(int id);

    EncoderConfig Config() const;   // a copy
    std::wstring Name() const;
    bool Hardware() const;
    EncodeTarget Target() const { return Config().target; }   // what is actually used (Cpu when no hardware encoder was usable)
    std::wstring TargetName() const;                          // "hybrid: GeForce GTX 1050" / "display GPU" / "software"
    static inline bool& s_forceSoftware = EncoderCore::s_forceSoftware;              // testing: never use hardware encoders
    static inline std::atomic<bool>& s_testStall = EncoderCore::s_testStall;         // testing: the next core started produces nothing
    static inline UINT& s_testAlign = EncoderCore::s_testAlign;                      // testing: code frame sizes rounded up (AMD-like)
    static inline std::atomic<bool>& s_testSlowOther = EncoderCore::s_testSlowOther; // testing: other-GPU encoders take every 2nd frame
    static inline std::atomic<bool>& s_testSlowSame = EncoderCore::s_testSlowSame;   // testing: display-GPU ones too
    static inline bool& s_videoProcessorOnly = EncoderCore::s_videoProcessorOnly;    // testing: never our conversion shader
    std::vector<uint8_t> ParameterSets() const;
    double LatencyMs() const;
    double OutputFps() const;
    double BitrateKbps() const;
    uint64_t Dropped() const;
    // Watchdog: ~1.5 s of frames went in and nothing came out (seen once with Intel's MFT right after start).
    bool Stalled() const;
    // Builds a replacement core in the background (new settings, or the same after a stall); the old one keeps
    // encoding until the new one is ready, so recordings only lose the frames in flight. Sinks stay attached.
    bool Restart(const EncoderConfig* newConfig = nullptr);
    uint64_t Submitted() const;
    uint64_t Emitted() const;

private:
    std::shared_ptr<EncoderCore> Core() const;
    void Dispatch(const PacketPtr& p);
    void SwapIn();
    void JoinRetired(bool all);

    ComPtr<ID3D11Device> m_device;
    ComPtr<IMFDXGIDeviceManager> m_manager;
    mutable std::mutex m_coreMutex;
    std::shared_ptr<EncoderCore> m_core;   // replaced only on the render thread (Submit) or in Start/Stop
    std::shared_ptr<EncoderCore> m_next;   // built by m_restartThread
    std::atomic<bool> m_nextReady{false};
    int64_t m_lastAccepted = INT64_MIN;   // Submit pacing (render thread)
    std::atomic<bool> m_restarting{false};
    std::thread m_restartThread;
    std::vector<std::thread> m_retired;    // old cores stopping in the background
    EncoderConfig m_lastCfg;

    mutable std::mutex m_sinkMutex;
    std::mutex m_emitMutex;   // held while packets are handed to sinks
    std::map<int, std::function<void(const PacketPtr&)>> m_sinks;
    int m_nextSink = 1;
};

} // namespace cc
