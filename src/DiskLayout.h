/*
** DiskLayout.h — Track layout engine declarations
**
** synthesizeSectorPositions: computes bit coordinates from gap params.
** computeTrackLayout / printTrackLayoutReport: for format validation/testing.
*/
#ifndef DISKLAYOUT_H
#define DISKLAYOUT_H

#include "DiskModel.h"

/* =========================================================
 * synthesizeSectorPositions
 *
 * Given a DiskTrack with sectors[] already populated
 * (IDAM fields + data + gap3), computes bitIDAM, bitDAM,
 * bitDataEnd, bitNextSlot for every sector.
 * Also sets track->usedBits.
 * ========================================================= */
void synthesizeSectorPositions(DiskTrack* track);

/* =========================================================
 * Track layout report — for FORMAT validation and testing
 * ========================================================= */
typedef enum {
    SECTOR_OK,
    SECTOR_IDAM_OVERLAPS_PREV_DATA, /* IDAM starts inside the previous sector */
    SECTOR_DATA_PAST_TRACK_END,      /* data extends beyond the ring */
    SECTOR_GAP3_TOO_SMALL            /* GAP3 < 12 bytes (minimum recommended) */
} SectorStatus;

typedef struct {
    SectorStatus status;
    uint8_t      R;
    uint32_t     bitIDAM;
    uint32_t     bitDAM;
    uint32_t     bitDataEnd;
    uint32_t     bitNextSlot;
    int32_t      gap3Bytes;     /* effective GAP3 in bytes; negative = overlap */
    int          readable;      /* 0 = NEC765 would set ST1_MA */
} SectorLayoutInfo;

typedef struct {
    uint32_t         totalBitsAvailable;
    uint32_t         totalBitsUsed;
    int32_t          slackBits;         /* negative = overflow */
    int              fits;              /* 1 if all sectors fit in the ring */
    int              allReadable;
    int              sectorCount;
    SectorLayoutInfo sectors[MAX_SECTORS];
    uint8_t          minGap3Bytes;      /* minimum gap3 that keeps the track in ring */
    uint8_t          optimalGap3Bytes;  /* uniform distribution of slack */
} TrackLayoutReport;

typedef struct {
    uint8_t  encoding;
    uint16_t bitrateKbps;
    uint16_t rpm;
    uint8_t  gap4a;
    uint8_t  gap1;
    uint8_t  gap2;
    uint8_t  gap3;
    int      sectorCount;
    uint8_t  C[MAX_SECTORS];
    uint8_t  H[MAX_SECTORS];
    uint8_t  R[MAX_SECTORS];
    uint8_t  N[MAX_SECTORS];
    uint8_t  ST1[MAX_SECTORS];
    uint8_t  ST2[MAX_SECTORS];
} TrackFormatParams;

TrackLayoutReport computeTrackLayout(const TrackFormatParams* p);
void              printTrackLayoutReport(const TrackLayoutReport* rep);

/* Standalone test: prints reports for standard SF-7000 and edge-case layouts */
void testFormatScenarios(void);

#endif /* DISKLAYOUT_H */
