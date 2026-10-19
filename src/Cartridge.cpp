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

#include <string>
#include <algorithm>
#include <ctype.h>
#include <cstring>
#include "Cartridge.h"
#include "ASC16LSFormat.h"
#include "ISystemBus.h"
#include "miniz/miniz.h"
#include "game_db.h"

Cartridge::Cartridge()
{
    InitPointer(m_pROM);
    m_iROMSize = 0;
    m_Type = CartridgeNotSupported;
    m_bTypeIdentified = false;
    m_bValidROM = false;
    m_bReady = false;
    m_szFilePath[0] = 0;
    m_szFileName[0] = 0;
    m_iROMBankCount = 0;
    m_bSRAM = false;
    m_bSF7000IPL = false;
    m_iCRC = 0;
    m_Asc16lRamBytes = 0x4000;
}

Cartridge::~Cartridge()
{
    SafeDeleteArray(m_pROM);
    SafeDeleteArray(m_pASC16LSEncodedROM);
}

void Cartridge::Init()
{
    Reset();
}

void Cartridge::Reset()
{
    SafeDeleteArray(m_pROM);
    SafeDeleteArray(m_pASC16LSEncodedROM);
    m_iROMSize = 0;
    m_Type = CartridgeNotSupported;
    m_bTypeIdentified = false;
    m_bASC16LSPackage = false;
    m_bASC16LSScrubbedSignature = false;
    m_bValidROM = false;
    m_bReady = false;
    m_szFilePath[0] = 0;
    m_szFileName[0] = 0;
    m_iROMBankCount = 0;
    m_bSRAM = false;
    m_bSF7000IPL = false;
    m_iCRC = 0;
    m_Asc16lRamBytes = 0x4000;
}

u32 Cartridge::GetCRC() const
{
    return m_iCRC;
}

bool Cartridge::HasSRAM() const
{
    return m_bSRAM;
}

bool Cartridge::IsSF7000IPL() const
{
    return m_bSF7000IPL;
}



bool Cartridge::IsValidROM() const
{
    return m_bValidROM;
}

bool Cartridge::IsReady() const
{
    return m_bReady;
}

bool Cartridge::HasASC16LSignature() const
{
    // A package may restore these bytes for the CPU, but they are not a
    // signature in the supplied cartridge image.
    return !m_bASC16LSPackage && IsValidPointer(m_pROM) && m_iROMSize >= 0x3FF4
        && m_pROM[0x3FF0] == 'A' && m_pROM[0x3FF1] == 'S'
        && m_pROM[0x3FF2] == '1' && m_pROM[0x3FF3] == '6';
}

bool Cartridge::IsASC16LSPackage() const
{
    return m_bASC16LSPackage;
}

bool Cartridge::HasASC16LSScrubbedSignature() const
{
    return m_bASC16LSScrubbedSignature;
}

int Cartridge::GetHeaderRamBytes() const
{
    return m_iHeaderRamBytes;
}

int Cartridge::GetASC16LRamBytes() const
{
    return m_Asc16lRamBytes;
}

bool Cartridge::IsTypeFromSg16Header() const
{
    return m_bSg16Header;
}

void Cartridge::UseDahjeeBRam()
{
    if (m_Type == SG1000_SEGA_315_5208)
        m_Type = SG1000_SEGA_315_5208_DAHJEE_B;
}

Cartridge::CartridgeTypes Cartridge::GetType() const
{
    return m_Type;
}

void Cartridge::ForceConfig(Cartridge::ForceConfiguration config)
{
    if (m_bASC16LSPackage)
    {
        // The package defines both the mapper and physical RAM. A stale
        // manual selection must not reinterpret its scrambled banks.
        Log("ASC16LS package: using Internal mapper");
        return;
    }
    if (config.type == SC3000_ASC16LS)
    {
        // Internal requires its package header and translation table.
        m_bReady = m_bValidROM = false;
        Log("Internal mapper requires an ASC16LS package");
        return;
    }
    m_bDetectSegaHeader = config.detectSegaHeader;
    m_Asc16lRamBytes = config.asc16lRamBytes == 0x2000 ? 0x2000 : 0x4000;
    m_iCRC = CalculateCRC32(0, m_pROM, m_iROMSize);
    GatherMetadata(m_iCRC);

    switch (config.type)
    {
        case Cartridge::SC3000_ASC16L:
            m_Type = config.type;
            Log("Forcing Mapper: ASCII 16 Light");
            break;
        case Cartridge::SG1000_1K:
            m_Type = config.type;
            Log("Forcing Mapper: SG-1000");
            break;
        case Cartridge::SG1000_16K:
            m_Type = config.type;
            Log("Forcing Mapper: SG-1000 (16K)");
            break;
        case Cartridge::SC3000_2K:
            m_Type = config.type;
            Log("Forcing Mapper: SC-3000 standard (console RAM)");
            break;
        case Cartridge::SC3000_32K:
            m_Type = config.type;
            Log("Forcing Mapper: SC-3000 (32K)");
            break;
        case Cartridge::SC3000_16K_AT_8000:
            m_Type = config.type;
            Log("Forcing Mapper: SC-3000 (16K cartridge RAM + console RAM)");
            break;
        case Cartridge::SC3000_FLAT64K:
            m_Type = config.type;
            // No validity check at all: that is the point of this mode.
            m_bValidROM = true;
            m_bSF7000IPL = false;
            Log("Forcing Mapper: flat 64K RAM");
            break;
        case Cartridge::SG1000_DAHJEE_TYPE_A:
            m_Type = config.type;
            Log("Forcing Mapper: SG-1000 Dahjee Type A (8K external + console RAM)");
            break;
        case Cartridge::SG1000_DAHJEE_TYPE_B:
            m_Type = config.type;
            Log("Forcing Mapper: SG-1000 Dahjee Type B (8K RAM adapter)");
            break;
        case Cartridge::SC3000_SURVIVORS_MULTICART:
            m_Type = config.type;
            Log("Forcing Mapper: SC-3000 Survivors MkII Multicart");
            break;
        case Cartridge::SC3000_SURVIVORS_MEGACART:
            m_Type = config.type;
            Log("Forcing Mapper: SC-3000 Survivors Megacart");
            break;
        case Cartridge::SG1000_OTHELLO_2K:
            m_Type = config.type;
            Log("Forcing Mapper: SG-1000 Othello cartridge RAM");
            break;
        case Cartridge::SG1000_CASTLE_8K:
            m_Type = config.type;
            Log("Forcing Mapper: SG-1000 The Castle cartridge RAM");
            break;
        case Cartridge::SG1000_SEGA_315_5208:
            m_Type = config.type;
            Log("Forcing Mapper: Sega 315-5208 with SG-1000 compatibility");
            break;
        case Cartridge::SG1000_SEGA_315_5208_DAHJEE_B:
            m_Type = config.type;
            Log("Forcing Mapper: Sega 315-5208 on a Dahjee B adapter");
            break;
        case Cartridge::SMS_SEGA_MAPPER:
            m_Type = config.type;
            Log("Forcing Mapper: Sega Master System");
            break;
        case Cartridge::SORD_M5_LINEAR:
        case Cartridge::MSX_LINEAR:
        case Cartridge::COLECO_MEGACART:
        case Cartridge::COLECOVISION:
            m_Type = config.type;
            // The $AA55/$55AA signature is the BIOS's business, not a
            // condition for the cartridge to sit on the bus.
            m_bValidROM = true;
            m_bSF7000IPL = false;
            Log("Forcing cartridge type %d", int(m_Type));
            break;
        default:
            break;
    }

    // Auto-detection: if nothing recognised the image, fall back to whatever
    // was chosen rather than to the historic hardcoded default. Only reached
    // when no type was forced, and only when detection actually failed - a
    // recognised image is never overridden.
    if (config.type == CartridgeNotSupported
        && config.fallbackType != CartridgeNotSupported
        && !m_bTypeIdentified)
    {
        m_Type = config.fallbackType;
        m_bValidROM = true;
        m_bSF7000IPL = false;
        Log("ROM not identified: falling back to the selected mapper");
    }
}

int Cartridge::GetROMSize() const
{
    return m_iROMSize;
}

int Cartridge::GetROMBankCount() const
{
    return m_iROMBankCount;
}

const char* Cartridge::GetFilePath() const
{
    return m_szFilePath;
}

const char* Cartridge::GetFileName() const
{
    return m_szFileName;
}

u8* Cartridge::GetROM() const
{
    return m_pROM;
}

const u8* Cartridge::GetASC16LSEncodedROM() const
{
    return m_pASC16LSEncodedROM;
}

bool Cartridge::LoadFromZipFile(const u8* buffer, int size)
{
    using namespace std;

    mz_zip_archive zip_archive;
    mz_bool status;
    memset(&zip_archive, 0, sizeof (zip_archive));

    status = mz_zip_reader_init_mem(&zip_archive, (void*) buffer, size, 0);
    if (!status)
    {
        Log("mz_zip_reader_init_mem() failed!");
        return false;
    }

    for (unsigned int i = 0; i < mz_zip_reader_get_num_files(&zip_archive); i++)
    {
        mz_zip_archive_file_stat file_stat;
        if (!mz_zip_reader_file_stat(&zip_archive, i, &file_stat))
        {
            Log("mz_zip_reader_file_stat() failed!");
            mz_zip_reader_end(&zip_archive);
            return false;
        }

        Log("ZIP Content - Filename: \"%s\", Comment: \"%s\", Uncompressed size: %u, Compressed size: %u", file_stat.m_filename, file_stat.m_comment, (unsigned int) file_stat.m_uncomp_size, (unsigned int) file_stat.m_comp_size);

        string fn((const char*) file_stat.m_filename);
        transform(fn.begin(), fn.end(), fn.begin(), (int(*)(int)) tolower);
        string extension = fn.substr(fn.find_last_of(".") + 1);

        bool supported_extension =
            (extension == "sg") || (extension == "sc") ||
            (extension == "sms") ||
            (extension == "asc") || (extension == "rom") ||
            (extension == "bin") || (extension == "asls");
#if !GEARSF7000_PRODUCT_SC3000
        supported_extension = supported_extension ||
            (extension == "col") || (extension == "cv");
#endif

        if (supported_extension)
        {
            void *p;
            size_t uncomp_size;

            p = mz_zip_reader_extract_file_to_heap(&zip_archive, file_stat.m_filename, &uncomp_size, 0);
            if (!p)
            {
                Log("mz_zip_reader_extract_file_to_heap() failed!");
                mz_zip_reader_end(&zip_archive);
                return false;
            }

            bool ok = LoadFromBuffer((const u8*) p, (int)uncomp_size);

            free(p);
            mz_zip_reader_end(&zip_archive);

            return ok;
        }
    }
    return false;
}

bool Cartridge::LoadFromFile(const char* path)
{
    using namespace std;

    Log("Loading %s...", path);

    Reset();

    strcpy(m_szFilePath, path);

    std::string pathstr(path);
    std::string filename;

    size_t pos = pathstr.find_last_of("\\");
    if (pos != std::string::npos)
    {
        filename.assign(pathstr.begin() + pos + 1, pathstr.end());
    }
    else
    {
        pos = pathstr.find_last_of("/");
        if (pos != std::string::npos)
        {
            filename.assign(pathstr.begin() + pos + 1, pathstr.end());
        }
        else
        {
            filename = pathstr;
        }
    }

    strcpy(m_szFileName, filename.c_str());

    ifstream file(path, ios::in | ios::binary | ios::ate);

    if (file.is_open())
    {
        int size = static_cast<int> (file.tellg());
        char* memblock = new char[size];
        file.seekg(0, ios::beg);
        file.read(memblock, size);
        file.close();

        string fn(path);
        transform(fn.begin(), fn.end(), fn.begin(), (int(*)(int)) tolower);
        string extension = fn.substr(fn.find_last_of(".") + 1);

#if GEARSF7000_PRODUCT_SC3000
        if ((extension == "col") || (extension == "cv"))
        {
            Log("Unsupported GearSC3000 file extension: %s", extension.c_str());
            SafeDeleteArray(memblock);
            Reset();
            return false;
        }
#endif

        if (extension == "zip")
        {
            Log("Loading from ZIP...");
            m_bReady = LoadFromZipFile(reinterpret_cast<u8*> (memblock), size);
        }
        else
        {
            m_bReady = LoadFromBuffer(reinterpret_cast<u8*> (memblock), size);
        }

        if (m_bReady)
        {
            Log("ROM loaded", path);
        }
        else
        {
            Log("There was a problem loading the memory for file %s...", path);
        }

        SafeDeleteArray(memblock);
    }
    else
    {
        Log("There was a problem loading the file %s...", path);
        m_bReady = false;
    }

    if (!m_bReady)
    {
        Reset();
    }

    return m_bReady;
}




bool Cartridge::LoadFromNull()
{
    using namespace std;

    Log("Loading %s...", "System");

    Reset();

    strcpy(m_szFilePath, "");
    strcpy(m_szFileName, "");

    int size = 8192;
    char* memblock = new char[size];
    memset(memblock, 0, size);
    m_bReady = LoadFromBuffer(reinterpret_cast<u8*> (memblock), size);

    if (m_bReady)
    {
        Log("System loaded");
    }
    else
    {
        Log("There was a problem loading the system...");
    }

    SafeDeleteArray(memblock);

    if (!m_bReady)
    {
        Reset();
    }

    return m_bReady;
}

bool Cartridge::LoadFromBuffer(const u8* buffer, int size)
{
    if (IsValidPointer(buffer))
    {
        Log("Loading from buffer... Size: %d", size);

        // SCLS v2: a small header followed by 16 or 32 physical pages.
        // The logical-to-physical table is shared with its mapper.
        if (size >= 8 && std::memcmp(buffer, ASC16LSFormat::Magic, 8) == 0)
        {
            constexpr int kHeaderSize = ASC16LSFormat::HeaderSize;
            if (size < kHeaderSize)
                return false;
            const int logicalPages = buffer[13];
            const int physicalPages = buffer[14];
            const int expectedPhysicalPages = logicalPages == 32 ? 32 : 16;
            if (logicalPages != 8 && logicalPages != 16 && logicalPages != 32)
            {
                Log("Invalid SCLS logical page count");
                return false;
            }
            if (physicalPages != expectedPhysicalPages
                || size != kHeaderSize + physicalPages * ASC16LSFormat::PageSize
                || (buffer[12] != 8 && buffer[12] != 16)
                || buffer[15] > 1)
            {
                Log("Invalid SCLS package size or header");
                return false;
            }
            const u32 expectedCrc = static_cast<u32>(buffer[8])
                | (static_cast<u32>(buffer[9]) << 8)
                | (static_cast<u32>(buffer[10]) << 16)
                | (static_cast<u32>(buffer[11]) << 24);
            const int physicalBytes = physicalPages * ASC16LSFormat::PageSize;
            m_pASC16LSEncodedROM = new u8[physicalBytes];
            m_pROM = new u8[logicalPages * ASC16LSFormat::PageSize];
            std::memcpy(m_pASC16LSEncodedROM, buffer + kHeaderSize, physicalBytes);
            for (int logical = 0; logical < logicalPages; ++logical)
                std::memcpy(m_pROM + logical * ASC16LSFormat::PageSize,
                    m_pASC16LSEncodedROM + ASC16LSFormat::PhysicalByLogical[logical]
                        * ASC16LSFormat::PageSize,
                    ASC16LSFormat::PageSize);
            m_iROMSize = logicalPages * ASC16LSFormat::PageSize;
            m_bASC16LSScrubbedSignature = buffer[15] == 1;
            if (m_bASC16LSScrubbedSignature)
                std::memcpy(m_pROM + 0x3ff0, "AS16", 4);
            m_iCRC = CalculateCRC32(0, m_pROM, m_iROMSize);
            if (m_iCRC != expectedCrc)
            {
                Log("ASC16LS package CRC mismatch");
                Reset();
                return false;
            }
            m_iROMBankCount = logicalPages;
            m_Asc16lRamBytes = static_cast<int>(buffer[12]) * 1024;
            m_Type = SC3000_ASC16LS;
            m_bTypeIdentified = m_bValidROM = m_bReady = true;
            m_bASC16LSPackage = true;
            Log("ASC16LS Internal package recognised: %d KiB ROM, %d KiB RAM",
                m_iROMSize / 1024, m_Asc16lRamBytes / 1024);
            return true;
        }

        // Unkown size
        if ((size % 1024) != 0)
        {
            Log("Invalid size found. %d bytes", size);
            //return false;
        }

        m_iROMSize = size;
        m_pROM = new u8[m_iROMSize];
        memcpy(m_pROM, buffer, m_iROMSize);

        m_bReady = true;

        m_iCRC = CalculateCRC32(0, m_pROM, m_iROMSize);
#ifdef _DEBUG
    printf("CARTRIGE CRC: \t  /FEED  (SCLK) => 0x%08x\n", m_iCRC);
#endif
        GatherMetadata(m_iCRC);

        return true;
    }
    else
        return false;
}

bool Cartridge::GatherMetadata(u32 crc)
{
    m_bSRAM = false;
    // Cleared here and not only in Init()/Reset(): ForceConfig() calls this
    // again, so a stale flag from a previous image would make an unidentified
    // ROM look identified and skip the fallback.
    m_bTypeIdentified = false;
    m_bSg16Header = false;
    m_iHeaderRamBytes = 0;

    Log("ROM Size: %d KB", m_iROMSize / 1024);

    m_iROMBankCount = (m_iROMSize / 0x4000) + (m_iROMSize % 0x4000 ? 1 : 0);

    Log("ROM Bank Count: %d", m_iROMBankCount);

    const auto colecoHeader = [&](int offset) {
        return (m_pROM[offset] == 0x55 && m_pROM[offset + 1] == 0xAA)
            || (m_pROM[offset] == 0xAA && m_pROM[offset + 1] == 0x55);
    };
    if ((m_iROMSize == 0x20000 || m_iROMSize == 0x40000
            || m_iROMSize == 0x80000 || m_iROMSize == 0x100000)
        && !colecoHeader(0) && colecoHeader(m_iROMSize - 0x4000))
    {
        m_Type = COLECO_MEGACART;
        m_bTypeIdentified = m_bValidROM = true;
        m_bSF7000IPL = false;
        return true;
    }

    // These published cartridge formats use their complete fixed-size image
    // as the banked store. MAME and the original SC3K-System implementation
    // use the same size distinction, which also permits future image
    // revisions without requiring a new CRC entry.
    if (m_iROMSize == 0x200000 || m_iROMSize == 0x400000)
    {
        m_Type = m_iROMSize == 0x200000
            ? Cartridge::SC3000_SURVIVORS_MULTICART
            : Cartridge::SC3000_SURVIVORS_MEGACART;
        m_bTypeIdentified = true;
        m_bValidROM = true;
        m_bSF7000IPL = false;
        Log(m_iROMSize == 0x200000
            ? "SC-3000 Survivors MkII Multicart size detected"
            : "SC-3000 Survivors Megacart size detected");
        return true;
    }

    //m_Type = Cartridge::CartridgeNotSupported;

    //int headerOffset = 0;
    //u16 header = m_pROM[headerOffset + 1] | (m_pROM[headerOffset + 0] << 8);
    //m_bValidROM = (header == 0xAA55) || (header == 0x55AA);

    //if (header == 0x6699)
    //{
    //    Log("Cartridge is a Colec Adam expansion ROM. Header: %X", header);
    //}

    //if (m_bValidROM && (m_iROMSize <= 0x8000))
    //{
    //    m_Type = Cartridge::CartridgeColecoVision;
    //    Log("Cartridge is Colecovision. ROM size: %d bytes.", m_iROMSize);
    //}
    //else if (m_bValidROM && (m_iROMSize > 0x8000))
    //{
    //    m_Type = Cartridge::CartridgeActivisionCart;
    //    Log("Cartridge is Activision Cart. ROM size: %d bytes. Banks %d.", m_iROMSize, m_iROMBankCount);
    //}
    //else if (!m_bValidROM && (m_iROMSize > 0x8000))
    //{
    //    headerOffset = m_iROMSize - 0x4000;
    //    header = m_pROM[headerOffset + 1] | (m_pROM[headerOffset + 0] << 8);
    //    m_bValidROM = (header == 0xAA55) || (header == 0x55AA);

    //    if (m_bValidROM)
    //    {
    //        m_Type = Cartridge::CartridgeMegaCart;
    //        Log("Cartridge is Mega Cart. ROM size: %d bytes. Banks %d.", m_iROMSize, m_iROMBankCount);
    //    }
    //}
    //else
    //{
    //    m_Type = Cartridge::CartridgeNotSupported;
    //    Log("ROM is NOT Valid. No header found.");
    //}

    if (m_iROMSize >= 65536)
    {
        if (HasASC16LSignature())
        {
            m_Type = Cartridge::SC3000_ASC16L;
            m_bTypeIdentified = true;
            m_bSF7000IPL = false; // Fondamentale per evitare conflitti con la modalità disco
            m_iROMBankCount = m_iROMSize / 0x4000;
            Log("ASC16L Signature found at 0x3FF0. Mapper activated.");
            return true;
        }
    }
    else
    {
        m_Type = Cartridge::SG1000_16K;
        m_bSF7000IPL = false; // Reset di sicurezza anche qui
    }


    GetInfoFromDB(crc, m_Type);

    // Unknown Taiwanese SG-1000 II dumps can still identify their RAM
    // adapter from the code and image layout. The cartridge leaves an empty
    // $2000-$3FFF ROM region for Type A RAM; MSX-derived code then contains
    // repeated absolute stores either into that window (Type A) or into the
    // expanded work-RAM region above $C000 (Type B). Known CRCs take priority
    // and manual configuration is applied later by ForceConfig().
    if (!m_bTypeIdentified && m_iROMSize >= 0x8000)
    {
        int typeAStores = 0;
        int typeBStores = 0;
        const int scanSize = std::min(m_iROMSize, 0x8000);
        for (int i = 0; i + 2 < scanSize; ++i)
        {
            if (m_pROM[i] != 0x32) // LD (nn),A
                continue;

            const u16 address = static_cast<u16>(m_pROM[i + 1]
                | (static_cast<u16>(m_pROM[i + 2]) << 8));
            switch (address & 0xF000)
            {
                case 0x2000:
                case 0x3000:
                    ++typeAStores;
                    i += 2;
                    break;
                case 0xD000:
                case 0xE000:
                case 0xF000:
                    ++typeBStores;
                    i += 2;
                    break;
                default:
                    break;
            }
        }

        bool emptyAdapterWindow = m_iROMSize >= 0x4000;
        for (int i = 0x2000; emptyAdapterWindow && i < 0x4000; ++i)
            emptyAdapterWindow = m_pROM[i] == 0xFF;

        if (emptyAdapterWindow && (typeAStores > 10 || typeBStores > 10))
        {
            m_Type = typeBStores > typeAStores
                ? Cartridge::SG1000_DAHJEE_TYPE_B
                : Cartridge::SG1000_DAHJEE_TYPE_A;
            m_bTypeIdentified = true;
            m_bValidROM = true;
            m_bSF7000IPL = false;
            Log("Dahjee RAM adapter heuristic: Type %c (%d Type-A stores, %d Type-B stores)",
                m_Type == Cartridge::SG1000_DAHJEE_TYPE_A ? 'A' : 'B',
                typeAStores, typeBStores);
        }
    }

    // "SG16SEGA" at $3FF0: a ROM that declares the 315-5208 and the RAM it
    // needs ($3FFC: 0 = 1 KB, 1 = 2 KB, 2 = 8 KB, 3 = 16 KB). The bank size at
    // $3FFD is a check: the 315-5208 pages 16 KB only (code 3), so anything
    // else is left to a manual choice. Original cartridges (Loretta) have no
    // header and come from the database.
    if (!m_bTypeIdentified && m_iROMSize >= 0x4000
        && std::memcmp(m_pROM + 0x3FF0, "SG16SEGA", 8) == 0)
    {
        static const int kRamBytes[4] = { 0x400, 0x800, 0x2000, 0x4000 };
        const u8 ramCode = m_pROM[0x3FFC];
        const u8 bankCode = m_pROM[0x3FFD];
        if (bankCode != 3 || ramCode > 3)
        {
            Log("SG16 header found at $3FF0 but bank code %d / RAM code %d is not supported: mapper left to a manual choice",
                bankCode, ramCode);
        }
        else
        {
            m_Type = Cartridge::SG1000_SEGA_315_5208;
            m_bTypeIdentified = true;
            m_bValidROM = true;
            m_bSF7000IPL = false;
            m_bSg16Header = true;
            m_iHeaderRamBytes = kRamBytes[ramCode];
            m_iROMBankCount = m_iROMSize / 0x4000;
            Log("SG16 header found at $3FF0: 315-5208 mapper, program needs %d bytes of RAM",
                m_iHeaderRamBytes);
        }
    }

    // Images with the Sega "TMR SEGA" header at $7FF0 run on the mini Sega
    // mapper, whatever the file extension says. Past 48 KB (three pages map
    // straight) they need the page register at $FFFF. They were made for a
    // Mark III, so they get its 8 KiB through the Dahjee B variant rather
    // than whatever RAM the selected console decodes.
    if (m_bDetectSegaHeader && !m_bTypeIdentified
        && m_iROMSize > 0xC000 && (m_iROMSize % 0x4000) == 0
        && std::memcmp(m_pROM + 0x7FF0, "TMR SEGA", 8) == 0)
    {
        m_Type = Cartridge::SG1000_SEGA_315_5208_DAHJEE_B;
        m_bTypeIdentified = true;
        m_bValidROM = true;
        m_bSF7000IPL = false;
        Log("TMR SEGA header found at $7FF0: mini Sega mapper + Dahjee B 8 KB");
    }

    if (m_bSF7000IPL)
    {
        m_Type = Cartridge::SF7000IPL;
        m_bValidROM = true;
        Log("Cartridge is SF-7000 IPL. ROM size: %d bytes.", m_iROMSize);
    }

    switch (m_Type)
    {
        case Cartridge::SF7000IPL:
            Log("SF-7000 IPL found");
            break;
        case Cartridge::SG1000_1K:
            Log("SG-1000 mapper found");
            break;
        case Cartridge::SG1000_16K:
            Log("SG-1000 with 16KB external ram mapper found");
            break;
        case Cartridge::SC3000_2K:
            Log("SC-3000 mapper found");
            break;
        case Cartridge::SC3000_32K:
            Log("SC-3000 with 32K external ram mapper found");
            break;
        case Cartridge::SC3000_ASC16L:
            Log("ASC16L mapper found");
            break;
        case Cartridge::SC3000_16K_AT_8000:
            Log("SC-3000 16K cartridge RAM + console RAM mapper selected");
            break;
        case Cartridge::SC3000_FLAT64K:
            Log("Flat 64K RAM mapper selected");
            break;
        case Cartridge::SG1000_DAHJEE_TYPE_A:
            Log("SG-1000 Dahjee Type A (8K external + console RAM) selected");
            break;
        case Cartridge::SG1000_DAHJEE_TYPE_B:
            Log("SG-1000 Dahjee Type B 8K RAM adapter selected");
            break;
        case Cartridge::SC3000_SURVIVORS_MULTICART:
            Log("SC-3000 Survivors MkII Multicart selected");
            break;
        case Cartridge::SC3000_SURVIVORS_MEGACART:
            Log("SC-3000 Survivors Megacart selected");
            break;
        case Cartridge::SG1000_OTHELLO_2K:
            Log("SG-1000 Othello 2K cartridge RAM selected");
            break;
        case Cartridge::SG1000_CASTLE_8K:
            Log("SG-1000 The Castle 8K cartridge RAM selected");
            break;
        case Cartridge::SG1000_SEGA_315_5208:
            Log("Sega 315-5208 mapper with SG-1000 compatibility selected");
            break;
        case Cartridge::SG1000_SEGA_315_5208_DAHJEE_B:
            Log("Sega 315-5208 mapper on a Dahjee B adapter selected");
            break;
        case Cartridge::SMS_SEGA_MAPPER:
            Log("Sega Master System mapper selected");
            break;
        case Cartridge::SORD_M5_LINEAR:
            Log("Sord M5 standard cartridge selected");
            break;
        case Cartridge::MSX_LINEAR:
            Log("MSX linear cartridge selected");
            break;
        case Cartridge::COLECOVISION:
            Log("ColecoVision standard cartridge selected");
            break;
        case Cartridge::COLECO_MEGACART:
            Log("Coleco MegaCart selected");
            break;
        case Cartridge::CartridgeNotSupported:
            Log("Unknown Cartridge !");
            break;
        default:
            Log("ERROR with cartridge type!!");
            break;
    }

    return (m_Type != CartridgeNotSupported);
}



bool Cartridge::IsTypeIdentified() const
{
    return m_bTypeIdentified;
}

Cartridge::CartridgeTypes Cartridge::DefaultTypeForMachine(
    MachineType machine)
{
    switch (machine)
    {
    case MachineType::SMS:
    case MachineType::GameGear:
        return SMS_SEGA_MAPPER;
    case MachineType::SordM5:
        return SORD_M5_LINEAR;
    case MachineType::MSX:
        return MSX_LINEAR;
    case MachineType::Coleco:
        return COLECOVISION;
    case MachineType::SG1000:
        return SG1000_1K;
    case MachineType::SC3000:
    default:
        return SC3000_2K;
    }
}

bool Cartridge::IsTypeCompatible(CartridgeTypes type, int romSize, const char** reason)
{
    const char* why = NULL;
    bool ok = true;

    switch (type)
    {
        // ROM is read up to $BFFF, so anything past 48 KB does not fit.
        case SG1000_1K:
        case SG1000_16K:
        case SG1000_DAHJEE_TYPE_A:
        case SG1000_DAHJEE_TYPE_B:
            if (romSize > 0xC000) { ok = false; why = "ROM over 48 KB: will not fit the $0000-$BFFF window"; }
            break;

        case SC3000_SURVIVORS_MULTICART:
            if (romSize != 0x200000) { ok = false; why = "SC-3000 Survivors Multicart images must be exactly 2 MB"; }
            break;
        case SC3000_SURVIVORS_MEGACART:
            if (romSize != 0x400000) { ok = false; why = "SC-3000 Survivors Megacart images must be exactly 4 MB"; }
            break;

        case SG1000_OTHELLO_2K:
            if (romSize > 0x8000) { ok = false; why = "Othello cartridge ROM over 32 KB"; }
            break;
        case SG1000_CASTLE_8K:
            if (romSize > 0x8000) { ok = false; why = "The Castle cartridge ROM over 32 KB"; }
            break;

        case SG1000_SEGA_315_5208:
        case SG1000_SEGA_315_5208_DAHJEE_B:
            if (romSize < 0xC000) { ok = false; why = "banked Sega images need at least three 16 KB pages"; }
            else if (romSize % 0x4000) { ok = false; why = "ROM is not a whole number of 16 KB pages"; }
            break;
        case SMS_SEGA_MAPPER:
            if (romSize < 0x4000) { ok = false; why = "SMS images need at least one 16 KB page"; }
            else if (romSize % 0x4000) { ok = false; why = "SMS ROM is not a whole number of 16 KB pages"; }
            break;
        case SORD_M5_LINEAR:
            if (romSize < 1 || romSize > 0x5000)
            { ok = false; why = "Sord M5 standard cartridges require 1..20480 bytes (no expansion RAM)"; }
            break;
        case MSX_LINEAR:
            if (romSize != 0x2000 && romSize != 0x4000 && romSize != 0x8000)
            { ok = false; why = "MSX linear cartridges currently require 8, 16 or 32 KiB"; }
            break;
        case COLECOVISION:
            // Four 8 KB chip selects from the console: nothing above $FFFF.
            if (romSize > 0x8000) { ok = false; why = "ROM over 32 KB: a standard ColecoVision cartridge has four 8 KB selects at $8000-$FFFF"; }
            break;

        case COLECO_MEGACART:
            if (romSize != 0x20000 && romSize != 0x40000
                && romSize != 0x80000 && romSize != 0x100000)
            { ok = false; why = "MegaCart requires 128, 256, 512 or 1024 KiB"; }
            break;

        // Read up to $7FFF; everything above is RAM.
        case SC3000_2K:
        case SC3000_32K:
            if (romSize > 0x8000) { ok = false; why = "ROM over 32 KB: will not fit the $0000-$7FFF window"; }
            break;

        // Pages 0-1 fixed (32 KB) plus one banked 16 KB window, so at least
        // 48 KB is needed. The bank count need not be a power of two - the
        // mapper wraps with modulo - but the size must be a whole number of
        // 16 KB banks, or the last bank is short and reads past the image.
        case SC3000_ASC16L:
            if (romSize < 0xC000) { ok = false; why = "ROM under 48 KB: ASC16 needs 32 KB fixed plus one bank"; }
            else if (romSize % 0x4000) { ok = false; why = "ROM is not a whole number of 16 KB banks: the last one would be short"; }
            break;
        case SC3000_ASC16LS:
            if (romSize != ASC16LSFormat::ROMSize / 2
                && romSize != ASC16LSFormat::ROMSize / 4
                && romSize != ASC16LSFormat::ROMSize
                && romSize != ASC16LSFormat::HeaderSize + ASC16LSFormat::ROMSize
                && romSize != ASC16LSFormat::HeaderSize + ASC16LSFormat::ROMSize / 2)
            { ok = false; why = "Internal requires a 128/256/512 KB SCLS package"; }
            break;

        // ROM ends where the RAM begins, at $8000.
        case SC3000_16K_AT_8000:
            if (romSize > 0x8000) { ok = false; why = "ROM over 32 KB: cartridge RAM starts at $8000"; }
            break;

        // The image is poured into the 64 KB: the only limit is not exceeding them.
        case SC3000_FLAT64K:
            if (romSize > 0x10000) { ok = false; why = "image over 64 KB: will not fit the flat map"; }
            break;

        // Not hand-selectable: the IPL is a role rather than a mapper, and
        // NotSupported means "you decide", which is auto.
        case SF7000IPL:
        case CartridgeNotSupported:
        default:
            ok = false;
            why = "not selectable by hand";
            break;
    }

    if (reason)
        *reason = ok ? NULL : why;
    return ok;
}

void Cartridge::GetMapperDescr(char *outstr) 
{
    switch (m_Type)
    {
    case Cartridge::SF7000IPL:
        strcpy(outstr, "SF-7000");
        break;
    case Cartridge::SG1000_1K:
        strcpy(outstr, "SG-1000 standard (console RAM)");
        break;
    case Cartridge::SG1000_16K:
        strcpy(outstr, "SG-1000 (16KB)");
        break;
    case Cartridge::SC3000_2K:
        strcpy(outstr, "SC-3000 standard (console RAM)");
        break;
    case Cartridge::SC3000_32K:
        strcpy(outstr, "SC-3000 (32KB)");
        break;
    case Cartridge::SC3000_ASC16L:
        strcpy(outstr, "ASC16L");
        break;
    case Cartridge::SC3000_ASC16LS:
        strcpy(outstr, "Internal");
        break;
    case Cartridge::SC3000_16K_AT_8000:
        strcpy(outstr, "SC-3000 (16K + console RAM)");
        break;
    case Cartridge::SC3000_FLAT64K:
        strcpy(outstr, "Flat 64K");
        break;
    case Cartridge::SG1000_DAHJEE_TYPE_A:
        strcpy(outstr, "SG-1000 Dahjee A (8K + console RAM)");
        break;
    case Cartridge::SG1000_DAHJEE_TYPE_B:
        strcpy(outstr, "SG-1000 Dahjee B (8K)");
        break;
    case Cartridge::SC3000_SURVIVORS_MULTICART:
        strcpy(outstr, "SC-3000 Survivors Multicart (2MB)");
        break;
    case Cartridge::SC3000_SURVIVORS_MEGACART:
        strcpy(outstr, "SC-3000 Survivors Megacart (4MB)");
        break;
    case Cartridge::SG1000_OTHELLO_2K:
        strcpy(outstr, "SG-1000 Othello (2K cart RAM)");
        break;
    case Cartridge::SG1000_CASTLE_8K:
        strcpy(outstr, "SG-1000 The Castle (8K cart + console RAM)");
        break;
    case Cartridge::SG1000_SEGA_315_5208:
        strcpy(outstr, "Sega 315-5208 (SG-1000 compatibility)");
        break;
    case Cartridge::SG1000_SEGA_315_5208_DAHJEE_B:
        strcpy(outstr, "Sega 315-5208 + Dahjee B (8K)");
        break;
    case Cartridge::SMS_SEGA_MAPPER:
        strcpy(outstr, "Sega Master System");
        break;
    case Cartridge::SORD_M5_LINEAR:
        strcpy(outstr, "Sord M5 standard");
        break;
    case Cartridge::MSX_LINEAR:
        strcpy(outstr, "MSX linear");
        break;
    case Cartridge::COLECOVISION:
        strcpy(outstr, "ColecoVision");
        break;
    case Cartridge::COLECO_MEGACART:
        strcpy(outstr, "Coleco MegaCart");
        break;
    case Cartridge::CartridgeNotSupported:
        strcpy(outstr, "Unknown");
        break;
    default:
        strcpy(outstr, "ERROR");
        break;
    }
}

void Cartridge::GetInfoFromDB(u32 crc, CartridgeTypes &type)
{
    int i = 0;
    bool found = false;
    m_bSF7000IPL = false;

    while(!found && (kGameDatabase[i].title != 0))
    {
        u32 db_crc = kGameDatabase[i].crc;

        if (db_crc == crc)
        {
            found = true;
            m_bTypeIdentified = true;

            Log("ROM found in database: %s. CRC: %X", kGameDatabase[i].title, crc);

            //if (kGameDatabase[i].mode & GC_GameDBMode_SRAM)
            //{
            //    Log("Cartridge with SRAM");
            //    m_bSRAM = true;
            //}

            if (kGameDatabase[i].mode & GC_GameDBMode_SG1000_DAHJEE_A)
            {
                type = CartridgeTypes::SG1000_DAHJEE_TYPE_A;
            }
            else if (kGameDatabase[i].mode & GC_GameDBMode_SG1000_DAHJEE_B)
            {
                type = CartridgeTypes::SG1000_DAHJEE_TYPE_B;
            }
            else if (kGameDatabase[i].mode & GC_GameDBMode_SC3000_SURVIVORS_MULTICART)
            {
                type = CartridgeTypes::SC3000_SURVIVORS_MULTICART;
            }
            else if (kGameDatabase[i].mode & GC_GameDBMode_SC3000_SURVIVORS_MEGACART)
            {
                type = CartridgeTypes::SC3000_SURVIVORS_MEGACART;
            }
            else if (kGameDatabase[i].mode & GC_GameDBMode_SG1000_OTHELLO_2K)
            {
                type = CartridgeTypes::SG1000_OTHELLO_2K;
            }
            else if (kGameDatabase[i].mode & GC_GameDBMode_SG1000_SEGA_315_5208)
            {
                type = CartridgeTypes::SG1000_SEGA_315_5208;
            }
            else if (kGameDatabase[i].mode & GC_GameDBMode_SG1000_CASTLE_8K)
            {
                type = CartridgeTypes::SG1000_CASTLE_8K;
            }
            else if (kGameDatabase[i].mode & GC_GameDBMode_SG1000)
            {
                type = CartridgeTypes::SG1000_1K;
            }
            else if (kGameDatabase[i].mode & GC_GameDBMode_SG1000_16K)
            {
                type = CartridgeTypes::SG1000_16K;
            }
            else if (kGameDatabase[i].mode & GC_GameDBMode_SC3000)
            {
                type = CartridgeTypes::SC3000_2K;
            }
            else if (kGameDatabase[i].mode & GC_GameDBMode_SC3000_32K)
            {
                type = CartridgeTypes::SC3000_32K;
            }
            else if (kGameDatabase[i].mode & GC_GameDBMode_SF7000)
            {
                m_bSF7000IPL = true;
                type = CartridgeTypes::SF7000IPL;
            }
            else
            {
                type = CartridgeTypes::SG1000_16K;
                m_bSF7000IPL = false;
            }
        }
        else
            i++;
    }

    if (!found)
    {
        Log("ROM not found in database. CRC: %X", crc);
    }
}
