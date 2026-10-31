// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#ifndef VDP_LINE_TYPE_H
#define VDP_LINE_TYPE_H

#include "definitions.h"

// Which Z80 pin the motherboard wires the VDP /INT output to. The VDP itself
// never knows: it only drives a level. SG-1000, SC-3000, Mark III and MSX
// route it to /INT; the ColecoVision routes it to /NMI.
enum class VdpLineType : u8
{
    VDP_INT,
    VDP_NMI,
    VDP_DEVICE // e.g. M5 CTC trigger; must attach the board as the VDP target.
};

#endif
