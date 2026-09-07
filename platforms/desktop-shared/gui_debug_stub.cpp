/*
 * GearSC3000 - no-debug desktop compatibility shim
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The regular desktop shell historically calls a few debugger lifecycle and
 * shortcut hooks unconditionally. A computer-only product has no debugger;
 * these no-op definitions keep that shell reusable without linking any of the
 * debugger implementation.
 */

#include "../../src/definitions.h"
#include "gui_debug.h"
#include "gui_debug_rewind.h"

void gui_debug_windows() {}
void gui_debug_reset() {}
void gui_debug_reset_symbols() {}
void gui_debug_load_symbols_file(const char*) {}
int gui_debug_symbol_count() { return 0; }
bool gui_debug_get_symbol(int, int*, u16*, const char**) { return false; }
void gui_debug_set_symbol(int, u16, const char*) {}
bool gui_debug_remove_symbol(int, u16) { return false; }
int gui_debug_load_symbols_file_counted(const char*) { return -1; }
void gui_debug_toggle_breakpoint() {}
void gui_debug_reset_breakpoints_cpu() {}
void gui_debug_reset_breakpoints_mem() {}
void gui_debug_reset_breakpoints_vram() {}
void gui_debug_runtocursor() {}
void gui_debug_go_back() {}
void gui_debug_copy_memory() {}
void gui_debug_paste_memory() {}

void gui_debug_window_rewind() {}
bool gui_debug_rewind_step_back() { return false; }
bool gui_debug_rewind_step_forward() { return false; }
bool gui_debug_rewind_resume_from_here() { return false; }
void gui_debug_rewind_reset_data() {}
