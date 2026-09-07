/*
 * Gearsystem - Sega Master System / Game Gear Emulator
 * Copyright (C) 2013  Ignacio Sanchez

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

#include "Ascii16MemoryRule.h"
#include "Memory.h"
#include "Cartridge.h"

Ascii16MemoryRule::Ascii16MemoryRule(Memory* pMemory, Cartridge* pCartridge, Input* pInput) : MemoryRule(pMemory, pCartridge, pInput)
{
    //m_pCartRAM = new u8[0x4000];

    m_iMapperSlot[0] = 0;
    m_iMapperSlotAddress[0] = m_iMapperSlot[0] * 0x4000;

    m_iMapperSlot[1] = 1;
    m_iMapperSlotAddress[1] = m_iMapperSlot[1] * 0x4000;

    m_iMapperSlot[2] = 2;
    m_iMapperSlotAddress[2] = m_iMapperSlot[2] * 0x4000;


    Reset();
}

Ascii16MemoryRule::~Ascii16MemoryRule()
{
    //SafeDeleteArray(m_pCartRAM);
}

u8 Ascii16MemoryRule::PerformRead(u16 address)
{
    if (address >= 0x0000 && address < 0x4000)
    {
        // ROM page 0
        u8* pROM = m_pCartridge->GetROM();
        return pROM[(address & 0x3FFF) + m_iMapperSlotAddress[0]];
    }
    else if (address >= 0x4000 && address < 0x8000)
    {
        // ROM page 1
        u8* pROM = m_pCartridge->GetROM();
        return pROM[(address & 0x3FFF) + m_iMapperSlotAddress[1]];
    }
    else if (address >= 0x8000 && address < 0xC000)
    {
        u8* pROM = m_pCartridge->GetROM();
        return pROM[(address & 0x3FFF) + m_iMapperSlotAddress[2]];
    }
    else
    {
        // RAM + RAM mirror
        return m_pMemory->Retrieve(address);
    }
}

void Ascii16MemoryRule::PerformWrite(u16 address, u8 value)
{
    if (address == 0x8000) // && address < 0xC000)
    //if (address == 0x7000)
    {
     //   value++;

        if (value > m_pCartridge->GetROMBankCount() - 1)
        {
            value %= m_pCartridge->GetROMBankCount();
        }


        m_iMapperSlot[2] = value;
        m_iMapperSlotAddress[2] = m_iMapperSlot[2] * 0x4000;
    }
    else if (address >= 0xC000)
    {
        // RAM
        m_pMemory->Load(address, value);
    }
}

void Ascii16MemoryRule::Reset()
{
    m_iMapperSlot[0] = 0;
    m_iMapperSlotAddress[0] = m_iMapperSlot[0] * 0x4000;
    m_iMapperSlot[1] = 1;
    m_iMapperSlotAddress[1] = m_iMapperSlot[1] * 0x4000;
    m_iMapperSlot[2] = 2;
    m_iMapperSlotAddress[2] = m_iMapperSlot[2] * 0x4000;
}

u8* Ascii16MemoryRule::GetRamBanks()
{
    return NULL;
}

u8* Ascii16MemoryRule::GetPage(int index)
{
    switch (index & 3)
    {
        case 0:
            return m_pMemory->GetMemoryMap() + m_iMapperSlotAddress[0]; // (0x4000 * index);
        case 1:
            return m_pMemory->GetMemoryMap() + m_iMapperSlotAddress[1]; // (0x4000 * index);
        case 2:
            return m_pMemory->GetMemoryMap() + m_iMapperSlotAddress[2]; // (0x4000 * index);
        default:
            return NULL;
    }

    //if ((index >= 0) && (index < 3))
    //    return m_pMemory->GetMemoryMap() + m_iMapperSlotAddress[2]; // (0x4000 * index);
    //else
    //    return NULL;
}

int Ascii16MemoryRule::GetBank(int index)
{
    switch (index)
    {
        case 0:
            return m_iMapperSlot[index];
        case 1:
            return m_iMapperSlot[index];
        case 2:
            return m_iMapperSlot[index];
        default:
            return 0;
    }
}

void Ascii16MemoryRule::SaveState(std::ostream& stream)
{
    using namespace std;

    stream.write(reinterpret_cast<const char*> (m_iMapperSlot), sizeof(m_iMapperSlot));
    stream.write(reinterpret_cast<const char*> (m_iMapperSlotAddress), sizeof(m_iMapperSlotAddress));
    //stream.write(reinterpret_cast<const char*> (m_pCartRAM), 0x4000);
    //stream.write(reinterpret_cast<const char*> (&m_bRAMBankActive), sizeof(m_bRAMBankActive));
}

void Ascii16MemoryRule::LoadState(std::istream& stream)
{
    using namespace std;

    stream.read(reinterpret_cast<char*> (m_iMapperSlot), sizeof(m_iMapperSlot));
    stream.read(reinterpret_cast<char*> (m_iMapperSlotAddress), sizeof(m_iMapperSlotAddress));
    //stream.read(reinterpret_cast<char*> (m_pCartRAM), 0x4000);
    //stream.read(reinterpret_cast<char*> (&m_bRAMBankActive), sizeof(m_bRAMBankActive));
}
