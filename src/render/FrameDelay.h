#pragma once
#include "core/Common.h"

#include <d3d11.h>
#include <vector>

namespace cc {

// Picture delay, screen only: the last pictures stay on the GPU and the one from `delayMs` ago is shown, for
// sound that reaches the ears late (Bluetooth headphones). Recordings and sharing keep the live picture.
// Memory: one BGRA copy per kept frame (1080p ~8 MB), only while a delay is set.
class FrameDelay {
public:
    // Copies the picture into the ring, keeping enough frames for delayMs at `fps`.
    void Push(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* src, UINT w, UINT h, int64_t qpc, int delayMs, double fps);
    // The newest picture taken at least delayMs before `now` (the oldest kept one while the ring fills up), or nullptr.
    ID3D11ShaderResourceView* Pick(int64_t now, int delayMs, UINT& w, UINT& h) const;
    void Release();
    bool Empty() const { return m_slots.empty(); }
    uint64_t Bytes() const;
    double ShownAgeMs() const { return m_shownAgeMs; }   // how old the last picked picture was (stats)

private:
    struct Slot {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        UINT w = 0, h = 0;
        int64_t qpc = 0;   // 0 = empty
    };
    std::vector<Slot> m_slots;
    size_t m_next = 0;
    int m_delayMs = 0;
    mutable double m_shownAgeMs = 0;
};

} // namespace cc
