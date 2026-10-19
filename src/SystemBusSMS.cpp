// SPDX-License-Identifier: GPL-3.0-or-later
#include "definitions.h"
#if GEARSF7000_ENABLE_SMS
#include "SystemRamRegions.h"
#include "SystemBusSMS.h"

#include "MachineIOPorts.h"
#include "Memory.h"

#include <utility>

SystemBusSMS::SystemBusSMS(Memory& memory, MachineIOPorts& ports, bool pal)
    : m_memory(memory), m_ports(ports), m_pal(pal)
{
    m_ports.ConfigureSMS(pal);
}

u8 SystemBusSMS::ReadMemory(u16 address) { return m_memory.Read(address); }
void SystemBusSMS::WriteMemory(u16 address, u8 value)
{
    m_memory.Write(address, value);
}
u8 SystemBusSMS::ReadIO(u64 tstates, u16 address)
{
    return m_ports.SMSIn(tstates, static_cast<u8>(address));
}
void SystemBusSMS::WriteIO(u64 tstates, u16 address, u8 value)
{
    m_ports.SMSOut(tstates, static_cast<u8>(address), value);
}
unsigned int SystemBusSMS::GetIOWaitStates(u64, u16 address, bool write) const
{
    return m_ports.SMSGetWaitStates(static_cast<u8>(address), write);
}
u8 SystemBusSMS::AcknowledgeInterrupt(u64 tstates)
{
    return m_ports.AcknowledgeInterrupt(tstates);
}
DebugProbe SystemBusSMS::GetIOProbe(u16 address) const
{
    return m_ports.GetSMSDebugProbe(static_cast<u8>(address));
}

DebugDeviceDefinition SystemBusSMS::DescribeDebugDevice() const
{
    DebugDeviceDefinition device;
    device.key = "event.sms.bus";
    device.parentKey = "event.sms";
    device.typeKey = "debug.system_bus";
    device.name = "System Bus";
    DebugSpaceDefinition ports;
    ports.id = DebugCommonSpace::IoPorts;
    ports.key = "io_ports";
    ports.name = "I/O Ports";
    ports.targetMax = 0xFF;
    ports.supportedAccesses = DebugEventAccess_Read | DebugEventAccess_Write;
    ports.afterValueAccesses = ports.supportedAccesses;
    device.spaces.push_back(std::move(ports));
    return device;
}

RamRegions SystemBusSMS::GetRamRegions() const
{ return DescribeSystemRam(m_memory, GetMachineType()); }

#endif /* GEARSF7000_ENABLE_SMS */
