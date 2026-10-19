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

#include <cstring>
#include "TMS99xxDebugEvents.h"
#include "TMS99xx.h"
#include "TMS99xxTableAddress.h"
#include "SaveStateStream.h"

namespace
{
// Bumped whenever the VDP field list changes. A state written by a different
// version is rejected outright rather than being read as the wrong fields.
#ifdef SPRITE_EXPANDER
constexpr std::uint16_t kVideoStateVersion = 3;
#else
constexpr std::uint16_t kVideoStateVersion = 2;
#endif
}
#include "Memory.h"
#include "cpu/CpuInterruptLines.h"

namespace
{
constexpr u32 kTmsVramSizeBytes = 0x4000;
constexpr const char* kTmsMemorySpaceCode = "TMS99XX.VRAM";
constexpr const char* kTmsControlBankCode = "TMS99XX.CONTROL";
constexpr const char* kTmsStatusBankCode = "TMS99XX.STATUS";

constexpr GC_VideoStateValueDescriptor kTmsStateValues[] =
{
    { "TMS99XX.STATE.MODE", "MODE", "Current display mode",
        GC_VideoDebugValueFormat::Hexadecimal, 8 },
    { "TMS99XX.STATE.LATCH_PHASE", "LATCH", "Waiting for first control byte",
        GC_VideoDebugValueFormat::Boolean, 1 },
    { "TMS99XX.STATE.LATCH_VALUE", "LATCHVAL", "First control byte latch",
        GC_VideoDebugValueFormat::Hexadecimal, 8 },
    { "TMS99XX.STATE.READ_BUFFER", "BUFFER", "VRAM read-ahead buffer",
        GC_VideoDebugValueFormat::Hexadecimal, 8 },
    { "TMS99XX.STATE.STATUS", "STATUS", "Internal status latch",
        GC_VideoDebugValueFormat::Hexadecimal, 8 },
    { "TMS99XX.STATE.VRAM_POINTER", "VRAMPTR", "VRAM address counter",
        GC_VideoDebugValueFormat::Address, 14 },
    { "TMS99XX.STATE.RASTER_X", "RASTER X", "Current raster dot",
        GC_VideoDebugValueFormat::Decimal, 9 },
    { "TMS99XX.STATE.RASTER_Y", "RASTER Y", "Current raster line",
        GC_VideoDebugValueFormat::Decimal, 9 },
    { "TMS99XX.STATE.RASTER_AREA", "REGION", "Current raster region",
        GC_VideoDebugValueFormat::Text, 0 },
    { "TMS99XX.STATE.SUBPIXEL_PHASE", "SUBPIXEL",
        "Master-clock phase inside the raster dot",
        GC_VideoDebugValueFormat::Decimal, 1 },
    { "TMS99XX.STATE.LINE_PHASE", "LINE PHASE",
        "Master-clock phase inside the raster line",
        GC_VideoDebugValueFormat::Decimal, 10 }
};

struct TmsRegisterDefinition
{
    const char* code;
    const char* shortName;
    const char* description;
    u32 implementedMask;
    bool hasDerivedAddress;
};

constexpr TmsRegisterDefinition kTmsControlRegisters[] =
{
    { "TMS99XX.CONTROL.R0", "CTRL 0", "Mode control register 0", 0x02, false },
    { "TMS99XX.CONTROL.R1", "CTRL 1", "Mode control register 1", 0xFB, false },
    { "TMS99XX.CONTROL.R2", "NAMTAB", "Pattern Name Table base address", 0x0F, true },
    { "TMS99XX.CONTROL.R3", "COLTAB", "Color Table base address and mask", 0xFF, true },
    { "TMS99XX.CONTROL.R4", "PATGEN", "Pattern Generator Table base address and mask", 0x07, true },
    { "TMS99XX.CONTROL.R5", "SPRATR", "Sprite Attribute Table base address", 0x7F, true },
    { "TMS99XX.CONTROL.R6", "SPRGEN", "Sprite Generator Table base address", 0x07, true },
    { "TMS99XX.CONTROL.R7", "FG/BG", "Text Color and Backdrop Color", 0xFF, false }
};

struct TmsRegisterFieldDefinition
{
    const char* code;
    const char* description;
    u32 registerIndex;
    u32 mask;
    u8 shift;
    u32 accessFlags;
};

constexpr u32 kTmsControlFieldAccess = GC_VIDEO_REGISTER_ACCESS_WRITE |
    GC_VIDEO_REGISTER_ACCESS_OBSERVE | GC_VIDEO_REGISTER_ACCESS_WRITE_SIDE_EFFECT;
constexpr u32 kTmsStatusFieldAccess = GC_VIDEO_REGISTER_ACCESS_READ |
    GC_VIDEO_REGISTER_ACCESS_OBSERVE | GC_VIDEO_REGISTER_ACCESS_READ_SIDE_EFFECT;

constexpr TmsRegisterFieldDefinition kTmsControlFields[] =
{
    { "TMS99XX.MODE.M3", "Mode bit M3", 0, 0x02, 1, kTmsControlFieldAccess },
    { "TMS99XX.SPRITE.MAGNIFY", "Sprite magnification", 1, 0x01, 0, kTmsControlFieldAccess },
    { "TMS99XX.SPRITE.SIZE", "Sprite size", 1, 0x02, 1, kTmsControlFieldAccess },
    { "TMS99XX.MODE.M2", "Mode bit M2", 1, 0x08, 3, kTmsControlFieldAccess },
    { "TMS99XX.MODE.M1", "Mode bit M1", 1, 0x10, 4, kTmsControlFieldAccess },
    { "TMS99XX.INTERRUPT.ENABLE", "Vertical interrupt enable", 1, 0x20, 5, kTmsControlFieldAccess },
    { "TMS99XX.DISPLAY.ENABLE", "Display enable", 1, 0x40, 6, kTmsControlFieldAccess },
    { "TMS99XX.VRAM.SIZE", "VRAM size selection", 1, 0x80, 7, kTmsControlFieldAccess },
    { "TMS99XX.TABLE.NAME.BASE", "Name table base", 2, 0x0F, 0, kTmsControlFieldAccess },
    { "TMS99XX.TABLE.COLOUR.BASE", "Colour table base and mask", 3, 0xFF, 0, kTmsControlFieldAccess },
    { "TMS99XX.TABLE.PATTERN.BASE", "Pattern table base and mask", 4, 0x07, 0, kTmsControlFieldAccess },
    { "TMS99XX.TABLE.SPRITE_ATTRIBUTE.BASE", "Sprite attribute table base", 5, 0x7F, 0, kTmsControlFieldAccess },
    { "TMS99XX.TABLE.SPRITE_PATTERN.BASE", "Sprite pattern table base", 6, 0x07, 0, kTmsControlFieldAccess },
    { "TMS99XX.COLOUR.BACKDROP", "Backdrop colour", 7, 0x0F, 0, kTmsControlFieldAccess },
    { "TMS99XX.COLOUR.FOREGROUND", "Text foreground colour", 7, 0xF0, 4, kTmsControlFieldAccess }
};

constexpr TmsRegisterFieldDefinition kTmsStatusFields[] =
{
    { "TMS99XX.STATUS.FIFTH_SPRITE_INDEX", "Fifth sprite index", 0, 0x1F, 0, kTmsStatusFieldAccess },
    { "TMS99XX.STATUS.SPRITE_COLLISION", "Sprite collision", 0, 0x20, 5, kTmsStatusFieldAccess },
    { "TMS99XX.STATUS.FIFTH_SPRITE", "Fifth sprite flag", 0, 0x40, 6, kTmsStatusFieldAccess },
    { "TMS99XX.STATUS.VBLANK", "Vertical blank interrupt flag", 0, 0x80, 7, kTmsStatusFieldAccess }
};

bool IsDescriptorCode(const char* value, const char* expected)
{
    return value != NULL && std::strcmp(value, expected) == 0;
}

u64 HashLegacyFrame(const u16* pixels, int count)
{
    // FNV-1a over colour values, deliberately independent from host endian.
    u64 hash = 1469598103934665603ull;
    for (int i = 0; i < count; ++i)
    {
        const u16 pixel = pixels[i];
        hash ^= static_cast<u8>(pixel & 0xFF);
        hash *= 1099511628211ull;
        hash ^= static_cast<u8>(pixel >> 8);
        hash *= 1099511628211ull;
    }
    return hash;
}
}

TMS99xx::TMS99xx(Memory* pMemory, CpuInterruptLines* interruptLines,
                 SystemVideoDevice device)
    : m_device(device)
{
    m_pMemory = pMemory;
    m_interruptLines = interruptLines;
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    InitPointer(m_pInfoBuffer);
    InitPointer(m_pFrameBuffer);
#endif
    InitPointer(m_pVdpVRAM);
    InitPointer(m_pNameTableDebugRGBA);
    InitPointer(m_pPatternTableDebugRGBA);
    InitPointer(m_pSpritePatternDebugRGBA);
    InitPointer(m_pSpriteDebugAtlasRGBA);
    InitPointer(m_pDebugPreviewRGBA);
    m_NameTableDebugGeneration = 0;
    m_DebugVRAMRevision = 1;
    m_PatternTableDebugGeneration = 0;
    m_SpritePatternDebugGeneration = 0;
    m_SpritePatternDebugBuiltOptionMask = ~0ull;
    m_DebugPreviewSourceGeneration = 1;
    m_DebugPreviewBuiltSourceGeneration = 0;
    m_DebugPreviewSurfaceGeneration = 0;
    m_DebugPreviewBuiltPartMask = ~0ull;
    m_DebugPreviewBuiltOptionMask = ~0ull;
    m_SpriteDebugAtlasGeneration = 0;
    m_NameTableDebugDirty = true;
    m_PatternTableDebugDirty = true;
    m_SpritePatternDebugDirty = true;
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    InitPointer(m_pLegacyBackground);
#endif
    InitPointer(m_pStreamingBackground);
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    InitPointer(m_pLegacyComposed);
#endif
    InitPointer(m_pStreamingComposed);
    InitPointer(m_pStreamingSpriteInfo);
    InitPointer(m_pPresentedFrame);
    m_bPresentStreaming = false;
    m_bFirstByteInSequence = true;
    for (int i = 0; i < 8; i++)
        m_VdpRegister[i] = 0;
    m_VdpBuffer = 0;
    m_VdpAddress = 0;
    m_iCycleCounter = 0;
    m_VdpStatus = 0;
    m_iLinesPerFrame = 0;
    m_bPAL = device == SystemVideoDevice::TMS9929;
    m_LineEvents.vint = false;
    m_LineEvents.render = false;
    m_LineEvents.display = false;
    m_LineEvents.collision = false;
    m_SuppressVBlankFlag = false;
    m_SuppressedVBlankFlags = 0;
    m_StreamingCollisionPhase = -1;
    m_StreamingCollisionLine = -1;
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    m_LegacyOverflowLine = -1;
    m_LegacyOverflowSprite = -1;
    m_LegacyCollisions = 0;
#endif
    m_StreamingOverflowLine = -1;
    m_StreamingOverflowSprite = -1;
    m_StreamingCollisions = 0;
    m_iRenderLine = 0;
    m_iMode = 0;
    m_iTstates = 0;
    m_Overscan = OverscanDisabled;
    m_FrameDescriptorRevision = 1;
    m_bDisplayEnabled = false;
    m_bSpriteOvrRequest = false;
#ifdef SPRITE_EXPANDER
    m_bNoSpriteLimit = false;
#endif
    m_FrameRenderDiagnostics = {};
    m_VisibleLineCpuVramWrites = 0;
    m_VisibleLineVdpRegisterWrites = 0;
    m_CompletedBackgroundFetches = 0;
    m_CompletedSpriteFetches = 0;
    m_CompletedSpriteSelectionScans = 0;
    m_BackgroundFetchLine = -1;
    m_BackgroundFetchAbsoluteLine = static_cast<u64>(-1);
    m_PendingBackgroundFetchActivity = TMS99xxVramSlotSchedule::Activity::Unknown;
    m_PendingBackgroundFetchIndex = 0;
    m_HasPendingBackgroundFetch = false;
    for (auto& state : m_SpriteLineFetchStates)
        state = {};
#ifdef SPRITE_EXPANDER
    for (auto& state : m_SpriteExpanderLineStates)
        state = {};
#endif
    m_PendingSpriteFetch = {};
    m_HasPendingSpriteFetch = false;
    m_RenderedSpriteDebugWorking = {};
    m_LastRenderedSpriteDebugFrame = {};
    m_SpriteSelectionHistoryWorking = {};
    m_LastSpriteSelectionHistory = {};
    m_LastCpuPortAccessTStates = 0;
    m_HasLastCpuPortAccess = false;

    m_pCurrentPalette = kPalette_888_tms9918_analog;
    m_ColorProfile = 2;
    m_PaletteRevision = 1;
}

TMS99xx::~TMS99xx()
{
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    SafeDeleteArray(m_pInfoBuffer);
    SafeDeleteArray(m_pFrameBuffer);
#endif
    SafeDeleteArray(m_pVdpVRAM);
    SafeDeleteArray(m_pNameTableDebugRGBA);
    SafeDeleteArray(m_pPatternTableDebugRGBA);
    SafeDeleteArray(m_pSpritePatternDebugRGBA);
    SafeDeleteArray(m_pSpriteDebugAtlasRGBA);
    SafeDeleteArray(m_pDebugPreviewRGBA);
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    SafeDeleteArray(m_pLegacyBackground);
#endif
    SafeDeleteArray(m_pStreamingBackground);
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    SafeDeleteArray(m_pLegacyComposed);
#endif
    SafeDeleteArray(m_pStreamingComposed);
    SafeDeleteArray(m_pStreamingSpriteInfo);
    SafeDeleteArray(m_pFullRasterDebugRGB);
    SafeDeleteArray(m_pPresentedFrame);
}

void TMS99xx::Init()
{
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    const GC_VideoFrameDescriptor descriptor = GetFrameDescriptor();
    m_pFrameBuffer = new u16[descriptor.buffer_width * descriptor.buffer_height];
    m_pInfoBuffer = new u8[GC_RESOLUTION_WIDTH * GC_LINES_PER_FRAME_PAL];
#endif
    m_pVdpVRAM = new u8[kTmsVramSizeBytes];
    m_pNameTableDebugRGBA = new u8[256 * 256 * 4];
    m_pPatternTableDebugRGBA = new u8[256 * 256 * 4];
    m_pSpritePatternDebugRGBA = new u8[256 * 64 * 4];
    m_pSpriteDebugAtlasRGBA = new u8[128 * 64 * 4];
    m_pDebugPreviewRGBA = new u8[GC_RESOLUTION_WIDTH * GC_RESOLUTION_HEIGHT * 4];
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    m_pLegacyBackground = new u16[GC_RESOLUTION_WIDTH * GC_RESOLUTION_HEIGHT];
#endif
    m_pStreamingBackground = new u16[GC_RESOLUTION_WIDTH * GC_RESOLUTION_HEIGHT];
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    m_pLegacyComposed = new u16[GC_RESOLUTION_WIDTH * GC_RESOLUTION_HEIGHT];
#endif
    m_pStreamingComposed = new u16[GC_RESOLUTION_WIDTH * GC_RESOLUTION_HEIGHT];
    m_pStreamingSpriteInfo = new u8[GC_RESOLUTION_WIDTH * GC_RESOLUTION_HEIGHT];
    m_pFullRasterDebugRGB = new u8[TMS99xxRasterTiming::DotsPerLine * kFullRasterDebugMaxLines * 3];
    const GC_VideoFrameDescriptor outputDescriptor = GetFrameDescriptor();
    m_pPresentedFrame = new u8[static_cast<size_t>(outputDescriptor.buffer_width) *
        outputDescriptor.buffer_height * 3]();
    InitPalettes();
    Reset();
}

void TMS99xx::Reset()
{
    m_iLinesPerFrame = m_bPAL ? GC_LINES_PER_FRAME_PAL
                              : GC_LINES_PER_FRAME_NTSC;
    m_bFirstByteInSequence = true;
    m_ControlLatchValue = 0;
    m_VdpBuffer = 0;
    m_VdpAddress = 0;
    m_VdpStatus = 0;
    m_NameTableDebugDirty = true;
    m_PatternTableDebugDirty = true;
    m_SpritePatternDebugDirty = true;
    ++m_DebugVRAMRevision;
    ++m_DebugPreviewSourceGeneration;

#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    const GC_VideoFrameDescriptor descriptor = GetFrameDescriptor();
    for (int i = 0; i < (descriptor.buffer_width * descriptor.buffer_height); i++)
        m_pFrameBuffer[i] = 1;
    for (int i = 0; i < (GC_RESOLUTION_WIDTH * GC_LINES_PER_FRAME_PAL); i++)
        m_pInfoBuffer[i] = 0;
#endif
    for (int i = 0; i < 0x4000; i++)
        m_pVdpVRAM[i] = 0;
    for (int i = 0; i < GC_RESOLUTION_WIDTH * GC_RESOLUTION_HEIGHT; ++i)
    {
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
        m_pLegacyBackground[i] = 0;
#endif
        m_pStreamingBackground[i] = 0;
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
        m_pLegacyComposed[i] = 0;
#endif
        m_pStreamingComposed[i] = 0;
        m_pStreamingSpriteInfo[i] = 0;
    }
    for (int i = 0; i < GC_RESOLUTION_HEIGHT; ++i)
    {
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
        m_LegacyBackgroundLineCaptured[i] = false;
#endif
        m_StreamingBackgroundLineReady[i] = false;
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
        m_LegacyComposedLineCaptured[i] = false;
#endif
        m_StreamingSpriteLineReady[i] = false;
        m_StreamingBlankFromColumn[i] = GC_RESOLUTION_WIDTH;
        m_Register7Line[i] = {};
    }
    for (int i = 0; i < 8; i++)
        m_VdpRegister[i] = 0;

    m_bDisplayEnabled = false;
    m_bSpriteOvrRequest = false;
    m_FrameRenderDiagnostics = {};
    m_VisibleLineCpuVramWrites = 0;
    m_VisibleLineVdpRegisterWrites = 0;
    m_CompletedBackgroundFetches = 0;
    m_CompletedSpriteFetches = 0;
    m_CompletedSpriteSelectionScans = 0;
    for (auto& latch : m_BackgroundFetchLatches)
        latch = {};
    m_BackgroundFetchLine = -1;
    m_BackgroundFetchAbsoluteLine = static_cast<u64>(-1);
    m_PendingBackgroundFetchActivity = TMS99xxVramSlotSchedule::Activity::Unknown;
    m_PendingBackgroundFetchIndex = 0;
    m_HasPendingBackgroundFetch = false;
    for (auto& state : m_SpriteLineFetchStates)
        state = {};
#ifdef SPRITE_EXPANDER
    for (auto& state : m_SpriteExpanderLineStates)
        state = {};
#endif
    m_PendingSpriteFetch = {};
    m_HasPendingSpriteFetch = false;
    m_RenderedSpriteDebugWorking = {};
    m_LastRenderedSpriteDebugFrame = {};
    m_SpriteSelectionHistoryWorking = {};
    m_LastSpriteSelectionHistory = {};

    m_LineEvents.vint = false;
    m_LineEvents.display = false;
    m_LineEvents.render = false;
    m_LineEvents.collision = false;
    m_SuppressVBlankFlag = false;
    m_SuppressedVBlankFlags = 0;
    m_StreamingCollisionPhase = -1;
    m_StreamingCollisionLine = -1;

    m_iCycleCounter = 0;
    m_iRenderLine = 0;
    m_VramSequencer.Reset();
    m_LastCpuPortAccessTStates = 0;
    m_HasLastCpuPortAccess = false;

    m_iTstates = 0;

    // Preserve the established event positions while expressing them in the
    // native VDP master-clock domain. These approximate thresholds will be
    // replaced individually by the raster sequencer; the clock-domain change
    // itself must not move existing NTSC behaviour.
    m_Timing[TIMING_VINT] = 220 * 3;
    m_Timing[TIMING_RENDER] = 195 * 3;
    m_Timing[TIMING_DISPLAY] = 37 * 3;

}



#ifdef SPRITE_EXPANDER
void TMS99xx::SetNoSpriteLimit(bool noSpriteLimit)
{
    m_bNoSpriteLimit = noSpriteLimit;
    if (!noSpriteLimit)
        for (auto& state : m_SpriteExpanderLineStates)
            state = {};
}
#endif

bool TMS99xx::Tick(unsigned int clockCycles)
{
    return TickDeviceClocks(clockCycles * 3u);
}

bool TMS99xx::TickDeviceClocks(unsigned int masterClocks)
{
    bool return_vblank = false;

    while (masterClocks != 0)
    {
        const unsigned int clocksToLineEnd =
            GC_VDP_MASTER_CLOCKS_PER_LINE - m_iCycleCounter;
        const int derivedPhase = NextDerivedRasterEventPhase(
            m_iRenderLine, m_iCycleCounter);
        const unsigned int clocksToDerivedEvent =
            derivedPhase > m_iCycleCounter &&
            derivedPhase <= GC_VDP_MASTER_CLOCKS_PER_LINE
                ? static_cast<unsigned int>(derivedPhase - m_iCycleCounter)
                : clocksToLineEnd;
        const unsigned int step = std::min(
            masterClocks, std::min(clocksToLineEnd, clocksToDerivedEvent));
        m_VramSequencer.SetCpuSchedule(
            TMS99xxCpuAccessTiming::SelectSchedule(GetCpuVRAMAccessState()));
        if (const auto* phases = GetCpuVRAMPhaseMap())
            m_VramSequencer.SetCpuPhaseMap(*phases);
        else
            m_VramSequencer.ClearCpuPhaseMap();
        m_VramSequencer.SetVideoSchedule(GetVideoVRAMSchedule());
        m_iCycleCounter += static_cast<int>(step);
        masterClocks -= step;
        m_VramSequencer.Advance(step,
            [this](const TMS99xxVramSequencer::Completion& completion) {
                CompleteVRAMTransfer(completion);
            },
            [this](const TMS99xxVramSequencer::Position& position,
                   const TMS99xxVramSlotSchedule::Slot& slot) {
                if (UseLegacyTimedVideoFetches())
                    IssueTimedVideoFetch(position, slot);
            });

        if (m_iCycleCounter == derivedPhase)
            OnDerivedRasterEvent(m_iRenderLine, derivedPhase);

        // Events are evaluated in raster order. Advancing a large block is
        // therefore equivalent to advancing it one native master clock at a
        // time, without paying that cost.
        if (!m_LineEvents.display &&
            m_iCycleCounter >= m_Timing[TIMING_DISPLAY])
        {
            m_LineEvents.display = true;
            m_bDisplayEnabled = IsSetBit(m_VdpRegister[1], 6);
            m_pMemory->CheckVideoTimingEvent(
                m_iRenderLine, VideoTimingDisplay);
        }

        // Sprite collision is a per-pixel signal: it must be raised where the
        // sprites overlap, not at the end of the line, because a program can
        // poll it to time a split at a chosen horizontal position.
        if (!m_LineEvents.collision &&
            m_StreamingCollisionPhase >= 0 &&
            m_StreamingCollisionLine == m_iRenderLine &&
            m_iCycleCounter >= m_StreamingCollisionPhase)
        {
            m_LineEvents.collision = true;
            ++m_StreamingCollisions;
            if (m_FrameRenderDiagnostics.streamingCollisionLine < 0)
            {
                m_FrameRenderDiagnostics.streamingCollisionLine = m_iRenderLine;
                m_FrameRenderDiagnostics.streamingCollisionDot =
                    m_StreamingCollisionPhase /
                    TMS99xxRasterTiming::PhasesPerDot;
            }
            if (IsStreamingStatusAuthoritative())
                SetStatus(SetBit(m_VdpStatus, 5),
                    DebugEventSource::DeviceInternal);
        }

        if (!m_LineEvents.render &&
            m_iCycleCounter >= m_Timing[TIMING_RENDER])
        {
            m_LineEvents.render = true;
            m_pMemory->CheckVideoTimingEvent(
                m_iRenderLine, VideoTimingRender);
            ScanLine(m_iRenderLine);
            // The packed CPU surface is produced only on demand. Mark its
            // indexed source dirty once for this rendered line, irrespective
            // of how often a faster host display presents it afterwards.
            ++m_PresentationSourceRevision;
            if (m_bFullRasterDebugEnabled)
                RenderFullRasterDebugLine(m_iRenderLine);
            if (m_pCrtSignalSink != NULL)
                EmitCrtSignalLine(m_iRenderLine);
        }

        if (m_iRenderLine == GC_RESOLUTION_HEIGHT &&
            !m_LineEvents.vint &&
            m_iCycleCounter >= m_Timing[TIMING_VINT])
        {
            m_LineEvents.vint = true;
            m_pMemory->CheckVideoTimingEvent(
                m_iRenderLine, VideoTimingVBlank);

            if (m_SuppressVBlankFlag)
            {
                // A status read was in flight across this instant. The CPU has
                // already sampled a zero and its /RD edge clears the flag right
                // after it is raised, so this frame's F never becomes visible
                // and no interrupt is produced. Polling loops lose the frame;
                // the documented remedy is to keep the VDP interrupt enabled
                // and read the status from the handler instead.
                m_SuppressVBlankFlag = false;
                ++m_SuppressedVBlankFlags;
            }
            else
            {
                SetStatus(SetBit(m_VdpStatus, 7),
                    DebugEventSource::DeviceInternal);
                RefreshInterruptLine();
            }
        }

        if (m_iCycleCounter == GC_VDP_MASTER_CLOCKS_PER_LINE)
        {
            OnRasterLineCompleted(m_iRenderLine);
            if (m_iRenderLine == GC_RESOLUTION_HEIGHT)
            {
                return_vblank = true;
                FinalizeFrameRenderDiagnostics();
            }

            m_iRenderLine = (m_iRenderLine + 1) % m_iLinesPerFrame;
            BeginRegister7Line(m_iRenderLine);
            m_iCycleCounter = 0;
            m_LineEvents.vint = false;
            m_LineEvents.render = false;
            m_LineEvents.display = false;
            m_LineEvents.collision = false;
        }
    }

    return return_vblank;
}

u8 TMS99xx::GetDataPort(uint64_t tstates)
{
    SetControlLatch(true, DebugEventSource::CpuInstruction);
    u8 ret = m_VdpBuffer;
    QueueVRAMTransfer(tstates, {TMS99xxVramSequencer::RequestKind::CpuRead,
                       m_VdpAddress, 0, false});
    SetAddressCounter((m_VdpAddress + 1) & 0x3FFF, DebugEventSource::AutoIncrement);
    return ret;
}

u8 TMS99xx::GetStatusFlags()
{
    SetControlLatch(true, DebugEventSource::CpuInstruction);

    u8 extendedStatus = 0;
    if (HandleExtendedStatusRead(extendedStatus))
        return extendedStatus;

    // If F is about to be raised while this read is still on the bus, the read
    // wins: the CPU has already sampled the old zero and the clear that ends
    // the read wipes the flag before software can ever see it. Only worth
    // testing when F is currently down, because a read that finds it up has
    // caught the frame and is simply acknowledging it.
    if (!IsSetBit(m_VdpStatus, 7) && !m_LineEvents.vint)
    {
        const int clocksPerFrame =
            m_iLinesPerFrame * GC_VDP_MASTER_CLOCKS_PER_LINE;
        const int readAt =
            m_iRenderLine * GC_VDP_MASTER_CLOCKS_PER_LINE + m_iCycleCounter;
        const int raisedAt = GC_RESOLUTION_HEIGHT *
            GC_VDP_MASTER_CLOCKS_PER_LINE + m_Timing[TIMING_VINT];
        int untilRaised = raisedAt - readAt;
        if (untilRaised < 0)
            untilRaised += clocksPerFrame;
        if (untilRaised < StatusReadRaceWindowClocks)
            m_SuppressVBlankFlag = true;
    }

    u8 ret = m_VdpStatus;
    SetStatus(m_VdpStatus & 0x1F, DebugEventSource::CpuInstruction);

    // Reading the TMS99xx status register acknowledges VBlank: status
    // bit 7 is cleared and the level-triggered INT output is released.  The
    // previous code tested bit 7 only after clearing it, so the line could
    // never be deasserted. The legacy CPU happened to hide that bug by
    // consuming INT as though it were edge-triggered; CLK correctly keeps
    // accepting the asserted level after EI.
    RefreshInterruptLine();

    return ret;
}

void TMS99xx::WriteData(uint64_t tstates, u8 data)
{
    SetControlLatch(true, DebugEventSource::CpuInstruction);
    if (HandleExtendedDataWrite(data))
        return;
    SetReadBuffer(data, DebugEventSource::CpuInstruction);

    QueueVRAMTransfer(tstates, {TMS99xxVramSequencer::RequestKind::CpuWrite,
                       m_VdpAddress, data, true});
    SetAddressCounter((m_VdpAddress + 1) & 0x3FFF, DebugEventSource::AutoIncrement);
}

void TMS99xx::WriteControl(uint64_t tstates, u8 control)
{
    if (m_bFirstByteInSequence)
    {
        // The first control byte goes straight into the low byte of the
        // address counter, and is kept in the latch for a register write.
        // openMSX (MSX1 VDPs: "no latch used, but VDP address writes are
        // done directly", found on hardware by hap), CLK (non-Yamaha 9918s,
        // Sega VDPs too) and Gearcoleco since 8ad5f92 agree; the 15/08 choice
        // of leaving the counter alone had no source. A $BE access or a
        // status read before the second byte ends the command, the new low
        // byte already in place. The $BE read-ahead buffer is not touched:
        // what an old Gearcoleco did, overwriting it, was the bug fixed then.
        SetControlLatchValue(control, DebugEventSource::CpuInstruction);
        SetControlLatch(false, DebugEventSource::CpuInstruction);
        SetAddressCounter((m_VdpAddress & 0x3F00) | control,
            DebugEventSource::CpuInstruction);
    }
    else
    {
        SetControlLatch(true, DebugEventSource::CpuInstruction);
        if (HandleExtendedControlCommand(tstates, control,
                                         m_ControlLatchValue))
            return;
        SetAddressCounter(((control & 0x3F) << 8) | m_ControlLatchValue,
            DebugEventSource::CpuInstruction);

        switch (control & 0xC0)
        {
            case 0x00:
            {
                QueueVRAMTransfer(tstates,
                                  {TMS99xxVramSequencer::RequestKind::CpuRead,
                                   m_VdpAddress, 0, false});
                SetAddressCounter((m_VdpAddress + 1) & 0x3FFF,
                    DebugEventSource::AutoIncrement);
                break;
            }
            case 0x80:
            {
                const u8 rawRegister = control & 0x3F;
                if (HandleExtendedRegisterWrite(rawRegister,
                                                m_ControlLatchValue))
                    break;
                bool old_nmi = IsSetBit(m_VdpRegister[1], 5);
                u8 masks[8] = { 0x03, 0xFB, 0x0F, 0xFF, 0x07, 0x7F, 0x07, 0xFF };
                u8 reg = rawRegister & 0x07;
                SetRegisterValue(reg, m_ControlLatchValue & masks[reg]);
                OnLegacyRegisterWritten(reg);
                // A VDP register command is complete only here, after the
                // second control-port byte.  The event target is R0-R7 and
                // the value is the final, chip-masked value.

                if (reg == 1 && IsSetBit(m_VdpRegister[1], 5) != old_nmi)
                    RefreshInterruptLine();

                if (reg < 2)
                {
                    SetVideoMode(((m_VdpRegister[1] & 0x08) >> 1)
                        | (m_VdpRegister[0] & 0x02)
                        | ((m_VdpRegister[1] & 0x10) >> 4),
                        DebugEventSource::DeviceInternal);
                }

                break;
            }
        }
    }
}

void TMS99xx::RefreshInterruptLine()
{
    const bool frame = IsSetBit(m_VdpRegister[1], 5) &&
                       IsSetBit(m_VdpStatus, 7);
    m_interruptLines->SetMaskableInterruptLine(frame ||
                                               ExtendedInterruptPending());
}

void TMS99xx::RaiseExtendedSpriteCollision()
{
    if (!IsSetBit(m_VdpStatus, 5))
        SetStatus(SetBit(m_VdpStatus, 5), DebugEventSource::DeviceInternal);
}

void TMS99xx::RaiseExtendedSpriteOverflow(u8 sprite)
{
    if (!IsSetBit(m_VdpStatus, 6))
        SetStatus((SetBit(m_VdpStatus, 6) & 0xE0) | (sprite & 0x1F),
                  DebugEventSource::DeviceInternal);
}

const GC_VideoDeviceDescriptor& TMS99xx::GetDeviceDescriptor() const
{
    return *FindVideoDeviceDescriptor(m_device);
}

void TMS99xx::CopyHostConfigurationFrom(const IVideoDevice& sourceDevice)
{
    const TMS99xx* source = dynamic_cast<const TMS99xx*>(&sourceDevice);
    if (!source)
        return;
    SetPredefinedPalette(static_cast<int>(source->m_ColorProfile));
    m_Overscan = source->m_Overscan;
    m_bPresentStreaming = source->m_bPresentStreaming;
    m_bFullRasterDebugEnabled = source->m_bFullRasterDebugEnabled;
    m_pCrtSignalSink = source->m_pCrtSignalSink;
#ifdef SPRITE_EXPANDER
    m_bNoSpriteLimit = source->m_bNoSpriteLimit;
#endif
    ++m_FrameDescriptorRevision;
    m_SpritePatternDebugDirty = true;
    ++m_DebugPreviewSourceGeneration;
}

std::size_t TMS99xx::GetOptionCount() const
{
#ifdef SPRITE_EXPANDER
    return 4;
#else
    return 3;
#endif
}

bool TMS99xx::DescribeOption(std::size_t index,
    GC_VideoOptionDescriptor& descriptor) const
{
    static const GC_VideoOptionDescriptor options[] = {
        {GC_VideoOption::Border, "Border", GC_VideoOptionType::Choice,
         GC_VideoBorder::Disabled, GC_VideoBorder::Full320},
        {GC_VideoOption::PredefinedPalette, "Palette",
         GC_VideoOptionType::Choice, 0, 4},
        {GC_VideoOption::FullRaster, "Full raster",
         GC_VideoOptionType::Boolean, 0, 1},
#ifdef SPRITE_EXPANDER
        {GC_VideoOption::UnlimitedSprites, "Unlimited sprites",
         GC_VideoOptionType::Boolean, 0, 1},
#endif
    };
    if (index >= sizeof(options) / sizeof(options[0]))
        return false;
    descriptor = options[index];
    return true;
}

bool TMS99xx::SetOption(const char* code, s64 value)
{
    if (!code)
        return false;
    if (std::strcmp(code, GC_VideoOption::Border) == 0)
    {
        if (value < GC_VideoBorder::Disabled ||
            value > GC_VideoBorder::Full320)
            return false;
        SetOverscan(static_cast<Overscan>(value));
        return true;
    }
    if (std::strcmp(code, GC_VideoOption::PredefinedPalette) == 0)
    {
        // 3 was the removed custom editor; left unused so an old config
        // falls back instead of landing on the SMS colours.
        if (value < 0 || value > 4 || value == 3)
            return false;
        SetPredefinedPalette(static_cast<int>(value));
        return true;
    }
    if (std::strcmp(code, GC_VideoOption::FullRaster) == 0)
    {
        SetFullRasterDebugEnabled(value != 0);
        return true;
    }
#ifdef SPRITE_EXPANDER
    if (std::strcmp(code, GC_VideoOption::UnlimitedSprites) == 0)
    {
        SetNoSpriteLimit(value != 0);
        return true;
    }
#endif
    return false;
}

std::size_t TMS99xx::GetColorProfileCount() const
{
    return 4;
}

bool TMS99xx::DescribeColorProfile(std::size_t index,
    GC_VideoColorProfileDescriptor& descriptor) const
{
    static const GC_VideoColorProfileDescriptor profiles[] = {
        {0, "coleco", "Coleco"},
        {1, "tms9918", "TMS9918"},
        {2, "tms9918.analog", "TMS9918 Analog"},
        {4, "sms.315-5124", "SMS VDP 315-5124"},
    };
    if (index >= sizeof(profiles) / sizeof(profiles[0]))
        return false;
    descriptor = profiles[index];
    return true;
}

bool TMS99xx::GetPaletteView(GC_VideoPaletteView& view) const
{
    view.rgb888 = m_pCurrentPalette;
    view.color_count = 16;
    view.revision = m_PaletteRevision;
    return view.IsValid();
}

std::size_t TMS99xx::GetStateValueCount() const
{
    return sizeof(kTmsStateValues) / sizeof(kTmsStateValues[0]);
}

bool TMS99xx::DescribeStateValue(std::size_t index,
    GC_VideoStateValueDescriptor& descriptor) const
{
    descriptor = {};
    if (index >= GetStateValueCount())
        return false;
    descriptor = kTmsStateValues[index];
    return true;
}

bool TMS99xx::ReadStateValueSnapshot(std::size_t index, u64& value) const
{
    const TMS99xxRasterTiming::DotPosition raster = GetRasterDotPosition();
    switch (index)
    {
        case 0: value = static_cast<u64>(m_iMode); return true;
        case 1: value = m_bFirstByteInSequence ? 1 : 0; return true;
        case 2: value = m_ControlLatchValue; return true;
        case 3: value = m_VdpBuffer; return true;
        case 4: value = m_VdpStatus; return true;
        case 5: value = m_VdpAddress; return true;
        case 6: value = raster.dot; return true;
        case 7: value = static_cast<u64>(m_iRenderLine); return true;
        case 8: return false;
        case 9: value = raster.phaseInDot; return true;
        case 10: value = static_cast<u64>(m_iCycleCounter); return true;
        default: return false;
    }
}

bool TMS99xx::ReadStateTextSnapshot(std::size_t index, const char*& value) const
{
    value = NULL;
    if (index == 0)
    {
        static constexpr const char* modeNames[] =
        {
            "Graphics I",
            "Text40",
            "Graphics II (3 banks)",
            "Text1Q (Graphics II banking)",
            "Multicolor",
            "Invalid (M1+M2)",
            "Multicolor1Q (Graphics II banking)",
            "Invalid (M1+M2+M3)"
        };
        value = m_iMode >= 0 && m_iMode < 8 ? modeNames[m_iMode] : "Unknown";
        return true;
    }
    if (index != 8)
        return false;

    using Timing = TMS99xxRasterTiming;
    const Timing::VerticalRegion vertical = Timing::GetVerticalRegion(
        static_cast<std::uint16_t>(m_iRenderLine), m_bPAL);
    if (vertical == Timing::VerticalRegion::VerticalSync)
        value = "VSync";
    else if (vertical == Timing::VerticalRegion::BottomBlanking ||
        vertical == Timing::VerticalRegion::TopBlanking)
        value = "VBlank";
    else if (vertical == Timing::VerticalRegion::BottomBorder ||
        vertical == Timing::VerticalRegion::TopBorder)
        value = "Border";
    else
    {
        const Timing::DotPosition raster = GetRasterDotPosition();
        const bool textMode = m_iMode == 1 || m_iMode == 3;
        const Timing::HorizontalRegion horizontal = Timing::GetHorizontalRegion(
            raster.dot,
            textMode ? Timing::DisplayWidth::Text : Timing::DisplayWidth::Graphics,
            GetRasterSignalOutput());
        switch (horizontal)
        {
            case Timing::HorizontalRegion::HSync: value = "HSync"; break;
            case Timing::HorizontalRegion::ColorBurst: value = "Color Burst"; break;
            case Timing::HorizontalRegion::LeftBorder:
            case Timing::HorizontalRegion::RightBorder: value = "Border"; break;
            case Timing::HorizontalRegion::Active: value = "Drawing"; break;
            default: value = "HBlank"; break;
        }
    }
    return true;
}

std::size_t TMS99xx::GetMemorySpaceCount() const
{
    return 1;
}

bool TMS99xx::DescribeMemorySpace(std::size_t index,
    GC_VideoMemorySpaceDescriptor& descriptor) const
{
    if (index != 0 || m_pVdpVRAM == NULL)
        return false;

    descriptor = {};
    descriptor.code = kTmsMemorySpaceCode;
    descriptor.name = "VRAM";
    descriptor.description = "TMS99xx video memory";
    descriptor.address_size_bytes = kTmsVramSizeBytes;
    descriptor.installed_size_bytes = kTmsVramSizeBytes;
    descriptor.host_window_size_bytes = kTmsVramSizeBytes;
    descriptor.host_page_size_bytes = kTmsVramSizeBytes;
    descriptor.host_page_count = 1;
    descriptor.mapping_flags = GC_VIDEO_MEMORY_MAPPING_LINEAR;
    descriptor.readable = true;
    descriptor.writable = true;
    descriptor.direct_data = m_pVdpVRAM;
    descriptor.layout_revision = 1;
    return true;
}

std::size_t TMS99xx::GetMemoryRegionCount(const char* spaceCode) const
{
    return spaceCode != NULL && std::strcmp(spaceCode, kTmsMemorySpaceCode) == 0
        ? 1 : 0;
}

bool TMS99xx::DescribeMemoryRegion(const char* spaceCode, std::size_t index,
    GC_VideoMemoryRegionDescriptor& descriptor) const
{
    if (spaceCode == NULL || std::strcmp(spaceCode, kTmsMemorySpaceCode) != 0 ||
        index != 0)
        return false;

    descriptor = {};
    descriptor.code = "TMS99XX.VRAM.DISPLAY";
    descriptor.description = "TMS99xx display VRAM";
    descriptor.kind = GC_VideoMemoryRegionKind::Storage;
    descriptor.role = GC_VideoMemoryRegionRole::DisplayRam;
    descriptor.start = 0;
    descriptor.size = kTmsVramSizeBytes;
    descriptor.access_flags = GC_VIDEO_MEMORY_ACCESS_READ |
        GC_VIDEO_MEMORY_ACCESS_WRITE |
        GC_VIDEO_MEMORY_ACCESS_DISPLAY_FETCH |
        GC_VIDEO_MEMORY_ACCESS_HOST_VISIBLE;
    descriptor.backing_store_code = "TMS99XX.VRAM.STORAGE";
    descriptor.address_mask = kTmsVramSizeBytes - 1;
    descriptor.priority = 0;
    return true;
}

bool TMS99xx::ReadMemorySpace(const char* spaceCode, u32 offset,
    u8* destination, std::size_t size) const
{
    if (m_pVdpVRAM == NULL || spaceCode == NULL ||
        std::strcmp(spaceCode, kTmsMemorySpaceCode) != 0 ||
        destination == NULL || offset > kTmsVramSizeBytes ||
        size > kTmsVramSizeBytes - offset)
        return false;

    std::memcpy(destination, m_pVdpVRAM + offset, size);
    return true;
}

bool TMS99xx::WriteMemorySpace(const char* spaceCode, u32 offset,
    const u8* source, std::size_t size)
{
    if (m_pVdpVRAM == NULL || spaceCode == NULL ||
        std::strcmp(spaceCode, kTmsMemorySpaceCode) != 0 ||
        source == NULL || offset > kTmsVramSizeBytes ||
        size > kTmsVramSizeBytes - offset)
        return false;

    for (std::size_t index = 0; index < size; ++index)
        WriteVRAM(static_cast<u16>(offset + index), source[index]);
    return true;
}

std::size_t TMS99xx::GetRegisterBankCount() const
{
    return 2;
}

bool TMS99xx::DescribeRegisterBank(std::size_t index,
    GC_VideoRegisterBankDescriptor& descriptor) const
{
    descriptor = {};
    if (index == 0)
    {
        descriptor.code = kTmsControlBankCode;
        descriptor.name = "Control Registers";
        descriptor.description = "TMS99xx control registers";
        descriptor.register_count = sizeof(kTmsControlRegisters) /
            sizeof(kTmsControlRegisters[0]);
    }
    else if (index == 1)
    {
        descriptor.code = kTmsStatusBankCode;
        descriptor.name = "Status Registers";
        descriptor.description = "TMS99xx status register";
        descriptor.register_count = 1;
    }
    else
        return false;

    descriptor.register_width_bits = 8;
    descriptor.schema_revision = 1;
    return true;
}

std::size_t TMS99xx::GetRegisterCount(const char* bankCode) const
{
    if (IsDescriptorCode(bankCode, kTmsControlBankCode))
        return sizeof(kTmsControlRegisters) / sizeof(kTmsControlRegisters[0]);
    if (IsDescriptorCode(bankCode, kTmsStatusBankCode))
        return 1;
    return 0;
}

bool TMS99xx::DescribeRegister(const char* bankCode, std::size_t index,
    GC_VideoRegisterDescriptor& descriptor) const
{
    descriptor = {};
    if (IsDescriptorCode(bankCode, kTmsControlBankCode))
    {
        if (index >= GetRegisterCount(bankCode))
            return false;
        const TmsRegisterDefinition& definition = kTmsControlRegisters[index];
        descriptor.code = definition.code;
        descriptor.short_name = definition.shortName;
        descriptor.description = definition.description;
        descriptor.bank_code = kTmsControlBankCode;
        descriptor.register_index = static_cast<u32>(index);
        descriptor.width_bits = 8;
        descriptor.implemented_mask = definition.implementedMask;
        descriptor.access_flags = GC_VIDEO_REGISTER_ACCESS_WRITE |
            GC_VIDEO_REGISTER_ACCESS_OBSERVE |
            GC_VIDEO_REGISTER_ACCESS_WRITE_SIDE_EFFECT;
        descriptor.has_derived_address = definition.hasDerivedAddress;
        descriptor.derived_address_width_bits = definition.hasDerivedAddress ? 14 : 0;
        return true;
    }

    if (!IsDescriptorCode(bankCode, kTmsStatusBankCode) || index != 0)
        return false;
    descriptor.code = "TMS99XX.STATUS.S0";
    descriptor.short_name = "STATUS";
    descriptor.description = "Status register";
    descriptor.bank_code = kTmsStatusBankCode;
    descriptor.register_index = 0;
    descriptor.width_bits = 8;
    descriptor.implemented_mask = 0xFF;
    descriptor.access_flags = GC_VIDEO_REGISTER_ACCESS_READ |
        GC_VIDEO_REGISTER_ACCESS_OBSERVE |
        GC_VIDEO_REGISTER_ACCESS_READ_SIDE_EFFECT;
    return true;
}

std::size_t TMS99xx::GetRegisterFieldCount(const char* bankCode,
    u32 registerIndex) const
{
    const TmsRegisterFieldDefinition* fields = NULL;
    std::size_t fieldCount = 0;
    if (IsDescriptorCode(bankCode, kTmsControlBankCode))
    {
        if (registerIndex >= GetRegisterCount(bankCode))
            return 0;
        fields = kTmsControlFields;
        fieldCount = sizeof(kTmsControlFields) / sizeof(kTmsControlFields[0]);
    }
    else if (IsDescriptorCode(bankCode, kTmsStatusBankCode) && registerIndex == 0)
    {
        fields = kTmsStatusFields;
        fieldCount = sizeof(kTmsStatusFields) / sizeof(kTmsStatusFields[0]);
    }
    else
        return 0;

    std::size_t result = 0;
    for (std::size_t index = 0; index < fieldCount; ++index)
        if (fields[index].registerIndex == registerIndex)
            ++result;
    return result;
}

bool TMS99xx::DescribeRegisterField(const char* bankCode, u32 registerIndex,
    std::size_t fieldIndex, GC_VideoRegisterFieldDescriptor& descriptor) const
{
    const TmsRegisterFieldDefinition* fields = NULL;
    std::size_t fieldCount = 0;
    if (IsDescriptorCode(bankCode, kTmsControlBankCode))
    {
        fields = kTmsControlFields;
        fieldCount = sizeof(kTmsControlFields) / sizeof(kTmsControlFields[0]);
    }
    else if (IsDescriptorCode(bankCode, kTmsStatusBankCode))
    {
        fields = kTmsStatusFields;
        fieldCount = sizeof(kTmsStatusFields) / sizeof(kTmsStatusFields[0]);
    }
    else
        return false;

    for (std::size_t index = 0; index < fieldCount; ++index)
    {
        if (fields[index].registerIndex != registerIndex)
            continue;
        if (fieldIndex-- != 0)
            continue;
        descriptor = {};
        descriptor.code = fields[index].code;
        descriptor.description = fields[index].description;
        descriptor.bank_code = bankCode;
        descriptor.register_index = registerIndex;
        descriptor.mask = fields[index].mask;
        descriptor.shift = fields[index].shift;
        descriptor.access_flags = fields[index].accessFlags;
        return true;
    }
    return false;
}

bool TMS99xx::ReadRegisterSnapshot(const char* bankCode, u32 registerIndex,
    u32& value) const
{
    if (IsDescriptorCode(bankCode, kTmsControlBankCode) &&
        registerIndex < sizeof(m_VdpRegister))
    {
        value = m_VdpRegister[registerIndex];
        return true;
    }
    if (IsDescriptorCode(bankCode, kTmsStatusBankCode) && registerIndex == 0)
    {
        value = m_VdpStatus;
        return true;
    }
    return false;
}

bool TMS99xx::ReadRegisterDerivedAddressSnapshot(const char* bankCode,
    u32 registerIndex, u32& address) const
{
    if (!IsDescriptorCode(bankCode, kTmsControlBankCode) ||
        registerIndex < 2 || registerIndex > 6)
        return false;

    const int mode = ((m_VdpRegister[0] & 0x06) << 8) |
        (m_VdpRegister[1] & 0x18);
    switch (registerIndex)
    {
        case 2:
            address = static_cast<u32>((m_VdpRegister[2] &
                (mode == 0x010 ? 0x7F : 0x0F)) << 10);
            return true;
        case 3:
            address = static_cast<u32>((mode == 0x200 || mode == 0x210)
                ? (m_VdpRegister[3] & 0x80) << 6
                : m_VdpRegister[3] << 6);
            return true;
        case 4:
            address = static_cast<u32>((m_VdpRegister[4] &
                ((mode == 0x200 || mode == 0x210) ? 0x04 :
                    (mode == 0x010 ? 0x3F : 0x07))) << 11);
            return true;
        case 5:
            address = static_cast<u32>((m_VdpRegister[5] & 0x7F) << 7);
            return true;
        case 6:
            address = static_cast<u32>((m_VdpRegister[6] & 0x07) << 11);
            return true;
        default:
            return false;
    }
}

// Table bases come from ReadRegisterDerivedAddressSnapshot, which already
// applies the per-mode masks; the sizes are the TMS9918A datasheet's. In
// Graphics II the colour and pattern tables are the full 6 KB three-thirds
// layout; games that shrink them with the low mask bits are rare, and a
// byte counted into a third that is not displayed is still pattern data.
std::size_t TMS99xx::DescribeVramTables(GC_VramTableSpan* spans,
    std::size_t capacity) const
{
    const int mode = ((m_VdpRegister[0] & 0x06) << 8) | (m_VdpRegister[1] & 0x18);
    const bool graphics2 = (mode & 0x200) != 0;
    const bool text = (mode & 0x010) != 0;
    const bool multicolor = !text && (mode & 0x008) != 0;

    std::size_t count = 0;
    const auto add = [&](GC_VramTableRole role, u32 registerIndex, u32 size)
    {
        u32 start = 0;
        if (count >= capacity || spans == NULL ||
            !ReadRegisterDerivedAddressSnapshot(kTmsControlBankCode, registerIndex, start))
            return;
        spans[count].role = role;
        spans[count].start = start & 0x3FFF;
        spans[count].size = size;
        ++count;
    };

    add(GC_VramTableRole::NameTable, 2, text ? 960 : 768);
    if (!text && !multicolor)
        add(GC_VramTableRole::ColorTable, 3, graphics2 ? 0x1800 : 32);
    add(GC_VramTableRole::PatternGenerator, 4,
        graphics2 ? 0x1800 : (multicolor ? 0x600 : 0x800));
    if (!text)
    {
        add(GC_VramTableRole::SpriteAttributes, 5, 128);
        add(GC_VramTableRole::SpritePatterns, 6, 0x800);
    }
    return count;
}

bool TMS99xx::DescribeDebugScene(GC_VideoDebugSceneDescriptor& descriptor) const
{
    descriptor = {};
    descriptor.canvas_width = 256;
    descriptor.canvas_height = 192;
    return true;
}

std::size_t TMS99xx::GetDebugLayerCount() const
{
    return 3;
}

std::size_t TMS99xx::GetDebugPreviewPartCount() const
{
    return 2;
}

bool TMS99xx::DescribeDebugPreviewPart(std::size_t index,
    GC_VideoDebugPreviewPartDescriptor& descriptor) const
{
    descriptor = {};
    switch (index)
    {
        case 0:
            descriptor.code = "TMS99XX.BACKGROUND";
            descriptor.name = "Background";
            descriptor.visible = true;
            return true;
        case 1:
            descriptor.code = "TMS99XX.SPRITES";
            descriptor.name = "Sprites";
            descriptor.visible = true;
            return true;
        default:
            return false;
    }
}

std::size_t TMS99xx::GetDebugPreviewOptionCount() const
{
    return 1;
}

bool TMS99xx::DescribeDebugPreviewOption(std::size_t index,
    GC_VideoDebugPreviewOptionDescriptor& descriptor) const
{
    if (index != 0)
        return false;
    descriptor = {};
    descriptor.code = "TMS99XX.DIM_HIDDEN_BACKGROUND";
    descriptor.name = "Dim hidden BG";
    descriptor.enabled = true;
    return true;
}

void TMS99xx::BuildDebugPreviewSurface(u64 visiblePartMask, u64 optionMask) const
{
    if (!m_pDebugPreviewRGBA ||
        (m_DebugPreviewBuiltSourceGeneration == m_DebugPreviewSourceGeneration &&
         m_DebugPreviewBuiltPartMask == visiblePartMask &&
         m_DebugPreviewBuiltOptionMask == optionMask))
        return;

    const bool showBackground = (visiblePartMask & (1ull << 0)) != 0;
    const bool showSprites = (visiblePartMask & (1ull << 1)) != 0;
    const bool dimBackground = !showBackground && showSprites &&
        (optionMask & (1ull << 0)) != 0;
    const int pixels = GC_RESOLUTION_WIDTH * GC_RESOLUTION_HEIGHT;
    for (int index = 0; index < pixels; ++index)
    {
        u8* output = &m_pDebugPreviewRGBA[index * 4];
        const bool spritePixel = showSprites &&
            (m_pStreamingSpriteInfo[index] & 0x01) != 0;
        if (spritePixel)
        {
            const u8* rgb = &m_pCurrentPalette[
                (m_pStreamingComposed[index] & 0x0F) * 3];
            output[0] = rgb[0];
            output[1] = rgb[1];
            output[2] = rgb[2];
        }
        else if (showBackground || dimBackground)
        {
            const u8* rgb = &m_pCurrentPalette[
                (m_pStreamingBackground[index] & 0x0F) * 3];
            const int divisor = dimBackground ? 4 : 1;
            output[0] = rgb[0] / divisor;
            output[1] = rgb[1] / divisor;
            output[2] = rgb[2] / divisor;
        }
        else
        {
            output[0] = 0;
            output[1] = 0;
            output[2] = 0;
        }
        output[3] = 0xFF;
    }
    m_DebugPreviewBuiltSourceGeneration = m_DebugPreviewSourceGeneration;
    m_DebugPreviewBuiltPartMask = visiblePartMask;
    m_DebugPreviewBuiltOptionMask = optionMask;
    ++m_DebugPreviewSurfaceGeneration;
}

bool TMS99xx::ReadDebugPreviewSurface(u64 visiblePartMask, u64 optionMask,
    GC_VideoDebugLayerSurface& surface) const
{
    if (!m_pDebugPreviewRGBA)
        return false;
    BuildDebugPreviewSurface(visiblePartMask, optionMask);
    surface = {};
    surface.rgba = m_pDebugPreviewRGBA;
    surface.width = GC_RESOLUTION_WIDTH;
    surface.height = GC_RESOLUTION_HEIGHT;
    surface.row_pitch = GC_RESOLUTION_WIDTH * 4;
    surface.generation = m_DebugPreviewSurfaceGeneration;
    return true;
}

std::size_t TMS99xx::GetDebugSpriteSourceCount() const
{
    return 4;
}

bool TMS99xx::DescribeDebugSpriteSource(std::size_t index,
    GC_VideoDebugSpriteSourceDescriptor& descriptor) const
{
    static const GC_VideoDebugSpriteSourceDescriptor sources[] = {
        { "live-sat", "Live SAT", nullptr,
          GC_VIDEO_DEBUG_SPRITE_CAP_SCREEN_PREVIEW, nullptr, nullptr },
        { "last-rendered-frame", "Last Rendered Frame",
          "Green: complete  Orange: partial  Gray: absent  Cyan: selected",
          GC_VIDEO_DEBUG_SPRITE_CAP_SCREEN_PREVIEW |
          GC_VIDEO_DEBUG_SPRITE_CAP_FETCH_STATE |
          GC_VIDEO_DEBUG_SPRITE_CAP_OVERFLOW |
          GC_VIDEO_DEBUG_SPRITE_CAP_COLLISION, nullptr, nullptr },
        { "current-scanline-fetch", "Current Scanline Fetch",
          "Green: fully latched  Orange: fetch in progress  Red: fifth/overflow  Cyan: selected",
          GC_VIDEO_DEBUG_SPRITE_CAP_SCREEN_PREVIEW |
          GC_VIDEO_DEBUG_SPRITE_CAP_FETCH_STATE |
          GC_VIDEO_DEBUG_SPRITE_CAP_SCANLINE |
          GC_VIDEO_DEBUG_SPRITE_CAP_OVERFLOW |
          GC_VIDEO_DEBUG_SPRITE_CAP_COLLISION, nullptr, nullptr },
        { "last-frame-scanline", "Last Frame Scanline",
          "Green: accepted by VDP  Red: fifth/overflow  Cyan: selected",
          GC_VIDEO_DEBUG_SPRITE_CAP_SCREEN_PREVIEW |
          GC_VIDEO_DEBUG_SPRITE_CAP_FETCH_STATE |
          GC_VIDEO_DEBUG_SPRITE_CAP_SCANLINE |
          GC_VIDEO_DEBUG_SPRITE_CAP_OVERFLOW |
          GC_VIDEO_DEBUG_SPRITE_CAP_COLLISION, "raster-y", "Raster Y" }
    };
    if (index >= sizeof(sources) / sizeof(sources[0]))
        return false;
    descriptor = sources[index];
    return true;
}

bool TMS99xx::ReadDebugSpriteAtlas(const char* sourceCode, int scanline,
    GC_VideoDebugSpriteAtlasSurface& surface) const
{
    if (!m_pSpriteDebugAtlasRGBA || !sourceCode)
        return false;
    const bool renderedSource = std::strcmp(sourceCode, "last-rendered-frame") == 0;
    const bool pipelineSource = std::strcmp(sourceCode, "current-scanline-fetch") == 0;
    const bool scanlineSource = std::strcmp(sourceCode, "last-frame-scanline") == 0;
    if (!renderedSource && !pipelineSource && !scanlineSource &&
        std::strcmp(sourceCode, "live-sat") != 0)
        return false;

    constexpr int atlasWidth = 128;
    constexpr int atlasHeight = 64;
    constexpr int cellSize = 16;
    std::fill(m_pSpriteDebugAtlasRGBA,
        m_pSpriteDebugAtlasRGBA + atlasWidth * atlasHeight * 4, 0);
    for (int pixel = 0; pixel < atlasWidth * atlasHeight; ++pixel)
        m_pSpriteDebugAtlasRGBA[pixel * 4 + 3] = 0xFF;

    const RenderedSpriteDebugFrame& rendered = m_LastRenderedSpriteDebugFrame;
    const SpriteSelectionFrameHistory& history = m_LastSpriteSelectionHistory;
    const SpriteSelectionHistoryRow* historyRow = scanlineSource && history.valid &&
        scanline >= 0 && scanline < history.rasterLines ? &history.rows[scanline] : nullptr;
    const SpritePipelineDebugSnapshot pipeline = GetSpritePipelineDebugSnapshot();
    const SpritePipelineLineDebug* activePipelineLine = nullptr;
    if (pipelineSource)
        for (const SpritePipelineLineDebug& line : pipeline.lines)
        {
            if (!line.valid)
                continue;
            if (pipeline.activeTargetValid &&
                line.absoluteTargetLine == pipeline.activeAbsoluteTargetLine)
            {
                activePipelineLine = &line;
                break;
            }
            if (!activePipelineLine)
                activePipelineLine = &line;
        }

    int spriteSize = renderedSource && rendered.valid
        ? (rendered.large ? 16 : 8)
        : (IsSetBit(m_VdpRegister[1], 1) ? 16 : 8);
    if (historyRow)
        spriteSize = (historyRow->flags & 0x04) ? 16 : 8;
    const u16 attributeBase = static_cast<u16>((m_VdpRegister[5] & 0x7F) << 7);
    const u16 patternBase = static_cast<u16>((m_VdpRegister[6] & 0x07) << 11);
    const auto putPixel = [&](const GC_VideoDebugSpriteAtlasItemDescriptor& item,
                              int x, int y, int colour)
    {
        u8* rgba = &m_pSpriteDebugAtlasRGBA[
            ((item.atlas_y + y) * atlasWidth + item.atlas_x + x) * 4];
        const u8* rgb = &m_pCurrentPalette[(colour & 0x0F) * 3];
        rgba[0] = rgb[0]; rgba[1] = rgb[1]; rgba[2] = rgb[2]; rgba[3] = 0xFF;
    };
    const auto setScreenRect = [&](GC_VideoDebugSpriteAtlasItemDescriptor& item,
                                   u8 rawX, u8 rawY, u8 colour, bool zoomed)
    {
        item.has_screen_rect = false;
        if ((colour & 0x0F) == 0)
            return;
        int y = (rawY + 1) & 0xFF;
        if (y >= 0xE0)
            y -= 0x100;
        const int x = rawX - ((colour & 0x80) ? 32 : 0);
        const int size = spriteSize * (zoomed ? 2 : 1);
        if (x >= GC_RESOLUTION_WIDTH || x + size <= 0 ||
            y >= GC_RESOLUTION_HEIGHT || y + size <= 0)
            return;
        item.has_screen_rect = true;
        item.screen_x = static_cast<float>(x);
        item.screen_y = static_cast<float>(y);
        item.screen_width = static_cast<float>(size);
        item.screen_height = static_cast<float>(size);
    };

    for (int sprite = 0; sprite < 32; ++sprite)
    {
        GC_VideoDebugSpriteAtlasItemDescriptor& item = m_SpriteDebugAtlasItems[sprite];
        item = {};
        item.index = static_cast<u32>(sprite);
        item.atlas_x = static_cast<u32>((sprite & 7) * cellSize);
        item.atlas_y = static_cast<u32>((sprite >> 3) * cellSize);
        item.width = static_cast<u32>(spriteSize);
        item.height = static_cast<u32>(spriteSize);

        if (renderedSource)
        {
            if (!rendered.valid)
                continue;
            const RenderedSpriteDebugEntry& entry = rendered.entries[sprite];
            const u16 completeMask = spriteSize == 16 ? 0xFFFFu : 0x00FFu;
            item.visual_state = !entry.seen
                ? GC_VideoDebugSpriteVisualState::Absent
                : ((entry.fetchedRows & completeMask) == completeMask
                    ? GC_VideoDebugSpriteVisualState::Complete
                    : GC_VideoDebugSpriteVisualState::Partial);
            if (entry.seen)
                setScreenRect(item, entry.x, entry.rawY, entry.colour,
                    rendered.magnified);
            for (int y = 0; y < spriteSize; ++y)
                for (int x = 0; x < spriteSize; ++x)
                    putPixel(item, x, y, entry.pixels[y * 16 + x]);
            continue;
        }
        if (historyRow)
        {
            for (int lane = 0; lane < 5; ++lane)
            {
                if (historyRow->rasterX[lane] == 0xFF)
                    continue;
                const auto slot = TMS99xxVramSlotSchedule::GetSlot(
                    TMS99xxVramSlotSchedule::Schedule::Graphics,
                    historyRow->rasterX[lane]);
                if (slot.activity != TMS99xxVramSlotSchedule::Activity::SpriteScanY ||
                    slot.index != sprite)
                    continue;
                item.visual_state = lane == 4
                    ? GC_VideoDebugSpriteVisualState::Overflow
                    : GC_VideoDebugSpriteVisualState::Complete;
                setScreenRect(item, historyRow->sat[lane][1],
                    historyRow->sat[lane][0], historyRow->sat[lane][3],
                    (historyRow->flags & 0x08) != 0);
                if (lane == 4)
                    break;
                int visibleY = (historyRow->sat[lane][0] + 1) & 0xFF;
                if (visibleY >= 0xE0) visibleY -= 0x100;
                const bool zoomed = (historyRow->flags & 0x08) != 0;
                const int sourceY = (scanline - visibleY) >> (zoomed ? 1 : 0);
                if (sourceY < 0 || sourceY >= spriteSize)
                    continue;
                const int colour = historyRow->sat[lane][3] & 0x0F;
                for (int x = 0; x < spriteSize; ++x)
                {
                    const u8 pattern = historyRow->pattern[lane][x < 8 ? 0 : 1];
                    if (colour && IsSetBit(pattern, 7 - (x & 7)))
                        putPixel(item, x, sourceY, colour);
                }
            }
            continue;
        }
        if (pipelineSource)
        {
            if (!activePipelineLine)
                continue;
            for (int lane = 0; lane < activePipelineLine->selectedCount && lane < 4; ++lane)
            {
                const SpritePipelineLatchDebug& entry = activePipelineLine->selected[lane];
                if (entry.satIndex != sprite)
                    continue;
                item.visual_state = GC_VideoDebugSpriteVisualState::Partial;
                if (entry.xValid && entry.yValid && entry.colourValid)
                    setScreenRect(item, entry.x, entry.y, entry.colour,
                        IsSetBit(m_VdpRegister[1], 0));
                if (!entry.yValid || !entry.colourValid || !entry.patternValid[0])
                    continue;
                const bool complete = entry.yValid && entry.xValid &&
                    entry.nameValid && entry.colourValid && entry.patternValid[0] &&
                    (spriteSize != 16 || entry.patternValid[1]);
                item.visual_state = complete
                    ? GC_VideoDebugSpriteVisualState::Complete
                    : GC_VideoDebugSpriteVisualState::Partial;
                int visibleY = (entry.y + 1) & 0xFF;
                if (visibleY >= 0xE0) visibleY -= 0x100;
                const bool zoomed = IsSetBit(m_VdpRegister[1], 0);
                const int sourceY = (activePipelineLine->targetLine - visibleY) >>
                    (zoomed ? 1 : 0);
                if (sourceY < 0 || sourceY >= spriteSize)
                    continue;
                const int colour = entry.colour & 0x0F;
                for (int x = 0; x < spriteSize; ++x)
                {
                    if (x >= 8 && !entry.patternValid[1]) continue;
                    if (colour && IsSetBit(entry.pattern[x < 8 ? 0 : 1], 7 - (x & 7)))
                        putPixel(item, x, sourceY, colour);
                }
            }
            if (activePipelineLine->overflowRecorded &&
                activePipelineLine->overflowSprite == sprite)
            {
                item.visual_state = GC_VideoDebugSpriteVisualState::Overflow;
                if (activePipelineLine->overflowSatValid)
                    setScreenRect(item, activePipelineLine->overflowSat[1],
                        activePipelineLine->overflowSat[0],
                        activePipelineLine->overflowSat[3],
                        IsSetBit(m_VdpRegister[1], 0));
            }
            continue;
        }

        const int attribute = attributeBase + (sprite << 2);
        int tile = m_pVdpVRAM[attribute + 2];
        tile &= spriteSize == 16 ? 0xFC : 0xFF;
        const int colour = m_pVdpVRAM[attribute + 3] & 0x0F;
        item.visual_state = GC_VideoDebugSpriteVisualState::None;
        GC_VideoDebugSpriteAttributeRecordDescriptor record;
        if (DescribeDebugSpriteAttributeRecord(0, static_cast<u32>(sprite), record))
            item.inactive = !record.visible;
        if (!item.inactive)
            setScreenRect(item, m_pVdpVRAM[attribute + 1],
                m_pVdpVRAM[attribute], m_pVdpVRAM[attribute + 3],
                IsSetBit(m_VdpRegister[1], 0));
        for (int y = 0; y < spriteSize; ++y)
        {
            const int address = patternBase + (tile << 3) + y;
            for (int x = 0; x < spriteSize; ++x)
                if (IsSetBit(m_pVdpVRAM[address + (x >= 8 ? 16 : 0)], 7 - (x & 7)))
                    putPixel(item, x, y, colour);
        }
    }

    surface = {};
    surface.rgba = m_pSpriteDebugAtlasRGBA;
    surface.width = atlasWidth;
    surface.height = atlasHeight;
    surface.row_pitch = atlasWidth * 4;
    surface.items = m_SpriteDebugAtlasItems;
    surface.item_count = 32;
    surface.generation = ++m_SpriteDebugAtlasGeneration;
    return true;
}

bool TMS99xx::DescribeDebugSpriteParameter(const char* sourceCode,
    int& minimum, int& maximum) const
{
    if (!sourceCode || std::strcmp(sourceCode, "last-frame-scanline") != 0)
        return false;
    minimum = 0;
    maximum = m_LastSpriteSelectionHistory.rasterLines > 0
        ? m_LastSpriteSelectionHistory.rasterLines - 1
        : GC_LINES_PER_FRAME_PAL - 1;
    return true;
}

std::size_t TMS99xx::GetDebugSpriteSourceFieldCount(const char* sourceCode,
    int scanline) const
{
    if (!sourceCode)
        return 0;
    if (std::strcmp(sourceCode, "last-rendered-frame") == 0)
        return 1;
    if (std::strcmp(sourceCode, "current-scanline-fetch") == 0)
    {
        const SpritePipelineDebugSnapshot pipeline = GetSpritePipelineDebugSnapshot();
        for (const SpritePipelineLineDebug& line : pipeline.lines)
            if (line.valid && line.collisionX >= 0)
                return 5;
        return pipeline.activeTargetValid ? 4 : 3;
    }
    if (std::strcmp(sourceCode, "last-frame-scanline") == 0)
    {
        const SpriteSelectionFrameHistory& history = m_LastSpriteSelectionHistory;
        const bool collision = history.valid && scanline >= 0 &&
            scanline < history.rasterLines && (history.rows[scanline].flags & 0x20);
        return collision ? 2 : 1;
    }
    return 0;
}

bool TMS99xx::DescribeDebugSpriteSourceField(const char* sourceCode, int scanline,
    std::size_t fieldIndex,
    GC_VideoDebugGridCellFieldDescriptor& descriptor) const
{
    descriptor = {};
    if (!sourceCode || fieldIndex >= GetDebugSpriteSourceFieldCount(sourceCode, scanline))
        return false;
    if (std::strcmp(sourceCode, "last-rendered-frame") == 0)
    {
        descriptor.code = "frame";
        descriptor.label = m_LastRenderedSpriteDebugFrame.valid
            ? "Frame" : "No complete frame captured";
        descriptor.format = m_LastRenderedSpriteDebugFrame.valid
            ? GC_VideoDebugValueFormat::Decimal : GC_VideoDebugValueFormat::Text;
        descriptor.value = m_LastRenderedSpriteDebugFrame.frameSerial;
        descriptor.text = m_LastRenderedSpriteDebugFrame.valid ? nullptr : "";
        descriptor.width_bits = 64;
        return true;
    }
    if (std::strcmp(sourceCode, "current-scanline-fetch") == 0)
    {
        const SpritePipelineDebugSnapshot pipeline = GetSpritePipelineDebugSnapshot();
        switch (fieldIndex)
        {
            case 0: descriptor.code = "raster-y"; descriptor.label = "Raster";
                descriptor.value = pipeline.rasterLine; break;
            case 1: descriptor.code = "dot"; descriptor.label = "Dot";
                descriptor.value = pipeline.dot; break;
            case 2: descriptor.code = "slot"; descriptor.label = "Slot";
                descriptor.value = pipeline.slot; break;
            case 3: descriptor.code = "target-y"; descriptor.label = "Target";
                descriptor.value = pipeline.activeTargetLine; break;
            case 4:
                for (const SpritePipelineLineDebug& line : pipeline.lines)
                    if (line.valid && line.collisionX >= 0)
                    {
                        descriptor.code = "collision-x";
                        descriptor.label = "Collision X";
                        descriptor.format = GC_VideoDebugValueFormat::Hexadecimal;
                        descriptor.value = line.collisionX;
                        descriptor.width_bits = 8;
                        return true;
                    }
                return false;
            default: return false;
        }
        descriptor.format = GC_VideoDebugValueFormat::Decimal;
        descriptor.width_bits = 16;
        return true;
    }
    if (std::strcmp(sourceCode, "last-frame-scanline") == 0)
    {
        const SpriteSelectionFrameHistory& history = m_LastSpriteSelectionHistory;
        if (fieldIndex == 0)
        {
            descriptor.code = "frame";
            descriptor.label = history.valid ? "Frame" : "No recorded frame history";
            descriptor.format = history.valid
                ? GC_VideoDebugValueFormat::Decimal : GC_VideoDebugValueFormat::Text;
            descriptor.value = history.frameSerial;
            descriptor.text = history.valid ? nullptr : "";
            descriptor.width_bits = 64;
            return true;
        }
        descriptor.code = "collision-x";
        descriptor.label = "Collision X";
        descriptor.format = GC_VideoDebugValueFormat::Hexadecimal;
        descriptor.value = history.rows[scanline].collisionX;
        descriptor.width_bits = 8;
        return true;
    }
    return false;
}

std::size_t TMS99xx::GetDebugSpriteItemFieldCount(const char* sourceCode,
    int scanline, u32 spriteIndex) const
{
    if (!sourceCode || spriteIndex >= 32)
        return 0;
    if (std::strcmp(sourceCode, "live-sat") == 0)
        return 8;
    if (std::strcmp(sourceCode, "last-rendered-frame") == 0)
        return 8;
    if (std::strcmp(sourceCode, "current-scanline-fetch") == 0)
    {
        const SpritePipelineDebugSnapshot pipeline = GetSpritePipelineDebugSnapshot();
        const SpritePipelineLineDebug* line = nullptr;
        for (const SpritePipelineLineDebug& candidate : pipeline.lines)
        {
            if (!candidate.valid) continue;
            if (pipeline.activeTargetValid &&
                candidate.absoluteTargetLine == pipeline.activeAbsoluteTargetLine)
            { line = &candidate; break; }
            if (!line) line = &candidate;
        }
        if (!line) return 1;
        for (int lane = 0; lane < line->selectedCount && lane < 4; ++lane)
            if (line->selected[lane].satIndex == static_cast<int>(spriteIndex))
                return 12;
        if (line->overflowRecorded && line->overflowSprite == static_cast<int>(spriteIndex))
            return 9;
        return 1;
    }
    if (std::strcmp(sourceCode, "last-frame-scanline") == 0)
    {
        const SpriteSelectionFrameHistory& history = m_LastSpriteSelectionHistory;
        if (!history.valid || scanline < 0 || scanline >= history.rasterLines)
            return 1;
        const SpriteSelectionHistoryRow& row = history.rows[scanline];
        for (int lane = 0; lane < 5; ++lane)
        {
            if (row.rasterX[lane] == 0xFF) continue;
            const TMS99xxVramSlotSchedule::Slot slot =
                TMS99xxVramSlotSchedule::GetSlot(
                    TMS99xxVramSlotSchedule::Schedule::Graphics,
                    row.rasterX[lane]);
            if (slot.activity == TMS99xxVramSlotSchedule::Activity::SpriteScanY &&
                slot.index == static_cast<int>(spriteIndex))
                return lane == 4 ? 9 : 11;
        }
        return 1;
    }
    return 0;
}

bool TMS99xx::DescribeDebugSpriteItemField(const char* sourceCode, int scanline,
    u32 spriteIndex, std::size_t fieldIndex,
    GC_VideoDebugGridCellFieldDescriptor& descriptor) const
{
    descriptor = {};
    if (!sourceCode || spriteIndex >= 32 ||
        fieldIndex >= GetDebugSpriteItemFieldCount(sourceCode, scanline, spriteIndex))
        return false;

    const auto setNumber = [&](const char* code, const char* label,
                               GC_VideoDebugValueFormat format, u64 value,
                               u8 widthBits)
    {
        descriptor.code = code;
        descriptor.label = label;
        descriptor.format = format;
        descriptor.value = value;
        descriptor.width_bits = widthBits;
    };
    const auto setText = [&](const char* code, const char* label, const char* value)
    {
        descriptor.code = code;
        descriptor.label = label;
        descriptor.format = GC_VideoDebugValueFormat::Text;
        descriptor.text = value;
    };
    if (fieldIndex == 0)
    {
        setNumber("sat-index", "SAT Index", GC_VideoDebugValueFormat::Decimal,
            spriteIndex, 8);
        return true;
    }

    const bool renderedSource = std::strcmp(sourceCode, "last-rendered-frame") == 0;
    const bool pipelineSource = std::strcmp(sourceCode, "current-scanline-fetch") == 0;
    const bool historySource = std::strcmp(sourceCode, "last-frame-scanline") == 0;
    u8 x = 0, y = 0, tile = 0, colour = 0;
    u16 fetchedRows = 0;
    int laneIndex = -1;
    u8 rasterX = 0xFF;
    u8 pattern[2] = {0, 0};
    u8 latchMask = 0;
    bool overflow = false;
    bool hasPattern = false;

    if (renderedSource)
    {
        const RenderedSpriteDebugEntry& entry = m_LastRenderedSpriteDebugFrame.entries[spriteIndex];
        x = entry.x; y = entry.rawY; tile = entry.name; colour = entry.colour;
        fetchedRows = entry.fetchedRows;
    }
    else if (pipelineSource)
    {
        const SpritePipelineDebugSnapshot pipeline = GetSpritePipelineDebugSnapshot();
        const SpritePipelineLineDebug* line = nullptr;
        for (const SpritePipelineLineDebug& candidate : pipeline.lines)
        {
            if (!candidate.valid) continue;
            if (pipeline.activeTargetValid &&
                candidate.absoluteTargetLine == pipeline.activeAbsoluteTargetLine)
            { line = &candidate; break; }
            if (!line) line = &candidate;
        }
        if (!line) return false;
        for (int lane = 0; lane < line->selectedCount && lane < 4; ++lane)
            if (line->selected[lane].satIndex == static_cast<int>(spriteIndex))
            {
                const SpritePipelineLatchDebug& entry = line->selected[lane];
                laneIndex = lane; rasterX = entry.rasterX;
                x = entry.x; y = entry.y; tile = entry.name; colour = entry.colour;
                pattern[0] = entry.pattern[0]; pattern[1] = entry.pattern[1];
                hasPattern = true;
                latchMask = (entry.yValid ? 0x20 : 0) | (entry.xValid ? 0x10 : 0) |
                    (entry.nameValid ? 0x08 : 0) | (entry.colourValid ? 0x04 : 0) |
                    (entry.patternValid[0] ? 0x02 : 0) |
                    (entry.patternValid[1] ? 0x01 : 0);
                break;
            }
        if (laneIndex < 0 && line->overflowRecorded &&
            line->overflowSprite == static_cast<int>(spriteIndex))
        {
            overflow = true; laneIndex = 4; rasterX = line->overflowRasterX;
            if (line->overflowSatValid)
            { y = line->overflowSat[0]; x = line->overflowSat[1];
              tile = line->overflowSat[2]; colour = line->overflowSat[3]; }
        }
    }
    else if (historySource)
    {
        const SpriteSelectionFrameHistory& history = m_LastSpriteSelectionHistory;
        if (!history.valid || scanline < 0 || scanline >= history.rasterLines)
            return false;
        const SpriteSelectionHistoryRow& row = history.rows[scanline];
        for (int lane = 0; lane < 5; ++lane)
        {
            if (row.rasterX[lane] == 0xFF) continue;
            const TMS99xxVramSlotSchedule::Slot slot = TMS99xxVramSlotSchedule::GetSlot(
                TMS99xxVramSlotSchedule::Schedule::Graphics, row.rasterX[lane]);
            if (slot.activity == TMS99xxVramSlotSchedule::Activity::SpriteScanY &&
                slot.index == static_cast<int>(spriteIndex))
            {
                laneIndex = lane; overflow = lane == 4; rasterX = row.rasterX[lane];
                y = row.sat[lane][0]; x = row.sat[lane][1];
                tile = row.sat[lane][2]; colour = row.sat[lane][3];
                if (!overflow)
                { pattern[0] = row.pattern[lane][0]; pattern[1] = row.pattern[lane][1];
                  hasPattern = true; }
                break;
            }
        }
    }
    else
    {
        const u32 attribute = ((m_VdpRegister[5] & 0x7F) << 7) + (spriteIndex << 2);
        y = m_pVdpVRAM[attribute]; x = m_pVdpVRAM[attribute + 1];
        tile = m_pVdpVRAM[attribute + 2]; colour = m_pVdpVRAM[attribute + 3];
    }

    if (pipelineSource || historySource)
    {
        if (laneIndex < 0 && !overflow) return false;
        switch (fieldIndex)
        {
            case 1: setNumber("x", "X", GC_VideoDebugValueFormat::Hexadecimal, x, 8); break;
            case 2: setNumber("y", "Y", GC_VideoDebugValueFormat::Hexadecimal, y, 8); break;
            case 3: setNumber("tile", "Tile", GC_VideoDebugValueFormat::Hexadecimal, tile, 8); break;
            case 4: setNumber("colour", "Color", GC_VideoDebugValueFormat::Hexadecimal, colour & 0x0F, 8); break;
            case 5: setNumber("early-clock", "Early Clock", GC_VideoDebugValueFormat::Boolean, colour & 0x80, 1); break;
            case 6: setNumber("lane", "Lane", GC_VideoDebugValueFormat::Decimal, laneIndex, 8); break;
            case 7: setNumber("raster-x", "Raster X", GC_VideoDebugValueFormat::Decimal, rasterX, 8); break;
            case 8: setText("result", pipelineSource ? "Pipeline Result" : "Scanline Result",
                overflow ? "FIFTH / REJECTED" : "ACCEPTED"); break;
            case 9:
                if (pipelineSource) setNumber("latches", "Latches YXNC01", GC_VideoDebugValueFormat::Hexadecimal, latchMask, 6);
                else if (hasPattern) setNumber("pattern-0", "Pattern Byte 0", GC_VideoDebugValueFormat::Hexadecimal, pattern[0], 8);
                else return false;
                break;
            case 10:
                if (pipelineSource) setNumber("pattern-0", "Pattern Byte 0", GC_VideoDebugValueFormat::Hexadecimal, pattern[0], 8);
                else if (hasPattern) setNumber("pattern-1", "Pattern Byte 1", GC_VideoDebugValueFormat::Hexadecimal, pattern[1], 8);
                else return false;
                break;
            case 11:
                if (pipelineSource && hasPattern) setNumber("pattern-1", "Pattern Byte 1", GC_VideoDebugValueFormat::Hexadecimal, pattern[1], 8);
                else return false;
                break;
            default: return false;
        }
        return true;
    }

    if (renderedSource)
    {
        const RenderedSpriteDebugEntry& entry = m_LastRenderedSpriteDebugFrame.entries[spriteIndex];
        const u16 completeMask = m_LastRenderedSpriteDebugFrame.large ? 0xFFFFu : 0x00FFu;
        switch (fieldIndex)
        {
            case 1: setNumber("x", "X", GC_VideoDebugValueFormat::Hexadecimal, x, 8); break;
            case 2: setNumber("y", "Y", GC_VideoDebugValueFormat::Hexadecimal, y, 8); break;
            case 3: setNumber("tile", "Tile", GC_VideoDebugValueFormat::Hexadecimal, tile, 8); break;
            case 4: setNumber("colour", "Color", GC_VideoDebugValueFormat::Hexadecimal, colour & 0x0F, 8); break;
            case 5: setNumber("early-clock", "Early Clock", GC_VideoDebugValueFormat::Boolean, colour & 0x80, 1); break;
            case 6: setNumber("fetched-rows", "Fetched Rows", GC_VideoDebugValueFormat::Hexadecimal, fetchedRows, 16); break;
            case 7: setText("result", "Frame Result", !entry.seen ? "NOT FETCHED" :
                ((entry.fetchedRows & completeMask) == completeMask ? "COMPLETE" : "PARTIAL / OVERFLOW OR CLIPPED")); break;
            default: return false;
        }
        return true;
    }

    const u32 attribute = ((m_VdpRegister[5] & 0x7F) << 7) + (spriteIndex << 2);
    const u32 patternBase = (m_VdpRegister[6] & 0x07) << 11;
    switch (fieldIndex)
    {
        case 1: setNumber("attribute-address", "Attribute Addr", GC_VideoDebugValueFormat::Address, attribute, 14); break;
        case 2: setNumber("x", "X", GC_VideoDebugValueFormat::Hexadecimal, x, 8); break;
        case 3: setNumber("y", "Y", GC_VideoDebugValueFormat::Hexadecimal, y, 8); break;
        case 4: setNumber("tile", "Tile", GC_VideoDebugValueFormat::Hexadecimal, tile, 8); break;
        case 5: setNumber("tile-address", "Tile Addr", GC_VideoDebugValueFormat::Address, patternBase + (tile << 3), 14); break;
        case 6: setNumber("colour", "Color", GC_VideoDebugValueFormat::Hexadecimal, colour & 0x0F, 8); break;
        case 7: setNumber("early-clock", "Early Clock", GC_VideoDebugValueFormat::Boolean, colour & 0x80, 1); break;
        default: return false;
    }
    return true;
}

std::size_t TMS99xx::GetDebugSpriteAttributeTableCount() const
{
    return 1;
}

bool TMS99xx::DescribeDebugSpriteAttributeTable(std::size_t index,
    GC_VideoDebugSpriteAttributeTableDescriptor& descriptor) const
{
    if (index != 0)
        return false;
    descriptor = {};
    descriptor.code = "TMS99XX.SAT";
    descriptor.name = "SAT";
    descriptor.record_count = 32;
    descriptor.column_count = 4;
    descriptor.base_address = static_cast<u32>(m_VdpRegister[5] & 0x7F) << 7;
    descriptor.record_stride = 4;
    descriptor.address_width_bits = 14;
    return true;
}

bool TMS99xx::DescribeDebugSpriteAttributeColumn(std::size_t tableIndex,
    std::size_t columnIndex,
    GC_VideoDebugSpriteAttributeColumnDescriptor& descriptor) const
{
    static const GC_VideoDebugSpriteAttributeColumnDescriptor columns[] = {
        { "y", "Y", 8 },
        { "x", "X", 8 },
        { "pattern", "PAT", 8 },
        { "colour", "COL/EC", 8 }
    };
    if (tableIndex != 0 || columnIndex >= sizeof(columns) / sizeof(columns[0]))
        return false;
    descriptor = columns[columnIndex];
    return true;
}

bool TMS99xx::DescribeDebugSpriteAttributeRecord(std::size_t tableIndex,
    u32 recordIndex,
    GC_VideoDebugSpriteAttributeRecordDescriptor& descriptor) const
{
    GC_VideoDebugSpriteAttributeTableDescriptor table;
    if (!DescribeDebugSpriteAttributeTable(tableIndex, table) ||
        recordIndex >= table.record_count)
        return false;

    descriptor = {};
    descriptor.address = table.base_address + recordIndex * table.record_stride;
    descriptor.visible = true;
    for (u32 index = 0; index <= recordIndex; ++index)
    {
        const u32 address = table.base_address + index * table.record_stride;
        if (m_pVdpVRAM[address & 0x3FFF] == 0xD0)
        {
            descriptor.visible = false;
            break;
        }
    }
    if (descriptor.visible)
    {
        const u32 address = descriptor.address & 0x3FFF;
        int y = (m_pVdpVRAM[address] + 1) & 0xFF;
        if (y >= 0xE0)
            y -= 0x100;
        int x = m_pVdpVRAM[(address + 1) & 0x3FFF];
        const u8 colour = m_pVdpVRAM[(address + 3) & 0x3FFF];
        if ((colour & 0x0F) == 0)
        {
            descriptor.visible = false;
            return true;
        }
        if (colour & 0x80)
            x -= 32;
        const int size = (IsSetBit(m_VdpRegister[1], 1) ? 16 : 8) *
            (IsSetBit(m_VdpRegister[1], 0) ? 2 : 1);
        descriptor.visible = x < 256 && x + size > 0 &&
            y < 192 && y + size > 0;
    }
    return true;
}

bool TMS99xx::ReadDebugSpriteAttributeValue(std::size_t tableIndex,
    u32 recordIndex, u32 columnIndex, u64& value) const
{
    GC_VideoDebugSpriteAttributeTableDescriptor table;
    if (!DescribeDebugSpriteAttributeTable(tableIndex, table) ||
        recordIndex >= table.record_count || columnIndex >= table.column_count)
        return false;
    value = m_pVdpVRAM[(table.base_address + recordIndex * table.record_stride +
        columnIndex) & 0x3FFF];
    return true;
}

bool TMS99xx::DescribeDebugLayer(std::size_t index,
    GC_VideoDebugLayerDescriptor& descriptor) const
{
    if (index > 2)
        return false;

    if (index == 2)
    {
        descriptor = {};
        descriptor.code = "TMS99XX.SPRPAT";
        descriptor.name = "Sprite Patterns";
        descriptor.kind = GC_VideoDebugLayerKind::TileCatalog;
        descriptor.blend_mode = GC_VideoDebugBlendMode::Opaque;
        descriptor.visible = true;
        descriptor.has_grid = true;
        descriptor.content_width = 256;
        descriptor.content_height = 64;
        descriptor.columns = 32;
        descriptor.rows = 8;
        descriptor.active_cell_count = 256;
        descriptor.cell_width = 8;
        descriptor.cell_height = 8;
        descriptor.primary_address_label = "Sprite Pattern Table";
        descriptor.primary_address = static_cast<u32>(m_VdpRegister[6] & 0x07) << 11;
        descriptor.address_width_bits = 14;
        return true;
    }

    if (index == 1)
    {
        const bool graphicsII = m_iMode == 2 || m_iMode == 3 || m_iMode == 6;
        const u32 activeTiles = graphicsII ? 768 : 256;
        descriptor = {};
        descriptor.code = "TMS99XX.PATTERNS";
        descriptor.name = "Background Patterns";
        descriptor.kind = GC_VideoDebugLayerKind::TileCatalog;
        descriptor.blend_mode = GC_VideoDebugBlendMode::Opaque;
        descriptor.visible = true;
        descriptor.has_grid = true;
        descriptor.content_width = 256;
        descriptor.content_height = ((activeTiles + 31) / 32) * 8;
        descriptor.columns = 32;
        descriptor.rows = (activeTiles + 31) / 32;
        descriptor.active_cell_count = activeTiles;
        descriptor.cell_width = 8;
        descriptor.cell_height = 8;
        descriptor.rows_per_region = graphicsII ? 8 : 0;
        descriptor.primary_address_label = "Pattern Table";
        descriptor.primary_address = static_cast<u32>(m_VdpRegister[4] &
            (graphicsII ? 0x04 : 0x07)) << 11;
        descriptor.address_width_bits = 14;
        return true;
    }

    // The emulator renders the two banked undocumented variants, but their
    // Name Table debug mapping has not been detached and verified yet. Do not
    // expose a plausible-looking but wrong generic grid.
    if (index == 0 && (m_iMode == 3 || m_iMode == 6))
        return false;

    const bool textMode = m_iMode == 1;
    descriptor = {};
    descriptor.code = "TMS99XX.NAMETABLE";
    descriptor.name = "Name Table";
    descriptor.kind = GC_VideoDebugLayerKind::TileGrid;
    descriptor.blend_mode = GC_VideoDebugBlendMode::Opaque;
    descriptor.z_order = 0;
    descriptor.visible = true;
    descriptor.has_grid = true;
    descriptor.content_width = textMode ? 240 : 256;
    descriptor.content_height = 192;
    descriptor.presentation_scale_x = 1;
    descriptor.presentation_scale_y = 1;
    // Legacy TMS99xx modes have no hardware scrolling, so there is no
    // meaningful movable viewport to overlay on this complete map.
    descriptor.has_viewport = false;
    descriptor.columns = textMode ? 40 : 32;
    descriptor.rows = 24;
    descriptor.active_cell_count = descriptor.columns * descriptor.rows;
    descriptor.cell_width = textMode ? 6 : 8;
    descriptor.cell_height = 8;
    descriptor.rows_per_region = (m_iMode == 2 || m_iMode == 3 || m_iMode == 6) ? 8 : 0;
    descriptor.primary_address_label = "Name Table Base";
    descriptor.primary_address = static_cast<u32>(m_VdpRegister[2]) << 10;
    descriptor.address_width_bits = 14;
    return true;
}

void TMS99xx::BuildPatternTableDebugSurface() const
{
    if (!m_pPatternTableDebugRGBA || !m_PatternTableDebugDirty)
        return;

    const bool graphicsII = m_iMode == 2 || m_iMode == 3 || m_iMode == 6;
    const bool textMode = m_iMode == 1 || m_iMode == 3;
    const bool multicolorMode = m_iMode == 4 || m_iMode == 6;
    const u32 patternTableAddress = static_cast<u32>(m_VdpRegister[4] &
        (graphicsII ? 0x04 : 0x07)) << 11;
    const u32 colourTableAddress = graphicsII
        ? static_cast<u32>(m_VdpRegister[3] & 0x80) << 6
        : static_cast<u32>(m_VdpRegister[3]) << 6;
    const int activeTiles = graphicsII ? 768 : 256;
    int backdrop = m_VdpRegister[7] & 0x0F;
    backdrop = backdrop ? backdrop : 1;
    int textForeground = (m_VdpRegister[7] >> 4) & 0x0F;
    textForeground = textForeground ? textForeground : backdrop;

    const auto putPixel = [this](u8* destination, int x, int y, int colour)
    {
        u8* rgba = &destination[(y * 256 + x) * 4];
        const u8* rgb = &m_pCurrentPalette[(colour & 0x0F) * 3];
        rgba[0] = rgb[0];
        rgba[1] = rgb[1];
        rgba[2] = rgb[2];
        rgba[3] = 0xFF;
    };

    // Inactive catalogue slots are deliberately not decoded from unrelated
    // VRAM. Keep any unused capacity near-black but distinguishable from a
    // real black pixel emitted by the VDP.
    for (int pixel = 0; pixel < 256 * 256; ++pixel)
    {
        u8* rgba = &m_pPatternTableDebugRGBA[pixel * 4];
        rgba[0] = 4;
        rgba[1] = 4;
        rgba[2] = 4;
        rgba[3] = 0xFF;
    }

    for (int tile = 0; tile < activeTiles; ++tile)
    {
        const int tileX = (tile & 31) * 8;
        const int tileY = (tile >> 5) * 8;
        for (int line = 0; line < 8; ++line)
        {
            const u32 patternAddress = (patternTableAddress + (tile << 3) + line) & 0x3FFF;
            const u8 pattern = m_pVdpVRAM[patternAddress];
            for (int pixel = 0; pixel < 8; ++pixel)
            {
                int foreground = 15;
                int background = 0;
                if (textMode)
                {
                    foreground = textForeground;
                    background = backdrop;
                }
                else if (multicolorMode)
                {
                    const u32 colourAddress = (patternTableAddress + (tile << 3) +
                        ((line >> 2) << 1)) & 0x3FFF;
                    const u8 colours = m_pVdpVRAM[colourAddress];
                    foreground = pixel < 4 ? (colours >> 4) : (colours & 0x0F);
                    background = foreground;
                }
                else
                {
                    const u32 colourAddress = graphicsII
                        ? (colourTableAddress + (tile << 3) + line) & 0x3FFF
                        : (colourTableAddress + (tile >> 3)) & 0x3FFF;
                    const u8 colours = m_pVdpVRAM[colourAddress];
                    foreground = colours >> 4;
                    background = colours & 0x0F;
                    foreground = foreground ? foreground : backdrop;
                    background = background ? background : backdrop;
                }
                putPixel(m_pPatternTableDebugRGBA, tileX + pixel, tileY + line,
                    multicolorMode ? foreground : (IsSetBit(pattern, 7 - pixel) ? foreground : background));
            }
        }
    }

    ++m_PatternTableDebugGeneration;
    m_PatternTableDebugDirty = false;
}

void TMS99xx::BuildSpritePatternDebugSurface(u64 optionMask) const
{
    if (!m_pSpritePatternDebugRGBA ||
        (!m_SpritePatternDebugDirty && m_SpritePatternDebugBuiltOptionMask == optionMask))
        return;

    const bool showColours = (optionMask & 1ull) != 0;
    const u32 patternBase = static_cast<u32>(m_VdpRegister[6] & 0x07) << 11;
    u8 tileColours[256] = {};
    if (showColours)
    {
        const u32 attributeBase = static_cast<u32>(m_VdpRegister[5] & 0x7F) << 7;
        const u32 patternsPerSprite = IsSetBit(m_VdpRegister[1], 1) ? 4 : 1;
        for (u32 sprite = 0; sprite < 32; ++sprite)
        {
            const u32 attribute = (attributeBase + sprite * 4) & 0x3FFF;
            if (m_pVdpVRAM[attribute] == 0xD0)
                break;
            const u8 colour = m_pVdpVRAM[(attribute + 3) & 0x3FFF] & 0x0F;
            if (colour == 0)
                continue;
            u32 tile = m_pVdpVRAM[(attribute + 2) & 0x3FFF];
            if (patternsPerSprite == 4)
                tile &= 0xFC;
            for (u32 part = 0; part < patternsPerSprite; ++part)
                if (tileColours[(tile + part) & 0xFF] == 0)
                    tileColours[(tile + part) & 0xFF] = colour;
        }
    }
    for (u32 tile = 0; tile < 256; ++tile)
    {
        const u32 tileX = (tile & 31) * 8;
        const u32 tileY = (tile >> 5) * 8;
        for (u32 line = 0; line < 8; ++line)
        {
            const u8 pattern = m_pVdpVRAM[(patternBase + (tile << 3) + line) & 0x3FFF];
            for (u32 pixel = 0; pixel < 8; ++pixel)
            {
                u8* rgba = &m_pSpritePatternDebugRGBA[
                    ((tileY + line) * 256 + tileX + pixel) * 4];
                const bool set = IsSetBit(pattern, 7 - pixel);
                if (set && tileColours[tile] != 0)
                {
                    const u8* rgb = &m_pCurrentPalette[tileColours[tile] * 3];
                    rgba[0] = rgb[0];
                    rgba[1] = rgb[1];
                    rgba[2] = rgb[2];
                }
                else
                {
                    const u8 intensity = set ? 0xFF : 0x00;
                    rgba[0] = intensity;
                    rgba[1] = intensity;
                    rgba[2] = intensity;
                }
                rgba[3] = 0xFF;
            }
        }
    }

    ++m_SpritePatternDebugGeneration;
    m_SpritePatternDebugDirty = false;
    m_SpritePatternDebugBuiltOptionMask = optionMask;
}

void TMS99xx::BuildNameTableDebugSurface() const
{
    if (!m_pNameTableDebugRGBA || !m_NameTableDebugDirty)
        return;

    // The backing texture remains 256x256 for now, while the scene exposes
    // the active 256x192 canvas. This keeps the existing renderer allocation
    // stable during the first extraction.
    for (int pixel = 0; pixel < 256 * 256; ++pixel)
    {
        u8* rgba = &m_pNameTableDebugRGBA[pixel * 4];
        rgba[0] = 0;
        rgba[1] = 0;
        rgba[2] = 0;
        rgba[3] = 0xFF;
    }

    const int nameTableAddress = m_VdpRegister[2] << 10;
    const int colourTableAddress = m_VdpRegister[3] << 6;
    const int patternTableAddress = m_VdpRegister[4] << 11;
    int backdrop = m_VdpRegister[7] & 0x0F;
    backdrop = backdrop ? backdrop : 1;

    const auto putPixel = [this](u8* surface, int x, int y, int colour)
    {
        u8* rgba = &surface[(y * 256 + x) * 4];
        const u8* rgb = &m_pCurrentPalette[(colour & 0x0F) * 3];
        rgba[0] = rgb[0];
        rgba[1] = rgb[1];
        rgba[2] = rgb[2];
        rgba[3] = 0xFF;
    };

    if (m_iMode == 1)
    {
        int foreground = (m_VdpRegister[7] >> 4) & 0x0F;
        foreground = foreground ? foreground : backdrop;
        for (int row = 0; row < 24; ++row)
        {
            for (int column = 0; column < 40; ++column)
            {
                const int tile = m_pVdpVRAM[(nameTableAddress + row * 40 + column) & 0x3FFF];
                for (int line = 0; line < 8; ++line)
                {
                    const u8 pattern = m_pVdpVRAM[(patternTableAddress + (tile << 3) + line) & 0x3FFF];
                    for (int pixel = 0; pixel < 6; ++pixel)
                        putPixel(m_pNameTableDebugRGBA, column * 6 + pixel, row * 8 + line,
                                 IsSetBit(pattern, 7 - pixel) ? foreground : backdrop);
                }
            }
        }
    }
    else
    {
        // Multicolor keeps the whole three-bit R4 base (only M3 banking masks
        // it); Graphics II addresses come from TMS99xxTableAddress.
        for (int row = 0; row < 24; ++row)
        {
            for (int column = 0; column < 32; ++column)
            {
                const int rawTile = m_pVdpVRAM[(nameTableAddress + row * 32 + column) & 0x3FFF];
                for (int line = 0; line < 8; ++line)
                {
                    if (m_iMode == 4)
                    {
                        const int address = (patternTableAddress + (rawTile << 3) +
                            ((row & 0x03) << 1) + ((line & 0x04) ? 1 : 0)) & 0x3FFF;
                        const u8 colours = m_pVdpVRAM[address];
                        int left = colours >> 4;
                        int right = colours & 0x0F;
                        left = left ? left : backdrop;
                        right = right ? right : backdrop;
                        for (int pixel = 0; pixel < 4; ++pixel)
                            putPixel(m_pNameTableDebugRGBA, column * 8 + pixel, row * 8 + line, left);
                        for (int pixel = 4; pixel < 8; ++pixel)
                            putPixel(m_pNameTableDebugRGBA, column * 8 + pixel, row * 8 + line, right);
                        continue;
                    }

                    int tile = rawTile;
                    u8 pattern = 0;
                    u8 colours = 0;
                    if (m_iMode == 0)
                    {
                        pattern = m_pVdpVRAM[(patternTableAddress + (tile << 3) + line) & 0x3FFF];
                        colours = m_pVdpVRAM[(colourTableAddress + (tile >> 3)) & 0x3FFF];
                    }
                    else if (m_iMode == 2)
                    {
                        const u16 rasterLine = static_cast<u16>(row * 8 + line);
                        pattern = m_pVdpVRAM[TMS99xxTableAddress::Pattern(
                            2, m_VdpRegister[3], m_VdpRegister[4], rasterLine, static_cast<u8>(tile))];
                        colours = m_pVdpVRAM[TMS99xxTableAddress::Colour(
                            2, m_VdpRegister[3], rasterLine, static_cast<u8>(tile))];
                    }
                    int foreground = colours >> 4;
                    int background = colours & 0x0F;
                    foreground = foreground ? foreground : backdrop;
                    background = background ? background : backdrop;
                    for (int pixel = 0; pixel < 8; ++pixel)
                        putPixel(m_pNameTableDebugRGBA, column * 8 + pixel, row * 8 + line,
                                 IsSetBit(pattern, 7 - pixel) ? foreground : background);
                }
            }
        }
    }
    ++m_NameTableDebugGeneration;
    m_NameTableDebugDirty = false;
}

bool TMS99xx::ReadDebugLayerSurface(std::size_t index,
    GC_VideoDebugLayerSurface& surface) const
{
    return ReadDebugLayerSurfaceWithOptions(index, index == 2 ? 1ull : 0ull, surface);
}

std::size_t TMS99xx::GetDebugLayerOptionCount(std::size_t index) const
{
    return index == 2 ? 1 : 0;
}

bool TMS99xx::DescribeDebugLayerOption(std::size_t layerIndex,
    std::size_t optionIndex, GC_VideoDebugLayerOptionDescriptor& descriptor) const
{
    if (layerIndex != 2 || optionIndex != 0)
        return false;
    descriptor = {};
    descriptor.code = "TMS99XX.SPRPAT.SHOW_COLOURS";
    descriptor.name = "Show colors";
    descriptor.enabled = true;
    return true;
}

bool TMS99xx::ReadDebugLayerSurfaceWithOptions(std::size_t index, u64 optionMask,
    GC_VideoDebugLayerSurface& surface) const
{
    if (index == 2)
    {
        if (!m_pSpritePatternDebugRGBA)
            return false;
        BuildSpritePatternDebugSurface(optionMask);
        surface = {};
        surface.rgba = m_pSpritePatternDebugRGBA;
        surface.width = 256;
        surface.height = 64;
        surface.row_pitch = 256 * 4;
        surface.generation = m_SpritePatternDebugGeneration;
        return true;
    }
    if (index == 1)
    {
        if (!m_pPatternTableDebugRGBA)
            return false;
        BuildPatternTableDebugSurface();
        surface = {};
        surface.rgba = m_pPatternTableDebugRGBA;
        surface.width = 256;
        surface.height = (m_iMode == 2 || m_iMode == 3 || m_iMode == 6) ? 192 : 64;
        surface.row_pitch = 256 * 4;
        surface.generation = m_PatternTableDebugGeneration;
        return true;
    }
    if (index != 0 || !m_pNameTableDebugRGBA)
        return false;
    BuildNameTableDebugSurface();
    surface = {};
    surface.rgba = m_pNameTableDebugRGBA;
    surface.width = 256;
    surface.height = 256;
    surface.row_pitch = 256 * 4;
    surface.generation = m_NameTableDebugGeneration;
    return true;
}

std::size_t TMS99xx::GetDebugLayerAreaCount(std::size_t index) const
{
    if (index == 2)
        return 1;
    if (index != 1)
        return 0;
    // Text gets its colours directly from R7 and Multicolor embeds them in
    // pattern bytes. The remaining tile modes have a dedicated Colour Table.
    return (m_iMode == 1 || m_iMode == 3 || m_iMode == 4 || m_iMode == 6) ? 1 : 2;
}

bool TMS99xx::DescribeDebugLayerArea(std::size_t index, std::size_t areaIndex,
    GC_VideoDebugLayerAreaDescriptor& descriptor) const
{
    if ((index != 1 && index != 2) || areaIndex >= GetDebugLayerAreaCount(index))
        return false;

    descriptor = {};
    if (index == 2)
    {
        descriptor.code = "SPRPAT";
        descriptor.label = "Sprite Pattern Table";
        descriptor.address = static_cast<u32>(m_VdpRegister[6] & 0x07) << 11;
        descriptor.size = 0x0800;
        descriptor.address_width_bits = 14;
        return true;
    }

    const bool graphicsII = m_iMode == 2 || m_iMode == 3 || m_iMode == 6;
    if (areaIndex == 0)
    {
        descriptor.code = "PAT";
        descriptor.label = "Pattern Table";
        descriptor.address = static_cast<u32>(m_VdpRegister[4] &
            (graphicsII ? 0x04 : 0x07)) << 11;
        descriptor.size = graphicsII ? 0x1800 : 0x0800;
        descriptor.address_width_bits = 14;
        return true;
    }

    descriptor.code = "COL";
    descriptor.label = "Colour Table";
    descriptor.address = graphicsII
        ? static_cast<u32>(m_VdpRegister[3] & 0x80) << 6
        : static_cast<u32>(m_VdpRegister[3]) << 6;
    descriptor.size = graphicsII ? 0x1800 : 0x20;
    descriptor.address_width_bits = 14;
    return true;
}

bool TMS99xx::DescribeDebugGridCell(std::size_t index, u32 column, u32 row,
    GC_VideoDebugGridCell& cell) const
{
    GC_VideoDebugLayerDescriptor layer;
    if (!DescribeDebugLayer(index, layer) ||
        column >= layer.columns || row >= layer.rows)
        return false;

    cell = {};
    cell.column = column;
    cell.row = row;
    if (index == 1 || index == 2)
    {
        const u32 tile = row * layer.columns + column;
        cell.valid = tile < layer.active_cell_count;
        cell.navigation_address = (layer.primary_address + (tile << 3)) & 0x3FFF;
        return true;
    }
    if (index != 0)
        return false;
    cell.valid = true;
    cell.navigation_address = (layer.primary_address + row * layer.columns + column) & 0x3FFF;
    return true;
}

std::size_t TMS99xx::GetDebugGridCellFieldCount(std::size_t index,
    u32 column, u32 row) const
{
    GC_VideoDebugGridCell cell;
    if (!DescribeDebugGridCell(index, column, row, cell) || !cell.valid)
        return 0;
    if (index == 2)
        return 2;
    if (index == 1)
    {
        if (m_iMode == 2)
            return 10;
        if (m_iMode == 4 || m_iMode == 6)
            return 2;
        return 3;
    }
    // Text uses global R7 colours, so there is no per-cell colour address.
    return m_iMode == 1 ? 3 : 4;
}

bool TMS99xx::DescribeDebugGridCellField(std::size_t index, u32 column, u32 row,
    std::size_t fieldIndex, GC_VideoDebugGridCellFieldDescriptor& descriptor) const
{
    GC_VideoDebugGridCell cell;
    if (!DescribeDebugGridCell(index, column, row, cell) || !cell.valid ||
        fieldIndex >= GetDebugGridCellFieldCount(index, column, row))
        return false;

    if (index == 2)
    {
        const u32 tile = row * 32 + column;
        const u32 patternBase = static_cast<u32>(m_VdpRegister[6] & 0x07) << 11;
        descriptor = {};
        if (fieldIndex == 0)
        {
            descriptor.code = "TMS99XX.SPRPAT.TILE";
            descriptor.label = "Tile Number";
            descriptor.format = GC_VideoDebugValueFormat::Hexadecimal;
            descriptor.value = tile;
            descriptor.width_bits = 8;
            return true;
        }
        descriptor.code = "TMS99XX.SPRPAT.ADDRESS";
        descriptor.label = "SPRPAT";
        descriptor.format = GC_VideoDebugValueFormat::Address;
        descriptor.value = (patternBase + (tile << 3)) & 0x3FFF;
        descriptor.width_bits = 14;
        return true;
    }

    if (index == 1)
    {
        const bool graphicsII = m_iMode == 2 || m_iMode == 3 || m_iMode == 6;
        const u32 tile = row * 32 + column;
        const u32 patternBase = static_cast<u32>(m_VdpRegister[4] &
            (graphicsII ? 0x04 : 0x07)) << 11;
        const u32 colourBase = graphicsII
            ? static_cast<u32>(m_VdpRegister[3] & 0x80) << 6
            : static_cast<u32>(m_VdpRegister[3]) << 6;
        descriptor = {};
        if (m_iMode == 2 && fieldIndex >= 2 && fieldIndex < 10)
        {
            const u32 line = static_cast<u32>(fieldIndex - 2);
            const u32 address = (colourBase + (tile << 3) + line) & 0x3FFF;
            descriptor.code = "TMS99XX.PATTERNS.COL";
            descriptor.label = "COL";
            descriptor.format = GC_VideoDebugValueFormat::Hexadecimal;
            descriptor.value = m_pVdpVRAM[address];
            descriptor.width_bits = 8;
            descriptor.has_source_address = true;
            descriptor.source_address = address;
            descriptor.source_address_width_bits = 14;
            return true;
        }
        switch (fieldIndex)
        {
            case 0:
                descriptor.code = "TMS99XX.PATTERNS.TILE";
                descriptor.label = "Tile Number";
                descriptor.format = GC_VideoDebugValueFormat::Hexadecimal;
                descriptor.value = tile;
                descriptor.width_bits = 10;
                return true;
            case 1:
                descriptor.code = "TMS99XX.PATTERNS.PATADDR";
                descriptor.label = "PATADDR";
                descriptor.format = GC_VideoDebugValueFormat::Address;
                descriptor.value = (patternBase + (tile << 3)) & 0x3FFF;
                descriptor.width_bits = 14;
                return true;
            case 2:
                if (m_iMode == 0)
                {
                    const u32 address = (colourBase + (tile >> 3)) & 0x3FFF;
                    descriptor.code = "TMS99XX.PATTERNS.COL";
                    descriptor.label = "COL";
                    descriptor.format = GC_VideoDebugValueFormat::Hexadecimal;
                    descriptor.value = m_pVdpVRAM[address];
                    descriptor.width_bits = 8;
                    descriptor.has_source_address = true;
                    descriptor.source_address = address;
                    descriptor.source_address_width_bits = 14;
                }
                else if (m_iMode == 1 || m_iMode == 3)
                {
                    descriptor.code = "TMS99XX.PATTERNS.R7";
                    descriptor.label = "R7";
                    descriptor.format = GC_VideoDebugValueFormat::Hexadecimal;
                    descriptor.value = m_VdpRegister[7];
                    descriptor.width_bits = 8;
                }
                else
                    return false;
                return true;
            default:
                return false;
        }
    }

    const int rawTile = m_pVdpVRAM[cell.navigation_address];
    int tile = rawTile;
    if (m_iMode == 2)
        tile += (row & 0x18) << 5;

    // Addresses of the cell's first raster line, from the shared generator.
    const u16 cellLine = static_cast<u16>(row * 8);
    const u32 patternAddress = (TMS99xxTableAddress::Pattern(
            m_iMode, m_VdpRegister[3], m_VdpRegister[4], cellLine, static_cast<u8>(rawTile)) -
        TMS99xxTableAddress::PatternRow(m_iMode, cellLine)) & 0x3FFF;
    u32 colourAddress = 0;
    if (m_iMode == 0 || m_iMode == 2)
        colourAddress = TMS99xxTableAddress::Colour(
            m_iMode, m_VdpRegister[3], cellLine, static_cast<u8>(rawTile));
    else if (m_iMode == 4)
        colourAddress = TMS99xxTableAddress::Pattern(
            m_iMode, m_VdpRegister[3], m_VdpRegister[4], cellLine, static_cast<u8>(rawTile));

    descriptor = {};
    switch (fieldIndex)
    {
        case 0:
            descriptor.code = "TMS99XX.NAMETABLE.NAME_ADDRESS";
            descriptor.label = "Name Address";
            descriptor.format = GC_VideoDebugValueFormat::Address;
            descriptor.value = cell.navigation_address;
            descriptor.width_bits = 14;
            return true;
        case 1:
            descriptor.code = "TMS99XX.NAMETABLE.TILE";
            descriptor.label = "Tile Number";
            descriptor.format = GC_VideoDebugValueFormat::Hexadecimal;
            descriptor.value = static_cast<u32>(tile);
            descriptor.width_bits = 10;
            return true;
        case 2:
            descriptor.code = "TMS99XX.NAMETABLE.PATTERN_ADDRESS";
            descriptor.label = "Tile Address";
            descriptor.format = GC_VideoDebugValueFormat::Address;
            descriptor.value = patternAddress;
            descriptor.width_bits = 14;
            return true;
        case 3:
            descriptor.code = "TMS99XX.NAMETABLE.COLOUR_ADDRESS";
            descriptor.label = "Color Address";
            descriptor.format = GC_VideoDebugValueFormat::Address;
            descriptor.value = colourAddress;
            descriptor.width_bits = 14;
            return true;
        default:
            return false;
    }
}

u8 TMS99xx::GetBufferReg()
{
    return m_VdpBuffer;
}

u8 TMS99xx::GetControlLatchValue() const
{
    return m_ControlLatchValue;
}

u16 TMS99xx::GetAddressReg()
{
    return m_VdpAddress;
}

u8 TMS99xx::GetStatusReg()
{
    return m_VdpStatus;
}

int TMS99xx::GetRenderLine() const
{
    return m_iRenderLine;
}

TMS99xxRasterTiming::DotPosition TMS99xx::GetRasterDotPosition() const
{
    return TMS99xxRasterTiming::GetDotPosition(
        static_cast<std::uint16_t>(m_iCycleCounter));
}

TMS99xxRasterTiming::SignalOutput TMS99xx::GetRasterSignalOutput() const
{
    return m_device == SystemVideoDevice::TMS9918 ||
           m_device == SystemVideoDevice::TMS9918B
        ? TMS99xxRasterTiming::SignalOutput::Composite9918
        : TMS99xxRasterTiming::SignalOutput::Component9928_9929;
}

TMS99xxCpuAccessTiming::Window TMS99xx::GetCpuVRAMAccessWindow() const
{
    return TMS99xxCpuAccessTiming::GetCpuWindow(GetCpuVRAMAccessState());
}

TMS99xxVramSequencer::Snapshot TMS99xx::GetVRAMSequencerSnapshot() const
{
    return m_VramSequencer.GetSnapshot();
}

TMS99xx::FrameRenderDiagnostics TMS99xx::GetFrameRenderDiagnostics() const
{
    return m_FrameRenderDiagnostics;
}

TMS99xxCpuAccessTiming::State TMS99xx::GetCpuVRAMAccessState() const
{
    TMS99xxCpuAccessTiming::State state{};
    state.phase = static_cast<std::uint16_t>(m_iCycleCounter);
    state.activeDisplayLine = m_iRenderLine < GC_RESOLUTION_HEIGHT;
    // Arbitration follows the live Display Enable bit. Rendering keeps its
    // separately latched line event, but a CPU-port request issued after R1
    // changes must see the new internal VRAM bandwidth immediately.
    state.displayEnabled = IsSetBit(m_VdpRegister[1], 6);

    switch (m_iMode)
    {
        case 1:
        case 3: // Text with Graphics II banking
        case 5: // bar pattern
        case 7: // bar pattern
            // vdp18_cpuio.vhd decodes every illegal combination to Text, so
            // the bar patterns borrow that calendar. Whether the real chip
            // frees those slots when it consults no table is unmeasured.
            state.mode = TMS99xxCpuAccessTiming::Mode::Text;
            break;
        case 4:
        case 6: // Multicolor with Graphics II banking
            state.mode = TMS99xxCpuAccessTiming::Mode::Multicolor;
            break;
        default:
            // Modes 0 (Graphics I) and 2 (Graphics II) currently use the
            // same compatibility slot calendar. The published table exposes
            // a per-cell Colour fetch even though Graphics I reuses each
            // colour-table byte for eight patterns; do not infer extra CPU
            // slots from that sharing without a hardware measurement.
            state.mode = TMS99xxCpuAccessTiming::Mode::Graphics;
            break;
    }

    return state;
}

TMS99xxVramSequencer::Schedule TMS99xx::GetVideoVRAMSchedule() const
{
    switch (m_iMode)
    {
        case 1:
        case 3:
        case 5:
        case 7:
            return TMS99xxVramSequencer::Schedule::Text;
        case 4:
        case 6:
            return TMS99xxVramSequencer::Schedule::Multicolor;
        default:
            return TMS99xxVramSequencer::Schedule::Graphics;
    }
}

int TMS99xx::GetCycleCounter()
{
    return m_iCycleCounter;
}

bool TMS99xx::GetLatch()
{
    return m_bFirstByteInSequence;
}

u16 TMS99xx::GetVRAMPtr()
{
    return m_VdpAddress;
}

void TMS99xx::SetAddressCounter(u16 value, DebugEventSource source)
{
    value &= 0x3FFF;
    const u16 before = m_VdpAddress;
    m_VdpAddress = value;
    if (before != value)
        m_pMemory->CheckVDPStateChange(DebugAddressCounter, before, value, source);
}

void TMS99xx::SetControlLatch(bool firstByte, DebugEventSource source)
{
    const bool before = m_bFirstByteInSequence;
    m_bFirstByteInSequence = firstByte;
    if (before != firstByte)
        m_pMemory->CheckVDPStateChange(DebugControlLatch,
            before ? 1 : 0, firstByte ? 1 : 0, source);
}

void TMS99xx::SetControlLatchValue(u8 value, DebugEventSource source)
{
    const u8 before = m_ControlLatchValue;
    m_ControlLatchValue = value;
    if (before != value)
        m_pMemory->CheckVDPStateChange(DebugControlLatchValue,
            before, value, source);
}

void TMS99xx::SetReadBuffer(u8 value, DebugEventSource source)
{
    const u8 before = m_VdpBuffer;
    m_VdpBuffer = value;
    if (before != value)
        m_pMemory->CheckVDPStateChange(DebugReadBuffer, before, value, source);
}

void TMS99xx::QueueVRAMTransfer(uint64_t tstates,
                              const TMS99xxVramSequencer::Request& request)
{
    const DebugEventRasterContext context = BuildPortAccessContext(tstates);
    m_LastCpuPortAccessTStates = tstates;
    m_HasLastCpuPortAccess = true;

    TMS99xxVramSequencer::LatchReplacement replacement;
    const bool accepted = m_VramSequencer.QueueRequest(request, &replacement);
    (void)accepted;
    if (replacement.occurred)
    {
        m_pMemory->CheckVDPStateChange(DebugCpuPortLatchReplacement,
            PackCpuPortRequest(replacement.discardedRequest),
            PackCpuPortRequest(replacement.replacementRequest),
            DebugEventSource::DeviceInternal, &context);
    }
}

DebugEventRasterContext TMS99xx::BuildPortAccessContext(uint64_t tstates) const
{
    using Timing = TMS99xxRasterTiming;

    DebugEventRasterContext context;
    context.rasterKnown = true;
    context.line = static_cast<u16>(m_iRenderLine);
    context.dot = static_cast<u16>(m_iCycleCounter / Timing::PhasesPerDot);

    switch (Timing::GetVerticalRegion(context.line, m_bPAL))
    {
        case Timing::VerticalRegion::Active:
            context.verticalRegion = DebugRasterRegion::ActiveDisplay;
            break;
        case Timing::VerticalRegion::TopBorder:
        case Timing::VerticalRegion::BottomBorder:
            context.verticalRegion = DebugRasterRegion::Border;
            break;
        case Timing::VerticalRegion::VerticalSync:
            context.verticalRegion = DebugRasterRegion::Sync;
            break;
        default:
            context.verticalRegion = DebugRasterRegion::Blanking;
            break;
    }

    // Text narrows the active window to 240 dots. The framebuffer path
    // deliberately always reads against the Graphics width, but here the
    // point is to say where the beam really is, so follow the mode.
    const bool text = m_iMode == 1 || m_iMode == 3;
    const Timing::DisplayWidth width = text ? Timing::DisplayWidth::Text
                                            : Timing::DisplayWidth::Graphics;
    switch (Timing::GetHorizontalRegion(context.dot, width,
                                        GetRasterSignalOutput()))
    {
        case Timing::HorizontalRegion::Active:
            context.horizontalRegion = DebugRasterRegion::ActiveDisplay;
            break;
        case Timing::HorizontalRegion::LeftBorder:
        case Timing::HorizontalRegion::RightBorder:
            context.horizontalRegion = DebugRasterRegion::Border;
            break;
        case Timing::HorizontalRegion::HSync:
            context.horizontalRegion = DebugRasterRegion::Sync;
            break;
        default:
            context.horizontalRegion = DebugRasterRegion::Blanking;
            break;
    }

    // The calendar in force is what decided how many slots the port could
    // have had, so report the arbitration schedule rather than the raw mode.
    switch (TMS99xxCpuAccessTiming::SelectSchedule(GetCpuVRAMAccessState()))
    {
        case TMS99xxVramSlotSchedule::Schedule::Graphics:
            context.calendar = DebugVdpSlotCalendar::Graphics; break;
        case TMS99xxVramSlotSchedule::Schedule::Text:
            context.calendar = DebugVdpSlotCalendar::Text; break;
        case TMS99xxVramSlotSchedule::Schedule::Multicolor:
            context.calendar = DebugVdpSlotCalendar::Multicolor; break;
        default:
            context.calendar = DebugVdpSlotCalendar::Refresh; break;
    }

    if (m_HasLastCpuPortAccess && tstates >= m_LastCpuPortAccessTStates)
    {
        context.gapKnown = true;
        context.tStatesSincePreviousAccess =
            static_cast<u32>(tstates - m_LastCpuPortAccessTStates);
    }

    return context;
}

u32 TMS99xx::PackCpuPortRequest(
    const TMS99xxVramSequencer::Request& request)
{
    // 23-bit diagnostic payload: W|AAAAAA AAAAAAAA|DDDDDDDD.
    // Keeping address and data in one before/after value lets a single event
    // describe both the discarded latch and its replacement without adding
    // allocations or a second trace stream to the hot path.
    return (request.write ? (1u << 22) : 0u)
        | (static_cast<u32>(request.address & 0x3FFF) << 8)
        | request.value;
}

void TMS99xx::CompleteVRAMTransfer(
    const TMS99xxVramSequencer::Completion& completion)
{
    if (completion.request.kind == TMS99xxVramSequencer::RequestKind::VideoFetch)
    {
        if (m_HasPendingBackgroundFetch &&
            m_PendingBackgroundFetchIndex < 40)
        {
            BackgroundFetchLatch& latch =
                m_BackgroundFetchLatches[m_PendingBackgroundFetchIndex];
            // This is an internal VDP DRAM fetch, not a CPU-visible $BE
            // read. Keep it out of the VRAM breakpoint/event stream.
            const u8 value = m_pVdpVRAM[completion.request.address & 0x3FFF];
            switch (m_PendingBackgroundFetchActivity)
            {
                case TMS99xxVramSlotSchedule::Activity::Name:
                    latch.name = value; latch.nameValid = true; break;
                case TMS99xxVramSlotSchedule::Activity::Colour:
                    latch.colour = value; latch.colourValid = true; break;
                case TMS99xxVramSlotSchedule::Activity::Pattern:
                    latch.pattern = value; latch.patternValid = true; break;
                default: break;
            }
            ++m_CompletedBackgroundFetches;
            if (m_PendingBackgroundFetchActivity ==
                    TMS99xxVramSlotSchedule::Activity::Pattern &&
                m_PendingBackgroundFetchIndex ==
                    ((m_iMode == 1 || m_iMode == 3) ? 39 : 31))
            {
                RenderStreamingBackgroundLine(m_BackgroundFetchLine);
            }
        }
        m_HasPendingBackgroundFetch = false;
        return;
    }

    if (completion.request.kind == TMS99xxVramSequencer::RequestKind::SpriteFetch)
    {
        CompleteSpriteFetch(completion);
        return;
    }

    if (completion.request.kind == TMS99xxVramSequencer::RequestKind::CpuRead ||
        completion.request.kind == TMS99xxVramSequencer::RequestKind::CpuWrite)
    {
        // issuedAt, not completedAt: that is the physical slot where the CPU
        // port actually accepted the byte, which is what a slot-history view
        // needs to line up against. The commit delay is small (tens of
        // phases) next to a 684-phase line, so this practically always lands
        // in the line still held in the buffer; the rare write issued right
        // at the line's edge can bleed into the next line's fresh history,
        // which is an acceptable inaccuracy for a debug view.
        const std::uint16_t physicalSlot =
            TMS99xxSlotGrid::SlotAtPhase(completion.issuedAt.phase);
        if (physicalSlot < TMS99xxSlotGrid::SlotsPerLine)
        {
            m_LineSlotHistory[physicalSlot].cpuTransacted = true;
            m_LineSlotHistory[physicalSlot].cpuWrite = completion.request.write;
            m_LineSlotHistory[physicalSlot].cpuAddress = completion.request.address;
            m_LineSlotHistory[physicalSlot].cpuValue = completion.request.write
                ? completion.request.value
                : ReadVRAM(completion.request.address);
        }
    }

    if (completion.request.write)
    {
        // Conservative on purpose: a commit on any of the 192 visible lines
        // disqualifies this frame from legacy/streaming bit-identical checks.
        // It is safe to report a few extra candidates while the exact fetch
        // calendar is still being introduced; reporting too few would hide a
        // real renderer regression.
        if (completion.request.kind == TMS99xxVramSequencer::RequestKind::CpuWrite &&
            (completion.completedAt.line % m_iLinesPerFrame) < GC_RESOLUTION_HEIGHT)
        {
            ++m_VisibleLineCpuVramWrites;
        }
        WriteVRAM(completion.request.address, completion.request.value);
        return;
    }

    SetReadBuffer(ReadVRAM(completion.request.address),
                  DebugEventSource::DeviceInternal);
}

void TMS99xx::IssueTimedVideoFetch(
    const TMS99xxVramSequencer::Position& position,
    const TMS99xxVramSlotSchedule::Slot& slot)
{
    // Fires once per physical slot boundary, all 171 of them every line
    // (TMS99xxVramSequencer::AdvanceWithSlotEvents), independent of what the
    // schedule classifies it as. Record the classification here before
    // anything else, so a slot the current mode leaves Cpu/Refresh/Unknown
    // still shows up in the line history rather than only the ones that
    // trigger a video fetch below.
    {
        const std::uint16_t physicalSlot =
            TMS99xxSlotGrid::SlotAtPhase(position.phase);
        if (physicalSlot < TMS99xxSlotGrid::SlotsPerLine)
        {
            // GetVideoVRAMSchedule() is mode-only and does not gate on
            // display-enabled/active-line, since IssueBackgroundFetch and
            // IssueSpriteFetch already refuse to fetch off-screen on their
            // own. That leaves `slot` labelling every slot as if the dense
            // per-mode calendar applied everywhere, when the real chip - and
            // this emulator's own CPU arbitration, TMS99xxCpuAccessTiming::
            // SelectSchedule - fall back to the sparse Refresh schedule
            // outside the active display window. Mirror that same condition
            // here so the debug label matches what actually happens instead
            // of what the mode's slot count would nominally suggest.
            const bool activeVideoLine =
                m_bDisplayEnabled && m_iRenderLine < GC_RESOLUTION_HEIGHT;
            const TMS99xxVramSlotSchedule::Slot historySlot = activeVideoLine
                ? slot
                : TMS99xxVramSlotSchedule::GetSlot(
                      TMS99xxVramSlotSchedule::Schedule::Refresh, physicalSlot);
            m_LineSlotHistory[physicalSlot].activity = historySlot.activity;
            m_LineSlotHistory[physicalSlot].index = historySlot.index;
            m_LineSlotHistory[physicalSlot].cpuTransacted = false;
        }
    }

#ifdef SPRITE_EXPANDER
    // The optional expander behaves like a read-only multi-port companion:
    // at the last physical slot of source line N it snapshots the additional
    // candidates already discovered by the real timed Y scan for line N+1.
    // No request is injected into the native VRAM sequencer.
    if (m_bNoSpriteLimit &&
        TMS99xxSlotGrid::SlotAtPhase(position.phase) == 0)
    {
        LatchExpandedSprites(position.line + 1);
    }
#endif

    switch (slot.activity)
    {
        case TMS99xxVramSlotSchedule::Activity::Name:
        case TMS99xxVramSlotSchedule::Activity::Colour:
        case TMS99xxVramSlotSchedule::Activity::Pattern:
            IssueBackgroundFetch(position, slot);
            return;

        case TMS99xxVramSlotSchedule::Activity::SpriteScanY:
        case TMS99xxVramSlotSchedule::Activity::SpriteSelectedY:
        case TMS99xxVramSlotSchedule::Activity::SpriteSelectedX:
        case TMS99xxVramSlotSchedule::Activity::SpriteSelectedName:
        case TMS99xxVramSlotSchedule::Activity::SpriteSelectedColour:
        case TMS99xxVramSlotSchedule::Activity::SpriteSelectedPattern:
            IssueSpriteFetch(position, slot);
            return;

        default:
            return;
    }
}

void TMS99xx::IssueBackgroundFetch(
    const TMS99xxVramSequencer::Position& position,
    const TMS99xxVramSlotSchedule::Slot& slot)
{
    // The legacy renderer remains authoritative while these timed latches are
    // collected. Graphics I and II share the 32-cell calendar; Text uses its
    // own 40-cell calendar.
    const int line = static_cast<int>(position.line % m_iLinesPerFrame);
    const bool graphicsI = m_iMode == 0;
    const bool graphicsII = m_iMode == 2;
    const bool graphics = graphicsI || graphicsII;
    const bool multicolor = m_iMode == 4 || m_iMode == 6;
    const bool text = m_iMode == 1 || m_iMode == 3;
    const bool bars = m_iMode == 5 || m_iMode == 7;
    const u8 cellsPerLine = text ? 40 : 32;
    if (bars || (!graphics && !text && !multicolor) ||
        line >= GC_RESOLUTION_HEIGHT ||
        !IsSetBit(m_VdpRegister[1], 6) || slot.index >= cellsPerLine)
        return;

    // The per-cell fetch latches belong to the current scanline. A new name
    // fetch begins the next line's sequence from an empty set of latches.
    if (m_BackgroundFetchLine != line)
    {
        for (auto& latch : m_BackgroundFetchLatches)
            latch = {};
        m_BackgroundFetchLine = line;
        m_BackgroundFetchAbsoluteLine = position.line;
    }
    const bool needsColour = graphics &&
        slot.activity == TMS99xxVramSlotSchedule::Activity::Colour;
    if (slot.activity != TMS99xxVramSlotSchedule::Activity::Name &&
        !needsColour &&
        slot.activity != TMS99xxVramSlotSchedule::Activity::Pattern)
        return;

    BackgroundFetchLatch& latch = m_BackgroundFetchLatches[slot.index];
    int address = 0;
    // One address generator for every mode, M3 banking included (Graphics II,
    // Text 1Q and Multicolor Q share the R4 third mask). See
    // TMS99xxTableAddress.h.
    const u16 fetchLine = static_cast<u16>(line);
    if (slot.activity == TMS99xxVramSlotSchedule::Activity::Name)
    {
        address = TMS99xxTableAddress::Name(
            m_iMode, m_VdpRegister[2], fetchLine, slot.index);
    }
    else if (slot.activity == TMS99xxVramSlotSchedule::Activity::Colour)
    {
        if (!latch.nameValid) return;
        address = TMS99xxTableAddress::Colour(
            m_iMode, m_VdpRegister[3], fetchLine, latch.name);
    }
    else
    {
        if (!latch.nameValid) return;
        address = TMS99xxTableAddress::Pattern(
            m_iMode, m_VdpRegister[3], m_VdpRegister[4], fetchLine, latch.name);
    }

    m_PendingBackgroundFetchActivity = slot.activity;
    m_PendingBackgroundFetchIndex = slot.index;
    m_HasPendingBackgroundFetch = m_VramSequencer.IssueRequest({
        TMS99xxVramSequencer::RequestKind::VideoFetch,
        static_cast<u16>(address & 0x3FFF), 0, false});
}

TMS99xx::SpriteLineFetchState* TMS99xx::FindSpriteLineFetchState(
    u64 absoluteTargetLine)
{
    SpriteLineFetchState& state =
        m_SpriteLineFetchStates[absoluteTargetLine & 1u];
    return state.absoluteTargetLine == absoluteTargetLine ? &state : nullptr;
}

TMS99xx::SpriteLineFetchState& TMS99xx::BeginSpriteLineFetchState(
    u64 absoluteTargetLine)
{
    SpriteLineFetchState& state =
        m_SpriteLineFetchStates[absoluteTargetLine & 1u];
    if (state.absoluteTargetLine != absoluteTargetLine)
    {
        state = {};
        state.absoluteTargetLine = absoluteTargetLine;
    }
    return state;
}

#ifdef SPRITE_EXPANDER
TMS99xx::SpriteExpanderLineState* TMS99xx::FindSpriteExpanderLineState(
    u64 absoluteTargetLine)
{
    SpriteExpanderLineState& state =
        m_SpriteExpanderLineStates[absoluteTargetLine & 1u];
    return state.absoluteTargetLine == absoluteTargetLine ? &state : nullptr;
}

TMS99xx::SpriteExpanderLineState& TMS99xx::BeginSpriteExpanderLineState(
    u64 absoluteTargetLine)
{
    SpriteExpanderLineState& state =
        m_SpriteExpanderLineStates[absoluteTargetLine & 1u];
    if (state.absoluteTargetLine != absoluteTargetLine)
    {
        state = {};
        state.absoluteTargetLine = absoluteTargetLine;
    }
    return state;
}

void TMS99xx::ObserveExpandedSprite(u64 absoluteTargetLine, u8 satIndex)
{
    SpriteExpanderLineState& state =
        BeginSpriteExpanderLineState(absoluteTargetLine);
    if (state.candidateCount >= kSpriteExpanderCapacity)
        return;
    state.satIndex[state.candidateCount++] = satIndex;
}

void TMS99xx::LatchExpandedSprites(u64 absoluteTargetLine)
{
    SpriteExpanderLineState* state =
        FindSpriteExpanderLineState(absoluteTargetLine);
    if (state == nullptr || state->candidateCount == 0)
        return;

    const int line = static_cast<int>(absoluteTargetLine %
        static_cast<u64>(m_iLinesPerFrame));
    if (line < 0 || line >= GC_RESOLUTION_HEIGHT)
        return;

    state->large = IsSetBit(m_VdpRegister[1], 1);
    state->magnified = IsSetBit(m_VdpRegister[1], 0);
    const u16 attributeBase = (m_VdpRegister[5] & 0x7F) << 7;
    const u16 patternBase = (m_VdpRegister[6] & 0x07) << 11;

    for (u8 i = 0; i < state->candidateCount; ++i)
    {
        SpriteFetchLatch& latch = state->sprite[i];
        latch = {};
        const u16 sat = static_cast<u16>(attributeBase +
            (state->satIndex[i] << 2));
        latch.y = m_pVdpVRAM[sat & 0x3FFF];
        latch.x = m_pVdpVRAM[(sat + 1) & 0x3FFF];
        latch.name = m_pVdpVRAM[(sat + 2) & 0x3FFF];
        latch.colour = m_pVdpVRAM[(sat + 3) & 0x3FFF];
        latch.yValid = latch.xValid = latch.nameValid = latch.colourValid = true;

        int spriteY = (latch.y + 1) & 0xFF;
        if (spriteY >= 0xE0)
            spriteY -= 0x100;
        const int row = (line - spriteY) >> (state->magnified ? 1 : 0);
        const int sourceHeight = state->large ? 16 : 8;
        if (row < 0 || row >= sourceHeight)
            continue;

        const u8 tile = latch.name & (state->large ? 0xFC : 0xFF);
        const u16 pattern = static_cast<u16>(patternBase +
            (tile << 3) + row);
        latch.pattern[0] = m_pVdpVRAM[pattern & 0x3FFF];
        latch.patternValid[0] = true;
        if (state->large)
        {
            latch.pattern[1] = m_pVdpVRAM[(pattern + 16) & 0x3FFF];
            latch.patternValid[1] = true;
        }
    }
    state->latched = true;
}
#endif

void TMS99xx::IssueSpriteFetch(
    const TMS99xxVramSequencer::Position& position,
    const TMS99xxVramSlotSchedule::Slot& slot)
{
    // Text mode never displays sprites. The schedule is deliberately still
    // described by the generic sequencer, but it must not produce a sprite
    // transaction for this mode.
    if (!SpritesEnabledInMode() || !IsSetBit(m_VdpRegister[1], 6))
        return;

    const u64 sourceLine = position.line;
    // Candidate Y values selected while scanning source line N feed the
    // pixels of N+1. Slots 157..170 fetch selected sprites 0/1 for N+1;
    // slots 5..14 of the next source line fetch selected sprites 2/3 for N.
    const std::uint16_t physicalSlot =
        TMS99xxSlotGrid::SlotAtPhase(position.phase);
    const u64 targetLine = slot.activity ==
            TMS99xxVramSlotSchedule::Activity::SpriteScanY ||
        physicalSlot >= 157
        ? sourceLine + 1
        : sourceLine;
    const int visibleTargetLine = static_cast<int>(
        targetLine % static_cast<u64>(m_iLinesPerFrame));
    if (visibleTargetLine >= GC_RESOLUTION_HEIGHT)
        return;

    const u16 attributeBase = (m_VdpRegister[5] & 0x7F) << 7;
    u16 address = 0;
    bool candidateScan = false;
    u8 selectedOrdinal = slot.index;

    if (slot.activity == TMS99xxVramSlotSchedule::Activity::SpriteScanY)
    {
        // Slot 19 begins the 32-entry scan for one target line. Starting the
        // state here makes the same two-element ring usable across frame
        // wrap without allocations.
        SpriteLineFetchState& state = BeginSpriteLineFetchState(targetLine);
        if (slot.index == 0)
        {
            state = {};
            state.absoluteTargetLine = targetLine;
        }
        if (state.selectionTerminated)
            return;
        address = static_cast<u16>(attributeBase + (slot.index << 2));
        candidateScan = true;
    }
    else
    {
        SpriteLineFetchState* state = FindSpriteLineFetchState(targetLine);
        if (state == nullptr || selectedOrdinal >= state->selectedCount)
            return;

        const u8 logicalSprite = state->selectedSprite[selectedOrdinal];
        switch (slot.activity)
        {
            case TMS99xxVramSlotSchedule::Activity::SpriteSelectedY:
                address = static_cast<u16>(attributeBase + (logicalSprite << 2));
                break;
            case TMS99xxVramSlotSchedule::Activity::SpriteSelectedX:
                address = static_cast<u16>(attributeBase + (logicalSprite << 2) + 1);
                break;
            case TMS99xxVramSlotSchedule::Activity::SpriteSelectedName:
                address = static_cast<u16>(attributeBase + (logicalSprite << 2) + 2);
                break;
            case TMS99xxVramSlotSchedule::Activity::SpriteSelectedColour:
                address = static_cast<u16>(attributeBase + (logicalSprite << 2) + 3);
                break;
            case TMS99xxVramSlotSchedule::Activity::SpriteSelectedPattern:
            {
                SpriteFetchLatch& latch = state->sprite[selectedOrdinal];
                if (!latch.yValid || !latch.nameValid)
                    return;
                int spriteY = (latch.y + 1) & 0xFF;
                if (spriteY >= 0xE0)
                    spriteY -= 0x100;
                const bool large = IsSetBit(m_VdpRegister[1], 1);
                const bool zoomed = IsSetBit(m_VdpRegister[1], 0);
                u8 tile = latch.name & (large ? 0xFC : 0xFF);
                const int row = (visibleTargetLine - spriteY) >>
                    (zoomed ? 1 : 0);
                address = static_cast<u16>(((m_VdpRegister[6] & 0x07) << 11) +
                    (tile << 3) + row + (slot.byte ? 16 : 0));
                break;
            }
            default:
                return;
        }
    }

    m_PendingSpriteFetch = {slot.activity, targetLine, selectedOrdinal,
                            slot.byte, candidateScan};
    m_HasPendingSpriteFetch = m_VramSequencer.IssueRequest({
        TMS99xxVramSequencer::RequestKind::SpriteFetch,
        static_cast<u16>(address & 0x3FFF), 0, false});
}

void TMS99xx::CompleteSpriteFetch(
    const TMS99xxVramSequencer::Completion& completion)
{
    if (!m_HasPendingSpriteFetch)
        return;

    const PendingSpriteFetch pending = m_PendingSpriteFetch;
    m_HasPendingSpriteFetch = false;
    const u8 value = m_pVdpVRAM[completion.request.address & 0x3FFF];
    SpriteLineFetchState* state = FindSpriteLineFetchState(
        pending.absoluteTargetLine);
    if (state == nullptr)
        return;

    if (pending.candidateScan)
    {
        ++m_CompletedSpriteSelectionScans;
        if (value == 0xD0)
        {
            state->selectionTerminated = true;
            return;
        }

        int spriteY = (value + 1) & 0xFF;
        if (spriteY >= 0xE0)
            spriteY -= 0x100;
        int spriteHeight = IsSetBit(m_VdpRegister[1], 1) ? 16 : 8;
        if (IsSetBit(m_VdpRegister[1], 0))
            spriteHeight *= 2;
        const int targetLine = static_cast<int>(pending.absoluteTargetLine %
            static_cast<u64>(m_iLinesPerFrame));
        if (spriteY <= targetLine && spriteY + spriteHeight > targetLine)
        {
#ifdef SPRITE_EXPANDER
            const bool beyondNativeLimit = state->selectedCount >= 4;
#endif
            if (state->selectedCount < 4)
            {
                const u8 selected = state->selectedCount++;
                state->selectedSprite[selected] = pending.index;
                state->selectedRasterX[selected] = static_cast<u8>(
                    TMS99xxSlotGrid::SlotAtPhase(
                        completion.completedAt.phase));
            }
            else if (!state->overflowRecorded)
            {
                // The fifth sprite on the line. vdp18_sprite.vhd raises this at
                // the scan access itself, not at the end of the line, and the
                // condition is purely vertical: colour and X are not consulted,
                // so sprites that are invisible still count. That is what makes
                // the flag usable as a mid-line clock, with the dot decided by
                // the sprite's position in the attribute table.
                state->overflowRecorded = true;
                state->overflowSprite = pending.index;
                state->overflowRasterX = static_cast<u8>(
                    TMS99xxSlotGrid::SlotAtPhase(
                        completion.completedAt.phase));
                const u16 overflowBase = static_cast<u16>(
                    ((m_VdpRegister[5] & 0x7F) << 7) +
                    (pending.index << 2));
                state->overflowSat[0] = value;
                state->overflowSat[1] =
                    m_pVdpVRAM[(overflowBase + 1) & 0x3FFF];
                state->overflowSat[2] =
                    m_pVdpVRAM[(overflowBase + 2) & 0x3FFF];
                state->overflowSat[3] =
                    m_pVdpVRAM[(overflowBase + 3) & 0x3FFF];
                state->overflowSatValid = true;
                if (m_StreamingOverflowLine < 0)
                {
                    m_StreamingOverflowLine = targetLine;
                    m_StreamingOverflowSprite = pending.index;
                }
                if (IsStreamingStatusAuthoritative() &&
                    !IsSetBit(m_VdpStatus, 6))
                {
                    SetStatus(
                        (SetBit(m_VdpStatus, 6) & 0xE0) | pending.index,
                        DebugEventSource::DeviceInternal);
                }
            }
#ifdef SPRITE_EXPANDER
            if (m_bNoSpriteLimit && beyondNativeLimit)
                ObserveExpandedSprite(pending.absoluteTargetLine,
                                      pending.index);
#endif
        }
        return;
    }

    if (pending.index >= state->selectedCount)
        return;
    SpriteFetchLatch& latch = state->sprite[pending.index];
    switch (pending.activity)
    {
        case TMS99xxVramSlotSchedule::Activity::SpriteSelectedY:
            latch.y = value; latch.yValid = true; break;
        case TMS99xxVramSlotSchedule::Activity::SpriteSelectedX:
            latch.x = value; latch.xValid = true; break;
        case TMS99xxVramSlotSchedule::Activity::SpriteSelectedName:
            latch.name = value; latch.nameValid = true; break;
        case TMS99xxVramSlotSchedule::Activity::SpriteSelectedColour:
            latch.colour = value; latch.colourValid = true; break;
        case TMS99xxVramSlotSchedule::Activity::SpriteSelectedPattern:
            if (pending.byte < 2)
            {
                latch.pattern[pending.byte] = value;
                latch.patternValid[pending.byte] = true;
            }
            break;
        default:
            break;
    }
    ++m_CompletedSpriteFetches;
    ScheduleStreamingSpriteCollision(pending.absoluteTargetLine);
}

void TMS99xx::ScheduleStreamingSpriteCollision(u64 absoluteTargetLine)
{
    SpriteLineFetchState* state =
        FindSpriteLineFetchState(absoluteTargetLine);
    if (state == nullptr || state->selectedCount == 0)
        return;

    const int line = static_cast<int>(
        absoluteTargetLine % static_cast<u64>(m_iLinesPerFrame));
    if (line < 0 || line >= GC_RESOLUTION_HEIGHT || !SpritesEnabledInMode())
        return;
    if (m_StreamingCollisionLine == line)
        return;

    const bool large = IsSetBit(m_VdpRegister[1], 1);
    const bool zoomed = IsSetBit(m_VdpRegister[1], 0);
    const int spriteSize = (large ? 16 : 8) * (zoomed ? 2 : 1);

    // Wait until every selected sprite is fully latched, otherwise the overlap
    // would be computed from a partial line.
    for (u8 selected = 0; selected < state->selectedCount; ++selected)
    {
        const SpriteFetchLatch& latch = state->sprite[selected];
        if (!latch.yValid || !latch.xValid || !latch.colourValid ||
            !latch.patternValid[0] || (large && !latch.patternValid[1]))
        {
            return;
        }
    }

    // vdp18_sprite.vhd counts a sprite towards a collision when its shifter is
    // over the current pixel and its pattern bit is set. It does not look at
    // the colour, so a fully transparent sprite still collides: the bit is a
    // timing signal, and a program can only use it as one if it does not depend
    // on anything being visible.
    u8 pixels[GC_RESOLUTION_WIDTH] = {0};
    int collisionDot = -1;
    for (u8 selected = 0; selected < state->selectedCount; ++selected)
    {
        const SpriteFetchLatch& latch = state->sprite[selected];

        int spriteY = (latch.y + 1) & 0xFF;
        if (spriteY >= 0xE0)
            spriteY -= 0x100;
        if (spriteY > line || spriteY + spriteSize <= line)
            continue;

        int spriteX = latch.x;
        if (IsSetBit(latch.colour, 7))
            spriteX -= 32;

        for (int pixelX = 0; pixelX < spriteSize; ++pixelX)
        {
            const int x = spriteX + pixelX;
            if (x >= GC_RESOLUTION_WIDTH)
                break;
            if (x < 0)
                continue;

            const int patternX = pixelX >> (zoomed ? 1 : 0);
            const u8 pattern = patternX < 8 ? latch.pattern[0] : latch.pattern[1];
            const int bit = patternX < 8 ? 7 - patternX : 15 - patternX;
            if (!IsSetBit(pattern, bit & 7))
                continue;

            if (++pixels[x] > 1 && (collisionDot < 0 || x < collisionDot))
                collisionDot = x;
        }
    }

    m_StreamingCollisionLine = line;
    m_StreamingCollisionPhase = collisionDot < 0
        ? -1
        : collisionDot * TMS99xxRasterTiming::PhasesPerDot;
    state->collisionX = collisionDot;
}

void TMS99xx::ApplyStreamingMidLineBlanking()
{
    // Applied once at end of frame rather than when each line is composed: the
    // background fetches run ahead of the beam, so a line can already be
    // composed when R1 bit 6 is cleared further along the same line. The
    // backdrop it paints with is still the one that line emitted, not the one
    // R7 happens to hold now that the frame is over.
    m_FrameRenderDiagnostics.streamingBlankFirstLine = -1;
    m_FrameRenderDiagnostics.streamingBlankFirstColumn = -1;
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    m_FrameRenderDiagnostics.legacyOverflowLine = m_LegacyOverflowLine;
    m_FrameRenderDiagnostics.legacyOverflowSprite = m_LegacyOverflowSprite;
    m_FrameRenderDiagnostics.legacyCollisions = m_LegacyCollisions;
#endif
    m_FrameRenderDiagnostics.streamingOverflowLine = m_StreamingOverflowLine;
    m_FrameRenderDiagnostics.streamingOverflowSprite = m_StreamingOverflowSprite;
    m_FrameRenderDiagnostics.streamingCollisions = m_StreamingCollisions;
    m_FrameRenderDiagnostics.streamingStatusAuthoritative =
        IsStreamingStatusAuthoritative();
    m_FrameRenderDiagnostics.suppressedVBlankFlags = m_SuppressedVBlankFlags;
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    m_LegacyOverflowLine = -1;
    m_LegacyOverflowSprite = -1;
    m_LegacyCollisions = 0;
#endif
    m_StreamingOverflowLine = -1;
    m_StreamingOverflowSprite = -1;
    m_StreamingCollisions = 0;
    m_FrameRenderDiagnostics.streamingCollisionLine = -1;
    m_FrameRenderDiagnostics.streamingCollisionDot = -1;
    for (int line = 0; line < GC_RESOLUTION_HEIGHT; ++line)
    {
        const u16 cut = m_StreamingBlankFromColumn[line];
        if (cut >= GC_RESOLUTION_WIDTH)
            continue;

        if (m_FrameRenderDiagnostics.streamingBlankFirstLine < 0)
        {
            m_FrameRenderDiagnostics.streamingBlankFirstLine = line;
            m_FrameRenderDiagnostics.streamingBlankFirstColumn = cut;
        }

        u8 register7[GC_RESOLUTION_WIDTH];
        FillRegister7Row(line, register7);

        const int lineOffset = line * GC_RESOLUTION_WIDTH;
        for (int pixel = cut; pixel < GC_RESOLUTION_WIDTH; ++pixel)
        {
            const u16 backdrop =
                static_cast<u16>(BackdropOf(register7[pixel]));
            m_pStreamingBackground[lineOffset + pixel] = backdrop;
            m_pStreamingComposed[lineOffset + pixel] = backdrop;
            m_pStreamingSpriteInfo[lineOffset + pixel] = 0;
        }
    }
}

void TMS99xx::FinalizeFrameRenderDiagnostics()
{
    ApplyStreamingMidLineBlanking();
    ++m_PresentationSourceRevision;
    ++m_DebugPreviewSourceGeneration;
    m_FrameRenderDiagnostics.frameSerial++;
    m_RenderedSpriteDebugWorking.valid = true;
    m_RenderedSpriteDebugWorking.frameSerial =
        m_FrameRenderDiagnostics.frameSerial;
    m_RenderedSpriteDebugWorking.rasterLines = m_iLinesPerFrame;
    m_LastRenderedSpriteDebugFrame = m_RenderedSpriteDebugWorking;
    m_RenderedSpriteDebugWorking = {};
    m_SpriteSelectionHistoryWorking.valid = true;
    m_SpriteSelectionHistoryWorking.frameSerial =
        m_FrameRenderDiagnostics.frameSerial;
    m_SpriteSelectionHistoryWorking.rasterLines = m_iLinesPerFrame;
    m_LastSpriteSelectionHistory = m_SpriteSelectionHistoryWorking;
    m_SpriteSelectionHistoryWorking = {};
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    const GC_VideoFrameDescriptor descriptor = GetFrameDescriptor();
    const int pixels = descriptor.frame_width * descriptor.frame_height;
    m_FrameRenderDiagnostics.legacyFrameHash =
        HashLegacyFrame(m_pFrameBuffer, pixels);
#endif
    m_FrameRenderDiagnostics.visibleLineCpuVramWrites =
        m_VisibleLineCpuVramWrites;
    m_FrameRenderDiagnostics.visibleLineVdpRegisterWrites =
        m_VisibleLineVdpRegisterWrites;
    m_FrameRenderDiagnostics.completedBackgroundFetches =
        m_CompletedBackgroundFetches;
    m_FrameRenderDiagnostics.completedSpriteFetches =
        m_CompletedSpriteFetches;
    m_FrameRenderDiagnostics.completedSpriteSelectionScans =
        m_CompletedSpriteSelectionScans;

#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    u16 comparedLines = 0;
    u32 mismatchedPixels = 0;
    bool everyLineReady = true;
    for (int line = 0; line < GC_RESOLUTION_HEIGHT; ++line)
    {
        if (!m_LegacyBackgroundLineCaptured[line] ||
            !m_StreamingBackgroundLineReady[line])
        {
            everyLineReady = false;
            continue;
        }
        ++comparedLines;
        const int offset = line * GC_RESOLUTION_WIDTH;
        for (int pixel = 0; pixel < GC_RESOLUTION_WIDTH; ++pixel)
        {
            if (m_pLegacyBackground[offset + pixel] !=
                m_pStreamingBackground[offset + pixel])
            {
                ++mismatchedPixels;
            }
        }
    }
    m_FrameRenderDiagnostics.streamingBackgroundComparable =
        everyLineReady && m_VisibleLineCpuVramWrites == 0 &&
        m_VisibleLineVdpRegisterWrites == 0;
    m_FrameRenderDiagnostics.streamingBackgroundComparedLines = comparedLines;
    m_FrameRenderDiagnostics.streamingBackgroundMismatchedPixels =
        mismatchedPixels;
#endif
    m_FrameRenderDiagnostics.streamingBackgroundHash =
        HashLegacyFrame(m_pStreamingBackground,
            GC_RESOLUTION_WIDTH * GC_RESOLUTION_HEIGHT);

#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    u16 spriteComparedLines = 0;
    u32 spriteMismatchedPixels = 0;
    bool everySpriteLineReady = true;
    for (int line = 0; line < GC_RESOLUTION_HEIGHT; ++line)
    {
        if (!m_LegacyComposedLineCaptured[line] ||
            !m_StreamingSpriteLineReady[line])
        {
            everySpriteLineReady = false;
            continue;
        }
        ++spriteComparedLines;
        const int offset = line * GC_RESOLUTION_WIDTH;
        for (int pixel = 0; pixel < GC_RESOLUTION_WIDTH; ++pixel)
        {
            if (m_pLegacyComposed[offset + pixel] !=
                m_pStreamingComposed[offset + pixel])
            {
                ++spriteMismatchedPixels;
            }
        }
    }
    m_FrameRenderDiagnostics.streamingSpriteComparable =
        everySpriteLineReady &&
#ifdef SPRITE_EXPANDER
        !m_bNoSpriteLimit &&
#endif
        m_VisibleLineCpuVramWrites == 0 &&
        m_VisibleLineVdpRegisterWrites == 0;
    m_FrameRenderDiagnostics.streamingSpriteComparedLines = spriteComparedLines;
    m_FrameRenderDiagnostics.streamingSpriteMismatchedPixels =
        spriteMismatchedPixels;
#endif
    m_FrameRenderDiagnostics.streamingSpriteHash =
        HashLegacyFrame(m_pStreamingComposed,
            GC_RESOLUTION_WIDTH * GC_RESOLUTION_HEIGHT);

    for (int line = 0; line < GC_RESOLUTION_HEIGHT; ++line)
    {
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
        m_LegacyBackgroundLineCaptured[line] = false;
#endif
        m_StreamingBackgroundLineReady[line] = false;
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
        m_LegacyComposedLineCaptured[line] = false;
#endif
        m_StreamingSpriteLineReady[line] = false;
        m_StreamingBlankFromColumn[line] = GC_RESOLUTION_WIDTH;
    }
    m_VisibleLineCpuVramWrites = 0;
    m_VisibleLineVdpRegisterWrites = 0;
    m_CompletedBackgroundFetches = 0;
    m_CompletedSpriteFetches = 0;
    m_CompletedSpriteSelectionScans = 0;
}

void TMS99xx::SetStatus(u8 value, DebugEventSource source)
{
    const u8 before = m_VdpStatus;
    m_VdpStatus = value;
    if (before != value)
        m_pMemory->CheckVDPStateChange(DebugStatus, before, value, source);
}

void TMS99xx::SetVideoMode(int value, DebugEventSource source)
{
    const int before = m_iMode;
    m_iMode = value;
    if (before != value)
    {
        m_NameTableDebugDirty = true;
        m_PatternTableDebugDirty = true;
        m_SpritePatternDebugDirty = true;
        m_pMemory->CheckVDPStateChange(DebugVideoMode,
            static_cast<u64>(before), static_cast<u64>(value), source);
    }
}

void TMS99xx::SetRegisterValue(u8 reg, u8 value)
{
    if (reg >= sizeof(m_VdpRegister))
        return;
    const u8 before = m_VdpRegister[reg];
    m_VdpRegister[reg] = value;
    if (before != value)
    {
        m_NameTableDebugDirty = true;
        m_PatternTableDebugDirty = true;
        m_SpritePatternDebugDirty = true;
    }
    if (reg == 7 && before != value)
        RecordRegister7Write(value);
    if (before != value && m_iRenderLine < GC_RESOLUTION_HEIGHT)
        ++m_VisibleLineVdpRegisterWrites;

    // Blanking mid line. m_bDisplayEnabled is latched once per line at
    // TIMING_DISPLAY, so clearing R1 bit 6 part way through a line cannot take
    // effect before the next one in the per-line model. Blanking is an output
    // stage though, not a fetch: on the real VDP the pixels already emitted
    // stay and the rest of the line becomes backdrop. Record where the line was
    // cut so the streaming renderer can reproduce it. Only the disable
    // direction is recorded: re-enabling mid line cannot bring the line back,
    // because its fetches were never issued.
    if (reg == 1 && IsSetBit(before, 6) && !IsSetBit(value, 6) &&
        m_bDisplayEnabled && m_iRenderLine < GC_RESOLUTION_HEIGHT)
    {
        const int column = GetColumn();
        const u16 cut = static_cast<u16>(
            column < 0 ? 0 : (column > GC_RESOLUTION_WIDTH
                ? GC_RESOLUTION_WIDTH : column));
        if (cut < m_StreamingBlankFromColumn[m_iRenderLine])
            m_StreamingBlankFromColumn[m_iRenderLine] = cut;

        // The remaining fetches for this line will never complete, so the
        // normal end-of-line compose will not happen. Compose it now, while the
        // latches for the line are still live, using whatever was prefetched.
        if (m_BackgroundFetchLine == m_iRenderLine)
            RenderStreamingBackgroundLine(m_iRenderLine, true);
    }

    m_pMemory->CheckVDPRegisterWrite(reg, before, value);
}

bool TMS99xx::SpritesEnabledInMode() const
{
    switch (m_iMode)
    {
        case 0: // Graphics I
        case 2: // Graphics II
        case 4: // Multicolor
        case 6: // Multicolor with Graphics II banking, TMS9918 only
            return true;
        default: // Text, Text with banking, and the two bar patterns
            return false;
    }
}

void TMS99xx::ScanLine(int line)
{
    if (m_bDisplayEnabled)
    {
        if (line < GC_RESOLUTION_HEIGHT)
        {
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
            RenderBackground(line);
            CaptureLegacyBackgroundLine(line);
#endif

            // The bar patterns read no table, so no fetch ever completes to
            // trigger the streaming compositor. Drive it from here instead.
            if (m_iMode == 5 || m_iMode == 7)
                RenderStreamingBackgroundLine(line);

#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
            if (SpritesEnabledInMode())
                RenderSprites(line);
            CaptureLegacyComposedLine(line);
#endif
        }
    }
    else
    {
        if (line < GC_RESOLUTION_HEIGHT)
        {
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
            u16 color = m_VdpRegister[7] & 0x0F;
            int line_width = line * GC_RESOLUTION_WIDTH;

            for (int scx = 0; scx < GC_RESOLUTION_WIDTH; scx++)
            {
                int pixel = line_width + scx;
                m_pFrameBuffer[pixel] = color;
                m_pInfoBuffer[pixel] = 0;
            }
#endif

            // A blanked VDP performs no pattern fetch, so the streaming
            // renderer is never invoked for this line and would otherwise
            // present a stale frame. Mark the whole line as blanked unless the
            // beam already cut it part way through, in which case the pixels
            // before the cut were genuinely emitted and must survive. The fill
            // itself happens once per frame in ApplyStreamingMidLineBlanking,
            // so both cases go through the same code.
            if (m_StreamingBlankFromColumn[line] == GC_RESOLUTION_WIDTH)
                m_StreamingBlankFromColumn[line] = 0;

#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
            CaptureLegacyBackgroundLine(line);
            CaptureLegacyComposedLine(line);
#endif
            m_StreamingBackgroundLineReady[line] = true;
            m_StreamingSpriteLineReady[line] = true;
        }
    }
}

bool TMS99xx::AttachCrtSignalSink(ICrtSignalSink* sink)
{
    m_pCrtSignalSink = sink;
    if (m_pCrtSignalSink != NULL)
    {
        const GC_CrtSignalOutput output =
            GetRasterSignalOutput() ==
                TMS99xxRasterTiming::SignalOutput::Composite9918
            ? GC_CrtSignalOutput::Composite
            : GC_CrtSignalOutput::Component;
        m_pCrtSignalSink->ConfigureSignal(m_bPAL, output);
    }
    return true;
}

ICrtSignalSink* TMS99xx::GetCrtSignalSink() const
{
    return m_pCrtSignalSink;
}

// Emits one scanline to the attached signal sink, in raster order - not the
// display order RenderFullRasterDebugLine uses. The remap that puts the top
// border at the top of a debug image would be wrong here: a receiver locks
// to the sync pulses in the order they actually arrive.
//
// Runs are emitted whole rather than dot by dot. Every boundary is a
// compile-time constant of TMS99xxRasterTiming, so there is nothing to scan
// for: the lengths below are those constants, and the only per-line
// variation is which vertical region the line falls in.
void TMS99xx::EmitCrtSignalLine(int line)
{
    using Timing = TMS99xxRasterTiming;
    ICrtSignalSink* sink = m_pCrtSignalSink;

    const Timing::VerticalRegion vregion =
        Timing::GetVerticalRegion(static_cast<std::uint16_t>(line), m_bPAL);

    // A vertical sync line is one full-line sync pulse and nothing else.
    // That abnormal length is the only thing telling the receiver where the
    // field starts - see the OutputSync contract in ICrtSignalSink.h.
    if (vregion == Timing::VerticalRegion::VerticalSync)
    {
        sink->OutputSync(Timing::DotsPerLine);
        return;
    }

    const Timing::SignalOutput signal = GetRasterSignalOutput();
    const int activeStart = Timing::GetActiveStart(Timing::DisplayWidth::Graphics, signal);
    const int activeEnd = Timing::GetActiveEnd(Timing::DisplayWidth::Graphics, signal);

    // Horizontal blanking is identical on every line that is not vertical
    // sync, picture or not: sync, then the burst nested in its two blanks.
    sink->OutputSync(26);
    sink->OutputBlank(2);
    sink->OutputColourBurst(14);
    sink->OutputBlank(8);

    const int pictureStart = 50;
    const int pictureEnd = 333;
    const int pictureDots = pictureEnd - pictureStart + 1;

    const bool activeLine = vregion == Timing::VerticalRegion::Active;
    const bool hasContent = activeLine && line >= 0 && line < GC_RESOLUTION_HEIGHT;

    if (vregion == Timing::VerticalRegion::BottomBlanking ||
        vregion == Timing::VerticalRegion::TopBlanking)
    {
        // Blanked lines carry no picture and no border, so the span that
        // would hold them is blanking too.
        sink->OutputBlank(pictureDots);
    }
    else
    {
        // Border dots are displayed pixels carrying the backdrop colour, so
        // they go out through OutputData alongside the active area, as one
        // run - a receiver draws them, it does not blank them.
        for (int dot = pictureStart; dot <= pictureEnd; dot++)
        {
            u8 index = static_cast<u8>(BackdropOf(
                Register7AtRasterDot(line, dot, activeStart)));
            if (hasContent && dot >= activeStart && dot <= activeEnd)
            {
                index = static_cast<u8>(
                    m_pStreamingComposed[line * GC_RESOLUTION_WIDTH +
                        (dot - activeStart)] & 0x0F);
            }
            m_CrtSignalLineIndices[dot - pictureStart] = index;
        }
        sink->OutputData(pictureDots, m_CrtSignalLineIndices,
            static_cast<std::size_t>(pictureDots));
    }

    sink->OutputBlank(Timing::DotsPerLine - pictureEnd - 1);
}

void TMS99xx::SetFullRasterDebugEnabled(bool enabled)
{
    if (m_bFullRasterDebugEnabled == enabled)
        return;

    // This flips the frame geometry between the cropped picture and the raw
    // 342-dot raster, so it is a descriptor change exactly like SetOverscan.
    m_bFullRasterDebugEnabled = enabled;
    ++m_FrameDescriptorRevision;
}

// Debug-only: paints one raster line (all 342 dots, not just the ~256-wide
// content window) using TMS99xxRasterTiming::GetHorizontalRegion, so the
// region boundaries can be checked visually before a real signal-level CRT
// sink consumes them. Border/active dots use the real backdrop/content
// color through m_pCurrentPalette; sync/blank/burst use fixed debug colors
// that do not appear anywhere in the real 16-color palette, so they cannot
// be mistaken for actual picture content.
//
// Content is always read against DisplayWidth::Graphics (the 256-dot
// active window) regardless of m_iMode: the existing content buffers
// (m_pStreamingComposed) are always laid out that way - text mode pads its
// own narrower 240-pixel glyph area with backdrop at buffer columns
// [0,7]/[248,255] rather than using TMS99xxRasterTiming's alternate
// (69-dot-start, 240-wide) text active window. Using DisplayWidth::Text
// here would misalign the buffer-to-dot mapping.
//
// `line` is 0 = first active display line (this codebase's own raster-order
// convention). The buffer is instead written in *display order* - row 0 is
// the first row of the top border - via GetVerticalDisplayRow, so the image
// reads top-to-bottom the way a real raster stacks: top border, active
// picture, bottom border, then the blanking/vsync cluster at the very
// bottom. Vertical region (GetVerticalRegion) decides what fills the
// LeftBorder/Active/RightBorder dot span: real border color on a border
// line, blanking level on a blanking/vsync line, actual picture only on an
// active line. HSync/blank/burst/back-porch are per-line constants and are
// painted the same regardless of vertical region, matching every real
// raster line having its own horizontal sync.
void TMS99xx::RenderFullRasterDebugLine(int line)
{
    using Timing = TMS99xxRasterTiming;
    const Timing::SignalOutput signal = GetRasterSignalOutput();
    const int activeStart = Timing::GetActiveStart(Timing::DisplayWidth::Graphics, signal);

    // Marker colour for the horizontal and vertical sync bands: a very dark
    // slate, between blue and grey. Dark enough to read as "not picture",
    // light enough to separate from the true black of the surrounding
    // blanking, and desaturated in a way no console palette ever produces -
    // the nearest TMS9918 entries are pure black and a mid green/purple,
    // both far away, so it cannot be mistaken for picture content.
    const u8 kSyncR = 22;
    const u8 kSyncG = 24;
    const u8 kSyncB = 38;

    const Timing::VerticalRegion vregion =
        Timing::GetVerticalRegion(static_cast<std::uint16_t>(line), m_bPAL);
    const bool activeLine = vregion == Timing::VerticalRegion::Active;
    const bool borderLine = vregion == Timing::VerticalRegion::TopBorder ||
        vregion == Timing::VerticalRegion::BottomBorder;
    const bool syncLine = vregion == Timing::VerticalRegion::VerticalSync;

    const bool hasContent = activeLine && line >= 0 && line < GC_RESOLUTION_HEIGHT;
    const int contentRowOffset = line * GC_RESOLUTION_WIDTH;

    const int displayRow = Timing::GetVerticalDisplayRow(
        static_cast<std::uint16_t>(line), m_bPAL);
    u8* row = m_pFullRasterDebugRGB +
        static_cast<long>(displayRow) * Timing::DotsPerLine * 3;

    for (int dot = 0; dot < Timing::DotsPerLine; dot++)
    {
        const Timing::HorizontalRegion region =
            Timing::GetHorizontalRegion(dot, Timing::DisplayWidth::Graphics, signal);

        u8 r, g, b;
        switch (region)
        {
            // Sync gets a very dark blue rather than black. It sits at the
            // same "never visible on a display" level as the surrounding
            // blanking, so on a real screen the two are indistinguishable -
            // but telling them apart is the whole point of this view. No
            // TMS9918 palette entry is anywhere near this colour, so it
            // still cannot be mistaken for picture content.
            case Timing::HorizontalRegion::HSync:
                r = kSyncR; g = kSyncG; b = kSyncB;
                break;
            // Blanking and burst are true black: at or below blanking level,
            // nothing in them is ever visible.
            case Timing::HorizontalRegion::LeftBlankA:
            case Timing::HorizontalRegion::LeftBlankB:
            case Timing::HorizontalRegion::RightBlank:
            case Timing::HorizontalRegion::ColorBurst:
                if (syncLine) { r = kSyncR; g = kSyncG; b = kSyncB; }
                else { r = 0; g = 0; b = 0; }
                break;
            // The picture-bearing span of the line. Vertical region decides
            // first: on a blanking/sync line none of these dots carry picture
            // *or* border, so the whole span is black - side borders included.
            // Checking the horizontal region alone here would leave the two
            // side-border columns painted with the backdrop right through the
            // vertical blanking band.
            case Timing::HorizontalRegion::LeftBorder:
            case Timing::HorizontalRegion::RightBorder:
            case Timing::HorizontalRegion::Active:
            default:
                if (syncLine)
                {
                    r = kSyncR; g = kSyncG; b = kSyncB;
                }
                else if (!activeLine && !borderLine)
                {
                    r = 0; g = 0; b = 0;
                }
                else if (borderLine || region != Timing::HorizontalRegion::Active ||
                    !hasContent)
                {
                    const u8* backdropRGB = &m_pCurrentPalette[BackdropOf(
                        Register7AtRasterDot(line, dot, activeStart)) * 3];
                    r = backdropRGB[0]; g = backdropRGB[1]; b = backdropRGB[2];
                }
                else
                {
                    const u16 colorIndex =
                        m_pStreamingComposed[contentRowOffset + (dot - activeStart)];
                    const u8* rgb = &m_pCurrentPalette[colorIndex * 3];
                    r = rgb[0]; g = rgb[1]; b = rgb[2];
                }
                break;
        }

        row[dot * 3 + 0] = r;
        row[dot * 3 + 1] = g;
        row[dot * 3 + 2] = b;
    }
}

#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
void TMS99xx::RenderBackground(int line)
{
    int line_offset = line * GC_RESOLUTION_WIDTH;

    int name_table_addr = m_VdpRegister[2] << 10;
    int color_table_addr = m_VdpRegister[3] << 6;
    int pattern_table_addr = m_VdpRegister[4] << 11;
    int backdrop_color = m_VdpRegister[7] & 0x0F;
    backdrop_color = (backdrop_color > 0) ? backdrop_color : 1;

    int tile_y = line >> 3;
    int tile_y_offset = line & 7;

    switch (m_iMode)
    {
        case 1:
        {
            int fg_color = (m_VdpRegister[7] >> 4) & 0x0F;
            int bg_color = backdrop_color;
            fg_color = (fg_color > 0) ? fg_color : backdrop_color;

            for (int i = 0; i < 8; i++)
            {
                int pixel = line_offset + i;
                m_pFrameBuffer[pixel] = bg_color;
                m_pFrameBuffer[pixel + 248] = bg_color;
                m_pInfoBuffer[pixel] = 0x00;
                m_pInfoBuffer[pixel + 248] = 0x00;
            }

            for (int tile_x = 0; tile_x < 40; tile_x++)
            {
                int tile_number = (tile_y * 40) + tile_x;
                int name_tile_addr = name_table_addr + tile_number;
                int name_tile = m_pVdpVRAM[name_tile_addr];


                u8 pattern_line = m_pVdpVRAM[pattern_table_addr + (name_tile << 3) + tile_y_offset];

                int screen_offset = line_offset + (tile_x * 6) + 8;

                for (int tile_pixel = 0; tile_pixel < 6; tile_pixel++)
                {
                    int pixel = screen_offset + tile_pixel;
                    m_pFrameBuffer[pixel] = IsSetBit(pattern_line, 7 - tile_pixel) ? fg_color : bg_color;
                    m_pInfoBuffer[pixel] = 0x00;
                }
            }
            return;
        }
        case 2:
        {
            // Addresses come from TMS99xxTableAddress in the cell loop.
            break;
        }
        case 4:
        {
            // Multicolor uses the whole three-bit R4 base, unlike Graphics II
            // where only bit 13 survives. Confirmed by vdp18_addr_mux.vhd,
            // which feeds reg_pgb_i straight into the address, and by openMSX,
            // whose renderMulti passes an all-ones mask.
            break;
        }

        // Undocumented mode combinations. M3 is the Graphics II bit, so
        // turning it on beside M1 or M2 drags the Graphics II address banking
        // into that mode; the two combinations without M3 stop the table
        // machinery altogether and leave a fixed bar pattern. Behaviour taken
        // from openMSX, which is the only reference that renders these:
        // CharacterConverter renderText1Q, renderMultiQ and renderBogus.
        case 5:
        case 7:
        {
            // No table fetch is enabled when M1 and M2 are both asserted.
            // The visible result still comes from the two active datapaths:
            // M1 supplies a six-pixel Text cell and R7 colours, while M2
            // supplies Multicolor's fixed 11110000 shift pattern. Consuming
            // six shifts per cell naturally produces 111100, not a separately
            // invented four-plus-two bar renderer.
            int fg_color = (m_VdpRegister[7] >> 4) & 0x0F;
            fg_color = (fg_color > 0) ? fg_color : backdrop_color;
            constexpr u8 multicolor_shift_pattern = 0xF0;
            constexpr int text_cell_width = 6;

            int pixel = line_offset;
            for (int i = 0; i < 8; i++, pixel++)
            {
                m_pFrameBuffer[pixel] = backdrop_color;
                m_pInfoBuffer[pixel] = 0x00;
            }
            for (int bar = 0; bar < 40; bar++)
            {
                for (int shift = 0; shift < text_cell_width; ++shift, ++pixel)
                {
                    const bool patternBit = (multicolor_shift_pattern &
                        (0x80u >> shift)) != 0;
                    m_pFrameBuffer[pixel] = patternBit
                        ? fg_color : backdrop_color;
                    m_pInfoBuffer[pixel] = 0x00;
                }
            }
            for (int i = 0; i < 8; i++, pixel++)
            {
                m_pFrameBuffer[pixel] = backdrop_color;
                m_pInfoBuffer[pixel] = 0x00;
            }
            return;
        }

        case 3:
        {
            // Text with Graphics II banking: normal R2 name table; the pattern
            // number is banked by screen third and masked by R4 bits 0-1,
            // exactly like Graphics II (TMS99xxTableAddress::Pattern).
            int fg_color = (m_VdpRegister[7] >> 4) & 0x0F;
            fg_color = (fg_color > 0) ? fg_color : backdrop_color;

            for (int i = 0; i < 8; i++)
            {
                int pixel = line_offset + i;
                m_pFrameBuffer[pixel] = backdrop_color;
                m_pFrameBuffer[pixel + 248] = backdrop_color;
                m_pInfoBuffer[pixel] = 0x00;
                m_pInfoBuffer[pixel + 248] = 0x00;
            }

            for (int tile_x = 0; tile_x < 40; tile_x++)
            {
                const int name_tile = m_pVdpVRAM[TMS99xxTableAddress::Name(
                    3, m_VdpRegister[2], static_cast<u16>(line), static_cast<u8>(tile_x))];
                const u8 pattern_line = m_pVdpVRAM[TMS99xxTableAddress::Pattern(
                    3, m_VdpRegister[3], m_VdpRegister[4], static_cast<u16>(line),
                    static_cast<u8>(name_tile))];

                int screen_offset = line_offset + (tile_x * 6) + 8;
                for (int tile_pixel = 0; tile_pixel < 6; tile_pixel++)
                {
                    int pixel = screen_offset + tile_pixel;
                    m_pFrameBuffer[pixel] =
                        IsSetBit(pattern_line, 7 - tile_pixel) ? fg_color : backdrop_color;
                    m_pInfoBuffer[pixel] = 0x00;
                }
            }
            return;
        }

        case 6:
        {
            // Multicolor with Graphics II banking: pattern number banked by
            // screen third and masked by R4 bits 0-1, like Graphics II.

            for (int tile_x = 0; tile_x < 32; tile_x++)
            {
                const int name_tile =
                    m_pVdpVRAM[(name_table_addr + (tile_y << 5) + tile_x) & 0x3FFF];
                const u8 colour_line = m_pVdpVRAM[TMS99xxTableAddress::Pattern(
                    6, m_VdpRegister[3], m_VdpRegister[4], static_cast<u16>(line),
                    static_cast<u8>(name_tile))];

                int left_color = colour_line >> 4;
                int right_color = colour_line & 0x0F;
                left_color = (left_color > 0) ? left_color : backdrop_color;
                right_color = (right_color > 0) ? right_color : backdrop_color;

                int screen_offset = line_offset + (tile_x << 3);
                for (int tile_pixel = 0; tile_pixel < 4; tile_pixel++)
                {
                    m_pFrameBuffer[screen_offset + tile_pixel] = left_color;
                    m_pInfoBuffer[screen_offset + tile_pixel] = 0x00;
                    m_pFrameBuffer[screen_offset + 4 + tile_pixel] = right_color;
                    m_pInfoBuffer[screen_offset + 4 + tile_pixel] = 0x00;
                }
            }
            return;
        }
    }

    for (int tile_x = 0; tile_x < 32; tile_x++)
    {
        int tile_number = (tile_y << 5) + tile_x;
        int name_tile_addr = name_table_addr + tile_number;
        int name_tile = m_pVdpVRAM[name_tile_addr];
        u8 pattern_line = 0;
        u8 color_line = 0;

        if (m_iMode == 4)
        {
            int offset_color = pattern_table_addr + (name_tile << 3) + ((tile_y & 0x03) << 1) + (line & 0x04 ? 1 : 0);
            color_line = m_pVdpVRAM[offset_color];

            int left_color = color_line >> 4;
            int right_color = color_line & 0x0F;
            left_color = (left_color > 0) ? left_color : backdrop_color;
            right_color = (right_color > 0) ? right_color : backdrop_color;

            int screen_offset = line_offset + (tile_x << 3);

            for (int tile_pixel = 0; tile_pixel < 4; tile_pixel++)
            {
                int pixel = screen_offset + tile_pixel;
                m_pFrameBuffer[pixel] = left_color;
                m_pInfoBuffer[pixel] = 0x00;
            }

            for (int tile_pixel = 4; tile_pixel < 8; tile_pixel++)
            {
                int pixel = screen_offset + tile_pixel;
                m_pFrameBuffer[pixel] = right_color;
                m_pInfoBuffer[pixel] = 0x00;
            }

            continue;
        }
        else if (m_iMode == 0)
        {
            pattern_line = m_pVdpVRAM[pattern_table_addr + (name_tile << 3) + tile_y_offset];
            color_line = m_pVdpVRAM[color_table_addr + (name_tile >> 3)];
        }
        else if (m_iMode == 2)
        {
            pattern_line = m_pVdpVRAM[TMS99xxTableAddress::Pattern(
                2, m_VdpRegister[3], m_VdpRegister[4], static_cast<u16>(line), static_cast<u8>(name_tile))];
            color_line = m_pVdpVRAM[TMS99xxTableAddress::Colour(
                2, m_VdpRegister[3], static_cast<u16>(line), static_cast<u8>(name_tile))];
        }

        int fg_color = color_line >> 4;
        int bg_color = color_line & 0x0F;
        fg_color = (fg_color > 0) ? fg_color : backdrop_color;
        bg_color = (bg_color > 0) ? bg_color : backdrop_color;

        int screen_offset = line_offset + (tile_x << 3);

        for (int tile_pixel = 0; tile_pixel < 8; tile_pixel++)
        {
            int pixel = screen_offset + tile_pixel;
            m_pFrameBuffer[pixel] = IsSetBit(pattern_line, 7 - tile_pixel) ? fg_color : bg_color;
            m_pInfoBuffer[pixel] = 0x00;
        }
    }
}

void TMS99xx::CaptureLegacyBackgroundLine(int line)
{
    if (line < 0 || line >= GC_RESOLUTION_HEIGHT)
        return;

    const int offset = line * GC_RESOLUTION_WIDTH;
    for (int pixel = 0; pixel < GC_RESOLUTION_WIDTH; ++pixel)
        m_pLegacyBackground[offset + pixel] = m_pFrameBuffer[offset + pixel];
    m_LegacyBackgroundLineCaptured[line] = true;
}

void TMS99xx::CaptureLegacyComposedLine(int line)
{
    if (line < 0 || line >= GC_RESOLUTION_HEIGHT)
        return;

    const int offset = line * GC_RESOLUTION_WIDTH;
    for (int pixel = 0; pixel < GC_RESOLUTION_WIDTH; ++pixel)
        m_pLegacyComposed[offset + pixel] = m_pFrameBuffer[offset + pixel];
    m_LegacyComposedLineCaptured[line] = true;
}
#endif // GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER

void TMS99xx::RenderStreamingBackgroundLine(int line, bool allowPartial)
{
    // This is deliberately a background-only parallel renderer. Sprite fetch
    // latches are the next stage; the legacy framebuffer remains authoritative
    // until both layers can be compared under the same timing rules.
    if (line < 0 || line >= GC_RESOLUTION_HEIGHT || m_iMode > 7)
        return;

    const int lineOffset = line * GC_RESOLUTION_WIDTH;
    // R7 can be rewritten while the beam is inside this line, so the backdrop
    // and the text ink are per dot, not per line. Everything below indexes
    // register7 by the active dot it is painting.
    u8 register7[GC_RESOLUTION_WIDTH];
    FillRegister7Row(line, register7);

    if (m_iMode == 5 || m_iMode == 7)
    {
        // M1 clocks six pixels per Text cell and selects R7 colours. M2 loads
        // Multicolor's fixed 11110000 shift pattern. With no valid table-fetch
        // path for M1+M2, consuming six bits yields the observed 111100 bars.
        constexpr u8 multicolorShiftPattern = 0xF0;
        constexpr int textCellWidth = 6;
        int pixel = lineOffset;
        int column = 0;
        for (int i = 0; i < 8; ++i, ++column)
            m_pStreamingBackground[pixel++] = BackdropOf(register7[column]);
        for (int bar = 0; bar < 40; ++bar)
        {
            for (int shift = 0; shift < textCellWidth; ++shift, ++column)
            {
                const bool patternBit = (multicolorShiftPattern &
                    (0x80u >> shift)) != 0;
                m_pStreamingBackground[pixel++] = patternBit
                    ? ForegroundOf(register7[column])
                    : BackdropOf(register7[column]);
            }
        }
        for (int i = 0; i < 8; ++i, ++column)
            m_pStreamingBackground[pixel++] = BackdropOf(register7[column]);

        m_StreamingBackgroundLineReady[line] = true;
        RenderStreamingSpriteLine(line, m_BackgroundFetchAbsoluteLine);
        return;
    }

    if (m_iMode == 1 || m_iMode == 3)
    {
        // The TMS9918 Text output is 40 × 6 pixels, centred in its regular
        // 256-pixel raster with an eight-pixel border at each side.
        for (int column = 0; column < 8; ++column)
            m_pStreamingBackground[lineOffset + column] =
                BackdropOf(register7[column]);
        for (int column = 248; column < GC_RESOLUTION_WIDTH; ++column)
            m_pStreamingBackground[lineOffset + column] =
                BackdropOf(register7[column]);

        for (int tileX = 0; tileX < 40; ++tileX)
        {
            const BackgroundFetchLatch& latch = m_BackgroundFetchLatches[tileX];
            if (!latch.nameValid || !latch.patternValid)
                return;

            for (int tilePixel = 0; tilePixel < 6; ++tilePixel)
            {
                const int column = 8 + tileX * 6 + tilePixel;
                m_pStreamingBackground[lineOffset + column] =
                    IsSetBit(latch.pattern, 7 - tilePixel)
                        ? ForegroundOf(register7[column])
                        : BackdropOf(register7[column]);
            }
        }
        m_StreamingBackgroundLineReady[line] = true;
        RenderStreamingSpriteLine(line, m_BackgroundFetchAbsoluteLine);
        return;
    }

    if (m_iMode == 4 || m_iMode == 6)
    {
        // Multicolor: the fetched byte is not a bit pattern but a pair of
        // colours, the high nibble for the left four pixels and the low nibble
        // for the right four. vdp18_pattern.vhd expresses it by forcing the
        // shift register to the constant 11110000 and feeding the byte in as
        // the colour register. There is no colour table in this mode, so the
        // colour latch is never filled and must not be waited for.
        for (int tileX = 0; tileX < 32; ++tileX)
        {
            const BackgroundFetchLatch& latch = m_BackgroundFetchLatches[tileX];
            if (!latch.nameValid || !latch.patternValid)
            {
                if (!allowPartial)
                    return;
                continue;
            }

            const int left = latch.pattern >> 4;
            const int right = latch.pattern & 0x0F;

            for (int tilePixel = 0; tilePixel < 4; ++tilePixel)
            {
                const int leftColumn = (tileX << 3) + tilePixel;
                const int rightColumn = leftColumn + 4;
                m_pStreamingBackground[lineOffset + leftColumn] = left > 0
                    ? left : BackdropOf(register7[leftColumn]);
                m_pStreamingBackground[lineOffset + rightColumn] = right > 0
                    ? right : BackdropOf(register7[rightColumn]);
            }
        }
        m_StreamingBackgroundLineReady[line] = true;
        RenderStreamingSpriteLine(line, m_BackgroundFetchAbsoluteLine);
        return;
    }

    for (int tileX = 0; tileX < 32; ++tileX)
    {
        const BackgroundFetchLatch& latch = m_BackgroundFetchLatches[tileX];
        if (!latch.nameValid || !latch.colourValid || !latch.patternValid)
        {
            // Normally an incomplete latch means the line is still being
            // fetched and there is nothing to draw yet. When the beam blanked
            // the line part way through, the remaining fetches will never
            // arrive: compose what was actually prefetched and let
            // ApplyStreamingMidLineBlanking paint the rest as backdrop.
            if (!allowPartial)
                return;
            continue;
        }

        const int foreground = latch.colour >> 4;
        const int background = latch.colour & 0x0F;

        for (int tilePixel = 0; tilePixel < 8; ++tilePixel)
        {
            const int column = (tileX << 3) + tilePixel;
            const int colour = IsSetBit(latch.pattern, 7 - tilePixel)
                ? foreground : background;
            m_pStreamingBackground[lineOffset + column] = colour > 0
                ? colour : BackdropOf(register7[column]);
        }
    }
    m_StreamingBackgroundLineReady[line] = true;
    RenderStreamingSpriteLine(line, m_BackgroundFetchAbsoluteLine);
}

TMS99xx::SpriteDebugSnapshot TMS99xx::GetSpriteDebugSnapshot(int line)
{
    SpriteDebugSnapshot snap;

    const int currentLine = GetRenderLine();
    snap.line = (line < 0) ? currentLine : line;
    snap.spritesEnabled = SpritesEnabledInMode();
    snap.magnified = IsSetBit(m_VdpRegister[1], 0);
    snap.height = IsSetBit(m_VdpRegister[1], 1) ? 16 : 8;
    // The last complete frame's figures, not the live counters: those reset
    // at the start of every frame, so a debugger paused in the VBlank would
    // read zeros for a frame that had plenty to report.
    snap.overflowLine = m_FrameRenderDiagnostics.streamingOverflowLine;
    snap.overflowSprite = m_FrameRenderDiagnostics.streamingOverflowSprite;
    snap.collisionLine = m_FrameRenderDiagnostics.streamingCollisionLine;
    snap.collisionDot = m_FrameRenderDiagnostics.streamingCollisionDot;
    snap.collisions = m_FrameRenderDiagnostics.streamingCollisions;

    // Only two lines are ever in flight, so per-line selection can only be
    // answered for the one the beam is on. Asking about any other line gets
    // the static half of the answer and lineStateValid false - better than a
    // confident wrong answer about which sprites were selected.
    const SpriteLineFetchState* state = nullptr;
    if (snap.line == currentLine)
    {
        state = FindSpriteLineFetchState(m_BackgroundFetchAbsoluteLine);
        snap.lineStateValid = (state != nullptr);
    }

    const u16 satBase = static_cast<u16>((m_VdpRegister[5] & 0x7F) << 7);

    for (int i = 0; i < 32; ++i)
    {
        const u16 base = static_cast<u16>((satBase + (i << 2)) & 0x3FFF);
        const u8 rawY = m_pVdpVRAM[base];

        // $D0 ends the list. Everything after it is not a sprite at all -
        // reporting those entries as sprites is how a debugger invents
        // objects the chip never looked at.
        if (rawY == 0xD0)
        {
            snap.terminatorIndex = i;
            break;
        }

        SpriteDebugEntry& e = snap.entries[snap.count];
        e.index = i;
        e.rawY = rawY;
        e.x = m_pVdpVRAM[(base + 1) & 0x3FFF];
        e.name = m_pVdpVRAM[(base + 2) & 0x3FFF];
        e.colour = m_pVdpVRAM[(base + 3) & 0x3FFF];
        e.visibleY = (rawY + 1) & 0xFF;
        e.earlyClock = (e.colour & 0x80) != 0;
        e.effectiveX = static_cast<int>(e.x) - (e.earlyClock ? 32 : 0);

        if (state != nullptr)
        {
            for (int s = 0; s < state->selectedCount && s < 4; ++s)
            {
                if (state->selectedSprite[s] == i)
                {
                    e.selectedOnLine = true;
                    break;
                }
            }
            e.overflowCause = state->overflowRecorded &&
                              state->overflowSprite == i;
        }

        ++snap.count;
    }

    return snap;
}

TMS99xx::SpritePipelineDebugSnapshot
TMS99xx::GetSpritePipelineDebugSnapshot() const
{
    SpritePipelineDebugSnapshot snap;
    const TMS99xxVramSequencer::Snapshot sequencer =
        m_VramSequencer.GetSnapshot();
    snap.rasterLine = m_iRenderLine;
    snap.absoluteRasterLine = sequencer.position.line;
    snap.phase = sequencer.position.phase;
    snap.dot = sequencer.position.phase / 2;
    snap.slot = TMS99xxSlotGrid::SlotAtPhase(sequencer.position.phase);

    const bool activeVideoLine = m_bDisplayEnabled &&
        m_iRenderLine < GC_RESOLUTION_HEIGHT;
    const TMS99xxVramSlotSchedule::Schedule schedule = activeVideoLine
        ? GetVideoVRAMSchedule()
        : TMS99xxVramSlotSchedule::Schedule::Refresh;
    const TMS99xxVramSlotSchedule::Slot current =
        TMS99xxVramSlotSchedule::GetSlot(schedule, snap.slot);
    snap.activity = current.activity;
    snap.activityIndex = current.index;
    snap.activityByte = current.byte;

    switch (current.activity)
    {
        case TMS99xxVramSlotSchedule::Activity::SpriteScanY:
        case TMS99xxVramSlotSchedule::Activity::SpriteSelectedY:
        case TMS99xxVramSlotSchedule::Activity::SpriteSelectedX:
        case TMS99xxVramSlotSchedule::Activity::SpriteSelectedName:
        case TMS99xxVramSlotSchedule::Activity::SpriteSelectedColour:
        case TMS99xxVramSlotSchedule::Activity::SpriteSelectedPattern:
            snap.activeTargetValid = true;
            snap.activeAbsoluteTargetLine =
                current.activity ==
                        TMS99xxVramSlotSchedule::Activity::SpriteScanY ||
                    snap.slot >= 157
                ? sequencer.position.line + 1
                : sequencer.position.line;
            snap.activeTargetLine = static_cast<int>(
                snap.activeAbsoluteTargetLine %
                static_cast<u64>(m_iLinesPerFrame));
            break;
        default:
            break;
    }
    snap.requestPending = m_HasPendingSpriteFetch;

    for (int lineIndex = 0; lineIndex < 2; ++lineIndex)
    {
        const SpriteLineFetchState& source =
            m_SpriteLineFetchStates[lineIndex];
        if (source.absoluteTargetLine == static_cast<u64>(-1))
            continue;
        SpritePipelineLineDebug& destination = snap.lines[lineIndex];
        destination.valid = true;
        destination.absoluteTargetLine = source.absoluteTargetLine;
        destination.targetLine = static_cast<int>(source.absoluteTargetLine %
            static_cast<u64>(m_iLinesPerFrame));
        destination.selectedCount = source.selectedCount;
        destination.selectionTerminated = source.selectionTerminated;
        destination.overflowRecorded = source.overflowRecorded;
        destination.overflowSprite = source.overflowRecorded
            ? source.overflowSprite : -1;
        destination.overflowRasterX = source.overflowRasterX;
        for (int byte = 0; byte < 4; ++byte)
            destination.overflowSat[byte] = source.overflowSat[byte];
        destination.overflowSatValid = source.overflowSatValid;
        destination.collisionX = source.collisionX;
        for (int selected = 0; selected < source.selectedCount && selected < 4;
             ++selected)
        {
            const SpriteFetchLatch& latch = source.sprite[selected];
            SpritePipelineLatchDebug& copy = destination.selected[selected];
            copy.satIndex = source.selectedSprite[selected];
            copy.rasterX = source.selectedRasterX[selected];
            copy.y = latch.y;
            copy.x = latch.x;
            copy.name = latch.name;
            copy.colour = latch.colour;
            copy.pattern[0] = latch.pattern[0];
            copy.pattern[1] = latch.pattern[1];
            copy.yValid = latch.yValid;
            copy.xValid = latch.xValid;
            copy.nameValid = latch.nameValid;
            copy.colourValid = latch.colourValid;
            copy.patternValid[0] = latch.patternValid[0];
            copy.patternValid[1] = latch.patternValid[1];
        }
    }
    return snap;
}

void TMS99xx::RenderStreamingSpriteLine(int line, u64 absoluteLine)
{
    // Parallel-only compositor. It deliberately does not set VDP status or
    // touch the presentation framebuffer: collision and overflow remain in
    // RenderSprites until the timed path is proven bit-identical.
    if (line < 0 || line >= GC_RESOLUTION_HEIGHT ||
        !m_StreamingBackgroundLineReady[line])
        return;

    const int lineOffset = line * GC_RESOLUTION_WIDTH;
    for (int pixel = 0; pixel < GC_RESOLUTION_WIDTH; ++pixel)
    {
        m_pStreamingComposed[lineOffset + pixel] =
            m_pStreamingBackground[lineOffset + pixel];
        m_pStreamingSpriteInfo[lineOffset + pixel] = 0;
    }

    m_RenderedSpriteDebugWorking.spritesEnabled = SpritesEnabledInMode();
    m_RenderedSpriteDebugWorking.large = IsSetBit(m_VdpRegister[1], 1);
    m_RenderedSpriteDebugWorking.magnified = IsSetBit(m_VdpRegister[1], 0);

    SpriteSelectionHistoryRow& compact =
        m_SpriteSelectionHistoryWorking.rows[line];
    compact = {};
    compact.flags = 0x01 |
        (SpritesEnabledInMode() ? 0x02 : 0) |
        (IsSetBit(m_VdpRegister[1], 1) ? 0x04 : 0) |
        (IsSetBit(m_VdpRegister[1], 0) ? 0x08 : 0);

    // M1 disables the sprite subsystem, so Text, Text with Graphics II
    // banking and the two fixed bar modes end with the background plane.
    if (!SpritesEnabledInMode())
    {
        m_StreamingSpriteLineReady[line] = true;
        return;
    }

    const SpriteLineFetchState* state =
        FindSpriteLineFetchState(absoluteLine);
    if (state == nullptr)
        return;

    RenderedSpriteScanlineDebug& scanline =
        m_RenderedSpriteDebugWorking.scanlines[line];
    scanline = {};
    scanline.valid = true;
    scanline.spritesEnabled = true;
    scanline.selectedCount = state->selectedCount;
    scanline.overflowRecorded = state->overflowRecorded;
    scanline.overflowSprite = state->overflowRecorded
        ? state->overflowSprite : -1;

    if (state->overflowRecorded)
    {
        compact.flags |= 0x10;
        compact.rasterX[4] = state->overflowRasterX;
        if (state->overflowSatValid)
            for (int byte = 0; byte < 4; ++byte)
                compact.sat[4][byte] = state->overflowSat[byte];
    }
    if (state->collisionX >= 0)
    {
        compact.flags |= 0x20;
        compact.collisionX = static_cast<u8>(state->collisionX);
    }

    for (u8 selected = 0; selected < state->selectedCount; ++selected)
    {
        const SpriteFetchLatch& latch = state->sprite[selected];
        RenderedSpriteScanlineSlot& captured = scanline.selected[selected];
        captured.satIndex = state->selectedSprite[selected];
        captured.y = latch.y;
        captured.x = latch.x;
        captured.name = latch.name;
        captured.colour = latch.colour;
        captured.pattern[0] = latch.pattern[0];
        captured.pattern[1] = latch.pattern[1];
        captured.fetchedMask =
            (latch.yValid ? 0x01 : 0) |
            (latch.xValid ? 0x02 : 0) |
            (latch.nameValid ? 0x04 : 0) |
            (latch.colourValid ? 0x08 : 0) |
            (latch.patternValid[0] ? 0x10 : 0) |
            (latch.patternValid[1] ? 0x20 : 0);
        compact.rasterX[selected] = state->selectedRasterX[selected];
        compact.sat[selected][0] = latch.yValid ? latch.y : 0xFF;
        compact.sat[selected][1] = latch.xValid ? latch.x : 0xFF;
        compact.sat[selected][2] = latch.nameValid ? latch.name : 0xFF;
        compact.sat[selected][3] = latch.colourValid ? latch.colour : 0xFF;
        compact.pattern[selected][0] = latch.patternValid[0]
            ? latch.pattern[0] : 0xFF;
        compact.pattern[selected][1] = latch.patternValid[1]
            ? latch.pattern[1] : 0xFF;
    }

    const bool large = IsSetBit(m_VdpRegister[1], 1);
    const bool zoomed = IsSetBit(m_VdpRegister[1], 0);
    int spriteSize = large ? 16 : 8;
    if (zoomed)
        spriteSize *= 2;

    for (u8 selected = 0; selected < state->selectedCount; ++selected)
    {
        const SpriteFetchLatch& latch = state->sprite[selected];
        RenderedSpriteScanlineSlot& scanlineSlot =
            scanline.selected[selected];
        if (!latch.yValid || !latch.xValid || !latch.nameValid ||
            !latch.colourValid || !latch.patternValid[0] ||
            (large && !latch.patternValid[1]))
        {
            return;
        }

        int spriteY = (latch.y + 1) & 0xFF;
        if (spriteY >= 0xE0)
            spriteY -= 0x100;
        if (spriteY > line || spriteY + spriteSize <= line)
            return;

        const int colour = latch.colour & 0x0F;

        // Preserve the source-sized row exactly as the timed VDP fetcher saw
        // it. A fifth sprite never reaches this loop, so its missing row stays
        // transparent in the rendered-frame debugger view.
        const int sourceY = (line - spriteY) >> (zoomed ? 1 : 0);
        const int spriteIndex = state->selectedSprite[selected];
        if (sourceY >= 0 && sourceY < 16 && spriteIndex < 32)
        {
            RenderedSpriteDebugEntry& captured =
                m_RenderedSpriteDebugWorking.entries[spriteIndex];
            if (!captured.seen)
            {
                captured.seen = true;
                captured.rawY = latch.y;
                captured.x = latch.x;
                captured.name = latch.name;
                captured.colour = latch.colour;
            }
            captured.fetchedRows |= static_cast<u16>(1u << sourceY);
            const int sourceWidth = large ? 16 : 8;
            for (int sourceX = 0; sourceX < sourceWidth; ++sourceX)
            {
                const u8 pattern = sourceX < 8
                    ? latch.pattern[0] : latch.pattern[1];
                const int bit = sourceX < 8 ? 7 - sourceX : 15 - sourceX;
                captured.pixels[sourceY * 16 + sourceX] =
                    (colour != 0 && IsSetBit(pattern, bit & 7))
                    ? static_cast<u8>(colour) : 0;
            }
        }

        int spriteX = latch.x;
        if (IsSetBit(latch.colour, 7))
            spriteX -= 32;
        if (spriteX >= GC_RESOLUTION_WIDTH)
            continue;

        for (int pixelX = 0; pixelX < spriteSize; ++pixelX)
        {
            const int x = spriteX + pixelX;
            if (x >= GC_RESOLUTION_WIDTH)
                break;
            if (x < 0)
                continue;

            const int patternX = pixelX >> (zoomed ? 1 : 0);
            const u8 pattern = patternX < 8 ? latch.pattern[0] : latch.pattern[1];
            const int bit = patternX < 8 ? 7 - patternX : 15 - patternX;
            if (!IsSetBit(pattern, bit & 7))
                continue;

            ++scanlineSlot.patternPixels;

            const int offset = lineOffset + x;
            if (!IsSetBit(m_pStreamingSpriteInfo[offset], 0) && colour != 0)
            {
                m_pStreamingComposed[offset] = colour;
                m_pStreamingSpriteInfo[offset] = SetBit(
                    m_pStreamingSpriteInfo[offset], 0);
                ++scanlineSlot.visiblePixels;
            }
            m_pStreamingSpriteInfo[offset] = SetBit(
                m_pStreamingSpriteInfo[offset], 1);
        }
    }

#ifdef SPRITE_EXPANDER
    if (m_bNoSpriteLimit)
        RenderExpandedSpriteLine(line, absoluteLine);
#endif

    m_StreamingSpriteLineReady[line] = true;
}

#ifdef SPRITE_EXPANDER
void TMS99xx::RenderExpandedSpriteLine(int line, u64 absoluteLine)
{
    const SpriteExpanderLineState* state =
        FindSpriteExpanderLineState(absoluteLine);
    if (state == nullptr || !state->latched)
        return;

    const int lineOffset = line * GC_RESOLUTION_WIDTH;
    const int spriteSize = (state->large ? 16 : 8) *
        (state->magnified ? 2 : 1);
    bool expandedOpaque[GC_RESOLUTION_WIDTH] = {};

    // SAT order still defines priority among the expanded sprites. Native
    // opaque pixels always win because bit 0 belongs exclusively to the real
    // four-sprite compositor and is only inspected here, never changed.
    for (u8 candidate = 0; candidate < state->candidateCount; ++candidate)
    {
        const SpriteFetchLatch& latch = state->sprite[candidate];
        if (!latch.yValid || !latch.xValid || !latch.nameValid ||
            !latch.colourValid || !latch.patternValid[0] ||
            (state->large && !latch.patternValid[1]))
            continue;

        int spriteY = (latch.y + 1) & 0xFF;
        if (spriteY >= 0xE0)
            spriteY -= 0x100;
        if (spriteY > line || spriteY + spriteSize <= line)
            continue;

        int spriteX = latch.x;
        if (IsSetBit(latch.colour, 7))
            spriteX -= 32;
        const int colour = latch.colour & 0x0F;

        for (int pixelX = 0; pixelX < spriteSize; ++pixelX)
        {
            const int x = spriteX + pixelX;
            if (x >= GC_RESOLUTION_WIDTH)
                break;
            if (x < 0)
                continue;

            const int patternX = pixelX >> (state->magnified ? 1 : 0);
            const u8 pattern = patternX < 8
                ? latch.pattern[0] : latch.pattern[1];
            const int bit = patternX < 8 ? 7 - patternX : 15 - patternX;
            if (!IsSetBit(pattern, bit & 7))
                continue;

            const int offset = lineOffset + x;
            if (!IsSetBit(m_pStreamingSpriteInfo[offset], 0) &&
                !expandedOpaque[x] && colour != 0)
            {
                m_pStreamingComposed[offset] = colour;
                expandedOpaque[x] = true;
            }
        }
    }
}
#endif

#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
void TMS99xx::RenderSprites(int line)
{
    int sprite_count = 0;
    int line_width = line * GC_RESOLUTION_WIDTH;
    int sprite_size = IsSetBit(m_VdpRegister[1], 1) ? 16 : 8;
    bool sprite_zoom = IsSetBit(m_VdpRegister[1], 0);
    if (sprite_zoom)
        sprite_size *= 2;
    u16 sprite_attribute_addr = (m_VdpRegister[5] & 0x7F) << 7;
    u16 sprite_pattern_addr = (m_VdpRegister[6] & 0x07) << 11;

    int max_sprite = 31;

    for (int sprite = 0; sprite <= max_sprite; sprite++)
    {
        if (m_pVdpVRAM[sprite_attribute_addr + (sprite << 2)] == 0xD0)
        {
            max_sprite = sprite - 1;
            break;
        }
    }

    for (int sprite = 0; sprite <= max_sprite; sprite++)
    {
        int sprite_attribute_offset = sprite_attribute_addr + (sprite << 2);
        int sprite_y = (m_pVdpVRAM[sprite_attribute_offset] + 1) & 0xFF;

        if (sprite_y >= 0xE0)
            sprite_y = -(0x100 - sprite_y);

        if ((sprite_y > line) || ((sprite_y + sprite_size) <= line))
            continue;

        sprite_count++;
        if (sprite_count == 5 && m_LegacyOverflowLine < 0)
        {
            m_LegacyOverflowLine = line;
            m_LegacyOverflowSprite = sprite;
        }
        if (!IsSetBit(m_VdpStatus, 6) && (sprite_count > 4) &&
            !IsStreamingStatusAuthoritative())
        {
            SetStatus((SetBit(m_VdpStatus, 6) & 0xE0) | sprite,
                DebugEventSource::DeviceInternal);
        }

        int sprite_color = m_pVdpVRAM[sprite_attribute_offset + 3] & 0x0F;

        // No early exit on a transparent sprite. vdp18_sprite.vhd counts a
        // sprite towards a collision on its pattern bit alone, so a colour 0
        // sprite still collides; only the drawing below is skipped. Programs
        // use the collision bit as a horizontal timing reference and place
        // invisible sprites for it.

        int sprite_shift = (m_pVdpVRAM[sprite_attribute_offset + 3] & 0x80) ? 32 : 0;
        int sprite_x = m_pVdpVRAM[sprite_attribute_offset + 1] - sprite_shift;

        if (sprite_x >= GC_RESOLUTION_WIDTH)
            continue;

        int sprite_tile = m_pVdpVRAM[sprite_attribute_offset + 2];
        sprite_tile &= IsSetBit(m_VdpRegister[1], 1) ? 0xFC : 0xFF;

        int sprite_line_addr = sprite_pattern_addr + (sprite_tile << 3) + ((line - sprite_y ) >> (sprite_zoom ? 1 : 0));

        for (int tile_x = 0; tile_x < sprite_size; tile_x++)
        {
            int sprite_pixel_x = sprite_x + tile_x;
            if (sprite_pixel_x >= GC_RESOLUTION_WIDTH)
                break;
            if (sprite_pixel_x < 0)
                continue;

            int pixel = line_width + sprite_pixel_x;

            bool sprite_pixel = false;

            int tile_x_adjusted = tile_x >> (sprite_zoom ? 1 : 0);

            if (tile_x_adjusted < 8)
                sprite_pixel = IsSetBit(m_pVdpVRAM[sprite_line_addr], 7 - tile_x_adjusted);
            else
                sprite_pixel = IsSetBit(m_pVdpVRAM[sprite_line_addr + 16], 15 - tile_x_adjusted);

            if (sprite_pixel && sprite_count < 5)
            {
                if (!IsSetBit(m_pInfoBuffer[pixel], 0) && (sprite_color > 0))
                {
                    m_pFrameBuffer[pixel] = sprite_color;
                    m_pInfoBuffer[pixel] = SetBit(m_pInfoBuffer[pixel], 0);
                }

                if (IsSetBit(m_pInfoBuffer[pixel], 1))
                {
                    ++m_LegacyCollisions;
                    if (!IsStreamingStatusAuthoritative())
                        SetStatus(SetBit(m_VdpStatus, 5),
                            DebugEventSource::DeviceInternal);
                }
                else
                {
                    m_pInfoBuffer[pixel] = SetBit(m_pInfoBuffer[pixel], 1);
                }
            }
        }
    }
}
#endif // GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER

void TMS99xx::BeginRegister7Line(int line)
{
    if (line < 0 || line >= GC_RESOLUTION_HEIGHT)
        return;
    Register7Timeline& timeline = m_Register7Line[line];
    timeline.start = m_VdpRegister[7];
    timeline.count = 0;
}

void TMS99xx::RecordRegister7Write(u8 value)
{
    const int line = m_iRenderLine;
    if (line < 0 || line >= GC_RESOLUTION_HEIGHT)
        return;

    Register7Timeline& timeline = m_Register7Line[line];
    const u16 column = static_cast<u16>(GetColumn());

    // Two writes inside the same dot are one cut: the second value is what
    // that dot actually emitted.
    if (timeline.count > 0 && timeline.column[timeline.count - 1] == column)
    {
        timeline.value[timeline.count - 1] = value;
        return;
    }
    // Past the fourth cut the line keeps the newest colour rather than the
    // oldest. Four is already far more than a split screen needs; a program
    // that beats R7 harder than this is chasing a per-dot effect the rest of
    // this model does not resolve anyway.
    if (timeline.count == kRegister7CutsPerLine)
    {
        timeline.value[timeline.count - 1] = value;
        return;
    }
    timeline.column[timeline.count] = column;
    timeline.value[timeline.count] = value;
    ++timeline.count;
}

u8 TMS99xx::Register7At(int line, int column) const
{
    if (line < 0 || line >= GC_RESOLUTION_HEIGHT)
        return m_VdpRegister[7];

    const Register7Timeline& timeline = m_Register7Line[line];
    u8 value = timeline.start;
    for (int cut = 0; cut < timeline.count; ++cut)
    {
        if (column < timeline.column[cut])
            break;
        value = timeline.value[cut];
    }
    return value;
}

u8 TMS99xx::Register7AtLineEnd(int line) const
{
    return Register7At(line, TMS99xxRasterTiming::DotsPerLine);
}

// Same lookup addressed by raster dot instead of active-window column, for the
// beam-order outputs. Dots before the active window are the line's left
// border: the beam passes them before any of its picture, so they carry the
// colour the line started with.
u8 TMS99xx::Register7AtRasterDot(int line, int dot, int activeStart) const
{
    const int column = dot - activeStart;
    if (column >= 0)
        return Register7At(line, column);
    if (line >= 0 && line < GC_RESOLUTION_HEIGHT)
        return m_Register7Line[line].start;
    return m_VdpRegister[7];
}

// Expands the timeline into one value per active dot, so the renderers can
// index it instead of walking the cut list for every pixel.
void TMS99xx::FillRegister7Row(int line, u8* row) const
{
    if (line < 0 || line >= GC_RESOLUTION_HEIGHT)
    {
        for (int column = 0; column < GC_RESOLUTION_WIDTH; ++column)
            row[column] = m_VdpRegister[7];
        return;
    }

    const Register7Timeline& timeline = m_Register7Line[line];
    // The overwhelmingly common case: R7 was not touched while the beam was
    // inside this line, so the whole row is one value. Keeping this fast means
    // the per-dot backdrop costs a memset per line instead of a walk, and an
    // ordinary frame pays about 3 us more than a per-line backdrop would.
    if (timeline.count == 0)
    {
        std::memset(row, timeline.start, GC_RESOLUTION_WIDTH);
        return;
    }

    u8 value = timeline.start;
    int cut = 0;
    for (int column = 0; column < GC_RESOLUTION_WIDTH; ++column)
    {
        while (cut < timeline.count && column >= timeline.column[cut])
            value = timeline.value[cut++];
        row[column] = value;
    }
}

// Border colour for one pixel of the output frame, by where the beam was when
// it emitted that pixel. A row's left margin leaves the chip before any of its
// active dots, so it carries the colour the line started with; the right
// margin leaves after all of them, so it carries the colour the line ended
// with. On a line that is cut part way through, those two are different, and
// that is the whole point: the border changes colour on the same line the
// picture does, at the side the beam had already passed.
int TMS99xx::BorderColorForRow(const GC_VideoFrameDescriptor& descriptor,
    int row, bool leading) const
{
    const int top = descriptor.content_area.y;
    const int bottom = top + descriptor.content_area.height;

    // Above the picture the beam has not reached line 0 yet; below it, it has
    // left the last line behind.
    if (row < top)
        return BackdropOf(m_Register7Line[0].start);
    if (row >= bottom)
        return BackdropOf(Register7AtLineEnd(GC_RESOLUTION_HEIGHT - 1));

    int line = row - top;
    if (line >= GC_RESOLUTION_HEIGHT)
        line = GC_RESOLUTION_HEIGHT - 1;

    // The right margin leaves the chip just after the last active dot, not at
    // the end of the whole line: a write that lands later - in the blanking,
    // which is exactly where a split screen wants it - belongs to the next
    // row, not to this row's border.
    return leading ? BackdropOf(m_Register7Line[line].start)
                   : BackdropOf(Register7At(line, GC_RESOLUTION_WIDTH));
}

void TMS99xx::Render24bit(u16* srcFrameBuffer, u8* dstFrameBuffer, GC_Color_Format pixelFormat, int size, bool overscan)
{
    int x = 0;
    int y = 0;
    const GC_VideoFrameDescriptor descriptor = GetFrameDescriptor();
    const bool overscan_enabled = overscan && (m_Overscan != OverscanDisabled);
    int border_leading = BorderColorForRow(descriptor, 0, true) * 3;
    int border_trailing = BorderColorForRow(descriptor, 0, false) * 3;
    int buffer_size = size * 3;
    bool bgr = (pixelFormat == GC_PIXEL_BGR888);

    for (int i = 0, j = 0; j < buffer_size; j += 3)
    {
        u16 src_color = 0;
        if (overscan_enabled)
        {
            const bool is_h_overscan = x < descriptor.content_area.x ||
                x >= descriptor.content_area.x + descriptor.content_area.width;
            const bool is_v_overscan = y < descriptor.content_area.y ||
                y >= descriptor.content_area.y + descriptor.content_area.height;

            if (is_h_overscan || is_v_overscan)
                src_color = (x < descriptor.content_area.x) ? border_leading
                                                           : border_trailing;
            else
                src_color = srcFrameBuffer[i++] * 3;

            if (++x == descriptor.frame_width)
            {
                x = 0;
                if (++y == descriptor.frame_height)
                {
                    y = 0;
                }
                border_leading = BorderColorForRow(descriptor, y, true) * 3;
                border_trailing = BorderColorForRow(descriptor, y, false) * 3;
            }
        }
        else
            src_color = srcFrameBuffer[i++] * 3;

        dstFrameBuffer[j + 0] = bgr ? m_pCurrentPalette[src_color + 2] : m_pCurrentPalette[src_color];
        dstFrameBuffer[j + 1] = m_pCurrentPalette[src_color + 1];
        dstFrameBuffer[j + 2] = bgr ? m_pCurrentPalette[src_color] : m_pCurrentPalette[src_color + 2];
    }
}

void TMS99xx::Render16bit(u16* srcFrameBuffer, u8* dstFrameBuffer, GC_Color_Format pixelFormat, int size, bool overscan)
{
    int x = 0;
    int y = 0;
    const GC_VideoFrameDescriptor descriptor = GetFrameDescriptor();
    const bool overscan_enabled = overscan && (m_Overscan != OverscanDisabled);
    int border_leading = BorderColorForRow(descriptor, 0, true);
    int border_trailing = BorderColorForRow(descriptor, 0, false);
    int buffer_size = size * 2;
    bool bgr = ((pixelFormat == GC_PIXEL_BGR555) || (pixelFormat == GC_PIXEL_BGR565));
    bool green_6bit = (pixelFormat == GC_PIXEL_RGB565) || (pixelFormat == GC_PIXEL_BGR565);
    const u16* pal;

    if (bgr)
        pal = green_6bit ? m_palette_565_bgr : m_palette_555_bgr;
    else
        pal = green_6bit ? m_palette_565_rgb : m_palette_555_rgb;

    for (int i = 0, j = 0; j < buffer_size; j += 2)
    {
        u16 src_color = 0;
        if (overscan_enabled)
        {
            const bool is_h_overscan = x < descriptor.content_area.x ||
                x >= descriptor.content_area.x + descriptor.content_area.width;
            const bool is_v_overscan = y < descriptor.content_area.y ||
                y >= descriptor.content_area.y + descriptor.content_area.height;

            if (is_h_overscan || is_v_overscan)
                src_color = (x < descriptor.content_area.x) ? border_leading
                                                           : border_trailing;
            else
                src_color = srcFrameBuffer[i++];

            if (++x == descriptor.frame_width)
            {
                x = 0;
                if (++y == descriptor.frame_height)
                {
                    y = 0;
                }
                border_leading = BorderColorForRow(descriptor, y, true);
                border_trailing = BorderColorForRow(descriptor, y, false);
            }
        }
        else
            src_color = srcFrameBuffer[i++];

        *(u16*)(&dstFrameBuffer[j]) = pal[src_color];
    }
}

void TMS99xx::SetPredefinedPalette(int palette)
{
    const u8* predefined;

    switch (palette)
    {
        case 0:
            predefined = kPalette_888_coleco;
            break;
        case 1:
            predefined = kPalette_888_tms9918;
            break;
        case 2:
            predefined = kPalette_888_tms9918_analog;
            break;
        case 4:
            predefined = kPalette_888_sms_vdp_tms;
            break;
        default:
            predefined = NULL;
    }

    if (IsValidPointer(predefined))
    {
        if (m_pCurrentPalette == predefined && m_ColorProfile == palette)
            return;
        m_pCurrentPalette = predefined;
        m_ColorProfile = palette;
        ++m_PaletteRevision;
        m_SpritePatternDebugDirty = true;
        ++m_DebugPreviewSourceGeneration;
        InitPalettes();
    }
}

void TMS99xx::InitPalettes()
{
    for (int i=0,j=0; i<16; i++,j+=3)
    {
        u8 red = m_pCurrentPalette[j];
        u8 green = m_pCurrentPalette[j+1];
        u8 blue = m_pCurrentPalette[j+2];

        u8 red_5 = red * 31 / 255;
        u8 green_5 = green * 31 / 255;
        u8 green_6 = green * 63 / 255;
        u8 blue_5 = blue * 31 / 255;

        m_palette_565_rgb[i] = red_5 << 11 | green_6 << 5 | blue_5;
        m_palette_555_rgb[i] = red_5 << 10 | green_5 << 5 | blue_5;
        m_palette_565_bgr[i] = blue_5 << 11 | green_6 << 5 | red_5;
        m_palette_555_bgr[i] = blue_5 << 10 | green_5 << 5 | red_5;
    }
}

void TMS99xx::SetOverscan(Overscan overscan)
{
    if (m_Overscan == overscan)
        return;

    m_Overscan = overscan;
    ++m_FrameDescriptorRevision;
}

TMS99xx::Overscan TMS99xx::GetOverscan()
{
    return m_Overscan;
}

GC_VideoFrameDescriptor TMS99xx::GetFrameDescriptor() const
{
    GC_VideoFrameDescriptor descriptor;
    descriptor.device_key = GetDeviceDescriptor().key;
    descriptor.mode_id = static_cast<u32>(m_iMode);

    // Full Frame debug: the packed frame *is* the raw 342-dot raster, not
    // the cropped picture. No border math applies here - content_area and
    // visible_area cover the whole thing, since the point is to see the
    // sync/blank/burst/border regions that overscan normally crops away.
    if (m_bFullRasterDebugEnabled)
    {
        descriptor.surface_kind = GC_VIDEO_SURFACE_FULL_RASTER_DEBUG;
        descriptor.postprocessing_capabilities =
            GC_VIDEO_POSTPROCESSING_CAPABILITY_SPATIAL_SCALING;
        const int rasterHeight = m_bPAL ? GC_LINES_PER_FRAME_PAL
                                        : GC_LINES_PER_FRAME_NTSC;
        descriptor.buffer_width = TMS99xxRasterTiming::DotsPerLine;
        descriptor.buffer_height = kFullRasterDebugMaxLines;
        descriptor.frame_width = TMS99xxRasterTiming::DotsPerLine;
        descriptor.frame_height = rasterHeight;
        descriptor.stride_pixels = descriptor.frame_width;
        descriptor.content_area = {0, 0, descriptor.frame_width, descriptor.frame_height};
        descriptor.visible_area = {0, 0, descriptor.frame_width, descriptor.frame_height};
        descriptor.raster_width = TMS99xxRasterTiming::DotsPerLine;
        descriptor.raster_height = rasterHeight;
        descriptor.logical_width = GC_RESOLUTION_WIDTH;
        descriptor.logical_height = GC_RESOLUTION_HEIGHT;
        descriptor.presentation_width = descriptor.visible_area.width;
        descriptor.presentation_height = descriptor.visible_area.height;
        descriptor.device_clock_hz = GC_VDP_MASTER_CLOCK_HZ;
        descriptor.refresh_rate = {
            GC_VDP_MASTER_CLOCK_HZ,
            GC_VDP_MASTER_CLOCKS_PER_LINE * rasterHeight};
        descriptor.pixel_aspect_ratio = {1, 1};
        descriptor.signal = GetDeviceDescriptor().native_signal;
        descriptor.region = m_bPAL ? Region_PAL : Region_NTSC;
        descriptor.revision = m_FrameDescriptorRevision;
        return descriptor;
    }

    // Capacity is stable for the lifetime of this TMS99xx instance and covers
    // every ordinary border setting plus the optional full-raster diagnostic
    // surface.  frame_width/frame_height below still describe only what this
    // mode actually produced.  Frontends can therefore allocate once and
    // change only the valid rectangle when the mode or border option changes.
    descriptor.buffer_width = TMS99xxRasterTiming::DotsPerLine;
    descriptor.buffer_height = kFullRasterDebugMaxLines;
    descriptor.frame_width = GC_RESOLUTION_WIDTH;
    descriptor.frame_height = GC_RESOLUTION_HEIGHT;
    descriptor.content_area = {0, 0, GC_RESOLUTION_WIDTH, GC_RESOLUTION_HEIGHT};
    descriptor.postprocessing_capabilities =
        GC_VIDEO_POSTPROCESSING_CAPABILITY_SPATIAL_SCALING |
        GC_VIDEO_POSTPROCESSING_CAPABILITY_SCANLINE_CRT;

    int borderLeft = 0;
    int borderRight = 0;
    int borderVertical = 0;

    if (m_Overscan != OverscanDisabled)
        borderVertical = m_bPAL ? GC_RESOLUTION_OVERSCAN_V_PAL
                                : GC_RESOLUTION_OVERSCAN_V;

    switch (m_Overscan)
    {
        case OverscanFull272:
            borderLeft = GC_RESOLUTION_SMS_OVERSCAN_H_272_L;
            borderRight = GC_RESOLUTION_SMS_OVERSCAN_H_272_R;
            break;
        case OverscanFull284:
            borderLeft = GC_RESOLUTION_SMS_OVERSCAN_H_284_L;
            borderRight = GC_RESOLUTION_SMS_OVERSCAN_H_284_R;
            break;
        case OverscanFull320:
            borderLeft = GC_RESOLUTION_SMS_OVERSCAN_H_320_L;
            borderRight = GC_RESOLUTION_SMS_OVERSCAN_H_320_R;
            break;
        case OverscanDisabled:
        case OverscanTopBottom:
            break;
    }

    descriptor.frame_width += borderLeft + borderRight;
    descriptor.frame_height += borderVertical * 2;
    descriptor.stride_pixels = descriptor.frame_width;
    descriptor.content_area.x = borderLeft;
    descriptor.content_area.y = borderVertical;
    descriptor.visible_area = {0, 0, descriptor.frame_width, descriptor.frame_height};

    descriptor.raster_width = 342;
    descriptor.raster_height = m_bPAL ? GC_LINES_PER_FRAME_PAL
                                      : GC_LINES_PER_FRAME_NTSC;
    descriptor.logical_width = GC_RESOLUTION_WIDTH;
    descriptor.logical_height = GC_RESOLUTION_HEIGHT;
    descriptor.presentation_width = descriptor.visible_area.width;
    descriptor.presentation_height = descriptor.visible_area.height;
    descriptor.device_clock_hz = GC_VDP_MASTER_CLOCK_HZ;
    descriptor.refresh_rate = {
        GC_VDP_MASTER_CLOCK_HZ,
        GC_VDP_MASTER_CLOCKS_PER_LINE * descriptor.raster_height};
    descriptor.pixel_aspect_ratio = {1, 1};
    descriptor.signal = GetDeviceDescriptor().native_signal;
    descriptor.region = m_bPAL ? Region_PAL : Region_NTSC;
    descriptor.revision = m_FrameDescriptorRevision;

    return descriptor;
}

GC_VideoFrameView TMS99xx::GetFrameView(GC_Color_Format pixelFormat)
{
    GC_VideoFrameView view;
    view.descriptor = GetFrameDescriptor();
    view.pixel_format = pixelFormat;
    const bool packed24 = pixelFormat == GC_PIXEL_RGB888 ||
        pixelFormat == GC_PIXEL_BGR888;
    view.row_stride_bytes = view.descriptor.frame_width * (packed24 ? 3 : 2);
    view.frame_serial = GetFrameSerial();

    if (m_bFullRasterDebugEnabled)
    {
        view.pixel_format = GC_PIXEL_RGB888;
        view.row_stride_bytes = view.descriptor.frame_width * 3;
        view.pixels = m_pFullRasterDebugRGB;
        return view;
    }

    const bool needsRepack =
        m_PresentedSourceRevision != m_PresentationSourceRevision ||
        m_PresentedDescriptorRevision != view.descriptor.revision ||
        m_PresentedPaletteRevision != m_PaletteRevision ||
        m_PresentedPixelFormat != static_cast<int>(pixelFormat);
    if (needsRepack)
    {
        const int pixelCount = view.descriptor.frame_width *
            view.descriptor.frame_height;
        if (packed24)
            Render24bit(GetFrameBuffer(), m_pPresentedFrame, pixelFormat,
                pixelCount, true);
        else
            Render16bit(GetFrameBuffer(), m_pPresentedFrame, pixelFormat,
                pixelCount, true);
        m_PresentedSourceRevision = m_PresentationSourceRevision;
        m_PresentedDescriptorRevision = view.descriptor.revision;
        m_PresentedPaletteRevision = m_PaletteRevision;
        m_PresentedPixelFormat = static_cast<int>(pixelFormat);
    }
    view.pixels = m_pPresentedFrame;
    return view;
}



void TMS99xx::SaveState(std::ostream& stream)
{
    StateWriter w(stream);

    w.U16(kVideoStateVersion);

    // ---- memories -------------------------------------------------------
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    w.Bytes(m_pInfoBuffer, GC_RESOLUTION_WIDTH * GC_LINES_PER_FRAME_PAL);
#endif
    w.Bytes(m_pVdpVRAM, 0x4000);

    // ---- programmer-visible VDP ----------------------------------------
    w.Bool(m_bFirstByteInSequence);
    w.U8(m_ControlLatchValue);
    w.Bytes(m_VdpRegister, sizeof(m_VdpRegister));
    w.U8(m_VdpBuffer);
    w.U16(m_VdpAddress);
    w.U8(m_VdpStatus);
    w.Bool(m_bDisplayEnabled);
    w.Bool(m_bSpriteOvrRequest);

    // ---- raster position and frame timing -------------------------------
    w.I32(m_iCycleCounter);
    w.I32(m_iRenderLine);
    w.I32(m_iLinesPerFrame);
    w.Bool(m_bPAL);
    w.I32(m_iMode);
    w.U64(m_iTstates);
    for (int i = 0; i < 3; ++i)
        w.I32(m_Timing[i]);
    w.Bool(m_LineEvents.vint);
    w.Bool(m_LineEvents.render);
    w.Bool(m_LineEvents.display);
    w.Bool(m_LineEvents.collision);

    // ---- interrupt gating -----------------------------------------------
    w.Bool(m_SuppressVBlankFlag);
    w.U32(m_SuppressedVBlankFlags);

    // ---- VRAM access arbitration ----------------------------------------
    m_VramSequencer.SaveState(stream);
    w.U64(m_LastCpuPortAccessTStates);
    w.Bool(m_HasLastCpuPortAccess);

    // ---- background fetch pipeline --------------------------------------
    // The beam can be part way through a character's name/colour/pattern
    // triplet, so the half-filled latches are part of the instant.
    for (const BackgroundFetchLatch& latch : m_BackgroundFetchLatches)
    {
        w.U8(latch.name);
        w.U8(latch.colour);
        w.U8(latch.pattern);
        w.Bool(latch.nameValid);
        w.Bool(latch.colourValid);
        w.Bool(latch.patternValid);
    }
    w.I32(m_BackgroundFetchLine);
    w.U64(m_BackgroundFetchAbsoluteLine);
    w.I32(static_cast<std::int32_t>(m_PendingBackgroundFetchActivity));
    w.U8(m_PendingBackgroundFetchIndex);
    w.Bool(m_HasPendingBackgroundFetch);

    // ---- sprite selection and fetch pipeline ----------------------------
    // Sprite work straddles the line boundary by design, so both the line
    // being scanned and the line being fetched for are live at once.
    for (const SpriteLineFetchState& line : m_SpriteLineFetchStates)
    {
        w.U64(line.absoluteTargetLine);
        w.Bytes(line.selectedSprite, sizeof(line.selectedSprite));
        w.U8(line.selectedCount);
        w.Bool(line.selectionTerminated);
        w.Bool(line.overflowRecorded);
        w.U8(line.overflowSprite);
        w.Bytes(line.selectedRasterX, sizeof(line.selectedRasterX));
        w.U8(line.overflowRasterX);
        w.Bytes(line.overflowSat, sizeof(line.overflowSat));
        w.Bool(line.overflowSatValid);
        w.I32(line.collisionX);
        for (const SpriteFetchLatch& sprite : line.sprite)
        {
            w.U8(sprite.y);
            w.U8(sprite.x);
            w.U8(sprite.name);
            w.U8(sprite.colour);
            w.U8(sprite.pattern[0]);
            w.U8(sprite.pattern[1]);
            w.Bool(sprite.yValid);
            w.Bool(sprite.xValid);
            w.Bool(sprite.nameValid);
            w.Bool(sprite.colourValid);
            w.Bool(sprite.patternValid[0]);
            w.Bool(sprite.patternValid[1]);
        }
    }
    w.I32(static_cast<std::int32_t>(m_PendingSpriteFetch.activity));
    w.U64(m_PendingSpriteFetch.absoluteTargetLine);
    w.U8(m_PendingSpriteFetch.index);
    w.U8(m_PendingSpriteFetch.byte);
    w.Bool(m_PendingSpriteFetch.candidateScan);
    w.Bool(m_HasPendingSpriteFetch);

#ifdef SPRITE_EXPANDER
    // Visual-only, but still part of the exact mid-scanline instant restored
    // by rewind: dropping these latches could omit expanded pixels from the
    // first line rendered after loading a state.
    for (const SpriteExpanderLineState& line : m_SpriteExpanderLineStates)
    {
        w.U64(line.absoluteTargetLine);
        w.U8(line.candidateCount);
        w.Bytes(line.satIndex, sizeof(line.satIndex));
        w.Bool(line.latched);
        w.Bool(line.large);
        w.Bool(line.magnified);
        for (const SpriteFetchLatch& sprite : line.sprite)
        {
            w.U8(sprite.y);
            w.U8(sprite.x);
            w.U8(sprite.name);
            w.U8(sprite.colour);
            w.U8(sprite.pattern[0]);
            w.U8(sprite.pattern[1]);
            w.Bool(sprite.yValid);
            w.Bool(sprite.xValid);
            w.Bool(sprite.nameValid);
            w.Bool(sprite.colourValid);
            w.Bool(sprite.patternValid[0]);
            w.Bool(sprite.patternValid[1]);
        }
    }
#endif

    // ---- overflow and collision ------------------------------------------
    // These decide when bits 5 and 6 of the status register are raised, so
    // they are machine state rather than statistics.
    w.I32(m_StreamingCollisionPhase);
    w.I32(m_StreamingCollisionLine);
    w.I32(m_StreamingOverflowLine);
    w.I32(m_StreamingOverflowSprite);
    w.U32(m_StreamingCollisions);
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    w.I32(m_LegacyOverflowLine);
    w.I32(m_LegacyOverflowSprite);
    w.U32(m_LegacyCollisions);
#endif

    // ---- mid-line backdrop timeline --------------------------------------
    // Written by the beam as R7 changes arrive and consumed at line end. A
    // load part way down a frame would otherwise repaint the lines already
    // behind the beam with the wrong border colour for one frame.
    for (const Register7Timeline& line : m_Register7Line)
    {
        w.U8(line.start);
        w.U8(line.count);
        for (int cut = 0; cut < kRegister7CutsPerLine; ++cut)
        {
            w.U16(line.column[cut]);
            w.U8(line.value[cut]);
        }
    }

    // ---- per-slot history of the line in progress ------------------------
    for (const SlotActivityRecord& slot : m_LineSlotHistory)
    {
        w.I32(static_cast<std::int32_t>(slot.activity));
        w.U8(slot.index);
        w.Bool(slot.cpuTransacted);
        w.Bool(slot.cpuWrite);
        w.U16(slot.cpuAddress);
        w.U8(slot.cpuValue);
    }

    // ---- counters reported over MCP --------------------------------------
    // Saved so that a figure read after a rewind matches the figure that was
    // read live at the same frame.
    w.U32(m_VisibleLineCpuVramWrites);
    w.U32(m_VisibleLineVdpRegisterWrites);
    w.U32(m_CompletedBackgroundFetches);
    w.U32(m_CompletedSpriteFetches);
    w.U32(m_CompletedSpriteSelectionScans);
    w.U64(m_FrameRenderDiagnostics.frameSerial);

    // Recorder history: 35 bytes per PAL raster line, including the four
    // winners, the fifth candidate, actual pattern rows and collision state.
    w.Bool(m_LastSpriteSelectionHistory.valid);
    w.U64(m_LastSpriteSelectionHistory.frameSerial);
    w.I32(m_LastSpriteSelectionHistory.rasterLines);
    for (const SpriteSelectionHistoryRow& row :
         m_LastSpriteSelectionHistory.rows)
    {
        w.Bytes(row.rasterX, sizeof(row.rasterX));
        w.Bytes(row.sat, sizeof(row.sat));
        w.Bytes(row.pattern, sizeof(row.pattern));
        w.U8(row.flags);
        w.U8(row.collisionX);
    }

    // Deliberately absent: frame buffers, per-line ready flags and the
    // comparison buffers, which are rebuilt from the state above; and the
    // palettes, overscan, sprite-limit and CRT sink, which are settings owned
    // by the user rather than by the machine. Restoring those from a snapshot
    // would let a rewind silently change how the emulator is configured.
}

u16 TMS99xx::GetStateFormatVersion() const
{
    return kVideoStateVersion;
}

void TMS99xx::LoadState(std::istream& stream)
{
    StateReader r(stream);

    const std::uint16_t version = r.U16();
    if (version < 1 || version > kVideoStateVersion)
        return;

#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    r.Bytes(m_pInfoBuffer, GC_RESOLUTION_WIDTH * GC_LINES_PER_FRAME_PAL);
#endif
    r.Bytes(m_pVdpVRAM, 0x4000);

    m_bFirstByteInSequence = r.Bool();
    m_ControlLatchValue = r.U8();
    r.Bytes(m_VdpRegister, sizeof(m_VdpRegister));
    m_VdpBuffer = r.U8();
    m_VdpAddress = r.U16();
    m_VdpStatus = r.U8();
    m_bDisplayEnabled = r.Bool();
    m_bSpriteOvrRequest = r.Bool();

    m_iCycleCounter = r.I32();
    m_iRenderLine = r.I32();
    r.I32(); // legacy serialized line count; the concrete chip owns it now
    r.Bool(); // legacy PAL flag; consumed without changing device identity
    m_iLinesPerFrame = m_bPAL ? GC_LINES_PER_FRAME_PAL
                              : GC_LINES_PER_FRAME_NTSC;
    m_iMode = r.I32();
    m_iTstates = r.U64();
    for (int i = 0; i < 3; ++i)
        m_Timing[i] = r.I32();
    m_LineEvents.vint = r.Bool();
    m_LineEvents.render = r.Bool();
    m_LineEvents.display = r.Bool();
    m_LineEvents.collision = r.Bool();

    m_SuppressVBlankFlag = r.Bool();
    m_SuppressedVBlankFlags = r.U32();

    m_VramSequencer.LoadState(stream);
    m_LastCpuPortAccessTStates = r.U64();
    m_HasLastCpuPortAccess = r.Bool();

    for (BackgroundFetchLatch& latch : m_BackgroundFetchLatches)
    {
        latch.name = r.U8();
        latch.colour = r.U8();
        latch.pattern = r.U8();
        latch.nameValid = r.Bool();
        latch.colourValid = r.Bool();
        latch.patternValid = r.Bool();
    }
    m_BackgroundFetchLine = r.I32();
    m_BackgroundFetchAbsoluteLine = r.U64();
    m_PendingBackgroundFetchActivity =
        static_cast<TMS99xxVramSlotSchedule::Activity>(r.I32());
    m_PendingBackgroundFetchIndex = r.U8();
    m_HasPendingBackgroundFetch = r.Bool();

    for (SpriteLineFetchState& line : m_SpriteLineFetchStates)
    {
        line.absoluteTargetLine = r.U64();
        r.Bytes(line.selectedSprite, sizeof(line.selectedSprite));
        line.selectedCount = r.U8();
        line.selectionTerminated = r.Bool();
        line.overflowRecorded = r.Bool();
        line.overflowSprite = r.U8();
        if (version >= 2)
        {
            r.Bytes(line.selectedRasterX, sizeof(line.selectedRasterX));
            line.overflowRasterX = r.U8();
            r.Bytes(line.overflowSat, sizeof(line.overflowSat));
            line.overflowSatValid = r.Bool();
            line.collisionX = r.I32();
        }
        for (SpriteFetchLatch& sprite : line.sprite)
        {
            sprite.y = r.U8();
            sprite.x = r.U8();
            sprite.name = r.U8();
            sprite.colour = r.U8();
            sprite.pattern[0] = r.U8();
            sprite.pattern[1] = r.U8();
            sprite.yValid = r.Bool();
            sprite.xValid = r.Bool();
            sprite.nameValid = r.Bool();
            sprite.colourValid = r.Bool();
            sprite.patternValid[0] = r.Bool();
            sprite.patternValid[1] = r.Bool();
        }
    }
    m_PendingSpriteFetch.activity =
        static_cast<TMS99xxVramSlotSchedule::Activity>(r.I32());
    m_PendingSpriteFetch.absoluteTargetLine = r.U64();
    m_PendingSpriteFetch.index = r.U8();
    m_PendingSpriteFetch.byte = r.U8();
    m_PendingSpriteFetch.candidateScan = r.Bool();
    m_HasPendingSpriteFetch = r.Bool();

#ifdef SPRITE_EXPANDER
    for (auto& line : m_SpriteExpanderLineStates)
        line = {};
    if (version >= 3)
    {
        for (SpriteExpanderLineState& line : m_SpriteExpanderLineStates)
        {
            line.absoluteTargetLine = r.U64();
            line.candidateCount = r.U8();
            r.Bytes(line.satIndex, sizeof(line.satIndex));
            line.latched = r.Bool();
            line.large = r.Bool();
            line.magnified = r.Bool();
            for (SpriteFetchLatch& sprite : line.sprite)
            {
                sprite.y = r.U8();
                sprite.x = r.U8();
                sprite.name = r.U8();
                sprite.colour = r.U8();
                sprite.pattern[0] = r.U8();
                sprite.pattern[1] = r.U8();
                sprite.yValid = r.Bool();
                sprite.xValid = r.Bool();
                sprite.nameValid = r.Bool();
                sprite.colourValid = r.Bool();
                sprite.patternValid[0] = r.Bool();
                sprite.patternValid[1] = r.Bool();
            }
            if (line.candidateCount > kSpriteExpanderCapacity)
                line.candidateCount = kSpriteExpanderCapacity;
        }
    }
#endif

    m_StreamingCollisionPhase = r.I32();
    m_StreamingCollisionLine = r.I32();
    m_StreamingOverflowLine = r.I32();
    m_StreamingOverflowSprite = r.I32();
    m_StreamingCollisions = r.U32();
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
    m_LegacyOverflowLine = r.I32();
    m_LegacyOverflowSprite = r.I32();
    m_LegacyCollisions = r.U32();
#endif

    for (Register7Timeline& line : m_Register7Line)
    {
        line.start = r.U8();
        line.count = r.U8();
        for (int cut = 0; cut < kRegister7CutsPerLine; ++cut)
        {
            line.column[cut] = r.U16();
            line.value[cut] = r.U8();
        }
    }

    for (SlotActivityRecord& slot : m_LineSlotHistory)
    {
        slot.activity =
            static_cast<TMS99xxVramSlotSchedule::Activity>(r.I32());
        slot.index = r.U8();
        slot.cpuTransacted = r.Bool();
        slot.cpuWrite = r.Bool();
        slot.cpuAddress = r.U16();
        slot.cpuValue = r.U8();
    }

    m_VisibleLineCpuVramWrites = r.U32();
    m_VisibleLineVdpRegisterWrites = r.U32();
    m_CompletedBackgroundFetches = r.U32();
    m_CompletedSpriteFetches = r.U32();
    m_CompletedSpriteSelectionScans = r.U32();
    m_FrameRenderDiagnostics.frameSerial = r.U64();

    m_LastSpriteSelectionHistory = {};
    if (version >= 2)
    {
        m_LastSpriteSelectionHistory.valid = r.Bool();
        m_LastSpriteSelectionHistory.frameSerial = r.U64();
        m_LastSpriteSelectionHistory.rasterLines = r.I32();
        for (SpriteSelectionHistoryRow& row :
             m_LastSpriteSelectionHistory.rows)
        {
            r.Bytes(row.rasterX, sizeof(row.rasterX));
            r.Bytes(row.sat, sizeof(row.sat));
            r.Bytes(row.pattern, sizeof(row.pattern));
            row.flags = r.U8();
            row.collisionX = r.U8();
        }
    }

    // These are host-side observations of pixels fetched after the restored
    // instant, not machine state. Keeping them would make the debugger show a
    // frame from the abandoned timeline until the next frame completed.
    m_RenderedSpriteDebugWorking = {};
    m_LastRenderedSpriteDebugFrame = {};
    m_SpriteSelectionHistoryWorking = {};

    // Output-stage buffers hold pixels produced before the load; nothing in
    // them belongs to the restored instant, so the partial frame is discarded
    // rather than blended with what comes next.
    for (int line = 0; line < GC_RESOLUTION_HEIGHT; ++line)
    {
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
        m_LegacyBackgroundLineCaptured[line] = false;
#endif
        m_StreamingBackgroundLineReady[line] = false;
        m_StreamingBlankFromColumn[line] = GC_RESOLUTION_WIDTH;
    }

    m_NameTableDebugDirty = true;
    m_PatternTableDebugDirty = true;
    m_SpritePatternDebugDirty = true;
    ++m_DebugVRAMRevision;
    ++m_DebugPreviewSourceGeneration;
}


void TMS99xx::WriteVRAM(u16 address, u8 value)
{
    address &= 0x3FFF;
    // This is the actual VDP VRAM transfer, not the $BE port write. It is
    // therefore the only point that knows the resolved 14-bit VRAM address.
    const u8 beforeValue = m_pVdpVRAM[address];
    m_pVdpVRAM[address] = value;
    if (beforeValue != value)
    {
        ++m_DebugVRAMRevision;
        m_NameTableDebugDirty = true;
        m_PatternTableDebugDirty = true;
        m_SpritePatternDebugDirty = true;
    }
    // Where the beam was when the VDP took it. This is the transfer itself,
    // so the position is the one the chip saw - not the one the CPU had when
    // it wrote the port, which can be several dots earlier.
    const DebugEventRasterContext context = BuildPortAccessContext(m_iTstates);
    m_pMemory->CheckVRAMAccess(address, true, beforeValue, true, value, true,
                                    &context);

    // Qui potresti anche segnare il tile come "dirty" per ridisegnarlo
    // MarkTileDirty(address); 
}

u8 TMS99xx::ReadVRAM(u16 address)
{
    address &= 0x3FFF;
    const u8 value = m_pVdpVRAM[address];
    // Renderer reads access m_pVdpVRAM directly; this represents only a VDP
    // transfer caused by the emulated program (data port or address command).
    const DebugEventRasterContext context = BuildPortAccessContext(m_iTstates);
    m_pMemory->CheckVRAMAccess(address, false, value, true, value, true,
                                    &context);
    return value;
}

DebugDeviceDefinition TMS99xx::DescribeDebugDevice() const
{
    DebugDeviceDefinition definition = DescribeTms99xxDebugEvents();
    const GC_VideoDeviceDescriptor& descriptor = GetDeviceDescriptor();
    definition.name = descriptor.model;
    // All three chips expose the same debugger contract. Their concrete
    // identity remains in the descriptor and display name.
    definition.typeKey = "ti.tms99xx";
    return definition;
}
