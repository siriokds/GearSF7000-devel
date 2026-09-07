/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026  Saverio Russo

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 *
 */

#ifndef VIDEO_H
#define	VIDEO_H

#include "definitions.h"
#include "DebugEvents.h"
#include "VideoFrame.h"
#include "TMS9918CpuAccessTiming.h"
#include "TMS9918RasterTiming.h"
#include "TMS9918VramSequencer.h"
#include "ICrtSignalSink.h"

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

class Video
{
public:
    enum VideoTimingEvent : u8
    {
        VideoTimingDisplay = 1 << 0,
        VideoTimingRender  = 1 << 1,
        VideoTimingVBlank  = 1 << 2
    };

    enum DebugStateTarget : u16
    {
        DebugAddressCounter = 0,
        DebugControlLatch = 1,
        DebugReadBuffer = 2,
        DebugStatus = 3,
        DebugIRQ = 4,
        DebugVideoMode = 5,
        DebugCpuPortLatchReplacement = 6,
        // First byte written to $BF.  This is deliberately distinct from
        // the VRAM read buffer returned by $BE.
        DebugControlLatchValue = 7
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
    struct FrameRenderDiagnostics
    {
        u64 frameSerial = 0;
        u64 legacyFrameHash = 0;
        u32 visibleLineCpuVramWrites = 0;
        u32 visibleLineVdpRegisterWrites = 0;
        u32 completedBackgroundFetches = 0;
        u32 completedSpriteFetches = 0;
        u32 completedSpriteSelectionScans = 0;
        bool streamingBackgroundComparable = false;
        u16 streamingBackgroundComparedLines = 0;
        u32 streamingBackgroundMismatchedPixels = 0;
        u64 streamingBackgroundHash = 0;
        bool streamingSpriteComparable = false;
        u16 streamingSpriteComparedLines = 0;
        u32 streamingSpriteMismatchedPixels = 0;
        u64 streamingSpriteHash = 0;
        // First visible line the beam blanked part way through, and the dot it
        // was cut at. -1 when no line was cut this frame.
        int streamingBlankFirstLine = -1;
        int streamingBlankFirstColumn = -1;
        // Where each renderer would raise the sprite status bits this frame.
        // Collected side by side so the streaming path can be shown equivalent
        // before it becomes the only source of R0 bits 5 and 6.
        int legacyOverflowLine = -1;
        int legacyOverflowSprite = -1;
        u32 legacyCollisions = 0;
        int streamingOverflowLine = -1;
        int streamingOverflowSprite = -1;
        int streamingCollisionLine = -1;
        int streamingCollisionDot = -1;
        u32 streamingCollisions = 0;
        bool streamingStatusAuthoritative = false;
        u32 suppressedVBlankFlags = 0;
    };

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
    struct SlotActivityRecord
    {
        TMS9918VramSlotSchedule::Activity activity =
            TMS9918VramSlotSchedule::Activity::Unknown;
        u8 index = 0;
        bool cpuTransacted = false;
        bool cpuWrite = false;
        u16 cpuAddress = 0;
        u8 cpuValue = 0;
    };

    struct PendingSpriteFetch
    {
        TMS9918VramSlotSchedule::Activity activity =
            TMS9918VramSlotSchedule::Activity::Unknown;
        u64 absoluteTargetLine = 0;
        u8 index = 0;
        u8 byte = 0;
        bool candidateScan = false;
    };

public:
    // One sprite as the debugger wants it: what the attribute table says, plus
    // what the chip actually did with it on a given line.
    struct SpriteDebugEntry
    {
        int index = 0;
        u8 rawY = 0;        // the byte in the SAT
        u8 x = 0;
        u8 name = 0;
        u8 colour = 0;      // bit 7 is early clock, low nibble the colour
        // The TMS9918 draws a sprite one line BELOW its Y byte, so this is
        // rawY + 1. Reporting only the raw value makes every comparison
        // against the screen off by one, which is the single easiest thing to
        // get wrong about this table.
        int visibleY = 0;
        // x - 32 when early clock is set. Negative means partly off the left
        // edge, which is what early clock is for.
        int effectiveX = 0;
        bool earlyClock = false;
        // True when this sprite was one of the four the chip selected for the
        // line the snapshot describes.
        bool selectedOnLine = false;
        // True for the fifth sprite on that line: the one that was dropped
        // and whose number the status register reports.
        bool overflowCause = false;
    };

    struct SpriteDebugSnapshot
    {
        // The line the per-line fields describe.
        int line = 0;
        // False when the selection state for that line is no longer held.
        // Only two lines are ever in flight (m_SpriteLineFetchStates), so the
        // static fields below are always right but selectedOnLine and
        // overflowCause mean nothing unless this is true.
        bool lineStateValid = false;
        // False in Text and the undocumented modes that have no sprite plane:
        // the table still reads, but nothing in it is drawn.
        bool spritesEnabled = false;
        int height = 8;             // before magnification
        bool magnified = false;
        // Where the $D0 that ends the list was found, or -1 if there is none
        // in all 32 entries. Entries past it are not part of the list at all.
        int terminatorIndex = -1;
        int count = 0;              // how many of entries[] are filled
        SpriteDebugEntry entries[32];
        // From the LAST COMPLETE FRAME, not the one being drawn. The live
        // counters reset at the start of each frame, so reading them while
        // paused in the VBlank - which is where a debugger usually is -
        // reports nothing at all. These are frame-level: the hardware records
        // that a collision happened, not which pair caused it, so there is
        // deliberately no per-sprite collision flag above.
        int overflowLine = -1;
        int overflowSprite = -1;
        int collisionLine = -1;
        int collisionDot = -1;
        unsigned long long collisions = 0;
    };

    // What the timed sprite fetcher actually acquired for the last complete
    // frame. Unlike a live SAT decode this honours the four-sprites-per-line
    // limit and preserves rows fetched before software rewrote SAT/SPRPAT.
    struct RenderedSpriteDebugEntry
    {
        bool seen = false;
        u8 rawY = 0;
        u8 x = 0;
        u8 name = 0;
        u8 colour = 0;
        u16 fetchedRows = 0;
        u8 pixels[16 * 16] = {};
    };

    struct RenderedSpriteScanlineSlot
    {
        int satIndex = -1;
        u8 y = 0;
        u8 x = 0;
        u8 name = 0;
        u8 colour = 0;
        u8 pattern[2] = {0, 0};
        // y, x, name, colour, pattern 0 and pattern 1 respectively.
        u8 fetchedMask = 0;
        // Pattern bits reaching the active 256-pixel area, including colour
        // zero. visiblePixels additionally requires a non-zero colour and
        // winning the SAT-priority test against earlier sprites.
        u16 patternPixels = 0;
        u16 visiblePixels = 0;
    };

    struct RenderedSpriteScanlineDebug
    {
        bool valid = false;
        bool spritesEnabled = false;
        u8 selectedCount = 0;
        bool overflowRecorded = false;
        int overflowSprite = -1;
        RenderedSpriteScanlineSlot selected[4];
    };

    struct RenderedSpriteDebugFrame
    {
        bool valid = false;
        bool spritesEnabled = false;
        bool large = false;
        bool magnified = false;
        u64 frameSerial = 0;
        int rasterLines = 0;
        RenderedSpriteDebugEntry entries[32];
        RenderedSpriteScanlineDebug scanlines[GC_LINES_PER_FRAME_PAL];
    };

    struct SpritePipelineLatchDebug
    {
        int satIndex = -1;
        u8 rasterX = 0xFF;
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

    struct SpritePipelineLineDebug
    {
        bool valid = false;
        u64 absoluteTargetLine = 0;
        int targetLine = 0;
        u8 selectedCount = 0;
        bool selectionTerminated = false;
        bool overflowRecorded = false;
        int overflowSprite = -1;
        u8 overflowRasterX = 0xFF;
        u8 overflowSat[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        bool overflowSatValid = false;
        int collisionX = -1;
        SpritePipelineLatchDebug selected[4];
    };

    // Cycle-current view of the two scanlines that can be in flight in the
    // TMS9918 sprite pipeline. This is deliberately separate from both a live
    // SAT decode and the summary of the last completed frame.
    struct SpritePipelineDebugSnapshot
    {
        int rasterLine = 0;
        u64 absoluteRasterLine = 0;
        u16 phase = 0;
        u16 dot = 0;
        u16 slot = 0;
        TMS9918VramSlotSchedule::Activity activity =
            TMS9918VramSlotSchedule::Activity::Unknown;
        u8 activityIndex = 0;
        u8 activityByte = 0;
        bool activeTargetValid = false;
        u64 activeAbsoluteTargetLine = 0;
        int activeTargetLine = 0;
        bool requestPending = false;
        SpritePipelineLineDebug lines[2];
    };

    struct SpriteSelectionHistoryRow
    {
        // Five records: the accepted lanes 0..3 and the rejected fifth
        // candidate. rasterX is the 0..170 VRAM raster column at which its Y
        // was scanned. sat is Y, X, name, colour. The fifth SAT tuple is a
        // diagnostic snapshot; only its Y was read by the real chip.
        u8 rasterX[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        u8 sat[5][4] = {
            {0xFF, 0xFF, 0xFF, 0xFF}, {0xFF, 0xFF, 0xFF, 0xFF},
            {0xFF, 0xFF, 0xFF, 0xFF}, {0xFF, 0xFF, 0xFF, 0xFF},
            {0xFF, 0xFF, 0xFF, 0xFF}
        };
        // Actual pattern row bytes fetched for the four accepted lanes.
        u8 pattern[4][2] = {
            {0xFF, 0xFF}, {0xFF, 0xFF},
            {0xFF, 0xFF}, {0xFF, 0xFF}
        };
        // bit 0 valid, 1 sprites enabled, 2 16x16, 3 magnified,
        // 4 overflow, 5 collision.
        u8 flags = 0;
        u8 collisionX = 0xFF;
    };

    struct SpriteSelectionFrameHistory
    {
        bool valid = false;
        u64 frameSerial = 0;
        int rasterLines = 0;
        SpriteSelectionHistoryRow rows[GC_LINES_PER_FRAME_PAL];
    };

    // Reads the sprite attribute table and crosses it with the chip's own
    // selection state. Pass -1 for the line the beam is on.
    SpriteDebugSnapshot GetSpriteDebugSnapshot(int line);
    SpritePipelineDebugSnapshot GetSpritePipelineDebugSnapshot() const;
    const SpriteSelectionFrameHistory& GetSpriteSelectionFrameHistory() const
    {
        return m_LastSpriteSelectionHistory;
    }
    const RenderedSpriteDebugFrame& GetLastRenderedSpriteDebugFrame() const
    {
        return m_LastRenderedSpriteDebugFrame;
    }

    // The 16 RGB triplets currently in use, so a debug view can colour a
    // sprite the way the screen would. Follows the palette the user picked,
    // rather than baking one in.
    const u8* GetCurrentPalette() const { return m_pCurrentPalette; }

    Video(Memory* pMemory, CpuInterruptLines* interruptLines);
    ~Video();
    void Init();
    void Reset(bool bPAL);
    bool TickMasterClocks(unsigned int masterClocks);
    // Compatibility entry point for tests/tools still expressed in the
    // historical NTSC-equivalent Z80 T-state unit.
    bool Tick(unsigned int clockCycles);
    u8 GetDataPort(uint64_t tstates);
    u8 GetStatusFlags();
    void WriteData(uint64_t tstates, u8 data);
    void WriteControl(uint64_t tstates, u8 control);
    void WriteVRAM(u16 address, u8 value);
    u8 ReadVRAM(u16 address);
    void SaveState(std::ostream& stream);
    void LoadState(std::istream& stream);
    u8* GetVRAM();
    u8* GetRegisters();
    u16* GetFrameBuffer();
    GC_VideoFrameDescriptor GetFrameDescriptor() const;
    int GetMode();
    void Render24bit(u16* srcFrameBuffer, u8* dstFrameBuffer, GC_Color_Format pixelFormat, int size, bool overscan = false);
    void Render16bit(u16* srcFrameBuffer, u8* dstFrameBuffer, GC_Color_Format pixelFormat, int size, bool overscan = false);
    void SetOverscan(Overscan overscan);
    Overscan GetOverscan();
    void SetCustomPalette(GC_Color* palette);
    void SetPredefinedPalette(int palette);
#ifdef SPRITE_EXPANDER
    void SetNoSpriteLimit(bool noSpriteLimit);
#endif
    bool IsPAL();
    u8 GetBufferReg();
    u8 GetControlLatchValue() const;
    u16 GetAddressReg();
    u8 GetStatusReg();
    int GetRenderLine();
    int GetCycleCounter();
    int GetColumn();
    TMS9918RasterTiming::DotPosition GetRasterDotPosition() const;
    TMS9918RasterTiming::SignalOutput GetRasterSignalOutput() const;

    // Debug-only visualization of the full 342-dot raster (sync, blanking,
    // color burst, border, active), not the ~256x192 content buffer the
    // normal renderer produces. Built to visually verify
    // TMS9918RasterTiming::GetHorizontalRegion's boundaries before wiring a
    // real signal-level CRT sink (see ICrtSignalSink.h) on top of it - off
    // by default, costs nothing unless enabled.
    void SetFullRasterDebugEnabled(bool enabled);
    bool IsFullRasterDebugEnabled() const;
    // width/height receive the buffer's actual dimensions (342 x current
    // m_iLinesPerFrame); the returned pointer is packed RGB888, row-major.
    const u8* GetFullRasterDebugBuffer(int& width, int& height) const;

    // Second, parallel video output: a real console drives its RGB and its
    // composite connectors at the same time from the same internal picture,
    // and this models that. The normal palette-index -> RGB path is
    // untouched and stays authoritative; attaching a sink adds an output,
    // it never replaces one. Pass nullptr to unplug. With nothing attached
    // the emission code never runs.
    void AttachCrtSignalSink(ICrtSignalSink* sink);
    ICrtSignalSink* GetCrtSignalSink() const;
    // Current CPU-owned VRAM access window, derived by the VDP from its own
    // raster and mode state. The I/O decoder must not duplicate this logic.
    TMS9918CpuAccessTiming::Window GetCpuVRAMAccessWindow() const;
    TMS9918VramSequencer::Snapshot GetVRAMSequencerSnapshot() const;
    FrameRenderDiagnostics GetFrameRenderDiagnostics() const;
    // Which of the two parallel renderers reaches the screen. The streaming
    // renderer runs in shadow mode by default: it is composed every frame and
    // compared, but the legacy per-line framebuffer stays authoritative until
    // this is flipped. Both buffers are packed GC_RESOLUTION_WIDTH x
    // GC_RESOLUTION_HEIGHT, so they are interchangeable here; the overscan
    // border is synthesised by Render16bit/Render24bit and is never read back
    // from the source buffer.
    void SetPresentStreaming(bool presentStreaming);
    bool IsPresentStreaming() const;
    const u16* GetComposedBuffer(bool streaming) const;
    // The 171-slot history of the current raster line, in physical slot
    // order, plus the slot the beam is at right now.
    const SlotActivityRecord* GetLineSlotHistory() const
    {
        return m_LineSlotHistory;
    }
    u16 GetCurrentPhysicalSlot() const
    {
        return TMS9918SlotGrid::SlotAtPhase(
            static_cast<std::uint16_t>(m_iCycleCounter));
    }
    bool GetLatch();
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
    // Which modes drive the sprite plane. openMSX gates it on the display
    // mode and notes the bogus modes were verified on real silicon to show no
    // sprites at all; Text and its undocumented Graphics II banked variant
    // have none either. Multicolor with M3 keeps them, but only on a TMS9918:
    // the V99x8 drops them there.
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
        const TMS9918VramSequencer::Request& request);
    static u32 PackCpuPortRequest(
        const TMS9918VramSequencer::Request& request);
    // Raster position and CPU-port spacing at this instant, for the trace.
    DebugEventRasterContext BuildPortAccessContext(uint64_t tstates) const;
    void CompleteVRAMTransfer(const TMS9918VramSequencer::Completion& completion);
    void IssueTimedVideoFetch(const TMS9918VramSequencer::Position& position,
        const TMS9918VramSlotSchedule::Slot& slot);
    void IssueBackgroundFetch(const TMS9918VramSequencer::Position& position,
        const TMS9918VramSlotSchedule::Slot& slot);
    void IssueSpriteFetch(const TMS9918VramSequencer::Position& position,
        const TMS9918VramSlotSchedule::Slot& slot);
    void CompleteSpriteFetch(const TMS9918VramSequencer::Completion& completion);
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
    TMS9918CpuAccessTiming::State GetCpuVRAMAccessState() const;
    TMS9918VramSequencer::Schedule GetVideoVRAMSchedule() const;

private:
    Memory* m_pMemory;
    CpuInterruptLines* m_interruptLines;
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    u8* m_pInfoBuffer;
    u16* m_pFrameBuffer;
#endif
    u8* m_pVdpVRAM;
    bool m_bFirstByteInSequence;
    u8 m_VdpRegister[8];
    // $BF has its own first-byte latch.  Do not alias it with m_VdpBuffer:
    // $BE reads must continue returning the previous VRAM prefetch while a
    // control word is only half-written.
    u8 m_ControlLatchValue;
    u8 m_VdpBuffer;
    u16 m_VdpAddress;
    TMS9918VramSequencer m_VramSequencer;
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
    void RenderFullRasterDebugLine(int line);

    // See AttachCrtSignalSink. Null unless something is plugged in.
    ICrtSignalSink* m_pCrtSignalSink = nullptr;
    // Scratch for one line's worth of border+active palette indices handed
    // to ICrtSignalSink::OutputData, so emission allocates nothing.
    u8 m_CrtSignalLineIndices[TMS9918RasterTiming::DotsPerLine] = {};
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
    SlotActivityRecord m_LineSlotHistory[TMS9918SlotGrid::SlotsPerLine];
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
    TMS9918VramSlotSchedule::Activity m_PendingBackgroundFetchActivity;
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

    u8 m_CustomPalette[48];
    u8* m_pCurrentPalette;
};


inline int Video::GetColumn()
{
    return TMS9918RasterTiming::GetDot(
        static_cast<std::uint16_t>(m_iCycleCounter));
}

inline u8* Video::GetVRAM()
{
    return m_pVdpVRAM;
}

inline u8* Video::GetRegisters()
{
    return m_VdpRegister;
}

inline int Video::GetMode()
{
    return m_iMode;
}

inline u16* Video::GetFrameBuffer()
{
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    return m_bPresentStreaming ? m_pStreamingComposed : m_pFrameBuffer;
#else
    return m_pStreamingComposed;
#endif
}

inline bool Video::IsStreamingStatusAuthoritative() const
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

inline void Video::SetPresentStreaming(bool presentStreaming)
{
    m_bPresentStreaming = presentStreaming;
}

inline bool Video::IsPresentStreaming() const
{
    return m_bPresentStreaming;
}

inline const u16* Video::GetComposedBuffer(bool streaming) const
{
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    return streaming ? m_pStreamingComposed : m_pLegacyComposed;
#else
    (void)streaming;
    return m_pStreamingComposed;
#endif
}

const u8 kPalette_888_coleco[48] = {0,0,0, 0,0,0, 33,200,66, 94,220,120, 84,85,237, 125,118,252, 212,82,77, 66,235,245, 252,85,84, 255,121,120, 212,193,84, 230,206,128, 33,176,59, 201,91,186, 204,204,204, 255,255,255};
//const u8 kPalette_888_tms9918[48] = {0,0,0, 0,8,0, 0,241,1, 50,251,65, 67,76,255, 112,110,255, 238,75,28, 9,255,255, 255,78,31, 255,112,65, 211,213,0, 228,221,52, 0,209,0, 219,79,211, 193,212,190, 244,255,241};

// Analog 888 version, CRT simulation
//const u8 kPalette_888_tms9918_analog[48] = { 0, 0, 4, 1, 0, 4, 58, 187, 69, 112, 211, 120, 84, 89, 212, 123, 123, 230, 179, 99, 76, 97, 223, 230, 212, 106, 85, 248, 142, 120, 199, 199, 92, 217, 212, 132, 54, 165, 61, 176, 107, 172, 199, 208, 197, 250, 255, 248 };

const u8 kPalette_888_tms9918[48] = {
    0, 0, 0,         // 0     Backdrop
    0, 0, 0,         // 1     Black
    51, 187, 68,     // 2     Medium green
    153, 221, 153,   // 3     Light green
    85, 85, 221,     // 4     Dark blue
    136, 136, 238,   // 5     Light blue
    187, 68, 51,     // 6     Dark red
    85, 238, 238,    // 7     Cyan
    238, 102, 68,    // 8     Medium red
    255, 170, 119,   // 9     Light red
    187, 187, 85,    // A     Medium yellow
    221, 221, 136,   // B     Light yellow
    51, 170, 51,     // C     Dark green
    204, 136, 204,   // D     Purple
    238, 238, 238,   // E     Gray
    255, 255, 255    // F     White
};

// Analog R2R DAC-444 to 888 version, CRT simulation
const u8 kPalette_888_tms9918_analog[48] = {
    0, 0, 0,         // 0     Backdrop
    17, 17, 17,         // 1     Black
    51, 187, 68,     // 2     Medium green
    153, 221, 153,   // 3     Light green
    85, 85, 221,     // 4     Dark blue
    136, 136, 238,   // 5     Light blue
    187, 68, 51,     // 6     Dark red
    85, 238, 238,    // 7     Cyan
    238, 102, 68,    // 8     Medium red
    255, 170, 119,   // 9     Light red
    187, 187, 85,    // A     Medium yellow
    221, 221, 136,   // B     Light yellow
    51, 170, 51,     // C     Dark green
    204, 136, 204,   // D     Purple
    238, 238, 238,   // E     Gray
    255, 255, 255    // F     White
};

// Analog R2R DAC-444 version, CRT simulation
const u8 kPalette_444_tms9918_analog[48] = {
    0x0, 0x0, 0x0,   // 0     Backdrop
    0x0, 0x0, 0x1,   // 1     Black
    0x3, 0xC, 0x4,   // 2     Medium green
    0x9, 0xE, 0x9,   // 3     Light green
    0x5, 0x5, 0xE,   // 4     Dark blue
    0x8, 0x8, 0xF,   // 5     Light blue
    0xB, 0x4, 0x3,   // 6     Dark red
    0x5, 0xF, 0xF,   // 7     Cyan
    0xF, 0x6, 0x4,   // 8     Medium red
    0xF, 0xA, 0x7,   // 9     Light red
    0xB, 0xB, 0x5,   // A     Medium yellow
    0xE, 0xE, 0x8,   // B     Light yellow
    0x3, 0xA, 0x3,   // C     Dark green
    0xC, 0x8, 0xC,   // D     Purple
    0xE, 0xE, 0xE,   // E     Gray
    0xF, 0xF, 0xF    // F     White
};


const u8 k2bitTo8bit[4] = {0,85,170,255};
const u8 k2bitTo5bit[4] = {0,10,21,31};
const u8 k2bitTo6bit[4] = {0,21,42,63};
const u8 k4bitTo8bit[16] = {0,17,34,51,68,86,102,119,136,153,170,187,204,221,238,255};
const u8 k4bitTo5bit[16] = {0,2,4,6,8,10,12,14,17,19,21,23,25,27,29,31};
const u8 k4bitTo6bit[16] = {0,4,8,13,17,21,25,29,34,38,42,46,50,55,59,63};

#endif	/* VIDEO_H */
