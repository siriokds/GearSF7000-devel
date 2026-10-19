#include "SystemRamRegions.h"
// SPDX-License-Identifier: GPL-3.0-or-later
#include "SystemBusSC3000.h"
#include "Memory.h"
#include "IOPorts.h"

SystemBusSC3000::SystemBusSC3000(Memory& memory, IOPorts& ports,
                                 u32 cpuPsgClockHz, bool sf7000Attached)
    : m_memory(memory), m_ports(ports), m_cpuPsgClockHz(cpuPsgClockHz),
      m_sf7000Attached(sf7000Attached) {}

u8 SystemBusSC3000::ReadMemory(u16 address) { return m_memory.Read(address); }
void SystemBusSC3000::WriteMemory(u16 address, u8 value) { m_memory.Write(address, value); }
u8 SystemBusSC3000::ReadIO(u64 tstates, u16 address)
{
    return m_ports.In(tstates, static_cast<u8>(address));
}
void SystemBusSC3000::WriteIO(u64 tstates, u16 address, u8 value)
{
    m_ports.Out(tstates, static_cast<u8>(address), value);
}
unsigned int SystemBusSC3000::GetIOWaitStates(u64 tstates, u16 address, bool write) const
{
    return m_ports.GetWaitStatesAt(tstates, static_cast<u8>(address), write);
}
u8 SystemBusSC3000::AcknowledgeInterrupt(u64 tstates)
{
    return m_ports.AcknowledgeInterrupt(tstates);
}
DebugProbe SystemBusSC3000::GetIOProbe(u16 address) const
{
    return m_ports.GetDebugProbe(static_cast<u8>(address));
}

DebugDeviceDefinition SystemBusSC3000::DescribeDebugDevice() const
{
    DebugDeviceDefinition device;
    device.key = "event.sc3000.bus";
    device.parentKey = "event.sc3000";
    device.typeKey = "debug.system_bus";
    device.name = "System Bus";
    DebugSpaceDefinition ports;
    ports.id = DebugCommonSpace::IoPorts;
    ports.key = "io_ports";
    ports.name = "I/O Ports";
    // Preserve saved eight-bit port rules until raw-address matching migrates.
    ports.targetMax = 0xff;
    ports.supportedAccesses = DebugEventAccess_Read | DebugEventAccess_Write;
    ports.afterValueAccesses = ports.supportedAccesses;
    device.spaces.push_back(std::move(ports));
    return device;
}

RamRegions SystemBusSC3000::GetRamRegions() const
{ return DescribeSystemRam(m_memory, GetMachineType()); }
