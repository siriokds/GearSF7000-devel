/*
 * GearSF7000 - CPU-independent Z80 disassembler service
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Z80Disassembler.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "Memory.h"
#include "opcode_names.h"

Z80Disassembler::Z80Disassembler(Memory* memory) : m_memory(memory)
{
}

void Z80Disassembler::OnCpuInstructionBoundary(u16 nextProgramCounter,
                                                u64 elapsedTStates)
{
    Disassemble(nextProgramCounter);
    Memory::stDisassembleRecord* record =
        m_memory->GetDisassembleRecord(nextProgramCounter, false);
    if (!IsValidPointer(record))
        return;
    if (record->execution_count == 0)
        record->first_execution_tstate = elapsedTStates;
    if (record->execution_count != std::numeric_limits<u64>::max())
        ++record->execution_count;
    record->last_execution_tstate = elapsedTStates;
}

void Z80Disassembler::Disassemble(u16 address)
{
    Memory::stDisassembleRecord* record =
        m_memory->GetDisassembleRecord(address, true);
    if (!IsValidPointer(record))
        return;

    bool changed = false;
    const int maxSize = std::min(record->size, 7);
    for (int i = 0; i < maxSize; ++i)
    {
        const u8 opcode = m_memory->Read(address + i);
        if (opcode != record->opcodes[i])
            changed = true;
    }

    if (record->size != 0 && !changed)
        return;

    std::vector<u8> bytes;
    u16 opcodeAddress = address;
    u8 opcodeValue = m_memory->Read(opcodeAddress);
    u8 indexPrefix = 0;
    int first = 0;

    while (opcodeValue == 0xDD || opcodeValue == 0xFD)
    {
        indexPrefix = opcodeValue;
        bytes.push_back(opcodeValue);
        ++opcodeAddress;
        ++first;
        opcodeValue = m_memory->Read(opcodeAddress);
    }

    for (int i = 0; i < 5; ++i)
        bytes.push_back(m_memory->Read(opcodeAddress + i));

    u8 opcode = bytes[first];
    stOPCodeInfo info;
    bool prefixed = false;

    if (opcode == 0xCB)
    {
        prefixed = true;
        if (indexPrefix == 0xDD)
        {
            opcode = bytes[first + 2];
            info = kOPCodeDDCBNames[opcode];
        }
        else if (indexPrefix == 0xFD)
        {
            opcode = bytes[first + 2];
            info = kOPCodeFDCBNames[opcode];
        }
        else
        {
            opcode = bytes[first + 1];
            info = kOPCodeCBNames[opcode];
        }
    }
    else if (opcode == 0xED)
    {
        prefixed = true;
        opcode = bytes[first + 1];
        info = kOPCodeEDNames[opcode];
    }
    else if (indexPrefix == 0xDD)
        info = kOPCodeDDNames[opcode];
    else if (indexPrefix == 0xFD)
        info = kOPCodeFDNames[opcode];
    else
        info = kOPCodeNames[opcode];

    if (first > 0 && bytes[first] == 0xED)
        record->size = info.size + first;
    else
        record->size = info.size + (first > 1 ? first - 1 : 0);

    record->address = address;
    record->name[0] = 0;
    record->bytes[0] = 0;
    record->jump = false;
    record->jump_address = 0;
    record->subroutine = false;
    int byteTextPosition = 0;
    for (int i = 0; i < static_cast<int>(bytes.size()); ++i)
    {
        if (i < record->size)
        {
            static const char hex[] = "0123456789ABCDEF";
            const u8 byte = bytes[i];
            record->bytes[byteTextPosition++] = hex[byte >> 4];
            record->bytes[byteTextPosition++] = hex[byte & 0x0F];
            record->bytes[byteTextPosition++] = ' ';
        }

        if (i < 7)
            record->opcodes[i] = bytes[i];
    }
    record->bytes[byteTextPosition] = 0;

    const int nameFirst = first + (prefixed ? 1 : 0);
    switch (info.type)
    {
        case GS_OPCode_Type_Implied:
            std::snprintf(record->name, sizeof(record->name), "%s", info.name);
            break;
        case GS_OPCode_Type_Index:
            std::snprintf(record->name, sizeof(record->name), info.name,
                          static_cast<s8>(bytes[nameFirst]));
            break;
        case GS_OPCode_Type_1b:
            std::snprintf(record->name, sizeof(record->name), info.name,
                          bytes[nameFirst + 1]);
            break;
        case GS_OPCode_Type_2b:
        {
            const u16 operand = (bytes[nameFirst + 2] << 8) | bytes[nameFirst + 1];
            if (!prefixed && (opcode == 0xC3 || opcode == 0xCD ||
                              (opcode & 0xC7) == 0xC2 ||
                              (opcode & 0xC7) == 0xC4))
            {
                record->jump = true;
                record->jump_address = operand;
            }
            std::snprintf(record->name, sizeof(record->name), info.name, operand);
            break;
        }
        case GS_OPCode_Type_Indexed:
            std::snprintf(record->name, sizeof(record->name), info.name,
                          static_cast<s8>(bytes[nameFirst + 1]));
            break;
        case GS_OPCode_Type_Relative:
        {
            record->jump = true;
            record->jump_address = address + record->size +
                                   static_cast<s8>(bytes[nameFirst + 1]);
            std::snprintf(record->name, sizeof(record->name), info.name,
                          record->jump_address,
                          static_cast<s8>(bytes[nameFirst + 1]));
            break;
        }
        case GS_OPCode_Type_Indexed_1b:
            std::snprintf(record->name, sizeof(record->name), info.name,
                          static_cast<s8>(bytes[nameFirst + 1]),
                          bytes[nameFirst + 2]);
            break;
        case GS_OPCode_Type_Data:
            std::snprintf(record->name, sizeof(record->name), "%s", info.name);
            break;
        default:
            std::snprintf(record->name, sizeof(record->name), "PARSE ERROR");
            break;
    }

    // Assign stable, automatic labels to branch destinations.  User-supplied
    // symbols remain higher priority in the GUI, but these labels make a raw
    // ROM readable even when no .sym file is available.
    if (!prefixed)
    {
        if (opcode == 0xCD || (opcode & 0xC7) == 0xC4)
            record->subroutine = true;
        else if ((opcode & 0xC7) == 0xC7)
        {
            record->subroutine = true;
            record->jump = true;
            record->jump_address = opcode & 0x38;
        }
    }

    if (record->jump)
    {
        Memory::stDisassembleRecord* target =
            m_memory->GetDisassembleRecord(record->jump_address, true);
        if (IsValidPointer(target))
        {
            const char* prefix = record->subroutine ? "SUB" : "TAG";
            std::snprintf(target->auto_symbol, sizeof(target->auto_symbol),
                          "%s_%02X_%04X", prefix,
                          target->bank < 0 ? 0 : target->bank,
                          record->jump_address);
        }
    }
}
