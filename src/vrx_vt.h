// SPDX-License-Identifier: MIT
//
// The compositor holds the panel's virtual terminal while it runs (plan 0028
// D2): the VT goes to KD_GRAPHICS, so the kernel draws no console text on it
// (a deferred fbcon takes the panel on the first character written to a VT in
// KD_TEXT, and this keeps it from ever being one), and its keyboard goes to
// K_OFF, so a key reaches libinput and nothing else (a getty, were one
// running, would otherwise read it). Releasing puts both back as found.
//
// Failing to open or drive the VT is not fatal: a machine without a VT
// (no CONFIG_VT, a container) has no console to keep off the panel.

#ifndef VRX_VT_H
#define VRX_VT_H

#include <stdbool.h>

struct vrx_vt {
    int fd;                    // the VT, -1 when not held
    int prev_mode;             // KD_TEXT as found, or KD_TEXT if found graphical
    int prev_kbmode;           // K_UNICODE etc. as found, or K_UNICODE if found off
    int (*ioctl_fn)(int fd, unsigned long req, void *arg);  // ioctl(2), replaced by tests
};

// The active VT; the one a panel console would be on.
#define VRX_VT_DEFAULT "/dev/tty0"

// Open `path` and hold it. Returns 0, or -1 with nothing changed.
int vrx_vt_hold(struct vrx_vt *vt, const char *path);

// As vrx_vt_hold on an fd already open (the fd is then owned by vt).
int vrx_vt_hold_fd(struct vrx_vt *vt, int fd);

// Put the VT back as found and close it. Safe on a VT never held.
void vrx_vt_release(struct vrx_vt *vt);

#endif
