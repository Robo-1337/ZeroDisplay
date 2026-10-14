<p align="center">
  <img src="docs/images/logo.png" width="96" alt="ZeroDisplay logo">
</p>

<h1 align="center">ZeroDisplay</h1>

<p align="center">
  <b>A lightweight, low-latency viewer and recorder for USB / HDMI capture cards on Windows.</b><br>
  One small <code>.exe</code>, no installer, no third-party code, free and open source.
</p>

<p align="center">
  <img src="docs/images/screenshot-dark.png" width="49%" alt="ZeroDisplay in dark mode">
  <img src="docs/images/screenshot-light.png" width="49%" alt="ZeroDisplay in light mode">
</p>

## Why ZeroDisplay?

Plug in a capture card, open ZeroDisplay, and your console (or camera, or second PC) is on screen right away, with
as little delay as the hardware allows. No accounts, no ads, no setup wizard.

- **Low latency, for real**: about 1.5 ms from a frame arriving to it being on screen, and sound that stays in
  sync without drifting. Good enough to play on.
- **Record, clip and look back**: H.264 / HEVC recording on your GPU's encoder, a replay buffer, *Flashback* (rewind
  the live picture while it keeps running) and a lossless trimmer.
- **Stream it around the house**: share to other PCs or any browser on your network, or cast your phone or laptop
  to this PC (Miracast / Smart View / Win+K).
- **Advanced mode** when you need it: scenes, layered sources, filters, chroma key and a studio mode with transitions.
- **Looks like Windows**: follows your light / dark setting and fits in on Windows 10 and 11.
- **Tiny and self-contained**: built only on components that ship with Windows, so there is nothing else to install.

Called **CaptureCard** up to version 1.0. Settings move over by themselves the first time you start ZeroDisplay.

## Download

Grab `ZeroDisplay-<version>-win-x64.zip` from the [latest release](../../releases/latest), unzip it anywhere and run
`ZeroDisplay.exe`. That's it: no installer, and nothing is added to your system.

Want it portable (settings next to the exe, e.g. on a USB stick)? Turn on *Portable* in Settings ▸ General.

### Heads up: Windows SmartScreen will warn you

The first time you run it, Windows will most likely show a blue box saying **"Windows protected your PC"**. That's
expected, and here's why:

- **The exe isn't code-signed.** Signing certificates cost a few hundred dollars a year and need a registered
  business or identity check. ZeroDisplay is a free hobby project, so it ships unsigned for now.
- **It's new.** SmartScreen trusts apps by *reputation*: how many people have downloaded and run that exact file.
  A brand-new release starts with none, so Windows plays it safe and asks you first. The warning tends to go away
  on its own as more people use a release.
- **It's not a virus detection.** SmartScreen is saying "I don't know this app", not "this app is harmful".

To run it anyway: click **More info**, then **Run anyway**. You only have to do this once.

If you'd rather be sure, you can:

- **Check the file hash.** Each release lists the SHA-256 of the zip. In PowerShell:
  `Get-FileHash .\ZeroDisplay-1.1.0-win-x64.zip` and compare it with the one on the release page.
- **Scan it** with Microsoft Defender or upload it to [VirusTotal](https://www.virustotal.com).
- **Build it yourself** from this source code (see [Building](#building)). It takes a few minutes.

## System requirements

ZeroDisplay is light: on the oldest PC it's tested on (a 2017 quad-core laptop), watching 1080p60 uses about 4 % of
the CPU and 100 MB of RAM. Most PCs that can run Windows 10 or 11 well will run it.

### Supported Windows versions

| Windows | Status |
|---|---|
| **Windows 11** (64-bit) | Supported. This is where ZeroDisplay is developed and tested. |
| **Windows 10, version 22H2** (64-bit) | Supported. It should work just like on 11, but it gets less testing. (Microsoft ended regular Windows 10 support in October 2025.) |
| Windows 10 before version 2004 | Not supported. The app may start, but casting to the PC needs version 2004 or later. |
| Windows 7, 8, 8.1, or any 32-bit Windows | Not supported (the app won't start). |
| Windows on ARM | Not tested. Windows 11's x64 emulation may run it. |
| Virtual machines and Remote Desktop | Not recommended: without a real GPU it falls back to software rendering, which is too slow for smooth 60 fps. |

Windows "N" editions also need Microsoft's free *Media Feature Pack*; the app tells you if it's missing.

### Minimum and recommended

| | Minimum | Recommended |
|---|---|---|
| **OS** | Windows 10 22H2, 64-bit | Windows 11, 64-bit |
| **CPU** | 4-core, e.g. Intel Core i5-7300HQ (2017) | 6 cores or more, any recent Intel Core / AMD Ryzen |
| **RAM** | 4 GB | 8 GB or more |
| **Graphics** | DirectX 11 GPU with a current driver, e.g. Intel HD Graphics 630 | A GPU with a hardware video encoder: Intel Quick Sync, NVIDIA GTX 10 series or newer (NVENC), or AMD Radeon (VCN) |
| **USB** | USB 2.0 (lower resolutions or MJPEG only) | USB 3.0 port for 1080p60 and up |
| **Storage** | 10 MB for the app | An SSD with room for recordings: about 11 GB per hour at 1080p60 on the default High quality |
| **Capture device** | Any UVC video device: USB / HDMI capture card, webcam, camera with a clean HDMI output | A USB 3.0 capture card that offers uncompressed YUY2 (lowest latency) |

### Extra hardware for optional features

| Feature | Needs |
|---|---|
| HEVC recording | A GPU with an HEVC encoder (Intel 6th gen or newer, NVIDIA GTX 10 or newer, most AMD Radeon). Without one, ZeroDisplay records H.264 and tells you. |
| AI upscaling | NVIDIA RTX 20 / 30 / 40 / 50, or Intel Xe / Arc graphics, with a recent driver (details in [Enhance and AI upscaling](#enhance-and-ai-upscaling)). |
| Casting to this PC | A Wi-Fi adapter that supports Miracast. Check in PowerShell: `netsh wlan show drivers`, look for *Wireless Display Supported: Yes*. |
| This PC's own Wi-Fi network | A Wi-Fi adapter that supports Wi-Fi Direct (most from the last few years). |
| HDR (experimental) | An HDR monitor with *Use HDR* on in Windows' display settings. |

### Tested on

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

## Quick start

1. **Connect your source**: plug the capture card into a USB 3 port and your console, camera or PC into the card's
   HDMI input.
2. **Run `ZeroDisplay.exe`**: it finds the card and starts showing its picture and sound. There's nothing to set up.
3. **Move the mouse** to bring up the top bar: pick the device, the format (resolution / fps) and the sound source,
   set the volume, take a screenshot (<kbd>F12</kbd>) or start recording (<kbd>F9</kbd>). The gear opens Settings.
4. **Go full screen** with a double-click or <kbd>F11</kbd>; <kbd>Esc</kbd> brings the window back.
5. **Catch the moment**: turn on the *Replay buffer* in Settings ▸ Recording and press <kbd>F8</kbd> to save the last
   seconds, or press <kbd>←</kbd> to rewind the live picture with *Flashback*.

Press <kbd>F1</kbd> any time for every key.

## Features at a glance

| Feature | What it does | Where |
|---|---|---|
| **Low-latency preview** | Shows each frame the moment it arrives (about 1.5 ms), with sound kept in sync and free of drift | Just open the app; <kbd>F3</kbd> shows the live numbers |
| **Any format** | Every resolution, refresh rate and pixel format the device offers; uncompressed YUY2 is preferred for the lowest delay | Format menu in the top bar |
| **Hot-plug** | Switches to a device when you plug it in and waits for it when you unplug it | Automatic |
| **Several devices** | All devices side by side, or each one in its own window | <kbd>Tab</kbd> / grid button; device menu ▸ *Open in a new window* |
| **Picture shape and scaling** | Auto, 4:3, 16:9 or source pixels; fit, fill, stretch or pixel-perfect integer scaling | Settings ▸ Video |
| **Crop, turn and mirror** | Rotate 90 / 180 / 270°, mirror, crop any edge; recordings and streams get the same picture | Settings ▸ Video ▸ Crop and turn |
| **Enhance and AI upscaling** | Sharpens and upscales soft sources on screen (recordings keep the original) | Settings ▸ Video |
| **Retro screen filters** | Sharp pixels, scanlines, CRT and LCD grid for older consoles | Settings ▸ Video ▸ Screen filter |
| **Auto-crop** | Removes black bars around the picture | Settings ▸ Video |
| **HDR** (experimental) | Tone-maps HDR sources for any screen; real HDR on an HDR monitor | Settings ▸ HDR |
| **Sync on screen** | Delays the sound or the picture to line them up, e.g. with Bluetooth headphones | Settings ▸ Audio |
| **Recording** | H.264 or HEVC on your GPU's encoder; split files, time limit, stops before the disk fills up | <kbd>F9</kbd>; Settings ▸ Recording |
| **Replay buffer** | Keeps the last 10 to 300 seconds in memory, ready to save | Settings ▸ Recording; <kbd>F8</kbd> saves |
| **Flashback** | Rewind the live picture while everything keeps running, mark a part and save it as a clip | <kbd>←</kbd> |
| **Trimmer** | Cut the start and end of a recording, instantly and without re-encoding | Settings ▸ Recording ▸ *Trim a recording* |
| **Screenshots** | PNG or JPG, optionally copied to the clipboard too | <kbd>F12</kbd> |
| **Microphone** | Mixes your mic into recordings with its own volume, noise gate and compressor; optional separate tracks | Settings ▸ Audio |
| **Share on your network** | Other PCs running ZeroDisplay and any browser can watch, protected by a PIN | Share button; Settings ▸ Network |
| **Cast to this PC** | Receive Android (Smart View / Screen mirroring) and Windows (<kbd>Win</kbd>+<kbd>K</kbd>) casts | Wireless tab |
| **This PC's own Wi-Fi** | Makes a Wi-Fi network for phones where there isn't one | Wireless tab |
| **Advanced mode** | Scenes with devices, images, text and colors; chroma key and other filters; studio mode with transitions | *Advanced* in the top bar |
| **Mini player** | A small always-on-top window | <kbd>F6</kbd> |
| **Clean output** | Hides every control, for screen sharing the window | <kbd>F10</kbd> |
| **Global hotkeys** | Record, save a replay, screenshot, mute and more from any app | <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+key; Settings ▸ Hotkeys |
| **Lag check** | Measures how long your capture chain takes | Settings ▸ General ▸ *Measure capture latency* |
| **Power and memory savers** | Lighter on battery, when memory runs low, or on very weak PCs | Settings ▸ General |
| **Light and dark** | Follows Windows, or pick one | Sun / moon button next to the Settings title |
| **Portable mode** | Keeps settings and recordings next to the exe, e.g. on a USB stick | Settings ▸ General ▸ *Portable* |

## FAQ

**The picture is black.**
Make sure the source is on and awake, and try another format from the format menu. Capture cards can't show
copy-protected (HDCP) video: on a PlayStation, turn off *Enable HDCP* in the console's settings; streaming apps on
the source may stay black for the same reason.

**It says the device is in use.**
Windows lets one app use a capture card at a time. Close the other one (OBS, Discord, Teams, Zoom, the Camera app or
a browser tab with the camera on) and ZeroDisplay picks the card up by itself.

**There's no sound.**
Check the sound source menu in the top bar (*Auto* pairs the card with its own audio device) and the volume. If a
*NO SOUND* pill shows, the card's sound driver stopped: unplug the card and plug it back in.

**1080p60 is missing, or the picture stutters.**
The card is probably in a USB 2 port (a *USB 2 SPEED* pill shows). Use a USB 3 port, usually the blue one, plugged
directly into the PC rather than a hub.

**Can I play games through it?**
Yes, that's what it's built for: a frame is on screen about 1.5 ms after it arrives. The capture card itself adds
some delay too; <kbd>F3</kbd> shows the numbers and Settings ▸ General ▸ *Measure capture latency* measures the whole chain. For
competitive games, play on the card's passthrough output if it has one.

**Can I use it together with OBS?**
Not on the same card at the same time (see above). You can press <kbd>F10</kbd> (clean output) and capture the
ZeroDisplay window in OBS, or use ZeroDisplay's own recording, replay buffer and Advanced scenes.

**Where are my recordings and screenshots?**
In `Videos\ZeroDisplay` and `Pictures\ZeroDisplay`. Settings ▸ Files opens them or picks other folders.

**Can my iPhone cast to it?**
No: iPhones and Pixels cast with AirPlay / Google Cast, which Windows can't receive. A USB-C or Lightning to HDMI
adapter into the capture card works.

**Does it collect data or need the internet?**
No. There are no accounts, no telemetry and no update checks. The network is only used by the sharing and casting
features, and Settings ▸ General ▸ *Wireless features* turns all of that off.

**Something went wrong. How do I report it?**
Open an [issue](../../issues) with what happened, your capture card and the `log.txt` (Settings ▸ General shows where
the settings and log are kept). If the app crashed, the next start tells you where the crash report was saved.

## User guide

Everything in detail. New here? [Quick start](#quick-start) and [Features at a glance](#features-at-a-glance) are the short version.

### The basics

Run `ZeroDisplay.exe`. The app opens the capture card and starts playing its picture and sound.
There is nothing to configure and nothing to install.

- **Top bar** (appears when you move the mouse):
  - Pick the device, the capture format (resolution / fps / YUY2 or MJPEG) and the audio source. In a narrow window
    the menus get room first and icon buttons that have a key step aside (the format menu needs about 1000 px of
    window width, the audio source about 1600 px, at 125 % scaling).
  - Change the volume, take a screenshot, show all devices at once, open the statistics or settings.
- **Hot-plug**: plug a device in and the app switches to it. Unplug it and the app waits for it to come back.
- **Settings** are grouped in sections you can fold away (click a header). A reset icon next to a setting puts its
  default back.
- **Light and dark**: the app follows Windows' light / dark setting, or keeps the one you pick with the sun / moon
  button next to the Settings title (also Settings ▸ General ▸ Appearance).
- **Multiple devices**: the grid button (or <kbd>Tab</kbd>) shows every capture device side by side. Recording
  (<kbd>F9</kbd>) in this view saves one file per device.
  - Click a tile to make it active; its audio is the one you hear.
  - Double-click a tile to show only that device.

### Keyboard shortcuts

These are the default keys: Settings ▸ Hotkeys changes any of them (in the window and globally).

| Key | Action |
|---|---|
| <kbd>F11</kbd> / <kbd>Alt</kbd>+<kbd>Enter</kbd> / double-click | Full screen |
| <kbd>Esc</kbd> | Close settings / leave full screen |
| <kbd>F3</kbd> | Statistics overlay (latency, fps, audio buffer, drift) |
| <kbd>F12</kbd> | Screenshot to `Pictures\ZeroDisplay` (portable mode: `Screenshots` next to the exe; Settings ▸ Files changes it) |
| <kbd>F9</kbd> | Start / stop recording (`Videos\ZeroDisplay`; portable mode: `Videos` next to the exe; Settings ▸ Files changes it) |
| <kbd>F8</kbd> | Save the replay buffer (when it's on) |
| <kbd>←</kbd> | **Flashback**: look back 5 s while everything keeps running live. Then <kbd>←</kbd>/<kbd>→</kbd> ±5 s, <kbd>Space</kbd> pause, <kbd>I</kbd>/<kbd>O</kbd> mark a clip, <kbd>Enter</kbd> save it, <kbd>Esc</kbd> back to live |
| <kbd>F6</kbd> | Pop-out mini player (always on top; drag to move, double-click to close) |
| <kbd>F10</kbd> | Clean output: hide every control, so only the picture shows (for screen sharing or capturing the window). <kbd>F10</kbd> or <kbd>Esc</kbd> brings the controls back |
| <kbd>M</kbd>, <kbd>↑</kbd>/<kbd>↓</kbd> | Mute, volume |
| <kbd>Tab</kbd> | Show all devices |
| <kbd>1</kbd>–<kbd>9</kbd> | Switch device (Advanced mode: switch scene) |
| <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>R</kbd> / <kbd>S</kbd> / <kbd>P</kbd> / <kbd>M</kbd> / <kbd>1</kbd>–<kbd>9</kbd> | Global hotkeys (work from any app): record, save replay, screenshot, mute, scene; <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>H</kbd> toggles clean output, <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>N</kbd> mutes the microphone |

### Recording, clips and extras

- **Recording** (Settings ▸ Recording): H.264 or **HEVC**, quality presets, encoder preset (**Auto** picks the best
  setting this PC can run in real time, or choose Fast / Balanced / Best), **split** into 15/30/60-minute files,
  **stop after** 30 min / 1 h / 2 h, and it stops by itself when the disk gets below 1 GB free (Settings ▸ Files
  sets the limit). A folder that can't be written is refused before recording starts, with the reason.
- **Encoding hardware** (Settings ▸ Recording): **Auto** records on a second graphics card's hardware encoder when the
  PC has one (e.g. a laptop's NVIDIA or AMD GPU next to its Intel or AMD integrated graphics), so the GPU that shows
  the picture keeps all its power for display and sharing; if that card can't keep up, Auto moves the recording to the
  display GPU, moves it back if that GPU can't keep up either, and remembers the move only once it has worked for a minute. **Hybrid** always does that, **Display GPU** keeps everything on the GPU that shows the picture,
  **Software** encodes recording and sharing on the CPU. Hybrid needs a second GPU with a hardware video encoder; on
  PCs with one GPU it is greyed out. Pressing record never freezes the picture: the encoder starts in the background.
- **Microphone commentary** (Settings ▸ Audio): your mic is mixed into recordings, replays and sharing, with
  its own volume, noise gate and compressor. **Separate audio tracks** keeps the mix plus each source on its own track.
- **Replay buffer + Flashback**: keep the last 10–300 s in memory, save it with <kbd>F8</kbd>, or scrub back through
  it with <kbd>←</kbd> and save just the part you want.
- **Trim a recording** (Settings ▸ Recording): pick an MP4, set start and end with a preview, and save a copy.
  Nothing is re-encoded, so it's instant and lossless.
- **Standby**: if the signal drops or the device is unplugged, the last picture stays up (and keeps going into
  recordings and streams) until it comes back.
- **Picture**: the device's own brightness / contrast / saturation / hue controls, **auto-crop** of black bars, and
  retro **screen filters** (sharp pixels, scanlines, CRT, LCD grid) for old consoles.
- **Per device**: picture shape, scaling, screen filter, auto-crop, Enhance, sync on screen and crop / turn are kept
  for each device and come back when you switch to it (a retro console on one card, a modern one on another).
  Settings ▸ Video names the device they belong to.
- **HDR (experimental, Settings ▸ HDR)**: still being tested, so the *HDR features* switch is off by default. Off,
  a source that says it sends HDR (HDR10 / HLG, 10-bit) is still tone-mapped so it looks right on any screen. On adds:
  real HDR on a monitor with Windows HDR on, HDR10 recordings (HEVC 10-bit) when the PC has a hardware HEVC encoder,
  HDR brightness / highlights, and HDR source ▸ HDR10 / HLG for cards that pass HDR without saying so. Sharing stays
  SDR. Tested so far: real HDR output on an HDR400 monitor; the capture cards used for testing don't pass HDR.
- **No sound?** If a device's sound stops arriving, ZeroDisplay restarts it once and then tells you to replug the
  device (a NO SOUND pill shows). A card running at USB 2 speed shows a USB 2 SPEED pill.
- **Crop and turn** (Settings ▸ Video): turn the picture 90 / 180 / 270°, mirror it, and crop any edge. Recordings,
  sharing, screenshots and the replay buffer get the same picture (a turned picture records upright, e.g. 1080×1920).
  Locked while that device records.
- **Sync on screen** (Settings ▸ Audio): one slider. Picture late? The sound waits. Sound late (Bluetooth headphones)?
  The picture waits, up to 300 ms, which adds that much to the picture: fine for watching, not for playing through
  ZeroDisplay. Only the screen and speakers; recordings and streams stay in sync as captured. It keeps a few frames in
  graphics memory while on (about 90 MB for 150 ms of 1080p60). In Advanced mode only the sound can wait.
- **Sharing on your network** (Settings ▸ Network): other PCs running ZeroDisplay and any browser
  (`http://<this PC>:8080`) can watch. *Stream quality* Smart adapts to the slowest viewer's connection, or pick a
  fixed 4 / 8 / 15 / 25 Mbps. Lost Wi-Fi packets are re-sent, so the picture doesn't freeze.
  **Sharing PIN** (on by default): viewers get the picture only with the 6-digit PIN shown in Settings ▸ Network. Other
  PCs and browsers type it once and remember it for this PC; *New PIN* makes everyone type the new one. Wrong guesses
  are limited (5 different wrong PINs a minute per address, 30 a minute in all), and the browser viewer accepts only
  its own page (no other website can pull the stream). Off: anyone on the network can watch while sharing is on.
- **Lag check** (Settings ▸ General): flashes a screen that feeds the card and measures how long the capture chain takes.
- **Any PC**: works with Intel, AMD and NVIDIA graphics, integrated or not, and without a usable GPU (software
  rendering and encoding). Settings ▸ Video ▸ *Graphics card* picks one; *Automatic* uses the GPU that drives the
  monitor the window is on.

### Enhance and AI upscaling

**Enhance** (Settings ▸ Video): makes soft pictures crisper on screen: *Sharpen*, or *Upscale + Sharpen* when the
picture is shown larger than it is (a Switch game on a big window, 720p sources). Our own shaders; recordings and
streams keep the original. It can't add detail that isn't in the signal.

**AI upscaling** (Settings ▸ Video, off by default): the graphics driver's own video super resolution sharpens a
picture shown larger than it is (1080p on a 1440p / 4K screen, 720p sources). Screen only; it costs GPU time
(about 6.5 ms per frame at 1080p → 1440p on an RTX 4060 laptop). ZeroDisplay only offers it where it can work and
says why not before you try:

| Graphics | What you need | Notes |
|---|---|---|
| NVIDIA RTX 20 / 30 / 40 / 50 (desktop and laptop) | Driver 531.18 or newer (RTX 20: 545.84). In the NVIDIA App ▸ Graphics ▸ Global settings ▸ RTX Video Enhancement ▸ Super Resolution on (or NVIDIA Control Panel ▸ Adjust video image settings) | The driver turns it off on battery. GTX and MX cards can't (no Tensor cores). |
| Intel (Xe / Arc graphics) | Driver 27.20.100.8681 or newer | Intel has no on/off switch for it; whether a given Intel chip does it is up to the driver (the HD 630 in the test laptop rejects it). |
| AMD Radeon | Not available | AMD offers its video upscaler only through its own developer kit, not to other apps. Enhance works on AMD. |

The driver's switch can't be read, so ZeroDisplay checks it silently when Settings ▸ Video is open (a still picture
rendered off screen with and without the driver's upscaler, never on screen), again when you come back from another
app and when the power source changes. If the driver doesn't apply it, the steps to switch it on are shown above the
toggle. On a laptop with two GPUs it follows the graphics card that draws the picture (Settings ▸ Video ▸ Graphics
card).

### Power & memory (Settings ▸ General)

| Setting | Default | What it turns off |
|---|---|---|
| **Battery saver** | On battery | Replay buffer; recording / sharing move to the display GPU (the second GPU sleeps); Enhance; studio preview at half rate; background network discovery. The preview keeps its frame rate. |
| **Memory saver** | When Windows runs low | Replay buffer (about 100 MB at the default 30 s and High quality) and Flashback; Enhance; images of Advanced scenes not shown; memory trimmed while minimized. |
| **Low-resource mode** | Off (on once for very weak PCs) | Both of the above, plus preview at 30 fps, screen filters, studio mode, multiview; sharing at 720p and at most 6 Mbps. |
| **Efficiency mode for background work** | On | Nothing: saving files, network discovery and the welcome page just run on efficient cores at a low clock (Windows' EcoQoS). Picture, sound and encoding stay at full speed. |
| **Limit preview to 30 fps** (Settings ▸ Video) | Off | Only what's drawn: recordings and sharing keep the full frame rate. Suggested automatically when the preview can't keep up. |

A pill on the top bar shows which profile is active. A minimized or fully covered window draws nothing at all.
If the app ever crashes, the crash report and that run's log are saved in their own folder next to the settings, `Crashes\<date>\` (`crash.dmp` + `log.txt`: send both along); the next start says where.

### Advanced mode

Switch to **Advanced** in the top bar to build scenes the way you would in OBS:

- **Scenes and sources** are in the panel on the right. A source can be a capture device, a network stream, an image, text or a solid color.
- **Arrange sources on the canvas:** click to select, drag to move, and drag a corner to resize. Hold <kbd>Shift</kbd> to resize freely. Sources snap to the edges and the centre.
- **Filters per source:** chroma key, color correction and sharpen.
- **Audio per device:** volume, mute, noise gate and compressor.
- **Studio mode:** edit the preview while the program stays on air, then send it across with a cut, fade, slide or wipe.
- **Undo / redo:** <kbd>Ctrl</kbd>+<kbd>Z</kbd> / <kbd>Ctrl</kbd>+<kbd>Y</kbd> (or the buttons at the top of the scene panel).
- **Text and colors:** any installed font (regular / semibold / bold, italic) and any color (picker or #RRGGBB).
- **Output:** recording, sharing and the replay buffer all use the composed program, with all program audio mixed together.

### Wireless (cast to this PC)

The **Wireless** tab (top bar: Simple | Advanced | Wireless) turns the PC into a cast receiver:

- **Android** (Samsung Smart View, Cast / Screen mirroring) and **Windows** (Win+K) cast to "ZeroDisplay (<PC name>)".
  A cast is shown in the Wireless tab (recorded with F9, replayed, shared there) and can be added to Advanced scenes.
  Simple mode keeps showing your capture card: a toast says when something casts. Its sound goes through
  ZeroDisplay's mixer and into recordings. A PIN can be required.
- **iPhone, iPad and Google Pixel** only cast with AirPlay / Google Cast, which Windows can't receive: use a USB-C or
  Lightning → HDMI adapter into the capture card.
- **Bluetooth is not involved**: casting uses Wi-Fi Direct.
- **Windows Firewall:** casts (and phones on the PC's own network) arrive over Wi-Fi Direct, which Windows treats as
  a public network. The tab says when ZeroDisplay isn't allowed there and offers *Allow through Windows Firewall*
  (one Windows administrator prompt).
- **Watch on phones and tablets:** turn sharing on and scan the QR code to open the browser viewer (the code carries
  the sharing PIN; typing the address instead asks for it).
- **This PC's own Wi-Fi** (for places without Wi-Fi): a WPA2 network "ZeroDisplay-XXXX" that exists only while it is
  on. Scan *1 Join* to join it, then *2 Open* for the welcome page (watch live, how to cast). Phones on it have no
  internet. On Wi-Fi adapters that can't do both (like the Intel AX210), casting pauses while this network is on.
- Latency: casting ~0.4 s (phone → PC, measured with a Galaxy S22), the browser viewer ~0.2 s. For games, use HDMI.
- While recording, a new cast or device doesn't take over the screen (that would end the recording): use *Show*.
- **Wireless features** (Settings ▸ General, or the top of the Wireless tab) is the master switch. Off: no cast
  receiver, own Wi-Fi, sharing, network discovery or network sources, and nothing listens on the network. Running
  casts and sharing end right away with a note; the choices below it come back when it's turned on again.

### More settings

- **Reset**: a reset icon next to every changed setting; Settings ▸ General ▸ *Export / Import / Reset all* for the
  whole set (settings + scenes in one file).
- **Ask before closing** while recording or sharing, and an optional sound when recording starts / stops.
- **Hotkeys** (Settings ▸ Hotkeys): every shortcut can be changed, in the window and globally (any app).
- **Files** (Settings ▸ Files): folders for recordings and screenshots, a file name template (`{type} {device}
  {date} {time}`), PNG or JPG screenshots (optionally also to the clipboard), the free-space limit and an optional
  "keep recordings under N GB" (oldest ZeroDisplay recordings to the Recycle Bin; nothing else is touched).
- **A window per device:** device menu ▸ "Open … in a new window" shows a second (third, …) capture device in its own
  resizable window; double-click or F11 for full screen on that screen. The windows come back where they were at the
  next start.
- **Hide devices:** Settings ▸ Devices (or "Hide … from this list" in the device menu) leaves a camera out of every
  list and never opens it, e.g. a laptop's built-in webcam.
- **Rename devices:** type a name in Settings ▸ Devices; it shows in the menu, tiles, windows and file names. Two
  identical cards are told apart by the USB port they're in ("USB3.0 Capture" and "USB3.0 Capture (2)"), each with
  its own settings; moving such a card to another port makes it a new device for ZeroDisplay (a card with a serial
  number keeps its settings in any port).

### Settings and portable mode

Settings are saved to `%APPDATA%\ZeroDisplay\settings.ini`.

**Portable mode** keeps everything next to `ZeroDisplay.exe`, so the folder can be copied to another PC as a whole:
settings (`settings.ini`, `scenes.ini`, `log.txt`), recordings and clips in `Videos`, screenshots in `Screenshots`
(a recording folder you chose yourself still wins). Turn it on with Settings ▸ General ▸ *Portable*, or put an
empty `settings.ini` next to the exe. Turning it off moves the settings back to `%APPDATA%\ZeroDisplay`. Portable mode
needs a folder you can write to (not `Program Files`).

## How it works

### Built with

Only what ships with Windows:

| | |
|---|---|
| Video | Media Foundation (capture, MJPEG decode), Direct3D 11 (color conversion, scaling, present) |
| Audio | WASAPI (capture + low-latency output), our own drift-compensating resampler |
| UI | Direct2D + DirectWrite, Segoe Fluent Icons |
| Other | WIC (PNG / JPG screenshots), cfgmgr32 (device pairing / hot-plug), MMCSS (thread priorities) |

### Latency design

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

## For developers

### Building

Requirements: Visual Studio 2022 Build Tools (C++ workload), Windows SDK 10.0.22621 or newer,
CMake ≥ 3.24, and Ninja.

```powershell
.\build.ps1            # Release -> out\ZeroDisplay.exe
.\build.ps1 -Clean     # full rebuild
```

The C runtime is linked statically and the shaders are compiled at build time and embedded, so the
exe depends only on Windows system DLLs, and it loads them from System32 only (a DLL with the same name placed
next to the exe, e.g. in a Downloads folder, is never used).

### Self-test

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

### Roadmap

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

## Contributing

Bug reports and ideas are welcome: open an [issue](../../issues) and include what you see, your capture card, and
the log (Settings ▸ General shows where the settings and `log.txt` are kept). Pull requests are welcome too; the
project sticks to Windows' built-in APIs only, so please don't add third-party libraries.

## License

ZeroDisplay is free and open source under the [MIT License](LICENSE). Use it, change it, share it.
