/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026 Saverio Russo
 *
 * ROM Inspector: point 4 of DOCS/ROM_INSPECTOR_PLAN.md. Loads an arbitrary
 * external file (not the live cartridge, no bank-switching/mapper applied)
 * into a scratch buffer and renders it as TMS9918 8x8 1bpp tiles at a
 * user-chosen scroll position and tiles-per-row, with an optional color
 * table (Mode 1 = Graphics I addressing, one byte per 8 tiles; Mode 2 =
 * Graphics II addressing, one byte per pattern row).
 *
 * The state-management functions below (load/set/get/render) work whether
 * or not the ImGui window is currently open or even ever drawn - the MCP
 * tools (mcp_debug_adapter.cpp) drive the same state to let a remote
 * caller "see" what's loaded without a human at the keyboard, the same way
 * get_screenshot doesn't need any particular window open either.
 */

#ifndef GUI_DEBUG_ROM_INSPECTOR_H
#define GUI_DEBUG_ROM_INSPECTOR_H

enum GuiDebugRomInspectorColorTableMode
{
    GuiDebugRomInspectorColorTableNone = 0,
    GuiDebugRomInspectorColorTableMode1 = 1, // Graphics I: one color byte per 8 consecutive tiles
    GuiDebugRomInspectorColorTableMode2 = 2, // Graphics II: one color byte per pattern row
};

struct GuiDebugRomInspectorStatus
{
    const char* file_path;
    int file_size;
    int tile_offset;
    int tiles_per_row;
    int colortable_mode;
    int colortable_offset;
    int scroll_row;
    int total_rows;
};

void gui_debug_rom_inspector_window(void);

bool gui_debug_rom_inspector_load_file(const char* file_path);
void gui_debug_rom_inspector_set_tile_offset(int value);
void gui_debug_rom_inspector_set_tiles_per_row(int value);
void gui_debug_rom_inspector_set_colortable(int mode, int offset);
void gui_debug_rom_inspector_set_scroll_row(int top_row);
GuiDebugRomInspectorStatus gui_debug_rom_inspector_get_status(void);
// Decodes the currently visible window into emu_debug_rom_inspector_buffer
// (top-left corner of the buffer, row-major RGB888, row stride
// ROM_INSPECTOR_TEXTURE_SIZE*3) and reports its real pixel size - callable
// without the ImGui window ever having been open.
void gui_debug_rom_inspector_render_visible(int* out_width, int* out_height);

#endif /* GUI_DEBUG_ROM_INSPECTOR_H */
