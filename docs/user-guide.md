# ZeroDisplay user guide

[← Back to the README](../README.md)

Everything ZeroDisplay can do, and where to find it.

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

## The basics

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

## Keyboard shortcuts

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

## Recording, clips and extras

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

## Enhance and AI upscaling

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

## Power & memory (Settings ▸ General)

| Setting | Default | What it turns off |
|---|---|---|
| **Battery saver** | On battery | Replay buffer; recording / sharing move to the display GPU (the second GPU sleeps); Enhance; studio preview at half rate; background network discovery. The preview keeps its frame rate. |
| **Memory saver** | When Windows runs low | Replay buffer (about 100 MB at the default 30 s and High quality) and Flashback; Enhance; images of Advanced scenes not shown; memory trimmed while minimized. |
| **Low-resource mode** | Off (on once for very weak PCs) | Both of the above, plus preview at 30 fps, screen filters, studio mode, multiview; sharing at 720p and at most 6 Mbps. |
| **Efficiency mode for background work** | On | Nothing: saving files, network discovery and the welcome page just run on efficient cores at a low clock (Windows' EcoQoS). Picture, sound and encoding stay at full speed. |
| **Limit preview to 30 fps** (Settings ▸ Video) | Off | Only what's drawn: recordings and sharing keep the full frame rate. Suggested automatically when the preview can't keep up. |

A pill on the top bar shows which profile is active. A minimized or fully covered window draws nothing at all.
If the app ever crashes, the crash report and that run's log are saved in their own folder next to the settings, `Crashes\<date>\` (`crash.dmp` + `log.txt`: send both along); the next start says where.

## Advanced mode

Switch to **Advanced** in the top bar to build scenes the way you would in OBS:

- **Scenes and sources** are in the panel on the right. A source can be a capture device, a network stream, an image, text or a solid color.
- **Arrange sources on the canvas:** click to select, drag to move, and drag a corner to resize. Hold <kbd>Shift</kbd> to resize freely. Sources snap to the edges and the centre.
- **Filters per source:** chroma key, color correction and sharpen.
- **Audio per device:** volume, mute, noise gate and compressor.
- **Studio mode:** edit the preview while the program stays on air, then send it across with a cut, fade, slide or wipe.
- **Undo / redo:** <kbd>Ctrl</kbd>+<kbd>Z</kbd> / <kbd>Ctrl</kbd>+<kbd>Y</kbd> (or the buttons at the top of the scene panel).
- **Text and colors:** any installed font (regular / semibold / bold, italic) and any color (picker or #RRGGBB).
- **Output:** recording, sharing and the replay buffer all use the composed program, with all program audio mixed together.

## Wireless (cast to this PC)

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

## More settings

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

## Settings and portable mode

Settings are saved to `%APPDATA%\ZeroDisplay\settings.ini`.

**Portable mode** keeps everything next to `ZeroDisplay.exe`, so the folder can be copied to another PC as a whole:
settings (`settings.ini`, `scenes.ini`, `log.txt`), recordings and clips in `Videos`, screenshots in `Screenshots`
(a recording folder you chose yourself still wins). Turn it on with Settings ▸ General ▸ *Portable*, or put an
empty `settings.ini` next to the exe. Turning it off moves the settings back to `%APPDATA%\ZeroDisplay`. Portable mode
needs a folder you can write to (not `Program Files`).
