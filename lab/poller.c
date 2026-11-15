/* poller.c — snapshot all readable webcam registers (XUs + UVC CT/PU) every 400 ms
 * and print any changes, to map Logi Options+ actions to USB controls.
 * usage: poller [seconds] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>

static IOUSBDeviceInterface **dev;

static IOReturn req(int unit, int in, int r, int sel, void *buf, int len) {
    IOUSBDevRequest q = {
        .bmRequestType = (UInt8)(in ? 0xA1 : 0x21), .bRequest = (UInt8)r,
        .wValue = (UInt16)(sel << 8), .wIndex = (UInt16)(unit << 8),
        .wLength = (UInt16)len, .pData = buf };
    return (*dev)->DeviceRequest(dev, &q);
}

#define MAXREG 64
static struct { int unit, sel, len; const char *name; } regs[MAXREG];
static int nregs;
static unsigned char prev[MAXREG][16];
static int have[MAXREG];

static void add(int unit, int sel, int len, const char *name) {
    regs[nregs] = (typeof(regs[0])){ unit, sel, len, name };
    nregs++;
}

int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 60;

    /* XUs */
    int x9[] = {1,3,4,5,6,9,10,11,12,13,14,15,16,17,18};
    int x9l[] = {2,2,1,2,2,2,1,1,1,1,1,1,1,4,1};
    for (unsigned i = 0; i < sizeof x9 / sizeof x9[0]; i++) { char nm[16]; snprintf(nm, 16, "x9.s%d", x9[i]); add(9, x9[i], x9l[i], strdup(nm)); }
    add(10, 2, 1, "x10.s2");
    int x8[] = {1,2,3,4,5,6,9,10,11,12};
    int x8l[] = {4,2,2,2,2,6,2,4,1,2};
    for (unsigned i = 0; i < sizeof x8 / sizeof x8[0]; i++) { char nm[16]; snprintf(nm, 16, "x8.s%d", x8[i]); add(8, x8[i], x8l[i], strdup(nm)); }
    int x6[] = {2,5,7,8,9,11,15};
    int x6l[] = {8,1,1,1,1,8,8};
    for (unsigned i = 0; i < sizeof x6 / sizeof x6[0]; i++) { char nm[16]; snprintf(nm, 16, "x6.s%d", x6[i]); add(6, x6[i], x6l[i], strdup(nm)); }
    int x14[] = {3,4,5,6};
    int x14l[] = {3,3,3,4};
    for (unsigned i = 0; i < sizeof x14 / sizeof x14[0]; i++) { char nm[16]; snprintf(nm, 16, "x14.s%d", x14[i]); add(14, x14[i], x14l[i], strdup(nm)); }
    /* UVC standard */
    add(1, 0x02, 2, "CT.aemode");  add(1, 0x03, 1, "CT.aeprio");  add(1, 0x04, 4, "CT.exposure");
    add(3, 0x01, 2, "PU.backlight"); add(3, 0x02, 2, "PU.brightness"); add(3, 0x04, 2, "PU.gain");
    add(3, 0x05, 1, "PU.powerline"); add(3, 0x07, 2, "PU.saturation"); add(3, 0x08, 2, "PU.sharpness");
    add(3, 0x09, 2, "PU.gamma"); add(3, 0x0A, 2, "PU.wbtemp"); add(3, 0x0B, 1, "PU.wbauto");

    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching(kIOUSBDeviceClassName), &it)) return 1;
    io_service_t svc;
    while ((svc = IOIteratorNext(it))) {
        IOCFPlugInInterface **plug; SInt32 score; IOUSBDeviceInterface **d = NULL;
        if (!IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score)) {
            (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&d);
            IODestroyPlugInInterface(plug);
        }
        UInt16 v = 0, pid = 0;
        if (d) { (*d)->GetDeviceVendor(d, &v); (*d)->GetDeviceProduct(d, &pid); }
        if (d && v == 0x046d && !dev) { dev = d; d = NULL; }
        if (d) (*d)->Release(d);
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    if (!dev) { printf("no logitech cam\n"); return 1; }

    printf("polling %d registers for %.0fs — change settings in Logi Options+ now\n", nregs, secs);
    for (double t = 0; t < secs; t += 0.4) {
        for (int i = 0; i < nregs; i++) {
            unsigned char b[16] = {0};
            IOReturn e = req(regs[i].unit, 1, 0x81, regs[i].sel, b, regs[i].len);
            if (e) { b[0] = 0xEE; b[1] = 0xEE; }
            if (have[i] && memcmp(prev[i], b, regs[i].len) != 0) {
                char o[40] = "", n[40] = "";
                for (int k = 0; k < regs[i].len; k++) sprintf(o + k * 2, "%02x", prev[i][k]);
                for (int k = 0; k < regs[i].len; k++) sprintf(n + k * 2, "%02x", b[k]);
                printf("t=%6.1fs  %-12s %s -> %s\n", t, regs[i].name, o, n);
            }
            memcpy(prev[i], b, regs[i].len);
            have[i] = 1;
        }
        usleep(400000);
    }
    printf("done\n");
    return 0;
}
