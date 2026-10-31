/*
 * GearSF7000 - ColecoVision controller keypad, host key defaults
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef COLECO_KEYPAD_LAYOUT_H
#define COLECO_KEYPAD_LAYOUT_H

#include <SDL3/SDL.h>

#include "../../src/definitions.h"

// The twelve keys of a ColecoVision controller, in the order they sit on the
// pad. Bound per player like the joypad keys, and saved next to them in
// [Machine.Coleco] as Keypad<A|B>_<id>. Defaults: player 1 on the number
// row (with - and = for * and #), player 2 on the QWERTY row (with [ and ]),
// clear of both players' joypad defaults on every platform - the numeric
// keypad is player 2's joypad where the keyboard has one.
struct ColecoKeypadKeyLayout
{
    const char* id;         // settings key suffix
    const char* label;      // as printed on the pad
    GC_Keys action;
    SDL_Scancode default_scancode[2];
};

constexpr int COLECO_KEYPAD_KEY_COUNT = 12;

static const ColecoKeypadKeyLayout coleco_keypad_layout[COLECO_KEYPAD_KEY_COUNT] =
{
    {"1", "1", Keypad_1, {SDL_SCANCODE_1, SDL_SCANCODE_Q}},
    {"2", "2", Keypad_2, {SDL_SCANCODE_2, SDL_SCANCODE_W}},
    {"3", "3", Keypad_3, {SDL_SCANCODE_3, SDL_SCANCODE_E}},
    {"4", "4", Keypad_4, {SDL_SCANCODE_4, SDL_SCANCODE_R}},
    {"5", "5", Keypad_5, {SDL_SCANCODE_5, SDL_SCANCODE_T}},
    {"6", "6", Keypad_6, {SDL_SCANCODE_6, SDL_SCANCODE_Y}},
    {"7", "7", Keypad_7, {SDL_SCANCODE_7, SDL_SCANCODE_U}},
    {"8", "8", Keypad_8, {SDL_SCANCODE_8, SDL_SCANCODE_I}},
    {"9", "9", Keypad_9, {SDL_SCANCODE_9, SDL_SCANCODE_O}},
    {"Asterisk", "*", Keypad_Asterisk, {SDL_SCANCODE_MINUS, SDL_SCANCODE_LEFTBRACKET}},
    {"0", "0", Keypad_0, {SDL_SCANCODE_0, SDL_SCANCODE_P}},
    {"Hash", "#", Keypad_Hash, {SDL_SCANCODE_EQUALS, SDL_SCANCODE_RIGHTBRACKET}},
};

#endif
