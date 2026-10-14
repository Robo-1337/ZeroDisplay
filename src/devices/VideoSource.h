#pragma once
#include "core/Common.h"
#include "devices/DeviceManager.h"
#include "devices/UsbLink.h"

#include <mfapi.h>
#include <mfidl.h>

namespace cc {

// P010: 10-bit 4:2:0 (NV12 layout, 16-bit samples); Y210: 10-bit 4:2:2 packed (YUY2 order, 16-bit samples). The
// formats HDR capture cards send.
enum class PixelLayout { None, YUY2, UYVY, NV12, BGRA, P010, Y210 };
enum class ColorMatrix { Auto, BT601, BT709, BT2020 };
// Brightness curve of the signal: SDR (gamma), HDR10 (PQ, SMPTE ST 2084) or HLG (ARIB STD-B67).
enum class Transfer { Sdr, Pq, Hlg };
enum class ColorRange { Auto, Limited, Full };

struct VideoFormat {
    GUID subtype{};
    UINT32 width = 0, height = 0;
    UINT32 fpsNum = 0, fpsDen = 1;
    ColorRange nativeRange = ColorRange::Auto;
    Transfer transfer = Transfer::Sdr;   // as the device reports it for this format

    bool IsCompressed() const;
    double Fps() const { return fpsDen ? double(fpsNum) / double(fpsDen) : 0.0; }
    std::wstring SubtypeName() const;
    std::wstring Label() const;
    std::wstring Key() const;
    bool Matches(const VideoFormat& o) const;
};

struct CapturedFrame {
    ComPtr<IMFSample> sample;
    PixelLayout layout = PixelLayout::None;
    UINT32 width = 0, height = 0;
    LONG stride = 0;
    ColorMatrix matrix = ColorMatrix::Auto;   // as reported by the pipeline
    ColorRange range = ColorRange::Auto;
    Transfer transfer = Transfer::Sdr;        // as reported (HDR sources: PQ / HLG)
    bool bt2020 = false;                      // BT.2020 colour primaries reported
    bool decoded = false;                     // came through the MJPEG decoder
    int64_t arrivalQpc = 0;
    int64_t sampleTime = 0;                   // 100 ns; QPC-based when the source provides it
    uint64_t seq = 0;
};

enum class CaptureState { Closed, Opening, Running, Error, Lost, InUse, Blocked, NeedsPin };   // NeedsPin: a shared stream asks for its PIN

struct CaptureStats {
    double fps = 0;
    uint64_t frames = 0;
    uint64_t replaced = 0;   // frames overwritten before the renderer picked them up
    uint64_t ticks = 0;      // stream gaps reported by the source (or lost network frames)
};

// A picture adjustment the device itself offers (UVC processing unit: brightness, contrast, ...).
struct PictureControl {
    int id = 0;               // VideoProcAmpProperty
    long min = 0, max = 0, step = 1, def = 0, value = 0;
    bool canAuto = false, autoOn = false;
};

// Anything that produces video frames for a Session: a local capture device or a network stream.
class VideoSource {
public:
    virtual ~VideoSource() = default;

    virtual void Open(const VideoDeviceInfo& info, const std::wstring& preferredKey, bool preferUncompressed) = 0;
    virtual void SetFormat(const VideoFormat& format) = 0;
    virtual void Reopen() = 0;
    virtual void Close() = 0;
    virtual bool TakeFrame(CapturedFrame& out) = 0;

    virtual CaptureState State() const = 0;
    virtual std::wstring StatusText() const = 0;
    virtual std::vector<VideoFormat> Formats() const = 0;
    virtual VideoFormat Current() const = 0;
    virtual CaptureStats Stats() const = 0;
    virtual int64_t LastFrameQpc() const = 0;
    virtual bool Busy() const = 0;
    virtual UsbLink Link() const { return UsbLink::Unknown; }   // USB connection speed (local USB devices)
    virtual bool IsNetwork() const { return false; }   // a stream from another ZeroDisplay (never re-shared)
    virtual bool OwnAudio() const { return IsNetwork(); }          // delivers its own audio (no paired audio device)
    virtual bool IsLocalDevice() const { return !IsNetwork(); }    // a capture device Windows lists (rescanned, retried)
    // Device picture adjustments; the generation changes every time the device is (re)opened.
    virtual std::vector<PictureControl> PictureControls(uint64_t* generation = nullptr) const { if (generation) *generation = 0; return {}; }
    virtual void SetPictureControl(int /*id*/, long /*value*/, bool /*autoOn*/) {}
};

} // namespace cc
