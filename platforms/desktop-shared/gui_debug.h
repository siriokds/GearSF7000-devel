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

#ifndef GUI_DEBUG_H
#define	GUI_DEBUG_H

#ifdef GUI_DEBUG_IMPORT
    #define EXTERN
#else
    #define EXTERN extern
#endif

#define BYTE_TO_BINARY_PATTERN "%c%c%c%c%c%c%c%c"
#define BYTE_TO_BINARY_PATTERN_SPACED "%c%c%c%c %c%c%c%c"
#define BYTE_TO_BINARY_PATTERN_ALL_SPACED "%c %c %c %c %c %c %c %c"
#define BYTE_TO_BINARY(byte)  \
  (byte & 0x80 ? '1' : '0'), \
  (byte & 0x40 ? '1' : '0'), \
  (byte & 0x20 ? '1' : '0'), \
  (byte & 0x10 ? '1' : '0'), \
  (byte & 0x08 ? '1' : '0'), \
  (byte & 0x04 ? '1' : '0'), \
  (byte & 0x02 ? '1' : '0'), \
  (byte & 0x01 ? '1' : '0') 

EXTERN void gui_debug_windows(void);
EXTERN void gui_debug_reset(void);
EXTERN void gui_debug_reset_symbols(void);
EXTERN void gui_debug_load_symbols_file(const char* path);

// The disassembler's symbol table, reached from outside so the MCP tools can
// share it rather than keep a second one. A symbol added either way shows up
// in both, which is the whole point: two tables would disagree the moment
// anyone used the tool that did not own the one being read.
//
// Symbols are keyed by (bank, address), not address alone. On a banked
// cartridge $8000-$BFFF is different code depending on the mapper's current
// bank, so a name attached to the address alone would not merely be missing
// from other banks - it would be wrong in them, printed confidently over
// unrelated code.
//
// These are host data, not machine state: they do not belong in a save state.
EXTERN int gui_debug_symbol_count(void);
// Fills the outputs for 0 <= index < gui_debug_symbol_count(). The text
// pointer stays valid until the table is next modified.
EXTERN bool gui_debug_get_symbol(int index, int* bank, u16* address,
                                 const char** text);
// Replaces any symbol already at that (bank, address) - one name per slot.
EXTERN void gui_debug_set_symbol(int bank, u16 address, const char* text);
// False when there was nothing there to remove.
EXTERN bool gui_debug_remove_symbol(int bank, u16 address);
// Appends, as the .sym format expects. Returns how many were added, or -1 if
// the file could not be opened.
EXTERN int gui_debug_load_symbols_file_counted(const char* path);
EXTERN void gui_debug_toggle_breakpoint(void);
EXTERN void gui_debug_reset_breakpoints_cpu(void);
EXTERN void gui_debug_reset_breakpoints_mem(void);
EXTERN void gui_debug_reset_breakpoints_vram(void);
EXTERN void gui_debug_runtocursor(void);
EXTERN void gui_debug_go_back(void);
EXTERN void gui_debug_copy_memory(void);
EXTERN void gui_debug_paste_memory(void);

#undef GUI_DEBUG_IMPORT
#undef EXTERN
#endif	/* GUI_DEBUG_H */