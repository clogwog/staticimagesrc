/*
 * Static Image Source - outputs a constant RGBA frame at a fixed framerate
 */

#ifndef __GST_STATIC_IMAGE_SRC_H__
#define __GST_STATIC_IMAGE_SRC_H__

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>

G_BEGIN_DECLS

#define GST_TYPE_STATICPNG_SRC (gst_static_png_src_get_type())

G_DECLARE_FINAL_TYPE (GstStaticPngSrc, gst_static_png_src, GST, STATICPNG_SRC, GstPushSrc)

G_END_DECLS

#endif /* __GST_STATIC_IMAGE_SRC_H__ */


