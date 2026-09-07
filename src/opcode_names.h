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

#ifndef OPCODE_NAMES_H
#define	OPCODE_NAMES_H

// Opcode metadata imported from the modern GearSystem disassembler.  The
// first syntax (GearSystem) is intentionally the canonical syntax here; its
// inline tags ({n}, {o}, {e}) preserve semantic colour information for the
// desktop debugger without coupling this CPU-independent service to its UI.
enum GS_OPCode_Type
{
    GS_OPCode_Type_Implied = 0,
    GS_OPCode_Type_Index,
    GS_OPCode_Type_1b,
    GS_OPCode_Type_2b,
    GS_OPCode_Type_Indexed,
    GS_OPCode_Type_Relative,
    GS_OPCode_Type_Indexed_1b,
    GS_OPCode_Type_Data
};

struct stOPCodeInfo
{
    const char* name;
    int size;
    int type;
};

#define GS_OPCODE(name, size, type) { name, size, type }
#define GS_OPCODE_SYNTAX(gearsystem, wladx, tniasm, z88dk, size, type) { gearsystem, size, type }

#include "opcodexx_names.h"
#include "opcodecb_names.h"
#include "opcodeed_names.h"
#include "opcodedd_names.h"
#include "opcodefd_names.h"
#include "opcodeddcb_names.h"
#include "opcodefdcb_names.h"

#endif	/* OPCODE_NAMES_H */
