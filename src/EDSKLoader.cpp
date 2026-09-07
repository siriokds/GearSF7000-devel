/*
** EDSKLoader.cpp — Extended DSK / Standard DSK loader implementation
*/
#include "EDSKLoader.h"
#include "DiskLayout.h"
#include <stdlib.h>
#include <string.h>

static const char SIG_EXTENDED[] = "EXTENDED CPC DSK File\r\nDisk-Info\r\n";  /* 34 bytes */
static const char SIG_STANDARD[] = "MV - CPC";                                  /* 8 bytes */

/* =========================================================
 * probe
 * ========================================================= */
bool EDSKLoader::probe(FILE* f)
{
    char sig[34] = {0};
    fseek(f, 0, SEEK_SET);
    fread(sig, 1, 34, f);
    return (memcmp(sig, SIG_EXTENDED, 34) == 0 ||
            memcmp(sig, SIG_STANDARD,  8) == 0);
}

/* =========================================================
 * load
 * ========================================================= */
bool EDSKLoader::load(FILE* f, DISC_DRIVE* drv)
{
    fseek(f, 0, SEEK_SET);
    uint8_t header[256];
    if (fread(header, 1, 256, f) != 256) return false;

    int tracks = header[0x30];
    int sides  = header[0x31];
    if (tracks <= 0 || tracks > MAX_TRACKS) return false;
    if (sides  <= 0 || sides  > MAX_SIDES)  return false;

    drv->sides  = sides;
    drv->tracks = tracks;

    if (memcmp(header, SIG_EXTENDED, 34) == 0) {
        return loadExtended(f, drv, tracks, sides, &header[0x34]);
    } else {
        uint16_t trackSize = (uint16_t)(header[0x32] | (header[0x33] << 8));
        return loadStandard(f, drv, tracks, sides, trackSize);
    }
}

/* =========================================================
 * loadExtended
 * ========================================================= */
bool EDSKLoader::loadExtended(FILE* f, DISC_DRIVE* drv,
                               int tracks, int sides,
                               const uint8_t* sizeTable)
{
    long dataOffset = 256;  /* byte offset after the 256-byte disk header */

    for (int track = 0; track < tracks; track++) {
        for (int side = 0; side < sides; side++) {
            int    tableIdx   = track * sides + side;
            uint8_t entry     = sizeTable[tableIdx];

            if (entry == 0x00) {
                drv->trackMap[side][track] = NULL;
                continue;
            }

            uint32_t trackBytes = (uint32_t)entry * 256;
            fseek(f, dataOffset, SEEK_SET);

            DiskTrack* t = parseTrackBlock(f, drv->bitsPerRevolution);
            drv->trackMap[side][track] = t;

            dataOffset += (long)trackBytes;
        }
    }
    return true;
}

/* =========================================================
 * loadStandard
 * ========================================================= */
bool EDSKLoader::loadStandard(FILE* f, DISC_DRIVE* drv,
                               int tracks, int sides, uint16_t trackSize)
{
    long dataOffset = 256;

    for (int track = 0; track < tracks; track++) {
        for (int side = 0; side < sides; side++) {
            fseek(f, dataOffset, SEEK_SET);
            DiskTrack* t = parseTrackBlock(f, drv->bitsPerRevolution);
            drv->trackMap[side][track] = t;
            dataOffset += (long)trackSize;
        }
    }
    return true;
}

/* =========================================================
 * parseTrackBlock
 *
 * Reads a 256-byte Track Info Block and the sector data that follows.
 * File position must be at the start of the Track Info Block.
 * ========================================================= */
DiskTrack* EDSKLoader::parseTrackBlock(FILE* f, uint32_t bitsPerRev)
{
    uint8_t tib[256];
    long    trackStart = ftell(f);

    if (fread(tib, 1, 256, f) != 256) return NULL;

    /* Check magic */
    if (memcmp(tib, "Track-Info\r\n", 12) != 0) return NULL;

    int     numSectors = tib[0x15];
    uint8_t gap3       = tib[0x16];
    uint8_t filler     = tib[0x17];

    if (numSectors < 0 || numSectors > MAX_SECTORS) return NULL;

    DiskTrack* t    = (DiskTrack*)calloc(1, sizeof(DiskTrack));
    if (!t) return NULL;

    t->sectorCount  = numSectors;
    t->totalBits    = (bitsPerRev > 0) ? bitsPerRev : (uint32_t)SF7_BITS_PER_REV;
    t->encoding     = 0;            /* EDSK presuppone MFM per SF-7000/CPC */
    t->bitrateKbps  = SF7_BITRATE_KBPS;
    /* Use SF-7000 preamble constants when the track matches SF-7000 geometry
     * (16 sectors per track, N=1 = 256-byte sectors); otherwise use generic
     * MFM IBM-format defaults. */
    {
        bool isSF7Track = (numSectors == SF7_SECTORS_PER_TRACK
                           && tib[0x18 + 3] == SF7_SECTOR_N);  /* N of sector 0 */
        t->gap4a = isSF7Track ? SF7_GAP4A : MFM_GAP4A;
        t->gap1  = isSF7Track ? SF7_GAP1  : MFM_GAP1;
    }
    t->gap2         = MFM_GAP2;
    t->gap3         = gap3;         /* real value from EDSK track header */
    t->fillerByte   = filler;
    t->rawMFM       = NULL;         /* EDSK never carries raw MFM */

    /* Parse Sector Info Records (8 bytes each, starting at TIB+0x18) */
    for (int s = 0; s < numSectors; s++) {
        const uint8_t* sib   = &tib[0x18 + s * 8];
        DiskSector*    sec   = &t->sectors[s];

        sec->C       = sib[0];
        sec->H       = sib[1];
        sec->R       = sib[2];
        sec->N       = sib[3];
        sec->ST1     = sib[4];   /* pre-baked error from protection */
        sec->ST2     = sib[5];
        sec->damType = 0xFB;

        /* ST2 bit 6 (CM) = control mark = deleted data */
        if (sec->ST2 & 0x40)
            sec->damType = 0xF8;

        /* Extended EDSK: data length in bytes 6-7 of SIR.
         * Standard EDSK has these bytes as 0x00, meaning use 128<<N. */
        uint16_t dl  = (uint16_t)(sib[6] | (sib[7] << 8));
        sec->dataLen = (dl > 0) ? (int)dl : (128 << sec->N);
    }

    /* Sector data immediately follows the 256-byte TIB */
    fseek(f, trackStart + 256, SEEK_SET);
    for (int s = 0; s < numSectors; s++) {
        DiskSector* sec = &t->sectors[s];
        sec->data = (uint8_t*)malloc(sec->dataLen);
        if (sec->data) {
            size_t rd = fread(sec->data, 1, sec->dataLen, f);
            if ((int)rd < sec->dataLen)
                memset(sec->data + rd, 0, sec->dataLen - (int)rd);
        }
    }

    /* Compute synthesized bit positions for all sectors */
    synthesizeSectorPositions(t);

    /* Note: if t->usedBits > t->totalBits the protection intentionally
     * used a very small GAP3.  We record it but do not error out. */

    return t;
}
