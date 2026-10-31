/*****************************************************************************
** $Source: /cygdrive/d/Private/_SVNROOT/bluemsx/blueMSX/Src/IoDevice/NEC765.h,v $
**
** $Revision: 1.4 $
**
** $Date: 2008-03-30 18:38:40 $
**
** More info: http://www.bluemsx.com
**
** Copyright (C) 2003-2006 Daniel Vik
** Copyright (C) 2024-2026  Saverio Russo
**
** Derived from blueMSX by Daniel Vik (GPL-2.0-or-later, used here under
** version 3) and largely extended for GearSF7000; the changes and additions
** by Saverio Russo are AGPL-3.0-or-later.
**
** SPDX-License-Identifier: GPL-2.0-or-later AND AGPL-3.0-or-later
**
** The original notice of blueMSX follows.
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
#ifndef NEC765_H
#define NEC765_H

#include <stdint.h>
#include <iosfwd>
#include <string>
//#include "MsxTypes.h"
#include "Audio.h"
#include "IDebugEventProvider.h"


typedef struct NEC765 NEC765;

/* Read-only diagnostic snapshot. Keeping this separate from the emulation
 * API prevents the debugger from changing an FDC command in progress. */
typedef struct {
    uint8_t mainStatus;
    uint8_t commandCode;
    uint8_t drive;
    uint8_t currentTrack;
    uint8_t destinationTrack;
    uint8_t cylinder;
    uint8_t side;
    uint8_t sector;
    uint8_t sizeCode;
    uint8_t phase;
    uint8_t phaseStep;
    uint8_t interrupt;
} NEC765DebugInfo;

NEC765* nec765Create();

void nec765SetAudio(NEC765* fdc, Audio* pAudio);
void nec765Destroy(NEC765* fdc);
void nec765Reset(NEC765* fdc);
void nec765ResetTimebase(NEC765* fdc);

uint8_t nec765Read(NEC765* fdc);
uint8_t nec765Peek(NEC765* fdc);
uint8_t nec765ReadStatus(NEC765* fdc);
uint8_t nec765PeekStatus(NEC765* fdc);
void nec765Write(NEC765* fdc, uint8_t value);

int nec765DiskChanged(NEC765* fdc, int drive);

void nec765SetClockRate(NEC765* fdc, uint32_t clockHz);
void nec765Tick(NEC765* fdc, uint64_t clockCycles, bool rotationActive);
uint64_t nec765ClocksToNextEvent(NEC765* fdc, bool rotationActive);

void nec765SetTCSignal(NEC765* fdc, int val);
void nec765SetReadOnly(NEC765* fdc, bool readonly);
int  nec765IsDiskPresent(NEC765* fdc);
int  nec765IsDiskEnabled(NEC765* fdc);

int nec765GetInt(NEC765* fdc);
int nec765GetIndex(NEC765* fdc);

/* Rotational position for GUI disk animation.
 * Returns the current head position in raw MFM bits (0 .. bitsPerRevolution-1). */
uint32_t nec765GetRotationBits(NEC765* fdc);
uint32_t nec765GetProjectedRotationBits(NEC765* fdc,
                                       uint64_t pendingClocks,
                                       bool rotationActive);
uint32_t nec765GetBitsPerRevolution(NEC765* fdc);
void nec765GetDebugInfo(NEC765* fdc, NEC765DebugInfo* info);

// The whole controller: command/result phase, CHRN, the sector buffer of a
// transfer in progress, and the head's absolute position on the spinning
// surface down to the fractional raw bit.
void nec765SaveState(NEC765* fdc, std::ostream& stream);
bool nec765LoadState(NEC765* fdc, std::istream& stream);

struct NEC765DebugConfiguration
{
    std::string key;
    std::string parentKey;
    std::string name;
    std::uint16_t ioPortBase = 0xE0;
};

// Owns the opaque blueMSX controller and gives that concrete instance the
// common device-provider interface used during machine composition. The C API
// remains available while the rest of the FDC implementation is modernised.
class NEC765Device : public IDebugEventProvider
{
public:
    explicit NEC765Device(NEC765DebugConfiguration debug = {});
    ~NEC765Device();
    NEC765Device(const NEC765Device&) = delete;
    NEC765Device& operator=(const NEC765Device&) = delete;

    DebugDeviceDefinition DescribeDebugDevice() const override;
    operator NEC765*() { return m_fdc; }
    operator NEC765*() const { return m_fdc; }
    explicit operator bool() const { return m_fdc != nullptr; }

private:
    NEC765* m_fdc = nullptr;
    NEC765DebugConfiguration m_debug;
};

/* =========================================================
 * FDC diagnostic log
 *
 * Define NEC765_LOG_ENABLED (below) to activate the log.
 * When not defined, all four functions are empty stubs
 * (no buffer allocated, zero run-time overhead).
 *
 * nec765LogClear()              — reset the log buffer
 * nec765LogSave(path)           — write the log to a text file
 * nec765LogDiskEject(driveId)   — record a disk-eject event
 * nec765LogDiskLoad(id, name)   — record a disk-load event
 *
 * Each FDC event line is prefixed with:
 *   [absolute_ms  +delta_ms]
 * ========================================================= */
//#define NEC765_LOG_ENABLED   /* comment out to disable */

void nec765LogClear(void);
bool nec765LogSave(const char* filename);
void nec765LogDiskEject(int driveId);
void nec765LogDiskLoad(int driveId, const char* fileName);

#endif
