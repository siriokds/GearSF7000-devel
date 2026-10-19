/*
 * GearSF7000 - TMS9918B video device
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TMS9918B.h"

#include <algorithm>
#include <cstring>

#include "SaveStateStream.h"
#include "tms9918b/TMS9918BAddress.h"
#include "tms9918b/TMS9918BCalendar.h"
#include "tms9918b/TMS9918BModes.h"

namespace
{
constexpr int kExtendedActiveMaxWidth = 512;
constexpr int kExtendedActiveHeight = 192;
constexpr int kExtendedSurfaceMaxWidth =
    TMS99xxRasterTiming::DotsPerLine * 2;
constexpr int kExtendedSurfaceMaxHeight = GC_LINES_PER_FRAME_PAL;
constexpr std::size_t kExtendedActiveRgbBytes =
    kExtendedActiveMaxWidth * kExtendedActiveHeight * 3;
constexpr std::size_t kExtendedSurfaceBytes =
    kExtendedSurfaceMaxWidth * kExtendedSurfaceMaxHeight * 3;
constexpr const char* kControlBank = "TMS99XX.CONTROL";
constexpr const char* kStatusBank = "TMS99XX.STATUS";

struct ExtendedRegisterDefinition
{
    const char* code;
    const char* name;
    const char* description;
    u8 mask;
};

constexpr ExtendedRegisterDefinition kExtendedRegisterDefinitions[] = {
    {"TMS9918B.CONTROL.R8", "R8", "Horizontal scroll", 0xFF},
    {"TMS9918B.CONTROL.R9", "R9", "Vertical scroll", 0xFF},
    {"TMS9918B.CONTROL.R10", "R10", "Scanline interrupt reload", 0xFF},
    {"TMS9918B.CONTROL.R11", "R11", "Extended mode and line interrupt", 0x83},
    {"TMS9918B.CONTROL.R12", "R12", "Screen locks, mask and Text64", 0x0F},
    {"TMS9918B.CONTROL.R13", "R13", "Reserved", 0x00},
    {"TMS9918B.CONTROL.R14", "R14", "Reserved", 0x00},
    {"TMS9918B.CONTROL.R15", "R15", "Status register select", 0x0F}
};

bool SameCode(const char* a, const char* b)
{
    return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}
}

TMS9918B::TMS9918B(Memory* memory, CpuInterruptLines* interruptLines)
    : TMS99xx(memory, interruptLines, SystemVideoDevice::TMS9918B)
{
}

void TMS9918B::Init()
{
    m_extendedRgb = std::make_unique<u8[]>(kExtendedActiveRgbBytes);
    m_extendedPacked = std::make_unique<u8[]>(kExtendedSurfaceBytes);
    m_debugNameRgba = std::make_unique<u8[]>(512u * 192u * 4u);
    m_debugPatternRgba = std::make_unique<u8[]>(256u * 192u * 4u);
    m_debugSpritePatternRgba = std::make_unique<u8[]>(256u * 128u * 4u);
    m_debugSpriteAtlasRgba = std::make_unique<u8[]>(128u * 64u * 4u);
    BuildCpuPhaseMaps();
    TMS99xx::Init();
}

void TMS9918B::Reset()
{
    TMS99xx::Reset();
    m_extendedUnlocked = false;
    m_unlockProgress = 0;
    m_paletteWriteMode = false;
    m_paletteAddress = 0;
    m_status1 = 0x18;
    m_lineCounter = 0;
    m_latchedHorizontalScroll = 0;
    m_latchedVerticalScroll = 0;
    m_scrollFrameHorizontal.fill(0);
    m_scrollFrameVertical.fill(0);
    m_scrollFrameControl.fill(0);
    m_lastScrollFrameHorizontal.fill(0);
    m_lastScrollFrameVertical.fill(0);
    m_lastScrollFrameControl.fill(0);
    m_lastScrollFrameValid = false;
    m_extendedRegisters.fill(0);
    m_paletteEntries.fill(0);
    m_paletteRgb.fill(0);
    m_debugFramePaletteRgb.fill(0);
    m_debugFramePaletteValid = false;
    if (m_extendedRgb)
        std::fill_n(m_extendedRgb.get(), kExtendedActiveRgbBytes, 0);
    if (m_extendedPacked)
        std::fill_n(m_extendedPacked.get(), kExtendedSurfaceBytes, 0);
    ++m_extendedSourceRevision;
    m_extendedPackedRevision = 0;
    m_extendedPackedDescriptorRevision = 0;
    ++m_descriptorRevision;
    m_extendedPackedFormat = -1;
    ++m_paletteRevision;
}

bool TMS9918B::HandleExtendedControlCommand(uint64_t, u8 command,
                                             u8 firstByte)
{
    m_paletteWriteMode = false;
    if ((command & 0xC0) != 0xC0 || !ExtendedEnable())
        return false;

    m_paletteAddress = firstByte & 0x0F;
    m_paletteWriteMode = true;
    return true;
}

bool TMS9918B::HandleExtendedRegisterWrite(u8 rawRegister, u8 value)
{
    if (!m_extendedUnlocked)
    {
        if (rawRegister == 59 && value == 0x5A)
        {
            if (++m_unlockProgress == 2)
            {
                m_extendedUnlocked = true;
                m_unlockProgress = 0;
            }
        }
        else
            m_unlockProgress = 0;

        // Locked register numbers alias R0-R7 exactly as on a TMS9918A,
        // including both writes of the unlock command landing in R3.
        return false;
    }

    const u8 reg = rawRegister & 0x0F;
    if (reg < 8)
        return false;

    static constexpr u8 masks[8] = {
        0xFF, 0xFF, 0xFF, 0x83, 0x0F, 0x00, 0x00, 0x0F
    };
    m_extendedRegisters[reg - 8] = value & masks[reg - 8];
    if (reg == 11)
        RefreshInterruptLine();
    if (reg == 10)
        m_lineCounter = m_extendedRegisters[2];
    if (reg == 11 && !ExtendedEnable())
        m_paletteWriteMode = false;
    if (reg == 11 || reg == 12)
        ++m_descriptorRevision;
    return true;
}

void TMS9918B::OnLegacyRegisterWritten(u8 reg)
{
    if (reg < 2)
        ++m_descriptorRevision;
}

bool TMS9918B::HandleExtendedDataWrite(u8 data)
{
    if (!m_paletteWriteMode || !ExtendedEnable())
        return false;

    const unsigned entry = m_paletteAddress & 0x0F;
    m_paletteEntries[entry] = data & 0x3F;
    RebuildPaletteEntry(entry);
    m_paletteAddress = (m_paletteAddress + 1) & 0x0F;
    ++m_paletteRevision;
    return true;
}

bool TMS9918B::HandleExtendedStatusRead(u8& value)
{
    if (!ExtendedEnable() || (m_extendedRegisters[7] & 0x0F) != 1)
        return false;

    value = m_status1;
    m_status1 &= 0xFE; // S1 reads acknowledge FL only.
    RefreshInterruptLine();
    return true;
}

void TMS9918B::OnRasterLineCompleted(int line)
{
    if (line >= 0 && line < GC_RESOLUTION_HEIGHT && IsExtendedDisplayMode())
    {
        RenderExtendedLine(line);
        if (line == GC_RESOLUTION_HEIGHT - 1)
        {
            m_lastScrollFrameHorizontal = m_scrollFrameHorizontal;
            m_lastScrollFrameVertical = m_scrollFrameVertical;
            m_lastScrollFrameControl = m_scrollFrameControl;
            m_lastScrollFrameValid = true;
        }
    }
}

int TMS9918B::NextDerivedRasterEventPhase(int line, int phase) const
{
    // Revision B samples both axes at the first background access. Text40's
    // active calendar begins later than the other documented extended modes.
    const u8* r = GetRegisters();
    const auto mode = TMS9918BModel::DecodeMode(
        r[0], r[1], m_extendedRegisters[3], m_extendedRegisters[4]);
    const int scrollLatchPhase =
        TMS9918BModel::Info(mode).calendar ==
                TMS9918BModel::CalendarId::Text40
            ? TMS9918BModel::TextWindowStart / 2
            : TMS9918BModel::GraphicsWindowStart / 2;
    constexpr int LineEndPhase = GC_VDP_MASTER_CLOCKS_PER_LINE;
    if (!ExtendedEnable() || line < 0 || line >= GC_RESOLUTION_HEIGHT)
        return GC_VDP_MASTER_CLOCKS_PER_LINE + 1;
    if (phase < scrollLatchPhase)
        return scrollLatchPhase;
    return LineEndPhase;
}

void TMS9918B::OnDerivedRasterEvent(int line, int phase)
{
    const u8* r = GetRegisters();
    const auto mode = TMS9918BModel::DecodeMode(
        r[0], r[1], m_extendedRegisters[3], m_extendedRegisters[4]);
    const int scrollLatchPhase =
        TMS9918BModel::Info(mode).calendar ==
                TMS9918BModel::CalendarId::Text40
            ? TMS9918BModel::TextWindowStart / 2
            : TMS9918BModel::GraphicsWindowStart / 2;
    constexpr int LineEndPhase = GC_VDP_MASTER_CLOCKS_PER_LINE;

    if (!ExtendedEnable() || line < 0 || line >= GC_RESOLUTION_HEIGHT)
        return;

    if (phase == scrollLatchPhase)
    {
        // VRAM viewers use one deterministic palette snapshot: the values at
        // the first displayed pixel of the frame. Mid-frame palette writes
        // belong to raster output and must not recolour the complete memory
        // map according to the instant at which ImGui happens to inspect it.
        if (line == 0)
        {
            if (!m_debugFramePaletteValid ||
                m_debugFramePaletteRgb != m_paletteRgb)
            {
                m_debugFramePaletteRgb = m_paletteRgb;
                ++m_debugFramePaletteRevision;
            }
            m_debugFramePaletteValid = true;
        }
        m_latchedHorizontalScroll = m_extendedRegisters[0];
        m_latchedVerticalScroll = m_extendedRegisters[1];
        m_scrollFrameHorizontal[static_cast<std::size_t>(line)] =
            m_latchedHorizontalScroll;
        m_scrollFrameVertical[static_cast<std::size_t>(line)] =
            m_latchedVerticalScroll;
        m_scrollFrameControl[static_cast<std::size_t>(line)] =
            m_extendedRegisters[4];
        if (line == 0)
            m_lineCounter = m_extendedRegisters[2];
        return;
    }

    if (phase != LineEndPhase)
        return;

    if (m_lineCounter == 0)
    {
        m_status1 |= 0x01;
        m_lineCounter = m_extendedRegisters[2];
        RefreshInterruptLine();
    }
    else
        --m_lineCounter;
}

bool TMS9918B::IsExtendedDisplayMode() const
{
    const u8* r = GetRegisters();
    const auto mode = TMS9918BModel::DecodeMode(
        r[0], r[1], m_extendedRegisters[3], m_extendedRegisters[4]);
    const auto info = TMS9918BModel::Info(mode);
    return info.extended && info.documented;
}

void TMS9918B::BuildCpuPhaseMaps()
{
    using namespace TMS9918BModel;
    constexpr CalendarId ids[] = {CalendarId::Tiles, CalendarId::Bitmap,
                                  CalendarId::Text40, CalendarId::Text64};
    for (std::size_t mapIndex = 0; mapIndex < m_cpuPhaseMaps.size(); ++mapIndex)
    {
        auto& map = m_cpuPhaseMaps[mapIndex];
        map.fill(false);
        for (const Cycle& cycle : GetCalendar(ids[mapIndex]).cycles)
        {
            if (cycle.kind != Kind::Cpu)
                continue;
            // The core advances at one full crystal period. A RAS edge on the
            // intervening half-period becomes visible at the following core
            // phase, never before the physical cycle starts.
            const unsigned phase = (cycle.Unit() + 1u) / 2u;
            map[phase % TMS99xxVramSequencer::PhasesPerLine] = true;
        }
    }
}

const std::array<bool, TMS99xxVramSequencer::PhasesPerLine>*
TMS9918B::GetCpuVRAMPhaseMap() const
{
    using namespace TMS9918BModel;
    if (!IsExtendedDisplayMode() || (GetRegisters()[1] & R1_BL) == 0 ||
        GetRenderLine() < 0 || GetRenderLine() >= kExtendedActiveHeight)
        return nullptr;

    const VideoMode mode = DecodeMode(GetRegisters()[0], GetRegisters()[1],
                                      m_extendedRegisters[3],
                                      m_extendedRegisters[4]);
    switch (Info(mode).calendar)
    {
        case CalendarId::Tiles:  return &m_cpuPhaseMaps[0];
        case CalendarId::Bitmap: return &m_cpuPhaseMaps[1];
        case CalendarId::Text40: return &m_cpuPhaseMaps[2];
        case CalendarId::Text64: return &m_cpuPhaseMaps[3];
        default: return nullptr;
    }
}

void TMS9918B::PutExtendedPixel(int x, int y, unsigned colour,
                                bool programmable)
{
    if (!m_extendedRgb || x < 0 || x >= kExtendedActiveMaxWidth ||
        y < 0 || y >= kExtendedActiveHeight)
        return;
    const u8* palette = programmable ? m_paletteRgb.data()
                                     : GetCurrentPalette();
    const u8* rgb = palette + (colour & 0x0F) * 3;
    u8* pixel = m_extendedRgb.get() +
        (static_cast<std::size_t>(y) * kExtendedActiveMaxWidth + x) * 3;
    pixel[0] = rgb[0];
    pixel[1] = rgb[1];
    pixel[2] = rgb[2];
}

void TMS9918B::RenderExtendedLine(int line)
{
    using namespace TMS9918BModel;
    const u8* r = GetRegisters();
    const u8* vram = GetVRAM();
    const VideoMode mode = DecodeMode(r[0], r[1], m_extendedRegisters[3],
                                      m_extendedRegisters[4]);
    const ModeInfo info = Info(mode);
    const int width = info.pixelsPerLine;
    unsigned backdrop = r[7] & 0x0F;
    if (backdrop == 0)
        backdrop = 1;

    for (int x = 0; x < width; ++x)
        PutExtendedPixel(x, line, backdrop, false);
    if ((r[1] & 0x40) == 0)
    {
        ++m_extendedSourceRevision;
        return;
    }

    bool backgroundPriority[kExtendedActiveMaxWidth] = {};
    for (int x = 0; x < width; ++x)
    {
        const bool text64 = mode == VideoMode::Text64 ||
                            mode == VideoMode::Text64Q;
        const int screenX = text64 ? (x >> 1) : x;
        int sourceX = screenX;
        int sourceY = line;
        if (info.hScroll && !(line < 16 && (m_extendedRegisters[4] & 0x08)))
            sourceX = (sourceX + m_latchedHorizontalScroll) & 0xFF;
        if (info.vScroll && !(screenX >= 192 &&
                              (m_extendedRegisters[4] & 0x04)))
            sourceY = (sourceY + m_latchedVerticalScroll) % 192;
        if (info.hScroll && (m_extendedRegisters[4] & 0x02) && screenX < 8)
            continue;

        unsigned colour = backdrop;
        bool programmable = false;
        if (mode == VideoMode::Graphics1X || mode == VideoMode::Graphics2Fat)
        {
            const unsigned column = static_cast<unsigned>(sourceX) >> 3;
            const unsigned row = static_cast<unsigned>(sourceY) >> 3;
            const u16 na = NameAttrAddress(r[2], row, column);
            const u8 name = vram[na];
            const u8 attr = vram[(na + 1) & VramMask];
            const u16 pa = mode == VideoMode::Graphics1X
                ? Graphics1XPatternAddress(r[4],
                    static_cast<u16>(name | ((attr & 0x08) << 5)), sourceY)
                : Graphics2FatPatternAddress(r[4], sourceY, name);
            const u8 low = vram[pa];
            const u8 high = vram[(pa + 1) & VramMask];
            if (mode == VideoMode::Graphics1X)
            {
                const int bit = 7 - (sourceX & 7);
                const unsigned value = ((low >> bit) & 1u) |
                                       (((high >> bit) & 1u) << 1);
                if (value != 0)
                {
                    colour = (attr & 3u) * 4u + value;
                    programmable = true;
                    backgroundPriority[x] = (attr & 0x04) != 0;
                }
            }
            else
            {
                const int dot = (sourceX & 7) >> 1;
                const int shift = 6 - dot * 2;
                const unsigned value = ((low >> shift) & 3u) |
                                       (((high >> shift) & 3u) << 2);
                if (value != 0)
                {
                    colour = value;
                    backgroundPriority[x] = (attr & 0x04) != 0;
                }
            }
        }
        else if (mode == VideoMode::Bitmap || mode == VideoMode::BitmapQ)
        {
            const unsigned cell = static_cast<unsigned>(sourceX) >> 3;
            const unsigned pair = cell >> 1;
            const u16 ba = mode == VideoMode::Bitmap
                ? BitmapQuadAddress(r[4], sourceY, pair)
                : BitmapQQuadAddress(r[4], sourceY, pair);
            const unsigned offset = (cell & 1u) * 2u;
            const u8 low = vram[(ba + offset) & VramMask];
            const u8 high = vram[(ba + offset + 1) & VramMask];
            const int bit = 7 - (sourceX & 7);
            const unsigned value = ((low >> bit) & 1u) |
                                   (((high >> bit) & 1u) << 1);
            if (value != 0)
            {
                const u16 ma = PaletteMapPairAddress(r[2], sourceY >> 3,
                                                     pair);
                const u8 map = vram[(ma + (cell & 1u)) & VramMask];
                colour = ((map >> 6) & 3u) * 4u + value;
                programmable = true;
            }
        }
        else if (mode == VideoMode::Text40X || mode == VideoMode::Text40XQ)
        {
            if (x < 8 || x >= 248)
                continue;
            const unsigned textX = static_cast<unsigned>(x - 8);
            const unsigned column = textX / 6;
            const unsigned pair = column >> 1;
            const u16 ca = static_cast<u16>(TextCharColourAddress(
                r[2], sourceY >> 3, pair) + (column & 1u) * 2u);
            const u8 name = vram[ca & VramMask];
            const u8 colours = vram[(ca + 1) & VramMask];
            const u16 pa = mode == VideoMode::Text40XQ
                ? TMS99xxTableAddress::Pattern(3, r[3], r[4], sourceY, name)
                : TextPatternAddress(r[4], name, sourceY);
            const bool ink = (vram[pa] & (0x80 >> (textX % 6))) != 0;
            colour = ink ? (colours >> 4) : (colours & 0x0F);
            if (colour == 0)
                colour = ink ? (r[7] >> 4) : (r[7] & 0x0F);
            if (colour == 0)
                colour = backdrop;
        }
        else if (text64)
        {
            const unsigned column = static_cast<unsigned>(x) >> 3;
            const u16 na = static_cast<u16>(Text64NameQuadAddress(
                r[2], sourceY >> 3, column >> 2) + (column & 3u));
            const u8 name = vram[na & VramMask];
            const u16 pa = mode == VideoMode::Text64Q
                ? TMS99xxTableAddress::Pattern(3, r[3], r[4], sourceY, name)
                : TextPatternAddress(r[4], name, sourceY);
            colour = (vram[pa] & (0x80 >> (x & 7)))
                ? (r[7] >> 4) : (r[7] & 0x0F);
            if (colour == 0)
                colour = backdrop;
        }
        PutExtendedPixel(x, line, colour, programmable);
    }
    RenderExtendedSprites(line, static_cast<unsigned>(mode),
                          backgroundPriority);
    ++m_extendedSourceRevision;
}

void TMS9918B::RenderExtendedSprites(int line, unsigned rawMode,
                                     bool* backgroundPriority)
{
    using namespace TMS9918BModel;
    const VideoMode mode = static_cast<VideoMode>(rawMode);
    const ModeInfo info = Info(mode);
    if (info.spritesPerLine == 0)
        return;

    const u8* r = GetRegisters();
    const u8* vram = GetVRAM();
    const u16 sat = static_cast<u16>((r[5] & 0x7F) << 7);
    const bool large = (r[1] & 0x02) != 0;
    const bool zoom = (r[1] & 0x01) != 0;
    const int sourceSize = large ? 16 : 8;
    const int displaySize = sourceSize * (zoom ? 2 : 1);
    bool selected[32] = {};
    unsigned selectedCount = 0;
    const int firstPairLine = line & ~1;

    const unsigned scanCount = info.spritesPerLine == 2 ? 2 : 32;
    for (unsigned sprite = 0; sprite < scanCount; ++sprite)
    {
        const u16 sa = static_cast<u16>((sat + sprite * 4) & VramMask);
        if (vram[sa] == 0xD0)
            break;
        int y = (vram[sa] + 1) & 0xFF;
        if (y >= 0xE0)
            y -= 0x100;
        const bool visible = info.linePairSpriteScan
            ? (y < firstPairLine + 2 && y + displaySize > firstPairLine)
            : (y <= line && y + displaySize > line);
        if (!visible)
            continue;
        if (selectedCount == info.spritesPerLine)
        {
            RaiseExtendedSpriteOverflow(static_cast<u8>(sprite));
            break;
        }
        selected[sprite] = true;
        ++selectedCount;
    }

    bool occupied[kExtendedActiveMaxWidth] = {};
    const bool text64 = mode == VideoMode::Text64 ||
                        mode == VideoMode::Text64Q;
    const int width = info.pixelsPerLine;

    const auto patternBit = [&](unsigned sprite, int x) -> bool
    {
        const u16 sa = static_cast<u16>((sat + sprite * 4) & VramMask);
        int y = (vram[sa] + 1) & 0xFF;
        if (y >= 0xE0)
            y -= 0x100;
        if (line < y || line >= y + displaySize)
            return false;
        const int sourceY = (line - y) >> (zoom ? 1 : 0);
        // Text64 shifts 512 real sprite pixels as well as 512 background
        // pixels. Only the coordinate is coarse; the pattern is not widened.
        const int sourceX = x / (zoom ? 2 : 1);
        if (sourceX < 0 || sourceX >= sourceSize)
            return false;
        u8 name = vram[(sa + 2) & VramMask];
        if (large)
            name &= 0xFC;
        const u8 attribute = vram[(sa + 3) & VramMask];
        const u16 pa = SpritePatternAddress(r[6], (attribute & 0x20) != 0,
            name, sourceY, sourceX >= 8);
        return (vram[pa] & (0x80 >> (sourceX & 7))) != 0;
    };

    for (unsigned sprite = 0; sprite < scanCount; ++sprite)
    {
        if (!selected[sprite])
            continue;
        const u16 sa = static_cast<u16>((sat + sprite * 4) & VramMask);
        const u8 attribute = vram[(sa + 3) & VramMask];
        const bool paired = (sprite & 1u) == 0 && sprite + 1 < scanCount &&
            selected[sprite + 1] &&
            (vram[(sa + 7) & VramMask] & 0x10) != 0;
        if ((sprite & 1u) != 0 && (attribute & 0x10) != 0 &&
            selected[sprite - 1])
            continue;

        const auto screenX = [&](u16 entry, u8 attr) -> int
        {
            int coarse = vram[(entry + 1) & VramMask];
            if (attr & 0x80)
                coarse -= 32;
            return text64
                ? coarse * 2 + ((attr & 0x40) ? 1 : 0)
                : coarse;
        };
        int startX = screenX(sa, attribute);
        int secondStartX = startX;
        if (paired)
        {
            const u16 second = static_cast<u16>((sa + 4) & VramMask);
            const u8 secondAttribute = vram[(second + 3) & VramMask];
            secondStartX = screenX(second, secondAttribute);
        }

        const int extent = displaySize;
        const int left = paired ? std::min(startX, secondStartX) : startX;
        const int right = paired
            ? std::max(startX + extent, secondStartX + extent)
            : startX + extent;
        for (int x = std::max(0, left); x < std::min(width, right); ++x)
        {
            const bool plane0 = patternBit(sprite, x - startX);
            const bool plane1 = paired && patternBit(sprite + 1,
                                                     x - secondStartX);
            const unsigned value = (plane0 ? 1u : 0u) |
                                   (plane1 ? 2u : 0u);
            if (value == 0)
                continue;
            if (occupied[x])
                RaiseExtendedSpriteCollision();
            else
                occupied[x] = true;
            if (backgroundPriority[x])
                continue;
            const unsigned entry = paired ? value : (attribute & 0x03);
            if (entry == 0)
                continue;
            const unsigned colour = ((attribute >> 2) & 3u) * 4u + entry;
            PutExtendedPixel(x, line, colour, true);
        }
        if (paired)
            ++sprite;
    }
}

bool TMS9918B::ExtendedInterruptPending() const
{
    return ExtendedEnable() && (m_extendedRegisters[3] & 0x80) != 0 &&
           (m_status1 & 0x01) != 0;
}

void TMS9918B::RebuildPaletteEntry(unsigned index)
{
    const u8 encoded = m_paletteEntries[index];
    const unsigned colour = encoded & 0x0F;
    const unsigned luminance = (encoded >> 4) & 0x03;
    const unsigned numerator = 4 - luminance;
    const u8* source = GetCurrentPalette() + colour * 3;
    for (unsigned channel = 0; channel < 3; ++channel)
        m_paletteRgb[index * 3 + channel] = static_cast<u8>(
            (static_cast<unsigned>(source[channel]) * numerator + 2) / 4);
}

bool TMS9918B::GetPaletteView(GC_VideoPaletteView& view) const
{
    if (!ExtendedEnable())
        return TMS99xx::GetPaletteView(view);
    view.rgb888 = m_paletteRgb.data();
    view.color_count = m_paletteEntries.size();
    view.revision = m_paletteRevision;
    view.native = m_paletteEntries.data();
    view.native_kind = GC_VideoPaletteView::NativeTms9918B;
    return view.IsValid();
}

GC_VideoFrameDescriptor TMS9918B::GetFrameDescriptor() const
{
    GC_VideoFrameDescriptor descriptor = TMS99xx::GetFrameDescriptor();
    if (!IsExtendedDisplayMode() ||
        descriptor.surface_kind == GC_VIDEO_SURFACE_FULL_RASTER_DEBUG)
        return descriptor;

    const u8* r = GetRegisters();
    const auto mode = TMS9918BModel::DecodeMode(
        r[0], r[1], m_extendedRegisters[3], m_extendedRegisters[4]);
    const auto info = TMS9918BModel::Info(mode);
    const int horizontalScale = info.pixelsPerLine == 512 ? 2 : 1;
    const int borderLeft = descriptor.content_area.x;
    const int borderTop = descriptor.content_area.y;
    const int borderRight = descriptor.frame_width -
        descriptor.content_area.x - descriptor.content_area.width;
    const int borderBottom = descriptor.frame_height -
        descriptor.content_area.y - descriptor.content_area.height;
    descriptor.mode_id = static_cast<u32>(mode);
    descriptor.buffer_width = kExtendedSurfaceMaxWidth;
    descriptor.buffer_height = kExtendedSurfaceMaxHeight;
    descriptor.frame_width = (borderLeft + borderRight) * horizontalScale +
        info.pixelsPerLine;
    descriptor.frame_height = borderTop + kExtendedActiveHeight + borderBottom;
    descriptor.stride_pixels = kExtendedSurfaceMaxWidth;
    descriptor.content_area = {borderLeft * horizontalScale, borderTop,
                               info.pixelsPerLine, kExtendedActiveHeight};
    descriptor.visible_area = {0, 0, descriptor.frame_width,
                               descriptor.frame_height};
    // The high-resolution text shifter exposes two samples for every normal
    // TMS dot. The line duration and refresh rate remain unchanged.
    descriptor.raster_width = info.pixelsPerLine == 512 ? 684 : 342;
    descriptor.logical_width = info.pixelsPerLine;
    descriptor.logical_height = kExtendedActiveHeight;
    descriptor.pixel_aspect_ratio = info.pixelsPerLine == 512
        ? GC_VideoRational{1, 2} : GC_VideoRational{1, 1};
    descriptor.presentation_width =
        (descriptor.visible_area.width *
         descriptor.pixel_aspect_ratio.numerator +
         descriptor.pixel_aspect_ratio.denominator / 2) /
        descriptor.pixel_aspect_ratio.denominator;
    descriptor.presentation_height = descriptor.visible_area.height;
    descriptor.revision += m_descriptorRevision;
    return descriptor;
}

GC_VideoFrameView TMS9918B::GetFrameView(GC_Color_Format pixelFormat)
{
    const GC_VideoFrameDescriptor descriptor = GetFrameDescriptor();
    if (!IsExtendedDisplayMode() ||
        descriptor.surface_kind == GC_VIDEO_SURFACE_FULL_RASTER_DEBUG)
        return TMS99xx::GetFrameView(pixelFormat);

    GC_VideoFrameView view;
    view.descriptor = descriptor;
    view.pixel_format = pixelFormat;
    view.frame_serial = GetFrameSerial();
    if (m_extendedPackedRevision != m_extendedSourceRevision ||
        m_extendedPackedDescriptorRevision != descriptor.revision ||
        m_extendedPackedFormat != static_cast<int>(pixelFormat))
    {
        const bool packed24 = pixelFormat == GC_PIXEL_RGB888 ||
                              pixelFormat == GC_PIXEL_BGR888;
        const bool bgr = pixelFormat == GC_PIXEL_BGR888 ||
                         pixelFormat == GC_PIXEL_BGR555 ||
                         pixelFormat == GC_PIXEL_BGR565;
        const bool green6 = pixelFormat == GC_PIXEL_RGB565 ||
                            pixelFormat == GC_PIXEL_BGR565;
        const u8* fixedPalette = GetCurrentPalette();
        for (int y = 0; y < descriptor.frame_height; ++y)
        {
            const bool contentY = y >= descriptor.content_area.y &&
                y < descriptor.content_area.y + descriptor.content_area.height;
            const int leading = GetBorderColorForOutputRow(
                descriptor, y, true) & 0x0F;
            const int trailing = GetBorderColorForOutputRow(
                descriptor, y, false) & 0x0F;
            for (int x = 0; x < descriptor.frame_width; ++x)
            {
                const bool content = contentY &&
                    x >= descriptor.content_area.x &&
                    x < descriptor.content_area.x + descriptor.content_area.width;
                const u8* source = nullptr;
                if (content)
                {
                    const int sourceX = x - descriptor.content_area.x;
                    const int sourceY = y - descriptor.content_area.y;
                    source = m_extendedRgb.get() +
                        (static_cast<std::size_t>(sourceY) *
                         kExtendedActiveMaxWidth + sourceX) * 3;
                }
                else
                {
                    const int colour = x < descriptor.content_area.x
                        ? leading : trailing;
                    source = fixedPalette + colour * 3;
                }

                const u8 red = source[0];
                const u8 green = source[1];
                const u8 blue = source[2];
                if (packed24)
                {
                    u8* destination = m_extendedPacked.get() +
                        (static_cast<std::size_t>(y) *
                         kExtendedSurfaceMaxWidth + x) * 3;
                    destination[0] = bgr ? blue : red;
                    destination[1] = green;
                    destination[2] = bgr ? red : blue;
                }
                else
                {
                    const unsigned r5 = red * 31u / 255u;
                    const unsigned g = green * (green6 ? 63u : 31u) / 255u;
                    const unsigned b5 = blue * 31u / 255u;
                    u16* destination = reinterpret_cast<u16*>(
                        m_extendedPacked.get()) +
                        static_cast<std::size_t>(y) *
                        kExtendedSurfaceMaxWidth + x;
                    *destination = static_cast<u16>(bgr
                        ? ((b5 << (green6 ? 11 : 10)) | (g << 5) | r5)
                        : ((r5 << (green6 ? 11 : 10)) | (g << 5) | b5));
                }
            }
        }
        m_extendedPackedRevision = m_extendedSourceRevision;
        m_extendedPackedDescriptorRevision = descriptor.revision;
        m_extendedPackedFormat = static_cast<int>(pixelFormat);
    }
    view.pixels = m_extendedPacked.get();
    view.row_stride_bytes = kExtendedSurfaceMaxWidth *
        ((pixelFormat == GC_PIXEL_RGB888 || pixelFormat == GC_PIXEL_BGR888)
             ? 3 : 2);
    return view;
}

bool TMS9918B::DescribeRegisterBank(std::size_t index,
    GC_VideoRegisterBankDescriptor& descriptor) const
{
    if (!TMS99xx::DescribeRegisterBank(index, descriptor))
        return false;
    descriptor.register_count = index == 0 ? 16 : 2;
    descriptor.description = index == 0
        ? "TMS9918B control registers" : "TMS9918B status registers";
    descriptor.schema_revision = 2;
    return true;
}

std::size_t TMS9918B::GetRegisterCount(const char* bankCode) const
{
    if (SameCode(bankCode, kControlBank))
        return 16;
    if (SameCode(bankCode, kStatusBank))
        return 2;
    return 0;
}

bool TMS9918B::DescribeRegister(const char* bankCode, std::size_t index,
    GC_VideoRegisterDescriptor& descriptor) const
{
    if (SameCode(bankCode, kControlBank) && index < 8)
        return TMS99xx::DescribeRegister(bankCode, index, descriptor);
    if (SameCode(bankCode, kControlBank) && index < 16)
    {
        const auto& definition = kExtendedRegisterDefinitions[index - 8];
        descriptor = {};
        descriptor.code = definition.code;
        descriptor.short_name = definition.name;
        descriptor.description = definition.description;
        descriptor.bank_code = kControlBank;
        descriptor.register_index = static_cast<u32>(index);
        descriptor.width_bits = 8;
        descriptor.implemented_mask = definition.mask;
        descriptor.access_flags = GC_VIDEO_REGISTER_ACCESS_WRITE |
            GC_VIDEO_REGISTER_ACCESS_OBSERVE |
            GC_VIDEO_REGISTER_ACCESS_WRITE_SIDE_EFFECT;
        return true;
    }
    if (SameCode(bankCode, kStatusBank) && index == 0)
        return TMS99xx::DescribeRegister(bankCode, index, descriptor);
    if (SameCode(bankCode, kStatusBank) && index == 1)
    {
        descriptor = {};
        descriptor.code = "TMS9918B.STATUS.S1";
        descriptor.short_name = "S1";
        descriptor.description = "Identification and scanline interrupt status";
        descriptor.bank_code = kStatusBank;
        descriptor.register_index = 1;
        descriptor.width_bits = 8;
        descriptor.implemented_mask = 0x19;
        descriptor.access_flags = GC_VIDEO_REGISTER_ACCESS_READ |
            GC_VIDEO_REGISTER_ACCESS_OBSERVE |
            GC_VIDEO_REGISTER_ACCESS_READ_SIDE_EFFECT;
        return true;
    }
    return false;
}

std::size_t TMS9918B::GetRegisterFieldCount(const char* bankCode,
                                             u32 registerIndex) const
{
    if ((SameCode(bankCode, kControlBank) && registerIndex < 8) ||
        (SameCode(bankCode, kStatusBank) && registerIndex == 0))
        return TMS99xx::GetRegisterFieldCount(bankCode, registerIndex);
    return 0;
}

bool TMS9918B::DescribeRegisterField(const char* bankCode, u32 registerIndex,
    std::size_t fieldIndex, GC_VideoRegisterFieldDescriptor& descriptor) const
{
    if ((SameCode(bankCode, kControlBank) && registerIndex < 8) ||
        (SameCode(bankCode, kStatusBank) && registerIndex == 0))
        return TMS99xx::DescribeRegisterField(bankCode, registerIndex,
                                              fieldIndex, descriptor);
    return false;
}

bool TMS9918B::ReadRegisterSnapshot(const char* bankCode, u32 registerIndex,
                                    u32& value) const
{
    if (SameCode(bankCode, kControlBank))
    {
        if (registerIndex < 8)
            return TMS99xx::ReadRegisterSnapshot(bankCode, registerIndex,
                                                 value);
        if (registerIndex < 16)
        {
            value = m_extendedRegisters[registerIndex - 8];
            return true;
        }
    }
    if (SameCode(bankCode, kStatusBank))
    {
        if (registerIndex == 0)
            return TMS99xx::ReadRegisterSnapshot(bankCode, registerIndex,
                                                 value);
        if (registerIndex == 1)
        {
            value = m_status1;
            return true;
        }
    }
    return false;
}

u64 TMS9918B::DebugSurfaceSignature() const
{
    // Registers are few enough to fold directly. This catches mode and table
    // remaps without forcing a VRAM scan. Programmable colours use the
    // stable first-pixel snapshot rather than live mid-frame palette writes.
    u64 signature = 1469598103934665603ull;
    for (unsigned index = 0; index < 8; ++index)
        signature = (signature ^ GetRegisters()[index]) * 1099511628211ull;
    for (u8 value : m_extendedRegisters)
        signature = (signature ^ value) * 1099511628211ull;
    signature ^= m_debugFramePaletteRevision * 0x9E3779B97F4A7C15ull;
    return signature ^ (GetFixedPaletteRevision() * 0xD6E8FEB86659FD93ull);
}

const u8* TMS9918B::GetDebugProgrammablePalette() const
{
    return m_debugFramePaletteValid ? m_debugFramePaletteRgb.data()
                                    : m_paletteRgb.data();
}

bool TMS9918B::DescribeDebugScene(GC_VideoDebugSceneDescriptor& descriptor) const
{
    if (!IsExtendedDisplayMode())
        return TMS99xx::DescribeDebugScene(descriptor);
    const auto info = TMS9918BModel::Info(TMS9918BModel::DecodeMode(
        GetRegisters()[0], GetRegisters()[1], m_extendedRegisters[3],
        m_extendedRegisters[4]));
    descriptor = {};
    descriptor.canvas_width = info.pixelsPerLine;
    descriptor.canvas_height = kExtendedActiveHeight;
    descriptor.pixel_aspect_x = info.pixelsPerLine == 512 ? 0.5f : 1.0f;
    return true;
}

std::size_t TMS9918B::GetDebugLayerCount() const
{
    return TMS99xx::GetDebugLayerCount();
}

bool TMS9918B::DescribeDebugLayer(std::size_t index,
    GC_VideoDebugLayerDescriptor& descriptor) const
{
    using namespace TMS9918BModel;
    if (!IsExtendedDisplayMode())
        return TMS99xx::DescribeDebugLayer(index, descriptor);
    if (index > 2)
        return false;

    const u8* r = GetRegisters();
    const VideoMode mode = DecodeMode(r[0], r[1], m_extendedRegisters[3],
                                      m_extendedRegisters[4]);
    const ModeInfo info = Info(mode);
    const bool bitmap = mode == VideoMode::Bitmap || mode == VideoMode::BitmapQ;
    const bool text40 = mode == VideoMode::Text40X || mode == VideoMode::Text40XQ;
    const bool text64 = mode == VideoMode::Text64 || mode == VideoMode::Text64Q;
    descriptor = {};
    descriptor.blend_mode = GC_VideoDebugBlendMode::Opaque;
    descriptor.visible = true;
    descriptor.has_grid = true;
    descriptor.address_width_bits = 14;

    if (index == 0)
    {
        descriptor.code = "TMS9918B.NAMTAB";
        descriptor.name = bitmap ? "Bitmap / Palette Map" : "Name Table";
        descriptor.kind = GC_VideoDebugLayerKind::TileGrid;
        descriptor.content_width = text64 ? 512 : (text40 ? 240 : 256);
        descriptor.content_height = 192;
        descriptor.columns = text64 ? 64 : (text40 ? 40 : 32);
        descriptor.rows = 24;
        descriptor.active_cell_count = descriptor.columns * descriptor.rows;
        descriptor.cell_width = text40 ? 6 : 8;
        descriptor.cell_height = 8;
        descriptor.has_viewport = info.hScroll || info.vScroll;
        descriptor.viewport_x = info.hScroll ? m_extendedRegisters[0] : 0;
        descriptor.viewport_y = info.vScroll ?
            static_cast<u32>(m_extendedRegisters[1] % kExtendedActiveHeight) : 0;
        descriptor.viewport_width = descriptor.content_width;
        descriptor.viewport_height = 192;
        descriptor.primary_address_label = bitmap ? "Palette Map Base" : "Name Table Base";
        descriptor.primary_address = static_cast<u32>(r[2] & 0x0F) << 10;
        return true;
    }
    if (index == 1)
    {
        if (bitmap)
            return false; // Bitmap modes have no pattern-generator catalogue.
        const bool graphics1x = mode == VideoMode::Graphics1X;
        const bool graphics2fat = mode == VideoMode::Graphics2Fat;
        const bool textQ = mode == VideoMode::Text40XQ || mode == VideoMode::Text64Q;
        const u32 patterns = graphics1x ? 512u : ((graphics2fat || textQ) ? 768u : 256u);
        descriptor.code = "TMS9918B.PATGEN";
        descriptor.name = graphics2fat ? "Background Patterns (2-pixel dots)" :
            (graphics1x ? "Background Patterns (2 bpp)" : "Background Patterns");
        descriptor.kind = GC_VideoDebugLayerKind::TileCatalog;
        descriptor.content_width = 256;
        descriptor.content_height = ((patterns + 31) / 32) * 8;
        descriptor.columns = 32;
        descriptor.rows = (patterns + 31) / 32;
        descriptor.active_cell_count = patterns;
        descriptor.cell_width = 8;
        descriptor.cell_height = 8;
        descriptor.rows_per_region = patterns == 768 ? 8 : 0;
        descriptor.primary_address_label = "Pattern Generator Base";
        descriptor.primary_address = static_cast<u32>(r[4] &
            ((graphics2fat || textQ) ? 0x04 : 0x07)) << 11;
        return true;
    }

    descriptor.code = "TMS9918B.SPRGEN";
    descriptor.name = "Sprite Patterns (BANK 0 / 1)";
    descriptor.kind = GC_VideoDebugLayerKind::TileCatalog;
    descriptor.content_width = 256;
    descriptor.content_height = 128;
    descriptor.columns = 32;
    descriptor.rows = 16;
    descriptor.active_cell_count = 512;
    descriptor.cell_width = 8;
    descriptor.cell_height = 8;
    descriptor.rows_per_region = 8;
    descriptor.primary_address_label = "Sprite Generator Base";
    descriptor.primary_address = static_cast<u32>(r[6] & 7) << 11;
    return true;
}

std::size_t TMS9918B::GetDebugViewportRegionCount(std::size_t index) const
{
    if (!IsExtendedDisplayMode())
        return TMS99xx::GetDebugViewportRegionCount(index);
    if (index != 0)
        return 0;
    BuildDebugViewportRegions();
    return m_debugViewportRegionCount;
}

bool TMS9918B::DescribeDebugViewportRegion(std::size_t index,
    std::size_t regionIndex,
    GC_VideoDebugViewportRegionDescriptor& descriptor) const
{
    using namespace TMS9918BModel;
    if (!IsExtendedDisplayMode())
        return TMS99xx::DescribeDebugViewportRegion(index, regionIndex,
                                                     descriptor);
    if (index != 0)
        return false;

    BuildDebugViewportRegions();
    if (regionIndex >= m_debugViewportRegionCount)
        return false;
    descriptor = m_debugViewportRegions[regionIndex];
    return true;
}

void TMS9918B::BuildDebugViewportRegions() const
{
    using namespace TMS9918BModel;
    m_debugViewportRegionCount = 0;

    const u8* r = GetRegisters();
    const ModeInfo info = Info(DecodeMode(r[0], r[1],
        m_extendedRegisters[3], m_extendedRegisters[4]));
    if (!info.hScroll && !info.vScroll)
        return;

    const bool text64 = info.pixelsPerLine == 512;
    const bool text40 = info.calendar == CalendarId::Text40;
    const u32 mapWidth = text64 ? 512u : (text40 ? 240u : 256u);
    // VLOCK starts at logical X=192. Text64 emits two physical samples for
    // each logical X; Text40's debug map omits its eight-pixel left border.
    const u32 verticalLockX = text64 ? 384u : (text40 ? 184u : 192u);

    struct TileBand
    {
        u32 y;
        u32 height;
        u8 horizontalScroll;
        u8 verticalScroll;
        bool verticalLock;
    };
    std::array<TileBand, 24> bands{};
    std::size_t bandCount = 0;

    // The VDP keeps exact scanline samples. The Name Table viewer deliberately
    // reduces them to its natural eight-line rows, so even software changing
    // scroll every line produces at most 24 visual bands.
    for (u32 y = 0; y < 192; y += 8)
    {
        const std::size_t sample = static_cast<std::size_t>(y);
        const u8 horizontal = m_lastScrollFrameValid
            ? m_lastScrollFrameHorizontal[sample] : m_extendedRegisters[0];
        const u8 vertical = m_lastScrollFrameValid
            ? m_lastScrollFrameVertical[sample] : m_extendedRegisters[1];
        const u8 control = m_lastScrollFrameValid
            ? m_lastScrollFrameControl[sample] : m_extendedRegisters[4];
        const bool horizontalLock = info.hScroll && (control & 0x08) != 0;
        const bool verticalLock = info.vScroll && (control & 0x04) != 0;
        const u8 effectiveHorizontal = info.hScroll &&
            !(horizontalLock && y < 16) ? horizontal : 0;
        const u8 effectiveVertical = info.vScroll ? vertical : 0;

        if (bandCount != 0)
        {
            TileBand& previous = bands[bandCount - 1];
            if (previous.horizontalScroll == effectiveHorizontal &&
                previous.verticalScroll == effectiveVertical &&
                previous.verticalLock == verticalLock)
            {
                previous.height += 8;
                continue;
            }
        }
        bands[bandCount++] = {
            y, 8, effectiveHorizontal, effectiveVertical, verticalLock};
    }

    for (std::size_t bandIndex = 0; bandIndex < bandCount; ++bandIndex)
    {
        const TileBand& band = bands[bandIndex];
        const std::size_t columns = band.verticalLock ? 2u : 1u;
        for (std::size_t column = 0; column < columns; ++column)
        {
            const u32 destinationX = column == 0 ? 0u : verticalLockX;
            GC_VideoDebugViewportRegionDescriptor& region =
                m_debugViewportRegions[m_debugViewportRegionCount++];
            region = {};
            region.source_x = (destinationX + band.horizontalScroll) % mapWidth;
            region.source_y = (band.y +
                ((band.verticalLock && column != 0)
                    ? 0u : band.verticalScroll)) % 192u;
            region.width = band.verticalLock
                ? (column == 0 ? verticalLockX : mapWidth - verticalLockX)
                : mapWidth;
            region.height = band.height;
        }
    }
}

namespace
{
void PutDebugRgba(u8* surface, unsigned pitchPixels, unsigned x, unsigned y,
                  const u8* palette, unsigned colour)
{
    u8* pixel = surface + (static_cast<std::size_t>(y) * pitchPixels + x) * 4;
    const u8* rgb = palette + (colour & 15u) * 3;
    pixel[0] = rgb[0]; pixel[1] = rgb[1]; pixel[2] = rgb[2]; pixel[3] = 0xFF;
}

void ClearDebugRgba(u8* surface, std::size_t pixels)
{
    for (std::size_t index = 0; index < pixels; ++index)
    {
        surface[index * 4 + 0] = 4;
        surface[index * 4 + 1] = 4;
        surface[index * 4 + 2] = 4;
        surface[index * 4 + 3] = 0xFF;
    }
}
}

void TMS9918B::BuildExtendedNameDebugSurface() const
{
    using namespace TMS9918BModel;
    const u64 signature = DebugSurfaceSignature();
    const u64 vramRevision = GetDebugVRAMRevision();
    if (m_debugBuiltSignature[0] == signature &&
        m_debugBuiltVramRevision[0] == vramRevision)
        return;
    ClearDebugRgba(m_debugNameRgba.get(), 512u * 192u);
    const u8* r = GetRegisters();
    const u8* vram = GetVRAM();
    const u8* debugPalette = GetDebugProgrammablePalette();
    const VideoMode mode = DecodeMode(r[0], r[1], m_extendedRegisters[3],
                                      m_extendedRegisters[4]);
    unsigned backdrop = r[7] & 15u;
    if (backdrop == 0) backdrop = 1;
    const bool text40 = mode == VideoMode::Text40X || mode == VideoMode::Text40XQ;
    const bool text64 = mode == VideoMode::Text64 || mode == VideoMode::Text64Q;

    for (unsigned y = 0; y < 192; ++y)
    {
        const unsigned columns = text64 ? 64u : (text40 ? 40u : 32u);
        for (unsigned column = 0; column < columns; ++column)
        {
            if (mode == VideoMode::Graphics1X || mode == VideoMode::Graphics2Fat)
            {
                const u16 na = NameAttrAddress(r[2], y >> 3, column);
                const u8 name = vram[na];
                const u8 attr = vram[(na + 1) & VramMask];
                const u16 pa = mode == VideoMode::Graphics1X
                    ? Graphics1XPatternAddress(r[4], name | ((attr & 8u) << 5), y)
                    : Graphics2FatPatternAddress(r[4], y, name);
                const u8 low = vram[pa], high = vram[(pa + 1) & VramMask];
                for (unsigned px = 0; px < 8; ++px)
                {
                    unsigned colour = backdrop;
                    const u8* palette = GetCurrentPalette();
                    if (mode == VideoMode::Graphics1X)
                    {
                        const unsigned bit = 7 - px;
                        const unsigned value = ((low >> bit) & 1u) | (((high >> bit) & 1u) << 1);
                        if (value) { colour = (attr & 3u) * 4u + value; palette = debugPalette; }
                    }
                    else
                    {
                        const unsigned shift = 6 - ((px >> 1) * 2);
                        const unsigned value = ((low >> shift) & 3u) | (((high >> shift) & 3u) << 2);
                        if (value) colour = value;
                    }
                    PutDebugRgba(m_debugNameRgba.get(), 512, column * 8 + px, y,
                                 palette, colour);
                }
            }
            else if (mode == VideoMode::Bitmap || mode == VideoMode::BitmapQ)
            {
                const unsigned pair = column >> 1;
                const u16 ba = mode == VideoMode::Bitmap
                    ? BitmapQuadAddress(r[4], y, pair) : BitmapQQuadAddress(r[4], y, pair);
                const unsigned offset = (column & 1u) * 2u;
                const u8 low = vram[(ba + offset) & VramMask];
                const u8 high = vram[(ba + offset + 1) & VramMask];
                const u16 ma = PaletteMapPairAddress(r[2], y >> 3, pair);
                const u8 map = vram[(ma + (column & 1u)) & VramMask];
                for (unsigned px = 0; px < 8; ++px)
                {
                    const unsigned bit = 7 - px;
                    const unsigned value = ((low >> bit) & 1u) | (((high >> bit) & 1u) << 1);
                    const unsigned colour = value ? ((map >> 6) & 3u) * 4u + value : backdrop;
                    PutDebugRgba(m_debugNameRgba.get(), 512, column * 8 + px, y,
                                 value ? debugPalette : GetCurrentPalette(), colour);
                }
            }
            else if (text40)
            {
                const u16 ca = TextCharColourAddress(r[2], y >> 3, column >> 1) +
                    (column & 1u) * 2u;
                const u8 name = vram[ca & VramMask];
                const u8 colours = vram[(ca + 1) & VramMask];
                const u16 pa = mode == VideoMode::Text40XQ
                    ? TMS99xxTableAddress::Pattern(3, r[3], r[4], y, name)
                    : TextPatternAddress(r[4], name, y);
                for (unsigned px = 0; px < 6; ++px)
                {
                    const bool ink = (vram[pa] & (0x80u >> px)) != 0;
                    unsigned colour = ink ? colours >> 4 : colours & 15u;
                    if (!colour) colour = ink ? r[7] >> 4 : r[7] & 15u;
                    if (!colour) colour = backdrop;
                    PutDebugRgba(m_debugNameRgba.get(), 512, column * 6 + px, y,
                                 GetCurrentPalette(), colour);
                }
            }
            else if (text64)
            {
                const u16 na = Text64NameQuadAddress(r[2], y >> 3, column >> 2) + (column & 3u);
                const u8 name = vram[na & VramMask];
                const u16 pa = mode == VideoMode::Text64Q
                    ? TMS99xxTableAddress::Pattern(3, r[3], r[4], y, name)
                    : TextPatternAddress(r[4], name, y);
                for (unsigned px = 0; px < 8; ++px)
                {
                    unsigned colour = (vram[pa] & (0x80u >> px)) ? r[7] >> 4 : r[7] & 15u;
                    if (!colour) colour = backdrop;
                    PutDebugRgba(m_debugNameRgba.get(), 512, column * 8 + px, y,
                                 GetCurrentPalette(), colour);
                }
            }
        }
    }
    m_debugBuiltSignature[0] = signature;
    m_debugBuiltVramRevision[0] = vramRevision;
    ++m_debugSurfaceGeneration[0];
}

void TMS9918B::BuildExtendedPatternDebugSurface() const
{
    using namespace TMS9918BModel;
    const u64 signature = DebugSurfaceSignature();
    const u64 vramRevision = GetDebugVRAMRevision();
    if (m_debugBuiltSignature[1] == signature && m_debugBuiltVramRevision[1] == vramRevision)
        return;
    ClearDebugRgba(m_debugPatternRgba.get(), 256u * 192u);
    const u8* r = GetRegisters(); const u8* vram = GetVRAM();
    const u8* debugPalette = GetDebugProgrammablePalette();
    const VideoMode mode = DecodeMode(r[0], r[1], m_extendedRegisters[3], m_extendedRegisters[4]);
    const bool g1x = mode == VideoMode::Graphics1X;
    const bool g2f = mode == VideoMode::Graphics2Fat;
    const bool textQ = mode == VideoMode::Text40XQ || mode == VideoMode::Text64Q;
    const unsigned count = g1x ? 512u : ((g2f || textQ) ? 768u : 256u);
    for (unsigned tile = 0; tile < count; ++tile)
        for (unsigned row = 0; row < 8; ++row)
        {
            u16 pa;
            if (g1x) pa = Graphics1XPatternAddress(r[4], tile, row);
            else if (g2f) pa = Graphics2FatPatternAddress(r[4], (tile >> 8) * 64 + row, tile & 255u);
            else if (textQ) pa = TMS99xxTableAddress::Pattern(3, r[3], r[4],
                static_cast<u16>((tile >> 8) * 64 + row), static_cast<u8>(tile));
            else pa = TextPatternAddress(r[4], static_cast<u8>(tile), row);
            const u8 low = vram[pa], high = vram[(pa + 1) & VramMask];
            for (unsigned px = 0; px < 8; ++px)
            {
                unsigned colour;
                const u8* palette;
                if (g1x)
                {
                    const unsigned bit = 7 - px;
                    colour = ((low >> bit) & 1u) | (((high >> bit) & 1u) << 1);
                    palette = debugPalette;
                }
                else if (g2f)
                {
                    const unsigned shift = 6 - ((px >> 1) * 2);
                    colour = ((low >> shift) & 3u) | (((high >> shift) & 3u) << 2);
                    palette = GetCurrentPalette();
                }
                else
                {
                    colour = (low & (0x80u >> px)) ? 15u : 0u;
                    palette = GetCurrentPalette();
                }
                PutDebugRgba(m_debugPatternRgba.get(), 256,
                    (tile & 31u) * 8 + px, (tile >> 5) * 8 + row, palette, colour);
            }
        }
    m_debugBuiltSignature[1] = signature; m_debugBuiltVramRevision[1] = vramRevision;
    ++m_debugSurfaceGeneration[1];
}

void TMS9918B::BuildExtendedSpritePatternDebugSurface(u64 optionMask) const
{
    using namespace TMS9918BModel;
    const u64 signature = DebugSurfaceSignature(); const u64 vr = GetDebugVRAMRevision();
    if (m_debugBuiltSignature[2] == signature && m_debugBuiltVramRevision[2] == vr &&
        m_debugSpriteBuiltOptionMask == optionMask) return;
    ClearDebugRgba(m_debugSpritePatternRgba.get(), 256u * 128u);
    const u8* r = GetRegisters(); const u8* vram = GetVRAM();
    const u8* debugPalette = GetDebugProgrammablePalette();
    u8 colours[512] = {};
    if (optionMask & 1ull)
    {
        const u16 sat = static_cast<u16>((r[5] & 0x7F) << 7);
        for (unsigned sprite = 0; sprite < 32; ++sprite)
        {
            const u16 sa = (sat + sprite * 4) & VramMask;
            if (vram[sa] == 0xD0) break;
            const u8 attr = vram[(sa + 3) & VramMask];
            unsigned colour = ((attr >> 2) & 3u) * 4u + (attr & 3u);
            if ((sprite & 1u) != 0 && (attr & 0x10) != 0)
            {
                const u8 firstAttr = vram[(sa - 1) & VramMask];
                colour = ((firstAttr >> 2) & 3u) * 4u + 2u;
            }
            else if ((sprite & 1u) == 0 && sprite + 1 < 32 &&
                     (vram[(sa + 7) & VramMask] & 0x10) != 0)
                colour = ((attr >> 2) & 3u) * 4u + 1u;
            unsigned tile = vram[(sa + 2) & VramMask];
            const unsigned parts = (r[1] & 2) ? 4u : 1u;
            if (parts == 4) tile &= 0xFC;
            tile += (attr & 0x20) ? 256u : 0u;
            if (colour == 0)
                continue;
            for (unsigned part = 0; part < parts; ++part)
                if (!colours[tile + part])
                    colours[tile + part] = static_cast<u8>(colour);
        }
    }
    for (unsigned tile = 0; tile < 512; ++tile)
        for (unsigned row = 0; row < 8; ++row)
        {
            const u16 pa = SpritePatternAddress(r[6], tile >= 256, static_cast<u8>(tile), row, false);
            const u8 bits = vram[pa];
            for (unsigned px = 0; px < 8; ++px)
            {
                const bool set = (bits & (0x80u >> px)) != 0;
                const unsigned colour = set ? (colours[tile] ? colours[tile] : 15u) : 0u;
                const u8* palette = (set && colours[tile]) ? debugPalette : GetCurrentPalette();
                PutDebugRgba(m_debugSpritePatternRgba.get(), 256,
                    (tile & 31u) * 8 + px, (tile >> 5) * 8 + row, palette, colour);
            }
        }
    m_debugBuiltSignature[2] = signature; m_debugBuiltVramRevision[2] = vr;
    m_debugSpriteBuiltOptionMask = optionMask; ++m_debugSurfaceGeneration[2];
}

bool TMS9918B::ReadDebugLayerSurface(std::size_t index,
    GC_VideoDebugLayerSurface& surface) const
{
    return ReadDebugLayerSurfaceWithOptions(index, index == 2 ? 1ull : 0ull,
                                            surface);
}

std::size_t TMS9918B::GetDebugLayerOptionCount(std::size_t index) const
{
    if (!IsExtendedDisplayMode())
        return TMS99xx::GetDebugLayerOptionCount(index);
    return index == 2 ? 1 : 0;
}

bool TMS9918B::DescribeDebugLayerOption(std::size_t layerIndex,
    std::size_t optionIndex, GC_VideoDebugLayerOptionDescriptor& descriptor) const
{
    if (!IsExtendedDisplayMode())
        return TMS99xx::DescribeDebugLayerOption(layerIndex, optionIndex,
                                                 descriptor);
    if (layerIndex != 2 || optionIndex != 0)
        return false;
    descriptor = {};
    descriptor.code = "TMS9918B.SPRGEN.SHOW_COLOURS";
    descriptor.name = "Show colors";
    descriptor.enabled = true;
    return true;
}

bool TMS9918B::ReadDebugLayerSurfaceWithOptions(std::size_t index,
    u64 optionMask, GC_VideoDebugLayerSurface& surface) const
{
    if (!IsExtendedDisplayMode())
        return TMS99xx::ReadDebugLayerSurfaceWithOptions(index, optionMask,
                                                         surface);
    GC_VideoDebugLayerDescriptor layer;
    if (!DescribeDebugLayer(index, layer))
        return false;
    surface = {};
    if (index == 0)
    {
        BuildExtendedNameDebugSurface();
        surface.rgba = m_debugNameRgba.get(); surface.width = 512;
        surface.height = 192; surface.row_pitch = 512 * 4;
    }
    else if (index == 1)
    {
        BuildExtendedPatternDebugSurface();
        surface.rgba = m_debugPatternRgba.get(); surface.width = 256;
        surface.height = layer.content_height; surface.row_pitch = 256 * 4;
    }
    else if (index == 2)
    {
        BuildExtendedSpritePatternDebugSurface(optionMask);
        surface.rgba = m_debugSpritePatternRgba.get(); surface.width = 256;
        surface.height = 128; surface.row_pitch = 256 * 4;
    }
    else return false;
    surface.generation = m_debugSurfaceGeneration[index];
    return surface.rgba != nullptr;
}

std::size_t TMS9918B::GetDebugLayerAreaCount(std::size_t index) const
{
    using namespace TMS9918BModel;
    if (!IsExtendedDisplayMode()) return TMS99xx::GetDebugLayerAreaCount(index);
    const VideoMode mode = DecodeMode(GetRegisters()[0], GetRegisters()[1],
                                      m_extendedRegisters[3], m_extendedRegisters[4]);
    if (index == 2) return 2;
    if (index == 1)
    {
        if (mode == VideoMode::Bitmap || mode == VideoMode::BitmapQ) return 0;
        return (mode == VideoMode::Graphics2Fat || mode == VideoMode::Text40XQ ||
                mode == VideoMode::Text64Q) ? 3 : 1;
    }
    if (index == 0) return (mode == VideoMode::Bitmap || mode == VideoMode::BitmapQ) ? 2 : 1;
    return 0;
}

bool TMS9918B::DescribeDebugLayerArea(std::size_t index,
    std::size_t areaIndex, GC_VideoDebugLayerAreaDescriptor& descriptor) const
{
    using namespace TMS9918BModel;
    if (!IsExtendedDisplayMode())
        return TMS99xx::DescribeDebugLayerArea(index, areaIndex, descriptor);
    if (areaIndex >= GetDebugLayerAreaCount(index)) return false;
    const u8* r = GetRegisters();
    const VideoMode mode = DecodeMode(r[0], r[1], m_extendedRegisters[3], m_extendedRegisters[4]);
    descriptor = {}; descriptor.address_width_bits = 14;
    if (index == 2)
    {
        descriptor.code = areaIndex ? "SPRGEN1" : "SPRGEN0";
        descriptor.label = areaIndex ? "Sprite Generator BANK 1" : "Sprite Generator BANK 0";
        descriptor.address = ((r[6] & 7u) << 11) + static_cast<u32>(areaIndex * 0x800u);
        descriptor.address &= VramMask; descriptor.size = 0x800;
        return true;
    }
    if (index == 1)
    {
        descriptor.code = "PATGEN"; descriptor.label = "Pattern Generator";
        if (mode == VideoMode::Graphics2Fat || mode == VideoMode::Text40XQ ||
            mode == VideoMode::Text64Q)
        {
            static const char* labels[] = {
                "Pattern Generator Top", "Pattern Generator Middle",
                "Pattern Generator Bottom"
            };
            descriptor.label = labels[areaIndex];
            descriptor.address = mode == VideoMode::Graphics2Fat
                ? Graphics2FatPatternAddress(r[4], static_cast<u16>(areaIndex * 64), 0)
                : TMS99xxTableAddress::Pattern(3, r[3], r[4],
                    static_cast<u16>(areaIndex * 64), 0);
            descriptor.size = mode == VideoMode::Graphics2Fat ? 0x1000 : 0x800;
        }
        else
        {
            descriptor.address = static_cast<u32>(r[4] & 7u) << 11;
            descriptor.size = mode == VideoMode::Graphics1X ? 0x2000 : 0x800;
        }
        return true;
    }
    if (mode == VideoMode::Bitmap || mode == VideoMode::BitmapQ)
    {
        descriptor.code = areaIndex ? "PALMAP" : "BITMAP";
        descriptor.label = areaIndex ? "Palette Map" : "Bitmap";
        descriptor.address = areaIndex ? static_cast<u32>(r[2] & 15u) << 10 :
            static_cast<u32>(r[4] & (mode == VideoMode::BitmapQ ? 4u : 7u)) << 11;
        descriptor.size = areaIndex ? 0x300 : 0x3000;
        return true;
    }
    descriptor.code = "NAMTAB"; descriptor.label = "Name Table";
    descriptor.address = static_cast<u32>(r[2] & 15u) << 10;
    descriptor.size = (mode == VideoMode::Text64 || mode == VideoMode::Text64Q) ? 0x600 :
        ((mode == VideoMode::Text40X || mode == VideoMode::Text40XQ) ? 0x780 : 0x600);
    return true;
}

bool TMS9918B::DescribeDebugGridCell(std::size_t index, u32 column, u32 row,
    GC_VideoDebugGridCell& cell) const
{
    using namespace TMS9918BModel;
    if (!IsExtendedDisplayMode()) return TMS99xx::DescribeDebugGridCell(index, column, row, cell);
    GC_VideoDebugLayerDescriptor layer;
    if (!DescribeDebugLayer(index, layer) || column >= layer.columns || row >= layer.rows) return false;
    cell = {}; cell.valid = row * layer.columns + column < layer.active_cell_count;
    cell.column = column; cell.row = row;
    if (!cell.valid) return true;
    const u8* r = GetRegisters();
    const VideoMode mode = DecodeMode(r[0], r[1], m_extendedRegisters[3], m_extendedRegisters[4]);
    if (index == 2)
    {
        const u32 tile = row * 32 + column;
        cell.navigation_address = SpritePatternAddress(r[6], tile >= 256,
            static_cast<u8>(tile), 0, false);
    }
    else if (index == 1)
    {
        const u32 tile = row * 32 + column;
        if (mode == VideoMode::Graphics1X) cell.navigation_address = Graphics1XPatternAddress(r[4], tile, 0);
        else if (mode == VideoMode::Graphics2Fat) cell.navigation_address = Graphics2FatPatternAddress(r[4], (tile >> 8) * 64, tile);
        else if (mode == VideoMode::Text40XQ || mode == VideoMode::Text64Q)
            cell.navigation_address = TMS99xxTableAddress::Pattern(3, r[3], r[4], (tile >> 8) * 64, tile);
        else cell.navigation_address = TextPatternAddress(r[4], tile, 0);
    }
    else if (mode == VideoMode::Graphics1X || mode == VideoMode::Graphics2Fat)
        cell.navigation_address = NameAttrAddress(r[2], row, column);
    else if (mode == VideoMode::Bitmap || mode == VideoMode::BitmapQ)
        cell.navigation_address = (mode == VideoMode::Bitmap ? BitmapQuadAddress(r[4], row * 8, column >> 1) :
            BitmapQQuadAddress(r[4], row * 8, column >> 1)) + (column & 1u) * 2u;
    else if (mode == VideoMode::Text40X || mode == VideoMode::Text40XQ)
        cell.navigation_address = TextCharColourAddress(r[2], row, column >> 1) + (column & 1u) * 2u;
    else cell.navigation_address = Text64NameQuadAddress(r[2], row, column >> 2) + (column & 3u);
    cell.navigation_address &= VramMask;
    return true;
}

std::size_t TMS9918B::GetDebugGridCellFieldCount(std::size_t index,
    u32 column, u32 row) const
{
    GC_VideoDebugGridCell cell;
    if (!IsExtendedDisplayMode()) return TMS99xx::GetDebugGridCellFieldCount(index, column, row);
    if (!DescribeDebugGridCell(index, column, row, cell) || !cell.valid) return 0;
    return index == 0 ? 4 : (index == 2 ? 3 : 2);
}

bool TMS9918B::DescribeDebugGridCellField(std::size_t index, u32 column,
    u32 row, std::size_t fieldIndex,
    GC_VideoDebugGridCellFieldDescriptor& descriptor) const
{
    using namespace TMS9918BModel;
    if (!IsExtendedDisplayMode()) return TMS99xx::DescribeDebugGridCellField(index, column, row, fieldIndex, descriptor);
    GC_VideoDebugGridCell cell;
    if (!DescribeDebugGridCell(index, column, row, cell) || !cell.valid ||
        fieldIndex >= GetDebugGridCellFieldCount(index, column, row)) return false;
    descriptor = {};
    const u8* r = GetRegisters(); const u8* vram = GetVRAM();
    const VideoMode mode = DecodeMode(r[0], r[1], m_extendedRegisters[3], m_extendedRegisters[4]);
    const u32 tileIndex = row * 32 + column;
    auto number = [&](const char* code, const char* label, GC_VideoDebugValueFormat format,
                      u64 value, u8 bits, bool source = false)
    {
        descriptor.code = code; descriptor.label = label; descriptor.format = format;
        descriptor.value = value; descriptor.width_bits = bits;
        descriptor.has_source_address = source; descriptor.source_address = cell.navigation_address;
        descriptor.source_address_width_bits = 14;
    };
    if (index == 1 || index == 2)
    {
        if (fieldIndex == 0) number(index == 1 ? "TMS9918B.PATGEN.INDEX" : "TMS9918B.SPRGEN.INDEX",
            "Pattern", GC_VideoDebugValueFormat::Hexadecimal, tileIndex, index == 2 ? 9 : 10);
        else if (fieldIndex == 1) number(index == 1 ? "TMS9918B.PATGEN.ADDRESS" : "TMS9918B.SPRGEN.ADDRESS",
            "Address", GC_VideoDebugValueFormat::Address, cell.navigation_address, 14);
        else number("TMS9918B.SPRGEN.BANK", "BANK", GC_VideoDebugValueFormat::Decimal, tileIndex >> 8, 1);
        return true;
    }
    const u8 first = vram[cell.navigation_address];
    if (fieldIndex == 0) { number("TMS9918B.MAP.ADDRESS", "Address", GC_VideoDebugValueFormat::Address, cell.navigation_address, 14); return true; }
    if (mode == VideoMode::Graphics1X || mode == VideoMode::Graphics2Fat)
    {
        const u8 attr = vram[(cell.navigation_address + 1) & VramMask];
        if (fieldIndex == 1) number("TMS9918B.NAMTAB.NAME", "Name", GC_VideoDebugValueFormat::Hexadecimal, first, 8, true);
        else if (fieldIndex == 2)
        {
            number("TMS9918B.NAMTAB.ATTRIBUTE", "Attribute",
                GC_VideoDebugValueFormat::Hexadecimal, attr, 8, true);
            descriptor.source_address = (cell.navigation_address + 1) & VramMask;
        }
        else
        {
            const u16 pa = mode == VideoMode::Graphics1X ? Graphics1XPatternAddress(r[4], first | ((attr & 8u) << 5), row * 8) :
                Graphics2FatPatternAddress(r[4], row * 8, first);
            number("TMS9918B.NAMTAB.PATTERN_ADDRESS", "Pattern Address", GC_VideoDebugValueFormat::Address, pa, 14);
        }
    }
    else if (mode == VideoMode::Bitmap || mode == VideoMode::BitmapQ)
    {
        const u16 map = PaletteMapPairAddress(r[2], row, column >> 1) + (column & 1u);
        if (fieldIndex == 1) number("TMS9918B.BITMAP.PIXELS0", "Stratum 0", GC_VideoDebugValueFormat::Hexadecimal, first, 8, true);
        else if (fieldIndex == 2)
        {
            number("TMS9918B.BITMAP.PIXELS1", "Stratum 1",
                GC_VideoDebugValueFormat::Hexadecimal,
                vram[(cell.navigation_address + 1) & VramMask], 8, true);
            descriptor.source_address = (cell.navigation_address + 1) & VramMask;
        }
        else number("TMS9918B.BITMAP.PALETTE_MAP", "Palette Map", GC_VideoDebugValueFormat::Address, map & VramMask, 14);
    }
    else
    {
        if (fieldIndex == 1) number("TMS9918B.NAMTAB.NAME", "Name", GC_VideoDebugValueFormat::Hexadecimal, first, 8, true);
        else if (fieldIndex == 2 && (mode == VideoMode::Text40X || mode == VideoMode::Text40XQ))
        {
            number("TMS9918B.NAMTAB.COLOUR", "Color",
                GC_VideoDebugValueFormat::Hexadecimal,
                vram[(cell.navigation_address + 1) & VramMask], 8, true);
            descriptor.source_address = (cell.navigation_address + 1) & VramMask;
        }
        else if (fieldIndex == 2) number("TMS9918B.NAMTAB.R7", "R7 Colors", GC_VideoDebugValueFormat::Hexadecimal, r[7], 8);
        else number("TMS9918B.NAMTAB.PATTERN_ADDRESS", "Pattern Address", GC_VideoDebugValueFormat::Address,
            (mode == VideoMode::Text40XQ || mode == VideoMode::Text64Q) ?
                TMS99xxTableAddress::Pattern(3, r[3], r[4], row * 8, first) : TextPatternAddress(r[4], first, 0), 14);
    }
    return true;
}

bool TMS9918B::DescribeDebugSpriteAttributeTable(std::size_t index,
    GC_VideoDebugSpriteAttributeTableDescriptor& descriptor) const
{
    if (!IsExtendedDisplayMode()) return TMS99xx::DescribeDebugSpriteAttributeTable(index, descriptor);
    if (!TMS99xx::DescribeDebugSpriteAttributeTable(index, descriptor)) return false;
    descriptor.code = "TMS9918B.SAT"; descriptor.name = "SAT";
    return true;
}

void TMS9918B::BuildExtendedLiveSpriteAtlas() const
{
    using namespace TMS9918BModel;
    const u64 signature = DebugSurfaceSignature();
    const u64 vramRevision = GetDebugVRAMRevision();
    if (m_debugSpriteAtlasBuiltSignature == signature &&
        m_debugSpriteAtlasBuiltVramRevision == vramRevision)
        return;

    constexpr unsigned atlasWidth = 128;
    constexpr unsigned atlasHeight = 64;
    constexpr unsigned cellSize = 16;
    ClearDebugRgba(m_debugSpriteAtlasRgba.get(), atlasWidth * atlasHeight);
    const u8* r = GetRegisters();
    const u8* vram = GetVRAM();
    const u8* debugPalette = GetDebugProgrammablePalette();
    const VideoMode mode = DecodeMode(r[0], r[1], m_extendedRegisters[3],
                                      m_extendedRegisters[4]);
    const ModeInfo info = Info(mode);
    const unsigned spriteSize = (r[1] & 2) ? 16u : 8u;
    const unsigned zoom = (r[1] & 1) ? 2u : 1u;
    const bool text64 = mode == VideoMode::Text64 ||
                        mode == VideoMode::Text64Q;
    const u16 sat = static_cast<u16>((r[5] & 0x7F) << 7);
    bool terminated = false;

    for (unsigned sprite = 0; sprite < 32; ++sprite)
    {
        auto& item = m_debugSpriteAtlasItems[sprite];
        item = {};
        item.index = sprite;
        item.atlas_x = (sprite & 7u) * cellSize;
        item.atlas_y = (sprite >> 3) * cellSize;
        item.width = spriteSize;
        item.height = spriteSize;
        if (terminated ||
            (info.spritesPerLine == 2 && sprite >= info.spritesPerLine))
            continue;

        const u16 sa = static_cast<u16>((sat + sprite * 4) & VramMask);
        if (vram[sa] == 0xD0)
        {
            terminated = true;
            continue;
        }
        const u8 attribute = vram[(sa + 3) & VramMask];
        const bool pairedPlane = (sprite & 1u) != 0 && (attribute & 0x10) != 0;
        const bool pairedLeader = (sprite & 1u) == 0 && sprite + 1 < 32 &&
            (vram[(sa + 7) & VramMask] & 0x10) != 0;
        unsigned colour = ((attribute >> 2) & 3u) * 4u + (attribute & 3u);
        if (pairedPlane)
        {
            const u8 leader = vram[(sa - 1) & VramMask];
            colour = ((leader >> 2) & 3u) * 4u + 2u;
        }
        else if (pairedLeader)
            colour = ((attribute >> 2) & 3u) * 4u + 1u;

        GC_VideoDebugSpriteAttributeRecordDescriptor record;
        if (DescribeDebugSpriteAttributeRecord(0, sprite, record) &&
            record.visible)
        {
            int screenY = (vram[sa] + 1) & 0xFF;
            if (screenY >= 0xE0) screenY -= 0x100;
            int coarseX = vram[(sa + 1) & VramMask];
            if (attribute & 0x80) coarseX -= 32;
            const int screenX = text64
                ? coarseX * 2 + ((attribute & 0x40) ? 1 : 0)
                : coarseX;
            item.has_screen_rect = true;
            item.screen_x = static_cast<float>(screenX);
            item.screen_y = static_cast<float>(screenY);
            item.screen_width = static_cast<float>(spriteSize * zoom);
            item.screen_height = static_cast<float>(spriteSize * zoom);
        }

        u8 name = vram[(sa + 2) & VramMask];
        if (spriteSize == 16) name &= 0xFC;
        for (unsigned y = 0; y < spriteSize; ++y)
            for (unsigned x = 0; x < spriteSize; ++x)
            {
                const u16 address = SpritePatternAddress(r[6],
                    (attribute & 0x20) != 0, name, y, x >= 8);
                if ((vram[address] & (0x80u >> (x & 7u))) == 0 || colour == 0)
                    continue;
                PutDebugRgba(m_debugSpriteAtlasRgba.get(), atlasWidth,
                    item.atlas_x + x, item.atlas_y + y,
                    debugPalette, colour);
            }
    }
    m_debugSpriteAtlasBuiltSignature = signature;
    m_debugSpriteAtlasBuiltVramRevision = vramRevision;
    ++m_debugSpriteAtlasGeneration;
}

bool TMS9918B::ReadDebugSpriteAtlas(const char* sourceCode, int scanline,
    GC_VideoDebugSpriteAtlasSurface& surface) const
{
    if (!IsExtendedDisplayMode() || !sourceCode ||
        std::strcmp(sourceCode, "live-sat") != 0)
        return TMS99xx::ReadDebugSpriteAtlas(sourceCode, scanline, surface);
    BuildExtendedLiveSpriteAtlas();
    surface = {};
    surface.rgba = m_debugSpriteAtlasRgba.get();
    surface.width = 128;
    surface.height = 64;
    surface.row_pitch = 128 * 4;
    surface.items = m_debugSpriteAtlasItems.data();
    surface.item_count = 32;
    surface.generation = m_debugSpriteAtlasGeneration;
    return true;
}

bool TMS9918B::DescribeDebugSpriteAttributeColumn(std::size_t tableIndex,
    std::size_t columnIndex, GC_VideoDebugSpriteAttributeColumnDescriptor& descriptor) const
{
    if (!IsExtendedDisplayMode()) return TMS99xx::DescribeDebugSpriteAttributeColumn(tableIndex, columnIndex, descriptor);
    static const GC_VideoDebugSpriteAttributeColumnDescriptor columns[] = {
        {"y", "Y", 8}, {"x", "X", 8}, {"pattern", "PAT", 8},
        {"attribute", "EC/XF/B/P/PL/E", 8}
    };
    if (tableIndex || columnIndex >= 4) return false;
    descriptor = columns[columnIndex]; return true;
}

bool TMS9918B::DescribeDebugSpriteAttributeRecord(std::size_t tableIndex,
    u32 recordIndex, GC_VideoDebugSpriteAttributeRecordDescriptor& descriptor) const
{
    using namespace TMS9918BModel;
    if (!IsExtendedDisplayMode()) return TMS99xx::DescribeDebugSpriteAttributeRecord(tableIndex, recordIndex, descriptor);
    if (tableIndex || recordIndex >= 32) return false;
    const u8* r = GetRegisters(); const u8* vram = GetVRAM();
    const VideoMode mode = DecodeMode(r[0], r[1], m_extendedRegisters[3], m_extendedRegisters[4]);
    const ModeInfo info = Info(mode); const u32 base = static_cast<u32>(r[5] & 0x7F) << 7;
    descriptor = {}; descriptor.address = (base + recordIndex * 4) & VramMask;
    descriptor.visible = info.spritesPerLine != 0 &&
        (info.spritesPerLine != 2 || recordIndex < 2);
    for (u32 i = 0; descriptor.visible && i <= recordIndex; ++i)
        if (vram[(base + i * 4) & VramMask] == 0xD0) descriptor.visible = false;
    if (!descriptor.visible) return true;
    const u8 attr = vram[(descriptor.address + 3) & VramMask];
    const bool pairedPlane = (recordIndex & 1u) && (attr & 0x10);
    const bool pairedLeader = (recordIndex & 1u) == 0 && recordIndex + 1 < 32 &&
        (vram[(descriptor.address + 7) & VramMask] & 0x10);
    if ((attr & 3u) == 0 && !pairedPlane && !pairedLeader)
        descriptor.visible = false;
    int y = (vram[descriptor.address] + 1) & 255; if (y >= 0xE0) y -= 256;
    int coarseX = vram[(descriptor.address + 1) & VramMask];
    if (attr & 0x80) coarseX -= 32;
    const bool text64 = mode == VideoMode::Text64 || mode == VideoMode::Text64Q;
    const int x = text64
        ? coarseX * 2 + ((attr & 0x40) ? 1 : 0)
        : coarseX;
    const int size = ((r[1] & 2) ? 16 : 8) * ((r[1] & 1) ? 2 : 1);
    descriptor.visible = descriptor.visible && x < Info(mode).pixelsPerLine &&
        x + size > 0 && y < 192 && y + size > 0;
    return true;
}

void TMS9918B::SaveState(std::ostream& stream)
{
    TMS99xx::SaveState(stream);
    StateWriter w(stream);
    w.Bool(m_extendedUnlocked);
    w.U8(m_unlockProgress);
    w.Bool(m_paletteWriteMode);
    w.U8(m_paletteAddress);
    w.U8(m_status1);
    w.U8(m_lineCounter);
    w.U8(m_latchedHorizontalScroll);
    w.U8(m_latchedVerticalScroll);
    w.Bytes(m_extendedRegisters.data(), m_extendedRegisters.size());
    w.Bytes(m_paletteEntries.data(), m_paletteEntries.size());
}

void TMS9918B::LoadState(std::istream& stream)
{
    TMS99xx::LoadState(stream);
    StateReader r(stream);
    m_extendedUnlocked = r.Bool();
    m_unlockProgress = r.U8();
    m_paletteWriteMode = r.Bool();
    m_paletteAddress = r.U8() & 0x0F;
    m_status1 = r.U8();
    m_lineCounter = r.U8();
    m_latchedHorizontalScroll = r.U8();
    m_latchedVerticalScroll = r.U8();
    r.Bytes(m_extendedRegisters.data(), m_extendedRegisters.size());
    r.Bytes(m_paletteEntries.data(), m_paletteEntries.size());
    // Raster history is presentation-only. Rebuild it from the next complete
    // frame instead of extending the state format with debugger data.
    m_lastScrollFrameValid = false;
    m_debugFramePaletteValid = false;
    for (unsigned i = 0; i < m_paletteEntries.size(); ++i)
        RebuildPaletteEntry(i);
    ++m_paletteRevision;
    RefreshInterruptLine();
}
