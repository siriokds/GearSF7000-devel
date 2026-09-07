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
#include "Cartridge.h"
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
    m_bPAL = false;
    m_bSRAM = false;
    m_bSF7000IPL = false;
    m_iCRC = 0;
}

Cartridge::~Cartridge()
{
    SafeDeleteArray(m_pROM);
}

void Cartridge::Init()
{
    Reset();
}

void Cartridge::Reset()
{
    SafeDeleteArray(m_pROM);
    m_iROMSize = 0;
    m_Type = CartridgeNotSupported;
    m_bTypeIdentified = false;
    m_bValidROM = false;
    m_bReady = false;
    m_szFilePath[0] = 0;
    m_szFileName[0] = 0;
    m_iROMBankCount = 0;
    m_bPAL = false;
    m_bSRAM = false;
    m_iCRC = 0;
}

u32 Cartridge::GetCRC() const
{
    return m_iCRC;
}

bool Cartridge::IsPAL() const
{
    return m_bPAL;
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

Cartridge::CartridgeTypes Cartridge::GetType() const
{
    return m_Type;
}

void Cartridge::ForceConfig(Cartridge::ForceConfiguration config)
{
    m_iCRC = CalculateCRC32(0, m_pROM, m_iROMSize);
    GatherMetadata(m_iCRC);

    if (config.region == CartridgePAL)
    {
        Log("Forcing Region: PAL");
        m_bPAL = true;
    }
    else if (config.region == CartridgeNTSC)
    {
        Log("Forcing Region: NTSC");
        m_bPAL = false;
    }

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
            Log("Forcing Mapper: SC-3000 (2K)");
            break;
        case Cartridge::SC3000_32K:
            m_Type = config.type;
            Log("Forcing Mapper: SC-3000 (32K)");
            break;
        case Cartridge::SC3000_16K_AT_8000:
            m_Type = config.type;
            Log("Forcing Mapper: SC-3000 (16K cartridge RAM + 2K internal)");
            break;
        case Cartridge::SC3000_FLAT64K:
            m_Type = config.type;
            // No validity check at all: that is the point of this mode.
            m_bValidROM = true;
            m_bSF7000IPL = false;
            Log("Forcing Mapper: flat 64K RAM");
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
            (extension == "asc") || (extension == "rom") ||
            (extension == "bin");
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
    m_bPAL = false;
    m_bSRAM = false;
    // Cleared here and not only in Init()/Reset(): ForceConfig() calls this
    // again, so a stale flag from a previous image would make an unidentified
    // ROM look identified and skip the fallback.
    m_bTypeIdentified = false;

    Log("ROM Size: %d KB", m_iROMSize / 1024);

    m_iROMBankCount = (m_iROMSize / 0x4000) + (m_iROMSize % 0x4000 ? 1 : 0);

    Log("ROM Bank Count: %d", m_iROMBankCount);

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
        if (m_pROM[0x3FF0] == 'A' && m_pROM[0x3FF1] == 'S' &&
            m_pROM[0x3FF2] == '1' && m_pROM[0x3FF3] == '6')
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
            Log("SC-3000 16K cartridge RAM + 2K internal mapper selected");
            break;
        case Cartridge::SC3000_FLAT64K:
            Log("Flat 64K RAM mapper selected");
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

bool Cartridge::IsTypeCompatible(CartridgeTypes type, int romSize, const char** reason)
{
    const char* why = NULL;
    bool ok = true;

    switch (type)
    {
        // ROM is read up to $BFFF, so anything past 48 KB does not fit.
        case SG1000_1K:
        case SG1000_16K:
            if (romSize > 0xC000) { ok = false; why = "ROM over 48 KB: will not fit the $0000-$BFFF window"; }
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
        strcpy(outstr, "SG-1000 (1KB)");
        break;
    case Cartridge::SG1000_16K:
        strcpy(outstr, "SG-1000 (16KB)");
        break;
    case Cartridge::SC3000_2K:
        strcpy(outstr, "SC-3000 (2KB)");
        break;
    case Cartridge::SC3000_32K:
        strcpy(outstr, "SC-3000 (32KB)");
        break;
    case Cartridge::SC3000_ASC16L:
        strcpy(outstr, "ASC16L");
        break;
    case Cartridge::SC3000_16K_AT_8000:
        strcpy(outstr, "SC-3000 (16K+2K)");
        break;
    case Cartridge::SC3000_FLAT64K:
        strcpy(outstr, "Flat 64K");
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

            if (kGameDatabase[i].mode & GC_GameDBMode_SG1000)
            {
                type = CartridgeTypes::SG1000_1K;
            }
            if (kGameDatabase[i].mode & GC_GameDBMode_SG1000_16K)
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
