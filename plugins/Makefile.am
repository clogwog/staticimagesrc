plugindir = @plugindir@

plugin_LTLIBRARIES = libgststaticimagesrc.la

libgststaticimagesrc_la_SOURCES = \
    gststaticpngsrc.cpp \
    gststaticpngsrc.h \
    plugin.cpp

# Apply pkg-config includes to all compilations (C/C++)
AM_CPPFLAGS = $(GST_CFLAGS) $(PNG_CFLAGS)

AM_CXXFLAGS = -std=c++17

libgststaticimagesrc_la_LIBADD  = $(GST_LIBS) $(PNG_LIBS)
libgststaticimagesrc_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)


