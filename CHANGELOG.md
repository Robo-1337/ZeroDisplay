# Changelog

All notable changes to ZeroDisplay. Newest first. Dates are when the work was done.

## [Unreleased]

Nothing yet.

## [1.1.0] - 2026-10-13

The first public release, open source under the MIT License.

### New look (desktop)
- **New logo**: the *Raster Z*, a display's scan path (across, the diagonal
  retrace back, across again) drawn as a Z. New app icon, logo and wordmark in the top bar, the welcome note and
  Settings > General.
- **New style, a blend of Windows 10 and 11**: solid accent blue for what is switched on, a lighter blue for
  markers, a hover light that follows the mouse, a big Settings title, sentence-case section headings with a
  marker, each open section on a card, Windows 10 switches and sliders, underlined choices, Windows 11 menus,
  Mica title bars with a blue window border, and a red line under the top bar while recording.
- **Light and dark**: the app follows Windows' light / dark setting as it changes, or keeps the one you pick:
  the sun / moon button next to the Settings title, or Settings > General > Appearance.

## [1.0.0] - 2026-10-13

The first full release of the desktop app. A summary of everything since 0.4.0; the rounds below have the details
(newest first) and SWEEP_FINDINGS.md lists the 64 checks of the release sweep with how each was verified.

- **Wireless**: receive casts from Android (Smart View, Cast / Screen mirroring) and Windows (Win+K), this PC's own
  Wi-Fi network for places without one (welcome page, QR codes), and a browser viewer for phones and tablets. One
  switch, *Wireless features*, turns all of it off: then the app uses no network at all.
- **Sharing** with other PCs and browsers, protected by a 6-digit **sharing PIN** (on by default, remembered per PC),
  with smart bitrate and repair of lost Wi-Fi packets.
- **Picture**: every resolution, refresh rate and pixel shape; Enhance (sharpen, upscale); AI upscaling where the
  graphics driver offers it; crop, mirror and turn; per-device profiles, a window per device, renamed and hidden
  devices; HDR (experimental, off by default).
- **Sound**, rewritten: any sample rate, format and channel layout, the real clocks of card and output measured and
  followed, Ultra / Low / Safe latency, exclusive output with an automatic fallback, microphone with gate and
  compressor, separate tracks.
- **Recording**: encoding hardware Auto (a second GPU's encoder, moved and moved back only when it helps), encoder
  presets, split, time limit, free-space limit, keep under N GB, file name template, replay buffer and Flashback,
  lossless trimmer.
- **Power and memory**: Battery saver, Memory saver, Low-resource mode, efficiency mode for background work.
- **Top bar and interface**: menus and buttons fit any window width (480 px and up), every hint and toast says what
  is really happening, Help (F1) shows the keys you set.
- **Robustness and security**: crash reports with their log in their own folder, DLLs loaded from System32 only,
  hostile settings files and network traffic tested, code analysis and AddressSanitizer clean, 30-minute runs with
  flat memory, a clean exit in every case tested.
- The Android version (Cast to PC, capture on the phone) was started in this period; it is a separate app, still in
  progress (see ANDROID_HANDOFF.md).


### Added (1.0 release sweep: security, asked for by the user)
- **Sharing PIN**, on by default. Anyone on the same network could watch while sharing was on (on public Wi-Fi too),
  and any website open in a browser on that network could pull the browser viewer's stream in the background. Now
  the LAN stream and the browser viewer go only to viewers who give the 6-digit PIN shown in Settings ▸ Network
  (random, encrypted for this Windows user, never written to the log, not exported; *New PIN* makes everyone type
  the new one, and viewers with the old one are disconnected). A ZeroDisplay receiver shows "The sharing PC asks
  for a PIN" with a box to type it and remembers it per PC (encrypted); the browser page asks once and remembers it
  for that address. The Wireless tab's QR code carries the PIN (the page takes it out of the address bar). Wrong
  guesses: 5 different wrong PINs a minute lock an address for a minute, 30 a minute from anywhere make everyone
  wait (the same wrong PIN sent again counts once). The refusal is no bigger than the request, so a spoofed request
  can no longer make this PC stream to someone else. The browser viewer also refuses WebSockets from other
  websites (Origin check). Tested: `--pin-test` (limits), PC to PC on 127.0.0.1 (right 60 fps / wrong / none), a
  WebSocket client (right PIN MSE + WebCodecs, wrong, none, foreign Origin 403, the lock after 5 guesses), and the
  Galaxy S22 over Wi-Fi in Samsung Internet (form, wrong PIN, right PIN 60 fps, reload remembered, new PIN asked
  again, QR link plays and the PIN leaves the address bar). New test flags `--pin`, `--pin-test`.
- **DLLs from System32 only**: the exe now tells Windows to load its DLLs from System32 only
  (`/DEPENDENTLOADFLAG:0x800`) and loads mf.dll from System32 itself like the other Media Foundation DLLs. Before, a
  DLL with the same name as one of six it uses (dwmapi, avrt, dcomp, winmm, iphlpapi, mf), placed next to the exe
  (e.g. in a Downloads folder), was loaded instead. Checked with genuine copies placed next to the old and the new
  exe: the old one loaded all six from its folder, the new one from System32.
- Checked: with Wireless features off (in Settings, also with sharing saved on, and switched off while sharing) the
  app holds no network socket at all; on but not sharing, only discovery's one UDP socket.

### Fixed / changed (1.0 release sweep, 2026-10-12 night; list with how each was checked: SWEEP_FINDINGS.md)
- **Top bar in narrow windows**: the window can be 480 px wide, but the top bar needed about 860 px (at 125 %): at
  800 px the right-hand buttons were drawn over the Simple / Advanced / Wireless switch, and while recording at 1280 px
  the device menu showed only "T...". Now the mode switch gets narrower below 1000 px and, when space runs short, the
  buttons that also have a key or a place in Settings give way, least needed first (statistics F3, help F1, multiview
  Tab, mini player F6, clean output F10, sharing, Flashback, save replay F8, then a shorter volume slider, the slider,
  mute M). Settings, Record and Pause always stay, and Simple mode keeps room for the device menu. Nothing changes when
  everything fits. Checked at 480 / 640 / 800 / 1000 / 1280 / 1600 px.
- **Format and sound source menus in usual window sizes**: they exist only in the top bar and were left out below about
  1900 px (at 125 %), so in a usual window the capture format couldn't be changed at all. They now get room before the
  icon buttons that have a key (statistics, help, multiview, mini player, clean output), then sharing / Flashback /
  replay, then the volume slider and mute; the format menu is wide enough for "1920×1080 60 fps" and spare room is
  shared evenly. Format from 1000 px, sound source from 1600 px.
- Settings ▸ Power & memory: the Memory saver hint shows the replay buffer's size only while it runs.
- Settings ▸ Audio: when the output refuses exclusive use (for example another app has it), the Exclusive note now says
  so ("using shared mode") instead of promising the lowest latency.
- **Crash reports in their own folder**: a crash now saves `Crashes\<date>\crash.dmp` and the run's log up to the
  crash (`log.txt`) next to the settings, so the log survives later starts (before, the next start moved it to
  log.txt.old and the one after that overwrote it). If the crash handler couldn't copy the log, the next start puts the
  crashed run's log there. The start-up notice names the folder; "Show in folder" selects the dump. Older
  `crash-<date>.dmp` files are still found. New test flag `--crash-test`.
- **Recordings that continue in a new part** say why: when Encoder Auto restarts the encoder to keep up (a faster
  preset, or the display GPU), the file continues in `_part2` because the stream headers changed; a toast now says
  "The encoder changed its settings to keep up, so the recording continues in part 2".
- **Switching Simple / Advanced while recording** ends the recording (they record different pictures); the toast now
  says so ("Recording ended: Simple and Advanced record different pictures. Saved ...").
- **Recording start, sound and picture**: when the first sound after the first keyframe started a little after the
  picture, its first sample was stamped late, but an MP4 sound track still starts at 0, so the sound could play early
  by up to one packet. The lead-in is now written as silence. Six test recordings (22.05 and 48 kHz): 0 to -3 ms.
- **Self-test safety**: `--selftest` with `--portable-toggle-test` but without `--profile-dir` turned off the guard that
  keeps self-tests from saving into the user's settings (the toggle itself needs a test profile and didn't run). Fixed.
- **Cast sound**: the channel count is read once per sound frame (another thread can change it), so a format change
  can't move the reads past the buffer (the /analyze C6385 note at CastAudioTap.cpp; the arithmetic was otherwise safe).
- **Log**: NVIDIA's encoder refuses the B-frame count but makes none in low-latency mode (checked with ffprobe), so that
  line is now INFO instead of a WARN on every recording.
- README: the self-test table lists every flag (19 were missing) and says what `--import-settings` / `--reset-settings`
  do outside a self-test.

- **Advanced mode in a narrow window** (800 px): the top bar showed only the red on-air icon, and the panel cut
  "Studio mode (preview + program)", the canvas note, the scene buttons ("+ A... Co... D... Up D...", Delete and Down
  both "D..."), the source Delete button, the font weight and the italic switch. Now "On air" shows only with its
  text, the studio label shortens to "Studio mode", the note wraps, the scene buttons take two rows when their labels
  need it (Add / Copy / Delete, Up / Down), Delete drops its icon and Italic gets its own row. Unchanged when wide.
- Texts: "Color" everywhere in the interface (two labels said "Colour").
- Log: a recording refused by the free-space limit now says why in the log too (it only showed a toast); the encoder
  line names the speed setting it got ("quality vs speed N" or the encoder's own).
- Code analysis (/analyze) now reports nothing: start-up and COM results are checked (and only undone when they
  succeeded), the encoder's name is checked for null, and the cast sound tap reads through a span of exactly the
  samples in its buffer.
- **Encoder Auto no longer gets stuck on a weaker GPU**: when the second GPU fell behind (even for a few seconds of
  load), Auto moved the recording to the display GPU and saved that for good. In a 30-minute test with recording and
  sharing, the display GPU then couldn't keep up either (49 fps for 26 minutes). Now the move is checked: if the
  display GPU falls behind too, the recording goes back to the second GPU once (a toast says so), and the move is
  saved only after a minute of the display GPU keeping up. New test flag `--display-slow-test`.
- **Muting the microphone** now also silences its own track with *Separate audio tracks* (Settings promised "left
  out of recordings"; only the mix did that).
- **A recording file deleted while recording** (e.g. in Explorer) is reported ("Recording failed: the file was
  deleted while recording", or which parts) instead of "Saved".
- **Closing hung with this PC's own Wi-Fi on**: beside the own network the cast receiver can't start (one Wi-Fi card
  can't do both), but the half-started receiver was kept; once a phone had joined and left the network, releasing
  it while closing never returned, so ZeroDisplay stayed "not responding" (keeping the card busy and the network up)
  and the exit watchdog never started. Now a receiver that can't start is let go at once, closing stops the own
  network first, and each wireless step gets 5 s before the exit goes on without it. Tested with the S22 joining and
  leaving: closes in under 3 s.
- **Own Wi-Fi: devices** counted from who uses the network's DNS / welcome page ("N device(s) seen in the last 2
  minutes"); before, a joined phone showed as "0 device(s) joined".
- **Version information**: the exe's version details (Explorer ▸ Properties ▸ Details) still said 0.1.0 from the first
  release; they now come from the single version number in CMakeLists.txt, like the app's title, Settings and log.
- Checked in this sweep without changes: Windows shutdown while recording (finalized in 1.4 s), ask before closing,
  single instance and exit time (0.3 s), recording quality / pause / screenshots / name template / trimmer, sound on
  every output incl. 10-minute runs, HDR and power texts, hostile settings files, device names and network traffic.

### Fixed / changed (round of 2026-10-12, night: sound at any rate, clock drift, HDR made experimental)
- **Crash on the RTX PC** (minidump `crash-2026-10-12_15-04-02.dmp`, after 36 minutes): an access violation in the cast
  receiver's "disconnected" event handler, on one of Windows' Miracast threads. The handlers captured the receiver
  object and were never removed, so an event that Windows delivered while or after the receiver stopped (Wireless off,
  the own Wi-Fi network starting, a PIN change, closing) reached freed memory. Now the handlers of the receiver and of
  the cast player (frame / opened / failed events) hold their state weakly, are removed when stopping, ignore events
  once stopping, and catch WinRT errors instead of ending the app. Tested: receiver off / on, a cast played and closed,
  both also under AddressSanitizer (clean).
- **Sound underruns with a 96 kHz output** (reported on the RTX PC: "underruns 151, drift -5000 ppm" with BEACN
  Studio at 96 kHz and the card at 48 kHz). Cause: the drift controller could only follow clocks within about 0.5 %
  and learned bigger differences only from quick runs of underruns, so a device a little further off (or a virtual
  mixer's clock) underran every few seconds forever. A simulation (`--audio-sim`) reproduced it, plus two more faults
  of the old controller: playback restarted with less than one capture packet buffered (with small output periods it
  ran dry again at once), and when capture packets and output wakeups drift into step, timing jitter now and then
  puts a pull before a packet (a dip of one period that the 0.5 s measurement never saw coming). New design:
  - **Both clocks are measured**: the capture device's and the output's real rate, from the earliest arrival in each
    half second over the last minute (median of pairwise slopes, so bursts, stalls and a Bluetooth output's fast start
    don't skew it). The resampler follows the measured ratio (any difference up to 8 %); the buffer controller only
    trims what's left. F3: "clocks: capture +x ppm, output +y ppm".
  - **Dips are learned**: the buffer is held at its usual level on the typical lowest point, plus the depth of recent
    dips (fades by 0.05 ms per second). An underrun teaches its depth at once (a repeat while it is still remembered
    makes it deeper); one that comes back within ten minutes after its memory faded adds one output period for good
    (at most 30 ms). F3 shows "reserve x (y learned)". Playback starts with one whole capture delivery buffered.
    The audio self-test runs off the render thread (its window used to stay white for the minute it takes).
  - **The output's own queue grows** when the device runs dry twice within a minute (late wakeups, a driver that takes
    audio in bursts), by half a period up to two periods. F3: "output ran dry N times (queue +x ms)".
  - Measured: card -> Kiwi Ears (Bluetooth) output, 100 s: 0 underruns, buffer at the 1.5 ms Ultra reserve, drift within
    +-25 ppm (the old controller had underruns on the same pair); card -> laptop speakers, 90 s: 0 underruns, clocks
    within 3 ppm. In other runs the headphones took an extra 10 ms every 10 to 15 s; the dip learning was built for
    that (simulated too: "output dips 10 ms every 12 s": 2 to 3 underruns in the first minute, then none).
- **Every sample rate and sample format**:
  - Playback resampler: band-limited (windowed sinc, Kaiser, 40 taps at 1:1 = 0.42 ms at 48 kHz; wider when going
    down so nothing folds back). Measured error about -95 dB for 8 kHz to 768 kHz in and out (was Catmull-Rom:
    about -50 dB and audible dulling above 10 kHz while steering).
  - **Recordings, replays, Flashback, sharing and the browser viewer now get sound from any input rate**: everything
    leaves the device at 48 kHz (converted once, with the same resampler, timestamps kept exact). Before, a device at
    96 / 192 / 88.2 / 32 kHz (anything but 44.1 / 48) was played but recorded and shared **without sound** ("can't be
    stored as AAC; skipping that track"). Tested 11.025 to 192 kHz: 48 kHz AAC, sound and picture within 3 ms.
  - Sample formats: 8-bit and 64-bit float added (16 / 24 / 32-bit integer, 20 / 24 bits in 32, 32-bit float were
    there). More than two channels (5.1 / 7.1 HDMI sound) are mixed down properly (centre and surrounds at -3 dB, LFE
    left out), by the device's speaker layout; before, only front left / right were kept (dialogue in the centre
    channel was lost). Outputs with more channels get front left / right, mono outputs the average.
  - A device whose shared-mode format can't be read directly is opened with Windows' own conversion to float.
    Exclusive output tries the device's own rate and layout, then 48 / 44.1 / 96 / 88.2 / 192 / 176.4 / 32 kHz.
- **Frames skipped on the RTX PC** ("skipped 1300+"): the render loop asked Windows about the monitor once a second
  (refresh rate, HDR state, SDR brightness: EnumDisplaySettings, DXGI GetDesc1, QueryDisplayConfig), calls that can take
  tens of ms on PCs with several monitors / HDR screens, enough to miss a frame each time. They now run on their own
  "Display watch" thread (once a second and on display changes); the render loop only reads the result.
- **HDR is experimental now**, with everything in one place: **Settings ▸ HDR (experimental)** holds the master
  switch *HDR features* (`[Video] HdrEnabled`, **off by default**), HDR source (Auto / Off / HDR10 / HLG, per
  device), Real HDR on HDR screens, HDR brightness, Highlights up to, and HDR recordings (moved from Video and
  Recording). Off: no HDR screen output, no HDR10 recordings, no forced HDR10 / HLG modes, default brightness; a
  source that says it sends HDR is still tone-mapped to SDR (checked: same picture as with the switch on). The
  per-device values are kept while off. "Real HDR on HDR screens" is shown disabled with the reason on SDR screens.
- Test options: `--audio-sim` (25 clock / jitter / burst / dip scenarios incl. 20-minute runs, 20 resampler tones, 11
  sample formats; PASS / FAIL in the log, about a minute: use `--selftest 90`), `--test-audio-rate <hz>` (the test pattern's sound at any rate), `--hdr-on` (HDR
  features on for the run; `--hdr-output-test` and `--hdr-mode` turn it on too, `--test-hdr` alone doesn't).

### Added (round of 2026-10-12, later: the "worth adding" list and HDR)
- **No sound from a device**: sound packets arrive every ~10 ms even in silence; none for 3 s means the driver stopped
  delivering (seen with this card until it was replugged). The sound capture restarts once by itself; if that doesn't
  help, a warning says to replug the device, a NO SOUND pill shows, F3 says "NO SOUND PACKETS for N s", and "sound is
  back" follows when it returns. Test flag `--sound-stop-test`.
- **Replays always keep their sound track**: gaps where a device delivered no sound are filled with silence (as in
  recordings), so clips line up in editors. Tested with the card's sound stopped for 8 s of a 15 s replay.
- **USB 2 SPEED pill** while the shown card runs at USB 2 (1080p60 isn't offered then), until it's replugged at USB 3.
- **Crop on the picture**: Settings ▸ Video ▸ Crop and turn ▸ *Crop on the picture...* shows the whole frame with a
  frame to drag by its edges (works with turned / mirrored pictures), says the resulting size ("Recorded and shared as
  1668×984"), Done (Enter) / No crop / Cancel (Esc). Locked while recording.
- **Copy a device's settings to another** (Settings ▸ Devices ▸ *Copy the settings of ... to*): shape, scaling,
  filters, Enhance, sync, crop / turn, HDR; the format and sound source stay each device's own.
- **Low-resource mode shares at 720p** (1280×720 or 720×1280, still at most 6 Mbps): about half the encoding work.
  The encoder's colour conversion scales on the way (GPU shader, video processor or CPU path).
- **Bluetooth headphones**: the app knows when the sound output is a Bluetooth device (its parent device is
  enumerated by Bluetooth). F3 adds "the Bluetooth output's own delay on top (Windows doesn't report it; often 100 to
  300 ms)" and Settings ▸ Audio ▸ Sync on screen suggests moving right until speech matches the lips. Switching
  outputs while playing (speakers / headphones / default): ~0.1 s, no dropouts.
- **HDR**, three parts:
  - *HDR sources on any screen*: 10-bit P010 / Y210 capture formats, the device's HDR flags (HDR10 / PQ, HLG,
    BT.2020), BT.2020 colours, the PQ / HLG curves and a tone mapping to SDR, all inside the existing colour conversion
    (one pass). Settings ▸ Video ▸ *HDR source* (per device): Auto / Off / HDR10 / HLG (for cards that pass HDR in
    8-bit formats without a flag: then BT.2020 is assumed too), *HDR brightness* (which level is SDR white, 203 nits by
    default) and *Highlights up to* (the source's peak, 1000 nits). Measured with the new HDR test source (`--test-hdr`:
    P010 HDR10 grey steps 0.1 to 10000 nits, BT.2020 colours, a 0-1000 nit ramp): 8-bit results within 1 of the
    expected values.
  - *Real HDR on HDR screens*: when Windows HDR is on for the monitor the window is on and an HDR picture is shown, the
    window switches to 16-bit scRGB; everything drawn as usual is shown at Windows' SDR brightness, the HDR picture on
    top at its real brightness (nits / 80) with a roll-off at the monitor's peak. Back to SDR output otherwise (lighter).
    *Real HDR on HDR screens* (on) in Settings ▸ Video. Screen filters, Enhance and a waiting picture (sync) use the SDR
    version. Measured with `--hdr-output-test` (forced, monitor peak 1000, SDR white 200): 100 nits = 1.248 (1.25
    expected), 203 = 2.545, 400 = 5.016, 1000 and up = 12.5. Real HDR monitors: to be checked by the user.
  - *HDR recordings*: an HDR source is recorded as HDR10 (HEVC Main10, 10-bit, BT.2020, PQ, with mastering display and
    content light level metadata in the MP4), from the picture's linear copy, on hardware HEVC encoders (tested on the
    GTX 1050 / hybrid; the file's 10-bit grey steps within 1 of the expected PQ codes). The replay buffer and Flashback
    follow (Flashback decodes 10-bit and tone-maps). Sharing stays SDR H.264. No 10-bit HEVC encoder: tone-mapped SDR
    with a message. Settings ▸ Recording ▸ *HDR recordings* (on).
- Fixed while testing: Flashback over 10-bit video could take minutes to close (and the app hung at exit with it):
  the decoder only took 8-bit output and retried for a long time; now it takes P010 and gives up at once if it can't.
  Shutdown now waits at most 5 s for each background job and names the one that hangs.
- Test helpers: `--ui-steps` gained `down` / `up` (drags); `--hdr-mode <0-3>`, `--test-hdr`, `--hdr-output-test`,
  `--sound-stop-test`.


### Fixed (2026-10-12: full in-app test through the real interface, and a security pass)
Every feature was driven through the interface itself (clicks, keys, typing; scripted with `--ui-steps`, screenshots at
each step with the new `t:shot:N` step; the real mouse is ignored and never hidden during such runs).
- **Text cut off with "..."** in Settings (auto-crop hint, no-signal hint, sound-cue label, "Pause / resume recording"
  in the hotkey table): every hint under a setting now wraps onto more lines when needed, and fixed-height hint blocks
  (Enhance, AI upscaling, encoder, hardware, savers, latency check, stream quality, Recycle Bin question) are as tall as
  their text, so nothing overlaps on a narrow window. The sound cue is now "Sound cues" with a line explaining it. The
  settings file path at the bottom shows in full. Two empty gaps (Network, latency check) are gone.
- **Status pills (REC, LIVE, SHARING) covered the Wireless and Advanced side panels** (the "Wireless features" switch
  was hidden under "LIVE 1 viewer"). They now sit at the right edge of the picture area.
- Sync on screen: the "0" mark in the middle of the slider was drawn under the slider; now on top.
- **A damaged scenes.ini could freeze the start for minutes** (a scene or source count of a million is read one entry
  at a time). At most 200 scenes and 200 sources per scene are read (Add stops at 200 with a toast), the canvas size
  (320x180 to 7680x4320), frame rate (1 to 240), source positions, crops and opacity are kept in range, and numbers
  like "nan" / "inf" in any settings file fall back to the default. Checked with 24 hostile settings + scenes files
  (huge, negative, non-numeric, 5000-character values, a 400-key section): all start and close normally.
- **File names from device names**: a device renamed to a Windows device name (`NUL`, `CON`, `COM1`, ...) gets a "_"
  in front when the name template is just `{device}`, and invisible text-direction characters (e.g. U+202E, which can
  make Explorer show a different extension) are removed. Path characters were already replaced (`..\..\x` stays in
  the folder as `.._.._x`). Checked with 10 hostile names.
- **Browser viewer: someone holding connections open could lock real viewers out** (the server takes 60 connections;
  idle ones are dropped after 10 s). When full, the oldest connection that never finished a request now makes room;
  viewers who are watching are never dropped. Checked: with 300 idle connections held open, the page still loads
  at once. Network fuzz: 160 malformed HTTP requests (path traversal gets 404; no files are ever served), 67 hostile
  WebSocket frames (absurd lengths, fragments, bad opcodes), 8000 random UDP packets to the stream server and
  discovery: no crash, and a normal viewer gets 12 Mbps of video afterwards. Nothing listens on the network unless
  sharing is on (checked with wireless off, and on without sharing).
- `/analyze`: the taskbar overlay call (no icon removes the overlay, as documented) is marked as intended.
- Verification after all of this: 37-test regression set PASS (26 files, 0 decode errors), ASan 28 runs clean, `/analyze`
  nothing new, clean build 0 warnings, only Windows DLLs. Footprint: exe 2.11 MB (1.88 MB before the 16 QoL round).
- Found while testing, not a ZeroDisplay bug: the capture card once stopped delivering sound until it was replugged
  (recordings then get a silent track, the replay buffer none), and it came back at USB 2 speed (1080p50 NV12).

### Added / changed (round of 2026-10-12: the 16 QoL ideas, part 2)
- **Per-device profiles**: picture shape, scaling, screen filter (+ original resolution, strength), auto-crop, Enhance
  (+ strength), sync on screen and crop / mirror / turn are kept for each device (`[Device.<key>]`) and load when you
  switch. Settings ▸ Video says which device they belong to. Multiview tiles, device windows and the mini player draw
  each device with its own. A device without saved values starts from the general ones (`[Video]` / `[Audio]`, which
  are also the Advanced canvas's); the user's existing values carry over that way. Test flag: `--profile-test`.
- **Rename devices** (Settings ▸ Devices: type in the box, empty = Windows' name): the name shows in the device menu,
  multiview tiles, device windows, toasts and the `{device}` part of file names.
- **Identical cards**: two cards with the same Windows name are told apart by their USB port (or serial number):
  "USB3.0 Capture" and "USB3.0 Capture (2)", each with its own settings key (`<name> #<port id>`). The first time
  this happens both take over the settings saved under the plain name, so the first card's profile, format and
  window aren't lost. Hidden devices, the last device, scenes and per-device settings now go by this key (for a
  single card it's still the plain name, so nothing changes). Test flag: `--twin-test`.
- **Crop, mirror and turn** (Settings ▸ Video ▸ Crop and turn, per device): 0 / 90 / 180 / 270°, mirror, and a crop
  per edge (even pixels, at least 64 left). Done inside the colour conversion (an exact pixel remap, no extra GPU
  pass), so the screen, recordings, sharing, screenshots, the replay buffer and Advanced sources all get the same
  picture; 90 / 270° swap the recorded size (tested: 1920×1080 → crop + 90° + mirror = 1026×1536 file). Locked while
  that device is being recorded (one picture size per recording); the replay buffer restarts at the new size,
  sharing follows on its own. The picture-shape setting keeps working on the cropped / turned picture (an SD source
  stays 4:3-shaped pixels). Test flags: `--rotate N`, `--mirror`, `--crop l,t,r,b`.
- **Sync on screen** (Settings ▸ Audio, replaces "Audio delay"): one slider, *Picture earlier* … *Sound earlier*, 0 in
  the middle, per device. Left: the sound waits (as before, up to 300 ms). Right (new, up to 300 ms): **the picture
  waits for late sound**, e.g. Bluetooth headphones: the last frames stay on the GPU and the one from X ms ago is
  shown ("adds X ms to the picture: fine for watching, not for playing through ZeroDisplay", with the GPU memory it
  takes, about 87 MB for 150 ms of 1080p60). Screen and speakers only: recordings and sharing are never shifted.
  Advanced mode: only the sound can wait (says so). F3 shows "picture waits 150 ms, shown picture 151 ms old".
  Nothing is kept while it's off. Test flag: `--sync <ms>`.

### Added / changed (round of 2026-10-11 late: the 16 QoL ideas, part 1)
- **Single instance**: opening ZeroDisplay again brings the running window to the front (starts with options, e.g. a
  second instance watching the network stream, are still allowed).
- **Closing ends everything**: shutdown waits briefly for background jobs and names any that hang; if final cleanup
  ever takes longer than 8 s the process ends anyway (found: a process that stayed after "Exit"). Tested with
  receiver, sharing, replay, recording, mini player and a device window running: gone in 1.9 s.
- **Windows shutting down / signing out** finishes the recording and saves the settings (no question asked).
- **Keep awake** (Settings ▸ General, on): the screen stays on while a live picture shows, the PC stays awake while
  recording / sharing / buffering replays (Windows power requests, released when not needed).
- **Pause / resume recording** (button next to record, F7): the same file continues, the paused part is cut out.
- **Taskbar**: a red dot (yellow when paused) on the taskbar button and "● REC" in the title while recording.
- **Microphone**: a live level meter in Settings ▸ Audio, a mute toggle, a top-bar mic button and a hotkey (N).
- **Toasts**: up to three at once, long text wraps, hovering keeps them; saved files get *Show in folder* / *Open*
  (videos also *Trim*).
- **No signal while recording**: a warning and a NO SIGNAL pill after 5 s; optionally stop after 1 / 5 / 10 minutes.
- **Help when there is no picture**: a checklist (console on, cable, PlayStation HDCP, 1080p); *Device busy* names the
  usual apps and has *Try again*; *Camera access blocked* opens the privacy settings; errors offer the log.
  Fixed: a card held by another app showed "Disconnected"; it now says "Device busy".
- **F1: every key on one page** (your own hotkeys included; also the "?" button) and a short welcome on the first start.
- **Mini player**: remembers where it was, follows the picture's shape (4:3 stays 4:3), adjustable opacity
  (DirectComposition, so it can be see-through over a game).
- **Advanced**: canvas size (720p / 1080p / 1440p / 4K) and 30 / 60 fps in the panel (sources scale along), *Copy*
  scene, Delete / arrow keys / Ctrl+D for the selected source.
- Small fixes: Flashback clip marks are labelled buttons; ← and the Flashback button both go 10 s back; drop an MP4 on
  the window to trim it; volume up to 200 % with a soft limiter (playback only).

### Added / changed (round of 2026-10-11 evening: wireless switch, casts, UI)
- **Wireless features master switch** (Settings ▸ General ▸ Wireless features, and the same toggle at the top of the
  Wireless tab; `[General] Wireless`, on by default). Off stops the cast receiver, the PC's own Wi-Fi and its
  welcome page, sharing (stream server + browser viewer) and network discovery, closes network sources and casts
  (Simple goes back to a device on this PC), and nothing listens on the network (checked: no TCP / UDP endpoints
  while off, with sharing, the receiver and a live S22 cast running before). Everything wireless is shown greyed
  out with a one-line reason; the top-bar share button too. The saved choices (receiver, own Wi-Fi, sharing) come
  back when it's turned on. Test flags: `--no-wireless`, `--wireless-toggle-test`.
- **Casts live in the Wireless tab** (and Advanced scenes): Simple mode no longer lists casts in the device menu,
  number keys or multiview, and no longer switches to a new cast; a toast points to the Wireless tab. Opening the
  Wireless tab shows a connected cast; going back to Simple returns to the device shown before (not while
  recording: switching would end it). Network sources from other PCs stay in Simple's menu.
- **No em dashes** anywhere in the interface, log or code comments (commas / colons instead).
- **Collapsible sections** in Settings (Video, Audio, Recording, Network, Power & memory, General), the Wireless
  panel and the Advanced side panel (Scenes, Sources, Properties, Filters): click the header, or Enter / Space on the
  header clicked last. Chevron at the right end; which sections are closed is remembered (`[Ui] Collapsed`). All
  open by default.
- **Fixed: the top bar hid late and unevenly.** Since the interface is only redrawn when something changes, the
  2.5 s hide started up to 0.25 s late (up to 1 s in Low-resource mode) and the fade ran per drawing, not per
  second. Now the redraw happens exactly at the hide moment and fades run on real time: measured 2.50 s to the
  fade start and 0.25 s to hidden at 60, 24 fps and in Low-resource mode (before: 2.64-3.17 s). A fully shown bar
  no longer keeps the main loop waking every 16 ms (it stays visible in Advanced and the Wireless tab, by design).
  Closing a menu or Settings, or moving the mouse off the bar, starts the 2.5 s again. Test flag: `--bar-test`.
- **Removed: the 3D LUT filter** (Advanced ▸ source ▸ Filters ▸ Apply 3D LUT, the `.cube` parser, the 3D texture and its
  shader step). Scenes saved with a LUT still load; the setting is ignored and dropped on the next save.
- **Every hint says what is true now** (audit of Settings, the Wireless panel, F3, pills and toasts):
  - *Picture shape* described a fixed "SD (720×480 / 576) shown 4:3" even for a 1080p source. It now describes the
    active source: "1920×1080 · square pixels (16:9)", "720×480 · shown 4:3 (SD)" or "… · stretched to 4:3".
  - **Fixed: a recording that couldn't write its file looked like it was recording** (the REC pill kept going, the
    error only came at the stop). Now the folder is checked first (created, then a write probe): "Can't save the
    recording: the folder … can't be written". The same for replays, Flashback clips and screenshots. A write error
    during a recording stops it right away and says why.
  - "Recording also stops by itself when less than 1 GB is free" now shows the free space there now.
  - *Separate audio tracks* says what the next recording gets ("3 tracks: the mix, then each source"), or that
    without a microphone (Simple) there is one sound and so one track.
  - *Global hotkeys*: the label lists Ctrl+Alt+H too, and says when another app already owns some of them.
  - "Wireless features off" names exactly what stopped.
- **AI upscaling is offered only where it can work, and says why not before you try.**
  - Graphics card model and driver version are read (DXGI adapter description, device id, user-mode driver version
    from `IDXGIAdapter::CheckInterfaceSupport`). NVIDIA: RTX only (GTX / MX disabled with "this is a GeForce GTX 1050"),
    driver 531.18+ (RTX 20: 545.84+, NVIDIA's version from the last digits, 32.0.15.8157 = 581.57). Intel: driver
    27.20.100.8681+. AMD: disabled, "AMD offers its video upscaler only through its own developer kit". Software
    rendering: disabled.
  - **Silent check of the driver's own switch**: while Settings ▸ Video is open, a 960×540 still is upscaled off
    screen to 1080p with and without the extension (12 blits spread over frames after the present, read back without
    waiting) and compared. Rejected → disabled with the reason; no effect → the steps to switch it on appear above the
    toggle (NVIDIA App path; "turned off on battery" when on battery). Checked again when the window becomes active
    (`WM_ACTIVATEAPP`, e.g. back from the NVIDIA App) and when the power source changes.
  - The toggle names the vendor's feature: "AI upscaling (RTX Video Super Resolution)" / "(Intel Video Super
    Resolution)". It follows the GPU that draws (a new check after a graphics card change).
  - Tested here: HD 630 → the driver rejects it (disabled, Intel reason); GTX 1050 (`--gpu 1`) → "Needs an NVIDIA RTX
    graphics card"; software → disabled. Other cards via `--superres-sim` (RTX on / off, old driver, RTX 20, AMD, Intel
    rejected / no effect). Real RTX and Iris Xe confirmation: on the user's PCs.
  - UI toolkit: `Ui::MeasureBlock` (height of wrapped text) so notes size themselves.
- **Checked at the end of the round:** clean build 0 warnings; exe 1.88 MB (was 1.60 MB; the new features),
  only Windows DLLs. Full regression set (30 self-tests incl. the new studio / bar / wireless / device-window tests,
  LAN with 2 % loss, portable toggle): every recording decodes without errors. `/analyze`: one new finding fixed
  (clipboard `GlobalLock` result not checked), the 8 older ones unchanged. AddressSanitizer: 24 self-tests (the earlier set plus studio test, wireless toggle,
  device window, hotkey capture + screenshot, export / reset, AI upscaling simulation, top bar), no reports.
- **Reset any setting**: a small reset icon follows a setting's name only while it differs from the default (one
  click puts it back, with what that setting does when changed, e.g. the audio output restarts). Hotkeys: *Default
  keys*. Folders: *Default*.
- **Settings ▸ General ▸ Export / Import / Reset all**: Export writes settings and scenes to one file (without the
  window position, this PC's learned encoder results and the own Wi-Fi password, which only works for this Windows
  user). Import and Reset all ask first, apply while ZeroDisplay closes (after its last save) and start it again;
  Reset keeps the scenes. Test flags `--export-settings`, `--import-settings`, `--reset-settings` (no restart).
- **Ask before closing** (Settings ▸ General, on by default): closing while recording, while someone watches what
  you share, or with replay footage that wasn't saved asks first and says which (Cancel keeps it running).
- **Sound cue** (Settings ▸ Recording, off by default): short tones (made by the app, no sound files) when a
  recording starts / stops and when a replay or clip is saved.
- **Custom hotkeys** (Settings ▸ Hotkeys): record, save replay, screenshot, mute, clean output, full screen,
  statistics and mini player each get a key in the window and a global key (works in any app). Click, press the new
  key (Esc cancels, Backspace removes). A key belongs to one action (taken over with a note); keys ZeroDisplay uses
  itself (Esc, Tab, Enter, Space, arrows, 1-9, Ctrl+Z / Y) are refused; global keys need Ctrl or Alt; a global key
  another app owns shows "taken". Tooltips and labels show the chosen keys. `[Hotkeys] <Action>`, `<Action>Global`.
  The "Global hotkeys" switch moved here.
- **Settings ▸ Files** (new section):
  - **Folders** for recordings / replays / clips and for screenshots: the path, *Change…* (Windows' folder picker;
    the folder is checked by writing a probe file before it's used), *Open*, *Default*. `[Recording] Folder` (had no
    UI before), `[Screenshots] Folder`.
  - **File name template** `{type} {device} {date} {time}` (default `{type}_{date}_{time}`, the old names), with the
    next name shown under the box. Characters Windows doesn't allow become `_`; an existing name gets " (2)".
    Used for recordings, replays, clips and screenshots.
  - **Screenshots**: PNG or JPG (WIC, quality 0.92), *also copy to the clipboard* (CF_DIB), *flash the screen* (a
    0.2 s white flash, on by default).
  - **Stop recording below** 1 / 2 / 5 / 10 / 20 GB free (was a fixed 1 GB), with today's free space.
  - **Keep recordings under** 50 / 100 / 250 / 500 GB (off by default; asks before turning on): above it, the oldest
    recordings, replays and clips *ZeroDisplay made* (listed in `recordings.txt` next to the settings, UTF-8) go to
    the Recycle Bin; other files in the folder are never touched, the newest is never removed, and a file too big
    for the Recycle Bin makes Windows ask before anything is deleted for good. Tested with `--keep-test-mb`.
- **Each device in its own window**: the device menu offers "Open <device> in a new window" for every device not
  in a window yet. A normal, resizable window named after the device (dark title bar), drawn after the main
  window's present with the same picture path (the device stays open while its window is, in any mode). Double-click
  or F11: full screen on that window's monitor, Esc leaves it. Position and "was open" are remembered per device and
  the windows come back at the next start; closing one closes the device unless something else shows it. Hiding a
  device closes its window. Test flag `--device-window <name>`.
- **Fixed: the mini player kept a swap chain of the old GPU device** after a graphics card change or device loss
  (now made again on the new device, like the device windows).
- **Self-tests can no longer write the user's settings or scenes at all** (`Settings::s_readOnly`): the profile API
  writes at once, and a few places wrote outside `SaveSettings` (window position on close, crash-report marker, format
  / audio / picture choices). Only a `--profile-dir` test profile is written.
- **Hide devices** (Settings ▸ Devices, or the last row of the device menu: "Hide <device> from this list"): a
  camera you never want (a laptop's webcam) is left out of the device menu, multiview, number keys and Advanced,
  and never opened. Hidden devices that aren't connected stay listed so they can be shown again. With everything
  hidden the screen says "N connected device(s) hidden: Settings ▸ Devices shows them again". `[Devices] Hidden`.
- **Studio mode checked end to end** with the live card (`--studio-test`): Cut / Fade / Slide / Wipe at 300 and
  1000 ms each end on the right scene in the expected time (Fade 1000 ms: 1018 ms), number keys pick the preview only,
  editing the preview leaves the program untouched, undo / redo restore exactly, recording + sharing during all of it
  (file decodes without errors, a second instance receives the program). HD 630: studio + recording 48.6 fps; with
  sharing too and a transition every second ~37 fps (GPU bound, as before).
- UI toolkit: disabled toggles / buttons (greyed out, say why next to them).
- Test flag `--ui-steps "t:click:x,y;t:move:x,y;t:key:vk"`: input fed into the app during a self-test (posted mouse
  messages lose their position to the real cursor when the window captures the mouse).

### Added / changed (optimization, Battery saver, Memory saver, Low-resource mode, bug check, 2026-10-06)
- **Settings ▸ General ▸ Power & memory** (one profile in the app that every feature asks):
  - **Battery saver**: Off / *On battery* (default) / Always: replay buffer off, recording and sharing encoded on the
    display GPU (the second GPU stays asleep), Enhance off, Advanced studio preview drawn at half rate, network
    discovery paused, the 1 ms timer released. The preview keeps its frame rate.
  - **Memory saver**: Off / *When Windows runs low* (default; Windows' low-memory signal or ≥ 90 % in use) / Always:
    replay buffer (~140 MB) and so Flashback off, Enhance off, images / text of Advanced scenes not shown freed (reloaded
    when shown), memory handed back to Windows while minimized.
  - **Low-resource mode** (manual, for older PCs): both savers plus preview at 30 fps, no screen filters, no studio
    mode or multiview, sharing at most 6 Mbps, interface numbers refreshed once a second. On a first start on a weak
    PC (< 3.5 GB RAM, ≤ 2 logical processors or software rendering) it's switched on once, with a note.
  - A pill on the top bar (BATTERY SAVER / MEMORY SAVER / LOW RESOURCE) and a toast say what's active and what paused.
  - **Efficiency mode for background work** (on by default, any profile): finishing recordings, saving replays /
    clips / screenshots, the recording file writer, network discovery, the welcome page and the firewall check run with
    Windows' EcoQoS (efficient cores, low clock). Picture, capture, sound, encoders, sharing, Flashback and trim playback
    never do. Per thread (`SetThreadInformation(ThreadPowerThrottling)`); pooled threads drop it when the job ends.
- **Replay length shows its memory** ("270 s · ~940 MB of memory"); the Memory saver text shows the current figure.
- **Settings ▸ Video ▸ Limit preview to 30 fps**: every other frame is still captured, recorded and shared, only not
  drawn. When the preview misses > 10 % of frames for 10 s, a toast suggests it (or Low-resource mode). The F3
  statistics show "on screen … fps".
- Test flags (not saved): `--power battery|ac`, `--low-memory`, `--low-resource`, `--preview-30`.
- **Crash reports**: on a crash a small minidump `crash-<date>.dmp` is saved next to the settings (dbghelp, part of
  Windows, loaded only then); the next start says where it is.
- **Low disk space**: recording start says how many minutes fit when it's under 30; a toast at ~5 minutes left
  (recording still stops at 1 GB free as before).

### Optimized (no feature lost)
- **Interface drawn only when it changes**: it lives in its own layer and is redrawn on input, animations, toasts,
  popups or every 250 ms for live numbers; otherwise the cached layer is blended in one quad. UI cost per frame
  1 ms → 0.05–0.07 ms (viewing).
- **Minimized / fully covered window**: nothing is drawn or presented; capture, recording, replay, sharing and the mini
  player continue.
- **Nothing new, nothing drawn**: wake-ups without a new picture or interface change (mouse moves over the picture,
  encoder events, timers) no longer redraw and present the same frame (Simple mode; at least every 100 ms anyway).
- **Second-GPU recording**: the per-frame read-back moved off the render thread to its own worker (2.5 → 0.05 ms per
  frame on the render thread).
- **Idle background work**: network discovery probes every 2 s only while the device menu / Settings is open or in
  the first 10 s, otherwise every 30 s (and not at all under Battery saver / Low-resource); the 1 ms timer only while
  something real-time runs.
- Measured (i5-7300HQ + HD 630, 1080p60, % of one core): viewing the card 16 % → 10 % (render thread 9.7 % → 3.9 %);
  with the 30 fps limit render thread 1.6 %; replay buffer 29 % / 207 MB private → under Battery or Memory saver 16 % /
  79 MB; minimized: no drawing at all.
- Self-test `[selftest final]` now logs CPU per thread, private / working set memory, GPU memory and CPU / GPU time per
  frame section (the measuring tool for this work).

### Fixed (bug check: MSVC `/analyze` of the whole app + AddressSanitizer through 10 scenarios)
- Welcome-page DNS parser: a malformed packet could read past the buffer; now bounds-checked (+ `--portal-test`:
  truncations and 20 000 random packets).
- Cast audio: the frame count came from the data instead of the buffer size; clamped.
- Clean fallbacks when Windows can't create an event / timer (hotspot, audio capture, program mixer: the mixer no
  longer spins) and when the random generator fails (hotspot password).
- AddressSanitizer: no memory errors in view, record, replay + flashback, HEVC split, network send/receive with loss,
  Advanced + studio + record + undo, multiview record, Enhance, trim, and the new profiles (battery, low-resource +
  low memory, 30 fps preview). Full regression set (24 scenarios): every recording decodes with 0 errors.

### Fixed (desktop: card without 1080p60, 2026-10-06)
- A USB 3 capture card that comes up at **USB 2 speed** (USB-A plug not fully in, a USB 2 port / cable / hub) offers
  no fast uncompressed formats: the MS2130 then lists 26 fewer (every YUY2 mode above ~48 MB/s, incl. 1080p60/50),
  and the app silently fell back. Now: the connection speed is read from the USB hub (documented
  `IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2`, read-only), logged, shown in F3 ("YUY2 (GPU copy, USB 3)"), and a
  toast says to plug the card fully into a USB 3 port when a USB 3 device runs at USB 2 speed.
- Default format choice: at equal size and rate, YUY2 / UYVY win over NV12 (on MJPEG cards NV12 is Windows decoding
  the card's MJPEG: compressed quality, more latency).

### Security / safety (desktop review, 2026-10-11)
- **Network stream receiver** (streams from another ZeroDisplay / the phone): a sender declared each frame's size and
  it was allocated before any check (up to 4 GB per frame, many frames); now sizes must be consistent and ≤ 32 MB,
  at most 64 frames in flight; names from the network are always terminated (they could be read past their end);
  absurd stream descriptions (0 / > 16384 px, > 1000 fps) are ignored. New fuzz test (20 000 malformed packets) runs
  with `--portal-test` next to the DNS one; both pass, also under AddressSanitizer.
- **Sharing server**: at most 16 viewers (a flood of Hellos, possibly spoofed, can't grow the list or make the PC
  stream to many addresses); whole-frame resends limited to one per 100 ms per viewer (a tiny request can't trigger
  megabits repeatedly).
- **Browser viewer**: connections that don't send a request within 10 s are closed (60 idle ones could block real
  viewers); text the page sends is logged as printable ASCII only (no fake log lines).
- **Own network password** is stored encrypted for the Windows user (DPAPI, `dpapi:` in settings.ini); plain values
  from older settings are encrypted on the next save; a copy from another PC / user can't decrypt it and gets a new one.
- **Crash reports** hold only stacks / threads / modules now (nothing they point to: no settings or picture data).
- **Portable on / off never overwrites** settings.ini / scenes.ini already in the destination (kept as `.bak`). Found
  the hard way: the portable self-test, run from `out\`, replaced the user's portable settings + scenes there
  (restored from a backup / an older copy). The test now runs a copy of the exe in a scratch folder.
- Reviewed, no change needed: DNS / HTTP welcome page (bounded, own IP only, own data only), viewer page (names as
  JSON + textContent, no HTML injection; only its own page and /ws served), discovery (names length-bounded),
  `/analyze` (no new findings), AddressSanitizer through 17 scenarios incl. the new features: 0 errors.
- Known, by design: LAN sharing and the viewer page have no password (off by default; anyone on the network can watch
  while it's on); with the cast receiver on and *Ask for a PIN* off, any nearby device can connect (like Windows' own
  "Projecting to this PC" with PIN off): turn the PIN on in public places.

### Fixed / added (desktop: every resolution, refresh rate and shape, 2026-10-11)
- **Sources faster than the screen** (120 / 144 fps on a 60 Hz monitor) were recorded and shared at the screen's rate
  (~60 fps): the loop took one frame per refresh. Now such a source is presented without waiting for the refresh, so
  every frame is captured, recorded and shared (720p120 → 119.6 fps in the file, 1080p144 → 143.5, 1440p120 → 115.5,
  1280×1024@75 → 74.9, 3440×1440@100 → 95.6). The monitor's refresh rate is read from Windows (and re-read on a
  display change). Encoder copies are now sent to the GPU right away instead of with the next present.
- **Sources that change mode mid-recording** (a game switching resolution / refresh rate, a phone rotating): the
  recording continues in one file at its starting size (other sizes fitted in), and each encoder takes frames at
  most at its own rate; before, a 60 → 120 fps change overran the encoder, which restarted and split the file.
- **This PC's display changing** (resolution, orientation, refresh, e.g. a full-screen game): the app now listens for
  it, re-reads the refresh rate, and Auto encoder decisions ignore the stall a mode change causes and act only on a
  sustained shortfall (two 2 s windows), so a brief hiccup no longer splits a recording.
- **Settings ▸ Video ▸ Picture shape**: Auto / 4:3 / 16:9 / Source pixels. Auto shows SD frames (720 / 704 × 480 /
  576, 240 / 288) as the 4:3 TV pictures they are (PS2 / Wii / GameCube-era 480p); everything else keeps square
  pixels. Screen only: hardware encoders refuse non-square pixels, so recordings keep the source pixels.
- Pictures wider or taller than 4096 try HEVC first (H.264 encoders stop at 4096), then H.264 software.
- Tests: `--test-mode WxH@fps` (any size / rate, 59.94 etc.; the test pattern's drawing is now clipped to any size),
  `--test-modes a,b,c` + `--test-switch-sec N` (mode changes with a signal drop), `--all-formats[-from N]` (every
  format of a device). Results: the card's 165 formats 163 + 2 (a one-off stall after 160 switches, didn't
  reproduce); 17 synthetic modes from 640×480 to 8K, SD 59.94 / 50, 16:10, 21:9, 32:9, portrait, all recorded with
  0 decode errors; display changes 1280×720 / 1024×768 / portrait / 1366×768 while recording the card: no crash,
  restored; phone cast rotated and resized while recording: one clean file.
- Limits seen on this laptop (not app bugs): 5120×1440 records in software (~40 fps; newer GPUs' HEVC handles it);
  any non-native screen mode slows the HD 630 a lot while encoding; the internal panel only offers 60 Hz.

### Changed (desktop: own Wi-Fi, 2026-10-11)
- **The network password is hidden by default** (dots) with a Show / Hide button; the "1 Join" QR code (it contains
  the password) is hidden with it until shown. Not saved: hidden again on the next start.
- Tested: **the Android app's Cast to PC over the PC's own network** (S22 joined "ZeroDisplay-Test"): found by
  discovery, 59.9 fps, 0 lost, **89 ms median** glass-to-PC (router: 64 ms; this laptop's one Wi-Fi card shares its
  airtime between the router and its own network). The Wireless page now says only Smart View / Win+K mirroring pauses
  while the own network is on.
- Fixed: an explicit `--profile-dir` now wins over portable settings next to the exe (tests never touch them).

### Added (desktop: AI upscaling, 2026-10-11)
- **Settings ▸ Video ▸ AI upscaling (RTX Video Super Resolution)**, off by default, screen only: NVIDIA RTX Video
  Super Resolution (and Intel Arc / Xe VSR) through the standard D3D11 video processor
  (`VideoProcessorSetStreamExtension` with the vendors' published interface values; `render/SuperRes.cpp`). Used for
  smooth pictures shown larger than their source, one picture per frame (not multiview / studio), off under the savers.
- Drivers don't say whether they apply it, so the app **checks**: after 30 frames it renders the same picture through
  a second processor without the extension and compares a centre crop. No difference → "Not active" with the reason
  (e.g. turn it on in the NVIDIA App) and the normal path draws. Status in Settings and F3; `--ai-upscale` test flag.
- Tested here (Intel HD 630): the Intel extension is rejected, the check reports no effect, the normal path draws at
  60 fps; studio + recording and Simple recording unchanged. NVIDIA path to be tested on an RTX card.

### Fixed (desktop: interface tied to the picture rate, 2026-10-06)
- With the 30 fps preview limit / Low-resource mode the whole frame was skipped, interface included, so menus and
  Settings ran at the picture's rate; an open top-bar dropdown also only woke the loop for input or new frames. Now the
  interface is drawn on every input / animation / open menu regardless of the picture (limits apply to picture-only
  frames). Measured with simulated mouse input (~63 moves/s, card at 10 fps): 30 fps limit 309 → 751 interface draws
  in 12 s; without the limit 716 (unchanged); normal 60 fps unaffected (arrival → present 1.4 ms).

### Fixed (desktop: replay length, 2026-10-06)
- Changing the replay length no longer throws the buffer away: longer keeps everything and fills up, shorter drops
  only the oldest part (`ReplayBuffer::SetSeconds`). Self-test `--replay-resize-test`: 10 → 30 s mid-run kept 11.2 s,
  the clip came out 26.7 s.

### Added (Android: saver, 2026-10-06)
- **Android Battery Saver or low memory** (polled every 5 s) turns off the replay buffer and Enhance, like the
  desktop's savers; picture, sound, recording and sharing stay. Toast, overlay line and Settings show it; back on
  automatically. Tested on the S22 (`--ez saver true` simulates it): no replay with the saver, a clip without it; the
  card at 60 fps with Enhance held off.

### Changed (desktop: Advanced re-check with a live Switch, 2026-10-06)
- Studio + recording + sharing on the live card: works, recordings clean. A busy GPU (HD 630: studio + two 25 Mbps
  encoders ≈ 17.5 ms GPU per frame) gives ~42–48 fps; the canvas encoder now drops frames instead of sending catch-up
  bursts (up to 4 frames within a millisecond, a stutter on playback). Same for standby frames.

### Added (Android: picture modes, 2026-10-06)
- **Settings ▸ Picture**: Auto, or any size / frame rate / format the card offers that the USB link can carry
  (uncompressed or MJPEG; a USB 2 link leaves out the big uncompressed ones). Saved; the card reopens with it.
- **Auto prefers 1080p60**: 50+ fps first, at the largest size up to 1920×1080, uncompressed over MJPEG, so 2K / 4K
  cards (e.g. 4K30 + 1440p30 + 1080p60) open at 1080p60; a card without 50+ fps gets its largest 30 fps mode.
  `uvc/CardModes`, unit-tested on the PC (23/23, incl. the real card's descriptors and simulated 2K/4K cards).
  Tested on the S22 with the card: 110 modes, switching Auto ↔ MJPEG ↔ other sizes works.
- **Fixed: MJPEG showed nothing after an uncompressed session** (the card kept its old stream running: a bulk
  stream has no alternate setting to switch off). The driver now resets the stream endpoint before starting and when
  stopping, like Windows' and Linux's drivers. MJPEG 1080p60 on the S22: 60 fps, 0 decode errors.

### Fixed (Android, 2026-10-06)
- **The capture card showed nothing on the phone**: Android refuses USB video devices to apps without the Camera
  permission (silently, no dialog). The app now declares it and asks for Camera + microphone access before opening
  the card. Tested on the S22 with the card and a Switch: 1080p60 YUY2, 60 fps shown, with sound.

### Added (Android version started, 2026-10-06)
- **ZeroDisplay for Android** (`android/`, see `ANDROID_HANDOFF.md`): built by `android\build.ps1` with the SDK's
  own tools (no Gradle, no AndroidX, no third-party code). First feature: **Cast to PC**: the phone's screen and
  sound go to ZeroDisplay on a PC in the desktop app's own LAN stream format (the PC lists "Network: <phone>").
  Measured on a Galaxy S22: phone screen → PC **64 ms** median (Smart View: 437 ms), 60 fps, rotation followed,
  recordings A/V −20…+14 ms.
- **Android: capture card driver** (own UVC driver over the Java USB API; the card streams over bulk), written and
  unit-tested on the PC against the card's real descriptors (15/15); **capture screen** (GL ES renderer, 60 fps),
  **sound** (the card's USB input), **recording** (H.264 + AAC to Movies/ZeroDisplay, A/V −5 ms) and
  **screenshots**, tested on the S22 with a built-in test pattern. The first run with the card itself waits for the
  card to be moved to the phone.
- **Android: crash-safe recordings** (own fragmented MP4 writer; killed mid-recording → plays to its last second),
  **replay buffer** (last 30 s, ↺), **Enhance** and **retro screen filters** (the desktop's shaders in GLSL ES),
  **mini player** (picture-in-picture), **no-signal standby** note, all tested on the S22 with the test pattern.
- **Android: MJPEG** for capture cards on a USB 2 link (decoded on the phone, 1080p60 at ~10.5 ms per frame),
  **microphone commentary** (recordings / replays / sharing only), **share the capture with PCs** (the PC lists
  "Network: <phone>"; 1080p ~60 fps, 0 lost).
- PC: network streams map the sender's timestamps to the PC clock (smallest arrival − send time of the last 5–10 s),
  so recordings of network sources line up sound and picture by capture time (PC→PC: 0…−2 ms).

### Added (Enhance, 2026-10-06)
- **Settings ▸ Video ▸ Enhance** (on screen only; recordings, replays and streams keep the original picture):
  *Off* (default) / *Sharpen* / *Upscale + Sharpen*, with a strength slider (50 % default). Two own shaders:
  - `ps_sharpen`: contrast-adaptive sharpening: unsharp mask on luma whose gain falls as local contrast rises (soft
    areas sharpen most, strong edges least), kept inside the pixel's neighbourhood range (a little wider at high
    strength) so there are no halos.
  - `ps_upscale`: edge-aware upscaling: 12 nearest pixels (four 4-texel gathers), edge direction from luma
    gradients, a kernel with a small negative lobe stretched along the edge and tightened across it, clamped to the 4
    nearest pixels (no ringing); half precision.
  - Pipeline: shown 1:1 → sharpen only; enlarged → sharpen at the source size, then upscale (edge-aware, or bicubic
    for *Sharpen*); shown smaller → bicubic as before. Needs Smooth scaling; off while a screen filter is on; not
    offered with software rendering (it halved the frame rate there).
- Measured (half-size picture shown at 1762×991 on the HD 630, against the full-size original): *Sharpen* +0.3 ms GPU
  per frame, slightly higher PSNR and crisper edges; *Upscale + Sharpen* +1.3 ms (bicubic alone is 1.6 ms here),
  edges closest to the original (edge energy 7.57 vs 6.72 bicubic, original 8.35) at −1 dB PSNR on small text.
  Everything at 60 fps (Simple, multiview, mini player, Advanced + studio + recording).
- Test tools: `--enhance <0-2>`, `--enhance-strength <0-1>`, `--scale <fit|fill|stretch|integer>`, `--test-image <png>`
  (the test source shows a still picture at its own size).

### Added (wireless streaming W2 + W3, 2026-10-06)
- **This PC's own Wi-Fi** (Wireless tab ▸ *Create a Wi-Fi network for phones*): a Wi-Fi Direct network in "legacy"
  mode, a normal WPA2 network named `ZeroDisplay-XXXX` with a random password (both editable, saved), that exists
  only while it is on; nothing is stored in Windows. If Wi-Fi Direct can't start, Windows Mobile Hotspot is used
  instead and its previous name / password are put back when it is turned off (verified: off and restored).
  Status shows how many devices joined. Tested with the Galaxy S22: joins at 5 GHz (626 Mbps link), the browser
  viewer plays 1080p60 over it at ~200–300 ms (4 ms round trip).
- **Casting and the own network take turns** on Wi-Fi adapters that can't do both (this PC's AX210): the receiver steps
  aside while the network starts, then tries again next to it and says *Paused while this PC's own Wi-Fi network is
  on*; it comes back by itself when the network is turned off.
- **Welcome page** for phones on the own network (`http://192.168.137.1/`): watch button (live when sharing is on),
  how to cast from Android / Windows, the iPhone note; picked by the phone's browser. A DNS responder and the page are
  bound to the hotspot address only. Windows' Wi-Fi Direct network gives phones no DNS server, so phones don't open it
  by themselves; the QR code does.
- **QR codes** (own encoder, byte mode, level M, versions 1–10, checked with an independent decoder and by decoding
  the app's own screenshots): the browser viewer address under *Watch on phones and tablets* (with a sharing toggle),
  and on the own network *1 Join* (Wi-Fi join code) + *2 Open* (the welcome page).
- **Windows Firewall check** (read-only): the Wireless tab knows whether ZeroDisplay may receive on public networks
  and shows *Allow through Windows Firewall* before anything fails (casts and phones on the own network both arrive
  over Wi-Fi Direct, a public network). One Windows administrator prompt; once allowed, both work.

### Fixed (wireless, measured with real casts from a Galaxy S22, 2026-10-06)
- **Cast recordings ran 5 % fast against their sound** (offset growing ~50 ms a second). Casts have a variable frame
  rate and Windows' MP4 writer builds the timeline from frame durations, which were all the nominal one. Each frame
  now lasts until the next one (held back one frame; the last frame of a part ends at the next part's start).
  Measured: flashes exactly 1.000 s apart, A/V −29…+1 ms (about −10 ms). Also helps network sources.
- **A cast arriving while recording ended the recording** (Simple switched to it, and switching stops recording;
  plugging in a new USB device did the same through "switch to new devices"). New devices and casts no longer take
  over while recording; a toast says so and the Wireless tab's *Show* switches when wanted.
- **A cast nobody watched was dropped by the phone after ~35 s** (the stream must be read). Connected casts now stay
  open in the background (unseen, unheard) in every mode until they disconnect.
- **Casting again after a disconnect** briefly reopened the capture card and started a new source; the cast now plays
  the new stream in the same source, so recording, sharing and scene placement carry on (verified: one 92 s
  recording across a disconnect and reconnect, picture back ~1.2 s after reconnecting).
- Removed the unused AudioGraph cast-sound code.
- Measured cast latency (phone screen → PC, clock-synced flips): median 437 ms (426–475). It is in Samsung's sender
  and Windows' Miracast stack (the player's audio category makes no difference); for games use HDMI.
- Test tools: `--hotspot`, `--hotspot-toggle-test`, `--record-cast`, `--flip-log`.
- Footprint after this round: exe 1.60 MB (was 1.24 MB), 1080p60 viewing 4.1 % CPU, 90 MB RAM, no NVIDIA driver
  and no wireless component loaded while those features are off. Full regression set passed (every file decodes cleanly).

### Added (wireless streaming, 2026-10-06)
- **Wireless tab** (top bar: Simple | Advanced | Wireless): "Cast to this PC" with per-device steps, receiver on/off
  (off until the user opens the tab or turns it on; it then keeps working in every mode), PIN option, connected
  devices with Show / Disconnect. Android (Smart View / Cast), Windows (Win+K) cast through Windows' Miracast receiver
  API; iPhone / iPad / Pixel are told to use an HDMI adapter (AirPlay / Google Cast can't be received).
- A cast becomes a source "Wireless: <device>" like the capture card (Simple, multiview, Advanced scenes, recording,
  replay, sharing). Simple switches to a new cast; if it never shows a picture the previous device comes back.
- **Cast picture**: MediaPlayer frame server → our BGRA textures → the normal GPU path. **Cast sound** goes through our
  mixer and recordings via an own audio effect (`ZeroDisplay.CastAudioTap`, registration-free WinRT served by the exe
  itself, declared in app.manifest), released at the player's presentation clock. Verified with a test file
  (`--media-test`): picture correct, sound recorded. With real casts: see the W2 + W3 entry above (A/V about −10 ms).
- **Windows Firewall**: the cast's video comes in over the Wi-Fi Direct link, a "public" network where ZeroDisplay is
  not allowed by default. The tab detects "connected but no picture" and offers *Allow casting through Windows
  Firewall* (one Windows administrator prompt that adds an inbound rule for ZeroDisplay on public wireless networks).
- Build: app.rc is rebuilt when app.manifest / app.ico change (they were embedded stale before).

### Added / changed (2026-10-06, open items + wireless round)
- **Encoder colour conversion by our own shader** (BGRA → NV12 Y and UV planes rendered straight into the encoder's
  textures, in a separate device-context state) instead of the GPU video processor; the video processor stays as the
  fallback (no NV12 render targets) and `--encode-convert-vp` forces it. Same colours (decoded bars and ramp identical).
  GPU time of the conversions with recording + sharing: display GPU 9.1 → 5.1 ms, hybrid 4.8 → 3.1 ms per frame.
- **Recordings say they are BT.709, limited range** (container `colr` + the stream where the encoder writes it).
  Before, tools that assume BT.601 for untagged video (e.g. ffmpeg) showed slightly wrong colours.
- **F3 statistics show GPU time per frame section** (upload / program / draw / UI / present / encode), measured with
  timestamp queries read back later (never waits); only while F3 is open or in self-tests.
- **Browser viewer reports what it sees**: the page syncs its clock with the PC and measures capture→screen latency,
  fps, dropped frames and buffer, shows latency next to fps and sends it to the PC's log every 2 s. Measured on a
  Galaxy S22 (Samsung Internet, MSE, Wi-Fi 6): 60 fps 1080p, ~175 ms average (min ~120), any bitrate 4–25 Mbps.
  The MSE live-edge tuning can be overridden for tests (`?seek=&hi=&lo=&fast=`); tighter settings measured worse.
- **No recording gap when the encoder changes** (Auto moving to a faster preset or to the display GPU, watchdog):
  `VideoEncoder` is now a stable front (recorder, replay, viewers stay attached) around an `EncoderCore`; a
  replacement core is built on a worker thread while the old one keeps encoding and is swapped in between two
  frames. Measured: the hole went from ~1.5 s to ~2 frames; every part decodes cleanly.
- **HEVC recordings are crash-safe**: our own fragmented-MP4 writer (`encode/Mp4File`, behind the IMFSinkWriter
  interface, hev1 + hvcC + BT.709 colr, AAC per track, a fragment about every second). Windows' fragmented MP4 sink
  still rejects HEVC (re-checked). Killed mid-recording → the file plays up to the last fragment, 0 decode errors.
  A/V sync measured with the test pattern (flash ↔ 1 kHz blip): H.264 −2…0 ms, HEVC −2…−1 ms, HEVC replay −4…−3 ms.
- **Software rendering (no usable GPU)**: the preview uses bilinear instead of bicubic scaling on WARP (23 → 60 fps
  at 1080p on the i5-7300HQ), and the encoder's CPU colour conversion moved to a worker thread (non-blocking staging
  ring, fixed-point maths); recording on WARP now holds ~59 fps (was ~16).
- **Advanced mode: undo / redo** (Ctrl+Z, Ctrl+Y or Ctrl+Shift+Z, and buttons at the top of the scene panel), up to
  50 steps. A whole drag or slider move is one step; switching the scene on air is not an edit and stays put.
- **Advanced mode: free colour picker** for text and colour sources (preset swatches, saturation/brightness square,
  hue bar, #RRGGBB box) instead of five fixed colours.
- **Advanced mode: fonts** for text sources: any installed font family, Regular / Semibold / Bold, Italic.
- **Multiview recording: one file per device.** In multiview, F9 records the active device as before (mic, tracks,
  replay) and every other device into its own file `<recording>_<device>.mp4` with its own sound and encoder (with
  the main recording on the second GPU, the others use the display GPU). The REC pill shows the number of files.
- Fixed: a device fed to an encoder other than recording/sharing would have crashed standby pacing (lookup assumed
  the sharing encoder exists).
- Test tools: `--undo-test`, `--ui-select <kind>`, `--demo-font <family>`, `--multiview-devices "a|b"`,
  `--encode-convert-vp`, `--settings-scroll` also scrolls the Advanced panel.
- Test tools: `--test-pattern` (synthetic 1080p60 source with frame counter, flash square, moving box, 1 kHz blips),
  `--profile-dir` + `--portable-toggle-test` (exercise Settings ▸ Portable without touching the real profile; passed).
- Miracast receiver spike (`--wireless`): Smart View on the S22 finds and connects to "ZeroDisplay (<PC>)"; the picture
  is blocked by Windows Firewall on the Wi-Fi Direct (Public) link until the user allows it (WIRELESS_HANDOFF.md).

### Added (encoding hardware, 2026-10-05)
- **Settings ▸ Recording ▸ Encoding hardware: Auto / Hybrid / Display GPU / Software.**
  - **Auto** (default) records on a second GPU's hardware encoder when there is one, so the GPU that shows the
    picture keeps capture, display and sharing to itself (zero copy). If that GPU can't keep up even on Fast, the
    recording moves to the display GPU in place (a new `_partN` file) and Auto remembers it for that format.
  - **Hybrid** always uses the second GPU. It is greyed out on PCs without one ("needs a second GPU").
  - **Display GPU** is the previous behaviour.
  - **Software** encodes recording *and* sharing with Microsoft's encoders (HEVC through the HEVC Video Extensions if
    installed, else H.264 with a toast).

  The second GPU is chosen by capability, never by vendor name, so any pair works (Intel/AMD/NVIDIA). Frames reach it as
  NV12 in system memory through a non-blocking read-back ring. The render thread never waits; recordings get ~1 frame
  of extra latency.
- **NVENC on switchable-graphics laptops**: NVIDIA's encoder MFT refused to start (E_UNEXPECTED) unless the exe asks
  for "high performance". The exe now exports `NvOptimusEnablement` and AMD's `AmdPowerXpressRequestHighPerformance`.
  Because that reorders the GPU list:
  - *Automatic* now renders on the GPU that drives the window's monitor (display configuration API);
  - Settings ▸ Graphics card lists GPUs in a fixed order (integrated first; `--gpu 0` is still the Intel GPU);
  - the GTX 1050 now also works as the render GPU with NVENC zero-copy.

  No NVIDIA driver is loaded while nothing is encoded on it.
- Encoders that need aligned frame sizes (some AMD MFTs): the frame is coded at a size rounded up to 16 or 32 lines
  and cropped back with the display aperture. Files still decode as the real size (tested with Intel and NVENC via
  `--encode-align 32`).
- Settings ▸ Recording shows which hardware encodes ("now NVIDIA GeForce GTX 1050"). F3 stats and logs show it too, and
  rejected encoder properties are now named in the log (e.g. NVENC ignores `CODECAPI_AVEncMPVDefaultBPictureCount`).
- Test flags: `--encode-hw`, `--encode-align`, `--hybrid-slow-test`, `--settings-scroll`.

### Added (portable folders)
- **Portable mode now keeps recordings and screenshots next to the exe too**: `Videos` (recordings, clips, replays,
  trims) and `Screenshots`, next to `settings.ini`. A recording folder you chose yourself still wins.
- **Settings ▸ General ▸ Portable** turns portable mode on or off. It moves `settings.ini` / `scenes.ini` next to the
  exe or back to `%APPDATA%\ZeroDisplay`, and says so if the exe's folder can't be written (e.g. Program Files).

### Changed (latency, measured with the live Switch picture, 1080p60)
- **Pressing record never freezes the picture any more.** The recording encoder is created on a worker thread, with
  all fallbacks (second GPU → display GPU → software, HEVC → H.264); "REC starting…" shows meanwhile and F9 again
  cancels. Before, the preview stopped for ~0.3 s, or ~1.1 s when the second GPU had to wake up.
- **Starting to share no longer freezes the picture**: the sharing encoder starts on a worker thread too (the
  old one keeps serving viewers until it's ready). Recording + replay + sharing together: 52 skipped preview frames → 0.
- **Encoder restarts and stops no longer freeze the picture.** Auto preset steps, Auto moving to the display GPU, the
  watchdog, and the encoder stopping at the end of a recording or time limit: before, the preview skipped ~60–120
  frames each time; now 0.
- **Encoders get their frame right after Present** (their colour conversion used to queue on the GPU ahead of the
  picture). Arrival→present while recording on the display GPU: 5.05 → 1.93 ms.
- **Advanced mode no longer renders twice per frame while encoding.** The canvas encoder is fed on capture-driven
  frames and the loop only wakes for its own deadline when capture is late. Arrival→present with Advanced +
  recording: 11.6 → 3.4 ms on the display GPU, 7.2 → 1.5 ms with Hybrid.

| Measured (Hybrid = GTX 1050 NVENC, display = HD 630) | Result |
|---|---|
| Hybrid H.264 Fast / Balanced / Best | 60 fps each, 0 dropped, frame→packet ~26 ms, 0 decode errors |
| Hybrid HEVC Balanced | 60 fps (the HD 630 manages 31) |
| Software H.264 / HEVC (i5-7300HQ) | ~59 fps on Fast / ~26 fps (CPU-bound; the hint says so) |
| Auto fallback (`--hybrid-slow-test`) | Balanced → Fast → display GPU, 3 parts, all decode cleanly |
| Advanced + studio + recording (+ sharing) | still 48–52 fps with either hardware: the HD 630 is saturated by the studio composites (open item) |

- Footprint: plain 1080p60 viewing 3.9 % CPU, 95 MB RAM, arrival→present ~1.7 ms; exe 1.16 → 1.24 MB.

### Fixed (this session)
- The watchdog's restart was counted by Auto as "can't keep up", which stepped the preset down wrongly and blocked the
  render thread waiting for the first restart.
- The log called a Graphics card change in Settings a "GPU device loss".

### Fixed
- **Scrolling a settings/Advanced panel changed sliders** that passed under the mouse: sliders no longer react to the
  wheel (only the top-bar volume slider does, where nothing scrolls).
- **Trimmed copies started their audio late** (up to ~1.3 s): the trimmer now seeks 5 s early and keeps audio that the
  file stores before the starting keyframe. Verified on H.264 and HEVC recordings: audio and video both start at 0.
- Screenshots during standby save the frozen picture (they failed with "nothing to capture").
- **Encoder watchdog**: once, right after a clean rebuild, Intel's MFT produced nothing while recording + sharing
  (empty recording). An encoder that took ~1.5 s of frames without output is now restarted in place; recorder, replay
  and viewers stay attached (`--encoder-stall-test` simulates it: recording continues after ~3 s).
- **Crash when switching to Advanced mode** with the replay buffer on (Flashback turns it on): the encoder was
  destroyed while the replay buffer still pointed at it (`VideoEncoder::RemoveSink` on freed memory, found from the
  two Windows Error Reporting entries + PDB). All encoder teardown now goes through `App::DestroyEncoder` (recording,
  sharing and replay detach first), and the encoder is never swapped while a recording uses it.
- Leaving Advanced mode returns to the device Simple mode showed before (it used to keep whatever the scene used,
  e.g. a webcam). The remembered device is only saved from Simple mode.
- Self-test runs (`--selftest`) never write settings or scenes any more (a test had replaced the user's scene with
  the webcam and saved it as the last device; restored to the capture card).
- **LAN receiver / Flashback decoder dropped frames under load**: when the GPU H.264 decoder answered
  `MF_E_NOTACCEPTING` the access unit was thrown away (and the following frames decoded with a missing reference).
  Now finished frames are collected and the same access unit is retried.
- **LAN streaming collapsed on Wi-Fi-like packet loss** (1 % loss → ~6 fps, 670 frames lost in 20 s): one lost
  fragment discarded the frame and waited for a keyframe, and keyframes (~250 packets with a real picture) almost
  always lost a fragment too. Now the receiver reassembles several frames at once, asks for exactly the missing
  fragments (`Msg::Nack`), the sender resends them from a 2 s history, frames are decoded strictly in order, and only
  a frame that can't be repaired within 150 ms falls back to a keyframe request. Measured with the real HDMI picture:
  0 % → 60 fps / 39 ms, 1 % → 60 fps / 0 lost / 42 ms, 5 % → 60 fps / 0 lost / 57 ms. `--net-loss <percent>` simulates loss.
- **Browser viewer (older browsers / MSE path) had no sound**: Windows' AAC encoder rejects samples without a timestamp
  (`MF_E_NO_SAMPLE_TIMESTAMP`) and the error was ignored. Samples are now timestamped; 47 AAC frames/s arrive.
- Browser viewer latency on slow connections: the backlog limit is now "oldest queued data waited > 250 ms" (any link
  speed) instead of 4 MB, and the socket send buffer is 128 KB so the backlog stays where it can be skipped.
  20 Mbps link: delay stays ≤ 0.5 s (was growing to several seconds); ≥ 40 Mbps: 60 fps, ≤ 2 ms drift.
- Audio rate trim no longer reacts to the one or two underruns that happen when a stream starts (needs 3 in a row).
- Receiver and stream server log a health line every 5 s (frames complete / decoded / lost / waiting for a keyframe,
  decode time; frames from the encoder / sent / failed sends).

### Added (encoder presets)
- **Settings ▸ Recording ▸ Encoder: Auto / Fast / Balanced / Best** (recording encoder speed vs. quality;
  CODECAPI_AVEncCommonQualityVsSpeed 0 / encoder default / 100). Default **Auto**: starts at Balanced; if the encoder
  keeps up with < 95 % of the frames it switches one step faster right away and remembers the answer per GPU + codec
  + resolution + frame rate (`[EncoderAuto]` in settings.ini), so later recordings start on the right preset.
  Manual presets are never overridden: if they can't keep up, Settings shows "can't keep up!" and a one-time toast
  suggests Fast/Auto. The HEVC "always fastest" override is gone (Auto handles it).
  Measured (1080p60, live Switch picture, HD 630): Fast 60 fps / 10.7 ms; Balanced 60 fps / 13.8 ms; Best 56 fps /
  36 ms; HEVC Balanced 31 fps → Auto switches HEVC to Fast within ~2.5 s.
- The recorder starts a new part (`_part2`) whenever the encoder's stream headers change (Auto switch, watchdog
  restart), and the replay buffer drops video from before such a change; restarting an HEVC encoder mid-file had
  produced an undecodable file (577 decode errors); now every part and the replay clip decode with 0 errors.

### Changed (optimisation, measured with the live HDMI signal from a Nintendo Switch)
- **Sharp bicubic scaling** (Catmull-Rom in 9 bilinear fetches, `ps_bicubic.hlsl`) whenever the picture isn't shown
  1:1 with "Smooth scaling" on, noticeably crisper in a window (this laptop runs at 125 % scaling, so the window
  is never 1:1). No latency cost (arrival→present 1.35–1.8 ms).
- Sharing encoder uses the fastest quality/speed preset; the recording encoder keeps the default. Recording +
  sharing together: 55 fps with drops and 45 ms encode time before → both 60 fps, 0 dropped, 13–20 ms.
  (Best-quality preset: 32 fps, not usable on the HD 630.)
- Viewers are told the wired address first (Ethernet > Wi-Fi > virtual adapters; 169.254.x.x skipped); the sharing
  addresses are logged.
- Windows N editions: the start-up message offers to open Settings ▸ Optional features to add Microsoft's free Media
  Feature Pack (Media Foundation can't be bundled into the exe; Microsoft documents that capture devices don't work for
  any app on N editions without it).

### Measured (no change needed)
- YUY2 1080p60 is the best capture format: capture→present 18.8 ms vs MJPEG 25 ms (default already prefers YUY2).
- CPU 4 % of the i5-7300HQ while previewing 1080p60, 108 MB RAM.
- Real-signal tests: standby (simulated unplug while recording, frozen frame recorded), Flashback + clip, HEVC + mic +
  3 tracks + 5 s split, trim (H.264 and HEVC), CRT filter, auto-crop (correctly no crop on 16:9 content), LAN.

### Added
- **Stream quality** (Settings ▸ Network): *Smart* (default) or fixed 4 / 8 / 15 / 25 Mbps. Sharing now has its **own
  H.264 encoder** (keyframe every second), separate from recording; recordings keep their own quality/codec, and
  **HEVC recording works while sharing** (the old "sharing waits for the HEVC recording" rule is gone).
  *Smart* follows the slowest viewer: a viewer giving up on frames, asking for a keyframe, a browser viewer falling
  behind, or > 3 % resent packets → 60 % bitrate (at most every 3 s, no rise for 8 s); clean → +25 % every 6 s,
  1.5–25 Mbps. Settings shows the current rate and why it was last lowered; F3 stats show the sharing encoder.
  Because Intel's encoder accepts a live bitrate change and ignores it, a new bitrate = a new encoder, created on a
  worker thread and swapped in between frames (viewers get a keyframe). `--stream-quality <0-4>` for testing.
  Measured: browser viewer on a 10 Mbps link → 15 → 9 → 5.4 Mbps in ~5 s, then ~57 fps with ≤ 0.3 s delay; fast
  viewer → back up to 25 Mbps in ~40 s (measured output matches the target); fixed 8 Mbps → 8.0 Mbps; HEVC
  recording + smart sharing together both at 60 fps.
- `WIRELESS_HANDOFF.md`: plan for casting from phones / PCs / browsers (Miracast receiver, own hotspot with QR and
  welcome page, HTTPS browser share). Documentation only; no wireless code yet.

## [0.4.0] — 2026-10-05 — Phase 4: compatibility + capture-software features

### Added
- **Works on any GPU** (Intel / AMD / NVIDIA / iGPU / no GPU): Settings ▸ Video ▸ *Graphics card* (Automatic, any
  adapter, or *Software*). Falls back to WARP (CPU rendering) when no hardware device can be created. Formats the
  GPU can't sample (YUY2/NV12 textures) fall back to the CPU upload path. The encoder converts BGRA→NV12 on the CPU
  when the device has no video processor, and uses the software H.264 MFT when no hardware encoder exists.
  `--gpu <n|warp|auto>`, `--software-encoder` for testing.
- **HEVC (H.265) recording** (Settings ▸ Recording ▸ Video format). Falls back to H.264 when no HEVC encoder exists.
  While LAN sharing / the browser viewer runs, H.264 is used (viewers need it); sharing waits while an HEVC recording runs.
- **Microphone commentary**: Settings ▸ Audio ▸ Microphone. Mixed into recordings, the replay buffer and sharing
  with its own volume, noise gate and compressor (no noise suppression). Not played back locally. Works in Simple
  and Advanced mode. `--mic <name>` for testing.
- **Separate audio tracks**: track 1 = the mix, then each source unprocessed (e.g. game, mic) for editing.
- **Split recordings** every 15/30/60 min (seamless: the next part starts on a keyframe requested at the split time,
  recent audio is carried over) → `…_part2.mp4`. **Stop after** 30 min / 1 h / 2 h. Recording stops by itself (and
  won't start) with less than 1 GB free. `--split <s>`, `--limit <s>`, `--tracks`, `--hevc` for testing.
- Program mixer resamples inputs of any rate (44.1 kHz / 16 kHz mics) and steers each input's backlog (±0.5 %), so
  clock drift between devices never causes gaps.
- Audio sources learn a **rate trim** (up to ±3 %) after repeated underruns — for capture devices whose audio clock is
  far off their stated rate. `--audio-skew <percent>` simulates one.
- **Standby — last picture on signal loss**: when a device is unplugged, stops sending, fails, or its picture stays
  completely black for 3 s (this card's "no HDMI signal" output), the last good (non-black) picture stays on screen
  with a small "Disconnected / No signal — showing the last picture" badge (nothing in clean mode). Recordings, the
  replay buffer, LAN sharing and Advanced scenes get the frozen picture at the full frame rate, and audio tracks are
  padded with silence, so nothing stalls. Settings ▸ Video (both on by default). `--standby-test` simulates an unplug.
- **Device picture adjustments**: Settings ▸ Video ▸ *Device picture* shows the controls the device itself offers
  (IAMVideoProcAmp on the Media Foundation source — this card: brightness, contrast, hue, saturation; the webcam: 8).
  Sliders, Auto where supported, *Reset picture*. Only changed values are saved per device and re-applied on every open.
- **Auto-crop black bars** (Settings ▸ Video, off by default; display only): letterbox/pillarbox borders are measured
  on the GPU twice a second (192×108 async readback); a crop is used only after three identical results, bars are
  taken symmetric (dark scenes don't zoom) and a completely black picture never changes the crop. `--autocrop`.
- **Screen filters** for retro consoles (Settings ▸ Video, on screen only, `ps_filter.hlsl`): *Sharp pixels* (crisp
  edges at non-integer scale), *Scanlines* (beam width follows brightness), *CRT* (scanlines + aperture-grille mask +
  glow), *LCD grid*. *Original resolution* Source / 240p / 480p / 720p (the console's real resolution when the source
  upscales), strength slider. Effects fade out automatically when the window is too small to show them cleanly.
- **Flashback** — look back while staying live: ← (or the clock button in the top bar) opens the replay buffer 5 s
  back and plays it; ←/→ jump 5 s, Space pauses, drag the timeline, Esc / End / L / *LIVE* returns. Capture,
  recording and sharing continue untouched; you hear the past audio meanwhile. Decoded on a worker thread with the
  GPU H.264/HEVC decoder (`encode/VideoDecoder`, `encode/Flashback`, `AppFlashback.cpp`). Turns the replay buffer on
  when it was off.
- **Clip trimmer**: in Flashback, I / O (or the two buttons) mark start / end and *Save clip* (Enter) writes just that
  part (`Clip_*.mp4`, starts on the keyframe before the mark, video not re-encoded). Settings ▸ Recording ▸
  *Trim a recording…* opens any MP4: start / end sliders with a preview frame, *Save trimmed copy* (`*_trim.mp4`) cuts
  every video and audio track by passthrough (Source Reader → Sink Writer, nothing re-encoded) — `encode/FileTrimmer`,
  `AppTrim.cpp`. `--trim <file>`, `--flashback-test` for testing.
- **Pop-out mini player** (F6 or the top-bar button): a small always-on-top borderless window (bottom-right, ¼ of the
  screen width) showing the picture — or the program in Advanced mode, or Flashback. Drag anywhere, resize from the
  edges (stays 16:9), double-click or right-click closes. Own swap chain on the same GPU device, drawn right after
  the main window's present so it never adds latency there (`AppMini.cpp`, `Renderer::CreateOutput`). `--mini`.
- **Lag check** (Settings ▸ General ▸ *Measure capture latency*): flashes a full-screen window white 10× on the
  monitor that isn't showing ZeroDisplay (the one wired to the card), stamps when each flash was presented
  (`DwmFlush`) and finds the first captured frame showing it (per-frame 8×8 centre brightness, async readback,
  tagged with the frame's arrival time). Reports average / median / min / max. Esc or any key/click on the flash
  window stops. `--lagcheck-sim <ms>` simulates a capture chain for testing.
- Windows "N" editions: a clear message about the Media Feature Pack instead of a loader error (Media Foundation
  DLLs are delay-loaded). Power-throttling opt-out retries without the Windows 11-only flag on older Windows 10.

### Fixed
- Advanced mode's canvas encoder ran at ~48 fps (paced by loop wakeups); the main loop now wakes for each encoder
  deadline (canvas and standby frames) → 60.0 fps.
- `--device` / `--connect` no longer overwrite the remembered device.

### Verified (self-tests)
- Final regression (clean rebuild, 0 warnings, 1061 KB, Windows DLLs only): format cycling, recording, multiview,
  Advanced + studio + replay, WARP, HEVC + mic + 3 tracks, clean + CRT filter + auto-crop + mini player, LAN pair
  (receiver arrival→present 5.4 ms) — all exit 0 with no errors. The shutdown "audio underrun" log line is gone
  (sources leave the output engine before their capture stops). Version set to 0.4.0.
- Mini player: always-on-top window at the bottom-right showing the streamed demo scene (desktop screenshot).
- Lag check (simulated 40 ms chain): 10/10 flashes measured at 49.7–50.0 ms (40 ms + the wait for the next 60 fps
  frame), result reported.
- Flashback (2nd instance receiving the demo stream): opens with 5.4 s buffered, plays 1.5 s in 1.5 s, jumps, pauses;
  GPU decode; clip 0.5–2.5 s → 2.5 s H.264 + AAC starting on the keyframe at 0.
- Trim: 10.5 s replay cut 3.14–7.33 → 5.3 s file starting on the 2.04 s keyframe, H.264 + AAC, preview frame shown.
- Screen filters: CRT 240p and LCD grid 240p screenshots of a streamed demo scene show the expected scanlines / grid.
- ProcAmp: saved brightness 70 applied on open and read back from the card on the next launch (restored to 50 after).
- Auto-crop: demo scene on black streamed to a 2nd instance → crop 3.9 % / 6.9 %, nothing cut; the black card → no crop.
- Standby: instance A shares its demo scene, B receives + records + simulated unplug at 8 s → B shows the frozen scene
  with the badge; the recording continues to the end at 60 fps with the frozen scene; audio padded to full length.
- Recording valid on Intel HD 630 (QSV), GTX 1050 adapter (software MFT), WARP (24 fps) and forced software encoder.
- HEVC: Intel HEVC MFT at 60 fps / ~10 ms with the speed preset (default preset: 52 fps, 45 ms, drops). Windows'
  fragmented-MP4 sink rejects HEVC (`MF_E_INVALIDMEDIATYPE`), so HEVC files use the regular MP4 sink (finalized at stop;
  H.264 stays crash-safe fragmented MP4).
- HEVC + mic + 3 tracks + 5 s split → 2 parts, each hevc + 3×AAC; 4 s split lands on 4.0 / 8.0 s; 4 s limit stops at 4.0 s.
- Mic level in the mix equals the raw mic track (gate off); gate closes on room noise below −45 dB.
- 2.5 % slow audio source: trim converges in ~1.5 s, no underruns afterwards.

## [0.3.1] — 2026-10-05

### Added
- **Clean output mode** for screen sharing / window capture / streaming: F10, Ctrl+Alt+H or the eye button in the top bar.
  Shows only the picture: no top bar, panels, REC/LIVE pills, notifications, stats or cursor, and mouse movement
  doesn't bring the controls back. In Advanced mode the program scene fills the whole window. F10 or Esc exits;
  double-click still toggles full screen; all keyboard shortcuts and global hotkeys keep working. `--clean` starts in it.

### Verified
- Advanced demo scene in clean mode while recording: the window shows only the composed scene (no REC pill or UI).

## [0.3.0] — 2026-10-05 — Phase 3: Advanced mode

### Added
- **Simple / Advanced switch** in the top bar (remembered). Advanced mode composes scenes on the GPU; recording,
  LAN sharing and the browser viewer use the composed program output (canvas encoder, symlink `#canvas`).
- **Scenes** (`scene/Scene`, saved to `scenes.ini` next to settings.ini): add / delete / reorder / rename;
  keys 1–9 switch scenes. Sources: capture devices (and network streams), images (WIC), text (Direct2D/DirectWrite,
  with shadow), solid colours. Sources list with show/hide, add menu, delete, forward/back.
- **Canvas editing**: click to select, drag to move, corner handles to resize (aspect kept, Shift = free),
  snapping to canvas edges and centre. Properties: name, Fit / Full / Center, opacity, crop (4 sides), text content,
  font size, text colours, colour presets, device volume / mute.
- **Filters per source** (`ps_item.hlsl`, premultiplied output): chroma key (similarity / smoothness / spill),
  colour correction (brightness, contrast, saturation, gamma, hue), sharpen, 3D LUTs (`.cube`, own parser,
  amount slider).
- **Studio mode**: preview (edit) + program (on air) side by side, Transition button, Cut / Fade / Slide / Wipe,
  100–2000 ms (`ps_transition.hlsl`). Scene switches outside studio mode use the same transition.
- **Program audio mixer** (`audio/ProgramMixer`): mixes every audible device in the on-air scene into one 48 kHz
  track for recording / sharing, with per-source volume, noise gate and compressor.
- **Replay buffer** (`encode/ReplayBuffer`): keeps the last 10–300 s of encoded video + audio in memory; F8 (or
  Ctrl+Alt+S, or the top-bar button) saves it as an MP4 in about a quarter of a second (`Recorder::WriteClip`).
- **Global hotkeys** (RegisterHotKey, work while another app has focus): Ctrl+Alt+R record, Ctrl+Alt+S save replay,
  Ctrl+Alt+P screenshot, Ctrl+Alt+M mute, Ctrl+Alt+1–9 scenes. Can be turned off in Settings ▸ General.
- `Ui::TextBox` (typed input; app shortcuts pause while typing), `Settings::Open/ClearSection`.
- Self-test flags: `--advanced`, `--demo-scene`, `--studio`, `--replay`.

### Verified (self-tests on the dev machine)
- Demo scene renders as designed: background, device, wallpaper image, semi-transparent box and text;
  a chroma-keyed green box is fully removed.
- Advanced-mode recording: 9.6 s H.264 + AAC, durations match; the extracted frame equals the composed scene.
- Studio mode screenshot mid-fade shows both scenes blended; transitions end on the new scene.
- Program-mixer audio track matches the video duration (9.62 s / 9.63 s).
- Replay: 10.3 s clip saved in ~0.25 s; with a 10 s buffer over an 18 s run it still holds ~10 s (whole GOPs).
- Advanced-mode sharing: the receiving instance shows the composed scene. Simple-mode regressions (format cycling,
  recording, share/connect at ~33 ms end to end) unchanged. Clean build: 0 warnings, 902 KB, Windows DLLs only.

## [0.2.0] — 2026-10-05 — Phase 2: recording & network streaming

### Added
- **Shared hardware H.264 encoder** (`encode/VideoEncoder`): BGRA → NV12 + letterbox scaling on the GPU
  (ID3D11VideoProcessor, BT.709 limited), async hardware MFT picked by adapter LUID (Intel Quick Sync on the
  dev machine), software MFT fallback. Low-latency mode, CBR, no B-frames, GOP = 2 s, on-demand keyframes,
  packets fanned out to any number of consumers. Never blocks the render thread (drops when the pool is full).
- **Recording** (`encode/Recorder`): fragmented MP4 (crash-safe), H.264 passthrough + AAC 192 kbps.
  Audio comes from a new `AudioTap` (timestamped copies of captured audio, WASAPI QPC positions) and is
  kept locked to the capture clock (silence insert / sample skip beyond ±20 ms). Starts on a keyframe.
- Record button (F9) with red icon, blinking REC timer pill, Recording settings (quality Low/Medium/High/Max =
  8/15/25/40 Mbps, folder, "Open folder"); encoder line in the statistics overlay.
- Self-test `--record` (records from t = 2 s to the end).

- **LAN streaming** (`net/`): "Share" button / Settings ▸ Network shares the active device over UDP
  (≤1200-byte fragments, PCM s16 audio in 5 ms packets, keyframe request on loss, config resent every second,
  viewer timeout 4 s, DSCP EF). Other instances find it by broadcast discovery (also loopback) and list it as
  "Network: <PC> — <device>"; opening it runs `NetReceiver` (GPU H.264 decode in low-latency mode, frame
  reassembly, waits for a keyframe after loss, reconnects automatically). Network audio gets +8 ms reserve.
  Sharing follows the active device (stays on the recorded device while recording). `--share`, `--connect ip[:port]`.
- `VideoSource` interface (capture device or network stream); `AudioTap` notify event; encoder `RemoveSink`
  now waits for in-flight dispatch. Second instance logs to `log-<pid>.txt`. LIVE/REC status pills.
- **Browser viewer** (`net/WebServer`, `net/WebPage.h`): starts with Share on `http://<PC>:8080`. Own HTTP/1.1 +
  WebSocket server (SHA-1/Base64 handshake via BCrypt/Crypt32, TCP_NODELAY, per-viewer 4 MB backpressure that skips
  to the next keyframe). The embedded page picks its path automatically:
  - secure contexts (localhost / https): raw H.264 to WebCodecs (`optimizeForLatency`) + PCM to an AudioWorklet;
  - plain http on the LAN: fragmented MP4 to Media Source Extensions (own fMP4 muxer `encode/Mp4Fragmenter`,
    H.264 passthrough + AAC from Windows' AAC encoder `encode/AacEncoder`, audio timestamps anchored to the capture
    clock) with live-edge chasing (seek when > 350 ms behind, gentle playback-rate nudges otherwise).
  - "Tap for sound", full screen, auto-hiding bar, automatic reconnect. Settings ▸ Network shows the address.

### Verified
- Localhost sender → receiver at 1080p60: 60 fps, 0 lost frames, GPU decode, network audio 0 underruns;
  capture-device timestamp → receiver present ≈ 29–34 ms (≈19 ms of it is the USB capture itself).
- 1080p60 recording: 694 frames / 11.58 s video + 11.58 s AAC, both starting at 0; encoder 60.0 fps,
  0 dropped, ~9 ms frame→packet. Process killed mid-recording → file still playable (6.3 s of 7 s).
- Browser viewer at http://localhost:8080 (WebCodecs path): 1920×1080 at 60 fps with the device name shown.
  The plain-http MSE path (other devices on the LAN) has not been tested yet.

## [0.1.0] — 2026-10-05 — Phase 1: core capture

### Added
- Single static exe (~490 KB) using only Windows components; CMake/Ninja build via `build.ps1`,
  shaders compiled by `fxc` at build time and embedded; icon generated by `tools/make_icon.ps1`.
- Device enumeration (Media Foundation video, WASAPI audio) with automatic audio pairing by USB
  container ID (name fallback); built-in PC devices are never auto-paired (prevents mic feedback).
- Hot-plug: device notifications + IMMNotificationClient, debounced rescans, auto-switch to newly
  connected devices, automatic reconnect when a device returns, retries when a device is busy.
- Video capture: async Source Reader per device on its own worker thread; all native formats listed
  (YUY2/UYVY/NV12/RGB32/MJPEG); live format switching without restarting the app.
- GPU path: frames arrive as D3D11 textures (uncompressed and hardware-decoded MJPEG); YUY2/UYVY/NV12/BGRA
  → RGB in pixel shaders with BT.601/BT.709 and limited/full range (auto from the device, overridable).
- Presentation: flip-discard swap chain, tearing allowed, max frame latency 1, waitable object waited
  *before* taking the newest frame; optional VSync. Scaling modes Fit/Fill/Stretch/Integer.
- Audio: WASAPI event-driven capture and output (IAudioClient3 minimum period when available), Catmull-Rom
  resampler steered by a PI controller on the *minimum* buffer level per 0.5 s window (phase-independent,
  drift-proof). Presets Ultra/Low/Safe = 1.5/4/15 ms reserve. Audio delay (A/V sync) slider.
- Exclusive audio output option with automatic fallback to shared mode when the driver services the
  exclusive stream in bursts (detected on Intel SST: 3 ms period delivered in 75 ms bursts).
- Multiview: all devices side by side; click a tile to make it active (its audio plays).
- Simple-mode UI (Direct2D/DirectWrite immediate-mode toolkit): auto-hiding top bar with device / format /
  audio dropdowns, volume + level meter, mute, screenshot, multiview, stats, settings, fullscreen.
- Settings panel, INI settings (`%APPDATA%\ZeroDisplay\settings.ini`, portable mode if next to exe), log file.
- Statistics overlay (F3): fps, skipped frames, arrival→present and capture→present latency, colour space,
  audio buffer/min/reserve/drift/underruns, output callback jitter, UI and present cost.
- "No signal" detection, including cards that keep sending black frames (async 16×9 GPU probe).
- Screenshots to `Pictures\ZeroDisplay` (WIC PNG).
- Latency setup: MMCSS (Pro Audio / Games), 1 ms timer, EcoQoS power throttling disabled, above-normal priority.
- Self-test mode (`--selftest N [--cycle] [--multiview] [--exclusive] [--shot f] [--ui-shot f] [--device x]`);
  command-line flags never change saved settings.

### Measured (i5-7300HQ, Intel HD 630, MS2130-class USB3 card, 1080p60 YUY2)
- Frame arrival → present ≈ 1.5 ms; device timestamp → present ≈ 19 ms (USB transfer dominated).
- Audio path ≈ 19 ms; 3-minute soak: 0 underruns, drift locked to ~0 ppm, buffer at the 4 ms reserve.
- UI cost ≈ 1.3 ms/frame with every panel open (text layouts cached).
