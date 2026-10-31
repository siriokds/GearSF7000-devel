// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#ifndef SYSTEM_BUS_COLECO_H
#define SYSTEM_BUS_COLECO_H

#include "ISystemBus.h"

class Memory;
class MachineIOPorts;

// ColecoVision motherboard (DOCS/manuals/Colecovision-Console-Schematic.pdf).
// One wait state on every /M1 cycle from the 74LS74 on the Processor sheet,
// the TMS9928A /INT wired to the Z80 /NMI, the controllers' keypad read
// through the same port as the joystick. /INT is left to the controller
// quadrature (spinner), which the standard controller does not have.
class SystemBusColeco final : public ISystemBus
{
public:
    SystemBusColeco(Memory& memory, MachineIOPorts& ports);
    RamRegions GetRamRegions() const override;
    MachineType GetMachineType() const override { return MachineType::Coleco; }
    u32 GetCpuPsgClockHz() const override { return 3'579'545; }
    DebugDeviceDefinition DescribeDebugDevice() const override;
    u8 ReadMemory(u16 address) override;
    void WriteMemory(u16 address, u8 value) override;
    u8 ReadIO(u64 tstates, u16 address) override;
    void WriteIO(u64 tstates, u16 address, u8 value) override;
    unsigned int GetIOWaitStates(u64 tstates, u16 address,
                                 bool write) const override;
    unsigned int GetWaitCyclesM1() const override { return 1; }
    u32 GetBiosSize() const override { return 0x2000; }
    VdpLineType GetVdpLineType() const override { return VdpLineType::VDP_NMI; }
    // SN76489AN on the $E0 block, writes only (schematic, Sound sheet).
    PsgPortDecode GetPsgPorts() const override { return {0xE0, 0xE0}; }
    std::size_t GetFirmwareSoundEntries(const u16** entries) const override;
    FirmwareRange GetFirmwareRange() const override { return {0x0000, 0x2000}; }
    SystemInputDescriptor DescribeInput() const override
    {
        return {2, true, false, "Left button", "Right button"};
    }
    u8 AcknowledgeInterrupt(u64 tstates) override;
    DebugProbe GetIOProbe(u16 address) const override;

private:
    Memory& m_memory;
    MachineIOPorts& m_ports;
};

#endif
