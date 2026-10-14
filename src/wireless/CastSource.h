#pragma once
#include "devices/VideoSource.h"

#include <d3d11_1.h>

#include <memory>

namespace cc {

class AudioSource;

// A device casting to this PC (Miracast), or any Windows.Media.Core.MediaSource, as a VideoSource.
// Video: MediaPlayer in frame-server mode copies each frame into our textures (BGRA, GPU). Audio: an
// AudioGraph reads the same source into our AudioSource (mixer, recordings, sharing); if that isn't
// possible the player plays the sound itself (heard, but not recorded).
class CastSource : public VideoSource {
public:
    CastSource(HANDLE frameEvent, std::shared_ptr<AudioSource> audio, ID3D11Device* device);
    ~CastSource() override;

    static bool IsCastLink(const std::wstring& symlink) { return symlink.rfind(L"miracast:", 0) == 0 || symlink.rfind(L"media:", 0) == 0; }
    // `mediaSource` = IInspectable of a Windows.Media.Core.MediaSource. Call before Open.
    void SetMediaSource(IUnknown* mediaSource);
    // The device cast again: play its new stream in this same source (recording, sharing, scenes carry on).
    void Reconnect(IUnknown* mediaSource, const VideoDeviceInfo& info);
    // Testing: a media source for a local video file (exercises the same playback path as a cast).
    static ComPtr<IUnknown> MediaSourceFromFile(const std::wstring& path);

    void Open(const VideoDeviceInfo& info, const std::wstring& preferredKey, bool preferUncompressed) override;
    void SetFormat(const VideoFormat&) override {}
    void Reopen() override {}
    void Close() override;
    bool TakeFrame(CapturedFrame& out) override;
    CaptureState State() const override;
    std::wstring StatusText() const override;
    std::vector<VideoFormat> Formats() const override { return {Current()}; }
    VideoFormat Current() const override;
    CaptureStats Stats() const override;
    int64_t LastFrameQpc() const override;
    bool Busy() const override { return false; }
    bool OwnAudio() const override { return true; }
    bool IsLocalDevice() const override { return false; }
    void Ended();   // the transmitter disconnected

private:
    struct Impl;
    std::shared_ptr<Impl> m;   // the player's event handlers hold it weakly (they may fire while the cast closes)
};

} // namespace cc
