/*
 * TMS9918B reference model - TMS9918B VRAM calendars (unit-exact)
 *
 * Copyright 2026 Saverio Russo
 * SPDX-License-Identifier: Apache-2.0
 */

#include "TMS9918BCalendar.h"

#include <algorithm>
#include <cstdio>

namespace TMS9918BModel
{
    namespace
    {
        using S = TMS99xxVramSlotSchedule;

        void Add(std::vector<Cycle>& v, std::uint16_t start, std::uint8_t bytes, Kind kind, std::uint8_t index = 0)
        {
            v.push_back({start, bytes, kind, index});
        }

        Calendar BuildTms(CalendarId id, S::Schedule schedule, const char* name)
        {
            Calendar c{id, name, 0, 0, {}};
            if (schedule == S::Schedule::Text) { c.activeFirst = TextWindowStart; c.activeEnd = TextWindowEnd; }
            else if (schedule != S::Schedule::Refresh) { c.activeFirst = GraphicsWindowStart; c.activeEnd = GraphicsWindowEnd; }
            for (std::uint16_t s = 0; s < 171; ++s)
            {
                const S::Slot slot = S::GetSlot(schedule, s);
                Cycle cy{SlotStartUnit(s), 1, slot.activity == S::Activity::Cpu ? Kind::Cpu : Kind::TmsSlot,
                         slot.index, slot.activity};
                c.cycles.push_back(cy);
            }
            std::sort(c.cycles.begin(), c.cycles.end(), [](const Cycle& a, const Cycle& b) { return a.start < b.start; });
            return c;
        }

        // Horizontal blanking of the tile and bitmap calendars: 344 units from 1236.
        void AddSpriteBlanking(std::vector<Cycle>& v)
        {
            std::uint16_t u = GraphicsWindowEnd;
            Add(v, u, 1, Kind::Cpu); u += 7;
            for (std::uint8_t g = 0; g < 4; ++g)
            {
                const std::uint8_t a = static_cast<std::uint8_t>(2 * g), b = static_cast<std::uint8_t>(2 * g + 1);
                Add(v, u, 3, Kind::SpriteXNameColour, a); u += 17;
                Add(v, u, 3, Kind::SpriteXNameColour, b); u += 17;
                Add(v, u, 1, Kind::Cpu);                  u += 7;
                Add(v, u, 1, Kind::SpriteLeft, a);        u += 7;
                Add(v, u, 1, Kind::SpriteRight, a);       u += 7;
                Add(v, u, 1, Kind::SpriteLeft, b);        u += 7;
                Add(v, u, 1, Kind::SpriteRight, b);       u += 7;
                Add(v, u, 1, Kind::Cpu);                  u += 7;
            }
            for (int i = 0; i < 4; ++i) { Add(v, u, 1, Kind::Cpu); u += 7; }
            // u == 1575; units 1575-1579 (207-211 of the next line) stay idle.
        }

        Calendar BuildTiles()
        {
            Calendar c{CalendarId::Tiles, "Tiles (Graphics1X, Graphics2Fat)", GraphicsWindowStart, GraphicsWindowEnd, {}};
            for (std::uint8_t p = 0; p < 16; ++p)
            {
                const std::uint16_t b = static_cast<std::uint16_t>(GraphicsWindowStart + 64u * p);
                const std::uint8_t even = static_cast<std::uint8_t>(2 * p), odd = static_cast<std::uint8_t>(2 * p + 1);
                Add(c.cycles, b + 0, 2, Kind::NameAttr, even);
                Add(c.cycles, b + 12, 1, Kind::SpriteScanY, p);
                Add(c.cycles, b + 19, 2, Kind::PatternRow, even);
                Add(c.cycles, b + 31, 2, Kind::NameAttr, odd);
                Add(c.cycles, b + 43, 1, Kind::Cpu);
                Add(c.cycles, b + 50, 2, Kind::PatternRow, odd);
            }
            AddSpriteBlanking(c.cycles);
            return c;
        }

        Calendar BuildBitmap()
        {
            Calendar c{CalendarId::Bitmap, "Bitmap (Bitmap, BitmapQ)", GraphicsWindowStart, GraphicsWindowEnd, {}};
            for (std::uint8_t p = 0; p < 16; ++p)
            {
                const std::uint16_t b = static_cast<std::uint16_t>(GraphicsWindowStart + 64u * p);
                const std::uint8_t first = static_cast<std::uint8_t>(2 * p);
                Add(c.cycles, b + 0, 4, Kind::BitmapQuad, first);
                Add(c.cycles, b + 22, 1, Kind::Cpu);
                Add(c.cycles, b + 29, 2, Kind::PaletteMapPair, first);
                Add(c.cycles, b + 41, 1, Kind::SpriteScanY, p);
                Add(c.cycles, b + 48, 1, Kind::Cpu);
            }
            AddSpriteBlanking(c.cycles);
            return c;
        }

        Calendar BuildText40()
        {
            Calendar c{CalendarId::Text40, "Text40 (Text40X, Text40XQ)", TextWindowStart, TextWindowEnd, {}};
            for (std::uint8_t q = 0; q < 20; ++q)
            {
                const std::uint16_t b = static_cast<std::uint16_t>(TextWindowStart + 48u * q);
                const std::uint8_t first = static_cast<std::uint8_t>(2 * q);
                Add(c.cycles, b + 0, 4, Kind::TextCharColour, first);
                Add(c.cycles, b + 22, 1, Kind::Cpu);
                Add(c.cycles, b + 29, 1, Kind::TextPattern, first);
                Add(c.cycles, b + 36, 1, Kind::TextPattern, static_cast<std::uint8_t>(first + 1));
            }
            // Blanking region: units 1196-1603 (408 units). Cursor sprites 0 and 1.
            std::uint16_t u = TextWindowEnd;
            for (int i = 0; i < 8; ++i) { Add(c.cycles, u, 1, Kind::Cpu); u += 7; }
            Add(c.cycles, u, 1, Kind::SpriteScanY, 0);       u += 7;
            Add(c.cycles, u, 1, Kind::Cpu);                  u += 7;
            Add(c.cycles, u, 1, Kind::SpriteScanY, 1);       u += 7;
            Add(c.cycles, u, 3, Kind::SpriteXNameColour, 0); u += 17;
            Add(c.cycles, u, 1, Kind::Cpu);                  u += 7;
            Add(c.cycles, u, 3, Kind::SpriteXNameColour, 1); u += 17;
            Add(c.cycles, u, 1, Kind::SpriteLeft, 0);        u += 7;
            Add(c.cycles, u, 1, Kind::Cpu);                  u += 7;
            Add(c.cycles, u, 1, Kind::SpriteRight, 0);       u += 7;
            Add(c.cycles, u, 1, Kind::SpriteLeft, 1);        u += 7;
            Add(c.cycles, u, 1, Kind::Cpu);                  u += 7;
            Add(c.cycles, u, 1, Kind::SpriteRight, 1);       u += 7;
            while (u + 7 <= TextWindowStart + UnitsPerLine) { Add(c.cycles, u, 1, Kind::Cpu); u += 7; }
            return c;
        }

        // Cursor sprites 0-1 and CPU cycles from 'start' to 'end' (exclusive).
        void AddCursorBlanking(std::vector<Cycle>& v, std::uint16_t start, std::uint16_t end, int leadingCpu)
        {
            std::uint16_t u = start;
            for (int i = 0; i < leadingCpu; ++i) { Add(v, u, 1, Kind::Cpu); u += 7; }
            Add(v, u, 1, Kind::SpriteScanY, 0);       u += 7;
            Add(v, u, 1, Kind::Cpu);                  u += 7;
            Add(v, u, 1, Kind::SpriteScanY, 1);       u += 7;
            Add(v, u, 3, Kind::SpriteXNameColour, 0); u += 17;
            Add(v, u, 1, Kind::Cpu);                  u += 7;
            Add(v, u, 3, Kind::SpriteXNameColour, 1); u += 17;
            Add(v, u, 1, Kind::SpriteLeft, 0);        u += 7;
            Add(v, u, 1, Kind::Cpu);                  u += 7;
            Add(v, u, 1, Kind::SpriteRight, 0);       u += 7;
            Add(v, u, 1, Kind::SpriteLeft, 1);        u += 7;
            Add(v, u, 1, Kind::Cpu);                  u += 7;
            Add(v, u, 1, Kind::SpriteRight, 1);       u += 7;
            while (u + 7 <= end) { Add(v, u, 1, Kind::Cpu); u += 7; }
        }

        Calendar BuildText64()
        {
            Calendar c{CalendarId::Text64, "Text64 (Text64, Text64Q)", GraphicsWindowStart, GraphicsWindowEnd, {}};
            for (std::uint8_t g = 0; g < 16; ++g)
            {
                const std::uint16_t b = static_cast<std::uint16_t>(GraphicsWindowStart + 64u * g);
                const std::uint8_t first = static_cast<std::uint8_t>(4 * g);
                Add(c.cycles, b + 0, 4, Kind::TextNameQuad, first);
                Add(c.cycles, b + 22, 1, Kind::Cpu);
                for (std::uint8_t k = 0; k < 4; ++k)
                    Add(c.cycles, static_cast<std::uint16_t>(b + 29 + 7 * k), 1, Kind::TextPattern, static_cast<std::uint8_t>(first + k));
                Add(c.cycles, b + 57, 1, Kind::Cpu);
            }
            AddCursorBlanking(c.cycles, GraphicsWindowEnd, GraphicsWindowStart + UnitsPerLine, 3);
            return c;
        }
    }

    const Calendar& GetCalendar(CalendarId id)
    {
        static const Calendar tmsRefresh = BuildTms(CalendarId::TmsRefresh, S::Schedule::Refresh, "TMS Refresh");
        static const Calendar tmsGraphics = BuildTms(CalendarId::TmsGraphics, S::Schedule::Graphics, "TMS Graphics");
        static const Calendar tmsText = BuildTms(CalendarId::TmsText, S::Schedule::Text, "TMS Text");
        static const Calendar tmsMulticolor = BuildTms(CalendarId::TmsMulticolor, S::Schedule::Multicolor, "TMS Multicolor");
        static const Calendar tiles = BuildTiles();
        static const Calendar bitmap = BuildBitmap();
        static const Calendar text40 = BuildText40();
        static const Calendar text64 = BuildText64();
        switch (id)
        {
            case CalendarId::TmsRefresh:    return tmsRefresh;
            case CalendarId::TmsGraphics:   return tmsGraphics;
            case CalendarId::TmsText:       return tmsText;
            case CalendarId::TmsMulticolor: return tmsMulticolor;
            case CalendarId::Tiles:         return tiles;
            case CalendarId::Bitmap:        return bitmap;
            case CalendarId::Text40:        return text40;
            case CalendarId::Text64:        return text64;
        }
        return tiles;
    }

    std::vector<CalendarId> AllCalendars()
    {
        return {CalendarId::TmsRefresh, CalendarId::TmsGraphics, CalendarId::TmsText, CalendarId::TmsMulticolor,
                CalendarId::Tiles, CalendarId::Bitmap, CalendarId::Text40, CalendarId::Text64};
    }

    std::string KindName(Kind kind)
    {
        switch (kind)
        {
            case Kind::Idle: return "idle";
            case Kind::Cpu: return "CPU";
            case Kind::TmsSlot: return "TMS";
            case Kind::NameAttr: return "name|attr";
            case Kind::PatternRow: return "pattern row";
            case Kind::BitmapQuad: return "bitmap x4";
            case Kind::PaletteMapPair: return "palette map x2";
            case Kind::SpriteScanY: return "sprite Y";
            case Kind::SpriteXNameColour: return "SAT X|name|colour";
            case Kind::SpriteLeft: return "sprite left half";
            case Kind::SpriteRight: return "sprite right half";
            case Kind::TextCharColour: return "char|colour x2";
            case Kind::TextPattern: return "text pattern row";
            case Kind::TextNameQuad: return "names x4";
        }
        return "?";
    }

    std::string CycleLabel(const Cycle& c)
    {
        char buf[64];
        switch (c.kind)
        {
            case Kind::Cpu: return "CPU";
            case Kind::SpriteScanY: std::snprintf(buf, sizeof buf, "sprite Y (even line: %u, odd line: %u)", c.index, c.index + 16); return buf;
            case Kind::NameAttr: case Kind::PatternRow: std::snprintf(buf, sizeof buf, "%s cell %u", KindName(c.kind).c_str(), c.index); return buf;
            case Kind::BitmapQuad: case Kind::PaletteMapPair: std::snprintf(buf, sizeof buf, "%s cells %u-%u", KindName(c.kind).c_str(), c.index, c.index + 1); return buf;
            case Kind::TextNameQuad: std::snprintf(buf, sizeof buf, "names columns %u-%u", c.index, c.index + 3); return buf;
            case Kind::TextCharColour: std::snprintf(buf, sizeof buf, "%s columns %u-%u", KindName(c.kind).c_str(), c.index, c.index + 1); return buf;
            case Kind::TextPattern: std::snprintf(buf, sizeof buf, "%s column %u", KindName(c.kind).c_str(), c.index); return buf;
            case Kind::SpriteXNameColour: case Kind::SpriteLeft: case Kind::SpriteRight: std::snprintf(buf, sizeof buf, "%s channel %u", KindName(c.kind).c_str(), c.index); return buf;
            case Kind::TmsSlot: std::snprintf(buf, sizeof buf, "TMS activity %u index %u", static_cast<unsigned>(c.tmsActivity), c.index); return buf;
            default: return KindName(c.kind);
        }
    }
}
