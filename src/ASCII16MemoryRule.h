/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2013  Ignacio Sanchez
 * Copyright (C) 2026  Saverio Russo
 *
 * Derived from Gearsystem by Ignacio Sanchez. His code is free software: you
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

#ifndef ASCII16MEMORYRULE_H
#define	ASCII16MEMORYRULE_H

#include "MemoryRule.h"

class Ascii16MemoryRule : public MemoryRule
{
public:
    Ascii16MemoryRule(Memory* pMemory, Cartridge* pCartridge, Input* pInput);
    virtual ~Ascii16MemoryRule();
    virtual u8 PerformRead(u16 address);
    virtual void PerformWrite(u16 address, u8 value);
    virtual void Reset();
    virtual u8* GetRamBanks();
    virtual u8* GetPage(int index);
    virtual int GetBank(int index);
    virtual void SaveState(std::ostream& stream);
    virtual void LoadState(std::istream& stream);

    virtual std::string GetID() { return "ASCII16"; }

private:
    int m_iMapperSlot[3];
    int m_iMapperSlotAddress[3];
//    u8* m_pCartRAM;
    bool m_bRAMBankActive;
};

#endif	/* CODEMASTERSMEMORYRULE_H */
