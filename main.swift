// main.swift — streamcam-ctl v2
// Controls UVC exposure/WB of the Logitech StreamCam on macOS.
// - `apply` opens its own capture stream (the camera only accepts SET_CUR while streaming)
//   and applies settings.
// - `watch` runs a highlight-protective auto-exposure loop ("shutter priority":
//   fixed shutter, camera auto-gain; or full manual with software gain/shutter control).
// UVC core in uvc_core.c (technique from fabioferrero90/webcam-settings, MIT).

import AVFoundation
import CoreMedia
import Foundation

// ---- UVC constants ---------------------------------------------------------
let UNIT_CT: Int32 = 0, UNIT_PU: Int32 = 1
let GET_CUR: Int32 = 0x81, GET_MIN: Int32 = 0x82, GET_MAX: Int32 = 0x83, GET_RES: Int32 = 0x84, GET_DEF: Int32 = 0x87
let CT_AE_MODE: Int32 = 0x02, CT_AE_PRIORITY: Int32 = 0x03, CT_EXPOSURE_ABS: Int32 = 0x04
let PU_BACKLIGHT: Int32 = 0x01, PU_BRIGHTNESS: Int32 = 0x02, PU_CONTRAST: Int32 = 0x03, PU_GAIN: Int32 = 0x04
let PU_POWERLINE: Int32 = 0x05, PU_SATURATION: Int32 = 0x07, PU_SHARPNESS: Int32 = 0x08, PU_GAMMA: Int32 = 0x09
let PU_WB_TEMP: Int32 = 0x0A, PU_WB_AUTO: Int32 = 0x0B

struct Ctl: Hashable {
    let unit: Int32, sel: Int32, len: Int32, label: String
    init(_ unit: Int32, _ sel: Int32, _ len: Int32, _ label: String) {
        self.unit = unit; self.sel = sel; self.len = len; self.label = label
    }
}

let TABLE: [Ctl] = [
    Ctl(UNIT_CT, CT_AE_MODE, 2, "AE mode"),
    Ctl(UNIT_CT, CT_AE_PRIORITY, 1, "AE priority (0=frame-rate 1=exposure)"),
    Ctl(UNIT_CT, CT_EXPOSURE_ABS, 4, "Exposure time absolute (1e-4 s)"),
    Ctl(UNIT_PU, PU_BACKLIGHT, 2, "Backlight compensation"),
    Ctl(UNIT_PU, PU_BRIGHTNESS, 2, "Brightness"),
    Ctl(UNIT_PU, PU_CONTRAST, 2, "Contrast"),
    Ctl(UNIT_PU, PU_GAIN, 2, "Gain"),
    Ctl(UNIT_PU, PU_POWERLINE, 1, "Power line frequency"),
    Ctl(UNIT_PU, PU_SATURATION, 2, "Saturation"),
    Ctl(UNIT_PU, PU_SHARPNESS, 2, "Sharpness"),
    Ctl(UNIT_PU, PU_GAMMA, 2, "Gamma"),
    Ctl(UNIT_PU, PU_WB_TEMP, 2, "WB temperature (K)"),
    Ctl(UNIT_PU, PU_WB_AUTO, 1, "WB temperature auto"),
]

func uvcGet(_ c: Ctl, _ req: Int32) -> Int? {
    var v: Int = 0
    return uvc_get(c.unit, c.sel, req, c.len, &v) == 0 ? v : nil
}

func aeName(_ v: Int) -> String {
    switch v {
    case 1: return "manual"
    case 2: return "auto"
    case 4: return "shutter-priority"
    case 8: return "aperture-priority"
    default: return "0x\(String(v, radix: 16))"
    }
}

func fmtShutter(_ units: Int) -> String {
    guard units > 0 else { return "?" }
    return String(format: "%.4fs (~1/%.0f)", Double(units) / 10000.0, 10000.0 / Double(units))
}

func parseShutter(_ s: String) -> Int? { // returns 1e-4 s units
    if s.contains("/") {
        let p = s.split(separator: "/")
        guard p.count == 2, let n = Int(p[0]), let d = Int(p[1]), n > 0, d > 0 else { return nil }
        return Int((10000.0 * Double(n) / Double(d)).rounded())
    }
    if s.hasSuffix("u"), let v = Int(s.dropLast()) { return v }
    guard let secs = Double(s), secs > 0 else { return nil }
    return Int((secs * 10000).rounded())
}

// ---- UVC list (C core builds text; Swift avoids C fixed-array structs) -----
func listCameras() {
    var buf = [CChar](repeating: 0, count: 8192)
    let n = uvc_list_text(&buf, 8192)
    print("\(n) UVC camera(s):")
    print(String(cString: buf))
}

func openCamera() -> Bool {
    guard uvc_open(0) != 0 else {
        fputs("could not open a UVC camera (run outside the sandbox?)\n", stderr)
        return false
    }
    return true
}

// ---- AVFoundation stream ----------------------------------------------------
struct Metrics { let median: Double, clip: Double, near: Double }
final class FrameSink: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    var frameCount = 0
    var latest: Metrics?
    let gotFirst = DispatchSemaphore(value: 0)
    var everyN = 10

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        frameCount += 1
        let isSample = frameCount % everyN == 0
        if frameCount == 1, let pb0 = CMSampleBufferGetImageBuffer(sampleBuffer), getenv("SC_DEBUG") != nil {
            let fcc = CVPixelBufferGetPixelFormatType(pb0)
            let s = String(format: "%c%c%c%c", (fcc >> 24) & 0xff, (fcc >> 16) & 0xff, (fcc >> 8) & 0xff, fcc & 0xff)
            var hex = ""
            CVPixelBufferLockBaseAddress(pb0, .readOnly)
            if let base = CVPixelBufferGetBaseAddress(pb0) {
                let p = base.assumingMemoryBound(to: UInt8.self)
                for k in 0..<48 { hex += String(format: "%02x", p[k]) }
            }
            CVPixelBufferUnlockBaseAddress(pb0, .readOnly)
            print("[dbg] first frame: fmt=\(s) (0x\(String(fcc, radix: 16))) \(CVPixelBufferGetWidth(pb0))x\(CVPixelBufferGetHeight(pb0)) bpr=\(CVPixelBufferGetBytesPerRow(pb0)) planes=\(CVPixelBufferGetPlaneCount(pb0)) bytes[0..48]=\(hex)")
        }
        guard isSample, let pb = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        gotFirst.signal()
        latest = Self.measure(pb)
        if getenv("SC_DEBUG") != nil {
            var hex = ""
            CVPixelBufferLockBaseAddress(pb, .readOnly)
            if let base = CVPixelBufferGetBaseAddress(pb) {
                let p = base.assumingMemoryBound(to: UInt8.self)
                for k in 0..<24 { hex += String(format: "%02x", p[k]) }
                let mid = (CVPixelBufferGetHeight(pb) / 2) * CVPixelBufferGetBytesPerRow(pb) + (CVPixelBufferGetWidth(pb) / 2) * 4
                var midhex = ""
                for k in 0..<8 { midhex += String(format: "%02x", p[mid + k]) }
                hex += " mid=" + midhex
            }
            CVPixelBufferUnlockBaseAddress(pb, .readOnly)
            let m = latest!
            print("[sample#\(frameCount)] med=\(String(format: "%.3f", m.median)) clip=\(String(format: "%.3f", m.clip)) bytes=\(hex)")
        }
    }

    // cheap histogram over strided pixels; median/near over a torso ROI,
    // clip over a tight face ROI (background windows/monitors are allowed to clip)
    static func measure(_ pb: CVPixelBuffer) -> Metrics {
        CVPixelBufferLockBaseAddress(pb, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(pb) else { return Metrics(median: 0, clip: 0, near: 0) }
        let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb)
        let bpr = CVPixelBufferGetBytesPerRow(pb)
        let x0 = Int(Double(w) * 0.2), x1 = Int(Double(w) * 0.8)
        let y0 = Int(Double(h) * 0.15), y1 = Int(Double(h) * 0.85)
        let fx0 = Int(Double(w) * 0.35), fx1 = Int(Double(w) * 0.65)
        let fy0 = Int(Double(h) * 0.2), fy1 = Int(Double(h) * 0.6)
        var hist = [Int](repeating: 0, count: 64)
        var total = 0, faceClip = 0, faceTotal = 0
        let ptr = base.assumingMemoryBound(to: UInt8.self)
        var y = y0
        while y < y1 {
            var x = x0
            let row = y * bpr
            while x < x1 {
                let o = row + x * 4 // BGRA
                let b = Double(ptr[o]), g = Double(ptr[o + 1]), r = Double(ptr[o + 2])
                let luma = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0
                hist[min(63, Int(luma * 64))] += 1
                total += 1
                if x >= fx0 && x < fx1 && y >= fy0 && y < fy1 {
                    faceTotal += 1
                    if luma >= 0.985 { faceClip += 1 } // blown pixel on the face
                }
                x += 8
            }
            y += 8
        }
        guard total > 0 else { return Metrics(median: 0, clip: 0, near: 0) }
        var acc = 0, medBin = 0
        for i in 0..<64 { acc += hist[i]; if acc * 2 >= total { medBin = i; break } }
        var near = 0
        for i in 0..<64 where Double(i + 1) / 64.0 >= 0.93 { near += hist[i] }
        return Metrics(median: Double(medBin) / 64.0,
                       clip: Double(faceClip) / Double(max(faceTotal, 1)),
                       near: Double(near) / Double(total))
    }
}

func cameraPermission() -> Bool {
    switch AVCaptureDevice.authorizationStatus(for: .video) {
    case .authorized: return true
    case .notDetermined:
        let sem = DispatchSemaphore(value: 0)
        var ok = false
        AVCaptureDevice.requestAccess(for: .video) { ok = $0; sem.signal() }
        sem.wait()
        return ok
    default: return false
    }
}

func findStreamCam() -> AVCaptureDevice? {
    var devs: [AVCaptureDevice] = []
    if #available(macOS 14.0, *) {
        devs += AVCaptureDevice.DiscoverySession(deviceTypes: [.external], mediaType: .video, position: .unspecified).devices
    }
    devs += AVCaptureDevice.DiscoverySession(deviceTypes: [.externalUnknown], mediaType: .video, position: .unspecified).devices
    return devs.first { $0.localizedName.contains("StreamCam") } ?? devs.first
}

final class CameraStream {
    let session = AVCaptureSession()
    let sink = FrameSink()
    let queue = DispatchQueue(label: "camframes")
    var started = false

    func start() -> Bool {
        guard cameraPermission() else {
            fputs("no camera permission. Grant it to your terminal app (System Settings → Privacy & Security → Camera), then retry.\n", stderr)
            return false
        }
        guard let dev = findStreamCam() else {
            fputs("AVFoundation sees no external camera\n", stderr)
            return false
        }
        do {
            let input = try AVCaptureDeviceInput(device: dev)
            if getenv("SC_DEBUG") != nil {
                for f in dev.formats {
                    let d = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
                    let ranges = f.videoSupportedFrameRateRanges.map { String(format: "%.0f", $0.maxFrameRate) }.joined(separator: ",")
                    print("[fmt] \(d.width)x\(d.height) fps=[\(ranges)]")
                }
            }
            session.beginConfiguration()
            session.sessionPreset = .high
            guard session.canAddInput(input) else { throw NSError(domain: "cam", code: 1) }
            session.addInput(input)
            let out = AVCaptureVideoDataOutput()
            out.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]
            out.alwaysDiscardsLateVideoFrames = true
            out.setSampleBufferDelegate(sink, queue: queue)
            guard session.canAddOutput(out) else { throw NSError(domain: "cam", code: 2) }
            session.addOutput(out)
            session.commitConfiguration()
            session.startRunning()
            started = true
            if sink.gotFirst.wait(timeout: .now() + 5) == .timedOut {
                fputs("no frames arrived within 5 s\n", stderr)
                return false
            }
            Thread.sleep(forTimeInterval: 0.8) // let AE settle a bit
            return true
        } catch {
            fputs("could not start capture session: \(error)\n", stderr)
            return false
        }
    }

    func stop() {
        if started { session.stopRunning(); started = false }
    }
}

// ---- UVC apply helpers -------------------------------------------------------
func clampToRange(_ c: Ctl, _ v0: Int) -> Int {
    var v = v0
    if let mn = uvcGet(c, GET_MIN), v < mn { v = mn }
    if let mx = uvcGet(c, GET_MAX), v > mx { v = mx }
    if let res = uvcGet(c, GET_RES), res > 1, let mn = uvcGet(c, GET_MIN) { v -= (v - mn) % res }
    return v
}

@discardableResult
func uvcApply(_ c: Ctl, _ v0: Int, quiet: Bool = false) -> Bool {
    let v = clampToRange(c, v0)
    let before = uvcGet(c, GET_CUR)
    let e = uvc_set(c.unit, c.sel, c.len, v)
    if e != 0 {
        print("✗ \(c.label): SET 0x\(String(e, radix: 16))" + (Int(e) == 0xe000404f ? " (stalled — camera rejected; needs an active stream?)" : ""))
        return false
    }
    if let after = uvcGet(c, GET_CUR) {
        let ok = after == v
        if !quiet || !ok {
            print("\(ok ? "✓" : "⚠") \(c.label): \(before.map(String.init) ?? "?") → \(after)" + (ok ? "" : " (wanted \(v))"))
        }
        return ok
    }
    return true
}

func aeModeSet(_ v: Int) -> Bool {
    uvcApply(Ctl(UNIT_CT, CT_AE_MODE, 2, "AE mode"), v)
}

// ---- commands -----------------------------------------------------------------
func cmdProbe(withStream: Bool) {
    var vcIf: Int32 = 0, ctId: Int32 = 0, puId: Int32 = 0
    var ctBm: UInt32 = 0, puBm: UInt32 = 0
    uvc_layout(&vcIf, &ctId, &ctBm, &puId, &puBm)
    print("VC interface=\(vcIf)  CT id=\(ctId) (bm=0x\(String(ctBm, radix: 16)))  PU id=\(puId) (bm=0x\(String(puBm, radix: 16)))")
    var cam: CameraStream?
    if withStream {
        let c = CameraStream()
        guard c.start() else { exit(1) }
        cam = c
    }
    defer { cam?.stop() }
    for c in TABLE {
        guard uvc_ctl_supported(c.unit, c.sel) != 0 else { print("  \(c.label.padding(toLength: 44, withPad: " ", startingAt: 0)): (not supported)"); continue }
        var info: UInt8 = 0
        let ie = uvc_getinfo(c.unit, c.sel, &info)
        let cur = uvcGet(c, GET_CUR), mn = uvcGet(c, GET_MIN), mx = uvcGet(c, GET_MAX)
        var extra = ie == 0 ? " info=0x\(String(info, radix: 16))" : ""
        if c.unit == UNIT_CT && c.sel == CT_AE_MODE {
            if let m = mx { extra += "  [modes:\(m & 1 == 1 ? " manual" : "")\(m & 2 == 2 ? " auto" : "")\(m & 4 == 4 ? " shutter-prio" : "")\(m & 8 == 8 ? " aperture-prio" : "")]" }
            if let v = cur { extra += "  [current: \(aeName(v))]" }
        }
        if c.unit == UNIT_CT && c.sel == CT_EXPOSURE_ABS, let v = cur { extra += "  [\(fmtShutter(v))]" }
        let failed = (cur == nil && mn == nil && mx == nil)
        print("  \(c.label.padding(toLength: 44, withPad: " ", startingAt: 0)): \(failed ? "(get failed — try --stream)" : "min=\(mn ?? -1) max=\(mx ?? -1) cur=\(cur ?? -1)")\(extra)")
    }
}

func cmdStatus(withStream: Bool) {
    var cam: CameraStream?
    if withStream {
        let c = CameraStream()
        guard c.start() else { exit(1) }
        cam = c
        currentSink = c.sink
    }
    defer { cam?.stop() }
    for c in TABLE where uvc_ctl_supported(c.unit, c.sel) != 0 {
        var line = "  \(c.label.padding(toLength: 44, withPad: " ", startingAt: 0)): "
        if let v = uvcGet(c, GET_CUR) {
            line += "\(v)"
            if c.sel == CT_AE_MODE { line += "  [\(aeName(v))]" }
            if c.sel == CT_EXPOSURE_ABS { line += "  [\(fmtShutter(v))]" }
        } else { line += "(get failed — try --stream)" }
        print(line)
    }
    if let sink = currentSink {
        print("stream metrics: median=\(String(format: "%.2f", sink.latest?.median ?? -1)) clip=\(String(format: "%.3f", sink.latest?.clip ?? -1)) near=\(String(format: "%.3f", sink.latest?.near ?? -1))")
    }
}

var currentSink: FrameSink?

struct Options {
    var aeMode = 1          // manual (Logitech auto = 8; plain auto/shutter-prio are rejected by firmware)
    var shutter: Int? = nil // units of 1e-4 s
    var gain: Int? = nil
    var wbAuto: Int? = nil
    var wbTemp: Int? = nil
    var backlight: Int? = nil
    var aePriority: Int? = nil
    var powerline: Int? = nil
    var extra: [Ctl: Int] = [:]
}

func parseOptions(_ args: ArraySlice<String>) -> Options {
    var o = Options()
    var it = args.makeIterator()
    while let a = it.next() {
        func val() -> String? { it.next() }
        switch a {
        case "--aemode": let v = val(); o.aeMode = ["manual": 1, "auto": 2, "shutter": 4, "aperture": 8, "logi": 8][v ?? ""] ?? 1
        case "--shutter": o.shutter = val().flatMap(parseShutter)
        case "--gain": o.gain = val().flatMap { Int($0) }
        case "--wbauto": o.wbAuto = val() == "off" ? 0 : 1
        case "--wb": o.wbTemp = val().flatMap { Int($0) }
        case "--backlight": o.backlight = val().flatMap { Int($0) }
        case "--aepriority": o.aePriority = val().flatMap { Int($0) }
        case "--powerline": o.powerline = val().flatMap { Int($0) }
        case "--brightness": o.extra[Ctl(UNIT_PU, PU_BRIGHTNESS, 2, "Brightness")] = val().flatMap { Int($0) }
        case "--contrast": o.extra[Ctl(UNIT_PU, PU_CONTRAST, 2, "Contrast")] = val().flatMap { Int($0) }
        case "--saturation": o.extra[Ctl(UNIT_PU, PU_SATURATION, 2, "Saturation")] = val().flatMap { Int($0) }
        case "--sharpness": o.extra[Ctl(UNIT_PU, PU_SHARPNESS, 2, "Sharpness")] = val().flatMap { Int($0) }
        case "--gamma": o.extra[Ctl(UNIT_PU, PU_GAMMA, 2, "Gamma")] = val().flatMap { Int($0) }
        default: break
        }
    }
    return o
}

func applyOptions(_ o: Options) {
    _ = aeModeSet(o.aeMode)
    if let s = o.shutter { _ = uvcApply(Ctl(UNIT_CT, CT_EXPOSURE_ABS, 4, "Exposure time"), s) }
    if let g = o.gain { _ = uvcApply(Ctl(UNIT_PU, PU_GAIN, 2, "Gain"), g) }
    if let w = o.wbAuto { _ = uvcApply(Ctl(UNIT_PU, PU_WB_AUTO, 1, "WB auto"), w) }
    if let k = o.wbTemp { _ = uvcApply(Ctl(UNIT_PU, PU_WB_TEMP, 2, "WB temperature"), k) }
    if let b = o.backlight { _ = uvcApply(Ctl(UNIT_PU, PU_BACKLIGHT, 2, "Backlight compensation"), b) }
    if let p = o.aePriority { _ = uvcApply(Ctl(UNIT_CT, CT_AE_PRIORITY, 1, "AE priority"), p) }
    if let f = o.powerline { _ = uvcApply(Ctl(UNIT_PU, PU_POWERLINE, 1, "Power line frequency"), f) }
    for (c, v) in o.extra { _ = uvcApply(c, v) }
}

func cmdApply(_ rest: ArraySlice<String>) {
    let o = parseOptions(rest)
    guard openCamera() else { exit(1) }
    let cam = CameraStream()
    guard cam.start() else { exit(1) }
    applyOptions(o)
    cam.stop()
}

// ---- watch: highlight-protective auto exposure ---------------------------------
func cmdWatch(_ rest: ArraySlice<String>) {
    var mode = "manual"
    var startShutter = 167            // 1/60
    var shutterCap = 333              // 1/30 — longest exposure we allow
    var gainCap = 248
    var target = 0.5
    var duration = 0.0
    var verbose = false
    var wbAuto = 1
    var healInterval = 60.0
    var startGain = 100

    var it = rest.makeIterator()
    while let a = it.next() {
        func val() -> String? { it.next() }
        switch a {
        case "--mode": mode = val() ?? mode
        case "--shutter": startShutter = val().flatMap(parseShutter) ?? startShutter
        case "--shutter-cap": shutterCap = val().flatMap(parseShutter) ?? shutterCap
        case "--gain-cap": gainCap = val().flatMap { Int($0) } ?? gainCap
        case "--start-gain": startGain = val().flatMap { Int($0) } ?? startGain
        case "--target": target = val().flatMap { Double($0) } ?? target
        case "--duration": duration = val().flatMap { Double($0) } ?? duration
        case "--wbauto": wbAuto = val() == "off" ? 0 : 1
        case "--heal-interval": healInterval = val().flatMap { Double($0) } ?? healInterval
        case "--verbose": verbose = true
        default: break
        }
    }

    guard openCamera() else { exit(1) }
    let cam = CameraStream()
    guard cam.start() else { exit(1) }
    currentSink = cam.sink

    let exposureCtl = Ctl(UNIT_CT, CT_EXPOSURE_ABS, 4, "Exposure time")
    let gainCtl = Ctl(UNIT_PU, PU_GAIN, 2, "Gain")
    let camMin = uvcGet(exposureCtl, GET_MIN) ?? 3
    let camMax = uvcGet(exposureCtl, GET_MAX) ?? 2047
    let gainMax = clampToRange(gainCtl, gainCap)

    var shutter = min(max(startShutter, camMin), camMax)
    var gain = startGain
    var lastChange = Date.distantPast
    var lastHeal = Date()
    let start = Date()

    print("watch: mode=\(mode) target=\(target) shutter=[\(camMin)..\(min(shutterCap, camMax))] gainCap=\(gainMax) — Ctrl+C to stop")

    // initial configuration
    if mode == "shutterpri" {
        if !aeModeSet(4) {
            print("AE-mode SET rejected even with a stream — camera may not support shutter priority. Falling back to --mode manual.")
            mode = "manual"
        } else {
            _ = uvcApply(exposureCtl, shutter)
        }
    }
    if mode == "manual" {
        _ = aeModeSet(1)
        _ = uvcApply(exposureCtl, shutter)
        _ = uvcApply(gainCtl, gain)
    }
    _ = uvcApply(Ctl(UNIT_PU, PU_WB_AUTO, 1, "WB auto"), wbAuto, quiet: true)

    var lastSample = Date.distantPast
    var tick = 0
    var lastMsg = ""
    var lastClipReact = Date.distantPast

    while true {
        if duration > 0, Date().timeIntervalSince(start) > duration {
            print("duration reached — final status:")
            cmdStatus(withStream: false)
            cam.stop()
            exit(0)
        }
        if Date().timeIntervalSince(lastHeal) > healInterval { // self-heal vs Logi Options+
            lastHeal = Date()
            _ = aeModeSet(1)
            _ = uvcApply(Ctl(UNIT_PU, PU_WB_AUTO, 1, "WB auto"), wbAuto, quiet: true)
        }
        if Date().timeIntervalSince(lastSample) < 0.4 { usleep(20000); continue }
        lastSample = Date()
        tick += 1
        guard let m = cam.sink.latest else { usleep(50000); continue }

        let faceClip = m.clip
        let clipped = faceClip > 0.008 || m.near > 0.10
        let tooBright = m.median > target + 0.18
        let tooDark = m.median < target - 0.20
        let wayTooDark = m.median < target - 0.35
        guard clipped || tooBright || tooDark || verbose else { continue }
        let recentlyClipped = Date().timeIntervalSince(lastClipReact) < 6
        let cooldown = clipped ? 0.8 : (wayTooDark ? 0.8 : recentlyClipped ? 2.2 : 1.5)
        if Date().timeIntervalSince(lastChange) < cooldown { continue }

        var changed = ""
        if clipped {
            if gain > 0 {
                let step = faceClip > 0.05 ? 24 : faceClip > 0.015 ? 12 : 5
                gain = max(0, gain - step)
                changed = faceClip > 0.05 ? "CLIP→gain↓↓" : "clip→gain↓"
                lastClipReact = Date()
            } else {
                let ns = max(camMin, Int(Double(shutter) * 0.85))
                if ns != shutter { shutter = ns; changed = "darker" }
                lastClipReact = Date()
            }
        } else if tooBright {
            if gain > 0 { gain = max(0, gain - 12); changed = "gain↓" }
            else {
                let ns = max(camMin, Int(Double(shutter) * 0.85))
                if ns != shutter { shutter = ns; changed = "darker" }
            }
        } else if wayTooDark {
            if gain < gainMax { gain = min(gainMax, gain + 24); changed = "dark→gain↑↑" }
            else {
                let ns = min(min(shutterCap, camMax), Int(Double(shutter) * 1.18))
                if ns != shutter { shutter = ns; changed = "brighter" }
                else { changed = "at limits (still dark)" }
            }
        } else if tooDark {
            if gain < gainMax {
                gain = min(gainMax, gain + (recentlyClipped ? 4 : 8))
                changed = recentlyClipped ? "gain↑ gentle" : "gain↑"
            } else {
                let ns = min(min(shutterCap, camMax), Int(Double(shutter) * 1.18))
                if ns != shutter { shutter = ns; changed = "brighter" }
                else { changed = "at limits (still dark)" }
            }
        }
        if !changed.isEmpty {
            let msg = String(format: "[%@] med=%.2f clip=%.1f%% near=%.1f%% → %@ (shutter %@, gain %d)",
                             DateFormatter.localizedString(from: Date(), dateStyle: .none, timeStyle: .medium),
                             m.median, m.clip * 100, m.near * 100, changed, fmtShutter(shutter), gain)
            if !(changed.hasPrefix("at limits") && lastMsg == msg) && !(changed.hasPrefix("at limits") && tick % 20 != 0) {
                print(msg)
            }
            lastMsg = msg
            lastChange = Date()
            _ = uvcApply(exposureCtl, shutter, quiet: true)
            _ = uvcApply(gainCtl, gain, quiet: true)
        } else if verbose, tick % 25 == 0 {
            print(String(format: "[%@] ok med=%.2f clip=%.1f%% near=%.1f%% (shutter %@, gain %d)",
                         DateFormatter.localizedString(from: Date(), dateStyle: .none, timeStyle: .medium),
                         m.median, m.clip * 100, m.near * 100, fmtShutter(shutter), gain))
        }
        usleep(100000)
    }
}

// ---- main ----------------------------------------------------------------------
setbuf(stdout, nil) // unbuffered stdout so `watch | tee log` streams live
let args = Array(CommandLine.arguments)
var idx = 1
var withStream = false
if idx < args.count, args[idx] == "--stream" { withStream = true; idx += 1 }

guard idx < args.count else {
    print("usage: streamcam-ctl [--stream] <list|probe|status|apply [opts]|watch [opts]>\n" +
          "  apply opts: --aemode manual|auto|shutter|aperture --shutter 1/60 --gain N --wbauto on|off --wb K --backlight N --aepriority N --powerline N\n" +
          "  watch opts: --mode shutterpri|manual --shutter 1/60 --shutter-cap 1/30 --gain-cap 180 --target 0.5 --duration SEC --verbose")
    exit(2)
}

let cmd = args[idx]
switch cmd {
case "list": listCameras()
case "probe": guard openCamera() else { exit(1) }; cmdProbe(withStream: withStream)
case "status": guard openCamera() else { exit(1) }; cmdStatus(withStream: withStream)
case "apply": cmdApply(args[(idx + 1)...])
case "watch": cmdWatch(args[(idx + 1)...])
case "hold": // open a stream and keep it running N seconds (for concurrent experiments)
    guard openCamera() else { exit(1) }
    let cam = CameraStream()
    guard cam.start() else { exit(1) }
    let secs = idx + 1 < args.count ? (Double(args[idx + 1]) ?? 30) : 30
    print("holding stream for \(secs)s...")
    Thread.sleep(forTimeInterval: secs)
    cam.stop()
case "frames": // passively print stream metrics every second (debug)
    guard openCamera() else { exit(1) }
    let cam = CameraStream()
    guard cam.start() else { exit(1) }
    currentSink = cam.sink
    let secs = idx + 1 < args.count ? (Double(args[idx + 1]) ?? 10) : 10
    for i in 0..<Int(secs) {
        Thread.sleep(forTimeInterval: 1)
        if let m = cam.sink.latest {
            print(String(format: "t=%ds med=%.3f clip=%.3f near=%.3f", i, m.median, m.clip, m.near))
        } else { print("t=\(i)s no metrics yet") }
    }
    cam.stop()
default: print("unknown command \(cmd)"); exit(2)
}
