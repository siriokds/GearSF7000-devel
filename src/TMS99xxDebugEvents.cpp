// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#include "TMS99xxDebugEvents.h"
#include <cstdio>
#include <cstring>
namespace {
constexpr u8 kReadWrite = DebugEventAccess_Read | DebugEventAccess_Write;
constexpr u8 kPublished = DebugDescriptorCapability_EventsPublished;
const DebugTargetDescriptor kVdpPortTargets[] = {
    { 0xBE, "Data", 8 },
    { 0xBF, "Control / Status", 8 }
};

const DebugTargetDescriptor kVdpRegisterTargets[] = {
    { 0, "R0 Mode Control 1", 8 },
    { 1, "R1 Mode Control 2", 8 },
    { 2, "R2 Name Table", 8 },
    { 3, "R3 Color Table", 8 },
    { 4, "R4 Pattern Table", 8 },
    { 5, "R5 Sprite Attribute Table", 8 },
    { 6, "R6 Sprite Pattern Table", 8 },
    { 7, "R7 Text / Backdrop Color", 8 }
};

const DebugTargetDescriptor kVdpStateTargets[] = {
    { 0, "VRAM Address Counter", 14 },
    { 1, "Control Sequence (Awaiting First Byte)", 1 },
    { 2, "Read Buffer", 8 },
    { 3, "Status", 8 },
    { 4, "IRQ", 1 },
    { 5, "Video Mode", 3 },
    { 6, "CPU Port Latch Overflow", 23 },
    { 7, "Control Latch Byte", 8 }
};

void DecodePortRequest(u64 packed, bool& write, u16& address, u8& value)
{
    write = (packed & (1u << 22)) != 0;
    address = static_cast<u16>((packed >> 8) & 0x3FFF);
    value = static_cast<u8>(packed & 0xFF);
}

bool FormatTms99xxEvent(const DebugEvent& event,
                        DebugEventPresentation& presentation)
{
    char raster[192];
    const bool hasRaster = FormatDebugRasterContext(
        event.raster, raster, sizeof(raster));
    const bool latchReplacement = event.space == DebugCommonSpace::State &&
        event.targetV2 == Tms99xxDebugTarget::CpuPortLatchReplacement &&
        event.beforeKnown && event.afterKnown;
    if (!latchReplacement)
    {
        if (hasRaster)
            std::snprintf(presentation.details,
                sizeof(presentation.details), "%s", raster);
        return hasRaster;
    }

    bool oldWrite = false;
    bool newWrite = false;
    u16 oldAddress = 0;
    u16 newAddress = 0;
    u8 oldValue = 0;
    u8 newValue = 0;
    DecodePortRequest(event.beforeValueV2, oldWrite, oldAddress, oldValue);
    DecodePortRequest(event.afterValueV2, newWrite, newAddress, newValue);
    std::snprintf(presentation.typeLabel, sizeof(presentation.typeLabel),
        "VDP CPU LATCH");
    std::snprintf(presentation.details, sizeof(presentation.details),
        "Discarded: %s $%04X = $%02X\n"
        "Replacement: %s $%04X = $%02X%s%s",
        oldWrite ? "WRITE" : "READ", oldAddress, oldValue,
        newWrite ? "WRITE" : "READ", newAddress, newValue,
        hasRaster ? "\n" : "", hasRaster ? raster : "");

    presentation.semanticKey = "vdp_latch_replacement";
    presentation.attributes[presentation.attributeCount++] = {
        "discarded", "write", DebugPresentationValueKind::Boolean,
        oldWrite ? 1u : 0u, nullptr};
    presentation.attributes[presentation.attributeCount++] = {
        "discarded", "address", DebugPresentationValueKind::Unsigned,
        oldAddress, nullptr};
    presentation.attributes[presentation.attributeCount++] = {
        "discarded", "value", DebugPresentationValueKind::Unsigned,
        oldValue, nullptr};
    presentation.attributes[presentation.attributeCount++] = {
        "replacement", "write", DebugPresentationValueKind::Boolean,
        newWrite ? 1u : 0u, nullptr};
    presentation.attributes[presentation.attributeCount++] = {
        "replacement", "address", DebugPresentationValueKind::Unsigned,
        newAddress, nullptr};
    presentation.attributes[presentation.attributeCount++] = {
        "replacement", "value", DebugPresentationValueKind::Unsigned,
        newValue, nullptr};
    return true;
}
}
const DebugSpaceDescriptor kTms99xxDebugSpaces[kTms99xxDebugSpaceCount] = {
    // The CPU's port transactions, as the System Bus decodes them: every
    // board maps its VDP select (and its mirrors) onto $BE data / $BF
    // control, so one rule on this space sees the port writes of any board.
    { DebugCommonSpace::IoPorts, "io_ports", "I/O Ports", 0x00BE, 0x00BF,
      kReadWrite, DebugEventAccess_None, kReadWrite, 8,
      kPublished, kVdpPortTargets, sizeof(kVdpPortTargets) / sizeof(kVdpPortTargets[0]) },
    { DebugCommonSpace::Registers, "registers", "Registers", 0x0000, 0x00FF,
      DebugEventAccess_Write, DebugEventAccess_Write, DebugEventAccess_Write,
      8, kPublished, kVdpRegisterTargets, sizeof(kVdpRegisterTargets) / sizeof(kVdpRegisterTargets[0]) },
    { DebugCommonSpace::Memory, "memory", "VRAM", 0x0000, 0x3FFF,
      kReadWrite, kReadWrite, kReadWrite, 8, kPublished, NULL, 0 },
    { DebugCommonSpace::State, "state", "Internal State", 0x0000, 0x0007,
      DebugEventAccess_Event, DebugEventAccess_Event, DebugEventAccess_Event,
      8, static_cast<u8>(kPublished | DebugDescriptorCapability_SnapshotReadable),
      kVdpStateTargets, sizeof(kVdpStateTargets) / sizeof(kVdpStateTargets[0]) },
    { DebugCommonSpace::Raster, "raster", "Raster", 0x0000, 0xFFFF,
      DebugEventAccess_Event, DebugEventAccess_None, DebugEventAccess_Event,
      16, kPublished, NULL, 0 }
};

DebugDeviceDefinition DescribeTms99xxDebugEvents()
{
    DebugDeviceDefinition d;
    // Retain the existing family identity and key for rules and decoders.
    // The concrete hardware identity is in GetDeviceDescriptor().
    d.key = "event.sc3000.vdp";
    d.parentKey = "event.sc3000";
    d.typeKey = "ti.tms99xx";
    d.name = "TMS99xx VDP";
    d.formatter = &FormatTms99xxEvent;
    for (const auto& s : kTms99xxDebugSpaces)
    {
        DebugSpaceDefinition space;
        space.id = s.id;
        space.key = s.key;
        space.name = s.name;
        space.targetMin = s.targetMin;
        // Raster targets remain unrestricted for compatibility. A line that
        // does not exist on the active variant simply never publishes an event.
        space.targetMax = s.targetMax;
        space.supportedAccesses = s.supportedAccesses;
        space.beforeValueAccesses = s.beforeValueAccesses;
        space.afterValueAccesses = s.afterValueAccesses;
        space.defaultBitWidth = s.defaultBitWidth;
        space.capabilities = s.capabilities;
        for (size_t i = 0; i < s.targetCount; ++i)
            space.targets.push_back({s.targets[i].id, s.targets[i].name, s.targets[i].bitWidth});
        d.spaces.push_back(std::move(space));
    }
    DebugRulePresetDefinition latch;
    latch.key = "cpu_port_latch";
    latch.name = "VDP latch overflow";
    latch.spaceKey = "state";
    latch.targetStart = latch.targetEnd =
        Tms99xxDebugTarget::CpuPortLatchReplacement;
    latch.accessMask = DebugEventAccess_Event;
    d.presets.push_back(std::move(latch));

    DebugRulePresetDefinition vblank;
    vblank.key = "vblank_start";
    vblank.name = "VBlank";
    vblank.spaceKey = "raster";
    vblank.targetStart = vblank.targetEnd = 192;
    vblank.accessMask = DebugEventAccess_Event;
    vblank.useValueCondition = true;
    vblank.valueCondition = DebugValueCondition::AfterMaskedEqual;
    vblank.valueMask = 0x04;
    vblank.valueExpected = 0x04;
    d.presets.push_back(std::move(vblank));
    return d;
}
