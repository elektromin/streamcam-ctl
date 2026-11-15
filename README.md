# streamcam-ctl

Fix the Logitech StreamCam's overexposure (blown-out face highlights) on macOS by
taking manual control of exposure — with a software auto-exposure loop that keeps
the face properly exposed as light changes through the day, and leaves white
balance on auto (something Logi Options+ refuses to do in manual mode).

## TL;DR usage

```bash
./streamcam-ctl status          # what the camera is doing right now
./streamcam-ctl watch           # highlight-protective auto exposure, runs until Ctrl+C
```

`watch` puts the camera in manual exposure mode and then continuously:

- keeps the **shutter at 1/60** (no motion blur, video-friendly),
- drives the **gain (ISO)** up/down so the frame brightness sits at the target,
- **protects your face from blowing out**: if >1% of face pixels are clipped
  (≥252/255) it darkens immediately, faster than it ever brightens,
- leaves **white balance on auto**,
- re-asserts the mode every 60 s in case Logi Options+ touches the camera.

Tune it: `--shutter 1/60`, `--shutter-cap 1/15` (allow slower shutter in the dark),
`--gain-cap 248`, `--target 0.5` (0–1 frame brightness), `--duration <sec>`.

## Other commands

```
./streamcam-ctl list                    # UVC cameras
./streamcam-ctl probe [--stream]        # capabilities + ranges of every control
./streamcam-ctl status [--stream]       # current values
./streamcam-ctl frames 10               # print frame brightness metrics (debug)
./streamcam-ctl hold 30                 # open a stream and hold it (for lab tools)
./streamcam-ctl apply [--aemode manual|logi] [--shutter 1/60] [--gain N]
                        [--wbauto on|off] [--wb 4600] [--backlight N] ...
```

`--aemode logi` (value 8) is Logitech's own auto mode — the stock behaviour.

## Building

```
swiftc -O -swift-version 5 -module-cache-path /tmp/swift-mc \
       -import-objc-header uvc_core.h main.swift uvc_core.c -o streamcam-ctl
```

Needs Xcode Command Line Tools only. First camera use triggers a macOS camera
permission prompt for your terminal app.

## How it works (the interesting bits)

The StreamCam is a UVC (USB Video Class) device. On macOS, AVFoundation does not
expose exposure/ISO/white-balance controls for UVC cameras (those APIs are
iOS-only), so this tool sends UVC class requests directly over USB via IOKit
(technique from github.com/fabioferrero90/webcam-settings, MIT).

Logitech's firmware adds nonstandard gating on top of the UVC spec (reverse
engineered — see `research.md`):

| Control | Rule on StreamCam (fw observed 2026-11-15) |
| --- | --- |
| AE mode (CT 0x02) | only **manual (1)** and **Logitech-auto (8)** can be SET; plain auto (2) and shutter-priority (4) stall |
| Exposure time (CT 0x04) | SET only accepted while AE mode = manual |
| Gain (PU 0x04) | SET always works |
| WB temperature (PU 0x0A) | SET only accepted while WB-auto = off |
| WB temperature auto (PU 0x0B) | SET always works |

So the tool implements *shutter priority in software*: camera in manual, fixed
shutter, our loop adjusts gain (and only extends the shutter past its cap when
the scene is really dark). That's exactly the behaviour Logi Options+ is missing.

Frame analysis: a 1080p BGRA stream at ~3 samples/s; median luma over a torso ROI
drives brightness, while clipped-highlight detection runs on a tight **face ROI**
only — a bright window behind you is allowed to clip; your face is not.

## Caveats

- **Logi Options+**: only writes camera settings when you touch its UI, but if
  its camera panel is open it can fight the daemon. The daemon re-asserts its
  mode every 60 s (self-heal). For best results, don't tinker in Options+
  while `watch` runs.
- Settings persist on the camera until changed or power-cycled; `watch` applies
  everything it needs on start.
- `lab/` contains the reverse-engineering tools (XU dumper, register poller,
  legacy CLI) used to map the camera's behaviour.

## Ideas / next steps

- launchd agent (or menubar app) to run `watch` at login
- face detection (Vision framework) instead of a fixed face ROI
- WB lock + Kelvin schedule by time of day (both already work: `--wbauto off --wb K`)

## License & credits

MIT — see [LICENSE](LICENSE). The IOKit/UVC access technique in `uvc_core.c` is
adapted from [webcam-settings](https://github.com/fabioferrero90/webcam-settings)
by Fabio Ferrero (MIT) — required license text and attribution in
[THIRD-PARTY-NOTES.md](THIRD-PARTY-NOTES.md).
