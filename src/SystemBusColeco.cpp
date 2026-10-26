#include "SystemRamRegions.h"
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#include "SystemBusColeco.h"

#include "MachineIOPorts.h"
#include "Memory.h"

#include <utility>

SystemBusColeco::SystemBusColeco(Memory& memory, MachineIOPorts& ports)
    : m_memory(memory), m_ports(ports)
{
}

u8 SystemBusColeco::ReadMemory(u16 address)
{
    return m_memory.Read(address);
}

void SystemBusColeco::WriteMemory(u16 address, u8 value)
{
    m_memory.Write(address, value);
}

u8 SystemBusColeco::ReadIO(u64 tstates, u16 address)
{
    return m_ports.ColecoIn(tstates, static_cast<u8>(address));
}

void SystemBusColeco::WriteIO(u64 tstates, u16 address, u8 value)
{
    m_ports.ColecoOut(tstates, static_cast<u8>(address), value);
}

unsigned int SystemBusColeco::GetIOWaitStates(u64 tstates, u16 address,
                                              bool write) const
{
    (void)tstates;
    return m_ports.ColecoGetWaitStates(static_cast<u8>(address), write);
}

u8 SystemBusColeco::AcknowledgeInterrupt(u64 tstates)
{
    return m_ports.AcknowledgeInterrupt(tstates);
}

DebugDeviceDefinition SystemBusColeco::DescribeDebugDevice() const
{
    DebugDeviceDefinition device;
    device.key = "event.coleco.bus";
    device.parentKey = "event.coleco";
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

DebugProbe SystemBusColeco::GetIOProbe(u16 address) const
{
    return m_ports.GetColecoDebugProbe(static_cast<u8>(address));
}

// OS 7 sound manager, from the ColecoVision Coding Guide's jump table. A
// program reaches it through the vectors ($1Fxx, fixed by Coleco for every
// BIOS revision) or, in some games, through the routines' locked addresses
// directly (coleco29.asm lists them as LOCKED). JUKE_BOX and SND_MANAGER
// walk the cartridge's song tables without writing the PSG themselves, so
// the ROM map could not learn them from PSG writes alone.
std::size_t SystemBusColeco::GetFirmwareSoundEntries(const u16** entries) const
{
    static const u16 kEntries[] = {
        0x1F61, // PLAY_SONGS   (vector)
        0x1FB2, // INIT_SOUNDQ
        0x1FB5, // JUKE_BOXQ
        0x1FD6, // ALL_OFF
        0x1FEE, // INIT_SOUND
        0x1FF1, // JUKE_BOX
        0x1FF4, // SND_MANAGER
        0x0213, // INIT_SOUND   (locked)
        0x023B, // ALL_OFF
        0x025E, // JUKE_BOX
        0x027F, // SND_MANAGER
        0x0300, // PLAY_SONGS
    };
    *entries = kEntries;
    return sizeof(kEntries) / sizeof(kEntries[0]);
}

RamRegions SystemBusColeco::GetRamRegions() const
{ return DescribeSystemRam(m_memory, GetMachineType()); }
