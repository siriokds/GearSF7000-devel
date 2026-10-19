/*
 * GearSF7000 - TMS99xx-specific debugger extension
 * Copyright (C) 2026 Saverio Russo
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ITMS99XX_DEBUG_PROVIDER_H
#define ITMS99XX_DEBUG_PROVIDER_H

#include "definitions.h"
#include "TMS99xxVramSequencer.h"

// Optional inspection contract for the internal architecture shared by the
// original TMS9918/9928/9929 family. Register-compatible implementations such
// as Pico9918, F18A and V9938 must not implement it unless they really share
// these DRAM slots, fetch pipelines and renderer-validation semantics.
class ITMS99xxDebugProvider
{
public:
    virtual ~ITMS99xxDebugProvider() = default;

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

    struct SlotActivityRecord
    {
        TMS99xxVramSlotSchedule::Activity activity =
            TMS99xxVramSlotSchedule::Activity::Unknown;
        u8 index = 0;
        bool cpuTransacted = false;
        bool cpuWrite = false;
        u16 cpuAddress = 0;
        u8 cpuValue = 0;
    };

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

    struct SpritePipelineDebugSnapshot
    {
        int rasterLine = 0;
        u64 absoluteRasterLine = 0;
        u16 phase = 0;
        u16 dot = 0;
        u16 slot = 0;
        TMS99xxVramSlotSchedule::Activity activity =
            TMS99xxVramSlotSchedule::Activity::Unknown;
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

    virtual FrameRenderDiagnostics GetFrameRenderDiagnostics() const = 0;
    virtual TMS99xxVramSequencer::Snapshot GetVRAMSequencerSnapshot() const = 0;
    virtual const SlotActivityRecord* GetLineSlotHistory() const = 0;
    virtual u16 GetCurrentPhysicalSlot() const = 0;

    virtual SpriteDebugSnapshot GetSpriteDebugSnapshot(int line) = 0;
    virtual SpritePipelineDebugSnapshot GetSpritePipelineDebugSnapshot() const = 0;
    virtual const SpriteSelectionFrameHistory& GetSpriteSelectionFrameHistory() const = 0;
    virtual const RenderedSpriteDebugFrame& GetLastRenderedSpriteDebugFrame() const = 0;
    virtual u8 GetBufferReg() = 0;
    virtual u8 GetControlLatchValue() const = 0;
    virtual u16 GetAddressReg() = 0;
    virtual u8 GetStatusReg() = 0;
    virtual int GetMode() = 0;
    virtual int GetRenderLine() const = 0;
    virtual int GetColumn() = 0;
    virtual bool GetLatch() = 0;

    virtual void SetPresentStreaming(bool presentStreaming) = 0;
    virtual bool IsPresentStreaming() const = 0;
    virtual const u16* GetComposedBuffer(bool streaming) const = 0;
};

#endif // ITMS99XX_DEBUG_PROVIDER_H
