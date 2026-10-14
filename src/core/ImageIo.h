#pragma once
#include "core/Common.h"

namespace cc {

// Saves 32-bit BGRA pixels (tightly packed) as PNG using WIC. Caller must have COM initialized.
HRESULT SavePng(const std::wstring& path, UINT width, UINT height, const std::vector<uint8_t>& bgra);
// The same as JPEG (quality 0..1).
HRESULT SaveJpeg(const std::wstring& path, UINT width, UINT height, const std::vector<uint8_t>& bgra, float quality = 0.92f);

} // namespace cc
