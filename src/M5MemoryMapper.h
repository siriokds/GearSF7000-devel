// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GEARSF7000_M5_MEMORY_MAPPER_H
#define GEARSF7000_M5_MEMORY_MAPPER_H

#include "IDebugEventProvider.h"
#include <array>
#include <span>

// Standard M5 motherboard decode + linear cartridge and optional EM-5 RAM.
// No bank registers or automatic mirroring. Storage is owned by Memory/Cartridge.
class M5MemoryMapper final : public IDebugEventProvider
{
public:
    static constexpr u32 BiosSize = 0x2000;
    static constexpr u32 RamSize = 0x1000;
    static constexpr u32 EM5RamSize = 0x8000;
    static constexpr u32 CartridgeBase = 0x2000;
    static constexpr u32 CartridgeCapacity = 0x5000;
    static constexpr u32 RamBase = 0x7000;
    static constexpr u32 EM5RamBase = 0x8000;

    enum class Area : u8 { Unmapped, Bios, Cartridge, InternalRam, ExternalRam };
    struct Location
    {
        Area area = Area::Unmapped;
        const u8* data = nullptr; // start of physical backing span
        u32 offset = 0;          // relative to data, not CPU address
        u32 size = 0;            // actual backing span size
        bool writable = false;
    };
    struct Mapping
    {
        Area area;
        u32 start;
        u32 end;                // inclusive CPU window
        u32 backingSize;        // zero means absent; may be smaller than window
        bool writable;
    };

    // BIOS may be absent or exactly 8 KB; cartridge absent or 1..20 KB;
    // Internal RAM must be 4 KB; optional EM-5 RAM must be 32 KB.
    // Buffers must outlive the binding. Failure leaves the old binding intact.
    // Binding never clears RAM. No owning pointers are serialized by this chip.
    bool Bind(std::span<const u8> bios, std::span<const u8> cartridge,
              std::span<u8> ram, std::span<u8> em5Ram = {});
    u8 Read(u16 address) const { return Peek(address); }
    u8 Peek(u16 address) const;
    void Write(u16 address, u8 value);
    Location Resolve(u16 address) const;
    std::array<Mapping, 4> GetMappings() const;
    bool HasEM5() const { return !m_em5Ram.empty(); }

    // A type description only; the future board supplies instance/parent keys
    // and publishes memory accesses. There are no bank-state change events.
    DebugDeviceDefinition DescribeDebugDevice() const override;

private:
    std::span<const u8> m_bios;
    std::span<const u8> m_cartridge;
    std::span<u8> m_ram;
    std::span<u8> m_em5Ram;
};
#endif
