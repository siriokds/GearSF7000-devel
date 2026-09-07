/*****************************************************************************
** $Source: /cygdrive/d/Private/_SVNROOT/bluemsx/blueMSX/Src/Board/Board.h,v $
**
** $Revision: 1.40 $
**
** $Date: 2007-03-20 02:30:31 $
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
#ifndef BOARD_H
#define BOARD_H
 
//#include "MsxTypes.h"
//#include "MediaDb.h"
//#include "Machine.h"
//#include "VDP.h"
//#include "AudioMixer.h"

#include <stdint.h>
#include <stdio.h>


void boardInit(uint32_t* systemTime);

int boardRun(Machine* machine, 
             BoardDeviceInfo* deviceInfo,
             Mixer* mixer,
             char* stateFile,
             int frequency,
             int reversePeriod,
             int reverseBufferCnt,
             int (*syncCallback)(int, int));

int boardRewind();
int boardRewindOne();
void boardEnableSnapshots(int enable);

BoardType boardGetType();

void boardSetMachine(Machine* machine);
void boardReset();

void boardSetDataBus(uint8_t value, uint8_t defaultValue, int setDefault);

uint64_t boardSystemTime64();

void boardCaptureStart(const char* filename);
void boardCaptureStop();
int boardCaptureHasData();
int boardCaptureIsRecording();
int boardCaptureIsPlaying();
int boardCaptureCompleteAmount();

uint8_t boardCaptureuint8_t(uint8_t logId, uint8_t value);

void boardSaveState(const char* stateFile, int screenshot);

void boardSetFrequency(int frequency);
int  boardGetRefreshRate();

void boardSetBreakpoint(uint16_t address);
void boardClearBreakpoint(uint16_t address);

void   boardSetInt(uint32_t irq);
void   boardClearInt(uint32_t irq);
uint32_t boardGetInt(uint32_t irq);

uint8_t* boardGetRamPage(int page);
uint32_t boardGetRamSize();
uint32_t boardGetVramSize();

int boardUseRom();
int boardUseMegaRom();
int boardUseMegaRam();
int boardUseFmPac();

void boardSetNoSpriteLimits(int enable);
int boardGetNoSpriteLimits();

RomType boardGetRomType(int cartNo);

typedef enum { HD_NONE, HD_SUNRISEIDE, HD_BEERIDE, HD_GIDE, HD_RSIDE,
               HD_MEGASCSI, HD_WAVESCSI, HD_GOUDASCSI, HD_NOWIND } HdType;
HdType boardGetHdType(int hdIndex);

const char* boardGetBaseDirectory();

Mixer* boardGetMixer();

void boardChangeCartridge(int cartNo, RomType romType, char* cart, char* cartZip);
void boardChangeDiskette(int driveId, char* fileName, const char* fileInZipFile);
void boardChangeCassette(int tapeId, char* name, const char* fileInZipFile);

int  boardGetCassetteInserted();

#define boardFrequency() (6 * 3579545)

static uint32_t boardSystemTime() {
    extern uint32_t* boardSysTime;
    return *boardSysTime;
}

uint64_t boardSystemTime64();

typedef void (*BoardTimerCb)(void* ref, uint32_t time);

typedef struct BoardTimer BoardTimer;

BoardTimer* boardTimerCreate(BoardTimerCb callback, void* ref);
void boardTimerDestroy(BoardTimer* timer);
void boardTimerAdd(BoardTimer* timer, uint32_t timeout);
void boardTimerRemove(BoardTimer* timer);
void boardTimerCheckTimeout(void* dummy);
uint32_t boardCalcRelativeTimeout(uint32_t timerFrequency, uint32_t nextTimeout);

void   boardOnBreakpoint(uint16_t pc);

int boardInsertExternalDevices();
int boardRemoveExternalDevices();

// The following methods are more generic config than board specific
// They should be moved from board.
void boardSetDirectory(const char* dir);

void boardSetFdcTimingEnable(int enable);
int  boardGetFdcTimingEnable();
void boardSetFdcActive();

void boardSetYm2413Oversampling(int value);
int  boardGetYm2413Oversampling();
void boardSetY8950Oversampling(int value);
int  boardGetY8950Oversampling();
void boardSetMoonsoundOversampling(int value);
int  boardGetMoonsoundOversampling();

void boardSetYm2413Enable(int value);
int  boardGetYm2413Enable();
void boardSetY8950Enable(int value);
int  boardGetY8950Enable();
void boardSetMoonsoundEnable(int value);
int  boardGetMoonsoundEnable();
void boardSetVideoAutodetect(int value);
int  boardGetVideoAutodetect();

void boardSetPeriodicCallback(BoardTimerCb cb, void* reference, uint32_t frequency);

#endif /* BOARD_H */

