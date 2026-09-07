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

#include <cstdio>
#include <vector>
#include "BasicProgramLoader.h"
#include "Memory.h"

namespace BasicProgramLoader
{

static u16 ReadPointer(Memory* memory, u16 address)
{
    return (u16)memory->Read(address) | ((u16)memory->Read(address + 1) << 8);
}

static void WritePointer(Memory* memory, u16 address, u16 value)
{
    memory->Write(address, value & 0xFF);
    memory->Write(address + 1, (value >> 8) & 0xFF);
}

bool LooksInitialised(Memory* memory, u16 block)
{
    if (!IsValidPointer(memory))
        return false;

    const u16 txtbgn  = ReadPointer(memory, block + TXTBGN_OFFSET);
    const u16 arybgn  = ReadPointer(memory, block + ARYBGN_OFFSET);
    const u16 varbgn  = ReadPointer(memory, block + VARBGN_OFFSET);
    const u16 freebgn = ReadPointer(memory, block + FREEBGN_OFFSET);
    const u16 freeend = ReadPointer(memory, block + FREEEND_OFFSET);

    // BASIC keeps these in order: program, then arrays, then variables, then
    // free memory, strictly below the limit, and the program cannot overlap
    // the block itself.
    if (!(txtbgn < arybgn && arybgn <= varbgn && varbgn <= freebgn && freebgn < freeend))
        return false;
    if (txtbgn <= (u16)(block + FREEEND_OFFSET))
        return false;

    // Plausibility, not structure: the arrays and variables of a program on a
    // machine with ~26 KB free do not run to eight kilobytes. Without some
    // such bound a second address passes on a live Disk BASIC; with it, one
    // does. A program that really does carry more than 8 KB of data will not
    // be found automatically and has to have its block pinned.
    //
    // This deliberately does *not* test for the zero bytes BASIC writes at
    // ARYBGN and VARBGN. Those are only there while no arrays and no variables
    // exist: one RUN and VARBGN points at variable data instead. Requiring
    // them made the loader refuse to save a program that had just been run,
    // which is exactly when you want to save it.
    if ((u32)(freebgn - arybgn) > 0x2000)
        return false;

    // A real block has real free memory behind it - hundreds or thousands of
    // bytes, not one. Home BASIC's pointer block is six words, not five, and
    // its layout puts a second five-in-a-row reading one word into the real
    // block: TXTBGN misread as the extra leading word, an all-zero region that
    // still satisfies every check above, right down to FREEEND landing one
    // byte past FREEBGN. Both readings passed until this was added; only the
    // real one has anywhere to put a program.
    if ((u32)(freeend - freebgn) < 0x100)
        return false;

    return true;
}

std::vector<u16> FindCandidateBlocks(Memory* memory)
{
    std::vector<u16> found;
    if (!IsValidPointer(memory))
        return found;

    // Every word-aligned address that could be RAM. Disk BASIC's block turned
    // out to be at $9954, which no round base explains, so assuming alignment
    // is exactly the mistake this is meant to avoid.
    for (u32 block = 0x8000; block <= 0xFFF6; block += 2)
        if (LooksInitialised(memory, (u16)block))
            found.push_back((u16)block);

    return found;
}

// Resolves which pointer block to work on, or explains why it cannot.
//
// AUTO_BLOCK means "scan for it", which is what makes this usable across
// BASICs whose layouts differ - the Level III cartridge at $8160, Disk BASIC
// at $9954 - without having to look the address up first. Auto proceeds only
// on exactly one match: several, or none, refuses, because an ambiguous answer
// is worse than no answer when being wrong means corrupting memory.
//
// An address the user pinned is used as pinned, and refused if it turns out
// wrong rather than quietly falling back to whatever the scan liked. Working
// somewhere other than where you were told is not a favour.
static bool ResolveBlock(Memory* memory, u16 configured, u16* block, bool* detected, std::string* error)
{
    if (configured != AUTO_BLOCK)
    {
        *block = configured;
        *detected = false;
        return true;
    }

    const std::vector<u16> candidates = FindCandidateBlocks(memory);
    if (candidates.size() != 1)
    {
        *error = candidates.empty()
            ? "No BASIC pointer block found: is BASIC running?"
            : "Several addresses look like a BASIC pointer block - choose one:";
        for (size_t i = 0; i < candidates.size() && i < 6; i++)
        {
            char one[16];
            snprintf(one, sizeof(one), " $%04X", candidates[i]);
            *error += one;
        }
        return false;
    }

    *block = candidates[0];
    *detected = true;
    return true;
}

// Describes a block that failed the consistency test, and says where the test
// would have passed. A wrong setting then answers itself instead of just
// failing.
static std::string DescribeBadBlock(Memory* memory, u16 block)
{
    char detail[256];
    snprintf(detail, sizeof(detail),
             "No initialised BASIC pointer block at $%04X: TXTBGN $%04X ARY $%04X VAR $%04X FREE $%04X END $%04X",
             block,
             ReadPointer(memory, block + TXTBGN_OFFSET),
             ReadPointer(memory, block + ARYBGN_OFFSET),
             ReadPointer(memory, block + VARBGN_OFFSET),
             ReadPointer(memory, block + FREEBGN_OFFSET),
             ReadPointer(memory, block + FREEEND_OFFSET));

    std::string text = detail;
    const std::vector<u16> candidates = FindCandidateBlocks(memory);
    if (!candidates.empty())
    {
        text += ". Consistent at";
        for (size_t i = 0; i < candidates.size() && i < 6; i++)
        {
            char one[16];
            snprintf(one, sizeof(one), " $%04X", candidates[i]);
            text += one;
        }
    }
    return text;
}

// Everything both directions need before touching a file: a machine, a block
// that resolves, and a block that looks like a running BASIC.
static bool Prepare(Memory* memory, u16 configuredBlock, Result* result, u16* block)
{
    if (!IsValidPointer(memory))
    {
        result->error = "No machine running";
        return false;
    }

    if (!ResolveBlock(memory, configuredBlock, block, &result->detected, &result->error))
        return false;

    result->block = *block;

    if (!LooksInitialised(memory, *block))
    {
        result->error = DescribeBadBlock(memory, *block);
        return false;
    }

    return true;
}

Result Load(Memory* memory, u16 configuredBlock, const std::string& file_path)
{
    Result result = { false, 0, 0, 0, 0, false, "" };
    u16 block = 0;

    if (!Prepare(memory, configuredBlock, &result, &block))
        return result;

    FILE* file = fopen(file_path.c_str(), "rb");
    if (!file)
    {
        result.error = "Unable to open " + file_path;
        return result;
    }

    fseek(file, 0, SEEK_END);
    const long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size <= 0)
    {
        fclose(file);
        result.error = "Empty file";
        return result;
    }

    std::vector<u8> payload((size_t)file_size);
    const size_t read = fread(&payload[0], 1, payload.size(), file);
    fclose(file);

    if (read != payload.size())
    {
        result.error = "Short read";
        return result;
    }

    // Where the program goes is BASIC's decision, not ours: TXTBGN is written
    // at cold start by InitializeBasicMemoryLayout according to how much RAM
    // the configuration found. Reading it means this follows a 16K machine to
    // a different address without being told.
    const u16 txtbgn  = ReadPointer(memory, block + TXTBGN_OFFSET);
    const u16 freeend = ReadPointer(memory, block + FREEEND_OFFSET);

    result.address = txtbgn;
    result.size = (u16)payload.size();
    result.free_end = freeend;

    // The same test the ROM makes before overwriting the program, from
    // CassetteLoad at $796C: the payload plus the two terminator bytes has to
    // fit between TXTBGN and FREEEND.
    if ((u32)payload.size() + 2 > (u32)(freeend - txtbgn))
    {
        result.error = "Out of memory: the program does not fit between TXTBGN and FREEEND";
        return result;
    }

    u16 address = txtbgn;
    for (size_t i = 0; i < payload.size(); i++)
        memory->Write(address++, payload[i]);

    // Finalisation copied from the ROM's own CassetteLoad_FinalizeProgramPointers
    // at $79B1: two zero bytes, then the empty array/variable/free boundaries
    // in that order. Three pointers, not four - an earlier attempt at this in
    // SC3K-System guessed four, and wrote them at 8160 *decimal*, which landed
    // in the middle of the program it had just loaded.
    memory->Write(address, 0x00);
    WritePointer(memory, block + ARYBGN_OFFSET, address);
    address++;
    memory->Write(address, 0x00);
    WritePointer(memory, block + VARBGN_OFFSET, address);
    address++;
    WritePointer(memory, block + FREEBGN_OFFSET, address);

    result.success = true;
    return result;
}

Result Save(Memory* memory, u16 configuredBlock, const std::string& file_path)
{
    Result result = { false, 0, 0, 0, 0, false, "" };
    u16 block = 0;

    if (!Prepare(memory, configuredBlock, &result, &block))
        return result;

    const u16 txtbgn  = ReadPointer(memory, block + TXTBGN_OFFSET);
    const u16 arybgn  = ReadPointer(memory, block + ARYBGN_OFFSET);
    const u16 freeend = ReadPointer(memory, block + FREEEND_OFFSET);

    result.address = txtbgn;
    result.free_end = freeend;

    // What the ROM's SAVE calls the program: ARYBGN-TXTBGN bytes, which is the
    // length it puts in the tape header at $7A87. Not including the two zero
    // terminators, which LOAD writes back itself - so a file saved here is the
    // same shape as one that came off a tape, and reloads through the same
    // path.
    const u16 size = (u16)(arybgn - txtbgn);
    result.size = size;

    // The ROM refuses here too, at $7A36, by testing the byte at TXTBGN. An
    // empty program would otherwise save as a zero-byte file that looks like a
    // program until someone loads it.
    if (size == 0 || memory->Read(txtbgn) == 0)
    {
        result.error = "No program to save";
        return result;
    }

    std::vector<u8> payload(size);
    for (u16 i = 0; i < size; i++)
        payload[i] = memory->Read((u16)(txtbgn + i));

    FILE* file = fopen(file_path.c_str(), "wb");
    if (!file)
    {
        result.error = "Unable to write " + file_path;
        return result;
    }

    const size_t written = fwrite(&payload[0], 1, payload.size(), file);
    fclose(file);

    if (written != payload.size())
    {
        result.error = "Short write";
        return result;
    }

    result.success = true;
    return result;
}

}
