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
        CartridgeNotSupported
    };

    enum CartridgeRegions
    {
        CartridgeNTSC,
        CartridgePAL,
        CartridgeUnknownRegion
    };

    struct ForceConfiguration
    {
        CartridgeTypes type;
        CartridgeRegions region;
        // Used only when `type` is CartridgeNotSupported, i.e. auto-detection.
        // Auto has always had a fallback for images it cannot identify - it
        // was SG1000_16K, hardcoded and invisible - and for images of 64 KB or
        // more it had none at all, so loading simply failed. This makes that
        // fallback a choice. CartridgeNotSupported keeps the historic
        // behaviour.
        CartridgeTypes fallbackType;

        ForceConfiguration()
            : type(CartridgeNotSupported)
            , region(CartridgeUnknownRegion)
            , fallbackType(CartridgeNotSupported)
        { }
    };

public:
    Cartridge();
    ~Cartridge();
    void Init();
    void Reset();
    u32 GetCRC() const;
    bool IsPAL() const;
    bool HasSRAM() const;
    bool IsSF7000IPL() const;
    bool IsValidROM() const;
    // True when the type came from the CRC database or from a signature in the
    // image, rather than from a default. Auto-detection has no other way to
    // say "I did not recognise this".
    bool IsTypeIdentified() const;
    bool IsReady() const;
    CartridgeTypes GetType() const;
    void ForceConfig(ForceConfiguration config);
    // Whether a type can be forced for a ROM of this size. Static and
    // stateless on purpose: a UI needs to ask before anything is loaded, and
    // to say *why* an entry does not fit rather than just hiding it. `reason`
    // receives a short explanation when the result is false; may be NULL.
    static bool IsTypeCompatible(CartridgeTypes type, int romSize, const char** reason);
    int GetROMSize() const;
    int GetROMBankCount() const;
    const char* GetFilePath() const;
    const char* GetFileName() const;
    u8* GetROM() const;
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
    int m_iROMSize;
    CartridgeTypes m_Type;
    bool m_bValidROM;
    bool m_bTypeIdentified;
    bool m_bReady;
    char m_szFilePath[512];
    char m_szFileName[512];
    int m_iROMBankCount;
    bool m_bPAL;
    u32 m_iCRC;
    bool m_bSRAM;
    bool m_bSF7000IPL;
};

#endif	/* CARTRIDGE_H */
