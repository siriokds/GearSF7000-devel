// SC-3000 host keyboard mapping for the integrated MacBook keyboard.
// Included by sk1100.cpp after t_sk1100_key has been declared.

#ifndef SK1100_MACOS_MAPPING_H
#define SK1100_MACOS_MAPPING_H

static const t_sk1100_key SK1100_KeyboardMode_Mapping[] =
{
    // Y7: works when an external keypad is present; a gamepad remains the
    // practical joystick input on a MacBook Air.
    { SDL_SCANCODE_KP_5, 7,    1,  0, "Joy 1 Up" },
    { SDL_SCANCODE_KP_2, 7,    2,  0, "Joy 1 Down" },
    { SDL_SCANCODE_KP_1, 7,    4,  0, "Joy 1 Left" },
    { SDL_SCANCODE_KP_3, 7,    8,  0, "Joy 1 Right" },
    { SDL_SCANCODE_KP_4, 7,   16,  0, "Joy 1 Trig 1" },
    { SDL_SCANCODE_KP_6, 7,   32,  0, "Joy 1 Trig 2" },

    { SDL_SCANCODE_1, 0, 1, '1', "1" }, { SDL_SCANCODE_Q, 0, 2, 'Q', "Q" },
    { SDL_SCANCODE_A, 0, 4, 'A', "A" }, { SDL_SCANCODE_Z, 0, 8, 'Z', "Z" },
    // Command is reserved by macOS. F4 is otherwise unused by the frontend
    // (F1, F5-F12 already have emulator/debug meanings) and is ENG on Mac.
    { SDL_SCANCODE_F4, 0, 16, 0, "ENG" },
    { SDL_SCANCODE_COMMA, 0, 32, ',', "," }, { SDL_SCANCODE_K, 0, 64, 'K', "K" },
    { SDL_SCANCODE_I, 0, 128, 'I', "I" }, { SDL_SCANCODE_8, 0, 256, '8', "8" },

    { SDL_SCANCODE_2, 1, 1, '2', "2" }, { SDL_SCANCODE_W, 1, 2, 'W', "W" },
    { SDL_SCANCODE_S, 1, 4, 'S', "S" }, { SDL_SCANCODE_X, 1, 8, 'X', "X" },
    { SDL_SCANCODE_SPACE, 1, 16, ' ', "Space" },
    { SDL_SCANCODE_PERIOD, 1, 32, '.', "." }, { SDL_SCANCODE_L, 1, 64, 'L', "L" },
    { SDL_SCANCODE_O, 1, 128, 'O', "O" }, { SDL_SCANCODE_9, 1, 256, '9', "9" },

    { SDL_SCANCODE_3, 2, 1, '3', "3" }, { SDL_SCANCODE_E, 2, 2, 'E', "E" },
    { SDL_SCANCODE_D, 2, 4, 'D', "D" }, { SDL_SCANCODE_C, 2, 8, 'C', "C" },
    // Right Option is next to Backspace, like Home/Clear on the SC-3000.
    { SDL_SCANCODE_RALT, 2, 16, 0, "Home/Clear" },
    { SDL_SCANCODE_SLASH, 2, 32, '/', "Slash" },
    { SDL_SCANCODE_SEMICOLON, 2, 64, ';', ";" }, { SDL_SCANCODE_P, 2, 128, 'P', "P" },
    { SDL_SCANCODE_0, 2, 256, '0', "0" },

    { SDL_SCANCODE_4, 3, 1, '4', "4" }, { SDL_SCANCODE_R, 3, 2, 'R', "R" },
    { SDL_SCANCODE_F, 3, 4, 'F', "F" }, { SDL_SCANCODE_V, 3, 8, 'V', "V" },
    { SDL_SCANCODE_BACKSPACE, 3, 16, 0, "Ins/Del" },
    // Row 5 mask 256, not row 3 mask 32: the ROM translation maps put the Yen
    // key here ($5C, and $7C for '|' with Shift). Row 3 mask 32 is the key
    // that gives nothing unshifted and '_' shifted - binding Yen there meant
    // this key typed nothing at all.
    { SDL_SCANCODE_BACKSLASH, 5, 256, '\\', "Yen / Backslash" },
    { SDL_SCANCODE_GRAVE, 5, 256, '\\', "Yen / Backslash" },
    { SDL_SCANCODE_APOSTROPHE, 3, 64, ':', "Colon" },
    { SDL_SCANCODE_MINUS, 3, 256, '-', "Minus" },

    { SDL_SCANCODE_5, 4, 1, '5', "5" }, { SDL_SCANCODE_T, 4, 2, 'T', "T" },
    { SDL_SCANCODE_G, 4, 4, 'G', "G" }, { SDL_SCANCODE_B, 4, 8, 'B', "B" },
    { SDL_SCANCODE_DOWN, 4, 32, 0, "Down" },
    { SDL_SCANCODE_RIGHTBRACKET, 4, 64, ']', "Right bracket" },
    { SDL_SCANCODE_LEFTBRACKET, 4, 128, '[', "Left bracket" },
    { SDL_SCANCODE_EQUALS, 4, 256, '^', "Caret" },

    { SDL_SCANCODE_6, 5, 1, '6', "6" }, { SDL_SCANCODE_Y, 5, 2, 'Y', "Y" },
    { SDL_SCANCODE_H, 5, 4, 'H', "H" }, { SDL_SCANCODE_N, 5, 8, 'N', "N" },
    { SDL_SCANCODE_LEFT, 5, 32, 0, "Left" },
    { SDL_SCANCODE_RETURN, 5, 64, '\n', "Return" },
    { SDL_SCANCODE_TAB, 5, 2048, 0, "Func" },

    { SDL_SCANCODE_7, 6, 1, '7', "7" }, { SDL_SCANCODE_U, 6, 2, 'U', "U" },
    { SDL_SCANCODE_J, 6, 4, 'J', "J" }, { SDL_SCANCODE_M, 6, 8, 'M', "M" },
    { SDL_SCANCODE_RIGHT, 6, 32, 0, "Right" }, { SDL_SCANCODE_UP, 6, 64, 0, "Up" },
    { SDL_SCANCODE_NONUSBACKSLASH, 6, 128, static_cast<char>(0xE3), "Pi" },
    { SDL_SCANCODE_F12, 6, 256, 0, "Break" },
    { SDL_SCANCODE_LALT, 6, 512, 0, "Graph" },
    { SDL_SCANCODE_LCTRL, 6, 1024, 0, "Ctrl" },
    { SDL_SCANCODE_LSHIFT, 6, 2048, 0, "Shift" },
    { SDL_SCANCODE_RSHIFT, 6, 2048, 0, "Shift" }
};

#endif
