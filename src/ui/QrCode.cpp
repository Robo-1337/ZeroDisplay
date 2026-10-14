#include "ui/QrCode.h"

#include <algorithm>
#include <cstdlib>

namespace cc {

namespace {

// Level M, versions 1..10: error correction codewords per block, number of blocks, total codewords.
constexpr int kEccPerBlock[11] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
constexpr int kBlocks[11] = {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};
constexpr int kTotal[11] = {0, 26, 44, 70, 100, 134, 172, 196, 242, 292, 346};
const std::vector<int> kAlign[11] = {{},           {},           {6, 18},      {6, 22},      {6, 26},     {6, 30},
                                     {6, 34},      {6, 22, 38},  {6, 24, 42},  {6, 26, 46},  {6, 28, 50}};

uint8_t GfMul(uint8_t x, uint8_t y)
{
    int z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return uint8_t(z);
}

std::vector<uint8_t> RsDivisor(int degree)
{
    std::vector<uint8_t> r(size_t(degree), 0);
    r.back() = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (size_t j = 0; j < r.size(); ++j) {
            r[j] = GfMul(r[j], root);
            if (j + 1 < r.size()) r[j] ^= r[j + 1];
        }
        root = GfMul(root, 2);
    }
    return r;
}

std::vector<uint8_t> RsRemainder(const std::vector<uint8_t>& data, const std::vector<uint8_t>& div)
{
    std::vector<uint8_t> r(div.size(), 0);
    for (uint8_t b : data) {
        const uint8_t f = uint8_t(b ^ r[0]);
        r.erase(r.begin());
        r.push_back(0);
        for (size_t i = 0; i < r.size(); ++i) r[i] ^= GfMul(div[i], f);
    }
    return r;
}

struct Grid {
    int n;
    std::vector<uint8_t> dark, function;
    explicit Grid(int size) : n(size), dark(size_t(size) * size_t(size)), function(size_t(size) * size_t(size)) {}
    void Set(int x, int y, bool d)
    {
        dark[size_t(y) * size_t(n) + size_t(x)] = d;
        function[size_t(y) * size_t(n) + size_t(x)] = 1;
    }
    bool D(int x, int y) const { return dark[size_t(y) * size_t(n) + size_t(x)] != 0; }
    bool F(int x, int y) const { return function[size_t(y) * size_t(n) + size_t(x)] != 0; }
};

void DrawFormat(Grid& g, int mask)
{
    const int data = (0 << 3) | mask;   // level M = 00
    int rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    const int bits = ((data << 10) | rem) ^ 0x5412;
    auto bit = [&](int i) { return ((bits >> i) & 1) != 0; };
    const int n = g.n;
    for (int i = 0; i <= 5; ++i) g.Set(8, i, bit(i));
    g.Set(8, 7, bit(6));
    g.Set(8, 8, bit(7));
    g.Set(7, 8, bit(8));
    for (int i = 9; i < 15; ++i) g.Set(14 - i, 8, bit(i));
    for (int i = 0; i < 8; ++i) g.Set(n - 1 - i, 8, bit(i));
    for (int i = 8; i < 15; ++i) g.Set(8, n - 15 + i, bit(i));
    g.Set(8, n - 8, true);   // the dark module
}

void DrawFunctionPatterns(Grid& g, int version)
{
    const int n = g.n;
    for (int i = 0; i < n; ++i) {   // timing
        g.Set(6, i, i % 2 == 0);
        g.Set(i, 6, i % 2 == 0);
    }
    auto finder = [&](int cx, int cy) {
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx) {
                const int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= n || y >= n) continue;
                const int d = std::max(std::abs(dx), std::abs(dy));
                g.Set(x, y, d != 2 && d != 4);
            }
    };
    finder(3, 3);
    finder(n - 4, 3);
    finder(3, n - 4);
    const auto& al = kAlign[version];
    for (size_t i = 0; i < al.size(); ++i)
        for (size_t j = 0; j < al.size(); ++j) {
            if ((i == 0 && j == 0) || (i == 0 && j == al.size() - 1) || (i == al.size() - 1 && j == 0)) continue;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) g.Set(al[i] + dx, al[j] + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
        }
    DrawFormat(g, 0);   // reserve the format areas (overwritten once the mask is chosen)
    if (version >= 7) {
        int rem = version;
        for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        const int bits = (version << 12) | rem;
        for (int i = 0; i < 18; ++i) {
            const bool b = ((bits >> i) & 1) != 0;
            const int a = n - 11 + i % 3, c = i / 3;
            g.Set(a, c, b);
            g.Set(c, a, b);
        }
    }
}

bool MaskBit(int mask, int x, int y)
{
    switch (mask) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

void ApplyMask(Grid& g, int mask)
{
    for (int y = 0; y < g.n; ++y)
        for (int x = 0; x < g.n; ++x)
            if (!g.F(x, y) && MaskBit(mask, x, y)) g.dark[size_t(y) * size_t(g.n) + size_t(x)] ^= 1;
}

long Penalty(const Grid& g)
{
    const int n = g.n;
    long p = 0;
    auto line = [&](bool rows) {
        for (int a = 0; a < n; ++a) {
            int run = 0;
            bool last = false;
            std::string seq;
            for (int b = 0; b < n; ++b) {
                const bool d = rows ? g.D(b, a) : g.D(a, b);
                seq.push_back(d ? '1' : '0');
                if (b > 0 && d == last) ++run;
                else {
                    if (run >= 5) p += 3 + (run - 5);
                    run = 1;
                }
                last = d;
            }
            if (run >= 5) p += 3 + (run - 5);
            // Finder-like 1:1:3:1:1 with four light modules on one side.
            const std::string padded = std::string(4, '0') + seq + std::string(4, '0');
            for (size_t i = 0; i + 11 <= padded.size(); ++i) {
                const std::string_view w(padded.data() + i, 11);
                if (w == "10111010000" || w == "00001011101") p += 40;
            }
        }
    };
    line(true);
    line(false);
    for (int y = 0; y + 1 < n; ++y)
        for (int x = 0; x + 1 < n; ++x) {
            const bool d = g.D(x, y);
            if (d == g.D(x + 1, y) && d == g.D(x, y + 1) && d == g.D(x + 1, y + 1)) p += 3;
        }
    long dark = 0;
    for (uint8_t v : g.dark) dark += v;
    const long total = long(n) * n;
    const long k = (std::labs(dark * 20 - total * 10) + total - 1) / total - 1;
    p += k * 10;
    return p;
}

} // namespace

QrCode QrCode::Encode(const std::string& text)
{
    QrCode out;
    int version = 0;
    for (int v = 1; v <= 10; ++v) {
        const int dataBytes = kTotal[v] - kEccPerBlock[v] * kBlocks[v];
        const int ccBits = v < 10 ? 8 : 16;
        if (4 + ccBits + int(text.size()) * 8 <= dataBytes * 8) {
            version = v;
            break;
        }
    }
    if (!version) return out;
    const int n = version * 4 + 17;
    const int dataBytes = kTotal[version] - kEccPerBlock[version] * kBlocks[version];

    // Data: mode 0100 (byte), character count, bytes, terminator, padding.
    std::vector<int> bits;
    auto put = [&](uint32_t v, int count) {
        for (int i = count - 1; i >= 0; --i) bits.push_back((v >> i) & 1);
    };
    put(4, 4);
    put(uint32_t(text.size()), version < 10 ? 8 : 16);
    for (unsigned char c : text) put(c, 8);
    put(0, std::min(4, dataBytes * 8 - int(bits.size())));
    while (bits.size() % 8) bits.push_back(0);
    std::vector<uint8_t> data;
    for (size_t i = 0; i < bits.size(); i += 8) {
        uint8_t b = 0;
        for (int k = 0; k < 8; ++k) b = uint8_t((b << 1) | bits[i + size_t(k)]);
        data.push_back(b);
    }
    for (uint8_t pad = 0xEC; int(data.size()) < dataBytes; pad ^= 0xEC ^ 0x11) data.push_back(pad);

    // Blocks + error correction, interleaved.
    const int blocks = kBlocks[version], ecc = kEccPerBlock[version], total = kTotal[version];
    const int shortBlocks = blocks - total % blocks, shortLen = total / blocks;
    const std::vector<uint8_t> div = RsDivisor(ecc);
    std::vector<std::vector<uint8_t>> parts;
    for (int i = 0, k = 0; i < blocks; ++i) {
        const int dlen = shortLen - ecc + (i < shortBlocks ? 0 : 1);
        std::vector<uint8_t> d(data.begin() + k, data.begin() + k + dlen);
        k += dlen;
        const std::vector<uint8_t> e = RsRemainder(d, div);
        if (i < shortBlocks) d.push_back(0);   // placeholder, skipped when interleaving
        d.insert(d.end(), e.begin(), e.end());
        parts.push_back(std::move(d));
    }
    std::vector<uint8_t> words;
    for (size_t i = 0; i < parts[0].size(); ++i)
        for (int j = 0; j < blocks; ++j)
            if (i != size_t(shortLen - ecc) || j >= shortBlocks) words.push_back(parts[size_t(j)][i]);

    Grid g(n);
    DrawFunctionPatterns(g, version);
    // Zig-zag placement, two columns at a time from the bottom right, skipping the vertical timing column.
    size_t bit = 0;
    for (int right = n - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < n; ++vert)
            for (int j = 0; j < 2; ++j) {
                const int x = right - j;
                const bool upward = ((right + 1) & 2) == 0;
                const int y = upward ? n - 1 - vert : vert;
                if (g.F(x, y) || bit >= words.size() * 8) continue;
                g.dark[size_t(y) * size_t(n) + size_t(x)] = (words[bit >> 3] >> (7 - (bit & 7))) & 1;
                ++bit;
            }
    }
    // Pick the mask with the lowest penalty.
    int best = 0;
    long bestScore = -1;
    for (int m = 0; m < 8; ++m) {
        Grid t = g;
        ApplyMask(t, m);
        DrawFormat(t, m);
        const long s = Penalty(t);
        if (bestScore < 0 || s < bestScore) {
            bestScore = s;
            best = m;
        }
    }
    ApplyMask(g, best);
    DrawFormat(g, best);
    out.size = n;
    out.modules = g.dark;
    return out;
}

} // namespace cc
