/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2021  Ignacio Sanchez
 * Copyright (C) 2024-2026  Saverio Russo
 *
 * Derived from Gearcoleco by Ignacio Sanchez. His code is free software: you
 * can redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * The changes and additions by Saverio Russo are free software: you can
 * redistribute them and/or modify them under the terms of the GNU Affero
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 * Section 13 of both licences permits the combination.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License and the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of both licences along with this
 * program.  If not, see http://www.gnu.org/licenses/
 *
 * SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later
 */

#ifndef TMS99XX_H
#define TMS99XX_H

#include "definitions.h"
#include "DebugEvents.h"
#include "VideoFrame.h"
#include "TMS99xxCpuAccessTiming.h"
#include "TMS99xxRasterTiming.h"
#include "TMS99xxVramSequencer.h"
#include "TMS99xxDebugEvents.h"
#include "ICrtSignalSink.h"
#include "IVideoPortDevice.h"
#include "IVideoDevice.h"
#include "IVideoCrtSignalProvider.h"
#include "IVideoDebugProvider.h"
#include "ITMS99xxDebugProvider.h"

// The original per-line renderer, kept only as the oracle the streaming
// renderer was verified against. All eight mode combinations, sprite status
// timing and mid-line blanking now match it exactly (see
// gearsf7000-tms9918-open-conjectures for what still doesn't have a
// hardware-verified answer, none of it a rendering gap). Off by default so
// an ordinary build carries only one renderer; flip to 1 and rebuild to bring
// the comparison path back if a future change needs re-verifying against it.
#ifndef GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
#define GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER 0
#endif

class Memory;
class CpuInterruptLines;

class TMS99xx : public IVideoDevice, public IVideoDebugProvider,
                public ITMS99xxDebugProvider, public IVideoCrtSignalProvider
{
public:
    DebugDeviceDefinition DescribeDebugDevice() const override;

    enum VideoTimingEvent : u8
    {
        VideoTimingDisplay = 1 << 0,
        VideoTimingRender  = 1 << 1,
        VideoTimingVBlank  = 1 << 2
    };

    enum DebugStateTarget : u16
    {
        DebugAddressCounter = Tms99xxDebugTarget::AddressCounter,
        DebugControlLatch = Tms99xxDebugTarget::ControlLatch,
        DebugReadBuffer = Tms99xxDebugTarget::ReadBuffer,
        DebugStatus = Tms99xxDebugTarget::Status,
        DebugIRQ = Tms99xxDebugTarget::IRQ,
        DebugVideoMode = Tms99xxDebugTarget::VideoMode,
        DebugCpuPortLatchReplacement = Tms99xxDebugTarget::CpuPortLatchReplacement,
        // First byte written to $BF.  This is deliberately distinct from
        // the VRAM read buffer returned by $BE.
        DebugControlLatchValue = Tms99xxDebugTarget::ControlLatchValue
    };

    enum Overscan
    {
        OverscanDisabled,
        OverscanTopBottom,
        OverscanFull272,
        OverscanFull284,
        OverscanFull320
    };

    // Passive validation data for the legacy scanline renderer. It is the
    // baseline used when the streaming renderer is introduced: a frame with
    // zero CPU VRAM commits on visible display lines must be bit-identical.
    using FrameRenderDiagnostics = ITMS99xxDebugProvider::FrameRenderDiagnostics;

    struct BackgroundFetchLatch
    {
        u8 name = 0;
        u8 colour = 0;
        u8 pattern = 0;
        bool nameValid = false;
        bool colourValid = false;
        bool patternValid = false;
    };

    // A sprite scan has two distinctly timed parts: all 32 Y positions are
    // inspected during a source line, then the attributes/pattern rows of up
    // to four selected sprites are fetched across its end and the beginning
    // of the target line. This state is passive for now: the old RenderSprites
    // path remains authoritative until the two outputs can be compared.
    struct SpriteFetchLatch
    {
        u8 y = 0;
        u8 x = 0;
        u8 name = 0;
        u8 colour = 0;
        u8 pattern[2] = {0, 0};
        bool yValid = false;
        bool xValid = false;
        bool nameValid = false;
        bool colourValid = false;
        bool patternValid[2] = {false, false};
    };

    struct SpriteLineFetchState
    {
        u64 absoluteTargetLine = static_cast<u64>(-1);
        u8 selectedSprite[4] = {0, 0, 0, 0};
        u8 selectedRasterX[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        u8 selectedCount = 0;
        bool selectionTerminated = false;
        // The fifth sprite on the line. The scan keeps looking past the four
        // that fit, because R0 bit 6 reports the number of the fifth one and
        // programs use the moment it is set as a mid-line timing reference.
        bool overflowRecorded = false;
        u8 overflowSprite = 0;
        u8 overflowRasterX = 0xFF;
        u8 overflowSat[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        bool overflowSatValid = false;
        int collisionX = -1;
        SpriteFetchLatch sprite[4];
    };

#ifdef SPRITE_EXPANDER
    // Optional visual companion to the real four-sprite TMS9918 pipeline.
    // It never participates in selection, overflow, collision or status:
    // those remain entirely owned by SpriteLineFetchState above.
    static constexpr int kSpriteExpanderCapacity = 28;
    struct SpriteExpanderLineState
    {
        u64 absoluteTargetLine = static_cast<u64>(-1);
        u8 candidateCount = 0;
        u8 satIndex[kSpriteExpanderCapacity] = {};
        SpriteFetchLatch sprite[kSpriteExpanderCapacity];
        bool latched = false;
        bool large = false;
        bool magnified = false;
    };
#endif

    // Live, per-scanline history of every DRAM slot the beam has reached so
    // far this raster line: 171 slots covering all 342 dots, blanking
    // included, keyed by physical position rather than by which logical line
    // a fetch happens to belong to. Sprite selection deliberately straddles
    // the line boundary (its Y scan lives in the border of the *previous*
    // line), so a view restricted to the active 256 pixels would misplace or
    // hide it; this does not.
    using SlotActivityRecord = ITMS99xxDebugProvider::SlotActivityRecord;

    struct PendingSpriteFetch
    {
        TMS99xxVramSlotSchedule::Activity activity =
            TMS99xxVramSlotSchedule::Activity::Unknown;
        u64 absoluteTargetLine = 0;
        u8 index = 0;
        u8 byte = 0;
        bool candidateScan = false;
    };

public:
    // One sprite as the debugger wants it: what the attribute table says, plus
    // what the chip actually did with it on a given line.
    using SpriteDebugEntry = ITMS99xxDebugProvider::SpriteDebugEntry;

    using SpriteDebugSnapshot = ITMS99xxDebugProvider::SpriteDebugSnapshot;

    // What the timed sprite fetcher actually acquired for the last complete
    // frame. Unlike a live SAT decode this honours the four-sprites-per-line
    // limit and preserves rows fetched before software rewrote SAT/SPRPAT.
    using RenderedSpriteDebugEntry = ITMS99xxDebugProvider::RenderedSpriteDebugEntry;

    using RenderedSpriteScanlineSlot = ITMS99xxDebugProvider::RenderedSpriteScanlineSlot;

    using RenderedSpriteScanlineDebug = ITMS99xxDebugProvider::RenderedSpriteScanlineDebug;

    using RenderedSpriteDebugFrame = ITMS99xxDebugProvider::RenderedSpriteDebugFrame;

    using SpritePipelineLatchDebug = ITMS99xxDebugProvider::SpritePipelineLatchDebug;

    using SpritePipelineLineDebug = ITMS99xxDebugProvider::SpritePipelineLineDebug;

    // Cycle-current view of the two scanlines that can be in flight in the
    // TMS9918 sprite pipeline. This is deliberately separate from both a live
    // SAT decode and the summary of the last completed frame.
    using SpritePipelineDebugSnapshot = ITMS99xxDebugProvider::SpritePipelineDebugSnapshot;

    using SpriteSelectionHistoryRow = ITMS99xxDebugProvider::SpriteSelectionHistoryRow;

    using SpriteSelectionFrameHistory = ITMS99xxDebugProvider::SpriteSelectionFrameHistory;

    // Reads the sprite attribute table and crosses it with the chip's own
    // selection state. Pass -1 for the line the beam is on.
    SpriteDebugSnapshot GetSpriteDebugSnapshot(int line) override;
    SpritePipelineDebugSnapshot GetSpritePipelineDebugSnapshot() const override;
    const SpriteSelectionFrameHistory& GetSpriteSelectionFrameHistory() const override
    {
        return m_LastSpriteSelectionHistory;
    }
    const RenderedSpriteDebugFrame& GetLastRenderedSpriteDebugFrame() const override
    {
        return m_LastRenderedSpriteDebugFrame;
    }

    // The 16 RGB triplets currently in use, so a debug view can colour a
    // sprite the way the screen would. Follows the palette the user picked,
    // rather than baking one in.
    const u8* GetCurrentPalette() const { return m_pCurrentPalette; }

protected:
    TMS99xx(Memory* pMemory, CpuInterruptLines* interruptLines,
            SystemVideoDevice device);
    u64 GetDebugVRAMRevision() const { return m_DebugVRAMRevision; }
    u64 GetFixedPaletteRevision() const { return m_PaletteRevision; }

    // Socket-compatible successors can extend the two-port protocol without
    // copying the TMS9918A command decoder. A hook returns true only when it
    // has consumed the operation; the legacy path remains authoritative in
    // every other case.
    virtual bool HandleExtendedControlCommand(uint64_t,
        u8, u8) { return false; }
    virtual bool HandleExtendedRegisterWrite(u8, u8)
        { return false; }
    virtual void OnLegacyRegisterWritten(u8) {}
    virtual bool HandleExtendedDataWrite(u8) { return false; }
    virtual bool HandleExtendedStatusRead(u8&) { return false; }
    // Derived devices may insert a small number of events into the native
    // raster clock. A phase beyond the end of the line means no event.
    virtual int NextDerivedRasterEventPhase(int, int) const
        { return GC_VDP_MASTER_CLOCKS_PER_LINE + 1; }
    virtual void OnDerivedRasterEvent(int, int) {}
    virtual void OnRasterLineCompleted(int) {}
    virtual bool ExtendedInterruptPending() const { return false; }
    virtual const std::array<bool,
        TMS99xxVramSequencer::PhasesPerLine>* GetCpuVRAMPhaseMap() const
        { return nullptr; }
    virtual bool UseLegacyTimedVideoFetches() const { return true; }
    int GetBorderColorForOutputRow(
        const GC_VideoFrameDescriptor& descriptor, int row,
        bool leading) const
    {
        return BorderColorForRow(descriptor, row, leading);
    }
    void RefreshInterruptLine();
    void RaiseExtendedSpriteCollision();
    void RaiseExtendedSpriteOverflow(u8 sprite);

public:
    ~TMS99xx() override;
    void Init() override;
    void Reset() override;
    bool TickDeviceClocks(unsigned int masterClocks) override;
    // Compatibility entry point for tests/tools still expressed in the
    // historical NTSC-equivalent Z80 T-state unit.
    bool Tick(unsigned int clockCycles);
    u8 GetDataPort(uint64_t tstates) override;
    u8 GetStatusFlags() override;
    void WriteData(uint64_t tstates, u8 data) override;
    void WriteControl(uint64_t tstates, u8 control) override;
    void WriteVRAM(u16 address, u8 value);
    u8 ReadVRAM(u16 address);
    void SaveState(std::ostream& stream) override;
    void LoadState(std::istream& stream) override;
    u16 GetStateFormatVersion() const override;
    u8* GetVRAM();
    const u8* GetVRAM() const;
    u8* GetRegisters();
    const u8* GetRegisters() const;
    // TMS-specific indexed surface and conversion helpers retained for the
    // renderer comparison tests. Generic consumers use GetFrameView().
    u16* GetFrameBuffer();
    GC_VideoFrameDescriptor GetFrameDescriptor() const override;
    GC_VideoFrameView GetFrameView(GC_Color_Format pixelFormat) override;
    const GC_VideoDeviceDescriptor& GetDeviceDescriptor() const override;
    SystemVideoDevice GetDeviceType() const { return m_device; }
    void CopyHostConfigurationFrom(const IVideoDevice& source) override;
    std::size_t GetOptionCount() const override;
    bool DescribeOption(std::size_t index,
        GC_VideoOptionDescriptor& descriptor) const override;
    bool SetOption(const char* code, s64 value) override;
    std::size_t GetColorProfileCount() const override;
    bool DescribeColorProfile(std::size_t index,
        GC_VideoColorProfileDescriptor& descriptor) const override;
    bool GetPaletteView(GC_VideoPaletteView& view) const override;
    IVideoDebugProvider* GetDebugProvider() override { return this; }
    const IVideoDebugProvider* GetDebugProvider() const override { return this; }
    std::size_t GetStateValueCount() const override;
    bool DescribeStateValue(std::size_t index,
        GC_VideoStateValueDescriptor& descriptor) const override;
    bool ReadStateValueSnapshot(std::size_t index, u64& value) const override;
    bool ReadStateTextSnapshot(std::size_t index,
        const char*& value) const override;
    std::size_t GetMemorySpaceCount() const override;
    bool DescribeMemorySpace(std::size_t index,
        GC_VideoMemorySpaceDescriptor& descriptor) const override;
    std::size_t GetMemoryRegionCount(const char* spaceCode) const override;
    bool DescribeMemoryRegion(const char* spaceCode, std::size_t index,
        GC_VideoMemoryRegionDescriptor& descriptor) const override;
    bool ReadMemorySpace(const char* spaceCode, u32 offset,
        u8* destination, std::size_t size) const override;
    bool WriteMemorySpace(const char* spaceCode, u32 offset,
        const u8* source, std::size_t size) override;
    std::size_t GetRegisterBankCount() const override;
    bool DescribeRegisterBank(std::size_t index,
        GC_VideoRegisterBankDescriptor& descriptor) const override;
    std::size_t GetRegisterCount(const char* bankCode) const override;
    bool DescribeRegister(const char* bankCode, std::size_t index,
        GC_VideoRegisterDescriptor& descriptor) const override;
    std::size_t GetRegisterFieldCount(const char* bankCode,
        u32 registerIndex) const override;
    bool DescribeRegisterField(const char* bankCode, u32 registerIndex,
        std::size_t fieldIndex, GC_VideoRegisterFieldDescriptor& descriptor) const override;
    bool ReadRegisterSnapshot(const char* bankCode, u32 registerIndex,
        u32& value) const override;
    std::size_t DescribeVramTables(GC_VramTableSpan* spans,
        std::size_t capacity) const override;
    bool ReadRegisterDerivedAddressSnapshot(const char* bankCode,
        u32 registerIndex, u32& address) const override;
    bool DescribeDebugScene(GC_VideoDebugSceneDescriptor& descriptor) const override;
    std::size_t GetDebugLayerCount() const override;
    bool DescribeDebugLayer(std::size_t index,
        GC_VideoDebugLayerDescriptor& descriptor) const override;
    bool ReadDebugLayerSurface(std::size_t index,
        GC_VideoDebugLayerSurface& surface) const override;
    std::size_t GetDebugLayerOptionCount(std::size_t index) const override;
    bool DescribeDebugLayerOption(std::size_t layerIndex, std::size_t optionIndex,
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
    std::size_t GetDebugPreviewPartCount() const override;
    bool DescribeDebugPreviewPart(std::size_t index,
        GC_VideoDebugPreviewPartDescriptor& descriptor) const override;
    std::size_t GetDebugPreviewOptionCount() const override;
    bool DescribeDebugPreviewOption(std::size_t index,
        GC_VideoDebugPreviewOptionDescriptor& descriptor) const override;
    bool ReadDebugPreviewSurface(u64 visiblePartMask, u64 optionMask,
        GC_VideoDebugLayerSurface& surface) const override;
    std::size_t GetDebugSpriteSourceCount() const override;
    bool DescribeDebugSpriteSource(std::size_t index,
        GC_VideoDebugSpriteSourceDescriptor& descriptor) const override;
    bool ReadDebugSpriteAtlas(const char* sourceCode, int scanline,
        GC_VideoDebugSpriteAtlasSurface& surface) const override;
    bool DescribeDebugSpriteParameter(const char* sourceCode,
        int& minimum, int& maximum) const override;
    std::size_t GetDebugSpriteSourceFieldCount(const char* sourceCode,
        int scanline) const override;
    bool DescribeDebugSpriteSourceField(const char* sourceCode, int scanline,
        std::size_t fieldIndex,
        GC_VideoDebugGridCellFieldDescriptor& descriptor) const override;
    std::size_t GetDebugSpriteItemFieldCount(const char* sourceCode,
        int scanline, u32 spriteIndex) const override;
    bool DescribeDebugSpriteItemField(const char* sourceCode, int scanline,
        u32 spriteIndex, std::size_t fieldIndex,
        GC_VideoDebugGridCellFieldDescriptor& descriptor) const override;
    std::size_t GetDebugSpriteAttributeTableCount() const override;
    bool DescribeDebugSpriteAttributeTable(std::size_t index,
        GC_VideoDebugSpriteAttributeTableDescriptor& descriptor) const override;
    bool DescribeDebugSpriteAttributeColumn(std::size_t tableIndex,
        std::size_t columnIndex,
        GC_VideoDebugSpriteAttributeColumnDescriptor& descriptor) const override;
    bool DescribeDebugSpriteAttributeRecord(std::size_t tableIndex,
        u32 recordIndex,
        GC_VideoDebugSpriteAttributeRecordDescriptor& descriptor) const override;
    bool ReadDebugSpriteAttributeValue(std::size_t tableIndex,
        u32 recordIndex, u32 columnIndex, u64& value) const override;
    int GetMode() override;
    void Render24bit(u16* srcFrameBuffer, u8* dstFrameBuffer, GC_Color_Format pixelFormat, int size, bool overscan = false);
    void Render16bit(u16* srcFrameBuffer, u8* dstFrameBuffer, GC_Color_Format pixelFormat, int size, bool overscan = false);
    void SetOverscan(Overscan overscan);
    Overscan GetOverscan();
    void SetPredefinedPalette(int palette);
#ifdef SPRITE_EXPANDER
    void SetNoSpriteLimit(bool noSpriteLimit);
#endif
    u64 GetFrameSerial() const override { return m_FrameRenderDiagnostics.frameSerial; }
    u8 GetBufferReg() override;
    u8 GetControlLatchValue() const override;
    u16 GetAddressReg() override;
    u8 GetStatusReg() override;
    int GetRenderLine() const override;
    int GetCycleCounter();
    int GetColumn() override;
    TMS99xxRasterTiming::DotPosition GetRasterDotPosition() const;
    TMS99xxRasterTiming::SignalOutput GetRasterSignalOutput() const;

    // Debug-only visualization of the full 342-dot raster (sync, blanking,
    // color burst, border, active), not the ~256x192 content buffer the
    // normal renderer produces. Built to visually verify
    // TMS99xxRasterTiming::GetHorizontalRegion's boundaries before wiring a
    // real signal-level CRT sink (see ICrtSignalSink.h) on top of it - off
    // by default, costs nothing unless enabled.
    void SetFullRasterDebugEnabled(bool enabled);

    // Second, parallel video output: a real console drives its RGB and its
    // composite connectors at the same time from the same internal picture,
    // and this models that. The normal palette-index -> RGB path is
    // untouched and stays authoritative; attaching a sink adds an output,
    // it never replaces one. Pass nullptr to unplug. With nothing attached
    // the emission code never runs.
    bool AttachCrtSignalSink(ICrtSignalSink* sink) override;
    ICrtSignalSink* GetCrtSignalSink() const;
    // Current CPU-owned VRAM access window, derived by the VDP from its own
    // raster and mode state. The I/O decoder must not duplicate this logic.
    TMS99xxCpuAccessTiming::Window GetCpuVRAMAccessWindow() const;
    TMS99xxVramSequencer::Snapshot GetVRAMSequencerSnapshot() const override;
    FrameRenderDiagnostics GetFrameRenderDiagnostics() const override;
    // Which of the two parallel renderers reaches the screen. The streaming
    // renderer runs in shadow mode by default: it is composed every frame and
    // compared, but the legacy per-line framebuffer stays authoritative until
    // this is flipped. Both buffers are packed GC_RESOLUTION_WIDTH x
    // GC_RESOLUTION_HEIGHT, so they are interchangeable here; the overscan
    // border is synthesised by Render16bit/Render24bit and is never read back
    // from the source buffer.
    void SetPresentStreaming(bool presentStreaming) override;
    bool IsPresentStreaming() const override;
    const u16* GetComposedBuffer(bool streaming) const override;
    // The 171-slot history of the current raster line, in physical slot
    // order, plus the slot the beam is at right now.
    const SlotActivityRecord* GetLineSlotHistory() const override
    {
        return m_LineSlotHistory;
    }
    u16 GetCurrentPhysicalSlot() const override
    {
        return TMS99xxSlotGrid::SlotAtPhase(
            static_cast<std::uint16_t>(m_iCycleCounter));
    }
    bool GetLatch() override;
    u16 GetVRAMPtr();


private:
    void ScanLine(int line);
    void ApplyStreamingMidLineBlanking();
    void ScheduleStreamingSpriteCollision(u64 absoluteTargetLine);
    // R0 bits 5 and 6 are timing signals, not drawing results: a program can
    // poll them to find out where the beam is. Only one renderer may own them,
    // otherwise whichever writes first wins and silences the other. The
    // streaming path writes a line earlier than the legacy one, so it would
    // always win by accident.
    bool IsStreamingStatusAuthoritative() const;
    // Which modes drive the sprite plane. On a TMS9918, M1 disables it: Text,
    // Text with Graphics II banking, and the two fixed bar modes therefore
    // have no sprites. Multicolor with Graphics II banking keeps them on the
    // TMS9918 family; later V99x8 chips decode that combination differently.
    // This is separate from the original-chip sprite-cloning defect caused by
    // collapsing Graphics II pattern banks with R4, whose exact circuit-level
    // behaviour is still undocumented and must not be inferred here.
    bool SpritesEnabledInMode() const;

    // The R7 timeline: one per visible line, seeded when the beam enters the
    // line and cut wherever the CPU writes the register while it is inside.
    void BeginRegister7Line(int line);
    void RecordRegister7Write(u8 value);
    u8 Register7At(int line, int column) const;
    u8 Register7AtLineEnd(int line) const;
    u8 Register7AtRasterDot(int line, int dot, int activeStart) const;
    void FillRegister7Row(int line, u8* row) const;
    int BorderColorForRow(const GC_VideoFrameDescriptor& descriptor, int row,
        bool leading) const;
    // Register value 0 is the transparent/undefined entry, never a displayable
    // colour; every path that draws the backdrop substitutes 1 for it, and the
    // border has to make the same substitution or it shows an unreachable
    // "blacker than black" next to the picture's true black.
    static int BackdropOf(u8 register7)
    {
        const int colour = register7 & 0x0F;
        return colour > 0 ? colour : 1;
    }
    static int ForegroundOf(u8 register7)
    {
        const int colour = (register7 >> 4) & 0x0F;
        return colour > 0 ? colour : BackdropOf(register7);
    }
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    void RenderBackground(int line);
    void RenderSprites(int line);
#endif
    void InitPalettes();
    void SetAddressCounter(u16 value, DebugEventSource source);
    void SetControlLatch(bool firstByte, DebugEventSource source);
    void SetControlLatchValue(u8 value, DebugEventSource source);
    void SetReadBuffer(u8 value, DebugEventSource source);
    void QueueVRAMTransfer(uint64_t tstates,
        const TMS99xxVramSequencer::Request& request);
    static u32 PackCpuPortRequest(
        const TMS99xxVramSequencer::Request& request);
    // Raster position and CPU-port spacing at this instant, for the trace.
    DebugEventRasterContext BuildPortAccessContext(uint64_t tstates) const;
    void CompleteVRAMTransfer(const TMS99xxVramSequencer::Completion& completion);
    void IssueTimedVideoFetch(const TMS99xxVramSequencer::Position& position,
        const TMS99xxVramSlotSchedule::Slot& slot);
    void IssueBackgroundFetch(const TMS99xxVramSequencer::Position& position,
        const TMS99xxVramSlotSchedule::Slot& slot);
    void IssueSpriteFetch(const TMS99xxVramSequencer::Position& position,
        const TMS99xxVramSlotSchedule::Slot& slot);
    void CompleteSpriteFetch(const TMS99xxVramSequencer::Completion& completion);
    SpriteLineFetchState* FindSpriteLineFetchState(u64 absoluteTargetLine);
    SpriteLineFetchState& BeginSpriteLineFetchState(u64 absoluteTargetLine);
#ifdef SPRITE_EXPANDER
    SpriteExpanderLineState* FindSpriteExpanderLineState(u64 absoluteTargetLine);
    SpriteExpanderLineState& BeginSpriteExpanderLineState(u64 absoluteTargetLine);
    void ObserveExpandedSprite(u64 absoluteTargetLine, u8 satIndex);
    void LatchExpandedSprites(u64 absoluteTargetLine);
    void RenderExpandedSpriteLine(int line, u64 absoluteLine);
#endif
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    void CaptureLegacyBackgroundLine(int line);
    void CaptureLegacyComposedLine(int line);
#endif
    void RenderStreamingBackgroundLine(int line, bool allowPartial = false);
    void RenderStreamingSpriteLine(int line, u64 absoluteLine);
    void FinalizeFrameRenderDiagnostics();
    void SetStatus(u8 value, DebugEventSource source);
    void SetVideoMode(int value, DebugEventSource source);
    void SetRegisterValue(u8 reg, u8 value);
    void BuildNameTableDebugSurface() const;
    void BuildPatternTableDebugSurface() const;
    void BuildSpritePatternDebugSurface(u64 optionMask) const;
    void BuildDebugPreviewSurface(u64 visiblePartMask, u64 optionMask) const;
    TMS99xxCpuAccessTiming::State GetCpuVRAMAccessState() const;
    TMS99xxVramSequencer::Schedule GetVideoVRAMSchedule() const;

private:
    Memory* m_pMemory;
    CpuInterruptLines* m_interruptLines;
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    u8* m_pInfoBuffer;
    u16* m_pFrameBuffer;
#endif
    u8* m_pVdpVRAM;
    // Monotonic source revision for derived debug surfaces. Enhanced VDPs
    // use it to cache mode-specific viewers without scanning or hashing VRAM
    // every UI frame.
    u64 m_DebugVRAMRevision;
    mutable u8* m_pNameTableDebugRGBA;
    mutable u8* m_pPatternTableDebugRGBA;
    mutable u8* m_pSpritePatternDebugRGBA;
    mutable u8* m_pSpriteDebugAtlasRGBA;
    mutable u8* m_pDebugPreviewRGBA;
    mutable GC_VideoDebugSpriteAtlasItemDescriptor m_SpriteDebugAtlasItems[32];
    mutable u64 m_SpriteDebugAtlasGeneration;
    mutable u64 m_NameTableDebugGeneration;
    mutable u64 m_PatternTableDebugGeneration;
    mutable u64 m_SpritePatternDebugGeneration;
    mutable u64 m_SpritePatternDebugBuiltOptionMask;
    mutable u64 m_DebugPreviewSourceGeneration;
    mutable u64 m_DebugPreviewBuiltSourceGeneration;
    mutable u64 m_DebugPreviewSurfaceGeneration;
    mutable u64 m_DebugPreviewBuiltPartMask;
    mutable u64 m_DebugPreviewBuiltOptionMask;
    // The snapshot is rebuilt only when its source state changed. Its
    // generation lets the desktop renderer skip redundant GPU uploads.
    mutable bool m_NameTableDebugDirty;
    mutable bool m_PatternTableDebugDirty;
    mutable bool m_SpritePatternDebugDirty;
    bool m_bFirstByteInSequence;
    u8 m_VdpRegister[8];
    // $BF has its own first-byte latch.  Do not alias it with m_VdpBuffer:
    // $BE reads must continue returning the previous VRAM prefetch while a
    // control word is only half-written.
    u8 m_ControlLatchValue;
    u8 m_VdpBuffer;
    u16 m_VdpAddress;
    TMS99xxVramSequencer m_VramSequencer;
    int m_iCycleCounter;
    u8 m_VdpStatus;
    int m_iLinesPerFrame;
    bool m_bPAL;
    int m_iMode;
    int m_iRenderLine;
    Overscan m_Overscan;
    u64 m_FrameDescriptorRevision;

    uint64_t m_iTstates;

    struct LineEvents 
    {
        bool vint;
        bool render;
        bool display;
        bool collision;
    };

    LineEvents m_LineEvents;

    enum Timing
    {
        TIMING_VINT = 0,
        TIMING_RENDER = 1,
        TIMING_DISPLAY = 2
    };

    int m_Timing[3];
    bool m_bDisplayEnabled;
    bool m_bSpriteOvrRequest;
#ifdef SPRITE_EXPANDER
    bool m_bNoSpriteLimit;
#endif
    FrameRenderDiagnostics m_FrameRenderDiagnostics;
    u32 m_VisibleLineCpuVramWrites;
    u32 m_VisibleLineVdpRegisterWrites;
    u32 m_CompletedBackgroundFetches;
    u32 m_CompletedSpriteFetches;
    u32 m_CompletedSpriteSelectionScans;
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    u16* m_pLegacyBackground;
#endif
    u16* m_pStreamingBackground;
    // Debug-only full-raster RGB888 buffer; see SetFullRasterDebugEnabled.
    static constexpr int kFullRasterDebugMaxLines = GC_LINES_PER_FRAME_PAL;
    bool m_bFullRasterDebugEnabled = false;
    u8* m_pFullRasterDebugRGB = nullptr;
    // Canonical presentation surface returned through IVideoDevice. It is
    // CPU memory allocated once at device capacity. The frontend owns any GPU
    // texture and uploads this surface only when it chooses to present it.
    u8* m_pPresentedFrame = nullptr;
    u64 m_PresentationSourceRevision = 1;
    u64 m_PresentedSourceRevision = 0;
    u64 m_PresentedDescriptorRevision = 0;
    u64 m_PresentedPaletteRevision = 0;
    int m_PresentedPixelFormat = -1;
    void RenderFullRasterDebugLine(int line);

    // See AttachCrtSignalSink. Null unless something is plugged in.
    ICrtSignalSink* m_pCrtSignalSink = nullptr;
    // Scratch for one line's worth of border+active palette indices handed
    // to ICrtSignalSink::OutputData, so emission allocates nothing.
    u8 m_CrtSignalLineIndices[TMS99xxRasterTiming::DotsPerLine] = {};
    void EmitCrtSignalLine(int line);
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    u16* m_pLegacyComposed;
#endif
    u16* m_pStreamingComposed;
    u8* m_pStreamingSpriteInfo;
    // Presentation source. Not part of the save state and not touched by
    // Reset(): it is a debugging preference, not emulated machine state.
    bool m_bPresentStreaming;
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    bool m_LegacyBackgroundLineCaptured[GC_RESOLUTION_HEIGHT];
#endif
    bool m_StreamingBackgroundLineReady[GC_RESOLUTION_HEIGHT];
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    bool m_LegacyComposedLineCaptured[GC_RESOLUTION_HEIGHT];
#endif
    bool m_StreamingSpriteLineReady[GC_RESOLUTION_HEIGHT];
    // Dot at which R1 bit 6 was cleared on each visible line, or
    // GC_RESOLUTION_WIDTH when the line was never cut. Blanking follows the
    // beam, so everything from this dot onward is backdrop even though the
    // fetches for the line already happened.
    u16 m_StreamingBlankFromColumn[GC_RESOLUTION_HEIGHT];
    // R7 is an output-stage register, not a fetch input. The backdrop and the
    // text ink it names leave the chip dot by dot, so rewriting it part way
    // along a line cuts the picture and the border right there, not at the
    // next line - which is how a split screen moves the border colour half way
    // down the frame. Reading R7 once when the frame is presented, as the
    // border used to, collapses all of that into whatever value happened to be
    // in the register at the end. Same shape as m_StreamingBlankFromColumn:
    // the value the line began with, plus where along the line it changed.
    static const int kRegister7CutsPerLine = 4;
    struct Register7Timeline
    {
        u8 start = 0;
        u8 count = 0;
        u16 column[kRegister7CutsPerLine] = {0, 0, 0, 0};
        u8 value[kRegister7CutsPerLine] = {0, 0, 0, 0};
    };
    Register7Timeline m_Register7Line[GC_RESOLUTION_HEIGHT];
    SlotActivityRecord m_LineSlotHistory[TMS99xxSlotGrid::SlotsPerLine];
    // Dot on the current line at which two sprite pixels overlap, in master
    // clocks so it can be compared against m_iCycleCounter directly. Computed
    // as soon as the sprite patterns for the line are latched, which happens
    // early in the line, and raised when the beam reaches it.
    // The status read races the F flag. The CPU samples the data bus slightly
    // before /RD goes inactive, and the VDP clears F on /RD going inactive, so
    // a read whose critical window contains the moment F is raised returns 0
    // and then wipes the flag: the program misses that whole frame. One
    // T-state of Z80 is three VDP master clocks.
    static constexpr int StatusReadRaceWindowClocks = 3;
    bool m_SuppressVBlankFlag;
    u32 m_SuppressedVBlankFlags;
    int m_StreamingCollisionPhase;
    int m_StreamingCollisionLine;
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    int m_LegacyOverflowLine;
    int m_LegacyOverflowSprite;
    u32 m_LegacyCollisions;
#endif
    int m_StreamingOverflowLine;
    int m_StreamingOverflowSprite;
    u32 m_StreamingCollisions;
    // Graphics modes fetch 32 cells per scanline, while Text mode fetches
    // 40. Keep one fixed-size array so the hot path stays allocation-free.
    BackgroundFetchLatch m_BackgroundFetchLatches[40];
    int m_BackgroundFetchLine;
    u64 m_BackgroundFetchAbsoluteLine;
    TMS99xxVramSlotSchedule::Activity m_PendingBackgroundFetchActivity;
    u8 m_PendingBackgroundFetchIndex;
    bool m_HasPendingBackgroundFetch;
    SpriteLineFetchState m_SpriteLineFetchStates[2];
#ifdef SPRITE_EXPANDER
    SpriteExpanderLineState m_SpriteExpanderLineStates[2];
#endif
    PendingSpriteFetch m_PendingSpriteFetch;
    bool m_HasPendingSpriteFetch;
    RenderedSpriteDebugFrame m_RenderedSpriteDebugWorking;
    RenderedSpriteDebugFrame m_LastRenderedSpriteDebugFrame;
    SpriteSelectionFrameHistory m_SpriteSelectionHistoryWorking;
    SpriteSelectionFrameHistory m_LastSpriteSelectionHistory;

    // End-of-transfer timestamp of the previous CPU port access that reached
    // VRAM, so the trace can state the spacing the VDP actually saw. Counting
    // from instruction start instead would understate it by the difference in
    // length between the two I/O instructions.
    u64 m_LastCpuPortAccessTStates;
    bool m_HasLastCpuPortAccess;

    u16 m_palette_565_rgb[16];
    u16 m_palette_555_rgb[16];
    u16 m_palette_565_bgr[16];
    u16 m_palette_555_bgr[16];

    const u8* m_pCurrentPalette;
    s64 m_ColorProfile;
    u64 m_PaletteRevision;
    SystemVideoDevice m_device;
};

class TMS9918 final : public TMS99xx
{
public:
    TMS9918(Memory* memory, CpuInterruptLines* interruptLines)
        : TMS99xx(memory, interruptLines, SystemVideoDevice::TMS9918) {}
};

class TMS9928 final : public TMS99xx
{
public:
    TMS9928(Memory* memory, CpuInterruptLines* interruptLines)
        : TMS99xx(memory, interruptLines, SystemVideoDevice::TMS9928) {}
};

class TMS9929 final : public TMS99xx
{
public:
    TMS9929(Memory* memory, CpuInterruptLines* interruptLines)
        : TMS99xx(memory, interruptLines, SystemVideoDevice::TMS9929) {}
};


inline int TMS99xx::GetColumn()
{
    return TMS99xxRasterTiming::GetDot(
        static_cast<std::uint16_t>(m_iCycleCounter));
}

inline u8* TMS99xx::GetVRAM()
{
    return m_pVdpVRAM;
}

inline const u8* TMS99xx::GetVRAM() const
{
    return m_pVdpVRAM;
}

inline u8* TMS99xx::GetRegisters()
{
    return m_VdpRegister;
}

inline const u8* TMS99xx::GetRegisters() const
{
    return m_VdpRegister;
}

inline int TMS99xx::GetMode()
{
    return m_iMode;
}

inline u16* TMS99xx::GetFrameBuffer()
{
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    return m_bPresentStreaming ? m_pStreamingComposed : m_pFrameBuffer;
#else
    return m_pStreamingComposed;
#endif
}

inline bool TMS99xx::IsStreamingStatusAuthoritative() const
{
    // Deliberately the same switch as the presented renderer. Showing the
    // streaming picture while the legacy path still owns R0 bits 5 and 6 would
    // be an incoherent machine, and two owners cannot be split by taste: the
    // first writer wins and silences the other. With no legacy renderer
    // compiled in, streaming is the only source there is.
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    return m_bPresentStreaming;
#else
    return true;
#endif
}

inline void TMS99xx::SetPresentStreaming(bool presentStreaming)
{
    if (m_bPresentStreaming == presentStreaming)
        return;
    m_bPresentStreaming = presentStreaming;
    ++m_PresentationSourceRevision;
}

inline bool TMS99xx::IsPresentStreaming() const
{
    return m_bPresentStreaming;
}

inline const u16* TMS99xx::GetComposedBuffer(bool streaming) const
{
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    return streaming ? m_pStreamingComposed : m_pLegacyComposed;
#else
    (void)streaming;
    return m_pStreamingComposed;
#endif
}

#include "TMS99xxPalette.h"

#endif /* TMS99XX_H */
