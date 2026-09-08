/*****************************************************************************
** $Source: /cygdrive/d/Private/_SVNROOT/bluemsx/blueMSX/Src/IoDevice/NEC765.c,v $
**
** $Revision: 1.6 $
**
** $Date: 2009-07-18 15:08:04 $
**
** More info: http://www.bluemsx.com
**
** Copyright (C) 2003-2006 Daniel Vik
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
** 
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
******************************************************************************
*/
#include "NEC765.h"
//#include "Board.h"
//#include "SaveState.h"
#include "Disk.h"
#include "FdcRotationClock.h"
#include "Led.h"
//#include "FdcAudio.h"
#include "Audio.h"
#include <algorithm>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "SaveStateStream.h"

namespace
{
constexpr std::uint16_t kNEC765StateVersion = 1;
}

#define FDC765_CYCLESPERTRACK_LATENCY   12.7
//#define FDC765_CYCLESPERTRACK           (((12.0+6.7)*3579545.0)/1000.0)   //74845

#define NEC765_STARTTRACK               5

/* extern int diskOffset; — removed, not used */

struct NEC765 {
    uint8_t drive;

    uint8_t mainStatus;
    uint8_t status0;
    uint8_t status1;
    uint8_t status2;
    uint8_t status3;
    uint8_t commandCode;

    int command;
    int phase;
    int phaseStep;

    int phase2;

    uint8_t cylinderNumber;
    uint8_t side;
    uint8_t sectorNumber;
    uint8_t number;
    uint8_t oldTrack;
    uint8_t currentTrack;
    uint8_t destinationTrack;
    uint64_t destinationTrackCyclesCounter;
    uint8_t sectorsPerCylinder;

    uint8_t fillerByte;

    int    sectorSize;
    int    sectorOffset;
    uint32_t dataTransferTime;

    uint64_t deltaCycles;
    uint64_t totalCycles;
    uint32_t clockHz;

    int    interrupt;

    uint8_t  sectorBuf[4096];

    uint8_t WriteProtected;
    uint8_t StepRateTimeMs;
    uint8_t HeadUnloadTimeMs;
    uint8_t HeadLoadTimeMs;
    uint8_t NonDMAMode;

    Audio *m_pAudio;
    //FdcAudio* fdcAudio;

    /* ---- Rotational timing (Phase 2) ---- */
    uint64_t totalBits;             /* raw MFM bits accumulated since power-on */
    uint32_t rotationBits;          /* current position within one revolution */
    uint32_t bitsPerRevolution;     /* from DISC_DRIVE (default SF7_BITS_PER_REV) */
    uint64_t clocksPerRevolution;   /* NEC765 input clocks per revolution */
    FdcRotationClock rotationClock; /* fractional raw-bit accumulator */

    /* ---- Async sector search (CMD_READ_DATA / CMD_READ_ID) ---- */
    int      pendingFind;           /* 0=idle, 1=READ_DATA search, 2=READ_ID search */
    uint8_t  findR;                 /* target sector R value */
    int      findSectorIdx;         /* index in track->sectors[]; -1 = not found */
    uint64_t findCompleteBit;       /* totalBits value when the search completes */

    /* ---- FORMAT revolution timing ---- */
    int      pendingFormat;         /* 1 = waiting one revolution before raising INT */
    uint64_t formatCompleteBit;     /* totalBits value when the FORMAT revolution ends */

    /* ---- FORMAT parameter capture (for log) ---- */
    uint8_t  formatGpl;             /* GPL byte from FORMAT command phase */
    uint8_t  formatH;               /* H byte from FORMAT execution phase  */

    /* ---- FORMAT CHRN collection ---- */
    FDC_FormatEntry formatEntries[MAX_SECTORS]; /* collected CHRN data */
    int             formatEntriesCount;         /* how many collected */
};

static void nec765RefreshRotationTiming(NEC765* fdc,
                                        bool resetFraction)
{
    fdc->bitsPerRevolution = diskGetBitsPerRevolution(fdc->drive);
    if (fdc->bitsPerRevolution == 0)
        fdc->bitsPerRevolution = SF7_BITS_PER_REV;

    const uint64_t inputClock = fdc->clockHz ? fdc->clockHz : 8000000u;
    // The SF-7000 has a fixed 300 RPM physical drive. Image metadata changes
    // the number/placement of raw bits, not the spindle mechanics.
    fdc->clocksPerRevolution = (inputClock * 60u) / SF7_RPM;
    if (fdc->clocksPerRevolution == 0)
        fdc->clocksPerRevolution = SF7_CLOCKS_PER_REV;

    if (resetFraction)
        fdc->rotationClock.Reset();
    fdc->rotationBits = static_cast<uint32_t>(
        fdc->totalBits % fdc->bitsPerRevolution);
}

/* =========================================================
 * FDC diagnostic log
 * (placed here so NEC765 struct is complete)
 * ========================================================= */
#ifdef NEC765_LOG_ENABLED

#define FDC_LOG_SIZE  (512 * 1024)

static char   fdcLogBuf[FDC_LOG_SIZE];
static int    fdcLogPos    = 0;
static double fdcLogLastMs = 0.0;

static const char* fdcCmdName(int cmd)
{
    static const char* names[] = {
        "UNKNOWN",      "READ_DATA",    "WRITE_DATA",   "WRITE_DELETED",
        "READ_DELETED", "READ_DIAG",    "READ_ID",      "FORMAT",
        "SCAN_EQUAL",   "SCAN_LOW_EQ",  "SCAN_HIGH_EQ", "SEEK",
        "RECALIBRATE",  "SENSE_INT",    "SPECIFY",      "SENSE_DEV"
    };
    if (cmd >= 0 && cmd < 16) return names[cmd];
    return "?";
}

static double fdcLogMs(const NEC765* fdc)
{
    return (double)fdc->totalCycles * 1000.0 /
        (double)(fdc->clockHz ? fdc->clockHz : 8000000u);
}

static void fdcLogPrintf(const NEC765* fdc, const char* fmt, ...)
{
    if (fdcLogPos >= FDC_LOG_SIZE - 512) return;

    double ms    = fdcLogMs(fdc);
    double delta = ms - fdcLogLastMs;
    fdcLogLastMs = ms;

    int n = snprintf(fdcLogBuf + fdcLogPos, (size_t)(FDC_LOG_SIZE - fdcLogPos),
                     "[%10.3f ms  +%8.3f ms]  ", ms, delta);
    if (n > 0) fdcLogPos += n;

    if (fdcLogPos >= FDC_LOG_SIZE - 256) return;

    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(fdcLogBuf + fdcLogPos, (size_t)(FDC_LOG_SIZE - fdcLogPos), fmt, ap);
    va_end(ap);
    if (n > 0) fdcLogPos += n;
}

#else  /* NEC765_LOG_ENABLED not defined — no buffer, no overhead */
#  define fdcLogPrintf(fdc, ...) ((void)0)
#endif /* NEC765_LOG_ENABLED */

void nec765LogClear(void)
{
#ifdef NEC765_LOG_ENABLED
    fdcLogBuf[0]  = '\0';
    fdcLogPos     = 0;
    fdcLogLastMs  = 0.0;
#endif
}

bool nec765LogSave(const char* filename)
{
#ifdef NEC765_LOG_ENABLED
    FILE* f = fopen(filename, "w");
    if (!f) return false;
    fwrite(fdcLogBuf, 1, (size_t)fdcLogPos, f);
    fclose(f);
    return true;
#else
    (void)filename;
    return false;
#endif
}

void nec765LogDiskEject(int driveId)
{
#ifdef NEC765_LOG_ENABLED
    if (fdcLogPos >= FDC_LOG_SIZE - 128) return;
    int n = snprintf(fdcLogBuf + fdcLogPos, (size_t)(FDC_LOG_SIZE - fdcLogPos),
                     "=== DISK EJECT   drive=%d ===\n", driveId);
    if (n > 0) fdcLogPos += n;
#else
    (void)driveId;
#endif
}

void nec765LogDiskLoad(int driveId, const char* fileName)
{
#ifdef NEC765_LOG_ENABLED
    if (fdcLogPos >= FDC_LOG_SIZE - 256) return;
    const char* name = fileName ? fileName : "(null)";
    /* Extract just the filename from the full path */
    const char* slash = name;
    for (const char* p = name; *p; p++)
        if (*p == '/' || *p == '\\') slash = p + 1;
    int n = snprintf(fdcLogBuf + fdcLogPos, (size_t)(FDC_LOG_SIZE - fdcLogPos),
                     "=== DISK LOAD    drive=%d: \"%s\" ===\n", driveId, slash);
    if (n > 0) fdcLogPos += n;
#else
    (void)driveId; (void)fileName;
#endif
}


#define CMD_UNKNOWN                 0
#define CMD_READ_DATA               1
#define CMD_WRITE_DATA              2
#define CMD_WRITE_DELETED_DATA      3
#define CMD_READ_DELETED_DATA       4
#define CMD_READ_DIAGNOSTIC         5
#define CMD_READ_ID                 6
#define CMD_FORMAT                  7
#define CMD_SCAN_EQUAL              8
#define CMD_SCAN_LOW_OR_EQUAL       9
#define CMD_SCAN_HIGH_OR_EQUAL      10
#define CMD_SEEK                    11
#define CMD_RECALIBRATE             12
#define CMD_SENSE_INTERRUPT_STATUS  13
#define CMD_SPECIFY                 14
#define CMD_SENSE_DEVICE_STATUS     15


#define PHASE_IDLE                  0
#define PHASE_COMMAND               1
#define PHASE_DATATRANSFER          2
#define PHASE_RESULT                3
#define PHASE_SEEKING               4

#define STM_DB0    0x01
#define STM_DB1    0x02
#define STM_DB2    0x04
#define STM_DB3    0x08
#define STM_CB     0x10
#define STM_NDM    0x20
#define STM_DIO    0x40
#define STM_RQM    0x80

#define ST0_DS0    0x01
#define ST0_DS1    0x02
#define ST0_HD     0x04
#define ST0_NR     0x08
#define ST0_EC     0x10
#define ST0_SE     0x20
#define ST0_IC0    0x40
#define ST0_IC1    0x80

#define ST1_MA     0x01
#define ST1_NW     0x02
#define ST1_ND     0x04
#define ST1_OR     0x10
#define ST1_DE     0x20
#define ST1_EN     0x80

#define ST2_MD     0x01
#define ST2_BC     0x02
#define ST2_SN     0x04
#define ST2_SH     0x08
#define ST2_NC     0x10
#define ST2_DD     0x20
#define ST2_CM     0x40

#define ST3_DS0    0x01
#define ST3_DS1    0x02
#define ST3_HD     0x04
#define ST3_2S     0x08
#define ST3_TK0    0x10
#define ST3_RDY    0x20
#define ST3_WP     0x40
#define ST3_FLT    0x80



#ifdef _DEBUG

const char* CommandNames[16] = {
     "CMD_UNKNOWN"                
    ,"CMD_READ_DATA"              
    ,"CMD_WRITE_DATA"             
    ,"CMD_WRITE_DELETED_DATA"     
    ,"CMD_READ_DELETED_DATA"      
    ,"CMD_READ_DIAGNOSTIC"        
    ,"CMD_READ_ID"
    ,"CMD_FORMAT"                 
    ,"CMD_SCAN_EQUAL"             
    ,"CMD_SCAN_LOW_OR_EQUAL"      
    ,"CMD_SCAN_HIGH_OR_EQUAL"     
    ,"CMD_SEEK"                   
    ,"CMD_RECALIBRATE"            
    ,"CMD_SENSE_INTERRUPT_STATUS" 
    ,"CMD_SPECIFY"                
    ,"CMD_SENSE_DEVICE_STATUS"    
};


#endif




void boardSetFdcActive()
{
}

uint32_t boardSystemTime(NEC765* fdc)
{
    return fdc->totalCycles;
}

void nec765SetAudio(NEC765* fdc, Audio* pAudio)
{
    fdc->m_pAudio = pAudio;
    
}


void nec765SetReadOnly(NEC765* fdc, bool readonly)
{
    if (readonly)
    {
        fdc->status1 |= ST1_NW;
        fdc->status3 |= ST3_WP;
    }
    else
    {
        fdc->status1 &= ~ST1_NW;
        fdc->status3 &= ~ST3_WP;
    }
}

static void nec765EndSeek(NEC765* fdc)
{
    //fdc->mainStatus |= STM_DIO;

    //if (diskIsReadOnly(fdc->drive))
    //{
    //    fdc->status1 |= ST1_NW;
    //    fdc->status3 |= ST3_WP;
    //    //fdc->mainStatus |= STM_DIO;
    //}

    fdc->status0 |= ST0_SE;
    fdc->mainStatus &= ~STM_CB;
    fdc->interrupt = 1;

}




void nec765SetTrack(NEC765* fdc, uint8_t track) 
{
    if (fdc->currentTrack == track) return;

#ifdef _DEBUG
    //printf("[nec765SetTrack] currentTrack=%d => Move to track=%d\n",        fdc->currentTrack, track);

    //printf("[nec765SetTrack] Move head from track %2d to track=%2d\n", fdc->currentTrack, track);
    printf("[nec765SetTrack] Move head to track %2d\n", track);

#endif

    // Aggiorna oldTrack con il valore corrente
    fdc->oldTrack = fdc->currentTrack;

    // Aggiorna currentTrack con la nuova traccia
    fdc->currentTrack = track;

    fdc->m_pAudio->PlayDriveTrack(fdc->oldTrack, fdc->currentTrack);

}



bool nec765MoveToTrack(NEC765* fdc)
{
    if (fdc->currentTrack < fdc->destinationTrack)
    {
#ifdef _DEBUG
        //printf("\n[nec765MoveToTrack] Move head from track %d to track=%d\n", fdc->currentTrack, fdc->destinationTrack);
#endif

        nec765SetTrack(fdc, fdc->currentTrack + 1);
    }
    else if (fdc->currentTrack > fdc->destinationTrack)
    {
#ifdef _DEBUG
        //printf("\n[nec765MoveToTrack] Move head from track %d to track=%d\n", fdc->currentTrack, fdc->currentTrack);
#endif


        nec765SetTrack(fdc, fdc->currentTrack - 1);
    }

    // A physical step that lands on the destination completes the seek at
    // this deadline; it must not wait one additional step interval merely to
    // discover that the head was already there.
    return fdc->currentTrack == fdc->destinationTrack;
}




static uint64_t nec765CyclesPerTrack(const NEC765* fdc)
{
    const double millisPerTrack =
        static_cast<double>(fdc->StepRateTimeMs) +
        FDC765_CYCLESPERTRACK_LATENCY;
    return static_cast<uint64_t>(
        (millisPerTrack * static_cast<double>(fdc->clockHz)) / 1000.0);
}

static uint32_t nec765DataTransferLatency(const NEC765* fdc)
{
    // Keep the existing approximately 0.5 us RQM recovery time independent
    // of the clock domain used to drive the controller.
    const uint32_t clockHz = fdc->clockHz ? fdc->clockHz : 8000000u;
    return std::max<uint32_t>(1u, clockHz / 2000000u);
}

void nec765MoveToTrackTick(NEC765* fdc, uint64_t cycles)
{
    if (fdc->phase2 != PHASE_SEEKING) return;

    fdc->destinationTrackCyclesCounter += cycles;


    const uint64_t cyclesPerTrack = nec765CyclesPerTrack(fdc);
    

    // Preserve overshoot and permit a future coarse event scheduler to
    // deliver more than one step interval without slowing or losing steps.
    while (fdc->phase2 == PHASE_SEEKING &&
           cyclesPerTrack != 0 &&
           fdc->destinationTrackCyclesCounter >= cyclesPerTrack)
    {
        fdc->destinationTrackCyclesCounter -= cyclesPerTrack;

        bool ret = nec765MoveToTrack(fdc);
        if (!ret)
        {
#ifdef _DEBUG
            //printf("\n[nec765MoveToTrackTick] Phase %d, curr %d, dest %d\n", fdc->phase2, fdc->currentTrack, fdc->destinationTrack);
#endif
        }
        else
        {
            nec765EndSeek(fdc);
            fdc->phase2 = PHASE_IDLE;
            fdc->interrupt = 1;
        }
    }

}


void nec765SetupMoveToTrack(NEC765* fdc, uint8_t value)
{
    fdc->destinationTrackCyclesCounter = 0;
    fdc->destinationTrack = value;
    fdc->phase2 = PHASE_SEEKING;
}


/* =========================================================
 * Rotational position helper
 * ========================================================= */
static inline uint32_t currentBitPos(const NEC765* fdc)
{
    return (uint32_t)(fdc->totalBits % (uint64_t)fdc->bitsPerRevolution);
}

/* =========================================================
 * nec765FindSector
 *
 * Called when CMD_READ_DATA or CMD_READ_ID is initiated.
 * Computes how many raw MFM bits until the target sector's
 * IDAM passes under the head, then schedules the completion.
 *
 * pendingFind = 1: READ_DATA (searches for findR)
 * pendingFind = 2: READ_ID  (finds the NEXT sector regardless of R)
 * ========================================================= */
static void nec765FindSector(NEC765* fdc, uint8_t targetR, int pendingMode)
{
    DiskTrack* t = diskGetTrack(fdc->drive, fdc->side, fdc->currentTrack);

    fdc->pendingFind   = pendingMode;
    fdc->findR         = targetR;
    fdc->findSectorIdx = -1;

    if (!t || t->sectorCount == 0) {
        /* No track: timeout after 2 revolutions → ST1_MA */
        fdc->findCompleteBit = fdc->totalBits
            + (uint64_t)fdc->bitsPerRevolution * 2;
        return;
    }

    uint32_t curBit  = currentBitPos(fdc);
    uint32_t bpr     = fdc->bitsPerRevolution;
    uint32_t minDist = UINT32_MAX;
    int      bestIdx = -1;

    for (int i = 0; i < t->sectorCount; i++) {
        /* For READ_ID: any sector; for READ_DATA: only matching R */
        if (pendingMode == 1 && t->sectors[i].R != targetR) continue;

        uint32_t idamBit = t->sectors[i].bitIDAM % bpr;
        /* Forward distance on the ring */
        uint32_t dist = (idamBit >= curBit)
                        ? (idamBit - curBit)
                        : (bpr - curBit + idamBit);

        if (dist < minDist) { minDist = dist; bestIdx = i; }
    }

    fdc->findSectorIdx   = bestIdx;

    if (bestIdx >= 0) {
        /* Schedule completion when the DAM arrives under the head
         * (= IDAM position + bytes from IDAM to DAM, in raw MFM bits) */
        uint32_t idamToDAM = (uint32_t)(MFM_IDAM_SYNC + MFM_IDAM_MARK
                                       + MFM_CHRN + MFM_CRC
                                       + MFM_GAP2
                                       + MFM_DAM_SYNC + MFM_DAM_MARK)
                             * MFM_BITS_PER_BYTE;
        fdc->findCompleteBit = fdc->totalBits + minDist + idamToDAM;
    } else {
        /* Sector not on this track: timeout after 2 revolutions */
        fdc->findCompleteBit = fdc->totalBits
            + (uint64_t)fdc->bitsPerRevolution * 2;
    }
}

/* =========================================================
 * nec765SectorFindTick
 *
 * Called every tick.  When the scheduled bit count is reached,
 * complete the pending READ_DATA or READ_ID operation.
 * ========================================================= */
static void nec765SectorFindTick(NEC765* fdc)
{
    if (!fdc->pendingFind) return;
    if (fdc->totalBits < fdc->findCompleteBit) return;

    int mode = fdc->pendingFind;
    fdc->pendingFind = 0;

    /* ---- Timeout: sector not found ---- */
    if (fdc->findSectorIdx < 0) {
        fdc->status0 |= ST0_IC0;
        fdc->status1 |= ST1_MA;          /* Missing Address Mark */
        fdc->mainStatus |= STM_DIO;
        fdc->phase      = PHASE_RESULT;
        fdc->phaseStep  = 0;
        fdc->interrupt  = 1;
        return;
    }

    DiskTrack* t = diskGetTrack(fdc->drive, fdc->side, fdc->currentTrack);

    /* Guard: disk could have been ejected after FindSector scheduled the completion */
    if (!t) {
        fdc->status0 |= ST0_IC0;
        fdc->status1 |= ST1_MA;
        fdc->mainStatus |= STM_DIO;
        fdc->phase      = PHASE_RESULT;
        fdc->phaseStep  = 0;
        fdc->interrupt  = 1;
        return;
    }

    if (mode == 2) {
        /* ---- READ_ID result: return CHRN of the found sector ---- */
        DiskSector* sec = &t->sectors[fdc->findSectorIdx];
        fdc->cylinderNumber = sec->C;
        fdc->side           = sec->H;
        fdc->sectorNumber   = sec->R;
        fdc->number         = sec->N;
        fdc->mainStatus |= STM_DIO;
        fdc->phase      = PHASE_RESULT;
        fdc->phaseStep  = 0;
        fdc->interrupt  = 1;
        return;
    }

    /* ---- READ_DATA: load sector data into buffer ---- */
    int sectorSize = 0;
    DSKE rv = diskReadSector(fdc->drive, fdc->sectorBuf,
                             fdc->sectorNumber, fdc->side,
                             fdc->currentTrack, 0, &sectorSize);
    fdc->sectorSize   = sectorSize;
    fdc->sectorOffset = 0;

    if (rv == DSKE_NO_DATA) {
        fdc->status0 |= ST0_IC0;
        fdc->status1 |= ST1_MA;
    } else if (rv == DSKE_CRC_ERROR) {
        fdc->status0 |= ST0_IC0;
        fdc->status1 |= ST1_DE;
        fdc->status2 |= ST2_DD;
    }

    boardSetFdcActive();
    fdc->mainStatus |= STM_DIO;
    fdc->phase      = PHASE_DATATRANSFER;
    fdc->phaseStep  = 0;
    fdc->interrupt  = 1;
}

/* =========================================================
 * nec765FormatTick
 *
 * Called every tick.  When the scheduled FORMAT revolution is complete
 * (one full revolution after the last CHRN byte was received), enters
 * PHASE_RESULT and raises INT — exactly as the real NEC765 does.
 * ========================================================= */
static void nec765FormatTick(NEC765* fdc)
{
    if (!fdc->pendingFormat) return;
    if (fdc->totalBits < fdc->formatCompleteBit) return;

    fdcLogPrintf(fdc, "FORMAT  revolution complete  track=%d  SC=%d\n",
                 fdc->currentTrack, fdc->sectorsPerCylinder);

    /* Build the track in the disk model from the collected CHRN data */
    diskSetFormattedTrack(fdc->drive, fdc->side, fdc->currentTrack,
                          fdc->formatEntries, fdc->formatEntriesCount,
                          fdc->fillerByte, fdc->formatGpl);

    fdc->pendingFormat = 0;
    fdc->phase         = PHASE_RESULT;
    fdc->phaseStep     = 0;
    fdc->mainStatus   |= STM_DIO;
    fdc->interrupt     = 1;
}

/* =========================================================
 * nec765Tick — updated to track rotational position
 * ========================================================= */
void nec765Tick(NEC765* fdc, uint64_t clockCycles, bool rotationActive)
{
    fdc->deltaCycles  = clockCycles;
    fdc->totalCycles += clockCycles;

    /* Advance the spinning medium with a persistent rational remainder.
     * This keeps segmentation exact and also supports image/timebase ratios
     * that do not divide into an integer number of input clocks per bit. */
    if (rotationActive) {
        fdc->totalBits += fdc->rotationClock.Advance(
            clockCycles, fdc->bitsPerRevolution,
            fdc->clocksPerRevolution);
        fdc->rotationBits = (uint32_t)(fdc->totalBits % fdc->bitsPerRevolution);
    }

    nec765MoveToTrackTick(fdc, clockCycles);
    nec765SectorFindTick(fdc);
    nec765FormatTick(fdc);
}

static uint64_t nec765ClocksUntilRawBits(const NEC765* fdc,
                                         uint64_t rawBits)
{
    return fdc->rotationClock.ClocksUntilBits(
        rawBits, fdc->bitsPerRevolution, fdc->clocksPerRevolution);
}

uint64_t nec765ClocksToNextEvent(NEC765* fdc, bool rotationActive)
{
    if (!fdc)
        return UINT64_MAX;

    uint64_t result = UINT64_MAX;
    if (fdc->phase2 == PHASE_SEEKING)
    {
        const uint64_t step = nec765CyclesPerTrack(fdc);
        const uint64_t remaining =
            fdc->destinationTrackCyclesCounter < step
                ? step - fdc->destinationTrackCyclesCounter
                : 0;
        result = remaining;
    }

    if (rotationActive && fdc->pendingFind)
    {
        const uint64_t bits = fdc->findCompleteBit > fdc->totalBits
            ? fdc->findCompleteBit - fdc->totalBits : 0;
        result = std::min(result, nec765ClocksUntilRawBits(fdc, bits));
    }

    if (rotationActive && fdc->pendingFormat)
    {
        const uint64_t bits = fdc->formatCompleteBit > fdc->totalBits
            ? fdc->formatCompleteBit - fdc->totalBits : 0;
        result = std::min(result, nec765ClocksUntilRawBits(fdc, bits));
    }
    return result;
}


static uint8_t nec765ExecutionPhasePeek(NEC765* fdc)
{
	switch (fdc->command) 
    {
	    case CMD_READ_DATA:
		    if (fdc->sectorOffset < fdc->sectorSize) {
			    return fdc->sectorBuf[fdc->sectorOffset];
            }
            break;
    }
    return 0xff;
}


static uint8_t nec765ExecutionPhaseRead(NEC765* fdc)
{
	switch (fdc->command) {
	case CMD_READ_DATA:
		if (fdc->sectorOffset < fdc->sectorSize) {
			uint8_t value = fdc->sectorBuf[fdc->sectorOffset++];
    		if (fdc->sectorOffset == fdc->sectorSize) {
                fdc->phase = PHASE_RESULT;
                fdc->phaseStep = 0;
                fdc->interrupt = 1;
            }
            return value;
        }
        break;
    }

    return 0xff;
}

static uint8_t nec765ResultsPhasePeek(NEC765* fdc)
{
	switch (fdc->command) {
	case CMD_READ_ID:
	case CMD_READ_DATA:
	case CMD_WRITE_DATA:
    case CMD_FORMAT:
		switch(fdc->phaseStep) {
		case 0:
            return fdc->status0;
		case 1:
            return fdc->status1 
                | (diskIsReadOnly(fdc->drive) ? ST1_NW : 0);
        case 2:
            return fdc->status2;
		case 3:
            return fdc->cylinderNumber;
		case 4:
            return fdc->side;
		case 5:
            return fdc->sectorNumber;
		case 6:
            return fdc->number;
		}
		break;

    case CMD_SENSE_INTERRUPT_STATUS:
		switch (fdc->phaseStep) {
		case 0:
            return fdc->status0;
		case 1:
            return fdc->currentTrack;
		}
		break;

    case CMD_SENSE_DEVICE_STATUS:
		switch (fdc->phaseStep) {
		case 0:
            return fdc->status3
                | (diskIsReadOnly(fdc->drive) ? ST3_WP : 0)
                | (diskPresent(fdc->drive) ? ST3_RDY : 0);
        }
		break;
    }

    return 0xff;
}

static uint8_t nec765ResultsPhaseRead(NEC765* fdc)
{
	switch (fdc->command) {
	case CMD_READ_ID:
	case CMD_READ_DATA:
	case CMD_WRITE_DATA:
    case CMD_FORMAT:
		switch	(fdc->phaseStep++) {
		case 0:
//#ifdef _DEBUG
//            printf("FDC: Status 0: $%02X\n", fdc->status0);
//#endif
            return fdc->status0;
		case 1:
//#ifdef _DEBUG
//            printf("FDC: Status 1: $%02X\n", fdc->status1);
//#endif
            return fdc->status1
                    | (diskIsReadOnly(fdc->drive) ? ST1_NW : 0);
		case 2:
//#ifdef _DEBUG
//            printf("FDC: Status 2: $%02X\n", fdc->status2);
//#endif
            return fdc->status2;
		case 3:
            return fdc->cylinderNumber;
		case 4:
            return fdc->side;
		case 5:
            return fdc->sectorNumber;
		case 6:
			fdc->phase       = PHASE_IDLE;
            fdc->mainStatus &= ~STM_CB;
            fdc->mainStatus &= ~STM_DIO;

//#ifdef _DEBUG
//            printf("FDC: Status 3: $%02X\n", fdc->status3);
//            printf("FDC: M.Status: $%02X\n\n", fdc->mainStatus);
//#endif

            return fdc->number;
		}
		break;

    case CMD_SENSE_INTERRUPT_STATUS:
		switch (fdc->phaseStep++) {
		case 0:
            return fdc->status0;
		case 1:
			fdc->phase       = PHASE_IDLE;
            fdc->mainStatus &= ~(STM_CB | STM_DIO);

            return fdc->currentTrack;
		}
		break;

    case CMD_SENSE_DEVICE_STATUS:
		switch (fdc->phaseStep++) {
		case 0:
			fdc->phase       = PHASE_IDLE;
            fdc->mainStatus &= ~(STM_CB | STM_DIO);
            
            return fdc->status3
                | (diskIsReadOnly(fdc->drive) ? ST3_WP : 0)
                | (diskPresent(fdc->drive) ? ST3_RDY : 0);
        }
		break;
    }

    return 0xff;
}

void nec765IdlePhaseWrite(NEC765* fdc, uint8_t value)
{
	fdc->command = CMD_UNKNOWN;
	if ((value & 0x1f) == 0x06) fdc->command = CMD_READ_DATA;
	if ((value & 0x3f) == 0x05) fdc->command = CMD_WRITE_DATA;
	if ((value & 0x3f) == 0x09) fdc->command = CMD_WRITE_DELETED_DATA;
	if ((value & 0x1f) == 0x0c) fdc->command = CMD_READ_DELETED_DATA;
	if ((value & 0xbf) == 0x02) fdc->command = CMD_READ_DIAGNOSTIC;
	if ((value & 0xbf) == 0x0a) fdc->command = CMD_READ_ID;
	if ((value & 0xbf) == 0x0d) fdc->command = CMD_FORMAT;
	if ((value & 0x1f) == 0x11) fdc->command = CMD_SCAN_EQUAL;
	if ((value & 0x1f) == 0x19) fdc->command = CMD_SCAN_LOW_OR_EQUAL;
	if ((value & 0x1f) == 0x1d) fdc->command = CMD_SCAN_HIGH_OR_EQUAL;
	if ((value & 0xff) == 0x0f) fdc->command = CMD_SEEK;
	if ((value & 0xff) == 0x07) fdc->command = CMD_RECALIBRATE;
	if ((value & 0xff) == 0x08) fdc->command = CMD_SENSE_INTERRUPT_STATUS;
	if ((value & 0xff) == 0x03) fdc->command = CMD_SPECIFY;
	if ((value & 0xff) == 0x04) fdc->command = CMD_SENSE_DEVICE_STATUS;

#ifdef _DEBUG
    printf("\t[%s] nec765IdlePhaseWrite value = %d\n", CommandNames[fdc->command], value);
#endif

    fdcLogPrintf(fdc, "CMD  %-16s  byte=0x%02X\n", fdcCmdName(fdc->command), value);

    fdc->commandCode = value;

	fdc->phase       = PHASE_COMMAND;
	fdc->phaseStep   = 1;
    fdc->mainStatus |= STM_CB;
    
    switch (fdc->command) {
	case CMD_READ_DATA:
	case CMD_WRITE_DATA:
	case CMD_FORMAT:
        fdc->status0 &= ~(ST0_IC0 | ST0_IC1);
        fdc->status1 &= ~(ST1_ND | ST1_NW);
		fdc->status2 &= ~ST2_DD;
        break;

	case CMD_RECALIBRATE:
        fdc->status0 &= ~ST0_SE;
		break;

	case CMD_SENSE_INTERRUPT_STATUS:
        if (fdc->interrupt) {
            fdc->mainStatus |= STM_DIO;
            fdc->phase       = PHASE_RESULT;
            fdc->phaseStep   = 0;
            fdc->interrupt   = 1;
        } else {
            /* No interrupt pending: return to IDLE with DIO=0.
             * IPL fill_fdc_command_result_buffer sees DIO=0 and exits. */
            fdc->mainStatus &= ~STM_CB;
            fdc->phase       = PHASE_IDLE;
        }
        break;

    case CMD_SEEK:
        break;
	case CMD_SPECIFY:
        break;
	case CMD_SENSE_DEVICE_STATUS:
        break;

    case CMD_READ_ID:
        /* CMD_READ_ID takes 1 parameter byte (HD/DS).  The command byte
         * itself encodes MFM flag (bit 6).  Phase completes in phaseStep 1. */
        break;

    default:
        fdc->mainStatus &= ~STM_CB;
		fdc->phase       = PHASE_IDLE;
        fdc->interrupt   = 1;
    }
}

static void nec765CommandPhaseWrite(NEC765* fdc, uint8_t value)
{
#ifdef _DEBUG
//    printf("\t[%s] nec765CommandPhaseWrite value = %d\n", CommandNames[fdc->command], value);
#endif


    switch (fdc->command) {
	case CMD_READ_DATA:
	case CMD_WRITE_DATA:
		switch (fdc->phaseStep++) {
		case 0:
			break;
        case 1:
            fdc->drive = value & 0x03;
            fdc->side = (value >> 2)& 1;
            fdc->sectorSize = diskGetSectorSize(fdc->drive, fdc->side, fdc->currentTrack, 0);
            
            fdc->status0 &= ~(ST0_DS0 | ST0_DS1 | ST0_IC0 | ST0_IC1);
            fdc->status0 |= (diskPresent(fdc->drive) ? 0 : ST0_DS0) | (value & (ST0_DS0 | ST0_DS1)) |
                           (diskIsEnabled(fdc->drive) ? 0 : ST0_IC1);
            fdc->status3  =     (value & (ST3_DS0 | ST3_DS1)) | 
                                (fdc->currentTrack == 0        ? ST3_TK0 : 0) 
                            |   (diskGetSides(fdc->drive) == 2 ? ST3_HD  : 0) 
                            |   (diskIsReadOnly(fdc->drive)    ? ST3_WP  : 0) 
                            |   (diskPresent(fdc->drive)       ? ST3_RDY : 0);

            
            break;
		case 2:
            fdc->cylinderNumber = value;
			break;
		case 4:
            fdc->sectorNumber = value;
			break;
		case 5:
            fdc->number = value;
            if (fdc->command == CMD_WRITE_DATA) {
                /* Use actual size from N, always start at offset 0 */
                fdc->sectorSize   = 128 << value;
                fdc->sectorOffset = 0;
            }
			break;
		case 8:
            if (fdc->command == CMD_READ_DATA) {
                /* Synchronous sector load: the IPL starts reading via INI
                 * immediately after the last command byte — no rotational
                 * latency is tolerated.  Load the sector now. */
                int sectorSize = 0;
                DSKE rv = diskReadSector(fdc->drive, fdc->sectorBuf,
                                         fdc->sectorNumber, fdc->side,
                                         fdc->currentTrack, 0, &sectorSize);
                fdc->sectorSize   = sectorSize;
                fdc->sectorOffset = 0;

                if (rv == DSKE_NO_DATA) {
                    fdc->status0 |= ST0_IC0;
                    fdc->status1 |= ST1_MA;
                } else if (rv == DSKE_CRC_ERROR) {
                    fdc->status0 |= ST0_IC0;
                    fdc->status1 |= ST1_DE;
                    fdc->status2 |= ST2_DD;
                }

                boardSetFdcActive();
                fdc->mainStatus |= STM_DIO;
                fdc->phase      = PHASE_DATATRANSFER;
                fdc->phaseStep  = 0;
                fdc->interrupt  = 1;
            }
            else {
                /* WRITE_DATA: go straight to transfer phase */
                fdc->mainStatus &= ~STM_DIO;
                fdc->phase      = PHASE_DATATRANSFER;
                fdc->phaseStep  = 0;
                fdc->interrupt  = 1;
            }
			break;
		}
		break;

	case CMD_FORMAT:
		switch (fdc->phaseStep++) {
		case 0:
			break;
        case 1:
            fdc->drive = value & 0x03;
            fdc->side = (value >> 2)& 1;
            fdc->sectorSize = diskGetSectorSize(fdc->drive, fdc->side, fdc->currentTrack, 0);

            fdc->status0 &= ~(ST0_DS0 | ST0_DS1 | ST0_IC0 | ST0_IC1);
            fdc->status0 |= (diskPresent(fdc->drive) ? 0 : ST0_DS0) | (value & (ST0_DS0 | ST0_DS1)) |
                           (diskIsEnabled(fdc->drive) ? 0 : ST0_IC1);
            fdc->status3  = (value & (ST3_DS0 | ST3_DS1)) |
                           (fdc->currentTrack == 0        ? ST3_TK0 : 0) |
                           (diskGetSides(fdc->drive) == 2 ? ST3_HD  : 0) |
                           (diskIsReadOnly(fdc->drive)      ? ST3_WP  : 0) |
                           (diskPresent(fdc->drive)       ? ST3_RDY : 0);
            fdcLogPrintf(fdc, "  FORMAT cmd  drive=%d side=%d\n", fdc->drive, fdc->side);
            break;
		case 2:
            fdc->number = value;
            fdcLogPrintf(fdc, "  FORMAT cmd  N=%d  sectorSize=%d B\n", value, 128 << value);
			break;
		case 3:
            fdc->sectorsPerCylinder = value;
            fdc->sectorNumber       = value;
            fdcLogPrintf(fdc, "  FORMAT cmd  SC=%d\n", value);
			break;
        case 4:
            fdc->formatGpl = value;
            fdcLogPrintf(fdc, "  FORMAT cmd  GPL=%d\n", value);
            break;
		case 5:
            fdc->fillerByte         = value;
            fdc->sectorOffset       = 0;
            fdc->formatEntriesCount = 0;   /* start fresh CHRN collection */
            fdc->mainStatus  &= ~STM_DIO;
			fdc->phase        = PHASE_DATATRANSFER;
			fdc->phaseStep    = 0;
            fdc->interrupt    = 1;
            fdcLogPrintf(fdc, "  FORMAT cmd  D=0x%02X  → exec phase (track=%d SC=%d N=%d GPL=%d)\n",
                         value, fdc->currentTrack, fdc->sectorsPerCylinder,
                         fdc->number, fdc->formatGpl);
			break;
        }
        break;

	case CMD_SEEK:
		switch (fdc->phaseStep++) {
		case 0:
			break;
        case 1:
            fdc->drive = value & 0x03;
            fdc->side = (value >>2)& 1;

            fdc->status0 &= ~(ST0_DS0 | ST0_DS1 | ST0_IC0 | ST0_IC1);
            fdc->status0 |= (diskPresent(fdc->drive) ? 0 : ST0_DS0) | (value & (ST0_DS0 | ST0_DS1)) |
                            (diskIsEnabled(fdc->drive) ? 0 : ST0_IC1);
            fdc->status3  = (value & (ST3_DS0 | ST3_DS1)) |
                            (fdc->currentTrack == 0        ? ST3_TK0 : 0) |
                            (diskGetSides(fdc->drive) == 2 ? ST3_HD  : 0) |
                            (diskIsReadOnly(fdc->drive)      ? ST3_WP  : 0) |
                            (diskPresent(fdc->drive)       ? ST3_RDY : 0);
            break;
		case 2:
            fdcLogPrintf(fdc, "  SEEK  track=%d → %d\n", fdc->currentTrack, value);
            fdc->phase = PHASE_IDLE;
            fdc->phaseStep = 0;

            nec765SetupMoveToTrack(fdc, value);

            //nec765SetTrack(fdc, value);
            //nec765EndSeek(fdc);
			break;
		}
		break;

	case CMD_READ_ID:
		switch (fdc->phaseStep++) {
		case 0:
            break;
        case 1:
            /* value = HD | DS.  We only need HD (bit 2) and DS (bits 0-1). */
            fdc->drive = value & 0x03;
            fdc->side  = (value >> 2) & 1;
            fdc->phase = PHASE_IDLE;   /* stays IDLE while searching */
            fdc->phaseStep = 0;
            /* Schedule async search: find the next IDAM in rotation */
            nec765FindSector(fdc, 0, 2 /* READ_ID mode */);
            break;
        }
        break;

	case CMD_RECALIBRATE:
		switch (fdc->phaseStep++)
        {
		case 0:
            break;
        case 1:
#ifdef _DEBUG
            printf("\n[CMD_RECALIBRATE] Move head from track %2d to track %2d\n\n", fdc->currentTrack, value);
#endif
            fdcLogPrintf(fdc, "  RECALIBRATE  track=%d → 0\n", fdc->currentTrack);
            fdc->drive = value & 0x03;
            
            fdc->status0 &= ~(ST0_DS0 | ST0_DS1 | ST0_IC0 | ST0_IC1);
            fdc->status0 |= (diskPresent(fdc->drive) ? 0 : ST0_DS0) | (value & (ST0_DS0 | ST0_DS1)) |
                           (diskIsEnabled(fdc->drive) ? 0 : ST0_IC1);
            fdc->status3  = (value & (ST3_DS0 | ST3_DS1)) | 
                           (fdc->currentTrack == 0        ? ST3_TK0 : 0) | 
                           (diskGetSides(fdc->drive) == 2 ? ST3_HD  : 0) |  
                           (diskIsReadOnly(fdc->drive)      ? ST3_WP  : 0) |
                           (diskPresent(fdc->drive)       ? ST3_RDY : 0);
            
            fdc->phase = PHASE_IDLE;
            fdc->phaseStep = 0;

            nec765SetupMoveToTrack(fdc, value);

            //nec765SetTrack(fdc, 0);
            //nec765EndSeek(fdc);
			break;
		}
		break;

	case CMD_SPECIFY:
#ifdef _DEBUG
        printf("\n[CMD_SPECIFY] Calling with value=%02x, phaseStep=%d\n", value, fdc->phaseStep);
#endif
		switch (fdc->phaseStep++) 
        {
            case 0:
#ifdef _DEBUG
                printf("\n[CMD_SPECIFY]:");
#endif
                break;

            case 1:
#ifdef _DEBUG
                printf("\n\tSRT (4 bit) | HUT (4 bit) value=$%02X ", value);
                printf("\n\t\tStep Rate Time:   \t%d ms", (value >> 4) * 1);
                printf("\n\t\tHead Unload Time: \t%d ms", (value & 15) * 16);
                printf("\n");
#endif
                fdc->StepRateTimeMs = (value >> 4) * 1;
                fdc->HeadUnloadTimeMs = (value & 15) * 16;
                fdcLogPrintf(fdc, "  SPECIFY  SRT=%d ms  HUT=%d ms\n",
                             fdc->StepRateTimeMs, fdc->HeadUnloadTimeMs);
                break;

            case 2:
#ifdef _DEBUG
                printf("\n\tHLT (7 bit) | ND  (1 bit) value=$%02X", value);

                printf("\n\t\tHead Load Time: \t%d ms", (value >> 1) * 4);
                printf("\n\t\tNon-DMA Mode:   \t%d", (value & 1));
                printf("\n");
#endif

                fdc->HeadLoadTimeMs = (value >> 1) * 4;
                fdc->NonDMAMode = (value & 1);
                fdcLogPrintf(fdc, "  SPECIFY  HLT=%d ms  ND=%d\n",
                             fdc->HeadLoadTimeMs, fdc->NonDMAMode);

                fdc->mainStatus &= ~STM_CB;
			    fdc->phase       = PHASE_IDLE;
                fdc->interrupt   = 1;
			    break;
		}
		break;


	case CMD_SENSE_DEVICE_STATUS:
		switch (fdc->phaseStep++) {
		case 1:
            fdc->drive = value & 0x03;
            fdc->side = (value >>2)& 1;
            
            fdc->status0 &= ~(ST0_DS0 | ST0_DS1 | ST0_IC0 | ST0_IC1);
            fdc->status0 |= (diskPresent(fdc->drive) ? 0 : ST0_DS0) | (value & (ST0_DS0 | ST0_DS1)) |
                           (diskIsEnabled(fdc->drive) ? 0 : ST0_IC1);
            fdc->status3  = (value & (ST3_DS0 | ST3_DS1)) | 
                           (fdc->currentTrack == 0        ? ST3_TK0 : 0) | 
                           (diskGetSides(fdc->drive) == 2 ? ST3_HD  : 0) |  
                           (diskIsReadOnly(fdc->drive)      ? ST3_WP  : 0) |
                           (diskPresent(fdc->drive)       ? ST3_RDY : 0);
            
            fdc->mainStatus |= STM_DIO;
		    fdc->phase       = PHASE_RESULT;
            fdc->phaseStep   = 0;
            fdc->interrupt   = 1;
			break;
		}
		break;
	}
}

static void nec765ExecutionPhaseWrite(NEC765* fdc, uint8_t value)
{
#ifdef _DEBUG
    //printf("\t[%s] nec765ExecutionPhaseWrite value = %d\n", CommandNames[fdc->command], value);
#endif
    int rv;

	switch (fdc->command) 
    {
	case CMD_WRITE_DATA:
		if (fdc->sectorOffset < fdc->sectorSize) {
			fdc->sectorBuf[fdc->sectorOffset++] = value;

    		if (fdc->sectorOffset == fdc->sectorSize) {
                rv = diskWriteSector(fdc->drive, fdc->sectorBuf, fdc->sectorNumber, fdc->side,
                                     fdc->currentTrack, 0);
                fdcLogPrintf(fdc, "WRITE_DATA  track=%d side=%d R=0x%02X N=%d  result=%s\n",
                             fdc->currentTrack, fdc->side, fdc->sectorNumber, fdc->number,
                             rv ? "OK" : "FAIL");
                if (!rv) {
                    fdc->status1 |= ST1_NW;

                    if (diskIsReadOnly(fdc->drive))
                    {
                        fdc->status3 |= ST3_WP;
                        fdc->status0 |= ST0_IC0;
                    }
                }


                //fdcAudioSetReadWrite(fdc->fdcAudio);

                boardSetFdcActive();

                fdc->phase       = PHASE_RESULT;
                fdc->phaseStep   = 0;
                fdc->mainStatus |= STM_DIO;
            }
        }
		break;

    case CMD_FORMAT:
        switch(fdc->phaseStep & 3) {
        case 0:  /* C — cylinder */
            nec765SetTrack(fdc, value);
#ifdef _DEBUG
            printf("\n[CMD_FORMAT] Calling nec765SetTrack with track %d\n", value);
#endif
            break;
        case 1:  /* H — head/side */
            fdc->formatH = value;
            memset(fdc->sectorBuf, fdc->fillerByte, fdc->sectorSize);
            rv = diskWrite(fdc->drive, fdc->sectorBuf, fdc->sectorNumber - 1 +
                      diskGetSectorsPerTrack(fdc->drive) * (fdc->currentTrack * diskGetSides(fdc->drive) + value));
            if (!rv) {
                fdc->status1 |= ST1_NW;

                if (diskIsReadOnly(fdc->drive))
                {
                    fdc->status3 |= ST3_WP;
                    fdc->status0 |= ST0_IC0;
                }
            }
            boardSetFdcActive();
            break;
        case 2:  /* R — sector ID */
            fdc->sectorNumber = value;
            break;
        case 3:  /* N — size code; complete CHRN received */
            if (fdc->formatEntriesCount < MAX_SECTORS) {
                FDC_FormatEntry* e = &fdc->formatEntries[fdc->formatEntriesCount++];
                e->C = fdc->currentTrack;
                e->H = fdc->formatH;
                e->R = fdc->sectorNumber;
                e->N = value;
            }
            fdcLogPrintf(fdc, "  FORMAT CHRN[%2d]  C=%d H=%d R=0x%02X N=%d  sectorSize=%d B\n",
                         fdc->phaseStep / 4,
                         fdc->currentTrack, fdc->formatH,
                         fdc->sectorNumber, value,
                         128 << value);
            break;
        }

        if (++fdc->phaseStep == 4 * fdc->sectorsPerCylinder) {
            /* All CHRN bytes received.  Wait one full revolution before
             * raising INT — same timing as the real NEC765 (~200 ms at
             * 300 RPM).  nec765FormatTick() completes the transition. */
            fdc->pendingFormat     = 1;
            fdc->formatCompleteBit = fdc->totalBits
                                   + (uint64_t)fdc->bitsPerRevolution;
            fdc->phase             = PHASE_IDLE;
            fdc->mainStatus       &= ~STM_DIO;
            /* CB stays set (STM_CB was set at command start) so the
             * CPU knows the FDC is still busy. */
        }
        break;
	}
}






















NEC765* nec765Create()
{
    NEC765* fdc = (NEC765*)malloc(sizeof(NEC765));

    //fdc->fdcAudio = fdcAudioCreate(FA_PANASONIC);
    fdc->currentTrack = NEC765_STARTTRACK;
    fdc->destinationTrack = 0;

    

    fdc->StepRateTimeMs = 6;
    fdc->HeadUnloadTimeMs = 0;
    fdc->HeadLoadTimeMs = 20;
    fdc->NonDMAMode = 1;
    fdc->clockHz = 8000000u;
    fdc->totalCycles = 0;
    fdc->deltaCycles = 0;
    fdc->totalBits = 0;
    fdc->rotationBits = 0;
    fdc->rotationClock.Reset();

    nec765Reset(fdc);

    return fdc;
}

void nec765Destroy(NEC765* fdc)
{
    //fdcAudioDestroy(fdc->fdcAudio);
    free(fdc);
}


void nec765Restart(NEC765* fdc) 
{

#ifdef _DEBUG
    //printf("[nec765SetTCSignal] TC signal activated. Finalizing current operation.\n");
#endif

    // Segnale di interruzione
    fdc->interrupt = 1;

    // Finalizza la fase corrente
    fdc->phase = PHASE_RESULT;
    fdc->phaseStep = 0;

    // Aggiorna lo stato principale
    fdc->mainStatus |= STM_RQM;  // Richiesta di dati pronta
    fdc->mainStatus &= ~STM_CB; // Comando completato

    // Resetta l'offset del settore
    fdc->sectorOffset = 0;
}

void nec765Reset(NEC765* fdc) {

    fdc->drive = 0;

    fdc->phase     = PHASE_IDLE;
    fdc->phaseStep = 0;
    fdc->phase2    = PHASE_IDLE;

    fdc->commandCode = 0;
    fdc->command     = 0;
    fdc->sectorOffset = 0;

    fdc->status0 = 0;
    fdc->status1 = 0;
    fdc->status2 = 0;
    fdc->status3 = 0;

    fdc->cylinderNumber = 0;
    fdc->side           = 0;
    fdc->sectorNumber   = 0;
    fdc->number         = 0;

    // A uPD765 reset clears its protocol state, not the drive mechanics.
    // The head stays where it physically is; RECALIBRATE moves it to track 0
    // one step at a time.  A reset only cancels a pending seek in place.
    fdc->destinationTrack = fdc->currentTrack;
    fdc->sectorsPerCylinder = 0;
    fdc->destinationTrackCyclesCounter = 0;

    fdc->fillerByte   = 0;
    fdc->sectorSize   = 0;
    fdc->sectorOffset = 0;
    fdc->dataTransferTime = 0;

    memset(fdc->sectorBuf, 0, sizeof(uint8_t) * 4096);

    fdc->mainStatus = STM_NDM | STM_RQM;
    fdc->interrupt  = 0;

    ledSetFdd1(0);
    ledSetFdd2(0);

    // A controller reset cancels protocol operations but does not reset the
    // spindle phase or the machine timebase. SF7000::Reset performs the
    // stronger power-reset operation explicitly through nec765ResetTimebase.
    fdc->deltaCycles = 0;
    nec765RefreshRotationTiming(fdc, false);

    /* Async find state */
    fdc->pendingFind      = 0;
    fdc->findR            = 0;
    fdc->findSectorIdx    = -1;
    fdc->findCompleteBit  = 0;

    /* FORMAT revolution timing */
    fdc->pendingFormat     = 0;
    fdc->formatCompleteBit = 0;
    fdc->formatGpl         = 0;
    fdc->formatH           = 0;

    /* FORMAT CHRN collection */
    fdc->formatEntriesCount = 0;
    memset(fdc->formatEntries, 0, sizeof(fdc->formatEntries));
}

void nec765SetClockRate(NEC765* fdc, uint32_t clockHz)
{
    if (!fdc || clockHz == 0)
        return;
    fdc->clockHz = clockHz;
    nec765RefreshRotationTiming(fdc, true);
}

void nec765ResetTimebase(NEC765* fdc)
{
    if (!fdc)
        return;
    fdc->totalCycles = 0;
    fdc->deltaCycles = 0;
    fdc->totalBits = 0;
    fdc->rotationBits = 0;
    fdc->rotationClock.Reset();
    nec765RefreshRotationTiming(fdc, false);
}


void nec765SetTCSignal(NEC765* fdc, int val)
{
    if (val)
    {
        //FDC765_Restart();
        nec765Restart(fdc);

        //FDC765_SetIntSignal(1);
        fdc->interrupt = 1;
    }
}


uint8_t nec765Read(NEC765* fdc)
{
    uint8_t value;
    
    fdc->interrupt = 0;

    switch (fdc->phase) 
    {            
	    case PHASE_DATATRANSFER:
            value = nec765ExecutionPhaseRead(fdc);
            fdc->dataTransferTime = boardSystemTime(fdc);
            fdc->mainStatus &= ~STM_RQM;
            return value;

	    case PHASE_RESULT:
            return nec765ResultsPhaseRead(fdc);
    }
    return 0xff;
}

uint8_t nec765Peek(NEC765* fdc)
{
    switch (fdc->phase) {            
	case PHASE_DATATRANSFER:
        return nec765ExecutionPhasePeek(fdc);
	case PHASE_RESULT:
        return nec765ResultsPhasePeek(fdc);
    }
    return 0xff;
}

uint8_t nec765ReadStatus(NEC765* fdc)
{
    if (~fdc->mainStatus & STM_RQM) {
        uint32_t elapsed = boardSystemTime(fdc) - fdc->dataTransferTime;
        if (elapsed >= nec765DataTransferLatency(fdc)) {
            fdc->mainStatus |= STM_RQM;

            //if (diskIsReadOnly(fdc->drive))
            //{
            //    fdc->status1 |= ST1_NW;
            //    fdc->status3 |= ST3_WP;
            //    fdc->mainStatus |= STM_DIO;
            //}

        } 
    }
    return fdc->mainStatus;
}

uint8_t nec765PeekStatus(NEC765* fdc)
{
    return fdc->mainStatus;
}

void nec765Write(NEC765* fdc, uint8_t value)
{
    //ledSetFdd1((value & 0x10) && diskEnabled(0)); /* 10:10 2004/10/09 FDD LED PATCH */
    //ledSetFdd2((value & 0x20) && diskEnabled(1)); /* 10:10 2004/10/09 FDD LED PATCH */

    /* Ignore writes while a FORMAT revolution is in progress.
     * The real NEC765 does not accept new commands until the revolution
     * completes and INT is raised. */
    if (fdc->pendingFormat) return;

    switch (fdc->phase)
    {
	case PHASE_IDLE:
#ifdef _DEBUG
        printf("\nPHASE_IDLE\n");
#endif
        nec765IdlePhaseWrite(fdc, value);
        break;

    case PHASE_COMMAND:
//#ifdef _DEBUG
//        printf("\nPHASE_COMMAND\n");
//#endif
        nec765CommandPhaseWrite(fdc, value);
        break;
        
	case PHASE_DATATRANSFER:
//#ifdef _DEBUG
//        printf("\nPHASE_DATATRANSFER\n");
//#endif
        nec765ExecutionPhaseWrite(fdc, value);
        fdc->dataTransferTime = boardSystemTime(fdc);
        fdc->mainStatus &= ~STM_RQM;
        break;
    }
}



int nec765GetInt(NEC765* fdc)
{
    return fdc->interrupt;
}

int nec765IsDiskPresent(NEC765* fdc)
{
    if (diskPresent(fdc->drive)) {
            return 1;
    }

    return 0;
}

int nec765IsDiskEnabled(NEC765* fdc)
{
    if (diskIsEnabled(fdc->drive)) {
        return 1;
    }

    return 0;
}

int nec765GetIndex(NEC765* fdc)
{
    if (diskIsEnabled(fdc->drive) && diskPresent(fdc->drive))
    {
        /* Index pulse is high for approximately 4 ms at the start of each
         * revolution.  At 300 RPM and 100,000 raw MFM bits/rev, 4 ms ≈ 2000 bits. */
        if (fdc->rotationBits < 2000u) {
            return 1;
        }
    }
    return 0;
}

/* New: expose rotational position for GUI disk animation */
uint32_t nec765GetRotationBits(NEC765* fdc)
{
    return fdc->rotationBits;
}

uint32_t nec765GetProjectedRotationBits(NEC765* fdc,
                                       uint64_t pendingClocks,
                                       bool rotationActive)
{
    if (!fdc || !rotationActive || pendingClocks == 0 ||
        fdc->bitsPerRevolution == 0)
        return fdc ? fdc->rotationBits : 0;

    const uint64_t pendingBits = fdc->rotationClock.PreviewAdvance(
        pendingClocks, fdc->bitsPerRevolution, fdc->clocksPerRevolution);
    return static_cast<uint32_t>(
        (fdc->rotationBits + pendingBits % fdc->bitsPerRevolution) %
        fdc->bitsPerRevolution);
}

uint32_t nec765GetBitsPerRevolution(NEC765* fdc)
{
    return fdc->bitsPerRevolution;
}

void nec765GetDebugInfo(NEC765* fdc, NEC765DebugInfo* info)
{
    if (!info)
        return;

    info->mainStatus       = fdc->mainStatus;
    info->commandCode      = fdc->commandCode;
    info->drive            = fdc->drive;
    info->currentTrack     = fdc->currentTrack;
    info->destinationTrack = fdc->destinationTrack;
    info->cylinder         = fdc->cylinderNumber;
    info->side             = fdc->side;
    info->sector           = fdc->sectorNumber;
    info->sizeCode         = fdc->number;
    info->phase            = (uint8_t)fdc->phase;
    info->phaseStep        = (uint8_t)fdc->phaseStep;
    info->interrupt        = (uint8_t)fdc->interrupt;
}

int nec765DiskChanged(NEC765* fdc, int drive)
{
    fdc->oldTrack = -1; // <--- INIZIALIZZAZIONE CORRETTA
    fdc->currentTrack = 10;
    fdc->destinationTrack = 0;
    fdc->totalBits = 0;
    fdc->rotationBits = 0;
    nec765RefreshRotationTiming(fdc, true);

    return diskChanged(drive);
}

void nec765SaveState(NEC765* fdc, std::ostream& stream)
{
    StateWriter w(stream);

    w.U16(kNEC765StateVersion);

    // Command and result phase.
    w.U8(fdc->drive);
    w.U8(fdc->mainStatus);
    w.U8(fdc->status0);
    w.U8(fdc->status1);
    w.U8(fdc->status2);
    w.U8(fdc->status3);
    w.U8(fdc->commandCode);
    w.I32(fdc->command);
    w.I32(fdc->phase);
    w.I32(fdc->phaseStep);
    w.I32(fdc->phase2);

    // CHRN and head position.
    w.U8(fdc->cylinderNumber);
    w.U8(fdc->side);
    w.U8(fdc->sectorNumber);
    w.U8(fdc->number);
    w.U8(fdc->oldTrack);
    w.U8(fdc->currentTrack);
    w.U8(fdc->destinationTrack);
    w.U64(fdc->destinationTrackCyclesCounter);
    w.U8(fdc->sectorsPerCylinder);
    w.U8(fdc->fillerByte);

    // Transfer in progress. The sector buffer can hold a sector the CPU is
    // half way through reading out, so it travels with the rest.
    w.I32(fdc->sectorSize);
    w.I32(fdc->sectorOffset);
    w.U32(fdc->dataTransferTime);
    w.Bytes(fdc->sectorBuf, sizeof(fdc->sectorBuf));

    w.U64(fdc->deltaCycles);
    w.U64(fdc->totalCycles);
    w.U32(fdc->clockHz);
    w.I32(fdc->interrupt);

    w.U8(fdc->WriteProtected);
    w.U8(fdc->StepRateTimeMs);
    w.U8(fdc->HeadUnloadTimeMs);
    w.U8(fdc->HeadLoadTimeMs);
    w.U8(fdc->NonDMAMode);

    // Where the head is over the spinning surface, including the fractional
    // part of a raw bit. Sector search and FORMAT both complete at an
    // absolute bit position, so the two have to come back together or a
    // reloaded transfer finishes at the wrong moment.
    w.U64(fdc->totalBits);
    w.U32(fdc->rotationBits);
    w.U32(fdc->bitsPerRevolution);
    w.U64(fdc->clocksPerRevolution);
    w.U64(fdc->rotationClock.numeratorRemainder);

    w.I32(fdc->pendingFind);
    w.U8(fdc->findR);
    w.I32(fdc->findSectorIdx);
    w.U64(fdc->findCompleteBit);

    w.I32(fdc->pendingFormat);
    w.U64(fdc->formatCompleteBit);
    w.U8(fdc->formatGpl);
    w.U8(fdc->formatH);
    w.I32(fdc->formatEntriesCount);
    for (int i = 0; i < MAX_SECTORS; ++i)
    {
        w.U8(fdc->formatEntries[i].C);
        w.U8(fdc->formatEntries[i].H);
        w.U8(fdc->formatEntries[i].R);
        w.U8(fdc->formatEntries[i].N);
    }
}

bool nec765LoadState(NEC765* fdc, std::istream& stream)
{
    StateReader r(stream);

    if (r.U16() != kNEC765StateVersion)
        return false;

    NEC765 in = *fdc;

    in.drive = r.U8();
    in.mainStatus = r.U8();
    in.status0 = r.U8();
    in.status1 = r.U8();
    in.status2 = r.U8();
    in.status3 = r.U8();
    in.commandCode = r.U8();
    in.command = r.I32();
    in.phase = r.I32();
    in.phaseStep = r.I32();
    in.phase2 = r.I32();

    in.cylinderNumber = r.U8();
    in.side = r.U8();
    in.sectorNumber = r.U8();
    in.number = r.U8();
    in.oldTrack = r.U8();
    in.currentTrack = r.U8();
    in.destinationTrack = r.U8();
    in.destinationTrackCyclesCounter = r.U64();
    in.sectorsPerCylinder = r.U8();
    in.fillerByte = r.U8();

    in.sectorSize = r.I32();
    in.sectorOffset = r.I32();
    in.dataTransferTime = r.U32();
    r.Bytes(in.sectorBuf, sizeof(in.sectorBuf));

    in.deltaCycles = r.U64();
    in.totalCycles = r.U64();
    in.clockHz = r.U32();
    in.interrupt = r.I32();

    in.WriteProtected = r.U8();
    in.StepRateTimeMs = r.U8();
    in.HeadUnloadTimeMs = r.U8();
    in.HeadLoadTimeMs = r.U8();
    in.NonDMAMode = r.U8();

    in.totalBits = r.U64();
    in.rotationBits = r.U32();
    in.bitsPerRevolution = r.U32();
    in.clocksPerRevolution = r.U64();
    in.rotationClock.numeratorRemainder = r.U64();

    in.pendingFind = r.I32();
    in.findR = r.U8();
    in.findSectorIdx = r.I32();
    in.findCompleteBit = r.U64();

    in.pendingFormat = r.I32();
    in.formatCompleteBit = r.U64();
    in.formatGpl = r.U8();
    in.formatH = r.U8();
    in.formatEntriesCount = r.I32();
    for (int i = 0; i < MAX_SECTORS; ++i)
    {
        in.formatEntries[i].C = r.U8();
        in.formatEntries[i].H = r.U8();
        in.formatEntries[i].R = r.U8();
        in.formatEntries[i].N = r.U8();
    }

    if (!r.Ok())
        return false;

    // Applied in one go, so a short read leaves the controller as it was
    // rather than half way into someone else's transfer. The audio hook is
    // wiring, not state, and is deliberately carried over untouched.
    Audio* const audio = fdc->m_pAudio;
    *fdc = in;
    fdc->m_pAudio = audio;
    return true;
}
