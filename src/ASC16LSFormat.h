// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#ifndef ASC16LSFORMAT_H
#define ASC16LSFORMAT_H

#include <cstdint>

namespace ASC16LSFormat {
inline constexpr int HeaderSize = 16;
inline constexpr int PageSize = 0x4000;
inline constexpr int PageCount = 32;
inline constexpr int MinimumPhysicalPages = 16;
inline constexpr int ROMSize = PageSize * PageCount;
inline constexpr char Magic[8] = {'S','C','L','S','0','0','0','2'};
// Header: magic[8], CRC32 of original ROM[4] LE, RAM KiB[1], logical
// page count[1] (8, 16 or 32), physical page count[1] (16 or 32),
// signature flag[1]. Flag 1 means the source's AS16 bytes at $3FF0 were
// scrubbed from the physical store and must be restored on CPU reads.
// Physical page index for each logical ASC16L page. Pages 0 and 1 are fixed
// in the CPU map, but live at physical positions 9 and 2 in the file.
inline constexpr std::uint8_t PhysicalByLogical[PageCount] =
    {9, 2, 14, 5, 12, 0, 7, 15, 3, 10, 1, 13, 6, 4, 11, 8,
     27, 20, 17, 31, 24, 18, 29, 22, 16, 26, 19, 30, 23, 28, 21, 25};
}

#endif
