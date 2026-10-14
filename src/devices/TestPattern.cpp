#include "devices/TestPattern.h"
#include "audio/AudioSource.h"
#include "core/Log.h"

#include <mfapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cc {

namespace {

struct Yuv {
    uint8_t y, u, v;
};

// BT.709, limited range.
Yuv ToYuv(float r, float g, float b)
{
    const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    const float u = -0.1146f * r - 0.3854f * g + 0.5f * b;
    const float v = 0.5f * r - 0.4542f * g - 0.0458f * b;
    auto c = [](float x) { return uint8_t(x < 0 ? 0 : (x > 255 ? 255 : x + 0.5f)); };
    return {c(16 + 219 * y), c(128 + 224 * u), c(128 + 224 * v)};
}

// Clipped to the picture (stride / 2 pixels wide, imgH rows): any test size is safe.
void FillRect(uint8_t* img, UINT stride, UINT imgH, UINT x, UINT y, UINT w, UINT h, Yuv c)
{
    const UINT imgW = stride / 2;
    if (x >= imgW || y >= imgH) return;
    w = std::min(w, imgW - x);
    h = std::min(h, imgH - y);
    x &= ~1u;
    w &= ~1u;
    for (UINT row = y; row < y + h; ++row) {
        uint8_t* p = img + size_t(row) * stride + size_t(x) * 2;
        for (UINT i = 0; i < w; i += 2, p += 4) {
            p[0] = c.y;
            p[1] = c.u;
            p[2] = c.y;
            p[3] = c.v;
        }
    }
}

// 5x7 digits, one row per byte (bit 4 = leftmost column).
const uint8_t kDigits[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}};

} // namespace

TestPattern::TestPattern(HANDLE frameEvent, std::shared_ptr<AudioSource> audio) : m_frameEvent(frameEvent), m_audio(std::move(audio)) {}

TestPattern::~TestPattern() { Close(); }

bool TestPattern::ParseMode(const std::wstring& mode)
{
    unsigned w = 0, h = 0;
    double fps = 0;
    if (swscanf_s(mode.c_str(), L"%ux%u@%lf", &w, &h, &fps) != 3 || w < 16 || h < 16 || w > 16384 || h > 16384 || fps < 1 || fps > 480) return false;
    s_width = w & ~1u;   // YUY2: even width
    s_height = h;
    const double nt = fps * 1.001;   // NTSC-style rates (23.976, 29.97, 59.94, 119.88): exact 1000/1001 fractions
    if (std::fabs(nt - std::round(nt)) < 0.01 && std::fabs(fps - std::round(fps)) > 0.01) {
        s_fpsNum = unsigned(std::round(nt)) * 1000;
        s_fpsDen = 1001;
    } else {
        s_fpsNum = unsigned(std::round(fps * 1000));
        s_fpsDen = 1000;
        while (s_fpsNum % 10 == 0 && s_fpsDen % 10 == 0) {
            s_fpsNum /= 10;
            s_fpsDen /= 10;
        }
    }
    return true;
}

VideoFormat TestPattern::Current() const
{
    std::lock_guard lock(m_mutex);   // the mode can change while running (--test-modes)
    VideoFormat f;
    f.subtype = s_hdr ? MFVideoFormat_P010 : MFVideoFormat_YUY2;
    if (s_hdr) f.transfer = Transfer::Pq;
    f.width = m_w;
    f.height = m_h;
    f.fpsNum = m_fpsNum;
    f.fpsDen = m_fpsDen;
    f.nativeRange = ColorRange::Limited;
    return f;
}

CaptureStats TestPattern::Stats() const
{
    std::lock_guard lock(m_mutex);
    CaptureStats s;
    s.fps = m_fps;
    s.frames = m_frames;
    s.replaced = m_replaced;
    return s;
}

// The picture as YUY2 (BT.709 limited, chroma averaged per pixel pair), cropped to an even width.
bool TestPattern::LoadImage(const std::wstring& path)
{
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    UINT w = 0, h = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(conv->GetSize(&w, &h)) || w < 2 || h < 2)
        return false;
    std::vector<uint8_t> bgra(size_t(w) * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, UINT(bgra.size()), bgra.data()))) return false;
    m_w = w & ~1u;
    m_h = h;
    m_background.assign(size_t(m_w) * 2 * m_h, 0);
    for (UINT y = 0; y < m_h; ++y)
        for (UINT x = 0; x < m_w; x += 2) {
            const uint8_t* a = &bgra[(size_t(y) * w + x) * 4];
            const Yuv p0 = ToYuv(a[2] / 255.0f, a[1] / 255.0f, a[0] / 255.0f), p1 = ToYuv(a[6] / 255.0f, a[5] / 255.0f, a[4] / 255.0f);
            uint8_t* d = &m_background[(size_t(y) * m_w + x) * 2];
            d[0] = p0.y;
            d[1] = uint8_t((p0.u + p1.u + 1) / 2);
            d[2] = p1.y;
            d[3] = uint8_t((p0.v + p1.v + 1) / 2);
        }
    return true;
}

void TestPattern::Open(const VideoDeviceInfo&, const std::wstring&, bool)
{
    if (m_running) return;
    if (!s_sequence.empty()) ParseMode(s_sequence[0]);
    m_w = s_width;
    m_h = s_height;
    m_fpsNum = s_fpsNum;
    m_fpsDen = s_fpsDen;
    m_still = !s_image.empty() && LoadImage(s_image);
    if (!s_image.empty() && !m_still) LOGW(L"Test pattern: couldn't load %s", s_image.c_str());
    if (m_still) {
        if (m_audio) m_audio->SetInputFormat(s_audioRate, s_audioRate * m_fpsDen / m_fpsNum + 1);
        m_running = true;
        m_thread = std::thread([this] { Worker(); });
        LOGI(L"Test pattern: picture %s, %ux%u @ %.3f fps", s_image.c_str(), m_w, m_h, double(m_fpsNum) / m_fpsDen);
        return;
    }
    BuildBackground();
    if (m_audio) m_audio->SetInputFormat(s_audioRate, s_audioRate * m_fpsDen / m_fpsNum + 1);
    m_running = true;
    m_thread = std::thread([this] { Worker(); });
    LOGI(L"Test pattern: %ux%u @ %.3f fps", m_w, m_h, double(m_fpsNum) / m_fpsDen);
}

// Background: 75 % colour bars on top, a grey ramp below.
void TestPattern::BuildBackground()
{
    const UINT stride = m_w * 2;
    m_background.assign(size_t(stride) * m_h, 0);
    const float bars[8][3] = {{.75f, .75f, .75f}, {.75f, .75f, 0}, {0, .75f, .75f}, {0, .75f, 0},
                              {.75f, 0, .75f}, {.75f, 0, 0},     {0, 0, .75f},     {0, 0, 0}};
    const UINT barW = m_w / 8, barsH = m_h * 2 / 3;
    for (UINT i = 0; i < 8; ++i) FillRect(m_background.data(), stride, m_h, i * barW, 0, barW, barsH, ToYuv(bars[i][0], bars[i][1], bars[i][2]));
    for (UINT x = 0; x < m_w; x += 2) {
        const float g = float(x) / float(m_w - 1);
        FillRect(m_background.data(), stride, m_h, x, barsH, 2, m_h - barsH, ToYuv(g, g, g));
    }
}

void TestPattern::Close()
{
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

bool TestPattern::TakeFrame(CapturedFrame& out)
{
    std::lock_guard lock(m_mutex);
    if (!m_hasNew) return false;
    out = m_latest;
    m_hasNew = false;
    return true;
}

// ---- HDR10 test frames ----
namespace {
struct Yuv10 {
    uint16_t y, u, v;
};
float Pq(float nits)   // SMPTE ST 2084 inverse EOTF
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float yp = std::pow(std::clamp(nits / 10000.0f, 0.0f, 1.0f), m1);
    return std::pow((c1 + c2 * yp) / (1.0f + c3 * yp), m2);
}
// Linear BT.2020 light in nits -> 10-bit limited BT.2020 YCbCr (non-constant luminance), stored in the top bits (P010).
Yuv10 ToYuv10(float r, float g, float b)
{
    const float R = Pq(r), G = Pq(g), B = Pq(b);
    const float y = 0.2627f * R + 0.6780f * G + 0.0593f * B;
    const float cb = (B - y) / 1.8814f, cr = (R - y) / 1.4746f;
    auto code = [](float v) { return uint16_t(std::clamp(int(std::lround(v)), 4, 1019) << 6); };
    return {code(64.0f + 876.0f * y), code(512.0f + 896.0f * cb), code(512.0f + 896.0f * cr)};
}
void Fill10(uint16_t* img, UINT w, UINT h, UINT x, UINT y, UINT rw, UINT rh, Yuv10 c)
{
    x &= ~1u, y &= ~1u, rw = (rw + 1) & ~1u, rh = (rh + 1) & ~1u;
    if (x >= w || y >= h) return;
    rw = std::min(rw, w - x), rh = std::min(rh, h - y);
    for (UINT row = y; row < y + rh; ++row) std::fill_n(img + size_t(row) * w + x, rw, c.y);
    uint16_t* uv = img + size_t(w) * h;
    for (UINT row = y / 2; row < (y + rh) / 2; ++row)
        for (UINT col = x / 2; col < (x + rw) / 2; ++col) {
            uv[size_t(row) * w + col * 2] = c.u;
            uv[size_t(row) * w + col * 2 + 1] = c.v;
        }
}
} // namespace

void TestPattern::BuildHdrBackground()
{
    const UINT w = m_w, h = m_h;
    m_hdrBackground.assign(size_t(w) * h * 3 / 2, 0);
    uint16_t* img = m_hdrBackground.data();
    Fill10(img, w, h, 0, 0, w, h, ToYuv10(0, 0, 0));
    // Grey steps (top), one per brightness in kHdrSteps.
    const UINT stepW = w / 11, bandH = h * 2 / 5;
    for (UINT i = 0; i < 11; ++i) Fill10(img, w, h, i * stepW, 0, stepW, bandH, ToYuv10(kHdrSteps[i], kHdrSteps[i], kHdrSteps[i]));
    // BT.2020 colours at reference white (middle): red, green, blue, yellow, cyan, magenta, white.
    const float c[7][3] = {{203, 0, 0}, {0, 203, 0}, {0, 0, 203}, {203, 203, 0}, {0, 203, 203}, {203, 0, 203}, {203, 203, 203}};
    const UINT colW = w / 7, colY = bandH, colH = h / 4;
    for (UINT i = 0; i < 7; ++i) Fill10(img, w, h, i * colW, colY, colW, colH, ToYuv10(c[i][0], c[i][1], c[i][2]));
    // A 0 to 1000 nit ramp (bottom), in steps of 2 pixels.
    const UINT rampY = colY + colH, rampH = h - rampY;
    for (UINT x = 0; x < w; x += 2) {
        const float n = 1000.0f * std::pow(float(x) / float(w - 1), 2.2f);
        Fill10(img, w, h, x, rampY, 2, rampH, ToYuv10(n, n, n));
    }
}

void TestPattern::DrawHdr(uint8_t* out, uint64_t frame)
{
    if (m_hdrBackground.size() != size_t(m_w) * m_h * 3 / 2) BuildHdrBackground();
    uint16_t* img = reinterpret_cast<uint16_t*>(out);
    std::memcpy(img, m_hdrBackground.data(), m_hdrBackground.size() * 2);
    // A moving 1000-nit box on the ramp, so frames change (encoders, latency).
    const UINT box = std::max(16u, m_h / 12), y = m_h - box - m_h / 30;
    const UINT x = UINT(frame * 8 % std::max(1u, m_w - box));
    Fill10(img, m_w, m_h, x, y, box, box, ToYuv10(1000, 1000, 1000));
}

void TestPattern::Draw(uint8_t* img, uint64_t frame)
{
    const UINT stride = m_w * 2;
    memcpy(img, m_background.data(), m_background.size());
    if (m_still) return;
    const Yuv white = ToYuv(1, 1, 1), black = ToYuv(0, 0, 0);
    // Sizes follow the picture (1080p: the original 200 / 240 / 10 px layout).
    const UINT unit = std::max(1u, std::min(m_w, m_h * 16 / 9) / 192);   // 10 at 1080p
    // Flash square (top left): white during even seconds, what lag checks look for.
    const bool on = ((frame * m_fpsDen / m_fpsNum) & 1) == 0;
    FillRect(img, stride, m_h, 4 * unit, 4 * unit, 20 * unit, 20 * unit, on ? white : black);
    // Moving box: shows motion, judder and dropped frames.
    const UINT boxS = 24 * unit, span = m_w > boxS ? m_w - boxS : 1;
    const UINT pos = UINT((frame * (12 * unit / 10 + 1)) % (2 * span));
    const UINT x = pos < span ? pos : 2 * span - pos;
    FillRect(img, stride, m_h, x, std::min(30 * unit, m_h / 3), boxS, boxS, white);
    // Frame counter (bottom left), 7 digits.
    wchar_t digits[16];
    swprintf_s(digits, L"%07llu", (unsigned long long)(frame % 10000000ull));
    const UINT cell = unit, ox = 6 * unit, oy = m_h > 13 * unit ? m_h - 7 * cell - 6 * unit : 0;
    FillRect(img, stride, m_h, ox - 2 * unit, oy >= 2 * unit ? oy - 2 * unit : 0, 7 * 6 * cell + 4 * unit, 7 * cell + 4 * unit, black);
    for (int d = 0; d < 7; ++d) {
        const uint8_t* g = kDigits[digits[d] - L'0'];
        for (UINT r = 0; r < 7; ++r)
            for (UINT c = 0; c < 5; ++c)
                if (g[r] & (0x10 >> c)) FillRect(img, stride, m_h, ox + (UINT(d) * 6 + c) * cell, oy + r * cell, cell, cell, white);
    }
}

void TestPattern::Worker()
{
    ComInit com;
    SetCurrentThreadName(L"Test pattern");
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const int64_t f = QpcFrequency();
    int64_t period = f * m_fpsDen / m_fpsNum;
    const int64_t start = QpcNow();
    int64_t next = start, frameBase = 0;   // frame times from the exact rate (59.94 stays 59.94)
    int64_t timeBase = start;
    size_t seqIndex = 0;
    uint64_t frame = 0, statFrames = 0;
    int64_t statStart = next;
    std::vector<float> audio(size_t(s_audioRate * m_fpsDen / m_fpsNum + 2) * 2);
    double phase = 0;
    while (m_running) {
        // --test-modes: switch like an HDMI source does, the signal drops briefly, then the new mode.
        if (s_sequence.size() > 1 && !m_still) {
            const size_t want = size_t(QpcToMs(QpcNow() - start) / 1000.0 / s_switchSec) % s_sequence.size();
            if (want != seqIndex) {
                seqIndex = want;
                Sleep(800);   // no signal
                if (!m_running) break;
                ParseMode(s_sequence[want]);
                {
                    std::lock_guard lock(m_mutex);
                    m_w = s_width;
                    m_h = s_height;
                    m_fpsNum = s_fpsNum;
                    m_fpsDen = s_fpsDen;
                    m_hasNew = false;
                }
                BuildBackground();
                timeBase = QpcNow();
                frameBase = int64_t(frame + 1);
                next = timeBase;
                period = f * m_fpsDen / m_fpsNum;
                audio.resize(size_t(s_audioRate * m_fpsDen / m_fpsNum + 2) * 2);   // a lower rate needs more samples per frame
                if (m_audio) m_audio->SetInputFormat(s_audioRate, s_audioRate * m_fpsDen / m_fpsNum + 1);
                LOGI(L"Test pattern: switched to %ux%u @ %.3f fps", m_w, m_h, double(m_fpsNum) / m_fpsDen);
            }
        }
        next = timeBase + (int64_t(frame + 1) - frameBase) * f * int64_t(m_fpsDen) / int64_t(m_fpsNum);
        const int64_t wait = next - QpcNow();
        if (wait > 0) {
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -(wait * 10000000 / f);
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, 100);
            } else {
                Sleep(DWORD(wait * 1000 / f));
            }
        } else if (wait < -4 * period) {
            next = QpcNow();   // fell far behind (e.g. debugger): don't burst
            frameBase = int64_t(frame + 1) - (next - timeBase) * int64_t(m_fpsNum) / (f * int64_t(m_fpsDen));
        }

        ComPtr<IMFMediaBuffer> buf;
        ComPtr<IMFSample> sample;
        BYTE* dst = nullptr;
        const DWORD size = s_hdr ? m_w * m_h * 3 : m_w * 2 * m_h;   // P010: 16-bit Y plane + half-height UV plane
        if (FAILED(MFCreateMemoryBuffer(size, &buf)) || FAILED(buf->Lock(&dst, nullptr, nullptr))) continue;
        if (s_hdr) DrawHdr(dst, frame);
        else Draw(dst, frame);
        buf->Unlock();
        buf->SetCurrentLength(size);
        MFCreateSample(&sample);
        sample->AddBuffer(buf.Get());
        const int64_t now = QpcNow();
        CapturedFrame cf;
        cf.sample = sample;
        cf.layout = s_hdr ? PixelLayout::P010 : PixelLayout::YUY2;
        cf.width = m_w;
        cf.height = m_h;
        cf.stride = LONG(m_w * 2);
        cf.matrix = s_hdr ? ColorMatrix::BT2020 : ColorMatrix::BT709;
        cf.range = ColorRange::Limited;
        if (s_hdr) {
            cf.transfer = Transfer::Pq;
            cf.bt2020 = true;
        }
        cf.arrivalQpc = now;
        cf.sampleTime = QpcTo100ns(now);
        cf.seq = frame;
        {
            std::lock_guard lock(m_mutex);
            if (m_hasNew) ++m_replaced;
            m_latest = std::move(cf);
            m_hasNew = true;
            ++m_frames;
            const double el = QpcToMs(now - statStart);
            if (el >= 1000.0) {
                m_fps = (statFrames + 1) * 1000.0 / el;
                statFrames = 0;
                statStart = now;
            } else {
                ++statFrames;
            }
        }
        m_lastFrameQpc = now;
        if (m_frameEvent) SetEvent(m_frameEvent);

        // Audio: a 100 ms 1 kHz blip whenever the flash square turns white, silence otherwise.
        if (m_audio) {
            // Exactly s_audioRate samples per second whatever the frame rate (59.94: 800 or 801 per frame).
            const uint32_t audioFrames = uint32_t((frame + 1) * uint64_t(s_audioRate) * m_fpsDen / m_fpsNum - frame * uint64_t(s_audioRate) * m_fpsDen / m_fpsNum);
            const double sec = double(frame) * m_fpsDen / m_fpsNum;
            const bool blip = std::fmod(sec, 2.0) < 0.1;
            for (uint32_t i = 0; i < audioFrames; ++i) {
                const float s = blip ? 0.25f * float(std::sin(phase)) : 0.0f;
                phase += 2.0 * 3.14159265358979 * 1000.0 / s_audioRate;
                audio[2 * i] = audio[2 * i + 1] = s;
            }
            if (phase > 1e6) phase = std::fmod(phase, 2.0 * 3.14159265358979);
            m_audio->Push(audio.data(), audioFrames, QpcTo100ns(now));
        }
        ++frame;
    }
    if (timer) CloseHandle(timer);
}

} // namespace cc
