/*
 * GearSF7000 - CLK Z80 processor facade
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "CLKZ80Processor.h"
#include "cpu/CpuInstructionObserver.h"
#include "cpu/clk/CLKZ80StateAccess.h"

#include "SaveStateStream.h"

CLKZ80Processor::CLKZ80Processor(
    Memory* memory, IOPorts* ioPorts, GearSF7000BusClockSink* clockSink)
    : m_bus(memory, ioPorts, clockSink),
      m_cpu(m_bus),
      m_instructionObserver(nullptr),
      m_debugPauseRequested(false),
      m_powerOnResetPending(true)
{
}

void CLKZ80Processor::AttachBus(
    Memory* memory, IOPorts* ioPorts, GearSF7000BusClockSink* clockSink)
{
    m_bus.Attach(memory, ioPorts, clockSink);
}

u64 CLKZ80Processor::RunForTStates(u64 tStates)
{
    m_debugPauseRequested = false;
    const u64 before = m_bus.GetElapsedHalfCycles();

    while (((m_bus.GetElapsedHalfCycles() - before) >> 1) < tStates)
    {
        RunToNextInstructionBoundary();
        NotifyInstructionBoundary();
        if (m_debugPauseRequested)
            break;
    }
    return (m_bus.GetElapsedHalfCycles() - before) >> 1;
}

u64 CLKZ80Processor::RunForInstruction()
{
    m_debugPauseRequested = false;
    const u64 elapsed = RunToNextInstructionBoundary();
    NotifyInstructionBoundary();
    return elapsed;
}

u64 CLKZ80Processor::RunToNextInstructionBoundary()
{
    const u64 before = m_bus.GetElapsedHalfCycles();

    // CLK exposes the power-on reset request as an execution boundary.  For
    // debugging, completing reset and arriving at the first opcode fetch are
    // a distinct quantum: this makes $0000 observable before its instruction
    // executes. Normal continuous execution immediately continues unless an
    // observer requests a pause at that boundary.
    if (m_powerOnResetPending)
    {
        bool resetRequestConsumed = false;
        do
        {
            m_cpu.run_for(Cycles(1));
            resetRequestConsumed = !m_cpu.get_is_resetting();
        }
        while (!resetRequestConsumed || !m_cpu.is_starting_new_instruction());

        m_powerOnResetPending = false;
        m_bus.FlushClockSink();
        return (m_bus.GetElapsedHalfCycles() - before) >> 1;
    }

    int boundaryTransitions = 0;

    // This is the sequence used by CLK's own AllRAM Z80 adapter: reach an
    // instruction boundary, leave it, then reach the following boundary.
    while (boundaryTransitions < 3)
    {
        if (m_cpu.is_at_execution_boundary() !=
            static_cast<bool>(boundaryTransitions & 1))
        {
            ++boundaryTransitions;
            if (boundaryTransitions == 3)
                break;
        }
        m_cpu.run_for(Cycles(1));
    }

    // Non-I/O cycles may be safely accumulated, but all devices must reach
    // the public instruction boundary before control returns to the machine.
    m_bus.FlushClockSink();

    return (m_bus.GetElapsedHalfCycles() - before) >> 1;
}

void CLKZ80Processor::NotifyInstructionBoundary()
{
    if (m_instructionObserver)
    {
        m_instructionObserver->OnCpuInstructionBoundary(
            m_cpu.value_of(CPU::Z80::Register::ProgramCounter),
            m_bus.GetElapsedTStates());
    }
}

void CLKZ80Processor::RequestPowerOnReset()
{
    m_cpu.set_power_on_reset();
    m_powerOnResetPending = true;
}

void CLKZ80Processor::SetResetLine(bool asserted)
{
    m_cpu.set_reset_line(asserted);
}

void CLKZ80Processor::SetInterruptLine(bool asserted, HalfCycles offset)
{
    m_cpu.set_interrupt_line(asserted, offset);
}

void CLKZ80Processor::SetMaskableInterruptLine(bool asserted)
{
    SetInterruptLine(asserted);
}

void CLKZ80Processor::SetNonMaskableInterruptLine(
    bool asserted, HalfCycles offset)
{
    m_cpu.set_non_maskable_interrupt_line(asserted, offset);
}

void CLKZ80Processor::PulseNonMaskableInterrupt()
{
    // NMI is edge-triggered. Deassert first so repeated UI reset presses each
    // generate a new edge, then release the line while the request stays
    // latched inside the CPU.
    m_cpu.set_non_maskable_interrupt_line(false);
    m_cpu.set_non_maskable_interrupt_line(true);
    m_cpu.set_non_maskable_interrupt_line(false);
}

CpuStateSnapshot CLKZ80Processor::GetCpuStateSnapshot() const
{
    using Register = CPU::Z80::Register;
    CpuStateSnapshot state;
    state.af = m_cpu.value_of(Register::AF);
    state.bc = m_cpu.value_of(Register::BC);
    state.de = m_cpu.value_of(Register::DE);
    state.hl = m_cpu.value_of(Register::HL);
    state.af2 = m_cpu.value_of(Register::AFDash);
    state.bc2 = m_cpu.value_of(Register::BCDash);
    state.de2 = m_cpu.value_of(Register::DEDash);
    state.hl2 = m_cpu.value_of(Register::HLDash);
    state.ix = m_cpu.value_of(Register::IX);
    state.iy = m_cpu.value_of(Register::IY);
    state.sp = m_cpu.value_of(Register::StackPointer);
    state.pc = m_cpu.value_of(Register::ProgramCounter);
    state.wz = m_cpu.value_of(Register::MemPtr);
    state.i = static_cast<u8>(m_cpu.value_of(Register::I));
    state.r = static_cast<u8>(m_cpu.value_of(Register::R));
    state.iff1 = m_cpu.value_of(Register::IFF1) != 0;
    state.iff2 = m_cpu.value_of(Register::IFF2) != 0;
    state.halted = m_cpu.get_halt_line();
    state.intLine = m_cpu.get_interrupt_line();
    state.nmiLine = m_cpu.get_non_maskable_interrupt_line();
    state.interruptMode = static_cast<int>(m_cpu.value_of(Register::IM));
    return state;
}

bool CLKZ80Processor::SetCpuRegister(CpuRegister reg, std::uint16_t value)
{
    using Register = CPU::Z80::Register;
    Register target;
    switch (reg)
    {
        case CpuRegister::AF: target = Register::AF; break;
        case CpuRegister::BC: target = Register::BC; break;
        case CpuRegister::DE: target = Register::DE; break;
        case CpuRegister::HL: target = Register::HL; break;
        case CpuRegister::AF2: target = Register::AFDash; break;
        case CpuRegister::BC2: target = Register::BCDash; break;
        case CpuRegister::DE2: target = Register::DEDash; break;
        case CpuRegister::HL2: target = Register::HLDash; break;
        case CpuRegister::IX: target = Register::IX; break;
        case CpuRegister::IY: target = Register::IY; break;
        case CpuRegister::SP: target = Register::StackPointer; break;
        case CpuRegister::PC: target = Register::ProgramCounter; break;
        case CpuRegister::WZ: target = Register::MemPtr; break;
        case CpuRegister::I: target = Register::I; break;
        case CpuRegister::R: target = Register::R; break;
        case CpuRegister::IFF1: target = Register::IFF1; break;
        case CpuRegister::IFF2: target = Register::IFF2; break;
        case CpuRegister::InterruptMode:
            if (value > 2)
                return false;
            target = Register::IM;
            break;
        default: return false;
    }
    m_cpu.set_value_of(target, value);
    return true;
}

std::uint64_t CLKZ80Processor::GetElapsedTStates() const
{
    return m_bus.GetElapsedTStates();
}

void CLKZ80Processor::SetElapsedTStates(std::uint64_t value)
{
    // The bus timestamp is monotonic machine time and cannot safely be
    // rewritten while devices are attached. Only zero is currently supported
    // as the debugger's relative-clock reset operation.
    if (value == 0)
        m_bus.ResetElapsedTime();
}

void CLKZ80Processor::InitializeExecution()
{
    ResetExecution();
}

void CLKZ80Processor::AttachIOPorts(IOPorts* ioPorts)
{
    m_bus.SetIOPorts(ioPorts);
}

void CLKZ80Processor::AttachInstructionObserver(
    CpuInstructionObserver* observer)
{
    m_instructionObserver = observer;
}

std::uint64_t CLKZ80Processor::ExecuteForTStates(std::uint32_t budget)
{
    return RunForTStates(budget);
}

std::uint64_t CLKZ80Processor::ExecuteInstruction()
{
    return RunForInstruction();
}

void CLKZ80Processor::ResetExecution()
{
    m_debugPauseRequested = false;
    // A machine reset initializes every device separately. Do not deliver a
    // stale partial CPU interval into the freshly reset machine.
    m_bus.DiscardElapsedTime();
    RequestPowerOnReset();
}

bool CLKZ80Processor::HasBreakpointHit() const
{
    return m_debugPauseRequested;
}

bool CLKZ80Processor::IsDuringInputOperation() const
{
    // CLK exposes the I/O cycle itself through the bus handler. Once a call to
    // ExecuteForTStates returns, there is no legacy late-input phase to mask.
    return false;
}

u16 CLKZ80Processor::GetRegister(CPU::Z80::Register reg) const
{
    return m_cpu.value_of(reg);
}

void CLKZ80Processor::SetRegister(CPU::Z80::Register reg, u16 value)
{
    m_cpu.set_value_of(reg, value);
}

bool CLKZ80Processor::Halted() const
{
    return m_cpu.get_halt_line();
}

bool CLKZ80Processor::IsStartingNewInstruction() const
{
    return m_cpu.is_starting_new_instruction();
}

CpuDebugSnapshot CLKZ80Processor::GetCpuDebugSnapshot() const
{
    return {
        m_cpu.value_of(CPU::Z80::Register::ProgramCounter),
        m_bus.GetElapsedTStates()
    };
}

void CLKZ80Processor::RequestCpuDebugPause()
{
    m_debugPauseRequested = true;
}

bool CLKZ80Processor::IsDebugPauseRequested() const
{
    return m_debugPauseRequested;
}

bool CLKZ80Processor::ConsumeDebugPauseRequest()
{
    const bool requested = m_debugPauseRequested;
    m_debugPauseRequested = false;
    return requested;
}

GearSF7000BusHandler& CLKZ80Processor::GetBusHandler()
{
    return m_bus;
}

const GearSF7000BusHandler& CLKZ80Processor::GetBusHandler() const
{
    return m_bus;
}

CLKZ80Processor::Core& CLKZ80Processor::GetCore()
{
    return m_cpu;
}

const CLKZ80Processor::Core& CLKZ80Processor::GetCore() const
{
    return m_cpu;
}

bool CLKZ80Processor::SaveCpuState(std::ostream& stream)
{
    if (!CPU::Z80::GearSF7000StateAccess::Save(m_cpu, stream))
        return false;

    m_bus.SaveState(stream);

    StateWriter w(stream);
    w.Bool(m_debugPauseRequested);
    w.Bool(m_powerOnResetPending);
    return w.Ok();
}

bool CLKZ80Processor::LoadCpuState(std::istream& stream)
{
    if (!CPU::Z80::GearSF7000StateAccess::Load(m_cpu, stream))
        return false;

    if (!m_bus.LoadState(stream))
        return false;

    StateReader r(stream);
    const bool debugPauseRequested = r.Bool();
    const bool powerOnResetPending = r.Bool();
    if (!r.Ok())
        return false;

    m_debugPauseRequested = debugPauseRequested;
    m_powerOnResetPending = powerOnResetPending;
    return true;
}
