/*
 * Static PNG Source - outputs a constant RGBA frame at a fixed framerate
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gststaticpngsrc.h"

#include <gst/base/gstbasesrc.h>
#include <gst/base/gstpushsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <jpeglib.h>
#include <png.h>

GST_DEBUG_CATEGORY_STATIC(gst_static_png_src_debug_category);
#define GST_CAT_DEFAULT gst_static_png_src_debug_category

/* Properties */
enum
{
    PROP_0,
    PROP_LOCATION,
    PROP_FPS,
    PROP_WIDTH,
    PROP_HEIGHT
};

/* Src pad template: allows negotiation while enabling fixed RGBA output */
static GstStaticPadTemplate gst_static_png_src_template =
    GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS,
                            GST_STATIC_CAPS("video/x-raw, "
                                            "format=(string){ RGBA, BGRA, ARGB, ABGR, NV12, I420 }, "
                                            "width=(int)[1,8192], "
                                            "height=(int)[1,8192], "
                                            "framerate=(fraction)[1/1,60/1]"));

struct _GstStaticPngSrc
{
    GstPushSrc parent;

    gchar* location;
    gchar selected_format[5];
    gint target_width;
    gint target_height;
    gint fps_n;
    gint fps_d;

    /* Original decoded RGBA (scaled), used as conversion source */
    guint8* rgba_data;
    gsize rgba_size;
    gint rgba_stride;

    /* Output buffer (may be YUV or RGBA variant) */
    guint8* frame_data;
    gsize frame_size;
    gint frame_stride;
    gint actual_width;
    gint actual_height;
    gint num_planes;

    GstMemory* shared_mem;
    guint64 frame_count;
    GstClockTime frame_duration;
};

G_DEFINE_TYPE_WITH_CODE(GstStaticPngSrc, gst_static_png_src, GST_TYPE_PUSH_SRC,
                        GST_DEBUG_CATEGORY_INIT(gst_static_png_src_debug_category, "staticpngsrc", 0,
                                                "debug category for the staticpngsrc element"));

/* Forward declarations */
static void gst_static_png_src_set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec);
static void gst_static_png_src_get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec);
static void gst_static_png_src_dispose(GObject* object);
static gboolean gst_static_png_src_start(GstBaseSrc* src);
static gboolean gst_static_png_src_stop(GstBaseSrc* src);
static GstFlowReturn gst_static_png_src_create(GstPushSrc* src, GstBuffer** buf);

static gboolean decode_png_to_rgba(const gchar* path, guint8** out_pixels, gint* out_w, gint* out_h);
static gboolean decode_jpeg_to_rgba(const gchar* path, guint8** out_pixels, gint* out_w, gint* out_h);
static guint8* scale_rgba_nearest(const guint8* src, gint src_w, gint src_h, gint dst_w, gint dst_h);
static void swizzle_from_rgba_inplace(guint8* pixels, gint width, gint height, const gchar* fmt);
static guint8* convert_rgba_to_nv12(const guint8* src, gint width, gint height);
static guint8* convert_rgba_to_i420(const guint8* src, gint width, gint height);

/* Local safe memdup to avoid runtime dependency on g_memdup2/g_memdup */
static inline gpointer memdup_fallback(const void* src, gsize size)
{
    if (src == NULL || size == 0)
    {
        return NULL;
    }

    gpointer dst = g_malloc(size);
    if (dst == NULL)
    {
        return NULL;
    }

    memcpy(dst, src, size);
    return dst;
}

/* GObject methods */
static void gst_static_png_src_class_init(GstStaticPngSrcClass* klass)
{
    GObjectClass* gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass* element_class = GST_ELEMENT_CLASS(klass);
    GstPushSrcClass* pushsrc_class = GST_PUSH_SRC_CLASS(klass);
    GstBaseSrcClass* base_src_class = GST_BASE_SRC_CLASS(klass);

    gst_element_class_add_static_pad_template(element_class, &gst_static_png_src_template);
    gst_element_class_set_static_metadata(element_class, "Static Image Source", "Source/Video",
                                          "Outputs a static RGBA image from a PNG or JPEG at a fixed framerate",
                                          "MTData");

    gobject_class->set_property = gst_static_png_src_set_property;
    gobject_class->get_property = gst_static_png_src_get_property;
    gobject_class->dispose = gst_static_png_src_dispose;

    g_object_class_install_property(gobject_class, PROP_LOCATION,
                                    g_param_spec_string("location", "location",
                                                        "Location of the image to load (png, jpg/jpeg/jpp)", NULL,
                                                        (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_FPS,
                                    gst_param_spec_fraction("fps", "fps", "Output framerate as a fraction", 1, 1, 60, 1,
                                                            25, 1,
                                                            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_WIDTH,
                                    g_param_spec_int("width", "width",
                                                     "Optional output width (scales image once if set)", 0, 8192, 0,
                                                     (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_HEIGHT,
                                    g_param_spec_int("height", "height",
                                                     "Optional output height (scales image once if set)", 0, 8192, 0,
                                                     (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    base_src_class->start = gst_static_png_src_start;
    base_src_class->stop = gst_static_png_src_stop;
    pushsrc_class->create = gst_static_png_src_create;
}

static void gst_static_png_src_init(GstStaticPngSrc* self)
{
    self->location = NULL;
    g_strlcpy(self->selected_format, "RGBA", sizeof(self->selected_format));
    self->target_width = 0;
    self->target_height = 0;
    self->fps_n = 25;
    self->fps_d = 1;
    self->rgba_data = NULL;
    self->rgba_size = 0;
    self->rgba_stride = 0;
    self->frame_data = NULL;
    self->frame_size = 0;
    self->frame_stride = 0;
    self->actual_width = 0;
    self->actual_height = 0;
    self->num_planes = 1;
    self->shared_mem = NULL;
    self->frame_count = 0;
    self->frame_duration = gst_util_uint64_scale_int(GST_SECOND, self->fps_d, self->fps_n);

    gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
    gst_base_src_set_live(GST_BASE_SRC(self), FALSE);
}

static void gst_static_png_src_dispose(GObject* object)
{
    GstStaticPngSrc* self = GST_STATICPNG_SRC(object);

    if (self->shared_mem != NULL)
    {
        gst_memory_unref(self->shared_mem);
        self->shared_mem = NULL;
    }

    if (self->location != NULL)
    {
        g_free(self->location);
        self->location = NULL;
    }

    G_OBJECT_CLASS(gst_static_png_src_parent_class)->dispose(object);
}

static void gst_static_png_src_set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec)
{
    GstStaticPngSrc* self = GST_STATICPNG_SRC(object);

    switch (prop_id)
    {
        case PROP_LOCATION:
        {
            const gchar* str = g_value_get_string(value);
            g_free(self->location);
            self->location = str != NULL ? g_strdup(str) : NULL;
            break;
        }
        case PROP_FPS:
        {
            self->fps_n = gst_value_get_fraction_numerator(value);
            self->fps_d = gst_value_get_fraction_denominator(value);
            if (self->fps_n <= 0 || self->fps_d <= 0)
            {
                self->fps_n = 25;
                self->fps_d = 1;
            }
            self->frame_duration = gst_util_uint64_scale_int(GST_SECOND, self->fps_d, self->fps_n);
            break;
        }
        case PROP_WIDTH:
        {
            self->target_width = g_value_get_int(value);
            break;
        }
        case PROP_HEIGHT:
        {
            self->target_height = g_value_get_int(value);
            break;
        }
        default:
        {
            G_OBJECT_CLASS(gst_static_png_src_parent_class)->set_property(object, prop_id, value, pspec);
            break;
        }
    }
}

static void gst_static_png_src_get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec)
{
    GstStaticPngSrc* self = GST_STATICPNG_SRC(object);

    switch (prop_id)
    {
        case PROP_LOCATION:
        {
            g_value_set_string(value, self->location);
            break;
        }
        case PROP_FPS:
        {
            gst_value_set_fraction(value, self->fps_n, self->fps_d);
            break;
        }
        case PROP_WIDTH:
        {
            g_value_set_int(value, self->target_width);
            break;
        }
        case PROP_HEIGHT:
        {
            g_value_set_int(value, self->target_height);
            break;
        }
        default:
        {
            G_OBJECT_CLASS(gst_static_png_src_parent_class)->get_property(object, prop_id, value, pspec);
            break;
        }
    }
}

static gboolean gst_static_png_src_start(GstBaseSrc* src)
{
    GstStaticPngSrc* self = GST_STATICPNG_SRC(src);

    if (self->location == NULL || self->location[0] == '\0')
    {
        GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND, ("'location' property not set"), (NULL));
        return FALSE;
    }

    /* Decode image (by file extension) to RGBA */
    guint8* decoded = NULL;
    gint img_w = 0;
    gint img_h = 0;

    gboolean decoded_ok = FALSE;
    if (self->location != NULL)
    {
        const gchar* dot = strrchr(self->location, '.');
        if (dot != NULL && *(dot + 1) != '\0')
        {
            gchar* ext = g_ascii_strdown(dot + 1, -1);
            if (ext != NULL)
            {
                if (g_strcmp0(ext, "png") == 0)
                {
                    decoded_ok = decode_png_to_rgba(self->location, &decoded, &img_w, &img_h);
                }
                else if (g_strcmp0(ext, "jpg") == 0 || g_strcmp0(ext, "jpeg") == 0 || g_strcmp0(ext, "jpp") == 0)
                {
                    decoded_ok = decode_jpeg_to_rgba(self->location, &decoded, &img_w, &img_h);
                }
                g_free(ext);
            }
        }
    }
    if (!decoded_ok)
    {
        GST_ELEMENT_ERROR(self, RESOURCE, READ,
                          ("Failed to decode image at '%s' (supported: png, jpg/jpeg/jpp)", self->location), (NULL));
        return FALSE;
    }

    /* Determine output dimensions */
    gint out_w = img_w;
    gint out_h = img_h;

    /* Prefer downstream fixed caps if any */
    GstPad* srcpad = gst_element_get_static_pad(GST_ELEMENT(self), "src");
    GstCaps* peer_caps = NULL;
    if (srcpad != NULL)
    {
        peer_caps = gst_pad_peer_query_caps(srcpad, NULL);
        gst_object_unref(srcpad);
    }
    if (peer_caps != NULL)
    {
        for (guint i = 0; i < gst_caps_get_size(peer_caps); ++i)
        {
            const GstStructure* s = gst_caps_get_structure(peer_caps, i);
            gint w = 0;
            gint h = 0;
            const gchar* fmt = gst_structure_get_string(s, "format");
            if (gst_structure_get_int(s, "width", &w) && gst_structure_get_int(s, "height", &h))
            {
                if (w > 0 && h > 0)
                {
                    out_w = w;
                    out_h = h;
                }
            }
            if (fmt != NULL)
            {
                if (g_strcmp0(fmt, "RGBA") == 0 || g_strcmp0(fmt, "BGRA") == 0 || g_strcmp0(fmt, "ARGB") == 0 ||
                    g_strcmp0(fmt, "ABGR") == 0)
                {
                    g_strlcpy(self->selected_format, fmt, sizeof(self->selected_format));
                }
            }
        }
        gst_caps_unref(peer_caps);
    }

    /* Otherwise properties if provided */
    if (self->target_width > 0 && self->target_height > 0)
    {
        out_w = self->target_width;
        out_h = self->target_height;
    }

    guint8* final_pixels = NULL;
    if (out_w != img_w || out_h != img_h)
    {
        final_pixels = scale_rgba_nearest(decoded, img_w, img_h, out_w, out_h);
        g_free(decoded);
        if (final_pixels == NULL)
        {
            GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("Failed to scale image"), (NULL));
            return FALSE;
        }
    }
    else
    {
        final_pixels = decoded;
    }

    self->actual_width = out_w;
    self->actual_height = out_h;
    self->rgba_stride = self->actual_width * 4;
    self->rgba_size = (gsize)self->rgba_stride * (gsize)self->actual_height;
    self->rgba_data = final_pixels;

    /* Defer building output format until first buffer (after negotiation) */
    self->frame_data = NULL;
    self->frame_size = 0;
    self->frame_stride = 0;
    self->num_planes = 1;
    self->shared_mem = NULL;

    self->frame_count = 0;
    return TRUE;
}

static gboolean gst_static_png_src_stop(GstBaseSrc* src)
{
    GstStaticPngSrc* self = GST_STATICPNG_SRC(src);

    if (self->shared_mem != NULL)
    {
        gst_memory_unref(self->shared_mem);
        self->shared_mem = NULL;
    }

    /* frame_data will be freed by shared_mem notify; ensure pointer not reused */
    self->frame_data = NULL;
    self->frame_size = 0;
    self->frame_stride = 0;
    self->actual_width = 0;
    self->actual_height = 0;
    if (self->rgba_data != NULL)
    {
        g_free(self->rgba_data);
        self->rgba_data = NULL;
        self->rgba_size = 0;
        self->rgba_stride = 0;
    }
    self->frame_count = 0;

    return TRUE;
}

static GstFlowReturn gst_static_png_src_create(GstPushSrc* src, GstBuffer** buf)
{
    GstStaticPngSrc* self = GST_STATICPNG_SRC(src);

    /* Build output memory on first call after negotiation */
    if (self->shared_mem == NULL)
    {
        if (self->rgba_data == NULL)
        {
            GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("No image loaded"), (NULL));
            return GST_FLOW_ERROR;
        }

        /* Ensure negotiation happened; get the chosen caps */
        GstPad* srcpad = gst_element_get_static_pad(GST_ELEMENT(self), "src");
        GstCaps* current = NULL;
        if (srcpad != NULL)
        {
            current = gst_pad_get_current_caps(srcpad);
            gst_object_unref(srcpad);
        }
        if (current == NULL)
        {
            /* Create default RGBA caps if none negotiated yet */
            GstCaps* default_caps = gst_caps_new_simple(
                "video/x-raw", "format", G_TYPE_STRING, "RGBA", "width", G_TYPE_INT, self->actual_width, "height",
                G_TYPE_INT, self->actual_height, "framerate", GST_TYPE_FRACTION, self->fps_n, self->fps_d, NULL);
            if (default_caps == NULL)
            {
                GST_ELEMENT_ERROR(self, CORE, NEGOTIATION, ("Failed to create default caps"), (NULL));
                return GST_FLOW_ERROR;
            }

            if (!gst_base_src_set_caps(GST_BASE_SRC(self), default_caps))
            {
                gst_caps_unref(default_caps);
                GST_ELEMENT_ERROR(self, CORE, NEGOTIATION, ("Failed to set default caps"), (NULL));
                return GST_FLOW_ERROR;
            }
            gst_caps_unref(default_caps);

            srcpad = gst_element_get_static_pad(GST_ELEMENT(self), "src");
            if (srcpad != NULL)
            {
                current = gst_pad_get_current_caps(srcpad);
                gst_object_unref(srcpad);
            }
        }
        const gchar* fmt = "RGBA";
        if (current != NULL && gst_caps_get_size(current) > 0)
        {
            const GstStructure* s = gst_caps_get_structure(current, 0);
            const gchar* f = gst_structure_get_string(s, "format");
            if (f != NULL)
            {
                fmt = f;
            }
            gst_caps_unref(current);
        }
        g_strlcpy(self->selected_format, fmt, sizeof(self->selected_format));

        if (g_strcmp0(fmt, "NV12") == 0)
        {
            self->frame_data = convert_rgba_to_nv12(self->rgba_data, self->actual_width, self->actual_height);
            if (self->frame_data == NULL)
            {
                GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("RGBA->NV12 conversion failed"), (NULL));
                return GST_FLOW_ERROR;
            }
            self->frame_stride = self->actual_width;
            self->frame_size = (gsize)self->actual_width * (gsize)self->actual_height * 3 / 2;
            self->num_planes = 2;
        }
        else if (g_strcmp0(fmt, "I420") == 0)
        {
            self->frame_data = convert_rgba_to_i420(self->rgba_data, self->actual_width, self->actual_height);
            if (self->frame_data == NULL)
            {
                GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("RGBA->I420 conversion failed"), (NULL));
                return GST_FLOW_ERROR;
            }
            self->frame_stride = self->actual_width;
            self->frame_size = (gsize)self->actual_width * (gsize)self->actual_height * 3 / 2;
            self->num_planes = 3;
        }
        else
        {
            self->frame_size = self->rgba_size;
            self->frame_stride = self->rgba_stride;
            self->frame_data = (guint8*)memdup_fallback(self->rgba_data, self->rgba_size);
            if (self->frame_data == NULL)
            {
                GST_ELEMENT_ERROR(self, RESOURCE, NO_SPACE_LEFT, ("Failed to allocate output frame"), (NULL));
                return GST_FLOW_ERROR;
            }
            if (g_strcmp0(fmt, "RGBA") != 0)
            {
                swizzle_from_rgba_inplace(self->frame_data, self->actual_width, self->actual_height, fmt);
            }
            self->num_planes = 1;
        }

        self->shared_mem = gst_memory_new_wrapped((GstMemoryFlags)0, self->frame_data, self->frame_size, 0,
                                                  self->frame_size, self->frame_data, (GDestroyNotify)g_free);
        if (self->shared_mem == NULL)
        {
            g_free(self->frame_data);
            self->frame_data = NULL;
            GST_ELEMENT_ERROR(self, RESOURCE, NO_SPACE_LEFT, ("Failed to wrap image memory"), (NULL));
            return GST_FLOW_ERROR;
        }
    }

    GstBuffer* buffer = gst_buffer_new();
    if (buffer == NULL)
    {
        return GST_FLOW_ERROR;
    }

    gst_buffer_append_memory(buffer, gst_memory_ref(self->shared_mem));

    /* Attach precise video meta (format, stride, offsets) so downstream interprets correctly */
    GstVideoFormat vfmt = gst_video_format_from_string(self->selected_format);
    if (vfmt == GST_VIDEO_FORMAT_UNKNOWN)
    {
        vfmt = GST_VIDEO_FORMAT_RGBA;
    }
    gsize offsets[GST_VIDEO_MAX_PLANES] = {0, 0, 0, 0};
    gint strides[GST_VIDEO_MAX_PLANES] = {0, 0, 0, 0};
    if (vfmt == GST_VIDEO_FORMAT_NV12)
    {
        offsets[0] = 0;
        offsets[1] = (gsize)self->actual_width * (gsize)self->actual_height;
        strides[0] = self->actual_width;
        strides[1] = self->actual_width;
        gst_buffer_add_video_meta_full(buffer, (GstVideoFrameFlags)0, vfmt, (gint)self->actual_width,
                                       (gint)self->actual_height, 2, offsets, strides);
    }
    else if (vfmt == GST_VIDEO_FORMAT_I420)
    {
        gsize y_size = (gsize)self->actual_width * (gsize)self->actual_height;
        gsize uv_size = ((gsize)self->actual_width / 2) * ((gsize)self->actual_height / 2);
        offsets[0] = 0;
        offsets[1] = y_size;
        offsets[2] = y_size + uv_size;
        strides[0] = self->actual_width;
        strides[1] = self->actual_width / 2;
        strides[2] = self->actual_width / 2;
        gst_buffer_add_video_meta_full(buffer, (GstVideoFrameFlags)0, vfmt, (gint)self->actual_width,
                                       (gint)self->actual_height, 3, offsets, strides);
    }
    else
    {
        strides[0] = self->frame_stride;
        gst_buffer_add_video_meta_full(buffer, (GstVideoFrameFlags)0, vfmt, (gint)self->actual_width,
                                       (gint)self->actual_height, 1, offsets, strides);
    }

    GstClockTime pts = self->frame_count * self->frame_duration;
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(buffer) = self->frame_duration;

    self->frame_count++;
    *buf = buffer;

    return GST_FLOW_OK;
}

/* Helpers */

static gboolean decode_png_to_rgba(const gchar* path, guint8** out_pixels, gint* out_w, gint* out_h)
{
    *out_pixels = NULL;
    *out_w = 0;
    *out_h = 0;

    FILE* fp = fopen(path, "rb");
    if (!fp)
    {
        return FALSE;
    }

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr)
    {
        fclose(fp);
        return FALSE;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr)
    {
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        fclose(fp);
        return FALSE;
    }

    if (setjmp(png_jmpbuf(png_ptr)))
    {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return FALSE;
    }

    png_init_io(png_ptr, fp);
    png_read_info(png_ptr, info_ptr);

    png_uint_32 width = png_get_image_width(png_ptr, info_ptr);
    png_uint_32 height = png_get_image_height(png_ptr, info_ptr);
    int bit_depth = png_get_bit_depth(png_ptr, info_ptr);
    int color_type = png_get_color_type(png_ptr, info_ptr);

    if (bit_depth == 16)
    {
        png_set_strip_16(png_ptr);
    }

    if (color_type == PNG_COLOR_TYPE_PALETTE)
    {
        png_set_palette_to_rgb(png_ptr);
    }

    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
    {
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    }

    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
    {
        png_set_tRNS_to_alpha(png_ptr);
    }

    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE)
    {
        png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
    }

    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
    {
        png_set_gray_to_rgb(png_ptr);
    }

    png_read_update_info(png_ptr, info_ptr);

    png_size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);

    guint8* pixels = (guint8*)g_malloc((gsize)rowbytes * height);
    if (!pixels)
    {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return FALSE;
    }

    png_bytep* row_pointers = (png_bytep*)g_malloc(sizeof(png_bytep) * height);
    if (!row_pointers)
    {
        g_free(pixels);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return FALSE;
    }

    for (png_uint_32 y = 0; y < height; ++y)
    {
        row_pointers[y] = pixels + y * rowbytes;
    }

    png_read_image(png_ptr, row_pointers);
    png_read_end(png_ptr, NULL);

    g_free(row_pointers);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    fclose(fp);

    /* Re-pack to tightly-packed RGBA if libpng rowbytes differ from width*4 */
    if (rowbytes != width * 4)
    {
        guint8* tight = (guint8*)g_malloc((gsize)width * height * 4);
        if (!tight)
        {
            g_free(pixels);
            return FALSE;
        }
        for (png_uint_32 y = 0; y < height; ++y)
        {
            memcpy(tight + (gsize)y * (gsize)width * 4, pixels + (gsize)y * (gsize)rowbytes, (gsize)width * 4);
        }
        g_free(pixels);
        pixels = tight;
    }

    *out_pixels = pixels;
    *out_w = (gint)width;
    *out_h = (gint)height;
    return TRUE;
}

static gboolean decode_jpeg_to_rgba(const gchar* path, guint8** out_pixels, gint* out_w, gint* out_h)
{
    *out_pixels = NULL;
    *out_w = 0;
    *out_h = 0;

    FILE* fp = fopen(path, "rb");
    if (!fp)
    {
        return FALSE;
    }

    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK)
    {
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return FALSE;
    }

    cinfo.out_color_space = JCS_RGB;
    if (!jpeg_start_decompress(&cinfo))
    {
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return FALSE;
    }

    const gint width = (gint)cinfo.output_width;
    const gint height = (gint)cinfo.output_height;
    const gint row_rgb_stride = (gint)cinfo.output_width * (gint)cinfo.output_components; /* expect 3 */

    guint8* rgba = (guint8*)g_malloc((gsize)width * (gsize)height * 4);
    if (rgba == NULL)
    {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return FALSE;
    }

    JSAMPARRAY buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, (JDIMENSION)row_rgb_stride, 1);
    if (buffer == NULL)
    {
        g_free(rgba);
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return FALSE;
    }

    while (cinfo.output_scanline < cinfo.output_height)
    {
        if (jpeg_read_scanlines(&cinfo, buffer, 1) != 1)
        {
            g_free(rgba);
            jpeg_finish_decompress(&cinfo);
            jpeg_destroy_decompress(&cinfo);
            fclose(fp);
            return FALSE;
        }

        guint8* dst = rgba + ((gsize)(cinfo.output_scanline - 1) * (gsize)width * 4);
        guint8* src = buffer[0];
        for (gint x = 0; x < width; ++x)
        {
            dst[x * 4 + 0] = src[x * 3 + 0];
            dst[x * 4 + 1] = src[x * 3 + 1];
            dst[x * 4 + 2] = src[x * 3 + 2];
            dst[x * 4 + 3] = 255;
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);

    *out_pixels = rgba;
    *out_w = width;
    *out_h = height;
    return TRUE;
}

static guint8* scale_rgba_nearest(const guint8* src, gint src_w, gint src_h, gint dst_w, gint dst_h)
{
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
    {
        return NULL;
    }

    guint8* dst = (guint8*)g_malloc((gsize)dst_w * (gsize)dst_h * 4);
    if (!dst)
    {
        return NULL;
    }

    for (gint y = 0; y < dst_h; ++y)
    {
        gint sy = (gint)((gint64)y * src_h / dst_h);
        const guint8* src_row = src + (gsize)sy * (gsize)src_w * 4;
        guint8* dst_row = dst + (gsize)y * (gsize)dst_w * 4;

        for (gint x = 0; x < dst_w; ++x)
        {
            gint sx = (gint)((gint64)x * src_w / dst_w);
            const guint8* sp = src_row + (gsize)sx * 4;
            guint8* dp = dst_row + (gsize)x * 4;

            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
            dp[3] = sp[3];
        }
    }

    return dst;
}

static void swizzle_from_rgba_inplace(guint8* pixels, gint width, gint height, const gchar* fmt)
{
    if (pixels == NULL || fmt == NULL)
    {
        return;
    }

    if (g_strcmp0(fmt, "RGBA") == 0)
    {
        return;
    }

    const gsize num_pixels = (gsize)width * (gsize)height;
    for (gsize i = 0; i < num_pixels; ++i)
    {
        guint8* p = pixels + i * 4;
        guint8 r = p[0];
        guint8 g = p[1];
        guint8 b = p[2];
        guint8 a = p[3];

        if (g_strcmp0(fmt, "BGRA") == 0)
        {
            p[0] = b;
            p[1] = g;
            p[2] = r;
            p[3] = a;
        }
        else if (g_strcmp0(fmt, "ARGB") == 0)
        {
            p[0] = a;
            p[1] = r;
            p[2] = g;
            p[3] = b;
        }
        else if (g_strcmp0(fmt, "ABGR") == 0)
        {
            p[0] = a;
            p[1] = b;
            p[2] = g;
            p[3] = r;
        }
        else
        {
            /* Unknown requested format, leave as-is */
        }
    }
}

/* Simple BT.601 conversion, full range, integer math */
static inline void rgba_to_yuv_bt601(guint8 r, guint8 g, guint8 b, guint8* y, gint16* u_acc, gint16* v_acc)
{
    gint yv = (66 * r + 129 * g + 25 * b + 128) >> 8;  /* 0..255 approx */
    gint uv = (-38 * r - 74 * g + 112 * b + 128) >> 8; /* -128..127 */
    gint vv = (112 * r - 94 * g - 18 * b + 128) >> 8;  /* -128..127 */
    *y = (guint8)CLAMP(yv + 16, 0, 255);
    *u_acc += (gint16)uv;
    *v_acc += (gint16)vv;
}

static guint8* convert_rgba_to_nv12(const guint8* src, gint width, gint height)
{
    if (src == NULL || width <= 0 || height <= 0)
    {
        return NULL;
    }

    gsize y_size = (gsize)width * (gsize)height;
    gsize uv_size = y_size / 2;
    guint8* dst = (guint8*)g_malloc(y_size + uv_size);
    if (dst == NULL)
    {
        return NULL;
    }

    guint8* y_plane = dst;
    guint8* uv_plane = dst + y_size;

    /* Luma */
    for (gint y = 0; y < height; ++y)
    {
        const guint8* srow = src + (gsize)y * (gsize)width * 4;
        guint8* drow = y_plane + (gsize)y * (gsize)width;
        for (gint x = 0; x < width; ++x)
        {
            const guint8* sp = srow + (gsize)x * 4;
            guint8 Y;
            gint16 u_acc = 0;
            gint16 v_acc = 0;
            rgba_to_yuv_bt601(sp[0], sp[1], sp[2], &Y, &u_acc, &v_acc);
            drow[x] = Y;
        }
    }

    /* Chroma (2x2 blocks) */
    for (gint y = 0; y < height; y += 2)
    {
        const guint8* srow0 = src + (gsize)y * (gsize)width * 4;
        const guint8* srow1 = src + (gsize)(y + 1) * (gsize)width * 4;
        guint8* uvrow = uv_plane + (gsize)(y / 2) * (gsize)width;
        for (gint x = 0; x < width; x += 2)
        {
            const guint8* p0 = srow0 + (gsize)x * 4;
            const guint8* p1 = srow0 + (gsize)(x + 1) * 4;
            const guint8* p2 = srow1 + (gsize)x * 4;
            const guint8* p3 = srow1 + (gsize)(x + 1) * 4;

            gint u_sum = 0;
            gint v_sum = 0;
            guint8 Ytmp;
            gint16 u_acc;
            gint16 v_acc;

            u_acc = v_acc = 0;
            rgba_to_yuv_bt601(p0[0], p0[1], p0[2], &Ytmp, &u_acc, &v_acc);
            u_sum += u_acc;
            v_sum += v_acc;
            u_acc = v_acc = 0;
            rgba_to_yuv_bt601(p1[0], p1[1], p1[2], &Ytmp, &u_acc, &v_acc);
            u_sum += u_acc;
            v_sum += v_acc;
            u_acc = v_acc = 0;
            rgba_to_yuv_bt601(p2[0], p2[1], p2[2], &Ytmp, &u_acc, &v_acc);
            u_sum += u_acc;
            v_sum += v_acc;
            u_acc = v_acc = 0;
            rgba_to_yuv_bt601(p3[0], p3[1], p3[2], &Ytmp, &u_acc, &v_acc);
            u_sum += u_acc;
            v_sum += v_acc;

            gint U = (u_sum / 4) + 128;
            gint V = (v_sum / 4) + 128;
            if (U < 0)
                U = 0;
            if (U > 255)
                U = 255;
            if (V < 0)
                V = 0;
            if (V > 255)
                V = 255;

            uvrow[x + 0] = (guint8)U;
            uvrow[x + 1] = (guint8)V;
        }
    }

    return dst;
}

static guint8* convert_rgba_to_i420(const guint8* src, gint width, gint height)
{
    if (src == NULL || width <= 0 || height <= 0)
    {
        return NULL;
    }

    gsize y_size = (gsize)width * (gsize)height;
    gsize uv_plane = y_size / 4;
    guint8* dst = (guint8*)g_malloc(y_size + uv_plane * 2);
    if (dst == NULL)
    {
        return NULL;
    }

    guint8* y_plane = dst;
    guint8* u_plane = dst + y_size;
    guint8* v_plane = u_plane + uv_plane;

    for (gint y = 0; y < height; ++y)
    {
        const guint8* srow = src + (gsize)y * (gsize)width * 4;
        guint8* drow = y_plane + (gsize)y * (gsize)width;
        for (gint x = 0; x < width; ++x)
        {
            const guint8* sp = srow + (gsize)x * 4;
            guint8 Y;
            gint16 u_acc = 0;
            gint16 v_acc = 0;
            rgba_to_yuv_bt601(sp[0], sp[1], sp[2], &Y, &u_acc, &v_acc);
            drow[x] = Y;
        }
    }

    for (gint y = 0; y < height; y += 2)
    {
        const guint8* srow0 = src + (gsize)y * (gsize)width * 4;
        const guint8* srow1 = src + (gsize)(y + 1) * (gsize)width * 4;
        guint8* urow = u_plane + (gsize)(y / 2) * (gsize)(width / 2);
        guint8* vrow = v_plane + (gsize)(y / 2) * (gsize)(width / 2);
        for (gint x = 0; x < width; x += 2)
        {
            const guint8* p0 = srow0 + (gsize)x * 4;
            const guint8* p1 = srow0 + (gsize)(x + 1) * 4;
            const guint8* p2 = srow1 + (gsize)x * 4;
            const guint8* p3 = srow1 + (gsize)(x + 1) * 4;

            gint u_sum = 0;
            gint v_sum = 0;
            guint8 Ytmp;
            gint16 u_acc;
            gint16 v_acc;
            u_acc = v_acc = 0;
            rgba_to_yuv_bt601(p0[0], p0[1], p0[2], &Ytmp, &u_acc, &v_acc);
            u_sum += u_acc;
            v_sum += v_acc;
            u_acc = v_acc = 0;
            rgba_to_yuv_bt601(p1[0], p1[1], p1[2], &Ytmp, &u_acc, &v_acc);
            u_sum += u_acc;
            v_sum += v_acc;
            u_acc = v_acc = 0;
            rgba_to_yuv_bt601(p2[0], p2[1], p2[2], &Ytmp, &u_acc, &v_acc);
            u_sum += u_acc;
            v_sum += v_acc;
            u_acc = v_acc = 0;
            rgba_to_yuv_bt601(p3[0], p3[1], p3[2], &Ytmp, &u_acc, &v_acc);
            u_sum += u_acc;
            v_sum += v_acc;

            gint U = (u_sum / 4) + 128;
            gint V = (v_sum / 4) + 128;
            if (U < 0)
                U = 0;
            if (U > 255)
                U = 255;
            if (V < 0)
                V = 0;
            if (V > 255)
                V = 255;
            urow[x / 2] = (guint8)U;
            vrow[x / 2] = (guint8)V;
        }
    }

    return dst;
}
