// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#include "SN76489DebugEvents.h"

namespace
{
constexpr u8 kPublished = DebugDescriptorCapability_EventsPublished;

const DebugTargetDescriptor kRegisterTargets[] = {
    { 0, "Tone 0", 10 }, { 1, "Volume 0", 4 },
    { 2, "Tone 1", 10 }, { 3, "Volume 1", 4 },
    { 4, "Tone 2", 10 }, { 5, "Volume 2", 4 },
    { 6, "Noise", 3 },   { 7, "Volume 3", 4 }
};
}

const DebugSpaceDescriptor kSn76489DebugSpaces[kSn76489DebugSpaceCount] = {
    // Port attempts are owned by the System Bus. This view documents the
    // chip-select alias but does not claim to publish a second bus event.
    { DebugCommonSpace::IoPorts, "io_ports", "I/O Ports", 0x007F, 0x007F,
      DebugEventAccess_Write, DebugEventAccess_None, DebugEventAccess_Write,
      8, DebugDescriptorCapability_None, NULL, 0 },
    // The chip publishes the logical register selected by its latch/data
    // protocol. The event payload remains the exact byte received on the bus;
    // complete 10-bit periods are available through the PSG status snapshot.
    { DebugCommonSpace::Registers, "registers", "Logical Registers", 0x0000, 0x0007,
      DebugEventAccess_Event, DebugEventAccess_None, DebugEventAccess_Event,
      8, kPublished, kRegisterTargets,
      sizeof(kRegisterTargets) / sizeof(kRegisterTargets[0]) }
};

DebugDeviceDefinition DescribeSn76489DebugEvents()
{
    DebugDeviceDefinition device;
    device.key = "event.sc3000.psg";
    device.parentKey = "event.sc3000";
    device.typeKey = "ti.sn76489";
    device.name = "SN76489 PSG";
    for (const auto& source : kSn76489DebugSpaces)
    {
        DebugSpaceDefinition space;
        space.id = source.id;
        space.key = source.key;
        space.name = source.name;
        space.targetMin = source.targetMin;
        space.targetMax = source.targetMax;
        space.supportedAccesses = source.supportedAccesses;
        space.beforeValueAccesses = source.beforeValueAccesses;
        space.afterValueAccesses = source.afterValueAccesses;
        space.defaultBitWidth = source.defaultBitWidth;
        space.capabilities = source.capabilities;
        for (size_t i = 0; i < source.targetCount; ++i)
            space.targets.push_back({source.targets[i].id,
                source.targets[i].name, source.targets[i].bitWidth});
        device.spaces.push_back(std::move(space));
    }
    DebugRulePresetDefinition preset;
    preset.key = "logical_registers";
    preset.name = "PSG";
    preset.spaceKey = "registers";
    preset.targetEnd = 7;
    preset.accessMask = DebugEventAccess_Event;
    device.presets.push_back(std::move(preset));
    return device;
}
