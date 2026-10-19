/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026  Saverio Russo

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 *
 */

#ifndef CARTRIDGE_H
#define	CARTRIDGE_H

#include <list>
#include "definitions.h"

enum class MachineType : u8;

class Cartridge
{
public:
    enum CartridgeTypes
    {
        SG1000_1K,
        SG1000_16K,
        SC3000_2K,
        SC3000_32K,
        SF7000IPL,
		SC3000_ASC16L,
        // Flat 64 KB map, the same one the SF-7000 has once its IPL is
        // switched off: no banking, no read-only ROM, the whole space is RAM
        // and the image is simply loaded into it at reset. Never detected
        // automatically - it has no signature or checksum to detect - so it
        // exists only to be chosen by hand. A development target, not a
        // cartridge format that ever shipped.
        SC3000_FLAT64K,
        // Appended rather than inserted: these values are persisted as
        // Emulator/MapperType, so renumbering an existing one would
        // silently change what a saved configuration selects.
        // SC-3000 with a 16 KB cartridge RAM at $8000-$BFFF, keeping the
        // machine's own 2 KB alive above it. The 32 KB cartridge asserts a pin
        // that switches that internal RAM off and takes $8000-$FFFF for
        // itself; this one does not, so both are visible at once and the 2 KB
        // mirrors eight times across $C000-$FFFF. It is a 16K+2K
        // configuration, not a 16 KB one.
        SC3000_16K_AT_8000,
        CartridgeNotSupported,
        // Taiwanese SG-1000 II RAM adapters used by Dahjee, Jumbo and other
        // publishers for MSX-derived games. Kept after CartridgeNotSupported
        // so every previously persisted mapper number retains its meaning.
        // Type A replaces ROM at $2000-$3FFF with 8 KiB of adapter RAM and
        // leaves the console's mirrored 1 KiB RAM at $C000-$FFFF.
        SG1000_DAHJEE_TYPE_A,
        // Type B retains ROM at $0000-$BFFF and replaces the normal SG-1000
        // work-RAM decode with 8 KiB at $C000-$DFFF, mirrored at $E000-$FFFF.
        SG1000_DAHJEE_TYPE_B,
        // SC-3000 Survivors cartridges expose one selected 32 KiB ROM block
        // at $0000-$7FFF and 32 KiB of RAM above it. The two boards use
        // different bank-register wiring and image capacities.
        SC3000_SURVIVORS_MULTICART,
        SC3000_SURVIVORS_MEGACART,
        // Othello cartridge hardware, also used by the Taiwanese Hei Bai Qi:
        // 2 KiB cartridge RAM at $8000-$9FFF plus SG-1000 internal RAM.
        SG1000_OTHELLO_2K,
        // SG-1000-specific use of Sega's 315-5208 in Loretta no Shouzou:
        // $0000-$7FFF fixed and one 16 KiB page at $8000-$BFFF selected by
        // $FFFF. ROM only: PCB 171-5363-01 carries the mapper and the ROM,
        // no SRAM, and leaves B3 alone, so $C000-$FFFF is the console's own
        // RAM (Memory::GetBoardRamSize) and the $FFFF write echoes into it.
        // It has neither the SMS fixed 1 KiB nor mapper-controlled RAM.
        // An unidentified "TMR SEGA" image on the libretro core gets the
        // Dahjee B variant below instead.
        SG1000_SEGA_315_5208,
        // Standard Master System mapper: fixed first 1 KiB, three 16 KiB
        // page registers and optional 32 KiB cartridge RAM.
        SMS_SEGA_MAPPER,
        // ColecoVision cartridge: up to 32 KiB straight at $8000-$FFFF (four
        // 8 KiB chip selects from the console's 74LS138), no banking. The
        // BIOS and the 1 KiB work RAM are the console's, not the cartridge's.
        COLECOVISION,
        COLECO_MEGACART,
        MSX_LINEAR,
        SORD_M5_LINEAR,
        // The Castle (G-1046), Enri's circuit: 32 KiB ROM on /EXM2
        // ($0000-$7FFF) and an 8 KiB 6264 on /EXM1 with only A0-A12, so it
        // repeats across $8000-$BFFF. B3 is not wired: the console's own
        // RAM stays at $C000-$FFFF (1 KiB on the SG-1000 it was made for).
        SG1000_CASTLE_8K,
        // The 315-5208 cartridge on a Dahjee Type B adapter: the mapper pages
        // the ROM as above, and the adapter puts 8 KiB at $C000-$DFFF
        // (mirrored to $FFFF) and switches the console's RAM off through B3.
        // The $FFFF page write echoes into the adapter RAM at $DFFF, as on a
        // Mark III. A development target, and what an unidentified "TMR SEGA"
        // image gets on the libretro core, which expects the Mark III's 8 KiB.
        SG1000_SEGA_315_5208_DAHJEE_B,
        // Deliberately outside the sequential persisted mapper range.
        // ASC16LS packages are recognised by their file signature; the
        // desktop can also name this type in its manual mapper menu.
        SC3000_ASC16LS = 0x41534C53
    };

    // Both variants page the ROM with the same 315-5208; only the RAM at
    // $C000 differs (the console's, or the Dahjee B's).
    static bool UsesSega3155208(CartridgeTypes type)
    {
        return type == SG1000_SEGA_315_5208 || type == SG1000_SEGA_315_5208_DAHJEE_B;
    }

    struct ForceConfiguration
    {
        CartridgeTypes type;
        // Used only when `type` is CartridgeNotSupported, i.e. auto-detection.
        // The frontend supplies the selected motherboard's physical default
        // so an unidentified image cannot silently retain another machine's
        // RAM or mapper layout. CartridgeNotSupported means no fallback.
        CartridgeTypes fallbackType;
        // Opt-in for the web core: an image with the "TMR SEGA" header and no
        // CRC match runs on the mini Sega mapper with a Dahjee B's 8 KiB, the
        // RAM a Mark III image expects. Off everywhere else, so a Master
        // System image on the SMS board keeps its own mapper.
        bool detectSegaHeader;
        // Physical ASC16L RAM. The 8 KiB variant repeats across the 16 KiB
        // CPU window; existing cartridges default to 16 KiB.
        int asc16lRamBytes;

        ForceConfiguration()
            : type(CartridgeNotSupported)
            , fallbackType(CartridgeNotSupported)
            , detectSegaHeader(false)
            , asc16lRamBytes(0x4000)
        { }
    };

public:
    Cartridge();
    ~Cartridge();
    void Init();
    void Reset();
    u32 GetCRC() const;
    bool HasSRAM() const;
    bool IsSF7000IPL() const;
    bool IsValidROM() const;
    // True when the type came from the CRC database or from a signature in the
    // image, rather than from a default. Auto-detection has no other way to
    // say "I did not recognise this".
    bool IsTypeIdentified() const;
    bool IsReady() const;
    bool HasASC16LSignature() const;
    bool IsASC16LSPackage() const;
    bool HasASC16LSScrubbedSignature() const;
    // "SG16SEGA" header at $3FF0 (DOCS of the SG16 mapper): the 315-5208 and
    // the RAM the program asks for at $C000, 0 when the image has no such
    // header or its bank size is not 16 KB.
    int GetHeaderRamBytes() const;
    int GetASC16LRamBytes() const;
    // The type came from that header, not from the database or a manual pick.
    bool IsTypeFromSg16Header() const;
    // The core swaps the 315-5208 for its Dahjee B variant when the header
    // asks for more RAM than the console has.
    void UseDahjeeBRam();
    CartridgeTypes GetType() const;
    void ForceConfig(ForceConfiguration config);
    // Whether a type can be forced for a ROM of this size. Static and
    // stateless on purpose: a UI needs to ask before anything is loaded, and
    // to say *why* an entry does not fit rather than just hiding it. `reason`
    // receives a short explanation when the result is false; may be NULL.
    static bool IsTypeCompatible(CartridgeTypes type, int romSize, const char** reason);
    // Whether this build has a machine the type belongs to. A type whose
    // machines are left out is not offered anywhere, as if it did not exist.
    static constexpr bool IsTypeBuilt(CartridgeTypes type)
    {
#if !GEARSF7000_ENABLE_SMS && !GEARSF7000_ENABLE_GAMEGEAR
        if (type == SMS_SEGA_MAPPER) return false;
#endif
        (void)type;
        return true;
    }
    // Default physical cartridge/RAM arrangement for an otherwise
    // unidentified image on the selected motherboard. Auto detection may
    // still replace it with a recognised special cartridge.
    static CartridgeTypes DefaultTypeForMachine(MachineType machine);
    int GetROMSize() const;
    int GetROMBankCount() const;
    const char* GetFilePath() const;
    const char* GetFileName() const;
    u8* GetROM() const;
    const u8* GetASC16LSEncodedROM() const;
    bool LoadFromNull();
    bool LoadFromFile(const char* path);
    bool LoadFromBuffer(const u8* buffer, int size);
    void GetMapperDescr(char* outstr);

private:
    bool GatherMetadata(u32 crc);
    void GetInfoFromDB(u32 crc, CartridgeTypes& type);
    bool LoadFromZipFile(const u8* buffer, int size);

private:
    u8* m_pROM;
    u8* m_pASC16LSEncodedROM = nullptr;
    int m_iROMSize;
    CartridgeTypes m_Type;
    bool m_bValidROM;
    bool m_bTypeIdentified;
    bool m_bASC16LSPackage = false;
    bool m_bASC16LSScrubbedSignature = false;
    bool m_bDetectSegaHeader = false;
    int m_iHeaderRamBytes = 0;
    int m_Asc16lRamBytes = 0x4000;
    bool m_bSg16Header = false;
    bool m_bReady;
    char m_szFilePath[512];
    char m_szFileName[512];
    int m_iROMBankCount;
    u32 m_iCRC;
    bool m_bSRAM;
    bool m_bSF7000IPL;
};

#endif	/* CARTRIDGE_H */
