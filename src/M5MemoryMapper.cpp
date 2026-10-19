// SPDX-License-Identifier: GPL-3.0-or-later
// Standard mapping checked against MAME sord/m5.cpp and bus/m5/rom.cpp.
#include "M5MemoryMapper.h"

bool M5MemoryMapper::Bind(std::span<const u8> bios,
                          std::span<const u8> cartridge, std::span<u8> ram,
                          std::span<u8> em5Ram)
{
    if ((!bios.empty() && (bios.size() != BiosSize || !bios.data())) ||
        cartridge.size() > CartridgeCapacity || (!cartridge.empty() && !cartridge.data()) ||
        ram.size() != RamSize || !ram.data() ||
        (!em5Ram.empty() && (em5Ram.size() != EM5RamSize || !em5Ram.data())))
        return false;
    m_bios = bios;
    m_cartridge = cartridge;
    m_ram = ram;
    m_em5Ram = em5Ram;
    return true;
}

M5MemoryMapper::Location M5MemoryMapper::Resolve(u16 address) const
{
    if (address < BiosSize && !m_bios.empty())
        return {Area::Bios, m_bios.data(), address, BiosSize, false};
    if (address >= CartridgeBase && address < RamBase)
    {
        const u32 offset = address - CartridgeBase;
        if (offset < m_cartridge.size())
            return {Area::Cartridge, m_cartridge.data(), offset,
                    static_cast<u32>(m_cartridge.size()), false};
    }
    if (address >= RamBase && address < RamBase + RamSize && !m_ram.empty())
        return {Area::InternalRam, m_ram.data(), address - RamBase, RamSize, true};
    if (address >= EM5RamBase && !m_em5Ram.empty())
        return {Area::ExternalRam, m_em5Ram.data(), address - EM5RamBase, EM5RamSize, true};
    return {};
}

u8 M5MemoryMapper::Peek(u16 address) const
{
    const auto location = Resolve(address);
    return location.data ? location.data[location.offset] : 0xFF;
}

void M5MemoryMapper::Write(u16 address, u8 value)
{
    if (address >= RamBase && address < RamBase + RamSize && !m_ram.empty())
        m_ram[address - RamBase] = value;
    else if (address >= EM5RamBase && !m_em5Ram.empty())
        m_em5Ram[address - EM5RamBase] = value;
}

std::array<M5MemoryMapper::Mapping, 4> M5MemoryMapper::GetMappings() const
{
    return {{{Area::Bios, 0, 0x1FFF, static_cast<u32>(m_bios.size()), false},
             {Area::Cartridge, CartridgeBase, 0x6FFF, static_cast<u32>(m_cartridge.size()), false},
             {Area::InternalRam, RamBase, 0x7FFF, static_cast<u32>(m_ram.size()), !m_ram.empty()},
             {m_em5Ram.empty() ? Area::Unmapped : Area::ExternalRam, 0x8000, 0xFFFF,
              static_cast<u32>(m_em5Ram.size()), !m_em5Ram.empty()}}};
}

DebugDeviceDefinition M5MemoryMapper::DescribeDebugDevice() const
{
    DebugDeviceDefinition device;
    device.typeKey = "sord.m5.standard_mapper";
    device.name = "Sord M5 standard memory mapper";
    DebugSpaceDefinition memory;
    memory.id = DebugCommonSpace::Memory;
    memory.key = "memory";
    memory.name = "CPU memory";
    memory.targetMax = 0xFFFF;
    memory.supportedAccesses = DebugEventAccess_Read | DebugEventAccess_Write;
    memory.afterValueAccesses = memory.supportedAccesses;
    memory.capabilities = DebugDescriptorCapability_None;
    device.spaces.push_back(std::move(memory));
    return device;
}
