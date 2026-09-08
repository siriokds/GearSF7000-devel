/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026  Saverio Russo

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/ 
 * 
 */

#ifndef MEMORY_INLINE_H
#define	MEMORY_INLINE_H

#include "Cartridge.h"

inline u8 Memory::Read(u16 address)
{
    #ifndef GEARSF7000_DISABLE_DISASSEMBLER
    #define MEMORY_READ_VALUE(expression) ([&]() -> u8 { const u8 debug_value = (expression); CheckBreakpoints(address, false, debug_value, true, debug_value, true); return debug_value; }())
    #else
    #define MEMORY_READ_VALUE(expression) (expression)
    #endif

    if (m_bSF7000Enabled)
    {
        if (address < 0x4000)
        {
            // IPL is physically 8 KiB and mirrored over the 16 KiB read
            // overlay. Writes still reach the SF-7000 external RAM.
            return MEMORY_READ_VALUE(m_bIPLRomDisabled ? m_pSGMRam[address] : m_pBios[address & 0x1FFF]);
        }
        return MEMORY_READ_VALUE(m_pSGMRam[address]);
    }
    // Every branch below this point reaches into m_pCartridge->GetROM(),
    // which Cartridge::Reset() frees and nulls on eject. No cartridge type
    // this dispatches on is ever CartridgeNotSupported while ready, so the
    // one guard here covers all of them: an unmapped bus reads high, the
    // conventional open-bus value, rather than dereferencing a null ROM
    // pointer at the address itself.
    else if (!m_pCartridge->IsReady())
    {
        return MEMORY_READ_VALUE(0xFF);
    }
    // Flat map: no masking, no windows, no banking. All 64 KB are the same
    // RAM the image was loaded into at reset - identical to the SF-7000 branch
    // above once its IPL is switched off.
    else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SC3000_FLAT64K)
    {
        return MEMORY_READ_VALUE(m_pSGMRam[address]);
    }
    else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SC3000_ASC16L)
    {
        if (address < 0xC000)
        {
            // Se siamo nel range della cartuccia (0-48KB), ci pensa il mapper
            return MEMORY_READ_VALUE(m_pMapper->Read(address));
        }
        else
        {
            // Se siamo sopra 0xC000, è RAM.
            // Quale RAM usa la tua versione ASC16L? 
            // Se usa i 32KB della SGM (come lo SC-3000 32K):
            return MEMORY_READ_VALUE(m_pSGMRam[address & 0x7FFF]);
        }
    }
    else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SC3000_2K)
    {
        u8* pRom = m_pCartridge->GetROM();
        //int romSize = m_pCartridge->GetROMSize();

        switch (address & 0xC000)
        {
            case 0x0000:
            case 0x4000:
                return MEMORY_READ_VALUE(pRom[address]);

            default:
                return MEMORY_READ_VALUE(m_pRam[address & 0x7FF]);
        }
    }
    else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SC3000_32K)
    {
        u8* pRom = m_pCartridge->GetROM();
        //int romSize = m_pCartridge->GetROMSize();

        switch (address & 0xC000)
        {
            case 0x0000:
            case 0x4000:
                return MEMORY_READ_VALUE(pRom[address]);

            default:
                return MEMORY_READ_VALUE(m_pSGMRam[address & 0x7FFF]);
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
            return MEMORY_READ_VALUE(m_pSGMRam[address & 0x3FFF]);
        return MEMORY_READ_VALUE(m_pRam[address & 0x07FF]);
    }
    else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SG1000_16K)
    {
        u8* pRom = m_pCartridge->GetROM();
        //int romSize = m_pCartridge->GetROMSize();

        switch (address & 0xC000)
        {
            case 0x0000:
            case 0x4000:
            case 0x8000:
                return MEMORY_READ_VALUE(pRom[address]);

            default:
                return MEMORY_READ_VALUE(m_pSGMRam[address & 0x3FFF]);
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
                return MEMORY_READ_VALUE(m_pRam[address & 0x3FF]);
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

    // IPL affects reads only.  While the SF-7000 I/O cartridge is active,
    // every write reaches its 64 KiB RAM regardless of the inserted cartridge.
    if (m_bSF7000Enabled)
    {
        const u8 beforeValue = m_pSGMRam[address];
        m_pSGMRam[address] = value;
        MEMORY_WRITE_VALUE(beforeValue, true);
        return;
    }

    switch (m_pCartridge->GetType())
    {
        case Cartridge::CartridgeTypes::SC3000_FLAT64K:
        {
            // The area holding the image is writable too: it is RAM, not
            // ROM. Self-modifying code and buffers placed inside the program
            // depend on that, which is half the reason this mode exists.
            const u8 beforeValue = m_pSGMRam[address];
            m_pSGMRam[address] = value;
            MEMORY_WRITE_VALUE(beforeValue, true);
        }
            break;

        case Cartridge::CartridgeTypes::SC3000_ASC16L:
            if (address < 0xC000)
            {
                // Se scriviamo in zona ROM/Cartuccia, attiviamo il bank switch
                if (IsValidPointer(m_pMapper))
                    m_pMapper->Write(address, value);
                MEMORY_WRITE_VALUE(0, false);
            }
            else
            {
                // Se scriviamo sopra 0xC000, scriviamo nella RAM (SGM 32KB)
                const u16 ramAddress = address & 0x7FFF;
                const u8 beforeValue = m_pSGMRam[ramAddress];
                m_pSGMRam[ramAddress] = value;
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
            break;
        case Cartridge::CartridgeTypes::SC3000_32K:
        {
            const u16 ramAddress = address & 0x7FFF;
            const u8 beforeValue = m_pSGMRam[ramAddress];
            m_pSGMRam[ramAddress] = value;
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
                const u8 beforeValue = m_pSGMRam[ramAddress];
                m_pSGMRam[ramAddress] = value;
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
            else
            {
                const u16 ramAddress = address & 0x07FF;
                const u8 beforeValue = m_pRam[ramAddress];
                m_pRam[ramAddress] = value;
                MEMORY_WRITE_VALUE(beforeValue, true);
            }
        }
            break;

        case Cartridge::CartridgeTypes::SG1000_16K:
        {
            const u16 ramAddress = address & 0x3FFF;
            const u8 beforeValue = m_pSGMRam[ramAddress];
            m_pSGMRam[ramAddress] = value;
            MEMORY_WRITE_VALUE(beforeValue, true);
        }
            break;

        case Cartridge::CartridgeTypes::SC3000_2K:
        {
            const u16 ramAddress = address & 0x7FF;
            const u8 beforeValue = m_pRam[ramAddress];
            m_pRam[ramAddress] = value;
            MEMORY_WRITE_VALUE(beforeValue, true);
        }
            break;

        case Cartridge::CartridgeTypes::SG1000_1K:
        {
            const u16 ramAddress = address & 0x3FF;
            const u8 beforeValue = m_pRam[ramAddress];
            m_pRam[ramAddress] = value;
            MEMORY_WRITE_VALUE(beforeValue, true);
        }
            break;

    }
}

#undef MEMORY_READ_VALUE
#undef MEMORY_WRITE_VALUE

inline Memory::stDisassembleRecord** Memory::GetDisassembledRomMemoryMap()
{
    return m_pDisassembledRomMap;
}

inline Memory::stDisassembleRecord** Memory::GetDisassembledRamMemoryMap()
{
    return m_pDisassembledRamMap;
}

inline Memory::stDisassembleRecord** Memory::GetDisassembledBiosMemoryMap()
{
    return m_pDisassembledBiosMap;
}

inline Memory::stDisassembleRecord** Memory::GetDisassembledSGMRamMemoryMap()
{
    return m_pDisassembledSGMRamMap;
}

#endif	/* MEMORY_INLINE_H */
