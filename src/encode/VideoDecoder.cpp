#include "encode/VideoDecoder.h"
#include "core/Log.h"

#include <mferror.h>
#include <strmif.h>   // ICodecAPI
#include <codecapi.h>
#include <wmcodecdsp.h>

namespace cc {

static ComPtr<IMFTransform> CreateDecoder(bool hevc)
{
    ComPtr<IMFTransform> t;
    if (!hevc) {
        if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t)))) return nullptr;
        return t;
    }
    // HEVC: whatever decoder Windows has (the HEVC Video Extensions / a vendor decoder). Sync MFTs only;
    // they handle D3D11 themselves.
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_HEVC};
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in,
                            nullptr, &acts, &count))) {
        for (UINT32 i = 0; i < count && !t; ++i)
            if (FAILED(acts[i]->ActivateObject(IID_PPV_ARGS(&t)))) t.Reset();
        for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
        CoTaskMemFree(acts);
    }
    return t;
}

bool VideoDecoder::Open(IMFDXGIDeviceManager* dxgi, UINT width, UINT height, UINT fpsNum, UINT fpsDen, bool hevc, bool hdr)
{
    Close();
    m_width = width;
    m_height = height;
    m_hdr = hdr;
    m_mft = CreateDecoder(hevc);
    if (!m_mft) {
        LOGW(L"Decoder: no %s decoder on this PC", hevc ? L"HEVC" : L"H.264");
        return false;
    }
    ComPtr<IMFAttributes> attrs;
    m_mft->GetAttributes(&attrs);
    UINT32 d3dAware = 0;
    if (attrs) {
        attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware);
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    m_gpu = d3dAware && dxgi && SUCCEEDED(m_mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, ULONG_PTR(dxgi)));
    ComPtr<ICodecAPI> api;
    if (SUCCEEDED(m_mft.As(&api))) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = TRUE;
        api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    }
    ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fpsNum, std::max(1u, fpsDen));
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    HRESULT hr = m_mft->SetInputType(0, in.Get(), 0);
    if (FAILED(hr) || !SelectOutput()) {
        LOGW(L"Decoder: setup failed: %s", HrToString(hr).c_str());
        m_mft.Reset();
        return false;
    }
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    LOGI(L"Decoder: %s %ux%u on the %s", hevc ? L"HEVC" : L"H.264", width, height, m_gpu ? L"GPU" : L"CPU");
    return true;
}

// NV12 for 8-bit streams; a 10-bit stream (our HDR10 recordings) offers P010, kept at 10 bits.
bool VideoDecoder::SelectOutput()
{
    bool set = false;
    for (const GUID want : {MFVideoFormat_NV12, MFVideoFormat_P010}) {
        for (DWORD i = 0; !set; ++i) {
            ComPtr<IMFMediaType> t;
            if (FAILED(m_mft->GetOutputAvailableType(0, i, &t))) break;
            GUID sub{};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == want && SUCCEEDED(m_mft->SetOutputType(0, t.Get(), 0))) {
                set = true;
                m_p010 = want == MFVideoFormat_P010;
            }
        }
        if (set) break;
    }
    if (!set) return false;
    MFT_OUTPUT_STREAM_INFO info{};
    m_mft->GetOutputStreamInfo(0, &info);
    m_providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    return true;
}

void VideoDecoder::Close()
{
    if (!m_mft) return;
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    m_mft.Reset();
}

void VideoDecoder::Flush()
{
    if (m_mft) m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
}

void VideoDecoder::Drain(const std::function<void(CapturedFrame&&)>& out)
{
    if (!m_mft) return;
    m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    Pull(out);
}

bool VideoDecoder::Decode(const std::vector<uint8_t>& au, int64_t time, const std::function<void(CapturedFrame&&)>& out)
{
    if (!m_mft || au.empty()) return false;
    ComPtr<IMFMediaBuffer> buf;
    BYTE* dst = nullptr;
    if (FAILED(MFCreateMemoryBuffer(DWORD(au.size()), &buf)) || FAILED(buf->Lock(&dst, nullptr, nullptr))) return false;
    memcpy(dst, au.data(), au.size());
    buf->Unlock();
    buf->SetCurrentLength(DWORD(au.size()));
    ComPtr<IMFSample> in;
    MFCreateSample(&in);
    in->AddBuffer(buf.Get());
    in->SetSampleTime(time);
    // GPU decoders work ahead and refuse input while their queue is full: collect finished frames and
    // retry. Never drop an access unit (later frames reference it).
    HRESULT hr = m_mft->ProcessInput(0, in.Get(), 0);
    for (int tries = 0; hr == MF_E_NOTACCEPTING && tries < 200; ++tries) {
        if (!Pull(out)) return false;   // the output can't be set up: give up at once (no waiting in a loop)
        hr = m_mft->ProcessInput(0, in.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) Sleep(tries < 2 ? 0 : 1);
    }
    if (FAILED(hr)) return false;
    return Pull(out);
}

bool VideoDecoder::Pull(const std::function<void(CapturedFrame&&)>& out)
{
    HRESULT hr = S_OK;
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        ComPtr<IMFSample> own;
        if (!m_providesSamples) {
            ComPtr<IMFMediaBuffer> b;
            MFCreateMemoryBuffer(DWORD(m_width) * ((m_height + 31) & ~31u) * 3 / 2 * (m_p010 ? 2 : 1), &b);
            MFCreateSample(&own);
            own->AddBuffer(b.Get());
            ob.pSample = own.Get();
        }
        DWORD status = 0;
        hr = m_mft->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!SelectOutput()) return false;
            continue;
        }
        if (FAILED(hr)) break;   // needs more input
        CapturedFrame f;
        f.sample = ob.pSample;
        if (m_providesSamples && ob.pSample) ob.pSample->Release();   // f.sample holds its own reference
        f.layout = m_p010 ? PixelLayout::P010 : PixelLayout::NV12;
        f.width = m_width;
        f.height = m_height;
        f.stride = LONG(m_width * (m_p010 ? 2 : 1));
        f.matrix = m_hdr ? ColorMatrix::BT2020 : ColorMatrix::BT709;   // what our encoder produces
        f.range = ColorRange::Limited;
        if (m_hdr) {
            f.transfer = Transfer::Pq;
            f.bt2020 = true;
        }
        LONGLONG t = 0;
        if (f.sample) f.sample->GetSampleTime(&t);
        f.sampleTime = t;
        f.arrivalQpc = QpcNow();
        out(std::move(f));
    }
    return true;
}

} // namespace cc
