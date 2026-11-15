/* xu-dump.c — enumerate UVC Extension Units (vendor controls) of the webcam
 * and probe each XU selector with GET_INFO/GET_CUR/GET_MIN/GET_MAX. */
#include <stdio.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>

typedef struct { UInt8 id; unsigned char guid[16]; UInt8 nctl; UInt32 bm; } xu_t;
static xu_t xus[8];
static int nxu;
static IOUSBDeviceInterface **dev;

static void guidstr(const unsigned char *g, char *out) { /* mixed-endian GUID */
    sprintf(out, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
            g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
            g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static int parse(IOUSBDeviceInterface **d) {
    IOUSBConfigurationDescriptorPtr cfg;
    if ((*d)->GetConfigurationDescriptorPtr(d, 0, &cfg)) return 0;
    UInt8 *p = (UInt8 *)cfg, *end = p + USBToHostWord(cfg->wTotalLength);
    int isVC = 0;
    for (; p + 2 <= end && p[0]; p += p[0]) {
        if (p[1] == 0x04) isVC = p[5] == 0x0E && p[6] == 0x01;
        else if (isVC && p[1] == 0x24 && p[2] == 0x06 && nxu < 8) { /* VC_EXTENSION_UNIT */
            xu_t *x = &xus[nxu++];
            x->id = p[3];
            memcpy(x->guid, p + 4, 16);
            int bNrInPins = p[21];
            int csOff = 22 + bNrInPins;
            UInt8 csz = p[csOff];
            x->nctl = p[20];
            for (int i = 0; i < csz && i < 4; i++) x->bm |= (UInt32)p[csOff + 1 + i] << (8 * i);
        }
    }
    return 1;
}

static IOReturn req(int unit, int in, int r, int sel, void *buf, int len, int *actual) {
    IOUSBDevRequest q = {
        .bmRequestType = (UInt8)(in ? 0xA1 : 0x21), .bRequest = (UInt8)r,
        .wValue = (UInt16)(sel << 8), .wIndex = (UInt16)(unit << 8),
        .wLength = (UInt16)len, .pData = buf };
    IOReturn e = (*dev)->DeviceRequest(dev, &q);
    if (actual) *actual = q.wLenDone;
    return e;
}

int main(void) {
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching(kIOUSBDeviceClassName), &it)) return 1;
    io_service_t svc;
    while ((svc = IOIteratorNext(it))) {
        IOCFPlugInInterface **plug; SInt32 score; IOUSBDeviceInterface **d = NULL;
        if (!IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score)) {
            (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&d);
            IODestroyPlugInInterface(plug);
        }
        UInt16 v = 0, pidd = 0;
        if (d) { (*d)->GetDeviceVendor(d, &v); (*d)->GetDeviceProduct(d, &pidd); }
        if (d && v == 0x046d && parse(d)) {
            UInt32 loc = 0; (*d)->GetLocationID(d, &loc);
            if (!dev) { dev = d; printf("opened %04x:%04x @0x%x\n", v, pidd, loc); d = NULL; }
        }
        if (d) (*d)->Release(d);
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    if (!dev) { printf("no logitech cam\n"); return 1; }

    for (int i = 0; i < nxu; i++) {
        char gs[64]; guidstr(xus[i].guid, gs);
        printf("\nXU unit=%d  guid=%s  nctl=%d bm=0x%08x\n", xus[i].id, gs, xus[i].nctl, xus[i].bm);
        for (int sel = 1; sel <= 32; sel++) {
            if (!((xus[i].bm >> (sel - 1)) & 1)) continue;
            unsigned char info = 0;
            int e = (int)req(xus[i].id, 1, 0x86, sel, &info, 1, NULL);
            if (e) { printf("  sel %2d: GET_INFO failed 0x%x\n", sel, e); continue; }
            printf("  sel %2d: info=0x%02x", sel, info);
            for (int r = 0; r < 4; r++) {
                int rq = r == 0 ? 0x81 : r == 1 ? 0x82 : r == 2 ? 0x83 : 0x87;
                unsigned char b[8] = {0}; int act = 0;
                int ee = (int)req(xus[i].id, 1, rq, sel, b, 8, &act);
                if (ee) { printf("  %s=ERR0x%x", rq == 0x81 ? "cur" : rq == 0x82 ? "min" : rq == 0x83 ? "max" : "def", ee); continue; }
                printf("  %s(len%d)=", rq == 0x81 ? "cur" : rq == 0x82 ? "min" : rq == 0x83 ? "max" : "def", act);
                for (int k = 0; k < act; k++) printf("%02x", b[k]);
            }
            printf("\n");
        }
    }
    return 0;
}
