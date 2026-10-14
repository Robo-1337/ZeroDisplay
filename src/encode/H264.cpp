#include "encode/H264.h"

#include <cstdio>

namespace cc::h264 {

std::vector<Nal> SplitAnnexB(const uint8_t* d, size_t n)
{
    std::vector<Nal> out;
    size_t i = 0;
    auto findStart = [&](size_t from, size_t& codeLen) -> size_t {
        for (size_t k = from; k + 3 <= n; ++k) {
            if (d[k] == 0 && d[k + 1] == 0) {
                if (d[k + 2] == 1) { codeLen = 3; return k; }
                if (k + 4 <= n && d[k + 2] == 0 && d[k + 3] == 1) { codeLen = 4; return k; }
            }
        }
        codeLen = 0;
        return n;
    };
    size_t len = 0;
    i = findStart(0, len);
    while (i < n) {
        size_t begin = i + len;
        size_t nextLen = 0;
        size_t next = findStart(begin, nextLen);
        size_t end = next;
        while (end > begin && d[end - 1] == 0 && next < n) --end;   // trailing zero bytes belong to the next start code
        if (end > begin) out.push_back({d + begin, end - begin});
        i = next;
        len = nextLen;
    }
    return out;
}

std::vector<uint8_t> ExtractParameterSets(const uint8_t* data, size_t size)
{
    std::vector<uint8_t> out;
    for (const Nal& nal : SplitAnnexB(data, size)) {
        if (nal.type() == 7 || nal.type() == 8) {
            const uint8_t sc[4] = {0, 0, 0, 1};
            out.insert(out.end(), sc, sc + 4);
            out.insert(out.end(), nal.data, nal.data + nal.size);
        }
    }
    return out;
}

bool IsKeyframe(const uint8_t* data, size_t size)
{
    for (const Nal& nal : SplitAnnexB(data, size))
        if (nal.type() == 5) return true;
    return false;
}

std::vector<uint8_t> ExtractParameterSetsHevc(const uint8_t* data, size_t size)
{
    std::vector<uint8_t> out;
    for (const Nal& nal : SplitAnnexB(data, size)) {
        const int t = nal.size ? (nal.data[0] >> 1) & 0x3F : -1;
        if (t == 32 || t == 33 || t == 34) {
            const uint8_t sc[4] = {0, 0, 0, 1};
            out.insert(out.end(), sc, sc + 4);
            out.insert(out.end(), nal.data, nal.data + nal.size);
        }
    }
    return out;
}

bool IsKeyframeHevc(const uint8_t* data, size_t size)
{
    for (const Nal& nal : SplitAnnexB(data, size)) {
        const int t = nal.size ? (nal.data[0] >> 1) & 0x3F : -1;
        if (t >= 16 && t <= 21) return true;
    }
    return false;
}

std::string CodecString(const std::vector<uint8_t>& ps)
{
    for (const Nal& nal : SplitAnnexB(ps.data(), ps.size())) {
        if (nal.type() == 7 && nal.size >= 4) {
            char s[32];
            snprintf(s, sizeof(s), "avc1.%02X%02X%02X", nal.data[1], nal.data[2], nal.data[3]);
            return s;
        }
    }
    return {};
}

} // namespace cc::h264
