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

#ifndef BASIC_PROGRAM_LOADER_H
#define BASIC_PROGRAM_LOADER_H

#include <string>
#include <vector>
#include "definitions.h"

class Memory;

// Writes a tokenised BASIC program straight into the machine's program area,
// the way a cassette LOAD would, and then rebuilds the pointers BASIC keeps
// around it. Seconds instead of minutes, for the .bas images that are already
// in memory format.
//
// This is the counterpart of the BASIC Typer, not a faster version of it: a
// .basic file is *source text*, which BASIC has to tokenise itself, so it has
// to arrive through the keyboard or the cassette. A .bas file is what that
// tokenising produced, so it can simply be put back.
namespace BasicProgramLoader
{
    // BASIC keeps its program and variable boundaries in five consecutive
    // words. The structure is fixed; where the block sits is not.
    //
    // The Level IIIB rebuild puts it at RAMBASE + $0160, which is $8160 for
    // both the 16K and 32K cartridge - only RAMSIZE changes between them. Disk
    // BASIC puts it at $9954, and that is not the same offset from any round
    // base: the block moved by $17F4 while the program area moved by $1F0F, so
    // Disk BASIC is not the cartridge layout displaced, it is a larger
    // workspace. Any port will move it again.
    //
    // So the configurable value is the address of the block itself, not a base
    // to add an offset to.
    // Known layouts, both measured on a running machine.
    const u16 AUTO_BLOCK       = 0x0000;   // scan for it
    const u16 LEVEL3_BLOCK     = 0x8160;   // Sega BASIC Level III cartridge
    const u16 DISK_BASIC_BLOCK = 0x9954;   // Sega Disk BASIC 1.0p
    const u16 TXTBGN_OFFSET  = 0;   // start of the program
    const u16 ARYBGN_OFFSET  = 2;   // start of the arrays
    const u16 VARBGN_OFFSET  = 4;   // start of the variables
    const u16 FREEBGN_OFFSET = 6;   // start of free memory
    const u16 FREEEND_OFFSET = 8;   // upper limit

    struct Result
    {
        bool  success;
        u16   address;      // where the program was written, from TXTBGN
        u16   size;         // bytes written
        u16   free_end;     // FREEEND at the time, for reporting
        u16   block;        // the block actually used
        bool  detected;     // true when the configured block was wrong and a
                            // single consistent one was found instead
        std::string error;
    };

    // Whether the five words at this address are consistent with each other
    // the way BASIC maintains them. Strict enough that arbitrary bytes
    // essentially never pass - over 32 KB of a live Disk BASIC it matched in
    // exactly one place - which is what makes it usable both as a safety check
    // and as a way to find the block on an unfamiliar BASIC.
    bool LooksInitialised(Memory* memory, u16 block);

    // Addresses where a pointer block looks like a running BASIC, lowest
    // first. Meant for reporting: a BASIC whose layout is not documented, or a
    // port being built, can be told where its block ended up instead of the
    // author hunting for it by hand.
    std::vector<u16> FindCandidateBlocks(Memory* memory);

    // Loads file_path at whatever TXTBGN says in the pointer block - read from
    // the machine rather than assumed, so this follows the RAM size instead of
    // hardcoding the program address.
    //
    // If configuredBlock does not look like a running BASIC but exactly one
    // other address does, that one is used and Result::detected says so. Two
    // or more, or none, refuses and reports what it found.
    Result Load(Memory* memory, u16 configuredBlock, const std::string& file_path);

    // The mirror image: writes out [TXTBGN, ARYBGN), which is how the ROM's
    // own SAVE decides what the program is - the length it puts on tape is
    // ARYBGN-TXTBGN. Refuses on an empty program the same way it does, by
    // testing the byte at TXTBGN, so that saving nothing produces an error
    // rather than a zero-byte file that looks like a program later.
    Result Save(Memory* memory, u16 configuredBlock, const std::string& file_path);
}

#endif /* BASIC_PROGRAM_LOADER_H */
