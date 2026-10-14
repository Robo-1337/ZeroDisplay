#pragma once
#include <cstdint>
#include <deque>
#include <vector>

namespace cc {

// Band-limited interpolation of stereo float audio (windowed sinc, Kaiser window) for any input and output rate.
// The kernel is built for the nominal rates; the step may change a little per call (clock drift steering). When
// downsampling the cutoff moves down to the output's Nyquist frequency, so nothing above it folds back.
// At equal rates and a whole-sample position it passes samples through unchanged.
class SincKernel {
public:
    void Build(uint32_t inRate, uint32_t outRate);
    bool Matches(uint32_t inRate, uint32_t outRate) const { return m_taps && inRate == m_in && outRate == m_out; }
    uint32_t Taps() const { return m_taps; }   // input frames under the kernel (even)
    uint32_t Half() const { return m_taps / 2; }
    // The value at fraction f (0..1) past window frame Half() - 1. `win` = Taps() stereo frames, oldest first.
    void Interpolate(const float* win, double f, float& l, float& r) const;

private:
    uint32_t m_phases = 0;
    uint32_t m_in = 0, m_out = 0, m_taps = 0;
    std::vector<float> m_table;   // m_phases + 1 rows of m_taps coefficients
};

// The last Taps() input frames, stored twice so the window is always one contiguous block.
class SincHistory {
public:
    void Init(uint32_t taps);
    void Clear();
    void Push(float l, float r)
    {
        m_buf[size_t(m_pos) * 2] = m_buf[size_t(m_pos + m_taps) * 2] = l;
        m_buf[size_t(m_pos) * 2 + 1] = m_buf[size_t(m_pos + m_taps) * 2 + 1] = r;
        if (++m_pos == m_taps) m_pos = 0;
    }
    const float* Window() const { return &m_buf[size_t(m_pos) * 2]; }

private:
    std::vector<float> m_buf;
    uint32_t m_taps = 0, m_pos = 0;
};

// Fixed-ratio, push-driven converter (e.g. any capture rate to 48 kHz for recording and sharing).
class Resampler {
public:
    void Configure(uint32_t inRate, uint32_t outRate);
    bool Matches(uint32_t inRate, uint32_t outRate) const { return m_kernel.Matches(inRate, outRate); }
    void Reset();
    // Appends the output for `frames` input frames to `out` (interleaved stereo) and returns how many frames it added.
    // `firstOffset` receives where the first new output frame lies relative to the first input frame, in input frames
    // (negative: the filter's delay), for timestamps.
    uint32_t Process(const float* in, uint32_t frames, std::vector<float>& out, double* firstOffset);

private:
    SincKernel m_kernel;
    SincHistory m_hist;
    double m_pos = 1.0;   // next output position, relative to the window's centre frame
    double m_step = 1.0;
};

// How fast a device's clock really runs, from (time, frames so far) observations. Packets arrive late by varying
// amounts, but the earliest arrival in each half-second block lies on a straight line whose slope is the clock error.
// The slope is the median of the slopes between pairs of those minima over the last minute (Theil-Sen), which ignores
// steps and stretches that don't fit (a Bluetooth output taking audio faster while it starts, a stall), and gives a few
// ppm even with bursty delivery. The first second after a reset is left out (streams settle).
class ClockEstimator {
public:
    void Reset();
    // `sec` = when the observation was made (one steady clock for all devices), `audioSec` = frames so far / nominal rate.
    void Add(double sec, double audioSec);
    // True rate / nominal rate; 0 while there is too little data (or the result is implausible).
    double Ratio() const { return m_ratio; }

private:
    struct Point {
        double x, y;
    };
    void Fit();
    std::deque<Point> m_points;   // block minima
    std::vector<double> m_slopes;
    Point m_blockMin{0, 1e300};
    double m_blockStart = -1.0;
    double m_first = -1.0;   // first observation
    double m_ratio = 0.0;
};

} // namespace cc
