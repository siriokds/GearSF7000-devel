// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
/* TMS99xx fixed palette tables.
 * Kept separate from the device class so TMS-specific offline decoders do
 * not acquire the complete emulated chip API.
 */
#ifndef TMS99XX_PALETTE_H
#define TMS99XX_PALETTE_H

#include "definitions.h"

const u8 kPalette_888_coleco[48] = {0,0,0, 0,0,0, 33,200,66, 94,220,120, 84,85,237, 125,118,252, 212,82,77, 66,235,245, 252,85,84, 255,121,120, 212,193,84, 230,206,128, 33,176,59, 201,91,186, 204,204,204, 255,255,255};
//const u8 kPalette_888_tms9918[48] = {0,0,0, 0,8,0, 0,241,1, 50,251,65, 67,76,255, 112,110,255, 238,75,28, 9,255,255, 255,78,31, 255,112,65, 211,213,0, 228,221,52, 0,209,0, 219,79,211, 193,212,190, 244,255,241};

// Analog 888 version, CRT simulation
//const u8 kPalette_888_tms9918_analog[48] = { 0, 0, 4, 1, 0, 4, 58, 187, 69, 112, 211, 120, 84, 89, 212, 123, 123, 230, 179, 99, 76, 97, 223, 230, 212, 106, 85, 248, 142, 120, 199, 199, 92, 217, 212, 132, 54, 165, 61, 176, 107, 172, 199, 208, 197, 250, 255, 248 };

const u8 kPalette_888_tms9918[48] = {
    0, 0, 0,         // 0     Backdrop
    0, 0, 0,         // 1     Black
    51, 187, 68,     // 2     Medium green
    153, 221, 153,   // 3     Light green
    85, 85, 221,     // 4     Dark blue
    136, 136, 238,   // 5     Light blue
    187, 68, 51,     // 6     Dark red
    85, 238, 238,    // 7     Cyan
    238, 102, 68,    // 8     Medium red
    255, 170, 119,   // 9     Light red
    187, 187, 85,    // A     Medium yellow
    221, 221, 136,   // B     Light yellow
    51, 170, 51,     // C     Dark green
    204, 136, 204,   // D     Purple
    238, 238, 238,   // E     Gray
    255, 255, 255    // F     White
};

// Analog R2R DAC-444 to 888 version, CRT simulation
const u8 kPalette_888_tms9918_analog[48] = {
    0, 0, 0,         // 0     Backdrop
    17, 17, 17,         // 1     Black
    51, 187, 68,     // 2     Medium green
    153, 221, 153,   // 3     Light green
    85, 85, 221,     // 4     Dark blue
    136, 136, 238,   // 5     Light blue
    187, 68, 51,     // 6     Dark red
    85, 238, 238,    // 7     Cyan
    238, 102, 68,    // 8     Medium red
    255, 170, 119,   // 9     Light red
    187, 187, 85,    // A     Medium yellow
    221, 221, 136,   // B     Light yellow
    51, 170, 51,     // C     Dark green
    204, 136, 204,   // D     Purple
    238, 238, 238,   // E     Gray
    255, 255, 255    // F     White
};

// Analog R2R DAC-444 version, CRT simulation
const u8 kPalette_444_tms9918_analog[48] = {
    0x0, 0x0, 0x0,   // 0     Backdrop
    0x0, 0x0, 0x1,   // 1     Black
    0x3, 0xC, 0x4,   // 2     Medium green
    0x9, 0xE, 0x9,   // 3     Light green
    0x5, 0x5, 0xE,   // 4     Dark blue
    0x8, 0x8, 0xF,   // 5     Light blue
    0xB, 0x4, 0x3,   // 6     Dark red
    0x5, 0xF, 0xF,   // 7     Cyan
    0xF, 0x6, 0x4,   // 8     Medium red
    0xF, 0xA, 0x7,   // 9     Light red
    0xB, 0xB, 0x5,   // A     Medium yellow
    0xE, 0xE, 0x8,   // B     Light yellow
    0x3, 0xA, 0x3,   // C     Dark green
    0xC, 0x8, 0xC,   // D     Purple
    0xE, 0xE, 0xE,   // E     Gray
    0xF, 0xF, 0xF    // F     White
};

// The SMS VDP (315-5124, Mark III / Master System) in a TMS mode: sixteen
// fixed colours taken from its own 2-bit-per-channel set, so 0,5,A,F per
// channel. Same values as Gearsystem's kSG1000_palette_888_sms.
const u8 kPalette_444_sms_vdp_tms[48] = {
    0x0, 0x0, 0x0,   // 0     Backdrop
    0x0, 0x0, 0x0,   // 1     Black
    0x0, 0xA, 0x0,   // 2     Medium green
    0x0, 0xF, 0x0,   // 3     Light green
    0x0, 0x0, 0x5,   // 4     Dark blue
    0x0, 0x0, 0xF,   // 5     Light blue
    0x5, 0x0, 0x0,   // 6     Dark red
    0x0, 0xF, 0xF,   // 7     Cyan
    0xA, 0x0, 0x0,   // 8     Medium red
    0xF, 0x0, 0x0,   // 9     Light red
    0x5, 0x5, 0x0,   // A     Medium yellow
    0xF, 0xF, 0x0,   // B     Light yellow
    0x0, 0x5, 0x0,   // C     Dark green
    0xF, 0x0, 0xF,   // D     Purple
    0x5, 0x5, 0x5,   // E     Gray
    0xF, 0xF, 0xF    // F     White
};

// The same sixteen in 8 bits per channel, for the TMS99xx machines: an
// SG-1000/SC-3000 game as a Mark III would show it.
const u8 kPalette_888_sms_vdp_tms[48] = {
    0, 0, 0,         // 0     Backdrop
    0, 0, 0,         // 1     Black
    0, 170, 0,       // 2     Medium green
    0, 255, 0,       // 3     Light green
    0, 0, 85,        // 4     Dark blue
    0, 0, 255,       // 5     Light blue
    85, 0, 0,        // 6     Dark red
    0, 255, 255,     // 7     Cyan
    170, 0, 0,       // 8     Medium red
    255, 0, 0,       // 9     Light red
    85, 85, 0,       // A     Medium yellow
    255, 255, 0,     // B     Light yellow
    0, 85, 0,        // C     Dark green
    255, 0, 255,     // D     Purple
    85, 85, 85,      // E     Gray
    255, 255, 255    // F     White
};

const u8 k2bitTo8bit[4] = {0,85,170,255};
const u8 k2bitTo5bit[4] = {0,10,21,31};
const u8 k2bitTo6bit[4] = {0,21,42,63};
const u8 k4bitTo8bit[16] = {0,17,34,51,68,86,102,119,136,153,170,187,204,221,238,255};
const u8 k4bitTo5bit[16] = {0,2,4,6,8,10,12,14,17,19,21,23,25,27,29,31};
const u8 k4bitTo6bit[16] = {0,4,8,13,17,21,25,29,34,38,42,46,50,55,59,63};

#endif /* TMS99XX_PALETTE_H */
