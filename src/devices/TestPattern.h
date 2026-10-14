#pragma once
#include "devices/VideoSource.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace cc {

class AudioSource;

// Synthetic source (1080p60 unless --test-mode WxH@fps) for automated tests when no real signal is available (`--test-pattern`):
// colour bars, a moving box, a frame counter, and a corner square that flips black/white every
// second together with a 1 kHz tone blip (lag checks, A/V sync). Frames are YUY2 in system memory.
class TestPattern : public VideoSource {
public:
    TestPattern(HANDLE frameEvent, std::shared_ptr<AudioSource> audio);
    ~TestPattern() override;

    static constexpr const wchar_t* kLink = L"test:pattern";
    static inline std::wstring s_image;   // testing (--test-image): show this picture at its own size instead
    static inline UINT s_width = 1920, s_height = 1080, s_fpsNum = 60, s_fpsDen = 1;   // --test-mode (59.94 = 60000/1001)
    static bool ParseMode(const std::wstring& mode);   // "WxH@fps", fps may be fractional ("59.94")
    // --test-modes a,b,c: the source changes mode every s_switchSec seconds with a short signal drop between (what an
    // HDMI source does when a game switches resolution / refresh rate).
    static inline std::vector<std::wstring> s_sequence;
    static inline double s_switchSec = 6.0;
    // --test-hdr: 10-bit HDR10 frames (P010, PQ, BT.2020): grey steps from 0.1 to 10000 nits, BT.2020 colours at
    // 203 nits (reference white), a 0 to 1000 nit ramp and a moving 1000-nit box. Known values for the HDR checks.
    static inline bool s_hdr = false;
    static inline uint32_t s_audioRate = 48000;   // testing (--test-audio-rate): the sound's sample rate
    static constexpr float kHdrSteps[11] = {0.0f, 0.1f, 1.0f, 10.0f, 50.0f, 100.0f, 203.0f, 400.0f, 1000.0f, 4000.0f, 10000.0f};
    static bool IsTestLink(const std::wstring& symlink) { return symlink.rfind(L"test:", 0) == 0; }

    void Open(const VideoDeviceInfo& info, const std::wstring& preferredKey, bool preferUncompressed) override;
    void SetFormat(const VideoFormat&) override {}
    void Reopen() override {}
    void Close() override;
    bool TakeFrame(CapturedFrame& out) override;

    CaptureState State() const override { return m_running ? CaptureState::Running : CaptureState::Closed; }
    std::wstring StatusText() const override { return L"Test pattern"; }
    std::vector<VideoFormat> Formats() const override { return {Current()}; }
    VideoFormat Current() const override;
    CaptureStats Stats() const override;
    int64_t LastFrameQpc() const override { return m_lastFrameQpc.load(); }
    bool Busy() const override { return false; }
    bool OwnAudio() const override { return true; }
    bool IsLocalDevice() const override { return false; }

private:
    void Worker();
    void Draw(uint8_t* yuy2, uint64_t frame);
    void BuildBackground();
    void BuildHdrBackground();
    void DrawHdr(uint8_t* p010, uint64_t frame);
    std::vector<uint16_t> m_hdrBackground;   // P010: Y plane, then the interleaved UV plane

    bool LoadImage(const std::wstring& path);
    UINT m_w = 1920, m_h = 1080, m_fpsNum = 60, m_fpsDen = 1;
    bool m_still = false;   // showing s_image
    HANDLE m_frameEvent;
    std::shared_ptr<AudioSource> m_audio;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<int64_t> m_lastFrameQpc{0};
    std::vector<uint8_t> m_background;   // bars, drawn once
    mutable std::mutex m_mutex;
    CapturedFrame m_latest;
    bool m_hasNew = false;
    uint64_t m_frames = 0, m_replaced = 0;
    double m_fps = 0;
};

} // namespace cc
