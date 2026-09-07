#include "sk1100.h"
#include <iostream>
#include <cctype>
#if GEARSF7000_ENABLE_SDL_INPUT
#include <SDL3/SDL.h>
#endif
#include "SaveStateStream.h"
#include "sk1100_translation.h"

namespace
{
constexpr std::uint16_t kSK1100StateVersion = 1;
}

//#define 

typedef struct
{
	int		pc_keycode;
	int		row;
	int		mask;
	char    keychar;
	char	def_string[24];

}  t_sk1100_key;

#if GEARSF7000_ENABLE_SDL_INPUT

#define SK1100_JOY_NUM 12+2+6

//t_sk1100_key SK1100_JoystickMode_Mapping[SK1100_NUM_KEYS] =
//{
//
//	// Row: 7 -------------------------------------------------------------------
//	{ SDLK_UP,      7,    1,  0 ,"Joy 1 Up" },
//	{ SDLK_DOWN,    7,    2,  0 ,"Joy 1 Down" },
//	{ SDLK_LEFT,    7,    4,  0 ,"Joy 1 Left" },
//	{ SDLK_RIGHT,   7,    8,  0 ,"Joy 1 Right" },
//	{ SDLK_a,       7,   16,  0 ,"Joy 1 Trig 1" },
//	{ SDLK_s,       7,   32,  0 ,"Joy 1 Trig 2" },
//
//	{ SDLK_KP_5,    7,   64,  0 ,"Joy 2 Up" },
//	{ SDLK_KP_2,    7,  128,  0 ,"Joy 2 Down" },
//
//	{ SDLK_KP_1,    7,    1 << 8,  0 ,"Joy 2 Left" },
//	{ SDLK_KP_3,    7,    2 << 8,  0 ,"Joy 2 Right" },
//	{ SDLK_KP_4,    7,    4 << 8,  0 ,"Joy 2 Trig 1" },
//	{ SDLK_KP_6,    7,    8 << 8,  0 ,"Joy 2 Trig 2" },
//};
//
//t_sk1100_key SK1100_KeyboardMode_Mapping [SK1100_NUM_KEYS] = 
//{
//
//	// Row: 7 -------------------------------------------------------------------
//	{ SDLK_KP_5,       7,    1,  0 ,"Joy 1 Up" },
//	{ SDLK_KP_2,       7,    2,  0 ,"Joy 1 Down" },
//	{ SDLK_KP_1,       7,    4,  0 ,"Joy 1 Left" },
//	{ SDLK_KP_3,       7,    8,  0 ,"Joy 1 Right" },
//	{ SDLK_KP_4,       7,   16,  0 ,"Joy 1 Trig 1" },
//	{ SDLK_KP_6,       7,   32,  0 ,"Joy 1 Trig 2" },
//	
//
//
//
//
//	// Row: 0 -------------------------------------------------------------------
//	{ SDLK_1,          0,    1, '1', "1"              },
//	{ SDLK_q,          0,    2, 'Q', "Q"              },
//	{ SDLK_a,          0,    4, 'A', "A"              },
//	{ SDLK_z,          0,    8, 'Z', "Z"              },
//	//{ SDLK_LGUI,       0,   16,  0 , "Eng Dier's"     },
//	{ SDLK_APPLICATION,       0,   16,  0 , "Eng Dier's"     },
//	{ SDLK_COMMA,      0,   32, ',', ","              },
//	{ SDLK_k,          0,   64, 'K', "K"              },
//	{ SDLK_i,          0,  128, 'I', "I"              },
//	{ SDLK_8,          0,  256, '8', "8"              },
//	// Row: 1 -------------------------------------------------------------------
//
//	{ SDLK_2,          1,    1, '2', "2"              },
//	{ SDLK_w,          1,    2, 'W', "W"              },
//	{ SDLK_s,          1,    4, 'S', "S"              },
//	{ SDLK_x,          1,    8, 'X', "X"              },
//	{ SDLK_SPACE,      1,   16, ' ', "Space"          },
//	{ SDLK_PERIOD,     1,   32, '.', "."              },
//	{ SDLK_l,          1,   64, 'L', "L"              },
//	{ SDLK_o,          1,  128, 'O', "O"              },
//	{ SDLK_9,          1,  256, '9', "9"              },
//	// Row: 2 -------------------------------------------------------------------
//	{ SDLK_3,          2,    1, '3', "3"              },
//	{ SDLK_e,          2,    2, 'E', "E"              },
//	{ SDLK_d,          2,    4, 'D', "D"              },
//	{ SDLK_c,          2,    8, 'C', "C"              },
//	{ SDLK_RCTRL,      2,   16,  0 , "Home/Clear"     },
//	{ SDLK_SLASH,      2,   32, '/', "Slash"          },
//	{ SDLK_SEMICOLON,  2,   64, ';', ";"              },
//	{ SDLK_p,          2,  128, 'P', "P"              },
//	{ SDLK_0,          2,  256, '0', "0"              },
//	// Row: 3 -------------------------------------------------------------------
//	{ SDLK_4,          3,    1, '4', "4"              },
//	{ SDLK_r,          3,    2, 'R', "R"              },
//	{ SDLK_f,          3,    4, 'F', "F"              },
//	{ SDLK_v,          3,    8, 'V', "V"              },
//	/*{ SDLK_RALT,	   3,   16,  0 , "Insert/Delete"  },*/
//	{ SDLK_BACKSPACE,  3,   16,  0 , "Insert/Delete"  }, // ++
//	{ SDLK_KP_VERTICALBAR,			   3,   32,  0 , "Pi"             },
//	{ SDLK_COLON,	   3,   64, ':', ":"              },
//	{ SDLK_AT,		   3,  128, '@', "@"              },
//	{ SDLK_MINUS,      3,  256, '-', "-"              },
//
//	// Row: 4 -------------------------------------------------------------------
//	{ SDLK_5,          4,    1, '5', "5"              },
//	{ SDLK_t,          4,    2, 'T', "T"              },
//	{ SDLK_g,          4,    4, 'G', "G"              },
//	{ SDLK_b,          4,    8, 'B', "B"              },
//	{ SDLK_DOWN,       4,   32,  0 , "Down Arrow"     },
//	{ SDLK_LEFTBRACKET,  4,   64, ']', "]"              },
//	{ SDLK_RIGHTBRACKET,   4,  128, '[', "["              },
//	{ 0x000000ec,     4,  256, '^', "^"              },
//	// Row: 5 -------------------------------------------------------------------
//	{ SDLK_6,          5,    1, '6', "6"              },
//	{ SDLK_y,          5,    2, 'Y', "Y"              },
//	{ SDLK_h,          5,    4, 'H', "H"              },
//	{ SDLK_n,          5,    8, 'N', "N"              },
//	{ SDLK_LEFT,       5,   32,  0 , "Left Arrow"     },
//	{ SDLK_RETURN,     5,   64, '\n', "Return"         },
//	{ 0x00000027,  5,  256,  0 , "\x9D"               }, // !!
//	{ SDLK_TAB,        5, 2048,  0 , "Func"           },
// 
//	// Row: 6 -------------------------------------------------------------------
//	{ SDLK_7,          6,    1, '7', "7"              },
//	{ SDLK_u,          6,    2, 'U', "U"              },
//	{ SDLK_j,          6,    4, 'J', "J"              },
//	{ SDLK_m,          6,    8, 'M', "M"              },
//	{ SDLK_RIGHT,      6,   32,  0 ,"Right Arrow"    },
//	{ SDLK_UP,         6,   64,  0 ,"Up Arrow"       },
//	{ SDLK_PAUSE,      6,  256,  0 ,"Break"          }, // !!
//	{ SDLK_LALT,       6,  512,  0 ,"Graph"          },
//	{ SDLK_LCTRL,      6, 1024,  0 ,"Ctrl"           },
//	{ SDLK_LSHIFT,     6, 2048,  0 ,"Shift"          },
//	//{ SDLK_RSHIFT,     6, 2048,  0 ,"Shift"          }  // ++
//	{ SDLK_RSHIFT,     2,   16,  0 , "Home/Clear"     }
//
//
//};


t_sk1100_key SK1100_JoystickMode_Mapping[SK1100_JOY_NUM] =
{

	// Row: 7 -------------------------------------------------------------------
	{ SDL_SCANCODE_UP,			7,    1,  0 ,"Joy 1 Up" },
	{ SDL_SCANCODE_DOWN,		7,    2,  0 ,"Joy 1 Down" },
	{ SDL_SCANCODE_LEFT,		7,    4,  0 ,"Joy 1 Left" },
	{ SDL_SCANCODE_RIGHT,		7,    8,  0 ,"Joy 1 Right" },
	{ SDL_SCANCODE_A,			7,   16,  0 ,"Joy 1 Trig 1" },
	{ SDL_SCANCODE_S,			7,   32,  0 ,"Joy 1 Trig 2" },

	{ SDL_SCANCODE_KP_5,		7,   64,  0 ,"Joy 2 Up" },
	{ SDL_SCANCODE_KP_2,		7,  128,  0 ,"Joy 2 Down" },

	{ SDL_SCANCODE_KP_1,		7,    1 << 8,  0 ,"Joy 2 Left" },
	{ SDL_SCANCODE_KP_3,		7,    2 << 8,  0 ,"Joy 2 Right" },
	{ SDL_SCANCODE_KP_4,		7,    4 << 8,  0 ,"Joy 2 Trig 1" },
	{ SDL_SCANCODE_KP_6,		7,    8 << 8,  0 ,"Joy 2 Trig 2" },

	{ SDL_SCANCODE_I,		7,   64,  0 ,"Joy 2 Up" },
	{ SDL_SCANCODE_K,		7,  128,  0 ,"Joy 2 Down" },
	{ SDL_SCANCODE_J,		7,    1 << 8,  0 ,"Joy 2 Left" },
	{ SDL_SCANCODE_L,		7,    2 << 8,  0 ,"Joy 2 Right" },
	{ SDL_SCANCODE_Z,		7,    4 << 8,  0 ,"Joy 2 Trig 1" },
	{ SDL_SCANCODE_X,		7,    8 << 8,  0 ,"Joy 2 Trig 2" },


	{ SDL_SCANCODE_KP_7,		7,    0x8000,  0 ,"Service Menu" },
	{ SDL_SCANCODE_KP_8,		7,    0x4000,  0 ,"Coin" }
};

#if defined(__APPLE__)
#include "sk1100_macos.h"
#else
#include "sk1100_windows.h"
#endif

constexpr int SK1100_NUM_KEYS = static_cast<int>(
    sizeof(SK1100_KeyboardMode_Mapping) / sizeof(SK1100_KeyboardMode_Mapping[0]));
#endif

// How to produce one character on the matrix. The host-keyboard path does not
// need this - SDL reports the shifted character itself - but text injected
// from a file or a paste has no such help.
//
// This used to be a hand-built table of shifted pairs, verified key by key on
// the running machine. The ROM's own translation maps replaced it: they are
// what the machine consults, so they are the authority, and they carry the
// graphic and accented sets that guesswork could never have reached. The
// hand-built version also had the Yen key in the wrong matrix position, which
// the maps caught.
struct t_sk1100_keystroke
{
	int  row;
	int  mask;
	bool shift;
	bool graph;   // needs the persistent GRAPH mode, which the GRAPH key toggles
	bool eng;     // needs ENG/DIER held, for the accented set
};

// Resolves a character code to the keystroke that produces it, preferring the
// map that needs fewest modifiers. Returns false when no key on the SK-1100
// can produce it: the graphic codes a program defines for itself, for
// instance, exist in no map. A caller must be prepared for that rather than
// assume printable ASCII is typable - it is neither a subset nor a superset of
// what this keyboard can do.
static bool SK1100_FindKeyForChar(char character, t_sk1100_keystroke* stroke)
{
	// Newline in either convention is the Return key, whose code is $0D.
	unsigned char code = (unsigned char)character;
	if (code == '\n' || code == '\r')
		code = 0x0D;

	static const struct { const unsigned char* map; bool shift; bool graph; bool eng; } lookup[] =
	{
		{ SK1100_TranslationMapLatin,        false, false, false },
		{ SK1100_TranslationMapLatinShifted, true,  false, false },
		{ SK1100_TranslationMapGraphics1,    false, true,  false },
		{ SK1100_TranslationMapGraphics2,    true,  true,  false },
		{ SK1100_TranslationMapExtended,     false, false, true  },
	};

	if (code == 0)
		return false;   // $00 fills every unused slot in every map

	for (size_t m = 0; m < sizeof(lookup) / sizeof(lookup[0]); m++)
		for (int i = 0; i < 64; i++)
			if (lookup[m].map[i] == code)
			{
				if (stroke)
				{
					SK1100_TranslationIndexToPosition(i, &stroke->row, &stroke->mask);
					stroke->shift = lookup[m].shift;
					stroke->graph = lookup[m].graph;
					stroke->eng   = lookup[m].eng;
				}
				return true;
			}

	return false;
}

bool SK1100::CanTypeChar(char character)
{
	return SK1100_FindKeyForChar(character, 0);
}


//t_sk1100_key SK1100_KeyboardMode_Mapping[SK1100_NUM_KEYS] =
//{
//
//	// Row: 7 -------------------------------------------------------------------
//	{ SDL_SCANCODE_KP_5,		7,    1,  0 ,"Joy 1 Up" },
//	{ SDL_SCANCODE_KP_2,		7,    2,  0 ,"Joy 1 Down" },
//	{ SDL_SCANCODE_KP_1,		7,    4,  0 ,"Joy 1 Left" },
//	{ SDL_SCANCODE_KP_3,		7,    8,  0 ,"Joy 1 Right" },
//	{ SDL_SCANCODE_KP_4,		7,   16,  0 ,"Joy 1 Trig 1" },
//	{ SDL_SCANCODE_KP_6,		7,   32,  0 ,"Joy 1 Trig 2" },
//
//
//
//
//
//	// Row: 0 -------------------------------------------------------------------
//	{ SDL_SCANCODE_1,          0,    1, '1', "1"              },
//	{ SDL_SCANCODE_Q,          0,    2, 'Q', "Q"              },
//	{ SDL_SCANCODE_A,          0,    4, 'A', "A"              },
//	{ SDL_SCANCODE_Z,          0,    8, 'Z', "Z"              },
//	//{ SDL_SCANCODE_LGUI,       0,   16,  0 , "Eng Dier's"     },
//	{ SDL_SCANCODE_APPLICATION,  0,   16,  0 , "Eng Dier's"     },
//	{ SDL_SCANCODE_COMMA,      0,   32, ',', ","              },
//	{ SDL_SCANCODE_K,          0,   64, 'K', "K"              },
//	{ SDL_SCANCODE_I,          0,  128, 'I', "I"              },
//	{ SDL_SCANCODE_8,          0,  256, '8', "8"              },
//	// Row: 1 -------------------------------------------------------------------
//
//	{ SDL_SCANCODE_2,          1,    1, '2', "2"              },
//	{ SDL_SCANCODE_W,          1,    2, 'W', "W"              },
//	{ SDL_SCANCODE_S,          1,    4, 'S', "S"              },
//	{ SDL_SCANCODE_X,          1,    8, 'X', "X"              },
//	{ SDL_SCANCODE_SPACE,      1,   16, ' ', "Space"          },
//	{ SDL_SCANCODE_PERIOD,     1,   32, '.', "."              },
//	{ SDL_SCANCODE_L,          1,   64, 'L', "L"              },
//	{ SDL_SCANCODE_O,          1,  128, 'O', "O"              },
//	{ SDL_SCANCODE_9,          1,  256, '9', "9"              },
//	// Row: 2 -------------------------------------------------------------------
//	{ SDL_SCANCODE_3,          2,    1, '3', "3"              },
//	{ SDL_SCANCODE_E,          2,    2, 'E', "E"              },
//	{ SDL_SCANCODE_D,          2,    4, 'D', "D"              },
//	{ SDL_SCANCODE_C,          2,    8, 'C', "C"              },
//	//{ SDL_SCANCODE_RCTRL,      2,   16,  0 , "Home/Clear"     },
//	{ SDL_SCANCODE_INSERT,      2,   16,  0 , "Home/Clear"     },
//	{ SDL_SCANCODE_SLASH,      2,   32, '/', "Slash"          },
//	{ SDL_SCANCODE_SEMICOLON,  2,   64, ';', ";"              },
//	{ SDL_SCANCODE_P,          2,  128, 'P', "P"              },
//	{ SDL_SCANCODE_0,          2,  256, '0', "0"              },
//	// Row: 3 -------------------------------------------------------------------
//	{ SDL_SCANCODE_4,          3,    1, '4', "4"              },
//	{ SDL_SCANCODE_R,          3,    2, 'R', "R"              },
//	{ SDL_SCANCODE_F,          3,    4, 'F', "F"              },
//	{ SDL_SCANCODE_V,          3,    8, 'V', "V"              },
//	/*{ SDL_SCANCODE_RALT,	   3,   16,  0 , "Insert/Delete"  },*/
//	{ SDL_SCANCODE_BACKSPACE,  3,   16,  0 , "Insert/Delete"  }, // ++
//
//	{ SDL_SCANCODE_KP_VERTICALBAR,	3,   32,  0 , "Pi"             },
//	{ SDL_SCANCODE_APOSTROPHE,      3,   64, ':', ":"              },
//	{ SDL_SCANCODE_BACKSLASH,		3,  128, '@', "@"              },
//	{ SDL_SCANCODE_MINUS,           3,  256, '-', "-"              },
//
//	// Row: 4 -------------------------------------------------------------------
//	{ SDL_SCANCODE_5,          4,    1, '5', "5"              },
//	{ SDL_SCANCODE_T,          4,    2, 'T', "T"              },
//	{ SDL_SCANCODE_G,          4,    4, 'G', "G"              },
//	{ SDL_SCANCODE_B,          4,    8, 'B', "B"              },
//	{ SDL_SCANCODE_DOWN,       4,   32,  0 , "Down Arrow"     },
//
//	{ SDL_SCANCODE_RIGHTBRACKET,  4,   64, ']', "]"              },
//	{ SDL_SCANCODE_LEFTBRACKET,   4,  128, '[', "["              },
//	{ SDL_SCANCODE_EQUALS,        4,  256, '^', "^"              },
//	// Row: 5 -------------------------------------------------------------------
//	{ SDL_SCANCODE_6,          5,    1, '6', "6"              },
//	{ SDL_SCANCODE_Y,          5,    2, 'Y', "Y"              },
//	{ SDL_SCANCODE_H,          5,    4, 'H', "H"              },
//	{ SDL_SCANCODE_N,          5,    8, 'N', "N"              },
//	{ SDL_SCANCODE_LEFT,       5,   32,  0 , "Left Arrow"     },
//	{ SDL_SCANCODE_RETURN,     5,   64, '\n', "Return"         },
//	{ SDL_SCANCODE_GRAVE,      5,  256,  0 , "\x9D"               }, // !!
//	{ SDL_SCANCODE_TAB,        5, 2048,  0 , "Func"           },
//
//	// Row: 6 -------------------------------------------------------------------
//	{ SDL_SCANCODE_7,          6,    1, '7', "7"              },
//	{ SDL_SCANCODE_U,          6,    2, 'U', "U"              },
//	{ SDL_SCANCODE_J,          6,    4, 'J', "J"              },
//	{ SDL_SCANCODE_M,          6,    8, 'M', "M"              },
//	{ SDL_SCANCODE_RIGHT,      6,   32,  0 ,"Right Arrow"    },
//	{ SDL_SCANCODE_UP,         6,   64,  0 ,"Up Arrow"       },
//	{ SDL_SCANCODE_PAUSE,      6,  256,  0 ,"Break"          }, // !!
//	{ SDL_SCANCODE_LALT,       6,  512,  0 ,"Graph"          },
//	{ SDL_SCANCODE_LCTRL,      6, 1024,  0 ,"Ctrl"           },
//	{ SDL_SCANCODE_LSHIFT,     6, 2048,  0 ,"Shift"          },
//	{ SDL_SCANCODE_RSHIFT,     6, 2048,  0 ,"Shift"          }  // ++
//	//{ SDL_SCANCODE_APPLICATION,     2,   16,  0 , "Home/Clear"     }
//
//
//};


/*-------------------------------------------------------*/
SK1100::SK1100(CpuInterruptLines* interruptLines)
/*-------------------------------------------------------*/
{
	m_interruptLines = interruptLines;
	m_iInputCycles = 0;
	m_text_queue_index = 0;
	m_text_ticks_left = 0;
	m_text_press_polls = 3;
	m_text_release_polls = 2;
	m_text_newline_polls = 25;
}

void SK1100::SetTextTiming(int pressPolls, int releasePolls, int newlinePolls)
{
	// One poll is the floor: an event with no duration would be overwritten by
	// the next one within the same matrix scan and never seen at all.
	m_text_press_polls = pressPolls > 0 ? pressPolls : 1;
	m_text_release_polls = releasePolls > 0 ? releasePolls : 1;
	m_text_newline_polls = newlinePolls > 0 ? newlinePolls : 1;
}


/*-------------------------------------------------------*/
void SK1100::Init()
/*-------------------------------------------------------*/
{
	m_keyb_enabled = true;

	Reset();
}

/*-------------------------------------------------------*/
void	SK1100::GetRows(uint16_t* _rows)
/*-------------------------------------------------------*/
{
	if (_rows == 0) return;

	for (int i = 0; i < 8; i++)
	{
		_rows[i] = m_keyb_rows[i];
	}
}

/*-------------------------------------------------------*/
void SK1100::Reset()
/*-------------------------------------------------------*/
{
	m_iInputCycles = 0;
	m_keyb_row = 7;	// Default row 7 = Joystick (SG-1000)

	for (int i = 0; i < 8; i++)	m_keyb_rows[i] = 0xFFFF;
	ClearTextQueue();

#ifdef _SK_DEBUG
	std::cout << "KBD: Reset - Mode: " << (m_keyb_enabled ? "Keyboard" : "Joystick") << std::endl;
#endif


}

/*-------------------------------------------------------*/
void	SK1100::SetKeyboardMode(bool keyboardModeEnabled)
/*-------------------------------------------------------*/
{
	if (m_keyb_enabled == keyboardModeEnabled)
		return;

	m_keyb_enabled = keyboardModeEnabled;
	Reset();
}

/*-------------------------------------------------------*/
int		SK1100::GetRow(void) 
/*-------------------------------------------------------*/
{ 
		return m_keyb_row; 
}


/*-------------------------------------------------------*/
uint8_t	SK1100::GetPortA(void)
/*-------------------------------------------------------*/
{
uint8_t	temp = 0xFF;


		temp = (uint8_t)(m_keyb_rows[m_keyb_row] & 0x00FF);

		return temp;
}


/*-------------------------------------------------------*/
uint8_t	SK1100::GetPortB(void)
/*-------------------------------------------------------*/
{
uint8_t	temp = 0xFF;
		
		temp = (uint8_t)(m_keyb_rows[m_keyb_row] >> 8);

		return temp;
}


/*-------------------------------------------------------*/
void	SK1100::SetPortC(int val)
/*-------------------------------------------------------*/
{
	m_keyb_val = val;
	m_keyb_row = m_keyb_val & 7;
}


/*-------------------------------------------------------*/
uint8_t	SK1100::GetPortC(void)
/*-------------------------------------------------------*/
{
	return m_keyb_enabled ? m_keyb_val : 0xFF;
}

/*-------------------------------------------------------*/
int		SK1100::GetSystemKey(int n)
/*-------------------------------------------------------*/
{
	return (m_keyb_systemkeys & (1 << n));
}


/*-------------------------------------------------------*/
void SK1100::Tick(unsigned int clockCycles)
/*-------------------------------------------------------*/
{
	// Physical keyboard and joystick edges update the matrix directly and do
	// not need a periodic clock. Only the automatic text queue has timed work.
	if (!HasPendingTimedEvents()) return;

	m_iInputCycles += clockCycles;

	// Preserve the historical synthetic-key cadence. Use a loop so a future
	// event scheduler may deliver a large block without losing transitions.
	while (HasPendingTimedEvents() && m_iInputCycles >= 71591)
	{
		m_iInputCycles -= 71591;
		AdvanceTextQueue();
		Update();
	}

	if (!HasPendingTimedEvents()) m_iInputCycles = 0;
}

void SK1100::SetMatrixKey(int row, int mask, bool pressed)
{
	if (row < 0 || row >= 8 || mask <= 0) return;
	if (pressed) m_keyb_rows[row] &= ~mask;
	else m_keyb_rows[row] |= mask;
}

bool SK1100::PressMatrixKeyByLabel(const std::string& label, bool pressed)
{
#if GEARSF7000_ENABLE_SDL_INPUT
	for (int i = 0; i < SK1100_NUM_KEYS; i++)
	{
		const t_sk1100_key& entry = SK1100_KeyboardMode_Mapping[i];
		if (label == entry.def_string)
		{
			SetMatrixKey(entry.row, entry.mask, pressed);
			return true;
		}
	}
#else
	// Printable contacts are described by the SC-3000 ROM translation table,
	// not by any host keyboard. Modifiers and non-printable contacts have
	// stable machine coordinates listed below.
	if (label.size() == 1)
	{
		t_sk1100_keystroke stroke;
		if (SK1100_FindKeyForChar(label[0], &stroke) &&
			!stroke.shift && !stroke.graph && !stroke.eng)
		{
			SetMatrixKey(stroke.row, stroke.mask, pressed);
			return true;
		}
	}

	static const struct { const char* label; int row; int mask; } keys[] =
	{
		{ "Space", 1, 16 }, { "Home/Clear", 2, 16 },
		{ "Ins/Del", 3, 16 }, { "Down", 4, 32 },
		{ "Left", 5, 32 }, { "Return", 5, 64 },
		{ "Func", 5, 2048 }, { "Right", 6, 32 },
		{ "Up", 6, 64 }, { "Break", 6, 256 },
		{ "Graph", 6, 512 }, { "Ctrl", 6, 1024 },
		{ "Shift", 6, 2048 }, { "ENG", 0, 16 }
	};
	for (const auto& key : keys)
	{
		if (label == key.label)
		{
			SetMatrixKey(key.row, key.mask, pressed);
			return true;
		}
	}
#endif
	return false;
}

void SK1100::SetMatrixKeyState(int row, int mask, bool pressed)
{
	SetMatrixKey(row, mask, pressed);
}

void SK1100::AdvanceTextQueue(void)
{
	if (m_text_queue_index >= m_text_queue.size()) return;
	if (m_text_ticks_left > 0 && --m_text_ticks_left > 0) return;
	const QueuedKey& key = m_text_queue[m_text_queue_index++];
	SetMatrixKey(key.row, key.mask, key.pressed);
	m_text_ticks_left = key.ticks;
	if (m_text_queue_index == m_text_queue.size() && !key.pressed)
	{
		m_text_queue.clear();
		m_text_queue_index = 0;
		m_text_ticks_left = 0;
	}
}

void SK1100::ClearTextQueue(void)
{
	if (m_text_queue_index > 0 && m_text_queue_index <= m_text_queue.size())
	{
		const QueuedKey& key = m_text_queue[m_text_queue_index - 1];
		if (key.pressed) SetMatrixKey(key.row, key.mask, false);
	}
	m_text_queue.clear();
	m_text_queue_index = 0;
	m_text_ticks_left = 0;
	m_iInputCycles = 0;
}

bool SK1100::QueueText(const std::string& text, std::string* error)
{
	if (m_text_queue_index < m_text_queue.size())
	{
		if (error) *error = "A keyboard text sequence is already active";
		return false;
	}
	m_iInputCycles = 0;
	const int shiftRow = 6, shiftMask = 2048;
	const int graphRow = 6, graphMask = 512;
	const int engRow = 0, engMask = 16;
	const int press_polls = m_text_press_polls;
	const int release_polls = m_text_release_polls;
	const int newline_settle_polls = m_text_newline_polls;

	// GRAPH is a switch, not a held modifier: pressing it changes mode and
	// pressing it again changes back, and the prompt cursor turns into an
	// asterisk while graphics are selected. So the mode has to be tracked
	// across the whole sequence and left the way it was found - a run of
	// graphic characters costs one toggle, not one per character.
	//
	// The machine's actual mode cannot be read from here, so this assumes it
	// starts in Latin, which is where the prompt normally sits, and always
	// returns there at the end.
	bool graph_mode = false;

	// Let the BASIC keyboard scanner observe an idle matrix before the first
	// synthetic edge.  Without this settling gap the first typed character can
	// be swallowed by a scan already in progress. It uses the same hold as a
	// line ending, because it is the same problem: the machine may still be
	// busy with whatever it was doing when the text was sent. Two polls, the
	// old value, was not enough to cover that.
	m_text_queue.push_back({-1, 0, false, newline_settle_polls});
	for (size_t index = 0; index < text.size(); index++)
	{
		// A CR immediately followed by LF is one line ending, not two. The
		// .basic files here are CRLF, and pressing Return twice per line
		// would double the time and litter the listing with blank entries.
		// This belongs here rather than in a caller: MCP's keyboard_text
		// reaches this function directly, without passing through the panel.
		if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n')
			continue;

		t_sk1100_keystroke stroke;
		const bool found_newline = text[index] == '\n' || text[index] == '\r';
		if (!SK1100_FindKeyForChar(text[index], &stroke))
		{
			if (error) *error = "Unsupported SC-3000 character at offset " + std::to_string(index);
			ClearTextQueue();
			return false;
		}

		if (stroke.graph != graph_mode)
		{
			m_text_queue.push_back({graphRow, graphMask, true, press_polls});
			m_text_queue.push_back({graphRow, graphMask, false, release_polls});
			graph_mode = stroke.graph;
		}

		// Held for press_polls matrix scans and released for release_polls:
		// counted in polls rather than host frames, so the cadence does not
		// change with the display rate or window focus.
		if (stroke.eng)   m_text_queue.push_back({engRow, engMask, true, press_polls});
		if (stroke.shift) m_text_queue.push_back({shiftRow, shiftMask, true, press_polls});
		m_text_queue.push_back({stroke.row, stroke.mask, true, press_polls});
		m_text_queue.push_back({stroke.row, stroke.mask, false, release_polls});
		if (stroke.shift) m_text_queue.push_back({shiftRow, shiftMask, false, release_polls});
		if (stroke.eng)   m_text_queue.push_back({engRow, engMask, false, release_polls});

		// BASIC does not buffer keystrokes: while it tokenises the line just
		// entered and inserts it into the program, it is not scanning the
		// matrix, and everything typed meanwhile is simply gone. Without this
		// hold, an eleven-line listing lost the first five to seven characters
		// of every line - "20 RESTORE 100" arrived as "TORE 100" - and each
		// mangled line printed "? Syntax error", which costs BASIC more time
		// still and ate the start of the line after it.
		//
		// Ten polls was measured to be enough for that listing, typed from a
		// settled prompt. Twenty-five is margin for the slower case, a line
		// that errors, which has not been measured. Half a second a line is
		// cheap next to losing characters.
		if (found_newline)
			m_text_queue.push_back({-1, 0, false, newline_settle_polls});
	}

	// Leave the keyboard as it was found. A sequence that ended on a graphic
	// character would otherwise hand the user a machine still in GRAPH mode,
	// where their next keystroke produces a picture.
	if (graph_mode)
	{
		m_text_queue.push_back({graphRow, graphMask, true, press_polls});
		m_text_queue.push_back({graphRow, graphMask, false, release_polls});
	}

	AdvanceTextQueue();
	return true;
}

/*-------------------------------------------------------*/
void	SK1100::Update(void)
/*-------------------------------------------------------*/
{
//	if (++m_keyboardBufferLatency < 400)
	{
		RunNormal();
	}
	//else
	//{
	//	if (m_keyboardBufferLatency >= 800)
	//		m_keyboardBufferLatency = 0;

	//	if (m_keyboardBufferCount > 0)
	//		RunBuffer();
	//	else
	//		RunNormal();
	//}


}


void SK1100::PauseKeyPressed()
{
	m_interruptLines->PulseNonMaskableInterrupt();
}

#if GEARSF7000_ENABLE_SDL_INPUT
void SK1100::SetEvent(SDL_Event event)
{
const t_sk1100_key* keyMap = m_keyb_enabled ? SK1100_KeyboardMode_Mapping : SK1100_JoystickMode_Mapping;
int count = m_keyb_enabled ? SK1100_NUM_KEYS : SK1100_JOY_NUM;

	for (int i = 0; i < count; i++)
	{
		const t_sk1100_key* p = &keyMap[i];

		//if (event.type == SDL_KEYDOWN)
		//{
		//}

		if (p->pc_keycode == event.key.scancode)
		{
			printf("MATCH %s sc=%04X row=%d mask=%04X type=%s\n",
				p->def_string,
				(unsigned)event.key.scancode,
				p->row,
				p->mask,
				(event.type == SDL_EVENT_KEY_DOWN) ? "DOWN" : "UP");


			if (event.type == SDL_EVENT_KEY_DOWN)
			{
				m_keyb_rows[p->row] &= ~(p->mask);
			//	printf("PC KeyCode %04X, SCANCODE: %04X - KEYDOWN\n", p->pc_keycode, event.key.keysym.scancode);

			}
			else if (event.type == SDL_EVENT_KEY_UP)
			{
				m_keyb_rows[p->row] |= p->mask;
			//printf("PC KeyCode %04X, SCANCODE: %04X - KEYUP\n", p->pc_keycode, event.key.keysym.scancode);
			}

		}


		//if (p->pc_keycode == event.key.keysym.sym)
		//{
		//	if (event.type == SDL_KEYDOWN)
		//	{
		//		m_keyb_rows[p->row] &= ~(p->mask);
		//	}
		//	else if (event.type == SDL_KEYUP)
		//	{
		//		m_keyb_rows[p->row] |= p->mask;
		//	}

		//}
	}


}
#endif




// Not SetBit/UnsetBit here: they take and return u8, and m_keyb_rows[7] is
// uint16_t. Passing it in truncates to the low byte before any bit is
// touched, and assigning the u8 result back zero-extends, clobbering the
// entire high byte on every single press or release - not just an out-of-
// range bit read, an unconditional corruption of whichever player's state
// happened to live above bit 7. Player 2's bits 8-11 could never be set at
// all: shifting into them happens in int, but the return value is truncated
// back to u8 before it reaches the register. The mask has to be built and
// applied at the register's own width.
void SK1100::JoystickPressed(GC_Controllers controller, GC_Keys key)
{
	int offset = controller == Controller_1 ? 0 : 6;
	int bit = -1;

	switch (key)
	{
		case Key_Up:           bit = 0 + offset; break;
		case Key_Down:         bit = 1 + offset; break;
		case Key_Left:         bit = 2 + offset; break;
		case Key_Right:        bit = 3 + offset; break;
		case Key_Left_Button:  bit = 4 + offset; break;
		case Key_Right_Button: bit = 5 + offset; break;
		default: break;
	}

	if (bit >= 0)
	{
		const uint16_t mask = uint16_t(1u) << bit;
		m_keyb_rows[7] &= static_cast<uint16_t>(~mask);
	}
}

void SK1100::JoystickReleased(GC_Controllers controller, GC_Keys key)
{
	int offset = controller == Controller_1 ? 0 : 6;
	int bit = -1;

	switch (key)
	{
		case Key_Up:           bit = 0 + offset; break;
		case Key_Down:         bit = 1 + offset; break;
		case Key_Left:         bit = 2 + offset; break;
		case Key_Right:        bit = 3 + offset; break;
		case Key_Left_Button:  bit = 4 + offset; break;
		case Key_Right_Button: bit = 5 + offset; break;
		default: break;
	}

	if (bit >= 0)
	{
		const uint16_t mask = uint16_t(1u) << bit;
		m_keyb_rows[7] |= mask;
	}

}

/*-------------------------------------------------------*/
void	SK1100::RunNormal(void)
/*-------------------------------------------------------*/
{
}


void SK1100::SaveState(std::ostream& stream)
{
	StateWriter w(stream);

	w.U16(kSK1100StateVersion);

	// The eight matrix rows are what the PPI actually reads back, so they
	// are the part a rewind has to restore: without them a key held across
	// the snapshot comes back released.
	w.Bool(m_keyb_enabled);
	w.I32(m_keyb_row);
	w.I32(m_keyb_val);
	for (int row = 0; row < 8; ++row)
		w.U16(m_keyb_rows[row]);
	w.U16(m_keyb_systemkeys);
	w.U64(m_iInputCycles);

	// Synthetic typing in flight. This is host-side injection rather than
	// machine state, but it is saved anyway: replaying a recording has to
	// type the same characters at the same moments, otherwise a rewind past
	// a queued listing produces a different program.
	w.U32(static_cast<std::uint32_t>(m_text_queue.size()));
	for (const QueuedKey& key : m_text_queue)
	{
		w.I32(key.row);
		w.I32(key.mask);
		w.Bool(key.pressed);
		w.I32(key.ticks);
	}
	w.U32(static_cast<std::uint32_t>(m_text_queue_index));
	w.I32(m_text_ticks_left);
	w.I32(m_text_press_polls);
	w.I32(m_text_release_polls);
	w.I32(m_text_newline_polls);
}

void SK1100::LoadState(std::istream& stream)
{
	StateReader r(stream);

	if (r.U16() != kSK1100StateVersion)
		return;

	const bool enabled = r.Bool();
	const int row = r.I32();
	const int val = r.I32();
	uint16_t rows[8];
	for (int i = 0; i < 8; ++i)
		rows[i] = r.U16();
	const uint16_t systemKeys = r.U16();
	const uint64_t inputCycles = r.U64();

	const std::uint32_t queueSize = r.U32();
	if (!r.Ok() || queueSize > 1000000u)
		return;

	std::vector<QueuedKey> queue;
	queue.reserve(queueSize);
	for (std::uint32_t i = 0; i < queueSize; ++i)
	{
		QueuedKey key{};
		key.row = r.I32();
		key.mask = r.I32();
		key.pressed = r.Bool();
		key.ticks = r.I32();
		queue.push_back(key);
	}

	const std::uint32_t queueIndex = r.U32();
	const int ticksLeft = r.I32();
	const int pressPolls = r.I32();
	const int releasePolls = r.I32();
	const int newlinePolls = r.I32();

	if (!r.Ok())
		return;

	m_keyb_enabled = enabled;
	m_keyb_row = row;
	m_keyb_val = val;
	for (int i = 0; i < 8; ++i)
		m_keyb_rows[i] = rows[i];
	m_keyb_systemkeys = systemKeys;
	m_iInputCycles = inputCycles;

	m_text_queue = queue;
	m_text_queue_index = queueIndex;
	m_text_ticks_left = ticksLeft;
	m_text_press_polls = pressPolls;
	m_text_release_polls = releasePolls;
	m_text_newline_polls = newlinePolls;
}
