#include "render/FrameDelay.h"

#include <algorithm>
#include <cmath>

namespace cc {

void FrameDelay::Push(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* src, UINT w, UINT h, int64_t qpc, int delayMs, double fps)
{
    if (!device || !ctx || !src || !w || !h || delayMs <= 0) return;
    if (delayMs != m_delayMs) {   // a new delay starts over (the picture waits once, then runs smoothly)
        Release();
        m_delayMs = delayMs;
    }
    // Frames for the delay at this rate plus two spare; a faster source (120 fps) grows the ring, never past 2 s worth.
    const size_t needed = size_t(std::clamp(std::ceil(delayMs * std::max(fps, 1.0) / 1000.0) + 2.0, 3.0, 128.0));
    if (needed > m_slots.size()) {
        // New empty slots go where the next frame lands, so the ring's order is kept.
        m_slots.insert(m_slots.begin() + ptrdiff_t(m_next), needed - m_slots.size(), Slot{});
    }
    Slot& s = m_slots[m_next];
    m_next = (m_next + 1) % m_slots.size();
    if (!s.tex || s.w != w || s.h != h) {
        s = Slot{};
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &s.tex)) || FAILED(device->CreateShaderResourceView(s.tex.Get(), nullptr, &s.srv))) {
            s = Slot{};
            return;
        }
        s.w = w;
        s.h = h;
    }
    ctx->CopyResource(s.tex.Get(), src);
    s.qpc = qpc;
}

ID3D11ShaderResourceView* FrameDelay::Pick(int64_t now, int delayMs, UINT& w, UINT& h) const
{
    const int64_t due = now - int64_t(double(delayMs) * double(QpcFrequency()) / 1000.0);
    const Slot* best = nullptr;     // newest one old enough
    const Slot* oldest = nullptr;   // while the ring fills up
    for (const Slot& s : m_slots) {
        if (!s.qpc || !s.srv) continue;
        if (s.qpc <= due && (!best || s.qpc > best->qpc)) best = &s;
        if (!oldest || s.qpc < oldest->qpc) oldest = &s;
    }
    if (!best) best = oldest;
    if (!best) return nullptr;
    w = best->w;
    h = best->h;
    m_shownAgeMs = double(now - best->qpc) * 1000.0 / double(QpcFrequency());
    return best->srv.Get();
}

void FrameDelay::Release()
{
    m_slots.clear();
    m_next = 0;
    m_delayMs = 0;
}

uint64_t FrameDelay::Bytes() const
{
    uint64_t b = 0;
    for (const Slot& s : m_slots) b += uint64_t(s.w) * s.h * 4;
    return b;
}

} // namespace cc
