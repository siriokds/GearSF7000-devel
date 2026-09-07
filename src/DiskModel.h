/*
** DiskModel.h — Physical Track Model for GearSF7000 FDC subsystem
**
** Unified in-memory disk representation used by all loaders
** (SF7 flat, EDSK, HFE) and by the NEC765 emulator.
**
** Bit coordinate system: raw MFM bit units (clock + data pairs).
**   1 data byte = 16 raw MFM bits.
**   1 revolution (300 RPM, 250 kbps MFM) = 100,000 raw MFM bits.
*/
#ifndef DISKMODEL_H
#define DISKMODEL_H

#include <stdint.h>
#include <stdio.h>

/* =========================================================
 * Limits
 * ========================================================= */
#define MAX_TRACKS    85    /* room for protection tracks (Burglar Bill uses 43) */
#define MAX_SIDES      2
#define MAX_SECTORS   36    /* generous limit for non-standard layouts */

/* =========================================================
 * MFM structural constants — byte counts for each field
 * Multiply by MFM_BITS_PER_BYTE to get raw MFM bit count.
 * ========================================================= */
#define MFM_BITS_PER_BYTE   16   /* 1 data byte = 16 raw MFM bits (clock+data) */

/* Track preamble */
#define MFM_GAP4A            80  /* bytes */
#define MFM_IAM_SYNC         12  /* 12 × 0x00 before IAM */
#define MFM_IAM_MARK          4  /* C2 C2 C2 FC */
#define MFM_GAP1             50  /* bytes after IAM before first IDAM */

/* Per-sector overhead (excluding data and GAP3) */
#define MFM_IDAM_SYNC        12  /* 12 × 0x00 before IDAM */
#define MFM_IDAM_MARK         4  /* A1 A1 A1 FE */
#define MFM_CHRN              4  /* C H R N fields */
#define MFM_CRC               2  /* 2-byte CRC after CHRN */
#define MFM_GAP2             22  /* bytes between IDAM CRC and DAM sync */
#define MFM_DAM_SYNC         12  /* 12 × 0x00 before DAM */
#define MFM_DAM_MARK          4  /* A1 A1 A1 FB (or F8 for deleted) */
#define MFM_DATA_CRC          2  /* 2-byte CRC after data */

/* Total fixed overhead per sector excluding data and GAP3:
 * IDAM_SYNC + IDAM_MARK + CHRN + CRC + GAP2 + DAM_SYNC + DAM_MARK + DATA_CRC
 * = 12+4+4+2+22+12+4+2 = 62 bytes */
#define MFM_SECTOR_OVERHEAD  62

/* =========================================================
 * SF-7000 hardware constants (from IPL.asm.txt)
 * ========================================================= */
#define SF7_TRACKS              40      /* standard track count */
#define SF7_SIDES                1
#define SF7_SECTORS_PER_TRACK   16
#define SF7_SECTOR_SIZE        256      /* N=1 → 128<<1 = 256 */
#define SF7_SECTOR_N             1
#define SF7_FORMAT_GPL          42      /* 0x2A — physical GAP3 written by FORMAT */
#define SF7_RW_GPL              14      /* 0x0E — timing param for READ/WRITE, not physical */
#define SF7_FILLER            0xFF

/* Drive timing (300 RPM, MFM 250 kbps) */
#define SF7_RPM                300
#define SF7_BITRATE_KBPS       250

/* Raw MFM bit counts (clock + data pairs) */
#define SF7_BITS_PER_REV    100000  /* 250000*2*60/300 = 100000 raw bits/rev */
#define SF7_BYTES_PER_REV     6250  /* data bytes per revolution */
#define SF7_CLOCKS_PER_REV  715909  /* Z80 clocks per revolution (3579545/5) */
#define SF7_CLOCKS_PER_BIT       7  /* 715909/100000 ≈ 7 Z80 clocks per raw bit */

/* SF-7000 track preamble constants — derived from HFE flux measurement.
 * The HFE of Sega Disk BASIC shows the first IDAM A1 sync at bit 1408
 * (= byte 88 from the index hole).  The 12-byte IDAM sync (0x00 × 12)
 * precedes A1, so bitIDAM = 1408 - 12×16 = 1216 bits = 76 bytes.
 * Preamble model: GAP4a(60) + IAM_SYNC(12) + IAM_MARK(4) + GAP1(0) = 76 bytes.
 * mfmPreambleBits(SF7_GAP4A, SF7_GAP1) = (60+12+4+0)×16 = 1216 bits. */
#define SF7_GAP4A               60   /* bytes of 0x4E before IAM (NEC765 FORMAT default) */
#define SF7_GAP1                 0   /* bytes of 0x4E between IAM and first IDAM sync */

/* NEC765 SPECIFY parameters from IPL (riga 1615–1619) */
#define SF7_SRT_MS               6   /* Step Rate Time */
#define SF7_HUT_MS               0   /* Head Unload Time */
#define SF7_HLT_MS              10   /* Head Load Time */

/* =========================================================
 * Format identification
 * ========================================================= */
typedef enum {
    DISK_FORMAT_NONE = 0,
    DISK_FORMAT_SF7,    /* flat raw image */
    DISK_FORMAT_EDSK,   /* Extended DSK (Amstrad/CPC) */
    DISK_FORMAT_HFE,    /* HxC Floppy Emulator v1 */
    DISK_FORMAT_DS7     /* GearSF7000 native Disk Sector Model */
} DiskImageFormat;

/* =========================================================
 * DiskSector — one decoded sector
 * ========================================================= */
typedef struct {
    /* IDAM fields — written physically in the Address Mark on disk */
    uint8_t  C;         /* Cylinder (track number written by FORMAT) */
    uint8_t  H;         /* Head (side) */
    uint8_t  R;         /* Record — sector ID, any uint8_t value (0xC1…0xC9 etc.) */
    uint8_t  N;         /* Size code: data bytes = 128 << N */

    /* Pre-baked error flags (from EDSK/HFE; 0x00 for SF7/DS7 normal sectors) */
    uint8_t  ST1;       /* NEC765 ST1 flags (ST1_DE etc.) */
    uint8_t  ST2;       /* NEC765 ST2 flags (ST2_DD, ST2_CM etc.) */

    uint8_t  damType;   /* 0xFB = normal, 0xF8 = deleted data */
    uint8_t  _pad;
    int      dataLen;   /* actual data bytes (may differ from 128<<N in EDSK extended) */
    uint8_t* data;      /* malloc'd sector data */

    /* Physical bit coordinates on the track ring (raw MFM bit units).
     * Synthesized by synthesizeSectorPositions() for SF7/EDSK/DS7.
     * Measured from the HFE bitstream for HFE-sourced tracks. */
    uint32_t bitIDAM;       /* start of A1 A1 A1 FE sync */
    uint32_t bitDAM;        /* start of A1 A1 A1 FB/F8 sync */
    uint32_t bitDataEnd;    /* first bit after data + CRC */
    uint32_t bitNextSlot;   /* start of next IDAM (= bitIDAM of next sector, or wrap) */
} DiskSector;

/* =========================================================
 * DiskTrack — one decoded track
 * ========================================================= */
typedef struct {
    int        sectorCount;
    uint32_t   totalBits;       /* ring capacity in raw MFM bits = bitsPerRevolution */
    uint32_t   usedBits;        /* bits used by preamble + all sectors */
                                /* if usedBits > totalBits: track overflows ring! */
    DiskSector sectors[MAX_SECTORS];

    /* Synthesis parameters — used by synthesizeSectorPositions() */
    uint8_t    encoding;        /* 0=MFM, 2=FM */
    uint16_t   bitrateKbps;    /* 250 for MFM DD */
    uint8_t    gap4a;           /* default MFM: MFM_GAP4A */
    uint8_t    gap1;            /* default MFM: MFM_GAP1 */
    uint8_t    gap2;            /* default MFM: MFM_GAP2 */
    uint8_t    gap3;            /* physical GAP3: SF7=42, from EDSK header, measured from HFE */
    uint8_t    fillerByte;      /* SF7: 0xFF */

    /* Raw MFM bitstream (only for HFE-sourced tracks; NULL if synthesized) */
    uint8_t*   rawMFM;          /* deinterleaved bitstream bytes for this side */
    uint32_t   rawMFMBits;      /* length in raw MFM bits */
} DiskTrack;

/* =========================================================
 * DISC_DRIVE — complete drive state
 * Replaces the old flat struct from blueMSX Disk.cpp.
 * ========================================================= */
typedef struct {
    int              enabled;
    int              readOnly;
    int              changed;       /* disk-change signal (cleared by diskChanged()) */
    int              dirty;         /* in-memory writes not yet saved to file */
    DiskImageFormat  format;

    /* File handle kept open for write-through (SF7/EDSK).
     * NULL for HFE (writes go in-memory only) and DS7 (closed after load). */
    FILE*            fileHandle;
    char             fileName[512]; /* path to source file, for save-as */

    /* Nominal geometry — for legacy API compatibility (diskGetSectorsPerTrack etc.) */
    int              sides;
    int              tracks;
    int              nominalSectorsPerTrack;
    int              nominalSectorSize;

    /* Per-track/side map — the physical track model */
    DiskTrack*       trackMap[MAX_SIDES][MAX_TRACKS];

    /* Drive physical constants */
    uint32_t         clocksPerRevolution;   /* Z80 clocks per disc revolution */
    uint32_t         bitsPerRevolution;     /* raw MFM bits per revolution */
    uint32_t         clocksPerBit;          /* Z80 clocks per raw MFM bit */
    uint8_t          encoding;              /* 0=MFM, 2=FM */
    uint16_t         bitrateKbps;
    uint16_t         rpm;
} DISC_DRIVE;

#endif /* DISKMODEL_H */
