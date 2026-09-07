/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026 Saverio Russo
 *
 * Disassembly-to-text export. Two modes: "visible" dumps the disassembler's
 * current live view (whatever GetDisassembleRecord() resolves right now,
 * address by address - respects the active mapper bank/IPL overlay, same
 * as the Disassembler window itself), "full" dumps every physical
 * disassembly map unconditionally (BIOS+RAM+SGM+ROM), independent of
 * which bank/mode happens to be active. Both are gapless, SC3K-System
 * style: addresses/offsets with no known instruction are emitted as `db`
 * byte directives instead of being skipped, so the dump covers the whole
 * range start to end. See DOCS/ROM_INSPECTOR_PLAN.md point 3.
 *
 * Deliberately has no ImGui/NFD dependency: the GUI menu (gui_debug.cpp)
 * and the MCP tool (mcp_debug_adapter.cpp) both call straight into this,
 * each supplying its own file path.
 */

#ifndef GUI_DEBUG_DISASSEMBLER_EXPORT_H
#define GUI_DEBUG_DISASSEMBLER_EXPORT_H

bool gui_debug_save_disassembly(const char* file_path, bool full);

#endif /* GUI_DEBUG_DISASSEMBLER_EXPORT_H */
