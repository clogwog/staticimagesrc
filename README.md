# staticimagesrc – GStreamer static image source

This plugin was created because common pipelines like `filesrc ! decodebin ! imagefreeze` frequently misbehave on Jetson devices (e.g., sporadic stalls, negotiation quirks, or high overhead when paired with zero-copy paths). `staticimagesrc` avoids those issues by decoding the image exactly once at startup and then reusing the same frame for every buffer. The result is a lightweight, predictable source with very low CPU and memory overhead.

## Table of Contents
- [Overview](#overview)
- [Dependencies](#dependencies)
- [Debian/Ubuntu Installation](#debianubuntu-installation)
- [Build Instructions](#build-instructions)
- [Install Instructions](#install-instructions)
- [Properties](#properties)
- [Usage Examples](#usage-examples)
- [Notes](#notes)
- [Changes](#changes)

## Overview
`staticimagesrc` is a simple GStreamer source element that outputs a constant video frame generated from an image at a fixed framerate. It supports RGBA family formats as well as NV12 and I420 via software conversion.

## Dependencies
- Autotools toolchain: autoconf, automake, libtool, pkg-config
- C/C++ toolchain: gcc/g++, make
- GStreamer 1.14+ development packages
  - Core: `gstreamer-1.0`
  - Base: `gstreamer-base-1.0`
  - Video: `gstreamer-video-1.0`
- libpng development package
- libjpeg development package

## Debian/Ubuntu Installation
```bash
sudo apt update
sudo apt install -y \
    build-essential autoconf automake libtool pkg-config \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libpng-dev libjpeg-dev
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

## Properties
- **location** (string): Path to the image file to load. Supported formats: PNG (`.png`), JPEG (`.jpg`, `.jpeg`, `.jpp`).
- **fps** (fraction): Output framerate as a fraction (e.g., `25/1` for 25 fps). Default: `25/1`.
- **width** (int): Optional output width in pixels. If set along with `height`, the image will be scaled once at startup. Range: 0-8192. Default: `0` (use image dimensions).
- **height** (int): Optional output height in pixels. If set along with `width`, the image will be scaled once at startup. Range: 0-8192. Default: `0` (use image dimensions).
- **num-buffers** (uint): Number of buffers to output before sending EOS (end-of-stream). Set to `0` for unlimited output (default). Range: 0-G_MAXUINT.

## Usage Examples
- Basic preview (matches pipeline_manager example):
```bash
gst-launch-1.0 \
  staticimagesrc location=/path/to/image/test-pattern.png fps=25/1 width=1280 height=720 ! \
  video/x-raw,format=RGBA ! videoconvert ! autovideosink
```

- Force format and size (RGBA):
```bash
gst-launch-1.0 \
  staticimagesrc location=/path/to/image.png fps=30/1 ! \
  video/x-raw,format=RGBA,width=1280,height=720 ! \
  videoconvert ! autovideosink
```

- NV12 output (preview via videoconvert):
```bash
gst-launch-1.0 \
  staticimagesrc location=/path/to/image.jpg fps=30/1 ! \
  video/x-raw,format=NV12 ! \
  videoconvert ! autovideosink
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
  video/x-raw,format=RGBA ! videoconvert ! autovideosink
```

- Limit output to a specific number of frames:
```bash
gst-launch-1.0 \
  staticimagesrc location=/path/to/image.png fps=30/1 num-buffers=150 ! \
  video/x-raw,format=RGBA ! videoconvert ! autovideosink
```
This will output exactly 150 frames (5 seconds at 30 fps) before sending EOS.

## Notes
- The element factory name is `staticimagesrc`.
- On older GStreamer (e.g., 1.14), when using width/height properties with videoconvert, add `video/x-raw,format=RGBA` to ensure negotiation.
- Supported file extensions are determined by the URI's path extension: `png` -> PNG decoder; `jpeg`, `jpg`, `jpp` -> JPEG decoder.
- The plugin performs a one-time image decode (PNG or JPEG) and optional scale at startup; subsequent buffers reuse the same memory.
- For NV12/I420, software color conversion (BT.601 full-range) is used.
- When `num-buffers` is set to a value greater than 0, the element will output exactly that many buffers and then send EOS. This is useful for creating fixed-duration test patterns or limiting output for testing purposes.

## Changes

### Added `num-buffers` Property (2025-10-14)
- Added support for limiting the number of output buffers via the `num-buffers` property.
- When `num-buffers` is set to a value > 0, the element outputs exactly that many buffers before sending EOS.
- Default value is `0` (unlimited output), maintaining backward compatibility.
- This property allows precise control over output duration, useful for testing and fixed-duration test pattern generation.

### Initial push (2025-10-07)