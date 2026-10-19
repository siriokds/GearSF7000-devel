// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SYSTEM_INPUT_DESCRIPTOR_H
#define SYSTEM_INPUT_DESCRIPTOR_H

#include "definitions.h"

// What a motherboard offers as player input. The Input menu is built from
// this, so a board with no keyboard has no Keyboard Mode to offer and a board
// whose controllers carry a keypad (ColecoVision) lists it with the joypad,
// as part of the same controller, not as a machine keyboard.
struct SystemInputDescriptor
{
    u8 joypads;                 // controller ports
    bool joypadKeypad;          // 12-key keypad on each controller
    bool machineKeyboard;       // a keyboard matrix (SK-1100) is fitted
    const char* fire1Name;      // how the board's own manual calls them
    const char* fire2Name;
    const char* startName = "";
};

inline constexpr SystemInputDescriptor kSegaJoypadInput{
    2, false, false, "Fire 1 (Left)", "Fire 2 (Right)"};
inline constexpr SystemInputDescriptor kSegaJoypadKeyboardInput{
    2, false, true, "Fire 1 (Left)", "Fire 2 (Right)"};
inline constexpr SystemInputDescriptor kGameGearInput{
    1, false, false, "Button 1", "Button 2", "Start"};

#endif
