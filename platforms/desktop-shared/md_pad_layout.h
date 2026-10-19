/*
 * GearSF7000 - Mega Drive pad buttons on a Master System controller port
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MD_PAD_LAYOUT_H
#define MD_PAD_LAYOUT_H

#include "../../src/definitions.h"

// The buttons a Mega Drive pad has beyond the SMS pad's two (B and C are its
// buttons 1 and 2). Bound per player, keyboard and gamepad, and saved in
// [Machine.SMS] as Pad<A|B>_Key<id> and Pad<A|B>_Gamepad<id>. No defaults: the
// user binds them. The MCP names them the same way.
struct MdPadButtonLayout
{
    const char* id;    // settings key suffix, MCP name
    const char* label;
    GC_Keys action;
};

constexpr int MD_PAD_BUTTON_COUNT = 6;

static const MdPadButtonLayout md_pad_layout[MD_PAD_BUTTON_COUNT] =
{
    {"A", "A", Key_MD_A},
    {"Start", "Start", Key_MD_Start},
    {"X", "X", Key_MD_X},
    {"Y", "Y", Key_MD_Y},
    {"Z", "Z", Key_MD_Z},
    {"Mode", "Mode", Key_MD_Mode},
};

#endif
