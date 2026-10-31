/*
 * TMS9918B reference model - TMS9918B VRAM calendars (unit-exact)
 *
 * Copyright 2026 Saverio Russo
 * SPDX-License-Identifier: Apache-2.0
 *
 * A calendar is the list of VRAM cycles of one scan line. Positions are in
 * units (half crystal periods, 46.56 ns) from phase 0 of the TMS99xx line:
 * unit = 2 x phase. TMS99xx slot s starts at unit (8s + 1364) mod 1368.
 *
 *   graphics active window  units 212-1235 (TMS slots 27-154), 32 cells x 32 units
 *   text active window      units 236-1195 (TMS slots 30-149), 40 chars x 24 units
 *   horizontal blanking     units 1236-1367 and 0-211 (344 units)
 *   first visible pixel     unit 252 (graphics, composite) / 276 (text)
 *
 * Extended calendars (see TMS9918BCalendar.cpp for the exact sequence):
 *
 *   Tiles    Graphics1X, Graphics2Fat. Two cells per 64 units:
 *            +0 name|attr (2), +12 sprite Y, +19 pattern row (2),
 *            +31 name|attr (2), +43 CPU, +50 pattern row (2), +62 idle 2.
 *   Bitmap   Bitmap, BitmapQ. Two cells per 64 units:
 *            +0 bitmap bytes of both cells (4), +22 CPU, +29 palette map (2),
 *            +41 sprite Y, +48 CPU, +55 idle 9.
 *   Text40   Text40X, Text40XQ. Two characters per 48 units:
 *            +0 char|colour|char|colour (4), +22 CPU, +29 pattern row,
 *            +36 pattern row, +43 idle 5.
 *   Text64   Text64 (R13 T64). Four 8x8 characters per 64 units from 212:
 *            +0 names of the 4 columns (4), +22 CPU, +29 +36 +43 +50 pattern
 *            rows, +57 CPU. Blanking: cursor sprites 0-1 and CPU cycles.
 *   Blanking (Tiles, Bitmap): CPU, then 4 groups of two sprites
 *            [SAT X|name|colour (3) a, b; CPU; left a; right a; left b; right b; CPU],
 *            then 4 CPU cycles; 8 sprites per line.
 *
 * Sprite Y scan: one read per cell pair, i.e. 16 per line. Sprites 0-15 are
 * scanned on even lines and 16-31 on odd lines; the selection made over a pair
 * of lines is used for the next two lines (visibility tested on both), so all
 * 32 sprites are scanned every two lines. The channel limit (8) applies to the
 * union of the two lines.
 */

#ifndef TMS9918B_CALENDAR_H
#define TMS9918B_CALENDAR_H

#include <cstdint>
#include <string>
#include <vector>

#include "TMS9918BTiming.h"
#include "TMS99xxVramSlotSchedule.h"

namespace TMS9918BModel
{
    enum class Kind : std::uint8_t
    {
        Idle,
        Cpu,
        TmsSlot,          // TMS9918A slot: 'tmsActivity' tells what it fetches
        NameAttr,         // 2 bytes: name, attribute; index = cell
        PatternRow,       // 2 bytes: stratum 0, stratum 1 (tiles) ; index = cell
        BitmapQuad,       // 4 bytes: cell pair; index = first cell
        PaletteMapPair,   // 2 bytes: 8x8 area palettes; index = first cell
        SpriteScanY,      // 1 byte: SAT +0; index = cell pair (sprite = pair or 16 + pair)
        SpriteXNameColour,// 3 bytes: SAT +1..+3; index = channel
        SpriteLeft,       // 1 byte; index = channel
        SpriteRight,      // 1 byte; index = channel
        TextCharColour,   // 4 bytes: char, colour, char, colour; index = first column
        TextPattern,      // 1 byte; index = column
        TextNameQuad      // 4 bytes: Text64 names of 4 columns; index = first column
    };

    struct Cycle
    {
        std::uint16_t start;     // RAS falling edge, units (may exceed 1367 inside a wrapped window)
        std::uint8_t bytes;
        Kind kind;
        std::uint8_t index;
        TMS99xxVramSlotSchedule::Activity tmsActivity = TMS99xxVramSlotSchedule::Activity::Unknown;

        std::uint16_t Length() const { return CycleUnits(bytes); }
        std::uint16_t End() const { return static_cast<std::uint16_t>(start + Length()); }
        std::uint16_t Sample(std::uint8_t byteIndex) const { return static_cast<std::uint16_t>(start + SampleOffset(byteIndex)); }
        std::uint16_t Unit() const { return static_cast<std::uint16_t>(start % UnitsPerLine); }
    };

    enum class CalendarId : std::uint8_t
    {
        TmsRefresh, TmsGraphics, TmsText, TmsMulticolor,
        Tiles, Bitmap, Text40, Text64
    };

    struct Calendar
    {
        CalendarId id;
        const char* name;
        std::uint16_t activeFirst;   // units, request classification
        std::uint16_t activeEnd;     // exclusive
        std::vector<Cycle> cycles;   // ordered by start
    };

    constexpr std::uint16_t GraphicsWindowStart = 212;
    constexpr std::uint16_t GraphicsWindowEnd   = 1236;
    constexpr std::uint16_t TextWindowStart     = 236;
    constexpr std::uint16_t TextWindowEnd       = 1196;
    constexpr std::uint16_t GraphicsFirstPixel  = 252;   // composite; component output is 260
    constexpr std::uint16_t TextFirstPixel      = 276;

    constexpr std::uint16_t SlotStartUnit(std::uint16_t slot)
    {
        return static_cast<std::uint16_t>((8u * slot + 1364u) % UnitsPerLine);
    }

    const Calendar& GetCalendar(CalendarId id);
    std::vector<CalendarId> AllCalendars();
    std::string KindName(Kind kind);
    std::string CycleLabel(const Cycle& cycle);
}

#endif // TMS9918B_CALENDAR_H
