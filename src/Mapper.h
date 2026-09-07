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

#ifndef MAPPER_H
#define MAPPER_H

#include "definitions.h"
#include <iostream>

class Cartridge;

class Mapper
{
public:
    Mapper(Cartridge* pCartridge) : m_pCartridge(pCartridge) { }
    virtual ~Mapper() { }
    
    virtual void Reset() = 0;
    virtual u8 Read(u16 address) = 0;
    virtual void Write(u16 address, u8 value) = 0;
    virtual void SaveState(std::ostream& stream) = 0;
    virtual void LoadState(std::istream& stream) = 0;

    // Save-state identity. Mappers hold different things - one bank register
    // here, three slots plus a RAM-enable flag there - so a snapshot cannot
    // assume a shape. It records this id alongside a byte count, and refuses
    // to apply a payload that was written by a different mapper. Pure virtual
    // on purpose: a new mapper has to state who it is before it can be saved.
    //
    // Ids are four-character codes so they read as themselves in a hex dump.
    virtual u32 GetMapperId() const = 0;
    virtual u16 GetStateVersion() const { return 1; }
    virtual u8 GetRomBank() { return 0; }
    virtual u32 GetRomBankAddress() { return 0; }
    virtual u8 GetBankReg(int) { return 0; }

protected:
    Cartridge* m_pCartridge;
};

#endif /* MAPPER_H */
