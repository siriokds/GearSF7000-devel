#ifndef ASC16LMAPPER_H
#define ASC16LMAPPER_H

#include "Mapper.h"
#include "SaveStateStream.h"
#include "Cartridge.h"

class ASC16LMapper : public Mapper
{
public:
    ASC16LMapper(Cartridge* pCartridge);
    virtual ~ASC16LMapper();

    virtual void Reset() override;
    virtual u8 Read(u16 address) override;
    virtual void Write(u16 address, u8 value) override;
    virtual void SaveState(std::ostream& stream) override;
    virtual void LoadState(std::istream& stream) override;

    virtual u8 GetRomBank() override { return m_RomBank; }
    virtual u32 GetRomBankAddress() override { return m_RomBankAddress; }

    // 'A16L'
    virtual u32 GetMapperId() const override { return 0x4C363141u; }

private:
    u8 m_RomBank;
    u32 m_RomBankAddress;
};

inline ASC16LMapper::ASC16LMapper(Cartridge* pCartridge) : Mapper(pCartridge)
{
    Reset();
}

inline ASC16LMapper::~ASC16LMapper()
{
}

inline void ASC16LMapper::Reset()
{
    m_RomBank = 0;
    m_RomBankAddress = 0;
}

inline void ASC16LMapper::Write(u16 address, u8 value)
{
    // Se la scrittura avviene nel range della Page 2 (0x8000-0xBFFF)
    if (address >= 0x8000 && address <= 0xBFFF)
    {
        // Modulo wrap, not a mask. `value & (bankCount - 1)` only wraps
        // correctly when the bank count is a power of two: with six banks
        // (96 KB) the mask 0b101 leaves only banks 0, 1, 4 and 5 selectable,
        // so banks 2 and 3 exist in the image but cannot be reached. For
        // power-of-two sizes, which is the normal case, modulo and mask agree,
        // so no existing ROM changes behaviour.
        const int bankCount = m_pCartridge->GetROMBankCount();
        m_RomBank = bankCount > 0 ? static_cast<u8>(value % bankCount) : 0;
        m_RomBankAddress = static_cast<u32>(m_RomBank) << 14; // 14 bit = 16KB
    }
}

inline u8 ASC16LMapper::Read(u16 address)
{
    u8* pRom = m_pCartridge->GetROM();

    // Page 0 e 1: fisse
    if (address < 0x8000)
    {
        return pRom[address];
    }
    // Page 2: Paginata tramite m_RomBankAddress
    else if (address < 0xC000)
    {
        return pRom[(address & 0x3FFF) + m_RomBankAddress];
    }

    return 0xFF;
}

inline void ASC16LMapper::SaveState(std::ostream& stream)
{
    StateWriter w(stream);
    w.U8(m_RomBank);
    w.U32(m_RomBankAddress);
}

inline void ASC16LMapper::LoadState(std::istream& stream)
{
    StateReader r(stream);
    const u8 romBank = r.U8();
    const u32 romBankAddress = r.U32();
    if (!r.Ok())
        return;

    m_RomBank = romBank;
    m_RomBankAddress = romBankAddress;
}

#endif /* ASC16LMAPPER_H */