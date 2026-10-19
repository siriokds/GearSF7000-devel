/*
 * GearSF7000 - SC-3000 Survivors Multicart and Megacart mappers
 * Copyright (C) 2026  Saverio Russo
 */

#ifndef SC3000_SURVIVORS_MAPPER_H
#define SC3000_SURVIVORS_MAPPER_H

#include "Mapper.h"
#include "SaveStateStream.h"
#include "Cartridge.h"

class SC3000SurvivorsMapper : public Mapper
{
public:
    enum Variant
    {
        Multicart,
        Megacart
    };

    SC3000SurvivorsMapper(Cartridge* cartridge, Variant variant)
        : Mapper(cartridge), m_variant(variant), m_block(0xFF)
    {
        Reset();
    }

    void Reset() override
    {
        // The undriven latch reads high. On the 2 MiB board that selects
        // block 63; on the 4 MiB board it selects block 127.
        m_block = 0xFF;
    }

    u8 Read(u16 address) override
    {
        if (address >= 0x8000)
            return 0xFF;

        const u32 romSize = static_cast<u32>(m_pCartridge->GetROMSize());
        if (romSize == 0)
            return 0xFF;
        const u32 offset = (GetRomBankAddress() + (address & 0x7FFFu)) % romSize;
        return m_pCartridge->GetROM()[offset];
    }

    void Write(u16, u8) override
    {
    }

    void WriteIO(u8 port, u8 value) override
    {
        // The cartridge decodes the upper three port bits, so every port in
        // $E0-$FF reaches the latch. Software normally writes $E0.
        if ((port & 0xE0) != 0xE0)
            return;

        if (m_variant == Multicart)
        {
            m_block = (value & 0x80)
                ? static_cast<u8>((value & 0x1F) | ((value & 0x40) ? 0x20 : 0x00))
                : 0x3F;
        }
        else
        {
            m_block = static_cast<u8>((value & 0x1F) | ((value & 0xC0) >> 1));
        }
    }

    void SaveState(std::ostream& stream) override
    {
        StateWriter writer(stream);
        writer.U8(m_block);
    }

    void LoadState(std::istream& stream) override
    {
        StateReader reader(stream);
        const u8 block = reader.U8();
        if (reader.Ok())
            m_block = block;
    }

    u32 GetMapperId() const override
    {
        // 'SCMC' and 'SCMG'.
        return m_variant == Multicart ? 0x434D4353u : 0x474D4353u;
    }

    u8 GetRomBank() override { return CurrentBlock(); }
    u32 GetRomBankAddress() override
    {
        const u32 romSize = static_cast<u32>(m_pCartridge->GetROMSize());
        return romSize == 0 ? 0 : (static_cast<u32>(CurrentBlock()) << 15) % romSize;
    }
    u8 GetBankReg(int index) override { return index == 0 ? m_block : 0; }
    DebugDeviceDefinition DescribeDebugDevice() const override
    {
        return DescribeBankedMapper(
            m_variant == Multicart
                ? "survivors_multicart"
                : "survivors_megacart",
            m_variant == Multicart
                ? "survivors.multicart" : "survivors.megacart",
            m_variant == Multicart
                ? "Survivors Multicart Mapper" : "Survivors Megacart Mapper");
    }

private:
    u8 CurrentBlock() const
    {
        const u32 blockCount = static_cast<u32>(m_pCartridge->GetROMSize()) >> 15;
        return blockCount == 0 ? 0 : static_cast<u8>(m_block % blockCount);
    }

    Variant m_variant;
    u8 m_block;
};

#endif /* SC3000_SURVIVORS_MAPPER_H */
