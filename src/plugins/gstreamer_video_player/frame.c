#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <gst/allocators/allocators.h>
#include <gst/video/video.h>

#include "egl_dmabuf.h"
#include "flutter-pi.h"
#include "texture_registry.h"

// This will error if we don't have EGL / OpenGL ES support.
#include "gl_renderer.h"
#include "plugins/gstreamer_video_player.h"
#include "util/logging.h"
#include "util/refcounting.h"

#define MAX_N_PLANES EGL_DMABUF_MAX_PLANES

#define GSTREAMER_VER(major, minor, patch) ((((major) &0xFF) << 16) | (((minor) &0xFF) << 8) | ((patch) &0xFF))
#define THIS_GSTREAMER_VER GSTREAMER_VER(LIBGSTREAMER_VERSION_MAJOR, LIBGSTREAMER_VERSION_MINOR, LIBGSTREAMER_VERSION_PATCH)

#define DRM_FOURCC_FORMAT "c%c%c%c"
#define DRM_FOURCC_ARGS(format) (format) & 0xFF, ((format) >> 8) & 0xFF, ((format) >> 16) & 0xFF, ((format) >> 24) & 0xFF

struct video_frame {
    GstSample *sample;

    struct frame_interface *interface;

    uint32_t drm_format;

    int n_dmabuf_fds;
    int dmabuf_fds[MAX_N_PLANES];

    struct egl_dmabuf_image image;
    size_t width, height;

    struct gl_texture_frame gl_frame;
};

/**
 * @brief Create a dmabuf fd from the given GstBuffer.
 *
 * Calls gst_buffer_map on the buffer, so buffer could have changed after the call.
 *
 */
UNUSED int dup_gst_buffer_range_as_dmabuf(struct gbm_device *gbm_device, GstBuffer *buffer, unsigned int memory_index, int n_memories) {
    struct gbm_bo *bo;
    GstMapInfo map_info;
    uint32_t stride;
    gboolean gst_ok;
    void *map, *map_data;
    int fd;

    gst_ok = gst_buffer_map_range(buffer, memory_index, n_memories, &map_info, GST_MAP_READ);
    if (gst_ok == FALSE) {
        LOG_ERROR("Couldn't map gstreamer video frame buffer to copy it into a dma buffer.\n");
        return -1;
    }

    bo = gbm_bo_create(gbm_device, map_info.size, 1, GBM_FORMAT_R8, GBM_BO_USE_LINEAR);
    if (bo == NULL) {
        LOG_ERROR("Couldn't create GBM BO to copy video frame into.\n");
        goto fail_unmap_buffer;
    }

    map_data = NULL;
    map = gbm_bo_map(bo, 0, 0, map_info.size, 1, GBM_BO_TRANSFER_WRITE, &stride, &map_data);
    if (map == NULL) {
        LOG_ERROR("Couldn't mmap GBM BO to copy video frame into it.\n");
        goto fail_destroy_bo;
    }

    memcpy(map, map_info.data, map_info.size);

    gbm_bo_unmap(bo, map_data);

    fd = gbm_bo_get_fd(bo);
    if (fd < 0) {
        LOG_ERROR("Couldn't filedescriptor of video frame GBM BO.\n");
        goto fail_destroy_bo;
    }

    /// TODO: Should we dup the fd before we destroy the bo?
    gbm_bo_destroy(bo);
    gst_buffer_unmap(buffer, &map_info);
    return fd;

fail_destroy_bo:
    gbm_bo_destroy(bo);

fail_unmap_buffer:
    gst_buffer_unmap(buffer, &map_info);
    return -1;
}

/**
 * @brief Create a dmabuf fd from the given GstMemory.
 *
 * Calls gst_memory_map on the memory.
 *
 */
UNUSED int dup_gst_memory_as_dmabuf(struct gbm_device *gbm_device, GstMemory *memory) {
    struct gbm_bo *bo;
    GstMapInfo map_info;
    uint32_t stride;
    gboolean gst_ok;
    void *map, *map_data;
    int fd;

    gst_ok = gst_memory_map(memory, &map_info, GST_MAP_READ);
    if (gst_ok == FALSE) {
        LOG_ERROR("Couldn't map gstreamer video frame memory to copy it into a dma buffer.\n");
        return -1;
    }

    bo = gbm_bo_create(gbm_device, map_info.size, 1, GBM_FORMAT_R8, GBM_BO_USE_LINEAR);
    if (bo == NULL) {
        LOG_ERROR("Couldn't create GBM BO to copy video frame into.\n");
        goto fail_unmap_buffer;
    }

    map_data = NULL;
    map = gbm_bo_map(bo, 0, 0, map_info.size, 1, GBM_BO_TRANSFER_WRITE, &stride, &map_data);
    if (map == NULL) {
        LOG_ERROR("Couldn't mmap GBM BO to copy video frame into it.\n");
        goto fail_destroy_bo;
    }

    memcpy(map, map_info.data, map_info.size);

    gbm_bo_unmap(bo, map_data);

    fd = gbm_bo_get_fd(bo);
    if (fd < 0) {
        LOG_ERROR("Couldn't filedescriptor of video frame GBM BO.\n");
        goto fail_destroy_bo;
    }

    /// TODO: Should we dup the fd before we destroy the bo?
    gbm_bo_destroy(bo);
    gst_memory_unmap(memory, &map_info);
    return fd;

fail_destroy_bo:
    gbm_bo_destroy(bo);

fail_unmap_buffer:
    gst_memory_unmap(memory, &map_info);
    return -1;
}

struct plane_info {
    int fd;
    uint32_t offset;
    uint32_t pitch;
    bool has_modifier;
    uint64_t modifier;
};

#if THIS_GSTREAMER_VER < GSTREAMER_VER(1, 14, 0)
    #error "Unsupported gstreamer version."
#endif

#if THIS_GSTREAMER_VER >= GSTREAMER_VER(1, 18, 0)
static bool get_plane_sizes_from_meta(const GstVideoMeta *meta, size_t plane_sizes_out[4]) {
    GstVideoMeta *meta_non_const;
    gboolean gst_ok;

    #ifdef DEBUG
    GstVideoMeta _meta_non_const;
    meta_non_const = &_meta_non_const;
    memcpy(meta_non_const, meta, sizeof *meta);
    #else
    meta_non_const = (GstVideoMeta *) meta;
    #endif

    gst_ok = gst_video_meta_get_plane_size(meta_non_const, plane_sizes_out);
    if (gst_ok != TRUE) {
        LOG_ERROR("Could not query video frame plane size. gst_video_meta_get_plane_size\n");
        return false;
    }

    return true;
}

static bool get_plane_sizes_from_video_info(const GstVideoInfo *info, size_t plane_sizes_out[4]) {
    GstVideoAlignment alignment;
    GstVideoInfo *info_non_const;
    gboolean gst_ok;

    gst_video_alignment_reset(&alignment);

    #ifdef DEBUG
    info_non_const = gst_video_info_copy(info);
    #else
    info_non_const = (GstVideoInfo *) info;
    #endif

    gst_ok = gst_video_info_align_full(info_non_const, &alignment, plane_sizes_out);
    if (gst_ok != TRUE) {
        LOG_ERROR("Could not query video frame plane size. gst_video_info_align_full\n");
        return false;
    }

    #ifdef DEBUG
    assert(gst_video_info_is_equal(info, info_non_const));
    gst_video_info_free(info_non_const);
    #endif

    return true;
}

static bool calculate_plane_size(const GstVideoInfo *info, int plane_index, size_t *plane_size_out) {
    // Taken from: https://github.com/GStreamer/gstreamer/blob/621604aa3e4caa8db27637f63fa55fac2f7721e5/subprojects/gst-plugins-base/gst-libs/gst/video/video-info.c#L1278-L1301

    #if THIS_GSTREAMER_VER >= GSTREAMER_VER(1, 21, 3)
    if (GST_VIDEO_FORMAT_INFO_IS_TILED(info->finfo)) {
        guint x_tiles = GST_VIDEO_TILE_X_TILES(info->stride[plane_index]);
        guint y_tiles = GST_VIDEO_TILE_Y_TILES(info->stride[plane_index]);
        *plane_size_out = x_tiles * y_tiles * GST_VIDEO_FORMAT_INFO_TILE_SIZE(info->finfo, plane_index);
        return true;
    }
    #endif

    gint comp[GST_VIDEO_MAX_COMPONENTS];
    guint plane_height;

    /* Convert plane index to component index */
    gst_video_format_info_component(info->finfo, plane_index, comp);
    plane_height = GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT(info->finfo, comp[0], GST_VIDEO_INFO_FIELD_HEIGHT(info));

    *plane_size_out = plane_height * GST_VIDEO_INFO_PLANE_STRIDE(info, plane_index);
    return true;
}

#else
static bool get_plane_sizes_from_meta(UNUSED const GstVideoMeta *meta, UNUSED size_t plane_sizes_out[4]) {
    return false;
}
static bool get_plane_sizes_from_video_info(UNUSED const GstVideoInfo *info, UNUSED size_t plane_sizes_out[4]) {
    return false;
}
static bool calculate_plane_size(UNUSED const GstVideoInfo *info, UNUSED int plane_index, UNUSED size_t *plane_size_out) {
    return false;
}
#endif

static int
get_plane_infos(GstBuffer *buffer, const GstVideoInfo *info, struct gbm_device *gbm_device, struct plane_info plane_infos[MAX_N_PLANES]) {
    GstVideoMeta *meta;
    GstMemory *memory;
    gboolean gst_ok;
    size_t plane_sizes[4] = { 0 };
    bool has_plane_sizes;
    int n_planes;

    n_planes = GST_VIDEO_INFO_N_PLANES(info);

    // There's so many ways to get the plane sizes.
    // 1. Preferably we should use the video meta.
    // 2. If that doesn't work, we'll use gst_video_info_align_full() with the video info.
    // 3. If that doesn't work, we'll calculate them ourselves.
    // 4. If that doesn't work, we can't determine the plane sizes.
    //    In that case, we'll error if we have more than one plane.
    has_plane_sizes = false;
    meta = gst_buffer_get_video_meta(buffer);
    if (meta != NULL) {
        has_plane_sizes = get_plane_sizes_from_meta(meta, plane_sizes);
    }

    if (!has_plane_sizes) {
        has_plane_sizes = get_plane_sizes_from_video_info(info, plane_sizes);
    }

    if (!has_plane_sizes) {
        has_plane_sizes = true;
        for (int i = 0; i < n_planes; i++) {
            has_plane_sizes = has_plane_sizes && calculate_plane_size(info, i, plane_sizes + i);
        }
    }

    if (!has_plane_sizes) {
        // We couldn't determine the plane sizes.
        // We can still continue if we have only one plane.
        if (n_planes != 1) {
            LOG_ERROR(
                "Couldn't determine video frame plane sizes. Without plane sizes, only planar framebuffer formats are supported, but the "
                "supplied format was not planar.\n"
            );
            return EINVAL;
        }

        // We'll just assume the first plane spans the entire buffer.
        plane_sizes[0] = gst_buffer_get_size(buffer);
        has_plane_sizes = true;
    }

    for (int i = 0; i < n_planes; i++) {
        size_t offset_in_memory = 0;
        size_t offset_in_buffer = 0;
        unsigned memory_index = 0;
        unsigned n_memories = 0;
        int stride, ok;

        if (meta) {
            offset_in_buffer = meta->offset[i];
            stride = meta->stride[i];
        } else {
            offset_in_buffer = GST_VIDEO_INFO_PLANE_OFFSET(info, i);
            stride = GST_VIDEO_INFO_PLANE_STRIDE(info, i);
        }

        gst_ok = gst_buffer_find_memory(buffer, offset_in_buffer, plane_sizes[i], &memory_index, &n_memories, &offset_in_memory);
        if (gst_ok != TRUE) {
            LOG_ERROR("Could not find video frame memory for plane.\n");
            ok = EIO;
            goto fail_close_fds;
        }

        if (n_memories != 1) {
            ok = dup_gst_buffer_range_as_dmabuf(gbm_device, buffer, memory_index, n_memories);
            if (ok < 0) {
                LOG_ERROR("Could not duplicate gstreamer buffer range as dmabuf.\n");
                ok = EIO;
                goto fail_close_fds;
            }

            plane_infos[i].fd = ok;
        } else {
            memory = gst_buffer_peek_memory(buffer, memory_index);
            if (gst_is_dmabuf_memory(memory)) {
                ok = gst_dmabuf_memory_get_fd(memory);
                if (ok < 0) {
                    LOG_ERROR("Could not get gstreamer memory as dmabuf.\n");
                    ok = EIO;
                    goto fail_close_fds;
                }

                ok = dup(ok);
                if (ok < 0) {
                    ok = errno;
                    LOG_ERROR("Could not dup fd. dup: %s\n", strerror(ok));
                    goto fail_close_fds;
                }

                plane_infos[i].fd = ok;
            } else {
                /// TODO: When duping, duplicate all non-dmabuf memories into one
                /// gbm buffer instead.
                ok = dup_gst_memory_as_dmabuf(gbm_device, memory);
                if (ok < 0) {
                    LOG_ERROR("Could not duplicate gstreamer memory as dmabuf.\n");
                    ok = EIO;
                    goto fail_close_fds;
                }

                plane_infos[i].fd = ok;
            }

            offset_in_memory += memory->offset;
        }

        plane_infos[i].offset = offset_in_memory;
        plane_infos[i].pitch = stride;

        /// TODO: Detect modifiers here
        /// Modifiers will be supported in future gstreamer, see:
        /// https://gstreamer.freedesktop.org/documentation/additional/design/dmabuf.html?gi-language=c
        plane_infos[i].has_modifier = false;
        plane_infos[i].modifier = DRM_FORMAT_MOD_LINEAR;
        continue;

fail_close_fds:
        for (int j = i - 1; j > 0; j--) {
            close(plane_infos[i].fd);
        }
        return ok;
    }

    return 0;
}

static uint32_t drm_format_from_gst_info(const GstVideoInfo *info) {
    switch (GST_VIDEO_INFO_FORMAT(info)) {
        case GST_VIDEO_FORMAT_YUY2: return DRM_FORMAT_YUYV;
        case GST_VIDEO_FORMAT_YVYU: return DRM_FORMAT_YVYU;
        case GST_VIDEO_FORMAT_UYVY: return DRM_FORMAT_UYVY;
        case GST_VIDEO_FORMAT_VYUY: return DRM_FORMAT_VYUY;
        case GST_VIDEO_FORMAT_AYUV: return DRM_FORMAT_AYUV;
#if THIS_GSTREAMER_VER >= GSTREAMER_VER(1, 16, 0)
        case GST_VIDEO_FORMAT_VUYA: return DRM_FORMAT_AYUV;
#endif
        case GST_VIDEO_FORMAT_NV12: return DRM_FORMAT_NV12;
        case GST_VIDEO_FORMAT_NV21: return DRM_FORMAT_NV21;
        case GST_VIDEO_FORMAT_NV16: return DRM_FORMAT_NV16;
        case GST_VIDEO_FORMAT_NV61: return DRM_FORMAT_NV61;
        case GST_VIDEO_FORMAT_NV24: return DRM_FORMAT_NV24;
        case GST_VIDEO_FORMAT_YUV9: return DRM_FORMAT_YUV410;
        case GST_VIDEO_FORMAT_YVU9: return DRM_FORMAT_YVU410;
        case GST_VIDEO_FORMAT_Y41B: return DRM_FORMAT_YUV411;
        case GST_VIDEO_FORMAT_I420: return DRM_FORMAT_YUV420;
        case GST_VIDEO_FORMAT_YV12: return DRM_FORMAT_YVU420;
        case GST_VIDEO_FORMAT_Y42B: return DRM_FORMAT_YUV422;
        case GST_VIDEO_FORMAT_Y444: return DRM_FORMAT_YUV444;
        case GST_VIDEO_FORMAT_RGB16: return DRM_FORMAT_RGB565;
        case GST_VIDEO_FORMAT_BGR16: return DRM_FORMAT_BGR565;
        case GST_VIDEO_FORMAT_RGBA: return DRM_FORMAT_ABGR8888;
        case GST_VIDEO_FORMAT_RGBx: return DRM_FORMAT_XBGR8888;
        case GST_VIDEO_FORMAT_BGRA: return DRM_FORMAT_ARGB8888;
        case GST_VIDEO_FORMAT_BGRx: return DRM_FORMAT_XRGB8888;
        case GST_VIDEO_FORMAT_ARGB: return DRM_FORMAT_BGRA8888;
        case GST_VIDEO_FORMAT_xRGB: return DRM_FORMAT_BGRX8888;
        case GST_VIDEO_FORMAT_ABGR: return DRM_FORMAT_RGBA8888;
        case GST_VIDEO_FORMAT_xBGR: return DRM_FORMAT_RGBX8888;
        default: return DRM_FORMAT_INVALID;
    }
}

ATTR_CONST GstVideoFormat gst_video_format_from_drm_format(uint32_t drm_format) {
    switch (drm_format) {
        case DRM_FORMAT_YUYV: return GST_VIDEO_FORMAT_YUY2;
        case DRM_FORMAT_YVYU: return GST_VIDEO_FORMAT_YVYU;
        case DRM_FORMAT_UYVY: return GST_VIDEO_FORMAT_UYVY;
        case DRM_FORMAT_VYUY: return GST_VIDEO_FORMAT_VYUY;
        case DRM_FORMAT_AYUV: return GST_VIDEO_FORMAT_AYUV;
#if THIS_GSTREAMER_VER >= GSTREAMER_VER(1, 16, 0)
            // GST_VIDEO_FORMAT_AYUV and _VUYA both map to DRM_FORMAT_AYUV
            // case DRM_FORMAT_AYUV: return GST_VIDEO_FORMAT_VUYA;
#endif
        case DRM_FORMAT_NV12: return GST_VIDEO_FORMAT_NV12;
        case DRM_FORMAT_NV21: return GST_VIDEO_FORMAT_NV21;
        case DRM_FORMAT_NV16: return GST_VIDEO_FORMAT_NV16;
        case DRM_FORMAT_NV61: return GST_VIDEO_FORMAT_NV61;
        case DRM_FORMAT_NV24: return GST_VIDEO_FORMAT_NV24;
        case DRM_FORMAT_YUV410: return GST_VIDEO_FORMAT_YUV9;
        case DRM_FORMAT_YVU410: return GST_VIDEO_FORMAT_YVU9;
        case DRM_FORMAT_YUV411: return GST_VIDEO_FORMAT_Y41B;
        case DRM_FORMAT_YUV420: return GST_VIDEO_FORMAT_I420;
        case DRM_FORMAT_YVU420: return GST_VIDEO_FORMAT_YV12;
        case DRM_FORMAT_YUV422: return GST_VIDEO_FORMAT_Y42B;
        case DRM_FORMAT_YUV444: return GST_VIDEO_FORMAT_Y444;
        case DRM_FORMAT_RGB565: return GST_VIDEO_FORMAT_RGB16;
        case DRM_FORMAT_BGR565: return GST_VIDEO_FORMAT_BGR16;
        case DRM_FORMAT_ABGR8888: return GST_VIDEO_FORMAT_RGBA;
        case DRM_FORMAT_XBGR8888: return GST_VIDEO_FORMAT_RGBx;
        case DRM_FORMAT_ARGB8888: return GST_VIDEO_FORMAT_BGRA;
        case DRM_FORMAT_XRGB8888: return GST_VIDEO_FORMAT_BGRx;
        case DRM_FORMAT_BGRA8888: return GST_VIDEO_FORMAT_ARGB;
        case DRM_FORMAT_BGRX8888: return GST_VIDEO_FORMAT_xRGB;
        case DRM_FORMAT_RGBA8888: return GST_VIDEO_FORMAT_ABGR;
        case DRM_FORMAT_RGBX8888: return GST_VIDEO_FORMAT_xBGR;
        default: return GST_VIDEO_FORMAT_UNKNOWN;
    }
}

static EGLint egl_color_space_from_gst_info(const GstVideoInfo *info) {
    if (gst_video_colorimetry_matches(&GST_VIDEO_INFO_COLORIMETRY(info), GST_VIDEO_COLORIMETRY_BT601)) {
        return EGL_ITU_REC601_EXT;
    } else if (gst_video_colorimetry_matches(&GST_VIDEO_INFO_COLORIMETRY(info), GST_VIDEO_COLORIMETRY_BT709)) {
        return EGL_ITU_REC709_EXT;
    } else if (gst_video_colorimetry_matches(&GST_VIDEO_INFO_COLORIMETRY(info), GST_VIDEO_COLORIMETRY_BT2020)) {
        return EGL_ITU_REC2020_EXT;
    } else {
        LOG_DEBUG("Unsupported video colorimetry: %s\n", gst_video_colorimetry_to_string(&GST_VIDEO_INFO_COLORIMETRY(info)));
        return EGL_NONE;
    }
}

static EGLint egl_sample_range_hint_from_gst_info(const GstVideoInfo *info) {
    if (GST_VIDEO_INFO_COLORIMETRY(info).range == GST_VIDEO_COLOR_RANGE_0_255) {
        return EGL_YUV_FULL_RANGE_EXT;
    } else if (GST_VIDEO_INFO_COLORIMETRY(info).range == GST_VIDEO_COLOR_RANGE_16_235) {
        return EGL_YUV_NARROW_RANGE_EXT;
    } else {
        return EGL_NONE;
    }
}

static EGLint egl_horizontal_chroma_siting_from_gst_info(const GstVideoInfo *info) {
    GstVideoChromaSite chroma_site = GST_VIDEO_INFO_CHROMA_SITE(info);

    if (chroma_site == GST_VIDEO_CHROMA_SITE_H_COSITED || chroma_site == GST_VIDEO_CHROMA_SITE_COSITED) {
        return EGL_YUV_CHROMA_SITING_0_EXT;
    } else if (chroma_site == GST_VIDEO_CHROMA_SITE_V_COSITED || chroma_site == GST_VIDEO_CHROMA_SITE_NONE) {
        return EGL_YUV_CHROMA_SITING_0_5_EXT;
    } else {
        return EGL_NONE;
    }
}

static EGLint egl_vertical_chroma_siting_from_gst_info(const GstVideoInfo *info) {
    GstVideoChromaSite chroma_site = GST_VIDEO_INFO_CHROMA_SITE(info);

    if (chroma_site == GST_VIDEO_CHROMA_SITE_V_COSITED || chroma_site == GST_VIDEO_CHROMA_SITE_COSITED) {
        return EGL_YUV_CHROMA_SITING_0_EXT;
    } else if (chroma_site == GST_VIDEO_CHROMA_SITE_H_COSITED || chroma_site == GST_VIDEO_CHROMA_SITE_NONE) {
        return EGL_YUV_CHROMA_SITING_0_5_EXT;
    } else {
        return EGL_NONE;
    }
}

struct video_frame *frame_new(struct frame_interface *interface, GstSample *sample, const GstVideoInfo *info) {
    struct video_frame *frame;
    struct plane_info planes[MAX_N_PLANES];
    struct egl_dmabuf_desc desc;
    struct egl_dmabuf_image image;
    GstVideoInfo _info;
    GstBuffer *buffer;
    gboolean gst_ok;
    uint32_t drm_format;
    GstCaps *caps;
    int ok, width, height, n_planes;

    buffer = gst_sample_get_buffer(sample);
    if (buffer == NULL) {
        LOG_ERROR("Could not get buffer from video sample.\n");
        return NULL;
    }

    // If we don't have an explicit info given, we determine it from the sample caps.
    if (info == NULL) {
        caps = gst_sample_get_caps(sample);
        if (caps == NULL) {
            return NULL;
        }

        info = &_info;

        gst_ok = gst_video_info_from_caps(&_info, caps);
        if (gst_ok == FALSE) {
            LOG_ERROR("Could not get video info from video sample caps.\n");
            return NULL;
        }
    } else {
        caps = NULL;
    }

    // Determine some basic frame info.
    width = GST_VIDEO_INFO_WIDTH(info);
    height = GST_VIDEO_INFO_HEIGHT(info);
    n_planes = GST_VIDEO_INFO_N_PLANES(info);

    // query the drm format for this sample
    drm_format = drm_format_from_gst_info(info);
    if (drm_format == DRM_FORMAT_INVALID) {
        LOG_ERROR("Video format has no EGL equivalent.\n");
        return NULL;
    }

    bool external_only;
    if (!egl_dmabuf_find_format(interface, drm_format, DRM_FORMAT_MOD_LINEAR, &external_only)) {
        LOG_ERROR(
            "Video format is not supported by EGL: %" DRM_FOURCC_FORMAT " (modifier: %" PRIu64 ").\n",
            DRM_FOURCC_ARGS(drm_format),
            (uint64_t) DRM_FORMAT_MOD_LINEAR
        );
        return NULL;
    }

    frame = malloc(sizeof *frame);
    if (frame == NULL) {
        return NULL;
    }

    ok = get_plane_infos(buffer, info, frame_interface_get_gbm_device(interface), planes);
    if (ok != 0) {
        goto fail_free_frame;
    }

    desc = (struct egl_dmabuf_desc){
        .width = width,
        .height = height,
        .drm_format = drm_format,
        .n_planes = n_planes,
        // the color space, sample range and chroma siting of this sample
        .color_space = egl_color_space_from_gst_info(info),
        .sample_range_hint = egl_sample_range_hint_from_gst_info(info),
        .horizontal_chroma_siting = egl_horizontal_chroma_siting_from_gst_info(info),
        .vertical_chroma_siting = egl_vertical_chroma_siting_from_gst_info(info),
        .external_only = external_only,
    };
    for (int i = 0; i < n_planes; i++) {
        desc.planes[i] = (struct egl_dmabuf_plane){
            .fd = planes[i].fd,
            .offset = planes[i].offset,
            .pitch = planes[i].pitch,
            .has_modifier = planes[i].has_modifier,
            .modifier = planes[i].modifier,
        };
    }

    ok = egl_dmabuf_import(interface, &desc, &image);
    if (ok != 0) {
        goto fail_release_planes;
    }

    frame->sample = gst_sample_ref(sample);
    frame->interface = frame_interface_ref(interface);
    frame->drm_format = drm_format;
    frame->n_dmabuf_fds = n_planes;
    frame->dmabuf_fds[0] = planes[0].fd;
    frame->dmabuf_fds[1] = planes[1].fd;
    frame->dmabuf_fds[2] = planes[2].fd;
    frame->dmabuf_fds[3] = planes[3].fd;
    frame->image = image;
    frame->gl_frame.target = image.target;
    frame->gl_frame.name = image.texture;
    frame->gl_frame.format = GL_RGBA8_OES;
    frame->gl_frame.width = 0;
    frame->gl_frame.height = 0;
    return frame;

fail_release_planes:
    for (int i = 0; i < n_planes; i++)
        close(planes[i].fd);

fail_free_frame:
    free(frame);
    return NULL;
}

void frame_destroy(struct video_frame *frame) {
    int ok;

    egl_dmabuf_release(frame->interface, &frame->image);
    frame_interface_unref(frame->interface);
    for (int i = 0; i < frame->n_dmabuf_fds; i++) {
        ok = close(frame->dmabuf_fds[i]);
        assert(ok == 0);
        (void) ok;
    }

    gst_sample_unref(frame->sample);
    free(frame);
}

const struct gl_texture_frame *frame_get_gl_frame(struct video_frame *frame) {
    return &frame->gl_frame;
}
