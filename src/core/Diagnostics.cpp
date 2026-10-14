#include "core/Diagnostics.h"

#include <d3d11.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <vector>

namespace cc {

static uint64_t ThreadCpu(HANDLE t)
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(t, &c, &e, &k, &u)) return 0;
    return ((uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime) + ((uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime);
}

template <typename F>
static void ForEachThread(F f)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof(te)};
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!t) continue;
        f(te.th32ThreadID, t);
        CloseHandle(t);
    }
    CloseHandle(snap);
}

void ResourceProbe::Mark()
{
    m_cpu.clear();
    ForEachThread([&](DWORD id, HANDLE t) { m_cpu[id] = ThreadCpu(t); });
    m_markQpc = QpcNow();
}

std::wstring ResourceProbe::Report(ID3D11Device* device) const
{
    const double sec = m_markQpc ? QpcToMs(QpcNow() - m_markQpc) / 1000.0 : 0;
    struct Row {
        std::wstring name;
        double pct;
    };
    std::vector<Row> rows;
    double total = 0;
    ForEachThread([&](DWORD id, HANDLE t) {
        const uint64_t now = ThreadCpu(t);
        auto it = m_cpu.find(id);
        const uint64_t before = it != m_cpu.end() ? it->second : 0;
        const double pct = sec > 0 ? (now - before) / 1e7 / sec * 100.0 : 0;
        total += pct;
        PWSTR desc = nullptr;
        std::wstring name;
        if (SUCCEEDED(GetThreadDescription(t, &desc)) && desc) {
            name = desc;
            LocalFree(desc);
        }
        if (name.empty()) name = Format(L"#%lu", id);
        rows.push_back({name, pct});
    });
    // Same-named threads (e.g. Media Foundation work queues) are summed.
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.name < b.name; });
    std::vector<Row> merged;
    for (const auto& r : rows) {
        if (!merged.empty() && merged.back().name == r.name && r.name[0] != L'#') merged.back().pct += r.pct;
        else merged.push_back(r);
    }
    std::sort(merged.begin(), merged.end(), [](const Row& a, const Row& b) { return a.pct > b.pct; });
    std::wstring s = Format(L"CPU %.1f %% of one core over %.0f s:", total, sec);
    int shown = 0;
    double other = 0;
    for (const auto& r : merged) {
        if (shown < 10 && r.pct >= 0.1) {
            s += Format(L" %s %.1f,", r.name.c_str(), r.pct);
            ++shown;
        } else {
            other += r.pct;
        }
    }
    s += Format(L" other %.1f", other);
    PROCESS_MEMORY_COUNTERS_EX pm{sizeof(pm)};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm)))
        s += Format(L"; memory: private %.0f MB, working set %.0f MB (peak %.0f MB)", pm.PrivateUsage / 1048576.0, pm.WorkingSetSize / 1048576.0,
                    pm.PeakWorkingSetSize / 1048576.0);
    if (device) {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIAdapter3> a3;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter.As(&a3))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
            a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
            a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
            s += Format(L"; GPU memory: dedicated %.0f MB, shared %.0f MB", local.CurrentUsage / 1048576.0, shared.CurrentUsage / 1048576.0);
        }
    }
    return s;
}

} // namespace cc
