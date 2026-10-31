// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
/*
 * GearSF7000 video-output contract.
 *
 * This header deliberately contains no SDL or renderer types.  A video
 * device describes the frame it produces; frontends decide how to allocate,
 * upload, crop and present that frame.
 */

#ifndef VIDEO_FRAME_H
#define VIDEO_FRAME_H

#include "definitions.h"

struct GC_VideoRect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct GC_VideoRational
{
    int numerator = 0;
    int denominator = 1;
};

enum GC_VideoSignalType
{
    GC_VIDEO_SIGNAL_UNKNOWN,
    GC_VIDEO_SIGNAL_ANALOG_NTSC,
    GC_VIDEO_SIGNAL_ANALOG_PAL,
    GC_VIDEO_SIGNAL_DIGITAL_RGB,
    GC_VIDEO_SIGNAL_VGA,
    GC_VIDEO_SIGNAL_DVI,
    GC_VIDEO_SIGNAL_ANALOG_COMPONENT
};

enum GC_VideoSurfaceKind
{
    GC_VIDEO_SURFACE_PRESENTATION,
    GC_VIDEO_SURFACE_FULL_RASTER_DEBUG
};

enum GC_VideoPostProcessingCapability : u32
{
    GC_VIDEO_POSTPROCESSING_CAPABILITY_NONE = 0,
    // The completed RGB surface may be resampled spatially. This covers
    // bilinear filtering and resolution-independent scaling kernels.
    GC_VIDEO_POSTPROCESSING_CAPABILITY_SPATIAL_SCALING = 1u << 0,
    // One source row represents one progressive display line, so a display
    // shader may shape scanlines from the source-row cadence.
    GC_VIDEO_POSTPROCESSING_CAPABILITY_SCANLINE_CRT = 1u << 1
};

struct GC_VideoFrameDescriptor
{
    // Stable device identity accompanies the geometry. A frontend can cache
    // resources by revision without accidentally reusing them after a device
    // replacement whose current mode happens to have the same dimensions.
    const char* device_key = nullptr;
    u32 mode_id = 0;

    GC_VideoSurfaceKind surface_kind = GC_VIDEO_SURFACE_PRESENTATION;
    u32 postprocessing_capabilities =
        GC_VIDEO_POSTPROCESSING_CAPABILITY_NONE;

    // Maximum packed output buffer required by the selected video device.
    // Frontends allocate against this pair, not against TMS-specific defines.
    int buffer_width = 0;
    int buffer_height = 0;

    // Dimensions and tight pixel stride of the frame currently produced.
    int frame_width = 0;
    int frame_height = 0;
    int stride_pixels = 0;

    // Program-visible content inside the packed frame.  With overscan off it
    // normally covers the whole frame; with borders enabled it is inset.
    GC_VideoRect content_area;

    // Default presentation crop.  It is separate from content_area because a
    // future CRT profile may expose borders while cropping blanking/sync.
    GC_VideoRect visible_area;

    // Complete timing raster.  It need not be present in the packed output.
    // TMS9918/9929 currently reports 342 dots and 262/313 lines here.
    int raster_width = 0;
    int raster_height = 0;

    // Logical coordinates used by the emulated software.  These may differ
    // from the emitted signal, e.g. F18A legacy 256x192 content in 640x480 VGA.
    int logical_width = 0;
    int logical_height = 0;

    // Desktop-space basis for explicit frontend scale choices. This is not a
    // pixel density and does not describe the geometry of a video mode. A
    // device may emit a 640x480 framebuffer while asking Scale 1x to occupy
    // 320x240 desktop units; the complete framebuffer is still presented
    // uniformly, so high-resolution modes keep their intended proportions.
    int presentation_width = 0;
    int presentation_height = 0;

    // Frequency of the native units accepted by IVideoDevice::TickDeviceClocks.
    // For TMS99xx this is the VDP master oscillator; another device may expose
    // a pixel clock or another indivisible internal tick instead.
    u32 device_clock_hz = 0;
    GC_VideoRational refresh_rate;
    GC_VideoRational pixel_aspect_ratio = {1, 1};
    GC_VideoSignalType signal = GC_VIDEO_SIGNAL_UNKNOWN;
    GC_Region region = Region_NTSC;

    // Changes when geometry or signal metadata changes.  Pixel contents do
    // not alter this value.
    u64 revision = 0;

    bool IsValid() const
    {
        const bool dimensionsValid = buffer_width > 0 && buffer_height > 0 &&
            frame_width > 0 && frame_height > 0 && stride_pixels >= frame_width &&
            frame_width <= buffer_width && frame_height <= buffer_height;
        const bool contentValid = content_area.x >= 0 && content_area.y >= 0 &&
            content_area.width > 0 && content_area.height > 0 &&
            content_area.x + content_area.width <= frame_width &&
            content_area.y + content_area.height <= frame_height;
        const bool visibleValid = visible_area.x >= 0 && visible_area.y >= 0 &&
            visible_area.width > 0 && visible_area.height > 0 &&
            visible_area.x + visible_area.width <= frame_width &&
            visible_area.y + visible_area.height <= frame_height;
        return device_key != nullptr && device_key[0] != '\0' &&
            dimensionsValid && contentValid && visibleValid &&
            raster_width > 0 && raster_height > 0 &&
            logical_width > 0 && logical_height > 0 &&
            presentation_width > 0 && presentation_height > 0 &&
            device_clock_hz > 0 &&
            refresh_rate.numerator > 0 && refresh_rate.denominator > 0 &&
            pixel_aspect_ratio.numerator > 0 &&
            pixel_aspect_ratio.denominator > 0;
    }

    bool SupportsPostProcessing(u32 requiredCapabilities) const
    {
        return (postprocessing_capabilities & requiredCapabilities) ==
            requiredCapabilities;
    }
};

// Immutable view of one device-owned, completed presentation frame. The
// descriptor and pixels are obtained together so a frontend never combines
// geometry from one mode with storage from another. The pointer remains owned
// by the device and is valid until that device produces or repacks a frame.
struct GC_VideoFrameView
{
    GC_VideoFrameDescriptor descriptor;
    const u8* pixels = nullptr;
    GC_Color_Format pixel_format = GC_PIXEL_RGB888;
    int row_stride_bytes = 0;
    u64 frame_serial = 0;

    bool IsValid() const
    {
        const int bytesPerPixel =
            (pixel_format == GC_PIXEL_RGB888 ||
             pixel_format == GC_PIXEL_BGR888) ? 3 : 2;
        return descriptor.IsValid() && pixels != nullptr &&
            row_stride_bytes >= descriptor.frame_width * bytesPerPixel;
    }
};

#endif /* VIDEO_FRAME_H */
