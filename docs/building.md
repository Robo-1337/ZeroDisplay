# Building from source

[← Back to the README](../README.md)

## Building

Requirements: Visual Studio 2022 Build Tools (C++ workload), Windows SDK 10.0.22621 or newer,
CMake ≥ 3.24, and Ninja.

```powershell
.\build.ps1            # Release -> out\ZeroDisplay.exe
.\build.ps1 -Clean     # full rebuild
```

The C runtime is linked statically and the shaders are compiled at build time and embedded, so the
exe depends only on Windows system DLLs, and it loads them from System32 only (a DLL with the same name placed
next to the exe, e.g. in a Downloads folder, is never used).

## Self-test

```powershell
out\ZeroDisplay.exe --selftest 30 --cycle --shot frame.png --ui-shot ui.png
```

| Option | What it does |
|---|---|
| `--selftest N` | Runs for N seconds, logs statistics once a second, then exits |
| `--cycle` | Switches through several capture formats during the run |
| `--multiview`, `--multiview-devices "a\|b"` | Opens every capture device at once / only the devices whose names contain one of these |
| `--device-window <name>` | Opens the device whose name contains `<name>` in its own window |
| `--exclusive` | Tries exclusive audio output |
| `--shot` | Saves the captured picture at the end |
| `--ui-shot` | Saves the whole window at the end, with the stats and settings panels open |
| `--device <name>` | Opens the first device whose name contains `<name>` |
| `--record` | Records from 2 s into the run until the end |
| `--share` | Shares the active device on the LAN (and starts the browser viewer) |
| `--connect <ip[:port]>` | Opens a network stream from another ZeroDisplay |
| `--advanced` / `--demo-scene` | Starts in Advanced mode, optionally with a demo scene (not saved) |
| `--demo-black`, `--demo-font <name>` | The demo scene on a black background (for auto-crop) / its title in this font |
| `--mode-switch` | Switches Simple to Advanced at 35 % of the run and back at 65 % |
| `--studio` | Studio mode, with a transition just before the end |
| `--studio-test`, `--ui-select <0\|2\|3>` | Studio mode check: 8 timed transitions, number keys pick the preview, an edit in the preview leaves the program alone / with `--ui-shot`: selects the first device, text or color source |
| `--replay` | Replay buffer on; a replay is saved just before the end |
| `--gpu <n\|warp\|auto>`, `--software-encoder` | Pick a graphics adapter / software rendering; never use hardware encoders |
| `--encode-hw <auto\|hybrid\|igpu\|software>` | Encoding hardware for this run |
| `--encode-align <n>`, `--hybrid-slow-test`, `--display-slow-test` | Code frame sizes rounded up to *n* lines (as some AMD encoders need) / make the second GPU's encoder fall behind (Auto must move the recording) / make the display GPU's encoders fall behind too (Auto must move it back) |
| `--encoder-speed <0-3>`, `--encoder-stall-test`, `--encode-convert-vp` | Encoder preset for this run (Auto / Fast / Balanced / Best) / the first encoder produces nothing (the watchdog must restart it) / color conversion for the encoder by the video processor instead of the shader |
| `--stream-quality <0-4>`, `--net-loss <percent>` | Sharing quality for this run (smart, or 4 / 8 / 15 / 25 Mbps) / the receiver drops this share of video packets |
| `--settings-scroll <px>` | Scrolls the settings panel for `--ui-shot` |
| `--hevc`, `--split <s>`, `--limit <s>`, `--tracks`, `--mic <name>` | Recording options |
| `--standby-test` | Simulates unplugging the active device halfway through |
| `--autocrop`, `--filter <0-4>`, `--filter-lines <n>` | Auto-crop and screen filters |
| `--flashback-test`, `--trim <file>` | Opens Flashback (and saves a clip) / opens the trimmer and cuts 30–70 % |
| `--mini`, `--clean` | Mini player / clean output |
| `--lagcheck-sim <ms>` | Runs the lag check against a simulated capture chain |
| `--audio-skew <percent>` | Simulates a capture device whose audio clock is off |
| `--audio-sim` | Sound self-test without devices: clock drift / jitter / burst scenarios, resampler accuracy, every sample format (PASS / FAIL in the log) |
| `--test-audio-rate <hz>` | The test pattern's sound at any sample rate (8000 to 768000) |
| `--test-pattern` | Offers a synthetic 1080p60 source "Test pattern" (flash square, frame counter, 1 kHz blips) |
| `--wireless`, `--wireless-view` | Starts the cast receiver / opens the Wireless tab |
| `--no-wireless`, `--wireless-toggle-test` | Wireless features off for this run / turns them off at 35 % and on at 65 % |
| `--hotspot`, `--hotspot-toggle-test` | Starts the PC's own Wi-Fi / turns it off at 35 % and on at 65 % |
| `--record-cast`, `--flip-log` | Records a cast once it shows a picture / logs when the picture turns bright or dark (latency tests) |
| `--media-test <file>` | Plays a video file as if it were a cast |
| `--profile-dir <dir>`, `--portable-toggle-test` | Settings in another folder / turns Portable on and off (only with `--profile-dir`) |
| `--export-settings <file>`, `--import-settings <file>`, `--reset-settings` | Exports the settings at the start / imports a file or resets everything while closing (no restart) |
| `--keep-test-mb <n>` | "Keep recordings under" counts megabytes instead of gigabytes |
| `--welcome-test`, `--bar-test` | Shows the first-start note during a self-test / the top bar auto-hides during the self-test too (fade times in the log) |
| `--pin <6 digits>`, `--pin-test` | Sharing PIN for this run (not saved): this PC's when sharing, the one sent with `--connect` / the PIN's wrong-guess limits (PASS / FAIL in the log) |
| `--crash-test` | Crashes halfway through the self-test (the crash report and its log must be written; use with `--profile-dir`) |
| `--undo-test`, `--qr-test <text>` | Advanced undo/redo check / logs a QR code |
| `--enhance <0-2>`, `--enhance-strength <0-1>` | Enhance off / sharpen / upscale + sharpen, and its strength |
| `--power battery\|ac`, `--low-memory` | Acts as if on battery / plugged in, or as if Windows were low on memory (the profiles react as they would) |
| `--low-resource`, `--preview-30` | Low-resource mode / the 30 fps preview limit for this run (not saved) |
| `--test-mode WxH@fps`, `--test-modes a,b,c`, `--test-switch-sec N` | The test pattern at any size / rate (59.94 too), or changing mode every N s with a short signal drop |
| `--all-formats`, `--all-formats-from N` | Steps through every format of the opened device, PASS / FAIL per format in the log |
| `--ai-upscale` | AI upscaling on for this run (not saved); status in the log and F3 |
| `--superres-sim "vendor\|name\|a.b.c.d\|on\|off\|rejected"` | AI upscaling as on another graphics card (vendor nvidia / intel / amd), for testing the wording |
| `--replay-resize-test` | With `--replay`: changes the replay length mid-run (the buffer must survive) |
| `--portal-test` | Checks the welcome page's DNS parser against malformed and random packets |
| `--scale <fit\|fill\|stretch\|integer>`, `--test-image <png>` | Scaling for this run / a still picture as the test source |
| `--rotate <0-3>`, `--mirror`, `--crop l,t,r,b`, `--sync <ms>` | Crop / mirror / turn (quarter turns) and sync on screen for every device this run (crop as parts of the source, e.g. `0.1,0,0.1,0`; sync < 0 = the picture waits) |
| `--test-hdr`, `--hdr-on`, `--hdr-mode <0-3>`, `--hdr-output-test` | HDR test source (10-bit HDR10 gray steps and colors) / HDR features on for this run / HDR source mode for every device / act as if the monitor had Windows HDR on (the last two turn the HDR features on too) |
| `--sound-stop-test` | The device's sound stops arriving at 30 % and comes back at 75 % |
| `--profile-test`, `--twin-test` | Per-device profiles across a device switch (needs `--test-pattern` and another device) / two identical test "cards", renamed and back |
| `--ui-steps "t:kind:x,y;..."` | Feeds input to the interface at t seconds (kinds: `move`, `click`, `key` (virtual-key code, Ctrl = +262144), `char`, `wheel`, `shot` = save the window as `<--ui-shot name>_NN.png`); the real mouse is ignored and never hidden meanwhile |

Command-line options never change saved settings. A self-test writes only a `--profile-dir` test profile; outside a
self-test, `--import-settings` and `--reset-settings` act on the settings in use when ZeroDisplay closes.

## Roadmap

1. **Core capture** (done): devices, hot-plug, format switching, low-latency audio, multiple devices, Simple mode UI.
2. **Recording and LAN streaming** (done):
   - Hardware H.264 encoding (Quick Sync / NVENC through Media Foundation) to crash-safe fragmented MP4 (F9).
   - Share button: other ZeroDisplay instances on the LAN list the stream as "Network: …" and play it.
   - Browser viewer at `http://<this PC>:8080` for phones, tablets and other PCs.
3. **Advanced mode** (done): scenes and layered sources, filters (chroma key, color correction, sharpen), studio mode with transitions, program audio mixer, replay buffer and global hotkeys.
4. **Compatibility + capture-software features** (done): any GPU / no GPU, HEVC, mic commentary, separate tracks,
   split / limits, standby, device picture controls, auto-crop, retro filters, Flashback, clip trimmer, mini player, lag check.
5. **Enhance** (done): on-screen sharpening and edge-aware upscaling (own shaders), Settings ▸ Video ▸ Enhance.
6. **Wireless** (W1–W3 done): cast receiver (Android / Windows), the PC's own Wi-Fi, welcome page and QR codes.
   Next: browser screen share for desktops (W4).
7. **Android version** (in progress, not in this repository yet): Cast to PC done (phone screen + sound to
   ZeroDisplay on a PC, ~64 ms); capture card on the phone with our own USB video driver (tested against the card's
   descriptors; first run with the card pending), recording (crash-safe), replay, screenshots, Enhance, filters, mini
   player, microphone, sharing with PCs.
