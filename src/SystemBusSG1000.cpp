#include "SystemRamRegions.h"
// SPDX-License-Identifier: GPL-3.0-or-later
#include "SystemBusSG1000.h"

#include "MachineIOPorts.h"
#include "Memory.h"

#include <utility>

SystemBusSG1000::SystemBusSG1000(Memory& memory, MachineIOPorts& ports,
                                 bool sk1100Attached)
    : m_memory(memory), m_ports(ports), m_sk1100Attached(sk1100Attached)
{
}

u8 SystemBusSG1000::ReadMemory(u16 address)
{
    return m_memory.Read(address);
}

void SystemBusSG1000::WriteMemory(u16 address, u8 value)
{
    m_memory.Write(address, value);
}

u8 SystemBusSG1000::ReadIO(u64 tstates, u16 address)
{
    return m_ports.SG1000In(tstates, static_cast<u8>(address),
                            m_sk1100Attached);
}

void SystemBusSG1000::WriteIO(u64 tstates, u16 address, u8 value)
{
    m_ports.SG1000Out(tstates, static_cast<u8>(address), value,
                      m_sk1100Attached);
}

unsigned int SystemBusSG1000::GetIOWaitStates(
    u64 tstates, u16 address, bool write) const
{
    (void)tstates;
    return m_ports.SG1000GetWaitStates(static_cast<u8>(address), write);
}

u8 SystemBusSG1000::AcknowledgeInterrupt(u64 tstates)
{
    return m_ports.AcknowledgeInterrupt(tstates);
}

DebugProbe SystemBusSG1000::GetIOProbe(u16 address) const
{
    return m_ports.GetSG1000DebugProbe(static_cast<u8>(address),
                                       m_sk1100Attached);
}

DebugDeviceDefinition SystemBusSG1000::DescribeDebugDevice() const
{
    DebugDeviceDefinition device;
    device.key = "event.sg1000.bus";
    device.parentKey = "event.sg1000";
    device.typeKey = "debug.system_bus";
    device.name = "System Bus";
    DebugSpaceDefinition ports;
    ports.id = DebugCommonSpace::IoPorts;
    ports.key = "io_ports";
    ports.name = "I/O Ports";
    ports.targetMax = 0xff;
    ports.supportedAccesses = DebugEventAccess_Read | DebugEventAccess_Write;
    ports.afterValueAccesses = ports.supportedAccesses;
    device.spaces.push_back(std::move(ports));
    return device;
}

RamRegions SystemBusSG1000::GetRamRegions() const
{ return DescribeSystemRam(m_memory, GetMachineType()); }
