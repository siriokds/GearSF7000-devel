// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#ifndef SYSTEM_BUS_M5_H
#define SYSTEM_BUS_M5_H
#include "ISystemBus.h"
#include "M5Input.h"
#include "M5MemoryMapper.h"
#include "Z80CTC.h"
#include "cpu/CpuInterruptLines.h"
#include <functional>

class Memory;
class Audio;
class IVideoPortDevice;

// Base motherboard, standard cartridge and optional EM-5. Owns CTC/input/latches, not ROM,
// RAM, VDP or audio. Desktop and headless tests use the same bus.
// PSG wait is deliberately explicit until the board's READY phase is verified.
class SystemBusM5 final : public ISystemBus, public CpuInterruptLines, private IZ80CTCPins
{
public:
    // Provisional fixed stall, not a measured residual READY duration.
    static constexpr unsigned ProvisionalPsgWaitTStates = 32;
    explicit SystemBusM5(unsigned psgWriteWaitTStates, DebugEventManager* events = nullptr);
    SystemBusM5(const SystemBusM5&) = delete;
    SystemBusM5& operator=(const SystemBusM5&) = delete;
    bool BindMemory(std::span<const u8> bios, std::span<const u8> cartridge,
                    std::span<u8> ram, std::span<u8> em5Ram = {});
    void SetEM5Installed(bool installed) { m_em5Installed = installed; }
    bool EM5Installed() const { return m_em5Installed; }
    void AttachMemoryDebug(Memory* memory) { m_memoryDebug = memory; }
    void AttachVideo(IVideoPortDevice* video) { m_video = video; }
    void AttachAudio(Audio* audio) { m_audio = audio; }
    void AttachCpu(CpuInterruptLines* cpu);
    void Reset(); // Does not erase backing memory or reset external chips.
    void ReconcileInterrupt(); // Explicit live restore step, never validation.
    u64 CpuClock() const { return m_halfCycles / 2; }
    u64 CpuHalfCycles() const { return m_halfCycles; }
    const Z80CTC& CTC() const { return m_ctc; }
    M5Input& Input() { return m_input; }
    const M5Input& Input() const { return m_input; }
    M5MemoryMapper& Mapper() { return m_mapper; }
    const M5MemoryMapper& Mapper() const { return m_mapper; }
    u8 Command() const { return m_command; }
    u8 PrinterData() const { return m_printerData; }
    bool TapeMotor() const { return m_command & 2; }
    bool TapeOutputNegative() const { return m_command & 1; }
    void SetTapeInput(bool high) { m_tapeInput = high; }
    void SetPrinterBusy(bool busy) { m_printerBusy = busy; }

    MachineType GetMachineType() const override { return MachineType::SordM5; }
    u32 GetCpuPsgClockHz() const override { return 3'579'545; }
    u32 GetBiosSize() const override { return M5MemoryMapper::BiosSize; }
    RamRegions GetRamRegions() const override;
    FirmwareRange GetFirmwareRange() const override { return {0, 0x2000}; }
    unsigned GetWaitCyclesM1() const override { return 0; }
    unsigned GetIOWaitStates(u64, u16 address, bool write) const override;
    VdpLineType GetVdpLineType() const override { return VdpLineType::VDP_DEVICE; }
    PsgPortDecode GetPsgPorts() const override { return {0xF0, 0x20}; }
    SystemInputDescriptor DescribeInput() const override { return {2, false, true, "Fire 1", "Fire 2"}; }
    u8 ReadMemory(u16 address) override;
    void WriteMemory(u16 address, u8 value) override;
    u8 ReadIO(u64, u16 address) override;
    void WriteIO(u64, u16 address, u8 value) override;
    u8 AcknowledgeInterrupt(u64) override;
    void AdvanceCpuHalfCycles(u64 halfCycles) override;
    void ObserveOpcodeFetch(u8 value) override;
    bool PublishesIOEvents() const override { return true; }
    // Convert the VDP's logical assertion to its active-low /INT pin at CK3.
    void SetMaskableInterruptLine(bool asserted) override;
    void PulseNonMaskableInterrupt() override {} // No NMI source on this board.

    DebugDeviceDefinition DescribeDebugDevice() const override;
    DebugDeviceDefinition DescribeMapper() const;
    DebugProbe GetIOProbe(u16 address) const override;
    // Registry must contain event.m5.{bus,mapper,ctc,input,psg}. Atomic binding.
    bool BindDebugEvents(const DebugDeviceRegistry& registry);
    void SetDebugContext(std::function<u16()> pc, std::function<void()> pause = {})
    { m_pc = std::move(pc); m_pause = std::move(pause); }
    bool TakeDebugPauseRequest() { const bool p = m_debugPause; m_debugPause = false; return p; }
    void SaveState(std::ostream& stream) const;
    bool LoadState(std::istream& stream); // local v1/v2, silent, transactional
private:
    void SetInterrupt(bool asserted, u64) override;
    void SetZeroCount(unsigned, bool, u64) override {} // EXCLK socket unused.
    void OnCTCEvent(const Z80CTCEvent& event) override;
    void Publish(DebugDeviceId device, DebugSpaceId space, u32 target, u8 access,
                 u64 before, bool beforeKnown, u64 after, u64 time, DebugEventSource source);
    Memory* m_memoryDebug = nullptr;
    M5MemoryMapper m_mapper;
    std::span<u8> m_ram;
    std::span<u8> m_em5Ram;
    bool m_em5Installed = false;
    Z80CTC m_ctc;
    M5Input m_input;
    IVideoPortDevice* m_video = nullptr;
    Audio* m_audio = nullptr;
    CpuInterruptLines* m_cpu = nullptr;
    DebugEventManager* m_events;
    std::function<u16()> m_pc;
    std::function<void()> m_pause;
    DebugDeviceId m_busId = 0, m_mapperId = 0, m_ctcId = 0, m_inputId = 0, m_psgId = 0, m_vdpId = 0;
    u64 m_halfCycles = 0;
    unsigned m_psgWait;
    u8 m_command = 0, m_printerData = 0;
    bool m_tapeInput = true, m_printerBusy = false, m_edPrefix = false, m_debugPause = false;
};
#endif
