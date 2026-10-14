#pragma once
#include "core/Common.h"

#include <map>

struct ID3D11Device;

namespace cc {

// Self-test diagnostics: where the CPU time and the memory go.
class ResourceProbe {
public:
    // Remembers every thread's CPU time now (the report shows what happened after this point).
    void Mark();
    // "threads: Render 3.1 %, Audio output 0.6 %, ..." (% of one core since Mark), private / working set memory,
    // and the GPU memory this process uses on the device's adapter.
    std::wstring Report(ID3D11Device* device) const;

private:
    std::map<DWORD, uint64_t> m_cpu;   // thread id -> 100 ns of CPU time at Mark()
    int64_t m_markQpc = 0;
};

} // namespace cc
