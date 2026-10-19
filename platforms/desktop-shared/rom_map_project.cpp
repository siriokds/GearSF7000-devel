/*
 * GearSF7000 - automatic per-ROM project holding the ROM map
 * Copyright (C) 2026 Saverio Russo
 */

#include "../../src/DeepSearch.h"
#include "../../src/JumpTables.h"
#include "rom_map_project.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <SDL3/SDL.h>

#include "config.h"
#include "emu.h"
#include "../../src/RomMap.h"

static std::string last_error;

// The project the live map was attached to, and its ROM: where it is saved.
// Not the current cartridge's project: when another ROM is loaded, the
// outgoing map is saved after the cartridge has already changed, and it must
// go back to its own project, not over the incoming one (Orguss's map was
// overwritten by Champion Golf's this way, 27/09).
struct Session
{
    std::string dir;
    std::string file;
    std::string system;
    u32 crc = 0;
    u32 size = 0;
    // The project's map could not be read: nothing is saved over it.
    bool loadFailed = false;
};
static Session session;
static bool read_only = false;
static const char* kReadOnlyError = "projects are read-only in the headless instance";
static u64 saved_generation = 0;
static u64 last_save_ticks = 0;
static time_t last_save_time = 0;

// Autosave while playing, so a crash or a killed instance loses at most this
// much. Writing a 32 KB map is nothing; rewriting it every frame would be.
static const u64 kAutosaveIntervalMs = 30000;

static RomMap* rom_map(void)
{
    GearSF7000Core* core = emu_get_core();
    return core ? core->GetRomMap() : nullptr;
}

// A name that is safe as a folder name on every host, and readable.
static std::string safe_folder_name(std::string name)
{
    for (char& c : name)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
    while (!name.empty() && name.front() == ' ')
        name.erase(name.begin());
    if (name.size() > 64)
        name.resize(64);
    return name;
}

// The ROM file name without folder and extension.
static std::string rom_stem(const char* file_name)
{
    std::string name = file_name ? file_name : "";
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        name.erase(dot);
    return safe_folder_name(name);
}

static std::string with_separator(std::string path)
{
    if (!path.empty() && path.back() != '/' && path.back() != '\\')
        path += '/';
    return path;
}

std::string rom_map_project_root(void)
{
    return with_separator(config_debug.projects_dir.empty() ? config_default_projects_dir()
                                                            : config_debug.projects_dir);
}

bool rom_map_project_root_is_default(void)
{
    return rom_map_project_root() == with_separator(config_default_projects_dir());
}

static bool path_exists(const std::string& path)
{
    SDL_PathInfo info;
    return !path.empty() && SDL_GetPathInfo(path.c_str(), &info);
}

static bool copy_file(const std::string& from, const std::string& to);

// Copies of the map in rommap/backups/, taken before the file is written
// over, at most one every kBackupEveryS, the last kBackupsKept in rotation:
// a map ruined by a bug, or by a session gone wrong, is found there as it
// was before (Saverio, 27/09, after Orguss's map was overwritten).
static const int kBackupsKept = 10;
static const double kBackupEveryS = 10 * 60;

void rom_map_project_backup(const std::string& dir)
{
    if (read_only)
        return;
    namespace fs = std::filesystem;
    const std::string map = dir + "rommap/rommap.bin";
    if (dir.empty() || !path_exists(map))
        return;
    const std::string folder = dir + "rommap/backups/";
    std::error_code error;
    std::vector<std::string> names;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder, error))
    {
        const std::string name = entry.path().filename().string();
        if (name.rfind("rommap-", 0) == 0 && entry.path().extension() == ".bin")
            names.push_back(name);
    }
    std::sort(names.begin(), names.end());      // the names are their time
    const time_t now = time(nullptr);
    if (!names.empty())
    {
        struct tm when = {};
        if (sscanf(names.back().c_str(), "rommap-%d-%d-%d-%2d%2d%2d.bin", &when.tm_year, &when.tm_mon,
                   &when.tm_mday, &when.tm_hour, &when.tm_min, &when.tm_sec) == 6)
        {
            when.tm_year -= 1900;
            when.tm_mon -= 1;
            when.tm_isdst = -1;
            const double age = difftime(now, mktime(&when));
            if (age >= 0 && age < kBackupEveryS)
                return;
        }
    }
    if (!SDL_CreateDirectory(folder.c_str()))
        return;
    char stamp[48];
    strftime(stamp, sizeof(stamp), "rommap-%Y-%m-%d-%H%M%S.bin", localtime(&now));
    if (!copy_file(map, folder + stamp))
    {
        DiagInfo("media", "ROM map: cannot keep a copy in %s", folder.c_str());
        return;
    }
    names.push_back(stamp);
    std::sort(names.begin(), names.end());
    for (std::size_t i = 0; i + kBackupsKept < names.size(); i++)
        fs::remove(folder + names[i], error);
}

static bool copy_file(const std::string& from, const std::string& to)
{
    FILE* in = fopen(from.c_str(), "rb");
    if (!in)
        return false;
    FILE* out = fopen(to.c_str(), "wb");
    if (!out)
    {
        fclose(in);
        return false;
    }
    char buffer[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0)
        if (fwrite(buffer, 1, n, out) != n)
            ok = false;
    fclose(in);
    return (fclose(out) == 0) && ok;
}

// True when a project card says it belongs to a ROM with this CRC.
static bool card_has_crc(const std::string& card_path, u32 crc)
{
    FILE* file = fopen(card_path.c_str(), "r");
    if (!file)
        return false;
    char want[16];
    snprintf(want, sizeof(want), "%08X", crc);
    char line[512];
    bool found = false;
    while (!found && fgets(line, sizeof(line), file))
    {
        unsigned value = 0;
        char key[32];
        if (sscanf(line, "%31s %x", key, &value) == 2 && strcmp(key, "crc32") == 0)
            found = (value == crc);
    }
    fclose(file);
    return found;
}

struct ProjectSearch
{
    u32 crc;
    std::string found;
};

static SDL_EnumerationResult find_project_visit(void* userdata, const char* dirname, const char* name)
{
    ProjectSearch* search = static_cast<ProjectSearch*>(userdata);
    const std::string project = std::string(dirname) + name + "/";
    // The card is in rommap/; the first layout kept it loose in the folder.
    if (card_has_crc(project + "rommap/project.txt", search->crc) ||
        card_has_crc(project + "project.txt", search->crc))
    {
        search->found = project;
        return SDL_ENUM_SUCCESS;
    }
    return SDL_ENUM_CONTINUE;
}

// The project a ROM belongs to, by the first of two rules that applies
// (DOCS/PROJECT_FOLDER_LAYOUT.md):
//  1. The ROM sits in a folder called reference/: the project is the folder
//     that holds it. Everything stays relative, so a project moves between
//     machines as it is - Orguss/ is one.
//  2. A project in the projects folder already keeps the map of a ROM with
//     this CRC, whatever it is called and wherever the ROM was loaded from.
// Neither: the ROM has no project, and the Live Disassembler has nothing to
// work in until one is made (New Project).
static std::string resolve_project_dir(u32 crc, const std::string& rom_path)
{
    // 1
    const size_t last = rom_path.find_last_of("/\\");
    if (last != std::string::npos && last > 0)
    {
        const std::string parent = rom_path.substr(0, last);
        const size_t up = parent.find_last_of("/\\");
        if (up != std::string::npos)
        {
            std::string leaf = parent.substr(up + 1);
            for (char& c : leaf)
                c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (leaf == "reference")
                return parent.substr(0, up + 1);
        }
    }
    // 2
    const std::string root = rom_map_project_root();
    ProjectSearch search{crc, std::string()};
    SDL_EnumerateDirectory(root.c_str(), find_project_visit, &search);
    return search.found;        // empty: no project
}

// The cartridge ROM the machine runs, if it has one: the SF-7000 runs from its
// IPL and disks, so there is no cartridge ROM to map.
static bool current_rom(u32& crc, u32& size, std::string& path, std::string& file)
{
    GearSF7000Core* core = emu_get_core();
    if (!core)
        return false;
    Cartridge* cartridge = core->GetCartridge();
    if (!cartridge->IsReady() || cartridge->GetROMSize() == 0 || core->GetMemory()->IsSF7000Enabled())
        return false;
    crc = cartridge->GetCRC();
    size = static_cast<u32>(cartridge->GetROMSize());
    path = cartridge->GetFilePath();
    file = cartridge->GetFileName();
    return true;
}

// Found once per ROM and per projects folder, not at every frame: the panel
// asks for it all the time, and finding it looks into the folders. Empty when
// the ROM belongs to no project: nothing is created on its own, a project is
// made on purpose (rom_map_project_create).
static bool dir_cache_valid = false;

std::string rom_map_project_dir(void)
{
    u32 crc, size;
    std::string rom, file;
    if (!current_rom(crc, size, rom, file))
        return std::string();
    static u32 cached_crc = 0;
    static std::string cached_root, cached_rom, cached_dir;
    const std::string root = rom_map_project_root();
    if (!dir_cache_valid || cached_crc != crc || cached_root != root || cached_rom != rom)
    {
        cached_dir = resolve_project_dir(crc, rom);
        cached_crc = crc;
        cached_root = root;
        cached_rom = rom;
        dir_cache_valid = true;
    }
    return cached_dir;
}

bool rom_map_project_active(void)
{
    return !rom_map_project_dir().empty();
}

// Inside the project: rommap/ holds what the tool collects, cache/ what it
// generates from it (exported source, listing, symbols): safe to delete,
// rebuilt on demand. reference/, and whatever else the project has, is the
// user's and is never written to.
static std::string map_file_path(void)
{
    const std::string& dir = session.dir;
    return dir.empty() ? dir : dir + "rommap/rommap.bin";
}

// Deep Search's jump tables, next to the map: the map re-marks their bytes
// with its other static rules at every save.
static std::string jump_tables_file_path(void)
{
    const std::string dir = rom_map_project_dir();
    return dir.empty() ? dir : dir + "rommap/jumptables.txt";
}

void rom_map_project_apply_jump_tables(RomMap* map)
{
    if (!map)
        return;
    // And the code it deduced (rommap/deduced.txt).
    std::vector<RomMap::DeducedBlock> deduced;
    const std::string dir = rom_map_project_dir();
    if (!dir.empty())
        DeepSearch::LoadDeduced(dir + "rommap/deduced.txt", deduced);
    map->SetDeducedCode(deduced);
    // And what its data step made of the bytes nobody read.
    std::vector<RomMap::DataHint> hints;
    if (!dir.empty())
        DeepSearch::LoadDataHints(dir + "rommap/datahints.txt", hints);
    map->SetDataHints(hints);
    std::vector<JumpTables::Site> sites;
    std::vector<std::pair<u32, u32>> ranges;
    if (JumpTables::Load(jump_tables_file_path(), sites))
        for (const JumpTables::Site& site : sites)
            for (const JumpTables::Table& t : site.tables)
                if (!t.vector)      // a table of jp instructions is code, not data
                    ranges.push_back({t.base, t.entries * 2});
    map->SetJumpTables(ranges);
}

// Jump table entries seen in use, next to the map.
static std::string pointers_seen_file_path(void)
{
    const std::string& dir = session.dir;
    return dir.empty() ? dir : dir + "rommap/pointers_seen.txt";
}

static std::string jumps_seen_file_path(void)
{
    const std::string& dir = session.dir;
    return dir.empty() ? dir : dir + "rommap/jumps_seen.txt";
}

// What the cartridge uses of the board's firmware, next to the map.
static std::string firmware_file_path(void)
{
    const std::string& dir = session.dir;
    return dir.empty() ? dir : dir + "rommap/firmware.txt";
}

// The first layout kept the map loose in the project folder.
static std::string legacy_map_file_path(void)
{
    const std::string& dir = session.dir;
    return dir.empty() ? dir : dir + "rommap.bin";
}

// A plain-text card next to the map: which ROM, how big, what CRC, when last
// written - the session's ROM. No path: it would be true on one machine only.
static void write_project_card(const std::string& dir, const Session& rom)
{
    if (rom.file.empty())
        return;
    RomMap* map = rom_map();
    FILE* out = fopen((dir + "rommap/project.txt").c_str(), "w");
    if (!out)
        return;
    char when[32];
    const time_t now = time(nullptr);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(out, "GearSF7000 ROM map project\n");
    fprintf(out, "rom      %s\n", rom.file.c_str());
    fprintf(out, "size     %u\n", rom.size);
    fprintf(out, "crc32    %08X\n", rom.crc);
    fprintf(out, "system   %s\n", rom.system.c_str());
    fprintf(out, "sessions %u\n", (map && map->IsCollecting() && map->GetCrc() == rom.crc) ? map->GetSessions() : 0u);
    fprintf(out, "updated  %s\n", when);
    fclose(out);
}

// Generated files are not committed by default: they come from the map.
static void write_gitignore_if_missing(const std::string& dir)
{
    const std::string path = dir + ".gitignore";
    if (path_exists(path))
        return;
    FILE* file = fopen(path.c_str(), "w");
    if (!file)
        return;
    fprintf(file, "# Generated from rommap/: rebuilt on demand, not committed.\n");
    fprintf(file, "cache/\n");
    fprintf(file, "# Copies of the map kept in rotation by the app: git has its history already.\n");
    fprintf(file, "rommap/backups/\n");
    fclose(file);
}

bool rom_map_project_save(void)
{
    if (read_only)
    {
        last_error = kReadOnlyError;
        return false;
    }
    RomMap* map = rom_map();
    if (!map || !map->IsCollecting())
        return false;
    const std::string dir = session.dir;
    if (dir.empty() || map->GetCrc() != session.crc || map->GetRomSize() != session.size)
        return false;
    if (session.loadFailed)
        return false;       // last_error says why; the file on disk is kept
    // Static candidates follow what has been executed so far: refreshed at
    // every save, so at most one autosave interval behind the play. Only
    // while the cartridge is still the map's: when another ROM has just been
    // loaded, the outgoing map is saved as it is.
    Cartridge* cartridge = emu_get_core()->GetCartridge();
    if (cartridge->GetCRC() == session.crc && static_cast<u32>(cartridge->GetROMSize()) == session.size)
    {
        map->MarkFill(cartridge->GetROM(), static_cast<u32>(cartridge->GetROMSize()));
        map->MarkStaticTables(cartridge->GetROM(), static_cast<u32>(cartridge->GetROMSize()));
        map->MarkBranchCandidates(cartridge->GetROM(), static_cast<u32>(cartridge->GetROMSize()));
    }
    // Only what the tool owns is created; reference/ and the rest are the user's.
    for (const char* sub : {"", "rommap/", "cache/"})
    {
        const std::string path = dir + sub;
        if (!SDL_CreateDirectory(path.c_str()))
        {
            last_error = std::string("cannot create ") + path + ": " + SDL_GetError();
            Log("ROM map: %s", last_error.c_str());
            return false;
        }
    }
    const std::string path = map_file_path();
    rom_map_project_backup(dir);
    if (!map->Save(path))
    {
        last_error = "cannot write " + path;
        Log("ROM map: %s", last_error.c_str());
        return false;
    }
    if (map->HasFirmware() && !map->SaveFirmwareUsage(firmware_file_path()))
        Log("ROM map: cannot write %s", firmware_file_path().c_str());
    // Written even empty: after a reset an old file must not come back.
    if ((!map->GetObservedJumps().empty() || path_exists(jumps_seen_file_path())) &&
        !map->SaveObservedJumps(jumps_seen_file_path()))
        Log("ROM map: cannot write %s", jumps_seen_file_path().c_str());
    if ((!map->GetObservedPointers().empty() || path_exists(pointers_seen_file_path())) &&
        !map->SaveObservedPointers(pointers_seen_file_path()))
        Log("ROM map: cannot write %s", pointers_seen_file_path().c_str());
    // A map kept loose in the project folder by the first layout: read, kept, renamed.
    const std::string legacy = legacy_map_file_path();
    if (path_exists(legacy))
        SDL_RenamePath(legacy.c_str(), (legacy + ".migrated").c_str());
    write_project_card(dir, session);
    write_gitignore_if_missing(dir);
    saved_generation = map->GetGeneration();
    last_save_ticks = SDL_GetTicks();
    last_save_time = time(nullptr);
    last_error.clear();
    return true;
}

long long rom_map_project_last_save_time(void)
{
    return static_cast<long long>(last_save_time);
}

bool rom_map_project_is_dirty(void)
{
    RomMap* map = rom_map();
    return map && map->IsCollecting() && map->GetGeneration() != saved_generation;
}

// Picks up where the last session on this ROM left off, then counts this
// one. A missing file is simply a ROM never seen. Loaded as soon as the map
// is attached, even while collection is suspended (no debug, no MCP):
// otherwise, when it resumes, it would collect on an empty map and the
// autosave would write that over the project's.
static void start_session(void)
{
    RomMap* map = rom_map();
    if (!map || !map->IsEnabled() || !map->IsAttached() || session.dir.empty())
        return;
    std::string error;
    bool loaded = map->Load(map_file_path(), &error);
    if (!loaded && error == "no file")
        loaded = map->Load(legacy_map_file_path(), &error);
    if (!loaded && error != "no file")
    {
        // Never saved over: whatever is wrong with it, it is the project's.
        session.loadFailed = true;
        last_error = "project map not loaded (" + error + "): not collecting into it, the file is kept";
        DiagInfo("media", "ROM map: %s: %s", map_file_path().c_str(), last_error.c_str());
        return;
    }
    if (map->HasFirmware())
    {
        std::string why;
        if (!map->LoadFirmwareUsage(firmware_file_path(), &why) && why != "no file")
            Log("ROM map: firmware usage not loaded: %s", why.c_str());
    }
    {
        std::string why;
        if (!map->LoadObservedJumps(jumps_seen_file_path(), &why) && why != "no file")
            Log("ROM map: jump entries seen not loaded: %s", why.c_str());
        if (!map->LoadObservedPointers(pointers_seen_file_path(), &why) && why != "no file")
            Log("ROM map: pointers seen not loaded: %s", why.c_str());
    }
    rom_map_project_apply_jump_tables(map);
    // Whatever already ran since the ROM was loaded - the boot code, when
    // the map is switched on mid-game - is in the execution coverage. Only
    // real instruction boundaries count there, so it is the same evidence.
    GearSF7000Core* core = emu_get_core();
    for (const Memory::CodeCoverageRecord& record : core->GetMemory()->GetCodeCoverage())
        if (record.segment.compare(0, 3, "ROM") == 0 || record.segment.compare(0, 3, "MAP") == 0)
            map->MarkExecuted(record.physical_address, static_cast<u32>(record.size));
    // Padding at the end of the ROM is known without running anything.
    Cartridge* cartridge = core->GetCartridge();
    map->MarkFill(cartridge->GetROM(), static_cast<u32>(cartridge->GetROMSize()));
    map->SetSessions(map->GetSessions() + 1);
    rom_map_project_save();
}

// The map is attached only to a ROM that belongs to a project: the Live
// Disassembler works inside one, and what it collects is added to it.
static void attach_current_media(void)
{
    RomMap* map = rom_map();
    if (!map)
        return;
    u32 crc, size;
    std::string path, file;
    session = Session();
    if (current_rom(crc, size, path, file) && rom_map_project_active())
    {
        // Attach keeps the map when the ROM is the same: a reload of it goes
        // on in the same session.
        map->Attach(size, crc);
        session.dir = rom_map_project_dir();
        session.file = file;
        session.system = GetSystemModelName(emu_get_core()->GetSystemModel());
        session.crc = crc;
        session.size = size;
    }
    else
        map->Detach();
}

void rom_map_project_init(void)
{
    RomMap* map = rom_map();
    if (!map)
        return;
    map->SetEnabled(config_debug.rom_map_enabled);
}

void rom_map_project_set_read_only(bool value)
{
    read_only = value;
}

bool rom_map_project_is_read_only(void)
{
    return read_only;
}

void rom_map_project_shutdown(void)
{
    rom_map_project_save();
}

void rom_map_project_on_media_changed(void)
{
    RomMap* map = rom_map();
    if (!map)
        return;
    const u32 crc = map->GetCrc();
    const u32 size = map->GetRomSize();
    GearSF7000Core* core = emu_get_core();
    Cartridge* cartridge = core->GetCartridge();
    const bool same = cartridge->IsReady() && !core->GetMemory()->IsSF7000Enabled()
        && cartridge->GetCRC() == crc && static_cast<u32>(cartridge->GetROMSize()) == size;
    if (same)
        return; // reloading the same ROM keeps accumulating in the same session
    rom_map_project_save();
    attach_current_media();
    start_session();
}

void rom_map_project_tick(void)
{
    RomMap* map = rom_map();
    if (!map || !map->IsCollecting() || map->GetGeneration() == saved_generation)
        return;
    if (SDL_GetTicks() - last_save_ticks >= kAutosaveIntervalMs)
        rom_map_project_save();
}

void rom_map_project_set_enabled(bool enabled)
{
    RomMap* map = rom_map();
    config_debug.rom_map_enabled = enabled;
    if (!map || map->IsEnabled() == enabled)
        return;
    if (!enabled)
    {
        rom_map_project_save();
        map->SetEnabled(false);
        return;
    }
    attach_current_media();
    map->SetEnabled(true);
    start_session();
}

bool rom_map_project_is_enabled(void)
{
    RomMap* map = rom_map();
    return map && map->IsEnabled();
}

void rom_map_project_reset(void)
{
    RomMap* map = rom_map();
    if (!map || !map->IsCollecting())
        return;
    if (read_only)
    {
        last_error = kReadOnlyError;
        return;
    }
    // Nothing the project collected is thrown away: what is on disk now is
    // copied aside first, and the copy stays in rommap/.
    // A map that could not be read is copied aside the same way, and then
    // the project starts over from this one.
    if (rom_map_project_save() || session.loadFailed)
    {
        char stamp[32];
        const time_t now = time(nullptr);
        strftime(stamp, sizeof(stamp), "%Y-%m-%d-%H%M%S", localtime(&now));
        const std::string backup = session.dir + "rommap/rommap-backup-" + stamp + ".bin";
        if (!copy_file(map_file_path(), backup))
        {
            last_error = "reset cancelled: cannot keep a copy at " + backup;
            Log("ROM map: %s", last_error.c_str());
            return;
        }
        if (path_exists(firmware_file_path()))
            copy_file(firmware_file_path(), session.dir + "rommap/firmware-backup-" + stamp + ".txt");
        if (path_exists(jumps_seen_file_path()))
            copy_file(jumps_seen_file_path(), session.dir + "rommap/jumps_seen-backup-" + stamp + ".txt");
        if (path_exists(pointers_seen_file_path()))
            copy_file(pointers_seen_file_path(), session.dir + "rommap/pointers_seen-backup-" + stamp + ".txt");
        session.loadFailed = false;
    }
    map->Reset();
    GearSF7000Core* core = emu_get_core();
    Cartridge* cartridge = core->GetCartridge();
    map->MarkFill(cartridge->GetROM(), static_cast<u32>(cartridge->GetROMSize()));
    rom_map_project_save();
}

const char* rom_map_project_last_error(void)
{
    return last_error.c_str();
}

bool rom_map_project_set_root(const std::string& path)
{
    const std::string root = with_separator(path.empty() ? config_default_projects_dir() : path);
    if (root == rom_map_project_root())
        return true;
    if (read_only)
    {
        last_error = kReadOnlyError;
        return false;
    }
    if (rom_map_project_is_dirty() && !rom_map_project_save())
    {
        last_error = "cannot save the current project; projects folder unchanged";
        return false;
    }
    // Usable? Created if it is not there yet, and written to now rather than
    // at the first autosave, when nobody is looking at the menu any more.
    if (!SDL_CreateDirectory(root.c_str()))
    {
        last_error = std::string("cannot create ") + root + ": " + SDL_GetError();
        return false;
    }
    const std::string probe = root + ".gearsf7000-write-test-" + std::to_string(SDL_GetPerformanceCounter());
    FILE* file = fopen(probe.c_str(), "wx");
    if (!file)
    {
        last_error = "cannot write in " + root;
        return false;
    }
    fclose(file);
    remove(probe.c_str());

    // The outgoing dirty map was saved before changing the root.
    config_debug.projects_dir = root;
    dir_cache_valid = false;
    // The ROM in memory may belong to a project in the new folder, or to none
    // any more: attach again to whatever it is now.
    RomMap* map = rom_map();
    if (map && map->IsEnabled())
    {
        attach_current_media();
        start_session();
    }
    last_error.clear();
    return true;
}

bool rom_map_project_create(const std::string& requested_name)
{
    if (read_only)
    {
        last_error = kReadOnlyError;
        return false;
    }
    u32 crc, size;
    std::string rom_path, rom_file;
    if (!current_rom(crc, size, rom_path, rom_file))
    {
        last_error = "load a cartridge ROM first";
        return false;
    }
    if (rom_map_project_active())
    {
        last_error = "this ROM already belongs to a project";
        return false;
    }
    const std::string name = safe_folder_name(requested_name.empty() ? rom_stem(rom_file.c_str())
                                                                     : requested_name);
    if (name.empty())
    {
        last_error = "the project needs a name";
        return false;
    }
    const std::string dir = rom_map_project_root() + name + "/";
    if (path_exists(dir + "rommap/project.txt"))
    {
        last_error = "a project called \"" + name + "\" already exists here";
        return false;
    }
    // Only what the tool owns is created; a folder that already has content
    // (a project the user began by hand) is adopted as it is.
    for (const char* sub : {"", "reference/", "rommap/", "cache/"})
    {
        const std::string path = dir + sub;
        if (!SDL_CreateDirectory(path.c_str()))
        {
            last_error = std::string("cannot create ") + path + ": " + SDL_GetError();
            return false;
        }
    }
    // The original goes in reference/, where the project finds it again by
    // relative path on any machine. Never over a file that is already there.
    const std::string copy = dir + "reference/" + rom_file;
    if (!path_exists(copy) && !copy_file(rom_path, copy))
    {
        last_error = "cannot copy the ROM to " + copy;
        return false;
    }
    Session card;
    card.file = rom_file;
    card.crc = crc;
    card.size = size;
    card.system = GetSystemModelName(emu_get_core()->GetSystemModel());
    write_project_card(dir, card);
    write_gitignore_if_missing(dir);
    dir_cache_valid = false;
    RomMap* map = rom_map();
    if (map && map->IsEnabled())
    {
        attach_current_media();
        start_session();
    }
    last_error.clear();
    return true;
}

bool rom_map_project_resolve_rom(const std::string& directory, std::string& rom)
{
    namespace fs = std::filesystem;
    rom.clear();
    last_error.clear();
    const fs::path root(directory);
    if (!root.is_absolute())
    {
        last_error = "project directory must be an absolute local path";
        return false;
    }
    std::ifstream card(root / "rommap" / "project.txt");
    std::string line, name;
    u32 crc = 0;
    bool haveCrc = false;
    while (std::getline(card, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream field(line);
        std::string key;
        field >> key;
        if (key == "rom") std::getline(field >> std::ws, name);
        else if (key == "crc32") haveCrc = bool(field >> std::hex >> crc);
    }
    if (name.empty() || !haveCrc || fs::path(name).filename() != fs::path(name)
        || name == "." || name == ".." || name.find('\\') != std::string::npos)
    {
        last_error = "missing or invalid rommap/project.txt (ROM name and CRC required)";
        return false;
    }
    const fs::path candidate = root / "reference" / name;
    std::error_code ec;
    if (!fs::is_regular_file(candidate, ec))
    {
        last_error = "project reference ROM is missing";
        return false;
    }
    Cartridge probe;
    if (!probe.LoadFromFile(candidate.string().c_str()) || probe.GetCRC() != crc)
    {
        last_error = "project reference ROM does not match its recorded CRC";
        return false;
    }
    rom = candidate.lexically_normal().string();
    return true;
}

// The OS file manager opens the folder (Finder, Explorer, the desktop's own).
void rom_map_project_open_folder(const std::string& dir)
{
    std::string url = "file://";
    if (!dir.empty() && dir[0] != '/')
        url += "/";                         // Windows: file:///C:/...
    for (char c : dir)
    {
        if (c == ' ')
            url += "%20";
        else if (c == '\\')
            url += '/';
        else
            url += c;
    }
    SDL_OpenURL(url.c_str());
}
