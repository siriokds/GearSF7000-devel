// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#ifndef SYSTEM_BUS_SC3000_H
#define SYSTEM_BUS_SC3000_H
#include "ISystemBus.h"
class Memory;
class IOPorts;

// First migration stage: routes to existing endpoints, owned by Core.
// In particular it preserves current PPI aliases, mapper policy and waits.
class SystemBusSC3000 final : public ISystemBus
{
public:
    static constexpr u32 IplSize = 0x2000;
    SystemBusSC3000(Memory& memory, IOPorts& ports,
                    u32 cpuPsgClockHz = 3'580'000, bool sf7000Attached = false);
    u32 GetBiosSize() const override { return m_sf7000Attached ? IplSize : 0; }
    RamRegions GetRamRegions() const override;
    MachineType GetMachineType() const override { return MachineType::SC3000; }
    u32 GetCpuPsgClockHz() const override { return m_cpuPsgClockHz; }
    DebugDeviceDefinition DescribeDebugDevice() const override;
    u8 ReadMemory(u16 address) override;
    void WriteMemory(u16 address, u8 value) override;
    u8 ReadIO(u64 tstates, u16 address) override;
    void WriteIO(u64 tstates, u16 address, u8 value) override;
    unsigned int GetIOWaitStates(u64 tstates, u16 address, bool write) const override;
    unsigned int GetWaitCyclesM1() const override { return 0; }
    VdpLineType GetVdpLineType() const override { return VdpLineType::VDP_INT; }
    PsgPortDecode GetPsgPorts() const override { return {0xC0, 0x40}; }
    SystemInputDescriptor DescribeInput() const override { return kSegaJoypadKeyboardInput; }
    u8 AcknowledgeInterrupt(u64 tstates) override;
    DebugProbe GetIOProbe(u16 address) const override;
private:
    Memory& m_memory;
    IOPorts& m_ports;
    u32 m_cpuPsgClockHz;
    bool m_sf7000Attached;
};
#endif
