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
        const u8 opcode = m_memory->Peek(address + i);
        if (opcode != record->opcodes[i])
            changed = true;
    }

    if (record->size != 0 && !changed)
        return;

    std::vector<u8> bytes;
    u16 opcodeAddress = address;
    u8 opcodeValue = m_memory->Peek(opcodeAddress);

    while (opcodeValue == 0xDD || opcodeValue == 0xFD)
    {
        bytes.push_back(opcodeValue);
        ++opcodeAddress;
        opcodeValue = m_memory->Peek(opcodeAddress);
    }

    for (int i = 0; i < 5; ++i)
        bytes.push_back(m_memory->Peek(opcodeAddress + i));

    Decoded decoded;
    Decode(bytes.data(), static_cast<int>(bytes.size()), address, decoded);
    record->size = decoded.size;
    record->address = address;
    std::snprintf(record->name, sizeof(record->name), "%s", decoded.name);
    record->jump = decoded.jump;
    record->jump_address = decoded.jumpAddress;
    record->subroutine = decoded.subroutine;
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

    // Assign stable, automatic labels to branch destinations.  User-supplied
    // symbols remain higher priority in the GUI, but these labels make a raw
    // ROM readable even when no .sym file is available.
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

void Z80Disassembler::Decode(const u8* bytes, int count, u16 address, Decoded& out)
{
    int first = 0;
    u8 indexPrefix = 0;
    while (first < count - 5 && (bytes[first] == 0xDD || bytes[first] == 0xFD))
        indexPrefix = bytes[first++];

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
        out.size = info.size + first;
    else
        out.size = info.size + (first > 1 ? first - 1 : 0);

    out.name[0] = 0;
    out.jump = false;
    out.jumpAddress = 0;
    out.subroutine = false;
    out.operandKind = Decoded::Operand_None;
    out.operand = 0;
    out.relative = false;
    out.rst = false;
    const int nameFirst = first + (prefixed ? 1 : 0);
    switch (info.type)
    {
        case GS_OPCode_Type_Implied:
            std::snprintf(out.name, sizeof(out.name), "%s", info.name);
            break;
        case GS_OPCode_Type_Index:
            std::snprintf(out.name, sizeof(out.name), info.name,
                          static_cast<s8>(bytes[nameFirst]));
            break;
        case GS_OPCode_Type_1b:
            std::snprintf(out.name, sizeof(out.name), info.name,
                          bytes[nameFirst + 1]);
            out.operand = bytes[nameFirst + 1];
            out.operandKind = (!prefixed && (opcode == 0xDB || opcode == 0xD3))
                ? Decoded::Operand_Port : Decoded::Operand_Imm8;
            break;
        case GS_OPCode_Type_2b:
        {
            const u16 operand = (bytes[nameFirst + 2] << 8) | bytes[nameFirst + 1];
            if (!prefixed && (opcode == 0xC3 || opcode == 0xCD ||
                              (opcode & 0xC7) == 0xC2 ||
                              (opcode & 0xC7) == 0xC4))
            {
                out.jump = true;
                out.jumpAddress = operand;
            }
            std::snprintf(out.name, sizeof(out.name), info.name, operand);
            out.operand = operand;
            if (out.jump)
                out.operandKind = Decoded::Operand_Jump;
            else if (std::strstr(info.name, "(") != NULL)
                out.operandKind = Decoded::Operand_Memory;
            else
                out.operandKind = Decoded::Operand_Imm16;
            break;
        }
        case GS_OPCode_Type_Indexed:
            std::snprintf(out.name, sizeof(out.name), info.name,
                          static_cast<s8>(bytes[nameFirst + 1]));
            break;
        case GS_OPCode_Type_Relative:
        {
            out.jump = true;
            out.jumpAddress = address + out.size +
                              static_cast<s8>(bytes[nameFirst + 1]);
            out.operandKind = Decoded::Operand_Jump;
            out.operand = out.jumpAddress;
            out.relative = true;
            std::snprintf(out.name, sizeof(out.name), info.name,
                          out.jumpAddress,
                          static_cast<s8>(bytes[nameFirst + 1]));
            break;
        }
        case GS_OPCode_Type_Indexed_1b:
            std::snprintf(out.name, sizeof(out.name), info.name,
                          static_cast<s8>(bytes[nameFirst + 1]),
                          bytes[nameFirst + 2]);
            break;
        case GS_OPCode_Type_Data:
            std::snprintf(out.name, sizeof(out.name), "%s", info.name);
            break;
        default:
            std::snprintf(out.name, sizeof(out.name), "PARSE ERROR");
            break;
    }

    // Calls are subroutines; an RST is also a jump, to its vector.
    if (!prefixed)
    {
        if (opcode == 0xCD || (opcode & 0xC7) == 0xC4)
            out.subroutine = true;
        else if ((opcode & 0xC7) == 0xC7)
        {
            out.subroutine = true;
            out.jump = true;
            out.jumpAddress = opcode & 0x38;
            out.operandKind = Decoded::Operand_Jump;
            out.operand = out.jumpAddress;
            out.rst = true;
        }
    }
}
