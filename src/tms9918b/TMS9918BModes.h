/*
 * TMS9918B reference model - TMS9918B display mode decode
 *
 * Copyright 2026 Saverio Russo
 * SPDX-License-Identifier: Apache-2.0
 *
 * Every TMS9918A mode, documented or not, plus the TMS9918B extended modes,
 * with the VRAM calendar each one uses on active lines.
 *
 * Bit weights (TI numbers bits with D0 = MSB):
 *   M1  = R1 10h (D3)   M2 = R1 08h (D4)   M3 = R0 02h (D6)
 *   XE  = R11 01h (D7)   MX = R11 02h (D6)   T64 = R12 01h (D7)
 *   m   = M2 x 4 + M3 x 2 + M1, i.e. TMS99xx::m_iMode
 *
 * With XE = 0 or MX = 0 the mode is the TMS9918A one, including Text 1Q (m 3),
 * Multicolor Q (m 6) and the bar modes (m 5, 7); calendars follow GearSF7000's
 * TMS99xx::GetVideoVRAMSchedule (1, 3, 5, 7 Text; 4, 6 Multicolor; 0, 2
 * Graphics). With XE = MX = 1 the bar modes stay bar modes.
 * TMS mode table addresses come from TMS99xxTableAddress.h.
 */

#ifndef TMS9918B_MODES_H
#define TMS9918B_MODES_H

#include <cstdint>

#include "TMS9918BCalendar.h"
#include "TMS99xxTableAddress.h"

namespace TMS9918BModel
{
    constexpr std::uint8_t R0_M3  = 0x02;
    constexpr std::uint8_t R1_BL  = 0x40;
    constexpr std::uint8_t R1_M1  = 0x10;
    constexpr std::uint8_t R1_M2  = 0x08;
    constexpr std::uint8_t R11_XE  = 0x01;
    constexpr std::uint8_t R11_MX  = 0x02;
    constexpr std::uint8_t R11_IE1 = 0x80;
    constexpr std::uint8_t R12_T64 = 0x01;

    enum class VideoMode : std::uint8_t
    {
        // TMS9918A, m = 0..7
        Graphics1, Text1, Graphics2, Text1Q, Multicolor, Bars5, MulticolorQ, Bars7,
        // TMS9918B extended, m = 0..7 with XE = MX = 1
        Graphics1X, Text40X, Graphics2Fat, Text40XQ, Bitmap, BarsX5, BitmapQ, BarsX7,
        // T64 = 1 on the extended text modes
        Text64, Text64Q
    };

    struct ModeInfo
    {
        const char* name;
        CalendarId calendar;          // active lines with the display enabled
        bool extended;
        bool documented;              // documented by TI (TMS modes) or in the TMS9918B Data Manual
        std::uint8_t spritesPerLine;  // 0 = none, 2 = cursor sprites 0-1
        bool linePairSpriteScan;      // extended graphics modes select sprites over line pairs
        bool usesPalette;
        bool hScroll, vScroll;
        std::uint16_t pixelsPerLine;
    };

    constexpr int ModeNumber(std::uint8_t r0, std::uint8_t r1)
    {
        return ((r1 & R1_M2) ? 4 : 0) | ((r0 & R0_M3) ? 2 : 0) | ((r1 & R1_M1) ? 1 : 0);
    }

    constexpr bool ExtendedModesEnabled(std::uint8_t r11)
    {
        return (r11 & (R11_XE | R11_MX)) == (R11_XE | R11_MX);
    }

    constexpr VideoMode DecodeMode(std::uint8_t r0, std::uint8_t r1, std::uint8_t r11, std::uint8_t r12)
    {
        const int m = ModeNumber(r0, r1);
        if (!ExtendedModesEnabled(r11))
            return static_cast<VideoMode>(m);
        if (m == 1) return (r12 & R12_T64) ? VideoMode::Text64 : VideoMode::Text40X;
        if (m == 3) return (r12 & R12_T64) ? VideoMode::Text64Q : VideoMode::Text40XQ;
        return static_cast<VideoMode>(static_cast<int>(VideoMode::Graphics1X) + m);
    }

    constexpr ModeInfo Info(VideoMode mode)
    {
        switch (mode)
        {
            case VideoMode::Graphics1:    return {"Graphics I", CalendarId::TmsGraphics, false, true, 4, false, false, false, false, 256};
            case VideoMode::Text1:        return {"Text", CalendarId::TmsText, false, true, 0, false, false, false, false, 256};
            case VideoMode::Graphics2:    return {"Graphics II", CalendarId::TmsGraphics, false, true, 4, false, false, false, false, 256};
            case VideoMode::Text1Q:       return {"Text 1Q (undocumented)", CalendarId::TmsText, false, false, 0, false, false, false, false, 256};
            case VideoMode::Multicolor:   return {"Multicolor", CalendarId::TmsMulticolor, false, true, 4, false, false, false, false, 256};
            case VideoMode::Bars5:        return {"Bars (undocumented, M1+M2)", CalendarId::TmsText, false, false, 0, false, false, false, false, 256};
            case VideoMode::MulticolorQ:  return {"Multicolor Q (undocumented)", CalendarId::TmsMulticolor, false, false, 4, false, false, false, false, 256};
            case VideoMode::Bars7:        return {"Bars (undocumented, M1+M2+M3)", CalendarId::TmsText, false, false, 0, false, false, false, false, 256};
            case VideoMode::Graphics1X:   return {"Graphics1X", CalendarId::Tiles, true, true, 8, true, true, true, true, 256};
            case VideoMode::Text40X:      return {"Text40X", CalendarId::Text40, true, true, 2, false, false, false, true, 256};
            case VideoMode::Graphics2Fat: return {"Graphics2Fat", CalendarId::Tiles, true, true, 8, true, false, true, true, 256};
            case VideoMode::Text40XQ:     return {"Text40XQ", CalendarId::Text40, true, true, 2, false, false, false, true, 256};
            case VideoMode::Bitmap:       return {"Bitmap", CalendarId::Bitmap, true, true, 8, true, true, true, true, 256};
            case VideoMode::BarsX5:       return {"Bars (extended, M1+M2)", CalendarId::TmsText, true, false, 0, false, false, false, false, 256};
            case VideoMode::BitmapQ:      return {"BitmapQ", CalendarId::Bitmap, true, true, 8, true, true, true, true, 256};
            case VideoMode::BarsX7:       return {"Bars (extended, M1+M2+M3)", CalendarId::TmsText, true, false, 0, false, false, false, false, 256};
            case VideoMode::Text64:       return {"Text64", CalendarId::Text64, true, true, 2, false, false, false, true, 512};
            case VideoMode::Text64Q:      return {"Text64Q", CalendarId::Text64, true, true, 2, false, false, false, true, 512};
        }
        return {"?", CalendarId::TmsRefresh, false, false, 0, false, false, false, false, 256};
    }

    // Calendar of a line: blanked display and non-active lines use Refresh.
    // The two lines before line 0 run the active calendar with the picture
    // blanked so that the line-pair sprite scan is ready for lines 0 and 1.
    constexpr CalendarId LineCalendar(VideoMode mode, bool displayEnabled, int line)
    {
        const bool scanLine = line >= -2 && line < 192;
        if (!displayEnabled || !scanLine)
            return CalendarId::TmsRefresh;
        return Info(mode).calendar;
    }

    // TMS mode number to use with TMS99xxTableAddress (the extended bar modes too).
    constexpr int TmsAddressMode(VideoMode mode)
    {
        const int v = static_cast<int>(mode);
        if (v <= static_cast<int>(VideoMode::Bars7))
            return v;
        if (mode == VideoMode::BarsX5) return 5;
        if (mode == VideoMode::BarsX7) return 7;
        return -1;   // extended table layout: see TMS9918BAddress.h
    }
}

namespace TMS9918BModeChecks
{
    using namespace TMS9918BModel;
    static_assert(DecodeMode(0x02, 0x10, 0x00, 0x00) == VideoMode::Text1Q);
    static_assert(DecodeMode(0x02, 0x08, R11_XE, 0x00) == VideoMode::MulticolorQ);           // XE without MX
    static_assert(DecodeMode(0x00, 0x18, R11_XE | R11_MX, R12_T64) == VideoMode::BarsX5);
    static_assert(DecodeMode(0x02, 0x10, R11_XE | R11_MX, R12_T64) == VideoMode::Text64Q);
    static_assert(Info(VideoMode::MulticolorQ).calendar == CalendarId::TmsMulticolor);
    static_assert(Info(VideoMode::Bars7).calendar == CalendarId::TmsText);
    static_assert(TmsAddressMode(VideoMode::BarsX7) == 7 && TmsAddressMode(VideoMode::Bitmap) == -1);
}

#endif // TMS9918B_MODES_H
