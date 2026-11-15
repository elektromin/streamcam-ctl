# Research notes — Logitech StreamCam exposure control on macOS

Date: 2026-11-15 · Machine: Apple Silicon Mac, macOS 27.2.1 · Camera: Logitech StreamCam, USB 046d:0893

## Prior art (what the internet had)

- `fabioferrero90/webcam-settings` (MIT, Oct 2026) — macOS app, direct UVC-over-IOKit
  control + AVFoundation preview. Proved the access technique. Manual GUI only.
- `lfroms/viewfinder` (archived) — menubar UVC picture-settings app (AVFoundation).
- `elskow/ToneSync` — hackintosh-oriented auto color fixer.
- "Webcam Settings Tool" (App Store, paid), "Camera Live", OBS — manual controls only.
- Nobody had built an auto-adapting, highlight-protective exposure controller.
- No StreamCam-specific exposure fixer found on GitHub; Reddit/HN searches blocked/unhelpful.

## macOS platform facts

- `AVCaptureDevice` exposure/ISO/white-balance-gain/bias APIs are
  `API_UNAVAILABLE(macos)` — AVFoundation cannot control UVC exposure on macOS.
  Logi Options+ therefore must use raw UVC/vendor requests.
- Legacy IOKit user client (`kIOUSBDeviceUserClientTypeID` → `IOUSBDeviceInterface`
  → `DeviceRequest` on EP0) still works on macOS 27 for UVC class requests,
  ad-hoc/unsigned binaries, no entitlements.
- `IOUSBDevRequest` actual-transfer-length field is `wLenDone` (not wLenResult).

## StreamCam register map (reverse engineered)

Video Control interface 0; Camera Terminal id=1 (bmControls 0x00022a2e:
AE mode, AE priority, exposure-absolute, focus, zoom, PTZ, privacy);
Processing Unit id=3 (bmControls 0x0000175b: backlight, brightness, gain,
powerline, saturation, gamma, WB temp, WB auto, digital multiplier).
Exposure time unit = 1e-4 s, min 3, max 2047, res 1, def 250.
Gain 0–255. WB temp 2000–7500 K.

### Firmware gating (the part that cost the day)

- `CT_AE_MODE` (0x02): GET_CUR works (reports 8 when in Logitech-auto, 1 in manual).
  SET only accepts **1 (manual)** and **8 (Logitech auto)**. UVC values 2 (auto)
  and 4 (shutter priority) → STALL (0xe000404f), both idle and streaming.
- `CT_EXPOSURE_TIME_ABSOLUTE` (0x04): GET always works. SET **stalls unless the
  camera is in manual mode (1)** — that's why the first SET attempts failed.
- `PU_GAIN` (0x04): SET always works (any mode).
- `PU_WB_TEMP` (0x0A): SET **stalls unless WB-auto (PU 0x0B) = 0**.
- `PU_WB_AUTO` (0x0B): SET always works.
- Streaming is NOT strictly required for the SETs that pass gating (they worked
  idle); opening a stream anyway is harmless and matches reference implementations.

### Extension units found (GET/SET probed)

- XU 14 `2C49D16A-32B8-4485-3EA8-643A152362F2` — 6 controls, 3-byte records, unknown.
- XU 6 `23E49ED0-1178-4F31-AE52-D2FB8A8D3B48` — 14 controls, mostly SET-only or fixed;
  sel 11 (8 B) changes when WB-auto is toggled. Untamed.
- XU 8 `69678EE4-410F-40DB-A850-7420D7D8240E` — mostly fixed values (fw version-ish?).
- XU 9 `1F5D4CA9-DE11-4487-840D-50933C8EC8D1` — 18 controls, most GET+SET.
  sel 2 (1 B, SET-only): poking it re-asserts AE defaults (auto-ish state).
  sel 9 (2 B): constantly-cycling status/heartbeat, correlates with AE adjustments.
  Not needed for our path — standard CT/PU controls turned out sufficient once the
  gating was understood.
- XU 10 `49E40215-F434-47FE-B158-0E885023E51B` — one 0/1 toggle, unexplained.

### Logi Options+ register-diff experiment (poller while user toggled UI)

- Switching auto→manual in Options+ sets `CT.aemode = 1`.
- Shutter slider → `CT.exposure` (1e-4 s units, same scale as UVC).
- ISO slider → `PU.gain` (0–255).
- WB auto off → `PU.wbauto = 0`, temperature slider → `PU.wbtemp` (K).
- I.e. Options+ uses the SAME standard UVC controls — just through the gated state
  machine. No secret vendor exposure register needed.

## Frame pipeline notes

- AVCaptureSession `.high` → BGRA 1920×1080; `videoSettings` BGRA honoured.
- In a dim room the raw frame can be almost black at low gain — the image is
  real, the scene is dark (verify with `frames`/SC_DEBUG byte dumps).
- Histogram median (64 bins) over torso ROI (x 20–80%, y 15–85%) drives
  brightness; face ROI (x 35–65%, y 20–60%) drives clipped-highlight detection
  (luma ≥ 0.985). Bin edge bug to avoid: value 1.0 lands in bin 63; count clip
  with upper-edge semantics (`(i+1)/64 >= threshold`), not lower-edge.

## Environment quirks (agent sandbox)

- The Xirp sandbox denies IOKit user-client opens (kIOReturnNoResources
  0xe00002be on every USB device, even hubs; `system_profiler SPUSBDataType`
  empty). Compile inside, **run UVC tools with `sandbox: off`**.
- Swift compiles need `-module-cache-path /tmp/...` in the sandbox (clang module
  cache in /var/folders is blocked).
- Bing/DDG mostly useless from this network for niche queries; GitHub REST API +
  raw.githubusercontent worked well.
