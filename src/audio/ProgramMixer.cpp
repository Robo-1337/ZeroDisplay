#include "audio/ProgramMixer.h"
#include "core/Log.h"

#include <algorithm>
#include <cmath>

namespace cc {

ProgramMixer::ProgramMixer() : m_bus(std::make_shared<AudioSource>())
{
    m_bus->SetInputFormat(kRate, kBlock);
    m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

ProgramMixer::~ProgramMixer()
{
    Stop();
    CloseHandle(m_stop);
}

void ProgramMixer::Start()
{
    if (m_thread.joinable()) return;
    ResetEvent(m_stop);
    m_thread = std::thread([this] { ThreadMain(); });
}

void ProgramMixer::Stop()
{
    if (!m_thread.joinable()) return;
    SetEvent(m_stop);
    m_thread.join();
    SetInputs({});
}

void ProgramMixer::SetInputs(const std::vector<MixInput>& inputs)
{
    std::lock_guard lock(m_mutex);
    // Remove inputs that are gone.
    for (auto it = m_inputs.begin(); it != m_inputs.end();) {
        bool keep = false;
        for (const auto& m : inputs)
            if (m.source == (*it)->cfg.source) keep = true;
        if (!keep) {
            (*it)->cfg.source->RemoveTap((*it)->tap.get());
            it = m_inputs.erase(it);
        } else {
            ++it;
        }
    }
    // Add new ones, update settings of existing ones.
    for (const auto& m : inputs) {
        if (!m.source) continue;
        Input* found = nullptr;
        for (auto& in : m_inputs)
            if (in->cfg.source == m.source) found = in.get();
        if (found) {
            found->cfg = m;
            continue;
        }
        auto in = std::make_unique<Input>();
        in->cfg = m;
        in->tap = std::make_shared<AudioTap>();
        m.source->AddTap(in->tap);
        m_inputs.push_back(std::move(in));
    }
}

static inline float DbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

void ProgramMixer::MixBlock(float* out, uint32_t frames)
{
    std::fill(out, out + size_t(frames) * 2, 0.0f);
    const float attack = 1.0f - std::exp(-1.0f / (0.005f * kRate));     // 5 ms
    const float release = 1.0f - std::exp(-1.0f / (0.100f * kRate));    // 100 ms
    std::lock_guard lock(m_mutex);
    for (auto& ip : m_inputs) {
        Input& in = *ip;
        AudioChunk c;
        while (in.tap->Pop(c)) {
            if (!c.rate) continue;
            if (c.rate != in.rate) {   // new or changed input rate: start over
                in.rate = c.rate;
                in.fifo.clear();
                in.primed = false;
            }
            in.fifo.insert(in.fifo.end(), c.stereo.begin(), c.stereo.end());
        }
        if (!in.rate) continue;
        // Keep each input's backlog small: start after 20 ms, never beyond 100 ms. Inputs run at their
        // own rate and clock; a linear resampler converts to 48 kHz and is steered (+-0.5 %) to hold the
        // backlog at the target, so clock drift between devices never causes gaps or jumps.
        const size_t target = in.rate / 50 * 2, maxFill = in.rate / 10 * 2;
        if (in.fifo.size() > maxFill) in.fifo.erase(in.fifo.begin(), in.fifo.begin() + ptrdiff_t(in.fifo.size() - target));
        if (!in.primed) {
            if (in.fifo.size() < target) continue;
            in.primed = true;
            in.frac = 0.0;
        }
        const double errSec = (double(in.fifo.size()) - double(target)) / 2.0 / in.rate;
        const double step = double(in.rate) / kRate * (1.0 + std::clamp(errSec * 0.5, -0.005, 0.005));
        const float gateThr = DbToLin(in.cfg.gateDb), compThr = DbToLin(in.cfg.compDb);
        for (uint32_t i = 0; i < frames; ++i) {
            if (in.fifo.size() < 4) {
                in.primed = false;   // ran dry: refill before continuing
                break;
            }
            const float t = float(in.frac);
            float l = in.fifo[0] + (in.fifo[2] - in.fifo[0]) * t;
            float r = in.fifo[1] + (in.fifo[3] - in.fifo[1]) * t;
            in.frac += step;
            while (in.frac >= 1.0 && in.fifo.size() >= 4) {
                in.fifo.pop_front();
                in.fifo.pop_front();
                in.frac -= 1.0;
            }
            const float level = std::max(std::fabs(l), std::fabs(r));
            in.env += (level > in.env ? attack : release) * (level - in.env);
            if (in.cfg.gate) {
                const float want = in.env >= gateThr ? 1.0f : 0.0f;
                in.gateGain += (want > in.gateGain ? attack : release) * (want - in.gateGain);
            } else {
                in.gateGain = 1.0f;
            }
            if (in.cfg.comp && in.env > compThr) {
                // Gain that brings the level down to threshold + (excess / ratio).
                const float excessDb = 20.0f * std::log10(in.env / compThr);
                const float want = DbToLin(-excessDb * (1.0f - 1.0f / std::max(1.0f, in.cfg.compRatio)));
                in.compGain += (want < in.compGain ? attack : release) * (want - in.compGain);
            } else {
                in.compGain += release * (1.0f - in.compGain);
            }
            const float g = in.cfg.gain * in.gateGain * in.compGain;
            out[2 * i] += l * g;
            out[2 * i + 1] += r * g;
        }
    }
    float peak = 0;
    for (uint32_t i = 0; i < frames * 2; ++i) {
        out[i] = std::clamp(out[i], -1.0f, 1.0f);
        peak = std::max(peak, std::fabs(out[i]));
    }
    float cur = m_peak.load();
    while (peak > cur && !m_peak.compare_exchange_weak(cur, peak)) {}
}

void ProgramMixer::ThreadMain()
{
    SetCurrentThreadName(L"Program mixer");
    MmcssScope mmcss(L"Pro Audio", 1);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    LARGE_INTEGER due{};
    due.QuadPart = -10000;   // 1 ms, then every 2 ms
    if (timer) SetWaitableTimer(timer, &due, 2, nullptr, nullptr, FALSE);
    const int64_t start = QpcNow();
    uint64_t produced = 0;
    std::vector<float> block(size_t(kBlock) * 2);
    HANDLE handles[2] = {m_stop, timer};
    // Without a timer (creation failed) wait on the stop event alone with a 2 ms timeout instead of spinning.
    while (WaitForMultipleObjects(timer ? 2 : 1, handles, FALSE, timer ? 50 : 2) != WAIT_OBJECT_0) {
        // Produce exactly 48000 frames per second of wall-clock (QPC) time.
        const int64_t now = QpcNow();
        const uint64_t due_ = uint64_t(QpcToMs(now - start) * kRate / 1000.0);
        while (produced + kBlock <= due_) {
            MixBlock(block.data(), kBlock);
            // Timestamp: roughly when this audio was captured (the inputs run ~20 ms behind).
            const int64_t t = QpcTo100ns(now) - 200000 - int64_t(due_ - produced) * 10000000 / kRate;
            m_bus->Push(block.data(), kBlock, t);
            produced += kBlock;
        }
    }
    if (timer) {
        CancelWaitableTimer(timer);
        CloseHandle(timer);
    }
}

} // namespace cc
