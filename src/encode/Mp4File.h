#pragma once
#include "core/Common.h"

#include <mfidl.h>
#include <mfreadwrite.h>

namespace cc {

// A fragmented-MP4 file writer behind the IMFSinkWriter interface, for HEVC (Windows' own fragmented MP4
// sink only takes H.264). Video samples are stored as they come (hev1, parameter sets in-band); PCM audio
// streams are encoded to AAC. A fragment (moof + mdat for every track) is written about once a second,
// so after a crash the file plays up to the last fragment.
// Supports what Recorder uses: AddStream, SetInputMediaType, BeginWriting, WriteSample, Finalize.
HRESULT CreateFragmentedMp4Writer(const std::wstring& path, IMFSinkWriter** writer);

} // namespace cc
