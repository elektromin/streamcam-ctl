/* uvc_core.c — UVC (USB Video Class) control access on macOS via IOKit.
 * Access technique adapted from fabioferrero90/webcam-settings (MIT).
 * NOTE: must run outside the agent sandbox (IOKit user-client opens are denied there).
 */
#include "uvc_core.h"
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>

typedef struct { UInt8 vcIf, unitId[2]; UInt32 controls[2]; } layout;

static IOUSBDeviceInterface **dev;
static layout lay;
static uvc_info info;

/* find the Video Control interface, camera terminal and processing unit */
static int parse(IOUSBDeviceInterface **d, layout *l) {
    IOUSBConfigurationDescriptorPtr cfg;
    if ((*d)->GetConfigurationDescriptorPtr(d, 0, &cfg)) return 0;
    UInt8 *p = (UInt8 *)cfg, *end = p + USBToHostWord(cfg->wTotalLength);
    int isVC = 0, found = 0;
    *l = (layout){0};
    for (; p + 2 <= end && p[0]; p += p[0]) {
        if (p[1] == 0x04) {
            isVC = p[5] == 0x0E && p[6] == 0x01;
            if (isVC && !found) { l->vcIf = p[2]; found = 1; }
        } else if (isVC && p[1] == 0x24) {
            if (p[2] == 0x02 && (p[4] | p[5] << 8) == 0x0201) { /* ITT_CAMERA */
                l->unitId[UVC_UNIT_CT] = p[3];
                for (int i = 0; i < p[14] && i < 4; i++)
                    l->controls[UVC_UNIT_CT] |= (UInt32)p[15 + i] << (8 * i);
            } else if (p[2] == 0x05) { /* processing unit */
                l->unitId[UVC_UNIT_PU] = p[3];
                for (int i = 0; i < p[7] && i < 4; i++)
                    l->controls[UVC_UNIT_PU] |= (UInt32)p[8 + i] << (8 * i);
            }
        }
    }
    return found;
}

static int scan(uvc_info *list, int max, int open, unsigned want) {
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching(kIOUSBDeviceClassName), &it)) return 0;
    io_service_t svc;
    int n = 0;
    while ((svc = IOIteratorNext(it))) {
        IOCFPlugInInterface **plug;
        SInt32 score;
        IOUSBDeviceInterface **d = NULL;
        if (!IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
                                               kIOCFPlugInInterfaceID, &plug, &score)) {
            (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&d);
            IODestroyPlugInInterface(plug);
        }
        layout l;
        if (d && parse(d, &l)) {
            UInt16 v, p;
            UInt32 loc = 0;
            (*d)->GetDeviceVendor(d, &v);
            (*d)->GetDeviceProduct(d, &p);
            (*d)->GetLocationID(d, &loc);
            uvc_info i = { .vid = v, .pid = p, .location = loc };
            io_name_t nm;
            IORegistryEntryGetName(svc, nm);
            strlcpy(i.name, nm, sizeof i.name);
            if (list && n < max) list[n] = i;
            n++;
            if (open && !dev && (!want || want == loc)) { dev = d; lay = l; info = i; d = NULL; }
        }
        if (d) (*d)->Release(d);
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    return n;
}

int uvc_scan_list(uvc_info *out, int max) { return scan(out, max, 0, 0); }

int uvc_open(unsigned location) {
    if (dev) { (*dev)->Release(dev); dev = NULL; }
    scan(NULL, 0, 1, location);
    return dev != NULL;
}

const char *uvc_name(void) { return info.name; }
int uvc_vid(void) { return info.vid; }
int uvc_pid(void) { return info.pid; }

/* Control transfers need explicit timeouts: a plain DeviceRequest on a wedged
 * camera (USB stall after sleep/wake) can block the caller for minutes at a
 * time, which stalled the whole watch loop (2026-11-16 log gaps). */
#define UVC_REQ_TIMEOUT_MS 2000

static IOReturn req(int unit, int in, int r, int sel, void *buf, int len) {
    if (!dev) return kIOReturnNotOpen;
    IOUSBDevRequestTO q = {
        .bmRequestType = (UInt8)(in ? 0xA1 : 0x21), .bRequest = (UInt8)r,
        .wValue = (UInt16)(sel << 8), .wIndex = (UInt16)((lay.unitId[unit] << 8) | lay.vcIf),
        .wLength = (UInt16)len, .pData = buf,
        .noDataTimeout = UVC_REQ_TIMEOUT_MS, .completionTimeout = UVC_REQ_TIMEOUT_MS };
    return (*dev)->DeviceRequestTO(dev, &q);
}

int uvc_get(int unit, int sel, int r, int len, long *out) {
    UInt8 b[4] = {0};
    IOReturn e = req(unit, 1, r, sel, b, len);
    if (e) return (int)e;
    if (len == 1) *out = b[0];
    else if (len == 2) *out = (SInt16)(b[0] | b[1] << 8);
    else *out = (SInt32)(b[0] | b[1] << 8 | b[2] << 16 | (UInt32)b[3] << 24);
    return 0;
}

int uvc_set(int unit, int sel, int len, long val) {
    UInt8 b[4] = { (UInt8)(val & 0xFF), (UInt8)(val >> 8 & 0xFF),
                   (UInt8)(val >> 16 & 0xFF), (UInt8)(val >> 24 & 0xFF) };
    return (int)req(unit, 0, 0x01, sel, b, len);
}

int uvc_getinfo(int unit, int sel, unsigned char *out) {
    return (int)req(unit, 1, 0x86, sel, out, 1);
}

void uvc_layout(int *vcIf, int *ctId, unsigned *ctBm, int *puId, unsigned *puBm) {
    *vcIf = lay.vcIf; *ctId = lay.unitId[0]; *ctBm = lay.controls[0];
    *puId = lay.unitId[1]; *puBm = lay.controls[1];
}

int uvc_ctl_supported(int unit, int sel) {
    if (unit < 0 || unit > 1 || !lay.unitId[unit]) return 0;
    return (lay.controls[unit] >> (sel - 1)) & 1;
}

int uvc_list_text(char *out, int cap) {
    uvc_info list[16];
    int n = scan(list, 16, 0, 0);
    int used = 0;
    for (int i = 0; i < n; i++) {
        int w = snprintf(out + used, cap - used, "%s\t0x%04x\t0x%04x\t0x%08x\n",
                         list[i].name, list[i].vid, list[i].pid, list[i].location);
        if (w < 0 || used + w >= cap) break;
        used += w;
    }
    return n;
}
