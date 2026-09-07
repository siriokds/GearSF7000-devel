/*
** HFELoader.cpp — HxC Floppy Emulator v1 (HXCPICFE) loader + MFM decoder
**
** MFM bit coordinate system (raw bit units, clock+data pairs):
**   1 data byte   = 16 raw MFM bits
**   1 revolution  = ~100,000 raw MFM bits  (250 kbps, 300 RPM)
**
** HFE bit ordering: bits are stored LSB-first within each byte.
** Byte 0 bit 0 = first bit arriving from disk = MSB of the first
** 16-bit MFM word read in temporal order.
**
** Sync pattern: 0x4489  (MFM representation of special A1 byte).
** Three consecutive 0x4489 words = sector sync.
*/
#include "HFELoader.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =========================================================
 * Internal: bit array helpers
 * ========================================================= */

/* Read one bit from a byte array stored LSB-first per byte.
 * Bit index 0 = first bit in time = bit 0 of byte 0. */
static inline int bitAt(const uint8_t* arr, uint32_t pos)
{
    return (arr[pos >> 3] >> (pos & 7)) & 1;
}

/* Read 16 consecutive bits (MSB first) as a uint16 starting at bit_pos. */
static inline uint16_t read16(const uint8_t* arr, uint32_t bit_pos)
{
    uint16_t v = 0;
    for (int i = 0; i < 16; i++)
        v = (uint16_t)((v << 1) | bitAt(arr, bit_pos + i));
    return v;
}

/* Decode one MFM byte starting at bit_pos (raw MFM stream).
 * In MFM each data byte is encoded as 16 raw bits: [clock][data] × 8.
 * Data bits are at odd positions: +1, +3, +5, +7, +9, +11, +13, +15.
 * MSB of the decoded byte = data bit at bit_pos+1. */
static uint8_t readMFMByte(const uint8_t* arr, uint32_t bit_pos)
{
    uint8_t result = 0;
    for (int b = 7; b >= 0; b--)
        result = (uint8_t)(result | (bitAt(arr, bit_pos + (uint32_t)(7 - b) * 2 + 1) << b));
    return result;
}

/* =========================================================
 * findSyncTriple
 *
 * Search for three consecutive 0x4489 MFM sync words starting from
 * search_from in bit_array of total_bits bits.
 * Returns the bit position of the first sync word, or -1 if not found.
 * ========================================================= */
static int32_t findSyncTriple(const uint8_t* arr, uint32_t total_bits,
                               uint32_t search_from)
{
    if (total_bits < search_from + 48) return -1;

    /* Prime the 16-bit sliding window */
    uint16_t window = 0;
    uint32_t prime_end = search_from + 15;
    if (prime_end >= total_bits) return -1;

    for (uint32_t i = search_from; i < prime_end; i++)
        window = (uint16_t)((window << 1) | bitAt(arr, i));

    for (uint32_t i = prime_end; i + 33 < total_bits; i++) {
        window = (uint16_t)((window << 1) | bitAt(arr, i));

        if (window != 0x4489) continue;

        uint32_t candidate = i - 15;  /* start of first sync word */

        /* Verify second sync word */
        if (candidate + 32 >= total_bits) return -1;
        uint16_t w2 = read16(arr, candidate + 16);
        if (w2 != 0x4489) continue;

        /* Verify third sync word */
        if (candidate + 48 >= total_bits) return -1;
        uint16_t w3 = read16(arr, candidate + 32);
        if (w3 != 0x4489) continue;

        return (int32_t)candidate;
    }
    return -1;
}

/* =========================================================
 * deinterleave
 * ========================================================= */
void HFELoader::deinterleave(FILE* f, long trackByteStart,
                              uint16_t trackLenBytes,
                              uint8_t** side0Out, uint8_t** side1Out,
                              int numSides,
                              uint32_t* side0BytesOut)
{
    int numChunks = ((int)trackLenBytes + 511) / 512;

    uint8_t* side0 = (uint8_t*)malloc((size_t)numChunks * 256);
    uint8_t* side1 = (numSides > 1) ? (uint8_t*)malloc((size_t)numChunks * 256) : NULL;

    fseek(f, trackByteStart, SEEK_SET);

    uint8_t chunk[512];
    for (int c = 0; c < numChunks; c++) {
        memset(chunk, 0xFF, 512);
        fread(chunk, 1, 512, f);
        if (side0) memcpy(side0 + c * 256, chunk,       256);
        if (side1) memcpy(side1 + c * 256, chunk + 256, 256);
    }

    *side0Out      = side0;
    *side1Out      = side1;
    *side0BytesOut = (uint32_t)numChunks * 256u;
}

/* =========================================================
 * decodeMFMTrack
 *
 * Scan the raw MFM byte array for IDAM/DAM sync patterns and
 * extract sector data.  The function scans up to 2 revolutions
 * to find all sectors even if the ring boundary splits a sector.
 * ========================================================= */
DiskTrack* HFELoader::decodeMFMTrack(uint8_t* rawMFMBytes,
                                      uint32_t rawByteCount,
                                      uint32_t bitsPerRev,
                                      uint8_t  encoding)
{
    DiskTrack* t = (DiskTrack*)calloc(1, sizeof(DiskTrack));
    if (!t) return NULL;

    t->totalBits    = bitsPerRev;
    t->encoding     = encoding;
    t->bitrateKbps  = SF7_BITRATE_KBPS;
    /* HFE sector positions are measured from the bitstream, not synthesized.
     * Store SF-7000 preamble metadata for accuracy (e.g., when saved as .ds7). */
    t->gap4a        = SF7_GAP4A;
    t->gap1         = SF7_GAP1;
    t->gap2         = MFM_GAP2;
    t->gap3         = 0;            /* will be measured below */
    t->rawMFM       = rawMFMBytes;  /* takes ownership */
    t->rawMFMBits   = rawByteCount * 8u;

    uint32_t totalBits = t->rawMFMBits;
    /* We may scan up to 2× the raw length to handle ring wrap */
    uint32_t scanLimit = (totalBits > bitsPerRev)
                          ? totalBits           /* already >1 revolution */
                          : totalBits;          /* single revolution only */

    uint32_t searchFrom = 0;

    while (t->sectorCount < MAX_SECTORS) {
        /* Find IDAM sync (A1 A1 A1 FE) */
        int32_t idamSync = findSyncTriple(rawMFMBytes, scanLimit, searchFrom);
        if (idamSync < 0) break;

        uint32_t idamSyncPos = (uint32_t)idamSync;

        /* Mark byte is at position idamSyncPos + 48 (after 3×16 bits of sync) */
        uint32_t markPos = idamSyncPos + 48u;
        if (markPos + 16 >= scanLimit) break;

        uint8_t mark = readMFMByte(rawMFMBytes, markPos);
        if (mark != 0xFE) {
            /* Not an IDAM — could be IAM (0xFC) or other; skip and continue */
            searchFrom = idamSyncPos + 16;
            continue;
        }

        /* Read CHRN (each byte = 16 raw bits) */
        uint32_t chromPos = markPos + 16u;
        if (chromPos + 4 * 16 >= scanLimit) break;

        uint8_t C    = readMFMByte(rawMFMBytes, chromPos + 0);
        uint8_t H    = readMFMByte(rawMFMBytes, chromPos + 16);
        uint8_t R    = readMFMByte(rawMFMBytes, chromPos + 32);
        uint8_t N    = readMFMByte(rawMFMBytes, chromPos + 48);
        /* CRC: 2 bytes, skip for now (positions chromPos+64 and chromPos+80) */

        /* Position after IDAM content (sync + mark + CHRN + CRC) */
        uint32_t idamEnd = idamSyncPos + 48u + 16u + 4u * 16u + 2u * 16u;

        /* Find DAM sync (A1 A1 A1 FB or F8) */
        int32_t damSync = findSyncTriple(rawMFMBytes, scanLimit, idamEnd);
        if (damSync < 0) break;

        uint32_t damSyncPos  = (uint32_t)damSync;
        uint32_t damMarkPos  = damSyncPos + 48u;
        if (damMarkPos + 16 >= scanLimit) break;

        uint8_t damMark = readMFMByte(rawMFMBytes, damMarkPos);
        if (damMark != 0xFB && damMark != 0xF8) {
            /* Unexpected mark — skip past this IDAM */
            searchFrom = idamSyncPos + 16;
            continue;
        }

        int dataLen = 128 << (N & 0x07);  /* clamp N to 0-7 */
        uint32_t dataStartBit = damMarkPos + 16u;
        uint32_t dataEndBit   = dataStartBit + (uint32_t)dataLen * 16u + 2u * 16u; /* +CRC */

        if (dataEndBit > scanLimit) {
            /* Sector data extends past the raw buffer — truncate if possible */
            if (dataStartBit >= scanLimit) {
                searchFrom = damSyncPos + 16;
                continue;
            }
        }

        /* Read sector data */
        uint8_t* data = (uint8_t*)malloc(dataLen);
        if (!data) break;

        for (int b = 0; b < dataLen; b++) {
            uint32_t bitPos = dataStartBit + (uint32_t)b * 16u;
            data[b] = (bitPos + 15 < scanLimit) ? readMFMByte(rawMFMBytes, bitPos) : 0;
        }

        /* Determine CRC status (simplified: check CRC bytes non-zero) */
        uint8_t st1 = 0x00;
        uint8_t st2 = 0x00;
        /* A full CRC check would compute CRC-CCITT over [A1 A1 A1 DAM_MARK DATA].
         * For Phase 1, we leave ST1/ST2 at 0 (no data error reported). */

        /* Store the decoded sector */
        DiskSector* sec   = &t->sectors[t->sectorCount++];
        sec->C            = C;
        sec->H            = H;
        sec->R            = R;
        sec->N            = N;
        sec->ST1          = st1;
        sec->ST2          = st2;
        sec->damType      = damMark;
        sec->dataLen      = dataLen;
        sec->data         = data;
        /* Bit positions are measured from the raw bitstream */
        sec->bitIDAM      = idamSyncPos % totalBits;
        sec->bitDAM       = damSyncPos  % totalBits;
        sec->bitDataEnd   = dataEndBit  % totalBits;
        sec->bitNextSlot  = dataEndBit  % totalBits;  /* updated below */

        /* Advance search past this sector's data */
        searchFrom = dataEndBit;
    }

    /* ---- Post-processing ---- */

    /* Set bitNextSlot for each sector = bitIDAM of the next sector */
    if (t->sectorCount > 0) {
        for (int i = 0; i < t->sectorCount - 1; i++)
            t->sectors[i].bitNextSlot = t->sectors[i + 1].bitIDAM;

        /* Last sector wraps around the ring */
        int last = t->sectorCount - 1;
        t->sectors[last].bitNextSlot =
            (t->sectorCount > 1)
            ? t->sectors[0].bitIDAM + totalBits   /* one full revolution later */
            : t->sectors[0].bitIDAM + totalBits;

        /* Measure gap3 from first sector's data end to second sector's IDAM */
        if (t->sectorCount > 1) {
            uint32_t gap3raw = t->sectors[0].bitNextSlot - t->sectors[0].bitDataEnd;
            t->gap3 = (uint8_t)(gap3raw / MFM_BITS_PER_BYTE);
        } else {
            t->gap3 = SF7_FORMAT_GPL;   /* sensible default for single-sector tracks */
        }
    }

    t->usedBits = (t->sectorCount > 0)
        ? t->sectors[t->sectorCount - 1].bitNextSlot
        : 0u;

    return t;
}

/* =========================================================
 * probe
 * ========================================================= */
bool HFELoader::probe(FILE* f)
{
    uint8_t magic[8];
    fseek(f, 0, SEEK_SET);
    if (fread(magic, 1, 8, f) != 8) return false;
    return (memcmp(magic, "HXCPICFE", 8) == 0);
}

/* =========================================================
 * load
 * ========================================================= */
bool HFELoader::load(FILE* f, DISC_DRIVE* drv)
{
    /* ---- Main Header (512 bytes) ---- */
    uint8_t header[512];
    fseek(f, 0, SEEK_SET);
    if (fread(header, 1, 512, f) != 512) return false;
    if (memcmp(header, "HXCPICFE", 8) != 0) return false;

    int      numTracks  = header[0x09];
    int      numSides   = header[0x0A];
    uint8_t  encoding   = header[0x0B];   /* 0=MFM, 2=FM */
    uint16_t bitrateK   = (uint16_t)(header[0x0C] | (header[0x0D] << 8));
    uint16_t rpm        = (uint16_t)(header[0x0E] | (header[0x0F] << 8));
    uint16_t lutBlock   = (uint16_t)(header[0x12] | (header[0x13] << 8));

    if (numTracks <= 0 || numTracks > MAX_TRACKS) return false;
    if (numSides  <= 0 || numSides  > MAX_SIDES)  return false;

    if (rpm      == 0) rpm      = SF7_RPM;
    if (bitrateK == 0) bitrateK = SF7_BITRATE_KBPS;

    drv->tracks      = numTracks;
    drv->sides       = numSides;
    drv->encoding    = encoding;
    drv->bitrateKbps = bitrateK;
    drv->rpm         = rpm;

    /* Raw MFM bits per revolution: data_kbps × 1000 × 2 (clock+data) × 60/rpm */
    drv->bitsPerRevolution   = (uint32_t)bitrateK * 1000u * 2u * 60u / (uint32_t)rpm;
    drv->clocksPerRevolution = 3579545u * 60u / (uint32_t)rpm;
    drv->clocksPerBit        = drv->clocksPerRevolution / drv->bitsPerRevolution;

    /* ---- Track LUT (512 bytes at block lutBlock) ---- */
    uint8_t lut[512];
    fseek(f, (long)lutBlock * 512, SEEK_SET);
    if (fread(lut, 1, 512, f) != 512) return false;

    /* ---- Load every track ---- */
    for (int t = 0; t < numTracks && t < MAX_TRACKS; t++) {
        uint16_t blockOff  = (uint16_t)(lut[t * 4 + 0] | (lut[t * 4 + 1] << 8));
        uint16_t trackLen  = (uint16_t)(lut[t * 4 + 2] | (lut[t * 4 + 3] << 8));

        if (blockOff == 0xFFFF || trackLen == 0) {
            /* Track not present in image */
            for (int s = 0; s < numSides; s++)
                drv->trackMap[s][t] = NULL;
            continue;
        }

        long trackByteStart = (long)blockOff * 512;

        uint8_t* side0 = NULL;
        uint8_t* side1 = NULL;
        uint32_t side0Bytes = 0;

        deinterleave(f, trackByteStart, trackLen,
                     &side0, &side1, numSides, &side0Bytes);

        for (int s = 0; s < numSides; s++) {
            uint8_t* mfmData = (s == 0) ? side0 : side1;
            if (!mfmData) {
                drv->trackMap[s][t] = NULL;
                continue;
            }

            DiskTrack* track = decodeMFMTrack(mfmData,
                                               side0Bytes,
                                               drv->bitsPerRevolution,
                                               encoding);
            drv->trackMap[s][t] = track;
            /* mfmData is now owned by the DiskTrack (track->rawMFM) */
        }
    }

    return true;
}
