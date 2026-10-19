/*
 * GearSF7000 - TMS99xx internal VRAM slot schedule
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TMS99XX_VRAM_SLOT_SCHEDULE_H
#define TMS99XX_VRAM_SLOT_SCHEDULE_H

#include <cstdint>

#include "TMS99xxSlotGrid.h"

// Pure description of the documented TMS99xx 171-slot scanline map. Address
// generation and pixel output remain in TMS99xx until their fetch latches exist.
class TMS99xxVramSlotSchedule final
{
public:
    static constexpr std::uint16_t SlotsPerLine =
        TMS99xxSlotGrid::SlotsPerLine;

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

#endif // TMS99XX_VRAM_SLOT_SCHEDULE_H
