#include "render/GpuProfiler.h"

namespace cc {

void GpuProfiler::Init(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
    Shutdown();
    m_ctx = ctx;
    D3D11_QUERY_DESC dj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC ts{D3D11_QUERY_TIMESTAMP, 0};
    for (auto& s : m_slots) {
        if (FAILED(device->CreateQuery(&dj, &s.disjoint))) {
            Shutdown();
            return;
        }
        for (auto& q : s.ts)
            if (FAILED(device->CreateQuery(&ts, &q))) {
                Shutdown();
                return;
            }
    }
}

void GpuProfiler::Shutdown()
{
    for (auto& s : m_slots) s = Slot{};
    m_ctx.Reset();
    m_cur = -1;
    m_valid = false;
}

void GpuProfiler::CpuMark(int next)
{
    const int64_t now = QpcNow();
    if (m_cpuLast && m_cpuSection >= 0 && m_cpuSection < kMarks)
        m_cpuMs[m_cpuSection] = m_cpuMs[m_cpuSection] * 0.95 + QpcToMs(now - m_cpuLast) * 0.05;
    m_cpuLast = now;
    m_cpuSection = next;
}

void GpuProfiler::BeginFrame()
{
    if (!m_ctx) return;
    m_cpuLast = 0;
    CpuMark(0);
    Collect();
    Slot& s = m_slots[m_next];
    if (s.pending) {   // still not readable after kFrames frames: skip this frame
        m_cur = -1;
        return;
    }
    m_cur = m_next;
    m_next = (m_next + 1) % kFrames;
    for (bool& u : s.used) u = false;
    m_ctx->Begin(s.disjoint.Get());
    m_ctx->End(s.ts[0].Get());
    s.used[0] = true;
}

void GpuProfiler::Mark(int index)
{
    if (index <= 0 || index >= kMarks) return;
    CpuMark(index);
    if (m_cur < 0) return;
    Slot& s = m_slots[m_cur];
    m_ctx->End(s.ts[index].Get());
    s.used[index] = true;
}

void GpuProfiler::EndFrame()
{
    CpuMark(-1);
    if (m_cur < 0) return;
    Slot& s = m_slots[m_cur];
    m_ctx->End(s.disjoint.Get());
    s.pending = true;
    m_cur = -1;
}

void GpuProfiler::Collect()
{
    for (auto& s : m_slots) {
        if (!s.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (m_ctx->GetData(s.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 t[kMarks] = {};
        bool ok = true;
        for (int i = 0; i < kMarks && ok; ++i)
            if (s.used[i] && m_ctx->GetData(s.ts[i].Get(), &t[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) ok = false;
        if (!ok) continue;
        s.pending = false;
        if (dj.Disjoint || dj.Frequency == 0) continue;
        const double toMs = 1000.0 / double(dj.Frequency);
        int last = -1;
        double sec[kMarks] = {};
        for (int i = 0; i < kMarks; ++i) {
            if (!s.used[i]) continue;
            if (last >= 0) sec[last] = double(t[i] - t[last]) * toMs;
            last = i;
        }
        const double total = last > 0 ? double(t[last] - t[0]) * toMs : 0.0;
        for (int i = 0; i < kMarks; ++i) m_ms[i] = m_valid ? m_ms[i] * 0.9 + sec[i] * 0.1 : sec[i];
        m_totalMs = m_valid ? m_totalMs * 0.9 + total * 0.1 : total;
        m_valid = true;
    }
}

} // namespace cc
