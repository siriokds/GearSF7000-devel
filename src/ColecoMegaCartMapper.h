/* GearSF7000 - Coleco MegaCart cartridge, independent of the SGM expansion. */
#ifndef COLECO_MEGACART_MAPPER_H
#define COLECO_MEGACART_MAPPER_H

#include "Mapper.h"
#include "Cartridge.h"
#include "SaveStateStream.h"

class ColecoMegaCartMapper : public Mapper
{
public:
    explicit ColecoMegaCartMapper(Cartridge* cartridge) : Mapper(cartridge) {}

    static bool IsSupportedSize(int size)
    {
        return size == 0x20000 || size == 0x40000
            || size == 0x80000 || size == 0x100000;
    }

    // Deterministic emulator reset; physical power-on bank is unspecified.
    void Reset() override { m_bank = 0; }

    u8 Read(u16 address) override
    {
        if (IsSupportedSize(m_pCartridge->GetROMSize()) && address >= 0xFFC0)
            m_bank = static_cast<u8>(address & (BankCount() - 1));
        return Peek(address);
    }

    // Inspect the currently mapped byte, including at selectors: no strobe.
    // Keeping this distinct from Read is essential for debugger/MCP callers.
    u8 Peek(u16 address) const
    {
        if (address < 0x8000 || !IsSupportedSize(m_pCartridge->GetROMSize()))
            return 0xFF;
        return m_pCartridge->GetROM()[PhysicalOffset(address)];
    }

    u32 PhysicalOffset(u16 address) const
    {
        if (address < 0x8000 || !IsSupportedSize(m_pCartridge->GetROMSize()))
            return 0xFFFFFFFFu;
        const u32 bank = address < 0xC000 ? BankCount() - 1 : m_bank;
        return (bank << 14) | (address & 0x3FFF);
    }

    // The documented switching protocol is a read strobe, as implemented
    // by openMSX. Gearcoleco/CLK additionally strobe on writes; that extension
    // is not adopted without an electrical reference (see integration plan).
    void Write(u16, u8) override {}

    void SaveState(std::ostream& stream) override
    {
        StateWriter writer(stream);
        writer.U8(m_bank);
    }

    void LoadState(std::istream& stream) override
    {
        StateReader reader(stream);
        const u8 bank = reader.U8();
        if (reader.Ok() && bank < BankCount())
            m_bank = bank;
        else
            stream.setstate(std::ios::failbit);
    }

    u32 GetMapperId() const override { return 0x474D5643u; } // 'CVMG'
    u8 GetRomBank() override { return m_bank; }
    u32 GetRomBankAddress() override { return static_cast<u32>(m_bank) << 14; }
    DebugDeviceDefinition DescribeDebugDevice() const override
    {
        return DescribeBankedMapper("coleco_megacart", "coleco.megacart",
            "Coleco MegaCart");
    }

private:
    u32 BankCount() const { return static_cast<u32>(m_pCartridge->GetROMSize()) / 0x4000; }
    u8 m_bank = 0;
};

#endif
