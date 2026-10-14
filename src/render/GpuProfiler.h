#pragma once
#include "core/Common.h"

#include <d3d11.h>

namespace cc {

// GPU time per frame section, measured with timestamp queries and read back a few frames later
// (never waits for the GPU). Sections: everything between consecutive Mark() calls.
class GpuProfiler {
public:
    static constexpr int kMarks = 8;     // up to 7 sections per frame
    static constexpr int kFrames = 4;    // queries in flight

    void Init(ID3D11Device* device, ID3D11DeviceContext* ctx);
    void Shutdown();
    void BeginFrame();          // first mark
    void Mark(int index);       // 1 .. kMarks-1, in increasing order (unused marks are skipped)
    void EndFrame();
    // Smoothed GPU milliseconds of section `s` (between mark s and the next mark that was set), and in total.
    double SectionMs(int s) const { return m_ms[s]; }
    double TotalMs() const { return m_totalMs; }
    // The same sections measured on the CPU (render thread time between the marks), smoothed.
    double CpuSectionMs(int s) const { return m_cpuMs[s]; }
    bool Valid() const { return m_valid; }

private:
    void Collect();

    ComPtr<ID3D11DeviceContext> m_ctx;
    struct Slot {
        ComPtr<ID3D11Query> disjoint;
        ComPtr<ID3D11Query> ts[kMarks];
        bool used[kMarks] = {};
        bool pending = false;
    };
    Slot m_slots[kFrames];
    int m_cur = -1;
    int m_next = 0;
    double m_ms[kMarks] = {};
    double m_totalMs = 0;
    double m_cpuMs[kMarks] = {};
    int64_t m_cpuLast = 0;
    int m_cpuSection = 0;
    void CpuMark(int next);
    bool m_valid = false;
};

} // namespace cc
