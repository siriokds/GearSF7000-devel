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

#include "MemoryRule.h"

MemoryRule::MemoryRule(Memory* pMemory, Cartridge* pCartridge, Input* pInput)
{
    m_pMemory = pMemory;
    m_pCartridge = pCartridge;
    m_pInput = pInput;
}

MemoryRule::~MemoryRule()
{
}


void MemoryRule::SaveRam(std::ostream&)
{
}

bool MemoryRule::LoadRam(std::istream&, s32)
{
    return false;
}

void MemoryRule::SetRamChangedCallback(RamChangedCallback callback)
{
    m_pRamChangedCallback = callback;
}

bool MemoryRule::PersistedRAM()
{
    return false;
}

size_t MemoryRule::GetRamSize()
{
    return 0;
}

u8* MemoryRule::GetRamBanks()
{
    return NULL;
}

int MemoryRule::GetRamBank()
{
    return 0;
}

u8* MemoryRule::GetPage(int)
{
    return NULL;
}

int MemoryRule::GetBank(int)
{
    return 0;
}

void MemoryRule::SaveState(std::ostream&)
{
}

void MemoryRule::LoadState(std::istream&)
{
}
