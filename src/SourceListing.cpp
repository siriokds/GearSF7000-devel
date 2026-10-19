/*
 * GearSF7000 - a cartridge ROM as source, from the ROM and its ROM map
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SourceListing.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "RomMap.h"
#include "Z80Disassembler.h"

namespace
{
// The decoded text without its {x} colour markers, upper case: what the
// rules below test the mnemonic against.
std::string plain_text(const char* text)
{
    std::string result;
    for (const char* p = text; *p; p++)
    {
        if (p[0] == '{' && p[1] && p[2] == '}')
        {
            p += 2;
            continue;
        }
        result += static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
    }
    return result;
}

// The decoded text in lower case, as the export writes it, but for its
// $-numbers and the {x} markers.
std::string lower_text(const char* text)
{
    std::string result;
    bool number = false;
    for (const char* p = text; *p; p++)
    {
        if (p[0] == '{' && p[1] && p[2] == '}')
        {
            result.append(p, 3);
            p += 2;
            number = false;
            continue;
        }
        if (*p == '$')
            number = true;
        else if (number && !std::isxdigit(static_cast<unsigned char>(*p)))
            number = false;
        result += number ? *p : static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    }
    return result;
}

bool starts_with(const std::string& text, const char* prefix)
{
    return text.compare(0, std::strlen(prefix), prefix) == 0;
}

std::string hex_name(const char* prefix, u16 address)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%s%X_", prefix, address);
    return text;
}
}

std::string SourceListing::SourceText(const char* decodedName, bool rst, u16 rstVector)
{
    if (rst)
    {
        // The decoder writes "RST 38H"; the source says rst $38.
        char text[24];
        std::snprintf(text, sizeof(text), "{n}rst {o}$%02X", rstVector);
        return text;
    }
    // The decoder's aside ({e}[+5], the relative offset) is left out: in the
    // source the target has a name.
    std::string text = lower_text(decodedName);
    const std::size_t aside = text.find("{e}");
    if (aside != std::string::npos)
    {
        text.erase(aside);
        while (!text.empty() && text.back() == ' ')
            text.pop_back();
    }
    return text;
}

bool SourceListing::HasLabelAt(u32 offset) const
{
    if (offset >= m_romSize)
        return false;
    const u16 address = static_cast<u16>(m_base + offset);
    return m_references.count(address) != 0 || (m_symbols && !m_symbols(address).empty());
}

std::string SourceListing::CodeLabelName(u16 address)
{
    return hex_name("_LABEL_", address);
}

std::string SourceListing::DataLabelName(u16 address)
{
    return hex_name("_DATA_", address);
}

u8 SourceListing::KindAt(u32 offset) const
{
    return m_map->GetBytes()[offset] & RomMap::KindMask;
}

bool SourceListing::IsStart(u32 offset) const
{
    return (m_map->GetBytes()[offset] & RomMap::InstructionStart) != 0;
}

// Bytes up to the next instruction start or kind change, as the map
// recorded them.
u32 SourceListing::RecordedLength(u32 offset) const
{
    u32 end = offset + 1;
    while (end < m_romSize && KindAt(end) == RomMap::Kind_Code && !IsStart(end))
        end++;
    return end - offset;
}

void SourceListing::Build(const u8* rom, u32 romSize, u16 base, const RomMap& map,
                          const SymbolLookup& symbols, const AddressCheck& answers,
                          const std::vector<CodeRange>& chosen, const PortLookup& ports,
                          const std::vector<JumpTable>& tables, const std::vector<u32>& keptWords)
{
    m_rom = rom;
    m_base = base;
    m_map = &map;
    m_symbols = symbols;
    m_answers = answers;
    m_ports = ports;
    m_tables.clear();
    m_tableCode.clear();
    m_entryNotes.clear();
    m_seenWords.clear();
    m_keptWords.clear();
    m_instructions.clear();
    m_references.clear();
    m_guessedPointers.clear();
    m_symbolized.clear();
    m_lines.clear();

    // The map has one byte per ROM byte; a map of another size is not this
    // ROM's, and a ROM past the CPU's reach is not flat.
    m_romSize = (map.GetBytes().size() == romSize) ? romSize : 0;
    if (static_cast<u32>(base) + m_romSize > 0x10000)
        m_romSize = 0x10000 - base;
    if (m_romSize == 0)
        return;

    Decode();
    DecodeChosen(chosen);
    // The user's pointers, where nothing ran: the live wins.
    for (u32 at : keptWords)
        if (at + 1 < m_romSize && KindAt(at) != RomMap::Kind_Code && KindAt(at + 1) != RomMap::Kind_Code)
            m_keptWords.insert(at);
    // A table stops where executed code starts (a map newer than the
    // analysis may have grown into it); a vector table is jp instructions,
    // code itself.
    for (JumpTable table : tables)
    {
        if (!table.vector)
        {
            u32 n = 0;
            while (n < table.entries && table.base + n * 2 + 1 < m_romSize &&
                   KindAt(table.base + n * 2) != RomMap::Kind_Code &&
                   KindAt(table.base + n * 2 + 1) != RomMap::Kind_Code)
                n++;
            table.entries = n;
        }
        if (table.entries > 0)
            m_tables.push_back(table);
    }
    CollectReferences();
    // Where the user said code starts is named, jumped to or not (the NMI
    // entry is reached by the hardware).
    for (const CodeRange& range : chosen)
        if (range.start < m_romSize && m_instructions.count(static_cast<u16>(m_base + range.start)))
            m_references.insert(static_cast<u16>(m_base + range.start));
    Emit();
}

void SourceListing::Decode()
{
    u32 offset = 0;
    while (offset < m_romSize)
    {
        const u8 kind = KindAt(offset);
        if (kind != RomMap::Kind_Code || !IsStart(offset))
        {
            offset++;
            continue;
        }

        u8 bytes[16] = {};
        for (int i = 0; i < 16 && offset + i < m_romSize; i++)
            bytes[i] = m_rom[offset + i];
        int count = 0;
        while (count < 8 && (bytes[count] == 0xDD || bytes[count] == 0xFD))
            count++;
        count += 5;

        const u16 address = static_cast<u16>(m_base + offset);
        Z80Disassembler::Decoded decoded;
        Z80Disassembler::Decode(bytes, count, address, decoded);
        const u32 size = static_cast<u32>(decoded.size);
        // What ran has the length the map recorded, or it is not this.
        const bool fits = size > 0 && offset + size <= m_romSize &&
                          RecordedLength(offset) == size;
        if (!fits)
        {
            offset++;
            continue;
        }

        Instruction instruction;
        instruction.size = decoded.size;
        instruction.text = SourceText(decoded.name, decoded.rst, decoded.jumpAddress);
        instruction.plain = plain_text(decoded.name);
        instruction.operandKind = decoded.operandKind;
        instruction.operand = decoded.operand;
        instruction.relative = decoded.relative;
        instruction.rst = decoded.rst;
        instruction.jumpIsCall = decoded.subroutine;
        m_instructions[address] = instruction;
        offset += size;
    }
}

void SourceListing::DecodeChosen(const std::vector<CodeRange>& chosen)
{
    for (const CodeRange& range : chosen)
    {
        u32 offset = range.start;
        const u32 end = std::min(range.end, m_romSize);
        while (offset < end)
        {
            const u16 address = static_cast<u16>(m_base + offset);
            // What ran stays as it ran.
            auto executed = m_instructions.find(address);
            if (executed != m_instructions.end())
            {
                offset += static_cast<u32>(executed->second.size);
                continue;
            }
            if (KindAt(offset) == RomMap::Kind_Code)
            {
                offset++;
                continue;
            }
            u8 bytes[16] = {};
            for (int i = 0; i < 16 && offset + i < m_romSize; i++)
                bytes[i] = m_rom[offset + i];
            Z80Disassembler::Decoded decoded;
            Z80Disassembler::Decode(bytes, 16, address, decoded);
            const u32 size = static_cast<u32>(decoded.size);
            if (size == 0 || offset + size > m_romSize)
                break;

            Instruction instruction;
            instruction.size = decoded.size;
            instruction.text = SourceText(decoded.name, decoded.rst, decoded.jumpAddress);
            instruction.plain = plain_text(decoded.name);
            instruction.operandKind = decoded.operandKind;
            instruction.operand = decoded.operand;
            instruction.relative = decoded.relative;
            instruction.rst = decoded.rst;
            instruction.jumpIsCall = decoded.subroutine;
            instruction.chosen = true;
            // Seen read as data since: the choice is contradicted, and said
            // so rather than quietly kept or undone.
            for (u32 i = 0; i < size; i++)
            {
                const u8 kind = KindAt(offset + i);
                if (kind != RomMap::Kind_Unknown && kind != RomMap::Kind_CodeCandidate &&
                    kind != RomMap::Kind_Fill && kind != RomMap::Kind_Code)
                    instruction.contradicted = true;
            }
            m_instructions[address] = instruction;
            offset += size;
        }
    }
}

// A word inside data whose value is an executed instruction start: a pointer
// the map did not recognise.
bool SourceListing::WordHitsLabel(u32 offset) const
{
    // The cartridge signature ($AA55/$55AA at the very start) is not one.
    if (offset == 0 && m_base == 0x8000)
        return false;
    const u16 value = static_cast<u16>(m_rom[offset] | (m_rom[offset + 1] << 8));
    if (!InRom(value))
        return false;
    const u32 target = value - m_base;
    return IsStart(target) && KindAt(target) == RomMap::Kind_Code;
}

bool SourceListing::ImmediateIsAddress(const Instruction& instruction, u16 value) const
{
    const u32 offset = value - m_base;
    // A cartridge above $8000: a 16-bit constant that high is rare (VRAM
    // addresses stop at $3FFF, counts are small), so a value in the ROM range
    // is an address.
    if (m_base >= 0x8000)
        return true;
    if (IsStart(offset) && KindAt(offset) == RomMap::Kind_Code)
        return true;
    if (starts_with(instruction.plain, "LD IX,") || starts_with(instruction.plain, "LD IY,"))
        return true;
    const u8 kind = KindAt(offset);
    return starts_with(instruction.plain, "LD HL,") && kind != RomMap::Kind_Code &&
           kind != RomMap::Kind_CodeCandidate && kind != RomMap::Kind_Unknown;
}

void SourceListing::CollectReferences()
{
    for (const auto& entry : m_instructions)
    {
        const Instruction& instruction = entry.second;
        const u16 value = instruction.operand;
        switch (instruction.operandKind)
        {
            case Z80Disassembler::Decoded::Operand_Jump:
            case Z80Disassembler::Decoded::Operand_Memory:
                m_references.insert(value);
                break;
            case Z80Disassembler::Decoded::Operand_Imm16:
                if (InRom(value) && ImmediateIsAddress(instruction, value))
                {
                    m_references.insert(value);
                    m_symbolized.insert(entry.first);
                }
                break;
            default:
                break;
        }
    }

    // Jump tables: each entry's target is code - the table says so - named
    // and noted with which entry of which table it is.
    static const char* ordinals[] = {"th", "st", "nd", "rd"};
    for (const JumpTable& table : m_tables)
    {
        // The table's start is named, and a data run stops there.
        m_references.insert(static_cast<u16>(m_base + table.base));
        if (table.vector)
            continue;
        for (u32 n = 0; n < table.entries; n++)
        {
            const u32 at = table.base + n * 2;
            const u16 target = static_cast<u16>(m_rom[at] | (m_rom[at + 1] << 8));
            if (!InRom(target))
                continue;
            m_references.insert(target);
            m_tableCode.insert(target);
            const u32 k = n + 1;
            const char* suffix = (k % 100 >= 11 && k % 100 <= 13) ? "th" : ordinals[k % 10 < 4 ? k % 10 : 0];
            char note[96];
            std::snprintf(note, sizeof(note), "; %u%s entry of Jump Table from %X", k, suffix,
                          static_cast<u16>(m_base + table.base));
            if (!m_entryNotes.count(target))
                m_entryNotes[target] = note;
        }
    }

    // Entries seen in use (RomMap::GetObservedJumps): proof, as executed code
    // is - the word is an address, its target code.
    for (const RomMap::ObservedJump& seen : m_map->GetObservedJumps())
    {
        if (seen.entry + 1 >= m_romSize || KindAt(seen.entry) == RomMap::Kind_Code ||
            KindAt(seen.entry + 1) == RomMap::Kind_Code)
            continue;
        m_seenWords.insert(seen.entry);
        const u16 target = static_cast<u16>(m_rom[seen.entry] | (m_rom[seen.entry + 1] << 8));
        if (!InRom(target))
            continue;
        m_references.insert(target);
        m_tableCode.insert(target);
    }

    // The user's pointers: their targets are named, code or data by what
    // they point at.
    for (u32 at : m_keptWords)
    {
        const u16 target = static_cast<u16>(m_rom[at] | (m_rom[at + 1] << 8));
        if (InRom(target))
            m_references.insert(target);
    }

    // Pointers seen in use (RomMap::GetObservedPointers): a word read and
    // then followed - proof it is an address, what a relocation rewrites.
    for (const RomMap::ObservedPointer& seen : m_map->GetObservedPointers())
    {
        if (seen.entry + 1 >= m_romSize || KindAt(seen.entry) == RomMap::Kind_Code ||
            KindAt(seen.entry + 1) == RomMap::Kind_Code)
            continue;
        m_seenWords.insert(seen.entry);
        if (InRom(seen.target))
            m_references.insert(seen.target);
    }

    // Pointer tables: TABLE runs, and words in data runs that hit a label.
    u32 offset = 0;
    while (offset + 1 < m_romSize)
    {
        const u8 kind = KindAt(offset);
        const bool table = kind == RomMap::Kind_Table;
        // A word that points at code is a pointer only in bytes the map saw
        // read as data: in unknown bytes, any pair of values that happens to
        // hit an instruction became one (_LABEL_67_ on every line, dw
        // _LABEL_0_ out of zeros).
        // Nor in graphics: bytes copied to VRAM are patterns, names and
        // sprite attributes, never addresses (dw _LABEL_0_ out of GFX SPR).
        const bool read = kind == RomMap::Kind_Data || kind == RomMap::Kind_Sound;
        if (table || (read && WordHitsLabel(offset)))
        {
            const u16 value = static_cast<u16>(m_rom[offset] | (m_rom[offset + 1] << 8));
            if (InRom(value))
            {
                m_references.insert(value);
                if (!table)
                    m_guessedPointers.insert(static_cast<u16>(m_base + offset));
            }
            offset += 2;
        }
        else
            offset++;
    }
}

std::string SourceListing::Label(u16 address) const
{
    if (m_symbols)
    {
        const std::string name = m_symbols(address);
        if (!name.empty())
            return name;
    }
    if (!InRom(address))
        return "";
    const u8 kind = KindAt(address - m_base);
    const bool code = m_instructions.count(address) != 0 || kind == RomMap::Kind_Code ||
                      m_tableCode.count(address) != 0;
    return code ? CodeLabelName(address) : DataLabelName(address);
}

// The instruction text with its address operand replaced by a name, where
// the export would write one.
std::string SourceListing::Resolve(const Instruction& instruction, u16 pc) const
{
    const u16 value = instruction.operand;
    if (instruction.operandKind == Z80Disassembler::Decoded::Operand_Port && m_ports)
    {
        // in a,($DC) / out ($7F),a: the board's name for the port.
        const bool out = starts_with(instruction.plain, "OUT");
        const std::string name = m_ports(static_cast<u8>(value), out);
        char number[8];
        std::snprintf(number, sizeof(number), "$%02X", value);
        std::string text = instruction.text;
        const std::size_t at = text.find(number);
        if (!name.empty() && at != std::string::npos)
            text.replace(at, std::strlen(number), "{s}" + name + "{o}");
        return text;
    }
    bool named = false;
    switch (instruction.operandKind)
    {
        case Z80Disassembler::Decoded::Operand_Jump:
            named = !instruction.rst;
            break;
        case Z80Disassembler::Decoded::Operand_Memory:
            named = true;
            break;
        case Z80Disassembler::Decoded::Operand_Imm16:
            named = m_references.count(value) != 0 && m_symbolized.count(pc) != 0;
            break;
        default:
            break;
    }
    if (!named)
        return instruction.text;
    const std::string name = InRom(value) ? Label(value)
                                          : (m_symbols ? m_symbols(value) : std::string());
    if (name.empty())
        return instruction.text;

    char number[8];
    std::snprintf(number, sizeof(number), "$%04X", value);
    std::string text = instruction.text;
    const std::size_t at = text.find(number);
    if (at != std::string::npos)
        text.replace(at, std::strlen(number), "{s}" + name + "{o}");
    return text;
}

void SourceListing::Emit()
{
    int block = -1;
    u8 blockKind = 0xFF;
    u32 offset = 0;
    while (offset < m_romSize)
    {
        const u16 address = static_cast<u16>(m_base + offset);
        const std::string symbol = m_symbols ? m_symbols(address) : std::string();
        auto note = m_entryNotes.find(address);
        if (note != m_entryNotes.end())
            m_lines.push_back({Line_Comment, 0, false, offset, 0, -1, note->second, ""});
        if (m_references.count(address) || !symbol.empty())
            m_lines.push_back({Line_Label, 0, false, offset, 0, -1, Label(address), ""});

        // A jump table: its own header, one entry a row.
        const JumpTable* table = nullptr;
        for (const JumpTable& t : m_tables)
            if (t.base == offset)
                table = &t;
        if (table)
        {
            char header[160];
            std::snprintf(header, sizeof(header), "; Jump %sTable from %X (%u entries, indexed by %s)",
                          table->vector ? "Vector " : "", address, table->entries,
                          table->index.c_str());
            if (table->vector)
            {
                // Its entries are jp instructions: shown as what they are.
                m_lines.push_back({Line_Comment, 0, false, offset, 0, -1, header, ""});
            }
            else
            {
                block = static_cast<int>(m_lines.size());
                m_lines.push_back({Line_DataHeader, RomMap::Kind_Table, false, offset,
                                   static_cast<u16>(table->entries * 2), block, header, ""});
                for (u32 n = 0; n < table->entries; n++)
                {
                    const u32 at = offset + n * 2;
                    const u16 value = static_cast<u16>(m_rom[at] | (m_rom[at + 1] << 8));
                    const std::string name = Label(value);
                    char number[8];
                    std::snprintf(number, sizeof(number), "$%04X", value);
                    Line row = {Line_Words, RomMap::Kind_Table, false, at, 2, block,
                                "{n}dw " + (name.empty() ? std::string("{o}") + number
                                                         : "{s}" + name), ""};
                    row.seen = m_seenWords.count(at) != 0;
                    m_lines.push_back(row);
                }
                blockKind = 0xFF;
                offset += table->entries * 2;
                continue;
            }
        }

        const u8 raw = m_map->GetBytes()[offset];
        const u8 kind = raw & RomMap::KindMask;
        auto found = m_instructions.find(address);
        if (found != m_instructions.end())
        {
            const Instruction& instruction = found->second;
            Line line = {Line_Instruction, kind, false, offset,
                         static_cast<u16>(instruction.size), -1,
                         Resolve(instruction, address), ""};
            line.chosen = instruction.chosen;
            line.contradicted = instruction.contradicted;
            m_lines.push_back(line);
            blockKind = 0xFF;
            offset += instruction.size;
            continue;
        }

        // A data run: up to the next label, instruction or kind change.
        u32 end = offset + 1;
        while (end < m_romSize)
        {
            const u16 next = static_cast<u16>(m_base + end);
            if (m_instructions.count(next) || m_references.count(next) ||
                KindAt(end) != kind || (m_symbols && !m_symbols(next).empty()))
                break;
            end++;
        }

        // A block, for the header and for folding, is the whole stretch of
        // one kind up to the next instruction or kind change, labels and all.
        const bool candidate = (raw & RomMap::CandidateFlag) != 0;
        if (kind != blockKind)
        {
            u32 blockEnd = end;
            while (blockEnd < m_romSize && KindAt(blockEnd) == kind &&
                   !m_instructions.count(static_cast<u16>(m_base + blockEnd)))
                blockEnd++;
            block = static_cast<int>(m_lines.size());
            blockKind = kind;
            RomMap::Range range = {offset, blockEnd - 1, kind, candidate};
            // "; Data from 20 to 27 (8 bytes) - GFX BG", as Emulicious writes
            // it; the kind is left out for UNKNOWN, which is what "Data"
            // says already.
            const u32 bytes = blockEnd - offset;
            char header[160];
            std::snprintf(header, sizeof(header), "; Data from %X to %X (%u byte%s)",
                          address, static_cast<u16>(m_base + blockEnd - 1), bytes,
                          bytes == 1 ? "" : "s");
            std::string text = header;
            const std::string pointer = PointerHint(offset, blockEnd);
            if (!pointer.empty())
                text += " - " + pointer;
            // Deep Search's data step, for bytes nobody read.
            if (kind == RomMap::Kind_Unknown || kind == RomMap::Kind_CodeCandidate)
                if (const RomMap::DataHint* data = m_map->DataHintAt(offset))
                    text += " - " + data->text;
            if (kind == RomMap::Kind_CodeCandidate)
                text += " - " + CandidateHint(offset, blockEnd);
            else if (kind != RomMap::Kind_Unknown)
                text += " - " + RomMap::RangeName(range);
            // A cartridge at $0000 owns the Z80's NMI entry: code that runs
            // only when the NMI fires (the pause key on the SG-1000, reset
            // on the SC-3000), so play rarely proves it.
            if (m_base == 0 && offset <= 0x66 && blockEnd > 0x66)
                text += " - NMI entry at 66 (pause/reset key)";
            m_lines.push_back({Line_DataHeader, kind, candidate, offset,
                               static_cast<u16>(std::min<u32>(bytes, 0xFFFF)),
                               block, text, m_map->DescribeEvidence(range)});
        }
        EmitData(offset, end, kind, candidate, block);
        offset = end;
    }
}

// A word the listing writes as a pointer: a jump table entry, a TABLE
// word, a guessed pointer, an entry seen in use or one the user kept.
bool SourceListing::PointerWordAt(u32 offset) const
{
    if (offset + 1 >= m_romSize)
        return false;
    if (KindAt(offset) == RomMap::Kind_Table && KindAt(offset + 1) == RomMap::Kind_Table)
        return true;
    if (m_guessedPointers.count(static_cast<u16>(m_base + offset)) || m_seenWords.count(offset) ||
        m_keptWords.count(offset))
        return true;
    for (const JumpTable& t : m_tables)
        if (!t.vector && offset >= t.base && offset < t.base + t.entries * 2 && (offset - t.base) % 2 == 0)
            return true;
    return false;
}

// Two bytes nobody recognised, with a pointer on each side, are most likely
// a pointer too (Saverio: two loose bytes among pointers, at Orguss's $176F,
// pointing at a lone ret). A hint, never a decision: the reader keeps it.
std::string SourceListing::PointerHint(u32 start, u32 end) const
{
    if (end - start != 2 || start < 2 || start + 1 >= m_romSize)
        return "";
    const u8 kind = KindAt(start);
    if ((kind != RomMap::Kind_Unknown && kind != RomMap::Kind_CodeCandidate) ||
        m_keptWords.count(start) || !PointerWordAt(start - 2) || !PointerWordAt(end))
        return "";
    const u16 target = static_cast<u16>(m_rom[start] | (m_rom[start + 1] << 8));
    if (!InRom(target))
        return "";
    const u32 at = target - m_base;
    const char* what = "";
    if (m_rom[at] == 0xC9)
        what = " (a ret)";      // a lone ret among code: the end of a routine
    else if (m_instructions.count(target) || KindAt(at) == RomMap::Kind_Code)
        what = " (code)";
    char hint[80];
    std::snprintf(hint, sizeof(hint), "pointer? like its neighbours, to %X%s", target, what);
    return hint;
}

std::string SourceListing::CandidateHint(u32 start, u32 end) const
{
    // Decoded linearly, as a reader would try it: what argues against code.
    std::vector<std::string> against;
    std::string previousDest;       // "A" after LD A,x
    u32 offset = start;
    while (offset < end && offset < m_romSize)
    {
        u8 bytes[16] = {};
        for (int i = 0; i < 16 && offset + i < m_romSize; i++)
            bytes[i] = m_rom[offset + i];
        int count = 0;
        while (count < 8 && (bytes[count] == 0xDD || bytes[count] == 0xFD))
            count++;
        count += 5;
        const u16 address = static_cast<u16>(m_base + offset);
        Z80Disassembler::Decoded decoded;
        Z80Disassembler::Decode(bytes, count, address, decoded);
        const std::string plain = plain_text(decoded.name);
        char note[64];

        if (decoded.size <= 0 || plain.find("PARSE ERROR") != std::string::npos ||
            plain.find("[UNDOC]") != std::string::npos)
        {
            std::snprintf(note, sizeof(note), "invalid opcode at %X", address);
            against.push_back(note);
        }
        else if (bytes[0] == 0xFF && offset + 1 < end && m_rom[offset + 1] == 0xFF)
        {
            std::snprintf(note, sizeof(note), "rst $38 twice at %X", address);
            against.push_back(note);
        }
        else if (m_answers && !decoded.rst && !decoded.relative &&
                 (decoded.operandKind == Z80Disassembler::Decoded::Operand_Jump ||
                  decoded.operandKind == Z80Disassembler::Decoded::Operand_Memory) &&
                 !m_answers(decoded.operand))
        {
            std::snprintf(note, sizeof(note), "%s $%04X, nothing there, at %X",
                          decoded.operandKind == Z80Disassembler::Decoded::Operand_Jump
                              ? (decoded.subroutine ? "call to" : "jump to") : "access to",
                          decoded.operand, address);
            against.push_back(note);
        }

        // Two loads of one register in a row, the second not reading it:
        // the first did nothing.
        std::string dest, source;
        if (starts_with(plain, "LD "))
        {
            const std::size_t comma = plain.find(',');
            if (comma != std::string::npos)
            {
                dest = plain.substr(3, comma - 3);
                source = plain.substr(comma + 1);
            }
        }
        const bool registerDest = !dest.empty() && dest.find('(') == std::string::npos;
        if (registerDest && dest == previousDest && source.find(dest) == std::string::npos)
        {
            std::string lower = dest;
            for (char& c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            std::snprintf(note, sizeof(note), "ld %s overwritten at %X", lower.c_str(), address);
            against.push_back(note);
        }
        previousDest = registerDest ? dest : std::string();

        offset += decoded.size > 0 ? static_cast<u32>(decoded.size) : 1;
    }

    if (!against.empty())
    {
        std::string hint = "probably data: " + against.front();
        if (against.size() > 1)
            hint += " (+" + std::to_string(against.size() - 1) + " more)";
        return hint;
    }

    // Nothing against it: where the guess came from - Deep Search's flow,
    // or a rule of the map.
    if (const RomMap::DeducedBlock* deduced = m_map->DeducedAt(start))
    {
        char text[128];
        std::snprintf(text, sizeof(text), "code? %s (%d%% like the game's code)", deduced->reason.c_str(),
                      deduced->score);
        return text;
    }
    const u16 site = m_map->GetDestination(start);
    const bool orphan = (m_map->GetBytes()[start] & RomMap::OrphanFlag) != 0;
    char hint[64];
    if (site == RomMap::NoDestination || site >= m_romSize)
        std::snprintf(hint, sizeof(hint), "code?");
    else if (orphan)
        std::snprintf(hint, sizeof(hint), "code? a routine nothing called, after %X",
                      static_cast<u16>(m_base + site));
    else
        std::snprintf(hint, sizeof(hint), "code? branch at %X never taken",
                      static_cast<u16>(m_base + site));
    return hint;
}

void SourceListing::EmitData(u32 start, u32 end, u8 kind, bool candidate, int block)
{
    auto word_at = [&](u32 p) { return static_cast<u16>(m_rom[p] | (m_rom[p + 1] << 8)); };
    auto is_pointer = [&](u32 p)
    {
        if (p + 1 >= end)
            return false;
        const u16 value = word_at(p);
        return (kind == RomMap::Kind_Table && InRom(value)) ||
               m_guessedPointers.count(static_cast<u16>(m_base + p)) != 0 ||
               m_seenWords.count(p) != 0 || m_keptWords.count(p) != 0;
    };

    u32 p = start;
    while (p < end)
    {
        if (is_pointer(p))
        {
            const u16 value = word_at(p);
            // An address outside the cartridge (RAM) has no name yet: its number.
            std::string name = Label(value);
            char number[8];
            std::snprintf(number, sizeof(number), "$%04X", value);
            Line row = {Line_Words, kind, candidate, p, 2, block,
                        name.empty() ? std::string("{n}dw {o}") + number : "{n}dw {s}" + name, ""};
            row.seen = m_seenWords.count(p) != 0;
            row.chosen = m_keptWords.count(p) != 0;
            m_lines.push_back(row);
            p += 2;
            continue;
        }
        u32 n = 1;
        while (n < 16 && p + n < end && !m_references.count(static_cast<u16>(m_base + p + n)) &&
               !is_pointer(p + n))
            n++;
        std::string text = "{n}db {o}";
        char byte[8];
        for (u32 i = 0; i < n; i++)
        {
            std::snprintf(byte, sizeof(byte), i ? ",$%02X" : "$%02X", m_rom[p + i]);
            text += byte;
        }
        m_lines.push_back({Line_Data, kind, candidate, p, static_cast<u16>(n), block, text, ""});
        p += n;
    }
}
