/*
 * GearSF7000 - TMS99xx table address generator
 *
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One address generator for every display mode, including the undocumented
 * ones. On the chip M3 drives a single banking circuit shared by Graphics II,
 * Text 1Q and Multicolor Q, so it is modelled once here instead of being
 * repeated in the fetch path, the legacy renderer and the debug views.
 *
 * Reference: openMSX VDP::updateNameBase/updateColorBase/updatePatternBase
 * and CharacterConverter (window semantics: address = baseMask & index).
 * test_tms99xx_table_address.cpp checks these closed forms exhaustively
 * against a literal transcription of that code.
 *
 * mode = (M2 << 2) | (M3 << 1) | M1, i.e. TMS99xx::m_iMode.
 */

#ifndef TMS99XX_TABLE_ADDRESS_H
#define TMS99XX_TABLE_ADDRESS_H

#include <cstdint>

namespace TMS99xxTableAddress
{
    constexpr std::uint16_t VramMask = 0x3FFF;

    // TMS9918A/28A/29A leak R3 bits 0-4 into the Graphics II pattern mask
    // (openMSX VM_PALCOL_MIRRORING, also MAME). The TMS9118/9128 do not.
    enum class Chip : std::uint8_t
    {
        TMS99x8A,   // TMS9918A, TMS9928A, TMS9929A
        TMS91x8     // TMS9118, TMS9128, TMS9129
    };

    constexpr bool IsTextMode(int mode)  { return (mode & 1) != 0; }       // 1, 3, 5, 7
    constexpr bool IsBarMode(int mode)   { return (mode & 5) == 5; }       // 5, 7
    constexpr bool UsesColourTable(int mode) { return mode == 0 || mode == 2; }

    // Physical screen third: 0, 1 or 2 for lines 0..191. In every M3 mode
    // (Graphics II, Text1Q and Multicolor1Q), R4.0 independently enables
    // address bit A11 for the middle third and R4.1 independently enables
    // A12 for the bottom third. They are two bank enables, not a binary bank
    // number: 00 -> 0/0/0, 01 -> 0/1/0, 10 -> 0/0/2, 11 -> 0/1/2.
    constexpr std::uint16_t Third(std::uint16_t line) { return line >> 6; }

    constexpr std::uint16_t PatternRow(int mode, std::uint16_t line)
    {
        // Multicolor (4, 6) steps its pattern byte every four lines.
        return ((mode & 4) != 0 && !IsTextMode(mode)) ? ((line >> 2) & 7u)
                                                      : (line & 7u);
    }

    // ----- name table -------------------------------------------------------
    // Same for every mode: R2 << 10 plus the cell index. openMSX's
    // "(index + 0xC00) | ~0 << 12" in the text renderers evaluates to exactly
    // this for the 960 text cells; it is not an offset.
    constexpr std::uint16_t Name(int mode, std::uint8_t r2, std::uint16_t line,
                                 std::uint8_t column)
    {
        const std::uint16_t columns = IsTextMode(mode) ? 40 : 32;
        return static_cast<std::uint16_t>(
            (((r2 & 0x0Fu) << 10) + (line >> 3) * columns + column) & VramMask);
    }

    // ----- pattern table ----------------------------------------------------
    constexpr std::uint16_t Pattern(int mode, std::uint8_t r3, std::uint8_t r4,
                                    std::uint16_t line, std::uint8_t name,
                                    Chip chip = Chip::TMS99x8A)
    {
        const std::uint16_t row = PatternRow(mode, line);
        if ((mode & 2) == 0)
        {
            // Graphics I, Text 1, Multicolor: whole three-bit R4 base.
            return static_cast<std::uint16_t>(
                (((r4 & 7u) << 11) + (name << 3) + row) & VramMask);
        }

        // M3 banking (2, 3, 6): R4 bit 2 selects the 8 KiB base. R4 bits 0
        // and 1 independently enable the middle and bottom screen banks.
        // Graphics II on the TMS99x8A also masks name bits 3-7 with R3 bits
        // 0-4; Text1Q and Multicolor1Q retain the complete pattern name.
        const std::uint16_t index = static_cast<std::uint16_t>((Third(line) << 8) | name);
        const bool leak = mode == 2 && chip == Chip::TMS99x8A;
        const std::uint16_t lowMask = leak
            ? static_cast<std::uint16_t>(((r3 & 0x1Fu) << 3) | 7u) : 0xFFu;
        const std::uint16_t enabledThirdAddressBits =
            static_cast<std::uint16_t>((r4 & 3u) << 8);
        const std::uint16_t mask =
            static_cast<std::uint16_t>(enabledThirdAddressBits | lowMask);
        return static_cast<std::uint16_t>(
            (((r4 & 4u) << 11) + ((index & mask) << 3) + row) & VramMask);
    }

    // ----- colour table (modes 0 and 2 only) --------------------------------
    constexpr std::uint16_t Colour(int mode, std::uint8_t r3, std::uint16_t line,
                                   std::uint8_t name)
    {
        if (mode == 0)
            return static_cast<std::uint16_t>(((r3 << 6) + (name >> 3)) & VramMask);

        const std::uint16_t index = static_cast<std::uint16_t>((Third(line) << 8) | name);
        const std::uint16_t mask = static_cast<std::uint16_t>(((r3 & 0x7Fu) << 3) | 7u);
        return static_cast<std::uint16_t>(
            (((r3 & 0x80u) << 6) + ((index & mask) << 3) + (line & 7u)) & VramMask);
    }
}

// The configuration most Graphics II software uses to share one 256-pattern
// set across the three thirds: R3 = 0x9F, R4 = 0x00.
static_assert(TMS99xxTableAddress::Pattern(2, 0x9F, 0x00, 0, 0x41) ==
              TMS99xxTableAddress::Pattern(2, 0x9F, 0x00, 190, 0x41) - 6);
// Text 1Q honours the same R4 mask: thirds share one set when R4 bits 0-1 = 00.
static_assert(TMS99xxTableAddress::Pattern(3, 0x00, 0x04, 3, 0x41) ==
              TMS99xxTableAddress::Pattern(3, 0x00, 0x04, 131, 0x41));

#endif // TMS99XX_TABLE_ADDRESS_H
