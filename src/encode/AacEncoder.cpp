#include "encode/AacEncoder.h"
#include "core/Log.h"

#include <mfapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cmath>

namespace cc {

bool AacEncoder::Start(uint32_t sampleRate, uint32_t bitrate)
{
    Stop();
    m_rate = sampleRate;
    HRESULT hr = CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_mft));
    if (FAILED(hr)) {
        LOGE(L"AAC encoder unavailable: %s", HrToString(hr).c_str());
        return false;
    }
    ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
    out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitrate / 8);
    out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);   // raw AAC frames
    ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
    in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * 4);
    hr = m_mft->SetOutputType(0, out.Get(), 0);
    if (SUCCEEDED(hr)) hr = m_mft->SetInputType(0, in.Get(), 0);
    if (FAILED(hr)) {
        LOGE(L"AAC encoder setup failed: %s", HrToString(hr).c_str());
        m_mft.Reset();
        return false;
    }
    MFT_OUTPUT_STREAM_INFO info{};
    m_mft->GetOutputStreamInfo(0, &info);
    m_outSize = std::max<DWORD>(info.cbSize, 8192);
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    m_fed = 0;
    return true;
}

void AacEncoder::Stop()
{
    if (m_mft) m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    m_mft.Reset();
}

void AacEncoder::Feed(const float* stereo, uint32_t frames)
{
    if (!m_mft || frames == 0) return;
    const DWORD bytes = frames * 4;
    ComPtr<IMFMediaBuffer> buf;
    BYTE* dst = nullptr;
    if (FAILED(MFCreateMemoryBuffer(bytes, &buf)) || FAILED(buf->Lock(&dst, nullptr, nullptr))) return;
    int16_t* pcm = reinterpret_cast<int16_t*>(dst);
    for (uint32_t i = 0; i < frames * 2; ++i) pcm[i] = int16_t(std::lround(std::clamp(stereo[i], -1.0f, 1.0f) * 32767.0f));
    buf->Unlock();
    buf->SetCurrentLength(bytes);
    ComPtr<IMFSample> s;
    MFCreateSample(&s);
    s->AddBuffer(buf.Get());
    // Windows' AAC encoder rejects samples without a timestamp (MF_E_NO_SAMPLE_TIMESTAMP).
    s->SetSampleTime(int64_t(m_fed) * 10000000 / m_rate);
    s->SetSampleDuration(int64_t(frames) * 10000000 / m_rate);
    m_fed += frames;
    HRESULT hr = m_mft->ProcessInput(0, s.Get(), 0);
    if (FAILED(hr)) {
        static int logged = 0;
        if (logged++ < 3) LOGW(L"AAC encoder input: %s", HrToString(hr).c_str());
    }
}

bool AacEncoder::Drain(std::vector<uint8_t>& frame)
{
    if (!m_mft) return false;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer(m_outSize, &buf))) return false;
    ComPtr<IMFSample> s;
    MFCreateSample(&s);
    s->AddBuffer(buf.Get());
    MFT_OUTPUT_DATA_BUFFER ob{};
    ob.pSample = s.Get();
    DWORD status = 0;
    HRESULT hr = m_mft->ProcessOutput(0, 1, &ob, &status);
    if (ob.pEvents) ob.pEvents->Release();
    if (FAILED(hr)) return false;
    BYTE* p = nullptr;
    DWORD len = 0;
    if (FAILED(buf->Lock(&p, nullptr, &len))) return false;
    frame.assign(p, p + len);
    buf->Unlock();
    return len > 0;
}

} // namespace cc
