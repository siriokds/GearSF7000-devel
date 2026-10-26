/*
 * GearSF7000 - ROM Map window
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef GUI_DEBUG_ROM_MAP_H
#define GUI_DEBUG_ROM_MAP_H

#include "imgui/imgui.h"

void gui_debug_rom_map_window(void);
// The colour of each RomMap kind.
ImVec4 gui_debug_rom_map_kind_color(unsigned char kind);

#endif
