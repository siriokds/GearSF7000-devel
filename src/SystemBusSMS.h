// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SYSTEM_BUS_SMS_H
#define SYSTEM_BUS_SMS_H

#include "ISystemBus.h"

class Memory;
class MachineIOPorts;

class SystemBusSMS final : public ISystemBus
{
public:
    SystemBusSMS(Memory& memory, MachineIOPorts& ports, bool pal);
    RamRegions GetRamRegions() const override;
    MachineType GetMachineType() const override { return MachineType::SMS; }
    u32 GetCpuPsgClockHz() const override
    {
        return m_pal ? 3'546'893u : 3'579'545u;
    }
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
    SystemInputDescriptor DescribeInput() const override { return kSegaJoypadInput; }
    u8 AcknowledgeInterrupt(u64 tstates) override;
    DebugProbe GetIOProbe(u16 address) const override;

private:
    Memory& m_memory;
    MachineIOPorts& m_ports;
    bool m_pal;
};

#endif
