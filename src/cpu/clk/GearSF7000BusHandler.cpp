/*
 * GearSF7000 - CLK Z80 bus adapter
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "GearSF7000BusHandler.h"

#include "IOPorts.h"
#include "ISystemBus.h"
#include "SaveStateStream.h"
#include "Memory.h"

namespace
{
u8 LowPort(const CPU::Z80::PartialMachineCycle& cycle)
{
    return cycle.address ? static_cast<u8>(*cycle.address & 0x00ff) : 0;
}
}

GearSF7000BusHandler::GearSF7000BusHandler(
    Memory* memory, IOPorts* ioPorts, GearSF7000BusClockSink* clockSink)
    : m_systemBus(nullptr),
      m_memory(memory),
      m_ioPorts(ioPorts),
      m_clockSink(clockSink),
      m_elapsedHalfCycles(0),
      m_pendingClockSinkHalfCycles(0)
{
}

void GearSF7000BusHandler::Attach(
    Memory* memory, IOPorts* ioPorts, GearSF7000BusClockSink* clockSink)
{
    m_systemBus = nullptr;
    m_memory = memory;
    m_ioPorts = ioPorts;
    m_clockSink = clockSink;
}

void GearSF7000BusHandler::SetIOPorts(IOPorts* ioPorts)
{
    m_systemBus = nullptr;
    m_ioPorts = ioPorts;
}

void GearSF7000BusHandler::SetSystemBus(ISystemBus* systemBus)
{
    m_systemBus = systemBus;
}

void GearSF7000BusHandler::SetClockSink(GearSF7000BusClockSink* clockSink)
{
    FlushClockSink();
    m_clockSink = clockSink;
}

void GearSF7000BusHandler::FlushClockSink()
{
    if (m_clockSink && m_pendingClockSinkHalfCycles != 0)
        m_clockSink->AdvanceZ80HalfCycles(m_pendingClockSinkHalfCycles);
    m_pendingClockSinkHalfCycles = 0;
}

void GearSF7000BusHandler::ResetElapsedTime()
{
	// Resetting the number displayed by the debugger must not delete machine
	// time that has already elapsed but has not yet reached the devices.
	FlushClockSink();
	m_elapsedHalfCycles = 0;
}

void GearSF7000BusHandler::DiscardElapsedTime()
{
    m_elapsedHalfCycles = 0;
    m_pendingClockSinkHalfCycles = 0;
}

std::uint64_t GearSF7000BusHandler::GetElapsedHalfCycles() const
{
    return m_elapsedHalfCycles;
}

void GearSF7000BusHandler::SaveState(std::ostream& stream) const
{
    StateWriter w(stream);
    w.U64(m_elapsedHalfCycles);
    w.U64(m_pendingClockSinkHalfCycles);
}

bool GearSF7000BusHandler::LoadState(std::istream& stream)
{
    StateReader r(stream);
    const std::uint64_t elapsed = r.U64();
    const std::uint64_t pending = r.U64();
    if (!r.Ok())
        return false;

    m_elapsedHalfCycles = elapsed;
    m_pendingClockSinkHalfCycles = pending;
    return true;
}

std::uint64_t GearSF7000BusHandler::GetElapsedTStates() const
{
    return m_elapsedHalfCycles >> 1;
}

HalfCycles GearSF7000BusHandler::GetM1WaitPenalty() const
{
    if (!m_systemBus)
        return HalfCycles(0);
    return HalfCycles(
        static_cast<HalfCycles::IntType>(m_systemBus->GetWaitCyclesM1()) * 2);
}

unsigned int GearSF7000BusHandler::GetHalfCycleRemainder() const
{
    return static_cast<unsigned int>(m_elapsedHalfCycles & 1u);
}

HalfCycles GearSF7000BusHandler::GetTerminalIOWaitPenalty(
    const CPU::Z80::PartialMachineCycle& cycle) const
{
    if ((!m_systemBus && !m_ioPorts) || !cycle.is_terminal() ||
        (cycle.operation != CPU::Z80::PartialMachineCycle::Input &&
         cycle.operation != CPU::Z80::PartialMachineCycle::Output))
        return HalfCycles(0);

    const bool write = cycle.operation == CPU::Z80::PartialMachineCycle::Output;
    const unsigned int waitTStates = m_systemBus
        ? m_systemBus->GetIOWaitStates(GetElapsedTStates(),
            cycle.address ? *cycle.address : 0, write)
        : m_ioPorts->GetWaitStatesAt(GetElapsedTStates(), LowPort(cycle), write);
    return HalfCycles(static_cast<HalfCycles::IntType>(waitTStates) * 2);
}

void GearSF7000BusHandler::Advance(HalfCycles duration)
{
    const std::uint64_t halfCycles = duration.as<std::uint64_t>();
    m_elapsedHalfCycles += halfCycles;
    m_pendingClockSinkHalfCycles += halfCycles;
    if (m_systemBus) m_systemBus->AdvanceCpuHalfCycles(halfCycles);
}

HalfCycles GearSF7000BusHandler::PerformMachineCycle(
    const CPU::Z80::PartialMachineCycle& cycle)
{
    // First reach the normal terminal point of the CPU I/O cycle. A device
    // that really drives READY must inspect its state at this exact instant;
    // determining a dynamic wait beforehand would use a stale timestamp.
    Advance(cycle.length);

    if (!cycle.is_terminal())
        return HalfCycles(0);

    // I/O and interrupt acknowledge can observe peripheral state. Bring the
    // machine to the exact transfer timestamp before committing those bus
    // actions. Ordinary memory cycles are batched until the instruction
    // boundary by CLKZ80Processor, avoiding a full peripheral fan-out for
    // every partial CPU cycle.
    if (cycle.operation == CPU::Z80::PartialMachineCycle::Input ||
        cycle.operation == CPU::Z80::PartialMachineCycle::Output ||
        cycle.operation == CPU::Z80::PartialMachineCycle::Interrupt)
    {
        FlushClockSink();
    }

    // Ask the motherboard for the device wait, then advance and synchronize
    // before transfer. The current TMS has no READY line and adds no wait.
    HalfCycles penalty = GetTerminalIOWaitPenalty(cycle);
    if (penalty != HalfCycles(0))
    {
        Advance(penalty);
        FlushClockSink();
    }
    else if (cycle.operation == CPU::Z80::PartialMachineCycle::ReadOpcode ||
             cycle.operation == CPU::Z80::PartialMachineCycle::Interrupt)
    {
        // The board's M1 wait circuit (ColecoVision, MSX). On the
        // ColecoVision schematic (Processor sheet, U8A 74LS74) it is clocked
        // from /M1 alone, so every cycle with /M1 active pays: opcode fetches,
        // prefix bytes, the NMI response (CLK issues it as an opcode fetch)
        // and the maskable interrupt acknowledge. An opcode fetch is an
        // ordinary memory cycle for the devices, so its time is only
        // accumulated here and reaches them at the instruction boundary.
        penalty = GetM1WaitPenalty();
        if (penalty != HalfCycles(0))
            Advance(penalty);
    }

    const u16 address = cycle.address ? *cycle.address : 0;
    switch (cycle.operation)
    {
        case CPU::Z80::PartialMachineCycle::ReadOpcode:
        case CPU::Z80::PartialMachineCycle::Read:
            if (cycle.value)
            {
                if (m_systemBus) *cycle.value = m_systemBus->ReadMemory(address);
                else if (m_memory) *cycle.value = m_memory->Read(address);
                if (m_systemBus && cycle.operation == CPU::Z80::PartialMachineCycle::ReadOpcode)
                    m_systemBus->ObserveOpcodeFetch(*cycle.value);
                // Only here are an opcode fetch (M1) and a data read still
                // told apart; the ROM map wants the data reads.
                if (m_memory && cycle.operation == CPU::Z80::PartialMachineCycle::Read)
                    m_memory->NotifyCpuDataRead(address, *cycle.value);
            }
            break;

        case CPU::Z80::PartialMachineCycle::Write:
            if (cycle.value)
            {
                if (m_systemBus) m_systemBus->WriteMemory(address, *cycle.value);
                else if (m_memory) m_memory->Write(address, *cycle.value);
            }
            break;

        case CPU::Z80::PartialMachineCycle::Input:
            if ((m_systemBus || m_ioPorts) && cycle.value)
            {
                const u8 port = LowPort(cycle);
                *cycle.value = m_systemBus
                    ? m_systemBus->ReadIO(GetElapsedTStates(), address)
                    : m_ioPorts->In(GetElapsedTStates(), port);
                if (m_memory && (!m_systemBus || !m_systemBus->PublishesIOEvents()))
                    m_memory->CheckIOPortAccess(m_systemBus
                        ? m_systemBus->GetIOProbe(address) : m_ioPorts->GetDebugProbe(port),
                        port, DebugEventAccess_Read, *cycle.value);
            }
            break;

        case CPU::Z80::PartialMachineCycle::Output:
            if ((m_systemBus || m_ioPorts) && cycle.value)
            {
                const u8 port = LowPort(cycle);
                // Before the write: a VDP may commit it to VRAM inside WriteIO.
                if (m_memory)
                    m_memory->NotifyCpuPortWrite(port, *cycle.value);
                if (m_systemBus) m_systemBus->WriteIO(GetElapsedTStates(), address, *cycle.value);
                else m_ioPorts->Out(GetElapsedTStates(), port, *cycle.value);
                if (m_memory && (!m_systemBus || !m_systemBus->PublishesIOEvents()))
                    m_memory->CheckIOPortAccess(m_systemBus
                        ? m_systemBus->GetIOProbe(address) : m_ioPorts->GetDebugProbe(port),
                        port, DebugEventAccess_Write, *cycle.value);
            }
            break;

        case CPU::Z80::PartialMachineCycle::Interrupt:
            // IM 1 ignores this byte on SC-3000, while IM 0 decodes it as an
            // opcode and IM 2 uses it as the low vector byte. Keeping the
            // source on IOPorts gives both CPU backends the same bus-level
            // acknowledgment protocol; its default is the idle-bus $ff.
            if ((m_systemBus || m_ioPorts) && cycle.value)
                *cycle.value = m_systemBus
                    ? m_systemBus->AcknowledgeInterrupt(GetElapsedTStates())
                    : m_ioPorts->AcknowledgeInterrupt(GetElapsedTStates());
            break;

        case CPU::Z80::PartialMachineCycle::Refresh:
        case CPU::Z80::PartialMachineCycle::Internal:
        case CPU::Z80::PartialMachineCycle::BusAcknowledge:
        default:
            break;
    }

    return penalty;
}
