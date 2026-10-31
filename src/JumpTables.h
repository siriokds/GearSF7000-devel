/*
 * GearSF7000 - jump tables, read from the code that jumps through them
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef GEARSF7000_JUMP_TABLES_H
#define GEARSF7000_JUMP_TABLES_H

#include <string>
#include <vector>

#include "definitions.h"

class RomMap;

// Every executed JP (HL), JP (IX) or JP (IY) is a question: did the code
// build that address from a table in the ROM? The answer is read from the
// executed instructions before the jump - a small tracer that knows what
// each register holds (a constant, an index from a RAM variable times two,
// the table's address plus that, the word read there) - and, when the
// table's address comes from outside the routine (a helper called as
//   ld hl, table / call helper), from the instructions before each call.
// No pattern list: the tracer follows ld, add, sla, inc, ex de,hl, push and
// pop, and the combinations come out on their own.
//
// It reads, never writes: what it finds is kept by the caller
// (rommap/jumptables.txt in a project). Run at a pause, not while the
// machine runs (Saverio).
namespace JumpTables
{
struct Table
{
    u32 base;           // ROM offset of the first entry
    u32 entries;
    bool counted;       // the count comes from the code (cp n / and mask),
                        // not from where plausible entries stop
    std::string index;  // what indexes it: "_RAM_C034_", "unknown"
    u32 caller;         // ROM offset of the call that passed the table,
                        // or the jump itself when the routine sets it
    bool vector = false; // the jump lands in the table: entries are 3-byte
                         // jp instructions, not addresses
};

struct Site
{
    u32 offset;         // the JP (rr) instruction, ROM offset
    u32 signature;      // what the analysis saw: re-run when it changes
    std::vector<Table> tables;
    std::string reason; // why none, when none
};

// The executed indirect jumps of a map.
std::vector<u32> FindSites(const u8* rom, u32 romSize, const RomMap& map);

// A cheap fingerprint of the code a site's analysis reads: the executed
// bytes of its routine and the calls into it. A site whose signature has
// not moved needs no second look.
u32 Signature(const u8* rom, u32 romSize, u16 base, const RomMap& map, u32 site);

Site Analyze(const u8* rom, u32 romSize, u16 base, const RomMap& map, u32 site);

// How the code uses a table it names (ld hl/de/bc/ix/iy,T): the tracer
// follows the instructions after it - into the routine it calls, as a helper
// that gets the table in a register - to the reads from T. What it learns:
// the size of an entry (the index's scale), which bytes of it are read, the
// index and how many entries it allows (cp n / and mask), and whether a word
// read there is then used as an address (read through, or jumped to): a
// pointer, not a value. Deep Search's data step, for the bytes nobody read.
struct DataRead
{
    u32 site;               // the instruction naming the table, ROM offset
    u32 table;              // T, ROM offset
    bool found = false;     // a read from T was reached
    u32 reader = 0;         // the first read, ROM offset
    int entrySize = 0;      // 0 when there is no index (a fixed record)
    std::vector<int> fields;// offsets from T (modulo the entry) read
    std::string index;      // "_RAM_C00F_", "(iy+$0C)", "unknown"
    int entries = 0;        // from the code, 0 when not known
    bool pointers = false;  // a word read there is used as an address
    u32 copied = 0;         // bytes copied from T whole (ldir/otir), if so
    std::string reason;     // when nothing was found
};

// The routine entries (targets of executed calls and rst), to pass to
// AnalyzeData when it is called many times on one map.
std::vector<u32> RoutineEntries(const u8* rom, u32 romSize, u16 base, const RomMap& map);
DataRead AnalyzeData(const u8* rom, u32 romSize, u16 base, const RomMap& map, u32 site, u32 table,
                     const std::vector<u32>* routineEntries = nullptr);

// rommap/jumptables.txt: one line per site, its tables or why none.
bool Load(const std::string& path, std::vector<Site>& sites);
std::string Format(const std::vector<Site>& sites);
}

#endif
