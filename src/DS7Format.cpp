/*
** DS7Format.cpp — DS7 loader and saver implementation
*/
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "DS7Format.h"
#include "DiskLayout.h"
#include <stdlib.h>
#include <string.h>

static const char DS7_MAGIC[8] = { 'G','E','A','R','S','F','7','\0' };
#define DS7_VERSION 1

/* =========================================================
 * probe
 * ========================================================= */
bool DS7Format::probe(FILE* f)
{
    char magic[8];
    fseek(f, 0, SEEK_SET);
    if (fread(magic, 1, 8, f) != 8) return false;
    return (memcmp(magic, DS7_MAGIC, 8) == 0);
}

/* =========================================================
 * load
 * ========================================================= */
bool DS7Format::load(FILE* f, DISC_DRIVE* drv)
{
    fseek(f, 0, SEEK_SET);

    /* --- File Header --- */
    uint8_t hdr[32];
    if (fread(hdr, 1, 32, f) != 32) return false;
    if (memcmp(hdr, DS7_MAGIC, 8) != 0) return false;
    if (hdr[8] != DS7_VERSION) return false;

    int      tracks   = hdr[9];
    int      sides    = hdr[10];
    uint8_t  encoding = hdr[11];
    uint16_t bitrate  = (uint16_t)(hdr[12] | (hdr[13] << 8));
    uint16_t rpm      = (uint16_t)(hdr[14] | (hdr[15] << 8));

    if (tracks <= 0 || tracks > MAX_TRACKS) return false;
    if (sides  <= 0 || sides  > MAX_SIDES)  return false;
    if (rpm    == 0) rpm     = SF7_RPM;
    if (bitrate== 0) bitrate = SF7_BITRATE_KBPS;

    drv->tracks      = tracks;
    drv->sides       = sides;
    drv->encoding    = encoding;
    drv->bitrateKbps = bitrate;
    drv->rpm         = rpm;

    drv->bitsPerRevolution   = (uint32_t)bitrate * 1000u * 2u * 60u / (uint32_t)rpm;
    drv->clocksPerRevolution = 3579545u * 60u / (uint32_t)rpm;
    drv->clocksPerBit        = drv->clocksPerRevolution / drv->bitsPerRevolution;

    /* --- Track Table --- */
    int numEntries = tracks * sides;
    uint8_t* table = (uint8_t*)malloc((size_t)numEntries * 8);
    if (!table) return false;
    if (fread(table, 1, (size_t)numEntries * 8, f) != (size_t)numEntries * 8) {
        free(table);
        return false;
    }

    /* --- Track Data --- */
    for (int side = 0; side < sides; side++) {
        for (int track = 0; track < tracks; track++) {
            int entry = side * tracks + track;
            const uint8_t* te = table + entry * 8;

            if (te[0] == 0) {
                drv->trackMap[side][track] = NULL;
                continue;
            }

            int     sectorCount = te[1];
            uint8_t gap3        = te[2];
            uint8_t gap4a       = te[3];
            uint8_t gap1        = te[4];
            uint8_t gap2        = te[5];
            uint8_t fillerByte  = te[6];

            DiskTrack* t = (DiskTrack*)calloc(1, sizeof(DiskTrack));
            if (!t) { free(table); return false; }

            t->sectorCount  = sectorCount;
            t->totalBits    = drv->bitsPerRevolution;
            t->encoding     = encoding;
            t->bitrateKbps  = bitrate;
            t->gap4a        = gap4a;
            t->gap1         = gap1;
            t->gap2         = gap2;
            t->gap3         = gap3;
            t->fillerByte   = fillerByte;
            t->rawMFM       = NULL;   /* DS7 never stores raw MFM */

            /* Read sector headers */
            for (int s = 0; s < sectorCount && s < MAX_SECTORS; s++) {
                uint8_t sh[12];
                if (fread(sh, 1, 12, f) != 12) { free(table); return false; }
                DiskSector* sec = &t->sectors[s];
                sec->C       = sh[0];
                sec->H       = sh[1];
                sec->R       = sh[2];
                sec->N       = sh[3];
                sec->ST1     = sh[4];
                sec->ST2     = sh[5];
                sec->damType = sh[6];
                sec->_pad    = 0;
                sec->dataLen = (int)(sh[8] | (sh[9] << 8));
            }

            /* Read sector data */
            for (int s = 0; s < sectorCount && s < MAX_SECTORS; s++) {
                DiskSector* sec = &t->sectors[s];
                if (sec->dataLen <= 0) continue;
                sec->data = (uint8_t*)malloc(sec->dataLen);
                if (!sec->data) { free(table); return false; }
                size_t rd = fread(sec->data, 1, sec->dataLen, f);
                if ((int)rd < sec->dataLen)
                    memset(sec->data + rd, 0, sec->dataLen - (int)rd);
            }

            /* Synthesize bit positions from gap parameters */
            synthesizeSectorPositions(t);

            drv->trackMap[side][track] = t;
        }
    }

    free(table);
    return true;
}

/* =========================================================
 * save
 * ========================================================= */
bool DS7Format::save(const DISC_DRIVE* drv, const char* fileName)
{
    FILE* f = fopen(fileName, "wb");
    if (!f) return false;

    int tracks = drv->tracks;
    int sides  = drv->sides;
    if (tracks <= 0) tracks = 1;
    if (sides  <= 0) sides  = 1;

    /* --- File Header --- */
    uint8_t hdr[32];
    memset(hdr, 0, 32);
    memcpy(hdr, DS7_MAGIC, 8);
    hdr[8]  = DS7_VERSION;
    hdr[9]  = (uint8_t)tracks;
    hdr[10] = (uint8_t)sides;
    hdr[11] = drv->encoding;
    hdr[12] = (uint8_t)(drv->bitrateKbps & 0xFF);
    hdr[13] = (uint8_t)(drv->bitrateKbps >> 8);
    hdr[14] = (uint8_t)(drv->rpm & 0xFF);
    hdr[15] = (uint8_t)(drv->rpm >> 8);
    fwrite(hdr, 1, 32, f);

    /* --- Track Table --- */
    for (int side = 0; side < sides; side++) {
        for (int track = 0; track < tracks; track++) {
            const DiskTrack* t = drv->trackMap[side][track];
            uint8_t te[8] = {0};
            if (t) {
                te[0] = 1;               /* present */
                te[1] = (uint8_t)t->sectorCount;
                te[2] = t->gap3;
                te[3] = t->gap4a;
                te[4] = t->gap1;
                te[5] = t->gap2;
                te[6] = t->fillerByte;
                te[7] = 0;
            }
            fwrite(te, 1, 8, f);
        }
    }

    /* --- Track Data --- */
    for (int side = 0; side < sides; side++) {
        for (int track = 0; track < tracks; track++) {
            const DiskTrack* t = drv->trackMap[side][track];
            if (!t) continue;

            /* Sector headers */
            for (int s = 0; s < t->sectorCount && s < MAX_SECTORS; s++) {
                const DiskSector* sec = &t->sectors[s];
                uint8_t sh[12] = {0};
                sh[0] = sec->C;
                sh[1] = sec->H;
                sh[2] = sec->R;
                sh[3] = sec->N;
                sh[4] = sec->ST1;
                sh[5] = sec->ST2;
                sh[6] = sec->damType;
                sh[7] = 0;
                sh[8] = (uint8_t)(sec->dataLen & 0xFF);
                sh[9] = (uint8_t)(sec->dataLen >> 8);
                fwrite(sh, 1, 12, f);
            }

            /* Sector data */
            for (int s = 0; s < t->sectorCount && s < MAX_SECTORS; s++) {
                const DiskSector* sec = &t->sectors[s];
                if (sec->data && sec->dataLen > 0)
                    fwrite(sec->data, 1, sec->dataLen, f);
            }
        }
    }

    fclose(f);
    return true;
}

/* =========================================================
 * makeDS7Name
 * Replace the extension of sourceFileName with ".ds7".
 * ========================================================= */
bool DS7Format::makeDS7Name(const char* sourceFileName,
                             char* outBuf, int outBufLen)
{
    if (!sourceFileName || !outBuf || outBufLen < 8) return false;

    /* Find the last dot (but not in the directory component) */
    const char* lastDot   = NULL;
    const char* lastSlash = NULL;
    for (const char* p = sourceFileName; *p; p++) {
        if (*p == '.' ) lastDot   = p;
        if (*p == '/' || *p == '\\') lastSlash = p;
    }

    /* Ensure the dot is after the last directory separator */
    if (lastSlash && lastDot && lastDot < lastSlash)
        lastDot = NULL;

    int prefixLen = lastDot
                    ? (int)(lastDot - sourceFileName)
                    : (int)strlen(sourceFileName);

    if (prefixLen + 4 + 1 > outBufLen) return false;

    memcpy(outBuf, sourceFileName, prefixLen);
    memcpy(outBuf + prefixLen, ".ds7", 5); /* includes '\0' */
    return true;
}
