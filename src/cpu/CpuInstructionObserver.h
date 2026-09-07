/*
 * GearSF7000 - CPU instruction-boundary observer
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_CPU_INSTRUCTION_OBSERVER_H
#define GEARSF7000_CPU_INSTRUCTION_OBSERVER_H

#include <array>
#include <cstddef>

#include "definitions.h"

// Receives the PC that will be executed next, after the previous instruction
// (or interrupt entry sequence) has completed.
class CpuInstructionObserver
{
public:
    virtual ~CpuInstructionObserver() = default;
    virtual void OnCpuInstructionBoundary(u16 nextProgramCounter,
                                          u64 elapsedTStates) = 0;
};

// Small allocation-free fan-out used by a machine to combine independent
// debugger services (event rules, disassembly, profiling, etc.).
class CpuInstructionObserverList final : public CpuInstructionObserver
{
public:
    static constexpr std::size_t Capacity = 4;

    bool Add(CpuInstructionObserver* observer)
    {
        if (!observer || m_count >= Capacity)
            return false;
        m_observers[m_count++] = observer;
        return true;
    }

    void OnCpuInstructionBoundary(u16 nextProgramCounter,
                                  u64 elapsedTStates) override
    {
        for (std::size_t i = 0; i < m_count; ++i)
            m_observers[i]->OnCpuInstructionBoundary(nextProgramCounter,
                                                      elapsedTStates);
    }

private:
    std::array<CpuInstructionObserver*, Capacity> m_observers{};
    std::size_t m_count = 0;
};

#endif
