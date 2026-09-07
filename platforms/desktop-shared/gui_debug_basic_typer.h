/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026  Saverio Russo

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 *
 */

#ifndef GUI_DEBUG_BASIC_TYPER_H
#define GUI_DEBUG_BASIC_TYPER_H

// Types pasted text into the SC-3000 as if it came from the SK-1100, for
// short fragments: a command, a couple of BASIC lines, an answer to a prompt.
//
// Not the way to load a whole program. A .basic file is a cassette format the
// emulator already reads - SR1000::LoadTape converts .bas/.basic through
// SegaBasicBitTape::LoadBas - and LOAD takes seconds where typing the same
// 12 KB costs about seventeen minutes at the matrix timing the injector needs.
void gui_debug_basic_typer_window(void);

// Driving the panel from outside the GUI - MCP tools can load a fragment,
// clear it, or send it without the user having to touch the window.
void gui_debug_basic_typer_set_text(const char* text);
void gui_debug_basic_typer_clear(void);
const char* gui_debug_basic_typer_get_text(void);
// Filters the current text and queues it. False if there was nothing typable
// left, or if a sequence is already running - the panel shows why. source names
// the caller ("panel", "MCP") and is displayed while the text is being typed.
bool gui_debug_basic_typer_send(const char* source);

#endif /* GUI_DEBUG_BASIC_TYPER_H */
