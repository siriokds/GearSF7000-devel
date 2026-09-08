/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2021  Ignacio Sanchez

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 *
 */

#ifndef DEFINITIONS_H
#define	DEFINITIONS_H

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include "DiagnosticLog.h"

#ifndef EMULATOR_BUILD
#define EMULATOR_BUILD "undefined"
#endif

// Existing IDE projects are the complete GearSF7000 product. Reduced targets
// opt out explicitly from their makefiles, so adding a product switch never
// silently removes features from Visual Studio/Xcode builds.
#ifndef GEARSF7000_PRODUCT_SC3000
#define GEARSF7000_PRODUCT_SC3000 0
#endif
#ifndef GEARSF7000_ENABLE_DEBUG_TOOLS
#define GEARSF7000_ENABLE_DEBUG_TOOLS 1
#endif
#ifndef GEARSF7000_ENABLE_RECORDER
#define GEARSF7000_ENABLE_RECORDER 1
#endif
#ifndef GEARSF7000_ENABLE_AY
#define GEARSF7000_ENABLE_AY 1
#endif
#ifndef GEARSF7000_ENABLE_SF7000
#define GEARSF7000_ENABLE_SF7000 1
#endif
#ifndef GEARSF7000_ENABLE_SR1000
#define GEARSF7000_ENABLE_SR1000 1
#endif
#ifndef GEARSF7000_ENABLE_SP400
#define GEARSF7000_ENABLE_SP400 1
#endif

// SDL is a desktop frontend detail. Existing IDE projects keep the legacy
// adapter; non-SDL frontends explicitly compile it out and inject input via
// the joystick and keyboard-matrix API.
#ifndef GEARSF7000_ENABLE_SDL_INPUT
#define GEARSF7000_ENABLE_SDL_INPUT 1
#endif

//#ifdef __AVX2__
//#define GEARSF7000_TITLE "GearSF7000 (AVX2)"
//#else
#if GEARSF7000_PRODUCT_SC3000
#define GEARSF7000_TITLE "GearSC3000"
#else
#define GEARSF7000_TITLE "GearSF7000"
#endif
//#endif
#define GEARSF7000_VERSION EMULATOR_BUILD
#if GEARSF7000_PRODUCT_SC3000
#define GEARSF7000_TITLE_ASCII "GearSC3000"
#else
#define GEARSF7000_TITLE_ASCII "" \
"  _____                   ___________    ___________  _____  _____  \n" \
" |  __ \\                 /  ___|  ___|  |___  /  _  ||  _  ||  _  | \n" \
" | |  \\/ ___  __ _ _ __  \\ `--.| |_ ______ / /| |/' || |/' || |/' | \n" \
" | | __ / _ \\/ _` | '__|  `--. \\  _|______/ / |  /| ||  /| ||  /| | \n" \
" | |_\\ \\  __/ (_| | |    /\\__/ / |      ./ /  \\ |_/ /\\ |_/ /\\ |_/ / \n" \
"  \\____/\\___|\\__,_|_|    \\____/\\_|      \\_/    \\___/  \\___/  \\___/  \n"
#endif



#if GEARSF7000_ENABLE_DEBUG_TOOLS
#define DEBUG_TOOLS     1
#endif
//#define WRITE_AUDIO_TO_FILE 
//#define DEBUG_PORT      1
#define SC3KSYSTEM_MEM_IMPORT_EXPORT 1
//#define SC3KSYSTEM_AUDIO_LOG 1
//#define SC3KSYSTEM_AUDIO_VGM_LOG


#ifdef DEBUG
#define DEBUG_GEARSF7000 1
#endif

#if defined(PS2) || defined(PSP)
#define PERFORMANCE
#endif

#ifndef NULL
#define NULL 0
#endif

#ifdef _WIN32
#define BLARGG_USE_NAMESPACE 1
#endif

//#define GEARSF7000_DISABLE_DISASSEMBLER

#define MAX_ROM_SIZE    0x800000
#define MAX_SRAM_SIZE   0x800   

#define SafeDelete(pointer) if(pointer != NULL) {delete pointer; pointer = NULL;}
#define SafeDeleteArray(pointer) if(pointer != NULL) {delete [] pointer; pointer = NULL;}

#define InitPointer(pointer) ((pointer) = NULL)
#define IsValidPointer(pointer) ((pointer) != NULL)

#if defined(MSB_FIRST) || defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define IS_BIG_ENDIAN
#else
#define IS_LITTLE_ENDIAN
#endif

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

typedef void (*RamChangedCallback) (void);

#define FLAG_CARRY 0x01
#define FLAG_NEGATIVE 0x02
#define FLAG_PARITY 0x04
#define FLAG_X 0x08
#define FLAG_HALF 0x10
#define FLAG_Y 0x20
#define FLAG_ZERO 0x40
#define FLAG_SIGN 0x80
#define FLAG_NONE 0

#define GC_RESOLUTION_WIDTH 256
#define GC_RESOLUTION_HEIGHT 192

//#define GC_RESOLUTION_WIDTH_WITH_OVERSCAN 320
//#define GC_RESOLUTION_HEIGHT_WITH_OVERSCAN 288
//#define GC_RESOLUTION_SMS_OVERSCAN_H_320_L 32
//#define GC_RESOLUTION_SMS_OVERSCAN_H_320_R 32
//#define GC_RESOLUTION_SMS_OVERSCAN_H_284_L 14
//#define GC_RESOLUTION_SMS_OVERSCAN_H_284_R 14
//#define GC_RESOLUTION_SMS_OVERSCAN_H_272_L 4
//#define GC_RESOLUTION_SMS_OVERSCAN_H_272_R 12
//#define GC_RESOLUTION_OVERSCAN_V 24
//#define GC_RESOLUTION_OVERSCAN_V_PAL 48


#define GC_RESOLUTION_WIDTH_WITH_OVERSCAN 288
#define GC_RESOLUTION_HEIGHT_WITH_OVERSCAN 256
#define GC_RESOLUTION_SMS_OVERSCAN_H_320_L 0
#define GC_RESOLUTION_SMS_OVERSCAN_H_320_R 0
#define GC_RESOLUTION_SMS_OVERSCAN_H_284_L 4
#define GC_RESOLUTION_SMS_OVERSCAN_H_284_R 12
#define GC_RESOLUTION_SMS_OVERSCAN_H_272_L 2
#define GC_RESOLUTION_SMS_OVERSCAN_H_272_R 6
#define GC_RESOLUTION_OVERSCAN_V 16
#define GC_RESOLUTION_OVERSCAN_V_PAL 32


#define GC_CYCLES_PER_LINE 228

// TMS9918/9929 native video timing. The VDP master oscillator advances two
// clocks for each of the 342 raster dots in a complete scanline. Keep this
// domain independent from GC_CYCLES_PER_LINE: they are exactly related only
// on the NTSC machine, while the PAL SC-3000 CPU has its own 3.580000 MHz
// oscillator.
#define GC_VDP_DOTS_PER_LINE 342
#define GC_VDP_MASTER_CLOCKS_PER_DOT 2
#define GC_VDP_MASTER_CLOCKS_PER_LINE \
    (GC_VDP_DOTS_PER_LINE * GC_VDP_MASTER_CLOCKS_PER_DOT)
#define GC_VDP_MASTER_CLOCK_HZ 10738635

#define GC_MASTER_CLOCK_NTSC 3579545
#define GC_LINES_PER_FRAME_NTSC 262
#define GC_FRAMES_PER_SECOND_NTSC 60

// Not a typo and not the same figure as the Master System's: the PAL SC-3000
// clocks its Z80 from its own external 3.580000 MHz oscillator instead of
// dividing the VDP clock down, so the CPU runs at the same rate in both
// regions and a scanline is 228 T either way. 3.546895 MHz is what you get by
// locking the CPU to the PAL colour subcarrier instead - 4.43361875 * 4 / 5,
// the Master System's arrangement - and this machine does not do that: its
// subcarrier sits on a separate daughterboard with its own crystal. The main
// board is silkscreened SYSTEM CLOCK (PAL=3.58M) next to the resonator.
#define GC_MASTER_CLOCK_PAL 3580000
#define GC_LINES_PER_FRAME_PAL 313
#define GC_FRAMES_PER_SECOND_PAL 50

#ifndef GC_AUDIO_SAMPLE_RATE
#define GC_AUDIO_SAMPLE_RATE 48000
#endif

// The tape monitor is intentionally rich in harmonics up to roughly 6 kHz.
// 44.1 kHz was useful only as a diagnostic experiment; the discrete tape
// renderer and the native output path require at least 48 kHz for stable,
// artifact-free playback on every desktop platform.
#if GC_AUDIO_SAMPLE_RATE < 48000
#error "GC_AUDIO_SAMPLE_RATE must be at least 48000 Hz"
#endif
#define GC_AUDIO_BUFFER_SIZE 8192
#define GC_AUDIO_BUFFER_NUM  2 

#define GC_SAVESTATE_MAGIC 0x09200902

struct GC_Color
{
    u8 red;
    u8 green;
    u8 blue;
};

enum GC_Color_Format
{
    GC_PIXEL_RGB565,
    GC_PIXEL_RGB555,
    GC_PIXEL_RGB888,
    GC_PIXEL_BGR565,
    GC_PIXEL_BGR555,
    GC_PIXEL_BGR888
};

enum GC_Keys
{
    Keypad_8 = 0x01,
    Keypad_4 = 0x02,
    Keypad_5 = 0x03,
    Key_Blue = 0x04,
    Keypad_7 = 0x05,
    Keypad_Hash = 0x06,
    Keypad_2 = 0x07,
    Key_Purple = 0x08,
    Keypad_Asterisk = 0x09,
    Keypad_0 = 0x0A,
    Keypad_9 = 0x0B,
    Keypad_3 = 0x0C,
    Keypad_1 = 0x0D,
    Keypad_6 = 0x0E,
    Key_Up = 0x10,
    Key_Right = 0x11,
    Key_Down = 0x12,
    Key_Left = 0x13,
    Key_Left_Button = 0x14,
    Key_Right_Button = 0x15
};

enum GC_Controllers
{
    Controller_1 = 0,
    Controller_2 = 1
};

enum GC_Region
{
    Region_NTSC,
    Region_PAL
};

struct GC_RuntimeInfo
{
    int screen_width;
    int screen_height;
    GC_Region region;
};

// Log() used to be guarded by DEBUG_GEARSF7000, which no build defines, so
// every one of these calls compiled away to nothing and the emulator produced
// no diagnostics at all. They now reach the diagnostic log at Debug level,
// which is off by default: a fair number of them sit in per-frame and
// per-snapshot paths - "Save state size" runs once per recorded frame - and
// switching them all on unconditionally would write sixty lines a second.
//
// Turn them on from the GUI or with DiagnosticLogSetLevel(DiagLevel::Debug)
// when chasing something. Lifecycle events worth keeping always use the
// DiagInfo/DiagWarn/DiagError macros instead, which are not gated.

#ifdef __ANDROID__
#include <android/log.h>
#endif

#define Log(msg, ...) DiagDebug("core", msg, ##__VA_ARGS__)
#define Debug(msg, ...) DiagDebug("core", msg, ##__VA_ARGS__)

inline u8 SetBit(const u8 value, const u8 bit)
{
    return value | (0x01 << bit);
}

inline u8 UnsetBit(const u8 value, const u8 bit)
{
    return value & (~(0x01 << bit));
}

inline bool IsSetBit(const u8 value, const u8 bit)
{
    return (value & (0x01 << bit)) != 0;
}

inline u8 FlipBit(const u8 value, const u8 bit)
{
    return value ^ (0x01 << bit);
}

inline u8 ReverseBits(const u8 value)
{
    u8 ret = value;
    ret = (ret & 0xF0) >> 4 | (ret & 0x0F) << 4;
    ret = (ret & 0xCC) >> 2 | (ret & 0x33) << 2;
    ret = (ret & 0xAA) >> 1 | (ret & 0x55) << 1;
    return ret;
}

inline int AsHex(const char c)
{
   return c >= 'A' ? c - 'A' + 0xA : c - '0';
}

inline unsigned int Pow2Ceil(u16 n)
{
    --n;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    ++n;
    return n;
}

#endif	/* DEFINITIONS_H */
