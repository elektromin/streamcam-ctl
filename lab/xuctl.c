/* xuctl.c — get/set a single UVC extension-unit control: xuctl UNIT SEL LEN get|set [value] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>

static IOUSBDeviceInterface **dev;

static IOReturn req(int unit, int in, int r, int sel, void *buf, int len, int *actual) {
    IOUSBDevRequest q = {
        .bmRequestType = (UInt8)(in ? 0xA1 : 0x21), .bRequest = (UInt8)r,
        .wValue = (UInt16)(sel << 8), .wIndex = (UInt16)(unit << 8),
        .wLength = (UInt16)len, .pData = buf };
    IOReturn e = (*dev)->DeviceRequest(dev, &q);
    if (actual) *actual = q.wLenDone;
    return e;
}

int main(int argc, char **argv) {
    if (argc < 5) { printf("usage: xuctl UNIT SEL LEN get|set [value]\n"); return 2; }
    int unit = atoi(argv[1]), sel = atoi(argv[2]), len = atoi(argv[3]);
    int isSet = !strcmp(argv[4], "set");

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

    unsigned char b[16] = {0};
    int act = 0;
    if (!isSet) {
        IOReturn e = req(unit, 1, 0x81, sel, b, len, &act);
        if (e) { printf("ERR 0x%x\n", e); return 1; }
        printf("cur(%d bytes)=", act);
        for (int k = 0; k < act; k++) printf("%02x", b[k]);
        printf("\n");
        return 0;
    }
    unsigned long val = strtoul(argv[5], NULL, 0);
    for (int k = 0; k < len; k++) b[k] = (unsigned char)(val >> (8 * k));
    IOReturn e = req(unit, 0, 0x01, sel, b, len, NULL);
    if (e) { printf("SET ERR 0x%x\n", e); return 1; }
    printf("ok\n");
    return 0;
}
