#include "audio/Resampler.h"

#include <algorithm>
#include <cmath>

namespace cc {

static double BesselI0(double x)
{
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 64; ++k) {
        term *= q / (double(k) * double(k));
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

void SincKernel::Build(uint32_t inRate, uint32_t outRate)
{
    m_in = inRate;
    m_out = outRate;
    // 40 taps at 1:1 (20 frames of delay, 0.42 ms at 48 kHz), Kaiser beta 9: flat within 0.003 dB up to 0.83 of the
    // Nyquist frequency (20 kHz at 48 kHz), about -90 dB of error and leakage. Downsampling widens the kernel by the
    // ratio and lowers the cutoff with it.
    const double scale = std::min(1.0, double(outRate) / double(inRate));
    const uint32_t half = uint32_t(std::ceil(20.0 / scale));
    m_taps = half * 2;
    // Coefficients between two table rows are interpolated linearly; the error shrinks with the square of the row
    // spacing and of the cutoff, so a wider (downsampling) kernel needs fewer rows. About -100 dB either way.
    m_phases = std::max(64u, uint32_t(1024.0 * scale));
    const double beta = 9.0, i0b = BesselI0(beta);
    const double pi = 3.14159265358979323846;
    m_table.assign(size_t(m_phases + 1) * m_taps, 0.0f);
    std::vector<double> row(m_taps);
    for (uint32_t p = 0; p <= m_phases; ++p) {
        const double f = double(p) / m_phases;
        double sum = 0.0;
        for (uint32_t t = 0; t < m_taps; ++t) {
            const double x = double(t) - double(half) + 1.0 - f;   // distance from the output point, input frames
            const double u = x / double(half);
            double w = 0.0;
            if (std::fabs(u) < 1.0) w = BesselI0(beta * std::sqrt(1.0 - u * u)) / i0b;
            const double a = pi * scale * x;
            const double s = std::fabs(a) < 1e-9 ? 1.0 : std::sin(a) / a;
            row[t] = s * w;
            sum += row[t];
        }
        // Unity gain at every phase (no level ripple as the position moves).
        for (uint32_t t = 0; t < m_taps; ++t) m_table[size_t(p) * m_taps + t] = float(row[t] / sum);
    }
}

void SincKernel::Interpolate(const float* win, double f, float& l, float& r) const
{
    const double pf = std::clamp(f, 0.0, 1.0) * m_phases;
    uint32_t p = uint32_t(pf);
    if (p >= m_phases) p = m_phases - 1;
    const float a = float(pf - p);
    const float* c0 = &m_table[size_t(p) * m_taps];
    const float* c1 = c0 + m_taps;
    float sl = 0.0f, sr = 0.0f;
    for (uint32_t t = 0; t < m_taps; ++t) {
        const float c = c0[t] + (c1[t] - c0[t]) * a;
        sl += win[2 * t] * c;
        sr += win[2 * t + 1] * c;
    }
    l = sl;
    r = sr;
}

void SincHistory::Init(uint32_t taps)
{
    m_taps = taps;
    m_buf.assign(size_t(taps) * 4, 0.0f);
    m_pos = 0;
}

void SincHistory::Clear()
{
    std::fill(m_buf.begin(), m_buf.end(), 0.0f);
    m_pos = 0;
}

// ---------------------------------------------------------------------------------------------

void Resampler::Configure(uint32_t inRate, uint32_t outRate)
{
    m_kernel.Build(inRate, outRate);
    m_hist.Init(m_kernel.Taps());
    m_step = double(inRate) / double(outRate);
    m_pos = 1.0;
}

void Resampler::Reset()
{
    m_hist.Clear();
    m_pos = 1.0;
}

uint32_t Resampler::Process(const float* in, uint32_t frames, std::vector<float>& out, double* firstOffset)
{
    if (!m_kernel.Taps()) return 0;
    uint32_t produced = 0;
    const double half = double(m_kernel.Half());
    for (uint32_t i = 0; i < frames; ++i) {
        m_hist.Push(in[2 * i], in[2 * i + 1]);
        m_pos -= 1.0;
        while (m_pos < 1.0) {
            float l, r;
            m_kernel.Interpolate(m_hist.Window(), m_pos, l, r);
            if (produced == 0 && firstOffset) *firstOffset = double(i) - half + m_pos;
            out.push_back(l);
            out.push_back(r);
            ++produced;
            m_pos += m_step;
        }
    }
    return produced;
}

// ---------------------------------------------------------------------------------------------

void ClockEstimator::Reset()
{
    m_points.clear();
    m_blockMin = {0, 1e300};
    m_blockStart = -1.0;
    m_first = -1.0;
    m_ratio = 0.0;
}

void ClockEstimator::Add(double sec, double audioSec)
{
    if (m_first < 0.0) m_first = sec;
    if (sec - m_first < 1.0) return;   // let the stream settle
    if (m_blockStart < 0.0) m_blockStart = sec;
    const double y = sec - audioSec;   // how late this audio is compared with a perfect clock (plus a constant)
    if (y < m_blockMin.y) m_blockMin = {audioSec, y};
    if (sec - m_blockStart < 0.5) return;
    m_points.push_back(m_blockMin);
    while (m_points.size() > 120) m_points.pop_front();   // the last minute
    m_blockMin = {0, 1e300};
    m_blockStart = sec;
    Fit();
}

void ClockEstimator::Fit()
{
    // A slope needs some time behind it (a 1 ms timing error over 4 s is 250 ppm). After 1.5 s it is used when the
    // points sit tightly on the line (a clock that is far off shows that early); from 3.5 s on it is always used.
    const double span = m_points.empty() ? 0.0 : m_points.back().x - m_points.front().x;
    if (m_points.size() < 4 || span < 1.5) return;
    m_slopes.clear();
    const size_t n = m_points.size();
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j) {
            const double dx = m_points[j].x - m_points[i].x;
            if (dx >= 1.0) m_slopes.push_back((m_points[j].y - m_points[i].y) / dx);
        }
    if (m_slopes.size() < 3) return;
    auto mid = m_slopes.begin() + ptrdiff_t(m_slopes.size() / 2);
    std::nth_element(m_slopes.begin(), mid, m_slopes.end());
    const double slope = *mid;   // y = audioSec * (nominal / true - 1) + delay
    if (span < 3.5) {
        // Early: only when the points fit the line closely (robust spread of the residuals over the span).
        std::vector<double> res;
        res.reserve(n);
        for (const auto& p : m_points) res.push_back(p.y - slope * p.x);
        auto rm = res.begin() + ptrdiff_t(n / 2);
        std::nth_element(res.begin(), rm, res.end());
        const double med = *rm;
        for (auto& r : res) r = std::fabs(r - med);
        std::nth_element(res.begin(), rm, res.end());
        const double sigma = 1.4826 * *rm;
        if (sigma / span * 2.0 > 3e-4) return;
    }
    const double ratio = 1.0 / (1.0 + slope);
    m_ratio = std::fabs(ratio - 1.0) < 0.08 ? ratio : 0.0;   // more than 8 % off: not a clock, something else
}

} // namespace cc
