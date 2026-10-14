#include "core/ImageIo.h"

#include <wincodec.h>

namespace cc {

static HRESULT SaveImage(const std::wstring& path, UINT width, UINT height, const std::vector<uint8_t>& bgra, const GUID& container, float quality)
{
    ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr)) return hr;
    ComPtr<IWICStream> stream;
    if (FAILED(hr = wic->CreateStream(&stream))) return hr;
    if (FAILED(hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return hr;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(hr = wic->CreateEncoder(container, nullptr, &encoder))) return hr;
    if (FAILED(hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return hr;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(hr = encoder->CreateNewFrame(&frame, &props))) return hr;
    if (container == GUID_ContainerFormatJpeg && props) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = quality;
        props->Write(1, &opt, &v);
    }
    if (FAILED(hr = frame->Initialize(props.Get()))) return hr;
    if (FAILED(hr = frame->SetSize(width, height))) return hr;
    // Write as 24-bit BGR: the alpha channel carries no information.
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(hr = frame->SetPixelFormat(&fmt))) return hr;
    std::vector<uint8_t> rgb;
    UINT stride = width * 4;
    const BYTE* src = bgra.data();
    if (fmt == GUID_WICPixelFormat24bppBGR) {
        rgb.resize(size_t(width) * 3 * height);
        for (size_t i = 0, n = size_t(width) * height; i < n; ++i) {
            rgb[i * 3] = bgra[i * 4];
            rgb[i * 3 + 1] = bgra[i * 4 + 1];
            rgb[i * 3 + 2] = bgra[i * 4 + 2];
        }
        stride = width * 3;
        src = rgb.data();
    }
    if (FAILED(hr = frame->WritePixels(height, stride, stride * height, const_cast<BYTE*>(src)))) return hr;
    if (FAILED(hr = frame->Commit())) return hr;
    return encoder->Commit();
}

HRESULT SavePng(const std::wstring& path, UINT width, UINT height, const std::vector<uint8_t>& bgra)
{
    return SaveImage(path, width, height, bgra, GUID_ContainerFormatPng, 1.0f);
}

HRESULT SaveJpeg(const std::wstring& path, UINT width, UINT height, const std::vector<uint8_t>& bgra, float quality)
{
    return SaveImage(path, width, height, bgra, GUID_ContainerFormatJpeg, quality);
}

} // namespace cc
