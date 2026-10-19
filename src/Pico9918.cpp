#include "Pico9918.h"

#if GEARSF7000_ENABLE_PICO9918

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#include "Memory.h"
#include "SaveStateStream.h"
#include "TMS99xxPalette.h"
#include "TMS99xxTableAddress.h"
#include "cpu/CpuInterruptLines.h"

namespace
{
constexpr u16 kDefaultPalette[64] = {
    0x0000, 0xF000, 0xF2C3, 0xF5D6, 0xF54F, 0xF76F, 0xFD54, 0xF4EF,
    0xFF54, 0xFF76, 0xFDC3, 0xFED6, 0xF2B2, 0xFC5C, 0xFCCC, 0xFFFF,
    0x0000, 0xF2C3, 0xF000, 0xF54F, 0xF000, 0xFD54, 0xF000, 0xF4EF,
    0xF000, 0xFCCC, 0xF000, 0xFDC3, 0xF000, 0xFC5C, 0xF000, 0xFFFF,
    0x0000, 0xF00A, 0xF0A0, 0xF0AA, 0xFA00, 0xFA0A, 0xFA50, 0xFAAA,
    0xF555, 0xF55F, 0xF5F5, 0xF5FF, 0xFF55, 0xFF5F, 0xFFF5, 0xFFFF,
    0x0000, 0xF555, 0xF000, 0xF00A, 0xF000, 0xF0A0, 0xF000, 0xF0AA,
    0xF000, 0xFA00, 0xF000, 0xFA0A, 0xF000, 0xFA50, 0xF000, 0xFFFF};

constexpr u32 kHostVramWindowSize = 0x4000;
constexpr u32 kPhysicalVramSize = 0x10000;
constexpr u32 kPaletteMapBase = 0x5000;
constexpr u32 kRegisterMapBase = 0x6000;
constexpr u32 kStatusMapBase = 0xB000;
constexpr u8 kR0DoubleRows = 0x08;
constexpr u8 kR49Row30 = 0x40;
constexpr const char* kMemorySpaceCode = "PICO9918.VRAM";
constexpr const char* kControlBankCode = "PICO9918.CONTROL";
constexpr const char* kStatusBankCode = "PICO9918.STATUS";

constexpr u8 kReadWrite = DebugEventAccess_Read | DebugEventAccess_Write;
constexpr u8 kPublished = DebugDescriptorCapability_EventsPublished;

u16 TmsFixedPaletteEntry12(const u8* table, std::size_t index)
{
    if (index == 0)
        return 0;
    const u8* rgb = table + index * 3;
    return static_cast<u16>(0xF000 | (rgb[0] << 8) |
        (rgb[1] << 4) | rgb[2]);
}

u32 SmsMode4NameTableBase(u8 r2, u16 activeLines)
{
    if (activeLines == 192)
        return static_cast<u32>(r2 & 0x0E) << 10;
    return (static_cast<u32>(r2 & 0x0C) << 10) + 0x700;
}

u32 SmsMode4NameTableRows(u16 activeLines)
{
    return activeLines == 192 ? 28u : 32u;
}

constexpr DebugTargetDescriptor kPortTargets[] = {
    {0xBE, "Data", 8},
    {0xBF, "Control / Status", 8}
};

constexpr DebugTargetDescriptor kRegisterTargets[] = {
    {0, "R0 Mode Control 1", 8},
    {1, "R1 Mode Control 2", 8},
    {2, "R2 Name Table", 8},
    {3, "R3 Color Table", 8},
    {4, "R4 Pattern Table", 8},
    {5, "R5 Sprite Attribute Table", 8},
    {6, "R6 Sprite Pattern Table", 8},
    {7, "R7 Text / Backdrop Color", 8}
};

constexpr DebugTargetDescriptor kEventStateTargets[] = {
    {Pico9918DebugTarget::AddressCounter, "VRAM Address Counter", 16},
    {Pico9918DebugTarget::ControlLatchPending, "Control Latch Pending", 1},
    {Pico9918DebugTarget::ReadBuffer, "Read Buffer", 8},
    {Pico9918DebugTarget::Status, "Status", 8},
    {Pico9918DebugTarget::IRQ, "IRQ", 1},
    {Pico9918DebugTarget::VideoMode, "Video Mode", 3},
    {Pico9918DebugTarget::ControlLatchValue, "Control Latch Byte", 8},
    {Pico9918DebugTarget::Unlocked, "Extended Registers Unlocked", 1},
    {Pico9918DebugTarget::UnlockStep, "Unlock Sequence Step", 2},
    {Pico9918DebugTarget::StatusSelect, "Selected Status Register", 4},
    {Pico9918DebugTarget::PaletteMode, "Palette Data Port Mode", 1},
    {Pico9918DebugTarget::PaletteWriteStep, "Palette Write Step", 1}
};

constexpr GC_VideoStateValueDescriptor kStateValues[] = {
    {"PICO9918.STATE.MODE", "MODE", "Current compatibility display mode",
        GC_VideoDebugValueFormat::Text, 0},
    {"PICO9918.STATE.VRAM_POINTER", "VRAMPTR", "VRAM address counter",
        GC_VideoDebugValueFormat::Address, 16},
    {"PICO9918.STATE.LATCH_PENDING", "LATCH", "Second control byte pending",
        GC_VideoDebugValueFormat::Boolean, 1},
    {"PICO9918.STATE.LATCH_VALUE", "LATCHVAL", "First control byte latch",
        GC_VideoDebugValueFormat::Hexadecimal, 8},
    {"PICO9918.STATE.READ_BUFFER", "BUFFER", "VRAM read-ahead buffer",
        GC_VideoDebugValueFormat::Hexadecimal, 8},
    {"PICO9918.STATE.STATUS", "STATUS", "Internal status latch",
        GC_VideoDebugValueFormat::Hexadecimal, 8},
    {"PICO9918.STATE.RASTER_LINE", "RASTER Y", "Current output raster line",
        GC_VideoDebugValueFormat::Decimal, 10},
    {"PICO9918.STATE.RASTER_TICK", "RASTER TICK", "PIO tick inside the line",
        GC_VideoDebugValueFormat::Decimal, 12},
    {"PICO9918.STATE.INTERRUPT", "INT", "Interrupt output state",
        GC_VideoDebugValueFormat::Boolean, 1},
    {"PICO9918.STATE.UNLOCKED", "UNLOCKED", "Extended register access enabled",
        GC_VideoDebugValueFormat::Boolean, 1},
    {"PICO9918.STATE.UNLOCK_STEP", "UNLOCK STEP", "Accepted R57 unlock writes",
        GC_VideoDebugValueFormat::Decimal, 2},
    {"PICO9918.STATE.STATUS_SELECT", "STATUS SELECT", "Status register selected by R15",
        GC_VideoDebugValueFormat::Decimal, 4},
    {"PICO9918.STATE.PALETTE_MODE", "PALETTE MODE", "Data port writes target palette RAM",
        GC_VideoDebugValueFormat::Boolean, 1},
    {"PICO9918.STATE.PALETTE_STEP", "PALETTE STEP", "First palette byte is pending",
        GC_VideoDebugValueFormat::Boolean, 1}
};

struct RegisterDefinition
{
    const char* code;
    const char* name;
    const char* description;
    u32 implementedMask;
    bool hasAddress;
};

constexpr RegisterDefinition kControlRegisters[] = {
    {"PICO9918.CONTROL.R0", "R0", "Legacy mode control register 0", 0x0A, false},
    {"PICO9918.CONTROL.R1", "R1", "Legacy mode control register 1", 0xFB, false},
    {"PICO9918.CONTROL.R2", "R2", "Pattern Name Table base address", 0x0F, true},
    {"PICO9918.CONTROL.R3", "R3", "Color Table base address and mask", 0xFF, true},
    {"PICO9918.CONTROL.R4", "R4", "Pattern Generator Table base address and mask", 0x07, true},
    {"PICO9918.CONTROL.R5", "R5", "Sprite Attribute Table base address", 0x7F, true},
    {"PICO9918.CONTROL.R6", "R6", "Sprite Generator Table base address", 0x07, true},
    {"PICO9918.CONTROL.R7", "R7", "Text and backdrop colors", 0xFF, false}
};

bool IsCode(const char* value, const char* expected)
{
    return value != nullptr && std::strcmp(value, expected) == 0;
}

template <std::size_t Count>
std::array<std::string, Count> MakeIndexedNames(const char* prefix)
{
    std::array<std::string, Count> result{};
    for (std::size_t i = 0; i < Count; ++i)
        result[i] = std::string(prefix) + std::to_string(i);
    return result;
}

const std::array<std::string, 64>& ControlRegisterCodes()
{
    static const auto values = MakeIndexedNames<64>("PICO9918.CONTROL.R");
    return values;
}

const std::array<std::string, 64>& ControlRegisterNames()
{
    static const auto values = MakeIndexedNames<64>("R");
    return values;
}

const std::array<std::string, 16>& StatusRegisterCodes()
{
    static const auto values = MakeIndexedNames<16>("PICO9918.STATUS.S");
    return values;
}

const std::array<std::string, 16>& StatusRegisterNames()
{
    static const auto values = MakeIndexedNames<16>("S");
    return values;
}
}

Pico9918::Pico9918(Memory* memory, CpuInterruptLines* interruptLines,
    SystemVideoDevice device)
    : m_memory(memory), m_interruptLines(interruptLines), m_device(device),
      m_legacy(device == SystemVideoDevice::PicoSMSVdp ||
               device == SystemVideoDevice::SegaSMSVdpNTSC ||
               device == SystemVideoDevice::SegaGameGearVdp
          ? Pico9918LegacyCore::Backend::PicoSMSVdp
          : Pico9918LegacyCore::Backend::Pico9918,
          // the Mark III bus: the SMS VDP in every mode, the TMS palette kept
          device == SystemVideoDevice::SegaSMSVdpNTSC ||
          device == SystemVideoDevice::SegaGameGearVdp)
{
    // Hardware identity selects the variant; both Sega VDPs share the backend.
    SetGameGearCram(device == SystemVideoDevice::SegaGameGearVdp);
    SetGameGearWindow(device == SystemVideoDevice::SegaGameGearVdp);
    SetTmsFixedPalette(0);
}

// The fixed sixteen of a TMS mode. picoSMSvdp keeps the TMS9918's on either
// board; the real 315-5124 on the Mark III bus has its own, offered there so
// an SG-1000/SC-3000 game can be seen as a Mark III would show it.
void Pico9918::SetTmsFixedPalette(int profile)
{
    const u8* table = profile == 1
        ? kPalette_444_sms_vdp_tms
        : kPalette_444_tms9918_analog;
    m_tmsFixedProfile = profile == 1 ? 1 : 0;
    for (std::size_t index = 0; index < m_tmsFixedEntries.size(); ++index)
    {
        const u16 value = TmsFixedPaletteEntry12(table, index);
        m_tmsFixedEntries[index] = value;
        m_tmsFixedRgb[index * 3] = static_cast<u8>(((value >> 8) & 0x0F) * 17);
        m_tmsFixedRgb[index * 3 + 1] = static_cast<u8>(((value >> 4) & 0x0F) * 17);
        m_tmsFixedRgb[index * 3 + 2] = static_cast<u8>((value & 0x0F) * 17);
    }
    ++m_paletteRevision;
    ++m_debugPreviewSourceGeneration;
}

// Only the Mark III bus: the TMS socket is a TMS9918 in TMS modes, and the
// Game Gear's TMS modes are not this table.
bool Pico9918::HasTmsColorProfiles() const
{
    return m_device == SystemVideoDevice::SegaSMSVdpNTSC;
}

std::size_t Pico9918::GetColorProfileCount() const
{
    return HasTmsColorProfiles() ? 2u : 0u;
}

bool Pico9918::DescribeColorProfile(std::size_t index,
    GC_VideoColorProfileDescriptor& descriptor) const
{
    static const GC_VideoColorProfileDescriptor profiles[] = {
        {0, "sms.tms9918", "TMS9918 (picoSMSvdp)"},
        {1, "sms.315-5124", "SMS VDP 315-5124"},
    };
    if (!HasTmsColorProfiles() || index >= 2)
        return false;
    descriptor = profiles[index];
    return true;
}

void Pico9918::Init()
{
    m_frame.assign(FrameBufferWidth() * FrameBufferHeight() * 3u, 0);
    Reset();
}

void Pico9918::Reset()
{
    m_legacy.Reset();
    m_legacy.ConfigureSmsRaster(m_smsPal);
    ResetExtendedState(true);
    ResetPalette();
    m_rasterClock = 0;
    m_rasterLine = 0;
    m_frameInterruptFlag = false;
    m_smsDebugLatchedVScroll = 0;
    SetInterrupt(false, DebugEventSource::Reset);
    std::fill(m_frame.begin(), m_frame.end(), 0);
    std::fill(m_debugBackground.begin(), m_debugBackground.end(), 0);
    std::fill(m_debugComposed.begin(), m_debugComposed.end(), 0);
    std::fill(m_debugSpriteMask.begin(), m_debugSpriteMask.end(), 0);
    ++m_debugPreviewSourceGeneration;
    ++m_frameSerial;
}

void Pico9918::ConfigureSmsRaster(bool pal)
{
    m_smsPal = pal;
    m_legacy.ConfigureSmsRaster(pal);
}

void Pico9918::SetGameGearCram(bool enabled)
{
    m_ggCram = enabled;
    m_legacy.SetGameGearCram(enabled);
}

void Pico9918::SetSmsRasterFromCpuTime(std::uint64_t tstates)
{
    // One Z80 T-state is three master clocks, i.e. 1.5 raster dots. This is
    // derived from bus time rather than the 640x480 host raster, so VGA output
    // does not alter the values software sees at ports $7E/$7F.
    constexpr std::uint64_t clocksPerLine = GC_VDP_MASTER_CLOCKS_PER_LINE;
    const std::uint64_t linesPerFrame = m_smsPal ? 313 : 262;
    const std::uint64_t rasterClocks = clocksPerLine * linesPerFrame;
    const std::uint64_t clock = (tstates * 3u) % rasterClocks;
    m_legacy.SetSmsRasterPosition(
        static_cast<std::uint16_t>(clock / clocksPerLine),
        static_cast<std::uint16_t>((clock % clocksPerLine) /
            GC_VDP_MASTER_CLOCKS_PER_DOT));
}

u8 Pico9918::ReadSmsVCounter(std::uint64_t tstates)
{
    SetSmsRasterFromCpuTime(tstates);
    return m_legacy.ReadSmsVCounter();
}

u8 Pico9918::ReadSmsHCounter(std::uint64_t tstates)
{
    SetSmsRasterFromCpuTime(tstates);
    return m_legacy.ReadSmsHCounter();
}

void Pico9918::LatchSmsHCounter(std::uint64_t tstates)
{
    SetSmsRasterFromCpuTime(tstates);
    m_legacy.LatchSmsHCounter();
}

bool Pico9918::TickDeviceClocks(unsigned int deviceClocks)
{
    bool completedFrame = false;
    const unsigned int clocksPerLine =
        static_cast<unsigned int>(TimingClocksPerLine());
    const int rasterHeight = TimingRasterHeight();
    while (deviceClocks != 0)
    {
        unsigned int boundary = clocksPerLine;
        if (IsNativeSmsVdp())
        {
            if (m_rasterClock < SmsScrollLatchClock)
                boundary = SmsScrollLatchClock;
            else if (m_rasterClock < SmsLineInterruptClock)
                boundary = SmsLineInterruptClock;
        }
        const unsigned int remaining = boundary - m_rasterClock;
        const unsigned int step = std::min(deviceClocks, remaining);
        m_rasterClock += step;
        deviceClocks -= step;
        if (IsNativeSmsVdp() && m_rasterClock == SmsScrollLatchClock)
        {
            m_legacy.LatchSmsScroll(static_cast<std::uint16_t>(m_rasterLine));
            continue;
        }
        if (IsNativeSmsVdp() && m_rasterClock == SmsLineInterruptClock)
        {
            m_legacy.ClockSmsLineCounter(static_cast<std::uint16_t>(m_rasterLine));
            UpdateInterrupt();
            continue;
        }
        if (m_rasterClock != clocksPerLine)
            continue;

        RenderRasterLine(m_rasterLine);
        m_rasterClock = 0;
        m_rasterLine = (m_rasterLine + 1) % rasterHeight;
        // Publish only after the selected device has scanned its complete
        // field. This includes the VGA lower border or the NTSC blanking
        // interval rather than exposing a partially updated surface.
        if (m_rasterLine == 0)
        {
            ++m_frameSerial;
            completedFrame = true;
        }
    }
    return completedFrame;
}

bool Pico9918::IsNativeSmsVdp() const
{
    return m_device == SystemVideoDevice::SegaSMSVdpNTSC ||
        m_device == SystemVideoDevice::SegaGameGearVdp;
}

// The SMS VDP draws its TMS modes from a fixed palette and leaves CRAM alone:
// an SMS BIOS that draws its logo in Mode 4 and then boots an SG-1000 card
// must not hand the card the logo's colours. picoSMSvdp does the same on a
// locked chip, on either board.
bool Pico9918::UsesFixedTmsPalette() const
{
    return m_legacy.IsSmsBackend() && !m_unlocked &&
        (m_legacy.ReadRegister(0) & 0x04) == 0;
}

// The colours a line is drawn with, and the mask for an index into them.
const u8* Pico9918::DrawPalette(u8& mask) const
{
    if (UsesFixedTmsPalette())
    {
        mask = 0x0F;
        return m_tmsFixedRgb.data();
    }
    mask = 0x3F;
    return m_paletteRgb.data();
}

int Pico9918::TimingClocksPerLine() const
{
    return IsNativeSmsVdp() ? SmsClocksPerLine : RasterWidth;
}

int Pico9918::TimingRasterHeight() const
{
    return IsNativeSmsVdp() ? SmsRasterHeight : RasterHeight;
}

int Pico9918::FrameBufferWidth() const
{
    return IsNativeSmsVdp() ? SmsFrameBufferWidth : FrameWidth;
}

int Pico9918::FrameBufferHeight() const
{
    return IsNativeSmsVdp() ? SmsFrameBufferHeight : FrameHeight;
}

bool Pico9918::HasDoubleRows() const
{
    if (m_legacy.GetMode() == Pico9918LegacyCore::Mode::SmsMode4)
        return false;
    return (m_legacy.ReadRegister(0) & kR0DoubleRows) != 0;
}

bool Pico9918::HasThirtyRows() const
{
    if (m_legacy.GetMode() == Pico9918LegacyCore::Mode::SmsMode4)
        return false;
    return (m_extendedRegisters[49] & kR49Row30) != 0;
}

int Pico9918::VerticalPixelScale() const
{
    // pico9918-core's progressive VGA host changes from two output lines per
    // virtual line to one when R0.DOUBLE_ROWS is selected.
    return HasDoubleRows() ? 1 : 2;
}

int Pico9918::LogicalContentHeight() const
{
    if (m_legacy.GetMode() == Pico9918LegacyCore::Mode::SmsMode4)
        return m_legacy.ActiveLineCount();
    const int baseHeight = HasThirtyRows() ? 240 : 192;
    return HasDoubleRows() ? baseHeight * 2 : baseHeight;
}

int Pico9918::PhysicalContentHeight() const
{
    return LogicalContentHeight() * VerticalPixelScale();
}

int Pico9918::PhysicalContentY() const
{
    return (FrameHeight - PhysicalContentHeight()) / 2;
}

void Pico9918::RenderRasterLine(int line)
{
    if (IsNativeSmsVdp())
    {
        RenderNativeSmsRasterLine(line);
        return;
    }

    if (line < 0 || line >= FrameHeight)
        return;

    const int contentY = PhysicalContentY();
    const int contentHeight = PhysicalContentHeight();
    const int verticalScale = VerticalPixelScale();

    // pico9918_frame_output_line reuses the complete preceding host line on
    // a vertical repeat. This includes both side borders and makes register
    // changes between the two physical lines visible only on the next pair.
    if (verticalScale > 1 && (line % verticalScale) != 0)
    {
        std::memcpy(m_frame.data() + line * FrameWidth * 3u,
            m_frame.data() + (line - 1) * FrameWidth * 3u,
            FrameWidth * 3u);
        if (line == contentY + contentHeight - 1)
        {
            m_legacy.EndActiveDisplay();
            m_frameInterruptFlag = true;
            m_statusRegisters[1] |= 0x02;
            UpdateInterrupt();
        }
        return;
    }

    const u8 tilePaletteBase = static_cast<u8>(
        (m_extendedRegisters[24] & 0x03) << 4);
    const u8 spritePaletteBase = m_extendedRegisters[24] & 0x30;
    const u8 maxScanlineSprites = m_extendedRegisters[30];
    const u8 maxSprites = m_extendedRegisters[51];
    const bool realSpriteY = (m_extendedRegisters[49] & 0x08) != 0;
    const u8 tileEcm = static_cast<u8>((m_extendedRegisters[49] >> 4) & 0x03);
    const u16 tileEcmOffset = static_cast<u16>(
        0x800 >> ((m_extendedRegisters[29] & 0x0C) >> 2));
    const u8 spriteEcm = m_extendedRegisters[49] & 0x03;
    const u16 spriteEcmOffset = static_cast<u16>(
        0x800 >> ((m_extendedRegisters[29] & 0xC0) >> 6));
    const unsigned backdrop = tilePaletteBase |
        (m_legacy.ReadRegister(7) & 0x0F);
    u8* output = m_frame.data() + line * FrameWidth * 3u;
    u8 paletteMask = 0x3F;
    const u8* palette = DrawPalette(paletteMask);
    const u8* backdropRgb = palette + (backdrop & paletteMask) * 3;
    for (int x = 0; x < FrameWidth; ++x)
    {
        output[x * 3] = backdropRgb[0];
        output[x * 3 + 1] = backdropRgb[1];
        output[x * 3 + 2] = backdropRgb[2];
    }

    if (line < contentY || line >= contentY + contentHeight)
    {
        m_statusRegisters[1] |= 0x02;
        return;
    }

    const int logicalLine = (line - contentY) / verticalScale;
    Pico9918LegacyCore::DebugState beforeState;
    const Pico9918LegacyCore::Mode beforeMode = m_legacy.GetMode();
    m_legacy.ReadDebugState(beforeState);
    // Start from the backdrop before the compatibility renderer supplies the
    // active 192/240/384/480-line picture.
    m_scanline.fill(static_cast<u8>(backdrop));
    const bool row30 = HasThirtyRows();
    const int renderHeight = LogicalContentHeight();
    if (logicalLine < renderHeight)
    {
        const std::size_t debugOffset =
            static_cast<std::size_t>(logicalLine) * Pico9918LegacyCore::Width;
        std::memset(m_debugSpriteMask.data() + debugOffset, 0,
            Pico9918LegacyCore::Width);
        m_legacy.RenderScanline(static_cast<std::size_t>(logicalLine),
            m_scanline.data(), row30, realSpriteY,
            tilePaletteBase, spritePaletteBase,
            maxScanlineSprites, maxSprites, tileEcm, tileEcmOffset,
            spriteEcm, spriteEcmOffset,
            m_debugBackground.data() + debugOffset,
            m_debugSpriteMask.data() + debugOffset);
        if (m_legacy.GetMode() == Pico9918LegacyCore::Mode::SmsMode4)
            BuildSmsDebugScanline(logicalLine,
                m_debugBackground.data() + debugOffset,
                m_debugSpriteMask.data() + debugOffset);
        PredictNextLegacySpriteOverflow(logicalLine);
        std::memcpy(m_debugComposed.data() + debugOffset, m_scanline.data(),
            Pico9918LegacyCore::Width);
        ++m_debugPreviewSourceGeneration;
    }
    // Frame timing belongs to Pico9918's VGA sequencer. The imported legacy
    // renderer raises its own 192-line F flag, which would end ROW30 early.
    Pico9918LegacyCore::DebugState renderedState;
    if (!m_legacy.IsSmsBackend() && m_legacy.ReadDebugState(renderedState) &&
        (renderedState.status & 0x80) != 0)
        m_legacy.WriteStatus(static_cast<u8>(renderedState.status & 0x7F));
    m_statusRegisters[1] &= static_cast<u8>(~0x02);
    m_statusRegisters[3] = static_cast<u8>(logicalLine);
    if (logicalLine != 0 &&
        m_extendedRegisters[19] == static_cast<u8>(logicalLine))
        m_statusRegisters[1] |= 0x01;
    PublishStateChanges(beforeState, beforeMode,
        DebugEventSource::DeviceInternal);
    UpdateInterrupt();
    for (std::size_t x = 0; x < m_scanline.size(); ++x)
    {
        const u8 color = m_scanline[x] & paletteMask;
        const u8* rgb = palette + color * 3;
        u8* pixel = output + (ContentX + static_cast<int>(x) * 2) * 3;
        pixel[0] = pixel[3] = rgb[0];
        pixel[1] = pixel[4] = rgb[1];
        pixel[2] = pixel[5] = rgb[2];
    }

    if (line == contentY + contentHeight - 1)
    {
        m_legacy.EndActiveDisplay();
        m_frameInterruptFlag = true;
        m_statusRegisters[1] |= 0x02;
        UpdateInterrupt();
    }
}

void Pico9918::RenderNativeSmsRasterLine(int line)
{
    const int activeHeight = m_legacy.ActiveLineCount();
    const GC_VideoFrameDescriptor descriptor = GetFrameDescriptor();
    const u8 tilePaletteBase = static_cast<u8>(
        (m_extendedRegisters[24] & 0x03) << 4);
    const unsigned backdrop = tilePaletteBase |
        (m_legacy.ReadRegister(7) & 0x0F);
    u8 paletteMask = 0x3F;
    const u8* palette = DrawPalette(paletteMask);
    const u8* backdropRgb = palette + (backdrop & paletteMask) * 3;

    // The presentation surface is tightly packed at its current width. Clear
    // all television border rows at the start of the field; active rows below
    // are then replaced as the native beam reaches them.
    if (line == 0)
    {
        const std::size_t pixels = static_cast<std::size_t>(
            descriptor.frame_width) * descriptor.frame_height;
        for (std::size_t pixel = 0; pixel < pixels; ++pixel)
        {
            m_frame[pixel * 3] = backdropRgb[0];
            m_frame[pixel * 3 + 1] = backdropRgb[1];
            m_frame[pixel * 3 + 2] = backdropRgb[2];
        }
    }

    if (line < 0 || line >= activeHeight)
        return;

    const int outputY = descriptor.content_area.y + line;
    u8* output = m_frame.data() +
        static_cast<std::size_t>(outputY) * descriptor.frame_width * 3u;

    Pico9918LegacyCore::DebugState beforeState;
    const Pico9918LegacyCore::Mode beforeMode = m_legacy.GetMode();
    m_legacy.ReadDebugState(beforeState);
    m_scanline.fill(static_cast<u8>(backdrop));

    const u8 spritePaletteBase = m_extendedRegisters[24] & 0x30;
    const u8 maxScanlineSprites = m_extendedRegisters[30];
    const u8 maxSprites = m_extendedRegisters[51];
    const bool realSpriteY = (m_extendedRegisters[49] & 0x08) != 0;
    const u8 tileEcm = static_cast<u8>((m_extendedRegisters[49] >> 4) & 0x03);
    const u16 tileEcmOffset = static_cast<u16>(
        0x800 >> ((m_extendedRegisters[29] & 0x0C) >> 2));
    const u8 spriteEcm = m_extendedRegisters[49] & 0x03;
    const u16 spriteEcmOffset = static_cast<u16>(
        0x800 >> ((m_extendedRegisters[29] & 0xC0) >> 6));
    const std::size_t debugOffset =
        static_cast<std::size_t>(line) * Pico9918LegacyCore::Width;
    std::memset(m_debugSpriteMask.data() + debugOffset, 0,
        Pico9918LegacyCore::Width);
    m_legacy.RenderScanline(static_cast<std::size_t>(line),
        m_scanline.data(), false, realSpriteY,
        tilePaletteBase, spritePaletteBase,
        maxScanlineSprites, maxSprites, tileEcm, tileEcmOffset,
        spriteEcm, spriteEcmOffset,
        m_debugBackground.data() + debugOffset,
        m_debugSpriteMask.data() + debugOffset);
    BuildSmsDebugScanline(line,
        m_debugBackground.data() + debugOffset,
        m_debugSpriteMask.data() + debugOffset);
    PredictNextLegacySpriteOverflow(line);
    std::memcpy(m_debugComposed.data() + debugOffset, m_scanline.data(),
        Pico9918LegacyCore::Width);
    ++m_debugPreviewSourceGeneration;

    m_statusRegisters[1] &= static_cast<u8>(~0x02);
    m_statusRegisters[3] = static_cast<u8>(line);
    PublishStateChanges(beforeState, beforeMode,
        DebugEventSource::DeviceInternal);
    UpdateInterrupt();

    output += descriptor.content_area.x * 3;
    for (std::size_t x = 0; x < m_scanline.size(); ++x)
    {
        const u8 color = m_scanline[x] & paletteMask;
        const u8* rgb = palette + color * 3;
        output[x * 3] = rgb[0];
        output[x * 3 + 1] = rgb[1];
        output[x * 3 + 2] = rgb[2];
    }

    if (line == activeHeight - 1)
    {
        m_legacy.EndActiveDisplay();
        m_frameInterruptFlag = true;
        m_statusRegisters[1] |= 0x02;
        UpdateInterrupt();
    }
}

void Pico9918::BuildSmsDebugScanline(int logicalLine, u8* background,
    u8* spriteMask)
{
    if (!background || !spriteMask)
        return;
    const auto reg = [this](int number) { return m_legacy.ReadRegister(number); };
    const auto vram = [this](u32 address) {
        return m_legacy.ReadVram(address & 0x3FFF);
    };
    if (logicalLine == 0)
        m_smsDebugLatchedVScroll = IsNativeSmsVdp()
            ? m_legacy.ReadSmsLatchedVScroll() : reg(9);

    const u16 lines = m_legacy.ActiveLineCount();
    const u8 backdrop = reg(7) & 0x0F;
    std::fill(background, background + Pico9918LegacyCore::Width, backdrop);
    std::fill(spriteMask, spriteMask + Pico9918LegacyCore::Width, 0);
    std::array<u8, Pico9918LegacyCore::Width> priority{};

    const bool holdH = (reg(0) & 0x40) && logicalLine < 16;
    const u8 horizontalScroll = IsNativeSmsVdp()
        ? m_legacy.ReadSmsLatchedHScroll() : reg(8);
    const int hCell = holdH ? 0 : horizontalScroll >> 3;
    const int hFine = holdH ? 0 : horizontalScroll & 7;
    const int wrap = lines == 192 ? 224 : 256;
    const int scrolledY = (logicalLine + m_smsDebugLatchedVScroll) % wrap;
    const u32 nameBase = SmsMode4NameTableBase(reg(2), lines);
    for (int cell = 0; cell < 32; ++cell)
    {
        const bool holdV = (reg(0) & 0x80) && cell >= 24;
        const int rowY = holdV ? logicalLine : scrolledY;
        const int sourceCell = (cell - hCell) & 31;
        const u32 entry = nameBase + (rowY >> 3) * 64 + sourceCell * 2;
        const u16 word = static_cast<u16>(vram(entry) |
            (static_cast<u16>(vram(entry + 1)) << 8));
        const u16 tile = word & 0x01FF;
        int tileRow = rowY & 7;
        if (word & 0x0400) tileRow = 7 - tileRow;
        const u32 pattern = tile * 32 + tileRow * 4;
        const u8 planes[4] = {vram(pattern), vram(pattern + 1),
            vram(pattern + 2), vram(pattern + 3)};
        for (int pixel = 0; pixel < 8; ++pixel)
        {
            const int x = cell * 8 + pixel + hFine;
            if (x >= 256) break;
            const int bit = (word & 0x0200) ? pixel : 7 - pixel;
            const int mask = 1 << bit;
            const u8 value = static_cast<u8>(
                (planes[0] & mask ? 1 : 0) |
                (planes[1] & mask ? 2 : 0) |
                (planes[2] & mask ? 4 : 0) |
                (planes[3] & mask ? 8 : 0));
            background[x] = static_cast<u8>(((word & 0x0800) ? 16 : 0) |
                value);
            priority[x] = static_cast<u8>((word & 0x1000) && value);
        }
    }

    const u32 sat = static_cast<u32>(reg(5) & 0x7E) << 7;
    const u32 patternBase = (reg(6) & 0x04) ? 0x2000u : 0u;
    const bool tall = (reg(1) & 0x02) != 0;
    const bool zoom = (reg(1) & 0x01) != 0;
    const bool shiftLeft = (reg(0) & 0x08) != 0;
    const int sourceHeight = tall ? 16 : 8;
    const int outputHeight = sourceHeight << (zoom ? 1 : 0);
    const bool hasTerminator = lines == 192;
    std::array<u8, Pico9918LegacyCore::Width> occupied{};
    int selected = 0;
    for (int sprite = 0; sprite < 64; ++sprite)
    {
        const u8 rawY = vram(sat + sprite);
        if (hasTerminator && rawY == 0xD0) break;
        const u8 top = static_cast<u8>(rawY + 1);
        const u8 outputRow = static_cast<u8>(logicalLine - top);
        if (outputRow >= outputHeight) continue;
        if (++selected > 8) break;
        const u32 xp = sat + 0x80 + sprite * 2;
        const int xBase = vram(xp) - (shiftLeft ? 8 : 0);
        u8 tile = vram(xp + 1);
        int sourceRow = outputRow >> (zoom ? 1 : 0);
        if (tall)
        {
            tile &= 0xFE;
            tile = static_cast<u8>(tile + (sourceRow >> 3));
            sourceRow &= 7;
        }
        const u32 pattern = patternBase + tile * 32 + sourceRow * 4;
        const u8 planes[4] = {vram(pattern), vram(pattern + 1),
            vram(pattern + 2), vram(pattern + 3)};
        for (int sourceX = 0; sourceX < 8; ++sourceX)
        {
            const int bit = 0x80 >> sourceX;
            const u8 value = static_cast<u8>(
                (planes[0] & bit ? 1 : 0) |
                (planes[1] & bit ? 2 : 0) |
                (planes[2] & bit ? 4 : 0) |
                (planes[3] & bit ? 8 : 0));
            if (!value) continue;
            const int copies = zoom ? 2 : 1;
            for (int copy = 0; copy < copies; ++copy)
            {
                const int x = xBase + sourceX * copies + copy;
                if (x < 0 || x >= 256 || occupied[x]) continue;
                occupied[x] = 1;
                if (!priority[x]) spriteMask[x] = 1;
            }
        }
    }
}

void Pico9918::PredictNextLegacySpriteOverflow(int logicalLine)
{
    // A TMS9918 selects the sprites for line N+1 while line N is being
    // displayed. Both imported scanline renderers normally discover the
    // fifth sprite only when asked to draw N+1, which is one line too late
    // for software that polls 5S to time a raster split.
    const Pico9918LegacyCore::Mode mode = m_legacy.GetMode();
    if (mode == Pico9918LegacyCore::Mode::Text)
        return;

    int nextLine = logicalLine + 1;
    if (nextLine >= LogicalContentHeight())
        return;
    if (HasDoubleRows())
        nextLine >>= 1;

    Pico9918LegacyCore::DebugState state;
    if (!m_legacy.ReadDebugState(state) || (state.status & 0x40) != 0)
        return;

    if (mode == Pico9918LegacyCore::Mode::SmsMode4)
    {
        const int sat = (m_legacy.ReadRegister(5) & 0x7E) << 7;
        const int spriteHeight =
            (m_legacy.ReadRegister(1) & 0x02) != 0 ? 16 : 8;
        const int outputHeight = spriteHeight <<
            ((m_legacy.ReadRegister(1) & 0x01) != 0 ? 1 : 0);
        const bool hasTerminator = LogicalContentHeight() == 192;
        int visible = 0;
        for (int index = 0; index < 64; ++index)
        {
            const u8 rawY = m_legacy.ReadVram(
                static_cast<std::size_t>(sat + index));
            if (hasTerminator && rawY == 0xD0)
                break;
            const u8 top = static_cast<u8>(rawY + 1);
            const u8 outputRow = static_cast<u8>(
                static_cast<u8>(nextLine) - top);
            if (outputRow >= outputHeight)
                continue;
            if (++visible == 9)
            {
                // Sega Mode 4 retains the common overflow bit but does not
                // put the ninth sprite index in S0 bits 0-4.
                m_legacy.WriteStatus(static_cast<u8>(state.status | 0x40));
                break;
            }
        }
        return;
    }

    const bool row30 = HasThirtyRows();
    const bool realY = (m_extendedRegisters[49] & 0x08) != 0;
    const int spriteHeight = (m_legacy.ReadRegister(1) & 0x02) != 0 ? 16 : 8;
    const bool magnified = (m_legacy.ReadRegister(1) & 0x01) != 0;
    const int sat = (m_legacy.ReadRegister(5) & 0x7F) << 7;
    const int maxSprites = std::min<int>(m_extendedRegisters[51], 32);
    int visible = 0;

    for (int index = 0; index < maxSprites; ++index)
    {
        int y = m_legacy.ReadVram(static_cast<std::size_t>(sat + index * 4));
        if (!row30 && y == 0xD0)
            break;
        const int wrapThreshold = row30
            ? (realY ? 0xF0 : 0xEF) : 0xE0;
        if (y > wrapThreshold)
            y -= 256;
        if (!realY)
            ++y;
        int patternRow = nextLine - y;
        if (magnified)
            patternRow >>= 1;
        if (patternRow < 0 || patternRow >= spriteHeight)
            continue;
        if (++visible == 5)
        {
            m_legacy.WriteStatus(static_cast<u8>(
                (state.status & 0xE0) | 0x40 | index));
            break;
        }
    }
}

u8 Pico9918::GetDataPort(std::uint64_t)
{
    Pico9918LegacyCore::DebugState before;
    const Pico9918LegacyCore::Mode beforeMode = m_legacy.GetMode();
    m_legacy.ReadDebugState(before);
    const u16 address = before.address & 0x3FFF;
    const u8 fetched = m_legacy.ReadVram(address);
    const u8 value = m_legacy.ReadData();
    m_legacy.AdjustAddress(AddressIncrementAdjustment());
    PublishVramAccess(address, false, fetched, fetched);
    PublishStateChanges(before, beforeMode, DebugEventSource::CpuInstruction);
    return value;
}

u8 Pico9918::GetStatusFlags()
{
    Pico9918LegacyCore::DebugState before;
    const Pico9918LegacyCore::Mode beforeMode = m_legacy.GetMode();
    m_legacy.ReadDebugState(before);
    if (m_legacy.IsSmsBackend())
    {
        const u8 value = static_cast<u8>(m_legacy.ReadStatus() |
            (m_frameInterruptFlag ? 0x80 : 0));
        m_frameInterruptFlag = false;
        UpdateInterrupt(DebugEventSource::CpuInstruction);
        PublishStateChanges(before, beforeMode,
            DebugEventSource::CpuInstruction);
        return value;
    }
    const u8 selectedStatus = m_unlocked
        ? static_cast<u8>(m_extendedRegisters[15] & 0x0F) : 0;
    const bool beforePaletteMode = (m_extendedRegisters[47] & 0x80) != 0;
    const u8 beforePaletteStep = m_paletteWriteStage;
    const u8 value = ReadSelectedStatus();
    m_paletteWriteStage = 0;
    m_extendedRegisters[47] &= 0x7F;
    if (selectedStatus == 0)
    {
        m_frameInterruptFlag = false;
    }
    else
    {
        m_legacy.CancelControlLatch();
    }
    UpdateInterrupt(DebugEventSource::CpuInstruction);
    PublishStateChanges(before, beforeMode, DebugEventSource::CpuInstruction);
    PublishPaletteStateChanges(beforePaletteMode, beforePaletteStep,
        DebugEventSource::CpuInstruction);
    return value;
}

void Pico9918::WriteData(std::uint64_t, u8 data)
{
    Pico9918LegacyCore::DebugState before;
    const Pico9918LegacyCore::Mode beforeMode = m_legacy.GetMode();
    m_legacy.ReadDebugState(before);
    const bool beforePaletteMode = (m_extendedRegisters[47] & 0x80) != 0;
    const u8 beforePaletteStep = m_paletteWriteStage;
    if (!m_legacy.IsSmsBackend() && HandlePaletteWrite(data))
    {
        PublishPaletteStateChanges(beforePaletteMode, beforePaletteStep,
            DebugEventSource::CpuInstruction);
        return;
    }
    const u16 address = before.address & 0x3FFF;
    const u8 previous = m_legacy.ReadVram(address);
    const bool paletteWrite = m_legacy.IsPaletteWriteActive();
    m_legacy.WriteData(data);
    if (paletteWrite)
        SyncBackendPalette();
    m_legacy.AdjustAddress(AddressIncrementAdjustment());
    PublishVramAccess(address, true, previous, data);
    PublishStateChanges(before, beforeMode, DebugEventSource::CpuInstruction);
}

void Pico9918::WriteControl(std::uint64_t, u8 control)
{
    // The PICO9918 firmware (vrEmuTms9918) keeps the first control byte aside
    // and touches the address only with the second: unlike a real TMS9918A,
    // whose first byte goes straight into the low address byte (TMS99xx,
    // openMSX, CLK). This is the board's behaviour, kept as the original.

    Pico9918LegacyCore::DebugState before;
    const Pico9918LegacyCore::Mode beforeMode = m_legacy.GetMode();
    m_legacy.ReadDebugState(before);
    if (m_legacy.IsSmsBackend())
    {
        // The SMS core decodes the command itself; publish the register
        // write it performs, as the TMS path below does. Mode 4 has eleven
        // registers (R0-R10): a write to a higher index goes nowhere.
        const bool smsRegisterWrite = before.controlLatchPending &&
            (control & 0xC0) == 0x80 && (control & 0x0F) <= 10;
        const u8 smsRegister = control & 0x0F;
        const u8 smsRegisterBefore = smsRegisterWrite
            ? m_legacy.ReadRegister(smsRegister) : 0;
        m_legacy.WriteControl(control);
        if (smsRegisterWrite && m_memory != nullptr)
            m_memory->CheckVDPRegisterWrite(smsRegister, smsRegisterBefore,
                m_legacy.ReadRegister(smsRegister));
        PublishStateChanges(before, beforeMode,
            DebugEventSource::CpuInstruction);
        UpdateInterrupt(DebugEventSource::CpuInstruction);
        return;
    }
    const bool completesCommand = before.controlLatchPending;
    const bool registerWrite = completesCommand && (control & 0xC0) == 0x80;
    const bool invalidRegisterCommand = completesCommand &&
        (control & 0xC0) == 0xC0;
    const bool vramReadCommand = completesCommand && (control & 0xC0) == 0;
    const u8 requestedRegister = control & 0x3F;
    const bool beforeUnlocked = m_unlocked;
    const u8 beforeUnlockCount = m_unlockCount;
    const u8 beforeStatusSelect = m_extendedRegisters[15] & 0x0F;
    const bool beforePaletteMode = (m_extendedRegisters[47] & 0x80) != 0;
    const u8 beforePaletteStep = m_paletteWriteStage;
    const u16 readAddress = static_cast<u16>(
        before.controlLatchValue | ((control & 0x3F) << 8));
    const u8 prefetched = vramReadCommand
        ? m_legacy.ReadVram(readAddress) : 0;
    bool acceptedRegister = false;
    u8 finalRegister = 0;
    u8 registerBefore = 0;
    u8 registerAfter = 0;
    if (registerWrite)
        acceptedRegister = WriteRegisterCommand(requestedRegister,
            before.controlLatchValue, finalRegister, registerBefore,
            registerAfter);
    else if (invalidRegisterCommand)
        m_legacy.CancelControlLatch();
    else
        m_legacy.WriteControl(control);
    if (vramReadCommand)
        m_legacy.AdjustAddress(AddressIncrementAdjustment());
    if (acceptedRegister && m_memory != nullptr)
        m_memory->CheckVDPRegisterWrite(finalRegister, registerBefore,
            registerAfter);
    if (vramReadCommand)
        PublishVramAccess(readAddress, false, prefetched, prefetched);
    PublishStateChanges(before, beforeMode, DebugEventSource::CpuInstruction);
    PublishExtendedStateChanges(beforeUnlocked, beforeUnlockCount,
        beforeStatusSelect, DebugEventSource::CpuInstruction);
    PublishPaletteStateChanges(beforePaletteMode, beforePaletteStep,
        DebugEventSource::CpuInstruction);
    UpdateInterrupt(DebugEventSource::CpuInstruction);
}

void Pico9918::ResetExtendedState(bool powerOn)
{
    m_extendedRegisters.fill(0);
    if (powerOn)
    {
        m_statusRegisters.fill(0);
        m_statusRegisters[1] = 0xE8;
        m_statusRegisters[14] = 0x1A;
        m_legacy.WriteStatus(0x1F);
    }
    m_extendedRegisters[30] = 31;
    m_extendedRegisters[48] = 1;
    m_extendedRegisters[51] = 32;
    m_extendedRegisters[54] = 0x40;
    m_unlockCount = 0;
    m_unlocked = false;
    m_legacy.WriteRegister(1, powerOn ? 0x00 : 0x40);
    m_legacy.WriteRegister(3, 0x10);
    m_legacy.WriteRegister(4, 0x01);
    m_legacy.WriteRegister(5, 0x0A);
    m_legacy.WriteRegister(6, 0x02);
    m_legacy.WriteRegister(7, powerOn ? 0x00 : 0xF2);
}

bool Pico9918::WriteRegisterCommand(u8 registerIndex, u8 value,
    u8& finalRegisterIndex, u8& beforeValue, u8& afterValue)
{
    // Register selection and the unlock latch follow pico9918-core 1.3.0,
    // commit 9e50746.  The real host interface masks a locked register
    // select to three bits; it does not reject registers 8-63.
    finalRegisterIndex = registerIndex;
    if (registerIndex == 57)
    {
        beforeValue = m_extendedRegisters[57];
        m_extendedRegisters[57] = value;
        afterValue = value;
        m_legacy.CancelControlLatch();

        const bool unlockValue = (value & 0xFC) == 0x1C;
        const bool unlocked = unlockValue && m_unlockCount != 0;
        m_unlockCount = unlockValue ? 1 : 0;
        if (unlocked != m_unlocked)
        {
            m_unlocked = unlocked;
            if (unlocked)
                m_extendedRegisters[30] = 31;
        }
        return true;
    }

    // Once M4 has been selected, the official core refuses a high-register
    // select while locked instead of aliasing it.  This preserves the F18A
    // text-80 unlock path without changing ordinary TMS-compatible aliasing.
    if (!m_unlocked && registerIndex >= 8 &&
        (m_legacy.ReadRegister(0) & 0x04) != 0)
    {
        m_legacy.CancelControlLatch();
        return false;
    }

    m_unlockCount = 0;
    if (!m_unlocked)
        finalRegisterIndex &= 0x07;

    beforeValue = ReadControlRegister(finalRegisterIndex);
    if (finalRegisterIndex < 8)
    {
        m_legacy.WriteControl(static_cast<u8>(0x80 | finalRegisterIndex));
        afterValue = m_legacy.ReadRegister(finalRegisterIndex);
    }
    else
    {
        m_legacy.CancelControlLatch();
        m_extendedRegisters[finalRegisterIndex] = value;
        if (finalRegisterIndex == 15)
            m_statusRegisters[15] = value & 0x0F;
        if (finalRegisterIndex == 30 && value == 0)
            m_extendedRegisters[30] = 31;
        if (finalRegisterIndex == 50 && (value & 0x80) != 0)
            ResetExtendedState(false);
        afterValue = m_extendedRegisters[finalRegisterIndex];
    }
    return true;
}

u8 Pico9918::ReadControlRegister(u8 registerIndex) const
{
    if (m_legacy.IsSmsBackend())
        return m_legacy.ReadRegister(registerIndex & 0x3F);
    return registerIndex < 8 ? m_legacy.ReadRegister(registerIndex)
                             : m_extendedRegisters[registerIndex & 0x3F];
}

u8 Pico9918::ReadSelectedStatus()
{
    if (!m_unlocked || (m_extendedRegisters[15] & 0x0F) == 0)
    {
        const u8 status = static_cast<u8>(m_legacy.ReadStatus() |
            (m_frameInterruptFlag ? 0x80 : 0x00));
        // pico9918-core 1.3.0 clears only the flags observed by the CPU.
        // The sprite-number field survives unless 5S was acknowledged.
        m_legacy.WriteStatus((status & 0x40) != 0
            ? 0x1F : static_cast<u8>(status & 0x1F));
        return status;
    }
    const u8 selected = m_extendedRegisters[15] & 0x0F;
    const u8 status = m_statusRegisters[selected];
    if (selected == 1)
        m_statusRegisters[1] &= static_cast<u8>(~0x01);
    return status;
}

std::int16_t Pico9918::AddressIncrementAdjustment() const
{
    const std::int8_t increment = static_cast<std::int8_t>(
        m_extendedRegisters[48]);
    // The replaceable legacy core has already applied its fixed +1.
    return static_cast<std::int16_t>(increment) - 1;
}

void Pico9918::ResetPalette()
{
    if (m_legacy.IsSmsBackend())
    {
        // picoSMSvdp owns its palette RAM, including Mode 4 CRAM writes.
        // Seed both Mode 4 banks directly from GearSF7000's canonical
        // analog TMS9918 RGB444 palette. Guest CRAM writes replace them.
        for (std::size_t index = 0; index < 32; ++index)
        {
            const u16 entry = TmsFixedPaletteEntry12(kPalette_444_tms9918_analog, index & 15);
            m_legacy.WriteVram(kPaletteMapBase + index * 2,
                static_cast<u8>(entry >> 8));
            m_legacy.WriteVram(kPaletteMapBase + index * 2 + 1,
                static_cast<u8>(entry));
        }
        for (std::size_t index = 0; index < m_paletteEntries.size(); ++index)
        {
            m_paletteEntries[index] = m_legacy.ReadPalette(index);
            RebuildPaletteEntry(index);
        }
        m_paletteWriteStage = 0;
        m_paletteWriteFirstByte = 0;
        ++m_paletteRevision;
        return;
    }
    std::copy(std::begin(kDefaultPalette), std::end(kDefaultPalette),
        m_paletteEntries.begin());
    for (std::size_t index = 0; index < 16; ++index)
        m_paletteEntries[index] = TmsFixedPaletteEntry12(kPalette_444_tms9918_analog, index);
    for (std::size_t index = 0; index < m_paletteEntries.size(); ++index)
    {
        RebuildPaletteEntry(index);
        m_legacy.WriteVram(kPaletteMapBase + index * 2,
            static_cast<u8>(m_paletteEntries[index] >> 8));
        m_legacy.WriteVram(kPaletteMapBase + index * 2 + 1,
            static_cast<u8>(m_paletteEntries[index]));
    }
    m_paletteWriteStage = 0;
    m_paletteWriteFirstByte = 0;
    ++m_paletteRevision;
}

void Pico9918::SyncBackendPalette()
{
    if (!m_legacy.IsSmsBackend())
        return;
    bool changed = false;
    for (std::size_t index = 0; index < m_paletteEntries.size(); ++index)
    {
        const u16 value = m_legacy.ReadPalette(index);
        if (m_paletteEntries[index] == value)
            continue;
        m_paletteEntries[index] = value;
        RebuildPaletteEntry(index);
        changed = true;
    }
    if (changed)
        ++m_paletteRevision;
}

void Pico9918::RebuildPaletteEntry(std::size_t index)
{
    const u16 value = m_paletteEntries[index];
    m_paletteRgb[index * 3] = static_cast<u8>(((value >> 8) & 0x0F) * 17);
    m_paletteRgb[index * 3 + 1] = static_cast<u8>(((value >> 4) & 0x0F) * 17);
    m_paletteRgb[index * 3 + 2] = static_cast<u8>((value & 0x0F) * 17);
}

bool Pico9918::HandlePaletteWrite(u8 data)
{
    if (!m_unlocked || (m_extendedRegisters[47] & 0x80) == 0)
        return false;
    if (m_paletteWriteStage == 0)
    {
        m_paletteWriteFirstByte = data & 0x0F;
        m_paletteWriteStage = 1;
        return true;
    }

    const std::size_t index = m_extendedRegisters[47] & 0x3F;
    m_paletteEntries[index] = static_cast<u16>(
        (m_paletteWriteFirstByte << 8) | data);
    m_legacy.WriteVram(kPaletteMapBase + index * 2,
        m_paletteWriteFirstByte);
    m_legacy.WriteVram(kPaletteMapBase + index * 2 + 1, data);
    RebuildPaletteEntry(index);
    ++m_paletteRevision;
    m_paletteWriteStage = 0;
    if ((m_extendedRegisters[47] & 0x40) != 0)
        ++m_extendedRegisters[47];
    else
        m_extendedRegisters[47] &= 0x7F;
    return true;
}

void Pico9918::SetInterrupt(bool asserted, DebugEventSource source)
{
    if (m_interruptAsserted == asserted)
        return;
    const bool before = m_interruptAsserted;
    m_interruptAsserted = asserted;
    if (m_interruptLines != nullptr)
        m_interruptLines->SetMaskableInterruptLine(asserted);
    if (m_memory != nullptr)
        m_memory->CheckVDPStateChange(Pico9918DebugTarget::IRQ,
            before ? 1 : 0, asserted ? 1 : 0, source);
}

void Pico9918::UpdateInterrupt(DebugEventSource source)
{
    if (m_legacy.IsSmsBackend())
    {
        // The scanline core owns sprite/line interrupt state, while this
        // VGA wrapper owns the end of the active picture. Present both on
        // the replacement board's single /INT pin.
        const bool frame = m_frameInterruptFlag &&
            ((m_legacy.ReadRegister(1) & 0x20) != 0);
        SetInterrupt(frame || m_legacy.InterruptAsserted(), source);
        return;
    }
    // Derived from pico9918-core 1.3.0 (9e50746): frame and horizontal
    // interrupts are independent and acknowledging one preserves the other.
    const bool frame = m_frameInterruptFlag &&
        ((m_legacy.ReadRegister(1) & 0x20) != 0);
    const bool horizontal = (m_statusRegisters[1] & 0x01) != 0 &&
        (m_legacy.ReadRegister(0) & 0x10) != 0;
    SetInterrupt(frame || horizontal, source);
}

void Pico9918::PublishStateChanges(
    const Pico9918LegacyCore::DebugState& before,
    Pico9918LegacyCore::Mode beforeMode, DebugEventSource source)
{
    if (m_memory == nullptr)
        return;
    Pico9918LegacyCore::DebugState after;
    if (!m_legacy.ReadDebugState(after))
        return;
    const auto publish = [this, source](u16 target, u64 oldValue,
        u64 newValue) {
        if (oldValue != newValue)
            m_memory->CheckVDPStateChange(target, oldValue, newValue, source);
    };
    publish(Pico9918DebugTarget::AddressCounter,
        before.address, after.address);
    publish(Pico9918DebugTarget::ControlLatchPending,
        before.controlLatchPending ? 1 : 0,
        after.controlLatchPending ? 1 : 0);
    publish(Pico9918DebugTarget::ReadBuffer,
        before.readAheadBuffer, after.readAheadBuffer);
    publish(Pico9918DebugTarget::Status, before.status, after.status);
    publish(Pico9918DebugTarget::VideoMode,
        static_cast<u8>(beforeMode), static_cast<u8>(m_legacy.GetMode()));
    publish(Pico9918DebugTarget::ControlLatchValue,
        before.controlLatchValue, after.controlLatchValue);
}

void Pico9918::PublishExtendedStateChanges(bool beforeUnlocked,
    u8 beforeUnlockCount, u8 beforeStatusSelect, DebugEventSource source)
{
    if (m_memory == nullptr)
        return;
    const auto publish = [this, source](u16 target, u64 oldValue,
        u64 newValue) {
        if (oldValue != newValue)
            m_memory->CheckVDPStateChange(target, oldValue, newValue, source);
    };
    publish(Pico9918DebugTarget::Unlocked, beforeUnlocked ? 1 : 0,
        m_unlocked ? 1 : 0);
    publish(Pico9918DebugTarget::UnlockStep, beforeUnlockCount, m_unlockCount);
    publish(Pico9918DebugTarget::StatusSelect, beforeStatusSelect,
        m_extendedRegisters[15] & 0x0F);
}

void Pico9918::PublishPaletteStateChanges(bool beforeMode, u8 beforeStep,
    DebugEventSource source)
{
    if (m_memory == nullptr)
        return;
    const bool afterMode = (m_extendedRegisters[47] & 0x80) != 0;
    if (beforeMode != afterMode)
        m_memory->CheckVDPStateChange(Pico9918DebugTarget::PaletteMode,
            beforeMode ? 1 : 0, afterMode ? 1 : 0, source);
    if (beforeStep != m_paletteWriteStage)
        m_memory->CheckVDPStateChange(Pico9918DebugTarget::PaletteWriteStep,
            beforeStep, m_paletteWriteStage, source);
}

void Pico9918::PublishVramAccess(u16 address, bool write,
    u8 beforeValue, u8 afterValue)
{
    if (m_memory != nullptr)
        m_memory->CheckVRAMAccess(address, write, beforeValue, true,
            afterValue, true);
}

const GC_VideoDeviceDescriptor& Pico9918::GetDeviceDescriptor() const
{
    return *FindVideoDeviceDescriptor(m_device);
}

GC_VideoFrameDescriptor Pico9918::GetFrameDescriptor() const
{
    GC_VideoFrameDescriptor descriptor;
    descriptor.device_key = GetDeviceDescriptor().key;
    descriptor.mode_id = static_cast<u32>(m_legacy.GetMode());
    descriptor.postprocessing_capabilities =
        GC_VIDEO_POSTPROCESSING_CAPABILITY_SPATIAL_SCALING |
        GC_VIDEO_POSTPROCESSING_CAPABILITY_SCANLINE_CRT;
    if (IsNativeSmsVdp())
    {
        const int activeHeight = m_legacy.ActiveLineCount();
        const bool borders = m_border != GC_VideoBorder::Disabled;
        int left = 0;
        int right = 0;
        if (m_border == GC_VideoBorder::Full272)
        {
            left = 4;
            right = 12;
        }
        else if (m_border == GC_VideoBorder::Full284)
        {
            left = 14;
            right = 14;
        }
        const int vertical = borders ? (240 - activeHeight) / 2 : 0;
        descriptor.buffer_width = SmsFrameBufferWidth;
        descriptor.buffer_height = SmsFrameBufferHeight;
        descriptor.frame_width = 256 + left + right;
        descriptor.frame_height = borders ? 240 : activeHeight;
        descriptor.stride_pixels = descriptor.frame_width;
        descriptor.content_area = {left, vertical, 256, activeHeight};
        descriptor.visible_area = {0, 0, descriptor.frame_width,
            descriptor.frame_height};
        descriptor.raster_width = SmsRasterWidth;
        descriptor.raster_height = SmsRasterHeight;
        descriptor.logical_width = 256;
        descriptor.logical_height = activeHeight;
        // The complete visible television picture is presented at 4:3. In
        // the 272/284-pixel captures this is the familiar horizontal stretch
        // performed by a CRT or by the selected desktop shader.
        descriptor.presentation_height = descriptor.frame_height;
        descriptor.presentation_width = descriptor.frame_height * 4 / 3;
        descriptor.device_clock_hz = SmsDeviceClockHz;
        descriptor.refresh_rate = {SmsDeviceClockHz,
            SmsClocksPerLine * SmsRasterHeight};
        descriptor.pixel_aspect_ratio = {
            descriptor.presentation_width, descriptor.frame_width};
        descriptor.signal = GC_VIDEO_SIGNAL_ANALOG_NTSC;
        descriptor.region = Region_NTSC;
        descriptor.revision = m_frameDescriptorRevision |
            (static_cast<u64>(activeHeight) << 16);
        ApplyGameGearWindow(descriptor);
        return descriptor;
    }

    descriptor.buffer_width = FrameWidth;
    descriptor.buffer_height = FrameHeight;
    descriptor.frame_width = FrameWidth;
    descriptor.frame_height = FrameHeight;
    descriptor.stride_pixels = FrameWidth;
    descriptor.content_area = {ContentX, PhysicalContentY(), ContentWidth,
        PhysicalContentHeight()};
    descriptor.visible_area = {0, 0, FrameWidth, FrameHeight};
    descriptor.raster_width = RasterWidth;
    descriptor.raster_height = RasterHeight;
    descriptor.logical_width = 256;
    descriptor.logical_height = LogicalContentHeight();
    descriptor.presentation_width = 320;
    descriptor.presentation_height = 240;
    descriptor.device_clock_hz = DeviceClockHz;
    descriptor.refresh_rate = {DeviceClockHz, RasterWidth * RasterHeight};
    descriptor.pixel_aspect_ratio = {1, 1};
    descriptor.signal = GC_VIDEO_SIGNAL_VGA;
    descriptor.region = Region_NTSC;
    // Geometry consumers can cheaply invalidate cached layout when either
    // extended vertical control changes.  Pixel contents alone do not alter
    // this value.
    descriptor.revision = 1u | (HasThirtyRows() ? 2u : 0u) |
        (HasDoubleRows() ? 4u : 0u) | (m_ggWindow ? 8u : 0u);
    ApplyGameGearWindow(descriptor);
    return descriptor;
}

void Pico9918::SetGameGearWindow(bool enabled)
{
    if (m_ggWindow == enabled)
        return;
    m_ggWindow = enabled;
    ++m_frameDescriptorRevision;
}

// 160x144 centred in the active picture, measured in logical pixels and scaled
// to however many frame pixels one of them takes (one on the SMS board, two
// across on the TMS socket's VGA frame): x 48, y 24 at 192 lines, 40 at 224.
// The original LCD is 4:3: its 160x144 dots have a 6:5 pixel aspect ratio.
void Pico9918::ApplyGameGearWindow(GC_VideoFrameDescriptor& descriptor) const
{
    if (!m_ggWindow || descriptor.logical_height < 144)
        return;
    const GC_VideoRect content = descriptor.content_area;
    const int sx = content.width / 256;
    const int sy = content.height / descriptor.logical_height;
    const int windowY = (descriptor.logical_height - 144) / 2;
    descriptor.visible_area = {content.x + 48 * sx, content.y + windowY * sy,
        160 * sx, 144 * sy};
    descriptor.presentation_width = 192;
    descriptor.presentation_height = 144;
    descriptor.pixel_aspect_ratio = {6, 5};
    descriptor.signal = GC_VIDEO_SIGNAL_DIGITAL_RGB;
}

GC_VideoFrameView Pico9918::GetFrameView(GC_Color_Format pixelFormat)
{
    if (pixelFormat != GC_PIXEL_RGB888 || m_frame.empty())
        return {};
    const GC_VideoFrameDescriptor descriptor = GetFrameDescriptor();
    return {descriptor, m_frame.data(), GC_PIXEL_RGB888,
        descriptor.stride_pixels * 3, m_frameSerial};
}

std::size_t Pico9918::GetOptionCount() const
{
    return IsNativeSmsVdp() ? 1u : 0u;
}

bool Pico9918::DescribeOption(std::size_t index,
    GC_VideoOptionDescriptor& descriptor) const
{
    if (!IsNativeSmsVdp() || index != 0)
        return false;
    descriptor = {GC_VideoOption::Border, "Border",
        GC_VideoOptionType::Choice, GC_VideoBorder::Disabled,
        GC_VideoBorder::Full284};
    return true;
}

bool Pico9918::SetOption(const char* code, s64 value)
{
    if (code && std::strcmp(code, GC_VideoOption::PredefinedPalette) == 0)
    {
        if (!HasTmsColorProfiles() || value < 0 || value > 1)
            return false;
        if (m_tmsFixedProfile != value)
            SetTmsFixedPalette(static_cast<int>(value));
        return true;
    }
    if (!IsNativeSmsVdp() || !code ||
        std::strcmp(code, GC_VideoOption::Border) != 0 ||
        value < GC_VideoBorder::Disabled ||
        value > GC_VideoBorder::Full284)
        return false;
    if (m_border == value)
        return true;
    m_border = value;
    ++m_frameDescriptorRevision;
    std::fill(m_frame.begin(), m_frame.end(), 0);
    return true;
}

void Pico9918::CopyHostConfigurationFrom(const IVideoDevice& sourceDevice)
{
    const Pico9918* source = dynamic_cast<const Pico9918*>(&sourceDevice);
    if (!source || !IsNativeSmsVdp() || !source->IsNativeSmsVdp())
        return;
    m_border = source->m_border;
    // Palette format and LCD window belong to the new VDP's identity.
    ++m_frameDescriptorRevision;
}

bool Pico9918::GetPaletteView(GC_VideoPaletteView& view) const
{
    view = {m_paletteRgb.data(), m_paletteEntries.size(), m_paletteRevision};
    view.native = m_paletteEntries.data();
    view.native_kind = GC_VideoPaletteView::NativeRgb444;
    // picoSMSvdp: the SMS CRAM, 32 entries written a byte each, in every mode.
    // PRAM keeps them in 12 bits and holds 64 - the other 32 are the F18A's,
    // which only an unlocked chip can reach. 12 bits come in only through the
    // extended palette feature (picoSMSvdp-core docs/VDP-FEATURES.md 3.1),
    // which the core does not have yet.
    if (m_legacy.IsSmsBackend() && !m_unlocked)
    {
        view.color_count = 32;
        view.native_kind = m_ggCram
            ? GC_VideoPaletteView::NativeRgb444
            : GC_VideoPaletteView::NativeSmsByte;
        // In a TMS mode CRAM waits, and the fixed sixteen are drawn: shown
        // after it.
        if (UsesFixedTmsPalette() && !m_ggCram)
        {
            std::copy_n(m_paletteRgb.begin(), 32 * 3, m_panelRgb.begin());
            std::copy(m_tmsFixedRgb.begin(), m_tmsFixedRgb.end(), m_panelRgb.begin() + 32 * 3);
            std::copy_n(m_paletteEntries.begin(), 32, m_panelEntries.begin());
            std::copy(m_tmsFixedEntries.begin(), m_tmsFixedEntries.end(), m_panelEntries.begin() + 32);
            view.rgb888 = m_panelRgb.data();
            view.native = m_panelEntries.data();
            view.color_count = 48;
            view.fixed_from = 32;
        }
    }
    return true;
}

std::size_t Pico9918::GetStateValueCount() const
{
    return std::size(kStateValues);
}

bool Pico9918::DescribeStateValue(std::size_t index,
    GC_VideoStateValueDescriptor& descriptor) const
{
    descriptor = {};
    if (index >= std::size(kStateValues))
        return false;
    descriptor = kStateValues[index];
    return true;
}

bool Pico9918::ReadStateValueSnapshot(std::size_t index, u64& value) const
{
    Pico9918LegacyCore::DebugState state;
    if (!m_legacy.ReadDebugState(state))
        return false;
    switch (index)
    {
        case 0: return false;
        case 1: value = state.address; return true;
        case 2: value = state.controlLatchPending ? 1 : 0; return true;
        case 3: value = state.controlLatchValue; return true;
        case 4: value = state.readAheadBuffer; return true;
        case 5: value = state.status; return true;
        case 6: value = static_cast<u64>(m_rasterLine); return true;
        case 7: value = m_rasterClock; return true;
        case 8: value = m_interruptAsserted ? 1 : 0; return true;
        case 9: value = m_unlocked ? 1 : 0; return true;
        case 10: value = m_unlockCount; return true;
        case 11: value = m_extendedRegisters[15] & 0x0F; return true;
        case 12: value = (m_extendedRegisters[47] & 0x80) != 0 ? 1 : 0; return true;
        case 13: value = m_paletteWriteStage; return true;
        default: return false;
    }
}

bool Pico9918::ReadStateTextSnapshot(std::size_t index, const char*& value) const
{
    value = nullptr;
    if (index != 0)
        return false;
    static constexpr const char* names[] = {
        "Graphics I", "Graphics II", "Text40", "Multicolor", "Mode 4"};
    value = names[static_cast<std::size_t>(m_legacy.GetMode())];
    return true;
}

bool Pico9918::DescribeMemorySpace(std::size_t index,
    GC_VideoMemorySpaceDescriptor& descriptor) const
{
    descriptor = {};
    if (index != 0)
        return false;
    descriptor.code = kMemorySpaceCode;
    descriptor.name = "VRAM";
    descriptor.description = "Pico9918 64 KiB physical backing map";
    descriptor.address_size_bytes = kPhysicalVramSize;
    descriptor.installed_size_bytes = kPhysicalVramSize;
    descriptor.host_window_size_bytes = kHostVramWindowSize;
    descriptor.host_page_size_bytes = kHostVramWindowSize;
    descriptor.host_page_count = 1;
    descriptor.mapping_flags = GC_VIDEO_MEMORY_MAPPING_HOST_WINDOWED |
        GC_VIDEO_MEMORY_MAPPING_MODE_DEPENDENT;
    descriptor.readable = true;
    descriptor.writable = true;
    descriptor.layout_revision = 1;
    return true;
}

std::size_t Pico9918::GetMemoryRegionCount(const char* spaceCode) const
{
    return IsCode(spaceCode, kMemorySpaceCode) ? 6 : 0;
}

bool Pico9918::DescribeMemoryRegion(const char* spaceCode, std::size_t index,
    GC_VideoMemoryRegionDescriptor& descriptor) const
{
    descriptor = {};
    if (!IsCode(spaceCode, kMemorySpaceCode) || index >= 6)
        return false;
    struct Region
    {
        const char* code;
        const char* description;
        GC_VideoMemoryRegionKind kind;
        GC_VideoMemoryRegionRole role;
        u32 start;
        u32 size;
        u32 access;
    };
    static constexpr Region regions[] = {
        {"PICO9918.VRAM.BASE", "Base VRAM and lower graphics RAM",
            GC_VideoMemoryRegionKind::Storage,
            GC_VideoMemoryRegionRole::DisplayRam, 0x0000, 0x5000,
            GC_VIDEO_MEMORY_ACCESS_READ | GC_VIDEO_MEMORY_ACCESS_WRITE |
            GC_VIDEO_MEMORY_ACCESS_DISPLAY_FETCH |
            GC_VIDEO_MEMORY_ACCESS_HOST_VISIBLE},
        {"PICO9918.VRAM.PRAM", "Palette RAM backing window",
            GC_VideoMemoryRegionKind::MappedDevice,
            GC_VideoMemoryRegionRole::PaletteRam, 0x5000, 0x1000,
            GC_VIDEO_MEMORY_ACCESS_READ | GC_VIDEO_MEMORY_ACCESS_WRITE},
        {"PICO9918.VRAM.REGISTERS", "Mapped write-only control registers",
            GC_VideoMemoryRegionKind::MappedDevice,
            GC_VideoMemoryRegionRole::RegisterBank, 0x6000, 0x0040,
            GC_VIDEO_MEMORY_ACCESS_READ |
            GC_VIDEO_MEMORY_ACCESS_WRITE_TRAPPED},
        {"PICO9918.VRAM.UPPER", "Upper graphics and processor RAM",
            GC_VideoMemoryRegionKind::Storage,
            GC_VideoMemoryRegionRole::ProcessorRam, 0x6040, 0x4FC0,
            GC_VIDEO_MEMORY_ACCESS_READ | GC_VIDEO_MEMORY_ACCESS_WRITE |
            GC_VIDEO_MEMORY_ACCESS_DISPLAY_FETCH},
        {"PICO9918.VRAM.STATUS", "Mapped read-only status registers",
            GC_VideoMemoryRegionKind::MappedDevice,
            GC_VideoMemoryRegionRole::StatusBank, 0xB000, 0x0010,
            GC_VIDEO_MEMORY_ACCESS_READ},
        {"PICO9918.VRAM.HIGH", "High graphics and processor RAM",
            GC_VideoMemoryRegionKind::Storage,
            GC_VideoMemoryRegionRole::ProcessorRam, 0xB010, 0x4FF0,
            GC_VIDEO_MEMORY_ACCESS_READ | GC_VIDEO_MEMORY_ACCESS_WRITE |
            GC_VIDEO_MEMORY_ACCESS_DISPLAY_FETCH}
    };
    const Region& region = regions[index];
    descriptor.code = region.code;
    descriptor.description = region.description;
    descriptor.kind = region.kind;
    descriptor.role = region.role;
    descriptor.start = region.start;
    descriptor.size = region.size;
    descriptor.access_flags = region.access;
    descriptor.backing_store_code = kMemorySpaceCode;
    descriptor.backing_offset = region.start;
    descriptor.address_mask = kPhysicalVramSize - 1;
    return true;
}

u8 Pico9918::ReadBackingMemory(u32 address) const
{
    if (address >= kRegisterMapBase &&
        address < kRegisterMapBase + RegisterCount)
        return ReadControlRegister(static_cast<u8>(address - kRegisterMapBase));
    if (address >= kStatusMapBase && address < kStatusMapBase + StatusCount)
    {
        const u32 status = address - kStatusMapBase;
        if (status == 0)
        {
            Pico9918LegacyCore::DebugState state;
            return m_legacy.ReadDebugState(state)
                ? static_cast<u8>(state.status |
                    (m_frameInterruptFlag ? 0x80 : 0x00)) : 0;
        }
        return m_statusRegisters[status];
    }
    if (address == 0x7000)
        return m_statusRegisters[3];
    if (address == 0x7001)
        return (m_statusRegisters[1] & 0x02) != 0 ? 1 : 0;
    return m_legacy.ReadVram(address);
}

bool Pico9918::IsBackingMemoryWritable(u32 address) const
{
    const bool registerFile = address >= kRegisterMapBase &&
        address < kRegisterMapBase + RegisterCount;
    const bool statusFile = address >= kStatusMapBase &&
        address < kStatusMapBase + StatusCount;
    return address < kPhysicalVramSize && !registerFile && !statusFile;
}

void Pico9918::WriteBackingMemory(u32 address, u8 value)
{
    m_legacy.WriteVram(address, value);
    if (address >= kPaletteMapBase && address < kPaletteMapBase + 128)
    {
        const std::size_t paletteIndex = (address - kPaletteMapBase) / 2;
        const u8 first = m_legacy.ReadVram(
            kPaletteMapBase + paletteIndex * 2);
        const u8 second = m_legacy.ReadVram(
            kPaletteMapBase + paletteIndex * 2 + 1);
        m_paletteEntries[paletteIndex] = static_cast<u16>(
            ((first & 0x0F) << 8) | second);
        RebuildPaletteEntry(paletteIndex);
        ++m_paletteRevision;
    }
}

bool Pico9918::ReadMemorySpace(const char* spaceCode, u32 offset,
    u8* destination, std::size_t size) const
{
    if (!IsCode(spaceCode, kMemorySpaceCode) || destination == nullptr ||
        offset > kPhysicalVramSize || size > kPhysicalVramSize - offset)
        return false;
    for (std::size_t index = 0; index < size; ++index)
        destination[index] = ReadBackingMemory(offset + index);
    return true;
}

bool Pico9918::WriteMemorySpace(const char* spaceCode, u32 offset,
    const u8* source, std::size_t size)
{
    if (!IsCode(spaceCode, kMemorySpaceCode) || source == nullptr ||
        offset > kPhysicalVramSize || size > kPhysicalVramSize - offset)
        return false;
    for (std::size_t index = 0; index < size; ++index)
        if (!IsBackingMemoryWritable(offset + index))
            return false;
    for (std::size_t index = 0; index < size; ++index)
        WriteBackingMemory(offset + index, source[index]);
    return true;
}

bool Pico9918::DescribeRegisterBank(std::size_t index,
    GC_VideoRegisterBankDescriptor& descriptor) const
{
    descriptor = {};
    if (index == 0)
    {
        descriptor.code = kControlBankCode;
        descriptor.name = "Control Registers";
        descriptor.description = "Pico9918 control registers";
        descriptor.register_count = RegisterCount;
    }
    else if (index == 1)
    {
        descriptor.code = kStatusBankCode;
        descriptor.name = "Status Registers";
        descriptor.description = "Pico9918 status registers";
        descriptor.register_count = StatusCount;
    }
    else
        return false;
    descriptor.register_width_bits = 8;
    descriptor.schema_revision = 1;
    return true;
}

std::size_t Pico9918::GetRegisterCount(const char* bankCode) const
{
    if (IsCode(bankCode, kControlBankCode))
        return RegisterCount;
    return IsCode(bankCode, kStatusBankCode) ? StatusCount : 0;
}

bool Pico9918::DescribeRegister(const char* bankCode, std::size_t index,
    GC_VideoRegisterDescriptor& descriptor) const
{
    descriptor = {};
    if (IsCode(bankCode, kControlBankCode))
    {
        if (index >= RegisterCount)
            return false;
        descriptor.code = ControlRegisterCodes()[index].c_str();
        descriptor.short_name = ControlRegisterNames()[index].c_str();
        descriptor.description = index < std::size(kControlRegisters)
            ? kControlRegisters[index].description
            : "Pico9918 extended control register";
        descriptor.bank_code = kControlBankCode;
        descriptor.register_index = static_cast<u32>(index);
        descriptor.width_bits = 8;
        descriptor.implemented_mask = index < std::size(kControlRegisters)
            ? kControlRegisters[index].implementedMask : 0xFF;
        descriptor.access_flags = GC_VIDEO_REGISTER_ACCESS_WRITE |
            GC_VIDEO_REGISTER_ACCESS_OBSERVE |
            GC_VIDEO_REGISTER_ACCESS_WRITE_SIDE_EFFECT;
        descriptor.has_derived_address = index < std::size(kControlRegisters) &&
            kControlRegisters[index].hasAddress;
        descriptor.derived_address_width_bits = descriptor.has_derived_address ? 14 : 0;
        return true;
    }
    if (!IsCode(bankCode, kStatusBankCode) || index >= StatusCount)
        return false;
    descriptor.code = StatusRegisterCodes()[index].c_str();
    descriptor.short_name = StatusRegisterNames()[index].c_str();
    descriptor.description = index == 0 ? "Legacy status register" :
        "Pico9918 extended status register";
    descriptor.bank_code = kStatusBankCode;
    descriptor.register_index = static_cast<u32>(index);
    descriptor.width_bits = 8;
    descriptor.implemented_mask = 0xFF;
    descriptor.access_flags = GC_VIDEO_REGISTER_ACCESS_READ |
        GC_VIDEO_REGISTER_ACCESS_OBSERVE |
        GC_VIDEO_REGISTER_ACCESS_READ_SIDE_EFFECT;
    return true;
}

bool Pico9918::ReadRegisterSnapshot(const char* bankCode, u32 registerIndex,
    u32& value) const
{
    if (IsCode(bankCode, kControlBankCode) && registerIndex < RegisterCount)
    {
        value = ReadControlRegister(static_cast<u8>(registerIndex));
        return true;
    }
    if (IsCode(bankCode, kStatusBankCode) && registerIndex < StatusCount)
    {
        if (registerIndex == 0)
        {
            Pico9918LegacyCore::DebugState state;
            if (!m_legacy.ReadDebugState(state))
                return false;
            value = state.status | (m_frameInterruptFlag ? 0x80 : 0x00);
        }
        else
            value = m_statusRegisters[registerIndex];
        return true;
    }
    return false;
}

bool Pico9918::ReadRegisterDerivedAddressSnapshot(const char* bankCode,
    u32 registerIndex, u32& address) const
{
    if (!IsCode(bankCode, kControlBankCode) || registerIndex < 2 ||
        registerIndex > 6)
        return false;
    const bool graphicsII = m_legacy.GetMode() ==
        Pico9918LegacyCore::Mode::GraphicsII;
    const u32 value = m_legacy.ReadRegister(registerIndex);
    switch (registerIndex)
    {
        case 2:
            address = m_legacy.GetMode() == Pico9918LegacyCore::Mode::SmsMode4
                ? SmsMode4NameTableBase(static_cast<u8>(value),
                    m_legacy.ActiveLineCount())
                : (value & 0x0F) << 10;
            return true;
        case 3: address = (value & (graphicsII ? 0x80 : 0xFF)) << 6; return true;
        case 4: address = (value & (graphicsII ? 0x04 : 0x07)) << 11; return true;
        case 5: address = (value & 0x7F) << 7; return true;
        case 6: address = (value & 0x07) << 11; return true;
        default: return false;
    }
}

bool Pico9918::DescribeDebugScene(
    GC_VideoDebugSceneDescriptor& descriptor) const
{
    descriptor = {};
    descriptor.canvas_width = 256;
    descriptor.canvas_height = static_cast<u32>(LogicalContentHeight());
    return true;
}

bool Pico9918::DescribeDebugLayer(std::size_t index,
    GC_VideoDebugLayerDescriptor& descriptor) const
{
    if (index > 2)
        return false;
    const Pico9918LegacyCore::Mode mode = m_legacy.GetMode();
    const bool graphicsII = mode == Pico9918LegacyCore::Mode::GraphicsII;
    const bool smsMode4 = mode == Pico9918LegacyCore::Mode::SmsMode4;
    if (index == 2)
    {
        const bool smsMode4 =
            mode == Pico9918LegacyCore::Mode::SmsMode4;
        descriptor = {};
        descriptor.code = "PICO9918.SPRPAT";
        descriptor.name = "Sprite Patterns";
        descriptor.kind = GC_VideoDebugLayerKind::TileCatalog;
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
        descriptor.primary_address = smsMode4
            ? ((m_legacy.ReadRegister(6) & 0x04) ? 0x2000u : 0u)
            : static_cast<u32>(m_legacy.ReadRegister(6) & 0x07) << 11;
        descriptor.address_width_bits = 14;
        return true;
    }
    if (index == 1)
    {
        const u32 tiles = smsMode4 ? 512 : (graphicsII ? 768 : 256);
        descriptor = {};
        descriptor.code = "PICO9918.PATTERNS";
        descriptor.name = "Background Patterns";
        descriptor.kind = GC_VideoDebugLayerKind::TileCatalog;
        descriptor.visible = true;
        descriptor.has_grid = true;
        descriptor.content_width = 256;
        descriptor.content_height = ((tiles + 31) / 32) * 8;
        descriptor.columns = 32;
        descriptor.rows = (tiles + 31) / 32;
        descriptor.active_cell_count = tiles;
        descriptor.cell_width = 8;
        descriptor.cell_height = 8;
        descriptor.rows_per_region = graphicsII ? 8 : 0;
        descriptor.primary_address_label = "Pattern Table";
        descriptor.primary_address = smsMode4 ? 0u
            : static_cast<u32>(m_legacy.ReadRegister(4) &
                (graphicsII ? 0x04 : 0x07)) << 11;
        descriptor.address_width_bits = 14;
        return true;
    }

    const bool text = mode == Pico9918LegacyCore::Mode::Text;
    descriptor = {};
    descriptor.code = "PICO9918.NAMETABLE";
    descriptor.name = "Name Table";
    descriptor.kind = GC_VideoDebugLayerKind::TileGrid;
    descriptor.visible = true;
    descriptor.has_grid = true;
    descriptor.has_viewport = smsMode4;
    descriptor.content_width = text ? 240 : 256;
    descriptor.content_height = smsMode4
        ? SmsMode4NameTableRows(m_legacy.ActiveLineCount()) * 8
        : static_cast<u32>(LogicalContentHeight());
    descriptor.columns = text ? 40 : 32;
    descriptor.rows = smsMode4
        ? SmsMode4NameTableRows(m_legacy.ActiveLineCount())
        : static_cast<u32>(LogicalContentHeight() / 8);
    descriptor.active_cell_count = descriptor.columns * descriptor.rows;
    descriptor.cell_width = text ? 6 : 8;
    descriptor.cell_height = 8;
    descriptor.rows_per_region = graphicsII ? 8 : 0;
    descriptor.primary_address_label = "Name Table Base";
    descriptor.primary_address = smsMode4
        ? SmsMode4NameTableBase(m_legacy.ReadRegister(2),
            m_legacy.ActiveLineCount())
        : static_cast<u32>(m_legacy.ReadRegister(2) & 0x0F) << 10;
    descriptor.address_width_bits = 14;
    return true;
}

std::size_t Pico9918::GetDebugViewportRegionCount(std::size_t index) const
{
    if (index != 0 ||
        m_legacy.GetMode() != Pico9918LegacyCore::Mode::SmsMode4)
        return 0;
    return 1;
}

bool Pico9918::DescribeDebugViewportRegion(std::size_t index,
    std::size_t regionIndex,
    GC_VideoDebugViewportRegionDescriptor& descriptor) const
{
    const std::size_t count = GetDebugViewportRegionCount(index);
    if (regionIndex >= count) return false;

    const u32 activeHeight = m_legacy.ActiveLineCount();
    const u32 mapHeight = activeHeight == 192 ? 224u : 256u;
    descriptor = {};
    // Show one logical screen viewport. The shared viewer draws every segment
    // when it wraps across a map edge, so none of the sampled area disappears.
    // H-lock and V-lock still affect the output but do not fragment this useful
    // overview into four overlapping rectangles.
    descriptor.source_x = (256u - m_legacy.ReadRegister(8)) & 0xFFu;
    descriptor.source_y = m_legacy.ReadRegister(9) % mapHeight;
    descriptor.width = 256;
    descriptor.height = activeHeight;
    return true;
}

void Pico9918::BuildDebugLayerSurface(std::size_t index, u64 optionMask) const
{
    const auto reg = [this](int number) { return m_legacy.ReadRegister(number); };
    const auto vram = [this](u32 address) {
        return m_legacy.ReadVram(address & 0x3FFF);
    };
    u8 paletteMask = 0x3F;
    const u8* palette = DrawPalette(paletteMask);
    const auto put = [palette, paletteMask](u8* surface, u32 stride, int x, int y, int color) {
        u8* rgba = surface + (static_cast<std::size_t>(y) * stride + x) * 4;
        const u8* rgb = palette + (color & paletteMask) * 3;
        rgba[0] = rgb[0]; rgba[1] = rgb[1]; rgba[2] = rgb[2]; rgba[3] = 0xFF;
    };
    const Pico9918LegacyCore::Mode mode = m_legacy.GetMode();
    const bool graphicsII = mode == Pico9918LegacyCore::Mode::GraphicsII;
    const bool text = mode == Pico9918LegacyCore::Mode::Text;
    const bool multicolor = mode == Pico9918LegacyCore::Mode::Multicolor;
    const bool smsMode4 = mode == Pico9918LegacyCore::Mode::SmsMode4;
    const int tilePaletteBase = (m_extendedRegisters[24] & 0x03) << 4;
    const int spritePaletteBase = m_extendedRegisters[24] & 0x30;
    const int tileEcm = (m_extendedRegisters[49] >> 4) & 0x03;
    const int tileEcmOffset = 0x800 >> ((m_extendedRegisters[29] & 0x0C) >> 2);
    const int spriteEcm = m_extendedRegisters[49] & 0x03;
    const int spriteEcmOffset = 0x800 >> ((m_extendedRegisters[29] & 0xC0) >> 6);
    const auto ecmColor = [tilePaletteBase, tileEcm](u8 attr, int pixel) {
        const int mask = tileEcm == 3 ? 0x0E : 0x0F;
        const int shift = tileEcm == 3 ? 2 : tileEcm;
        const int palette = tileEcm == 1 ? (tilePaletteBase & 0x20) : 0;
        return palette | ((attr & mask) << shift) | pixel;
    };
    const auto spriteEcmColor = [spritePaletteBase, spriteEcm](u8 attr,
        int pixel) {
        const int mask = spriteEcm == 3 ? 0x0E : 0x0F;
        const int shift = spriteEcm == 3 ? 2 : spriteEcm;
        const int palette = spriteEcm == 1 ? (spritePaletteBase & 0x20) : 0;
        return palette | ((attr & mask) << shift) | pixel;
    };
    int backdrop = reg(7) & 0x0F;
    backdrop = backdrop ? backdrop : 1;

    if (index == 0)
    {
        std::fill(m_debugNameTable.begin(), m_debugNameTable.end(), 0);
        const int rows = smsMode4
            ? static_cast<int>(SmsMode4NameTableRows(
                m_legacy.ActiveLineCount()))
            : LogicalContentHeight() / 8;
        const std::size_t surfaceHeight = static_cast<std::size_t>(
            std::max(256, LogicalContentHeight()));
        const std::size_t surfacePixels = 256u * surfaceHeight;
        for (std::size_t p = 0; p < surfacePixels; ++p)
            m_debugNameTable[p * 4 + 3] = 0xFF;
        const u32 nameBase = smsMode4
            ? SmsMode4NameTableBase(reg(2), m_legacy.ActiveLineCount())
            : static_cast<u32>(reg(2) & 0x0F) << 10;
        const u32 colorBase = static_cast<u32>(reg(3)) << 6;
        const u32 patternBase = static_cast<u32>(reg(4) & 0x07) << 11;
        const int columns = text ? 40 : 32;
        int textForeground = (reg(7) >> 4) & 0x0F;
        textForeground = textForeground ? textForeground : backdrop;
        const int modeBits = graphicsII ? 2 : (text ? 1 : (multicolor ? 4 : 0));
        for (int row = 0; row < rows; ++row)
            for (int column = 0; column < columns; ++column)
            {
                if (smsMode4)
                {
                    const u32 entry = nameBase + (row * 32 + column) * 2;
                    const u16 word = static_cast<u16>(vram(entry) |
                        (static_cast<u16>(vram(entry + 1)) << 8));
                    const u16 tile = word & 0x01FF;
                    const int palette = (word & 0x0800) ? 16 : 0;
                    const bool flipX = (word & 0x0200) != 0;
                    const bool flipY = (word & 0x0400) != 0;
                    for (int line = 0; line < 8; ++line)
                    {
                        const int sourceLine = flipY ? 7 - line : line;
                        const u32 address = tile * 32 + sourceLine * 4;
                        const u8 planes[4] = {vram(address),
                            vram(address + 1), vram(address + 2),
                            vram(address + 3)};
                        for (int pixel = 0; pixel < 8; ++pixel)
                        {
                            const int sourcePixel = flipX ? 7 - pixel : pixel;
                            const int mask = 0x80 >> sourcePixel;
                            const int value = (planes[0] & mask ? 1 : 0) |
                                (planes[1] & mask ? 2 : 0) |
                                (planes[2] & mask ? 4 : 0) |
                                (planes[3] & mask ? 8 : 0);
                            put(m_debugNameTable.data(), 256,
                                column * 8 + pixel, row * 8 + line,
                                palette | value);
                        }
                    }
                    continue;
                }
                const u8 tile = vram(nameBase + row * columns + column);
                for (int line = 0; line < 8; ++line)
                {
                    if (tileEcm && !graphicsII && !text && !multicolor)
                    {
                        const u32 address = patternBase + (tile << 3) + line;
                        const u8 plane0 = vram(address);
                        const u8 plane1 = tileEcm >= 2
                            ? vram(address + tileEcmOffset) : 0;
                        const u8 plane2 = tileEcm >= 3
                            ? vram(address + tileEcmOffset * 2) : 0;
                        const u8 attr = vram(colorBase + tile);
                        for (int pixel = 0; pixel < 8; ++pixel)
                        {
                            const int mask = 0x80 >> pixel;
                            const int value = (plane0 & mask ? 1 : 0) |
                                (plane1 & mask ? 2 : 0) |
                                (plane2 & mask ? 4 : 0);
                            put(m_debugNameTable.data(), 256,
                                column * 8 + pixel, row * 8 + line,
                                ecmColor(attr, value));
                        }
                        continue;
                    }
                    if (multicolor)
                    {
                        const u8 colors = vram(patternBase + (tile << 3) +
                            ((row & 3) << 1) + (line >= 4 ? 1 : 0));
                        int left = colors >> 4, right = colors & 0x0F;
                        left = left ? left : backdrop;
                        right = right ? right : backdrop;
                        for (int pixel = 0; pixel < 8; ++pixel)
                            put(m_debugNameTable.data(), 256,
                                column * 8 + pixel, row * 8 + line,
                                tilePaletteBase | (pixel < 4 ? left : right));
                        continue;
                    }
                    const u16 rasterLine = static_cast<u16>(row * 8 + line);
                    const u8 pattern = graphicsII
                        ? vram(TMS99xxTableAddress::Pattern(modeBits, reg(3),
                            reg(4), rasterLine, tile))
                        : vram(patternBase + (tile << 3) + line);
                    const u8 colors = text ? 0 : (graphicsII
                        ? vram(TMS99xxTableAddress::Colour(modeBits, reg(3),
                            rasterLine, tile))
                        : vram(colorBase + (tile >> 3)));
                    int foreground = text ? textForeground : colors >> 4;
                    int background = text ? backdrop : colors & 0x0F;
                    foreground = foreground ? foreground : backdrop;
                    background = background ? background : backdrop;
                    const int pixels = text ? 6 : 8;
                    for (int pixel = 0; pixel < pixels; ++pixel)
                        put(m_debugNameTable.data(), 256,
                            column * pixels + pixel, row * 8 + line,
                            tilePaletteBase | ((pattern & (0x80 >> pixel))
                                ? foreground : background));
                }
            }
        ++m_debugSurfaceGeneration;
        return;
    }

    if (index == 1)
    {
        for (std::size_t p = 0; p < 256u * 256u; ++p)
        {
            m_debugPatternTable[p * 4 + 0] = 4;
            m_debugPatternTable[p * 4 + 1] = 4;
            m_debugPatternTable[p * 4 + 2] = 4;
            m_debugPatternTable[p * 4 + 3] = 0xFF;
        }
        const u32 patternBase = smsMode4 ? 0u
            : static_cast<u32>(reg(4) &
                (graphicsII ? 0x04 : 0x07)) << 11;
        const u32 colorBase = graphicsII
            ? static_cast<u32>(reg(3) & 0x80) << 6
            : static_cast<u32>(reg(3)) << 6;
        const int tiles = smsMode4 ? 512 : (graphicsII ? 768 : 256);
        int textForeground = (reg(7) >> 4) & 0x0F;
        textForeground = textForeground ? textForeground : backdrop;
        for (int tile = 0; tile < tiles; ++tile)
            for (int line = 0; line < 8; ++line)
            {
                if (smsMode4)
                {
                    const u32 address = tile * 32 + line * 4;
                    const u8 planes[4] = {vram(address),
                        vram(address + 1), vram(address + 2),
                        vram(address + 3)};
                    for (int pixel = 0; pixel < 8; ++pixel)
                    {
                        const int mask = 0x80 >> pixel;
                        const int value = (planes[0] & mask ? 1 : 0) |
                            (planes[1] & mask ? 2 : 0) |
                            (planes[2] & mask ? 4 : 0) |
                            (planes[3] & mask ? 8 : 0);
                        put(m_debugPatternTable.data(), 256,
                            (tile & 31) * 8 + pixel,
                            (tile >> 5) * 8 + line, value);
                    }
                    continue;
                }
                const u8 pattern = vram(patternBase + (tile << 3) + line);
                for (int pixel = 0; pixel < 8; ++pixel)
                {
                    if (tileEcm && !graphicsII && !text && !multicolor)
                    {
                        const u32 address = patternBase + (tile << 3) + line;
                        const int mask = 0x80 >> pixel;
                        const int value = (vram(address) & mask ? 1 : 0) |
                            (tileEcm >= 2 && (vram(address + tileEcmOffset) & mask) ? 2 : 0) |
                            (tileEcm >= 3 && (vram(address + tileEcmOffset * 2) & mask) ? 4 : 0);
                        put(m_debugPatternTable.data(), 256,
                            (tile & 31) * 8 + pixel, (tile >> 5) * 8 + line,
                            ecmColor(vram(colorBase + tile), value));
                        continue;
                    }
                    int foreground = 15, background = 0;
                    if (text)
                    {
                        foreground = textForeground; background = backdrop;
                    }
                    else if (multicolor)
                    {
                        const u8 colors = vram(patternBase + (tile << 3) +
                            ((line >> 2) << 1));
                        foreground = pixel < 4 ? colors >> 4 : colors & 0x0F;
                        background = foreground;
                    }
                    else
                    {
                        const u8 colors = vram(graphicsII
                            ? colorBase + (tile << 3) + line
                            : colorBase + (tile >> 3));
                        foreground = colors >> 4; background = colors & 0x0F;
                        foreground = foreground ? foreground : backdrop;
                        background = background ? background : backdrop;
                    }
                    put(m_debugPatternTable.data(), 256,
                        (tile & 31) * 8 + pixel, (tile >> 5) * 8 + line,
                        tilePaletteBase | (multicolor ? foreground :
                            ((pattern & (0x80 >> pixel)) ? foreground : background)));
                }
            }
        ++m_debugSurfaceGeneration;
        return;
    }

    std::fill(m_debugSpritePatterns.begin(), m_debugSpritePatterns.end(), 0);
    const bool showColors = (optionMask & 1) != 0;
    const u32 patternBase = smsMode4
        ? ((reg(6) & 0x04) ? 0x2000u : 0u)
        : static_cast<u32>(reg(6) & 0x07) << 11;
    if (smsMode4)
    {
        for (int tile = 0; tile < 256; ++tile)
            for (int line = 0; line < 8; ++line)
            {
                const u32 address = patternBase + tile * 32 + line * 4;
                const u8 planes[4] = {vram(address), vram(address + 1),
                    vram(address + 2), vram(address + 3)};
                for (int pixel = 0; pixel < 8; ++pixel)
                {
                    const int mask = 0x80 >> pixel;
                    const int value = (planes[0] & mask ? 1 : 0) |
                        (planes[1] & mask ? 2 : 0) |
                        (planes[2] & mask ? 4 : 0) |
                        (planes[3] & mask ? 8 : 0);
                    put(m_debugSpritePatterns.data(), 256,
                        (tile & 31) * 8 + pixel,
                        (tile >> 5) * 8 + line, 16 | value);
                }
            }
        ++m_debugSurfaceGeneration;
        return;
    }
    std::array<u8, 256> spriteAttributes{};
    std::array<bool, 256> spriteAttributePresent{};
    if (showColors)
    {
        const u32 sat = static_cast<u32>(reg(5) & 0x7F) << 7;
        const int count = (reg(1) & 0x02) ? 4 : 1;
        const bool row30 = HasThirtyRows();
        for (int sprite = 0; sprite < 32; ++sprite)
        {
            const u32 address = sat + sprite * 4;
            if (vram(address) == 0xD0 && !row30) break;
            const u8 attribute = vram(address + 3);
            int tile = vram(address + 2);
            if (count == 4) tile &= 0xFC;
            for (int part = 0; part < count; ++part)
                if (!spriteAttributePresent[(tile + part) & 0xFF])
                {
                    spriteAttributes[(tile + part) & 0xFF] = attribute;
                    spriteAttributePresent[(tile + part) & 0xFF] = true;
                }
        }
    }
    for (int tile = 0; tile < 256; ++tile)
        for (int line = 0; line < 8; ++line)
        {
            const u32 address = patternBase + (tile << 3) + line;
            const u8 plane0 = vram(address);
            const u8 plane1 = spriteEcm >= 2
                ? vram(address + spriteEcmOffset) : 0;
            const u8 plane2 = spriteEcm >= 3
                ? vram(address + spriteEcmOffset * 2) : 0;
            for (int pixel = 0; pixel < 8; ++pixel)
            {
                const int mask = 0x80 >> pixel;
                const int value = (plane0 & mask ? 1 : 0) |
                    (plane1 & mask ? 2 : 0) | (plane2 & mask ? 4 : 0);
                const int color = value && spriteAttributePresent[tile]
                    ? (spriteEcm ? spriteEcmColor(spriteAttributes[tile], value)
                        : (spritePaletteBase | (spriteAttributes[tile] & 0x0F)))
                    : (value ? (spritePaletteBase | 15) : (spritePaletteBase | 1));
                put(m_debugSpritePatterns.data(), 256,
                    (tile & 31) * 8 + pixel, (tile >> 5) * 8 + line,
                    color);
            }
        }
    ++m_debugSurfaceGeneration;
}

bool Pico9918::ReadDebugLayerSurface(std::size_t index,
    GC_VideoDebugLayerSurface& surface) const
{
    return ReadDebugLayerSurfaceWithOptions(index, index == 2 ? 1 : 0, surface);
}

std::size_t Pico9918::GetDebugLayerOptionCount(std::size_t index) const
{
    return index == 2 &&
        m_legacy.GetMode() != Pico9918LegacyCore::Mode::SmsMode4 ? 1 : 0;
}

bool Pico9918::DescribeDebugLayerOption(std::size_t layerIndex,
    std::size_t optionIndex,
    GC_VideoDebugLayerOptionDescriptor& descriptor) const
{
    if (layerIndex != 2 || optionIndex != 0) return false;
    descriptor = {"PICO9918.SPRPAT.SHOW_COLORS", "Show colors", true};
    return true;
}

bool Pico9918::ReadDebugLayerSurfaceWithOptions(std::size_t index,
    u64 optionMask, GC_VideoDebugLayerSurface& surface) const
{
    if (index > 2) return false;
    BuildDebugLayerSurface(index, optionMask);
    surface = {};
    surface.rgba = index == 0 ? m_debugNameTable.data()
        : (index == 1 ? m_debugPatternTable.data() : m_debugSpritePatterns.data());
    surface.width = 256;
    const auto mode = m_legacy.GetMode();
    surface.height = index == 0
        ? static_cast<u32>(std::max(256, mode ==
            Pico9918LegacyCore::Mode::SmsMode4
                ? static_cast<int>(SmsMode4NameTableRows(
                    m_legacy.ActiveLineCount()) * 8)
                : LogicalContentHeight()))
        : (index == 1 && mode == Pico9918LegacyCore::Mode::GraphicsII ? 192
            : (index == 1 && mode == Pico9918LegacyCore::Mode::SmsMode4
                ? 128 : 64));
    surface.row_pitch = 256 * 4;
    surface.generation = m_debugSurfaceGeneration;
    return true;
}

std::size_t Pico9918::GetDebugLayerAreaCount(std::size_t index) const
{
    if (index == 2) return 1;
    if (index != 1) return 0;
    const auto mode = m_legacy.GetMode();
    return mode == Pico9918LegacyCore::Mode::GraphicsI ||
        mode == Pico9918LegacyCore::Mode::GraphicsII ? 2 : 1;
}

bool Pico9918::DescribeDebugLayerArea(std::size_t index,
    std::size_t areaIndex, GC_VideoDebugLayerAreaDescriptor& descriptor) const
{
    if (areaIndex >= GetDebugLayerAreaCount(index)) return false;
    descriptor = {};
    const bool graphicsII =
        m_legacy.GetMode() == Pico9918LegacyCore::Mode::GraphicsII;
    const bool smsMode4 =
        m_legacy.GetMode() == Pico9918LegacyCore::Mode::SmsMode4;
    if (index == 2)
    {
        if (smsMode4)
        {
            descriptor = {"SPRPAT", "Sprite Pattern Table",
                (m_legacy.ReadRegister(6) & 0x04) ? 0x2000u : 0u,
                0x2000, 14};
            return true;
        }
        descriptor = {"SPRPAT", "Sprite Pattern Table",
            static_cast<u32>(m_legacy.ReadRegister(6) & 7) << 11, 0x800, 14};
        return true;
    }
    if (areaIndex == 0)
    {
        descriptor = {"PAT", "Pattern Table",
            smsMode4 ? 0u : static_cast<u32>(m_legacy.ReadRegister(4) &
                (graphicsII ? 4 : 7)) << 11,
            smsMode4 ? 0x4000u : (graphicsII ? 0x1800u : 0x800u), 14};
        return true;
    }
    descriptor = {"COL", "Color Table",
        graphicsII ? static_cast<u32>(m_legacy.ReadRegister(3) & 0x80) << 6
                   : static_cast<u32>(m_legacy.ReadRegister(3)) << 6,
        graphicsII ? 0x1800u : 0x20u, 14};
    return true;
}

bool Pico9918::DescribeDebugGridCell(std::size_t index, u32 column, u32 row,
    GC_VideoDebugGridCell& cell) const
{
    GC_VideoDebugLayerDescriptor layer;
    if (!DescribeDebugLayer(index, layer) || column >= layer.columns ||
        row >= layer.rows) return false;
    cell = {};
    cell.column = column; cell.row = row;
    const u32 item = row * layer.columns + column;
    cell.valid = item < layer.active_cell_count;
    const bool smsMode4 =
        m_legacy.GetMode() == Pico9918LegacyCore::Mode::SmsMode4;
    cell.navigation_address = index == 0
        ? (layer.primary_address + item * (smsMode4 ? 2u : 1u)) & 0x3FFF
        : (layer.primary_address + item * (smsMode4 ? 32u : 8u)) & 0x3FFF;
    return true;
}

bool Pico9918::DescribeDebugPreviewPart(std::size_t index,
    GC_VideoDebugPreviewPartDescriptor& descriptor) const
{
    descriptor = {};
    switch (index)
    {
        case 0:
            descriptor.code = "PICO9918.BACKGROUND";
            descriptor.name = "Background";
            descriptor.visible = true;
            return true;
        case 1:
            descriptor.code = "PICO9918.SPRITES";
            descriptor.name = "Sprites";
            descriptor.visible = true;
            return true;
        default:
            return false;
    }
}

bool Pico9918::DescribeDebugPreviewOption(std::size_t index,
    GC_VideoDebugPreviewOptionDescriptor& descriptor) const
{
    if (index != 0)
        return false;
    descriptor = {};
    descriptor.code = "PICO9918.DIM_HIDDEN_BACKGROUND";
    descriptor.name = "Dim hidden BG";
    descriptor.enabled = true;
    return true;
}

void Pico9918::BuildDebugPreviewSurface(u64 visiblePartMask,
    u64 optionMask) const
{
    if (m_debugPreviewBuiltSourceGeneration ==
            m_debugPreviewSourceGeneration &&
        m_debugPreviewBuiltPartMask == visiblePartMask &&
        m_debugPreviewBuiltOptionMask == optionMask)
        return;

    const bool showBackground = (visiblePartMask & (1ull << 0)) != 0;
    const bool showSprites = (visiblePartMask & (1ull << 1)) != 0;
    const bool dimBackground = !showBackground && showSprites &&
        (optionMask & (1ull << 0)) != 0;
    const std::size_t pixels = static_cast<std::size_t>(
        Pico9918LegacyCore::Width * LogicalContentHeight());
    u8 paletteMask = 0x3F;
    const u8* palette = DrawPalette(paletteMask);
    for (std::size_t index = 0; index < pixels; ++index)
    {
        const bool spritePixel = showSprites && m_debugSpriteMask[index] != 0;
        const u8 color = spritePixel
            ? m_debugComposed[index] : m_debugBackground[index];
        const u8* rgb = palette + (color & paletteMask) * 3;
        u8* output = m_debugPreview.data() + index * 4;
        if (spritePixel || showBackground || dimBackground)
        {
            const int divisor = !spritePixel && dimBackground ? 4 : 1;
            output[0] = static_cast<u8>(rgb[0] / divisor);
            output[1] = static_cast<u8>(rgb[1] / divisor);
            output[2] = static_cast<u8>(rgb[2] / divisor);
        }
        else
        {
            output[0] = 0;
            output[1] = 0;
            output[2] = 0;
        }
        output[3] = 0xFF;
    }
    m_debugPreviewBuiltSourceGeneration = m_debugPreviewSourceGeneration;
    m_debugPreviewBuiltPartMask = visiblePartMask;
    m_debugPreviewBuiltOptionMask = optionMask;
    ++m_debugPreviewGeneration;
}

bool Pico9918::ReadDebugPreviewSurface(u64 visiblePartMask, u64 optionMask,
    GC_VideoDebugLayerSurface& surface) const
{
    BuildDebugPreviewSurface(visiblePartMask, optionMask);
    surface = {};
    surface.rgba = m_debugPreview.data();
    surface.width = Pico9918LegacyCore::Width;
    surface.height = static_cast<u32>(LogicalContentHeight());
    surface.row_pitch = Pico9918LegacyCore::Width * 4;
    surface.generation = m_debugPreviewGeneration;
    return true;
}

bool Pico9918::DescribeDebugSpriteSource(std::size_t index,
    GC_VideoDebugSpriteSourceDescriptor& descriptor) const
{
    if (index != 0) return false;
    descriptor = {"live-sat", "Live SAT", nullptr,
        GC_VIDEO_DEBUG_SPRITE_CAP_SCREEN_PREVIEW, nullptr, nullptr};
    return true;
}

bool Pico9918::ReadDebugSpriteAtlas(const char* sourceCode, int,
    GC_VideoDebugSpriteAtlasSurface& surface) const
{
    if (!sourceCode || std::strcmp(sourceCode, "live-sat") != 0) return false;
    std::fill(m_debugSpriteAtlas.begin(), m_debugSpriteAtlas.end(), 0);
    for (std::size_t p = 0; p < 128u * 128u; ++p)
        m_debugSpriteAtlas[p * 4 + 3] = 0xFF;
    if (m_legacy.GetMode() == Pico9918LegacyCore::Mode::SmsMode4)
    {
        const u32 sat = static_cast<u32>(
            m_legacy.ReadRegister(5) & 0x7E) << 7;
        const u32 patterns = (m_legacy.ReadRegister(6) & 0x04)
            ? 0x2000u : 0u;
        const int size = (m_legacy.ReadRegister(1) & 0x02) ? 16 : 8;
        const int zoom = (m_legacy.ReadRegister(1) & 0x01) ? 2 : 1;
        const bool hasTerminator = m_legacy.ActiveLineCount() == 192;
        bool ended = false;
        for (int sprite = 0; sprite < 64; ++sprite)
        {
            auto& item = m_debugSpriteItems[sprite];
            item = {};
            item.index = sprite;
            item.atlas_x = (sprite & 7) * 16;
            item.atlas_y = (sprite >> 3) * 16;
            item.width = 8;
            item.height = size;
            const u8 rawY = m_legacy.ReadVram((sat + sprite) & 0x3FFF);
            if (hasTerminator && rawY == 0xD0) ended = true;
            if (ended)
            {
                item.visual_state = GC_VideoDebugSpriteVisualState::Absent;
                continue;
            }
            const u32 xp = sat + 0x80 + sprite * 2;
            const int x = m_legacy.ReadVram(xp & 0x3FFF) -
                ((m_legacy.ReadRegister(0) & 0x08) ? 8 : 0);
            u8 tile = m_legacy.ReadVram((xp + 1) & 0x3FFF);
            if (size == 16) tile &= 0xFE;
            const u8 top = static_cast<u8>(rawY + 1);
            const int y = top >= 0xF1 ? static_cast<int>(top) - 256 : top;
            item.visual_state = GC_VideoDebugSpriteVisualState::Complete;
            item.screen_x = static_cast<float>(x);
            item.screen_y = static_cast<float>(y);
            item.screen_width = static_cast<float>(8 * zoom);
            item.screen_height = static_cast<float>(size * zoom);
            bool hasPixels = false;
            for (int py = 0; py < size; ++py)
                for (int px = 0; px < 8; ++px)
                {
                    const u8 sourceTile = static_cast<u8>(tile + (py >> 3));
                    const u32 address = patterns + sourceTile * 32 +
                        (py & 7) * 4;
                    const u8 mask = static_cast<u8>(0x80 >> px);
                    const u8 value = static_cast<u8>(
                        (m_legacy.ReadVram(address & 0x3FFF) & mask ? 1 : 0) |
                        (m_legacy.ReadVram((address + 1) & 0x3FFF) & mask ? 2 : 0) |
                        (m_legacy.ReadVram((address + 2) & 0x3FFF) & mask ? 4 : 0) |
                        (m_legacy.ReadVram((address + 3) & 0x3FFF) & mask ? 8 : 0));
                    if (!value) continue;
                    hasPixels = true;
                    u8* rgba = m_debugSpriteAtlas.data() +
                        ((item.atlas_y + py) * 128 + item.atlas_x + px) * 4;
                    const u8* rgb = m_paletteRgb.data() + (16 + value) * 3;
                    rgba[0] = rgb[0]; rgba[1] = rgb[1]; rgba[2] = rgb[2];
                }
            item.has_screen_rect = hasPixels;
        }
        surface = {};
        surface.rgba = m_debugSpriteAtlas.data();
        surface.width = 128; surface.height = 128;
        surface.row_pitch = 128 * 4;
        surface.items = m_debugSpriteItems.data(); surface.item_count = 64;
        surface.generation = ++m_debugSurfaceGeneration;
        return true;
    }
    const u32 sat = static_cast<u32>(m_legacy.ReadRegister(5) & 0x7F) << 7;
    const u32 patterns = static_cast<u32>(m_legacy.ReadRegister(6) & 7) << 11;
    const int size = (m_legacy.ReadRegister(1) & 0x02) ? 16 : 8;
    const int zoom = (m_legacy.ReadRegister(1) & 0x01) ? 2 : 1;
    const int verticalScale = HasDoubleRows() ? 2 : 1;
    const u8 spritePaletteBase = m_extendedRegisters[24] & 0x30;
    const u8 spriteEcm = m_extendedRegisters[49] & 0x03;
    const u16 spriteEcmOffset = static_cast<u16>(
        0x800 >> ((m_extendedRegisters[29] & 0xC0) >> 6));
    const bool row30 = HasThirtyRows();
    const bool realSpriteY = (m_extendedRegisters[49] & 0x08) != 0;
    bool ended = false;
    for (int sprite = 0; sprite < 32; ++sprite)
    {
        auto& item = m_debugSpriteItems[sprite];
        item = {};
        item.index = sprite;
        item.atlas_x = (sprite & 7) * 16;
        item.atlas_y = (sprite >> 3) * 16;
        item.width = size; item.height = size;
        const u32 attr = sat + sprite * 4;
        const u8 rawY = m_legacy.ReadVram(attr & 0x3FFF);
        if (rawY == 0xD0 && !row30) ended = true;
        if (ended)
        {
            item.visual_state = GC_VideoDebugSpriteVisualState::Absent;
            continue;
        }
        int x = m_legacy.ReadVram((attr + 1) & 0x3FFF);
        int y = rawY;
        const int wrapThreshold = row30
            ? (realSpriteY ? 0xF0 : 0xEF) : 0xE0;
        if (y > wrapThreshold) y -= 256;
        if (!realSpriteY) ++y;
        u8 tile = m_legacy.ReadVram((attr + 2) & 0x3FFF);
        const u8 flags = m_legacy.ReadVram((attr + 3) & 0x3FFF);
        if (size == 16) tile &= 0xFC;
        if (flags & 0x80) x -= 32;
        item.visual_state = GC_VideoDebugSpriteVisualState::Complete;
        item.has_screen_rect = spriteEcm || (flags & 0x0F) != 0;
        item.screen_x = static_cast<float>(x);
        item.screen_y = static_cast<float>(y * verticalScale);
        item.screen_width = static_cast<float>(size * zoom);
        item.screen_height = static_cast<float>(size * zoom * verticalScale);
        for (int py = 0; py < size; ++py)
            for (int px = 0; px < size; ++px)
            {
                const u32 address = patterns + (tile << 3) + py +
                    (px >= 8 ? 16 : 0);
                const u8 mask = static_cast<u8>(0x80 >> (px & 7));
                const u8 value = static_cast<u8>(
                    (m_legacy.ReadVram(address & 0x3FFF) & mask ? 1 : 0) |
                    (spriteEcm >= 2 && m_legacy.ReadVram(
                        (address + spriteEcmOffset) & 0x3FFF) & mask ? 2 : 0) |
                    (spriteEcm >= 3 && m_legacy.ReadVram(
                        (address + spriteEcmOffset * 2) & 0x3FFF) & mask ? 4 : 0));
                if (value == 0)
                    continue;
                u8* rgba = m_debugSpriteAtlas.data() +
                    ((item.atlas_y + py) * 128 + item.atlas_x + px) * 4;
                const u8 paletteIndex = spriteEcm
                    ? static_cast<u8>((spriteEcm == 1
                        ? (spritePaletteBase & 0x20) : 0) |
                        ((flags & (spriteEcm == 3 ? 0x0E : 0x0F)) <<
                            (spriteEcm == 3 ? 2 : spriteEcm)) | value)
                    : static_cast<u8>(spritePaletteBase | (flags & 0x0F));
                u8 paletteMask = 0x3F;
                const u8* rgb = DrawPalette(paletteMask) + (paletteIndex & paletteMask) * 3;
                rgba[0] = rgb[0]; rgba[1] = rgb[1]; rgba[2] = rgb[2];
            }
    }
    surface = {};
    surface.rgba = m_debugSpriteAtlas.data();
    surface.width = 128; surface.height = 64; surface.row_pitch = 128 * 4;
    surface.items = m_debugSpriteItems.data(); surface.item_count = 32;
    surface.generation = ++m_debugSurfaceGeneration;
    return true;
}

bool Pico9918::DescribeDebugSpriteAttributeTable(std::size_t index,
    GC_VideoDebugSpriteAttributeTableDescriptor& descriptor) const
{
    if (index != 0) return false;
    if (m_legacy.GetMode() == Pico9918LegacyCore::Mode::SmsMode4)
    {
        descriptor = {"PICO9918.SAT", "SAT", 64, 3,
            static_cast<u32>(m_legacy.ReadRegister(5) & 0x7E) << 7, 1, 14};
        return true;
    }
    descriptor = {"PICO9918.SAT", "SAT", 32, 4,
        static_cast<u32>(m_legacy.ReadRegister(5) & 0x7F) << 7, 4, 14};
    return true;
}

bool Pico9918::DescribeDebugSpriteAttributeColumn(std::size_t tableIndex,
    std::size_t columnIndex,
    GC_VideoDebugSpriteAttributeColumnDescriptor& descriptor) const
{
    static const GC_VideoDebugSpriteAttributeColumnDescriptor legacyColumns[] = {
        {"y", "Y", 8}, {"x", "X", 8}, {"pattern", "PAT", 8},
        {"color", "COL/EC", 8}};
    static const GC_VideoDebugSpriteAttributeColumnDescriptor mode4Columns[] = {
        {"y", "Y", 8}, {"x", "X", 8}, {"pattern", "PAT", 8}};
    if (tableIndex != 0) return false;
    const bool smsMode4 =
        m_legacy.GetMode() == Pico9918LegacyCore::Mode::SmsMode4;
    if (columnIndex >= (smsMode4 ? std::size(mode4Columns)
                                 : std::size(legacyColumns))) return false;
    descriptor = smsMode4 ? mode4Columns[columnIndex]
                          : legacyColumns[columnIndex];
    return true;
}

bool Pico9918::DescribeDebugSpriteAttributeRecord(std::size_t tableIndex,
    u32 recordIndex,
    GC_VideoDebugSpriteAttributeRecordDescriptor& descriptor) const
{
    GC_VideoDebugSpriteAttributeTableDescriptor table;
    if (!DescribeDebugSpriteAttributeTable(tableIndex, table) ||
        recordIndex >= table.record_count) return false;
    descriptor = {};
    descriptor.address = table.base_address + recordIndex * 4;
    descriptor.visible = true;
    if (m_legacy.GetMode() == Pico9918LegacyCore::Mode::SmsMode4)
    {
        if (m_legacy.ActiveLineCount() == 192)
            for (u32 index = 0; index <= recordIndex; ++index)
                if (m_legacy.ReadVram(
                        (table.base_address + index) & 0x3FFF) == 0xD0)
                { descriptor.visible = false; break; }
        return true;
    }
    if (!HasThirtyRows())
        for (u32 index = 0; index <= recordIndex; ++index)
            if (m_legacy.ReadVram(
                    (table.base_address + index * 4) & 0x3FFF) == 0xD0)
            { descriptor.visible = false; break; }
    return true;
}

bool Pico9918::ReadDebugSpriteAttributeValue(std::size_t tableIndex,
    u32 recordIndex, u32 columnIndex, u64& value) const
{
    GC_VideoDebugSpriteAttributeTableDescriptor table;
    if (!DescribeDebugSpriteAttributeTable(tableIndex, table) ||
        recordIndex >= table.record_count ||
        columnIndex >= table.column_count) return false;
    if (m_legacy.GetMode() == Pico9918LegacyCore::Mode::SmsMode4)
    {
        const u32 address = columnIndex == 0
            ? table.base_address + recordIndex
            : table.base_address + 0x80 + recordIndex * 2 + columnIndex - 1;
        value = m_legacy.ReadVram(address & 0x3FFF);
        return true;
    }
    value = m_legacy.ReadVram((table.base_address + recordIndex * 4 +
        columnIndex) & 0x3FFF);
    return true;
}

void Pico9918::SaveState(std::ostream& stream)
{
    StateWriter writer(stream);
    writer.U16(GetStateFormatVersion());
    writer.U32(m_rasterClock);
    writer.I32(m_rasterLine);
    writer.U64(m_frameSerial);
    writer.Bool(m_interruptAsserted);
    writer.Bool(m_frameInterruptFlag);
    writer.Bool(m_unlocked);
    writer.U8(m_unlockCount);
    writer.Bytes(m_extendedRegisters.data(), m_extendedRegisters.size());
    writer.Bytes(m_statusRegisters.data(), m_statusRegisters.size());
    writer.U8(m_paletteWriteStage);
    writer.U8(m_paletteWriteFirstByte);
    for (u16 entry : m_paletteEntries)
        writer.U16(entry);
    std::vector<u8> state(m_legacy.StateSize());
    m_legacy.SaveState(state.data());
    writer.U32(static_cast<u32>(state.size()));
    writer.Bytes(state.data(), state.size());
    writer.U32(static_cast<u32>(m_frame.size()));
    writer.Bytes(m_frame.data(), m_frame.size());
}

void Pico9918::LoadState(std::istream& stream)
{
    StateReader reader(stream);
    const u16 version = reader.U16();
    if (version != 3 && version != GetStateFormatVersion())
        return;
    const u32 rasterClock = reader.U32();
    const int rasterLine = reader.I32();
    const u64 frameSerial = reader.U64();
    const bool interruptAsserted = reader.Bool();
    const bool frameInterruptFlag = reader.Bool();
    const bool unlocked = reader.Bool();
    const u8 unlockCount = reader.U8();
    std::array<u8, RegisterCount> extendedRegisters{};
    std::array<u8, StatusCount> statusRegisters{};
    reader.Bytes(extendedRegisters.data(), extendedRegisters.size());
    reader.Bytes(statusRegisters.data(), statusRegisters.size());
    const u8 paletteWriteStage = reader.U8();
    const u8 paletteWriteFirstByte = reader.U8();
    std::array<u16, 64> paletteEntries{};
    for (u16& entry : paletteEntries)
        entry = reader.U16();
    const u32 coreSize = reader.U32();
    std::vector<u8> coreState(coreSize);
    reader.Bytes(coreState.data(), coreState.size());
    const u32 frameSize = reader.U32();
    std::vector<u8> frame(frameSize);
    reader.Bytes(frame.data(), frame.size());
    if (!reader.Ok() || rasterClock >=
            static_cast<u32>(TimingClocksPerLine()) ||
        rasterLine < 0 || rasterLine >= TimingRasterHeight() ||
        unlockCount > 1 || paletteWriteStage > 1 ||
        frame.size() != static_cast<std::size_t>(FrameBufferWidth()) *
            FrameBufferHeight() * 3u ||
        !m_legacy.LoadState(coreState.data(), coreState.size()))
        return;
    m_rasterClock = rasterClock;
    m_rasterLine = rasterLine;
    m_frameSerial = frameSerial;
    m_frameInterruptFlag = frameInterruptFlag;
    m_unlocked = unlocked;
    m_unlockCount = unlockCount;
    m_extendedRegisters = extendedRegisters;
    m_statusRegisters = statusRegisters;
    m_paletteWriteStage = paletteWriteStage;
    m_paletteWriteFirstByte = paletteWriteFirstByte;
    m_paletteEntries = paletteEntries;
    for (std::size_t index = 0; index < m_paletteEntries.size(); ++index)
    {
        RebuildPaletteEntry(index);
        m_legacy.WriteVram(kPaletteMapBase + index * 2,
            static_cast<u8>(m_paletteEntries[index] >> 8));
        m_legacy.WriteVram(kPaletteMapBase + index * 2 + 1,
            static_cast<u8>(m_paletteEntries[index]));
    }
    ++m_paletteRevision;
    m_frame = std::move(frame);
    m_interruptAsserted = !interruptAsserted;
    SetInterrupt(interruptAsserted);
}

DebugDeviceDefinition Pico9918::DescribeDebugDevice() const
{
    DebugDeviceDefinition definition;
    definition.key = "event.sc3000.vdp";
    definition.parentKey = "event.sc3000";
    definition.typeKey = m_device == SystemVideoDevice::SegaGameGearVdp
        ? "video.sega_gamegear_vdp" : IsNativeSmsVdp() ? "video.sega_sms_vdp" :
        (m_legacy.IsSmsBackend() ? "video.picosmsvdp" : "video.pico9918");
    definition.name = GetDeviceDescriptor().display_name;
    const auto addSpace = [&definition](DebugSpaceId id, const char* key,
        const char* name, u32 min, u32 max, u8 accesses, u8 beforeAccesses,
        u8 afterAccesses, u8 width, u8 capabilities,
        const DebugTargetDescriptor* targets, std::size_t targetCount) {
        DebugSpaceDefinition space;
        space.id = id;
        space.key = key;
        space.name = name;
        space.targetMin = min;
        space.targetMax = max;
        space.supportedAccesses = accesses;
        space.beforeValueAccesses = beforeAccesses;
        space.afterValueAccesses = afterAccesses;
        space.defaultBitWidth = width;
        space.capabilities = capabilities;
        for (std::size_t i = 0; i < targetCount; ++i)
            space.targets.push_back({targets[i].id, targets[i].name,
                targets[i].bitWidth});
        definition.spaces.push_back(std::move(space));
    };
    // Published: the System Bus decodes every board's VDP select onto
    // $BE/$BF, as for the TMS99xx.
    addSpace(DebugCommonSpace::IoPorts, "io_ports", "I/O Ports",
        0xBE, 0xBF, kReadWrite, DebugEventAccess_None, kReadWrite, 8,
        kPublished, kPortTargets, std::size(kPortTargets));
    addSpace(DebugCommonSpace::Registers, "registers", "Registers",
        0, RegisterCount - 1, DebugEventAccess_Write, DebugEventAccess_Write,
        DebugEventAccess_Write, 8, kPublished, kRegisterTargets,
        std::size(kRegisterTargets));
    for (u32 index = static_cast<u32>(std::size(kRegisterTargets));
         index < RegisterCount; ++index)
        definition.spaces.back().targets.push_back(
            {index, ControlRegisterNames()[index], 8});
    addSpace(DebugCommonSpace::Memory, "memory", "VRAM",
        0, kHostVramWindowSize - 1, kReadWrite, kReadWrite, kReadWrite, 8,
        kPublished, nullptr, 0);
    addSpace(DebugCommonSpace::State, "state", "Internal State",
        0, Pico9918DebugTarget::PaletteWriteStep,
        DebugEventAccess_Event, DebugEventAccess_Event,
        DebugEventAccess_Event, 8,
        static_cast<u8>(kPublished |
            DebugDescriptorCapability_SnapshotReadable),
        kEventStateTargets, std::size(kEventStateTargets));
    return definition;
}

#endif
