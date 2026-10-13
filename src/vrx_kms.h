// SPDX-License-Identifier: MIT
//
// Which DRM node drives the display. On split display/render hardware
// (the Pi family: v3d renders, vc4 scans out) the two are separate cards,
// and their numbers follow probe order, which changes between boots
// (bench 2026-10-13: card0 was vc4 one boot and v3d the next). The display
// is the primary node whose KMS resources have a connector and a CRTC.
// The compositor (which modesets on it) and the app role (which allocates
// scanout buffers on it) both ask here, so they always agree.

#ifndef VRX_KMS_H
#define VRX_KMS_H

#include <stddef.h>

// Open the display's primary node read-write (O_CLOEXEC). Writes its path
// into `path` (when not NULL) and returns the fd, or -1 if no node drives
// a display.
int vrx_open_kms_node(char *path, size_t path_len);

#endif  // VRX_KMS_H
