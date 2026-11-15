/* uvc_core.h — minimal UVC control access via IOKit (technique from
 * fabioferrero90/webcam-settings, MIT). Exposed to Swift. */
#ifndef UVC_CORE_H
#define UVC_CORE_H

#define UVC_UNIT_CT 0 /* camera terminal */
#define UVC_UNIT_PU 1 /* processing unit */

typedef struct { char name[128]; int vid, pid; unsigned location; } uvc_info;

int uvc_scan_list(uvc_info *out, int max);
int uvc_open(unsigned location); /* 0 = first UVC camera */
const char *uvc_name(void);
int uvc_vid(void);
int uvc_pid(void);

/* class-specific control requests; req: GET_CUR=0x81 MIN=0x82 MAX=0x83 RES=0x84 DEF=0x87 */
int uvc_get(int unit, int sel, int req, int len, long *out); /* 0 on success */
int uvc_set(int unit, int sel, int len, long val);           /* 0 on success */
int uvc_getinfo(int unit, int sel, unsigned char *out);      /* GET_INFO byte */

/* descriptor layout after open */
void uvc_layout(int *vcIf, int *ctId, unsigned *ctBm, int *puId, unsigned *puBm);
/* is control (selector sel) supported on unit, per descriptor bitmap */
int uvc_ctl_supported(int unit, int sel);
/* newline-separated "name\t0xvid\t0xpid\tlocation" into out; returns count */
int uvc_list_text(char *out, int cap);

#endif
