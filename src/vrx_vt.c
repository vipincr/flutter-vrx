// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include "vrx_vt.h"

#include <fcntl.h>
#include <linux/kd.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int real_ioctl(int fd, unsigned long req, void *arg) {
    return ioctl(fd, req, arg);
}

static void vt_init(struct vrx_vt *vt) {
    vt->fd = -1;
    vt->prev_mode = KD_TEXT;
    vt->prev_kbmode = K_UNICODE;
    if (vt->ioctl_fn == NULL) vt->ioctl_fn = real_ioctl;
}

int vrx_vt_hold_fd(struct vrx_vt *vt, int fd) {
    vt_init(vt);
    int kb = 0;
    long mode = 0;
    // a fd that is not a VT refuses these: nothing to hold
    if (vt->ioctl_fn(fd, KDGKBMODE, &kb) != 0 || vt->ioctl_fn(fd, KDGETMODE, &mode) != 0) {
        close(fd);
        return -1;
    }
    // a VT a crashed compositor left graphical or keyless is restored to the
    // ordinary state, not to the leftover
    int prev_mode = mode == KD_GRAPHICS ? KD_TEXT : (int) mode;
    int prev_kb = kb == K_OFF ? K_UNICODE : kb;
    if (vt->ioctl_fn(fd, KDSETMODE, (void *) (long) KD_GRAPHICS) != 0) {
        close(fd);
        return -1;
    }
    if (vt->ioctl_fn(fd, KDSKBMODE, (void *) (long) K_OFF) != 0) {
        vt->ioctl_fn(fd, KDSETMODE, (void *) (long) prev_mode);
        close(fd);
        return -1;
    }
    vt->fd = fd;
    vt->prev_mode = prev_mode;
    vt->prev_kbmode = prev_kb;
    return 0;
}

int vrx_vt_hold(struct vrx_vt *vt, const char *path) {
    vt_init(vt);
    int fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) return -1;
    return vrx_vt_hold_fd(vt, fd);
}

void vrx_vt_release(struct vrx_vt *vt) {
    if (vt->fd < 0) return;
    // keyboard first: text mode with keys still off would be a dead console
    vt->ioctl_fn(vt->fd, KDSKBMODE, (void *) (long) vt->prev_kbmode);
    vt->ioctl_fn(vt->fd, KDSETMODE, (void *) (long) vt->prev_mode);
    close(vt->fd);
    vt->fd = -1;
}
