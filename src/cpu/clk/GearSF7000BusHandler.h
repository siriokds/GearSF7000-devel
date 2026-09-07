/*
 * GearSF7000 - CLK Z80 bus adapter
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_CLK_BUS_HANDLER_H
#define GEARSF7000_CLK_BUS_HANDLER_H

#include <cstdint>
#include <iosfwd>

#include "Processors/Z80/Z80.hpp"

class IOPorts;
class Memory;

// Receives objective elapsed time from the CLK Z80.  A future machine clock
// implementation will fan this out to the VDP, PSG, tape, FDC and other
// devices without rounding every partial machine cycle to a whole T-state.
class GearSF7000BusClockSink
{
public:
    virtual ~GearSF7000BusClockSink() = default;
    virtual void AdvanceZ80HalfCycles(std::uint64_t halfCycles) = 0;
};

// Connects CLK's partial Z80 machine cycles to the existing GearSF7000 memory
// and I/O decoders.  It is compiled but not selected as the active CPU yet.
// This makes the integration independently testable without changing current
// emulator behaviour.
class GearSF7000BusHandler final : public CPU::Z80::BusHandler
{
public:
    GearSF7000BusHandler(Memory* memory = nullptr, IOPorts* ioPorts = nullptr,
                         GearSF7000BusClockSink* clockSink = nullptr);

    void Attach(Memory* memory, IOPorts* ioPorts,
                GearSF7000BusClockSink* clockSink = nullptr);
    void SetIOPorts(IOPorts* ioPorts);
    void SetClockSink(GearSF7000BusClockSink* clockSink);
    void FlushClockSink();
    // Debugger-relative clock reset: first delivers pending physical time.
    void ResetElapsedTime();
    // Machine/power reset: devices are reset separately, so stale time is
    // deliberately discarded rather than delivered into their new state.
    void DiscardElapsedTime();

    std::uint64_t GetElapsedHalfCycles() const;
    std::uint64_t GetElapsedTStates() const;
    unsigned int GetHalfCycleRemainder() const;

    // Saved as-is, without flushing: the amount of machine time that has
    // elapsed but not yet reached the devices is part of the instant being
    // captured, and flushing it here would advance the devices past it.
    void SaveState(std::ostream& stream) const;
    bool LoadState(std::istream& stream);

    // Kept as a thin inline entry point because CLK calls this for every
    // partial machine cycle.
    forceinline HalfCycles perform_machine_cycle(
        const CPU::Z80::PartialMachineCycle& cycle)
    {
        return PerformMachineCycle(cycle);
    }

private:
    HalfCycles PerformMachineCycle(
        const CPU::Z80::PartialMachineCycle& cycle);
    HalfCycles GetTerminalIOWaitPenalty(
        const CPU::Z80::PartialMachineCycle& cycle) const;
    void Advance(HalfCycles duration);

private:
    Memory* m_memory;
    IOPorts* m_ioPorts;
    GearSF7000BusClockSink* m_clockSink;
    std::uint64_t m_elapsedHalfCycles;
    std::uint64_t m_pendingClockSinkHalfCycles;
};

#endif
