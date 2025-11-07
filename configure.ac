AC_PREREQ([2.53])
AC_INIT([staticimagesrc],[1.0.0])

AC_CONFIG_SRCDIR([plugins/gststaticimagesrc.cpp])
AC_CONFIG_HEADERS([config.h])

AM_INIT_AUTOMAKE([foreign subdir-objects])
AC_CONFIG_MACRO_DIR([m4])
AM_MAINTAINER_MODE([enable])

AC_PROG_CC
AC_PROG_CXX
AC_PROG_LIBTOOL
LT_PREREQ([2.2.6])
LT_INIT

AC_CHECK_PROG(HAVE_PKGCONFIG, pkg-config, [ ], [
  AC_MSG_ERROR([You need to have pkg-config installed!])
])

GST_REQUIRED=1.14.0

PKG_CHECK_MODULES([GST], [
  gstreamer-1.0 >= $GST_REQUIRED
  gstreamer-base-1.0 >= $GST_REQUIRED
  gstreamer-video-1.0 >= $GST_REQUIRED
], [
  AC_SUBST([GST_CFLAGS])
  AC_SUBST([GST_LIBS])
], [
  AC_MSG_ERROR([
    You need libgstreamer and gst-plugins-base development packages.
  ])
])

PKG_CHECK_MODULES([PNG], [libpng], [
  AC_SUBST([PNG_CFLAGS])
  AC_SUBST([PNG_LIBS])
], [
  AC_MSG_ERROR([You need libpng development package (e.g. libpng-dev).])
])

# JPEG (libjpeg)
PKG_CHECK_MODULES([JPEG], [libjpeg], [
  AC_SUBST([JPEG_CFLAGS])
  AC_SUBST([JPEG_LIBS])
], [
  AC_MSG_ERROR([You need libjpeg development package (e.g. libjpeg-dev).])
])

AC_MSG_CHECKING([plugindir])
if test "x${prefix}" = "x$HOME"; then
  plugindir="$HOME/.gstreamer-1.0/plugins"
else
  plugindir="\$(libdir)/gstreamer-1.0"
fi
AC_MSG_RESULT([$plugindir])
AC_SUBST([plugindir])

GST_PLUGIN_LDFLAGS='-module -avoid-version -export-symbols-regex [_]*\(gst_\|Gst\|GST_\).*'
AC_SUBST([GST_PLUGIN_LDFLAGS])

AC_CONFIG_FILES([
  Makefile
  plugins/Makefile
])
AC_OUTPUT


