/*
 * GearSF7000 - default implementation for optional video debug capabilities
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef VIDEO_DEBUG_PROVIDER_BASE_H
#define VIDEO_DEBUG_PROVIDER_BASE_H

#include "IVideoDebugProvider.h"

// A video device overrides only the capabilities it can describe faithfully.
// Empty catalogues keep partially implemented and future VDPs usable without
// teaching the GUI which concrete class they belong to.
class VideoDebugProviderBase : public IVideoDebugProvider
{
public:
    std::size_t GetStateValueCount() const override { return 0; }
    bool DescribeStateValue(std::size_t,
        GC_VideoStateValueDescriptor&) const override { return false; }
    bool ReadStateValueSnapshot(std::size_t, u64&) const override { return false; }
    bool ReadStateTextSnapshot(std::size_t, const char*&) const override { return false; }

    std::size_t GetMemorySpaceCount() const override { return 0; }
    bool DescribeMemorySpace(std::size_t,
        GC_VideoMemorySpaceDescriptor&) const override { return false; }
    std::size_t GetMemoryRegionCount(const char*) const override { return 0; }
    bool DescribeMemoryRegion(const char*, std::size_t,
        GC_VideoMemoryRegionDescriptor&) const override { return false; }
    bool ReadMemorySpace(const char*, u32, u8*, std::size_t) const override
        { return false; }
    bool WriteMemorySpace(const char*, u32, const u8*, std::size_t) override
        { return false; }

    std::size_t GetRegisterBankCount() const override { return 0; }
    bool DescribeRegisterBank(std::size_t,
        GC_VideoRegisterBankDescriptor&) const override { return false; }
    std::size_t GetRegisterCount(const char*) const override { return 0; }
    bool DescribeRegister(const char*, std::size_t,
        GC_VideoRegisterDescriptor&) const override { return false; }
    std::size_t GetRegisterFieldCount(const char*, u32) const override { return 0; }
    bool DescribeRegisterField(const char*, u32, std::size_t,
        GC_VideoRegisterFieldDescriptor&) const override { return false; }
    bool ReadRegisterSnapshot(const char*, u32, u32&) const override { return false; }
    bool ReadRegisterDerivedAddressSnapshot(const char*, u32, u32&) const override
        { return false; }

    bool DescribeDebugScene(GC_VideoDebugSceneDescriptor&) const override
        { return false; }
    std::size_t GetDebugLayerCount() const override { return 0; }
    bool DescribeDebugLayer(std::size_t,
        GC_VideoDebugLayerDescriptor&) const override { return false; }
    bool ReadDebugLayerSurface(std::size_t,
        GC_VideoDebugLayerSurface&) const override { return false; }
    std::size_t GetDebugLayerAreaCount(std::size_t) const override { return 0; }
    bool DescribeDebugLayerArea(std::size_t, std::size_t,
        GC_VideoDebugLayerAreaDescriptor&) const override { return false; }
    bool DescribeDebugGridCell(std::size_t, u32, u32,
        GC_VideoDebugGridCell&) const override { return false; }
    std::size_t GetDebugGridCellFieldCount(std::size_t, u32, u32) const override
        { return 0; }
    bool DescribeDebugGridCellField(std::size_t, u32, u32, std::size_t,
        GC_VideoDebugGridCellFieldDescriptor&) const override { return false; }

    std::size_t GetDebugPreviewPartCount() const override { return 0; }
    bool DescribeDebugPreviewPart(std::size_t,
        GC_VideoDebugPreviewPartDescriptor&) const override { return false; }
    std::size_t GetDebugPreviewOptionCount() const override { return 0; }
    bool DescribeDebugPreviewOption(std::size_t,
        GC_VideoDebugPreviewOptionDescriptor&) const override { return false; }
    bool ReadDebugPreviewSurface(u64, u64,
        GC_VideoDebugLayerSurface&) const override { return false; }

    std::size_t GetDebugSpriteSourceCount() const override { return 0; }
    bool DescribeDebugSpriteSource(std::size_t,
        GC_VideoDebugSpriteSourceDescriptor&) const override { return false; }
    bool ReadDebugSpriteAtlas(const char*, int,
        GC_VideoDebugSpriteAtlasSurface&) const override { return false; }
    bool DescribeDebugSpriteParameter(const char*, int&, int&) const override
        { return false; }
    std::size_t GetDebugSpriteSourceFieldCount(const char*, int) const override
        { return 0; }
    bool DescribeDebugSpriteSourceField(const char*, int, std::size_t,
        GC_VideoDebugGridCellFieldDescriptor&) const override { return false; }
    std::size_t GetDebugSpriteItemFieldCount(const char*, int, u32) const override
        { return 0; }
    bool DescribeDebugSpriteItemField(const char*, int, u32, std::size_t,
        GC_VideoDebugGridCellFieldDescriptor&) const override { return false; }
    std::size_t GetDebugSpriteAttributeTableCount() const override { return 0; }
    bool DescribeDebugSpriteAttributeTable(std::size_t,
        GC_VideoDebugSpriteAttributeTableDescriptor&) const override
        { return false; }
    bool DescribeDebugSpriteAttributeColumn(std::size_t, std::size_t,
        GC_VideoDebugSpriteAttributeColumnDescriptor&) const override
        { return false; }
    bool DescribeDebugSpriteAttributeRecord(std::size_t, u32,
        GC_VideoDebugSpriteAttributeRecordDescriptor&) const override
        { return false; }
    bool ReadDebugSpriteAttributeValue(std::size_t, u32, u32, u64&) const override
        { return false; }
};

#endif
