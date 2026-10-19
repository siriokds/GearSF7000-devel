/*
 * GearSF7000 - Sega 315-5208 cartridge mapper in SG-1000 compatibility mode
 * Copyright (C) 2026  Saverio Russo
 */

#ifndef SEGA_315_5208_SG1000_MAPPER_H
#define SEGA_315_5208_SG1000_MAPPER_H

#include "Mapper.h"
#include "SaveStateStream.h"
#include "Cartridge.h"

class Sega3155208SG1000Mapper : public Mapper
{
public:
    explicit Sega3155208SG1000Mapper(Cartridge* cartridge)
        : Mapper(cartridge), m_bank(0)
    {
        Reset();
    }

    void Reset() override
    {
        m_bank = NormalizeBank(2);
    }

    u8 Read(u16 address) override
    {
        if (address >= 0xC000)
            return 0xFF;

        const u32 romSize = static_cast<u32>(m_pCartridge->GetROMSize());
        if (romSize == 0)
            return 0xFF;

        if (address < 0x8000)
            return m_pCartridge->GetROM()[address % romSize];
        return m_pCartridge->GetROM()[
            ((static_cast<u32>(m_bank) << 14) + (address & 0x3FFFu)) % romSize];
    }

    void Write(u16 address, u8 value) override
    {
        if (address == 0xFFFF)
            m_bank = NormalizeBank(value);
    }

    void SaveState(std::ostream& stream) override
    {
        StateWriter writer(stream);
        writer.U8(m_bank);
    }

    void LoadState(std::istream& stream) override
    {
        StateReader reader(stream);
        const u8 bank = reader.U8();
        if (reader.Ok())
            m_bank = NormalizeBank(bank);
    }

    u32 GetMapperId() const override { return 0x30323553u; } // 'S520'
    u8 GetRomBank() override { return m_bank; }
    u32 GetRomBankAddress() override
    {
        return static_cast<u32>(m_bank) << 14;
    }
    u8 GetBankReg(int index) override
    {
        return index == 0 ? 0 : index == 1 ? 1 : index == 2 ? m_bank : 0;
    }
    DebugDeviceDefinition DescribeDebugDevice() const override
    {
        return DescribeBankedMapper("sega3155208_sg1000",
            "sega.315_5208.sg1000", "Sega 315-5208 SG-1000 Mapper");
    }

private:
    u8 NormalizeBank(u8 bank) const
    {
        const int count = m_pCartridge->GetROMBankCount();
        return count > 0 ? static_cast<u8>(bank % count) : 0;
    }

    u8 m_bank;
};

#endif /* SEGA_315_5208_SG1000_MAPPER_H */
