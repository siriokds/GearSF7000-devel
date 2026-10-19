// SPDX-License-Identifier: GPL-3.0-or-later
#include "definitions.h"
#if GEARSF7000_ENABLE_GAMEGEAR
#include "SystemBusGG.h"

#include "Audio.h"
#include "MachineIOPorts.h"
#include "Memory.h"
#include "SegaKeyboard.h"
#include "SystemRamRegions.h"

#include <utility>

SystemBusGG::SystemBusGG(Memory& memory, MachineIOPorts& ports,
                         SegaKeyboard& keyboard, Audio& audio,
                         bool exportRegion)
    : m_memory(memory), m_ports(ports),
      m_internal(audio, ports, keyboard, exportRegion)
{
    m_ports.ConfigureSMS(false);
}

u8 SystemBusGG::ReadMemory(u16 address)
{
    return m_memory.Read(address);
}

void SystemBusGG::WriteMemory(u16 address, u8 value)
{
    m_memory.Write(address, value);
}

u8 SystemBusGG::ReadIO(u64 tstates, u16 address)
{
    const BusReadResult result =
        m_internal.Read(BusAddressSpace::IO, tstates, address);
    return result.drivesDataBus ? result.value : 0xFF;
}

void SystemBusGG::WriteIO(u64 tstates, u16 address, u8 value)
{
    m_internal.Write(BusAddressSpace::IO, tstates, address, value);
}

unsigned int SystemBusGG::GetIOWaitStates(u64, u16 address, bool write) const
{
    return m_ports.SMSGetWaitStates(static_cast<u8>(address), write);
}

u8 SystemBusGG::AcknowledgeInterrupt(u64 tstates)
{
    return m_ports.AcknowledgeInterrupt(tstates);
}

DebugProbe SystemBusGG::GetIOProbe(u16 address) const
{
    return m_ports.GetSMSDebugProbe(static_cast<u8>(address));
}

DebugDeviceDefinition SystemBusGG::DescribeDebugDevice() const
{
    DebugDeviceDefinition device;
    device.key = "event.gg.bus";
    device.parentKey = "event.gg";
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

RamRegions SystemBusGG::GetRamRegions() const
{
    return DescribeSystemRam(m_memory, GetMachineType());
}

#endif /* GEARSF7000_ENABLE_GAMEGEAR */
