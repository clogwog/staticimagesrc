# staticimagesrc – GStreamer static image source

## Table of Contents
- [Overview](#overview)
- [Dependencies](#dependencies)
- [Debian/Ubuntu Installation](#debianubuntu-installation)
- [Build Instructions](#build-instructions)
- [Install Instructions](#install-instructions)
- [Usage Examples](#usage-examples)
- [Notes](#notes)

## Overview
`staticimagesrc` is a simple GStreamer source element that outputs a constant video frame generated from a PNG image at a fixed framerate. It supports RGBA family formats as well as NV12 and I420 via software conversion.

## Dependencies
- Autotools toolchain: autoconf, automake, libtool, pkg-config
- C/C++ toolchain: gcc/g++, make
- GStreamer 1.16+ development packages
  - Core: `gstreamer-1.0`
  - Base: `gstreamer-base-1.0`
  - Video: `gstreamer-video-1.0`
- libpng development package

## Debian/Ubuntu Installation
```bash
sudo apt update
sudo apt install -y \
    build-essential autoconf automake libtool pkg-config \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libpng-dev
```

## Build Instructions
From the project root (`/workspace/staticimagesrc`):

```bash
# Generate build system and configure
./autogen.sh

# Or, run configure explicitly (after autogen)
./configure

# Build
make -j$(nproc)
```

## Install Instructions
```bash
# Install the plugin library into the system GStreamer plugin directory
sudo make install

# Optionally refresh the GStreamer plugin registry
gst-inspect-1.0 staticimagesrc || true
```

If installing into a non-default prefix, ensure `GST_PLUGIN_PATH` includes the install location, e.g.:
```bash
export GST_PLUGIN_PATH="/usr/local/lib/gstreamer-1.0:${GST_PLUGIN_PATH}"
```

## Usage Examples
- Basic preview (auto-negotiated RGBA):
```bash
gst-launch-1.0 \
  staticimagesrc location=/path/to/image.png fps=25/1 ! \
  videoconvert ! autovideosink
```

- Force format and size (RGBA):
```bash
gst-launch-1.0 \
  staticimagesrc location=/path/to/image.png fps=30/1 ! \
  video/x-raw,format=RGBA,width=1280,height=720 ! \
  videoconvert ! autovideosink
```

- NV12 output (useful for encoders):
```bash
gst-launch-1.0 \
  staticimagesrc location=/path/to/image.png fps=30/1 ! \
  video/x-raw,format=NV12 ! \
  autovideosink
```

- I420 output and encode to H.264:
```bash
gst-launch-1.0 \
  staticimagesrc location=/path/to/image.png fps=25/1 ! \
  video/x-raw,format=I420,width=1920,height=1080 ! \
  x264enc tune=zerolatency ! mp4mux ! filesink location=out.mp4 -e
```

- Scale via properties (one-time scale at startup):
```bash
gst-launch-1.0 \
  staticimagesrc location=/path/to/image.png width=640 height=360 ! \
  autovideosink
```

## Notes
- The element factory name is `staticimagesrc`.
- The plugin performs a one-time PNG decode and optional scale at startup; subsequent buffers reuse the same memory.
- For NV12/I420, software color conversion (BT.601 full-range) is used.
