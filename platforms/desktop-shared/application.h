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

#ifndef APPLICATION_H
#define	APPLICATION_H

#include <SDL3/SDL.h>

#ifdef APPLICATION_IMPORT
    #define EXTERN
#else
    #define EXTERN extern
#endif

EXTERN SDL_Gamepad* application_gamepad[2];
EXTERN int application_gamepad_mappings;
EXTERN float application_display_scale;
EXTERN int application_sdl_version;

EXTERN int application_init(const char* rom_file, const char* symbol_file);
EXTERN void application_destroy(void);
EXTERN void application_mainloop(void);
EXTERN void application_trigger_quit(void);
EXTERN void application_trigger_fullscreen(bool fullscreen);
// Refresh rate of the display the window is on, in Hz, or 0 if it cannot be
// determined. Needed to say whether vertical sync is pacing the machine at
// something other than its own frame rate.
EXTERN double application_get_display_refresh_hz(void);
EXTERN void application_trigger_fit_to_content(int width, int height);
// Shared desktop command used by GUI/menu/shortcuts and MCP commands. It
// deliberately preserves the historical gui_load_rom sequence.
EXTERN bool application_load_rom(const char* path);
// Shared media commands used by both the desktop GUI and MCP.  Mounting a
// disk never starts SF-7000 implicitly; start_sf7000 is the explicit boot
// command and loads the configured IPL when required.
EXTERN bool application_load_tape(const char* path);
EXTERN bool application_mount_disk(const char* path, bool write_protected);
// Mounts the disc and cassette named in the configuration, as the windowed
// front end has always done at startup. Shared so a headless session inserts
// the same media rather than starting with empty drives.
EXTERN void application_apply_startup_media(void);
EXTERN bool application_start_sf7000(void);

#undef APPLICATION_IMPORT
#undef EXTERN
#endif	/* APPLICATION_H */
