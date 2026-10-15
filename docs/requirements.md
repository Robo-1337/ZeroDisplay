# System requirements

[← Back to the README](../README.md)

ZeroDisplay is light: on the oldest PC it's tested on (a 2017 quad-core laptop), watching 1080p60 uses about 4 % of
the CPU and 100 MB of RAM. Most PCs that can run Windows 10 or 11 well will run it.

## Supported Windows versions

| Windows | Status |
|---|---|
| **Windows 11** (64-bit) | Supported. This is where ZeroDisplay is developed and tested. |
| **Windows 10, version 22H2** (64-bit) | Supported. It should work just like on 11, but it gets less testing. (Microsoft ended regular Windows 10 support in October 2025.) |
| Windows 10 before version 2004 | Not supported. The app may start, but casting to the PC needs version 2004 or later. |
| Windows 7, 8, 8.1, or any 32-bit Windows | Not supported (the app won't start). |
| Windows on ARM | Not tested. Windows 11's x64 emulation may run it. |
| Virtual machines and Remote Desktop | Not recommended: without a real GPU it falls back to software rendering, which is too slow for smooth 60 fps. |

Windows "N" editions also need Microsoft's free *Media Feature Pack*; the app tells you if it's missing.

## Minimum and recommended

| | Minimum | Recommended |
|---|---|---|
| **OS** | Windows 10 22H2, 64-bit | Windows 11, 64-bit |
| **CPU** | 4-core, e.g. Intel Core i5-7300HQ (2017) | 6 cores or more, any recent Intel Core / AMD Ryzen |
| **RAM** | 4 GB | 8 GB or more |
| **Graphics** | DirectX 11 GPU with a current driver, e.g. Intel HD Graphics 630 | A GPU with a hardware video encoder: Intel Quick Sync, NVIDIA GTX 10 series or newer (NVENC), or AMD Radeon (VCN) |
| **USB** | USB 2.0 (lower resolutions or MJPEG only) | USB 3.0 port for 1080p60 and up |
| **Storage** | 10 MB for the app | An SSD with room for recordings: about 11 GB per hour at 1080p60 on the default High quality |
| **Capture device** | Any UVC video device: USB / HDMI capture card, webcam, camera with a clean HDMI output | A USB 3.0 capture card that offers uncompressed YUY2 (lowest latency) |

## Extra hardware for optional features

| Feature | Needs |
|---|---|
| HEVC recording | A GPU with an HEVC encoder (Intel 6th gen or newer, NVIDIA GTX 10 or newer, most AMD Radeon). Without one, ZeroDisplay records H.264 and tells you. |
| AI upscaling | NVIDIA RTX 20 / 30 / 40 / 50, or Intel Xe / Arc graphics, with a recent driver (details in [Enhance and AI upscaling](user-guide.md#enhance-and-ai-upscaling)). |
| Casting to this PC | A Wi-Fi adapter that supports Miracast. Check in PowerShell: `netsh wlan show drivers`, look for *Wireless Display Supported: Yes*. |
| This PC's own Wi-Fi network | A Wi-Fi adapter that supports Wi-Fi Direct (most from the last few years). |
| HDR (experimental) | An HDR monitor with *Use HDR* on in Windows' display settings. |

## Tested on

| PC | CPU | Graphics |
|---|---|---|
| Laptop | Intel Core i5-7300HQ | Intel HD Graphics 630 + NVIDIA GeForce GTX 1050 |
| Laptop | Intel Core i9-12900HX | NVIDIA GeForce RTX 4060 Laptop GPU |
| Laptop | Intel Core i5-12500H | Intel integrated graphics only (Iris Xe), no separate GPU |
| Desktop | AMD Ryzen 9 5900X | NVIDIA GeForce RTX 3070 Ti (with an HDR400 monitor) |

Measured on the oldest of them (i5-7300HQ, 1080p60):

| Doing | CPU | RAM |
|---|---|---|
| Watching | about 4 % | about 100 MB |
| Recording with the replay buffer on | about 8 % | about 210 MB |
| Advanced studio mode, recording and sharing at once | about 12 % | about 330 MB |
