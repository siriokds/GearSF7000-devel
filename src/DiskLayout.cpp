/*
** DiskLayout.cpp — Track layout engine implementation
*/
#include "DiskLayout.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* =========================================================
 * Internal helpers
 * ========================================================= */

/* Raw MFM bits for the track preamble (GAP4a + IAM + GAP1) */
static uint32_t mfmPreambleBits(uint8_t gap4a, uint8_t gap1)
{
    return (uint32_t)(gap4a + MFM_IAM_SYNC + MFM_IAM_MARK + gap1)
           * MFM_BITS_PER_BYTE;
}

/* Raw MFM bits from the start of an IDAM sync to the start of the DAM sync.
 * Covers: IDAM_SYNC + IDAM_MARK + CHRN + CRC + GAP2 + DAM_SYNC + DAM_MARK */
static uint32_t mfmIDAMToDAMBits(uint8_t gap2)
{
    return (uint32_t)(MFM_IDAM_SYNC + MFM_IDAM_MARK + MFM_CHRN + MFM_CRC
                    + gap2
                    + MFM_DAM_SYNC + MFM_DAM_MARK)
           * MFM_BITS_PER_BYTE;
}

/* Raw MFM bits consumed by one full sector slot (IDAM..GAP3) */
static uint32_t mfmSectorSlotBits(int dataLen, uint8_t gap2, uint8_t gap3)
{
    return (uint32_t)(MFM_IDAM_SYNC + MFM_IDAM_MARK + MFM_CHRN + MFM_CRC
                    + gap2
                    + MFM_DAM_SYNC + MFM_DAM_MARK + dataLen + MFM_DATA_CRC
                    + gap3)
           * MFM_BITS_PER_BYTE;
}

/* =========================================================
 * synthesizeSectorPositions
 * ========================================================= */
void synthesizeSectorPositions(DiskTrack* track)
{
    uint32_t bitPos = mfmPreambleBits(track->gap4a, track->gap1);

    for (int i = 0; i < track->sectorCount; i++) {
        DiskSector* sec = &track->sectors[i];
        int dataLen = (sec->dataLen > 0) ? sec->dataLen : (128 << sec->N);

        sec->bitIDAM = bitPos;
        sec->bitDAM  = bitPos + mfmIDAMToDAMBits(track->gap2);

        /* bitDataEnd: after DAM_MARK + data bytes + CRC */
        sec->bitDataEnd = sec->bitDAM
            + (uint32_t)(MFM_DAM_MARK + dataLen + MFM_DATA_CRC)
              * MFM_BITS_PER_BYTE;

        bitPos += mfmSectorSlotBits(dataLen, track->gap2, track->gap3);
        sec->bitNextSlot = bitPos;
    }

    track->usedBits = bitPos;
    /* If usedBits > totalBits the track overflows the ring.
     * The caller (EDSKLoader, etc.) should check and may report a warning. */
}

/* =========================================================
 * computeTrackLayout — format validator
 * ========================================================= */

/* Compute total raw bits used by one revolution given format params */
static uint32_t computeBitsPerRev(uint16_t bitrateKbps, uint16_t rpm)
{
    /* raw MFM bits = data_kbps * 1000 * 2 [clock+data] * 60/rpm */
    return (uint32_t)bitrateKbps * 1000u * 2u * 60u / (uint32_t)rpm;
}

TrackLayoutReport computeTrackLayout(const TrackFormatParams* p)
{
    TrackLayoutReport rep;
    memset(&rep, 0, sizeof(rep));

    uint32_t totalBits = computeBitsPerRev(p->bitrateKbps, p->rpm);
    rep.totalBitsAvailable = totalBits;
    rep.sectorCount        = p->sectorCount;

    /* Build a temporary DiskTrack to call synthesizeSectorPositions */
    DiskTrack t;
    memset(&t, 0, sizeof(t));
    t.sectorCount  = p->sectorCount;
    t.totalBits    = totalBits;
    t.encoding     = p->encoding;
    t.bitrateKbps  = p->bitrateKbps;
    t.gap4a        = p->gap4a;
    t.gap1         = p->gap1;
    t.gap2         = p->gap2;
    t.gap3         = p->gap3;

    for (int i = 0; i < p->sectorCount && i < MAX_SECTORS; i++) {
        t.sectors[i].C       = p->C[i];
        t.sectors[i].H       = p->H[i];
        t.sectors[i].R       = p->R[i];
        t.sectors[i].N       = p->N[i];
        t.sectors[i].ST1     = p->ST1[i];
        t.sectors[i].ST2     = p->ST2[i];
        t.sectors[i].dataLen = 128 << p->N[i];
        t.sectors[i].data    = NULL;
    }

    synthesizeSectorPositions(&t);

    rep.totalBitsUsed = t.usedBits;
    rep.slackBits     = (int32_t)totalBits - (int32_t)t.usedBits;
    rep.fits          = (t.usedBits <= totalBits);

    /* Fill per-sector info */
    rep.allReadable = 1;
    for (int i = 0; i < p->sectorCount && i < MAX_SECTORS; i++) {
        DiskSector* sec      = &t.sectors[i];
        SectorLayoutInfo* si = &rep.sectors[i];

        si->R          = sec->R;
        si->bitIDAM    = sec->bitIDAM;
        si->bitDAM     = sec->bitDAM;
        si->bitDataEnd = sec->bitDataEnd;
        si->bitNextSlot= sec->bitNextSlot;

        /* GAP3 in bytes */
        si->gap3Bytes  = (int32_t)(sec->bitNextSlot - sec->bitDataEnd)
                         / (int32_t)MFM_BITS_PER_BYTE;

        /* Check overlap with previous sector */
        if (i > 0) {
            DiskSector* prev = &t.sectors[i - 1];
            if (sec->bitIDAM < prev->bitDataEnd) {
                si->status   = SECTOR_IDAM_OVERLAPS_PREV_DATA;
                si->readable = 0;
            }
        }

        /* Check if data extends past ring */
        if (sec->bitDataEnd > totalBits) {
            si->status   = SECTOR_DATA_PAST_TRACK_END;
            si->readable = 0;
        }

        /* Check minimum GAP3 */
        if (si->status == SECTOR_OK && si->gap3Bytes < 12) {
            si->status = SECTOR_GAP3_TOO_SMALL;
            /* Still physically readable if no overlap */
        }

        if (si->status == SECTOR_OK)
            si->readable = 1;

        if (!si->readable)
            rep.allReadable = 0;
    }

    /* Compute min and optimal GAP3 */
    {
        uint32_t preambleBits = mfmPreambleBits(p->gap4a, p->gap1);
        uint32_t perSecBits   = 0;
        for (int i = 0; i < p->sectorCount && i < MAX_SECTORS; i++) {
            int dataLen = 128 << p->N[i];
            perSecBits += (uint32_t)(MFM_IDAM_SYNC + MFM_IDAM_MARK + MFM_CHRN + MFM_CRC
                                   + p->gap2
                                   + MFM_DAM_SYNC + MFM_DAM_MARK + dataLen + MFM_DATA_CRC)
                          * MFM_BITS_PER_BYTE;
        }

        int32_t slack = (int32_t)totalBits - (int32_t)preambleBits - (int32_t)perSecBits;
        if (slack > 0 && p->sectorCount > 0) {
            int gap3Bits = slack / p->sectorCount;
            int gap3Min  = 12; /* bytes, minimum MFM recommendation */
            rep.minGap3Bytes     = (uint8_t)(gap3Min < 255 ? gap3Min : 255);
            rep.optimalGap3Bytes = (uint8_t)(gap3Bits / MFM_BITS_PER_BYTE);
        }
    }

    return rep;
}

/* =========================================================
 * printTrackLayoutReport — debug helper
 * ========================================================= */
void printTrackLayoutReport(const TrackLayoutReport* rep)
{
    printf("Track layout: %d sectors, %s\n",
           rep->sectorCount,
           rep->fits ? "FITS" : "OVERFLOW");
    printf("  totalBits available : %u\n", rep->totalBitsAvailable);
    printf("  totalBits used      : %u\n", rep->totalBitsUsed);
    printf("  slack               : %d bits (%d bytes)\n",
           rep->slackBits, rep->slackBits / MFM_BITS_PER_BYTE);
    printf("  minGap3             : %d bytes\n", rep->minGap3Bytes);
    printf("  optimalGap3         : %d bytes\n", rep->optimalGap3Bytes);
    printf("  allReadable         : %s\n", rep->allReadable ? "yes" : "NO");

    for (int i = 0; i < rep->sectorCount; i++) {
        const SectorLayoutInfo* si = &rep->sectors[i];
        const char* status = "OK";
        if (si->status == SECTOR_IDAM_OVERLAPS_PREV_DATA) status = "OVERLAP";
        if (si->status == SECTOR_DATA_PAST_TRACK_END)      status = "PAST_END";
        if (si->status == SECTOR_GAP3_TOO_SMALL)           status = "GAP3<12";

        printf("  [%2d] R=%02X  IDAM=%6u DAM=%6u end=%6u next=%6u gap3=%3dB %s\n",
               i, si->R,
               si->bitIDAM, si->bitDAM, si->bitDataEnd, si->bitNextSlot,
               si->gap3Bytes, status);
    }
}

/* =========================================================
 * testFormatScenarios — standalone test function
 * ========================================================= */
void testFormatScenarios(void)
{
    TrackFormatParams p;
    memset(&p, 0, sizeof(p));
    p.encoding     = 0;
    p.bitrateKbps  = SF7_BITRATE_KBPS;
    p.rpm          = SF7_RPM;
    p.gap4a        = SF7_GAP4A;
    p.gap1         = SF7_GAP1;
    p.gap2         = MFM_GAP2;
    p.sectorCount  = SF7_SECTORS_PER_TRACK;
    for (int i = 0; i < SF7_SECTORS_PER_TRACK; i++) {
        p.C[i] = 0; p.H[i] = 0; p.R[i] = (uint8_t)(i + 1); p.N[i] = SF7_SECTOR_N;
    }

    printf("=== Standard SF-7000 (GAP3=%d from IPL) ===\n", SF7_FORMAT_GPL);
    p.gap3 = SF7_FORMAT_GPL;
    TrackLayoutReport r = computeTrackLayout(&p);
    printTrackLayoutReport(&r);

    printf("\n=== GAP3=5 (too small) ===\n");
    p.gap3 = 5;
    r = computeTrackLayout(&p);
    printTrackLayoutReport(&r);

    printf("\n=== GAP3=0 (guaranteed overflow) ===\n");
    p.gap3 = 0;
    r = computeTrackLayout(&p);
    printTrackLayoutReport(&r);

    printf("\n=== Custom sector IDs 0x41..0x50 (protection pattern) ===\n");
    p.gap3 = SF7_FORMAT_GPL;
    for (int i = 0; i < SF7_SECTORS_PER_TRACK; i++)
        p.R[i] = (uint8_t)(0x41 + i);
    r = computeTrackLayout(&p);
    printTrackLayoutReport(&r);
}
