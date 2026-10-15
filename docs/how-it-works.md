# How it works

[← Back to the README](../README.md)

## Built with

Only what ships with Windows:

| | |
|---|---|
| Video | Media Foundation (capture, MJPEG decode), Direct3D 11 (color conversion, scaling, present) |
| Audio | WASAPI (capture + low-latency output), our own drift-compensating resampler |
| UI | Direct2D + DirectWrite, Segoe Fluent Icons |
| Other | WIC (PNG / JPG screenshots), cfgmgr32 (device pairing / hot-plug), MMCSS (thread priorities) |

## Latency design

- **Video:**
  - Each captured frame is presented the moment it arrives. The swap chain uses flip-discard with tearing allowed and a maximum frame latency of 1, and the app waits on the frame-latency object *before* taking the newest frame.
  - Uncompressed YUY2 is preferred over MJPEG because it needs no decoding.
  - Frames arrive as GPU textures and are converted to RGB in a pixel shader. MJPEG is decoded on the GPU.
  - Measured on the dev machine (i5-7300HQ, HD 630, card at 1080p60): about 1.5 ms from a frame arriving to it being presented.
  - The total from the device's timestamp to present is about 19 ms. That is dominated by the USB transfer of a 1080p frame.
- **Audio:**
  - WASAPI runs event-driven at the smallest period the device allows (`IAudioClient3`).
  - Any sample rate (8 to 768 kHz), any sample format (8 / 16 / 24 / 32-bit, float, 64-bit float) and any channel layout (5.1 / 7.1 are mixed down to stereo, centre included) in and out. A band-limited resampler (windowed sinc, about -95 dB, 0.42 ms at 48 kHz) converts between the device and the output.
  - The real clock rates of the capture device and the output are measured, and the resampler follows their ratio; a controller trims the rest so the *lowest* buffer level holds at a small reserve: 1.5, 4 or 15 ms for the Ultra, Low and Safe presets. Outputs that take sound irregularly (Bluetooth, virtual mixers) get a little more room, learned from the dips they cause. F3 shows the measured clocks, the reserve and what was learned.
  - This corrects clock drift between the capture card and your speakers without ever drifting out of sync.
  - Recordings, replays and sharing always get 48 kHz, whatever the device sends.
  - Exclusive output mode is available. If the driver services it in bursts (some Intel SST laptops do), the app detects that and falls back to shared mode automatically.
- **System:**
  - Threads are registered with MMCSS ("Pro Audio" for audio, "Games" for rendering).
  - The timer resolution is 1 ms, and EcoQoS power throttling is turned off for the process.

The statistics overlay (<kbd>F3</kbd>) shows all of these numbers live.
