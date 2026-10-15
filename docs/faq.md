# FAQ and troubleshooting

[← Back to the README](../README.md)

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
Open an issue on the project's **Issues** tab on GitHub with what happened, your capture card and the `log.txt` (Settings ▸ General shows where
the settings and log are kept). If the app crashed, the next start tells you where the crash report was saved.
