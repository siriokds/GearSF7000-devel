// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#ifndef SYSTEM_BUS_SG1000_H
#define SYSTEM_BUS_SG1000_H

#include "ISystemBus.h"

class Memory;
class MachineIOPorts;

// SG-1000 motherboard bus. With no SK-1100, $C0-$FF reaches the onboard
// joystick mux. The expansion changes that decode: A5 selects its 8255 or
// the joystick path.
class SystemBusSG1000 final : public ISystemBus
{
public:
    SystemBusSG1000(Memory& memory, MachineIOPorts& ports,
                    bool sk1100Attached = false);
    RamRegions GetRamRegions() const override;
    MachineType GetMachineType() const override { return MachineType::SG1000; }
    u32 GetCpuPsgClockHz() const override { return 3'579'545; }
    DebugDeviceDefinition DescribeDebugDevice() const override;
    u8 ReadMemory(u16 address) override;
    void WriteMemory(u16 address, u8 value) override;
    u8 ReadIO(u64 tstates, u16 address) override;
    void WriteIO(u64 tstates, u16 address, u8 value) override;
    unsigned int GetIOWaitStates(u64 tstates, u16 address,
                                 bool write) const override;
    unsigned int GetWaitCyclesM1() const override { return 0; }
    VdpLineType GetVdpLineType() const override { return VdpLineType::VDP_INT; }
    PsgPortDecode GetPsgPorts() const override { return {0xC0, 0x40}; }
    SystemInputDescriptor DescribeInput() const override
    {
        return m_sk1100Attached ? kSegaJoypadKeyboardInput : kSegaJoypadInput;
    }
    u8 AcknowledgeInterrupt(u64 tstates) override;
    DebugProbe GetIOProbe(u16 address) const override;

private:
    Memory& m_memory;
    MachineIOPorts& m_ports;
    bool m_sk1100Attached;
};

#endif
