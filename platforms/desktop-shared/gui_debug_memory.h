/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026 Saverio Russo
 *
 * Debug-memory controller shared by the desktop debugger.  It intentionally
 * owns editor state independently from the surrounding ImGui windows so the
 * SC-3000/SF-7000 memory views can later move out of gui_debug.cpp without
 * changing clipboard, selection or navigation behaviour.
 */

#ifndef GUI_DEBUG_MEMORY_H
#define GUI_DEBUG_MEMORY_H

#include <cstdint>

struct ImFont;

enum GuiDebugMemoryEditorSlot
{
    GuiDebugMemoryEditorSlotPrimary = 0,
    GuiDebugMemoryEditorSlotSecondary,
    GuiDebugMemoryEditorSlotTertiary,
    GuiDebugMemoryEditorSlotReserved,
    GuiDebugMemoryEditorSlotVram,
    GuiDebugMemoryEditorSlotCount
};

void gui_debug_memory_window(void);
// Top-level windows owned by the memory editors; must be pumped every frame
// alongside gui_debug_memory_window(), not from inside it.
void gui_debug_memory_search_window(void);
void gui_debug_memory_find_bytes_window(void);
void gui_debug_memory_watches_window(void);
void gui_debug_memory_draw_view(int slot, const char* title, std::uint8_t* data, int size,
                                int base_address, ImFont* font,
                                bool request_select);
bool gui_debug_memory_should_select(int slot);
void gui_debug_memory_goto(int slot, int address);
void gui_debug_memory_copy(void);
void gui_debug_memory_paste(void);

// One physical buffer backing a CPU address range, exactly as shown in the
// Memory Editor's own tabs (same title/size/base_address) - the single
// source of truth for "what RAM/ROM actually exists right now", since that
// depends on the active machine profile (SF-7000 vs SC-3000 cartridge type)
// and isn't one flat 64 KiB buffer. Consumers outside gui_debug_memory.cpp
// (Mem Import/Export, future MCP tools) should read this instead of
// re-deriving the profile dispatch themselves.
struct GuiDebugMemoryView
{
    const char* title;
    std::uint8_t* data;
    int size;
    int base_address;
};

// Fills out_views (capacity max_views) with the RAM/ROM views for the
// currently active machine profile, VRAM excluded (callers that want VRAM
// use Video::GetVRAM() directly - it's profile-independent, always 16 KiB
// at base 0). Returns how many views were written.
int gui_debug_memory_get_current_views(GuiDebugMemoryView* out_views, int max_views);

#endif /* GUI_DEBUG_MEMORY_H */
