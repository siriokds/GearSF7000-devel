/*
 * GearSF7000 - TMS9918/TMS9929 internal VRAM slot schedule
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TMS9918_VRAM_SLOT_SCHEDULE_H
#define TMS9918_VRAM_SLOT_SCHEDULE_H

#include <cstdint>

#include "TMS9918SlotGrid.h"

// Pure description of the documented TMS9918 171-slot scanline map. Address
// generation and pixel output remain in Video until their fetch latches exist.
class TMS9918VramSlotSchedule final
{
public:
    static constexpr std::uint16_t SlotsPerLine =
        TMS9918SlotGrid::SlotsPerLine;

    enum class Schedule : std::uint8_t
    {
        Refresh,
        Graphics,
        Text,
        // Multicolor shares the Graphics calendar exactly, minus the colour
        // table access: vdp18_ctrl.vhd skips AC_PCT in that mode and the slot
        // falls back to its AC_CPU default, so the CPU gains one slot per
        // character.
        Multicolor
    };

    enum class Activity : std::uint8_t
    {
        Unknown,
        Cpu,
        Refresh,
        Name,
        Colour,
        Pattern,
        // The VDP first scans all 32 Y values, then fetches the complete
        // attributes/pattern rows for the first four selected sprites. These
        // are deliberately distinct: their indices have different meanings.
        SpriteScanY,
        SpriteSelectedY,
        SpriteSelectedX,
        SpriteSelectedName,
        SpriteSelectedColour,
        SpriteSelectedPattern
    };

    struct Slot
    {
        Activity activity = Activity::Unknown;
        std::uint8_t index = 0;
        std::uint8_t byte = 0;
    };

    static Slot GetSlot(Schedule schedule, std::uint16_t slot);
    static bool IsCpuSlot(Schedule schedule, std::uint16_t slot);
};

#endif // TMS9918_VRAM_SLOT_SCHEDULE_H
