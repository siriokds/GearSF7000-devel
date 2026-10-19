/*
 * GearSF7000 - CPU-independent Z80 disassembler service
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_Z80_DISASSEMBLER_H
#define GEARSF7000_Z80_DISASSEMBLER_H

#include "cpu/CpuInstructionObserver.h"

class Memory;

class Z80Disassembler final : public CpuInstructionObserver
{
public:
    // One instruction decoded from bytes, with no memory behind them: the
    // text keeps the {n}/{o}/{e} markers the debugger colours.
    struct Decoded
    {
        int size;
        char name[96];
        bool jump;
        u16 jumpAddress;
        bool subroutine;
        // The instruction's 8- or 16-bit operand and what it is, for tools
        // that name addresses (the source listing). Jump covers JP, CALL,
        // JR, DJNZ and RST, whose value is jumpAddress.
        enum OperandKind : u8
        {
            Operand_None,
            Operand_Imm8,
            Operand_Port,       // IN A,(n) / OUT (n),A
            Operand_Imm16,      // LD rr,nn: a number or an address, unknown which
            Operand_Memory,     // (nn): an address read or written
            Operand_Jump
        };
        u8 operandKind;
        u16 operand;
        bool relative;          // JR / DJNZ
        bool rst;
    };

    explicit Z80Disassembler(Memory* memory);

    // Decodes the instruction at the start of bytes, which must hold its
    // DD/FD prefixes plus 5 more bytes (pad with zeros past the end of a ROM).
    static void Decode(const u8* bytes, int count, u16 address, Decoded& out);

    void Disassemble(u16 address);
    void OnCpuInstructionBoundary(u16 nextProgramCounter,
                                  u64 elapsedTStates) override;

private:
    Memory* m_memory;
};

#endif
