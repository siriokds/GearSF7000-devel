/*
 * GearSF7000 - TMS9918B video device
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef TMS9918B_H
#define TMS9918B_H

#include <array>
#include <memory>

#include "TMS99xx.h"

// The B device deliberately starts through the exact TMS9918A implementation.
// Its extended register file and renderers are layered on this compatibility
// state only after the documented unlock sequence succeeds.
class TMS9918B final : public TMS99xx
{
public:
    TMS9918B(Memory* memory, CpuInterruptLines* interruptLines);
    void Init() override;
    void Reset() override;
    void SaveState(std::ostream& stream) override;
    void LoadState(std::istream& stream) override;
    u16 GetStateFormatVersion() const override
    {
        // A separate range prevents a B payload from ever being mistaken for
        // the shorter base payload, while retaining the base renderer variant.
        return static_cast<u16>(200 + TMS99xx::GetStateFormatVersion());
    }
    bool GetPaletteView(GC_VideoPaletteView& view) const override;
    GC_VideoFrameDescriptor GetFrameDescriptor() const override;
    GC_VideoFrameView GetFrameView(GC_Color_Format pixelFormat) override;
    bool DescribeRegisterBank(std::size_t index,
        GC_VideoRegisterBankDescriptor& descriptor) const override;
    std::size_t GetRegisterCount(const char* bankCode) const override;
    bool DescribeRegister(const char* bankCode, std::size_t index,
        GC_VideoRegisterDescriptor& descriptor) const override;
    std::size_t GetRegisterFieldCount(const char* bankCode,
        u32 registerIndex) const override;
    bool DescribeRegisterField(const char* bankCode, u32 registerIndex,
        std::size_t fieldIndex,
        GC_VideoRegisterFieldDescriptor& descriptor) const override;
    bool ReadRegisterSnapshot(const char* bankCode, u32 registerIndex,
        u32& value) const override;
    bool DescribeDebugScene(GC_VideoDebugSceneDescriptor& descriptor) const override;
    std::size_t GetDebugLayerCount() const override;
    bool DescribeDebugLayer(std::size_t index,
        GC_VideoDebugLayerDescriptor& descriptor) const override;
    std::size_t GetDebugViewportRegionCount(std::size_t index) const override;
    bool DescribeDebugViewportRegion(std::size_t index,
        std::size_t regionIndex,
        GC_VideoDebugViewportRegionDescriptor& descriptor) const override;
    bool ReadDebugLayerSurface(std::size_t index,
        GC_VideoDebugLayerSurface& surface) const override;
    std::size_t GetDebugLayerOptionCount(std::size_t index) const override;
    bool DescribeDebugLayerOption(std::size_t layerIndex,
        std::size_t optionIndex,
        GC_VideoDebugLayerOptionDescriptor& descriptor) const override;
    bool ReadDebugLayerSurfaceWithOptions(std::size_t index, u64 optionMask,
        GC_VideoDebugLayerSurface& surface) const override;
    std::size_t GetDebugLayerAreaCount(std::size_t index) const override;
    bool DescribeDebugLayerArea(std::size_t index, std::size_t areaIndex,
        GC_VideoDebugLayerAreaDescriptor& descriptor) const override;
    bool DescribeDebugGridCell(std::size_t index, u32 column, u32 row,
        GC_VideoDebugGridCell& cell) const override;
    std::size_t GetDebugGridCellFieldCount(std::size_t index,
        u32 column, u32 row) const override;
    bool DescribeDebugGridCellField(std::size_t index, u32 column, u32 row,
        std::size_t fieldIndex,
        GC_VideoDebugGridCellFieldDescriptor& descriptor) const override;
    bool ReadDebugSpriteAtlas(const char* sourceCode, int scanline,
        GC_VideoDebugSpriteAtlasSurface& surface) const override;
    bool DescribeDebugSpriteAttributeTable(std::size_t index,
        GC_VideoDebugSpriteAttributeTableDescriptor& descriptor) const override;
    bool DescribeDebugSpriteAttributeColumn(std::size_t tableIndex,
        std::size_t columnIndex,
        GC_VideoDebugSpriteAttributeColumnDescriptor& descriptor) const override;
    bool DescribeDebugSpriteAttributeRecord(std::size_t tableIndex,
        u32 recordIndex,
        GC_VideoDebugSpriteAttributeRecordDescriptor& descriptor) const override;

    bool IsExtendedUnlocked() const { return m_extendedUnlocked; }
    u8 GetExtendedRegister(unsigned index) const
    {
        return index < m_extendedRegisters.size()
            ? m_extendedRegisters[index] : 0;
    }
    u8 GetProgrammablePaletteEntry(unsigned index) const
    {
        return index < m_paletteEntries.size() ? m_paletteEntries[index] : 0;
    }

protected:
    bool HandleExtendedControlCommand(uint64_t tstates, u8 command,
        u8 firstByte) override;
    bool HandleExtendedRegisterWrite(u8 rawRegister, u8 value) override;
    void OnLegacyRegisterWritten(u8 reg) override;
    bool HandleExtendedDataWrite(u8 data) override;
    bool HandleExtendedStatusRead(u8& value) override;
    int NextDerivedRasterEventPhase(int line, int phase) const override;
    void OnDerivedRasterEvent(int line, int phase) override;
    void OnRasterLineCompleted(int line) override;
    bool ExtendedInterruptPending() const override;
    const std::array<bool,
        TMS99xxVramSequencer::PhasesPerLine>* GetCpuVRAMPhaseMap() const override;
    bool UseLegacyTimedVideoFetches() const override
        { return !IsExtendedDisplayMode(); }

private:
    bool ExtendedEnable() const { return (m_extendedRegisters[3] & 0x01) != 0; }
    void RebuildPaletteEntry(unsigned index);
    bool IsExtendedDisplayMode() const;
    void RenderExtendedLine(int line);
    void RenderExtendedSprites(int line, unsigned mode,
        bool* backgroundPriority);
    void PutExtendedPixel(int x, int y, unsigned colour, bool programmable);
    void BuildCpuPhaseMaps();
    u64 DebugSurfaceSignature() const;
    const u8* GetDebugProgrammablePalette() const;
    void BuildExtendedNameDebugSurface() const;
    void BuildExtendedPatternDebugSurface() const;
    void BuildExtendedSpritePatternDebugSurface(u64 optionMask) const;
    void BuildExtendedLiveSpriteAtlas() const;
    void BuildDebugViewportRegions() const;

    bool m_extendedUnlocked = false;
    u8 m_unlockProgress = 0;
    bool m_paletteWriteMode = false;
    u8 m_paletteAddress = 0;
    u8 m_status1 = 0x18;
    u8 m_lineCounter = 0;
    u8 m_latchedHorizontalScroll = 0;
    u8 m_latchedVerticalScroll = 0;
    std::array<u8, GC_RESOLUTION_HEIGHT> m_scrollFrameHorizontal{};
    std::array<u8, GC_RESOLUTION_HEIGHT> m_scrollFrameVertical{};
    std::array<u8, GC_RESOLUTION_HEIGHT> m_scrollFrameControl{};
    std::array<u8, GC_RESOLUTION_HEIGHT> m_lastScrollFrameHorizontal{};
    std::array<u8, GC_RESOLUTION_HEIGHT> m_lastScrollFrameVertical{};
    std::array<u8, GC_RESOLUTION_HEIGHT> m_lastScrollFrameControl{};
    bool m_lastScrollFrameValid = false;
    std::array<u8, 8> m_extendedRegisters{};
    std::array<u8, 16> m_paletteEntries{};
    std::array<u8, 16 * 3> m_paletteRgb{};
    u64 m_paletteRevision = 1;
    std::array<u8, 16 * 3> m_debugFramePaletteRgb{};
    u64 m_debugFramePaletteRevision = 1;
    bool m_debugFramePaletteValid = false;
    std::unique_ptr<u8[]> m_extendedRgb;
    std::unique_ptr<u8[]> m_extendedPacked;
    mutable std::unique_ptr<u8[]> m_debugNameRgba;
    mutable std::unique_ptr<u8[]> m_debugPatternRgba;
    mutable std::unique_ptr<u8[]> m_debugSpritePatternRgba;
    mutable std::unique_ptr<u8[]> m_debugSpriteAtlasRgba;
    mutable std::array<GC_VideoDebugSpriteAtlasItemDescriptor, 32>
        m_debugSpriteAtlasItems{};
    mutable std::array<u64, 3> m_debugBuiltVramRevision{};
    mutable std::array<u64, 3> m_debugBuiltSignature{};
    mutable std::array<u64, 3> m_debugSurfaceGeneration{};
    mutable u64 m_debugSpriteBuiltOptionMask = ~0ull;
    mutable u64 m_debugSpriteAtlasBuiltVramRevision = 0;
    mutable u64 m_debugSpriteAtlasBuiltSignature = 0;
    mutable u64 m_debugSpriteAtlasGeneration = 0;
    // Raster scroll is summarized at the Name Table's eight-line granularity.
    // VLOCK can split a tile row into two independently mapped source regions.
    mutable std::array<GC_VideoDebugViewportRegionDescriptor, 48>
        m_debugViewportRegions{};
    mutable std::size_t m_debugViewportRegionCount = 0;
    u64 m_extendedSourceRevision = 1;
    u64 m_extendedPackedRevision = 0;
    u64 m_extendedPackedDescriptorRevision = 0;
    u64 m_descriptorRevision = 1;
    int m_extendedPackedFormat = -1;
    std::array<std::array<bool, TMS99xxVramSequencer::PhasesPerLine>, 4>
        m_cpuPhaseMaps{};
};

#endif // TMS9918B_H
