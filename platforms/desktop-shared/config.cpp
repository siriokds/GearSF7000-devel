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

#include <algorithm>
#include <fstream>
#include <SDL3/SDL.h>
#include "../../src/gearsf7000.h"

#define MINI_CASE_SENSITIVE
#include "mINI/ini.h"

#define CONFIG_IMPORT
#include "config.h"
#include "sk1100_keyboard_layout.h"
#include "coleco_keypad_layout.h"
#include "md_pad_layout.h"
#include "ConfigProfiles.h"
#include "DiagnosticLog.h"

static std::unique_ptr<config_storage::Repository> config_repository;
static std::string config_storage_error, config_storage_notice;
const std::string& config_get_storage_error() { return config_storage_error; }
const std::string& config_get_storage_notice() { return config_storage_notice; }
bool config_storage_writable() { return config_repository && config_repository->writable(); }

config_SaveResult config_get_storage_result() {
    return {config_storage_error.empty(), config_storage_writable(),
        config_repository ? config_repository->file : config_emu_file_path,
        config_repository ? config_repository->operation : "init",
        config_storage_error};
}

// Version 3: global settings plus machines/<family>.ini.
static const int config_format_version = config_storage::globalVersion;

static bool check_portable(void);
static int read_int(const char* group, const char* key, int default_value);
static void write_int(const char* group, const char* key, int integer);
static float read_float(const char* group, const char* key, float default_value);
static void write_float(const char* group, const char* key, float value);
static bool read_bool(const char* group, const char* key, bool default_value);
static void write_bool(const char* group, const char* key, bool boolean);
static std::string read_string(const char* group, const char* key);
static void write_string(const char* group, const char* key, std::string value);
static config_Hotkey read_hotkey(const char* key, config_Hotkey default_value);
static void write_hotkey(const char* key, const config_Hotkey& hotkey);
static void build_config_path(char* destination, size_t destination_size, const char* filename);


// Joypad defaults. Player 2 sits on the numeric keypad where one exists; Mac
// laptops have none, so on macOS it gets I/J/K/L with U and O, clear of player
// 1's arrows and A/S. Only defaults: a saved configuration keeps its keys.
static const SDL_Scancode kDefaultKeys[2][6] = {
    {SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
     SDL_SCANCODE_A, SDL_SCANCODE_S},
#if defined(__APPLE__)
    {SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_I, SDL_SCANCODE_K,
     SDL_SCANCODE_U, SDL_SCANCODE_O},
#else
    {SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_3, SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_2,
     SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6},
#endif
};

bool config_default_key_binding(const SDL_Scancode* binding, SDL_Scancode* value)
{
    for (int player = 0; player < 2; ++player)
    {
        const config_Input& input = config_input[player];
        const SDL_Scancode* keys[6] = {&input.key_left, &input.key_right, &input.key_up,
                                       &input.key_down, &input.key_left_button, &input.key_right_button};
        for (int i = 0; i < 6; ++i)
            if (keys[i] == binding)
            {
                *value = kDefaultKeys[player][i];
                return true;
            }
        for (int key = 0; key < SK1100_KEYBOARD_KEY_COUNT; ++key)
            if (&input.sk1100_key[key] == binding)
            {
                *value = sk1100_keyboard_layout[key].default_scancode;
                return true;
            }
        for (int key = 0; key < COLECO_KEYPAD_KEY_COUNT; ++key)
            if (&input.keypad_key[key] == binding)
            {
                *value = coleco_keypad_layout[key].default_scancode[player];
                return true;
            }
    }
    return false;
}

bool config_default_button_binding(const int* binding, int* value)
{
    for (int player = 0; player < 2; ++player)
    {
        if (binding == &config_input[player].gamepad_left_button)
        {
            *value = SDL_GAMEPAD_BUTTON_SOUTH;
            return true;
        }
        if (binding == &config_input[player].gamepad_right_button)
        {
            *value = SDL_GAMEPAD_BUTTON_EAST;
            return true;
        }
    }
    return false;
}

// The headless instance keeps its settings in a headless/ folder beside the
// main ones, so a GUI and a headless instance open together never overwrite
// each other's file on exit. The first time, the folder starts as a copy of
// the main settings (BIOS paths, mappers, projects root); after that the two
// live apart.
void config_init_headless(void)
{
    char* main_root = nullptr;
    if (check_portable())
        main_root = SDL_strdup(SDL_GetBasePath());
#if defined(__APPLE__)
    else
        main_root = SDL_GetPrefPath("siriokds", "GearSF7000");
#else
    else
        main_root = SDL_GetPrefPath("Geardome", "GearSF7000");
#endif
    if (!main_root)
    {
        config_init();
        return;
    }

    namespace fs = std::filesystem;
    const fs::path from = config_storage::utf8Path(main_root);
    const fs::path to = from / "headless";
    SDL_free(main_root);

    std::error_code ec;
    if (!fs::exists(to / "gearconfig.ini", ec))
    {
        fs::create_directories(to / "machines", ec);
        // Paths into the main folder (save files, save states) are moved
        // into headless/ so the two never share a battery save or a state.
        std::ifstream in(from / "gearconfig.ini", std::ios::binary);
        if (in)
        {
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const std::string old_root = config_storage::utf8String(from / "");
            const std::string new_root = config_storage::utf8String(to / "");
            for (size_t at = text.find(old_root); at != std::string::npos;
                 at = text.find(old_root, at + new_root.size()))
                text.replace(at, old_root.size(), new_root);
            std::ofstream(to / "gearconfig.ini", std::ios::binary) << text;
        }
        for (const auto& entry : fs::directory_iterator(from / "machines", ec))
            if (entry.path().extension() == ".ini")
                fs::copy_file(entry.path(), to / "machines" / entry.path().filename(), ec);
    }

    config_init(config_storage::utf8String(to).c_str());
}

void config_init(const char* settings_root)
{
    if (settings_root) {
        std::string path = settings_root;
        if (path.empty()) path = "./";
        if (path.back() != '/' && path.back() != '\\') path += '/';
        config_root_path = SDL_strdup(path.c_str());
    }
    else if (check_portable())
        config_root_path = SDL_strdup(SDL_GetBasePath());
#if defined(__APPLE__)
    else
        config_root_path = SDL_GetPrefPath("siriokds", "GearSF7000");
#else
    else
        config_root_path = SDL_GetPrefPath("Geardome", "GearSF7000");
#endif

    if (!config_root_path)
    {
        Log("Unable to determine settings directory: %s", SDL_GetError());
        config_root_path = SDL_strdup("./");
    }

    build_config_path(config_emu_file_path, sizeof(config_emu_file_path), "gearconfig.ini");
    build_config_path(config_imgui_file_path, sizeof(config_imgui_file_path), "imgui.ini");



    for (int player = 0; player < 2; ++player)
    {
        config_Input& input = config_input[player];
        SDL_Scancode* keys[6] = {&input.key_left, &input.key_right, &input.key_up,
                                 &input.key_down, &input.key_left_button, &input.key_right_button};
        for (SDL_Scancode* key : keys)
            config_default_key_binding(key, key);
        // A Mega Drive pad's extra buttons: unbound. Zero would be the south
        // gamepad button, hence -1.
        input.controller_device = 0;
        for (int button = 0; button < 6; ++button)
        {
            input.md_key[button] = SDL_SCANCODE_UNKNOWN;
            input.gamepad_md_button[button] = -1;
        }
    }
    //config_input[0].key_blue = SDL_SCANCODE_D;
    //config_input[0].key_purple = SDL_SCANCODE_F;
    //config_input[0].key_0 = SDL_SCANCODE_KP_0;
    //config_input[0].key_1 = SDL_SCANCODE_KP_1;
    //config_input[0].key_2 = SDL_SCANCODE_KP_2;
    //config_input[0].key_3 = SDL_SCANCODE_KP_3;
    //config_input[0].key_4 = SDL_SCANCODE_KP_4;
    //config_input[0].key_5 = SDL_SCANCODE_KP_5;
    //config_input[0].key_6 = SDL_SCANCODE_KP_6;
    //config_input[0].key_7 = SDL_SCANCODE_KP_7;
    //config_input[0].key_8 = SDL_SCANCODE_KP_8;
    //config_input[0].key_9 = SDL_SCANCODE_KP_9;
    //config_input[0].key_asterisk = SDL_SCANCODE_KP_MULTIPLY;
    //config_input[0].key_hash = SDL_SCANCODE_KP_DIVIDE;
    config_input[0].gamepad = true;
    config_input[0].gamepad_invert_x_axis = false;
    config_input[0].gamepad_invert_y_axis = false;
    config_input[0].gamepad_left_button = SDL_GAMEPAD_BUTTON_SOUTH;
    config_input[0].gamepad_right_button = SDL_GAMEPAD_BUTTON_EAST;
    //config_input[0].gamepad_blue = SDL_CONTROLLER_BUTTON_GUIDE;
    //config_input[0].gamepad_purple = SDL_CONTROLLER_BUTTON_GUIDE;
    config_input[0].gamepad_x_axis = 0;
    config_input[0].gamepad_y_axis = 1;
    //config_input[0].gamepad_1 = SDL_CONTROLLER_BUTTON_X;
    //config_input[0].gamepad_2 = SDL_CONTROLLER_BUTTON_Y;
    //config_input[0].gamepad_3 = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    //config_input[0].gamepad_4 = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
    //config_input[0].gamepad_5 = SDL_CONTROLLER_BUTTON_RIGHTSTICK;
    //config_input[0].gamepad_6 = SDL_CONTROLLER_BUTTON_LEFTSTICK;
    //config_input[0].gamepad_7 = SDL_CONTROLLER_BUTTON_GUIDE;
    //config_input[0].gamepad_8 = SDL_CONTROLLER_BUTTON_GUIDE;
    //config_input[0].gamepad_9 = SDL_CONTROLLER_BUTTON_GUIDE;
    //config_input[0].gamepad_0 = SDL_CONTROLLER_BUTTON_GUIDE;
    //config_input[0].gamepad_asterisk = SDL_CONTROLLER_BUTTON_START;
    //config_input[0].gamepad_hash = SDL_CONTROLLER_BUTTON_BACK;

    //config_input[1].key_blue = SDL_SCANCODE_J;
    //config_input[1].key_purple = SDL_SCANCODE_K;
    //config_input[1].key_0 = SDL_SCANCODE_NONUSBACKSLASH;
    //config_input[1].key_1 = SDL_SCANCODE_Z;
    //config_input[1].key_2 = SDL_SCANCODE_X;
    //config_input[1].key_3 = SDL_SCANCODE_C;
    //config_input[1].key_4 = SDL_SCANCODE_V;
    //config_input[1].key_5 = SDL_SCANCODE_B;
    //config_input[1].key_6 = SDL_SCANCODE_N;
    //config_input[1].key_7 = SDL_SCANCODE_M;
    //config_input[1].key_8 = SDL_SCANCODE_COMMA;
    //config_input[1].key_9 = SDL_SCANCODE_PERIOD;
    //config_input[1].key_asterisk = SDL_SCANCODE_SLASH;
    //config_input[1].key_hash = SDL_SCANCODE_RSHIFT;
    config_input[1].gamepad = true;
    config_input[1].gamepad_invert_x_axis = false;
    config_input[1].gamepad_invert_y_axis = false;
    config_input[1].gamepad_left_button = SDL_GAMEPAD_BUTTON_SOUTH;
    config_input[1].gamepad_right_button = SDL_GAMEPAD_BUTTON_EAST;
    //config_input[1].gamepad_blue = SDL_CONTROLLER_BUTTON_GUIDE;
    //config_input[1].gamepad_purple = SDL_CONTROLLER_BUTTON_GUIDE;
    config_input[1].gamepad_x_axis = 0;
    config_input[1].gamepad_y_axis = 1;
    //config_input[1].gamepad_1 = SDL_CONTROLLER_BUTTON_X;
    //config_input[1].gamepad_2 = SDL_CONTROLLER_BUTTON_Y;
    //config_input[1].gamepad_3 = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    //config_input[1].gamepad_4 = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
    //config_input[1].gamepad_5 = SDL_CONTROLLER_BUTTON_RIGHTSTICK;
    //config_input[1].gamepad_6 = SDL_CONTROLLER_BUTTON_LEFTSTICK;
    //config_input[1].gamepad_7 = SDL_CONTROLLER_BUTTON_GUIDE;
    //config_input[1].gamepad_8 = SDL_CONTROLLER_BUTTON_GUIDE;
    //config_input[1].gamepad_9 = SDL_CONTROLLER_BUTTON_GUIDE;
    //config_input[1].gamepad_0 = SDL_CONTROLLER_BUTTON_GUIDE;
    //config_input[1].gamepad_asterisk = SDL_CONTROLLER_BUTTON_START;
    //config_input[1].gamepad_hash = SDL_CONTROLLER_BUTTON_BACK;

    for (int player = 0; player < 2; ++player)
        for (int key = 0; key < SK1100_KEYBOARD_KEY_COUNT; ++key)
            config_input[player].sk1100_key[key] = sk1100_keyboard_layout[key].default_scancode;

    for (int hotkey = 0; hotkey < config_HotkeyIndex_COUNT; ++hotkey)
        config_hotkeys[hotkey] = config_default_hotkey((config_HotkeyIndex)hotkey);

    config_repository.reset(new config_storage::Repository(
        config_storage::utf8Path(config_root_path), {}, config_profiles::migrate));
}

// The projects folder for a config that names none: the one that fits the
// host, and from then on written to config.ini and shown in Debug > Analysis
// rather than left implicit. It is where a person looks for their own work,
// and SDL knows where that is on each system:
//   macOS    ~/Documents/GearSF7000 Projects
//   Windows  C:\Users\<name>\Documents\GearSF7000 Projects
//   Linux    $XDG_DOCUMENTS_DIR, or ~/Documents, whichever the desktop set up
// When the system names no Documents folder (a bare Linux account) the home
// folder stands in, and only if that fails too the settings folder, which
// always exists. Portable mode keeps everything beside the program, as it
// does for the settings.
std::string config_default_projects_dir(void)
{
    const std::string beside_settings =
        std::string(config_root_path ? config_root_path : "./") + "projects/";
    if (check_portable())
        return beside_settings;
    const char* base = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    if (!base || !base[0])
        base = SDL_GetUserFolder(SDL_FOLDER_HOME);
    if (!base || !base[0])
        return beside_settings;
    std::string path = base;
#if defined(_WIN32)
    const char separator = '\\';
#else
    const char separator = '/';
#endif
    if (path.back() != '/' && path.back() != '\\')
        path += separator;
    path += "GearSF7000 Projects";
    path += separator;
    return path;
}

void config_destroy(void)
{
    config_repository.reset();
    SDL_free(config_root_path);
}

void config_read(void)
{
    config_repository->load(config_ini_data);
    config_storage_error = config_repository->error;
    config_storage_notice = config_repository->notice;
    if (!config_storage_error.empty()) Log("Configuration error: %s", config_storage_error.c_str());
    if (!config_storage_notice.empty()) Log("%s", config_storage_notice.c_str());

#if GEARSF7000_ENABLE_DEBUG_TOOLS
    config_debug.debug = read_bool("Debug", "Debug", false);
#else
    config_debug.debug = false;
#endif
    config_debug.show_disassembler = read_bool("Debug", "Disassembler", true);
    config_debug.show_screen = read_bool("Debug", "Screen", true);
    config_debug.show_memory = read_bool("Debug", "Memory", true);
    config_debug.show_processor = read_bool("Debug", "Processor", true);
    config_debug.show_pattern_table = read_bool("Debug", "PatternTable",
        read_bool("Debug", "Video", false));
    config_debug.show_sprites = read_bool("Debug", "Sprites", false);
    config_debug.show_nametable = read_bool("Debug", "NameTable", false);
    config_debug.show_video_register_explorer = read_bool("Debug", "VideoRegisterExplorer", false);
    config_debug.show_psg = read_bool("Debug", "PSG", false);
#if GEARSF7000_ENABLE_YM2413
    config_debug.show_ym2413 = read_bool("Debug", "YM2413", false);
#endif
    
    config_debug.show_cassette = read_bool("Debug", "Cassette", false);
    config_debug.show_rewind = read_bool("Debug", "Rewind", false);
#if GEARSF7000_ENABLE_RECORDER
    config_debug.rewind_enabled = read_bool("Debug", "RecorderEnabled", true);
#else
    config_debug.rewind_enabled = false;
#endif
    config_debug.log_level = read_int("Debug", "LogLevel", 3);
    config_debug.log_max_mb = read_int("Debug", "LogMaxMB", 4);
    config_debug.rewind_seconds = read_int("Debug", "RewindSeconds", 60);
    config_debug.show_ppi_registers = read_bool("Debug", "PPI8255A", false);
    config_debug.show_sf7000 = read_bool("Debug", "SF7000", false);
    config_debug.show_events = read_bool("Debug", "Events", false);
    config_debug.show_rom_map = read_bool("Debug", "RomMap", false);
    config_debug.show_disassembler_new = read_bool("Debug", "DisassemblerNew", false);
    config_debug.show_deep_search = read_bool("Debug", "DeepSearch", false);
    config_debug.show_to_check = read_bool("Debug", "ToCheck", false);
    config_debug.show_palettes = read_bool("Debug", "Palettes", false);
    config_debug.rom_map_enabled = read_bool("Debug", "RomMapEnabled", false);
    config_debug.projects_dir = read_string("Debug", "ProjectsDir");
    if (config_debug.projects_dir.empty())
        config_debug.projects_dir = config_default_projects_dir();
    config_debug.events_category_filter = read_int("Debug", "EventsCategoryFilter", -1);
    config_debug.events_newest_first = read_bool("Debug", "EventsNewestFirst", true);
    config_debug.events_group_by_pc = read_bool("Debug", "EventsGroupByPc", false);
    config_debug.events_normalize_clock = read_bool("Debug", "EventsNormalizeClock", false);
    config_debug.show_memory_import = read_bool("Debug", "MemoryImport", false);
    config_debug.show_rom_inspector = read_bool("Debug", "RomInspector", false);
    config_debug.show_disc_explorer = read_bool("Debug", "DiscExplorer", false);
    config_debug.vram_background_area_scale = read_float("Debug", "VramBackgroundAreaScale", 2.0f);
    config_debug.vram_tiles_area_scale = read_float("Debug", "VramTilesAreaScale", 2.0f);
    config_debug.vram_sprites_area_scale = read_float("Debug", "VramSpritesAreaScale", 4.0f);
    config_debug.vram_sprites_preview_area_scale = read_float("Debug", "VramSpritesPreviewAreaScale", 1.0f);
    config_debug.vram_sprites_source_code = read_string("Debug", "VramSpritesSourceCode");
    if (config_debug.vram_sprites_source_code.empty())
    {
        static const char* legacySources[] = {
            "live-sat", "last-rendered-frame", "current-scanline-fetch",
            "last-frame-scanline"
        };
        const int legacySource = std::clamp(
            read_int("Debug", "VramSpritesSource", 0), 0, 3);
        config_debug.vram_sprites_source_code = legacySources[legacySource];
    }
    config_debug.vram_sprites_scanline = read_int("Debug", "VramSpritesScanline", 0);
    config_debug.vram_sprites_hidden_preview_parts = read_string(
        "Debug", "VramSpritesHiddenPreviewParts");
    config_debug.vram_sprites_disabled_preview_options = read_string(
        "Debug", "VramSpritesDisabledPreviewOptions");
    config_debug.vram_pattern_disabled_layer_options = read_string(
        "Debug", "VramPatternDisabledLayerOptions");
    config_debug.gray_inactive_sprite_attributes = read_bool(
        "Debug", "GrayInactiveSpriteAttributes", true);
    config_debug.show_grid_background = read_bool("Debug", "ShowGridBackground", true);
    config_debug.show_viewport_background = read_bool("Debug", "ShowViewportBackground", true);
    config_debug.show_regions_background = read_bool("Debug", "ShowRegionsBackground", false);
    config_debug.show_grid_tiles = read_bool("Debug", "ShowGridTiles", true);
    config_debug.show_crt_test_pass = read_bool("Debug", "CrtTestPass", false);
    config_debug.show_composite_decode_test = read_bool("Debug", "CompositeDecodeTest", false);
    config_debug.show_crt_consumer_test = read_bool("Debug", "CrtConsumerTest", false);
    config_debug.show_crt_lottes_test = read_bool("Debug", "CrtLottesTest", false);
#if GEARSF7000_ENABLE_MCP
    config_debug.show_mcp_server = read_bool("Debug", "ShowMCPServer", false);
    config_debug.show_watch_monitor = read_bool("Debug", "ShowWatchMonitor", false);
#endif
    config_debug.show_basic_typer = read_bool("Debug", "ShowBasicTyper", false);
    config_debug.basic_typer_decode_escapes = read_bool("Debug", "BasicTyperDecodeEscapes", false);
    config_debug.basic_typer_speed = read_int("Debug", "BasicTyperSpeed", 0);
    config_debug.basic_pointer_block = read_int("Debug", "BasicPointerBlock", 0);
    config_debug.dis_follow_pc = read_bool("Debug", "DisFollowPC", true);
    config_debug.dis_show_opcodes = read_bool("Debug", "DisOpcodes", true);
    config_debug.dis_show_symbols = read_bool("Debug", "DisSymbols", true);
    config_debug.dis_show_segments = read_bool("Debug", "DisSegments", true);
    config_debug.dis_show_bank = read_bool("Debug", "DisBank", true);

    config_debug.font_size = read_int("Debug", "FontSize", 0);
    // FontSize is an index into gui.cpp's four cached default fonts.  Keep
    // old or hand-edited configuration files from indexing outside it.
    if (config_debug.font_size < 0 || config_debug.font_size > 3)
        config_debug.font_size = 0;
    config_debug.ui_density = read_int("Debug", "UiDensity", 0);
    if (config_debug.ui_density < 0 || config_debug.ui_density > 2)
        config_debug.ui_density = 0;
    config_debug.ui_scale = read_float("Debug", "UiScale", 1.0f);
    if (config_debug.ui_scale < 0.75f || config_debug.ui_scale > 2.0f)
        config_debug.ui_scale = 1.0f;
#if defined(__APPLE__)
    // Retina displays retain the 1:1 pixel geometry of the condensed grid;
    // Light has proved the most comfortable default for long debug sessions.
    constexpr int default_memory_data_font = 2;
#else
    constexpr int default_memory_data_font = 0;
#endif
    config_debug.memory_data_font = read_int("Debug", "MemoryDataFont", default_memory_data_font);
    if (config_debug.memory_data_font < 0 || config_debug.memory_data_font > 2)
        config_debug.memory_data_font = default_memory_data_font;
    config_debug.multi_viewport = read_bool("Debug", "MultiViewport", false);

    config_emulator.fullscreen = read_bool("Emulator", "FullScreen", false);
    config_emulator.show_menu = read_bool("Emulator", "ShowMenu", true);
    config_emulator.keyboard_mode = read_bool("Machine.SK1100", "KeyboardMode", true);
    config_emulator.ffwd_speed = read_int("Emulator", "FFWD", 1);
    config_emulator.save_slot = read_int("Emulator", "SaveSlot", 0);
    config_emulator.start_paused = read_bool("Emulator", "StartPaused", false);
    config_emulator.machine = read_int("Emulator", "Machine",
                                       (int)SystemModel::SC3000Export);
    config_emulator.last_sega_machine = read_int("Emulator", "LastSegaMachine",
        config_emulator.last_sega_machine);
    config_emulator.coleco_sgm = read_bool("Machine.Coleco", "SGM", HasSGM(static_cast<SystemModel>(config_emulator.machine)));
    if (config_emulator.machine < 0 || config_emulator.machine > static_cast<int>(SystemModel::GameGearExport) ||
        !IsValidSystemModel(static_cast<SystemModel>(config_emulator.machine)))
        config_emulator.machine = (int)SystemModel::SC3000Export;
    for (const auto& group : GetMachineMenuGroups()) {
        const int saved = read_int("Emulator", (std::string("Board.") + group.code).c_str(), static_cast<int>(group.defaultModel));
        config_emulator.last_board_by_machine[group.code] = MachineGroupContains(group, static_cast<SystemModel>(saved))
            ? saved : static_cast<int>(group.defaultModel);
    }
    for (const auto& p : config_profiles::profiles) {
        int value = read_int(p.section, p.key, 0);
        if (value && !GetVideoCatalog().IsDeclared(p.model, config_resolve_video_option(value, {}))) value = 0;
        const std::string optionKey = std::string(p.key) + ".Option";
        const std::string savedOption = read_string(p.section, optionKey.c_str());
        std::string optionCode;
        if (savedOption == "default") value = 0;
        else if (!savedOption.empty()) {
            const auto* option = GetVideoCatalog().FindOption(savedOption);
            if (option && GetVideoCatalog().IsDeclared(p.model, savedOption)) {
                value = option->legacyConfig;
                optionCode = savedOption;
            } else {
                Log("Unknown or incompatible video option '%s'; using machine default", savedOption.c_str());
                value = 0;
            }
        } else optionCode = config_resolve_video_option(value, {});
        config_emulator.video_option_by_model[static_cast<int>(p.model)] = optionCode;
        config_emulator.vdp_by_model[static_cast<int>(p.model)] = value;
    }
    config_emulator.vdp = config_emulator.vdp_by_model[config_emulator.machine];
    config_emulator.video_option = config_emulator.video_option_by_model[config_emulator.machine];
    for (int f = 0; f < config_MapperFamily_COUNT; ++f) {
        const SystemModel fallback = static_cast<SystemModel>(config_emulator.last_model_by_family[f]);
        const int value = read_int(config_profiles::familySection(fallback), "LastModel", static_cast<int>(fallback));
        const SystemModel model = static_cast<SystemModel>(value);
        if (value >= 0 && value <= static_cast<int>(SystemModel::GameGearExport) && IsValidSystemModel(model) &&
            config_mapper_family(model) == f) config_emulator.last_model_by_family[f] = value;
    }
    {
        // Legacy global mapper keys have already been routed by the migrator.
        static const char* const kFamilySections[config_MapperFamily_COUNT] = {
            "Machine.SG1000", "Machine.SMS", "Machine.Coleco", "Machine.MSX", "Machine.SordM5", "Machine.GameGear"};
        const config_MapperFamily current =
            config_mapper_family(static_cast<SystemModel>(config_emulator.machine));
        for (int f = 0; f < config_MapperFamily_COUNT; f++)
        {
            config_emulator.mapper_mode_by_family[f] = read_int(kFamilySections[f],
                "MapperMode", 0);
            config_emulator.mapper_type_by_family[f] = read_int(kFamilySections[f],
                "MapperType", config_emulator.mapper_type_by_family[f]);
        }
        config_emulator.mapper_mode = config_emulator.mapper_mode_by_family[current];
        config_emulator.mapper_type = config_emulator.mapper_type_by_family[current];
    }
#if GEARSF7000_ENABLE_MCP
    config_emulator.mcp_tcp_port = read_int("Emulator", "MCPTCPPort", 7777);
    config_emulator.mcp_http_address = read_string("Emulator", "MCPHTTPAddress");
    if (config_emulator.mcp_http_address.empty())
        config_emulator.mcp_http_address = "127.0.0.1";
#endif


    const auto readTape = [](const char* section, config_Tape& tape) {
        tape.path = read_string(section, "CassettePath");
        tape.write_protected = read_bool(section, "CassetteWriteProtected", true);
        for (int i = 0; i < config_max_recent_cassettes; ++i) {
            const std::string key = "RecentCassette" + std::to_string(i);
            tape.recent[i] = read_string(section, key.c_str());
        }
    };
    readTape("Machine.SR1000", config_emulator.sr1000_tape);
    readTape("Machine.MSXTape", config_emulator.msx_tape);

    config_emulator.bios_path = read_string("Machine.SF7000", "BiosPath");
    config_emulator.coleco_bios_path = read_string("Machine.Coleco", "BiosPath");
    config_emulator.m5_bios_path = read_string("Machine.SordM5", "BiosPath");
    config_emulator.m5_em5_installed = read_bool("Machine.SordM5", "EM5Installed", false);
    config_emulator.msx_bios_path = read_string("Machine.MSX", "BiosPath");
    config_emulator.msx_use_cbios = read_bool("Machine.MSX", "UseCBios", config_emulator.msx_bios_path.empty());
    config_emulator.disc_path = read_string("Machine.SF7000", "DiscPath");
    config_emulator.disc_write_protected = read_bool("Machine.SF7000", "DiscWriteProtected", true);

    for (int i = 0; i < config_max_recent_discs; i++)
    {
        std::string item = "RecentDISC" + std::to_string(i);
        config_emulator.recent_discs[i] = read_string("Machine.SF7000", item.c_str());
    }

    

    config_emulator.savefiles_dir_option = read_int("Emulator", "SaveFilesDirOption", 0);
    config_emulator.savefiles_path = read_string("Emulator", "SaveFilesPath");
    config_emulator.savestates_dir_option = read_int("Emulator", "SaveStatesDirOption", 0);
    config_emulator.savestates_path = read_string("Emulator", "SaveStatesPath");
    config_emulator.last_open_path = read_string("Emulator", "LastOpenPath");
    config_emulator.window_width = read_int("Emulator", "WindowWidth", 770);
    config_emulator.window_height = read_int("Emulator", "WindowHeight", 600);
    //config_emulator.spinner = read_int("Emulator", "Spinner", 0);
    //config_emulator.spinner_sensitivity = read_int("Emulator", "SpinnerSensitivity", 4);
    config_emulator.status_messages = read_bool("Emulator", "StatusMessages", false);

    if (config_emulator.savefiles_path.empty())
    {
        config_emulator.savefiles_path = config_root_path;
    }
    if (config_emulator.savestates_path.empty())
    {
        config_emulator.savestates_path = config_root_path;
    }

    for (int i = 0; i < config_max_recent_roms; i++)
    {
        std::string item = "RecentROM" + std::to_string(i);
        config_emulator.recent_roms[i] = read_string("Emulator", item.c_str());
    }

    config_video.scale = read_int("Video", "Scale", 0);
    config_video.ratio = read_int("Video", "AspectRatio", 1);
    config_video.overscan = read_int("Video", "Overscan", 1);
    config_video.fps = read_bool("Video", "ShowFPS",
                                 read_bool("Video", "FPS", false));
    config_video.frame_pacing_info = read_bool("Video", "ShowFramePacingInfo", false);
    {
        // One-time migration from the old standalone Bilinear checkbox to
        // the new mutually-exclusive Postprocessing state - a config file
        // written before this change has "Bilinear" but no
        // "Postprocessing" key yet, so derive the new field's default from
        // the old one instead of losing the user's existing choice.
        const bool legacy_bilinear = read_bool("Video", "Bilinear", false);
        config_video.postprocessing = read_int("Video", "Postprocessing",
            legacy_bilinear ? POSTPROCESSING_BILINEAR : POSTPROCESSING_NONE);
    }
    config_video.crt_lottes_mask_type = read_float("Video", "CrtLottesMaskType", 0.0f);
    config_video.crt_lottes_mask_intensity = read_float("Video", "CrtLottesMaskIntensity", 0.12f);
    config_video.crt_lottes_scanline_thinness = read_float("Video", "CrtLottesScanlineThinness", 0.39f);
    config_video.crt_lottes_scan_blur = read_float("Video", "CrtLottesScanBlur", 6.0f);
    config_video.crt_lottes_gamma = read_float("Video", "CrtLottesGamma", 2.54f);
    config_video.crt_lottes_black_threshold = read_float("Video", "CrtLottesBlackThreshold", 0.15f);
    config_video.crt_lottes_boldness = read_float("Video", "CrtLottesBoldness", 0.42f);
    config_video.crt_lottes_edge_radius = read_float("Video", "CrtLottesEdgeRadius", 0.41f);
    config_video.advanced_scaling_sharpness = read_float("Video", "AdvancedScalingSharpness", 4.0f);
    config_video.advanced_scaling_black_threshold = read_float("Video", "AdvancedScalingBlackThreshold", 0.15f);
    config_video.advanced_scaling_boldness = read_float("Video", "AdvancedScalingBoldness", 0.5f);
    config_video.advanced_scaling_edge_radius = read_float("Video", "AdvancedScalingEdgeRadius", 1.0f);
#ifdef SPRITE_EXPANDER
    config_video.sprite_limit = read_bool("Video", "SpriteLimit", false);
#endif
    config_video.scanlines = read_bool("Video", "Scanlines", true);
    config_video.scanlines_intensity = read_float("Video", "ScanlinesIntensity", 0.10f);
    // Full Frame used to be encoded as a fifth Computer-mode overscan value.
    // It is a debugger source now, so an old INI must not make it leak back
    // into Computer mode after this split.
    if (config_video.overscan < 0 || config_video.overscan > 3)
        config_video.overscan = 1;

    // The debug output follows the main profile by default. Its independent
    // values remain persistent and become active when that link is disabled.
#if GEARSF7000_ENABLE_DEBUG_TOOLS
    config_debug.video_as_main_settings = read_bool("DebugVideo", "AsMainVideoSettings", true);
    config_debug.video.scale = read_int("DebugVideo", "Scale", config_debug.video.scale);
    config_debug.video.ratio = read_int("DebugVideo", "AspectRatio", config_debug.video.ratio);
    config_debug.video.overscan = read_int("DebugVideo", "Overscan", config_debug.video.overscan);
    config_debug.video.postprocessing = read_int("DebugVideo", "Postprocessing", config_debug.video.postprocessing);
    config_debug.video.crt_lottes_mask_type = read_float("DebugVideo", "CrtLottesMaskType", config_debug.video.crt_lottes_mask_type);
    config_debug.video.crt_lottes_mask_intensity = read_float("DebugVideo", "CrtLottesMaskIntensity", config_debug.video.crt_lottes_mask_intensity);
    config_debug.video.crt_lottes_scanline_thinness = read_float("DebugVideo", "CrtLottesScanlineThinness", config_debug.video.crt_lottes_scanline_thinness);
    config_debug.video.crt_lottes_scan_blur = read_float("DebugVideo", "CrtLottesScanBlur", config_debug.video.crt_lottes_scan_blur);
    config_debug.video.crt_lottes_gamma = read_float("DebugVideo", "CrtLottesGamma", config_debug.video.crt_lottes_gamma);
    config_debug.video.crt_lottes_black_threshold = read_float("DebugVideo", "CrtLottesBlackThreshold", config_debug.video.crt_lottes_black_threshold);
    config_debug.video.crt_lottes_boldness = read_float("DebugVideo", "CrtLottesBoldness", config_debug.video.crt_lottes_boldness);
    config_debug.video.crt_lottes_edge_radius = read_float("DebugVideo", "CrtLottesEdgeRadius", config_debug.video.crt_lottes_edge_radius);
    config_debug.video.advanced_scaling_sharpness = read_float("DebugVideo", "AdvancedScalingSharpness", config_debug.video.advanced_scaling_sharpness);
    config_debug.video.advanced_scaling_black_threshold = read_float("DebugVideo", "AdvancedScalingBlackThreshold", config_debug.video.advanced_scaling_black_threshold);
    config_debug.video.advanced_scaling_boldness = read_float("DebugVideo", "AdvancedScalingBoldness", config_debug.video.advanced_scaling_boldness);
    config_debug.video.advanced_scaling_edge_radius = read_float("DebugVideo", "AdvancedScalingEdgeRadius", config_debug.video.advanced_scaling_edge_radius);
#endif
    config_video.palette = read_int("Video", "Palette", 0);
    config_video.sms_tms_palette = read_int("Video", "SmsTmsPalette", 0);

    config_video.pacing = config_video_pacing_from_name(
        read_string("Video", "Pacing").c_str(), PACING_GAMING_VSYNC);
    
    config_audio.enable = read_bool("Audio", "Enable", true);
#if GEARSF7000_ENABLE_AY
    config_audio.ay_enable = read_bool("Machine.SegaAudio", "AYEnable", false);
    config_audio.ay_chip = read_int("Machine.SegaAudio", "AYChip", 0);
    config_audio.ay_port_base = read_int("Machine.SegaAudio", "AYPortBase", 0x20);
#endif
#if GEARSF7000_ENABLE_YM2413
    config_audio.ym2413_enable = read_bool("Machine.SMS", "YM2413Enable", false);
#endif

    config_input[0].key_left = (SDL_Scancode)read_int("InputA", "KeyLeft", SDL_SCANCODE_LEFT);
    config_input[0].key_right = (SDL_Scancode)read_int("InputA", "KeyRight", SDL_SCANCODE_RIGHT);
    config_input[0].key_up = (SDL_Scancode)read_int("InputA", "KeyUp", SDL_SCANCODE_UP);
    config_input[0].key_down = (SDL_Scancode)read_int("InputA", "KeyDown", SDL_SCANCODE_DOWN);
    config_input[0].key_left_button = (SDL_Scancode)read_int("InputA", "KeyLeftButton", SDL_SCANCODE_A);
    config_input[0].key_right_button = (SDL_Scancode)read_int("InputA", "KeyRightButton", SDL_SCANCODE_S);
    //config_input[0].key_blue = (SDL_Scancode)read_int("InputA", "KeyBlue", SDL_SCANCODE_D);
    //config_input[0].key_purple = (SDL_Scancode)read_int("InputA", "KeyPurple", SDL_SCANCODE_F);
    //config_input[0].key_0 = (SDL_Scancode)read_int("InputA", "Key0", SDL_SCANCODE_KP_0);
    //config_input[0].key_1 = (SDL_Scancode)read_int("InputA", "Key1", SDL_SCANCODE_KP_1);
    //config_input[0].key_2 = (SDL_Scancode)read_int("InputA", "Key2", SDL_SCANCODE_KP_2);
    //config_input[0].key_3 = (SDL_Scancode)read_int("InputA", "Key3", SDL_SCANCODE_KP_3);
    //config_input[0].key_4 = (SDL_Scancode)read_int("InputA", "Key4", SDL_SCANCODE_KP_4);
    //config_input[0].key_5 = (SDL_Scancode)read_int("InputA", "Key5", SDL_SCANCODE_KP_5);
    //config_input[0].key_6 = (SDL_Scancode)read_int("InputA", "Key6", SDL_SCANCODE_KP_6);
    //config_input[0].key_7 = (SDL_Scancode)read_int("InputA", "Key7", SDL_SCANCODE_KP_7);
    //config_input[0].key_8 = (SDL_Scancode)read_int("InputA", "Key8", SDL_SCANCODE_KP_8);
    //config_input[0].key_9 = (SDL_Scancode)read_int("InputA", "Key9", SDL_SCANCODE_KP_9);
    //config_input[0].key_asterisk = (SDL_Scancode)read_int("InputA", "KeyAsterisk", SDL_SCANCODE_KP_MULTIPLY);
    //config_input[0].key_hash = (SDL_Scancode)read_int("InputA", "KeyHash", SDL_SCANCODE_KP_DIVIDE);

    config_input[0].gamepad = read_bool("InputA", "Gamepad", true);
    config_input[0].gamepad_directional = read_int("InputA", "GamepadDirectional", 0);
    config_input[0].gamepad_invert_x_axis = read_bool("InputA", "GamepadInvertX", false);
    config_input[0].gamepad_invert_y_axis = read_bool("InputA", "GamepadInvertY", false);
    config_input[0].gamepad_left_button = read_int("InputA", "GamepadLeft", SDL_GAMEPAD_BUTTON_SOUTH);
    config_input[0].gamepad_right_button = read_int("InputA", "GamepadRight", SDL_GAMEPAD_BUTTON_EAST);
    //config_input[0].gamepad_blue = (SDL_Scancode)read_int("InputA", "GamepadBlue", SDL_CONTROLLER_BUTTON_GUIDE);
    //config_input[0].gamepad_purple = (SDL_Scancode)read_int("InputA", "GamepadPurple", SDL_CONTROLLER_BUTTON_GUIDE);
    config_input[0].gamepad_x_axis = read_int("InputA", "GamepadX", SDL_GAMEPAD_AXIS_LEFTX);
    config_input[0].gamepad_y_axis = read_int("InputA", "GamepadY", SDL_GAMEPAD_AXIS_LEFTY);
    //config_input[0].gamepad_1 = read_int("InputA", "Gamepad1", SDL_CONTROLLER_BUTTON_X);
    //config_input[0].gamepad_2 = read_int("InputA", "Gamepad2", SDL_CONTROLLER_BUTTON_Y);
    //config_input[0].gamepad_3 = read_int("InputA", "Gamepad3", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    //config_input[0].gamepad_4 = read_int("InputA", "Gamepad4", SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    //config_input[0].gamepad_5 = read_int("InputA", "Gamepad5", SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    //config_input[0].gamepad_6 = read_int("InputA", "Gamepad6", SDL_CONTROLLER_BUTTON_LEFTSTICK);
    //config_input[0].gamepad_7 = read_int("InputA", "Gamepad7", SDL_CONTROLLER_BUTTON_GUIDE);
    //config_input[0].gamepad_8 = read_int("InputA", "Gamepad8", SDL_CONTROLLER_BUTTON_GUIDE);
    //config_input[0].gamepad_9 = read_int("InputA", "Gamepad9", SDL_CONTROLLER_BUTTON_GUIDE);
    //config_input[0].gamepad_0 = read_int("InputA", "Gamepad0", SDL_CONTROLLER_BUTTON_GUIDE);
    //config_input[0].gamepad_asterisk = read_int("InputA", "GamepadAsterisk", SDL_CONTROLLER_BUTTON_START);
    //config_input[0].gamepad_hash = read_int("InputA", "GamepadHash", SDL_CONTROLLER_BUTTON_BACK);

    config_input[1].key_left = (SDL_Scancode)read_int("InputB", "KeyLeft", SDL_SCANCODE_J);
    config_input[1].key_right = (SDL_Scancode)read_int("InputB", "KeyRight", SDL_SCANCODE_L);
    config_input[1].key_up = (SDL_Scancode)read_int("InputB", "KeyUp", SDL_SCANCODE_I);
    config_input[1].key_down = (SDL_Scancode)read_int("InputB", "KeyDown", SDL_SCANCODE_K);
    config_input[1].key_left_button = (SDL_Scancode)read_int("InputB", "KeyLeftButton", SDL_SCANCODE_G);
    config_input[1].key_right_button = (SDL_Scancode)read_int("InputB", "KeyRightButton", SDL_SCANCODE_H);
    //config_input[1].key_blue = (SDL_Scancode)read_int("InputB", "KeyBlue", SDL_SCANCODE_J);
    //config_input[1].key_purple = (SDL_Scancode)read_int("InputB", "KeyPurple", SDL_SCANCODE_K);
    //config_input[1].key_0 = (SDL_Scancode)read_int("InputB", "Key0", SDL_SCANCODE_NONUSBACKSLASH);
    //config_input[1].key_1 = (SDL_Scancode)read_int("InputB", "Key1", SDL_SCANCODE_Z);
    //config_input[1].key_2 = (SDL_Scancode)read_int("InputB", "Key2", SDL_SCANCODE_X);
    //config_input[1].key_3 = (SDL_Scancode)read_int("InputB", "Key3", SDL_SCANCODE_C);
    //config_input[1].key_4 = (SDL_Scancode)read_int("InputB", "Key4", SDL_SCANCODE_V);
    //config_input[1].key_5 = (SDL_Scancode)read_int("InputB", "Key5", SDL_SCANCODE_B);
    //config_input[1].key_6 = (SDL_Scancode)read_int("InputB", "Key6", SDL_SCANCODE_N);
    //config_input[1].key_7 = (SDL_Scancode)read_int("InputB", "Key7", SDL_SCANCODE_M);
    //config_input[1].key_8 = (SDL_Scancode)read_int("InputB", "Key8", SDL_SCANCODE_COMMA);
    //config_input[1].key_9 = (SDL_Scancode)read_int("InputB", "Key9", SDL_SCANCODE_PERIOD);
    //config_input[1].key_asterisk = (SDL_Scancode)read_int("InputB", "KeyAsterisk", SDL_SCANCODE_SLASH);
    //config_input[1].key_hash = (SDL_Scancode)read_int("InputB", "KeyHash", SDL_SCANCODE_RSHIFT);

    config_input[1].gamepad = read_bool("InputB", "Gamepad", true);
    config_input[1].gamepad_directional = read_int("InputB", "GamepadDirectional", 0);
    config_input[1].gamepad_invert_x_axis = read_bool("InputB", "GamepadInvertX", false);
    config_input[1].gamepad_invert_y_axis = read_bool("InputB", "GamepadInvertY", false);
    config_input[1].gamepad_left_button = read_int("InputB", "GamepadLeft", SDL_GAMEPAD_BUTTON_SOUTH);
    config_input[1].gamepad_right_button = read_int("InputB", "GamepadRight", SDL_GAMEPAD_BUTTON_EAST);
    //config_input[1].gamepad_blue = (SDL_Scancode)read_int("InputB", "GamepadBlue", SDL_CONTROLLER_BUTTON_GUIDE);
    //config_input[1].gamepad_purple = (SDL_Scancode)read_int("InputB", "GamepadPurple", SDL_CONTROLLER_BUTTON_GUIDE);
    config_input[1].gamepad_x_axis = read_int("InputB", "GamepadX", SDL_GAMEPAD_AXIS_LEFTX);
    config_input[1].gamepad_y_axis = read_int("InputB", "GamepadY", SDL_GAMEPAD_AXIS_LEFTY);
    //config_input[1].gamepad_1 = read_int("InputB", "Gamepad1", SDL_CONTROLLER_BUTTON_X);
    //config_input[1].gamepad_2 = read_int("InputB", "Gamepad2", SDL_CONTROLLER_BUTTON_Y);
    //config_input[1].gamepad_3 = read_int("InputB", "Gamepad3", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    //config_input[1].gamepad_4 = read_int("InputB", "Gamepad4", SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    //config_input[1].gamepad_5 = read_int("InputB", "Gamepad5", SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    //config_input[1].gamepad_6 = read_int("InputB", "Gamepad6", SDL_CONTROLLER_BUTTON_LEFTSTICK);
    //config_input[1].gamepad_7 = read_int("InputB", "Gamepad7", SDL_CONTROLLER_BUTTON_GUIDE);
    //config_input[1].gamepad_8 = read_int("InputB", "Gamepad8", SDL_CONTROLLER_BUTTON_GUIDE);
    //config_input[1].gamepad_9 = read_int("InputB", "Gamepad9", SDL_CONTROLLER_BUTTON_GUIDE);
    //config_input[1].gamepad_0 = read_int("InputB", "Gamepad0", SDL_CONTROLLER_BUTTON_GUIDE);
    //config_input[1].gamepad_asterisk = read_int("InputB", "GamepadAsterisk", SDL_CONTROLLER_BUTTON_START);
    //config_input[1].gamepad_hash = read_int("InputB", "GamepadHash", SDL_CONTROLLER_BUTTON_BACK);

    // The SK-1100 is one physical keyboard, not one per joystick. Store it in
    // InputA for ABI compatibility with config_Input while giving it its own
    // INI section. InputB mirrors the values so either profile can be copied
    // without carrying stale data.
    for (int key = 0; key < SK1100_KEYBOARD_KEY_COUNT; ++key)
    {
        config_input[0].sk1100_key[key] = (SDL_Scancode)read_int(
            "Machine.SK1100", sk1100_keyboard_layout[key].id,
            sk1100_keyboard_layout[key].default_scancode);
        config_input[1].sk1100_key[key] = config_input[0].sk1100_key[key];
    }

    for (int player = 0; player < 2; ++player)
        for (int key = 0; key < COLECO_KEYPAD_KEY_COUNT; ++key)
        {
            const std::string item = std::string(player == 0 ? "KeypadA_" : "KeypadB_")
                + coleco_keypad_layout[key].id;
            config_input[player].keypad_key[key] = (SDL_Scancode)read_int(
                "Machine.Coleco", item.c_str(),
                coleco_keypad_layout[key].default_scancode[player]);
        }

    for (int player = 0; player < 2; ++player)
    {
        const std::string pad = player == 0 ? "PadA" : "PadB";
        const int device = read_int("Machine.SMS", pad.c_str(), 0);
        config_input[player].controller_device = IsValidControllerDevice(device) ? device : 0;
        for (int button = 0; button < MD_PAD_BUTTON_COUNT; ++button)
        {
            const std::string id = md_pad_layout[button].id;
            config_input[player].md_key[button] = (SDL_Scancode)read_int(
                "Machine.SMS", (pad + "_Key" + id).c_str(), SDL_SCANCODE_UNKNOWN);
            config_input[player].gamepad_md_button[button] = read_int(
                "Machine.SMS", (pad + "_Gamepad" + id).c_str(), -1);
        }
    }
    config_input[0].gg_start_key = static_cast<SDL_Scancode>(read_int(
        "Machine.GameGear", "KeyStart", SDL_SCANCODE_RETURN));
    config_input[0].gamepad_gg_start_button = read_int(
        "Machine.GameGear", "GamepadStart", SDL_GAMEPAD_BUTTON_START);

    for (int hotkey = 0; hotkey < config_HotkeyIndex_COUNT; ++hotkey)
        config_hotkeys[hotkey] = read_hotkey(
            config_hotkey_name((config_HotkeyIndex)hotkey),
            config_default_hotkey((config_HotkeyIndex)hotkey));

#if defined(__APPLE__)
    // F12 used to be the macOS default for the physical SC-3000 BREAK key.
    // It is now the frontend Fullscreen default, and service hotkeys have
    // priority by design. Migrate only that exact historical default; later
    // user choices remain untouched and are handled by the conflict UI.
    for (int key = 0; key < SK1100_KEYBOARD_KEY_COUNT; ++key)
    {
        if (strcmp(sk1100_keyboard_layout[key].id, "break") == 0 &&
            config_input[0].sk1100_key[key] == SDL_SCANCODE_F12)
        {
            config_input[0].sk1100_key[key] = SDL_SCANCODE_F3;
            config_input[1].sk1100_key[key] = SDL_SCANCODE_F3;
            break;
        }
    }
#endif

    Log("Settings loaded");
}

config_SaveResult config_write(void)
{
    Log("Saving settings to %s", config_emu_file_path);
    write_int("Config", "Version", config_format_version);

#if GEARSF7000_ENABLE_DEBUG_TOOLS
    write_bool("Debug", "Debug", config_debug.debug);
#endif
    write_bool("Debug", "Disassembler", config_debug.show_disassembler);
    write_bool("Debug", "Screen", config_debug.show_screen);
    write_bool("Debug", "Memory", config_debug.show_memory);
    write_bool("Debug", "Processor", config_debug.show_processor);
    write_bool("Debug", "PatternTable", config_debug.show_pattern_table);
    write_bool("Debug", "Sprites", config_debug.show_sprites);
    write_bool("Debug", "NameTable", config_debug.show_nametable);
    write_bool("Debug", "VideoRegisterExplorer", config_debug.show_video_register_explorer);
    write_bool("Debug", "PSG", config_debug.show_psg);
#if GEARSF7000_ENABLE_YM2413
    write_bool("Debug", "YM2413", config_debug.show_ym2413);
#endif
    write_bool("Debug", "PPI8255A", config_debug.show_ppi_registers);
    write_bool("Debug", "SF7000", config_debug.show_sf7000);
    write_bool("Debug", "Events", config_debug.show_events);
    write_bool("Debug", "RomMap", config_debug.show_rom_map);
    write_bool("Debug", "DisassemblerNew", config_debug.show_disassembler_new);
    write_bool("Debug", "DeepSearch", config_debug.show_deep_search);
    write_bool("Debug", "ToCheck", config_debug.show_to_check);
    write_bool("Debug", "Palettes", config_debug.show_palettes);
    write_bool("Debug", "RomMapEnabled", config_debug.rom_map_enabled);
    write_string("Debug", "ProjectsDir", config_debug.projects_dir);
    write_int("Debug", "EventsCategoryFilter", config_debug.events_category_filter);
    write_bool("Debug", "EventsNewestFirst", config_debug.events_newest_first);
    write_bool("Debug", "EventsGroupByPc", config_debug.events_group_by_pc);
    write_bool("Debug", "EventsNormalizeClock", config_debug.events_normalize_clock);
    write_bool("Debug", "MemoryImport", config_debug.show_memory_import);
    write_bool("Debug", "RomInspector", config_debug.show_rom_inspector);
    write_bool("Debug", "DiscExplorer", config_debug.show_disc_explorer);
    write_float("Debug", "VramBackgroundAreaScale", config_debug.vram_background_area_scale);
    write_float("Debug", "VramTilesAreaScale", config_debug.vram_tiles_area_scale);
    write_float("Debug", "VramSpritesAreaScale", config_debug.vram_sprites_area_scale);
    write_float("Debug", "VramSpritesPreviewAreaScale", config_debug.vram_sprites_preview_area_scale);
    write_string("Debug", "VramSpritesSourceCode",
        config_debug.vram_sprites_source_code);
    write_int("Debug", "VramSpritesScanline", config_debug.vram_sprites_scanline);
    write_string("Debug", "VramSpritesHiddenPreviewParts",
        config_debug.vram_sprites_hidden_preview_parts);
    write_string("Debug", "VramSpritesDisabledPreviewOptions",
        config_debug.vram_sprites_disabled_preview_options);
    write_string("Debug", "VramPatternDisabledLayerOptions",
        config_debug.vram_pattern_disabled_layer_options);
    write_bool("Debug", "GrayInactiveSpriteAttributes",
        config_debug.gray_inactive_sprite_attributes);
    write_bool("Debug", "ShowGridBackground", config_debug.show_grid_background);
    write_bool("Debug", "ShowViewportBackground", config_debug.show_viewport_background);
    write_bool("Debug", "ShowRegionsBackground", config_debug.show_regions_background);
    write_bool("Debug", "ShowGridTiles", config_debug.show_grid_tiles);
    write_bool("Debug", "CrtTestPass", config_debug.show_crt_test_pass);
    write_bool("Debug", "CompositeDecodeTest", config_debug.show_composite_decode_test);
#if GEARSF7000_ENABLE_MCP
    write_bool("Debug", "ShowMCPServer", config_debug.show_mcp_server);
    write_bool("Debug", "ShowWatchMonitor", config_debug.show_watch_monitor);
#endif
    write_bool("Debug", "ShowBasicTyper", config_debug.show_basic_typer);
    write_bool("Debug", "BasicTyperDecodeEscapes", config_debug.basic_typer_decode_escapes);
    write_int("Debug", "BasicTyperSpeed", config_debug.basic_typer_speed);
    write_int("Debug", "BasicPointerBlock", config_debug.basic_pointer_block);
    write_bool("Debug", "DisFollowPC", config_debug.dis_follow_pc);
    write_bool("Debug", "DisOpcodes", config_debug.dis_show_opcodes);
    write_bool("Debug", "DisSymbols", config_debug.dis_show_symbols);
    write_bool("Debug", "DisSegments", config_debug.dis_show_segments);
    write_bool("Debug", "DisBank", config_debug.dis_show_bank);
    write_int("Debug", "FontSize", config_debug.font_size);
    write_int("Debug", "UiDensity", config_debug.ui_density);
    write_float("Debug", "UiScale", config_debug.ui_scale);
    write_int("Debug", "MemoryDataFont", config_debug.memory_data_font);
    write_bool("Debug", "MultiViewport", config_debug.multi_viewport);
    write_bool("Debug", "Cassette", config_debug.show_cassette);
    write_bool("Debug", "Rewind", config_debug.show_rewind);
#if GEARSF7000_ENABLE_RECORDER
    write_bool("Debug", "RecorderEnabled", config_debug.rewind_enabled);
#endif
    write_int("Debug", "LogLevel", config_debug.log_level);
    write_int("Debug", "LogMaxMB", config_debug.log_max_mb);
    write_int("Debug", "RewindSeconds", config_debug.rewind_seconds);

    write_bool("Emulator", "FullScreen", config_emulator.fullscreen);
    write_bool("Emulator", "ShowMenu", config_emulator.show_menu);
    write_bool("Machine.SK1100", "KeyboardMode", config_emulator.keyboard_mode);
    write_int("Emulator", "FFWD", config_emulator.ffwd_speed);
    write_int("Emulator", "SaveSlot", config_emulator.save_slot);
    write_bool("Emulator", "StartPaused", config_emulator.start_paused);
    write_int("Emulator", "Machine", config_emulator.machine);
    write_int("Emulator", "LastSegaMachine", config_emulator.last_sega_machine);
    write_bool("Machine.Coleco", "SGM", config_emulator.coleco_sgm);
    config_emulator.vdp_by_model[config_emulator.machine] = config_emulator.vdp;
    config_emulator.last_board_by_machine[GetMachineMenuGroup(static_cast<SystemModel>(config_emulator.machine)).code] = config_emulator.machine;
    for (const auto& group : GetMachineMenuGroups()) {
        const auto saved = config_emulator.last_board_by_machine.find(group.code);
        write_int("Emulator", (std::string("Board.") + group.code).c_str(),
            saved == config_emulator.last_board_by_machine.end() ? static_cast<int>(group.defaultModel) : saved->second);
    }
    config_emulator.video_option_by_model[config_emulator.machine] = config_selected_video_option();
    config_emulator.last_model_by_family[config_mapper_family(static_cast<SystemModel>(config_emulator.machine))] = config_emulator.machine;
    for (const auto& p : config_profiles::profiles) {
        const int index = static_cast<int>(p.model);
        const int legacy = config_emulator.vdp_by_model[index];
        const auto code = config_resolve_video_option(legacy, config_emulator.video_option_by_model[index]);
        write_int(p.section, p.key, legacy);
        write_string(p.section, (std::string(p.key) + ".Option").c_str(), code.empty() ? "default" : code);
    }
    for (int f = 0; f < config_MapperFamily_COUNT; ++f)
        write_int(config_profiles::familySection(static_cast<SystemModel>(config_emulator.last_model_by_family[f])),
                  "LastModel", config_emulator.last_model_by_family[f]);
    config_mapper_store();
    config_ini_data["Emulator"].remove("MapperMode");
    config_ini_data["Emulator"].remove("MapperType");
    {
        static const char* const kFamilySections[config_MapperFamily_COUNT] = {
            "Machine.SG1000", "Machine.SMS", "Machine.Coleco", "Machine.MSX", "Machine.SordM5", "Machine.GameGear"};
        for (int f = 0; f < config_MapperFamily_COUNT; f++)
        {
            write_int(kFamilySections[f], "MapperMode", config_emulator.mapper_mode_by_family[f]);
            write_int(kFamilySections[f], "MapperType", config_emulator.mapper_type_by_family[f]);
        }
    }
#if GEARSF7000_ENABLE_MCP
    write_int("Emulator", "MCPTCPPort", config_emulator.mcp_tcp_port);
    write_string("Emulator", "MCPHTTPAddress", config_emulator.mcp_http_address);
#endif


    const auto writeTape = [](const char* section, const config_Tape& tape) {
        write_string(section, "CassettePath", tape.path);
        write_bool(section, "CassetteWriteProtected", tape.write_protected);
        for (int i = 0; i < config_max_recent_cassettes; ++i) {
            const std::string key = "RecentCassette" + std::to_string(i);
            write_string(section, key.c_str(), tape.recent[i]);
        }
    };
    writeTape("Machine.SR1000", config_emulator.sr1000_tape);
    writeTape("Machine.MSXTape", config_emulator.msx_tape);

    write_string("Machine.SF7000", "BiosPath", config_emulator.bios_path);
    write_string("Machine.Coleco", "BiosPath", config_emulator.coleco_bios_path);
    write_string("Machine.SordM5", "BiosPath", config_emulator.m5_bios_path);
    write_bool("Machine.SordM5", "EM5Installed", config_emulator.m5_em5_installed);
    write_string("Machine.MSX", "BiosPath", config_emulator.msx_bios_path);
    write_bool("Machine.MSX", "UseCBios", config_emulator.msx_use_cbios);
    write_string("Machine.SF7000", "DiscPath", config_emulator.disc_path);
    write_bool("Machine.SF7000", "DiscWriteProtected", config_emulator.disc_write_protected);

    for (int i = 0; i < config_max_recent_discs; i++)
    {
        std::string item = "RecentDISC" + std::to_string(i);
        write_string("Machine.SF7000", item.c_str(), config_emulator.recent_discs[i]);
    }

    write_int("Emulator", "SaveFilesDirOption", config_emulator.savefiles_dir_option);
    write_string("Emulator", "SaveFilesPath", config_emulator.savefiles_path);
    write_int("Emulator", "SaveStatesDirOption", config_emulator.savestates_dir_option);
    write_string("Emulator", "SaveStatesPath", config_emulator.savestates_path);
    write_string("Emulator", "LastOpenPath", config_emulator.last_open_path);
    write_int("Emulator", "WindowWidth", config_emulator.window_width);
    write_int("Emulator", "WindowHeight", config_emulator.window_height);
    //write_int("Emulator", "Spinner", config_emulator.spinner);
    //write_int("Emulator", "SpinnerSensitivity", config_emulator.spinner_sensitivity);
    write_bool("Emulator", "StatusMessages", config_emulator.status_messages);

    for (int i = 0; i < config_max_recent_roms; i++)
    {
        std::string item = "RecentROM" + std::to_string(i);
        write_string("Emulator", item.c_str(), config_emulator.recent_roms[i]);
    }

    write_int("Video", "Scale", config_video.scale);
    write_int("Video", "AspectRatio", config_video.ratio);
    write_int("Video", "Overscan", config_video.overscan);
    write_bool("Video", "ShowFPS", config_video.fps);
    write_bool("Video", "ShowFramePacingInfo", config_video.frame_pacing_info);
    write_int("Video", "Postprocessing", config_video.postprocessing);
    write_float("Video", "CrtLottesMaskType", config_video.crt_lottes_mask_type);
    write_float("Video", "CrtLottesMaskIntensity", config_video.crt_lottes_mask_intensity);
    write_float("Video", "CrtLottesScanlineThinness", config_video.crt_lottes_scanline_thinness);
    write_float("Video", "CrtLottesScanBlur", config_video.crt_lottes_scan_blur);
    write_float("Video", "CrtLottesGamma", config_video.crt_lottes_gamma);
    write_float("Video", "CrtLottesBlackThreshold", config_video.crt_lottes_black_threshold);
    write_float("Video", "CrtLottesBoldness", config_video.crt_lottes_boldness);
    write_float("Video", "CrtLottesEdgeRadius", config_video.crt_lottes_edge_radius);
    write_float("Video", "AdvancedScalingSharpness", config_video.advanced_scaling_sharpness);
    write_float("Video", "AdvancedScalingBlackThreshold", config_video.advanced_scaling_black_threshold);
    write_float("Video", "AdvancedScalingBoldness", config_video.advanced_scaling_boldness);
    write_float("Video", "AdvancedScalingEdgeRadius", config_video.advanced_scaling_edge_radius);
#ifdef SPRITE_EXPANDER
    write_bool("Video", "SpriteLimit", config_video.sprite_limit);
#endif
    write_bool("Video", "Scanlines", config_video.scanlines);
    write_float("Video", "ScanlinesIntensity", config_video.scanlines_intensity);

#if GEARSF7000_ENABLE_DEBUG_TOOLS
    write_bool("DebugVideo", "AsMainVideoSettings", config_debug.video_as_main_settings);
    write_int("DebugVideo", "Scale", config_debug.video.scale);
    write_int("DebugVideo", "AspectRatio", config_debug.video.ratio);
    write_int("DebugVideo", "Overscan", config_debug.video.overscan);
    write_int("DebugVideo", "Postprocessing", config_debug.video.postprocessing);
    write_float("DebugVideo", "CrtLottesMaskType", config_debug.video.crt_lottes_mask_type);
    write_float("DebugVideo", "CrtLottesMaskIntensity", config_debug.video.crt_lottes_mask_intensity);
    write_float("DebugVideo", "CrtLottesScanlineThinness", config_debug.video.crt_lottes_scanline_thinness);
    write_float("DebugVideo", "CrtLottesScanBlur", config_debug.video.crt_lottes_scan_blur);
    write_float("DebugVideo", "CrtLottesGamma", config_debug.video.crt_lottes_gamma);
    write_float("DebugVideo", "CrtLottesBlackThreshold", config_debug.video.crt_lottes_black_threshold);
    write_float("DebugVideo", "CrtLottesBoldness", config_debug.video.crt_lottes_boldness);
    write_float("DebugVideo", "CrtLottesEdgeRadius", config_debug.video.crt_lottes_edge_radius);
    write_float("DebugVideo", "AdvancedScalingSharpness", config_debug.video.advanced_scaling_sharpness);
    write_float("DebugVideo", "AdvancedScalingBlackThreshold", config_debug.video.advanced_scaling_black_threshold);
    write_float("DebugVideo", "AdvancedScalingBoldness", config_debug.video.advanced_scaling_boldness);
    write_float("DebugVideo", "AdvancedScalingEdgeRadius", config_debug.video.advanced_scaling_edge_radius);
#endif
    write_int("Video", "Palette", config_video.palette);
    write_int("Video", "SmsTmsPalette", config_video.sms_tms_palette);
    write_string("Video", "Pacing", config_video_pacing_name());

    write_bool("Audio", "Enable", config_audio.enable);
#if GEARSF7000_ENABLE_AY
    write_bool("Machine.SegaAudio", "AYEnable", config_audio.ay_enable);
    write_int("Machine.SegaAudio", "AYChip", config_audio.ay_chip);
    write_int("Machine.SegaAudio", "AYPortBase", config_audio.ay_port_base);
#endif
#if GEARSF7000_ENABLE_YM2413
    write_bool("Machine.SMS", "YM2413Enable", config_audio.ym2413_enable);
#endif

    write_int("InputA", "KeyLeft", config_input[0].key_left);
    write_int("InputA", "KeyRight", config_input[0].key_right);
    write_int("InputA", "KeyUp", config_input[0].key_up);
    write_int("InputA", "KeyDown", config_input[0].key_down);
    write_int("InputA", "KeyLeftButton", config_input[0].key_left_button);
    write_int("InputA", "KeyRightButton", config_input[0].key_right_button);
    //write_int("InputA", "KeyBlue", config_input[0].key_blue);
    //write_int("InputA", "KeyPurple", config_input[0].key_purple);
    //write_int("InputA", "Key0", config_input[0].key_0);
    //write_int("InputA", "Key1", config_input[0].key_1);
    //write_int("InputA", "Key2", config_input[0].key_2);
    //write_int("InputA", "Key3", config_input[0].key_3);
    //write_int("InputA", "Key4", config_input[0].key_4);
    //write_int("InputA", "Key5", config_input[0].key_5);
    //write_int("InputA", "Key6", config_input[0].key_6);
    //write_int("InputA", "Key7", config_input[0].key_7);
    //write_int("InputA", "Key8", config_input[0].key_8);
    //write_int("InputA", "Key9", config_input[0].key_9);
    //write_int("InputA", "KeyAsterisk", config_input[0].key_asterisk);
    //write_int("InputA", "KeyHash", config_input[0].key_hash);

    write_bool("InputA", "Gamepad", config_input[0].gamepad);
    write_int("InputA", "GamepadDirectional", config_input[0].gamepad_directional);
    write_bool("InputA", "GamepadInvertX", config_input[0].gamepad_invert_x_axis);
    write_bool("InputA", "GamepadInvertY", config_input[0].gamepad_invert_y_axis);
    write_int("InputA", "GamepadLeft", config_input[0].gamepad_left_button);
    write_int("InputA", "GamepadRight", config_input[0].gamepad_right_button);
    //write_int("InputA", "GamepadBlue", config_input[0].gamepad_blue);
    //write_int("InputA", "GamepadPurple", config_input[0].gamepad_purple);
    write_int("InputA", "GamepadX", config_input[0].gamepad_x_axis);
    write_int("InputA", "GamepadY", config_input[0].gamepad_y_axis);
    //write_int("InputA", "Gamepad1", config_input[0].gamepad_1);
    //write_int("InputA", "Gamepad2", config_input[0].gamepad_2);
    //write_int("InputA", "Gamepad3", config_input[0].gamepad_3);
    //write_int("InputA", "Gamepad4", config_input[0].gamepad_4);
    //write_int("InputA", "Gamepad5", config_input[0].gamepad_5);
    //write_int("InputA", "Gamepad6", config_input[0].gamepad_6);
    //write_int("InputA", "Gamepad7", config_input[0].gamepad_7);
    //write_int("InputA", "Gamepad8", config_input[0].gamepad_8);
    //write_int("InputA", "Gamepad9", config_input[0].gamepad_9);
    //write_int("InputA", "Gamepad0", config_input[0].gamepad_0);
    //write_int("InputA", "GamepadAsterisk", config_input[0].gamepad_asterisk);
    //write_int("InputA", "GamepadHash", config_input[0].gamepad_hash);

    write_int("InputB", "KeyLeft", config_input[1].key_left);
    write_int("InputB", "KeyRight", config_input[1].key_right);
    write_int("InputB", "KeyUp", config_input[1].key_up);
    write_int("InputB", "KeyDown", config_input[1].key_down);
    write_int("InputB", "KeyLeftButton", config_input[1].key_left_button);
    write_int("InputB", "KeyRightButton", config_input[1].key_right_button);
    //write_int("InputB", "KeyBlue", config_input[1].key_blue);
    //write_int("InputB", "KeyPurple", config_input[1].key_purple);
    //write_int("InputB", "Key0", config_input[1].key_0);
    //write_int("InputB", "Key1", config_input[1].key_1);
    //write_int("InputB", "Key2", config_input[1].key_2);
    //write_int("InputB", "Key3", config_input[1].key_3);
    //write_int("InputB", "Key4", config_input[1].key_4);
    //write_int("InputB", "Key5", config_input[1].key_5);
    //write_int("InputB", "Key6", config_input[1].key_6);
    //write_int("InputB", "Key7", config_input[1].key_7);
    //write_int("InputB", "Key8", config_input[1].key_8);
    //write_int("InputB", "Key9", config_input[1].key_9);
    //write_int("InputB", "KeyAsterisk", config_input[1].key_asterisk);
    //write_int("InputB", "KeyHash", config_input[1].key_hash);

    write_bool("InputB", "Gamepad", config_input[1].gamepad);
    write_int("InputB", "GamepadDirectional", config_input[1].gamepad_directional);
    write_bool("InputB", "GamepadInvertX", config_input[1].gamepad_invert_x_axis);
    write_bool("InputB", "GamepadInvertY", config_input[1].gamepad_invert_y_axis);
    write_int("InputB", "GamepadLeft", config_input[1].gamepad_left_button);
    write_int("InputB", "GamepadRight", config_input[1].gamepad_right_button);
    //write_int("InputB", "GamepadBlue", config_input[1].gamepad_blue);
    //write_int("InputB", "GamepadPurple", config_input[1].gamepad_purple);
    write_int("InputB", "GamepadX", config_input[1].gamepad_x_axis);
    write_int("InputB", "GamepadY", config_input[1].gamepad_y_axis);
    //write_int("InputB", "Gamepad1", config_input[1].gamepad_1);
    //write_int("InputB", "Gamepad2", config_input[1].gamepad_2);
    //write_int("InputB", "Gamepad3", config_input[1].gamepad_3);
    //write_int("InputB", "Gamepad4", config_input[1].gamepad_4);
    //write_int("InputB", "Gamepad5", config_input[1].gamepad_5);
    //write_int("InputB", "Gamepad6", config_input[1].gamepad_6);
    //write_int("InputB", "Gamepad7", config_input[1].gamepad_7);
    //write_int("InputB", "Gamepad8", config_input[1].gamepad_8);
    //write_int("InputB", "Gamepad9", config_input[1].gamepad_9);
    //write_int("InputB", "Gamepad0", config_input[1].gamepad_0);
    //write_int("InputB", "GamepadAsterisk", config_input[1].gamepad_asterisk);
    //write_int("InputB", "GamepadHash", config_input[1].gamepad_hash);

    for (int key = 0; key < SK1100_KEYBOARD_KEY_COUNT; ++key)
        write_int("Machine.SK1100", sk1100_keyboard_layout[key].id,
                  config_input[0].sk1100_key[key]);

    for (int player = 0; player < 2; ++player)
        for (int key = 0; key < COLECO_KEYPAD_KEY_COUNT; ++key)
        {
            const std::string item = std::string(player == 0 ? "KeypadA_" : "KeypadB_")
                + coleco_keypad_layout[key].id;
            write_int("Machine.Coleco", item.c_str(), config_input[player].keypad_key[key]);
        }

    for (int player = 0; player < 2; ++player)
    {
        const std::string pad = player == 0 ? "PadA" : "PadB";
        write_int("Machine.SMS", pad.c_str(), config_input[player].controller_device);
        for (int button = 0; button < MD_PAD_BUTTON_COUNT; ++button)
        {
            const std::string id = md_pad_layout[button].id;
            write_int("Machine.SMS", (pad + "_Key" + id).c_str(),
                      config_input[player].md_key[button]);
            write_int("Machine.SMS", (pad + "_Gamepad" + id).c_str(),
                      config_input[player].gamepad_md_button[button]);
        }
    }
    write_int("Machine.GameGear", "KeyStart", config_input[0].gg_start_key);
    write_int("Machine.GameGear", "GamepadStart",
              config_input[0].gamepad_gg_start_button);

    for (int hotkey = 0; hotkey < config_HotkeyIndex_COUNT; ++hotkey)
        write_hotkey(config_hotkey_name((config_HotkeyIndex)hotkey),
                     config_hotkeys[hotkey]);

    const bool saved = config_repository->save(config_ini_data);
    config_storage_error = config_repository->error;
    if (saved) DiagInfo("config", "Settings saved (format 3)");
    else DiagError("config", "Settings not saved: %s", config_storage_error.c_str());
    return {saved, config_repository->writable(), config_repository->file, config_repository->operation, config_storage_error};
}

static void build_config_path(char* destination, size_t destination_size, const char* filename)
{
    if (!destination || destination_size == 0)
        return;

    const int written = SDL_snprintf(destination, destination_size, "%s%s",
        config_root_path ? config_root_path : "./", filename);
    if (written < 0 || static_cast<size_t>(written) >= destination_size)
    {
        destination[0] = '\0';
        Log("Settings path is too long for %s", filename);
    }
}



static bool check_portable(void)
{
    const char* base_path = SDL_GetBasePath();
    if (!base_path)
        return false;

    char portable_file_path[4096];
    const int written = SDL_snprintf(portable_file_path, sizeof(portable_file_path), "%sportable.ini", base_path);

    if (written < 0 || static_cast<size_t>(written) >= sizeof(portable_file_path))
        return false;

    FILE* file = fopen(portable_file_path, "r");
    if (!IsValidPointer(file))
        return false;

    fclose(file);
    return true;
}

static int read_int(const char* group, const char* key, int default_value)
{
    int ret = default_value;

    std::string value = config_ini_data.get(group).get(key);

    if (!value.empty())
    {
        try
        {
        size_t end = 0;
        ret = std::stoi(value, &end);
        if (end != value.size()) ret = default_value;
        }
        catch (...)
        {
            Log("Invalid integer setting: [%s][%s]=%s; using default", group, key, value.c_str());
        }
    }

    Log("Load setting: [%s][%s]=%d", group, key, ret);
    return ret;
}

static void write_int(const char* group, const char* key, int integer)
{
    std::string value = std::to_string(integer);
    config_ini_data[group][key] = value;
    Log("Save setting: [%s][%s]=%s", group, key, value.c_str());
}

static config_Hotkey make_hotkey(SDL_Scancode key, SDL_Keymod mod)
{
    config_Hotkey result;
    result.key = key;
    result.mod = mod;
    config_update_hotkey_string(&result);
    return result;
}

static config_Hotkey read_hotkey(const char* key, config_Hotkey default_value)
{
    const std::string scancode_key = std::string(key) + "Scancode";
    const std::string mod_key = std::string(key) + "Mod";
    config_Hotkey result = default_value;
    result.key = (SDL_Scancode)read_int("Hotkeys", scancode_key.c_str(), default_value.key);
    result.mod = (SDL_Keymod)read_int("Hotkeys", mod_key.c_str(), default_value.mod);
    config_update_hotkey_string(&result);
    return result;
}

static void write_hotkey(const char* key, const config_Hotkey& hotkey)
{
    const std::string scancode_key = std::string(key) + "Scancode";
    const std::string mod_key = std::string(key) + "Mod";
    write_int("Hotkeys", scancode_key.c_str(), hotkey.key);
    write_int("Hotkeys", mod_key.c_str(), hotkey.mod);
}

const char* config_hotkey_name(config_HotkeyIndex index)
{
    static const char* names[config_HotkeyIndex_COUNT] = {
        "OpenROM", "ReloadROM", "Reset", "Pause", "FFWD", "SaveState",
        "LoadState", "Screenshot", "Fullscreen", "ShowMainMenu",
        "DebugStepInto", "DebugStepOver", "DebugStepLine", "DebugStepFrame", "DebugStepBack",
        "DebugContinue", "DebugContinueFromHere", "DebugBreak", "DebugRunToCursor",
        "DebugBreakpoint", "DebugGoBack", "DebugCopy", "DebugPaste"
    };
    return index >= 0 && index < config_HotkeyIndex_COUNT ? names[index] : "Unknown";
}

const char* config_hotkey_label(config_HotkeyIndex index)
{
    static const char* labels[config_HotkeyIndex_COUNT] = {
        "Open ROM", "Reload current ROM", "Reset machine", "Pause / Resume",
        "Fast forward", "Save state", "Load state", "Screenshot", "Fullscreen",
        "Show main menu", "Step into", "Step over", "Step line", "Next frame",
        "Previous recorded frame", "Continue live", "Continue from here",
        "Break", "Run to cursor",
        "Toggle breakpoint", "Go back", "Copy", "Paste"
    };
    return index >= 0 && index < config_HotkeyIndex_COUNT ? labels[index] : "Unknown";
}

config_Hotkey config_default_hotkey(config_HotkeyIndex index)
{
    static const SDL_Scancode keys[config_HotkeyIndex_COUNT] = {
        SDL_SCANCODE_O, SDL_SCANCODE_D, SDL_SCANCODE_R, SDL_SCANCODE_P,
        SDL_SCANCODE_F, SDL_SCANCODE_S, SDL_SCANCODE_L, SDL_SCANCODE_X,
        SDL_SCANCODE_F12, SDL_SCANCODE_M, SDL_SCANCODE_F11, SDL_SCANCODE_F10,
        SDL_SCANCODE_F6, SDL_SCANCODE_F6, SDL_SCANCODE_F6, SDL_SCANCODE_F5, SDL_SCANCODE_F5,
        SDL_SCANCODE_F7,
        SDL_SCANCODE_F8, SDL_SCANCODE_F9, SDL_SCANCODE_BACKSPACE,
        SDL_SCANCODE_C, SDL_SCANCODE_V
    };
    static const SDL_Keymod mods[config_HotkeyIndex_COUNT] = {
        SDL_KMOD_CTRL, SDL_KMOD_CTRL, SDL_KMOD_CTRL, SDL_KMOD_CTRL,
        SDL_KMOD_CTRL, SDL_KMOD_CTRL, SDL_KMOD_CTRL, SDL_KMOD_CTRL,
        SDL_KMOD_NONE, SDL_KMOD_CTRL, SDL_KMOD_NONE, SDL_KMOD_NONE,
        SDL_KMOD_CTRL, SDL_KMOD_NONE, SDL_KMOD_SHIFT, SDL_KMOD_NONE, SDL_KMOD_CTRL,
        SDL_KMOD_NONE,
        SDL_KMOD_NONE, SDL_KMOD_NONE, SDL_KMOD_CTRL,
        SDL_KMOD_CTRL, SDL_KMOD_CTRL
    };
    if (index < 0 || index >= config_HotkeyIndex_COUNT)
        return make_hotkey(SDL_SCANCODE_UNKNOWN, SDL_KMOD_NONE);
    return make_hotkey(keys[index], mods[index]);
}

void config_update_hotkey_string(config_Hotkey* hotkey)
{
    if (!hotkey)
        return;
    if (hotkey->key == SDL_SCANCODE_UNKNOWN)
    {
        SDL_snprintf(hotkey->str, sizeof(hotkey->str), "Unassigned");
        return;
    }
    std::string value;
    if (hotkey->mod & SDL_KMOD_CTRL) value += "Ctrl+";
    if (hotkey->mod & SDL_KMOD_SHIFT) value += "Shift+";
    if (hotkey->mod & SDL_KMOD_ALT) value += "Alt+";
    if (hotkey->mod & SDL_KMOD_GUI) value += "Cmd+";
    const char* name = SDL_GetScancodeName(hotkey->key);
    value += name && name[0] ? name : "Unknown";
    SDL_snprintf(hotkey->str, sizeof(hotkey->str), "%s", value.c_str());
}

static float read_float(const char* group, const char* key, float default_value)
{
    float ret = 0.0f;

    std::string value = config_ini_data.get(group).get(key);

    if(value.empty())
        ret = default_value;
    else
        ret = strtof(value.c_str(), NULL);

    Log("Load setting: [%s][%s]=%.2f", group, key, ret);
    return ret;
}

static void write_float(const char* group, const char* key, float value)
{
    std::string value_str = std::to_string(value);
    config_ini_data[group][key] = value_str;
    Log("Save setting: [%s][%s]=%s", group, key, value_str.c_str());
}

static bool read_bool(const char* group, const char* key, bool default_value)
{
    bool ret = default_value;

    std::string value = config_ini_data.get(group).get(key);

    if (!value.empty())
    {
        if (value == "true") ret = true;
        else if (value == "false") ret = false;
        else Log("Invalid boolean setting: [%s][%s]=%s; using default", group, key, value.c_str());
    }

    Log("Load setting: [%s][%s]=%s", group, key, ret ? "true" : "false");
    return ret;
}

static void write_bool(const char* group, const char* key, bool boolean)
{
    std::stringstream converter;
    converter << std::boolalpha << boolean;
    std::string value;
    value = converter.str();
    config_ini_data[group][key] = value;
    Log("Save setting: [%s][%s]=%s", group, key, value.c_str());
}

static std::string read_string(const char* group, const char* key)
{
    std::string ret = config_ini_data.get(group).get(key);
    Log("Load setting: [%s][%s]=%s", group, key, ret.c_str());
    return ret;
}

static void write_string(const char* group, const char* key, std::string value)
{
    config_ini_data[group][key] = value;
    Log("Save setting: [%s][%s]=%s", group, key, value.c_str());
}

bool config_video_vsync(void)
{
    return (config_video.pacing & PACING_VSYNC_BIT) != 0;
}

bool config_video_pacing_aligned(void)
{
    return (config_video.pacing & PACING_ALIGN_BIT) != 0;
}

const char* config_video_pacing_name(void)
{
    switch (config_video.pacing)
    {
        case PACING_ACCURATE:       return "accurate";
        case PACING_ACCURATE_VSYNC: return "accurate_vsync";
        case PACING_GAMING:         return "gaming";
        default:                    return "gaming_vsync";
    }
}

int config_video_pacing_from_name(const char* name, int fallback)
{
    if (name == NULL)
        return fallback;
    if (strcmp(name, "accurate") == 0)
        return PACING_ACCURATE;
    if (strcmp(name, "accurate_vsync") == 0)
        return PACING_ACCURATE_VSYNC;
    if (strcmp(name, "gaming") == 0)
        return PACING_GAMING;
    if (strcmp(name, "gaming_vsync") == 0)
        return PACING_GAMING_VSYNC;
    return fallback;
}
