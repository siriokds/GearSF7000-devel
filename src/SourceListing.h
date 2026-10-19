/*
 * GearSF7000 - a cartridge ROM as source, from the ROM and its ROM map
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_SOURCE_LISTING_H
#define GEARSF7000_SOURCE_LISTING_H

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "definitions.h"

class RomMap;

// The whole cartridge ROM as a listing of source lines: what ran is code,
// everything else is data runs with their kind from the map, and every
// address something refers to has a label. The rules are those of
// tools/game_export.py, so what the Disassembler window shows is what an
// export writes: executed instruction starts (and CODE? candidates, decoded
// linearly inside their run) are instructions; a jump, call or (nn) into the
// ROM names its target; a 16-bit immediate names it only when it is clearly
// an address; TABLE runs, and words in data that point at executed code, are
// pointers (dw). A data run ends at the next label, instruction or kind
// change and is written in rows of up to 16 bytes.
//
// Only what ran is code. CODE? - bytes a rule of the map guessed are code,
// the side of a branch never taken or a routine nothing called - is written
// as data (Saverio: a program cannot tell code from bytes that merely decode;
// the reader can). Its header carries a hint drawn from the bytes themselves:
// what argues against code (two loads of one register in a row, rst $38
// twice, a jump to where nothing answers, an invalid opcode), or, with
// nothing against it, where the guess came from.
//
// Names follow Emulicious: _LABEL_<hex>_ for code, _DATA_<hex>_ for data, by
// CPU address; a symbol the user gave wins. Firmware, RAM and port names are
// not here yet.
//
// Flat ROMs only: the ROM is seen whole at base (no banking).
class SourceListing
{
public:
    enum LineType : u8
    {
        Line_Label,
        Line_Instruction,
        Line_DataHeader,    // start of a data run: range and kind
        Line_Data,          // db, up to 16 bytes
        Line_Words,         // dw, one pointer
        Line_Comment        // "; 4th entry of Jump Table from 1741"
    };

    struct Line
    {
        u8 type;
        u8 kind;            // RomMap kind of the bytes
        bool candidate;     // CODE? or a statically inferred kind
        u32 offset;         // in the ROM
        u16 count;          // bytes the line covers (a data header: the run)
        int block;          // data lines: the run they belong to; -1 otherwise
        // Line_Label: the name. Line_Instruction: the decoded text with the
        // {n}/{o}/{e} markers and {s} before a name that replaced a number.
        // Line_Data/Line_Words: "db ..." / "dw ..." likewise. Lower case
        // but for numbers and names, as the export writes it.
        std::string text;
        // Line_DataHeader: why the run has its kind ("read by sound routine
        // $0100"), from the map; empty otherwise.
        std::string detail;
        // Line_Instruction: code because the user said so (a kept range),
        // not because it ran; and whether the map has seen one of its bytes
        // read as data since. Line_Words: a pointer the user said is one.
        bool chosen = false;
        bool contradicted = false;
        // Line_Words: a jump table entry seen in use while the game ran.
        bool seen = false;
    };

    // A jump table found by JumpTables (rommap/jumptables.txt): its entries
    // are written one dw a row, its targets named as code, each with a
    // comment saying which entry of which table it is.
    struct JumpTable
    {
        u32 base;           // ROM offset
        u32 entries;
        bool vector;        // entries are 3-byte jp instructions
        std::string index;  // "_RAM_C034_", "(ix+4)", "unknown"
    };

    // A range the user keeps as code: ROM offsets, end exclusive.
    struct CodeRange
    {
        u32 start;
        u32 end;
    };

    // A user symbol for an address, or empty.
    using SymbolLookup = std::function<std::string(u16 address)>;
    // Whether anything answers at an address on the board (Memory::AnswersAt);
    // without one, that check is skipped.
    using AddressCheck = std::function<bool(u16 address)>;
    // The board's name for a port, read (in) or written (out), or empty.
    using PortLookup = std::function<std::string(u8 port, bool out)>;

    // chosen: the ranges the user keeps as code (rommap/choices.txt). They
    // are decoded linearly and never change what ran. keptWords: ROM offsets
    // of words the user keeps as pointers (choices.txt too), written as dw
    // with their target named, where no executed byte is.
    void Build(const u8* rom, u32 romSize, u16 base, const RomMap& map,
               const SymbolLookup& symbols, const AddressCheck& answers = nullptr,
               const std::vector<CodeRange>& chosen = {},
               const PortLookup& ports = nullptr,
               const std::vector<JumpTable>& tables = {},
               const std::vector<u32>& keptWords = {});

    // The hint for a lone word nobody recognised between two pointers:
    // "pointer? like its neighbours, to 2135 (a ret)", or "". Public for the
    // tests.
    std::string PointerHint(u32 start, u32 end) const;

    // The hint for a CODE? run [start, end): "probably data: ..." or
    // "code? ...". Public for the tests.
    std::string CandidateHint(u32 start, u32 end) const;

    const std::vector<Line>& GetLines() const { return m_lines; }
    u16 GetBase() const { return m_base; }
    u32 GetRomSize() const { return m_romSize; }

    // The name an address gets, or "" outside the ROM with no symbol.
    std::string Label(u16 address) const;

    // A decoded instruction's text as the listing writes it: lower case but
    // for numbers, with the {x} markers; rst as $38.
    static std::string SourceText(const char* decodedName, bool rst, u16 rstVector);
    // Whether a line of the listing starts a label at this ROM offset.
    bool HasLabelAt(u32 offset) const;

    static std::string CodeLabelName(u16 address);
    static std::string DataLabelName(u16 address);

private:
    struct Instruction
    {
        int size;
        std::string text;       // with markers
        std::string plain;      // without, upper case
        u8 operandKind;
        u16 operand;
        bool relative;
        bool rst;
        bool jumpIsCall;
        bool chosen = false;
        bool contradicted = false;
    };

    bool InRom(u16 address) const
    {
        return address >= m_base && static_cast<u32>(address - m_base) < m_romSize;
    }
    u8 KindAt(u32 offset) const;
    bool IsStart(u32 offset) const;
    u32 RecordedLength(u32 offset) const;
    void Decode();
    void DecodeChosen(const std::vector<CodeRange>& chosen);
    void CollectReferences();
    bool WordHitsLabel(u32 offset) const;
    bool PointerWordAt(u32 offset) const;
    bool ImmediateIsAddress(const Instruction& instruction, u16 value) const;
    std::string Resolve(const Instruction& instruction, u16 pc) const;
    void Emit();
    void EmitData(u32 start, u32 end, u8 kind, bool candidate, int block);

    const u8* m_rom = nullptr;
    u32 m_romSize = 0;
    u16 m_base = 0;
    const RomMap* m_map = nullptr;
    SymbolLookup m_symbols;
    AddressCheck m_answers;
    PortLookup m_ports;

    std::unordered_map<u16, Instruction> m_instructions;   // by CPU address
    std::unordered_set<u16> m_references;
    std::unordered_set<u16> m_guessedPointers;             // where the word is
    std::unordered_set<u16> m_symbolized;                   // imm16 named, by pc
    std::vector<JumpTable> m_tables;
    std::unordered_set<u16> m_tableCode;        // table targets: code by the table's word
    std::unordered_map<u16, std::string> m_entryNotes;  // target -> "; 4th entry of ..."
    std::unordered_set<u32> m_seenWords;        // ROM offsets of entries seen in use
    std::unordered_set<u32> m_keptWords;        // ROM offsets of the user's pointers
    std::vector<Line> m_lines;
};

#endif
