/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026  Saverio Russo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#ifndef VIDEO_DEBUG_DESCRIPTOR_H
#define VIDEO_DEBUG_DESCRIPTOR_H

#include "definitions.h"

enum GC_VideoMemoryMappingFlag : u32
{
    GC_VIDEO_MEMORY_MAPPING_LINEAR         = 0,
    GC_VIDEO_MEMORY_MAPPING_ALIASED        = 1u << 0,
    GC_VIDEO_MEMORY_MAPPING_HOST_WINDOWED  = 1u << 1,
    GC_VIDEO_MEMORY_MAPPING_MODE_DEPENDENT = 1u << 2
};

enum class GC_VideoMemoryRegionKind : u8
{
    Storage,
    MappedDevice,
    Alias,
    Reserved
};

enum class GC_VideoMemoryRegionRole : u8
{
    DisplayRam,
    ProcessorRam,
    ExpansionRam,
    PaletteRam,
    FirmwareRom,
    RegisterBank,
    StatusBank,
    RasterState,
    CommandBlock,
    Other
};

enum GC_VideoMemoryAccessFlag : u32
{
    GC_VIDEO_MEMORY_ACCESS_NONE              = 0,
    GC_VIDEO_MEMORY_ACCESS_READ              = 1u << 0,
    GC_VIDEO_MEMORY_ACCESS_WRITE             = 1u << 1,
    GC_VIDEO_MEMORY_ACCESS_EXECUTE           = 1u << 2,
    GC_VIDEO_MEMORY_ACCESS_DISPLAY_FETCH     = 1u << 3,
    GC_VIDEO_MEMORY_ACCESS_HOST_VISIBLE      = 1u << 4,
    GC_VIDEO_MEMORY_ACCESS_PROCESSOR_VISIBLE = 1u << 5,
    GC_VIDEO_MEMORY_ACCESS_READ_SIDE_EFFECT  = 1u << 6,
    GC_VIDEO_MEMORY_ACCESS_WRITE_SIDE_EFFECT = 1u << 7,
    GC_VIDEO_MEMORY_ACCESS_WRITE_TRAPPED     = 1u << 8
};

enum GC_VideoRegisterAccessFlag : u32
{
    GC_VIDEO_REGISTER_ACCESS_NONE              = 0,
    GC_VIDEO_REGISTER_ACCESS_READ              = 1u << 0,
    GC_VIDEO_REGISTER_ACCESS_WRITE             = 1u << 1,
    GC_VIDEO_REGISTER_ACCESS_OBSERVE           = 1u << 2,
    GC_VIDEO_REGISTER_ACCESS_READ_SIDE_EFFECT  = 1u << 3,
    GC_VIDEO_REGISTER_ACCESS_WRITE_SIDE_EFFECT = 1u << 4
};

enum class GC_VideoDebugValueFormat : u8
{
    Boolean,
    Decimal,
    Hexadecimal,
    Address,
    Text
};

// A device-owned debug scene is expressed in video-native pixels. The desktop
// viewer may scale or compose it, but never needs to decode VDP registers.
enum class GC_VideoDebugLayerKind : u8
{
    TileGrid,
    TileCatalog,
    Bitmap,
    Sprites,
    Overlay
};

enum class GC_VideoDebugBlendMode : u8
{
    Opaque,
    Alpha
};

struct GC_VideoDebugSceneDescriptor
{
    u32 canvas_width = 0;
    u32 canvas_height = 0;
    float pixel_aspect_x = 1.0f;
    float pixel_aspect_y = 1.0f;
};

// A layer surface contains its complete native map, never only the part
// currently visible on the emulated display. Providers that need render state
// to decode such a frame snapshot (for example a programmable palette) sample
// it at the first active pixel of scanline 0. Live register, palette and raw
// memory views remain cycle-current. The optional viewport identifies the
// displayed part inside the map and is drawn by the viewer as an overlay.
struct GC_VideoDebugLayerDescriptor
{
    const char* code = NULL;
    const char* name = NULL;
    GC_VideoDebugLayerKind kind = GC_VideoDebugLayerKind::Bitmap;
    GC_VideoDebugBlendMode blend_mode = GC_VideoDebugBlendMode::Opaque;
    int z_order = 0;
    bool visible = true;
    bool has_grid = false;
    u32 content_x = 0;
    u32 content_y = 0;
    u32 content_width = 0;
    u32 content_height = 0;
    // Presentation-only integer expansion. The source RGBA map remains at
    // native resolution; the viewer applies this to image, grid, viewport and
    // hit-testing. Double-row modes therefore need not duplicate pixels.
    u8 presentation_scale_x = 1;
    u8 presentation_scale_y = 1;
    bool has_viewport = false;
    u32 viewport_x = 0;
    u32 viewport_y = 0;
    u32 viewport_width = 0;
    u32 viewport_height = 0;
    u32 columns = 0;
    u32 rows = 0;
    // Number of cells backed by the current mode. A tile-catalogue surface
    // may keep a stable larger grid and paint the remaining cells as inactive.
    u32 active_cell_count = 0;
    u32 cell_width = 0;
    u32 cell_height = 0;
    // Optional horizontal bands meaningful to the device (Graphics II has
    // three 8-row regions, for example). Zero means no such subdivision.
    u32 rows_per_region = 0;
    // The viewer owns neither the label nor an assumed 16-bit formatting of
    // the layer's primary address. V9938-class devices, for example, expose
    // physical VRAM addresses wider than the TMS9918's 14-bit bus.
    const char* primary_address_label = NULL;
    u32 primary_address = 0;
    u8 address_width_bits = 0;
};

// One screen region projected into a layer's native source map. A VDP may
// expose more than one region when part of the screen is locked against one
// scroll axis. Coordinates wrap at the layer's content bounds.
struct GC_VideoDebugViewportRegionDescriptor
{
    u32 source_x = 0;
    u32 source_y = 0;
    u32 width = 0;
    u32 height = 0;
};

// RGBA is used even by an opaque first implementation so later VDPs can make
// device-defined pixels transparent without changing the renderer contract.
struct GC_VideoDebugLayerSurface
{
    const u8* rgba = NULL;
    u32 width = 0;
    u32 height = 0;
    u32 row_pitch = 0;
    u64 generation = 0;
};

// Optional device-owned render choices for an individual debug layer. Bit N
// in the option mask corresponds to descriptor N.
struct GC_VideoDebugLayerOptionDescriptor
{
    const char* code = NULL;
    const char* name = NULL;
    bool enabled = false;
};

// Logical parts of the VDP's final picture. The GUI chooses which parts are
// visible, then asks the provider to compose them. This keeps priority,
// masking and other chip-specific rules inside the video device. Bit N in the
// visibility mask corresponds to descriptor N.
struct GC_VideoDebugPreviewPartDescriptor
{
    const char* code = NULL;
    const char* name = NULL;
    bool visible = true;
};

// Device-defined presentation choices for the composed debug preview. They
// are separate from content visibility: for example a provider may offer a
// dimmed background behind isolated sprites without exposing how it renders
// that diagnostic view.
struct GC_VideoDebugPreviewOptionDescriptor
{
    const char* code = NULL;
    const char* name = NULL;
    bool enabled = false;
};

enum GC_VideoDebugSpriteCapability : u32
{
    GC_VIDEO_DEBUG_SPRITE_CAP_NONE           = 0,
    GC_VIDEO_DEBUG_SPRITE_CAP_SCREEN_PREVIEW = 1u << 0,
    GC_VIDEO_DEBUG_SPRITE_CAP_FETCH_STATE    = 1u << 1,
    GC_VIDEO_DEBUG_SPRITE_CAP_SCANLINE       = 1u << 2,
    GC_VIDEO_DEBUG_SPRITE_CAP_OVERFLOW       = 1u << 3,
    GC_VIDEO_DEBUG_SPRITE_CAP_COLLISION      = 1u << 4
};

enum class GC_VideoDebugSpriteVisualState : u8
{
    None,
    Absent,
    Complete,
    Partial,
    Overflow
};

struct GC_VideoDebugSpriteSourceDescriptor
{
    const char* code = NULL;
    const char* name = NULL;
    const char* legend = NULL;
    u32 capabilities = GC_VIDEO_DEBUG_SPRITE_CAP_NONE;
    const char* parameter_code = NULL;
    const char* parameter_name = NULL;
};

struct GC_VideoDebugSpriteAtlasItemDescriptor
{
    u32 index = 0;
    u32 atlas_x = 0;
    u32 atlas_y = 0;
    u32 width = 0;
    u32 height = 0;
    GC_VideoDebugSpriteVisualState visual_state =
        GC_VideoDebugSpriteVisualState::None;
    // Keep inactive live SAT patterns inspectable, but dim their thumbnails.
    bool inactive = false;
    bool has_screen_rect = false;
    float screen_x = 0.0f;
    float screen_y = 0.0f;
    float screen_width = 0.0f;
    float screen_height = 0.0f;
};

struct GC_VideoDebugSpriteAtlasSurface
{
    const u8* rgba = NULL;
    u32 width = 0;
    u32 height = 0;
    u32 row_pitch = 0;
    const GC_VideoDebugSpriteAtlasItemDescriptor* items = NULL;
    u32 item_count = 0;
    u64 generation = 0;
};

// Raw sprite-attribute records. The provider defines both dimensions so the
// GUI can present TMS9918's 32 x 4-byte SAT and wider enhanced-VDP formats
// without knowing either layout.
struct GC_VideoDebugSpriteAttributeTableDescriptor
{
    const char* code = NULL;
    const char* name = NULL;
    u32 record_count = 0;
    u32 column_count = 0;
    u32 base_address = 0;
    u32 record_stride = 0;
    u8 address_width_bits = 0;
};

struct GC_VideoDebugSpriteAttributeColumnDescriptor
{
    const char* code = NULL;
    const char* name = NULL;
    u8 width_bits = 0;
};

struct GC_VideoDebugSpriteAttributeRecordDescriptor
{
    bool visible = true;
    u32 address = 0;
};

// A video surface often needs other tables to be decoded correctly. The GUI
// lists these verbatim below the surface: Pattern/Colour for a TMS tile set,
// attribute tables for enhanced chips, or bitmap layers for a bitmap device.
struct GC_VideoDebugLayerAreaDescriptor
{
    const char* code = NULL;
    const char* label = NULL;
    u32 address = 0;
    u32 size = 0;
    u8 address_width_bits = 0;
};

struct GC_VideoDebugGridCell
{
    bool valid = false;
    u32 column = 0;
    u32 row = 0;
    // Optional destination for a generic "go to backing memory" action.
    u32 navigation_address = 0;
};

// A cell has no fixed set of interesting values across VDPs. TMS9918 exposes
// name/pattern/colour addresses; enhanced devices may add page, palette,
// priority, flip and attribute fields. The viewer renders this list verbatim.
struct GC_VideoDebugGridCellFieldDescriptor
{
    const char* code = NULL;
    const char* label = NULL;
    GC_VideoDebugValueFormat format = GC_VideoDebugValueFormat::Hexadecimal;
    u64 value = 0;
    const char* text = NULL;
    u8 width_bits = 0;
    // Optional memory location from which this value was read. This lets a
    // provider publish addressed bytes without making the viewer understand
    // the device's table layout.
    bool has_source_address = false;
    u64 source_address = 0;
    u8 source_address_width_bits = 0;
};

struct GC_VideoStateValueDescriptor
{
    const char* code = NULL;
    const char* name = NULL;
    const char* description = NULL;
    GC_VideoDebugValueFormat format = GC_VideoDebugValueFormat::Decimal;
    u8 width_bits = 0;
};

struct GC_VideoRegisterBankDescriptor
{
    const char* code = NULL;
    const char* name = NULL;
    const char* description = NULL;
    u32 register_count = 0;
    u8 register_width_bits = 0;
    u64 schema_revision = 0;
};

struct GC_VideoRegisterDescriptor
{
    const char* code = NULL;
    const char* short_name = NULL;
    const char* description = NULL;
    const char* bank_code = NULL;
    u32 register_index = 0;
    u8 width_bits = 0;
    u32 implemented_mask = 0;
    u32 reset_value = 0;
    u32 access_flags = GC_VIDEO_REGISTER_ACCESS_NONE;
    bool has_derived_address = false;
    u8 derived_address_width_bits = 0;
};

// Describes one semantic function inside a register. A one-bit function has
// a one-bit mask; a packed value uses the complete mask and an explicit shift.
// The bank, register number and mask form its physical identity, while code is
// the stable semantic identity consumed by GUI, recorder and MCP.
struct GC_VideoRegisterFieldDescriptor
{
    const char* code = NULL;
    const char* description = NULL;
    const char* bank_code = NULL;
    u32 register_index = 0;
    u32 mask = 0;
    u8 shift = 0;
    u32 access_flags = GC_VIDEO_REGISTER_ACCESS_NONE;
};

// Describes storage actually installed in one video-device instance. The
// static device catalogue may describe supported limits, but tools must use
// this runtime value so a 16 KiB device is never presented as a larger one.
struct GC_VideoMemorySpaceDescriptor
{
    const char* code = NULL;
    const char* name = NULL;
    const char* description = NULL;
    u32 address_size_bytes = 0;
    u32 installed_size_bytes = 0;

    // Size visible through one host address-counter window. A zero value
    // means the space is private to an internal processor or debug provider.
    u32 host_window_size_bytes = 0;
    u32 host_page_size_bytes = 0;
    u32 host_page_count = 0;
    u32 mapping_flags = GC_VIDEO_MEMORY_MAPPING_LINEAR;

    bool readable = false;
    bool writable = false;

    // Optional direct view for simple linear storage. It remains owned by the
    // device and is valid only while the provider and layout_revision remain
    // unchanged. Complex or side-effectful spaces leave this null and expose
    // their contents only through ReadMemorySpace/WriteMemorySpace.
    u8* direct_data = NULL;
    u64 layout_revision = 0;
};

// What a stretch of video memory is used for, as the chip is programmed right
// now. The ROM map uses it to classify the cartridge bytes the CPU copies
// into VRAM (DOCS/PROGRAM_ANALYSIS_AUDIT.md section 12): each VDP family
// answers for its own layout, since the tables differ between them - the
// SMS mode 4 has no colour table and shares patterns between background
// and sprites.
enum class GC_VramTableRole : u8
{
    NameTable,        // NAMETAB: which pattern goes where on screen
    ColorTable,       // COLTAB
    PatternGenerator, // PATGEN: background patterns
    SpriteAttributes, // SAT
    SpritePatterns    // SPRPAT
};

struct GC_VramTableSpan
{
    GC_VramTableRole role = GC_VramTableRole::NameTable;
    u32 start = 0;
    u32 size = 0;
};

// One ordered interval inside an address space. More specific mapped regions
// have a higher priority than the storage below them. Alias metadata covers
// simple mirrors; active devices such as palette, DMA and status use provider
// callbacks instead of exposing a raw pointer.
struct GC_VideoMemoryRegionDescriptor
{
    const char* code = NULL;
    const char* description = NULL;
    GC_VideoMemoryRegionKind kind = GC_VideoMemoryRegionKind::Reserved;
    GC_VideoMemoryRegionRole role = GC_VideoMemoryRegionRole::Other;
    u32 start = 0;
    u32 size = 0;
    u32 access_flags = GC_VIDEO_MEMORY_ACCESS_NONE;
    const char* backing_store_code = NULL;
    u32 backing_offset = 0;
    u32 address_mask = 0;
    int priority = 0;
};

#endif // VIDEO_DEBUG_DESCRIPTOR_H
