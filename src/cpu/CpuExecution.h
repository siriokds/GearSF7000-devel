/*
 * GearSF7000 - CPU-independent execution contract
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_CPU_EXECUTION_H
#define GEARSF7000_CPU_EXECUTION_H

#include <cstdint>

#include "cpu/CpuDebugContext.h"
#include "cpu/CpuInterruptLines.h"
#include "cpu/CpuStateAccess.h"

class IOPorts;
class CpuInstructionObserver;

// Minimal surface required by the machine loop. Disassembly, save-state
// encoding and legacy breakpoint storage intentionally remain separate.
class CpuExecution : public CpuDebugContext,
                     public CpuInterruptLines,
                     public CpuStateAccess
{
public:
    virtual ~CpuExecution() = default;

    virtual void InitializeExecution() = 0;
    virtual void AttachIOPorts(IOPorts* ioPorts) = 0;
    virtual void AttachInstructionObserver(CpuInstructionObserver* observer) = 0;
    virtual std::uint64_t ExecuteForTStates(std::uint32_t budget) = 0;
    // Executes through the next complete instruction boundary. This is kept
    // separate from a time budget because a cycle-accurate core may legally
    // stop in the middle of an instruction when its budget expires.
    virtual std::uint64_t ExecuteInstruction() = 0;
    virtual void ResetExecution() = 0;
    virtual bool HasBreakpointHit() const = 0;
    virtual bool IsDuringInputOperation() const = 0;
};

#endif
