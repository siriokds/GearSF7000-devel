// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef M5_KEYBOARD_LAYOUT_H
#define M5_KEYBOARD_LAYOUT_H
#include <SDL3/SDL_scancode.h>
struct M5HostKey { SDL_Scancode scancode; int row; int mask; };
// Physical Sord layout. RESET is a polled input, not an emulator reset.
inline constexpr M5HostKey m5_keyboard_layout[] = {
    {SDL_SCANCODE_LCTRL, 0, 1},
    {SDL_SCANCODE_TAB, 0, 2},
    {SDL_SCANCODE_LSHIFT, 0, 4},
    {SDL_SCANCODE_RSHIFT, 0, 8},
    {SDL_SCANCODE_SPACE, 0, 64},
    {SDL_SCANCODE_RETURN, 0, 128},
    {SDL_SCANCODE_1, 1, 1},
    {SDL_SCANCODE_2, 1, 2},
    {SDL_SCANCODE_3, 1, 4},
    {SDL_SCANCODE_4, 1, 8},
    {SDL_SCANCODE_5, 1, 16},
    {SDL_SCANCODE_6, 1, 32},
    {SDL_SCANCODE_7, 1, 64},
    {SDL_SCANCODE_8, 1, 128},
    {SDL_SCANCODE_Q, 2, 1},
    {SDL_SCANCODE_W, 2, 2},
    {SDL_SCANCODE_E, 2, 4},
    {SDL_SCANCODE_R, 2, 8},
    {SDL_SCANCODE_T, 2, 16},
    {SDL_SCANCODE_Y, 2, 32},
    {SDL_SCANCODE_U, 2, 64},
    {SDL_SCANCODE_I, 2, 128},
    {SDL_SCANCODE_A, 3, 1},
    {SDL_SCANCODE_S, 3, 2},
    {SDL_SCANCODE_D, 3, 4},
    {SDL_SCANCODE_F, 3, 8},
    {SDL_SCANCODE_G, 3, 16},
    {SDL_SCANCODE_H, 3, 32},
    {SDL_SCANCODE_J, 3, 64},
    {SDL_SCANCODE_K, 3, 128},
    {SDL_SCANCODE_Z, 4, 1},
    {SDL_SCANCODE_X, 4, 2},
    {SDL_SCANCODE_C, 4, 4},
    {SDL_SCANCODE_V, 4, 8},
    {SDL_SCANCODE_B, 4, 16},
    {SDL_SCANCODE_N, 4, 32},
    {SDL_SCANCODE_M, 4, 64},
    {SDL_SCANCODE_COMMA, 4, 128},
    {SDL_SCANCODE_9, 5, 1},
    {SDL_SCANCODE_0, 5, 2},
    {SDL_SCANCODE_MINUS, 5, 4},
    {SDL_SCANCODE_EQUALS, 5, 8},
    {SDL_SCANCODE_PERIOD, 5, 16},
    {SDL_SCANCODE_SLASH, 5, 32},
    {SDL_SCANCODE_NONUSBACKSLASH, 5, 64},
    {SDL_SCANCODE_GRAVE, 5, 128},
    {SDL_SCANCODE_O, 6, 1},
    {SDL_SCANCODE_P, 6, 2},
    {SDL_SCANCODE_LEFTBRACKET, 6, 4},
    {SDL_SCANCODE_RIGHTBRACKET, 6, 8},
    {SDL_SCANCODE_L, 6, 16},
    {SDL_SCANCODE_SEMICOLON, 6, 32},
    {SDL_SCANCODE_APOSTROPHE, 6, 64},
    {SDL_SCANCODE_BACKSLASH, 6, 128},
    {SDL_SCANCODE_RCTRL, 0, 1},
    {SDL_SCANCODE_KP_ENTER, 0, 128},
    {SDL_SCANCODE_ESCAPE, 8, 128},
};
inline const M5HostKey* M5HostBinding(SDL_Scancode code) {
    for (const auto& key : m5_keyboard_layout) if (key.scancode == code) return &key;
    return nullptr;
}
inline bool M5HostContactHeld(int row, int mask, const bool* held, int count) {
    for (const auto& key : m5_keyboard_layout)
        if (key.row == row && key.mask == mask && key.scancode < count && held[key.scancode]) return true;
    return false;
}
#endif
