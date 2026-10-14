#include "render/Compositor.h"
#include "core/Log.h"

#include <d2d1_1helper.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>

namespace cc {

bool Compositor::Init(Renderer& renderer)
{
    m_renderer = &renderer;
    Reset();
    if (FAILED(renderer.D2DDevice()->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_dc))) return false;
    m_dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (!m_wic && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_wic))))
        m_wic.Reset();   // image sources then say they can't be loaded
    return true;
}

void Compositor::Reset()
{
    m_images.clear();
    m_texts.clear();
    m_dc.Reset();
}

void Compositor::Forget(uint32_t itemId)
{
    m_images.erase(itemId);
    m_texts.erase(itemId);
}

void Compositor::KeepOnly(const std::vector<uint32_t>& itemIds)
{
    auto unused = [&](const auto& kv) { return std::find(itemIds.begin(), itemIds.end(), kv.first) == itemIds.end(); };
    const size_t before = m_images.size() + m_texts.size();
    std::erase_if(m_images, unused);
    std::erase_if(m_texts, unused);
    if (const size_t freed = before - m_images.size() - m_texts.size()) LOGI(L"Memory saver: freed %zu image(s) of scenes not shown", freed);
}

// ---------------------------------------------------------------------------------------------
// Resources

bool Compositor::LoadImage(const std::wstring& path, Texture& out)
{
    if (!m_wic || path.empty()) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(m_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) return false;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;
    if (FAILED(m_wic->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (w == 0 || h == 0 || w > 16384 || h > 16384) return false;
    std::vector<uint8_t> px(size_t(w) * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data()))) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{px.data(), w * 4, 0};
    if (FAILED(m_renderer->Device()->CreateTexture2D(&td, &init, &out.tex))) return false;
    if (FAILED(m_renderer->Device()->CreateShaderResourceView(out.tex.Get(), nullptr, &out.srv))) return false;
    out.width = w;
    out.height = h;
    return true;
}

bool Compositor::RenderText(const SceneItem& item, Texture& out)
{
    if (!m_dc || item.text.empty()) return false;
    IDWriteFactory* dw = m_renderer->DWrite();
    ComPtr<IDWriteTextFormat> fmt;
    // A family that isn't installed (scene from another PC) falls back to Segoe UI inside DirectWrite.
    if (FAILED(dw->CreateTextFormat(item.fontFamily.empty() ? L"Segoe UI" : item.fontFamily.c_str(), nullptr, DWRITE_FONT_WEIGHT(item.fontWeight),
                                    item.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    std::clamp(item.fontSize, 6.0f, 512.0f), L"en-us", &fmt)))
        return false;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dw->CreateTextLayout(item.text.c_str(), UINT32(item.text.size()), fmt.Get(), 8192.0f, 8192.0f, &layout))) return false;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS tm{};
    layout->GetMetrics(&tm);
    const float pad = std::ceil(item.fontSize * 0.08f) + 2.0f;
    const UINT w = UINT(std::ceil(tm.widthIncludingTrailingWhitespace + pad * 2)), h = UINT(std::ceil(tm.height + pad * 2));
    if (w == 0 || h == 0 || w > 8192 || h > 8192) return false;
    layout->SetMaxWidth(float(w));
    layout->SetMaxHeight(float(h));

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(m_renderer->Device()->CreateTexture2D(&td, nullptr, &tex))) return false;
    ComPtr<IDXGISurface> surface;
    tex.As(&surface);
    ComPtr<ID2D1Bitmap1> target;
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(m_dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target))) return false;
    m_dc->SetTarget(target.Get());
    m_dc->BeginDraw();
    m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    const uint32_t c = item.textColor;
    ComPtr<ID2D1SolidColorBrush> shadow, brush;
    m_dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.55f * float(c >> 24) / 255.0f), &shadow);
    m_dc->CreateSolidColorBrush(D2D1::ColorF(float((c >> 16) & 0xFF) / 255.0f, float((c >> 8) & 0xFF) / 255.0f, float(c & 0xFF) / 255.0f,
                                             float(c >> 24) / 255.0f),
                                &brush);
    const float off = std::max(1.0f, item.fontSize * 0.04f);
    m_dc->DrawTextLayout(D2D1::Point2F(pad + off, pad + off), layout.Get(), shadow.Get());
    m_dc->DrawTextLayout(D2D1::Point2F(pad, pad), layout.Get(), brush.Get());
    HRESULT hr = m_dc->EndDraw();
    m_dc->SetTarget(nullptr);
    if (FAILED(hr)) return false;
    out.tex = tex;
    m_renderer->Device()->CreateShaderResourceView(tex.Get(), nullptr, &out.srv);
    out.width = w;
    out.height = h;
    return out.srv != nullptr;
}

Compositor::Texture* Compositor::ImageFor(const SceneItem& item)
{
    Texture& t = m_images[item.id];
    if (t.key != item.path) {
        t = Texture{};
        t.key = item.path;
        t.failed = !LoadImage(item.path, t);
        if (t.failed && !item.path.empty()) LOGW(L"Image could not be loaded: %s", item.path.c_str());
    }
    return t.failed ? nullptr : &t;
}

Compositor::Texture* Compositor::TextFor(const SceneItem& item)
{
    Texture& t = m_texts[item.id];
    std::wstring key = item.text + Format(L"\x1F%.1f\x1F%08X\x1F%s\x1F%d\x1F%d", item.fontSize, item.textColor, item.fontFamily.c_str(),
                                          item.fontWeight, item.italic ? 1 : 0);
    if (t.key != key) {
        t = Texture{};
        t.key = key;
        t.failed = !RenderText(item, t);
    }
    return t.failed ? nullptr : &t;
}

bool Compositor::ContentSize(const SceneItem& item, const DeviceLookup& devices, UINT& w, UINT& h)
{
    switch (item.kind) {
    case SourceKind::Device: {
        DeviceFrame f;
        if (!devices(item, f) || !f.width) return false;
        w = f.width;
        h = f.height;
        return true;
    }
    case SourceKind::Image:
        if (Texture* t = ImageFor(item)) {
            w = t->width;
            h = t->height;
            return true;
        }
        return false;
    case SourceKind::Text:
        if (Texture* t = TextFor(item)) {
            w = t->width;
            h = t->height;
            return true;
        }
        return false;
    default: return false;
    }
}

// ---------------------------------------------------------------------------------------------

void Compositor::Render(RenderTarget& target, const Scene& scene, const DeviceLookup& devices, float backgroundGray)
{
    m_renderer->BeginTarget(target, backgroundGray, backgroundGray, backgroundGray);
    for (const SceneItem& item : scene.items) {
        if (!item.visible || item.opacity <= 0.0f) continue;
        ItemDraw d;
        d.dest = item.rect;
        d.opacity = item.opacity;
        d.filters = &item.filters;
        d.u0 = Clamp(item.cropL, 0.0f, 0.95f);
        d.v0 = Clamp(item.cropT, 0.0f, 0.95f);
        d.u1 = std::max(d.u0 + 0.01f, 1.0f - Clamp(item.cropR, 0.0f, 0.95f));
        d.v1 = std::max(d.v0 + 0.01f, 1.0f - Clamp(item.cropB, 0.0f, 0.95f));
        switch (item.kind) {
        case SourceKind::Device: {
            DeviceFrame f;
            if (!devices(item, f) || !f.srv) continue;
            d.srv = f.srv;
            d.srcW = f.width;
            d.srcH = f.height;
            break;
        }
        case SourceKind::Image: {
            Texture* t = ImageFor(item);
            if (!t) continue;
            d.srv = t->srv.Get();
            d.srcW = t->width;
            d.srcH = t->height;
            d.premultiplied = true;
            break;
        }
        case SourceKind::Text: {
            Texture* t = TextFor(item);
            if (!t) continue;
            d.srv = t->srv.Get();
            d.srcW = t->width;
            d.srcH = t->height;
            d.premultiplied = true;
            break;
        }
        case SourceKind::Color: {
            const uint32_t c = item.color;
            d.solid[0] = float((c >> 16) & 0xFF) / 255.0f;
            d.solid[1] = float((c >> 8) & 0xFF) / 255.0f;
            d.solid[2] = float(c & 0xFF) / 255.0f;
            d.solid[3] = float(c >> 24) / 255.0f;
            break;
        }
        }
        m_renderer->DrawItem(target, d);
    }
}

} // namespace cc
