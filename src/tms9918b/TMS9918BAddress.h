/*
 * TMS9918B reference model - TMS9918B VRAM addresses (byte layout, 16 KB)
 *
 * Copyright 2026 Saverio Russo
 * SPDX-License-Identifier: Apache-2.0
 *
 * The TMS9918B keeps 16 KB of 8-bit VRAM. Data read together is stored in
 * adjacent bytes and fetched with one page-mode cycle. Page mode keeps the
 * DRAM row: the multiplexer puts A0-A6 (high) on RAS and A7-A13 (low) on CAS,
 * so a burst of n bytes must start at a multiple of n and stay inside a
 * 128-byte page. Every generator below starts bursts at multiples of 2 or 4
 * from a base that is a multiple of 128, which guarantees it.
 */

#ifndef TMS9918B_ADDRESS_H
#define TMS9918B_ADDRESS_H

#include <cstdint>

namespace TMS9918BModel
{
    constexpr std::uint16_t VramMask = 0x3FFF;
    constexpr std::uint16_t PageBytes = 128;

    constexpr bool BurstInsidePage(std::uint16_t address, std::uint8_t bytes)
    {
        return (address % bytes) == 0 && ((address & (PageBytes - 1u)) + bytes) <= PageBytes;
    }

    constexpr std::uint16_t Wrap(unsigned v) { return static_cast<std::uint16_t>(v & VramMask); }

    // Graphics1X / Graphics2Fat name table: name, attribute per cell.
    constexpr std::uint16_t NameAttrAddress(std::uint8_t r2, std::uint16_t row, std::uint16_t column)
    {
        return Wrap(((r2 & 0x0Fu) << 10) + 2u * (row * 32u + column));
    }

    // Graphics1X pattern row: 16 bytes per pattern (8 rows x stratum 0, 1); name9 = NAME8 | name.
    constexpr std::uint16_t Graphics1XPatternAddress(std::uint8_t r4, std::uint16_t name9, std::uint16_t row)
    {
        return Wrap(((r4 & 7u) << 11) + 16u * (name9 & 0x1FFu) + 2u * (row & 7u));
    }

    // Graphics2Fat pattern row with M3 third banking (R4 bit 2 base, bits 0-1 mask).
    constexpr std::uint16_t Graphics2FatPatternAddress(std::uint8_t r4, std::uint16_t line, std::uint8_t name)
    {
        const std::uint16_t third = static_cast<std::uint16_t>(line >> 6);
        const std::uint16_t index = static_cast<std::uint16_t>((third << 8) | name);
        const std::uint16_t mask = static_cast<std::uint16_t>(((r4 & 3u) << 8) | 0xFFu);
        return Wrap(((r4 & 4u) << 11) + 16u * (index & mask) + 2u * (line & 7u));
    }

    // Bitmap: 64 bytes per line (2 bytes per 8 pixels); the calendar reads 4 bytes per cell pair.
    constexpr std::uint16_t BitmapQuadAddress(std::uint8_t r4, std::uint16_t line, std::uint16_t cellPair)
    {
        return Wrap(((r4 & 7u) << 11) + 64u * line + 4u * cellPair);
    }

    // BitmapQ: 4096-byte blocks per third (64 lines x 64 bytes).
    constexpr std::uint16_t BitmapQQuadAddress(std::uint8_t r4, std::uint16_t line, std::uint16_t cellPair)
    {
        const std::uint16_t third = static_cast<std::uint16_t>(line >> 6);
        const std::uint16_t block = third == 0 ? 0 : (((r4 >> (third - 1)) & 1u) ? third : 0);
        return Wrap(((r4 & 4u) << 11) + 0x1000u * block + 64u * (line & 63u) + 4u * cellPair);
    }

    // Bitmap palette map: one byte per 8x8 area; the calendar reads 2 per cell pair.
    constexpr std::uint16_t PaletteMapPairAddress(std::uint8_t r2, std::uint16_t row, std::uint16_t cellPair)
    {
        return Wrap(((r2 & 0x0Fu) << 10) + 32u * row + 2u * cellPair);
    }

    // Text40X: character, colour per column; the calendar reads 4 per column pair.
    constexpr std::uint16_t TextCharColourAddress(std::uint8_t r2, std::uint16_t row, std::uint16_t columnPair)
    {
        return Wrap(((r2 & 0x0Fu) << 10) + 4u * (row * 20u + columnPair));
    }

    // Text64: one name per column (64 per row); the calendar reads 4 per group.
    constexpr std::uint16_t Text64NameQuadAddress(std::uint8_t r2, std::uint16_t row, std::uint16_t group)
    {
        return Wrap(((r2 & 0x0Fu) << 10) + 64u * row + 4u * group);
    }

    // Text pattern byte: TMS Text layout (single read).
    constexpr std::uint16_t TextPatternAddress(std::uint8_t r4, std::uint8_t name, std::uint16_t row)
    {
        return Wrap(((r4 & 7u) << 11) + 8u * name + (row & 7u));
    }

    // SAT: TMS layout; the fetch reads X, name, colour (+1..+3).
    constexpr std::uint16_t SatXNameColourAddress(std::uint8_t r5, std::uint8_t sprite)
    {
        return Wrap(((r5 & 0x7Fu) << 7) + 4u * sprite + 1u);
    }

    constexpr std::uint16_t SatYAddress(std::uint8_t r5, std::uint8_t sprite)
    {
        return Wrap(((r5 & 0x7Fu) << 7) + 4u * sprite);
    }

    // Sprite pattern byte; BANK (colour byte bit 5) selects the next 2048-byte table.
    constexpr std::uint16_t SpritePatternAddress(std::uint8_t r6, bool bank, std::uint8_t name, std::uint16_t row, bool rightHalf)
    {
        return Wrap(((r6 & 7u) << 11) + (bank ? 0x800u : 0u) + 8u * name + (rightHalf ? 16u : 0u) + row);
    }
}

namespace TMS9918BAddressChecks
{
    using namespace TMS9918BModel;
    static_assert(BurstInsidePage(SatXNameColourAddress(0x76, 31) - 1u, 4));
    static_assert(BurstInsidePage(NameAttrAddress(0x0E, 23, 31), 2));
    static_assert(BurstInsidePage(BitmapQuadAddress(0x00, 191, 15), 4));
    static_assert(Graphics2FatPatternAddress(0x03, 191, 0xFF) == 0x2FFE);
}

#endif // TMS9918B_ADDRESS_H
