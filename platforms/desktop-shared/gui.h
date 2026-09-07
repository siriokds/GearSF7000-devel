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

#ifndef GUI_H
#define	GUI_H

#include "imgui/imgui.h"

#ifdef GUI_IMPORT
    #define EXTERN
#else
    #define EXTERN extern
#endif

#define BYTE_TO_BINARY_PATTERN "%c%c%c%c%c%c%c%c"
#define BYTE_TO_BINARY_PATTERN_SPACED "%c%c%c%c %c%c%c%c"
#define BYTE_TO_BINARY(byte)  \
  (byte & 0x80 ? '1' : '0'), \
  (byte & 0x40 ? '1' : '0'), \
  (byte & 0x20 ? '1' : '0'), \
  (byte & 0x10 ? '1' : '0'), \
  (byte & 0x08 ? '1' : '0'), \
  (byte & 0x04 ? '1' : '0'), \
  (byte & 0x02 ? '1' : '0'), \
  (byte & 0x01 ? '1' : '0') 

enum gui_ShortCutEvent
{
    gui_ShortcutOpenROM = 0,
    gui_ShortcutReloadROM,
    gui_ShortcutReset,
    gui_ShortcutPause,
    gui_ShortcutFFWD,
    gui_ShortcutSaveState,
    gui_ShortcutLoadState,
    gui_ShortcutScreenshot,
    gui_ShortcutFullscreen,
    gui_ShortcutDebugStepInto,
    gui_ShortcutDebugStepOver,
    gui_ShortcutDebugStepLine,
    gui_ShortcutDebugBreak,
    gui_ShortcutDebugContinue,
    gui_ShortcutDebugContinueFromHere,
    gui_ShortcutDebugNextFrame,
    gui_ShortcutDebugPreviousFrame,
    gui_ShortcutDebugBreakpoint,
    gui_ShortcutDebugRuntocursor,
    gui_ShortcutDebugGoBack,
    gui_ShortcutDebugCopy,
    gui_ShortcutDebugPaste,
    gui_ShortcutShowMainMenu,
    gui_ShortcutRunSF7000
};

EXTERN bool gui_in_use;
EXTERN bool gui_main_window_hovered;
EXTERN bool gui_main_menu_hovered;
EXTERN ImFont* gui_roboto_font;
EXTERN ImFont* gui_sc3000_font;
EXTERN ImFont* gui_unifont_font;
EXTERN ImFont* gui_default_font;
EXTERN ImFont* gui_memory_condensed_font;
EXTERN ImFont* gui_memory_condensed_light_font;
EXTERN ImFont* gui_material_icons_font;

EXTERN void gui_init(void);
EXTERN void gui_destroy(void);
EXTERN bool gui_render(void);
EXTERN void gui_shortcut(gui_ShortCutEvent event);
EXTERN void gui_load_rom(const char* path);
EXTERN void gui_set_status_message(const char* message, u32 milliseconds);
// Returns true when an SDL event belongs to the active input-learning modal
// and must not reach the emulated machine or frontend shortcuts.
EXTERN bool gui_input_capture_event(const SDL_Event* event);
// The on-screen SC-3000 keyboard is a second input source. The SDL keyboard
// path uses this query to avoid releasing a matrix contact still held by the
// mouse, and focus/quit handling uses the release helper to prevent stuck keys.
EXTERN bool gui_sk1100_virtual_matrix_down(int row, int mask);
EXTERN void gui_sk1100_release_virtual_keys(void);

EXTERN ImFont* gui_get_font(int index);
EXTERN float gui_get_default_font_size(void);
// Combo widths follow the active font and global content scale. The helper
// includes the arrow button, frame padding and a small readability margin.
EXTERN float gui_combo_width_for_text(const char* longest_item);
EXTERN float gui_combo_width_for_items(const char* items_separated_by_zeros);
EXTERN float gui_combo_width_for_array(const char* const items[], int item_count);

// Raw binary export/import between an arbitrary in-memory buffer and a file
// on disk. Offsets/lengths are relative to memoryPtr itself (buffer start),
// not a CPU address - callers translate CPU address -> buffer offset first
// (e.g. against a GuiDebugMemoryView's base_address, see gui_debug_memory.h).
EXTERN void MemoryExport(const char* dstFilename, u8* memoryPtr, u32 srcOffset, int length);
EXTERN int MemoryImport(const char* fname, u8* memoryPtr, u32 dstOffset, int maxLength);


#undef GUI_IMPORT
#undef EXTERN
#endif	/* GUI_H */
