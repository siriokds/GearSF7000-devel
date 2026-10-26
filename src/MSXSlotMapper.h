// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#ifndef MSX_SLOT_MAPPER_H
#define MSX_SLOT_MAPPER_H

#include "IDebugEventProvider.h"
#include <array>
#include <functional>
#include <span>
#include <utility>

class StateReader;
class StateWriter;

// A physical byte behind a CPU address, for debugger inspection. A device
// without a byte buffer (or a hole) may return an empty result.
struct MSXMemoryLocation
{
    const u8* data = nullptr;
    u32 offset = 0;
    bool writable = false;
    u32 size = 0;
};

// Addresses stay in CPU coordinates, including writes to cartridge bank
// registers. Device and backing storage must outlive their slot bindings.
class MSXSlotDevice
{
public:
    virtual ~MSXSlotDevice() = default;
    virtual u8 Read(u16 address) { return Peek(address); }
    virtual u8 Peek(u16 address) const = 0;
    virtual void Write(u16 address, u8 value) = 0;
    virtual MSXMemoryLocation Resolve(u16) const { return {}; }
};

// Non-owning linear window for BIOS, RAM and unbanked cartridges. Mirroring
// is an explicit wiring choice. This does not infer a cartridge's ROM layout.
class MSXLinearMemory final : public MSXSlotDevice
{
public:
    static MSXLinearMemory Rom(std::span<const u8> bytes, u32 base, u32 windowSize,
                               bool mirror = false);
    static MSXLinearMemory Ram(std::span<u8> bytes, u32 base, u32 windowSize,
                               bool mirror = false);
    u8 Peek(u16 address) const override;
    void Write(u16 address, u8 value) override;
    MSXMemoryLocation Resolve(u16 address) const override;

private:
    MSXLinearMemory(std::span<const u8> bytes, u8* writable, u32 base,
                    u32 windowSize, bool mirror);
    std::span<const u8> m_bytes;
    u8* m_writable;
    u32 m_base;
    u32 m_windowSize;
    bool m_mirror;
};

// ASCII 16 KiB MegaROM cartridge, after openMSX RomAscii16kB (revision in
// DOCS/MSX_SLOT_MAPPER.md): two 16 KiB windows, $4000-$7FFF and $8000-$BFFF,
// each showing the bank last written to $6000-$67FF and $7000-$77FF; both
// start at bank 0 and pages 0 and 3 are not decoded. A bank past the image
// wraps modulo the bank count. The ROM must be whole 16 KiB banks.
class MSXAscii16Rom final : public MSXSlotDevice
{
public:
    explicit MSXAscii16Rom(std::span<const u8> rom) : m_rom(rom) {}
    u8 Peek(u16 address) const override;
    void Write(u16 address, u8 value) override;
    MSXMemoryLocation Resolve(u16 address) const override;
    void Reset() { m_banks = {}; }
    u8 GetBank(unsigned window) const { return m_banks[window & 1]; }
    void SetBanks(u8 first, u8 second) { m_banks = {first, second}; } // silent: state restore
    // Targets 0 and 1: the bank registers of $4000 and $8000.
    using StateChange = std::function<void(u32 target, u8 before, u8 after)>;
    void SetStateChangeCallback(StateChange callback) { m_stateChange = std::move(callback); }
    // The board registers it whatever cartridge is fitted, so it is static.
    static DebugDeviceDefinition DescribeDebugDevice();

private:
    std::span<const u8> m_rom;
    std::array<u8, 2> m_banks{};
    StateChange m_stateChange;
};

// Motherboard slot selection, distinct from cartridge bank switching and
// the optional RAM mapper at ports FC-FF. Hardware behavior checked against
// openMSX MSXCPUInterface (reference revision recorded in DOCS/MSX_SLOT_MAPPER.md).
class MSXSlotMapper final : public IDebugEventProvider
{
public:
    struct Selection
    {
        u8 page;
        u8 primary;
        u8 secondary;
        bool secondaryRegister;
        MSXSlotDevice* device;
    };

    // Bits 0-3 describe physical slot expanders; configuration survives Reset.
    explicit MSXSlotMapper(u8 expandedSlots = 0);
    bool Bind(unsigned primary, unsigned secondary, unsigned page, MSXSlotDevice* device);
    void Reset();
    void SetPrimarySlots(u8 value); // future PPI port A output
    u8 GetPrimarySlots() const { return m_primary; }
    u8 GetSecondarySlots(unsigned primary) const;
    u8 GetExpandedSlots() const { return m_expanded; }
    Selection Inspect(u16 address) const;
    MSXMemoryLocation Resolve(u16 address) const;
    u8 Read(u16 address);
    u8 Peek(u16 address) const;
    void Write(u16 address, u8 value);

    // Only selector registers. The future board serializes RAM/devices and
    // validates its complete hardware/media identity separately.
    void SaveState(StateWriter& writer) const;
    bool LoadState(StateReader& reader);

    // Targets: 0 = primary, 1..4 = secondary register of primary slot 0..3.
    // Bind to the board's debug event publisher during machine composition.
    using StateChange = std::function<void(u32 target, u8 before, u8 after)>;
    void SetStateChangeCallback(StateChange callback) { m_stateChange = std::move(callback); }
    DebugDeviceDefinition DescribeDebugDevice() const override;

private:
    bool IsExpanded(unsigned slot) const { return (m_expanded & (1u << slot)) != 0; }
    void Refresh();
    void SetSecondarySlots(unsigned primary, u8 value);
    void Publish(u32 target, u8 before, u8 after);
    const u8 m_expanded;
    u8 m_primary = 0;
    std::array<u8, 4> m_secondary{};
    MSXSlotDevice* m_layout[4][4][4]{};
    std::array<Selection, 4> m_visible{};
    StateChange m_stateChange;
};

#endif
