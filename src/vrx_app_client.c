// SPDX-License-Identifier: MIT
//
// flutter-vrx app-role IPC client (plan 0005, milestone B2).
//
// The --app half of the wire in vrx_ipc.h: connect to the compositor's
// socket, register the bundle, learn the output geometry, send each
// presented frame's dmabuf, and receive routed input events. The caller
// (flutter-pi.c, adapted for the app role) drives this from its event
// loop via vrx_app_client_dispatch().

#define _GNU_SOURCE

#include "vrx_app_client.h"

#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <drm_fourcc.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "vrx_ipc.h"

struct vrx_app_client {
    int fd;
    bool registered;
    struct vrx_msg_registered info;
    uint32_t last_sent_sequence;
};

void *vrx_app_client_recv(struct vrx_app_client *c, struct vrx_msg_header *out_h,
                          int *out_fd, int *out_n_fds);

struct vrx_app_client *vrx_app_client_connect(const char *bundle_id) {
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, VRX_UI_SOCKET, sizeof(addr.sun_path) - 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fprintf(stderr, "[vrx-app] socket: %s\n", strerror(errno));
        return NULL;
    }
    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[vrx-app] connect %s: %s (is the compositor running?)\n",
                VRX_UI_SOCKET, strerror(errno));
        close(fd);
        return NULL;
    }
    struct vrx_app_client *c = calloc(1, sizeof *c);
    c->fd = fd;

    struct vrx_msg_register reg = {0};
    snprintf(reg.id, sizeof(reg.id), "%s", bundle_id);
    vrx_app_client_send(c, VRX_MSG_REGISTER, &reg, sizeof(reg), -1);

    // Synchronous handshake: the registration reply decides whether we can
    // render at all (geometry + formats), so block for it once.
    struct vrx_msg_header h;
    void *payload = vrx_app_client_recv(c, &h, NULL, NULL);
    if (!payload || h.type != VRX_MSG_REGISTERED) {
        fprintf(stderr, "[vrx-app] no registration reply from compositor\n");
        close(fd);
        free(c);
        return NULL;
    }
    memcpy(&c->info, payload, sizeof(c->info) < h.length ? sizeof(c->info) : h.length);
    free(payload);
    c->registered = true;
    fprintf(stderr, "[vrx-app] registered: %ux%u, %u format(s)\n",
            c->info.width, c->info.height, c->info.n_formats);
    return c;
}

void vrx_app_client_destroy(struct vrx_app_client *c) {
    if (!c) return;
    vrx_app_client_send(c, VRX_MSG_CLOSED, NULL, 0, -1);
    close(c->fd);
    free(c);
}

int vrx_app_client_fd(const struct vrx_app_client *c) {
    return c ? c->fd : -1;
}

uint32_t vrx_app_client_width(const struct vrx_app_client *c) {
    return c ? c->info.width : 0;
}

uint32_t vrx_app_client_height(const struct vrx_app_client *c) {
    return c ? c->info.height : 0;
}

bool vrx_app_client_accepts_format(const struct vrx_app_client *c,
                                   uint32_t format, uint64_t modifier) {
    if (!c) return false;
    for (uint32_t i = 0; i < c->info.n_formats && i < VRX_MAX_FORMATS; i++) {
        if (c->info.formats[i] == format &&
            (c->info.modifiers[i] == modifier ||
             c->info.modifiers[i] == DRM_FORMAT_MOD_INVALID)) {
            return true;
        }
    }
    return false;
}

bool vrx_app_client_send(struct vrx_app_client *c, uint32_t type,
                         const void *payload, size_t len, int fd) {
    if (!c) return false;
    struct vrx_msg_header h = { .type = type, .length = (uint32_t) len };
    struct iovec iov[2] = {
        { .iov_base = &h, .iov_len = sizeof(h) },
        { .iov_base = (void *) payload, .iov_len = len },
    };
    struct msghdr mh = { .msg_iov = iov, .msg_iovlen = len ? 2 : 1 };
    char cbuf[CMSG_SPACE(sizeof(int))] = {0};
    if (fd >= 0) {
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof(cbuf);
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    }
    return sendmsg(c->fd, &mh, MSG_NOSIGNAL) >= 0;
}

bool vrx_app_client_present(struct vrx_app_client *c, int dmabuf_fd,
                            uint32_t width, uint32_t height, uint32_t pitch,
                            uint32_t format, uint64_t modifier) {
    if (!c || !c->registered) return false;
    struct vrx_msg_buffer b = {
        .width = width,
        .height = height,
        .pitch = pitch,
        .format = format,
        .modifier = modifier,
        .sequence = ++c->last_sent_sequence,
    };
    // The fd is dup'd by sendmsg into the message; the caller keeps ownership
    // of the original and closes it after the bo is released.
    return vrx_app_client_send(c, VRX_MSG_BUFFER, &b, sizeof(b), dmabuf_fd);
}

// Read one message. Returns a malloc'd payload the caller frees, or NULL on
// error/disconnect. If out_fd is non-NULL and a descriptor arrived, it is
// written there (caller owns it); extra descriptors are closed.
void *vrx_app_client_recv(struct vrx_app_client *c, struct vrx_msg_header *out_h,
                          int *out_fd, int *out_n_fds) {
    if (out_fd) *out_fd = -1;
    if (out_n_fds) *out_n_fds = 0;

    char cbuf[CMSG_SPACE(sizeof(int) * 4)] = {0};
    struct {
        struct vrx_msg_header h;
        unsigned char payload[4096];
    } pkt;

    struct iovec iov = { .iov_base = &pkt, .iov_len = sizeof(pkt) };
    struct msghdr mh = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = cbuf, .msg_controllen = sizeof(cbuf),
    };
    ssize_t n = recvmsg(c->fd, &mh, 0);
    if (n <= 0) return NULL;
    if ((size_t) n < sizeof(pkt.h) || pkt.h.length > sizeof(pkt.payload)) return NULL;
    if (pkt.h.length > 0 && (size_t) n < sizeof(pkt.h) + pkt.h.length) return NULL;

    if (out_h) *out_h = pkt.h;
    int fds[4] = {-1, -1, -1, -1};
    int n_fds = 0;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            int cnt = (int) ((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            if (cnt > 4) cnt = 4;
            memcpy(fds, CMSG_DATA(cm), (size_t) cnt * sizeof(int));
            n_fds = cnt;
        }
    }
    if (out_fd && n_fds > 0) *out_fd = fds[0];
    if (out_n_fds) *out_n_fds = n_fds;
    for (int i = out_fd && n_fds > 0 ? 1 : 0; i < n_fds; i++) {
        if (fds[i] >= 0) close(fds[i]);  // extra fds: not ours to keep
    }

    void *payload = malloc(pkt.h.length ? pkt.h.length : 1);
    memcpy(payload, pkt.payload, pkt.h.length);
    return payload;
}

// Feed one received message into the caller's callbacks. Returns false when
// the connection is dead and the app should exit.
bool vrx_app_client_dispatch(struct vrx_app_client *c,
                             const struct vrx_app_client_callbacks *cb) {
    struct vrx_msg_header h;
    void *p = vrx_app_client_recv(c, &h, NULL, NULL);
    if (!p) return false;
    bool alive = true;
    switch (h.type) {
        case VRX_MSG_FOCUS:
            if (h.length >= sizeof(struct vrx_msg_focus) && cb && cb->on_focus) {
                cb->on_focus(((struct vrx_msg_focus *) p)->focused, cb->userdata);
            }
            break;
        case VRX_MSG_INPUT:
            if (h.length >= sizeof(struct vrx_msg_input) && cb && cb->on_input) {
                cb->on_input((const struct vrx_msg_input *) p, cb->userdata);
            }
            break;
        case VRX_MSG_SHUTDOWN:
            alive = false;
            break;
        default:
            break;  // unknown types are ignored, not fatal (forward compat)
    }
    free(p);
    return alive;
}
