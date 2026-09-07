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

#include <SDL3/SDL.h>
#include "../../src/gearsf7000.h"

#define MINI_CASE_SENSITIVE
#include "mINI/ini.h"

#define CONFIG_IMPORT
#include "config.h"
#include "sk1100_keyboard_layout.h"

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
#if defined(_WIN32)
static void migrate_legacy_windows_config(void);
static void copy_file_if_missing(const char* source_path, const char* destination_path);
#endif

void config_init(void)
{
    if (check_portable())
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

    build_config_path(config_emu_file_path, sizeof(config_emu_file_path), "config.ini");
    build_config_path(config_imgui_file_path, sizeof(config_imgui_file_path), "imgui.ini");

#if defined(_WIN32)
    if (!check_portable())
        migrate_legacy_windows_config();
#endif

    config_input[0].key_left = SDL_SCANCODE_LEFT;
    config_input[0].key_right = SDL_SCANCODE_RIGHT;
    config_input[0].key_up = SDL_SCANCODE_UP;
    config_input[0].key_down = SDL_SCANCODE_DOWN;
    config_input[0].key_left_button = SDL_SCANCODE_A;
    config_input[0].key_right_button = SDL_SCANCODE_S;
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

    config_input[1].key_up = SDL_SCANCODE_KP_5;
    config_input[1].key_down = SDL_SCANCODE_KP_2;
    config_input[1].key_left = SDL_SCANCODE_KP_1;
    config_input[1].key_right = SDL_SCANCODE_KP_3;
    config_input[1].key_left_button = SDL_SCANCODE_KP_4;
    config_input[1].key_right_button = SDL_SCANCODE_KP_6;
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

    config_ini_file = new mINI::INIFile(config_emu_file_path);
}

void config_destroy(void)
{
    SafeDelete(config_ini_file)
    SDL_free(config_root_path);
}

void config_read(void)
{
    if (!config_ini_file->read(config_ini_data))
    {
        Log("Unable to load settings from %s; using defaults", config_emu_file_path);
    }
    else
    {
        Log("Loading settings from %s", config_emu_file_path);
    }

#if GEARSF7000_ENABLE_DEBUG_TOOLS
    config_debug.debug = read_bool("Debug", "Debug", false);
#else
    config_debug.debug = false;
#endif
    config_debug.show_disassembler = read_bool("Debug", "Disassembler", true);
    config_debug.show_screen = read_bool("Debug", "Screen", true);
    config_debug.show_memory = read_bool("Debug", "Memory", true);
    config_debug.show_processor = read_bool("Debug", "Processor", true);
    config_debug.show_video = read_bool("Debug", "Video", false);
    config_debug.show_video_registers = read_bool("Debug", "VideoRegisters", false);
    config_debug.show_psg = read_bool("Debug", "PSG", false);
    
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
    config_debug.vram_sprites_source = read_int("Debug", "VramSpritesSource", 0);
    config_debug.vram_sprites_scanline = read_int("Debug", "VramSpritesScanline", 0);
    config_debug.show_grid_background = read_bool("Debug", "ShowGridBackground", true);
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
    config_emulator.keyboard_mode = read_bool("Input", "KeyboardMode", true);
    config_emulator.ffwd_speed = read_int("Emulator", "FFWD", 1);
    config_emulator.save_slot = read_int("Emulator", "SaveSlot", 0);
    config_emulator.start_paused = read_bool("Emulator", "StartPaused", false);
    config_emulator.region = read_int("Emulator", "Region", 0);
    config_emulator.mapper_mode = read_int("Emulator", "MapperMode", 0);
    config_emulator.mapper_type = read_int("Emulator", "MapperType",
        (int)Cartridge::SC3000_FLAT64K);
    config_emulator.mapper_fallback = read_int("Emulator", "MapperFallback",
        (int)Cartridge::SG1000_16K);
#if GEARSF7000_ENABLE_MCP
    config_emulator.mcp_tcp_port = read_int("Emulator", "MCPTCPPort", 7777);
    config_emulator.mcp_http_address = read_string("Emulator", "MCPHTTPAddress");
    if (config_emulator.mcp_http_address.empty())
        config_emulator.mcp_http_address = "127.0.0.1";
#endif


    config_emulator.cassette_path = read_string("SR-1000", "CassettePath");
    config_emulator.cassette_write_protected = read_bool("SR-1000", "CassetteWriteProtected", true);
    for (int i = 0; i < config_max_recent_cassettes; i++)
    {
        std::string item = "RecentCassette" + std::to_string(i);
        config_emulator.recent_cassettes[i] = read_string("SR-1000", item.c_str());

        // Compatibility with the typo written by builds before the macOS
        // configuration audit. It is migrated in memory and saved under the
        // canonical singular key on the next configuration write.
        if (config_emulator.recent_cassettes[i].empty())
        {
            const std::string legacy_item = "RecentCassettes" + std::to_string(i);
            config_emulator.recent_cassettes[i] = read_string("SR-1000", legacy_item.c_str());
        }
    }


    config_emulator.bios_path = read_string("SF-7000", "BiosPath");
    config_emulator.disc_path = read_string("SF-7000", "DiscPath");
    config_emulator.disc_write_protected = read_bool("SF-7000", "DiscWriteProtected", true);

    for (int i = 0; i < config_max_recent_discs; i++)
    {
        std::string item = "RecentDISC" + std::to_string(i);
        config_emulator.recent_discs[i] = read_string("SF-7000", item.c_str());
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
    config_video.fps = read_bool("Video", "FPS", false);
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

    // A missing [DebugVideo] section intentionally uses config_Debug's
    // conservative defaults. The first config_write() materializes every
    // key, after which this profile evolves independently from [Video].
#if GEARSF7000_ENABLE_DEBUG_TOOLS
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

    for (int i = 0; i < 16; i++)
    {
        char pal_label_r[32];
        char pal_label_g[32];
        char pal_label_b[32];
        sprintf(pal_label_r, "CustomPalette%dR", i);
        sprintf(pal_label_g, "CustomPalette%dG", i);
        sprintf(pal_label_b, "CustomPalette%dB", i);
        config_video.color[i].red = read_int("Video", pal_label_r, config_video.color[i].red);
        config_video.color[i].green = read_int("Video", pal_label_g, config_video.color[i].green);
        config_video.color[i].blue = read_int("Video", pal_label_b, config_video.color[i].blue);
    }

    config_video.pacing = config_video_pacing_from_name(
        read_string("Video", "Pacing").c_str(), PACING_GAMING_VSYNC);
    
    config_audio.enable = read_bool("Audio", "Enable", true);
#if GEARSF7000_ENABLE_AY
    config_audio.ay_enable = read_bool("Audio", "AYEnable", false);
    config_audio.ay_chip = read_int("Audio", "AYChip", 0);
    config_audio.ay_port_base = read_int("Audio", "AYPortBase", 0x20);
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
            "SK1100", sk1100_keyboard_layout[key].id,
            sk1100_keyboard_layout[key].default_scancode);
        config_input[1].sk1100_key[key] = config_input[0].sk1100_key[key];
    }

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

void config_write(void)
{
    Log("Saving settings to %s", config_emu_file_path);

    write_bool("Debug", "Debug", config_debug.debug);
    write_bool("Debug", "Disassembler", config_debug.show_disassembler);
    write_bool("Debug", "Screen", config_debug.show_screen);
    write_bool("Debug", "Memory", config_debug.show_memory);
    write_bool("Debug", "Processor", config_debug.show_processor);
    write_bool("Debug", "Video", config_debug.show_video);
    write_bool("Debug", "VideoRegisters", config_debug.show_video_registers);
    write_bool("Debug", "PSG", config_debug.show_psg);
    write_bool("Debug", "PPI8255A", config_debug.show_ppi_registers);
    write_bool("Debug", "SF7000", config_debug.show_sf7000);
    write_bool("Debug", "Events", config_debug.show_events);
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
    write_int("Debug", "VramSpritesSource", config_debug.vram_sprites_source);
    write_int("Debug", "VramSpritesScanline", config_debug.vram_sprites_scanline);
    write_bool("Debug", "ShowGridBackground", config_debug.show_grid_background);
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
    write_int("Debug", "MemoryDataFont", config_debug.memory_data_font);
    write_bool("Debug", "MultiViewport", config_debug.multi_viewport);
    write_bool("Debug", "Cassette", config_debug.show_cassette);
    write_bool("Debug", "Rewind", config_debug.show_rewind);
    write_bool("Debug", "RecorderEnabled", config_debug.rewind_enabled);
    write_int("Debug", "LogLevel", config_debug.log_level);
    write_int("Debug", "LogMaxMB", config_debug.log_max_mb);
    write_int("Debug", "RewindSeconds", config_debug.rewind_seconds);

    write_bool("Emulator", "FullScreen", config_emulator.fullscreen);
    write_bool("Emulator", "ShowMenu", config_emulator.show_menu);
    write_bool("Input", "KeyboardMode", config_emulator.keyboard_mode);
    write_int("Emulator", "FFWD", config_emulator.ffwd_speed);
    write_int("Emulator", "SaveSlot", config_emulator.save_slot);
    write_bool("Emulator", "StartPaused", config_emulator.start_paused);
    write_int("Emulator", "Region", config_emulator.region);
    write_int("Emulator", "MapperMode", config_emulator.mapper_mode);
    write_int("Emulator", "MapperType", config_emulator.mapper_type);
    write_int("Emulator", "MapperFallback", config_emulator.mapper_fallback);
#if GEARSF7000_ENABLE_MCP
    write_int("Emulator", "MCPTCPPort", config_emulator.mcp_tcp_port);
    write_string("Emulator", "MCPHTTPAddress", config_emulator.mcp_http_address);
#endif


    write_string("SR-1000", "CassettePath", config_emulator.cassette_path);
    write_bool("SR-1000", "CassetteWriteProtected", config_emulator.cassette_write_protected);

    for (int i = 0; i < config_max_recent_cassettes; i++)
    {
        std::string item = "RecentCassette" + std::to_string(i);
        write_string("SR-1000", item.c_str(), config_emulator.recent_cassettes[i]);
    }


    write_string("SF-7000", "BiosPath", config_emulator.bios_path);
    write_string("SF-7000", "DiscPath", config_emulator.disc_path);
    write_bool("SF-7000", "DiscWriteProtected", config_emulator.disc_write_protected);

    for (int i = 0; i < config_max_recent_discs; i++)
    {
        std::string item = "RecentDISC" + std::to_string(i);
        write_string("SF-7000", item.c_str(), config_emulator.recent_discs[i]);
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
    write_bool("Video", "FPS", config_video.fps);
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
    for (int i = 0; i < 16; i++)
    {
        char pal_label_r[32];
        char pal_label_g[32];
        char pal_label_b[32];
        sprintf(pal_label_r, "CustomPalette%dR", i);
        sprintf(pal_label_g, "CustomPalette%dG", i);
        sprintf(pal_label_b, "CustomPalette%dB", i);
        write_int("Video", pal_label_r, config_video.color[i].red);
        write_int("Video", pal_label_g, config_video.color[i].green);
        write_int("Video", pal_label_b, config_video.color[i].blue);
    }
    write_string("Video", "Pacing", config_video_pacing_name());

    write_bool("Audio", "Enable", config_audio.enable);
#if GEARSF7000_ENABLE_AY
    write_bool("Audio", "AYEnable", config_audio.ay_enable);
    write_int("Audio", "AYChip", config_audio.ay_chip);
    write_int("Audio", "AYPortBase", config_audio.ay_port_base);
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
        write_int("SK1100", sk1100_keyboard_layout[key].id,
                  config_input[0].sk1100_key[key]);

    for (int hotkey = 0; hotkey < config_HotkeyIndex_COUNT; ++hotkey)
        write_hotkey(config_hotkey_name((config_HotkeyIndex)hotkey),
                     config_hotkeys[hotkey]);

    if (config_ini_file->write(config_ini_data, true))
    {
        Log("Settings saved");
    }
    else
    {
        Log("Unable to save settings to %s", config_emu_file_path);
    }
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

#if defined(_WIN32)
static void copy_file_if_missing(const char* source_path, const char* destination_path)
{
    FILE* destination = fopen(destination_path, "rb");
    if (destination)
    {
        fclose(destination);
        return;
    }

    FILE* source = fopen(source_path, "rb");
    if (!source)
        return;

    destination = fopen(destination_path, "wb");
    if (!destination)
    {
        Log("Unable to migrate settings file to %s", destination_path);
        fclose(source);
        return;
    }

    char buffer[8192];
    bool failed = false;
    size_t read_count = 0;
    while ((read_count = fread(buffer, 1, sizeof(buffer), source)) > 0)
    {
        if (fwrite(buffer, 1, read_count, destination) != read_count)
        {
            failed = true;
            break;
        }
    }

    failed = failed || ferror(source) || ferror(destination);
    fclose(source);
    fclose(destination);

    if (failed)
        Log("Unable to completely migrate settings file from %s", source_path);
    else
        Log("Migrated settings file from %s", source_path);
}

static void migrate_legacy_windows_config(void)
{
    FILE* config_file = fopen(config_emu_file_path, "rb");
    FILE* imgui_file = fopen(config_imgui_file_path, "rb");
    const bool config_exists = config_file != nullptr;
    const bool imgui_exists = imgui_file != nullptr;

    if (config_file)
        fclose(config_file);
    if (imgui_file)
        fclose(imgui_file);
    if (config_exists && imgui_exists)
        return;

    char* legacy_root_path = SDL_GetPrefPath("Geardome", "GearSystem");
    if (!legacy_root_path)
        return;

    char legacy_config_path[4096];
    char legacy_imgui_path[4096];
    const int config_length = SDL_snprintf(legacy_config_path, sizeof(legacy_config_path), "%sconfig.ini", legacy_root_path);
    const int imgui_length = SDL_snprintf(legacy_imgui_path, sizeof(legacy_imgui_path), "%simgui.ini", legacy_root_path);
    SDL_free(legacy_root_path);

    if (config_length >= 0 && static_cast<size_t>(config_length) < sizeof(legacy_config_path))
        copy_file_if_missing(legacy_config_path, config_emu_file_path);
    if (imgui_length >= 0 && static_cast<size_t>(imgui_length) < sizeof(legacy_imgui_path))
        copy_file_if_missing(legacy_imgui_path, config_imgui_file_path);
}
#endif

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

    std::string value = config_ini_data[group][key];

    if (!value.empty())
    {
        try
        {
        ret = std::stoi(value);
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

    std::string value = config_ini_data[group][key];

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

    std::string value = config_ini_data[group][key];

    if (!value.empty())
    {
        std::istringstream parser(value);
        parser >> std::boolalpha >> ret;
        if (parser.fail())
        {
            ret = default_value;
            Log("Invalid boolean setting: [%s][%s]=%s; using default", group, key, value.c_str());
        }
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
    std::string ret = config_ini_data[group][key];
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
