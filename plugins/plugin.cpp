#include "config.h"

#include <gst/gst.h>

#include "gststaticimagesrc.h"

static gboolean plugin_init(GstPlugin* plugin)
{
    return gst_element_register(plugin, "staticimagesrc", GST_RANK_NONE, GST_TYPE_STATICPNG_SRC);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, staticimagesrc, "Static image source plugin", plugin_init,
                  VERSION, "LGPL", PACKAGE_NAME, PACKAGE_STRING)
