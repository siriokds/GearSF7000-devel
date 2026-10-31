/*
 * GearSF7000 - automatic per-ROM project holding the ROM map
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef ROM_MAP_PROJECT_H
#define ROM_MAP_PROJECT_H

#include <string>

// The ROM map (src/RomMap.h) lives in memory; this keeps it on disk, in an
// automatic project per ROM under <config>/projects/<CRC32> <name>/, so play
// sessions add up across runs. Decided with Saverio on 23/09/2026 - see
// DOCS/PROGRAM_ANALYSIS_AUDIT.md sections 7 and 12.
//
// One switch: enabled means accumulating. Disabling saves and frees the map.

void rom_map_project_init(void);
// Read-only: projects are read (the map, jump tables) but nothing is written
// to the projects folder. The headless instance runs this way, so tests run
// beside the GUI never touch the user's projects.
void rom_map_project_set_read_only(bool read_only);
bool rom_map_project_is_read_only(void);
void rom_map_project_shutdown(void);
// Call after a ROM was loaded, ejected, or the SF-7000 took the slot.
void rom_map_project_on_media_changed(void);
// Called once per GUI/headless loop turn; saves now and then while playing.
void rom_map_project_tick(void);

void rom_map_project_set_enabled(bool enabled);
bool rom_map_project_is_enabled(void);
// Back to 100% UNKNOWN, as a ROM never seen, and writes that to disk.
void rom_map_project_reset(void);
bool rom_map_project_save(void);
// Wall-clock time of the last successful save, 0 if none since start-up;
// and whether the map has changed since then.
long long rom_map_project_last_save_time(void);
bool rom_map_project_is_dirty(void);

// Where the projects live: the folder chosen in Debug > Analysis, which is
// never empty (config_default_projects_dir() until one is chosen). Ends with
// a separator. One folder per ROM hangs off it: <root><CRC> <name>/.
std::string rom_map_project_root(void);
bool rom_map_project_root_is_default(void);
// Switches to another projects folder ("" = the default). The map collected so
// far is saved where it was; then the ROM in memory picks up its project in
// the new folder if there is one, or takes its map there if there is not.
// False, with rom_map_project_last_error(), when the folder cannot be written.
bool rom_map_project_set_root(const std::string& path);
// The loaded ROM belongs to a project. The Live Disassembler works only inside
// one; a ROM with none has nothing collected until one is made on purpose.
bool rom_map_project_active(void);
// Makes a project for the loaded ROM in the projects folder: <root>/<name>/
// with reference/ (a copy of the ROM), rommap/ and cache/. An empty name is the
// ROM's own. False, with rom_map_project_last_error(), when it cannot.
bool rom_map_project_create(const std::string& name);
// Read-only preflight for opening a project. Resolves its recorded ROM in
// reference/ and verifies identity before any running session is changed.
bool rom_map_project_resolve_rom(const std::string& directory, std::string& rom);
void rom_map_project_open_folder(const std::string& dir);

class RomMap;
// Reads the project's jumptables.txt and deduced.txt into a map, for its
// static rules (Deep Search's findings, redone at every save).
void rom_map_project_apply_jump_tables(RomMap* map);

// Before the project's map is written over: a copy in rommap/backups/, at
// most one every ten minutes, the last ten kept.
void rom_map_project_backup(const std::string& dir);

// Empty when no ROM is attached.
std::string rom_map_project_dir(void);
const char* rom_map_project_last_error(void);

#endif
