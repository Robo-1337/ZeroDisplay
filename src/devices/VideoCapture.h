#pragma once
#include "core/Common.h"
#include "devices/VideoSource.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <thread>

namespace cc {

// One capture device: async Source Reader on a dedicated worker thread for open/close/format changes.
class VideoCapture : public VideoSource {
public:
    VideoCapture(IMFDXGIDeviceManager* dxgi, HANDLE frameEvent);
    ~VideoCapture();
    VideoCapture(const VideoCapture&) = delete;
    VideoCapture& operator=(const VideoCapture&) = delete;

    void Open(const VideoDeviceInfo& info, const std::wstring& preferredKey, bool preferUncompressed) override;
    void SetFormat(const VideoFormat& format) override;
    void Reopen() override;   // same device and format (e.g. after a GPU reset)
    void Close() override;

    bool TakeFrame(CapturedFrame& out) override;

    CaptureState State() const override;
    std::wstring StatusText() const override;
    std::vector<VideoFormat> Formats() const override;
    VideoFormat Current() const override;
    CaptureStats Stats() const override;
    UsbLink Link() const override { return m_link.load(); }
    int64_t LastFrameQpc() const override { return m_lastFrameQpc.load(); }
    bool Busy() const override { return m_busy.load(); }
    std::vector<PictureControl> PictureControls(uint64_t* generation) const override;
    void SetPictureControl(int id, long value, bool autoOn) override;

private:
    class Callback;

    void Post(std::function<void()> fn);
    void WorkerLoop();

    void DoOpen(VideoDeviceInfo info, std::wstring preferredKey, bool preferUncompressed);
    void DoClose();
    bool StartReader(const VideoFormat& format);
    void StopReader();
    bool EnumerateFormats();
    void UpdateOutputType();
    void SetState(CaptureState s, std::wstring text);
    void HandleError(HRESULT hr, const wchar_t* where);
    void QueryPictureControls();
    void ApplyPendingControls();

    void OnSample(HRESULT hr, DWORD flags, LONGLONG timestamp, IMFSample* sample);
    void OnFlushDone();

    ComPtr<IMFDXGIDeviceManager> m_dxgi;
    HANDLE m_frameEvent = nullptr;

    // worker
    std::thread m_worker;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<std::function<void()>> m_queue;
    bool m_quit = false;
    std::atomic<bool> m_busy{false};
    std::atomic<UsbLink> m_link{UsbLink::Unknown};

    // owned by worker
    VideoDeviceInfo m_info;
    ComPtr<IMFMediaSource> m_source;
    ComPtr<IMFMediaTypeHandler> m_handler;
    std::vector<ComPtr<IMFMediaType>> m_nativeTypes;
    ComPtr<Callback> m_callback;
    HANDLE m_flushEvent = nullptr;

    // shared with callback
    std::mutex m_readerMutex;
    ComPtr<IMFSourceReader> m_reader;
    bool m_stopping = false;

    // output description + latest frame
    mutable std::mutex m_frameMutex;
    CapturedFrame m_out;   // template: layout/size/stride/color
    CapturedFrame m_latest;
    uint64_t m_seq = 0;

    // info for UI
    mutable std::mutex m_infoMutex;
    CaptureState m_state = CaptureState::Closed;
    std::wstring m_status;
    std::vector<VideoFormat> m_formats;
    VideoFormat m_current;
    CaptureStats m_stats;
    std::vector<PictureControl> m_controls;
    uint64_t m_controlsGen = 0;
    std::map<int, std::pair<long, bool>> m_pendingControls;   // guarded by m_infoMutex
    bool m_controlsTaskQueued = false;
    int64_t m_fpsWindowStart = 0;
    uint64_t m_fpsWindowFrames = 0;
    std::atomic<int64_t> m_lastFrameQpc{0};
};

} // namespace cc
