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
    GC_VIDEO_SIGNAL_DVI
};

struct GC_VideoFrameDescriptor
{
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
        return dimensionsValid && contentValid && visibleValid &&
            raster_width > 0 && raster_height > 0 &&
            logical_width > 0 && logical_height > 0 &&
            refresh_rate.numerator > 0 && refresh_rate.denominator > 0 &&
            pixel_aspect_ratio.numerator > 0 &&
            pixel_aspect_ratio.denominator > 0;
    }
};

#endif /* VIDEO_FRAME_H */
