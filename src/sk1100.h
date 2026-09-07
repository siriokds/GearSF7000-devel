// SEGA SC-3000 Keyboard Routines

#ifndef _SK1100_H_
#define _SK1100_H_

#include <stdint.h>
#include <string>
#include <vector>
#include "definitions.h"
#if GEARSF7000_ENABLE_SDL_INPUT
#include <SDL3/SDL.h>
#endif

#include "cpu/CpuInterruptLines.h"
//#include "Input.h"


#define	SK1100_JOY_UP		0x01
#define	SK1100_JOY_DOWN		0x02
#define	SK1100_JOY_LEFT		0x04
#define	SK1100_JOY_RIGHT	0x08
#define	SK1100_JOY_BUTTON1	0x10
#define	SK1100_JOY_BUTTON2	0x20


class	SK1100
{
private:
	uint64_t	m_iInputCycles;
	bool		m_keyb_enabled;
	int			m_keyb_row;
	int			m_keyb_val;
	uint16_t	m_keyb_rows[8];
	uint16_t	m_keyb_systemkeys;
	struct QueuedKey { int row; int mask; bool pressed; int ticks; };
	std::vector<QueuedKey> m_text_queue;
	size_t		m_text_queue_index;
	int			m_text_ticks_left;
	// Matrix polls per synthetic keystroke, and the idle hold after Return.
	// The defaults are the values measured to work on a full listing typed
	// from a settled prompt; SetTextTiming lets the user trade margin for
	// speed, in either direction.
	int			m_text_press_polls;
	int			m_text_release_polls;
	int			m_text_newline_polls;
	void		RunNormal(void);
	void		SetMatrixKey(int row, int mask, bool pressed);
	void		AdvanceTextQueue(void);

	CpuInterruptLines* m_interruptLines;
public:
	SK1100(CpuInterruptLines* interruptLines);
	void	Init();
	void	Reset();
	void	SetKeyboardMode(bool keyboardModeEnabled);

	int		GetRow(void);

	void	PauseKeyPressed();
	bool	QueueText(const std::string& text, std::string* error = 0);
	// How hard the injector leans on the machine. press/release are polls per
	// keystroke; newline is the idle hold after Return, which exists because
	// BASIC does not scan the matrix while it tokenises a line. Applies to the
	// next QueueText, not to one already running.
	void	SetTextTiming(int pressPolls, int releasePolls, int newlinePolls);
	int		GetTextPressPolls() const { return m_text_press_polls; }
	int		GetTextReleasePolls() const { return m_text_release_polls; }
	int		GetTextNewlinePolls() const { return m_text_newline_polls; }
	// Whether QueueText can produce this character at all. QueueText rejects a
	// whole string on the first character it cannot type, so a caller that
	// accepts arbitrary pasted text should filter with this first.
	static bool CanTypeChar(char character);
	void	ClearTextQueue();
#if GEARSF7000_ENABLE_SDL_INPUT
	void	SetEvent(SDL_Event event);
#endif
	// Direct press/release of a single non-printable keyboard matrix key by
	// its SK1100_KeyboardMode_Mapping label (e.g. "Up", "Down", "Left",
	// "Right") - the same matrix cursor keys, not the joystick port. Text
	// characters go through QueueText instead. Returns false if the label
	// isn't in the mapping table.
	bool	PressMatrixKeyByLabel(const std::string& label, bool pressed);
	// Runtime host bindings use the physical matrix coordinate directly. This
	// keeps SDL policy in the desktop frontend while the core owns electrical
	// active-low matrix state.
	void	SetMatrixKeyState(int row, int mask, bool pressed);

	void	JoystickPressed(GC_Controllers controller, GC_Keys key);
	void	JoystickReleased(GC_Controllers controller, GC_Keys key);

	void	GetRows(uint16_t* rows);

	void	SetPortC(int val);
	uint8_t	GetPortC(void);
	uint8_t	GetPortA(void);
	uint8_t	GetPortB(void);

	int		GetSystemKey(int n);

	void	Tick(unsigned int clockCycles);
	bool	HasPendingTimedEvents() const
	{
		return m_text_queue_index < m_text_queue.size();
	}
	// Progress of the queued text, in matrix events rather than characters:
	// a shifted character costs four events and a plain one two, so the ratio
	// is what a caller should show, not the counts themselves.
	size_t	GetTextQueueSize() const { return m_text_queue.size(); }
	size_t	GetTextQueueIndex() const { return m_text_queue_index; }
	void	Update(void);

	void	LoadState(std::istream& stream);
	void	SaveState(std::ostream& stream);
};


#endif /* _SK1100_H_ */
