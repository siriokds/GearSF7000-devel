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
    explicit Z80Disassembler(Memory* memory);

    void Disassemble(u16 address);
    void OnCpuInstructionBoundary(u16 nextProgramCounter,
                                  u64 elapsedTStates) override;

private:
    Memory* m_memory;
};

#endif
