/*
 * GearSF7000 - Deep Search: the heavy post-collection analysis, in rounds
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef GEARSF7000_DEEP_SEARCH_H
#define GEARSF7000_DEEP_SEARCH_H

#include <atomic>
#include <string>
#include <utility>
#include <vector>

#include "JumpTables.h"
#include "RomMap.h"
#include "definitions.h"

class RomMap;

// Starts from what the live collection proved - executed code, the pieces
// the user kept, jump table entries seen in use - and works outward into
// what is unknown, never questioning what ran. Round after round, until a
// round finds nothing new:
//   1. jump tables, read from the code before every jp (hl)/(ix)/(iy),
//      including those in code deduced by earlier rounds;
//   2. guided static disassembly: from every edge of the known code - a
//      jump or call into unknown bytes, the side of a condition never
//      taken, the return after a call, a table's targets - follow the flow,
//      block by block; a block is dropped whole at the first sign it is not
//      code (an invalid opcode, rst $38 twice, a jump or access to where the
//      board has nothing, running into data);
//   3. a score for every block: how much its instructions look like this
//      game's own executed code;
// and, once the rounds are done, 4. the data: for every stretch still not
// sure, the code that names the table around it (ld hl,T and what follows,
// into the helper it calls) says the size of an entry, the bytes read, the
// index and the count, and whether the words are used as addresses; a
// stretch of an even length between pointers, all of it words into the
// cartridge, is pointers too. What it says is "data?" / "pointers?": hints.
// What it deduces is CODE? - a strong guess, never proof. It works on a
// copy of the map and writes nothing; the caller applies the result.
namespace DeepSearch
{
struct Options
{
    bool jumpTables = true;
    bool guided = true;
    bool data = true;       // 4. what the bytes nobody read are: tables, pointers
    int maxRounds = 12;
};

// A block of code deduced by the guided disassembly.
struct Block
{
    u32 start;          // ROM offsets, end exclusive
    u32 end;
    u32 from;           // the instruction, or table entry, that leads here
    int round;
    int score;          // 0-100: how much it looks like the game's own code
    std::string reason; // "jp z at 0B21", "entry 4 of the jump table at 0453"
};

struct Round
{
    int round;
    int tables;         // new jump tables
    int entries;
    int blocks;         // new blocks of deduced code
    u32 bytes;
    int dropped;        // blocks given up at a sign they are not code
};

struct Result
{
    std::vector<JumpTables::Site> sites;
    std::vector<Block> blocks;
    std::vector<Round> rounds;
    std::vector<RomMap::DataHint> dataHints;    // "data? ..." / "pointers? ..."
    bool cancelled = false;
};

// answers: 65536 entries, non-zero where something answers on the board
// (Memory::AnswersAt). kept: the user's code pieces (ROM offsets, end
// exclusive). progress: the round being worked, for a progress display.
Result Run(const u8* rom, u32 romSize, u16 base, const RomMap& map,
           const std::vector<u8>& answers,
           const std::vector<std::pair<u32, u32>>& kept,
           std::vector<JumpTables::Site> sites, const Options& options,
           std::atomic<int>* progress = nullptr, std::atomic<bool>* cancel = nullptr);

// rommap/datahints.txt: what the data step made of the bytes nobody read.
std::string FormatDataHints(const std::vector<RomMap::DataHint>& hints);
bool LoadDataHints(const std::string& path, std::vector<RomMap::DataHint>& hints);

// rommap/deduced.txt: the deduced blocks, for the map's static rules.
std::string FormatDeduced(const std::vector<Block>& blocks);
bool LoadDeduced(const std::string& path, std::vector<RomMap::DeducedBlock>& blocks);
}

#endif
