/*****************************************************************************
** Disk.h — GearSF7000 FDC disk subsystem public API
**
** Extended from the original blueMSX Disk.h to support the Physical Track
** Model (DiskTrack / DiskSector) and four disk image formats:
**   SF7 (flat raw), EDSK (Amstrad/CPC), HFE (HxC v1), DS7 (native model).
**
** Copyright (C) 2003-2006 Daniel Vik, Tomas Karlsson
** Copyright (C) 2024-2026  Saverio Russo
**
** Derived from blueMSX by Daniel Vik and Tomas Karlsson (GPL-2.0-or-later,
** used here under version 3); the changes and additions by Saverio Russo
** are AGPL-3.0-or-later.
**
** SPDX-License-Identifier: GPL-2.0-or-later AND AGPL-3.0-or-later
******************************************************************************
*/
#ifndef DISK_H
#define DISK_H

#include <stdint.h>
#include "DiskModel.h"   /* DiskTrack, DiskSector, DISC_DRIVE, all constants */

#ifdef __cplusplus
#include "IDebugEventProvider.h"
#include <string>
#endif

/* =========================================================
 * Configuration
 * ========================================================= */
#define MAX_FDC_COUNT    2
#define MAXDRIVES        (MAX_FDC_COUNT)

/* =========================================================
 * Error codes
 * ========================================================= */
typedef enum {
    DSKE_OK,
    DSKE_NO_DATA,
    DSKE_CRC_ERROR
} DSKE;

/* Read-only image metadata for the SF-7000 debugger. */
typedef struct {
    int present;
    int enabled;
    int readOnly;
    int dirty;
    DiskImageFormat format;
    int tracks;
    int sides;
    int sectorsPerTrack;
    int sectorSize;
    uint16_t rpm;
    uint16_t bitrateKbps;
    const char* fileName;
} DiskDebugInfo;

/* =========================================================
 * Lifecycle
 * ========================================================= */
void    diskInit(int driveId);

/* Load/change the disk image.  Accepts SF7, EDSK, HFE, and DS7.
 * On HFE load, a .ds7 sidecar is auto-saved.
 * Call with fileName=NULL (or diskEject) to eject. */
uint8_t diskChange(int driveId, const char* fileName);
uint8_t diskEject (int driveId);

/* =========================================================
 * Properties
 * ========================================================= */
void    diskEnable  (int driveId, int enable);
uint8_t diskIsEnabled(int driveId);

void    diskReadOnly  (int driveId, int readonly);
uint8_t diskIsReadOnly(int driveId);

uint8_t diskPresent(int driveId);
int     diskChanged(int driveId);   /* returns 1 once then clears */

/* Nominal geometry (for legacy NEC765 compatibility) */
int diskGetSectorsPerTrack(int driveId);
int diskGetSides(int driveId);
int diskGetSectorSize(int driveId, int side, int track, int density);

/* =========================================================
 * Physical track model access (used by NEC765)
 * ========================================================= */

/* Returns a pointer to the DiskTrack for a given side/track, or NULL. */
DiskTrack* diskGetTrack(int driveId, int side, int track);

/* Drive rotation constants */
uint32_t diskGetBitsPerRevolution(int driveId);   /* raw MFM bits per rev */
uint32_t diskGetClocksPerBit(int driveId);         /* Z80 clocks per raw bit */
void     diskGetDebugInfo(int driveId, DiskDebugInfo* info);

/* =========================================================
 * Sector I/O
 *
 * 'sector' is the R value (sector ID), NOT a linear index.
 * This allows custom sector IDs like Amstrad's 0xC1..0xC9.
 * ========================================================= */
DSKE    diskReadSector(int driveId, uint8_t* buffer,
                       int sector, int side, int track,
                       int density, int* sectorSize);

uint8_t diskWriteSector(int driveId, uint8_t* buffer,
                        int sector, int side, int track, int density);

/* Legacy linear-sector write used by FORMAT command */
uint8_t diskWrite(int driveId, uint8_t* buffer, int sector);

/* =========================================================
 * diskSetFormattedTrack
 *
 * Called by NEC765 after a FORMAT revolution completes.
 * Replaces the track at (side, track) with a new DiskTrack
 * built from the CHRN data collected during the FORMAT exec phase.
 * Sector data is pre-filled with fillerByte; gap3 is the GPL value.
 * ========================================================= */
typedef struct {
    uint8_t C, H, R, N;
} FDC_FormatEntry;

void diskSetFormattedTrack(int driveId, int side, int track,
                            const FDC_FormatEntry* entries, int count,
                            uint8_t fillerByte, uint8_t gap3);

/* =========================================================
 * DS7 native format save
 *
 * Call after loading any format to create a fast-load sidecar.
 * The emulator may also call this on eject when dirty==1.
 * ========================================================= */
uint8_t diskSaveDS7(int driveId, const char* fileName);

/* =========================================================
 * diskCreateEmptyDS7
 *
 * Creates a blank, pre-formatted SF-7000 DS7 disk image:
 *   40 tracks, 1 side, 16 sectors/track, 256 B/sector, all 0xFF.
 * All sector headers carry proper CHRN values (R = 1..16) so that
 * diskWriteSector() can locate sectors by R value after FORMAT.
 * The file is written directly to disk; no drive slot is involved.
 * Returns true on success.
 * ========================================================= */
bool diskCreateEmptyDS7(const char* filename);

#ifdef __cplusplus

class DebugDeviceRegistry;
class Memory;

struct FloppyDriveDebugConfiguration
{
    std::string key;
    std::string parentKey;
    std::string name;
};

// Device facade for one physical drive slot. The disk-image engine remains
// behind the C API while it is modernised; this object supplies stable device
// identity and publishes only discrete drive-line transitions.
class FloppyDriveDevice : public IDebugEventProvider
{
public:
    enum State : uint8_t
    {
        MediaPresent = 0,
        Enabled,
        WriteProtected,
        Motor,
        Index,
        StateCount
    };

    FloppyDriveDevice(int driveId, Memory* memory,
                      FloppyDriveDebugConfiguration debug);
    DebugDeviceDefinition DescribeDebugDevice() const override;
    void BindDebugEvents(const DebugDeviceRegistry& registry);
    // Establishes the current electrical state without creating startup or
    // catalog-recomposition events.
    void Capture(bool motor, bool index);
    void Observe(bool motor, bool index);

private:
    void Publish(State state, bool value);

    int m_driveId;
    Memory* m_memory;
    FloppyDriveDebugConfiguration m_debug;
    DebugDeviceId m_debugDevice = 0;
    bool m_observed[StateCount] = {};
    bool m_hasObservation = false;
};

#endif

#endif /* DISK_H */
