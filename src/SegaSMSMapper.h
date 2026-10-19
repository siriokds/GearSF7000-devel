/*
 * GearSF7000 - standard Sega Master System cartridge mapper
 * Copyright (C) 2026  Saverio Russo
 */

#ifndef SEGA_SMS_MAPPER_H
#define SEGA_SMS_MAPPER_H

#include <array>
#include "Mapper.h"
#include "SaveStateStream.h"
#include "Cartridge.h"

// Standard Sega SMS mapper. The system bus owns the console's mirrored 8 KiB
// work RAM at $C000-$FFFF and must forward writes at $FFFC-$FFFF here as well.
// This class owns only cartridge ROM paging and the optional 32 KiB cartridge
// RAM visible in frame 2 ($8000-$BFFF).
class SegaSMSMapper : public Mapper
{
public:
    explicit SegaSMSMapper(Cartridge* cartridge)
        : Mapper(cartridge)
    {
        m_cartridgeRam.fill(0);
        Reset();
    }

    void Reset() override
    {
        m_control = 0;
        m_frame[0] = 0;
        m_frame[1] = 1;
        m_frame[2] = 2;
        // Cartridge RAM is non-volatile storage and survives a machine reset.
    }

    u8 Read(u16 address) override
    {
        if (address >= 0xC000)
            return 0xFF;

        if (address >= 0x8000 && CartridgeRamEnabled())
            return m_cartridgeRam[CartridgeRamOffset(address)];

        const u32 romSize = static_cast<u32>(m_pCartridge->GetROMSize());
        if (romSize == 0)
            return 0xFF;

        // The first KiB always comes from the first ROM page. This keeps the
        // reset vectors available even after software pages frame 0.
        if (address < 0x0400)
            return m_pCartridge->GetROM()[address % romSize];

        const unsigned frame = static_cast<unsigned>(address >> 14);
        const u32 offset = (static_cast<u32>(EffectiveRomBank(frame)) << 14)
                         + (address & 0x3FFFu);
        return m_pCartridge->GetROM()[offset % romSize];
    }

    void Write(u16 address, u8 value) override
    {
        if (address >= 0x8000 && address < 0xC000 && CartridgeRamEnabled())
            m_cartridgeRam[CartridgeRamOffset(address)] = value;

        switch (address)
        {
        case 0xFFFC: m_control = value; break;
        case 0xFFFD: m_frame[0] = value; break;
        case 0xFFFE: m_frame[1] = value; break;
        case 0xFFFF: m_frame[2] = value; break;
        default: break;
        }
    }

    void SaveState(std::ostream& stream) override
    {
        StateWriter writer(stream);
        writer.U8(m_control);
        writer.Bytes(m_frame.data(), m_frame.size());
        writer.Bytes(m_cartridgeRam.data(), m_cartridgeRam.size());
    }

    void LoadState(std::istream& stream) override
    {
        StateReader reader(stream);
        const u8 control = reader.U8();
        std::array<u8, 3> frame = {};
        std::array<u8, 0x8000> cartridgeRam = {};
        reader.Bytes(frame.data(), frame.size());
        reader.Bytes(cartridgeRam.data(), cartridgeRam.size());
        if (!reader.Ok())
            return;

        m_control = control;
        m_frame = frame;
        m_cartridgeRam = cartridgeRam;
    }

    u32 GetMapperId() const override { return 0x4D534D53u; } // 'SMSM'
    u8 GetRomBank() override { return EffectiveRomBank(2); }
    u32 GetRomBankAddress() override
    {
        return static_cast<u32>(EffectiveRomBank(2)) << 14;
    }
    u8 GetBankReg(int index) override
    {
        if (index == 0)
            return m_control;
        return index >= 1 && index <= 3 ? m_frame[index - 1] : 0;
    }

    bool CartridgeRamEnabled() const { return (m_control & 0x08) != 0; }
    unsigned CartridgeRamBank() const { return (m_control >> 2) & 1; }
    u8* GetCartridgeRam() { return m_cartridgeRam.data(); }
    const u8* GetCartridgeRam() const { return m_cartridgeRam.data(); }
    std::size_t GetCartridgeRamSize() const { return m_cartridgeRam.size(); }

    DebugDeviceDefinition DescribeDebugDevice() const override
    {
        DebugDeviceDefinition device;
        device.key = m_debugParentKey + ".mapper.sega_sms";
        device.parentKey = m_debugParentKey;
        device.typeKey = "sega.sms";
        device.name = "Sega Master System Mapper";

        DebugSpaceDefinition state;
        state.id = DebugCommonSpace::State;
        state.key = "state";
        state.name = "Mapper State";
        state.targetMax = 3;
        state.supportedAccesses = DebugEventAccess_Event;
        state.beforeValueAccesses = DebugEventAccess_Event;
        state.afterValueAccesses = DebugEventAccess_Event;
        state.targets.push_back({0, "Control", 8});
        state.targets.push_back({1, "Frame 0", 8});
        state.targets.push_back({2, "Frame 1", 8});
        state.targets.push_back({3, "Frame 2", 8});
        device.spaces.push_back(std::move(state));
        return device;
    }

private:
    u8 EffectiveRomBank(unsigned frame) const
    {
        const int count = m_pCartridge->GetROMBankCount();
        if (frame >= m_frame.size() || count <= 0)
            return 0;
        return static_cast<u8>(m_frame[frame] % count);
    }

    std::size_t CartridgeRamOffset(u16 address) const
    {
        return (static_cast<std::size_t>(CartridgeRamBank()) << 14)
             | (address & 0x3FFFu);
    }

    u8 m_control = 0;
    std::array<u8, 3> m_frame = {};
    std::array<u8, 0x8000> m_cartridgeRam = {};
};

#endif /* SEGA_SMS_MAPPER_H */
