/*
** Disk.cpp — FDC disk subsystem for GearSF7000
**
** Replaces the original blueMSX flat-sector implementation with a
** Physical Track Model that supports SF7, EDSK, HFE, and DS7 formats.
**
** Key changes from the original:
**   - DISC_DRIVE now holds a DiskTrack* trackMap[2][85] per-track map.
**   - diskReadSector searches by sector ID (R value), not linear offset.
**   - diskChange detects the format and calls the appropriate loader.
**   - On HFE load, a .ds7 sidecar is saved automatically.
**   - On eject/disk-change with dirty==1, the model is auto-saved as .ds7.
**   - diskGetTrack / diskGetRotationConstants exposed for NEC765.
*/
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS   /* suppress MSVC deprecated CRT warnings */
#endif
#include "Disk.h"
#include "DiskModel.h"
#include "DiskLayout.h"
#include "EDSKLoader.h"
#include "HFELoader.h"
#include "DS7Format.h"
#include "NEC765.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#define STRCASECMP _stricmp
#else
#include <strings.h>
#define STRCASECMP strcasecmp
#endif

/* =========================================================
 * Global drive array
 * ========================================================= */
static DISC_DRIVE diskDrives[MAXDRIVES];

/* =========================================================
 * Free all DiskTrack/DiskSector memory for a drive
 * ========================================================= */
static void diskFreeModel(DISC_DRIVE* drv)
{
    for (int side = 0; side < MAX_SIDES; side++) {
        for (int track = 0; track < MAX_TRACKS; track++) {
            DiskTrack* t = drv->trackMap[side][track];
            if (!t) continue;
            for (int s = 0; s < t->sectorCount; s++)
                free(t->sectors[s].data);
            free(t->rawMFM);
            free(t);
            drv->trackMap[side][track] = NULL;
        }
    }
}

/* =========================================================
 * Set drive constants to SF-7000 defaults
 * ========================================================= */
static void diskSetSF7Defaults(DISC_DRIVE* drv)
{
    drv->encoding              = 0;
    drv->bitrateKbps           = SF7_BITRATE_KBPS;
    drv->rpm                   = SF7_RPM;
    drv->bitsPerRevolution     = SF7_BITS_PER_REV;
    drv->clocksPerRevolution   = SF7_CLOCKS_PER_REV;
    drv->clocksPerBit          = SF7_CLOCKS_PER_BIT;
}

/* =========================================================
 * SF7 flat image loader
 * ========================================================= */

/* Detect geometry of an SF7 flat image from file size and boot sector.
 * Returns true if recognised.  All OUT params are set.
 * Reads directly from the file (no trackMap involved). */
static bool detectSF7Geometry(FILE* f, long fileSize,
                               int* tracksOut, int* sidesOut,
                               int* sptOut, int* sectorSizeOut)
{
    /* Defaults for unknown geometry */
    *tracksOut     = 80;
    *sidesOut      = 2;
    *sptOut        = 9;
    *sectorSizeOut = 512;

    switch (fileSize) {
    case 163840:
        /* Could be 40T/1S/16S/256B (SF-7000) or 40T/1S/8S/512B (FAT/DOS).
         * DOS/FAT disks ALWAYS start with a JMP instruction (0xEB nn or 0xE9 nn nn).
         * SF-7000 disks start with "SYS:", executable code, or other content —
         * never with 0xEB/0xE9 as their first byte. */
        {
            uint8_t first = 0;
            fseek(f, 0, SEEK_SET);
            fread(&first, 1, 1, f);
            if (first == 0xEB || first == 0xE9) {
                *tracksOut = 40; *sidesOut = 1; *sptOut = 8;  *sectorSizeOut = 512;
            } else {
                *tracksOut = 40; *sidesOut = 1; *sptOut = 16; *sectorSizeOut = 256;
            }
        }
        return true;

    case 184320:   /* BW-12 SSDD */
        *tracksOut = 40; *sidesOut = 1; *sptOut = 18; *sectorSizeOut = 256;
        return true;

    case 327680:
        *tracksOut = 80; *sidesOut = 1; *sptOut = 8;  *sectorSizeOut = 512;
        return true;

    case 348160:   /* SVI-728 DSDD CP/M */
        *tracksOut = 40; *sidesOut = 2; *sptOut = 17; *sectorSizeOut = 256;
        return true;

    case 368640:
        *tracksOut = 80; *sidesOut = 1; *sptOut = 9;  *sectorSizeOut = 512;
        return true;

    case 655360:
        *tracksOut = 80; *sidesOut = 2; *sptOut = 8;  *sectorSizeOut = 512;
        return true;

    case 737280:
        *tracksOut = 80; *sidesOut = 2; *sptOut = 9;  *sectorSizeOut = 512;
        return true;

    default:
        /* Try to read geometry from boot sector (FAT BPB or media descriptor) */
        {
            uint8_t buf[512] = {0};
            fseek(f, 0, SEEK_SET);
            if (fread(buf, 1, 512, f) != 512) return false;

            if (buf[0] == 0xEB || buf[0] == 0xE9) {
                /* BPB present: sectors per track and heads at standard offsets */
                int spt   = buf[0x18] | (buf[0x19] << 8);
                int heads = buf[0x1A] | (buf[0x1B] << 8);
                if (spt > 0 && spt <= 36 && heads >= 1 && heads <= 2) {
                    *sptOut        = spt;
                    *sidesOut      = heads;
                    *sectorSizeOut = 512;
                    long expected  = (long)80 * heads * spt * 512;
                    *tracksOut     = (fileSize == expected) ? 80 : 40;
                    return true;
                }
            }
        }
        return false;
    }
}

/* Build DiskTrack objects from an SF7 flat image using detected geometry */
static bool loadSF7Flat(FILE* f, DISC_DRIVE* drv,
                         int tracks, int sides, int spt, int sectorSize)
{
    uint8_t N = (uint8_t)(sectorSize == 128 ? 0 :
                          sectorSize == 256 ? 1 :
                          sectorSize == 512 ? 2 : 3);

    /* For standard SF-7000 geometry, use the exact GPL from the IPL.
     * For other geometries, compute an optimal GAP3. */
    uint8_t gap3;
    if (spt == SF7_SECTORS_PER_TRACK && sectorSize == SF7_SECTOR_SIZE) {
        gap3 = SF7_FORMAT_GPL;
    } else {
        /* Compute optimal GAP3 that distributes the slack evenly */
        uint32_t preambleBits = (uint32_t)(MFM_GAP4A + MFM_IAM_SYNC
                                           + MFM_IAM_MARK + MFM_GAP1)
                                * MFM_BITS_PER_BYTE;
        uint32_t perSecBase   = (uint32_t)(MFM_IDAM_SYNC + MFM_IDAM_MARK
                                           + MFM_CHRN + MFM_CRC + MFM_GAP2
                                           + MFM_DAM_SYNC + MFM_DAM_MARK
                                           + sectorSize + MFM_DATA_CRC)
                                * MFM_BITS_PER_BYTE;
        int32_t slack = (int32_t)drv->bitsPerRevolution
                      - (int32_t)preambleBits
                      - (int32_t)(perSecBase * spt);
        int g = (slack > 0) ? (int)(slack / spt / MFM_BITS_PER_BYTE) : 1;
        gap3  = (uint8_t)(g > 255 ? 255 : (g < 1 ? 1 : g));
    }

    for (int track = 0; track < tracks && track < MAX_TRACKS; track++) {
        for (int side = 0; side < sides && side < MAX_SIDES; side++) {
            DiskTrack* t = (DiskTrack*)calloc(1, sizeof(DiskTrack));
            if (!t) return false;

            t->sectorCount  = spt;
            t->totalBits    = drv->bitsPerRevolution;
            t->encoding     = 0;
            t->bitrateKbps  = drv->bitrateKbps;
            /* Use SF-7000-specific preamble constants for native geometry;
             * fall back to generic MFM defaults for non-standard geometries. */
            bool isSF7Geom = (spt == SF7_SECTORS_PER_TRACK && sectorSize == SF7_SECTOR_SIZE);
            t->gap4a        = isSF7Geom ? SF7_GAP4A : MFM_GAP4A;
            t->gap1         = isSF7Geom ? SF7_GAP1  : MFM_GAP1;
            t->gap2         = MFM_GAP2;
            t->gap3         = gap3;
            t->fillerByte   = SF7_FILLER;
            t->rawMFM       = NULL;

            for (int s = 0; s < spt; s++) {
                DiskSector* sec = &t->sectors[s];
                sec->C       = (uint8_t)track;
                sec->H       = (uint8_t)side;
                sec->R       = (uint8_t)(s + 1);   /* SF-7000: sectors 1-based */
                sec->N       = N;
                sec->ST1     = 0x00;
                sec->ST2     = 0x00;
                sec->damType = 0xFB;
                sec->dataLen = sectorSize;
                sec->data    = (uint8_t*)malloc(sectorSize);
                if (!sec->data) { free(t); return false; }

                long offset = (long)(s + spt * (track * sides + side)) * sectorSize;
                fseek(f, offset, SEEK_SET);
                size_t rd = fread(sec->data, 1, sectorSize, f);
                if ((int)rd < sectorSize)
                    memset(sec->data + rd, 0, sectorSize - (int)rd);
            }

            synthesizeSectorPositions(t);
            drv->trackMap[side][track] = t;
        }
    }
    return true;
}

/* Update nominal geometry fields from the loaded trackMap */
static void diskUpdateNominalInfo(DISC_DRIVE* drv)
{
    /* Find the first present track to read nominal geometry */
    for (int side = 0; side < MAX_SIDES; side++) {
        for (int track = 0; track < MAX_TRACKS; track++) {
            DiskTrack* t = drv->trackMap[side][track];
            if (!t || t->sectorCount == 0) continue;
            drv->nominalSectorsPerTrack = t->sectorCount;
            drv->nominalSectorSize =
                (t->sectors[0].dataLen > 0)
                ? t->sectors[0].dataLen
                : (128 << t->sectors[0].N);
            return;
        }
    }
}

/* =========================================================
 * Public API — simple accessors
 * ========================================================= */

void diskInit(int driveId)
{
    if (driveId < 0 || driveId >= MAXDRIVES) return;
    DISC_DRIVE* drv = &diskDrives[driveId];
    if (drv->fileHandle) { fclose(drv->fileHandle); drv->fileHandle = NULL; }
    diskFreeModel(drv);
    memset(drv, 0, sizeof(DISC_DRIVE));
    drv->enabled  = 1;
    drv->readOnly = 1;
    diskSetSF7Defaults(drv);
}

void diskEnable(int driveId, int enable)
{
    if (driveId >= 0 && driveId < MAXDRIVES)
        diskDrives[driveId].enabled = enable;
}

void diskReadOnly(int driveId, int readonly)
{
    if (driveId >= 0 && driveId < MAXDRIVES)
        diskDrives[driveId].readOnly = readonly;
}

uint8_t diskIsEnabled(int driveId)
{
    return (driveId >= 0 && driveId < MAXDRIVES)
           ? (uint8_t)diskDrives[driveId].enabled : 0;
}

uint8_t diskIsReadOnly(int driveId)
{
    if (!diskPresent(driveId)) return 0;
    return (uint8_t)diskDrives[driveId].readOnly;
}

uint8_t diskPresent(int driveId)
{
    return (driveId >= 0 && driveId < MAXDRIVES)
           && (diskDrives[driveId].trackMap[0][0] != NULL
               || diskDrives[driveId].tracks > 0);
}

int diskChanged(int driveId)
{
    if (driveId >= 0 && driveId < MAXDRIVES) {
        int c = diskDrives[driveId].changed;
        diskDrives[driveId].changed = 0;
        return c;
    }
    return 0;
}

int diskGetSectorsPerTrack(int driveId)
{
    if (driveId < 0 || driveId >= MAXDRIVES) return 0;
    return diskDrives[driveId].nominalSectorsPerTrack;
}

int diskGetSides(int driveId)
{
    if (driveId < 0 || driveId >= MAXDRIVES) return 0;
    return diskDrives[driveId].sides;
}

int diskGetSectorSize(int driveId, int side, int track, int density)
{
    (void)density;
    if (driveId < 0 || driveId >= MAXDRIVES) return 0;
    /* Return per-track size if available, otherwise nominal */
    DISC_DRIVE* drv = &diskDrives[driveId];
    if (side >= 0 && side < MAX_SIDES && track >= 0 && track < MAX_TRACKS) {
        DiskTrack* t = drv->trackMap[side][track];
        if (t && t->sectorCount > 0 && t->sectors[0].dataLen > 0)
            return t->sectors[0].dataLen;
    }
    return drv->nominalSectorSize;
}

/* New: access a raw DiskTrack pointer */
DiskTrack* diskGetTrack(int driveId, int side, int track)
{
    if (driveId < 0 || driveId >= MAXDRIVES) return NULL;
    if (side  < 0 || side  >= MAX_SIDES)     return NULL;
    if (track < 0 || track >= MAX_TRACKS)    return NULL;
    return diskDrives[driveId].trackMap[side][track];
}

/* New: expose rotation constants for NEC765 rotational timing */
uint32_t diskGetBitsPerRevolution(int driveId)
{
    if (driveId < 0 || driveId >= MAXDRIVES) return SF7_BITS_PER_REV;
    return diskDrives[driveId].bitsPerRevolution;
}

uint32_t diskGetClocksPerBit(int driveId)
{
    if (driveId < 0 || driveId >= MAXDRIVES) return SF7_CLOCKS_PER_BIT;
    return diskDrives[driveId].clocksPerBit;
}

void diskGetDebugInfo(int driveId, DiskDebugInfo* info)
{
    if (!info)
        return;

    memset(info, 0, sizeof(*info));
    if (driveId < 0 || driveId >= MAXDRIVES)
        return;

    const DISC_DRIVE* drv = &diskDrives[driveId];
    info->present          = diskPresent(driveId);
    info->enabled          = drv->enabled;
    info->readOnly         = drv->readOnly;
    info->dirty            = drv->dirty;
    info->format           = drv->format;
    info->tracks           = drv->tracks;
    info->sides            = drv->sides;
    info->sectorsPerTrack  = drv->nominalSectorsPerTrack;
    info->sectorSize       = drv->nominalSectorSize;
    info->rpm              = drv->rpm;
    info->bitrateKbps      = drv->bitrateKbps;
    info->fileName         = drv->fileName;
}

/* =========================================================
 * diskReadSector — IDAM search by R value
 *
 * Replaces the original linear-offset lookup.
 * 'sector' parameter IS the R value (sector ID).
 * ========================================================= */
DSKE diskReadSector(int driveId, uint8_t* buffer,
                    int sector, int side, int track,
                    int density, int* sectorSize)
{
    (void)density;

    if (driveId < 0 || driveId >= MAXDRIVES) return DSKE_NO_DATA;
    DISC_DRIVE* drv = &diskDrives[driveId];
    if (!drv->enabled) return DSKE_NO_DATA;

    if (side  < 0 || side  >= MAX_SIDES)    return DSKE_NO_DATA;
    if (track < 0 || track >= MAX_TRACKS)   return DSKE_NO_DATA;

    DiskTrack* t = drv->trackMap[side][track];
    if (!t) return DSKE_NO_DATA;

    uint8_t targetR = (uint8_t)sector;

    for (int i = 0; i < t->sectorCount; i++) {
        DiskSector* sec = &t->sectors[i];
        if (sec->R != targetR) continue;

        if (sectorSize) *sectorSize = sec->dataLen;
        if (buffer && sec->data)
            memcpy(buffer, sec->data, sec->dataLen);

        /* Propagate pre-baked errors from EDSK/HFE image */
        if (sec->ST1 & 0x20) return DSKE_CRC_ERROR;   /* ST1_DE: data error */
        return DSKE_OK;
    }

    return DSKE_NO_DATA;   /* sector not found → NEC765 sets ST1_MA */
}

/* =========================================================
 * diskWriteSector — update in-memory model + write-through
 * ========================================================= */
uint8_t diskWriteSector(int driveId, uint8_t* buffer,
                        int sector, int side, int track, int density)
{
    (void)density;

    if (driveId < 0 || driveId >= MAXDRIVES) return 0;
    DISC_DRIVE* drv = &diskDrives[driveId];
    if (!drv->enabled || drv->readOnly) return 0;

    if (side  < 0 || side  >= MAX_SIDES)    return 0;
    if (track < 0 || track >= MAX_TRACKS)   return 0;

    DiskTrack* t = drv->trackMap[side][track];
    if (!t) return 0;

    uint8_t targetR = (uint8_t)sector;

    for (int i = 0; i < t->sectorCount; i++) {
        DiskSector* sec = &t->sectors[i];
        if (sec->R != targetR) continue;
        if (!sec->data) continue;

        /* Update in-memory copy */
        memcpy(sec->data, buffer, sec->dataLen);
        drv->dirty = 1;

        /* Write-through to file for SF7 and EDSK.
         * HFE writes stay in-memory (dirty=1, save via diskSaveDS7 on eject). */
        if (drv->format == DISK_FORMAT_SF7 && drv->fileHandle && !drv->readOnly) {
            int spt  = drv->nominalSectorsPerTrack;
            int nsid = drv->sides;
            long offset = (long)((targetR - 1)
                               + spt * (track * nsid + side))
                        * sec->dataLen;
            fseek(drv->fileHandle, offset, SEEK_SET);
            fwrite(buffer, 1, sec->dataLen, drv->fileHandle);
        }
        /* EDSK write-through: the EDSK format stores sectors at variable
         * offsets that we'd need to re-walk.  For now, mark dirty only.
         * A proper implementation would cache the file offset per sector. */

        return 1;
    }

    return 0;
}

/* Legacy diskWrite (used by FORMAT command in original NEC765.cpp) */
uint8_t diskWrite(int driveId, uint8_t* buffer, int sector)
{
    if (driveId < 0 || driveId >= MAXDRIVES) return 0;
    DISC_DRIVE* drv = &diskDrives[driveId];

    /* Linear-sector write for FORMAT.  Only meaningful for SF7 flat images. */
    if (drv->format == DISK_FORMAT_SF7 && drv->fileHandle && !drv->readOnly) {
        int secSize = drv->nominalSectorSize;
        fseek(drv->fileHandle, (long)sector * secSize, SEEK_SET);
        uint8_t success = (fwrite(buffer, 1, secSize, drv->fileHandle)
                           == (size_t)secSize);
        if (success) drv->dirty = 1;
        return success;
    }
    drv->dirty = 1;
    return 1;   /* in-memory only for other formats */
}

/* =========================================================
 * diskSetFormattedTrack
 * ========================================================= */
void diskSetFormattedTrack(int driveId, int side, int track,
                            const FDC_FormatEntry* entries, int count,
                            uint8_t fillerByte, uint8_t gap3)
{
    if (driveId < 0 || driveId >= MAXDRIVES) return;
    if (side  < 0 || side  >= MAX_SIDES)     return;
    if (track < 0 || track >= MAX_TRACKS)    return;
    if (count <= 0 || count > MAX_SECTORS)   return;

    DISC_DRIVE* drv = &diskDrives[driveId];

    /* Free the existing track */
    DiskTrack* old = drv->trackMap[side][track];
    if (old) {
        for (int s = 0; s < old->sectorCount; s++)
            free(old->sectors[s].data);
        free(old);
        drv->trackMap[side][track] = NULL;
    }

    /* Allocate new track */
    DiskTrack* t = (DiskTrack*)calloc(1, sizeof(DiskTrack));
    if (!t) return;

    t->sectorCount = count;
    t->totalBits   = drv->bitsPerRevolution;
    t->encoding    = drv->encoding;
    t->bitrateKbps = drv->bitrateKbps;
    t->fillerByte  = fillerByte;
    t->gap3        = gap3;

    /* Inherit gap4a / gap1 / gap2 from an existing track, or use SF7 defaults */
    DiskTrack* ref = NULL;
    for (int tt = 0; tt < MAX_TRACKS && !ref; tt++)
        for (int ss = 0; ss < MAX_SIDES && !ref; ss++)
            ref = drv->trackMap[ss][tt];
    if (ref) {
        t->gap4a = ref->gap4a;
        t->gap1  = ref->gap1;
        t->gap2  = ref->gap2;
    } else {
        t->gap4a = SF7_GAP4A;
        t->gap1  = SF7_GAP1;
        t->gap2  = MFM_GAP2;
    }

    /* Fill sectors from CHRN entries */
    bool ok = true;
    for (int i = 0; i < count && ok; i++) {
        DiskSector* sec = &t->sectors[i];
        sec->C       = entries[i].C;
        sec->H       = entries[i].H;
        sec->R       = entries[i].R;
        sec->N       = entries[i].N;
        sec->ST1     = 0;
        sec->ST2     = 0;
        sec->damType = 0xFB;           /* normal DAM */
        sec->dataLen = 128 << entries[i].N;
        sec->data    = (uint8_t*)malloc((size_t)sec->dataLen);
        if (!sec->data) {
            /* allocation failure: free everything built so far */
            for (int j = 0; j < i; j++) free(t->sectors[j].data);
            free(t);
            return;
        }
        memset(sec->data, fillerByte, (size_t)sec->dataLen);
    }

    synthesizeSectorPositions(t);
    drv->trackMap[side][track] = t;
    drv->dirty = 1;

    /* Expand geometry counters if the formatted track is beyond current bounds */
    if (track + 1 > drv->tracks) drv->tracks = track + 1;
    if (side  + 1 > drv->sides)  drv->sides  = side  + 1;
}

/* =========================================================
 * diskCreateEmptyDS7 — create a blank pre-formatted DS7 image
 *
 * Generates a 40-track, 1-side, 16-sector-per-track, 256-byte-per-sector
 * SF-7000 disk filled with 0xFF.  All sector headers use proper CHRN
 * values (R = 1..16) so diskWriteSector() can locate sectors by R value
 * when the IPL writes the FAT after FORMAT.
 * ========================================================= */
bool diskCreateEmptyDS7(const char* filename)
{
    DISC_DRIVE drv;
    memset(&drv, 0, sizeof(drv));
    drv.encoding              = 0;
    drv.bitrateKbps           = SF7_BITRATE_KBPS;
    drv.rpm                   = SF7_RPM;
    drv.bitsPerRevolution     = SF7_BITS_PER_REV;
    drv.clocksPerRevolution   = SF7_CLOCKS_PER_REV;
    drv.clocksPerBit          = SF7_CLOCKS_PER_BIT;
    drv.tracks                = SF7_TRACKS;
    drv.sides                 = SF7_SIDES;
    drv.nominalSectorsPerTrack = SF7_SECTORS_PER_TRACK;
    drv.nominalSectorSize      = SF7_SECTOR_SIZE;

    bool ok = true;
    for (int track = 0; track < SF7_TRACKS && ok; track++) {
        DiskTrack* t = (DiskTrack*)calloc(1, sizeof(DiskTrack));
        if (!t) { ok = false; break; }

        t->sectorCount = SF7_SECTORS_PER_TRACK;
        t->totalBits   = SF7_BITS_PER_REV;
        t->encoding    = 0;
        t->bitrateKbps = SF7_BITRATE_KBPS;
        t->gap4a       = SF7_GAP4A;
        t->gap1        = SF7_GAP1;
        t->gap2        = MFM_GAP2;
        t->gap3        = SF7_FORMAT_GPL;
        t->fillerByte  = SF7_FILLER;

        for (int s = 0; s < SF7_SECTORS_PER_TRACK && ok; s++) {
            DiskSector* sec = &t->sectors[s];
            sec->C       = (uint8_t)track;
            sec->H       = 0;
            sec->R       = (uint8_t)(s + 1);
            sec->N       = SF7_SECTOR_N;
            sec->ST1     = 0x00;
            sec->ST2     = 0x00;
            sec->damType = 0xFB;
            sec->dataLen = SF7_SECTOR_SIZE;
            sec->data    = (uint8_t*)malloc(SF7_SECTOR_SIZE);
            if (!sec->data) { ok = false; break; }
            memset(sec->data, SF7_FILLER, SF7_SECTOR_SIZE);
        }

        synthesizeSectorPositions(t);
        drv.trackMap[0][track] = t;
    }

    bool saved = false;
    if (ok)
        saved = DS7Format::save(&drv, filename);

    /* Free temporary track model */
    for (int track = 0; track < SF7_TRACKS; track++) {
        DiskTrack* t = drv.trackMap[0][track];
        if (!t) continue;
        for (int s = 0; s < t->sectorCount; s++)
            free(t->sectors[s].data);
        free(t);
    }

    return saved;
}

/* =========================================================
 * diskSaveDS7 — save current in-memory model as .ds7
 * ========================================================= */
uint8_t diskSaveDS7(int driveId, const char* fileName)
{
    if (driveId < 0 || driveId >= MAXDRIVES) return 0;
    return DS7Format::save(&diskDrives[driveId], fileName) ? 1 : 0;
}

/* =========================================================
 * diskEject
 * ========================================================= */
uint8_t diskEject(int driveId)
{
    return diskChange(driveId, NULL);
}

/* =========================================================
 * diskChange — format detection and loading
 * ========================================================= */
uint8_t diskChange(int driveId, const char* fileName)
{
    if (driveId < 0 || driveId >= MAXDRIVES) return 0;

    DISC_DRIVE* drv = &diskDrives[driveId];

    /* Auto-save dirty in-memory model as .ds7 before releasing it.
     * SF7 uses write-through (already on disk); all other formats are in-memory. */
    if (drv->dirty && drv->format != DISK_FORMAT_SF7
                   && drv->format != DISK_FORMAT_NONE
                   && drv->fileName[0]) {
        char ds7Save[512];
        if (DS7Format::makeDS7Name(drv->fileName, ds7Save, sizeof(ds7Save)))
            DS7Format::save(drv, ds7Save);
    }

    /* Close previous file handle */
    if (drv->fileHandle) {
        fclose(drv->fileHandle);
        drv->fileHandle = NULL;
    }

    /* Free existing track model */
    diskFreeModel(drv);

    /* Clear geometry */
    drv->format  = DISK_FORMAT_NONE;
    drv->sides   = 0;
    drv->tracks  = 0;
    drv->nominalSectorsPerTrack = 0;
    drv->nominalSectorSize      = 0;
    drv->dirty   = 0;
    drv->changed = 1;

    if (!fileName || !fileName[0]) {
        /* Eject only */
        nec765LogDiskEject(driveId);
        return 1;
    }

    /* Try to open read-write first, fall back to read-only.
     * fopen returns NULL for directories too, so no separate stat check needed. */
    FILE* f = fopen(fileName, "r+b");
    int   readOnly = 0;
    if (!f) {
        f = fopen(fileName, "rb");
        readOnly = 1;
    }
    if (!f) return 0;

    /* Store filename for save-as (snprintf: safe on MSVC, truncates cleanly) */
    snprintf(drv->fileName, sizeof(drv->fileName), "%s", fileName);

    diskSetSF7Defaults(drv);   /* sensible defaults before loader may override */

    bool ok = false;

    /* ---- Check for .ds7 sidecar (fast path) ---- */
    {
        char ds7Name[512];
        if (DS7Format::makeDS7Name(fileName, ds7Name, sizeof(ds7Name))) {
            FILE* ds7f = fopen(ds7Name, "rb");
            if (ds7f && DS7Format::probe(ds7f)) {
                fseek(ds7f, 0, SEEK_SET);
                ok = DS7Format::load(ds7f, drv);
                fclose(ds7f);
                ds7f = NULL;   /* prevent double-close below if load failed */
                if (ok) {
                    drv->format = DISK_FORMAT_DS7;
                    /* Keep the original file handle open for SF7 write-through
                     * (if the original was SF7) — but for DS7 loads we use
                     * DS7Format::save on eject, so close the original. */
                    fclose(f);
                    f = NULL;
                    drv->readOnly = readOnly;
                    diskUpdateNominalInfo(drv);
                    nec765LogDiskLoad(driveId, fileName);
                    return 1;
                }
            }
            if (ds7f) fclose(ds7f);
        }
    }

    /* ---- .sf7 extension → SF-7000 flat image, geometry always 40T/1S/16S/256B ---- */
    if (!ok) {
        const char* ext = strrchr(fileName, '.');
        if (ext && STRCASECMP(ext, ".sf7") == 0) {
            drv->tracks = 40;
            drv->sides  = 1;
            ok = loadSF7Flat(f, drv, 40, 1, SF7_SECTORS_PER_TRACK, SF7_SECTOR_SIZE);
            if (ok) drv->format = DISK_FORMAT_SF7;
        }
    }

    /* ---- HFE loader ---- */
    if (!ok && HFELoader::probe(f)) {
        fseek(f, 0, SEEK_SET);
        ok = HFELoader::load(f, drv);
        if (ok) {
            drv->format = DISK_FORMAT_HFE;
            /* Auto-save as .ds7 for future fast loads */
            char ds7Name[512];
            if (DS7Format::makeDS7Name(fileName, ds7Name, sizeof(ds7Name)))
                DS7Format::save(drv, ds7Name);
        }
    }

    /* ---- EDSK loader ---- */
    if (!ok && EDSKLoader::probe(f)) {
        fseek(f, 0, SEEK_SET);
        ok = EDSKLoader::load(f, drv);
        if (ok) drv->format = DISK_FORMAT_EDSK;
    }

    /* ---- Generic flat image (fallback) — geometry from file size / BPB ---- */
    if (!ok) {
        fseek(f, 0, SEEK_END);
        long fileSize = ftell(f);
        int  tracks = 0, sides = 0, spt = 0, secSize = 0;
        if (detectSF7Geometry(f, fileSize, &tracks, &sides, &spt, &secSize)) {
            drv->tracks = tracks;
            drv->sides  = sides;
            ok = loadSF7Flat(f, drv, tracks, sides, spt, secSize);
            if (ok) drv->format = DISK_FORMAT_SF7;
        }
    }

    if (!ok) {
        fclose(f);
        return 0;
    }

    drv->readOnly = readOnly;
    drv->changed  = 1;
    drv->dirty    = 0;

    /* Keep file open for SF7 write-through */
    if (drv->format == DISK_FORMAT_SF7 && !readOnly) {
        drv->fileHandle = f;
    } else {
        fclose(f);
        drv->fileHandle = NULL;
    }

    diskUpdateNominalInfo(drv);
    nec765LogDiskLoad(driveId, fileName);
    return 1;
}
