/*
 * streamcam-ctl.c — read and set UVC (USB Video Class) controls of a webcam
 * on macOS via IOKit USB device requests. Built for the Logitech StreamCam
 * (046d:0893-ish; any UVC cam works).
 *
 * UVC access technique adapted from fabioferrero90/webcam-settings (MIT).
 *
 * Build:  clang -O2 -framework CoreFoundation -framework IOKit streamcam-ctl.c -o streamcam-ctl
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>

/* ---- UVC constants ------------------------------------------------------ */
enum { UNIT_CT = 0, UNIT_PU = 1 }; /* camera terminal, processing unit */

/* Camera Terminal control selectors */
enum { CT_AE_MODE = 0x02, CT_AE_PRIORITY = 0x03, CT_EXPOSURE_ABS = 0x04 };
/* Processing Unit control selectors */
enum {
    PU_BACKLIGHT = 0x01, PU_BRIGHTNESS = 0x02, PU_CONTRAST = 0x03,
    PU_GAIN = 0x04, PU_POWERLINE = 0x05, PU_SATURATION = 0x07,
    PU_SHARPNESS = 0x08, PU_GAMMA = 0x09, PU_WB_TEMP = 0x0A, PU_WB_AUTO = 0x0B
};
/* class-specific requests */
enum { GET_CUR = 0x81, GET_MIN = 0x82, GET_MAX = 0x83, GET_RES = 0x84,
       GET_INFO = 0x86, GET_DEF = 0x87 };
enum { SET_CUR = 0x01 };

typedef struct { UInt8 vcIf, unitId[2]; UInt32 controls[2]; } layout;

static IOUSBDeviceInterface **dev;
static layout lay;

/* ---- descriptor parsing (from webcam-settings, MIT) --------------------- */
static int parse(IOUSBDeviceInterface **d, layout *l) {
    IOUSBConfigurationDescriptorPtr cfg;
    if ((*d)->GetConfigurationDescriptorPtr(d, 0, &cfg)) return 0;
    UInt8 *p = (UInt8 *)cfg, *end = p + USBToHostWord(cfg->wTotalLength);
    int isVC = 0, found = 0;
    *l = (layout){0};
    for (; p + 2 <= end && p[0]; p += p[0]) {
        if (p[1] == 0x04) { /* interface descriptor */
            isVC = p[5] == 0x0E && p[6] == 0x01; /* class VIDEO, subclass VC */
            if (isVC && !found) { l->vcIf = p[2]; found = 1; }
        } else if (isVC && p[1] == 0x24) { /* CS_INTERFACE */
            if (p[2] == 0x02 && (p[4] | p[5] << 8) == 0x0201) { /* input terminal, ITT_CAMERA */
                l->unitId[UNIT_CT] = p[3];
                for (int i = 0; i < p[14] && i < 4; i++)
                    l->controls[UNIT_CT] |= (UInt32)p[15 + i] << (8 * i);
            } else if (p[2] == 0x05) { /* processing unit */
                l->unitId[UNIT_PU] = p[3];
                for (int i = 0; i < p[7] && i < 4; i++)
                    l->controls[UNIT_PU] |= (UInt32)p[8 + i] << (8 * i);
            }
        }
    }
    return found;
}

typedef struct { char name[128]; int vid, pid; unsigned location; } uvc_info;

static int scan_list(uvc_info *list, int max, int open, unsigned want) {
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching(kIOUSBDeviceClassName), &it)) return 0;
    io_service_t svc;
    int n = 0, saw = 0, dbg = getenv("UVC_DEBUG") != NULL;
    while ((svc = IOIteratorNext(it))) {
        saw++;
        IOCFPlugInInterface **plug;
        SInt32 score;
        IOUSBDeviceInterface **d = NULL;
        kern_return_t k = IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
                                               kIOCFPlugInInterfaceID, &plug, &score);
        if (dbg) { io_name_t cn; IORegistryEntryGetName(svc, cn); printf("[dbg] svc=%s plugin=%d (0x%x)\n", cn, k, k); }
        if (!k) {
            (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&d);
            IODestroyPlugInInterface(plug);
        }
        layout l;
        kern_return_t pk = kIOReturnSuccess;
        if (d) pk = parse(d, &l) ? 0 : 1;
        if (dbg && d) printf("[dbg] parse=%d\n", pk ? 0 : 1);
        if (d && pk == 0) {
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
            if (open && !dev && (!want || want == loc)) { dev = d; lay = l; d = NULL; }
        }
        if (d) (*d)->Release(d);
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    return n;
}

static int uvc_open(unsigned location) {
    if (dev) { (*dev)->Release(dev); dev = NULL; }
    scan_list(NULL, 0, 1, location);
    return dev != NULL;
}

/* ---- control I/O -------------------------------------------------------- */
static IOReturn req(int unit, int in, int r, int sel, void *buf, int len) {
    if (!dev) return kIOReturnNotOpen;
    IOUSBDevRequest q = {
        .bmRequestType = (UInt8)(in ? 0xA1 : 0x21), .bRequest = (UInt8)r,
        .wValue = (UInt16)(sel << 8), .wIndex = (UInt16)((lay.unitId[unit] << 8) | lay.vcIf),
        .wLength = (UInt16)len, .pData = buf };
    return (*dev)->DeviceRequest(dev, &q);
}

static int uvc_get(int unit, int sel, int r, int len, long *out) {
    UInt8 b[4] = {0};
    IOReturn e = req(unit, 1, r, sel, b, len);
    if (e) return (int)e;
    if (len == 1) *out = b[0];
    else if (len == 2) *out = (SInt16)(b[0] | b[1] << 8);
    else *out = (SInt32)(b[0] | b[1] << 8 | b[2] << 16 | (UInt32)b[3] << 24);
    return 0;
}

static int uvc_set(int unit, int sel, int len, long val) {
    UInt8 b[4] = { (UInt8)(val & 0xFF), (UInt8)(val >> 8 & 0xFF),
                   (UInt8)(val >> 16 & 0xFF), (UInt8)(val >> 24 & 0xFF) };
    return (int)req(unit, 0, SET_CUR, sel, b, len);
}

/* ---- helpers ------------------------------------------------------------ */
static const char *aemode_name(long v) {
    switch (v) {
        case 1: return "manual";
        case 2: return "auto";
        case 4: return "shutter-priority (manual shutter, auto gain)";
        case 8: return "aperture-priority (auto shutter, manual gain)";
        default: return "?";
    }
}

static void fmt_shutter(long units, char *out, size_t n) { /* units of 0.0001 s */
    if (units <= 0) { snprintf(out, n, "?"); return; }
    double s = units / 10000.0;
    double fps = 1.0 / s;
    snprintf(out, n, "%.5fs (~1/%.0f)", s, fps);
}

struct ctlinfo { int unit, sel, len; const char *label; };
static const struct ctlinfo TABLE[] = {
    { UNIT_CT, CT_AE_MODE,     2, "AE mode" },
    { UNIT_CT, CT_AE_PRIORITY, 1, "AE priority (0=frame-rate, 1=exposure)" },
    { UNIT_CT, CT_EXPOSURE_ABS, 4, "Exposure time absolute (1e-4 s)" },
    { UNIT_PU, PU_BACKLIGHT,   2, "Backlight compensation" },
    { UNIT_PU, PU_BRIGHTNESS,  2, "Brightness" },
    { UNIT_PU, PU_CONTRAST,    2, "Contrast" },
    { UNIT_PU, PU_GAIN,        2, "Gain" },
    { UNIT_PU, PU_POWERLINE,   1, "Power line frequency (0=off 1=50Hz 2=60Hz)" },
    { UNIT_PU, PU_SATURATION,  2, "Saturation" },
    { UNIT_PU, PU_SHARPNESS,   2, "Sharpness" },
    { UNIT_PU, PU_GAMMA,       2, "Gamma" },
    { UNIT_PU, PU_WB_TEMP,     2, "White balance temperature (K)" },
    { UNIT_PU, PU_WB_AUTO,     1, "White balance temperature auto (0/1)" },
};

static int ctl_supported(const struct ctlinfo *c) {
    /* UVC bmControls bitmap: bit (selector-1) corresponds to selector n */
    return lay.unitId[c->unit] && (lay.controls[c->unit] >> (c->sel - 1) & 1);
}

static void print_ctl(const struct ctlinfo *c) {
    if (!ctl_supported(c)) { printf("  %-44s : (not supported)\n", c->label); return; }
    unsigned char gi = 0;
    int e = (int)req(c->unit, 1, GET_INFO, c->sel, &gi, 1);
    long mn = 0, mx = 0, res = 0, def = 0, cur = 0;
    int g1 = uvc_get(c->unit, c->sel, GET_MIN, c->len, &mn);
    int g2 = uvc_get(c->unit, c->sel, GET_MAX, c->len, &mx);
    int g3 = uvc_get(c->unit, c->sel, GET_RES, c->len, &res);
    int g4 = uvc_get(c->unit, c->sel, GET_DEF, c->len, &def);
    int g5 = uvc_get(c->unit, c->sel, GET_CUR, c->len, &cur);
    char extra[160] = "";
    if (e == 0) snprintf(extra, sizeof extra, "info=0x%02x", gi);
    if (c->unit == UNIT_CT && c->sel == CT_AE_MODE && g5 == 0) {
        char sb[96]; snprintf(sb, sizeof sb, "  [current: %s]", aemode_name(cur)); strlcat(extra, sb, sizeof extra);
    }
    if (c->unit == UNIT_CT && c->sel == CT_AE_MODE && g2 == 0) {
        char sb[128] = "  [supported:";
        if (mx & 1) strlcat(sb, " manual", sizeof sb);
        if (mx & 2) strlcat(sb, " auto", sizeof sb);
        if (mx & 4) strlcat(sb, " shutter-prio", sizeof sb);
        if (mx & 8) strlcat(sb, " aperture-prio", sizeof sb);
        strlcat(sb, "]", sizeof sb);
        strlcat(extra, sb, sizeof extra);
    }
    if (c->unit == UNIT_CT && c->sel == CT_EXPOSURE_ABS && g5 == 0) {
        char sb[80], sh[40]; fmt_shutter(cur, sh, sizeof sh);
        snprintf(sb, sizeof sb, "  [%s]", sh); strlcat(extra, sb, sizeof extra);
    }
    printf("  %-44s : %s min=%ld max=%ld res=%ld def=%ld cur=%ld %s%s%s%s%s\n",
           c->label,
           (g1|g2|g3|g4|g5) ? "(get failed)" : "",
           mn, mx, res, def, cur, extra,
           g1 ? " MIN!" : "", g2 ? " MAX!" : "", g3 ? " RES!" : "", g5 ? " CUR!" : "");
}

static void usage(void) {
    printf(
"usage: streamcam-ctl [--location N] <command> [args]\n"
"\n"
"  list                          list UVC cameras (name, vid:pid, location)\n"
"  probe                         full capability + range dump\n"
"  status                        current values\n"
"  set-aemode   manual|auto|shutter|aperture\n"
"                                shutter = manual shutter + AUTO gain\n"
"  set-shutter  1/60 | 0.0166 | <raw 1e-4 s units>\n"
"  set-gain     <n>              (takes effect in manual/aperture modes)\n"
"  set-wbauto   on|off\n"
"  set-wb       <kelvin>         (takes effect when wbauto off)\n"
"  set-backlight <n>\n"
"  set-aepriority 0|1\n"
"  set-powerline 0|1|2\n"
"  set-brightness/contrast/saturation/sharpness/gamma <n>\n");
}

/* arg parsing for shutter strings ----------------------------------------- */
static long parse_shutter(const char *s) { /* returns 1e-4 s units, or <=0 on error */
    if (strchr(s, '/')) {
        long num = 0, den = 0;
        if (sscanf(s, "%ld/%ld", &num, &den) != 2 || num <= 0 || den <= 0) return -1;
        return (long)((10000.0 * num) / den + 0.5);
    }
    char *end = NULL;
    double v = strtod(s, &end);
    if (end && *end == 'u' && end != s) return (long)v; /* raw units */
    if (v <= 0 || (end && *end != '\0')) return -1;
    return (long)(v * 10000.0 + 0.5);
}

static long clampval(const struct ctlinfo *c, long v) {
    long mn = 0, mx = 0, res = 1;
    uvc_get(c->unit, c->sel, GET_MIN, c->len, &mn);
    uvc_get(c->unit, c->sel, GET_MAX, c->len, &mx);
    uvc_get(c->unit, c->sel, GET_RES, c->len, &res);
    if (res <= 0) res = 1;
    if (v < mn) v = mn;
    if (v > mx) v = mx;
    v -= (v - mn) % res; /* snap to resolution grid */
    return v;
}

static int do_set(const struct ctlinfo *c, long v) {
    long before = 0;
    uvc_get(c->unit, c->sel, GET_CUR, c->len, &before);
    int e = uvc_set(c->unit, c->sel, c->len, v);
    if (e) { printf("SET failed: 0x%x (%s)\n", e, e == 0xe000404f ? "pipe stalled (control rejected?)" : "error"); return 1; }
    long after = 0;
    if (uvc_get(c->unit, c->sel, GET_CUR, c->len, &after) == 0) {
        if (after == v) printf("%s: %ld -> %ld  (verified)\n", c->label, before, after);
        else printf("%s: %ld -> %ld  WARNING: camera reports %ld, wanted %ld\n", c->label, before, after, after, v);
    } else {
        printf("%s: %ld -> %ld  (read-back failed)\n", c->label, before, v);
    }
    return 0;
}

int main(int argc, char **argv) {
    unsigned want = 0;
    int argi = 1;
    if (argi < argc && !strcmp(argv[argi], "--location") && argi + 1 < argc) {
        want = (unsigned)strtoul(argv[argi + 1], NULL, 0);
        argi += 2;
    }
    if (argi >= argc) { usage(); return 2; }
    const char *cmd = argv[argi++];

    if (!strcmp(cmd, "list")) {
        uvc_info list[16];
        int n = scan_list(list, 16, 0, 0);
        printf("%d UVC camera(s):\n", n);
        for (int i = 0; i < n; i++)
            printf("  %s  vid=0x%04x pid=0x%04x  location=0x%08x\n", list[i].name, list[i].vid, list[i].pid, list[i].location);
        return 0;
    }

    if (!uvc_open(want)) { fprintf(stderr, "could not open a UVC camera\n"); return 1; }

    if (!strcmp(cmd, "probe")) {
        printf("VC interface=%d  CT id=%d (bmControls=0x%08x)  PU id=%d (bmControls=0x%08x)\n\n",
               lay.vcIf, lay.unitId[UNIT_CT], lay.controls[UNIT_CT], lay.unitId[UNIT_PU], lay.controls[UNIT_PU]);
        printf("camera terminal + processing unit controls:\n");
        for (unsigned i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) print_ctl(&TABLE[i]);
        return 0;
    }

    if (!strcmp(cmd, "status")) {
        for (unsigned i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) {
            const struct ctlinfo *c = &TABLE[i];
            if (!ctl_supported(c)) continue;
            long cur = 0;
            if (uvc_get(c->unit, c->sel, GET_CUR, c->len, &cur)) { printf("%-44s : <get failed>\n", c->label); continue; }
            char extra[128] = "";
            if (c->sel == CT_AE_MODE) { char sb[96]; snprintf(sb, sizeof sb, "  [%s]", aemode_name(cur)); strlcat(extra, sb, sizeof extra); }
            if (c->sel == CT_EXPOSURE_ABS) { char sb[96], sh[48]; fmt_shutter(cur, sh, sizeof sh); snprintf(sb, sizeof sb, "  [%s]", sh); strlcat(extra, sb, sizeof extra); }
            printf("%-44s : %ld%s\n", c->label, cur, extra);
        }
        return 0;
    }

    /* ---- setters --------------------------------------------------------- */
    if (!strcmp(cmd, "set-aemode") && argi < argc) {
        long v = 0;
        if      (!strcmp(argv[argi], "manual"))    v = 1;
        else if (!strcmp(argv[argi], "auto"))      v = 2;
        else if (!strcmp(argv[argi], "shutter"))   v = 4;
        else if (!strcmp(argv[argi], "aperture"))  v = 8;
        else { fprintf(stderr, "unknown AE mode '%s'\n", argv[argi]); return 2; }
        const struct ctlinfo c = { UNIT_CT, CT_AE_MODE, 2, "AE mode" };
        return do_set(&c, v);
    }
    if (!strcmp(cmd, "set-shutter") && argi < argc) {
        long v = parse_shutter(argv[argi]);
        if (v <= 0) { fprintf(stderr, "bad shutter '%s' (use 1/60, 0.0166, or raw 1e-4s units)\n", argv[argi]); return 2; }
        const struct ctlinfo c = { UNIT_CT, CT_EXPOSURE_ABS, 4, "Exposure time absolute (1e-4 s)" };
        return do_set(&c, clampval(&c, v));
    }
    if (!strcmp(cmd, "set-gain") && argi < argc) {
        long v = strtol(argv[argi], NULL, 0);
        const struct ctlinfo c = { UNIT_PU, PU_GAIN, 2, "Gain" };
        return do_set(&c, clampval(&c, v));
    }
    if (!strcmp(cmd, "set-wbauto") && argi < argc) {
        long v = (!strcmp(argv[argi], "on") || !strcmp(argv[argi], "1")) ? 1 : 0;
        const struct ctlinfo c = { UNIT_PU, PU_WB_AUTO, 1, "White balance temperature auto (0/1)" };
        return do_set(&c, v);
    }
    if (!strcmp(cmd, "set-wb") && argi < argc) {
        long v = strtol(argv[argi], NULL, 0);
        const struct ctlinfo c = { UNIT_PU, PU_WB_TEMP, 2, "White balance temperature (K)" };
        return do_set(&c, clampval(&c, v));
    }
    if (!strcmp(cmd, "set-backlight") && argi < argc) {
        long v = strtol(argv[argi], NULL, 0);
        const struct ctlinfo c = { UNIT_PU, PU_BACKLIGHT, 2, "Backlight compensation" };
        return do_set(&c, clampval(&c, v));
    }
    if (!strcmp(cmd, "set-aepriority") && argi < argc) {
        long v = strtol(argv[argi], NULL, 0);
        const struct ctlinfo c = { UNIT_CT, CT_AE_PRIORITY, 1, "AE priority (0=frame-rate, 1=exposure)" };
        return do_set(&c, v);
    }
    if (!strcmp(cmd, "set-powerline") && argi < argc) {
        long v = strtol(argv[argi], NULL, 0);
        const struct ctlinfo c = { UNIT_PU, PU_POWERLINE, 1, "Power line frequency (0=off 1=50Hz 2=60Hz)" };
        return do_set(&c, v);
    }
    if (!strncmp(cmd, "set-", 4) && argi < argc) {
        long v = strtol(argv[argi], NULL, 0);
        const char *name = cmd + 4;
        const struct ctlinfo *c = NULL;
        if      (!strcmp(name, "brightness")) { static const struct ctlinfo k = { UNIT_PU, PU_BRIGHTNESS, 2, "Brightness" }; c = &k; }
        else if (!strcmp(name, "contrast"))   { static const struct ctlinfo k = { UNIT_PU, PU_CONTRAST, 2, "Contrast" }; c = &k; }
        else if (!strcmp(name, "saturation")) { static const struct ctlinfo k = { UNIT_PU, PU_SATURATION, 2, "Saturation" }; c = &k; }
        else if (!strcmp(name, "sharpness"))  { static const struct ctlinfo k = { UNIT_PU, PU_SHARPNESS, 2, "Sharpness" }; c = &k; }
        else if (!strcmp(name, "gamma"))      { static const struct ctlinfo k = { UNIT_PU, PU_GAMMA, 2, "Gamma" }; c = &k; }
        if (c) return do_set(c, clampval(c, v));
    }

    usage();
    return 2;
}
