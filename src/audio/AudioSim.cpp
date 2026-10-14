// Testing (--audio-sim): runs AudioSource between a simulated capture device and a simulated output device, each with
// its own clock error, period, jitter and burst size, and checks that playback settles without underruns. Also
// measures the resampler's quality for common rate pairs and reads / writes every sample format. Deterministic (fixed
// random seed), runs in simulated time.
#include "audio/AudioSim.h"
#include "audio/AudioEngine.h"
#include "audio/AudioSource.h"
#include "audio/Resampler.h"
#include "core/Log.h"

#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <random>

namespace cc {

namespace {

struct SimCase {
    const wchar_t* name;
    uint32_t inRate, outRate;
    double inPeriodMs, outPeriodMs;
    double inPpm, outPpm;              // clock errors: the device really runs at nominal * (1 + ppm / 1e6)
    double inJitterMs, outJitterMs;    // packets / wakeups arrive up to this much late (random)
    int inBurst = 1;                   // packets arrive in groups of this many
    double outBurstMs = 0;             // the device drains its buffer in chunks this long (virtual mixers); 0 = smoothly
    double outStartMs = 0;             // extra audio the device takes over its first 3 s (Bluetooth filling its buffer)
    double seconds = 300;
    double outDipMs = 0;               // every 12 s the device takes this much extra at once, given back over 1 s
};

struct SimResult {
    uint32_t underruns = 0, lateUnderruns = 0, secondHalf = 0, resyncs = 0;
    double fillMs = 0, maxFillMs = 0, driftPpm = 0, reserveMs = 0, inPpm = 0, outPpm = 0, queueMs = 0;
    uint32_t dry = 0;
    std::wstring timeline;   // underruns per 5 s
    std::wstring first;      // the first underruns
};

SimResult RunCase(const SimCase& c, AudioLatency preset)
{
    const int64_t freq = QpcFrequency();
    auto ticks = [&](double sec) { return int64_t(sec * double(freq)); };
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> u01(0.0, 1.0);

    AudioSource src;
    src.SetTestNow(ticks(1.0));
    const uint32_t inPacket = uint32_t(std::lround(c.inRate * c.inPeriodMs / 1000.0));
    src.SetInputFormat(c.inRate, inPacket);

    const double inRateReal = c.inRate * (1.0 + c.inPpm / 1e6);
    const double outRateReal = c.outRate * (1.0 + c.outPpm / 1e6);
    const uint32_t outPeriod = uint32_t(std::lround(c.outRate * c.outPeriodMs / 1000.0));
    OutputQueue queue;
    queue.period = outPeriod;
    queue.buffer = outPeriod * 4;
    queue.rate = c.outRate;
    double lastWake = 0.0;
    const double safety = AudioEngine::SafetyMs(preset);

    std::vector<float> packet(size_t(inPacket) * 2), mix;
    double phase = 0.0;
    double nextIn = 0.0;            // when the next group of packets arrives (seconds)
    double nextOut = 0.0;           // next output wakeup
    uint64_t outWakes = 0;
    double written = 0.0;           // frames handed to the device
    SimResult r;
    double fillSum = 0.0;
    int fillCount = 0;
    const double settle = 20.0;
    uint32_t underrunsAtSettle = 0, underrunsAtHalf = 0;
    bool settled = false, half = false;

    auto inArrival = [&](uint64_t group) {
        // A group of packets is complete when its last frame was captured, then delivered with some delay.
        const double end = double((group + 1) * c.inBurst) * inPacket / inRateReal;
        return end + c.inJitterMs / 1000.0 * u01(rng);
    };
    auto outWake = [&](uint64_t k) { return double(k + 1) * outPeriod / outRateReal + c.outJitterMs / 1000.0 * u01(rng); };
    nextIn = inArrival(0);
    nextOut = outWake(0);
    uint64_t group = 0;
    double nextMark = 5.0;
    uint32_t lastU = 0;

    for (;;) {
        const double t = std::min(nextIn, nextOut);
        if (t > c.seconds) break;
        src.SetTestNow(ticks(1.0 + t));
        if (t >= nextMark) {
            r.timeline += Format(L"%u/%.0f ", src.Underruns() - lastU, src.DriftPpm());
            lastU = src.Underruns();
            nextMark += 5.0;
        }
        if (!half && t >= c.seconds / 2) {
            half = true;
            underrunsAtHalf = src.Underruns();
        }
        if (!settled && t >= settle) {
            settled = true;
            underrunsAtSettle = src.Underruns();
        }
        if (nextIn <= nextOut) {
            for (int b = 0; b < c.inBurst; ++b) {
                for (uint32_t i = 0; i < inPacket; ++i) {
                    const float v = float(0.25 * std::sin(phase));
                    phase += 2.0 * 3.14159265358979 * 1000.0 / c.inRate;
                    packet[2 * i] = packet[2 * i + 1] = v;
                }
                src.Push(packet.data(), inPacket, 0);
            }
            ++group;
            nextIn = std::max(nextIn, inArrival(group));
        } else {
            ++outWakes;
            nextOut = std::max(nextOut, outWake(outWakes));
            // What the device has played by now (a virtual mixer takes whole chunks at a time).
            double consumed = t * outRateReal + c.outRate * c.outStartMs / 1000.0 * std::min(t, 3.0) / 3.0;
            if (c.outDipMs > 0 && t > 6.0) {
                const double since = std::fmod(t - 6.0, 12.0);
                if (since < 1.0) consumed += c.outRate * c.outDipMs / 1000.0 * (1.0 - since);
            }
            if (c.outBurstMs > 0) {
                const double chunk = c.outRate * c.outBurstMs / 1000.0;
                consumed = std::floor(consumed / chunk) * chunk;
            }
            const uint32_t wroteBefore = written > 0 ? 1 : 0;
            if (written < consumed) written = consumed;   // the device ran dry: it played silence meanwhile
            const uint32_t padding = uint32_t(written - consumed);
            if (queue.Wake(padding, wroteBefore, t - lastWake)) src.OutputGap();
            lastWake = t;
            const uint32_t queueTarget = queue.Target(preset);
            if (padding >= queueTarget) continue;
            const uint32_t frames = queueTarget - padding;
            mix.assign(size_t(frames) * 2, 0.0f);
            const double dt = double(frames) / c.outRate;
            const double start = safety + dt * 1000.0 + 1.0;
            const uint32_t before = src.Underruns();
            src.Pull(mix.data(), frames, c.outRate, safety, start, dt, 1.0f);
            if (src.Underruns() != before && src.Underruns() <= 12)
                r.first += Format(L"%.3f(%u,min %.1f, fill %.1f) ", t, frames, src.MinLeftMs(), src.FillMs());
            written += frames;
            if (settled) {
                fillSum += src.FillMs();
                ++fillCount;
                r.maxFillMs = std::max(r.maxFillMs, src.FillMs());
            }
        }
    }
    r.underruns = src.Underruns();
    r.lateUnderruns = src.Underruns() - underrunsAtSettle;
    r.secondHalf = src.Underruns() - underrunsAtHalf;
    r.resyncs = src.Overruns();
    r.fillMs = fillCount ? fillSum / fillCount : 0.0;
    r.driftPpm = src.DriftPpm();
    r.reserveMs = src.TargetMs();
    r.dry = queue.dry;
    r.queueMs = queue.Target(preset) * 1000.0 / c.outRate;
    r.inPpm = src.InClockPpm();
    r.outPpm = src.OutClockPpm();
    return r;
}

// Converts a tone and compares the result with the exact tone at the output rate. Returns the error in dB below the
// signal (or, for a tone above the output's Nyquist frequency, the level that leaked through, in dB).
double ToneTest(uint32_t inRate, uint32_t outRate, double hz)
{
    const double pi = 3.14159265358979323846;
    std::vector<float> in(size_t(inRate) * 2);
    for (uint32_t i = 0; i < inRate; ++i) in[2 * i] = in[2 * i + 1] = float(0.5 * std::sin(2.0 * pi * hz * i / inRate));
    Resampler rs;
    rs.Configure(inRate, outRate);
    std::vector<float> out;
    double first = 0.0;
    const uint32_t n = rs.Process(in.data(), inRate, out, &first);
    const double step = double(inRate) / outRate;
    const bool above = hz > outRate / 2.0;
    double sig = 0.0, err = 0.0;
    for (uint32_t k = n / 10; k < n - n / 10; ++k) {
        const double ideal = above ? 0.0 : 0.5 * std::sin(2.0 * pi * hz * (first + k * step) / inRate);
        sig += 0.25 / 2.0;
        err += (out[2 * k] - ideal) * (out[2 * k] - ideal);
    }
    return 10.0 * std::log10(std::max(err, 1e-30) / sig);
}

// Writes a stereo test signal in a device format and reads it back; for more than two channels also checks the
// downmix (a centre-only signal must reach both sides at -3 dB). Returns the largest error.
double FormatTest(uint16_t bits, bool isFloat, uint16_t channels, DWORD mask, bool extensible)
{
    WAVEFORMATEXTENSIBLE w{};
    w.Format.wFormatTag = extensible ? WAVE_FORMAT_EXTENSIBLE : (isFloat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM);
    w.Format.nChannels = channels;
    w.Format.nSamplesPerSec = 48000;
    w.Format.wBitsPerSample = bits;
    w.Format.nBlockAlign = WORD(channels * bits / 8);
    w.Format.nAvgBytesPerSec = 48000 * w.Format.nBlockAlign;
    w.Format.cbSize = extensible ? WORD(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) : 0;
    w.Samples.wValidBitsPerSample = bits;
    w.dwChannelMask = mask;
    w.SubFormat = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    const PcmFormat f = ParseFormat(&w.Format);
    if (f.kind == SampleKind::Unknown) return 1.0;
    const uint32_t n = 256;
    std::vector<float> in(n * 2), back(n * 2);
    for (uint32_t i = 0; i < n; ++i) {
        in[2 * i] = float(std::sin(i * 0.05)) * 0.9f;
        in[2 * i + 1] = channels == 1 ? in[2 * i] : float(std::cos(i * 0.07)) * -0.6f;
    }
    std::vector<BYTE> dev(size_t(n) * f.blockAlign);
    FromStereoFloat(in.data(), n, f, dev.data());
    ToStereoFloat(dev.data(), n, f, back.data());
    double err = 0.0;
    for (uint32_t i = 0; i < n * 2; ++i) err = std::max(err, double(std::fabs(back[i] - in[i])));
    if (channels > 2) {
        // Centre channel only (its position in the layout = number of lower mask bits that are set).
        int centre = -1, idx = 0;
        for (DWORD m = f.channelMask; m; m &= m - 1, ++idx)
            if ((m & (~m + 1)) == SPEAKER_FRONT_CENTER) centre = idx;
        if (centre >= 0) {
            std::fill(dev.begin(), dev.end(), BYTE(bits == 8 ? 128 : 0));
            // Write 0.5 into the centre channel of every frame through a mono format of the same sample type.
            PcmFormat mono = f;
            mono.channels = 1;
            mono.outLeft = mono.outRight = -1;
            std::vector<float> half(n * 2, 0.5f);
            std::vector<BYTE> tmp(f.blockAlign);
            for (uint32_t i = 0; i < n; ++i) {
                FromStereoFloat(&half[2 * i], 1, mono, tmp.data());
                memcpy(&dev[size_t(i) * f.blockAlign + size_t(centre) * (bits / 8)], tmp.data(), bits / 8);
            }
            ToStereoFloat(dev.data(), n, f, back.data());
            for (uint32_t i = 0; i < n * 2; ++i) err = std::max(err, double(std::fabs(back[i] - 0.5f * 0.7071f)));
        }
    }
    return err;
}

} // namespace

bool AudioSelfTest()
{
    struct Fmt {
        uint16_t bits;
        bool isFloat;
        uint16_t channels;
        DWORD mask;
        bool ext;
        const wchar_t* name;
    };
    const Fmt fmts[] = {
        {8, false, 2, 0, false, L"8-bit stereo"},          {16, false, 1, 0, false, L"16-bit mono"},
        {16, false, 2, 0, false, L"16-bit stereo"},        {24, false, 2, KSAUDIO_SPEAKER_STEREO, true, L"24-bit stereo"},
        {32, false, 2, KSAUDIO_SPEAKER_STEREO, true, L"32-bit int stereo"}, {32, true, 2, KSAUDIO_SPEAKER_STEREO, true, L"float stereo"},
        {64, true, 2, KSAUDIO_SPEAKER_STEREO, true, L"64-bit float stereo"},
        {16, false, 6, KSAUDIO_SPEAKER_5POINT1, true, L"16-bit 5.1"}, {32, true, 8, KSAUDIO_SPEAKER_7POINT1_SURROUND, true, L"float 7.1"},
        {24, false, 8, 0, true, L"24-bit 8 ch, no mask"},  {32, true, 6, 0, false, L"float 6 ch, plain header"},
    };
    bool fmtOk = true;
    for (const auto& f : fmts) {
        const double err = FormatTest(f.bits, f.isFloat, f.channels, f.mask, f.ext);
        const double limit = f.bits == 8 ? 0.02 : f.bits == 16 ? 1e-4 : 1e-6;
        const bool pass = err <= limit;
        fmtOk = fmtOk && pass;
        LOGI(L"[audio-sim] format %-26s largest error %.2g: %s", f.name, err, pass ? L"PASS" : L"FAIL");
    }
    LOGI(L"[audio-sim] formats: %s", fmtOk ? L"PASS" : L"FAIL");

    struct Tone {
        uint32_t in, out;
        double hz, limitDb;
    };
    const Tone tones[] = {
        {48000, 48000, 1000, -140},  {48000, 48000, 15000, -140}, {44100, 48000, 1000, -80},  {48000, 44100, 1000, -80},
        {48000, 96000, 1000, -80},   {48000, 96000, 18000, -60},  {96000, 48000, 1000, -80},  {96000, 48000, 30000, -55},
        {192000, 48000, 1000, -80},  {192000, 48000, 60000, -55}, {32000, 48000, 1000, -80},  {48000, 192000, 5000, -80},
        {88200, 48000, 1000, -80},   {176400, 44100, 1000, -80},  {8000, 48000, 440, -80},    {384000, 48000, 1000, -80},
        {48000, 384000, 1000, -80},  {22050, 48000, 1000, -80},   {11025, 48000, 1000, -80},  {768000, 48000, 1000, -80},
    };
    bool tonesOk = true;
    for (const auto& t : tones) {
        const double db = ToneTest(t.in, t.out, t.hz);
        const bool pass = db <= t.limitDb;
        tonesOk = tonesOk && pass;
        LOGI(L"[audio-sim] resampler %6u -> %6u Hz, %5.0f Hz tone: %s %.1f dB (limit %.0f): %s", t.in, t.out, t.hz,
             t.hz > t.out / 2.0 ? L"leak" : L"error", db, t.limitDb, pass ? L"PASS" : L"FAIL");
    }
    LOGI(L"[audio-sim] resampler: %s", tonesOk ? L"PASS" : L"FAIL");

    const SimCase cases[] = {
        // name, in Hz, out Hz, in period, out period, in ppm, out ppm, in jitter, out jitter, in burst, out burst, out start, seconds
        {L"48k -> 48k, matched", 48000, 48000, 10, 10, 0, 0, 1, 1},
        {L"48k -> 48k, 300 ppm apart", 48000, 48000, 10, 10, 150, -150, 1, 1},
        {L"48k -> 96k (the BEACN case), 100 ppm", 48000, 96000, 10, 10, 50, -50, 1, 1},
        {L"48k -> 96k, out 0.6 % fast", 48000, 96000, 10, 10, 0, 6000, 1, 1},
        {L"48k -> 96k, out 0.7 % fast", 48000, 96000, 10, 10, 0, 7000, 1, 1},
        {L"48k -> 96k, out 0.8 % slow", 48000, 96000, 10, 10, 0, -8000, 1, 1},
        {L"48k -> 96k, in 1.5 % slow", 48000, 96000, 10, 10, -15000, 0, 1, 1},
        {L"48k -> 96k, out 3 % slow", 48000, 96000, 10, 10, 0, -30000, 1, 1},
        {L"48k -> 96k, virtual mixer 20 ms chunks", 48000, 96000, 10, 20, 0, 2000, 2, 2, 1, 20},
        {L"48k -> 96k, jittery wakeups (8 ms)", 48000, 96000, 10, 10, 0, 500, 2, 8},
        {L"48k -> 44.1k", 48000, 44100, 10, 10, 100, 0, 1, 1},
        {L"44.1k -> 48k", 44100, 48000, 10, 10, 0, 100, 1, 1},
        {L"96k -> 48k", 96000, 48000, 10, 10, -200, 0, 1, 1},
        {L"192k -> 48k", 192000, 48000, 10, 10, 0, 0, 1, 1},
        {L"48k -> 192k", 48000, 192000, 10, 3, 0, 300, 1, 1},
        {L"32k -> 48k (USB 2 card)", 32000, 48000, 1, 10, 0, 0, 1, 1},
        {L"16k -> 384k", 16000, 384000, 10, 10, 0, 0, 1, 1},
        {L"48k bursts of 3 packets -> 96k", 48000, 96000, 10, 10, 0, 0, 2, 1, 3},
        {L"48k -> 48k, 1 ms low-latency output", 48000, 48000, 10, 1, 0, 200, 1, 0.5},
        {L"48k -> 48k, Bluetooth start (+60 ms in 3 s)", 48000, 48000, 10, 10, 0, 50, 1, 1, 1, 0, 60},
        {L"48k -> 48k, Ultra, 1 ms jitter", 48000, 48000, 10, 10, 20, -20, 1, 1},
        {L"48k -> 48k, output dips 10 ms every 12 s", 48000, 48000, 10, 10, 0, 20, 1, 1, 1, 0, 0, 300, 10},
        {L"48k -> 48k, Ultra, dips 10 ms every 12 s", 48000, 48000, 10, 10, 0, 20, 1, 1, 1, 0, 0, 300, 10},
        {L"48k -> 48k, 50 ppm, 20 minutes", 48000, 48000, 10, 10, 0, 50, 1, 1, 1, 0, 0, 1200},
        {L"44.1k -> 48k, 30 ppm, 20 minutes", 44100, 48000, 10, 10, 30, 0, 1, 1, 1, 0, 0, 1200},
    };
    bool all = true;
    for (const auto& c : cases) {
        const SimResult r = RunCase(c, wcsstr(c.name, L"Ultra") ? AudioLatency::Ultra : AudioLatency::Low);
        // Settled: at most a short episode of underruns (until the reserve has learned a rare timing pattern), none
        // in the second half of a long run (one allowed in a short one: it can be the episode), a small buffer.
        const bool pass = r.lateUnderruns <= 3 && r.secondHalf <= (c.seconds > 400 ? 0u : 1u) && r.fillMs < 40.0;
        all = all && pass;
        LOGI(L"[audio-sim] %-42s underruns %u (after settling %u, 2nd half %u) resyncs %u, buffer %.1f ms (max %.1f), reserve %.1f ms, "
             L"drift %+.0f ppm (clocks measured: in %+.0f, out %+.0f ppm), output ran dry %u, queue %.1f ms: %s",
             c.name, r.underruns, r.lateUnderruns, r.secondHalf, r.resyncs, r.fillMs, r.maxFillMs, r.reserveMs, r.driftPpm, r.inPpm, r.outPpm,
             r.dry, r.queueMs, pass ? L"PASS" : L"FAIL");
        if (!pass || r.underruns > 5) {
            LOGI(L"[audio-sim]   underruns / drift per 5 s: %s", r.timeline.substr(0, 400).c_str());
            LOGI(L"[audio-sim]   first underruns: %s", r.first.c_str());
        }
    }
    LOGI(L"[audio-sim] controller: %s", all ? L"PASS" : L"FAIL");
    return all && tonesOk && fmtOk;
}

} // namespace cc
