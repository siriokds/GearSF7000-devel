/*
 * GearSF7000 - the new Disassembler window, built up next to the old one
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GUI_DEBUG_DISASSEMBLER_VIEW_H
#define GUI_DEBUG_DISASSEMBLER_VIEW_H

#include "imgui/imgui.h"

void gui_debug_disassembler_view_window(void);
// The project's files changed (Deep Search wrote them): read them again.
void gui_debug_disassembler_view_reload(void);
// To Check: every stretch not sure yet (UNKNOWN or CODE? not kept), a list
// to go through, with the reference disassembly's opinion when there is one.
void gui_debug_to_check_window(void);
void gui_debug_to_check_open(void);
// Shows the window at an address, as a jump there (Back returns).
void gui_debug_disassembler_view_goto(unsigned short address);

// The old window's instruction painter ({n}/{o}/{e}/{s} markers), shared so
// both windows colour an instruction the same way.
void gui_debug_draw_instruction(const char* instruction, const ImVec4& fallback,
                                bool highlight);

#endif
