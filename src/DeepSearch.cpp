/*
 * GearSF7000 - Deep Search: the heavy post-collection analysis, in rounds
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "DeepSearch.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "RomMap.h"
#include "Z80Disassembler.h"

namespace
{
enum State : u8
{
    Free,       // unknown, or only guessed: open to the search
    Code,       // executed, kept by the user, or deduced by now
    Data        // seen read as data, or a kind that is not code
};

struct Instruction
{
    int size = 0;
    std::string text;       // upper case, no markers
    Z80Disassembler::Decoded decoded;
};

Instruction decode(const u8* rom, u32 romSize, u16 base, u32 offset)
{
    u8 bytes[16] = {};
    for (u32 i = 0; i < 16 && offset + i < romSize; i++)
        bytes[i] = rom[offset + i];
    Instruction i;
    Z80Disassembler::Decode(bytes, 16, static_cast<u16>(base + offset), i.decoded);
    i.size = i.decoded.size;
    for (const char* p = i.decoded.name; *p; p++)
    {
        if (p[0] == '{' && p[1] && p[2] == '}')
        {
            p += 2;
            continue;
        }
        i.text += static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
    }
    return i;
}

bool conditional(const Instruction& i)
{
    const std::string& t = i.text;
    if (t.compare(0, 4, "DJNZ") == 0)
        return true;
    if (t.compare(0, 4, "RET ") == 0)
        return true;
    return (t.compare(0, 3, "JP ") == 0 || t.compare(0, 3, "JR ") == 0 || t.compare(0, 5, "CALL ") == 0) &&
           t.find(',') != std::string::npos;
}

bool ends_flow(const Instruction& i)
{
    const std::string& t = i.text;
    if (t == "RET" || t == "RETI" || t == "RETN" || t == "HALT" || t.compare(0, 4, "JP (") == 0)
        return true;
    return (t.compare(0, 3, "JP ") == 0 || t.compare(0, 3, "JR ") == 0) && t.find(',') == std::string::npos;
}

bool invalid(const Instruction& i)
{
    return i.size <= 0 || i.text.find("PARSE ERROR") != std::string::npos ||
           i.text.find("[UNDOC]") != std::string::npos;
}

// A key for an instruction's kind, for the style of the game's own code:
// the opcode, with its second byte behind a prefix.
u16 style_key(const u8* rom, u32 romSize, u32 offset)
{
    const u8 op = rom[offset];
    if ((op == 0xCB || op == 0xED || op == 0xDD || op == 0xFD) && offset + 1 < romSize)
        return static_cast<u16>((op << 8) | rom[offset + 1]);
    return op;
}

struct Seed
{
    u32 at;
    u32 from;
    std::string reason;
};

struct Search
{
    const u8* rom;
    u32 size;
    u16 base;
    RomMap work;                    // the map, with deduced code marked as run
    const std::vector<u8>& answers;
    std::vector<u8> state;
    std::unordered_map<u16, int> style;     // executed instructions per key
    std::deque<Seed> seeds;
    std::vector<DeepSearch::Block> blocks;
    int dropped = 0;

    Search(const u8* r, u32 s, u16 b, const RomMap& m, const std::vector<u8>& a)
        : rom(r), size(s), base(b), work(m), answers(a), state(s, Free)
    {
    }

    bool in_rom(u16 address) const
    {
        return address >= base && static_cast<u32>(address - base) < size;
    }

    // The edges of a known instruction: where it leads that is still open.
    void edges(u32 offset, const Instruction& i, const std::string& where)
    {
        char text[64];
        const bool call = i.decoded.subroutine && !i.decoded.rst;
        if (i.decoded.jump && !i.decoded.rst && in_rom(i.decoded.jumpAddress))
        {
            const u32 target = static_cast<u32>(i.decoded.jumpAddress - base);
            if (state[target] == Free)
            {
                std::snprintf(text, sizeof(text), "%s at %X", call ? "call" : "jump", base + offset);
                seeds.push_back({target, offset, where.empty() ? text : where});
            }
        }
        // What follows a condition never taken, or a call that returns.
        if ((conditional(i) || call) && offset + i.size < size && state[offset + i.size] == Free)
        {
            std::snprintf(text, sizeof(text), "%s at %X", call ? "after the call" : "condition not taken",
                          base + offset);
            seeds.push_back({offset + static_cast<u32>(i.size), offset, text});
        }
    }

    // Follows one seed: a block of instructions to its end, kept whole or
    // dropped whole at the first sign it is not code.
    void explore(const Seed& seed, int round)
    {
        if (seed.at >= size || state[seed.at] != Free)
            return;
        std::vector<std::pair<u32, Instruction>> taken;
        u32 offset = seed.at;
        bool ok = true;
        while (true)
        {
            if (offset >= size)
                break;
            if (state[offset] == Code)
                break;                          // joins known code
            if (state[offset] == Data)
            {
                ok = false;                     // runs into data
                break;
            }
            Instruction i = decode(rom, size, base, offset);
            if (invalid(i) || offset + i.size > size)
            {
                ok = false;
                break;
            }
            bool overlap = false;
            for (int k = 1; k < i.size; k++)
                overlap = overlap || state[offset + k] != Free;
            if (overlap)
            {
                ok = false;
                break;
            }
            if (rom[offset] == 0xFF && offset + 1 < size && rom[offset + 1] == 0xFF)
            {
                ok = false;                     // rst $38 twice: padding
                break;
            }
            const u8 kind = i.decoded.operandKind;
            if (!i.decoded.rst && !i.decoded.relative &&
                (kind == Z80Disassembler::Decoded::Operand_Jump ||
                 kind == Z80Disassembler::Decoded::Operand_Memory) &&
                !answers[i.decoded.operand])
            {
                ok = false;                     // a jump or access to nothing
                break;
            }
            taken.push_back({offset, i});
            if (ends_flow(i))
                break;
            offset += static_cast<u32>(i.size);
            if (taken.size() > 1024)
            {
                ok = false;
                break;
            }
        }
        if (!ok || taken.empty())
        {
            if (!taken.empty() || !ok)
                dropped++;
            return;
        }

        // Kept: its bytes are code from now on, as if run, for the tables.
        DeepSearch::Block block;
        block.start = seed.at;
        block.end = taken.back().first + static_cast<u32>(taken.back().second.size);
        block.from = seed.from;
        block.round = round;
        block.reason = seed.reason;
        int familiar = 0;
        for (const auto& step : taken)
        {
            for (int k = 0; k < step.second.size; k++)
                state[step.first + k] = Code;
            work.MarkExecuted(step.first, static_cast<u32>(step.second.size));
            auto it = style.find(style_key(rom, size, step.first));
            if (it != style.end() && it->second >= 2)
                familiar++;
        }
        block.score = static_cast<int>(100 * familiar / static_cast<int>(taken.size()));
        blocks.push_back(block);
        for (const auto& step : taken)
            edges(step.first, step.second, std::string());
    }
};
}

namespace
{
std::string hex_list(const std::vector<int>& values)
{
    // "0-1", "2-7", "0,3"
    std::string text;
    for (std::size_t i = 0; i < values.size();)
    {
        std::size_t j = i;
        while (j + 1 < values.size() && values[j + 1] == values[j] + 1)
            j++;
        char part[24];
        if (j > i)
            std::snprintf(part, sizeof(part), "%s%d-%d", text.empty() ? "" : ",", values[i], values[j]);
        else
            std::snprintf(part, sizeof(part), "%s%d", text.empty() ? "" : ",", values[i]);
        text += part;
        i = j + 1;
    }
    return text;
}

// 4. The data: for each stretch still not sure after the rounds.
std::vector<RomMap::DataHint> data_step(const u8* rom, u32 romSize, u16 base, const Search& search,
                                        const std::vector<JumpTables::Site>& sites)
{
    const RomMap& work = search.work;
    const std::vector<u8>& bytes = work.GetBytes();
    auto kind_at = [&](u32 o) { return static_cast<u8>(bytes[o] & RomMap::KindMask); };
    auto in_rom = [&](u16 a) { return a >= base && static_cast<u32>(a - base) < romSize; };
    auto word_at = [&](u32 o) { return static_cast<u16>(rom[o] | (rom[o + 1] << 8)); };

    // Who names what: every address a known instruction loads or reads.
    std::unordered_map<u32, std::vector<u32>> named;    // table offset -> sites
    for (u32 o = 0; o < romSize; o++)
    {
        if (kind_at(o) != RomMap::Kind_Code || !(bytes[o] & RomMap::InstructionStart))
            continue;
        const Instruction i = decode(rom, romSize, base, o);
        const u8 k = i.decoded.operandKind;
        if ((k == Z80Disassembler::Decoded::Operand_Imm16 || k == Z80Disassembler::Decoded::Operand_Memory) &&
            in_rom(i.decoded.operand))
            named[static_cast<u32>(i.decoded.operand - base)].push_back(o);
    }
    std::vector<u32> tableOffsets;
    for (const auto& n : named)
        tableOffsets.push_back(n.first);
    std::sort(tableOffsets.begin(), tableOffsets.end());
    const std::vector<u32> entries = JumpTables::RoutineEntries(rom, romSize, base, work);

    // A pointer for sure: a jump table entry, a TABLE word, an entry seen
    // in use. (A word in read data that happens to hit code is not enough:
    // $0F00 out of "00 0F" in Orguss's records.)
    std::unordered_set<u32> tableWords;
    for (const JumpTables::Site& site : sites)
        for (const JumpTables::Table& t : site.tables)
            for (u32 n = 0; n < t.entries && !t.vector; n++)
                tableWords.insert(t.base + n * 2);
    for (const RomMap::ObservedJump& seen : work.GetObservedJumps())
        tableWords.insert(seen.entry);
    for (const RomMap::ObservedPointer& seen : work.GetObservedPointers())
        tableWords.insert(seen.entry);
    auto pointer_word = [&](u32 o)
    {
        return o + 1 < romSize &&
               (tableWords.count(o) || (kind_at(o) == RomMap::Kind_Table && kind_at(o + 1) == RomMap::Kind_Table));
    };
    // Where a pointer can lead: an instruction, known or deduced, or a
    // lone ret nobody ran (Saverio: a series of C9 are the ends of routines).
    auto plausible_target = [&](u16 v)
    {
        if (!in_rom(v))
            return false;
        const u32 t = v - base;
        return (search.state[t] == Code && (bytes[t] & RomMap::InstructionStart)) || rom[t] == 0xC9 ||
               (kind_at(t) == RomMap::Kind_Code && (bytes[t] & RomMap::InstructionStart));
    };
    auto target_text = [&](u16 v)
    {
        char text[24];
        const u32 o = v - base;
        std::snprintf(text, sizeof(text), "%X%s", v, rom[o] == 0xC9 ? " (a ret)" : " (code)");
        return std::string(text);
    };
    // What play read of a table, as a period: "bytes 0-1 of every 8". The
    // map knows which bytes were read (any data kind); with no count from
    // the code, the pattern they make says the size of a record.
    auto read_kind = [&](u32 o)
    {
        const u8 k = kind_at(o);
        return k != RomMap::Kind_Unknown && k != RomMap::Kind_CodeCandidate && k != RomMap::Kind_Code &&
               k != RomMap::Kind_Fill;
    };
    auto period_of = [&](u32 table, u32 limit, std::vector<int>& fields)
    {
        const u32 span = std::min<u32>(limit - table, 512);
        // The smallest period whose columns agree with their majority -
        // read, or not read - clearly better than no period at all: some
        // records were read more than others (Orguss's $10CE: bytes 2-3 of
        // every 8 in most, 69% against 56% for the table as one column).
        u32 readAll = 0;
        for (u32 i = 0; i < span; i++)
            readAll += read_kind(table + i) ? 1 : 0;
        const u32 baseline = std::max(readAll, span - readAll) * 100 / std::max<u32>(span, 1);
        for (u32 p = 2; p <= 32; p++)
        {
            if (span < p * 4)
                break;
            std::vector<int> readCount(p, 0), total(p, 0);
            for (u32 i = 0; i < span; i++)
            {
                total[i % p]++;
                readCount[i % p] += read_kind(table + i) ? 1 : 0;
            }
            u32 agree = 0;
            bool someRead = false, someUnread = false;
            for (u32 c = 0; c < p; c++)
            {
                const bool read = readCount[c] * 2 > total[c];
                agree += static_cast<u32>(read ? readCount[c] : total[c] - readCount[c]);
                someRead = someRead || read;
                someUnread = someUnread || !read;
            }
            const u32 score = agree * 100 / span;
            if (!someRead || !someUnread || score < 65 || score < baseline + 10)
                continue;
            fields.clear();
            for (u32 c = 0; c < p; c++)
                if (readCount[c] * 2 > total[c])
                    fields.push_back(static_cast<int>(c));
            return static_cast<int>(p);
        }
        return 0;
    };

    std::vector<RomMap::DataHint> hints;
    u32 o = 0;
    while (o < romSize)
    {
        if (search.state[o] != Free)
        {
            o++;
            continue;
        }
        u32 end = o + 1;
        while (end < romSize && search.state[end] == Free)
            end++;
        const u32 start = o;
        o = end;
        const u32 length = end - start;

        // The table around it: the code that names an address at or before
        // it, in the stretch of bytes that are not code, nearest first.
        u32 regionStart = start;
        while (regionStart > 0 && start - regionStart < 1024 && search.state[regionStart - 1] != Code)
            regionStart--;
        u32 regionEnd = end;
        while (regionEnd < romSize && regionEnd - end < 1024 && search.state[regionEnd] != Code)
            regionEnd++;
        std::vector<std::pair<u32, u32>> candidates;       // table, site
        auto first = std::upper_bound(tableOffsets.begin(), tableOffsets.end(), end - 1);
        while (first != tableOffsets.begin() && candidates.size() < 12)
        {
            --first;
            if (*first < regionStart)
                break;
            for (u32 site : named[*first])
                if (candidates.size() < 12)
                    candidates.push_back({*first, site});
        }
        std::string hint;
        bool values = false;        // the code reads them as values
        for (const auto& c : candidates)
        {
            const JumpTables::DataRead r = JumpTables::AnalyzeData(rom, romSize, base, work, c.second, c.first,
                                                                  &entries);
            if (!r.found)
                continue;
            int entrySize = r.entrySize > 1 ? r.entrySize : 0;
            std::vector<int> fields = r.fields;
            // No size from the code: the pattern of what play read.
            std::vector<int> played;
            // Only when the code does not say: an index it knows the source
            // of, scaled by 1, is a table of bytes ($2866, 18 of them).
            const bool known = !r.index.empty() && r.index != "unknown";
            const int period = (entrySize || known) ? 0 : period_of(c.first, regionEnd, played);
            if (period)
            {
                entrySize = period;
                fields = played;
            }
            const u32 size = entrySize > 1 ? static_cast<u32>(entrySize) : 1;
            u32 reach = 0;              // where the table ends, when the code says
            if (r.copied)
                reach = c.first + r.copied;
            else if (r.entries > 0)
                reach = c.first + static_cast<u32>(r.entries) * size;
            else if (r.entrySize == 0 && !period && !r.fields.empty() && r.index.empty())
                reach = c.first + static_cast<u32>(r.fields.back()) + 1;
            // An index from a byte, never bounded: 256 entries at most
            // (Orguss's $6D39, by _RAM_C019_, ends at $6E38).
            else if (known && !period)
                reach = c.first + 256 * size;
            if (reach && reach < end)
                continue;               // this table stops before the stretch
            char text[256];
            if (r.copied)
                std::snprintf(text, sizeof(text), "data? copied whole from %X (%u bytes) at %X",
                              base + c.first, r.copied, base + r.reader);
            else
            {
                std::string details;
                char part[96];
                if (entrySize > 1)
                {
                    std::snprintf(part, sizeof(part), period ? ", %d-byte records? (from what play read)"
                                                             : ", %d-byte entries", entrySize);
                    details += part;
                }
                if (r.entries > 0)
                {
                    std::snprintf(part, sizeof(part), ", %d entries", r.entries);
                    details += part;
                }
                if (!r.index.empty() && r.index != "unknown")
                    details += ", indexed by " + r.index;
                if (entrySize > 1 && !fields.empty())
                {
                    details += ", bytes " + hex_list(fields) + " read";
                    std::vector<int> here;
                    for (u32 p = std::max(start, c.first); p < end; p++)
                    {
                        const int f = static_cast<int>((p - c.first) % size);
                        if (std::find(here.begin(), here.end(), f) == here.end())
                            here.push_back(f);
                    }
                    std::sort(here.begin(), here.end());
                    bool read = false;
                    for (int f : here)
                        read = read || std::find(fields.begin(), fields.end(), f) != fields.end();
                    if (!read && !here.empty())
                        details += "; here bytes " + hex_list(here) + ", never read";
                }
                else if (entrySize <= 1 && r.fields.size() > 1)
                    details += ", bytes +" + hex_list(r.fields) + " read at each index";
                if (r.pointers)
                    details += " - words used as addresses";
                else if (r.entrySize == 2 || (r.fields.size() >= 2 && r.fields[1] == r.fields[0] + 1))
                {
                    details += " - values, not addresses";
                    values = true;
                }
                std::snprintf(text, sizeof(text), "data? table at %X read at %X%s", base + c.first,
                              base + r.reader, details.c_str());
            }
            hint = text;
            break;
        }

        // Pointers around: an even number of bytes between them, every
        // word an address in the cartridge where code can start (Saverio) -
        // unless the code that reads the table takes them as values.
        if (!values && start >= 2 && pointer_word(start - 2) && pointer_word(end) && length % 2 == 0 &&
            length <= 64)
        {
            bool all = true;
            std::string targets;
            for (u32 p = start; p < end && all; p += 2)
            {
                const u16 v = word_at(p);
                all = plausible_target(v);
                if (all && p < start + 8)
                    targets += (targets.empty() ? "" : ", ") + target_text(v);
            }
            if (all)
            {
                char text[160];
                std::snprintf(text, sizeof(text), "pointers? %u word%s between pointers, to %s%s", length / 2,
                              length == 2 ? "" : "s", targets.c_str(), length > 8 ? ", ..." : "");
                hint = text;
            }
        }
        // Padding: the same $FF or $00 all along, where no table claims it
        // (Orguss's $6E39: four $FF before the code at $6E3D).
        if (hint.empty() && length >= 2 && (rom[start] == 0xFF || rom[start] == 0x00))
        {
            bool same = true;
            for (u32 p = start; p < end && same; p++)
                same = rom[p] == rom[start];
            if (same)
            {
                char text[96];
                std::snprintf(text, sizeof(text), "fill? %u x $%02X%s", length, rom[start],
                              end < romSize && search.state[end] == Code ? ", before code" : "");
                hint = text;
            }
        }
        // Nothing names it: what it sits in, when both sides are the same
        // kind of data (a byte the copy of a screen map skipped).
        if (hint.empty() && start > 0 && end < romSize && kind_at(start - 1) == kind_at(end) &&
            read_kind(start - 1))
        {
            RomMap::Range range = {start - 1, start - 1, kind_at(start - 1), false};
            hint = "data? inside " + RomMap::RangeName(range) + ", like its neighbours";
        }
        if (!hint.empty())
            hints.push_back({start, end, hint});
    }
    return hints;
}
}

namespace DeepSearch
{
std::string FormatDataHints(const std::vector<RomMap::DataHint>& hints)
{
    std::ostringstream out;
    out << "GearSF7000 data hints 1\n";
    out << "# What Deep Search made of bytes nobody read: suggestions, never proof.\n";
    out << "# ROM offsets, first-last, then the hint.\n";
    char line[64];
    for (const RomMap::DataHint& h : hints)
    {
        std::snprintf(line, sizeof(line), "data %04X-%04X ", h.start, h.end - 1);
        out << line << h.text << "\n";
    }
    return out.str();
}

bool LoadDataHints(const std::string& path, std::vector<RomMap::DataHint>& hints)
{
    hints.clear();
    std::ifstream in(path);
    std::string line;
    if (!in || !std::getline(in, line) || line.rfind("GearSF7000 data hints", 0) != 0)
        return false;
    while (std::getline(in, line))
    {
        std::istringstream words(line);
        std::string word, range;
        if (!(words >> word >> range) || word != "data")
            continue;
        const std::size_t dash = range.find('-');
        if (dash == std::string::npos)
            continue;
        RomMap::DataHint h;
        h.start = static_cast<u32>(std::strtoul(range.substr(0, dash).c_str(), NULL, 16));
        h.end = static_cast<u32>(std::strtoul(range.substr(dash + 1).c_str(), NULL, 16)) + 1;
        std::getline(words, h.text);
        h.text.erase(0, h.text.find_first_not_of(' '));
        if (h.end > h.start)
            hints.push_back(h);
    }
    return true;
}

std::string FormatDeduced(const std::vector<Block>& blocks)
{
    std::ostringstream out;
    out << "GearSF7000 deduced code 1\n";
    out << "# Code Deep Search deduced by following the flow out of known code: a guess\n";
    out << "# (CODE?), never proof. ROM offsets, first-last; score = % like the game's code.\n";
    char line[160];
    for (const Block& b : blocks)
    {
        std::snprintf(line, sizeof(line), "code %04X-%04X from %04X score %d round %d %s\n",
                      b.start, b.end - 1, b.from, b.score, b.round, b.reason.c_str());
        out << line;
    }
    return out.str();
}

bool LoadDeduced(const std::string& path, std::vector<RomMap::DeducedBlock>& blocks)
{
    blocks.clear();
    std::ifstream in(path);
    std::string line;
    if (!in || !std::getline(in, line) || line.rfind("GearSF7000 deduced code", 0) != 0)
        return false;
    while (std::getline(in, line))
    {
        std::istringstream words(line);
        std::string word, range, from, score, round;
        if (!(words >> word) || word != "code" || !(words >> range >> word >> from >> word >> score >> word >> round))
            continue;
        const std::size_t dash = range.find('-');
        if (dash == std::string::npos)
            continue;
        RomMap::DeducedBlock b;
        b.start = static_cast<u32>(std::strtoul(range.substr(0, dash).c_str(), NULL, 16));
        b.end = static_cast<u32>(std::strtoul(range.substr(dash + 1).c_str(), NULL, 16)) + 1;
        b.from = static_cast<u32>(std::strtoul(from.c_str(), NULL, 16));
        b.score = std::atoi(score.c_str());
        std::getline(words, b.reason);
        b.reason.erase(0, b.reason.find_first_not_of(' '));
        if (b.end > b.start)
            blocks.push_back(b);
    }
    return true;
}

Result Run(const u8* rom, u32 romSize, u16 base, const RomMap& map,
           const std::vector<u8>& answers,
           const std::vector<std::pair<u32, u32>>& kept,
           std::vector<JumpTables::Site> sites, const Options& options,
           std::atomic<int>* progress, std::atomic<bool>* cancel)
{
    Result result;
    result.sites = std::move(sites);
    if (map.GetBytes().size() != romSize || answers.size() < 0x10000)
        return result;
    Search search(rom, romSize, base, map, answers);

    // The ground: what ran, what the map knows is data; the rest is open.
    const std::vector<u8>& bytes = map.GetBytes();
    for (u32 o = 0; o < romSize; o++)
    {
        const u8 kind = bytes[o] & RomMap::KindMask;
        if (kind == RomMap::Kind_Code)
            search.state[o] = Code;
        else if (kind != RomMap::Kind_Unknown && kind != RomMap::Kind_CodeCandidate)
            search.state[o] = Data;
    }
    // The style of the game's own code, from what ran.
    for (u32 o = 0; o < romSize; o++)
        if ((bytes[o] & RomMap::KindMask) == RomMap::Kind_Code && (bytes[o] & RomMap::InstructionStart))
            search.style[style_key(rom, romSize, o)]++;
    // The pieces the user kept are code too.
    for (const auto& piece : kept)
        for (u32 o = piece.first; o < piece.second && o < romSize;)
        {
            const Instruction i = decode(rom, romSize, base, o);
            if (i.size <= 0)
                break;
            for (int k = 0; k < i.size && o + k < romSize; k++)
                search.state[o + k] = Code;
            search.work.MarkExecuted(o, static_cast<u32>(i.size));
            o += static_cast<u32>(i.size);
        }

    // First seeds: every edge of the known code, and the targets of the
    // table entries seen in use.
    for (u32 o = 0; o < romSize; o++)
    {
        const u8 raw = search.work.GetBytes()[o];
        if ((raw & RomMap::KindMask) != RomMap::Kind_Code || !(raw & RomMap::InstructionStart))
            continue;
        const Instruction i = decode(rom, romSize, base, o);
        if (i.size > 0)
            search.edges(o, i, std::string());
    }
    for (const RomMap::ObservedJump& seen : map.GetObservedJumps())
    {
        if (seen.entry + 1 >= romSize)
            continue;
        const u16 target = static_cast<u16>(rom[seen.entry] | (rom[seen.entry + 1] << 8));
        if (search.in_rom(target))
        {
            char text[64];
            std::snprintf(text, sizeof(text), "table entry seen in use at %X", base + seen.entry);
            search.seeds.push_back({static_cast<u32>(target - base), seen.entry, text});
        }
    }

    // Each entry of a table is data, its target a seed.
    auto seed_table = [&](const JumpTables::Table& t)
    {
        for (u32 n = 0; n < t.entries && !t.vector; n++)
        {
            const u32 at = t.base + n * 2;
            if (at + 1 >= romSize)
                break;
            if (search.state[at] == Free)
                search.state[at] = Data;
            if (search.state[at + 1] == Free)
                search.state[at + 1] = Data;
            const u16 target = static_cast<u16>(rom[at] | (rom[at + 1] << 8));
            if (search.in_rom(target))
            {
                char text[64];
                std::snprintf(text, sizeof(text), "entry %u of the jump table at %X", n + 1, base + t.base);
                search.seeds.push_back({static_cast<u32>(target - base), at, text});
            }
        }
    };

    // The tables an earlier run found are read again only if the code
    // around their jump changed, but their entries lead where they led:
    // seeds from the start, or what was reached only through them would be
    // lost at every run after the first (Orguss, 27/09: UNKNOWN 312 -> 363).
    std::vector<u32> tableBases;
    for (const JumpTables::Site& site : result.sites)
        for (const JumpTables::Table& t : site.tables)
        {
            tableBases.push_back(t.base);
            seed_table(t);
        }

    for (int round = 1; round <= options.maxRounds; round++)
    {
        if (cancel && cancel->load())
        {
            result.cancelled = true;
            break;
        }
        if (progress)
            progress->store(round);
        Round log = {round, 0, 0, 0, 0, 0};
        const std::size_t blocksBefore = search.blocks.size();
        const int droppedBefore = search.dropped;

        // 1. Jump tables, in all the code known so far.
        if (options.jumpTables)
        {
            for (u32 site : JumpTables::FindSites(rom, romSize, search.work))
            {
                const u32 signature = JumpTables::Signature(rom, romSize, base, search.work, site);
                JumpTables::Site* known = nullptr;
                for (JumpTables::Site& s : result.sites)
                    if (s.offset == site)
                        known = &s;
                if (known && known->signature == signature)
                    continue;
                JumpTables::Site found = JumpTables::Analyze(rom, romSize, base, search.work, site);
                for (const JumpTables::Table& t : found.tables)
                {
                    const bool isNew = std::find(tableBases.begin(), tableBases.end(), t.base) == tableBases.end();
                    if (isNew)
                    {
                        tableBases.push_back(t.base);
                        log.tables++;
                        log.entries += static_cast<int>(t.entries);
                    }
                    seed_table(t);
                }
                if (known)
                    *known = found;
                else
                    result.sites.push_back(found);
            }
        }

        // 2. Guided disassembly, until no seed is left.
        if (options.guided)
        {
            while (!search.seeds.empty())
            {
                if (cancel && cancel->load())
                    break;
                const Seed seed = search.seeds.front();
                search.seeds.pop_front();
                search.explore(seed, round);
            }
        }
        else
            search.seeds.clear();

        log.blocks = static_cast<int>(search.blocks.size() - blocksBefore);
        for (std::size_t b = blocksBefore; b < search.blocks.size(); b++)
            log.bytes += search.blocks[b].end - search.blocks[b].start;
        log.dropped = search.dropped - droppedBefore;
        result.rounds.push_back(log);
        if (log.tables == 0 && log.blocks == 0)
            break;
    }
    result.blocks = std::move(search.blocks);
    if (options.data && !result.cancelled)
        result.dataHints = data_step(rom, romSize, base, search, result.sites);
    return result;
}
}
