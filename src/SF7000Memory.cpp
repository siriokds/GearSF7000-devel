/*
 * GearSF7000 - SF-7000 memory subsystem debug description
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "SF7000Memory.h"

#include <utility>

DebugDeviceDefinition SF7000MemoryDevice::DescribeDebugDevice() const
{
    DebugDeviceDefinition device;
    device.key = "event.sc3000.sf7000.memory";
    device.parentKey = "event.sc3000.sf7000";
    device.typeKey = "sega.sf7000.memory";
    device.name = "SF-7000 Memory";

    DebugSpaceDefinition ram;
    ram.id = RamSpace;
    ram.key = "ram";
    ram.name = "RAM 64K";
    ram.targetMax = 0xFFFF;
    ram.supportedAccesses = DebugEventAccess_Read | DebugEventAccess_Write;
    ram.beforeValueAccesses = DebugEventAccess_Write;
    ram.afterValueAccesses = ram.supportedAccesses;
    ram.capabilities = DebugDescriptorCapability_EventsPublished |
                       DebugDescriptorCapability_SnapshotReadable;
    device.spaces.push_back(std::move(ram));

    DebugSpaceDefinition ipl;
    ipl.id = IplSpace;
    ipl.key = "ipl";
    ipl.name = "IPL ROM 8K";
    ipl.targetMax = 0x1FFF;
    ipl.supportedAccesses = DebugEventAccess_Read;
    ipl.afterValueAccesses = DebugEventAccess_Read;
    ipl.capabilities = DebugDescriptorCapability_EventsPublished |
                       DebugDescriptorCapability_SnapshotReadable;
    device.spaces.push_back(std::move(ipl));

    DebugSpaceDefinition state;
    state.id = DebugCommonSpace::State;
    state.key = "ipl_visibility";
    state.name = "IPL Visibility";
    state.targetMax = StateCount - 1;
    state.supportedAccesses = DebugEventAccess_Event;
    state.beforeValueAccesses = DebugEventAccess_Event;
    state.afterValueAccesses = DebugEventAccess_Event;
    state.defaultBitWidth = 1;
    state.capabilities = DebugDescriptorCapability_EventsPublished |
                         DebugDescriptorCapability_SnapshotReadable;
    state.targets = {
        {IplVisible, "Visible", 1}
    };
    device.spaces.push_back(std::move(state));
    return device;
}
