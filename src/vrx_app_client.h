// SPDX-License-Identifier: MIT
#ifndef VRX_APP_CLIENT_H
#define VRX_APP_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vrx_ipc.h"

struct vrx_app_client;

struct vrx_app_client_callbacks {
    void (*on_focus)(uint32_t focused, void *userdata);
    void (*on_input)(const struct vrx_msg_input *input, void *userdata);
    // the compositor is done with the buffer presented as `sequence`
    void (*on_release)(uint32_t sequence, void *userdata);
    // keep clear of the top `top` output pixels (the shell's strip)
    void (*on_insets)(uint32_t top, void *userdata);
    void (*on_back)(void *userdata);
    // the screen went off (0) or came back (1)
    void (*on_display)(uint32_t on, void *userdata);
    // the system is about to sleep (1) or woke (0)
    void (*on_sleep)(uint32_t sleeping, void *userdata);
    // the panel's vblank phase: a real flip's time and the refresh period
    // (plan 0014 D6)
    void (*on_vblank)(uint64_t flip_ns, uint64_t period_ns, void *userdata);
    // build the semantics tree or stop (plan 0014 D8)
    void (*on_semantics)(uint32_t on, void *userdata);
    void *userdata;
};

struct vrx_app_client *vrx_app_client_connect(const char *bundle_id);
void vrx_app_client_destroy(struct vrx_app_client *c);
int vrx_app_client_fd(const struct vrx_app_client *c);
uint32_t vrx_app_client_width(const struct vrx_app_client *c);
uint32_t vrx_app_client_height(const struct vrx_app_client *c);
// The panel's physical size from REGISTERED; 0 = unknown.
uint32_t vrx_app_client_width_mm(const struct vrx_app_client *c);
uint32_t vrx_app_client_height_mm(const struct vrx_app_client *c);
bool vrx_app_client_accepts_format(const struct vrx_app_client *c,
                                   uint32_t format, uint64_t modifier);
bool vrx_app_client_send(struct vrx_app_client *c, uint32_t type,
                         const void *payload, size_t len, int fd);
// Present a frame; returns its sequence (>0), or 0 when the send failed.
// The buffer must not be reused until on_release reports that sequence.
uint32_t vrx_app_client_present(struct vrx_app_client *c, int dmabuf_fd,
                                uint32_t width, uint32_t height, uint32_t pitch,
                                uint32_t format, uint64_t modifier);
bool vrx_app_client_dispatch(struct vrx_app_client *c,
                             const struct vrx_app_client_callbacks *cb);

#endif
