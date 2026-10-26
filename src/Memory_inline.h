/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2021  Ignacio Sanchez
 * Copyright (C) 2024-2026  Saverio Russo
 *
 * Derived from Gearcoleco by Ignacio Sanchez. His code is free software: you
 * can redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * The changes and additions by Saverio Russo are free software: you can
 * redistribute them and/or modify them under the terms of the GNU Affero
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 * Section 13 of both licences permits the combination.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License and the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of both licences along with this
 * program.  If not, see http://www.gnu.org/licenses/
 *
 * SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later
 */

#ifndef MEMORY_INLINE_H
#define	MEMORY_INLINE_H

#include "Cartridge.h"

inline u8 Memory::Read(u16 address)
{
    return ReadMapped(address, true);
}

inline u8 Memory::Peek(u16 address)
{
    return ReadMapped(address, false);
}

inline u8 Memory::ReadMapped(u16 address, bool publish)
{
    #ifndef GEARSF7000_DISABLE_DISASSEMBLER
    #define MEMORY_READ_VALUE(expression) ([&]() -> u8 { const u8 debug_value = (expression); if (publish) CheckBreakpoints(address, false, debug_value, true, debug_value, true); return debug_value; }())
    #else
    (void)publish;
    #define MEMORY_READ_VALUE(expression) (expression)
    #endif

    if (m_m5Mapper) return MEMORY_READ_VALUE(m_m5Mapper->Peek(address));
    if (m_msxSlots)
        return MEMORY_READ_VALUE(publish ? m_msxSlots->Read(address) : m_msxSlots->Peek(address));
    if (m_sgm && m_sgm->ResolvesRam(address))
        return MEMORY_READ_VALUE(m_pExternalRam[address]);
    if (m_bSF7000Enabled)
    {
        if (address < 0x4000)
        {
            // IPL is physically 8 KiB and mirrored over the 16 KiB read
            // overlay. Writes still reach the SF-7000 external RAM.
            return MEMORY_READ_VALUE(m_bIPLRomDisabled ? m_pExternalRam[address] : (m_bBiosLoaded ? m_pBios[address & GetBiosMask()] : 0xFF));
        }
        return MEMORY_READ_VALUE(m_pExternalRam[address]);
    }
    // ColecoVision (schematic, Processor sheet): the 74LS138 on A13-A15
    // gives 8 KiB blocks. $0000 is the BIOS; $6000 is 1 KiB of 2114 RAM with
    // only A0-A9 wired, so it repeats eight times up to $7FFF; $8000-$FFFF is
    // the cartridge, each block on its own select, unmapped past the image.
    // $2000-$5FFF is left to the expansion connector.
    if (m_colecoMotherboard)
    {
        switch (address & 0xE000)
        {
            case 0x0000:
                return MEMORY_READ_VALUE(m_bBiosLoaded ? m_pBios[address] : 0xFF);
            case 0x6000:
                return MEMORY_READ_VALUE(m_pInternalRam[BoardRamOffset(address)]);
            case 0x2000:
            case 0x4000:
                return MEMORY_READ_VALUE(0xFF);
            default:
            {
                if (!m_pCartridge->IsReady()) return MEMORY_READ_VALUE(0xFF);
                if (m_pCartridge->GetType() == Cartridge::COLECO_MEGACART)
                    return MEMORY_READ_VALUE(ReadMegaCart(address, publish));
                const int offset = address - 0x8000;
                return MEMORY_READ_VALUE(offset < m_pCartridge->GetROMSize()
                    ? m_pCartridge->GetROM()[offset] : 0xFF);
            }
        }
    }
    // Cartridge-backed boards below this point reach into GetROM(),
    // which Cartridge::Reset() frees and nulls on eject. No cartridge type
    // this dispatches on is ever CartridgeNotSupported while ready, so the
    // one guard here covers all of them: an unmapped bus reads high, the
    // conventional open-bus value, rather than dereferencing a null ROM
    // pointer at the address itself.
    else if (!m_pCartridge->IsReady())
    {
        return MEMORY_READ_VALUE(0xFF);
    }
    else if (address >= 0xC000 && UsesSMSWorkRam())
    {
        return MEMORY_READ_VALUE(m_pInternalRam[BoardRamOffset(address)]);
    }
    else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SMS_SEGA_MAPPER)
    {
        if (address < 0xC000)
            return MEMORY_READ_VALUE(m_pMapper->Read(address));
        return MEMORY_READ_VALUE(m_pInternalRam[BoardRamOffset(address)]);
    }
    // Flat map: no masking, no windows, no banking. All 64 KB are the same
    // RAM the image was loaded into at reset - identical to the SF-7000 branch
    // above once its IPL is switched off.
    else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SC3000_FLAT64K)
    {
        return MEMORY_READ_VALUE(m_pExternalRam[address]);
    }
    else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SC3000_ASC16L
          || m_pCartridge->GetType() == Cartridge::CartridgeTypes::SC3000_ASC16LS)
    {
        if (address < 0xC000)
        {
            // Se siamo nel range della cartuccia (0-48KB), ci pensa il mapper
            return MEMORY_READ_VALUE(m_pMapper->Read(address));
        }
        else
        {
            // The 8 KiB option mirrors twice inside the 16 KiB CPU window.
            const u16 ramAddress = static_cast<u16>(0x4000 +
                (address & (m_pCartridge->GetASC16LRamBytes() - 1)));
            return MEMORY_READ_VALUE(m_pExternalRam[ramAddress]);
        }
    }
    else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SC3000_SURVIVORS_MULTICART
          || m_pCartridge->GetType() == Cartridge::CartridgeTypes::SC3000_SURVIVORS_MEGACART)
    {
        if (address < 0x8000)
            return MEMORY_READ_VALUE(m_pMapper->Read(address));
        return MEMORY_READ_VALUE(m_pExternalRam[address & 0x7FFF]);
    }
    // The 315-5208 pages ROM only (PCB 171-5363-01: mapper and ROM, no
    // SRAM, B3 left alone). $C000-$FFFF is the console's own RAM, whatever
    // the console: 1 KiB on an SG-1000, 2 KiB on an SC-3000, 8 KiB on a
    // Mark III, each mirrored up to $FFFF.
    else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SG1000_SEGA_315_5208)
    {
        if (address < 0xC000)
            return MEMORY_READ_VALUE(m_pMapper->Read(address));
        return MEMORY_READ_VALUE(m_pInternalRam[BoardRamOffset(address)]);
    }
    // Same ROM paging; the Dahjee B answers at $C000 and B3 silences the
    // console's RAM.
    else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SG1000_SEGA_315_5208_DAHJEE_B)
    {
        if (address < 0xC000)
            return MEMORY_READ_VALUE(m_pMapper->Read(address));
        return MEMORY_READ_VALUE(m_pExternalRam[address & 0x1FFF]);
    }
    else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SC3000_2K)
    {
        u8* pRom = m_pCartridge->GetROM();

        switch (address & 0xC000)
        {
            case 0x0000:
            case 0x4000:
                return MEMORY_READ_VALUE(pRom[address]);

            default:
                return MEMORY_READ_VALUE(m_pInternalRam[BoardRamOffset(address)]);
        }
    }
    else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SC3000_32K)
    {
        u8* pRom = m_pCartridge->GetROM();

        switch (address & 0xC000)
        {
            case 0x0000:
            case 0x4000:
                return MEMORY_READ_VALUE(pRom[address]);

            default:
                return MEMORY_READ_VALUE(m_pExternalRam[address & 0x7FFF]);
        }
    }
    else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SC3000_16K_AT_8000)
    {
        u8* pRom = m_pCartridge->GetROM();

        // Two separate memories, both visible at once. The 32 KB cartridge
        // asserts a pin that switches the machine's own 2 KB off; this one
        // does not, so its 16 KB answers at $8000-$BFFF and the internal 2 KB
        // still answers above that, mirrored eight times across $C000-$FFFF
        // because only A0-A10 reach it.
        if (address < 0x8000)
            return MEMORY_READ_VALUE(pRom[address]);
        if (address < 0xC000)
            return MEMORY_READ_VALUE(m_pExternalRam[address & 0x3FFF]);
        return MEMORY_READ_VALUE(m_pInternalRam[BoardRamOffset(address)]);
    }
    else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SG1000_DAHJEE_TYPE_A)
    {
        // The pass-through adapter replaces only $2000-$3FFF with its 8 KiB
        // RAM. The cartridge ROM remains visible on both sides of that hole,
        // and the SG-1000's own 1 KiB RAM remains decoded above $C000.
        if (address >= 0x2000 && address < 0x4000)
            return MEMORY_READ_VALUE(m_pExternalRam[address & 0x1FFF]);
        if (address >= 0xC000)
            return MEMORY_READ_VALUE(m_pInternalRam[BoardRamOffset(address)]);

        u8* pRom = m_pCartridge->GetROM();
        const int romSize = m_pCartridge->GetROMSize();
        return MEMORY_READ_VALUE(pRom[romSize > 0 ? address % romSize : 0]);
    }
    else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SG1000_DAHJEE_TYPE_B)
    {
        // Type B has the Mark III/Master System-style 8 KiB work-RAM decode,
        // but the rest of the machine remains an SG-1000. The physical RAM is
        // mirrored once across the 16 KiB CPU window.
        if (address >= 0xC000)
            return MEMORY_READ_VALUE(m_pExternalRam[address & 0x1FFF]);

        u8* pRom = m_pCartridge->GetROM();
        const int romSize = m_pCartridge->GetROMSize();
        return MEMORY_READ_VALUE(pRom[romSize > 0 ? address % romSize : 0]);
    }
    else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SG1000_CASTLE_8K)
    {
        if (address >= 0xC000)
            return MEMORY_READ_VALUE(m_pInternalRam[BoardRamOffset(address)]);
        if (address >= 0x8000)
            return MEMORY_READ_VALUE(m_pExternalRam[address & 0x1FFF]);

        u8* pRom = m_pCartridge->GetROM();
        const int romSize = m_pCartridge->GetROMSize();
        return MEMORY_READ_VALUE(pRom[romSize > 0 ? address % romSize : 0]);
    }
    else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SG1000_OTHELLO_2K)
    {
        if (address >= 0x8000 && address < 0xA000)
            return MEMORY_READ_VALUE(m_pExternalRam[address & 0x07FF]);
        if (address >= 0xC000)
            return MEMORY_READ_VALUE(m_pInternalRam[BoardRamOffset(address)]);

        u8* pRom = m_pCartridge->GetROM();
        const int romSize = m_pCartridge->GetROMSize();
        return MEMORY_READ_VALUE(pRom[romSize > 0 ? address % romSize : 0]);
    }
    else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SG1000_16K)
    {
        u8* pRom = m_pCartridge->GetROM();

        switch (address & 0xC000)
        {
            case 0x0000:
            case 0x4000:
            case 0x8000:
                return MEMORY_READ_VALUE(pRom[address]);

            default:
                return MEMORY_READ_VALUE(m_pExternalRam[address & 0x3FFF]);
            }
    }
    else
    {
        u8* pRom = m_pCartridge->GetROM();
        int romSize = m_pCartridge->GetROMSize();

        switch (address & 0xC000)
        {
            case 0x0000:
            case 0x4000:
                return MEMORY_READ_VALUE(pRom[address]);

            case 0x8000:
            {
                if (address >= romSize)
                {
                    Log("--> ** Attempting to read from outer ROM: %X. ROM Size: %X", address, romSize);
                    return MEMORY_READ_VALUE(pRom[address & 0x7FFF]);

                    //                    return 0xFF;
                }

                return MEMORY_READ_VALUE(pRom[address]);
            }

            default:
                return MEMORY_READ_VALUE(m_pInternalRam[BoardRamOffset(address)]);
        }
    }
}

inline void Memory::Write(u16 address, u8 value)
{
    #ifndef GEARSF7000_DISABLE_DISASSEMBLER
    #define MEMORY_WRITE_VALUE(before_value, before_known) CheckBreakpoints(address, true, before_value, before_known, value, true)
    #else
    #define MEMORY_WRITE_VALUE(before_value, before_known) do { } while (0)
    #endif

    if (m_m5Mapper) {
        const u8 before = m_m5Mapper->Peek(address);
        m_m5Mapper->Write(address, value);
        MEMORY_WRITE_VALUE(before, true);
        return;
    }
    if (m_msxSlots)
    {
        const u8 before = m_msxSlots->Peek(address);
        m_msxSlots->Write(address, value);
        MEMORY_WRITE_VALUE(before, true);
        return;
    }
    if (m_sgm && m_sgm->ResolvesRam(address))
    {
        const u8 before = m_pExternalRam[address];
        m_pExternalRam[address] = value;
        MEMORY_WRITE_VALUE(before, true);
        return;
    }
    if (m_colecoMotherboard) {
        if ((address & 0xE000) == 0x6000) {
            const u16 offset = BoardRamOffset(address);
            const u8 before = m_pInternalRam[offset];
            m_pInternalRam[offset] = value;
            MEMORY_WRITE_VALUE(before, true);
        } else { MEMORY_WRITE_VALUE(0, false); }
        return;
    }
    // IPL affects reads only.  While the SF-7000 I/O cartridge is active,
    // every write reaches its 64 KiB RAM regardless of the inserted cartridge.
    if (m_bSF7000Enabled)
    {
        const u8 beforeValue = m_pExternalRam[address];
        m_pExternalRam[address] = value;
        MEMORY_WRITE_VALUE(beforeValue, true);
        return;
    }

    switch (m_pCartridge->GetType())
    {
        case Cartridge::SORD_M5_LINEAR:
        case Cartridge::MSX_LINEAR: // decoded by the attached MSX slot selector
            break;
        case Cartridge::CartridgeTypes::SMS_SEGA_MAPPER:
        {
            if (address >= 0xC000)
            {
                const u16 ramAddress = BoardRamOffset(address);
                const u8 beforeValue = m_pInternalRam[ramAddress];
                m_pInternalRam[ramAddress] = value;
                // $FFFC-$FFFF are both mirrored system RAM and mapper
                // registers. A single bus write reaches both responders.
                if (address >= 0xFFFC && IsValidPointer(m_pMapper))
                    WriteMapperMemory(address, value);
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
            else if (address >= 0x8000 && IsValidPointer(m_pMapper))
            {
                const u8 beforeValue = m_pMapper->Read(address);
                WriteMapperMemory(address, value);
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
            else
            {
                MEMORY_WRITE_VALUE(0, false);
            }
        }
            break;

        case Cartridge::CartridgeTypes::SC3000_FLAT64K:
        {
            // The area holding the image is writable too: it is RAM, not
            // ROM. Self-modifying code and buffers placed inside the program
            // depend on that, which is half the reason this mode exists.
            const u8 beforeValue = m_pExternalRam[address];
            m_pExternalRam[address] = value;
            MEMORY_WRITE_VALUE(beforeValue, true);
        }
            break;

        case Cartridge::CartridgeTypes::SC3000_ASC16L:
        case Cartridge::CartridgeTypes::SC3000_ASC16LS:
            if (address < 0xC000)
            {
                // Se scriviamo in zona ROM/Cartuccia, attiviamo il bank switch
                if (IsValidPointer(m_pMapper))
                    WriteMapperMemory(address, value);
                MEMORY_WRITE_VALUE(0, false);
            }
            else
            {
                const u16 ramAddress = static_cast<u16>(0x4000 +
                    (address & (m_pCartridge->GetASC16LRamBytes() - 1)));
                const u8 beforeValue = m_pExternalRam[ramAddress];
                m_pExternalRam[ramAddress] = value;
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
            break;
        case Cartridge::CartridgeTypes::SC3000_SURVIVORS_MULTICART:
        case Cartridge::CartridgeTypes::SC3000_SURVIVORS_MEGACART:
        {
            if (address >= 0x8000)
            {
                const u16 ramAddress = address & 0x7FFF;
                const u8 beforeValue = m_pExternalRam[ramAddress];
                m_pExternalRam[ramAddress] = value;
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
            else
            {
                MEMORY_WRITE_VALUE(0, false);
            }
        }
            break;

        case Cartridge::CartridgeTypes::SG1000_SEGA_315_5208:
        {
            if (address >= 0xC000)
            {
                const u16 ramAddress = BoardRamOffset(address);
                const u8 beforeValue = m_pInternalRam[ramAddress];
                m_pInternalRam[ramAddress] = value;
                // The single mapper latch overlaps the mirrored work RAM. The
                // write reaches both, while reads still return work RAM.
                if (address == 0xFFFF && IsValidPointer(m_pMapper))
                    WriteMapperMemory(address, value);
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
            else
            {
                MEMORY_WRITE_VALUE(0, false);
            }
        }
            break;
        case Cartridge::CartridgeTypes::SG1000_SEGA_315_5208_DAHJEE_B:
        {
            if (address >= 0xC000)
            {
                const u16 ramAddress = address & 0x1FFF;
                const u8 beforeValue = m_pExternalRam[ramAddress];
                m_pExternalRam[ramAddress] = value;
                // The latch hears the write; the adapter RAM keeps the byte.
                if (address == 0xFFFF && IsValidPointer(m_pMapper))
                    WriteMapperMemory(address, value);
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
            else
            {
                MEMORY_WRITE_VALUE(0, false);
            }
        }
            break;
        case Cartridge::CartridgeTypes::SC3000_32K:
        {
            const u16 ramAddress = address & 0x7FFF;
            const u8 beforeValue = m_pExternalRam[ramAddress];
            m_pExternalRam[ramAddress] = value;
            MEMORY_WRITE_VALUE(beforeValue, true);
        }
            break;

        case Cartridge::CartridgeTypes::SC3000_16K_AT_8000:
        {
            // Same split as the read side: ROM below $8000 drops the write,
            // the cartridge's 16 KB takes $8000-$BFFF, and the machine's own
            // 2 KB takes what is above, mirrored.
            if (address < 0x8000)
            {
                MEMORY_WRITE_VALUE(0, false);
            }
            else if (address < 0xC000)
            {
                const u16 ramAddress = address & 0x3FFF;
                const u8 beforeValue = m_pExternalRam[ramAddress];
                m_pExternalRam[ramAddress] = value;
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
            else
            {
                const u16 ramAddress = BoardRamOffset(address);
                const u8 beforeValue = m_pInternalRam[ramAddress];
                m_pInternalRam[ramAddress] = value;
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
        }
            break;

        case Cartridge::CartridgeTypes::SG1000_DAHJEE_TYPE_A:
        {
            if (address >= 0x2000 && address < 0x4000)
            {
                const u16 ramAddress = address & 0x1FFF;
                const u8 beforeValue = m_pExternalRam[ramAddress];
                m_pExternalRam[ramAddress] = value;
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
            else if (address >= 0xC000)
            {
                const u16 ramAddress = BoardRamOffset(address);
                const u8 beforeValue = m_pInternalRam[ramAddress];
                m_pInternalRam[ramAddress] = value;
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
            else
            {
                MEMORY_WRITE_VALUE(0, false);
            }
        }
            break;

        case Cartridge::CartridgeTypes::SG1000_DAHJEE_TYPE_B:
        {
            if (address >= 0xC000)
            {
                const u16 ramAddress = address & 0x1FFF;
                const u8 beforeValue = m_pExternalRam[ramAddress];
                m_pExternalRam[ramAddress] = value;
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
            else
            {
                MEMORY_WRITE_VALUE(0, false);
            }
        }
            break;

        case Cartridge::CartridgeTypes::SG1000_CASTLE_8K:
        {
            if (address >= 0xC000)
            {
                const u16 ramAddress = BoardRamOffset(address);
                const u8 beforeValue = m_pInternalRam[ramAddress];
                m_pInternalRam[ramAddress] = value;
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
            else if (address >= 0x8000)
            {
                const u16 ramAddress = address & 0x1FFF;
                const u8 beforeValue = m_pExternalRam[ramAddress];
                m_pExternalRam[ramAddress] = value;
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
            else
            {
                MEMORY_WRITE_VALUE(0, false);
            }
        }
            break;

        case Cartridge::CartridgeTypes::SG1000_OTHELLO_2K:
        {
            if (address >= 0x8000 && address < 0xA000)
            {
                const u16 ramAddress = address & 0x07FF;
                const u8 beforeValue = m_pExternalRam[ramAddress];
                m_pExternalRam[ramAddress] = value;
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
            else if (address >= 0xC000)
            {
                const u16 ramAddress = BoardRamOffset(address);
                const u8 beforeValue = m_pInternalRam[ramAddress];
                m_pInternalRam[ramAddress] = value;
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
            else
            {
                MEMORY_WRITE_VALUE(0, false);
            }
        }
            break;

        case Cartridge::CartridgeTypes::SG1000_16K:
        {
            const u16 ramAddress = address & 0x3FFF;
            const u8 beforeValue = m_pExternalRam[ramAddress];
            m_pExternalRam[ramAddress] = value;
            MEMORY_WRITE_VALUE(beforeValue, true);
        }
            break;

        // Writes reach the board RAM only inside the window its reads use:
        // $8000-$FFFF for this profile (the SC-3000's Mitec selects the SRAM
        // on A15), $C000-$FFFF below. A write into the ROM half used to land
        // in RAM as well.
        case Cartridge::CartridgeTypes::SC3000_2K:
        {
            if (address >= 0x8000)
            {
                const u16 ramAddress = BoardRamOffset(address);
                const u8 beforeValue = m_pInternalRam[ramAddress];
                m_pInternalRam[ramAddress] = value;
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
            else
            {
                MEMORY_WRITE_VALUE(0, false);
            }
        }
            break;

        case Cartridge::CartridgeTypes::SG1000_1K:
        {
            if (address >= 0xC000)
            {
                const u16 ramAddress = BoardRamOffset(address);
                const u8 beforeValue = m_pInternalRam[ramAddress];
                m_pInternalRam[ramAddress] = value;
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
            else
            {
                MEMORY_WRITE_VALUE(0, false);
            }
        }
            break;

        case Cartridge::CartridgeTypes::SF7000IPL:
        case Cartridge::CartridgeTypes::CartridgeNotSupported:
            break;

    }
}

#undef MEMORY_READ_VALUE
#undef MEMORY_WRITE_VALUE

inline Memory::stDisassembleRecord** Memory::GetDisassembledRomMemoryMap()
{
    return m_pDisassembledRomMap;
}

inline Memory::stDisassembleRecord** Memory::GetDisassembledInternalRamMemoryMap()
{
    return m_pDisassembledInternalRamMap;
}

inline Memory::stDisassembleRecord** Memory::GetDisassembledBiosMemoryMap()
{
    return m_pDisassembledBiosMap;
}

inline Memory::stDisassembleRecord** Memory::GetDisassembledExternalRamMemoryMap()
{
    return m_pDisassembledExternalRamMap;
}

#endif	/* MEMORY_INLINE_H */
