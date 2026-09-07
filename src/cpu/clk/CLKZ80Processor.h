/*
 * GearSF7000 - CLK Z80 processor facade
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_CLK_Z80_PROCESSOR_H
#define GEARSF7000_CLK_Z80_PROCESSOR_H

#include "cpu/CpuExecution.h"
#include "cpu/CpuStatePersistence.h"
#include "cpu/clk/GearSF7000BusHandler.h"

class CLKZ80Processor final : public CpuExecution,
                             public CpuStatePersistence
{
public:
    using Core = CPU::Z80::Processor<GearSF7000BusHandler, false, false>;

    CLKZ80Processor(Memory* memory = nullptr, IOPorts* ioPorts = nullptr,
                    GearSF7000BusClockSink* clockSink = nullptr);

    void AttachBus(Memory* memory, IOPorts* ioPorts,
                   GearSF7000BusClockSink* clockSink = nullptr);

    // Runs for objective machine time. Deterministic device waits consume part
    // of this budget and are included in the returned elapsed T-state count.
    u64 RunForTStates(u64 tStates);
    u64 RunForInstruction();

    void RequestPowerOnReset();
    void SetResetLine(bool asserted);
    void SetInterruptLine(bool asserted, HalfCycles offset = HalfCycles(0));
    void SetNonMaskableInterruptLine(bool asserted,
                                     HalfCycles offset = HalfCycles(0));
    void SetMaskableInterruptLine(bool asserted) override;
    void PulseNonMaskableInterrupt() override;
    CpuStateSnapshot GetCpuStateSnapshot() const override;
    bool SetCpuRegister(CpuRegister reg, std::uint16_t value) override;
    std::uint64_t GetElapsedTStates() const override;
    void SetElapsedTStates(std::uint64_t value) override;
    void InitializeExecution() override;
    void AttachIOPorts(IOPorts* ioPorts) override;
    void AttachInstructionObserver(CpuInstructionObserver* observer) override;
    std::uint64_t ExecuteForTStates(std::uint32_t budget) override;
    std::uint64_t ExecuteInstruction() override;
    void ResetExecution() override;
    bool HasBreakpointHit() const override;
    bool IsDuringInputOperation() const override;

    u16 GetRegister(CPU::Z80::Register reg) const;
    void SetRegister(CPU::Z80::Register reg, u16 value);
    bool Halted() const;
    bool IsStartingNewInstruction() const;

    // CpuStatePersistence. Captures the CLK core including its micro-op
    // scheduler, plus the bus adapter's elapsed and not-yet-delivered machine
    // time, so a snapshot taken part-way through an instruction resumes
    // without divergence.
    bool SaveCpuState(std::ostream& stream) override;
    bool LoadCpuState(std::istream& stream) override;

    CpuDebugSnapshot GetCpuDebugSnapshot() const override;
    void RequestCpuDebugPause() override;
    bool IsDebugPauseRequested() const;
    bool ConsumeDebugPauseRequest();

    GearSF7000BusHandler& GetBusHandler();
    const GearSF7000BusHandler& GetBusHandler() const;
    Core& GetCore();
    const Core& GetCore() const;

private:
    u64 RunToNextInstructionBoundary();
    void NotifyInstructionBoundary();

    GearSF7000BusHandler m_bus;
    Core m_cpu;
    CpuInstructionObserver* m_instructionObserver;
    bool m_debugPauseRequested;
    bool m_powerOnResetPending;
};

#endif
