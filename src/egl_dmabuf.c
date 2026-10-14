// SPDX-License-Identifier: MIT
/*
 * EGL dmabuf import, shared by the gstreamer video player and the vrx camera
 * plugin. The frame_interface half is moved verbatim from the gstreamer
 * plugin's frame.c; egl_dmabuf_import() is its plane-attribute and texture
 * binding half, with the gstreamer parts taken out.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <pthread.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>

#include "egl_dmabuf.h"
#include "flutter-pi.h"
#include "gl_renderer.h"
#include "util/logging.h"
#include "util/refcounting.h"

#define DRM_FOURCC_FORMAT "c%c%c%c"
#define DRM_FOURCC_ARGS(format) (format) & 0xFF, ((format) >> 8) & 0xFF, ((format) >> 16) & 0xFF, ((format) >> 24) & 0xFF

struct frame_interface {
    struct gbm_device *gbm_device;
    EGLDisplay display;

    pthread_mutex_t context_lock;
    EGLContext context;

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;

    bool supports_external_target;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;

    bool supports_extended_imports;
#ifdef EGL_EXT_image_dma_buf_import_modifiers
    PFNEGLQUERYDMABUFFORMATSEXTPROC eglQueryDmaBufFormatsEXT;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC eglQueryDmaBufModifiersEXT;
#endif

    int n_formats;
    struct egl_modified_format *formats;

    refcount_t n_refs;
};

#ifdef EGL_EXT_image_dma_buf_import_modifiers
static bool query_formats(
    EGLDisplay display,
    PFNEGLQUERYDMABUFFORMATSEXTPROC egl_query_dmabuf_formats,
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC egl_query_dmabuf_modifiers,
    int *n_formats_out,
    struct egl_modified_format **formats_out
) {
    // EGLuint64KHR is defined by EGL_KHR_stream
    #ifndef EGL_KHR_stream
        #error "EGL header definitions for extension EGL_KHR_stream are required."
    #endif

    struct egl_modified_format *modified_formats;
    EGLBoolean *external_only;
    EGLuint64KHR *modifiers;
    EGLint *formats;
    EGLint egl_ok, n_modifiers;
    int n_formats, n_modified_formats, max_n_modifiers;

    egl_ok = egl_query_dmabuf_formats(display, 0, NULL, &n_formats);
    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Could not query number of dmabuf formats supported by EGL.\n");
        goto fail;
    }

    formats = malloc(n_formats * sizeof *formats);
    if (formats == NULL) {
        goto fail;
    }

    egl_ok = egl_query_dmabuf_formats(display, n_formats, formats, &n_formats);
    if (egl_ok != EGL_TRUE) {
        LOG_ERROR("Could not query dmabuf formats supported by EGL.\n");
        goto fail_free_formats;
    }

    // first, count how many modifiers we have for each format.
    n_modified_formats = 0;
    max_n_modifiers = 0;
    for (int i = 0; i < n_formats; i++) {
        egl_ok = egl_query_dmabuf_modifiers(display, formats[i], 0, NULL, NULL, &n_modifiers);
        if (egl_ok != EGL_TRUE) {
            LOG_ERROR("Could not query dmabuf formats supported by EGL.\n");
            goto fail_free_formats;
        }

        n_modified_formats += n_modifiers;

        if (n_modifiers > max_n_modifiers) {
            max_n_modifiers = n_modifiers;
        }
    }

    modified_formats = malloc(n_modified_formats * sizeof *modified_formats);
    if (modified_formats == NULL) {
        goto fail_free_formats;
    }

    modifiers = malloc(max_n_modifiers * sizeof *modifiers);
    if (modifiers == NULL) {
        goto fail_free_modified_formats;
    }

    external_only = malloc(max_n_modifiers * sizeof *external_only);
    if (external_only == NULL) {
        goto fail_free_modifiers;
    }

    LOG_DEBUG("supported formats for EGL import: ");
    for (int i = 0, j = 0; i < n_formats; i++) {
        egl_ok = egl_query_dmabuf_modifiers(display, formats[i], max_n_modifiers, modifiers, external_only, &n_modifiers);
        if (egl_ok != EGL_TRUE) {
            LOG_ERROR("Could not query dmabuf formats supported by EGL.\n");
            goto fail_free_formats;
        }

        LOG_DEBUG_UNPREFIXED("%" DRM_FOURCC_FORMAT ", ", DRM_FOURCC_ARGS(formats[i]));

        for (int k = 0; k < n_modifiers; k++, j++) {
            modified_formats[j].format = formats[i];
            modified_formats[j].modifier = modifiers[k];
            modified_formats[j].external_only = external_only[k];
        }
    }

    LOG_DEBUG_UNPREFIXED("\n");

    free(formats);
    free(modifiers);
    free(external_only);

    *n_formats_out = n_modified_formats;
    *formats_out = modified_formats;
    return true;

fail_free_modifiers:
    free(modifiers);

fail_free_modified_formats:
    free(modified_formats);

fail_free_formats:
    free(formats);

fail:
    *n_formats_out = 0;
    *formats_out = NULL;
    return false;
}
#endif

struct frame_interface *frame_interface_new(struct gl_renderer *renderer) {
    struct frame_interface *interface;
    struct gbm_device *gbm_device;
    EGLBoolean egl_ok;
    EGLContext context;
    EGLDisplay display;
    struct egl_modified_format *formats;
    bool supports_extended_imports, supports_external_target;
    int n_formats;

    interface = malloc(sizeof *interface);
    if (interface == NULL) {
        return NULL;
    }

    if (!gl_renderer_supports_egl_extension(renderer, "EGL_EXT_image_dma_buf_import")) {
        LOG_ERROR("EGL does not support EGL_EXT_image_dma_buf_import extension. Video frames cannot be uploaded.\n");
        goto fail_free;
    }

    if (gl_renderer_supports_egl_extension(renderer, "EGL_EXT_image_dma_buf_import_modifiers")) {
#ifndef EGL_EXT_image_dma_buf_import_modifiers
        LOG_ERROR(
            "EGL supports EGL_EXT_image_dma_buf_import_modifiers, "
            "but EGL headers didn't contain definitions for EGL_EXT_image_dma_buf_import_modifiers."
            "Extended imports and pixel format information will not be used.\n"
        );
        supports_extended_imports = false;
#else
        supports_extended_imports = true;
#endif
    } else {
        supports_extended_imports = false;
    }

    if (gl_renderer_supports_gl_extension(renderer, "GL_OES_EGL_image_external")) {
        supports_external_target = true;
    } else {
        supports_external_target = false;
    }

    display = gl_renderer_get_egl_display(renderer);
    if (display == EGL_NO_DISPLAY) {
        goto fail_free;
    }

    context = gl_renderer_create_context(renderer);
    if (context == EGL_NO_CONTEXT) {
        goto fail_free;
    }

    PFNEGLCREATEIMAGEKHRPROC create_image = (PFNEGLCREATEIMAGEKHRPROC) gl_renderer_get_proc_address(renderer, "eglCreateImageKHR");
    if (create_image == NULL) {
        LOG_ERROR("Could not resolve eglCreateImageKHR EGL procedure.\n");
        goto fail_destroy_context;
    }

    PFNEGLDESTROYIMAGEKHRPROC destroy_image = (PFNEGLDESTROYIMAGEKHRPROC) gl_renderer_get_proc_address(renderer, "eglDestroyImageKHR");
    if (destroy_image == NULL) {
        LOG_ERROR("Could not resolve eglDestroyImageKHR EGL procedure.\n");
        goto fail_destroy_context;
    }

    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_egl_image_target_texture2d = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC
    ) gl_renderer_get_proc_address(renderer, "glEGLImageTargetTexture2DOES");
    if (gl_egl_image_target_texture2d == NULL) {
        LOG_ERROR("Could not resolve glEGLImageTargetTexture2DOES EGL procedure.\n");
        goto fail_destroy_context;
    }

    // These two are optional.
    // Might be useful in the future.
#ifdef EGL_EXT_image_dma_buf_import_modifiers
    PFNEGLQUERYDMABUFFORMATSEXTPROC egl_query_dmabuf_formats = (PFNEGLQUERYDMABUFFORMATSEXTPROC
    ) gl_renderer_get_proc_address(renderer, "eglQueryDmaBufFormatsEXT");
    if (egl_query_dmabuf_formats == NULL && supports_extended_imports) {
        LOG_ERROR("Could not resolve eglQueryDmaBufFormatsEXT egl procedure, even though it is listed as supported.\n");
        supports_extended_imports = false;
    }

    PFNEGLQUERYDMABUFMODIFIERSEXTPROC egl_query_dmabuf_modifiers = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC
    ) gl_renderer_get_proc_address(renderer, "eglQueryDmaBufModifiersEXT");
    if (egl_query_dmabuf_modifiers == NULL && supports_extended_imports) {
        LOG_ERROR("Could not resolve eglQueryDmaBufModifiersEXT egl procedure, even though it is listed as supported.\n");
        supports_extended_imports = false;
    }
#endif

    gbm_device = gl_renderer_get_gbm_device(renderer);
    if (gbm_device == NULL) {
        LOG_ERROR("GL Render doesn't have a GBM device associated with it, which is necessary for importing the video frames.\n");
        goto fail_destroy_context;
    }

    if (supports_extended_imports) {
#ifdef EGL_EXT_image_dma_buf_import_modifiers
        query_formats(display, egl_query_dmabuf_formats, egl_query_dmabuf_modifiers, &n_formats, &formats);
#else
        UNREACHABLE();
#endif
    } else {
        n_formats = 0;
        formats = NULL;
    }

    interface->gbm_device = gbm_device;
    interface->display = display;
    pthread_mutex_init(&interface->context_lock, NULL);
    interface->context = context;
    interface->eglCreateImageKHR = create_image;
    interface->eglDestroyImageKHR = destroy_image;
    interface->supports_external_target = supports_external_target;
    interface->glEGLImageTargetTexture2DOES = gl_egl_image_target_texture2d;
    interface->supports_extended_imports = supports_extended_imports;
#ifdef EGL_EXT_image_dma_buf_import_modifiers
    interface->eglQueryDmaBufFormatsEXT = egl_query_dmabuf_formats;
    interface->eglQueryDmaBufModifiersEXT = egl_query_dmabuf_modifiers;
#endif
    interface->n_formats = n_formats;
    interface->formats = formats;
    interface->n_refs = REFCOUNT_INIT_1;
    return interface;

fail_destroy_context:
    egl_ok = eglDestroyContext(display, context);
    ASSERT_EGL_TRUE(egl_ok);
    (void) egl_ok;

fail_free:
    free(interface);
    return NULL;
}

void frame_interface_destroy(struct frame_interface *interface) {
    EGLBoolean egl_ok;

    pthread_mutex_destroy(&interface->context_lock);
    egl_ok = eglDestroyContext(interface->display, interface->context);
    ASSERT_EGL_TRUE(egl_ok);
    (void) egl_ok;
    if (interface->formats != NULL) {
        free(interface->formats);
    }
    free(interface);
}

ATTR_PURE int frame_interface_get_n_formats(struct frame_interface *interface) {
    return interface->n_formats;
}

ATTR_PURE const struct egl_modified_format *frame_interface_get_format(struct frame_interface *interface, int index) {
    assert(index < interface->n_formats);
    return interface->formats + index;
}

DEFINE_LOCK_OPS(frame_interface, context_lock)

DEFINE_REF_OPS(frame_interface, n_refs)

struct gbm_device *frame_interface_get_gbm_device(struct frame_interface *interface) {
    return interface->gbm_device;
}

bool egl_dmabuf_find_format(struct frame_interface *interface, uint32_t drm_format, uint64_t modifier, bool *external_only_out) {
    for_each_format_in_frame_interface(i, format, interface) {
        if (format->format == drm_format && format->modifier == modifier) {
            *external_only_out = format->external_only;
            return true;
        }
    }
    return false;
}

#ifdef EGL_EXT_image_dma_buf_import_modifiers
    #define MODIFIER_ATTRS(_n)                                        \
        { EGL_DMA_BUF_PLANE##_n##_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE##_n##_MODIFIER_HI_EXT }
static const EGLint plane_fd_attrs[EGL_DMABUF_MAX_PLANES] = {
    EGL_DMA_BUF_PLANE0_FD_EXT,
    EGL_DMA_BUF_PLANE1_FD_EXT,
    EGL_DMA_BUF_PLANE2_FD_EXT,
    EGL_DMA_BUF_PLANE3_FD_EXT,
};
static const EGLint plane_offset_attrs[EGL_DMABUF_MAX_PLANES] = {
    EGL_DMA_BUF_PLANE0_OFFSET_EXT,
    EGL_DMA_BUF_PLANE1_OFFSET_EXT,
    EGL_DMA_BUF_PLANE2_OFFSET_EXT,
    EGL_DMA_BUF_PLANE3_OFFSET_EXT,
};
static const EGLint plane_pitch_attrs[EGL_DMABUF_MAX_PLANES] = {
    EGL_DMA_BUF_PLANE0_PITCH_EXT,
    EGL_DMA_BUF_PLANE1_PITCH_EXT,
    EGL_DMA_BUF_PLANE2_PITCH_EXT,
    EGL_DMA_BUF_PLANE3_PITCH_EXT,
};
static const EGLint plane_modifier_attrs[EGL_DMABUF_MAX_PLANES][2] = {
    MODIFIER_ATTRS(0),
    MODIFIER_ATTRS(1),
    MODIFIER_ATTRS(2),
    MODIFIER_ATTRS(3),
};
    #define MAX_IMPORT_PLANES EGL_DMABUF_MAX_PLANES
#else
static const EGLint plane_fd_attrs[3] = { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT };
static const EGLint plane_offset_attrs[3] = { EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT };
static const EGLint plane_pitch_attrs[3] = { EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT };
    #define MAX_IMPORT_PLANES 3
#endif

int egl_dmabuf_import(struct frame_interface *interface, const struct egl_dmabuf_desc *desc, struct egl_dmabuf_image *out) {
#define PUT_ATTR(_key, _value)                            \
    do {                                                  \
        assert(attr_index + 2 <= ARRAY_SIZE(attributes)); \
        attributes[attr_index++] = (_key);                \
        attributes[attr_index++] = (_value);              \
    } while (false)
    EGLint attributes[2 * 7 + EGL_DMABUF_MAX_PLANES * 2 * 5 + 1];
    EGLImageKHR egl_image;
    EGLBoolean egl_ok;
    EGLint egl_error;
    GLuint texture;
    GLenum gl_error, target;
    int attr_index = 0;

    if (desc->n_planes < 1 || desc->n_planes > MAX_IMPORT_PLANES) {
        LOG_ERROR("The dmabuf has %d planes, which EGL cannot import here.\n", desc->n_planes);
        return -EINVAL;
    }

    if (desc->n_planes > 3 && !interface->supports_extended_imports) {
        LOG_ERROR(
            "The video frame has more than 3 planes but that can't be imported as a GL texture if EGL doesn't support the "
            "EGL_EXT_image_dma_buf_import_modifiers extension.\n"
        );
        return -ENOTSUP;
    }

    PUT_ATTR(EGL_WIDTH, desc->width);
    PUT_ATTR(EGL_HEIGHT, desc->height);
    PUT_ATTR(EGL_LINUX_DRM_FOURCC_EXT, desc->drm_format);

    // could be one of EGL_ITU_REC601_EXT, EGL_ITU_REC709_EXT or EGL_ITU_REC2020_EXT
    if (desc->color_space != EGL_NONE) {
        PUT_ATTR(EGL_YUV_COLOR_SPACE_HINT_EXT, desc->color_space);
    }
    if (desc->sample_range_hint != EGL_NONE) {
        PUT_ATTR(EGL_SAMPLE_RANGE_HINT_EXT, desc->sample_range_hint);
    }
    if (desc->horizontal_chroma_siting != EGL_NONE) {
        PUT_ATTR(EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT, desc->horizontal_chroma_siting);
    }
    if (desc->vertical_chroma_siting != EGL_NONE) {
        PUT_ATTR(EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT, desc->vertical_chroma_siting);
    }

    for (int i = 0; i < desc->n_planes; i++) {
        const struct egl_dmabuf_plane *plane = desc->planes + i;

        PUT_ATTR(plane_fd_attrs[i], plane->fd);
        PUT_ATTR(plane_offset_attrs[i], plane->offset);
        PUT_ATTR(plane_pitch_attrs[i], plane->pitch);
        if (plane->has_modifier) {
            if (!interface->supports_extended_imports) {
                LOG_ERROR(
                    "video frame buffer uses modified format but EGL doesn't support the EGL_EXT_image_dma_buf_import_modifiers "
                    "extension.\n"
                );
                return -ENOTSUP;
            }
#ifdef EGL_EXT_image_dma_buf_import_modifiers
            PUT_ATTR(plane_modifier_attrs[i][0], uint32_to_int32(plane->modifier & 0xFFFFFFFFlu));
            PUT_ATTR(plane_modifier_attrs[i][1], uint32_to_int32(plane->modifier >> 32));
#else
            UNREACHABLE();
#endif
        }
    }

    assert(attr_index < ARRAY_SIZE(attributes));
    attributes[attr_index++] = EGL_NONE;

    egl_image = interface->eglCreateImageKHR(interface->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attributes);
    if (egl_image == EGL_NO_IMAGE_KHR) {
        LOG_ERROR("Couldn't create EGL image from dmabuf.\n");
        return -EIO;
    }

    frame_interface_lock(interface);

    /// TODO: Make a single EGL context current for the whole lifetime of the
    /// importing thread, instead of making one current and clearing it after
    /// for the duration of this procedure, to save some cycles.
    egl_ok = eglMakeCurrent(interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, interface->context);
    if (egl_ok == EGL_FALSE) {
        egl_error = eglGetError();
        LOG_ERROR("Could not make EGL context current. eglMakeCurrent: %" PRId32 "\n", egl_error);
        goto fail_unlock_interface;
    }

    glGenTextures(1, &texture);
    if (texture == 0) {
        gl_error = glGetError();
        LOG_ERROR("Could not create GL texture. glGenTextures: %" PRIu32 "\n", gl_error);
        goto fail_clear_context;
    }

    if (desc->external_only) {
        target = GL_TEXTURE_EXTERNAL_OES;
    } else {
        target = GL_TEXTURE_2D;
    }

    glBindTexture(target, texture);

    interface->glEGLImageTargetTexture2DOES(target, egl_image);
    gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        LOG_ERROR("Couldn't attach EGL Image to OpenGL texture. glEGLImageTargetTexture2DOES: %" PRIu32 "\n", gl_error);
        goto fail_unbind_texture;
    }

    glBindTexture(target, 0);

    egl_ok = eglMakeCurrent(interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_ok == EGL_FALSE) {
        egl_error = eglGetError();
        LOG_ERROR("Could not clear EGL context. eglMakeCurrent: %" PRId32 "\n", egl_error);
        goto fail_delete_texture;
    }

    frame_interface_unlock(interface);

    out->image = egl_image;
    out->target = target;
    out->texture = texture;
    return 0;

fail_unbind_texture:
    glBindTexture(target, 0);

fail_delete_texture:
    glDeleteTextures(1, &texture);

fail_clear_context:
    eglMakeCurrent(interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

fail_unlock_interface:
    frame_interface_unlock(interface);
    interface->eglDestroyImageKHR(interface->display, egl_image);
    return -EIO;
#undef PUT_ATTR
}

void egl_dmabuf_release(struct frame_interface *interface, struct egl_dmabuf_image *image) {
    EGLBoolean egl_ok;

    frame_interface_lock(interface);
    egl_ok = eglMakeCurrent(interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, interface->context);
    ASSERT_EGL_TRUE(egl_ok);
    glDeleteTextures(1, &image->texture);
    assert(GL_NO_ERROR == glGetError());
    egl_ok = eglMakeCurrent(interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    ASSERT_EGL_TRUE(egl_ok);
    frame_interface_unlock(interface);

    egl_ok = interface->eglDestroyImageKHR(interface->display, image->image);
    ASSERT_EGL_TRUE(egl_ok);
    (void) egl_ok;
}

bool frame_interface_make_current(struct frame_interface *interface) {
    frame_interface_lock(interface);
    if (eglMakeCurrent(interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, interface->context) == EGL_FALSE) {
        LOG_ERROR("Could not make EGL context current. eglMakeCurrent: %" PRId32 "\n", (int32_t) eglGetError());
        frame_interface_unlock(interface);
        return false;
    }
    return true;
}

void frame_interface_clear_current(struct frame_interface *interface) {
    eglMakeCurrent(interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    frame_interface_unlock(interface);
}
