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
    void *userdata;
};

struct vrx_app_client *vrx_app_client_connect(const char *bundle_id);
void vrx_app_client_destroy(struct vrx_app_client *c);
int vrx_app_client_fd(const struct vrx_app_client *c);
uint32_t vrx_app_client_width(const struct vrx_app_client *c);
uint32_t vrx_app_client_height(const struct vrx_app_client *c);
bool vrx_app_client_accepts_format(const struct vrx_app_client *c,
                                   uint32_t format, uint64_t modifier);
bool vrx_app_client_send(struct vrx_app_client *c, uint32_t type,
                         const void *payload, size_t len, int fd);
bool vrx_app_client_present(struct vrx_app_client *c, int dmabuf_fd,
                            uint32_t width, uint32_t height, uint32_t pitch,
                            uint32_t format, uint64_t modifier);
bool vrx_app_client_dispatch(struct vrx_app_client *c,
                             const struct vrx_app_client_callbacks *cb);

#endif
