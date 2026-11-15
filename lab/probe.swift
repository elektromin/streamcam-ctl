// probe.swift — what macOS AVFoundation exposes for our cameras.
// (Mac note: exposure duration/ISO/WB-gain APIs are iOS-only, so this just
// documents modes, formats and frame rates.)
import AVFoundation
import CoreMedia

func fmtTime(_ d: CMTime) -> String {
    let s = CMTimeGetSeconds(d)
    guard s.isFinite, s > 0 else { return "\(s)" }
    return String(format: "%.4fs (1/%.0f)", s, 1.0 / s)
}

print("camera authorizationStatus: \(AVCaptureDevice.authorizationStatus(for: .video).rawValue)  (0=notDetermined 1=restricted 2=denied 3=authorized)")

var devices: [AVCaptureDevice] = []
if #available(macOS 14.0, *) {
    devices += AVCaptureDevice.DiscoverySession(deviceTypes: [.external], mediaType: .video, position: .unspecified).devices
} else {
    devices += AVCaptureDevice.DiscoverySession(deviceTypes: [.externalUnknown], mediaType: .video, position: .unspecified).devices
}

var seen = Set<String>()
for d in devices where !seen.contains(d.uniqueID) {
    seen.insert(d.uniqueID)
    print("======================================================")
    print("name:        \(d.localizedName)")
    print("uniqueID:    \(d.uniqueID)")
    print("modelID:     \(d.modelID)")
    print("connected:   \(d.isConnected)")
    let f = d.activeFormat
    let dims = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
    print("activeFormat: \(dims.width)x\(dims.height)")
    for r in f.videoSupportedFrameRateRanges.prefix(8) {
        print("   fps range: \(r.minFrameRate)–\(r.maxFrameRate)  minDur=\(fmtTime(r.minFrameDuration)) maxDur=\(fmtTime(r.maxFrameDuration))")
    }
    print("formats available: \(d.formats.count)")
    print("exposure modes supported: continuous=\(d.isExposureModeSupported(.continuousAutoExposure)) auto=\(d.isExposureModeSupported(.autoExpose)) custom=\(d.isExposureModeSupported(.custom)) locked=\(d.isExposureModeSupported(.locked))")
    print("white balance modes supported: continuous=\(d.isWhiteBalanceModeSupported(.continuousAutoWhiteBalance)) auto=\(d.isWhiteBalanceModeSupported(.autoWhiteBalance)) locked=\(d.isWhiteBalanceModeSupported(.locked))")
    print("focus modes supported: continuous=\(d.isFocusModeSupported(.continuousAutoFocus)) auto=\(d.isFocusModeSupported(.autoFocus)) locked=\(d.isFocusModeSupported(.locked))")
}
if seen.isEmpty { print("!! no video capture devices discovered") }
