/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026 Saverio Russo
 */

#include "gui_debug_disassembler_export.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "emu.h"
#include "../../src/gearsf7000.h"

namespace
{

// Same compact markup as draw_disassembler_instruction() in gui_debug.cpp
// ({n}=mnemonic, {o}=operand, {e}=relative-offset annotation, {s}=symbol) -
// duplicated here rather than shared, since that one is ImGui-drawing code
// and this one produces plain text for a file.
void strip_markup(const char* in, char* out, size_t out_size)
{
    size_t o = 0;
    for (const char* c = in; (*c != 0) && (o + 1 < out_size);)
    {
        if ((c[0] == '{') && (c[2] == '}') &&
            ((c[1] == 'n') || (c[1] == 'o') || (c[1] == 'e') || (c[1] == 's')))
        {
            c += 3;
            continue;
        }
        out[o++] = *c++;
    }
    out[o] = 0;
}

// SC3K-style gap fill: unknown bytes become `db` directives, 16 per line,
// instead of being silently skipped - the dump stays gapless from start to
// end of the address range instead of only showing known instructions.
void write_db_gap(FILE* file, const std::vector<u8>& bytes)
{
    for (size_t i = 0; i < bytes.size(); i++)
    {
        if ((i % 16) == 0)
            fprintf(file, (i == 0) ? "\tdb\t" : "\n\tdb\t");
        else
            fprintf(file, ", ");
        fprintf(file, "$%02X", bytes[i]);
    }
    fprintf(file, "\n");
}

void write_instruction_line(FILE* file, const char* label, Memory::stDisassembleRecord* record)
{
    char instr[96];
    strip_markup(record->name, instr, sizeof(instr));
    fprintf(file, "%s:\t%s\t;%s\n", label, instr, record->bytes);
}

// Dumps the disassembler's current live view: whatever GetDisassembleRecord()
// resolves for each CPU address right now, same records the Disassembler
// window itself is built from - respects the active mapper bank/IPL overlay.
// Gapless: addresses with no known instruction are read live via
// memory->Read() (so they go through the same mapper dispatch) and emitted
// as `db` blocks, same structure SC3K-System used for its ROM dump.
void save_visible(FILE* file, Memory* memory)
{
    int address = 0;
    char label[16];

    while (address < 0x10000)
    {
        Memory::stDisassembleRecord* record = memory->GetDisassembleRecord((u16)address, false);
        if ((record != nullptr) && (record->name[0] != 0))
        {
            snprintf(label, sizeof(label), "L%04X", address);
            write_instruction_line(file, label, record);
            address += (record->size > 0) ? record->size : 1;
            continue;
        }

        int gap_start = address;
        std::vector<u8> gap_bytes;
        while (address < 0x10000)
        {
            Memory::stDisassembleRecord* r = memory->GetDisassembleRecord((u16)address, false);
            if ((r != nullptr) && (r->name[0] != 0))
                break;
            gap_bytes.push_back(memory->Read((u16)address));
            address++;
        }
        fprintf(file, "L%04X:\n", gap_start);
        write_db_gap(file, gap_bytes);
        fprintf(file, "\n");
    }
}

// Dumps one physical disassembly map gaplessly - same principle as
// save_visible, but against a raw buffer instead of memory->Read(), so it
// stays mapper-agnostic (the point of "Full") instead of only reading
// whatever is CPU-visible right now.
void save_map_gapfilled(FILE* file, const char* segment, const u8* raw,
                         Memory::stDisassembleRecord** records, u32 size)
{
    if (raw == nullptr)
        return;

    u32 offset = 0;
    char label[24];

    while (offset < size)
    {
        Memory::stDisassembleRecord* record = (records != nullptr) ? records[offset] : nullptr;
        if ((record != nullptr) && (record->name[0] != 0))
        {
            snprintf(label, sizeof(label), "%s_%06X", segment, offset);
            write_instruction_line(file, label, record);
            offset += (u32)((record->size > 0) ? record->size : 1);
            continue;
        }

        u32 gap_start = offset;
        std::vector<u8> gap_bytes;
        while (offset < size)
        {
            Memory::stDisassembleRecord* r = (records != nullptr) ? records[offset] : nullptr;
            if ((r != nullptr) && (r->name[0] != 0))
                break;
            gap_bytes.push_back(raw[offset]);
            offset++;
        }
        fprintf(file, "%s_%06X:\n", segment, gap_start);
        write_db_gap(file, gap_bytes);
        fprintf(file, "\n");
    }
}

// Dumps every physical disassembly map unconditionally, independent of
// which bank/IPL-vs-RAM overlay happens to be active right now - the
// counterpart to the mapper-agnostic "internal buffers" design already
// used by Mem Import (point 1 of the same plan).
void save_full(FILE* file, Memory* memory, Cartridge* cartridge)
{
    save_map_gapfilled(file, "BIOS", memory->GetBios(), memory->GetDisassembledBiosMemoryMap(), 0x2000);
    save_map_gapfilled(file, "RAM", memory->GetRam(), memory->GetDisassembledRamMemoryMap(), 0x0800);
    save_map_gapfilled(file, "SGM", memory->GetSGMRam(), memory->GetDisassembledSGMRamMemoryMap(), 0x10000);
    save_map_gapfilled(file, "ROM", cartridge->GetROM(), memory->GetDisassembledRomMemoryMap(),
                        (u32)cartridge->GetROMSize());
}

}

bool gui_debug_save_disassembly(const char* file_path, bool full)
{
    FILE* file = fopen(file_path, "w");
    if (file == nullptr)
        return false;

    GearSF7000Core* core = emu_get_core();
    Memory* memory = core->GetMemory();
    Cartridge* cartridge = core->GetCartridge();

    if (full)
        save_full(file, memory, cartridge);
    else
        save_visible(file, memory);

    fclose(file);
    return true;
}
