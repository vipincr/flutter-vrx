// SPDX-License-Identifier: MIT
/*
 * EGL dmabuf import: one implementation shared by the gstreamer video
 * player and the vrx camera plugin (vrx plan 0018 Part D step 1).
 *
 * A `struct frame_interface` holds the EGL display, a context the importing
 * thread makes current under a lock, the extension procedures and the list
 * of (format, modifier) pairs EGL can import. egl_dmabuf_import() turns a
 * description of dmabuf planes into an EGLImage bound to a new GL texture.
 *
 * Needs EGL and OpenGL ES 2 (HAVE_EGL_GLES2).
 */

#ifndef _FLUTTERPI_SRC_EGL_DMABUF_H
#define _FLUTTERPI_SRC_EGL_DMABUF_H

#include <stdbool.h>
#include <stdint.h>

#include "util/collection.h"
#include "util/lock_ops.h"
#include "util/refcounting.h"

#include "config.h"

#if !defined(HAVE_EGL_GLES2)
    #error "egl_dmabuf requires EGL and OpenGL ES2 support."
#else
    #include "egl.h"
    #include "gles.h"
#endif

#include "texture_registry.h"

#define EGL_DMABUF_MAX_PLANES 4

struct gl_renderer;

struct egl_modified_format {
    uint32_t format;
    uint64_t modifier;
    bool external_only;
};

struct frame_interface;

struct frame_interface *frame_interface_new(struct gl_renderer *renderer);

struct gbm_device;
struct gbm_device *frame_interface_get_gbm_device(struct frame_interface *interface);

ATTR_PURE int frame_interface_get_n_formats(struct frame_interface *interface);

ATTR_PURE const struct egl_modified_format *frame_interface_get_format(struct frame_interface *interface, int index);

#define for_each_format_in_frame_interface(index, format, interface)                                                          \
    for (const struct egl_modified_format *format = frame_interface_get_format((interface), 0), *guard = NULL; guard == NULL; \
         guard = (void *) 1)                                                                                                  \
        for (size_t index = 0; index < frame_interface_get_n_formats(interface); index++,                                     \
                    format = (index) < frame_interface_get_n_formats(interface) ? frame_interface_get_format((interface), (index)) : NULL)

DECLARE_LOCK_OPS(frame_interface)

DECLARE_REF_OPS(frame_interface)

/// Lock the interface and make its context current on this thread; false
/// (unlocked) when EGL refuses. Pair with frame_interface_clear_current().
bool frame_interface_make_current(struct frame_interface *interface);
void frame_interface_clear_current(struct frame_interface *interface);

/// One plane of a dmabuf. The fd stays the caller's.
struct egl_dmabuf_plane {
    int fd;
    uint32_t offset;
    uint32_t pitch;
    bool has_modifier;
    uint64_t modifier;
};

/// What to import. The EGL_NONE hints are skipped.
struct egl_dmabuf_desc {
    uint32_t width, height;
    uint32_t drm_format;
    int n_planes;
    struct egl_dmabuf_plane planes[EGL_DMABUF_MAX_PLANES];
    EGLint color_space, sample_range_hint, horizontal_chroma_siting, vertical_chroma_siting;
    bool external_only;
};

/// An imported dmabuf: the EGLImage and the GL texture bound to it.
struct egl_dmabuf_image {
    EGLImageKHR image;
    GLenum target;
    GLuint texture;
};

/// Look for (drm_format, modifier) in the formats EGL reports. Returns false
/// when EGL does not list it.
bool egl_dmabuf_find_format(struct frame_interface *interface, uint32_t drm_format, uint64_t modifier, bool *external_only_out);

/// Import `desc` into an EGLImage and bind a new GL texture to it. Takes
/// the interface's context lock itself. On failure returns a negative errno
/// and logs; nothing is left allocated.
int egl_dmabuf_import(struct frame_interface *interface, const struct egl_dmabuf_desc *desc, struct egl_dmabuf_image *out);

/// Delete the texture and destroy the image.
void egl_dmabuf_release(struct frame_interface *interface, struct egl_dmabuf_image *image);

#endif  // _FLUTTERPI_SRC_EGL_DMABUF_H
