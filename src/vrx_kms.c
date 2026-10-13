// SPDX-License-Identifier: MIT

#include "vrx_kms.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

int vrx_open_kms_node(char *path, size_t path_len) {
    drmDevicePtr devices[64];
    int n = drmGetDevices2(0, devices, sizeof(devices) / sizeof(*devices));
    if (n < 0) return -1;
    int found = -1;
    for (int i = 0; i < n && found < 0; i++) {
        if (!(devices[i]->available_nodes & (1 << DRM_NODE_PRIMARY))) continue;
        const char *node = devices[i]->nodes[DRM_NODE_PRIMARY];
        int fd = open(node, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmModeRes *res = drmModeGetResources(fd);
        bool is_kms = res != NULL && res->count_connectors > 0 && res->count_crtcs > 0;
        if (res != NULL) drmModeFreeResources(res);
        if (!is_kms) {
            close(fd);
            continue;
        }
        if (path != NULL && path_len > 0) snprintf(path, path_len, "%s", node);
        found = fd;
    }
    drmFreeDevices(devices, n);
    return found;
}
