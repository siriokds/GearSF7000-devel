// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#ifndef SYSTEM_BUS_GG_H
#define SYSTEM_BUS_GG_H

#include "GameGearInternalBusHandler.h"
#include "ISystemBus.h"

class Audio;
class MachineIOPorts;
class Memory;
class SegaKeyboard;

// CPU-facing Game Gear motherboard. It owns the fixed internal decoder;
// Memory still owns the existing SMS-compatible cartridge mapping.
class SystemBusGG final : public ISystemBus
{
public:
    SystemBusGG(Memory& memory, MachineIOPorts& ports, SegaKeyboard& keyboard,
                Audio& audio, bool exportRegion);

    MachineType GetMachineType() const override { return MachineType::GameGear; }
    u32 GetCpuPsgClockHz() const override { return 3'579'545u; }
    RamRegions GetRamRegions() const override;
    DebugDeviceDefinition DescribeDebugDevice() const override;
    u8 ReadMemory(u16 address) override;
    void WriteMemory(u16 address, u8 value) override;
    u8 ReadIO(u64 tstates, u16 address) override;
    void WriteIO(u64 tstates, u16 address, u8 value) override;
    unsigned int GetIOWaitStates(u64 tstates, u16 address,
                                 bool write) const override;
    unsigned int GetWaitCyclesM1() const override { return 0; }
    VdpLineType GetVdpLineType() const override { return VdpLineType::VDP_INT; }
    SystemInputDescriptor DescribeInput() const override { return kGameGearInput; }
    PsgPortDecode GetPsgPorts() const override { return {0xC0, 0x40}; }
    u8 AcknowledgeInterrupt(u64 tstates) override;
    DebugProbe GetIOProbe(u16 address) const override;

    void Reset() { m_internal.Reset(); }
    void SetStartPressed(bool pressed) { m_internal.SetStartPressed(pressed); }
    bool IsStartPressed() const { return m_internal.IsStartPressed(); }
    void SetExportRegion(bool exportRegion) { m_internal.SetExportRegion(exportRegion); }
    void SaveState(std::ostream& stream) const { m_internal.SaveState(stream); }
    bool LoadState(std::istream& stream) { return m_internal.LoadState(stream); }

private:
    Memory& m_memory;
    MachineIOPorts& m_ports;
    GameGearInternalBusHandler m_internal;
};

#endif
