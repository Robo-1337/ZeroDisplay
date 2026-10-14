#include "audio/AudioCapture.h"
#include "audio/AudioClient.h"
#include "core/Log.h"

namespace cc {

AudioCapture::AudioCapture(std::wstring endpointId, std::shared_ptr<AudioSource> sink)
    : m_id(std::move(endpointId)), m_sink(std::move(sink))
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

AudioCapture::~AudioCapture()
{
    Stop();
    CloseHandle(m_stopEvent);
}

void AudioCapture::Start()
{
    if (m_thread.joinable()) return;
    ResetEvent(m_stopEvent);
    m_failed = false;
    m_thread = std::thread([this] { ThreadMain(); });
}

void AudioCapture::Stop()
{
    SetEvent(m_stopEvent);
    if (m_thread.joinable()) m_thread.join();
}

std::wstring AudioCapture::Description() const
{
    std::lock_guard lock(m_descMutex);
    return m_desc;
}

void AudioCapture::ThreadMain()
{
    ComInit com;
    SetCurrentThreadName(L"Audio capture");
    MmcssScope mmcss(L"Pro Audio", 2);

    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ready) {
        LOGE(L"Audio capture: no event (%lu)", GetLastError());
        m_failed = true;
        return;
    }
    WasapiStream s;
    HRESULT hr = OpenWasapiStream(m_id, eCapture, ready, s);
    ComPtr<IAudioCaptureClient> capture;
    if (SUCCEEDED(hr)) hr = s.client->GetService(IID_PPV_ARGS(&capture));
    if (SUCCEEDED(hr)) hr = s.client->Start();
    if (FAILED(hr)) {
        LOGE(L"Audio capture open failed: %s", HrToString(hr).c_str());
        m_failed = true;
        CloseHandle(ready);
        return;
    }

    {
        std::lock_guard lock(m_descMutex);
        m_desc = Format(L"%s: %s, period %.2f ms%s", s.name.c_str(), s.format.Describe().c_str(),
                        s.periodFrames * 1000.0 / s.format.rate, s.lowLatency ? L" (low-latency)" : L"");
    }
    LOGI(L"Audio capture: %s", Description().c_str());
    m_sink->SetInputFormat(s.format.rate, s.periodFrames);
    m_running = true;

    std::vector<float> tmp;
    HANDLE handles[2] = {m_stopEvent, ready};
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, handles, FALSE, 500);
        if (w == WAIT_OBJECT_0) break;
        UINT32 packet = 0;
        while (SUCCEEDED(hr = capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 qpcPosition = 0;   // capture time of the first frame, 100 ns QPC units
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, &qpcPosition);
            if (FAILED(hr)) break;
            const int64_t time = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) ? 0 : int64_t(qpcPosition);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                m_sink->PushSilence(frames, time);
            } else {
                tmp.resize(size_t(frames) * 2);
                ToStereoFloat(data, frames, s.format, tmp.data());
                m_sink->Push(tmp.data(), frames, time);
            }
            capture->ReleaseBuffer(frames);
        }
        if (FAILED(hr)) {
            LOGW(L"Audio capture stopped: %s", HrToString(hr).c_str());
            m_failed = true;
            break;
        }
    }
    s.client->Stop();
    m_running = false;
    CloseHandle(ready);
}

} // namespace cc
