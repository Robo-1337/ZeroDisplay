#pragma once
#include <cstdint>
#include <memory>

namespace cc {

class AudioSource;

// Audio effect "CaptureCard.CastAudioTap" (registration-free WinRT class declared in app.manifest and
// served by this exe's DllGetActivationFactory export): added to a cast's MediaPlayer, it copies every
// audio buffer the player decodes into an AudioSource, so cast sound goes through our mixer, recordings
// and sharing. Taps are found through ids passed in the effect configuration.
namespace casttap {
constexpr const wchar_t* kClassId = L"CaptureCard.CastAudioTap";
int Register(std::shared_ptr<AudioSource> target);   // returns the id for the configuration ("tap")
void Unregister(int id);
uint64_t Frames(int id);   // frames delivered so far (diagnostics)
// The player decodes sound ahead of time; each buffer is released into the AudioSource when it is due.
// "Due" comes from the video: a frame with media position `media100ns` was shown at `qpc`.
void SetClock(int id, int64_t media100ns, int64_t qpc);
} // namespace casttap

} // namespace cc
