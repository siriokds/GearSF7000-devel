#ifndef SK1100_KEYBOARD_LAYOUT_H
#define SK1100_KEYBOARD_LAYOUT_H

#include <SDL3/SDL.h>

// Geometry is expressed in the compact SVG's 2193x692 coordinate space.
// row/mask identify the physical SK-1100 matrix contact. RESET is the sole
// key outside the matrix and therefore uses row -1.
struct SK1100KeyboardKeyLayout
{
    const char* id;
    const char* name;
    int x, y, w, h;
    int row;
    int mask;
    SDL_Scancode default_scancode;
};

constexpr int SK1100_KEYBOARD_IMAGE_WIDTH = 2193;
constexpr int SK1100_KEYBOARD_IMAGE_HEIGHT = 692;
constexpr int SK1100_KEYBOARD_KEY_COUNT = 64;

#if defined(__APPLE__)
#define SK1100_DEFAULT_ENG SDL_SCANCODE_F4
#define SK1100_DEFAULT_HOME SDL_SCANCODE_RALT
#define SK1100_DEFAULT_BREAK SDL_SCANCODE_F3
#else
#define SK1100_DEFAULT_ENG SDL_SCANCODE_APPLICATION
#define SK1100_DEFAULT_HOME SDL_SCANCODE_HOME
#define SK1100_DEFAULT_BREAK SDL_SCANCODE_PAUSE
#endif

static const SK1100KeyboardKeyLayout sk1100_keyboard_layout[SK1100_KEYBOARD_KEY_COUNT] =
{
    {"1", "1", 82,24,118,122, 0,1, SDL_SCANCODE_1},
    {"2", "2", 211,24,118,122, 1,1, SDL_SCANCODE_2},
    {"3", "3", 339,24,118,122, 2,1, SDL_SCANCODE_3},
    {"4", "4", 468,24,118,122, 3,1, SDL_SCANCODE_4},
    {"5", "5", 597,24,118,122, 4,1, SDL_SCANCODE_5},
    {"6", "6", 725,24,118,122, 5,1, SDL_SCANCODE_6},
    {"7", "7", 854,24,118,122, 6,1, SDL_SCANCODE_7},
    {"8", "8", 983,24,118,122, 0,256, SDL_SCANCODE_8},
    {"9", "9", 1112,24,118,122, 1,256, SDL_SCANCODE_9},
    {"0", "0", 1240,24,118,122, 2,256, SDL_SCANCODE_0},
    {"minus", "Minus", 1369,24,118,122, 3,256, SDL_SCANCODE_MINUS},
    {"caret", "Caret", 1498,24,118,122, 4,256, SDL_SCANCODE_EQUALS},
    {"pound", "Pound", 1626,24,118,122, 5,256, SDL_SCANCODE_BACKSLASH},

    {"Q", "Q", 153,157,118,122, 0,2, SDL_SCANCODE_Q},
    {"W", "W", 282,157,118,122, 1,2, SDL_SCANCODE_W},
    {"E", "E", 410,157,118,122, 2,2, SDL_SCANCODE_E},
    {"R", "R", 539,157,118,122, 3,2, SDL_SCANCODE_R},
    {"T", "T", 668,157,118,122, 4,2, SDL_SCANCODE_T},
    {"Y", "Y", 797,157,118,122, 5,2, SDL_SCANCODE_Y},
    {"U", "U", 925,157,118,122, 6,2, SDL_SCANCODE_U},
    {"I", "I", 1054,157,118,122, 0,128, SDL_SCANCODE_I},
    {"O", "O", 1183,157,118,122, 1,128, SDL_SCANCODE_O},
    {"P", "P", 1311,157,118,122, 2,128, SDL_SCANCODE_P},
    {"at", "At", 1440,157,118,122, 3,128, SDL_SCANCODE_UNKNOWN},
    {"lbracket", "Left bracket", 1569,157,118,122, 4,128, SDL_SCANCODE_LEFTBRACKET},

    {"A", "A", 188,288,118,122, 0,4, SDL_SCANCODE_A},
    {"S", "S", 317,288,118,122, 1,4, SDL_SCANCODE_S},
    {"D", "D", 445,288,118,122, 2,4, SDL_SCANCODE_D},
    {"F", "F", 574,288,118,122, 3,4, SDL_SCANCODE_F},
    {"G", "G", 703,288,118,122, 4,4, SDL_SCANCODE_G},
    {"H", "H", 831,288,118,122, 5,4, SDL_SCANCODE_H},
    {"J", "J", 960,288,118,122, 6,4, SDL_SCANCODE_J},
    {"K", "K", 1089,288,118,122, 0,64, SDL_SCANCODE_K},
    {"L", "L", 1218,288,118,122, 1,64, SDL_SCANCODE_L},
    {"plus", "Plus", 1346,288,118,122, 2,64, SDL_SCANCODE_SEMICOLON},
    {"star", "Asterisk", 1475,288,118,122, 3,64, SDL_SCANCODE_APOSTROPHE},
    {"rbracket", "Right bracket", 1604,288,118,122, 4,64, SDL_SCANCODE_RIGHTBRACKET},

    {"Z", "Z", 250,419,118,122, 0,8, SDL_SCANCODE_Z},
    {"X", "X", 379,419,118,122, 1,8, SDL_SCANCODE_X},
    {"C", "C", 507,419,118,122, 2,8, SDL_SCANCODE_C},
    {"V", "V", 636,419,118,122, 3,8, SDL_SCANCODE_V},
    {"B", "B", 765,419,118,122, 4,8, SDL_SCANCODE_B},
    {"N", "N", 893,419,118,122, 5,8, SDL_SCANCODE_N},
    {"M", "M", 1022,419,118,122, 6,8, SDL_SCANCODE_M},
    {"less", "Comma / Less", 1151,419,118,122, 0,32, SDL_SCANCODE_COMMA},
    {"greater", "Period / Greater", 1280,419,118,122, 1,32, SDL_SCANCODE_PERIOD},
    {"question", "Slash / Question", 1408,419,118,122, 2,32, SDL_SCANCODE_SLASH},
    {"pi", "Pi", 1537,419,118,122, 6,128, SDL_SCANCODE_NONUSBACKSLASH},

    {"func", "Func", 24,157,118,122, 5,2048, SDL_SCANCODE_TAB},
    {"ctrl", "Ctrl", 27,288,149,122, 6,1024, SDL_SCANCODE_LCTRL},
    {"shift_l", "Left Shift", 57,419,180,122, 6,2048, SDL_SCANCODE_LSHIFT},
    {"shift_r", "Right Shift", 1661,419,180,122, 6,2048, SDL_SCANCODE_RSHIFT},
    {"graph", "Graph", 315,546,118,122, 6,512, SDL_SCANCODE_LALT},
    {"eng", "Eng/Dier's", 465,546,118,122, 0,16, SK1100_DEFAULT_ENG},
    {"space", "Space", 633,564,627,90, 1,16, SDL_SCANCODE_SPACE},
    {"home", "Home/Clear", 1312,546,118,122, 2,16, SK1100_DEFAULT_HOME},
    {"insdel", "Ins/Del", 1463,546,118,122, 3,16, SDL_SCANCODE_BACKSPACE},
    {"break", "Break", 1805,24,118,122, 6,256, SK1100_DEFAULT_BREAK},
    {"reset", "Reset / NMI", 2051,24,118,122, -1,0, SDL_SCANCODE_F1},
    {"cr", "Return", 1729,159,118,242, 5,64, SDL_SCANCODE_RETURN},
    {"up", "Up", 1979,157,118,122, 6,64, SDL_SCANCODE_UP},
    {"left", "Left", 1911,288,118,122, 5,32, SDL_SCANCODE_LEFT},
    {"right", "Right", 2047,288,118,122, 6,32, SDL_SCANCODE_RIGHT},
    {"down", "Down", 1979,419,118,122, 4,32, SDL_SCANCODE_DOWN}
};

#undef SK1100_DEFAULT_ENG
#undef SK1100_DEFAULT_HOME
#undef SK1100_DEFAULT_BREAK

#endif
