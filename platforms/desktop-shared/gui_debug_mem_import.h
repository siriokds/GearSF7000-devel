/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026 Saverio Russo
 *
 * "Mem Import": raw binary import/export between disk files and the
 * currently active RAM/ROM/VRAM buffers. See DOCS/ROM_INSPECTOR_PLAN.md
 * point 1 for why this window exists and how it's meant to be used (a fast
 * edit-on-disk-then-reload loop, not a one-shot file browse).
 */

#ifndef GUI_DEBUG_MEM_IMPORT_H
#define GUI_DEBUG_MEM_IMPORT_H

void gui_debug_mem_import_window(void);

#endif /* GUI_DEBUG_MEM_IMPORT_H */
