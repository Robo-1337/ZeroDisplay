#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cc {

// QR Code encoder (ISO/IEC 18004): byte mode, error correction level M, versions 1-10 (up to 213 bytes).
// Our own implementation, used for the Wi-Fi join code and the viewer address.
struct QrCode {
    int size = 0;                  // modules per side (21 .. 57); 0 = the text didn't fit
    std::vector<uint8_t> modules;  // size * size, row-major, 1 = dark
    bool Dark(int x, int y) const { return modules[size_t(y) * size_t(size) + size_t(x)] != 0; }

    static QrCode Encode(const std::string& utf8);
};

} // namespace cc
