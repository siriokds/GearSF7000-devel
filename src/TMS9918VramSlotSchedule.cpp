/*
 * GearSF7000 - TMS9918/TMS9929 internal VRAM slot schedule
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TMS9918VramSlotSchedule.h"

namespace
{
using Schedule = TMS9918VramSlotSchedule::Schedule;
using Activity = TMS9918VramSlotSchedule::Activity;
using Slot = TMS9918VramSlotSchedule::Slot;

constexpr bool InRange(std::uint16_t value, std::uint16_t first,
                       std::uint16_t last)
{
    return value >= first && value <= last;
}

constexpr Slot Make(Activity activity, std::uint8_t index = 0,
                    std::uint8_t byte = 0)
{
    return {activity, index, byte};
}

Slot GetRefreshSlot(std::uint16_t slot)
{
    if (InRange(slot, 0, 26) ||
        (InRange(slot, 28, 152) && (slot & 1u) == 0) ||
        InRange(slot, 154, 170))
    {
        return Make(Activity::Cpu);
    }
    return Make(Activity::Refresh);
}

Slot GetGraphicsSlot(std::uint16_t slot)
{
    if (InRange(slot, 0, 4) || InRange(slot, 15, 18) ||
        InRange(slot, 155, 156))
    {
        return Make(Activity::Cpu);
    }

    // Sprite 2 continues from slots 169/170 of the preceding scanline.
    switch (slot)
    {
        case 5:  return Make(Activity::SpriteSelectedName, 2);
        case 6:  return Make(Activity::SpriteSelectedColour, 2);
        case 7:  return Make(Activity::SpriteSelectedPattern, 2, 0);
        case 8:  return Make(Activity::SpriteSelectedPattern, 2, 1);
        case 9:  return Make(Activity::SpriteSelectedY, 3);
        case 10: return Make(Activity::SpriteSelectedX, 3);
        case 11: return Make(Activity::SpriteSelectedName, 3);
        case 12: return Make(Activity::SpriteSelectedColour, 3);
        case 13: return Make(Activity::SpriteSelectedPattern, 3, 0);
        case 14: return Make(Activity::SpriteSelectedPattern, 3, 1);
        default: break;
    }

    if (InRange(slot, 19, 26))
        return Make(Activity::SpriteScanY, static_cast<std::uint8_t>(slot - 19));

    if (InRange(slot, 27, 154))
    {
        const std::uint16_t character = (slot - 27) / 4;
        switch ((slot - 27) % 4)
        {
            case 0: return Make(Activity::Name, static_cast<std::uint8_t>(character));
            case 1:
                if ((character % 4) == 0)
                    return Make(Activity::Cpu);
                return Make(Activity::SpriteScanY, static_cast<std::uint8_t>(
                    8 + character - ((character + 3) / 4)));
            case 2: return Make(Activity::Colour, static_cast<std::uint8_t>(character));
            case 3: return Make(Activity::Pattern, static_cast<std::uint8_t>(character));
        }
    }

    switch (slot)
    {
        case 157: return Make(Activity::SpriteSelectedY, 0);
        case 158: return Make(Activity::SpriteSelectedX, 0);
        case 159: return Make(Activity::SpriteSelectedName, 0);
        case 160: return Make(Activity::SpriteSelectedColour, 0);
        case 161: return Make(Activity::SpriteSelectedPattern, 0, 0);
        case 162: return Make(Activity::SpriteSelectedPattern, 0, 1);
        case 163: return Make(Activity::SpriteSelectedY, 1);
        case 164: return Make(Activity::SpriteSelectedX, 1);
        case 165: return Make(Activity::SpriteSelectedName, 1);
        case 166: return Make(Activity::SpriteSelectedColour, 1);
        case 167: return Make(Activity::SpriteSelectedPattern, 1, 0);
        case 168: return Make(Activity::SpriteSelectedPattern, 1, 1);
        case 169: return Make(Activity::SpriteSelectedY, 2);
        case 170: return Make(Activity::SpriteSelectedX, 2);
        default: return Make(Activity::Unknown);
    }
}

Slot GetTextSlot(std::uint16_t slot)
{
    if (InRange(slot, 0, 29) || InRange(slot, 150, 170))
        return Make(Activity::Cpu);

    const std::uint16_t character = (slot - 30) / 3;
    switch ((slot - 30) % 3)
    {
        case 0: return Make(Activity::Name, static_cast<std::uint8_t>(character));
        case 1: return Make(Activity::Cpu);
        case 2: return Make(Activity::Pattern, static_cast<std::uint8_t>(character));
    }
    return Make(Activity::Unknown);
}

bool IsSpriteActivity(Activity activity)
{
    switch (activity)
    {
        case Activity::SpriteScanY:
        case Activity::SpriteSelectedY:
        case Activity::SpriteSelectedX:
        case Activity::SpriteSelectedName:
        case Activity::SpriteSelectedColour:
        case Activity::SpriteSelectedPattern:
            return true;
        default:
            return false;
    }
}

Slot GetMulticolorSlot(std::uint16_t slot)
{
    // Identical to Graphics except that there is no colour table: the VDP does
    // not fetch one, so that slot is simply left to the CPU. Multicolor is
    // therefore the mode with the most CPU bandwidth while the display is on.
    const Slot graphics = GetGraphicsSlot(slot);
    return graphics.activity == Activity::Colour ? Make(Activity::Cpu)
                                                 : graphics;
}
}

TMS9918VramSlotSchedule::Slot TMS9918VramSlotSchedule::GetSlot(
    Schedule schedule, std::uint16_t slot)
{
    slot %= SlotsPerLine;
    switch (schedule)
    {
        case Schedule::Refresh: return GetRefreshSlot(slot);
        case Schedule::Graphics: return GetGraphicsSlot(slot);
        case Schedule::Text: return GetTextSlot(slot);
        case Schedule::Multicolor: return GetMulticolorSlot(slot);
    }
    return Make(Activity::Unknown);
}

bool TMS9918VramSlotSchedule::IsCpuSlot(Schedule schedule,
                                        std::uint16_t slot)
{
    return GetSlot(schedule, slot).activity == Activity::Cpu;
}
