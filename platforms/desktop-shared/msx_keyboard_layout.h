// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#ifndef MSX_KEYBOARD_LAYOUT_H
#define MSX_KEYBOARD_LAYOUT_H
#include <SDL3/SDL_scancode.h>
struct MSXHostKey { SDL_Scancode scancode; int row; int mask; };
// Physical international layout, independent of host text/IME. F5 deliberately
// has no host binding. On compact Macs Fn+arrows/Delete produce navigation keys.
inline constexpr MSXHostKey msx_keyboard_layout[] = {
    {SDL_SCANCODE_0, 0, 1},
    {SDL_SCANCODE_1, 0, 2},
    {SDL_SCANCODE_2, 0, 4},
    {SDL_SCANCODE_3, 0, 8},
    {SDL_SCANCODE_4, 0, 16},
    {SDL_SCANCODE_5, 0, 32},
    {SDL_SCANCODE_6, 0, 64},
    {SDL_SCANCODE_7, 0, 128},
    {SDL_SCANCODE_8, 1, 1},
    {SDL_SCANCODE_9, 1, 2},
    {SDL_SCANCODE_MINUS, 1, 4},
    {SDL_SCANCODE_EQUALS, 1, 8},
    {SDL_SCANCODE_BACKSLASH, 1, 16},
    {SDL_SCANCODE_LEFTBRACKET, 1, 32},
    {SDL_SCANCODE_RIGHTBRACKET, 1, 64},
    {SDL_SCANCODE_SEMICOLON, 1, 128},
    {SDL_SCANCODE_APOSTROPHE, 2, 1},
    {SDL_SCANCODE_GRAVE, 2, 2},
    {SDL_SCANCODE_COMMA, 2, 4},
    {SDL_SCANCODE_PERIOD, 2, 8},
    {SDL_SCANCODE_SLASH, 2, 16},
    {SDL_SCANCODE_NONUSBACKSLASH, 2, 32},
    {SDL_SCANCODE_A, 2, 64},
    {SDL_SCANCODE_B, 2, 128},
    {SDL_SCANCODE_C, 3, 1},
    {SDL_SCANCODE_D, 3, 2},
    {SDL_SCANCODE_E, 3, 4},
    {SDL_SCANCODE_F, 3, 8},
    {SDL_SCANCODE_G, 3, 16},
    {SDL_SCANCODE_H, 3, 32},
    {SDL_SCANCODE_I, 3, 64},
    {SDL_SCANCODE_J, 3, 128},
    {SDL_SCANCODE_K, 4, 1},
    {SDL_SCANCODE_L, 4, 2},
    {SDL_SCANCODE_M, 4, 4},
    {SDL_SCANCODE_N, 4, 8},
    {SDL_SCANCODE_O, 4, 16},
    {SDL_SCANCODE_P, 4, 32},
    {SDL_SCANCODE_Q, 4, 64},
    {SDL_SCANCODE_R, 4, 128},
    {SDL_SCANCODE_S, 5, 1},
    {SDL_SCANCODE_T, 5, 2},
    {SDL_SCANCODE_U, 5, 4},
    {SDL_SCANCODE_V, 5, 8},
    {SDL_SCANCODE_W, 5, 16},
    {SDL_SCANCODE_X, 5, 32},
    {SDL_SCANCODE_Y, 5, 64},
    {SDL_SCANCODE_Z, 5, 128},
    {SDL_SCANCODE_LSHIFT, 6, 1},
    {SDL_SCANCODE_LCTRL, 6, 2},
    {SDL_SCANCODE_LALT, 6, 4},
    {SDL_SCANCODE_CAPSLOCK, 6, 8},
    {SDL_SCANCODE_RALT, 6, 16},
    {SDL_SCANCODE_F1, 6, 32},
    {SDL_SCANCODE_F2, 6, 64},
    {SDL_SCANCODE_F3, 6, 128},
    {SDL_SCANCODE_F4, 7, 1},
    {SDL_SCANCODE_ESCAPE, 7, 4},
    {SDL_SCANCODE_TAB, 7, 8},
    {SDL_SCANCODE_PAGEDOWN, 7, 16},
    {SDL_SCANCODE_BACKSPACE, 7, 32},
    {SDL_SCANCODE_PAGEUP, 7, 64},
    {SDL_SCANCODE_RETURN, 7, 128},
    {SDL_SCANCODE_SPACE, 8, 1},
    {SDL_SCANCODE_HOME, 8, 2},
    {SDL_SCANCODE_END, 8, 4},
    {SDL_SCANCODE_DELETE, 8, 8},
    {SDL_SCANCODE_LEFT, 8, 16},
    {SDL_SCANCODE_UP, 8, 32},
    {SDL_SCANCODE_DOWN, 8, 64},
    {SDL_SCANCODE_RIGHT, 8, 128},
    {SDL_SCANCODE_KP_MULTIPLY, 9, 1},
    {SDL_SCANCODE_KP_PLUS, 9, 2},
    {SDL_SCANCODE_KP_DIVIDE, 9, 4},
    {SDL_SCANCODE_KP_0, 9, 8},
    {SDL_SCANCODE_KP_1, 9, 16},
    {SDL_SCANCODE_KP_2, 9, 32},
    {SDL_SCANCODE_KP_3, 9, 64},
    {SDL_SCANCODE_KP_4, 9, 128},
    {SDL_SCANCODE_KP_5, 10, 1},
    {SDL_SCANCODE_KP_6, 10, 2},
    {SDL_SCANCODE_KP_7, 10, 4},
    {SDL_SCANCODE_KP_8, 10, 8},
    {SDL_SCANCODE_KP_9, 10, 16},
    {SDL_SCANCODE_KP_MINUS, 10, 32},
    {SDL_SCANCODE_KP_COMMA, 10, 64},
    {SDL_SCANCODE_KP_PERIOD, 10, 128},
    {SDL_SCANCODE_RSHIFT, 6, 1},
    {SDL_SCANCODE_RCTRL, 6, 2},
    {SDL_SCANCODE_KP_ENTER, 7, 128},
    {SDL_SCANCODE_INSERT, 8, 4},
};
inline const MSXHostKey* MSXHostBinding(SDL_Scancode scancode)
{
    for (const auto& key : msx_keyboard_layout)
        if (key.scancode == scancode) return &key;
    return nullptr;
}
// All aliases for a contact contribute. Releasing one Shift cannot release
// the MSX Shift contact while the other remains held.
inline bool MSXHostContactHeld(int row, int mask, const bool* held, int count)
{
    for (const auto& key : msx_keyboard_layout)
        if (key.row == row && key.mask == mask && key.scancode < count && held[key.scancode]) return true;
    return false;
}
#endif
