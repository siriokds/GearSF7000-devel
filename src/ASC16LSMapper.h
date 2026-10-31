// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#ifndef ASC16LSMAPPER_H
#define ASC16LSMAPPER_H

#include "ASC16LMapper.h"
#include "ASC16LSFormat.h"

// The cartridge keeps the packed bytes in their physical order. ASC16L's
// register still selects logical 16 KiB pages; only the ROM address wiring
// differs. The separate logical copy in Cartridge is for debugger views and
// CRC checks, never for CPU reads through this mapper.
class ASC16LSMapper final : public ASC16LMapper
{
public:
    explicit ASC16LSMapper(Cartridge* cartridge) : ASC16LMapper(cartridge) { }

    u8 Read(u16 address) override
    {
        if (address >= 0xC000)
            return 0xFF;
        if (m_pCartridge->HasASC16LSScrubbedSignature()
            && address >= 0x3ff0 && address < 0x3ff4)
            return static_cast<u8>("AS16"[address - 0x3ff0]);
        const unsigned logical = address < 0x8000
            ? address >> 14 : GetRomBank();
        const unsigned physical = ASC16LSFormat::PhysicalByLogical[logical];
        return m_pCartridge->GetASC16LSEncodedROM()[
            physical * ASC16LSFormat::PageSize + (address & 0x3FFF)];
    }

    // 'A16S': state shape is ASC16L's, but a different ROM layout.
    u32 GetMapperId() const override { return 0x53363141u; }
    DebugDeviceDefinition DescribeDebugDevice() const override
    {
        return DescribeBankedMapper("internal", "sega.internal", "Internal Mapper");
    }
};

#endif
