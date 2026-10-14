#pragma once
#include "audio/AudioSource.h"

#include <deque>
#include <memory>
#include <thread>

namespace cc {

struct MixInput {
    std::shared_ptr<AudioSource> source;
    float gain = 1.0f;
    bool gate = false;
    float gateDb = -45.0f;
    bool comp = false;
    float compDb = -18.0f;
    float compRatio = 4.0f;
};

// Mixes several captured sources into one "program" track (48 kHz stereo) for recording and
// streaming in Advanced mode. Each input has its own gain, noise gate and compressor. The output
// is an AudioSource whose taps carry the mix; it is never played locally (monitoring uses the
// per-source path with lower latency).
class ProgramMixer {
public:
    ProgramMixer();
    ~ProgramMixer();

    void Start();
    void Stop();
    bool Running() const { return m_thread.joinable(); }
    void SetInputs(const std::vector<MixInput>& inputs);   // app thread; cheap when nothing changed
    std::shared_ptr<AudioSource> Output() const { return m_bus; }
    float TakePeak() { return m_peak.exchange(0.0f); }

private:
    struct Input {
        MixInput cfg;
        std::shared_ptr<AudioTap> tap;
        std::deque<float> fifo;   // interleaved stereo
        bool primed = false;
        uint32_t rate = 0;   // input sample rate
        double frac = 0.0;   // resampler position between fifo[0] and fifo[1]
        float env = 0.0f;
        float gateGain = 1.0f;
        float compGain = 1.0f;
    };
    void ThreadMain();
    void MixBlock(float* out, uint32_t frames);

    static constexpr uint32_t kRate = 48000;
    static constexpr uint32_t kBlock = 240;   // 5 ms

    std::shared_ptr<AudioSource> m_bus;
    std::mutex m_mutex;
    std::vector<std::unique_ptr<Input>> m_inputs;
    std::thread m_thread;
    HANDLE m_stop = nullptr;
    std::atomic<float> m_peak{0.0f};
};

} // namespace cc
