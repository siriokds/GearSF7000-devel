/*
 * GearSF7000 - ROM map: what each byte of the cartridge ROM turned out to be
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "RomMap.h"

#include <algorithm>
#include <utility>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "Memory.h"

namespace
{
const char kMagic[4] = {'G', 'R', 'M', 'P'};
const u32 kVersion = 3;

// A byte of data read from ROM is taken as the source of a port write when
// the values match and the read happened this recently: covers OUTI/OTIR
// (same instruction) and "ld a,(hl) / [inc hl] / out ($BE),a".
const u64 kSourceWindowInstructions = 3;

void put32(std::ostream& out, u32 value)
{
    const u8 bytes[4] = {static_cast<u8>(value), static_cast<u8>(value >> 8),
                         static_cast<u8>(value >> 16), static_cast<u8>(value >> 24)};
    out.write(reinterpret_cast<const char*>(bytes), 4);
}

bool get32(std::istream& in, u32& value)
{
    u8 bytes[4];
    if (!in.read(reinterpret_cast<char*>(bytes), 4))
        return false;
    value = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (static_cast<u32>(bytes[3]) << 24);
    return true;
}

// Which evidence wins when one byte is seen used in more than one way.
// Executed beats everything; a known VRAM destination beats a plain read.
int Rank(u8 kind)
{
    switch (kind)
    {
    case RomMap::Kind_Unknown: return 0;
    case RomMap::Kind_Fill: return 0; // above Unknown only by being set on it
    case RomMap::Kind_CodeCandidate: return 0;
    case RomMap::Kind_Table: return 0;
    case RomMap::Kind_Data: return 1;
    case RomMap::Kind_Sound: return 2;
    case RomMap::Kind_Vram: return 2;
    case RomMap::Kind_Code: return 4;
    default: return 3; // the typed VRAM kinds
    }
}


// Length and control flow of the Z80 instruction at rom[at], for the static
// candidates only - the running machine never goes through here.
struct Flow
{
    u32 length;
    bool hasTarget;
    u32 target;
    bool conditional;
    bool fallsThrough;
};

Flow DecodeFlow(const u8* rom, u32 size, u32 at)
{
    Flow f{1, false, 0, false, true};
    const auto byte = [&](u32 i) -> u8 { return i < size ? rom[i] : 0; };
    const auto word = [&](u32 i) -> u32 { return byte(i) | (byte(i + 1) << 8); };
    const auto rel = [&](u32 i) -> u32 { return (at + 2 + static_cast<s8>(byte(i))) & 0xFFFF; };
    const u8 op = byte(at);
    if (op == 0xDD || op == 0xFD)
    {
        const u8 p = byte(at + 1);
        f.length = 2;
        if (p == 0xCB || p == 0x21 || p == 0x22 || p == 0x2A || p == 0x36)
            f.length = 4;
        else if (p == 0x26 || p == 0x2E || p == 0x34 || p == 0x35 ||
                 (p >= 0x40 && p <= 0xBF && p != 0x76 && ((p & 7) == 6 || (p & 0xF8) == 0x70)))
            f.length = 3;
        else if (p == 0xE9)
            f.fallsThrough = false; // JP (IX/IY)
        return f;
    }
    if (op == 0xED)
    {
        const u8 p = byte(at + 1);
        f.length = 2;
        if ((p & 0xC7) == 0x43)
            f.length = 4; // LD (nn),rr / LD rr,(nn)
        else if ((p & 0xC7) == 0x45)
            f.fallsThrough = false; // RETN, RETI
        return f;
    }
    if (op == 0xCB)
    {
        f.length = 2;
        return f;
    }
    switch (op)
    {
    case 0xC3: f.length = 3; f.hasTarget = true; f.target = word(at + 1); f.fallsThrough = false; return f;
    case 0xCD: f.length = 3; f.hasTarget = true; f.target = word(at + 1); return f;
    case 0x18: f.length = 2; f.hasTarget = true; f.target = rel(at + 1); f.fallsThrough = false; return f;
    case 0x10: case 0x20: case 0x28: case 0x30: case 0x38:
        f.length = 2; f.hasTarget = true; f.target = rel(at + 1); f.conditional = true; return f;
    case 0xC9: case 0xE9: f.fallsThrough = false; return f;
    case 0x76: f.fallsThrough = false; return f; // HALT: treat as an end
    default: break;
    }
    if ((op & 0xC7) == 0xC2 || (op & 0xC7) == 0xC4)
    {
        f.length = 3; f.hasTarget = true; f.target = word(at + 1); f.conditional = true;
        return f;
    }
    if ((op & 0xC7) == 0xC0) { f.conditional = true; return f; } // RET cc
    if ((op & 0xC7) == 0xC7) { f.hasTarget = true; f.target = op & 0x38; return f; } // RST
    if (op == 0x01 || op == 0x11 || op == 0x21 || op == 0x31 || op == 0x22 || op == 0x2A ||
        op == 0x32 || op == 0x3A)
        f.length = 3;
    else if ((op & 0xC7) == 0x06 || (op & 0xC7) == 0xC6 || op == 0xD3 || op == 0xDB)
        f.length = 2;
    return f;
}

const char* TableName(GC_VramTableRole role)
{
    switch (role)
    {
    case GC_VramTableRole::NameTable: return "NAMETAB";
    case GC_VramTableRole::ColorTable: return "COLTAB";
    case GC_VramTableRole::PatternGenerator: return "PATGEN";
    case GC_VramTableRole::SpriteAttributes: return "SAT";
    case GC_VramTableRole::SpritePatterns: return "SPRPAT";
    }
    return "?";
}
}

RomMap::RomMap(Memory* memory) : m_memory(memory)
{
}

void RomMap::SetVramTableSource(VramTableSource source)
{
    m_vramTableSource = std::move(source);
    m_layoutDirty = true;
}

void RomMap::Attach(u32 romSize, u32 crc)
{
    if (romSize == m_romSize && crc == m_crc)
        return;
    m_romSize = romSize;
    m_crc = crc;
    m_sessions = 0;
    m_bytes.clear();
    m_destinations.clear();
    m_readers.clear();
    m_readerRoutines.clear();
    ClearFirmwareUsage();
    if (m_enabled)
        Allocate();
    Recount();
}

void RomMap::Detach()
{
    ClearFirmwareUsage();
    m_romSize = 0;
    m_crc = 0;
    m_sessions = 0;
    std::vector<u8>().swap(m_bytes);
    std::vector<u16>().swap(m_destinations);
    std::vector<u16>().swap(m_readers);
    std::vector<u16>().swap(m_readerRoutines);
    Recount();
}

void RomMap::SetEnabled(bool enabled)
{
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    if (enabled)
        Allocate();
    else
    {
        std::vector<u8>().swap(m_bytes);
        std::vector<u16>().swap(m_destinations);
        std::vector<u16>().swap(m_readers);
        std::vector<u16>().swap(m_readerRoutines);
        std::vector<u8>().swap(m_soundEntries);
    }
    Recount();
}

void RomMap::SetRuntimeActive(bool active)
{
    if (active == m_runtimeActive)
        return;
    m_runtimeActive = active;
    // No provenance/call-stack edge may cross an unobserved interval.
    m_prevValid = false;
    m_lastReadValid = false;
    m_reads[0].valid = m_reads[1].valid = false;
    m_instructionSize = 0;
    m_pendingCount = 0;
    m_pendingHead = 0;
    m_frameCount = 0;
    m_layoutDirty = true;
}

void RomMap::Reset()
{
    std::fill(m_bytes.begin(), m_bytes.end(), 0);
    std::fill(m_destinations.begin(), m_destinations.end(), NoDestination);
    std::fill(m_readers.begin(), m_readers.end(), NoDestination);
    std::fill(m_readerRoutines.begin(), m_readerRoutines.end(), NoDestination);
    ClearFirmwareUsage();
    m_sessions = 0;
    Recount();
}

void RomMap::ClearFirmwareUsage()
{
    m_observedJumps.clear();
    m_observedPointers.clear();
    for (RecentWord& w : m_recentWords)
        w.valid = false;
    m_reads[0].valid = m_reads[1].valid = false;
    m_firmwareEntries.clear();
    std::fill(m_firmwareReads.begin(), m_firmwareReads.end(), 0);
}

void RomMap::SetFirmwareRange(u16 start, u16 end)
{
    if (start == m_firmwareStart && end == m_firmwareEnd)
        return;
    m_firmwareStart = start;
    m_firmwareEnd = end;
    m_firmwareEntries.clear();
    m_firmwareReads.assign(end > start ? ((end - start) + 7) / 8 : 0, 0);
}

// Every arrival into the firmware from outside it that is not a
// fall-through, plus the interrupt vectors the hardware jumps to.
void RomMap::TrackFirmware(u16 next)
{
    if (m_memory && m_memory->GetSGMDevice() && m_memory->GetSGMDevice()->ResolvesRam(next))
        return;
    if (!HasFirmware() || !m_prevValid || m_bytes.empty())
        return;
    const bool inside = next >= m_firmwareStart && next < m_firmwareEnd;
    if (!inside)
        return;
    const u16 fallThrough = static_cast<u16>(m_prevPc + m_prevSize);
    if (next == fallThrough)
        return;
    const bool fromOutside = m_prevPc < m_firmwareStart || m_prevPc >= m_firmwareEnd;
    const bool interrupt = (next == 0x0038 || next == 0x0066) && next != m_prevTarget;
    if (!fromOutside && !interrupt)
        return;
    auto it = std::lower_bound(m_firmwareEntries.begin(), m_firmwareEntries.end(), next,
        [](const FirmwareEntry& e, u16 a) { return e.address < a; });
    if (it != m_firmwareEntries.end() && it->address == next)
    {
        ++it->count;
        it->interrupt = it->interrupt || interrupt;
        return;
    }
    m_firmwareEntries.insert(it, FirmwareEntry{next, 1, interrupt});
    ++m_generation;
}

std::vector<RomMap::FirmwareEntry> RomMap::GetFirmwareEntries() const
{
    return m_firmwareEntries;
}

std::vector<std::pair<u16, u16>> RomMap::GetFirmwareReadRanges() const
{
    std::vector<std::pair<u16, u16>> ranges;
    const u32 size = m_firmwareEnd - m_firmwareStart;
    u32 i = 0;
    while (i < size)
    {
        if (!(m_firmwareReads[i >> 3] & (1 << (i & 7))))
        {
            ++i;
            continue;
        }
        u32 j = i;
        while (j < size && (m_firmwareReads[j >> 3] & (1 << (j & 7))))
            ++j;
        ranges.emplace_back(static_cast<u16>(m_firmwareStart + i), static_cast<u16>(m_firmwareStart + j));
        i = j;
    }
    return ranges;
}

u32 RomMap::GetFirmwareReadBytes() const
{
    u32 n = 0;
    for (const auto& r : GetFirmwareReadRanges())
        n += r.second - r.first;
    return n;
}

bool RomMap::SaveFirmwareUsage(const std::string& path) const
{
    std::ofstream out(path, std::ios::trunc);
    if (!out)
        return false;
    out << "GearSF7000 firmware usage 1\n";
    out << "range " << std::hex << std::uppercase;
    char line[64];
    snprintf(line, sizeof(line), "%04X %04X\n", m_firmwareStart, m_firmwareEnd);
    out << line;
    for (const FirmwareEntry& e : m_firmwareEntries)
    {
        snprintf(line, sizeof(line), "entry %04X %u%s\n", e.address, e.count, e.interrupt ? " irq" : "");
        out << line;
    }
    for (const auto& r : GetFirmwareReadRanges())
    {
        snprintf(line, sizeof(line), "read %04X %04X\n", r.first, r.second);
        out << line;
    }
    return static_cast<bool>(out);
}

bool RomMap::LoadFirmwareUsage(const std::string& path, std::string* error)
{
    std::ifstream in(path);
    if (!in)
    {
        if (error) *error = "no file";
        return false;
    }
    std::string head;
    std::getline(in, head);
    if (head != "GearSF7000 firmware usage 1")
    {
        if (error) *error = "not a firmware usage file";
        return false;
    }
    std::string word;
    while (in >> word)
    {
        if (word == "range")
        {
            unsigned a, b;
            in >> std::hex >> a >> b >> std::dec;
            if (a != m_firmwareStart || b != m_firmwareEnd)
            {
                if (error) *error = "firmware range differs from this board";
                return false;
            }
        }
        else if (word == "entry")
        {
            unsigned address, count;
            in >> std::hex >> address >> std::dec >> count;
            std::string flag;
            const std::streampos pos = in.tellg();
            if (in >> flag && flag != "irq")
            {
                in.seekg(pos);
                flag.clear();
            }
            auto it = std::lower_bound(m_firmwareEntries.begin(), m_firmwareEntries.end(), static_cast<u16>(address),
                [](const FirmwareEntry& e, u16 x) { return e.address < x; });
            if (it != m_firmwareEntries.end() && it->address == address)
            {
                it->count += count;
                it->interrupt = it->interrupt || flag == "irq";
            }
            else
                m_firmwareEntries.insert(it, FirmwareEntry{static_cast<u16>(address), count, flag == "irq"});
        }
        else if (word == "read")
        {
            unsigned a, b;
            in >> std::hex >> a >> b >> std::dec;
            for (unsigned x = a; x < b && x < m_firmwareEnd; ++x)
                if (x >= m_firmwareStart)
                    m_firmwareReads[(x - m_firmwareStart) >> 3] |= static_cast<u8>(1 << ((x - m_firmwareStart) & 7));
        }
        else
            std::getline(in, head);
    }
    return true;
}

void RomMap::MarkFill(const u8* rom, u32 romSize)
{
    if (m_bytes.empty() || rom == nullptr || romSize != m_romSize || romSize == 0)
        return;
    const auto fill = [this](u32 offset, u16 vector) {
        if ((m_bytes[offset] & KindMask) != Kind_Unknown)
            return;
        --m_counts[Kind_Unknown];
        ++m_counts[Kind_Fill];
        m_bytes[offset] = static_cast<u8>((m_bytes[offset] & ~KindMask) | Kind_Fill);
        m_destinations[offset] = vector;
        ++m_generation;
    };

    const u8 filler = rom[romSize - 1];
    if (filler == 0x00 || filler == 0xFF)
    {
        u32 start = romSize;
        while (start > 0 && rom[start - 1] == filler)
            --start;
        if (romSize - start >= kMinFill)
        {
            m_fillValue = filler;
            for (u32 offset = start; offset < romSize; ++offset)
                fill(offset, NoDestination);
        }
    }

    // Between the vectors: RST $08..$38 and the NMI at $66. Sega games put
    // standard routines in there; what is left before the next vector, never
    // touched and $00 or $FF, only aligns it. Tied by position, so even one
    // byte counts - never further back than the previous vector.
    static const u16 kVectors[] = {0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x66};
    u16 previous = 0;
    for (u16 vector : kVectors)
    {
        if (vector > romSize)
            break;
        u32 at = vector;
        const u8 value = rom[vector - 1];
        while (at > previous + 1 && (rom[at - 1] == 0x00 || rom[at - 1] == 0xFF) &&
               rom[at - 1] == value && (m_bytes[at - 1] & KindMask) == Kind_Unknown)
            --at;
        for (u32 offset = at; offset < vector; ++offset)
            fill(offset, vector);
        previous = vector;
    }

    // Before a 256-byte page: a table aligned so that its low byte indexes
    // it (Saverio: "a volte si fa per velocita`") leaves padding in front. A
    // run of identical $00/$FF, never touched, ending exactly at a page whose
    // first byte is something known. At least 2 bytes: one alone is as likely
    // a value as a pad.
    for (u32 page = 0x100; page < romSize; page += 0x100)
    {
        if ((m_bytes[page] & KindMask) == Kind_Unknown)
            continue;
        const u8 value = rom[page - 1];
        if (value != 0x00 && value != 0xFF)
            continue;
        u32 at = page;
        while (at > page - 0x100 && rom[at - 1] == value &&
               (m_bytes[at - 1] & KindMask) == Kind_Unknown)
            --at;
        if (page - at >= 2)
            for (u32 offset = at; offset < page; ++offset)
                fill(offset, static_cast<u16>(page));
    }
}

void RomMap::SetDeducedCode(std::vector<DeducedBlock> blocks)
{
    std::sort(blocks.begin(), blocks.end(),
              [](const DeducedBlock& a, const DeducedBlock& b) { return a.start < b.start; });
    m_deduced = std::move(blocks);
}

const RomMap::DeducedBlock* RomMap::DeducedAt(u32 offset) const
{
    auto it = std::upper_bound(m_deduced.begin(), m_deduced.end(), offset,
                               [](u32 o, const DeducedBlock& b) { return o < b.start; });
    if (it == m_deduced.begin())
        return nullptr;
    --it;
    return offset < it->end ? &*it : nullptr;
}

void RomMap::SetDataHints(std::vector<DataHint> hints)
{
    std::sort(hints.begin(), hints.end(), [](const DataHint& a, const DataHint& b) { return a.start < b.start; });
    m_dataHints = std::move(hints);
}

const RomMap::DataHint* RomMap::DataHintAt(u32 offset) const
{
    auto it = std::upper_bound(m_dataHints.begin(), m_dataHints.end(), offset,
                               [](u32 o, const DataHint& h) { return o < h.start; });
    if (it == m_dataHints.begin())
        return nullptr;
    --it;
    return offset < it->end ? &*it : nullptr;
}

void RomMap::MarkBranchCandidates(const u8* rom, u32 romSize)
{
    if (m_bytes.empty() || rom == nullptr || romSize != m_romSize)
        return;
    // Start clean: a candidate since executed is already CODE, and one no
    // longer reachable must not linger.
    for (u32 offset = 0; offset < m_romSize; ++offset)
        if ((m_bytes[offset] & KindMask) == Kind_CodeCandidate)
        {
            m_bytes[offset] = static_cast<u8>(m_bytes[offset] & ~(KindMask | OrphanFlag));
            m_destinations[offset] = NoDestination;
        }

    // Deep Search's deduced code first: it followed the flow out of known
    // code, block by block, and is a firmer guess than the rules below.
    for (const DeducedBlock& block : m_deduced)
        for (u32 offset = block.start; offset < block.end && offset < m_romSize; ++offset)
            if ((m_bytes[offset] & KindMask) == Kind_Unknown)
            {
                m_bytes[offset] = static_cast<u8>((m_bytes[offset] & ~(KindMask | OrphanFlag | CandidateFlag)) |
                                                  Kind_CodeCandidate);
                m_destinations[offset] = static_cast<u16>(block.from);
            }

    std::vector<std::pair<u32, u32>> work; // entry, the branch that skipped it
    for (u32 at = 0; at < m_romSize; ++at)
    {
        if ((m_bytes[at] & InstructionStart) == 0)
            continue;
        const Flow flow = DecodeFlow(rom, romSize, at);
        if (!flow.conditional)
            continue;
        if (flow.hasTarget && flow.target < m_romSize)
            work.push_back({flow.target, at});
        work.push_back({at + flow.length, at});
    }
    for (const auto& item : work)
    {
        u32 at = item.first;
        // Bounded: a candidate path that runs this long is data, not an else.
        for (int steps = 0; steps < 256 && at < m_romSize; ++steps)
        {
            if ((m_bytes[at] & KindMask) != Kind_Unknown)
                break;
            const Flow flow = DecodeFlow(rom, romSize, at);
            const u32 end = std::min<u32>(at + flow.length, m_romSize);
            bool clear = true;
            for (u32 i = at; i < end; ++i)
                clear = clear && (m_bytes[i] & KindMask) == Kind_Unknown;
            if (!clear)
                break;
            for (u32 i = at; i < end; ++i)
            {
                m_bytes[i] = static_cast<u8>((m_bytes[i] & ~KindMask) | Kind_CodeCandidate);
                m_destinations[i] = static_cast<u16>(item.second);
            }
            if (!flow.fallsThrough)
                break;
            at = end;
        }
    }

    // Never-called routines. Measured on Saverio's Orguss map: 18 gaps, 602
    // bytes, among them $2527 and $299A - two of the 13 absolute references
    // found by hand the day before - and the standard Sega VRAM read at $0009.
    u32 lastStart = 0;
    bool haveStart = false;
    for (u32 gap = 0; gap < m_romSize;)
    {
        if ((m_bytes[gap] & InstructionStart) != 0)
        {
            lastStart = gap;
            haveStart = true;
        }
        if ((m_bytes[gap] & KindMask) != Kind_Unknown)
        {
            ++gap;
            continue;
        }
        u32 gapEnd = gap;
        while (gapEnd + 1 < m_romSize && (m_bytes[gapEnd + 1] & KindMask) == Kind_Unknown)
            ++gapEnd;
        const u32 next = gapEnd + 1;
        // Right after an executed instruction that does not fall through.
        bool follows = false;
        if (haveStart && (m_bytes[lastStart] & KindMask) == Kind_Code)
        {
            const Flow previous = DecodeFlow(rom, romSize, lastStart);
            follows = !previous.fallsThrough && lastStart + previous.length == gap;
        }
        if (follows && rom[gap] != 0xFF)
        {
            u32 at = gap;
            int count = 0;
            bool branchesToCode = false, fits = true, lastFalls = true;
            while (at <= gapEnd)
            {
                const Flow flow = DecodeFlow(rom, romSize, at);
                if (at + flow.length - 1 > gapEnd)
                {
                    fits = false;
                    break;
                }
                if (flow.hasTarget && flow.target < m_romSize &&
                    ((m_bytes[flow.target] & InstructionStart) != 0 ||
                     (flow.target >= gap && flow.target <= gapEnd)))
                    branchesToCode = true;
                lastFalls = flow.fallsThrough;
                at += flow.length;
                ++count;
            }
            const bool ends = !lastFalls ||
                (next < m_romSize && (m_bytes[next] & InstructionStart) != 0);
            if (fits && ends && branchesToCode && count >= 2)
                for (u32 i = gap; i <= gapEnd; ++i)
                {
                    m_bytes[i] = static_cast<u8>((m_bytes[i] & ~KindMask) |
                                                 Kind_CodeCandidate | OrphanFlag);
                    m_destinations[i] = static_cast<u16>(lastStart);
                }
        }
        gap = next;
    }
    Recount();
}

void RomMap::MarkStaticTables(const u8* rom, u32 romSize)
{
    if (m_bytes.empty() || rom == nullptr || romSize != m_romSize)
        return;
    const auto isStatic = [this](u32 i) {
        return m_destinations[i] >= Static_Base && m_destinations[i] != NoDestination;
    };
    // start clean: previous static marks go back to Unknown
    for (u32 i = 0; i < m_romSize; ++i)
        if (isStatic(i))
        {
            m_bytes[i] = static_cast<u8>(m_bytes[i] & ~(KindMask | CandidateFlag));
            m_destinations[i] = NoDestination;
        }
    const auto word = [&](u32 a) -> u32 { return a + 1 < romSize ? rom[a] | (rom[a + 1] << 8) : 0; };
    const auto kindAt = [&](u32 a) -> u8 { return m_bytes[a] & KindMask; };
    const auto dataLike = [&](u32 a) {
        const u8 k = kindAt(a);
        return k == Kind_Unknown || k == Kind_Data || k == Kind_Sound;
    };
    const auto mark = [&](u32 from, u32 to, u8 kind, u16 reason) {
        for (u32 i = from; i < to && i < m_romSize; ++i)
            if (kindAt(i) == Kind_Unknown)
            {
                m_bytes[i] = static_cast<u8>((m_bytes[i] & ~KindMask) | kind);
                m_destinations[i] = reason;
            }
    };

    // 1. Tone table (Sega driver): 24+ SN76489 divisors, each a semitone below
    //    the previous one; the entry before may be the silence. First, because
    //    its values ($03FF, $03C7 ...) would otherwise pass for addresses.
    u32 toneStart = 0, toneEnd = 0;
    for (u32 a = 0; a + 4 < romSize && toneEnd == 0; ++a)
    {
        u32 n = 0, p = a, prev = 0;
        while (p + 1 < romSize)
        {
            const u32 v = word(p);
            if (v == 0 || v > 0x3FF || (prev && !(prev * 100 > v * 103 && prev * 100 < v * 109)))
                break;
            prev = v;
            ++n;
            p += 2;
        }
        if (n >= 24)
        {
            toneStart = (a >= 2 && (word(a - 2) == 0 || word(a - 2) > 0x3FF)) ? a - 2 : a;
            toneEnd = p;
            mark(toneStart, toneEnd, Kind_Sound, Static_ToneTable);
        }
    }

    // 1b. Jump tables Deep Search read from the code before a jp (hl): the
    //     code says these bytes are addresses.
    //     They win over CODE? too: the code that reads them says what they
    //     are, where the never-called-routine rule only saw bytes that decode.
    for (const auto& table : m_jumpTables)
        for (u32 i = table.first; i < table.first + table.second && i < m_romSize; ++i)
        {
            const u8 kind = kindAt(i);
            if (kind != Kind_Unknown && kind != Kind_CodeCandidate)
                continue;
            m_bytes[i] = static_cast<u8>((m_bytes[i] & ~(KindMask | OrphanFlag | CandidateFlag)) | Kind_Table);
            m_destinations[i] = Static_JumpTable;
        }

    // 2. Pointer tables: 3+ consecutive words to routine entries (targets of an
    //    executed absolute CALL/JP or RST) or to data blocks of 8+ bytes. An
    //    arbitrary instruction start was too loose: pairs of small parameters
    //    landed on one by chance.
    std::vector<u8> target(m_romSize, 0);
    for (u32 a = 0; a + 2 < m_romSize; ++a)
    {
        if (kindAt(a) != Kind_Code || (m_bytes[a] & InstructionStart) == 0)
            continue;
        const u8 op = rom[a];
        if (op == 0xC3 || op == 0xCD || (op & 0xC7) == 0xC2 || (op & 0xC7) == 0xC4)
        {
            const u32 t = word(a + 1);
            if (t < m_romSize)
                target[t] = 1;
        }
        else if ((op & 0xC7) == 0xC7)
            target[op & 0x38] = 1;
    }
    for (u32 a = 0; a < m_romSize;)
    {
        const u8 k = kindAt(a);
        u32 e = a;
        while (e < m_romSize && kindAt(e) == k)
            ++e;
        if ((k == Kind_Data || k == Kind_Sound) && e - a >= 8)
            target[a] = 1;
        a = e;
    }
    for (u32 a = 0x100; a + 1 < m_romSize;)
    {
        u32 p = a, n = 0;
        while (p + 1 < m_romSize && !(p >= toneStart && p < toneEnd) && dataLike(p) && dataLike(p + 1))
        {
            const u32 t = word(p);
            if (t < 0x100 || t >= m_romSize || !target[t])
                break;
            ++n;
            p += 2;
        }
        if (n >= 3)
        {
            mark(a, p, Kind_Table, Static_PointerTable);
            a = p;
        }
        else
            ++a;
    }

    // 3. Song table (Sega driver): 3+ words to plausible song headers - a word
    //    pointing just past the header to a channel list of 1-4 voices whose
    //    templates are nearby. The headers themselves are sound data. A header
    //    whose list sits at +4 carries at +2 the volume-curve table, shared by
    //    every song: its words up to the first curve, and each curve, a string
    //    of nibbles closed by a $0 nibble.
    const auto plausibleHeader = [&](u32 h) {
        if (h < 0x100 || h + 6 >= romSize)
            return false;
        const u32 list = word(h);
        if (list < h || list > h + 8 || list >= romSize)
            return false;
        const u8 voices = rom[list];
        if (voices < 1 || voices > 4)
            return false;
        for (u8 v = 0; v < voices; ++v)
        {
            const u32 t = word(list + 1 + 2 * v);
            if ((t & 0xFF) != 0xFF && (t + 0x400 < h || t > h + 0x400))
                return false;
        }
        return true;
    };
    for (u32 a = 0x100; a + 1 < m_romSize;)
    {
        u32 p = a, n = 0;
        while (p + 1 < m_romSize && dataLike(p) && dataLike(p + 1) && plausibleHeader(word(p)))
        {
            ++n;
            p += 2;
        }
        if (n < 3)
        {
            ++a;
            continue;
        }
        mark(a, p, Kind_Table, Static_SongTable);
        for (u32 x = a; x < p; x += 2)
        {
            const u32 h = word(x);
            const u32 list = word(h);
            mark(h, list + 1 + 2 * rom[list], Kind_Sound, Static_SongHeader);
            if (list != h + 4)
                continue;
            const u32 curves = word(h + 2);
            if (curves + 8 >= romSize)
                continue;
            u32 first = 0xFFFFFFFF;
            for (u32 i = 0; i < 4; ++i)
                first = std::min(first, word(curves + 2 * i));
            u32 i = 0;
            while (curves + 2 * i < first)
            {
                const u32 t = word(curves + 2 * i);
                if (t <= curves || t >= curves + 0x400 || t >= romSize)
                    break;
                first = std::min(first, t);
                u32 e = t;
                while (e < romSize && (rom[e] >> 4) != 0 && (rom[e] & 0x0F) != 0)
                    ++e;
                mark(t, e + 1, Kind_Sound, Static_VolumeCurve);
                ++i;
            }
            mark(curves, curves + 2 * i, Kind_Table, Static_CurveTable);
        }
        a = p;
    }

    // 5. Table gaps. The nearest reader inside the data run on each side:
    //    bytes read before readers were recorded have none, their neighbours
    //    may.
    const auto readerNear = [&](u32 from, int step) -> u16 {
        for (int n = 0; n < 16; ++n)
        {
            const long i = static_cast<long>(from) + step * n;
            if (i < 0 || i >= static_cast<long>(m_romSize))
                break;
            const u8 k = kindAt(static_cast<u32>(i));
            if (k != Kind_Data && k != Kind_Sound)
                break;
            const u16 key = GetReaderKey(static_cast<u32>(i));
            if (key != NoDestination)
                return key;
        }
        return NoDestination;
    };
    for (u32 a = 1; a < m_romSize;)
    {
        if (kindAt(a) != Kind_Unknown)
        {
            ++a;
            continue;
        }
        u32 e = a;
        while (e + 1 < m_romSize && kindAt(e + 1) == Kind_Unknown)
            ++e;
        if (e - a + 1 <= kMaxTableGap && e + 1 < m_romSize)
        {
            const u16 before = readerNear(a - 1, -1);
            if (before != NoDestination && before == readerNear(e + 1, 1))
            {
                const bool sound = kindAt(a - 1) == Kind_Sound && kindAt(e + 1) == Kind_Sound;
                mark(a, e + 1, sound ? Kind_Sound : Kind_Data, Static_TableGap);
                for (u32 i = a; i <= e; ++i)
                    m_bytes[i] |= CandidateFlag;
            }
        }
        a = e + 1;
    }
    Recount();
}

void RomMap::Allocate()
{
    m_bytes.assign(m_romSize, 0);
    m_destinations.assign(m_romSize, NoDestination);
    m_readers.assign(m_romSize, NoDestination);
    m_readerRoutines.assign(m_romSize, NoDestination);
    m_soundEntries.assign(m_romSize ? 0x10000 / 8 : 0, 0);
    if (!m_soundEntries.empty())
        for (u16 entry : m_firmwareSoundEntries)
            m_soundEntries[entry >> 3] |= static_cast<u8>(1 << (entry & 7));
    m_frameCount = 0;
    m_prevValid = false;
    m_lastReadValid = false;
    m_pendingCount = 0;
    m_layoutDirty = true;
}

void RomMap::Recount()
{
    for (u32& count : m_counts)
        count = 0;
    for (u8 value : m_bytes)
        ++m_counts[(value & KindMask) < Kind_Count ? (value & KindMask) : Kind_Unknown];
    ++m_generation;
}

void RomMap::Classify(u32 offset, u8 kind)
{
    const u8 current = m_bytes[offset] & KindMask;
    if (current == kind)
    {
        // Seen happening what a static rule only inferred.
        if (m_bytes[offset] & CandidateFlag)
        {
            m_bytes[offset] &= static_cast<u8>(~CandidateFlag);
            if (offset < m_destinations.size() && m_destinations[offset] >= Static_Base &&
                m_destinations[offset] != NoDestination)
                m_destinations[offset] = NoDestination;
            ++m_generation;
        }
        return;
    }
    u8 next = kind;
    // One pattern byte copied both to the background and to the sprite
    // patterns is shared graphics.
    if ((current == Kind_GfxBg && kind == Kind_GfxSpr) ||
        (current == Kind_GfxSpr && kind == Kind_GfxBg))
        next = Kind_Gfx;
    else if (Rank(kind) <= Rank(current))
        return;
    --m_counts[current < Kind_Count ? current : Kind_Unknown];
    ++m_counts[next];
    // The flag bit means something per kind; it does not survive a change, and
    // neither does a static reason once something is observed.
    m_bytes[offset] = static_cast<u8>((m_bytes[offset] & ~(KindMask | OrphanFlag | CandidateFlag)) | next);
    if (offset < m_destinations.size() && m_destinations[offset] >= Static_Base &&
        m_destinations[offset] != NoDestination)
        m_destinations[offset] = NoDestination;
    ++m_generation;
}

// The hot path. Every instruction the CPU starts comes through here, so the
// common case - an instruction already seen - has to be one lookup and one
// test. The translation to a physical ROM offset is the disassembler's own:
// it already resolves every mapper, and runs before this observer.
void RomMap::OnCpuInstructionBoundary(u16 nextProgramCounter, u64 elapsedTStates)
{
    (void)elapsedTStates;
    if (!IsCollecting())
        return;

    ++m_instructionIndex;
    TrackIndirectJump(nextProgramCounter);
    TrackCallStack(nextProgramCounter);
    TrackFirmware(nextProgramCounter);
    m_instructionPc = nextProgramCounter;
    m_instructionSize = 0;
    const Memory::stDisassembleRecord* record =
        m_memory->GetDisassembleRecord(nextProgramCounter, false);
    if (record == nullptr || record->size <= 0)
    {
        m_prevValid = false;
        return;
    }
    m_instructionSize = static_cast<u16>(record->size);
    RememberInstruction(nextProgramCounter, record->opcodes, record->size, record->jump_address);
    // Cartridge ROM, accepting the legacy "MAP " label too; not IPL/BIOS/RAM/SRAM.
    const bool rom = (record->segment[0] == 'R' && record->segment[1] == 'O')
                  || (record->segment[0] == 'M' && record->segment[1] == 'A');
    if (!rom)
        return;

    MarkExecuted(record->physical_address, static_cast<u32>(record->size));
}

// What kind of control transfer the instruction about to run is, so that the
// next boundary can tell a taken call or return from a fall-through.
void RomMap::RememberInstruction(u16 pc, const u8* opcodes, int size, u16 jumpAddress)
{
    m_prevValid = true;
    m_prevPc = pc;
    m_prevSize = static_cast<u16>(size);
    m_prevTarget = jumpAddress;
    m_prevFlow = 0;
    const u8 op = opcodes[0];
    if (op == 0xCD || (op & 0xC7) == 0xC4)
        m_prevFlow = 1; // CALL, CALL cc
    else if ((op & 0xC7) == 0xC7)
    {
        m_prevFlow = 1; // RST
        m_prevTarget = op & 0x38;
    }
    else if (op == 0xC9)
        m_prevFlow = 2; // RET
    else if ((op & 0xC7) == 0xC0)
        m_prevFlow = 3; // RET cc
    else if (op == 0xED && size >= 2 && (opcodes[1] == 0x4D || opcodes[1] == 0x45))
        m_prevFlow = 2; // RETI, RETN
    else if (op == 0xE9 || ((op == 0xDD || op == 0xFD) && size >= 2 && opcodes[1] == 0xE9))
        m_prevFlow = 4; // JP (HL), JP (IX), JP (IY)
}

// A JP (rr) that lands on the word read just before from the ROM: the
// word is a jump table entry, seen in use. Only the two last reads, side by
// side, within a few instructions - the table read and the jump are close.
void RomMap::TrackIndirectJump(u16 next)
{
    if (!m_prevValid || m_prevFlow != 4)
        return;
    const Read& a = m_reads[0];
    const Read& b = m_reads[1];
    if (!a.valid || !b.valid || m_instructionIndex - a.instruction > 16 ||
        m_instructionIndex - b.instruction > 16)
        return;
    const Read& low = a.offset < b.offset ? a : b;
    const Read& high = a.offset < b.offset ? b : a;
    if (high.offset != low.offset + 1 || static_cast<u16>(low.value | (high.value << 8)) != next)
        return;
    u32 site = 0;
    if (!m_memory->TranslateToRomOffset(m_prevPc, site) || site >= m_romSize)
        return;
    auto it = std::lower_bound(m_observedJumps.begin(), m_observedJumps.end(), ObservedJump{site, low.offset, 0},
        [](const ObservedJump& x, const ObservedJump& y)
        { return x.site != y.site ? x.site < y.site : x.entry < y.entry; });
    if (it != m_observedJumps.end() && it->site == site && it->entry == low.offset)
    {
        if (it->count != 0xFFFFFFFFu)
            it->count++;
        return;
    }
    m_observedJumps.insert(it, ObservedJump{site, low.offset, 1});
    ++m_generation;
}

void RomMap::TrackPointer(u16 address, u32 readOffset, bool inRom)
{
    // A read in the middle of a walk - the same instruction read the byte
    // before - was coming anyway: a pointer leads to where a read starts
    // (Orguss's $0AFE held $3834, a VRAM address, and the copy of the
    // screen data went through ROM $3834 every time).
    if (inRom && m_reads[1].valid && m_reads[1].offset + 1 == readOffset && m_reads[1].pc == m_instructionPc)
        return;
    // A few frames: a sound driver keeps the pointer it read in RAM and
    // follows it at its next call.
    const u64 kWindow = 200000;
    for (RecentWord& w : m_recentWords)
    {
        if (!w.valid || w.value != address)
            continue;
        if (m_instructionIndex - w.instruction > kWindow)
        {
            w.valid = false;
            continue;
        }
        // The word itself, or the byte right after it read on: not a use.
        if (inRom && readOffset >= w.entry && readOffset <= w.entry + 2)
            continue;
        auto it = std::lower_bound(m_observedPointers.begin(), m_observedPointers.end(), w.entry,
                                   [](const ObservedPointer& p, u32 e) { return p.entry < e; });
        if (it != m_observedPointers.end() && it->entry == w.entry)
        {
            if (it->count != 0xFFFFFFFFu)
                it->count++;
        }
        else
        {
            m_observedPointers.insert(it, ObservedPointer{w.entry, w.value, 1});
            ++m_generation;
        }
        w.valid = false;        // one use proves it; the next read of it counts again
    }
}

bool RomMap::SaveObservedPointers(const std::string& path) const
{
    std::ofstream out(path, std::ios::trunc);
    if (!out)
        return false;
    out << "GearSF7000 pointers seen 1\n";
    out << "# A word read from the ROM at entry, then an access at the address it holds:\n";
    out << "# an absolute address. ROM offset of the word, the address, how many times.\n";
    char line[64];
    for (const ObservedPointer& p : m_observedPointers)
    {
        snprintf(line, sizeof(line), "entry %04X target %04X %u\n", p.entry, p.target, p.count);
        out << line;
    }
    return static_cast<bool>(out);
}

bool RomMap::LoadObservedPointers(const std::string& path, std::string* error)
{
    std::ifstream in(path);
    if (!in)
    {
        if (error) *error = "no file";
        return false;
    }
    std::string head;
    std::getline(in, head);
    if (head.rfind("GearSF7000 pointers seen", 0) != 0)
    {
        if (error) *error = "not a pointers file";
        return false;
    }
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream words(line);
        std::string word, entry, targetWord, target;
        unsigned count = 0;
        if (!(words >> word) || word != "entry" || !(words >> entry >> targetWord >> target >> count))
            continue;
        const ObservedPointer p{static_cast<u32>(std::strtoul(entry.c_str(), NULL, 16)),
                                static_cast<u16>(std::strtoul(target.c_str(), NULL, 16)), count};
        if (p.entry + 1 >= m_romSize)
            continue;
        auto it = std::lower_bound(m_observedPointers.begin(), m_observedPointers.end(), p.entry,
                                   [](const ObservedPointer& x, u32 e) { return x.entry < e; });
        if (it != m_observedPointers.end() && it->entry == p.entry)
            it->count = std::max(it->count, p.count);
        else
            m_observedPointers.insert(it, p);
    }
    ++m_generation;
    return true;
}

bool RomMap::SaveObservedJumps(const std::string& path) const
{
    std::ofstream out(path, std::ios::trunc);
    if (!out)
        return false;
    out << "GearSF7000 jump entries seen 1\n";
    out << "# A jp (hl)/(ix)/(iy) at site landed on the word read at entry: ROM offsets.\n";
    char line[64];
    for (const ObservedJump& j : m_observedJumps)
    {
        snprintf(line, sizeof(line), "site %04X entry %04X %u\n", j.site, j.entry, j.count);
        out << line;
    }
    return static_cast<bool>(out);
}

bool RomMap::LoadObservedJumps(const std::string& path, std::string* error)
{
    std::ifstream in(path);
    if (!in)
    {
        if (error) *error = "no file";
        return false;
    }
    std::string head;
    std::getline(in, head);
    if (head.rfind("GearSF7000 jump entries seen", 0) != 0)
    {
        if (error) *error = "not a jump entries file";
        return false;
    }
    std::string word, site, entryWord, entry;
    unsigned count = 0;
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream words(line);
        if (!(words >> word) || word != "site" || !(words >> site >> entryWord >> entry >> count))
            continue;
        const ObservedJump j{static_cast<u32>(std::strtoul(site.c_str(), NULL, 16)),
                             static_cast<u32>(std::strtoul(entry.c_str(), NULL, 16)), count};
        if (j.site >= m_romSize || j.entry + 1 >= m_romSize)
            continue;
        auto it = std::lower_bound(m_observedJumps.begin(), m_observedJumps.end(), j,
            [](const ObservedJump& x, const ObservedJump& y)
            { return x.site != y.site ? x.site < y.site : x.entry < y.entry; });
        if (it != m_observedJumps.end() && it->site == j.site && it->entry == j.entry)
            it->count = std::max(it->count, j.count);
        else
            m_observedJumps.insert(it, j);
    }
    ++m_generation;
    return true;
}

void RomMap::TrackCallStack(u16 next)
{
    if (!m_prevValid)
        return;
    const u16 fallThrough = static_cast<u16>(m_prevPc + m_prevSize);
    const bool called = m_prevFlow == 1 && next == m_prevTarget && next != fallThrough;
    const bool returned = m_prevFlow == 2 || (m_prevFlow == 3 && next != fallThrough);
    // An interrupt: the CPU went to a vector nobody jumped to.
    const bool interrupt = !called && (next == 0x0038 || next == 0x0066) &&
        next != fallThrough && next != m_prevTarget;
    if (called || interrupt)
    {
        if (m_frameCount == kMaxFrames)
        {
            // A program that manipulates its stack can leave frames behind
            // forever; losing the oldest keeps the recent ones right.
            std::memmove(m_frames, m_frames + 1, sizeof(Frame) * (kMaxFrames - 1));
            --m_frameCount;
        }
        Frame& frame = m_frames[m_frameCount];
        frame.entry = next;
        frame.interrupt = interrupt;
        const bool parentSound = m_frameCount > 0 && m_frames[m_frameCount - 1].sound
            && !interrupt;
        const bool sgmOverlay = m_memory && m_memory->GetSGMDevice() &&
            m_memory->GetSGMDevice()->ResolvesRam(next);
        frame.sound = parentSound ||
            (!sgmOverlay && !m_soundEntries.empty() && (m_soundEntries[next >> 3] & (1 << (next & 7))));
        ++m_frameCount;
    }
    else if (returned && m_frameCount > 0)
        --m_frameCount;
}

void RomMap::MarkExecuted(u32 start, u32 size)
{
    if (m_bytes.empty() || start >= m_romSize || (m_bytes[start] & InstructionStart) != 0)
        return;

    m_bytes[start] |= InstructionStart;
    const u32 end = std::min<u32>(start + size, m_romSize);
    for (u32 offset = start; offset < end; ++offset)
        Classify(offset, Kind_Code);
    ++m_generation;
}

void RomMap::OnCpuDataRead(u16 address, u8 value)
{
    if (!IsCollecting())
        return;
    // The instruction's own operand bytes arrive as plain reads too.
    if (static_cast<u16>(address - m_instructionPc) < m_instructionSize)
        return;
    if (HasFirmware() && address >= m_firmwareStart && address < m_firmwareEnd &&
        !(m_memory && m_memory->GetSGMDevice() && m_memory->GetSGMDevice()->ResolvesRam(address)))
    {
        const u32 bit = address - m_firmwareStart;
        if (!(m_firmwareReads[bit >> 3] & (1 << (bit & 7))))
        {
            m_firmwareReads[bit >> 3] |= static_cast<u8>(1 << (bit & 7));
            ++m_generation;
        }
        return;
    }
    u32 offset = 0;
    const bool inRom = m_memory->TranslateToRomOffset(address, offset) && offset < m_romSize;
    // A read at an address a word from the ROM held: that word is a pointer.
    TrackPointer(address, inRom ? offset : 0xFFFFFFFFu, inRom);
    if (!inRom)
        return;
    m_lastReadValid = true;
    m_lastReadOffset = offset;
    m_lastReadValue = value;
    m_lastReadInstruction = m_instructionIndex;
    m_reads[0] = m_reads[1];
    m_reads[1] = Read{true, offset, value, m_instructionIndex, m_instructionPc};
    // Two bytes side by side, read by one instruction (ld hl,(nn)) or by
    // two different ones (ld e,(hl) / inc hl / ld d,(hl)): a word. The
    // same instruction reading them one after the other is a copy
    // walking through a block (ldir, otir), whose pairs mean nothing.
    {
        const Read& a = m_reads[0];
        const Read& b = m_reads[1];
        if (a.valid && m_instructionIndex - a.instruction <= 4 &&
            (a.offset + 1 == b.offset || b.offset + 1 == a.offset) &&
            (a.instruction == b.instruction || a.pc != b.pc))
        {
            const Read& low = a.offset < b.offset ? a : b;
            const Read& high = a.offset < b.offset ? b : a;
            m_recentWords[m_recentNext] = RecentWord{true, static_cast<u16>(low.value | (high.value << 8)),
                                                     low.offset, m_instructionIndex};
            m_recentNext = (m_recentNext + 1) % kRecentWords;
        }
    }
    if (m_readers[offset] == NoDestination)
    {
        m_readers[offset] = m_instructionPc;
        m_readerRoutines[offset] = m_frameCount > 0 ? m_frames[m_frameCount - 1].entry
                                                     : NoDestination;
        ++m_generation;
    }
    if (m_frameCount > 0 && m_frames[m_frameCount - 1].sound)
    {
        Classify(offset, Kind_Sound);
        if ((m_bytes[offset] & KindMask) == Kind_Sound &&
            m_destinations[offset] == NoDestination)
        {
            // Name the outermost sound frame - the driver, not the helper
            // it happens to be in.
            int first = m_frameCount - 1;
            while (first > 0 && m_frames[first - 1].sound && !m_frames[first - 1].interrupt)
                --first;
            m_destinations[offset] = m_frames[first].entry;
        }
    }
    else
        Classify(offset, Kind_Data);
}

void RomMap::SetBoardSound(u8 psgMask, u8 psgMatch, const u16* entries, std::size_t count)
{
    m_psgMask = psgMask;
    m_psgMatch = psgMatch;
    m_firmwareSoundEntries.assign(entries, entries + count);
    if (!m_soundEntries.empty())
        for (u16 entry : m_firmwareSoundEntries)
            m_soundEntries[entry >> 3] |= static_cast<u8>(1 << (entry & 7));
}

void RomMap::OnCpuPortWrite(u8 port, u8 value)
{
    if (!IsCollecting())
        return;
    // Writing the PSG makes the running routine sound code, and its caller
    // too - the song parser that feeds the register writer - unless the
    // caller is the interrupt handler, which does everything else as well.
    if ((port & m_psgMask) == m_psgMatch && m_frameCount > 0)
    {
        const int top = m_frameCount - 1;
        const auto mark = [this](int index) {
            Frame& frame = m_frames[index];
            frame.sound = true;
            m_soundEntries[frame.entry >> 3] |= static_cast<u8>(1 << (frame.entry & 7));
        };
        if (!m_frames[top].interrupt)
        {
            mark(top);
            if (top > 0 && !m_frames[top - 1].interrupt)
                mark(top - 1);
        }
    }
    PendingWrite write;
    write.value = value;
    write.fromRom = m_lastReadValid && m_lastReadValue == value &&
        m_instructionIndex - m_lastReadInstruction <= kSourceWindowInstructions;
    write.romOffset = m_lastReadOffset;
    if (m_pendingCount == kPendingCapacity)
    {
        m_pendingHead = (m_pendingHead + 1) % kPendingCapacity;
        --m_pendingCount;
    }
    m_pending[(m_pendingHead + m_pendingCount) % kPendingCapacity] = write;
    ++m_pendingCount;
}

void RomMap::OnVramWrite(u16 vramAddress, u8 value)
{
    if (!IsCollecting())
        return;
    // Oldest pending write with this value; anything before it went to
    // another port (the PSG, the PPI) and is dropped.
    while (m_pendingCount > 0)
    {
        const PendingWrite write = m_pending[m_pendingHead];
        m_pendingHead = (m_pendingHead + 1) % kPendingCapacity;
        --m_pendingCount;
        if (write.value != value)
            continue;
        if (!write.fromRom || write.romOffset >= m_romSize)
            return; // computed, or copied from RAM: its origin is not known yet
        bool colorTable = false;
        const u8 kind = KindForVram(vramAddress, colorTable);
        const u8 before = m_bytes[write.romOffset] & KindMask;
        Classify(write.romOffset, kind);
        // A byte first seen read by the sound routine and now copied to VRAM:
        // the destination replaces the routine as its evidence.
        if (before == Kind_Sound && (m_bytes[write.romOffset] & KindMask) != Kind_Sound)
            m_destinations[write.romOffset] = NoDestination;
        if (m_destinations[write.romOffset] == NoDestination)
        {
            m_destinations[write.romOffset] = vramAddress;
            if (colorTable)
                m_bytes[write.romOffset] |= ColorTableFlag;
        }
        return;
    }
}

u8 RomMap::KindForVram(u16 vramAddress, bool& colorTable)
{
    colorTable = false;
    if (m_layoutDirty)
    {
        m_tableCount = m_vramTableSource ? m_vramTableSource(m_tables, kMaxTables) : 0;
        m_layoutDirty = false;
    }
    bool bg = false, spr = false, name = false, sat = false, color = false;
    for (std::size_t i = 0; i < m_tableCount; ++i)
    {
        const GC_VramTableSpan& span = m_tables[i];
        if (vramAddress < span.start || vramAddress >= span.start + span.size)
            continue;
        switch (span.role)
        {
        case GC_VramTableRole::NameTable: name = true; break;
        case GC_VramTableRole::ColorTable: color = true; break;
        case GC_VramTableRole::PatternGenerator: bg = true; break;
        case GC_VramTableRole::SpriteAttributes: sat = true; break;
        case GC_VramTableRole::SpritePatterns: spr = true; break;
        }
    }
    // The small, specific tables first: they are often placed inside the
    // span of a bigger one that is not fully used.
    if (sat) return Kind_Sat;
    if (name) return Kind_Screen;
    if (color) { colorTable = true; return Kind_GfxBg; }
    if (bg && spr) return Kind_Gfx;
    if (bg) return Kind_GfxBg;
    if (spr) return Kind_GfxSpr;
    return Kind_Vram;
}

std::vector<RomMap::Range> RomMap::BuildRanges() const
{
    std::vector<Range> ranges;
    if (m_bytes.empty())
    {
        if (m_romSize != 0)
            ranges.push_back({0, m_romSize - 1, Kind_Unknown});
        return ranges;
    }
    const u8 keyMask = KindMask | CandidateFlag;
    u32 start = 0;
    u8 key = m_bytes[0] & keyMask;
    for (u32 offset = 1; offset < m_romSize; ++offset)
    {
        const u8 current = m_bytes[offset] & keyMask;
        if (current == key)
            continue;
        ranges.push_back({start, offset - 1, static_cast<u8>(key & KindMask), (key & CandidateFlag) != 0});
        start = offset;
        key = current;
    }
    ranges.push_back({start, m_romSize - 1, static_cast<u8>(key & KindMask), (key & CandidateFlag) != 0});
    return ranges;
}

const char* RomMap::KindName(u8 kind)
{
    switch (kind)
    {
    case Kind_Unknown: return "UNKNOWN";
    case Kind_Code: return "CODE";
    case Kind_Data: return "DATA";
    case Kind_GfxBg: return "GFX BG";
    case Kind_GfxSpr: return "GFX SPR";
    case Kind_Gfx: return "GFX BG+SPR";
    case Kind_Screen: return "SCREEN";
    case Kind_Sat: return "SAT";
    case Kind_Vram: return "VRAM";
    case Kind_Sound: return "SOUND";
    case Kind_Fill: return "FILL";
    case Kind_CodeCandidate: return "CODE?";
    case Kind_Table: return "TABLE";
    default: return "?";
    }
}

std::string RomMap::RangeName(const Range& range)
{
    std::string name = KindName(range.kind);
    if (range.candidate)
        name += "?";
    return name;
}

std::string RomMap::DescribeEvidence(const Range& range) const
{
    switch (GetDestination(range.start))
    {
    case Static_PointerTable: return "pointer table (static)";
    case Static_JumpTable: return "jump table, read from the code (Deep Search)";
    case Static_ToneTable: return "tone table (static, Sega driver)";
    case Static_SongTable: return "song table (static, Sega driver)";
    case Static_SongHeader: return "song header (static, Sega driver)";
    case Static_CurveTable: return "volume-curve table (static, Sega driver)";
    case Static_VolumeCurve: return "volume curve (static, Sega driver)";
    case Static_TableGap:
    {
        // The rule's reader: the nearest one read before the gap.
        u16 key = NoDestination;
        for (u32 i = range.start; i > 0 && key == NoDestination && range.start - i < 16; --i)
            key = GetReaderKey(i - 1);
        char text[64];
        if (key == NoDestination)
            return "unreached entry of a table (static)";
        snprintf(text, sizeof(text), "unreached entry of a table read by $%04X (static)", key);
        return text;
    }
    default: break;
    }
    switch (range.kind)
    {
    case Kind_Unknown: return std::string();
    case Kind_Code: return "executed";
    case Kind_Data:
    {
        const u16 reader = GetReader(range.start);
        if (reader == NoDestination)
            return "read as data";
        char text[48];
        const u16 routine = GetReaderRoutine(range.start);
        if (routine == NoDestination || routine == reader)
            snprintf(text, sizeof(text), "read by $%04X", reader);
        else
            snprintf(text, sizeof(text), "read by $%04X in $%04X", reader, routine);
        return text;
    }
    case Kind_CodeCandidate:
    {
        char text[48];
        const u16 source = GetDestination(range.start);
        const bool orphan = range.start < m_bytes.size() && (m_bytes[range.start] & OrphanFlag);
        if (source == NoDestination)
            return orphan ? "never-called routine" : "never-taken branch";
        snprintf(text, sizeof(text), orphan ? "never-called routine after $%04X"
                                            : "never-taken branch of $%04X", source);
        return text;
    }
    case Kind_Fill:
    {
        char text[48];
        const u16 vector = GetDestination(range.start);
        if (vector != NoDestination && vector >= 0x100 && (vector & 0xFF) == 0)
            snprintf(text, sizeof(text), "padding to page $%04X", vector);
        else if (vector != NoDestination)
            snprintf(text, sizeof(text), "padding before vector $%04X", vector);
        else
            snprintf(text, sizeof(text), "$%02X padding to the end of the ROM", m_fillValue);
        return text;
    }
    case Kind_Sound:
    {
        const u16 routine = GetDestination(range.start);
        if (routine == NoDestination)
            return "read by a sound routine";
        char text[48];
        snprintf(text, sizeof(text), "read by sound routine $%04X", routine);
        return text;
    }
    default: break;
    }
    // A VRAM kind: where the first byte of the range went.
    const u16 destination = GetDestination(range.start);
    if (destination == NoDestination)
        return "copied to VRAM";
    const char* table = "VRAM";
    switch (range.kind)
    {
    case Kind_GfxBg:
        table = (range.start < m_bytes.size() && (m_bytes[range.start] & ColorTableFlag))
            ? TableName(GC_VramTableRole::ColorTable)
            : TableName(GC_VramTableRole::PatternGenerator);
        break;
    case Kind_GfxSpr: table = TableName(GC_VramTableRole::SpritePatterns); break;
    case Kind_Gfx: table = "PATGEN+SPRPAT"; break;
    case Kind_Screen: table = TableName(GC_VramTableRole::NameTable); break;
    case Kind_Sat: table = TableName(GC_VramTableRole::SpriteAttributes); break;
    default: break;
    }
    char text[48];
    snprintf(text, sizeof(text), "-> %s $%04X", table, destination);
    return text;
}

bool RomMap::Save(const std::string& path) const
{
    if (m_bytes.empty())
        return false;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(kMagic, 4);
    put32(out, kVersion);
    put32(out, m_crc);
    put32(out, m_romSize);
    put32(out, m_sessions);
    out.write(reinterpret_cast<const char*>(m_bytes.data()),
              static_cast<std::streamsize>(m_bytes.size()));
    const auto putWords = [&out](const std::vector<u16>& words) {
        for (u16 word : words)
        {
            const u8 bytes[2] = {static_cast<u8>(word), static_cast<u8>(word >> 8)};
            out.write(reinterpret_cast<const char*>(bytes), 2);
        }
    };
    putWords(m_destinations);
    putWords(m_readers);
    putWords(m_readerRoutines);
    return static_cast<bool>(out);
}

bool RomMap::Load(const std::string& path, std::string* error)
{
    auto fail = [error](const char* why) {
        if (error)
            *error = why;
        return false;
    };
    if (m_bytes.empty())
        return fail("map not enabled or no ROM attached");
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return fail("no file");
    char magic[4];
    u32 version = 0, crc = 0, size = 0, sessions = 0;
    if (!in.read(magic, 4) || std::memcmp(magic, kMagic, 4) != 0)
        return fail("not a ROM map file");
    if (!get32(in, version) || version < 1 || version > kVersion)
        return fail("unsupported ROM map version");
    if (!get32(in, crc) || !get32(in, size) || !get32(in, sessions))
        return fail("truncated header");
    if (crc != m_crc || size != m_romSize)
        return fail("file belongs to a different ROM");
    std::vector<u8> bytes(size);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size))
        return fail("truncated map");
    std::vector<u8> raw(static_cast<size_t>(size) * 2);
    const auto getWords = [&in, &raw, size](std::vector<u16>& words) {
        if (!in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size())))
            return false;
        for (u32 i = 0; i < size; ++i)
            words[i] = static_cast<u16>(raw[i * 2] | (raw[i * 2 + 1] << 8));
        return true;
    };
    std::vector<u16> destinations(size, NoDestination);
    std::vector<u16> readers(size, NoDestination);
    std::vector<u16> readerRoutines(size, NoDestination);
    if (version >= 2 && !getWords(destinations))
        return fail("truncated destinations");
    if (version >= 3 && (!getWords(readers) || !getWords(readerRoutines)))
        return fail("truncated readers");
    m_bytes.swap(bytes);
    m_destinations.swap(destinations);
    m_readers.swap(readers);
    m_readerRoutines.swap(readerRoutines);
    m_sessions = sessions;
    Recount();
    return true;
}
