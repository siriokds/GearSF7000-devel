/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026  Saverio Russo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#ifndef IVIDEO_DEBUG_PROVIDER_H
#define IVIDEO_DEBUG_PROVIDER_H

#include <cstddef>

#include "VideoDebugDescriptor.h"

// Optional inspection surface published by a video-device instance. Register,
// surface and panel catalogues will join this interface as their concrete TMS
// dependencies are detached.
class IVideoDebugProvider
{
public:
    virtual ~IVideoDebugProvider() = default;

    virtual std::size_t GetStateValueCount() const = 0;
    virtual bool DescribeStateValue(std::size_t index,
        GC_VideoStateValueDescriptor& descriptor) const = 0;
    virtual bool ReadStateValueSnapshot(std::size_t index, u64& value) const = 0;
    virtual bool ReadStateTextSnapshot(std::size_t index,
        const char*& value) const = 0;

    virtual std::size_t GetMemorySpaceCount() const = 0;
    virtual bool DescribeMemorySpace(std::size_t index,
        GC_VideoMemorySpaceDescriptor& descriptor) const = 0;
    virtual std::size_t GetMemoryRegionCount(const char* spaceCode) const = 0;
    virtual bool DescribeMemoryRegion(const char* spaceCode,
        std::size_t index, GC_VideoMemoryRegionDescriptor& descriptor) const = 0;
    virtual bool ReadMemorySpace(const char* spaceCode, u32 offset,
        u8* destination, std::size_t size) const = 0;
    virtual bool WriteMemorySpace(const char* spaceCode, u32 offset,
        const u8* source, std::size_t size) = 0;

    virtual std::size_t GetRegisterBankCount() const = 0;
    virtual bool DescribeRegisterBank(std::size_t index,
        GC_VideoRegisterBankDescriptor& descriptor) const = 0;
    virtual std::size_t GetRegisterCount(const char* bankCode) const = 0;
    virtual bool DescribeRegister(const char* bankCode, std::size_t index,
        GC_VideoRegisterDescriptor& descriptor) const = 0;
    virtual std::size_t GetRegisterFieldCount(const char* bankCode,
        u32 registerIndex) const = 0;
    virtual bool DescribeRegisterField(const char* bankCode, u32 registerIndex,
        std::size_t fieldIndex, GC_VideoRegisterFieldDescriptor& descriptor) const = 0;
    virtual bool ReadRegisterSnapshot(const char* bankCode, u32 registerIndex,
        u32& value) const = 0;
    virtual bool ReadRegisterDerivedAddressSnapshot(const char* bankCode,
        u32 registerIndex, u32& address) const = 0;
    // The VRAM tables as currently programmed, at most `capacity` of them.
    // Returns how many were written; 0 means this device does not describe
    // its layout (yet), and its VRAM writes stay unclassified.
    virtual std::size_t DescribeVramTables(GC_VramTableSpan* spans,
        std::size_t capacity) const
    {
        (void)spans;
        (void)capacity;
        return 0;
    }

    virtual bool DescribeDebugScene(GC_VideoDebugSceneDescriptor& descriptor) const = 0;
    virtual std::size_t GetDebugLayerCount() const = 0;
    virtual bool DescribeDebugLayer(std::size_t index,
        GC_VideoDebugLayerDescriptor& descriptor) const = 0;
    virtual std::size_t GetDebugViewportRegionCount(std::size_t index) const
    {
        GC_VideoDebugLayerDescriptor layer;
        return DescribeDebugLayer(index, layer) && layer.has_viewport ? 1 : 0;
    }
    virtual bool DescribeDebugViewportRegion(std::size_t index,
        std::size_t regionIndex,
        GC_VideoDebugViewportRegionDescriptor& descriptor) const
    {
        GC_VideoDebugLayerDescriptor layer;
        if (regionIndex != 0 || !DescribeDebugLayer(index, layer) ||
            !layer.has_viewport)
            return false;
        descriptor = {};
        descriptor.source_x = layer.viewport_x;
        descriptor.source_y = layer.viewport_y;
        descriptor.width = layer.viewport_width;
        descriptor.height = layer.viewport_height;
        return true;
    }
    virtual bool ReadDebugLayerSurface(std::size_t index,
        GC_VideoDebugLayerSurface& surface) const = 0;
    virtual std::size_t GetDebugLayerOptionCount(std::size_t) const { return 0; }
    virtual bool DescribeDebugLayerOption(std::size_t, std::size_t,
        GC_VideoDebugLayerOptionDescriptor&) const { return false; }
    virtual bool ReadDebugLayerSurfaceWithOptions(std::size_t index, u64,
        GC_VideoDebugLayerSurface& surface) const
    {
        return ReadDebugLayerSurface(index, surface);
    }
    virtual std::size_t GetDebugLayerAreaCount(std::size_t index) const = 0;
    virtual bool DescribeDebugLayerArea(std::size_t index, std::size_t areaIndex,
        GC_VideoDebugLayerAreaDescriptor& descriptor) const = 0;
    virtual bool DescribeDebugGridCell(std::size_t index, u32 column, u32 row,
        GC_VideoDebugGridCell& cell) const = 0;
    virtual std::size_t GetDebugGridCellFieldCount(std::size_t index,
        u32 column, u32 row) const = 0;
    virtual bool DescribeDebugGridCellField(std::size_t index, u32 column,
        u32 row, std::size_t fieldIndex,
        GC_VideoDebugGridCellFieldDescriptor& descriptor) const = 0;

    virtual std::size_t GetDebugPreviewPartCount() const = 0;
    virtual bool DescribeDebugPreviewPart(std::size_t index,
        GC_VideoDebugPreviewPartDescriptor& descriptor) const = 0;
    virtual std::size_t GetDebugPreviewOptionCount() const = 0;
    virtual bool DescribeDebugPreviewOption(std::size_t index,
        GC_VideoDebugPreviewOptionDescriptor& descriptor) const = 0;
    virtual bool ReadDebugPreviewSurface(u64 visiblePartMask, u64 optionMask,
        GC_VideoDebugLayerSurface& surface) const = 0;

    virtual std::size_t GetDebugSpriteSourceCount() const = 0;
    virtual bool DescribeDebugSpriteSource(std::size_t index,
        GC_VideoDebugSpriteSourceDescriptor& descriptor) const = 0;
    virtual bool ReadDebugSpriteAtlas(const char* sourceCode, int scanline,
        GC_VideoDebugSpriteAtlasSurface& surface) const = 0;
    virtual bool DescribeDebugSpriteParameter(const char* sourceCode,
        int& minimum, int& maximum) const = 0;
    virtual std::size_t GetDebugSpriteSourceFieldCount(
        const char* sourceCode, int scanline) const = 0;
    virtual bool DescribeDebugSpriteSourceField(const char* sourceCode,
        int scanline, std::size_t fieldIndex,
        GC_VideoDebugGridCellFieldDescriptor& descriptor) const = 0;
    virtual std::size_t GetDebugSpriteItemFieldCount(const char* sourceCode,
        int scanline, u32 spriteIndex) const = 0;
    virtual bool DescribeDebugSpriteItemField(const char* sourceCode,
        int scanline, u32 spriteIndex, std::size_t fieldIndex,
        GC_VideoDebugGridCellFieldDescriptor& descriptor) const = 0;
    virtual std::size_t GetDebugSpriteAttributeTableCount() const = 0;
    virtual bool DescribeDebugSpriteAttributeTable(std::size_t index,
        GC_VideoDebugSpriteAttributeTableDescriptor& descriptor) const = 0;
    virtual bool DescribeDebugSpriteAttributeColumn(std::size_t tableIndex,
        std::size_t columnIndex,
        GC_VideoDebugSpriteAttributeColumnDescriptor& descriptor) const = 0;
    virtual bool DescribeDebugSpriteAttributeRecord(std::size_t tableIndex,
        u32 recordIndex,
        GC_VideoDebugSpriteAttributeRecordDescriptor& descriptor) const = 0;
    virtual bool ReadDebugSpriteAttributeValue(std::size_t tableIndex,
        u32 recordIndex, u32 columnIndex, u64& value) const = 0;
};

#endif // IVIDEO_DEBUG_PROVIDER_H
