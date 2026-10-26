// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#include "MSXSlotMapper.h"
#include "SaveStateStream.h"
#include <stdexcept>

MSXLinearMemory::MSXLinearMemory(std::span<const u8> bytes, u8* writable,
                               u32 base, u32 windowSize, bool mirror)
    : m_bytes(bytes), m_writable(writable), m_base(base),
      m_windowSize(windowSize), m_mirror(mirror)
{
    if (bytes.empty() || bytes.data() == nullptr || bytes.size() > 0x10000 ||
        (bytes.size() & (bytes.size() - 1)) != 0 || base >= 0x10000 ||
        windowSize == 0 || windowSize > 0x10000 - base)
        throw std::invalid_argument("Invalid MSX linear memory window");
}

MSXLinearMemory MSXLinearMemory::Rom(std::span<const u8> bytes, u32 base,
                                    u32 windowSize, bool mirror)
{
    return MSXLinearMemory(bytes, nullptr, base, windowSize, mirror);
}

MSXLinearMemory MSXLinearMemory::Ram(std::span<u8> bytes, u32 base,
                                    u32 windowSize, bool mirror)
{
    return MSXLinearMemory(bytes, bytes.data(), base, windowSize, mirror);
}

MSXMemoryLocation MSXLinearMemory::Resolve(u16 address) const
{
    if (address < m_base || u32(address) - m_base >= m_windowSize)
        return {};
    u32 offset = u32(address) - m_base;
    if (m_mirror)
        offset &= static_cast<u32>(m_bytes.size() - 1);
    else if (offset >= m_bytes.size())
        return {};
    return {m_bytes.data(), offset, m_writable != nullptr, static_cast<u32>(m_bytes.size())};
}

u8 MSXLinearMemory::Peek(u16 address) const
{
    const auto location = Resolve(address);
    return location.data ? location.data[location.offset] : 0xFF;
}

void MSXLinearMemory::Write(u16 address, u8 value)
{
    const auto location = Resolve(address);
    if (location.writable)
        m_writable[location.offset] = value;
}

MSXSlotMapper::MSXSlotMapper(u8 expandedSlots) : m_expanded(expandedSlots)
{
    if (expandedSlots & 0xF0)
        throw std::invalid_argument("Invalid MSX expanded slot mask");
    Refresh();
}

bool MSXSlotMapper::Bind(unsigned primary, unsigned secondary, unsigned page,
                         MSXSlotDevice* device)
{
    if (primary >= 4 || secondary >= 4 || page >= 4 ||
        (secondary != 0 && !IsExpanded(primary)))
        return false;
    m_layout[primary][secondary][page] = device;
    Refresh();
    return true;
}

void MSXSlotMapper::Reset()
{
    m_primary = 0;
    m_secondary.fill(0);
    Refresh();
}

void MSXSlotMapper::Refresh()
{
    for (unsigned page = 0; page < 4; ++page)
    {
        const u8 primary = (m_primary >> (page * 2)) & 3;
        const u8 secondary = IsExpanded(primary) ?
            (m_secondary[primary] >> (page * 2)) & 3 : 0;
        m_visible[page] = {static_cast<u8>(page), primary, secondary, false,
                           m_layout[primary][secondary][page]};
    }
}

void MSXSlotMapper::Publish(u32 target, u8 before, u8 after)
{
    if (before != after && m_stateChange)
        m_stateChange(target, before, after);
}

void MSXSlotMapper::SetPrimarySlots(u8 value)
{
    const u8 before = m_primary;
    m_primary = value;
    Refresh();
    Publish(0, before, value);
}

void MSXSlotMapper::SetSecondarySlots(unsigned primary, u8 value)
{
    const u8 before = m_secondary[primary];
    m_secondary[primary] = value;
    Refresh();
    Publish(primary + 1, before, value);
}

u8 MSXSlotMapper::GetSecondarySlots(unsigned primary) const
{
    if (primary >= 4)
        throw std::out_of_range("Invalid MSX primary slot");
    return m_secondary[primary];
}

MSXSlotMapper::Selection MSXSlotMapper::Inspect(u16 address) const
{
    auto selection = m_visible[address >> 14];
    selection.secondaryRegister = address == 0xFFFF && IsExpanded(selection.primary);
    if (selection.secondaryRegister)
        selection.device = nullptr;
    return selection;
}

MSXMemoryLocation MSXSlotMapper::Resolve(u16 address) const
{
    const auto selection = Inspect(address);
    return selection.device ? selection.device->Resolve(address) : MSXMemoryLocation{};
}

u8 MSXSlotMapper::Read(u16 address)
{
    const auto selection = Inspect(address);
    if (selection.secondaryRegister)
        return static_cast<u8>(~m_secondary[selection.primary]);
    return selection.device ? selection.device->Read(address) : 0xFF;
}

u8 MSXSlotMapper::Peek(u16 address) const
{
    const auto selection = Inspect(address);
    if (selection.secondaryRegister)
        return static_cast<u8>(~m_secondary[selection.primary]);
    return selection.device ? selection.device->Peek(address) : 0xFF;
}

void MSXSlotMapper::Write(u16 address, u8 value)
{
    const auto selection = Inspect(address);
    if (selection.secondaryRegister)
        SetSecondarySlots(selection.primary, value);
    else if (selection.device)
        selection.device->Write(address, value);
}

void MSXSlotMapper::SaveState(StateWriter& writer) const
{
    writer.U16(1);
    writer.U8(m_expanded);
    writer.U8(m_primary);
    for (u8 value : m_secondary)
        writer.U8(value);
}

bool MSXSlotMapper::LoadState(StateReader& reader)
{
    const auto version = reader.U16();
    const auto expanded = reader.U8();
    const auto primary = reader.U8();
    std::array<u8, 4> secondary{};
    for (auto& value : secondary)
        value = reader.U8();
    if (!reader.Ok() || version != 1 || expanded != m_expanded)
        return false;
    for (unsigned slot = 0; slot < 4; ++slot)
        if (!IsExpanded(slot) && secondary[slot] != 0)
            return false;
    m_primary = primary;
    m_secondary = secondary;
    Refresh();
    return true;
}

DebugDeviceDefinition MSXSlotMapper::DescribeDebugDevice() const
{
    DebugDeviceDefinition device;
    device.key = "event.msx.slots";
    device.parentKey = "event.msx";
    device.typeKey = "msx.slots";
    device.name = "MSX slot selector";
    DebugSpaceDefinition state;
    state.id = DebugCommonSpace::State;
    state.key = "state";
    state.name = "Slot registers";
    state.supportedAccesses = DebugEventAccess_Event;
    state.beforeValueAccesses = DebugEventAccess_Event;
    state.afterValueAccesses = DebugEventAccess_Event;
    state.targets.push_back({0, "Primary slots", 8});
    for (unsigned slot = 0; slot < 4; ++slot)
        if (IsExpanded(slot))
            state.targets.push_back({slot + 1, "Secondary slots of primary " + std::to_string(slot), 8});
    state.targetMax = state.targets.back().id;
    device.spaces.push_back(std::move(state));
    return device;
}
