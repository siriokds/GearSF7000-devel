/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef VIDEO_DEVICE_OPTIONS_H
#define VIDEO_DEVICE_OPTIONS_H

#include <cstddef>
#include "definitions.h"

inline constexpr std::size_t GC_VIDEO_MAX_COLOR_PROFILES = 4;

enum class GC_VideoOptionType : u8
{
    Boolean,
    Integer,
    Choice
};

struct GC_VideoOptionDescriptor
{
    const char* code = nullptr;
    const char* label = nullptr;
    GC_VideoOptionType type = GC_VideoOptionType::Integer;
    s64 minimum = 0;
    s64 maximum = 0;
};

// A video device with a fixed colour generator may expose a small set of
// calibrated RGB interpretations. Devices with programmable palettes usually
// expose no profiles: software writes their colours directly.
struct GC_VideoColorProfileDescriptor
{
    s64 value = 0;
    const char* code = nullptr;
    const char* label = nullptr;
};

// Read-only view of the RGB values currently used by the device. Storage is
// device-owned and remains valid until the palette revision changes.
struct GC_VideoPaletteView
{
    const u8* rgb888 = nullptr;
    std::size_t color_count = 0;
    u64 revision = 0;

    // What the device itself stores for each entry, when it stores more than
    // a fixed colour: the Palettes panel shows it beside the swatch.
    enum NativeKind : u8
    {
        NativeNone = 0,     // fixed colours (TMS9918A): the index says it all
        NativeRgb444 = 1,   // const u16*: 0x0RGB, four bits a component (F18A, PICO9918)
        NativeTms9918B = 2, // const u8*: colour index (bits 0-3) and luminance (bits 4-5)
        NativeSmsByte = 3   // const u16*: 0x0RGB as stored, written as an SMS CRAM byte (--BBGGRR)
    };
    const void* native = nullptr;
    u8 native_kind = NativeNone;
    // Entries from here on are fixed colours the device draws with, not ones
    // it stores (picoSMSvdp's TMS modes). 0: none.
    std::size_t fixed_from = 0;

    bool IsValid() const
    {
        return rgb888 != nullptr && color_count > 0;
    }
};

// Stable semantic codes used by frontends. A device advertises only the
// options it implements; callers must not infer support from its family.
namespace GC_VideoOption
{
inline constexpr const char* Border = "presentation.border";
inline constexpr const char* PredefinedPalette = "presentation.palette";
inline constexpr const char* UnlimitedSprites = "debug.unlimited_sprites";
inline constexpr const char* FullRaster = "debug.full_raster";
}

namespace GC_VideoBorder
{
inline constexpr s64 Disabled = 0;
inline constexpr s64 TopBottom = 1;
inline constexpr s64 Full272 = 2;
inline constexpr s64 Full284 = 3;
inline constexpr s64 Full320 = 4;
}

#endif // VIDEO_DEVICE_OPTIONS_H
