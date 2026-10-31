// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#ifndef PICO9918_H
#define PICO9918_H

#include <array>
#include <vector>

#include "IVideoDevice.h"
#include "Pico9918LegacyCore.h"
#include "VideoDebugProviderBase.h"

class Memory;
class CpuInterruptLines;

namespace Pico9918DebugTarget
{
    enum State : u16
    {
        AddressCounter = 0,
        ControlLatchPending,
        ReadBuffer,
        Status,
        IRQ,
        VideoMode,
        ControlLatchValue,
        Unlocked,
        UnlockStep,
        StatusSelect,
        PaletteMode,
        PaletteWriteStep
    };
}

#if GEARSF7000_ENABLE_PICO9918
class Pico9918 final : public IVideoDevice, public VideoDebugProviderBase
{
public:
    Pico9918(Memory* memory, CpuInterruptLines* interruptLines,
        SystemVideoDevice device = SystemVideoDevice::Pico9918);

    void Init() override;
    void Reset() override;
    bool TickDeviceClocks(unsigned int deviceClocks) override;
    int GetRenderLine() const override { return m_rasterLine; }

    u8 GetDataPort(std::uint64_t tstates) override;
    u8 GetStatusFlags() override;
    void WriteData(std::uint64_t tstates, u8 data) override;
    void WriteControl(std::uint64_t tstates, u8 control) override;

    void SaveState(std::ostream& stream) override;
    void LoadState(std::istream& stream) override;
    bool SupportsSmsCounters() const override
    {
        return m_legacy.SupportsSmsCounters();
    }
    void ConfigureSmsRaster(bool pal) override;
    u8 ReadSmsVCounter(std::uint64_t tstates) override;
    u8 ReadSmsHCounter(std::uint64_t tstates) override;
    void LatchSmsHCounter(std::uint64_t tstates) override;
    u16 GetStateFormatVersion() const override { return 4; }

    const GC_VideoDeviceDescriptor& GetDeviceDescriptor() const override;
    IVideoDebugProvider* GetDebugProvider() override { return this; }
    const IVideoDebugProvider* GetDebugProvider() const override { return this; }
    u64 GetFrameSerial() const override { return m_frameSerial; }
    GC_VideoFrameDescriptor GetFrameDescriptor() const override;
    GC_VideoFrameView GetFrameView(GC_Color_Format pixelFormat) override;

    std::size_t GetOptionCount() const override;
    bool DescribeOption(std::size_t index,
        GC_VideoOptionDescriptor& descriptor) const override;
    bool SetOption(const char* code, s64 value) override;
    std::size_t GetColorProfileCount() const override;
    bool DescribeColorProfile(std::size_t index,
        GC_VideoColorProfileDescriptor& descriptor) const override;
    bool GetPaletteView(GC_VideoPaletteView& view) const override;
    void CopyHostConfigurationFrom(const IVideoDevice& source) override;
    // The Game Gear's window: the LCD shows the middle 160x144 of a picture the
    // VDP still renders whole. Only the presented rectangle (visible_area)
    // changes - the frame, the debug views and a full-frame capture keep what
    // the window hides. Off (the default) is the SMS: the whole picture. Set by
    // the machine, like the core's ggCram; not a user option, not saved.
    void SetGameGearWindow(bool enabled);
    bool IsGameGearWindow() const { return m_ggWindow; }
    void SetGameGearCram(bool enabled);
    bool IsGameGearCram() const { return m_ggCram; }
    DebugDeviceDefinition DescribeDebugDevice() const override;

    std::size_t GetStateValueCount() const override;
    bool DescribeStateValue(std::size_t index,
        GC_VideoStateValueDescriptor& descriptor) const override;
    bool ReadStateValueSnapshot(std::size_t index, u64& value) const override;
    bool ReadStateTextSnapshot(std::size_t index, const char*& value) const override;
    std::size_t GetMemorySpaceCount() const override { return 1; }
    bool DescribeMemorySpace(std::size_t index,
        GC_VideoMemorySpaceDescriptor& descriptor) const override;
    std::size_t GetMemoryRegionCount(const char* spaceCode) const override;
    bool DescribeMemoryRegion(const char* spaceCode, std::size_t index,
        GC_VideoMemoryRegionDescriptor& descriptor) const override;
    bool ReadMemorySpace(const char* spaceCode, u32 offset,
        u8* destination, std::size_t size) const override;
    bool WriteMemorySpace(const char* spaceCode, u32 offset,
        const u8* source, std::size_t size) override;
    std::size_t GetRegisterBankCount() const override { return 2; }
    bool DescribeRegisterBank(std::size_t index,
        GC_VideoRegisterBankDescriptor& descriptor) const override;
    std::size_t GetRegisterCount(const char* bankCode) const override;
    bool DescribeRegister(const char* bankCode, std::size_t index,
        GC_VideoRegisterDescriptor& descriptor) const override;
    bool ReadRegisterSnapshot(const char* bankCode, u32 registerIndex,
        u32& value) const override;
    bool ReadRegisterDerivedAddressSnapshot(const char* bankCode,
        u32 registerIndex, u32& address) const override;

    bool DescribeDebugScene(GC_VideoDebugSceneDescriptor& descriptor) const override;
    std::size_t GetDebugLayerCount() const override { return 3; }
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

    std::size_t GetDebugPreviewPartCount() const override { return 2; }
    bool DescribeDebugPreviewPart(std::size_t index,
        GC_VideoDebugPreviewPartDescriptor& descriptor) const override;
    std::size_t GetDebugPreviewOptionCount() const override { return 1; }
    bool DescribeDebugPreviewOption(std::size_t index,
        GC_VideoDebugPreviewOptionDescriptor& descriptor) const override;
    bool ReadDebugPreviewSurface(u64 visiblePartMask, u64 optionMask,
        GC_VideoDebugLayerSurface& surface) const override;

    std::size_t GetDebugSpriteSourceCount() const override { return 1; }
    bool DescribeDebugSpriteSource(std::size_t index,
        GC_VideoDebugSpriteSourceDescriptor& descriptor) const override;
    bool ReadDebugSpriteAtlas(const char* sourceCode, int scanline,
        GC_VideoDebugSpriteAtlasSurface& surface) const override;
    std::size_t GetDebugSpriteAttributeTableCount() const override { return 1; }
    bool DescribeDebugSpriteAttributeTable(std::size_t index,
        GC_VideoDebugSpriteAttributeTableDescriptor& descriptor) const override;
    bool DescribeDebugSpriteAttributeColumn(std::size_t tableIndex,
        std::size_t columnIndex,
        GC_VideoDebugSpriteAttributeColumnDescriptor& descriptor) const override;
    bool DescribeDebugSpriteAttributeRecord(std::size_t tableIndex,
        u32 recordIndex,
        GC_VideoDebugSpriteAttributeRecordDescriptor& descriptor) const override;
    bool ReadDebugSpriteAttributeValue(std::size_t tableIndex, u32 recordIndex,
        u32 columnIndex, u64& value) const override;

private:
    static constexpr std::size_t RegisterCount = 64;
    static constexpr std::size_t StatusCount = 16;

    void ResetExtendedState(bool powerOn);
    bool WriteRegisterCommand(u8 registerIndex, u8 value,
        u8& finalRegisterIndex, u8& beforeValue, u8& afterValue);
    u8 ReadControlRegister(u8 registerIndex) const;
    u8 ReadSelectedStatus();
    std::int16_t AddressIncrementAdjustment() const;
    void ResetPalette();
    void SyncBackendPalette();
    void RebuildPaletteEntry(std::size_t index);
    bool HandlePaletteWrite(u8 data);
    void PublishStateChanges(const Pico9918LegacyCore::DebugState& before,
        Pico9918LegacyCore::Mode beforeMode, DebugEventSource source);
    void PublishExtendedStateChanges(bool beforeUnlocked, u8 beforeUnlockCount,
        u8 beforeStatusSelect, DebugEventSource source);
    void PublishPaletteStateChanges(bool beforeMode, u8 beforeStep,
        DebugEventSource source);
    void PublishVramAccess(u16 address, bool write, u8 beforeValue,
        u8 afterValue);
    u8 ReadBackingMemory(u32 address) const;
    bool IsBackingMemoryWritable(u32 address) const;
    void WriteBackingMemory(u32 address, u8 value);
    void BuildDebugLayerSurface(std::size_t index, u64 optionMask) const;
    void BuildDebugPreviewSurface(u64 visiblePartMask, u64 optionMask) const;
    // The firmware starts from nominal 640x480 VGA timings, then quantizes
    // every horizontal segment to the RP2040 PIO clock. At the default
    // 252 MHz system clock the PIO divider is 3 (84 MHz). RGB pixels consume
    // three PIO ticks (28 MHz effective), while active/porch/sync durations
    // total 2668 PIO ticks per line. This is VGA-compatible timing rather
    // than an exact 25.175 MHz, 800-dot VGA raster.
    static constexpr int RasterWidth = 2668;
    static constexpr int RasterHeight = 525;
    static constexpr int FrameWidth = 640;
    static constexpr int FrameHeight = 480;
    static constexpr int ContentWidth = 512;
    static constexpr int ContentX = (FrameWidth - ContentWidth) / 2;
    static constexpr u32 DeviceClockHz = 84000000;

    // The original SMS VDP has a 342-dot, 262-line NTSC raster. Its master
    // oscillator advances twice per dot and is exactly three times the Z80
    // clock, hence 684 device clocks per scanline.
    static constexpr int SmsRasterWidth = GC_VDP_DOTS_PER_LINE;
    static constexpr int SmsRasterHeight = GC_LINES_PER_FRAME_NTSC;
    static constexpr int SmsClocksPerLine = GC_VDP_MASTER_CLOCKS_PER_LINE;
    // SMS1/315-5124 timing in the 342-dot scanline coordinate used by MAME's
    // hardware-tested VDP model. This device advances two master phases per
    // dot. R8 is copied just before HINT so the handler prepares the following
    // scanline, as required by raster-road games such as Out Run.
    static constexpr unsigned int SmsScrollLatchClock =
        21 * GC_VDP_MASTER_CLOCKS_PER_DOT;
    static constexpr unsigned int SmsLineInterruptClock =
        26 * GC_VDP_MASTER_CLOCKS_PER_DOT;
    static constexpr int SmsFrameBufferWidth = 284;
    static constexpr int SmsFrameBufferHeight = 240;
    static constexpr u32 SmsDeviceClockHz = GC_VDP_MASTER_CLOCK_HZ;

    bool HasDoubleRows() const;
    bool HasThirtyRows() const;
    int VerticalPixelScale() const;
    int LogicalContentHeight() const;
    int PhysicalContentHeight() const;
    int PhysicalContentY() const;
    bool IsNativeSmsVdp() const;
    bool UsesFixedTmsPalette() const;
    bool HasTmsColorProfiles() const;
    void SetTmsFixedPalette(int profile);
    const u8* DrawPalette(u8& mask) const;
    int TimingClocksPerLine() const;
    int TimingRasterHeight() const;
    int FrameBufferWidth() const;
    int FrameBufferHeight() const;
    void RenderRasterLine(int line);
    void RenderNativeSmsRasterLine(int line);
    void BuildSmsDebugScanline(int logicalLine, u8* background,
        u8* spriteMask);
    void PredictNextLegacySpriteOverflow(int logicalLine);
    void UpdateInterrupt(
        DebugEventSource source = DebugEventSource::DeviceInternal);
    void SetInterrupt(bool asserted,
        DebugEventSource source = DebugEventSource::DeviceInternal);
    void SetSmsRasterFromCpuTime(std::uint64_t tstates);

    Memory* m_memory = nullptr;
    CpuInterruptLines* m_interruptLines = nullptr;
    SystemVideoDevice m_device = SystemVideoDevice::Pico9918;
    Pico9918LegacyCore m_legacy;
    std::vector<u8> m_frame;
    std::array<u8, Pico9918LegacyCore::Width> m_scanline{};
    unsigned int m_rasterClock = 0;
    int m_rasterLine = 0;
    u64 m_frameSerial = 0;
    s64 m_border = GC_VideoBorder::Disabled;
    bool m_ggWindow = false;
    bool m_ggCram = false;
    void ApplyGameGearWindow(GC_VideoFrameDescriptor& descriptor) const;
    u64 m_frameDescriptorRevision = 1;
    bool m_interruptAsserted = false;
    bool m_frameInterruptFlag = false;
    bool m_smsPal = false;
    std::array<u8, RegisterCount> m_extendedRegisters{};
    std::array<u8, StatusCount> m_statusRegisters{};
    std::array<u16, 64> m_paletteEntries{};
    std::array<u8, 64 * 3> m_paletteRgb{};
    u64 m_paletteRevision = 1;
    // picoSMSvdp's TMS modes on a locked chip: sixteen fixed colours that
    // CRAM writes cannot reach (picoSMSvdp-core tmsPalette).
    std::array<u16, 16> m_tmsFixedEntries{};
    std::array<u8, 16 * 3> m_tmsFixedRgb{};
    // Which sixteen, on the Mark III bus only: 0 = picoSMSvdp's TMS9918
    // colours, 1 = the real 315-5124's. Presentation, not machine state.
    int m_tmsFixedProfile = 0;
    // The Palettes panel's view in those modes: CRAM, then the fixed sixteen.
    mutable std::array<u8, 48 * 3> m_panelRgb{};
    mutable std::array<u16, 48> m_panelEntries{};
    u8 m_paletteWriteStage = 0;
    u8 m_paletteWriteFirstByte = 0;
    u8 m_unlockCount = 0;
    bool m_unlocked = false;
    u8 m_smsDebugLatchedVScroll = 0;
    mutable std::array<u8, 256 * 480 * 4> m_debugNameTable{};
    mutable std::array<u8, 256 * 256 * 4> m_debugPatternTable{};
    mutable std::array<u8, 256 * 64 * 4> m_debugSpritePatterns{};
    mutable std::array<u8, 128 * 128 * 4> m_debugSpriteAtlas{};
    std::array<u8, 256 * 480> m_debugBackground{};
    std::array<u8, 256 * 480> m_debugComposed{};
    std::array<u8, 256 * 480> m_debugSpriteMask{};
    mutable std::array<u8, 256 * 480 * 4> m_debugPreview{};
    mutable std::array<GC_VideoDebugSpriteAtlasItemDescriptor, 64>
        m_debugSpriteItems{};
    mutable u64 m_debugSurfaceGeneration = 0;
    u64 m_debugPreviewSourceGeneration = 1;
    mutable u64 m_debugPreviewBuiltSourceGeneration = 0;
    mutable u64 m_debugPreviewGeneration = 0;
    mutable u64 m_debugPreviewBuiltPartMask = 0;
    mutable u64 m_debugPreviewBuiltOptionMask = 0;
};
#endif

#endif
