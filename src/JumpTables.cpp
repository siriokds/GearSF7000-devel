/*
 * GearSF7000 - jump tables, read from the code that jumps through them
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "JumpTables.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

#include "RomMap.h"
#include "Z80Disassembler.h"

namespace
{
// ---- the ROM as executed instructions ------------------------------------

struct Instruction
{
    u32 offset;
    int size;
    std::string text;       // upper case, no markers: "LD HL,$1741"
    Z80Disassembler::Decoded decoded;
};

bool executed_start(const RomMap& map, u32 offset)
{
    const std::vector<u8>& bytes = map.GetBytes();
    return offset < bytes.size() && (bytes[offset] & RomMap::KindMask) == RomMap::Kind_Code &&
           (bytes[offset] & RomMap::InstructionStart) != 0;
}

Instruction decode(const u8* rom, u32 romSize, u16 base, u32 offset)
{
    u8 bytes[16] = {};
    for (u32 i = 0; i < 16 && offset + i < romSize; i++)
        bytes[i] = rom[offset + i];
    Instruction instruction;
    instruction.offset = offset;
    Z80Disassembler::Decode(bytes, 16, static_cast<u16>(base + offset), instruction.decoded);
    instruction.size = instruction.decoded.size;
    for (const char* p = instruction.decoded.name; *p; p++)
    {
        if (p[0] == '{' && p[1] && p[2] == '}')
        {
            p += 2;
            continue;
        }
        instruction.text += static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
    }
    return instruction;
}

bool ends_flow(const Instruction& i)
{
    const std::string& t = i.text;
    if (t == "RET" || t == "RETI" || t == "RETN" || t == "JP (HL)" || t == "JP (IX)" || t == "JP (IY)")
        return true;
    return (t.compare(0, 3, "JP ") == 0 || t.compare(0, 3, "JR ") == 0) && t.find(',') == std::string::npos;
}

// The executed instruction ending right where offset starts, if any.
bool previous(const u8* rom, u32 romSize, u16 base, const RomMap& map, u32 offset, Instruction& out)
{
    for (u32 back = 1; back <= 4 && back <= offset; back++)
    {
        const u32 p = offset - back;
        if (!executed_start(map, p))
            continue;
        Instruction i = decode(rom, romSize, base, p);
        if (static_cast<u32>(i.size) == back)
        {
            out = i;
            return true;
        }
    }
    return false;
}

// Routine entries: the targets of executed calls and rst.
std::set<u32> call_targets(const u8* rom, u32 romSize, u16 base, const RomMap& map)
{
    std::set<u32> targets;
    for (u32 o = 0; o < romSize; o++)
    {
        if (!executed_start(map, o))
            continue;
        const u8 op = rom[o];
        if (op == 0xCD || (op & 0xC7) == 0xC4 || (op & 0xC7) == 0xC7)
        {
            Instruction i = decode(rom, romSize, base, o);
            if (i.decoded.subroutine && i.decoded.jumpAddress >= base &&
                static_cast<u32>(i.decoded.jumpAddress - base) < romSize)
                targets.insert(static_cast<u32>(i.decoded.jumpAddress - base));
        }
    }
    return targets;
}

// The routine a site is in: back through executed straight-line code to a
// routine entry, or to where the flow could not have come from above.
u32 routine_entry(const u8* rom, u32 romSize, u16 base, const RomMap& map,
                  const std::set<u32>& entries, u32 site, int limit = 40)
{
    u32 current = site;
    for (int steps = 0; steps < limit; steps++)
    {
        if (current != site && entries.count(current))
            return current;
        Instruction before;
        if (!previous(rom, romSize, base, map, current, before) || ends_flow(before))
            return current;
        current = before.offset;
    }
    return current;
}

std::vector<Instruction> window(const u8* rom, u32 romSize, u16 base, u32 from, u32 to)
{
    std::vector<Instruction> list;
    for (u32 o = from; o < to && o < romSize;)
    {
        Instruction i = decode(rom, romSize, base, o);
        if (i.size <= 0)
            break;
        list.push_back(i);
        o += static_cast<u32>(i.size);
    }
    return list;
}

// ---- the tracer -----------------------------------------------------------

// What a register holds, as far as building a table address goes.
struct Sym
{
    enum Kind : u8 { Unknown, Const, Index, Address, Word, Byte } kind = Unknown;
    int value = 0;          // Const; the table's address when baseKnown
    bool baseKnown = false;
    std::string index;      // the index's source
    int scale = 0;          // how many times the index is added
    int offset = 0;
    int limit = 0;          // entries the code allows (cp n / and mask)
    bool fromRam = false;   // a Word read at a constant RAM address
    bool fromStack = false; // taken off the stack: a return address
};

Sym constant(int v) { Sym s; s.kind = Sym::Const; s.value = v & 0xFFFF; return s; }

// A value the code scales and adds to an address is an index, even when
// where it came from is not known.
Sym unknown_index(int scale)
{
    Sym s;
    s.kind = Sym::Index;
    s.index = "unknown";
    s.scale = scale;
    return s;
}

enum Reg8 { A, B, C, D, E, H, L, Reg8Count };
enum Reg16 { BC, DE, HL, IX, IY, Reg16Count };

struct State
{
    Sym r8[Reg8Count];
    Sym r16[Reg16Count];
    bool r16Valid[Reg16Count] = {};
    std::vector<Sym> stack;
    int pendingCp = -1;

    void clear()
    {
        for (Sym& s : r8) s = Sym();
        for (int i = 0; i < Reg16Count; i++) { r16[i] = Sym(); r16Valid[i] = true; }
        stack.clear();
        pendingCp = -1;
    }
};

int pair_of(Reg8 r)
{
    switch (r)
    {
        case B: case C: return BC;
        case D: case E: return DE;
        case H: case L: return HL;
        default: return -1;
    }
}

void set8(State& s, Reg8 r, const Sym& v)
{
    s.r8[r] = v;
    const int p = pair_of(r);
    if (p >= 0)
        s.r16Valid[p] = false;
}

Sym get16(const State& s, Reg16 p)
{
    if (s.r16Valid[p] || p == IX || p == IY)
        return s.r16[p];
    const Sym& hi = s.r8[p == BC ? B : (p == DE ? D : H)];
    const Sym& lo = s.r8[p == BC ? C : (p == DE ? E : L)];
    if (hi.kind == Sym::Const && lo.kind == Sym::Const)
        return constant(hi.value * 256 + lo.value);
    if (hi.kind == Sym::Const && hi.value == 0 && (lo.kind == Sym::Index || lo.kind == Sym::Const))
        return lo;
    if (hi.kind == Sym::Const && hi.value == 0 && lo.kind == Sym::Unknown)
        return unknown_index(1);
    // An index scaled in the low byte (sla e) with the high byte not set in
    // the code seen: a small index all the same.
    if (hi.kind == Sym::Unknown && lo.kind == Sym::Index && lo.scale > 1)
        return lo;
    // A word read byte by byte: the low byte at X, the high at X+1.
    if (lo.kind == Sym::Byte && hi.kind == Sym::Byte && hi.offset == lo.offset + 1 &&
        hi.baseKnown == lo.baseKnown && hi.value == lo.value && hi.index == lo.index &&
        hi.scale == lo.scale)
    {
        Sym w = lo;
        w.kind = Sym::Word;
        return w;
    }
    return Sym();
}

void set16(State& s, Reg16 p, const Sym& v)
{
    s.r16[p] = v;
    s.r16Valid[p] = true;
    if (p == BC) { s.r8[B] = Sym(); s.r8[C] = Sym(); }
    if (p == DE) { s.r8[D] = Sym(); s.r8[E] = Sym(); }
    if (p == HL) { s.r8[H] = Sym(); s.r8[L] = Sym(); }
}

Sym add16(const Sym& a, const Sym& b)
{
    if (a.kind == Sym::Const && b.kind == Sym::Const)
        return constant(a.value + b.value);
    const Sym* addr = (a.kind == Sym::Const || a.kind == Sym::Address) ? &a : nullptr;
    const Sym* other = addr == &a ? &b : &a;
    if (!addr && (b.kind == Sym::Const || b.kind == Sym::Address))
    {
        addr = &b;
        other = &a;
    }
    if (a.kind == Sym::Index && b.kind == Sym::Index && a.index == b.index)
    {
        Sym s = a;
        s.scale += b.scale;
        s.limit = std::max(a.limit, b.limit);
        return s;
    }
    if (!addr)
        return Sym();
    Sym s = *addr;
    if (s.kind == Sym::Const)
    {
        s.kind = Sym::Address;
        s.baseKnown = true;
        s.offset = 0;
        s.scale = 0;
        s.index.clear();
    }
    if (other->kind == Sym::Const)
    {
        s.offset += other->value;
        return s;
    }
    if (other->kind == Sym::Index && (s.scale == 0 || s.index == other->index))
    {
        s.index = other->index;
        s.scale += other->scale;
        s.limit = std::max(s.limit, other->limit);
        return s;
    }
    return Sym();
}

bool parse_number(const std::string& text, int& value)
{
    if (text.size() < 2 || text[0] != '$')
        return false;
    value = static_cast<int>(std::strtol(text.c_str() + 1, NULL, 16));
    return true;
}

int reg8(const std::string& name)
{
    static const char* names[] = {"A", "B", "C", "D", "E", "H", "L"};
    for (int i = 0; i < 7; i++)
        if (name == names[i])
            return i;
    return -1;
}

int reg16(const std::string& name)
{
    static const char* names[] = {"BC", "DE", "HL", "IX", "IY"};
    for (int i = 0; i < 5; i++)
        if (name == names[i])
            return i;
    return -1;
}

std::string ram_name(int address)
{
    char text[16];
    std::snprintf(text, sizeof(text), "_RAM_%X_", address);
    return text;
}

void forget_all(State& s)
{
    s.clear();
}

// One instruction on the state: what matters for building an address is
// followed, anything else that writes a register makes it unknown.
void step(State& s, const Instruction& i)
{
    const std::string& t = i.text;
    const std::size_t space = t.find(' ');
    const std::string op = t.substr(0, space);
    std::string first, second;
    if (space != std::string::npos)
    {
        const std::string args = t.substr(space + 1);
        const std::size_t comma = args.find(',');
        first = args.substr(0, comma);
        if (comma != std::string::npos)
            second = args.substr(comma + 1);
    }
    const int pendingCp = s.pendingCp;
    s.pendingCp = -1;
    int n = 0;

    if (op == "LD")
    {
        const int d8 = reg8(first), d16 = reg16(first);
        if (d8 >= 0)
        {
            const int s8 = reg8(second);
            if (s8 >= 0)
                set8(s, static_cast<Reg8>(d8), s.r8[s8]);
            else if (parse_number(second, n))
                set8(s, static_cast<Reg8>(d8), constant(n & 0xFF));
            else if (second == "(HL)" || second == "(DE)" || second == "(BC)")
            {
                Sym at = get16(s, static_cast<Reg16>(reg16(second.substr(1, 2))));
                Sym b;
                if (at.kind == Sym::Address || at.kind == Sym::Const)
                {
                    b = at;
                    if (at.kind == Sym::Const)
                    {
                        b.baseKnown = true;
                        b.offset = 0;
                        b.scale = 0;
                    }
                    b.kind = Sym::Byte;
                }
                set8(s, static_cast<Reg8>(d8), b);
            }
            else if (second.compare(0, 3, "(IX") == 0 || second.compare(0, 3, "(IY") == 0)
            {
                // ld e,(ix+4): an index read from a structure, named after it.
                Sym index;
                index.kind = Sym::Index;
                index.index = second;
                for (char& c : index.index)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                index.scale = 1;
                set8(s, static_cast<Reg8>(d8), index);
            }
            else if (second.size() > 2 && second[0] == '(' && second[1] == '$' && d8 == A)
            {
                // ld a,(nn): the index, named after its variable.
                Sym index;
                index.kind = Sym::Index;
                index.index = ram_name(static_cast<int>(std::strtol(second.c_str() + 2, NULL, 16)));
                index.scale = 1;
                set8(s, A, index);
            }
            else
                set8(s, static_cast<Reg8>(d8), Sym());
            return;
        }
        if (d16 >= 0)
        {
            if (parse_number(second, n))
                set16(s, static_cast<Reg16>(d16), constant(n));
            else if (second.size() > 2 && second[0] == '(' && second[1] == '$')
            {
                // ld hl,(nn): an address kept in RAM - a function pointer.
                Sym w;
                w.kind = Sym::Word;
                w.fromRam = true;
                w.value = static_cast<int>(std::strtol(second.c_str() + 2, NULL, 16));
                set16(s, static_cast<Reg16>(d16), w);
            }
            else
                set16(s, static_cast<Reg16>(d16), Sym());
            return;
        }
        return;     // a store: no register changes
    }
    if (op == "SLA" && reg8(first) >= 0 && first != "A")
    {
        const Reg8 r = static_cast<Reg8>(reg8(first));
        Sym v = s.r8[r];
        if (v.kind == Sym::Index)
            v.scale *= 2;
        else if (v.kind == Sym::Const)
            v.value = (v.value * 2) & 0xFF;
        else
            v = unknown_index(2);
        set8(s, r, v);
        return;
    }
    if ((op == "ADD" && first == "A" && second == "A") || (op == "SLA" && first == "A") ||
        op == "RLCA")
    {
        Sym a = s.r8[A];
        if (a.kind == Sym::Index)
            a.scale *= 2;
        else if (a.kind == Sym::Const)
            a.value = (a.value * 2) & 0xFF;
        else
            a = unknown_index(2);
        set8(s, A, a);
        return;
    }
    if (op == "AND" && parse_number(first, n))
    {
        Sym a = s.r8[A];
        if (a.kind == Sym::Index && ((n + 1) & n) == 0)
            a.limit = n + 1;        // a mask of low bits: that many entries
        else if (a.kind == Sym::Const)
            a.value &= n;
        else
            a = Sym();
        set8(s, A, a);
        return;
    }
    if (op == "CP" && parse_number(first, n))
    {
        if (s.r8[A].kind == Sym::Index && s.r8[A].scale == 1)
            s.pendingCp = n;
        return;
    }
    if ((op == "RET" || op == "JR" || op == "JP") && (first == "NC" || first == "C") && pendingCp > 0)
    {
        // cp n / ret nc: fewer than n entries - for every register holding
        // that index, copied before the cp (ld e,a / cp $12, Orguss $0C20).
        const Sym index = s.r8[A];
        for (Sym& r : s.r8)
            if (r.kind == Sym::Index && r.index == index.index && r.scale == index.scale)
                r.limit = pendingCp;
        return;
    }
    if (op == "ADD" && reg16(first) >= 0 && reg16(second) >= 0)
    {
        const Reg16 d = static_cast<Reg16>(reg16(first));
        const Sym a = get16(s, d), b = get16(s, static_cast<Reg16>(reg16(second)));
        set16(s, d, add16(a, b));
        return;
    }
    if ((op == "INC" || op == "DEC") && reg16(first) >= 0)
    {
        const Reg16 d = static_cast<Reg16>(reg16(first));
        Sym v = get16(s, d);
        if (v.kind == Sym::Address)
            v.offset += op == "INC" ? 1 : -1;
        else if (v.kind == Sym::Const)
            v.value = (v.value + (op == "INC" ? 1 : -1)) & 0xFFFF;
        else
            v = Sym();
        set16(s, d, v);
        return;
    }
    if (op == "EX" && first == "DE" && second == "HL")
    {
        std::swap(s.r8[D], s.r8[H]);
        std::swap(s.r8[E], s.r8[L]);
        std::swap(s.r16[DE], s.r16[HL]);
        std::swap(s.r16Valid[DE], s.r16Valid[HL]);
        return;
    }
    if (op == "PUSH" && reg16(first) >= 0)
    {
        s.stack.push_back(get16(s, static_cast<Reg16>(reg16(first))));
        return;
    }
    if (op == "POP" && reg16(first) >= 0)
    {
        Sym v;
        if (!s.stack.empty())
        {
            v = s.stack.back();
            s.stack.pop_back();
        }
        else
            v.fromStack = true;     // more pops than pushes: the caller's return address
        set16(s, static_cast<Reg16>(reg16(first)), v);
        return;
    }
    if (op == "PUSH" || op == "POP")
    {
        if (op == "PUSH") s.stack.push_back(Sym());
        else if (!s.stack.empty()) s.stack.pop_back();
        return;
    }
    if (op == "CPIR" || op == "CPDR")
    {
        // A search: BC ends as where the value was found - an index, with
        // as many entries as values searched (ld bc,n before it).
        const Sym count = get16(s, BC);
        Sym found;
        found.kind = Sym::Index;
        found.index = op == "CPIR" ? "a search (cpir)" : "a search (cpdr)";
        found.scale = 1;
        found.limit = count.kind == Sym::Const ? count.value : 0;
        set16(s, BC, found);
        set16(s, HL, Sym());
        return;
    }
    if (op == "EX" && first == "(SP)" && reg16(second) >= 0)
    {
        Sym v;
        v.fromStack = true;
        set16(s, static_cast<Reg16>(reg16(second)), v);
        return;
    }
    if (op == "EXX" || op == "EX" || op == "CALL" || op == "RST" || op == "LDIR" ||
        op == "LDDR" || op == "LDI" || op == "LDD" || op == "INIR" || op == "OTIR")
    {
        forget_all(s);
        return;
    }
    if (op == "DJNZ")
    {
        set8(s, B, Sym());
        return;
    }
    // Anything else that writes a register: it is no longer known.
    static const char* aluA[] = {"SUB", "AND", "OR", "XOR", "NEG", "CPL", "DAA", "RLA", "RRA", "RRCA"};
    for (const char* w : aluA)
        if (op == w)
        {
            set8(s, A, Sym());
            return;
        }
    if (op == "ADD" || op == "ADC" || op == "SBC")
    {
        if (reg16(first) >= 0)
            set16(s, static_cast<Reg16>(reg16(first)), Sym());
        else
            set8(s, A, Sym());
        return;
    }
    if (op == "CP" || op == "OUT" || op == "BIT" || op == "JP" || op == "JR" || op == "RET" ||
        op == "NOP" || op == "DI" || op == "EI" || op == "HALT" || op == "SCF" || op == "CCF" ||
        op == "IM")
        return;
    const std::string& target = (op == "SET" || op == "RES") ? second : first;
    if (reg8(target) >= 0)
        set8(s, static_cast<Reg8>(reg8(target)), Sym());
    else if (reg16(target) >= 0)
        set16(s, static_cast<Reg16>(reg16(target)), Sym());
}

// A table from the address the jump used: entries of two bytes.
bool table_from(const Sym& v, u16 base, u32 romSize, JumpTables::Table& table, std::string& reason)
{
    if (v.fromStack)
    {
        reason = "a return address taken from the stack, not a table";
        return false;
    }
    if (v.kind == Sym::Address && v.baseKnown && v.scale == 3)
    {
        // jp (hl) into the table itself: each entry a 3-byte jp.
        const int address = (v.value + v.offset) & 0xFFFF;
        if (address < base || static_cast<u32>(address - base) >= romSize)
        {
            reason = "the table is not in the cartridge";
            return false;
        }
        table.base = static_cast<u32>(address - base);
        table.entries = static_cast<u32>(v.limit);
        table.counted = v.limit > 0;
        table.index = v.index.empty() ? "unknown" : v.index;
        table.vector = true;
        return true;
    }
    if (v.kind == Sym::Word && v.fromRam)
    {
        char text[80];
        std::snprintf(text, sizeof(text), "address kept in RAM (%s): a function pointer",
                      ram_name(v.value).c_str());
        reason = text;
        return false;
    }
    if (v.kind != Sym::Word || !v.baseKnown)
    {
        reason = "the address is not read from a table";
        return false;
    }
    if (v.scale != 2)
    {
        char text[64];
        std::snprintf(text, sizeof(text), "entries of %d bytes, not a table of addresses", v.scale);
        reason = v.scale == 0 ? "a fixed address, no index" : text;
        return false;
    }
    const int address = (v.value + v.offset) & 0xFFFF;
    if (address < base || static_cast<u32>(address - base) >= romSize)
    {
        reason = "the table is not in the cartridge";
        return false;
    }
    table.base = static_cast<u32>(address - base);
    table.entries = static_cast<u32>(v.limit);
    table.counted = v.limit > 0;
    table.index = v.index.empty() ? "unknown" : v.index;
    return true;
}

// Without a count from the code, the entries go on while they point into
// the cartridge at code or at bytes nobody has classified, and stop at the
// first that does not, at executed code, or after 128.
u32 plausible_entries(const u8* rom, u32 romSize, u16 base, const RomMap& map, u32 start)
{
    const std::vector<u8>& bytes = map.GetBytes();
    u32 n = 0;
    for (u32 o = start; o + 1 < romSize && n < 128; o += 2, n++)
    {
        const u8 k0 = bytes[o] & RomMap::KindMask, k1 = bytes[o + 1] & RomMap::KindMask;
        if (k0 == RomMap::Kind_Code || k1 == RomMap::Kind_Code)
            break;
        const u16 target = static_cast<u16>(rom[o] | (rom[o + 1] << 8));
        if (target < base || static_cast<u32>(target - base) >= romSize)
            break;
        const u8 kind = bytes[target - base] & RomMap::KindMask;
        if (kind != RomMap::Kind_Code && kind != RomMap::Kind_Unknown &&
            kind != RomMap::Kind_CodeCandidate)
            break;
    }
    return n;
}

Reg16 jump_register(const u8* rom, u32 site)
{
    return rom[site] == 0xDD ? IX : (rom[site] == 0xFD ? IY : HL);
}

void entry_state(State& s)
{
    s.clear();
    // Unknown on entry, but an address the caller may have set: a table.
    for (int p = 0; p < Reg16Count; p++)
    {
        s.r16[p].kind = Sym::Address;
        s.r16[p].baseKnown = false;
    }
    s.r8[A].kind = Sym::Index;
    s.r8[A].index = "unknown";
    s.r8[A].scale = 1;
}
}

namespace JumpTables
{
std::vector<u32> FindSites(const u8* rom, u32 romSize, const RomMap& map)
{
    std::vector<u32> sites;
    if (map.GetBytes().size() != romSize)
        return sites;
    for (u32 o = 0; o + 1 < romSize; o++)
    {
        if (!executed_start(map, o))
            continue;
        if (rom[o] == 0xE9 || ((rom[o] == 0xDD || rom[o] == 0xFD) && rom[o + 1] == 0xE9))
            sites.push_back(o);
    }
    return sites;
}

u32 Signature(const u8* rom, u32 romSize, u16 base, const RomMap& map, u32 site)
{
    // The executed bytes before the site, back to where straight-line code
    // stops, and every executed call into the cartridge: new code there or
    // a new caller is what could change the answer.
    const std::set<u32> entries = call_targets(rom, romSize, base, map);
    const u32 entry = routine_entry(rom, romSize, base, map, entries, site);
    return (site - entry) * 131u + static_cast<u32>(entries.size()) * 7u + entry;
}

std::vector<u32> RoutineEntries(const u8* rom, u32 romSize, u16 base, const RomMap& map)
{
    const std::set<u32> targets = call_targets(rom, romSize, base, map);
    return std::vector<u32>(targets.begin(), targets.end());
}

DataRead AnalyzeData(const u8* rom, u32 romSize, u16 base, const RomMap& map, u32 site, u32 table,
                     const std::vector<u32>* routineEntries)
{
    DataRead result;
    result.site = site;
    result.table = table;
    if (map.GetBytes().size() != romSize || site >= romSize)
    {
        result.reason = "no map for this ROM";
        return result;
    }
    const int target = static_cast<int>(base + table);
    // The index is often set before the table is named (ld a,(nn) / ld e,a
    // / ld hl,T / add hl,de): from the routine's entry.
    const std::set<u32> entries = routineEntries
        ? std::set<u32>(routineEntries->begin(), routineEntries->end())
        : call_targets(rom, romSize, base, map);
    const u32 entry = routine_entry(rom, romSize, base, map, entries, site);
    State state;
    state.clear();
    for (const Instruction& i : window(rom, romSize, base, entry, site))
        step(state, i);

    // What an address register holds, when it is built from T.
    auto from_table = [target](const Sym& v)
    {
        return (v.kind == Sym::Address || v.kind == Sym::Byte || v.kind == Sym::Word) && v.baseKnown &&
               v.value == target;
    };
    auto note_read = [&](const Instruction& i, const Sym& at, int displacement)
    {
        if (!result.found)
        {
            result.found = true;
            result.reader = i.offset;
            result.entrySize = at.scale;
            result.index = at.index.empty() ? std::string() : at.index;
            result.entries = at.limit;
        }
        int field = at.offset + displacement;
        if (result.entrySize > 1)
            field = ((field % result.entrySize) + result.entrySize) % result.entrySize;
        if (std::find(result.fields.begin(), result.fields.end(), field) == result.fields.end())
            result.fields.push_back(field);
    };

    // Forward from the site: straight-line code, into unconditional calls
    // and jumps (two levels), a conditional branch taken as not taken.
    u32 pc = site;
    int depth = 0;
    for (int steps = 0; steps < 64 && pc < romSize; steps++)
    {
        const Instruction i = decode(rom, romSize, base, pc);
        if (i.size <= 0)
            break;
        const std::string& t = i.text;
        // A read through a register pair, or an index register plus d.
        static const char* pairs[] = {"(HL)", "(DE)", "(BC)"};
        for (int p = 0; p < 3; p++)
        {
            const std::size_t at = t.find(pairs[p]);
            // A store (ld (hl),a) reads nothing; a jump through it is a site.
            if (at == std::string::npos || t.compare(0, 3, "JP ") == 0 ||
                (t.compare(0, 3, "LD ") == 0 && at == 3))
                continue;
            const Sym v = get16(state, static_cast<Reg16>(p == 0 ? HL : (p == 1 ? DE : BC)));
            if (v.kind == Sym::Word && v.baseKnown && v.value == target)
                result.pointers = true;         // a word from T, read through
            else if (from_table(v))
                note_read(i, v, 0);
        }
        for (Reg16 r : {IX, IY})
        {
            const char* name = r == IX ? "(IX" : "(IY";
            const std::size_t at = t.find(name);
            if (at == std::string::npos || (t.compare(0, 3, "LD ") == 0 && at == 3))
                continue;
            int displacement = 0;
            const std::size_t sign = at + 3;
            if (sign < t.size() && (t[sign] == '+' || t[sign] == '-'))
            {
                displacement = static_cast<int>(std::strtol(t.c_str() + sign + 2, NULL, 16));
                if (t[sign] == '-')
                    displacement = -displacement;
            }
            const Sym v = get16(state, r);
            if (from_table(v) || (v.kind == Sym::Const && v.value == target))
            {
                Sym record = v;
                if (record.kind == Sym::Const)
                {
                    record.kind = Sym::Address;
                    record.baseKnown = true;
                    record.offset = 0;
                }
                note_read(i, record, displacement);
            }
        }
        // A block copied whole: ldir / otir from T with a count in BC or B.
        if (t == "LDIR" || t == "OTIR" || t == "LDI" || t == "OUTI")
        {
            const Sym v = get16(state, HL);
            if (from_table(v) || (v.kind == Sym::Const && v.value == target))
            {
                const Sym count = t == "LDIR" ? get16(state, BC) : state.r8[B];
                if (count.kind == Sym::Const)
                    result.copied = static_cast<u32>(count.value ? count.value : (t == "OTIR" ? 256 : 65536));
                if (!result.found)
                {
                    result.found = true;
                    result.reader = i.offset;
                }
            }
        }
        // A word from T jumped to: a pointer (a table of routines).
        if (t == "JP (HL)" || t == "JP (IX)" || t == "JP (IY)")
        {
            const Sym v = get16(state, jump_register(rom, pc));
            if (v.kind == Sym::Word && v.baseKnown && v.value == target)
                result.pointers = true;
            break;
        }

        // Where the flow goes next.
        const bool unconditional = i.decoded.jump && !i.decoded.rst &&
                                   t.find(',') == std::string::npos;
        if (unconditional && i.decoded.subroutine && depth < 2 && i.decoded.jumpAddress >= base &&
            static_cast<u32>(i.decoded.jumpAddress - base) < romSize)
        {
            depth++;                            // into the helper, registers kept
            pc = static_cast<u32>(i.decoded.jumpAddress - base);
            continue;
        }
        if (unconditional && !i.decoded.subroutine && i.decoded.jumpAddress >= base &&
            static_cast<u32>(i.decoded.jumpAddress - base) < romSize)
        {
            pc = static_cast<u32>(i.decoded.jumpAddress - base);
            continue;
        }
        if (t == "RET" || t == "RETI" || t == "RETN" || t == "HALT")
            break;
        if (i.decoded.subroutine || t.compare(0, 3, "RST") == 0)
        {
            pc += static_cast<u32>(i.size);     // a call not followed: what it changes is not known
            forget_all(state);
            continue;
        }
        step(state, i);
        pc += static_cast<u32>(i.size);
    }
    if (!result.found)
        result.reason = "no read from the table in the code after it";
    std::sort(result.fields.begin(), result.fields.end());
    return result;
}

bool Load(const std::string& path, std::vector<Site>& sites)
{
    sites.clear();
    std::ifstream in(path);
    std::string line;
    if (!in || !std::getline(in, line) || line.rfind("GearSF7000 jump tables", 0) != 0)
        return false;
    while (std::getline(in, line))
    {
        // site 044E sig 123 table 0453 64 counted pointers caller 044E index _RAM_C034_
        // site 0B93 sig 99 none a return address taken from the stack, not a table
        std::istringstream words(line);
        std::string word, site, sig;
        if (!(words >> word) || word != "site" || !(words >> site >> word >> sig >> word))
            continue;
        const u32 offset = static_cast<u32>(std::strtoul(site.c_str(), NULL, 16));
        Site* entry = NULL;
        for (Site& known : sites)
            if (known.offset == offset)
                entry = &known;
        if (!entry)
        {
            sites.push_back(Site());
            entry = &sites.back();
            entry->offset = offset;
        }
        entry->signature = static_cast<u32>(std::strtoul(sig.c_str(), NULL, 10));
        if (word == "none")
        {
            std::getline(words, entry->reason);
            entry->reason.erase(0, entry->reason.find_first_not_of(' '));
            continue;
        }
        Table table;
        std::string base, entries, counted, kind, callerWord, caller, indexWord;
        words >> base >> entries >> counted >> kind >> callerWord >> caller >> indexWord;
        table.base = static_cast<u32>(std::strtoul(base.c_str(), NULL, 16));
        table.entries = static_cast<u32>(std::strtoul(entries.c_str(), NULL, 10));
        table.counted = counted == "counted";
        table.vector = kind == "vector";
        table.caller = static_cast<u32>(std::strtoul(caller.c_str(), NULL, 16));
        std::getline(words, table.index);
        table.index.erase(0, table.index.find_first_not_of(' '));
        entry->tables.push_back(table);
    }
    return true;
}

std::string Format(const std::vector<Site>& sites)
{
    std::ostringstream out;
    out << "GearSF7000 jump tables 1\n";
    out << "# Each executed jp (hl)/(ix)/(iy): the tables its code reads, or why none.\n";
    out << "# ROM offsets in hex. Found by Deep Search, reading the code; nothing observed.\n";
    for (const Site& site : sites)
    {
        char head[48];
        std::snprintf(head, sizeof(head), "site %04X sig %u ", site.offset, site.signature);
        if (site.tables.empty())
            out << head << "none " << site.reason << "\n";
        for (const Table& t : site.tables)
        {
            char line[160];
            std::snprintf(line, sizeof(line), "table %04X %u %s %s caller %04X index %s\n",
                          t.base, t.entries, t.counted ? "counted" : "guessed",
                          t.vector ? "vector" : "pointers", t.caller, t.index.c_str());
            out << head << line;
        }
    }
    return out.str();
}

Site Analyze(const u8* rom, u32 romSize, u16 base, const RomMap& map, u32 siteOffset)
{
    Site result;
    result.offset = siteOffset;
    result.signature = Signature(rom, romSize, base, map, siteOffset);
    if (map.GetBytes().size() != romSize || siteOffset >= romSize)
    {
        result.reason = "no map for this ROM";
        return result;
    }
    const std::set<u32> entries = call_targets(rom, romSize, base, map);
    const u32 entry = routine_entry(rom, romSize, base, map, entries, siteOffset);
    const std::vector<Instruction> body = window(rom, romSize, base, entry, siteOffset);
    const Reg16 reg = jump_register(rom, siteOffset);

    std::string dropped;    // why a table the code builds was given up
    auto finish = [&](Table& table, u32 caller)
    {
        table.caller = caller;
        // An index that starts at 1 (or more): the code adds it to the table
        // minus 2 (ld hl,table-2), so the first entries it could name are
        // the executed code just before the table - Orguss's jp (hl) at
        // $1740 through $173F. They are skipped: the table starts after.
        if (!table.vector)
        {
            const std::vector<u8>& bytes = map.GetBytes();
            const u32 most = table.counted ? table.entries : 4;
            u32 skipped = 0;
            while (skipped < most && table.base + 1 < romSize &&
                   ((bytes[table.base] & RomMap::KindMask) == RomMap::Kind_Code ||
                    (bytes[table.base + 1] & RomMap::KindMask) == RomMap::Kind_Code))
            {
                table.base += 2;
                skipped++;
            }
            if (table.counted)
                table.entries -= skipped;
        }
        if (!table.counted && table.vector)
        {
            // As many jp instructions as follow one another.
            u32 n = 0;
            while (table.base + n * 3 + 2 < romSize && rom[table.base + n * 3] == 0xC3 && n < 128)
                n++;
            table.entries = n;
        }
        else if (!table.counted)
            table.entries = plausible_entries(rom, romSize, base, map, table.base);
        else if (!table.vector)
        {
            // A count from the code is an upper bound (and $07 allows 8):
            // the table stops where executed code starts.
            const std::vector<u8>& bytes = map.GetBytes();
            u32 n = 0;
            while (n < table.entries && table.base + n * 2 + 1 < romSize &&
                   (bytes[table.base + n * 2] & RomMap::KindMask) != RomMap::Kind_Code &&
                   (bytes[table.base + n * 2 + 1] & RomMap::KindMask) != RomMap::Kind_Code)
                n++;
            table.entries = n;
        }
        if (table.entries == 0)
        {
            char text[96];
            std::snprintf(text, sizeof(text), "the table at %X has no entry before executed code",
                          base + table.base);
            dropped = text;
            return;
        }
        for (const Table& t : result.tables)
            if (t.base == table.base)
                return;
        result.tables.push_back(table);
    };

    // The routine alone.
    State state;
    entry_state(state);
    for (const Instruction& i : body)
        step(state, i);
    Table table;
    std::string reason;
    const Sym target = get16(state, reg);
    if (table_from(target, base, romSize, table, reason))
    {
        finish(table, siteOffset);
        if (result.tables.empty())
            result.reason = dropped;
        return result;
    }
    if (!(target.kind == Sym::Word && !target.baseKnown && !target.fromRam))
    {
        result.reason = reason;
        return result;
    }

    // The table comes from the caller: each executed call into the routine,
    // with the code before it, then the routine.
    const u16 entryAddress = static_cast<u16>(base + entry);
    int callers = 0;
    for (u32 o = 0; o < romSize; o++)
    {
        if (!executed_start(map, o))
            continue;
        const u8 op = rom[o];
        if (!(op == 0xCD || (op & 0xC7) == 0xC4 || (op & 0xC7) == 0xC7 || op == 0xC3))
            continue;
        const Instruction call = decode(rom, romSize, base, o);
        if (!call.decoded.jump || call.decoded.jumpAddress != entryAddress)
            continue;
        callers++;
        const u32 from = routine_entry(rom, romSize, base, map, entries, o, 16);
        State s;
        s.clear();
        s.r8[A].kind = Sym::Index;
        s.r8[A].index = "unknown";
        s.r8[A].scale = 1;
        for (const Instruction& i : window(rom, romSize, base, from, o))
            step(s, i);
        for (const Instruction& i : body)
            step(s, i);
        Table t;
        std::string why;
        if (table_from(get16(s, reg), base, romSize, t, why))
            finish(t, o);
    }
    if (result.tables.empty())
        result.reason = !dropped.empty() ? dropped
                      : callers == 0 ? "the table comes from a caller, and none has run"
                                     : "no caller passes a table";
    return result;
}
}
