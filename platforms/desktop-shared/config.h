#include "../../src/VideoCatalog.h"
#include "MachineMenuOptions.h"
/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2021  Ignacio Sanchez
 * Copyright (C) 2024-2026  Saverio Russo
 *
 * Derived from Gearcoleco by Ignacio Sanchez. His code is free software: you
 * can redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * The changes and additions by Saverio Russo are free software: you can
 * redistribute them and/or modify them under the terms of the GNU Affero
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 * Section 13 of both licences permits the combination.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License and the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of both licences along with this
 * program.  If not, see http://www.gnu.org/licenses/
 *
 * SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later
 */

#ifndef CONFIG_H
#define	CONFIG_H

#include <SDL3/SDL.h>
#include "../../src/gearsf7000.h"
#define MINI_CASE_SENSITIVE
#include "mINI/ini.h"
#include "imgui/imgui.h"

#ifdef CONFIG_IMPORT
    #define EXTERN
#else
    #define EXTERN extern
#endif

static const int config_max_recent_roms = 10;

static const int config_max_recent_discs = 10;

static const int config_max_recent_cassettes = 10;

// Separate instances per recorder; changing machine never swaps or merges them.
struct config_Tape
{
    std::string path;
    bool write_protected = true;
    std::string recent[config_max_recent_cassettes];
    void Remember(const std::string& filename) {
        if (filename.empty()) return;
        // Copy first: filename can refer to an entry that is about to move.
        const std::string value = filename;
        int slot = 0;
        while (slot < config_max_recent_cassettes && recent[slot] != value) ++slot;
        if (slot == config_max_recent_cassettes) --slot;
        for (int i = slot; i > 0; --i) recent[i] = recent[i - 1];
        recent[0] = value;
    }
};

struct config_Emulator
{
    bool fullscreen = false;
    bool show_menu = true;
    bool keyboard_mode = true;
    bool paused = false;
    int save_slot = 0;
    bool start_paused = false;
    bool ffwd = false;
    int ffwd_speed = 1;
    int machine = (int)SystemModel::SC3000Export;
    // The Sega model in use before a ColecoVision image selected that board,
    // so loading a Sega image afterwards returns to it: a ColecoVision cannot
    // run a Sega cartridge and must not silently keep it.
    int last_sega_machine = (int)SystemModel::SC3000Export;
    bool coleco_sgm = false;
    int vdp = 0; // Compatibility mirror for older callers.
    std::map<std::string, int> last_board_by_machine;
    std::string video_option; // Empty means machine default.
    std::string video_option_by_model[static_cast<int>(SystemModel::GameGearExport) + 1];
    // Stable disk keys are declared by ConfigProfiles; this array is runtime only.
    int vdp_by_model[static_cast<int>(SystemModel::GameGearExport) + 1] = {};
    int last_model_by_family[6] = {static_cast<int>(SystemModel::SC3000Export),
        static_cast<int>(SystemModel::SegaMarkIII), static_cast<int>(SystemModel::ColecoVision),
        static_cast<int>(SystemModel::MSX1NTSC), static_cast<int>(SystemModel::SordM5PAL),
        static_cast<int>(SystemModel::GameGearExport)};
    // 0 = auto (signature, CRC and heuristics), 1 = manual (mapper_type wins).
    // Manual performs no signature or checksum check at all.
    int mapper_mode = 0;
    // Only meaningful when mapper_mode == 1. Holds a Cartridge::CartridgeTypes.
    int mapper_type = (int)Cartridge::SC3000_FLAT64K;
    // Mapper choices are stored per machine profile. Game Gear and SMS can
    // use the same mapper implementation but retain independent selections.
    // mapper_mode/mapper_type are the live values; config_select_machine()
    // swaps them when the profile changes.
    int mapper_mode_by_family[6] = {0, 0, 0, 0, 0, 0};
    int mapper_type_by_family[6] = {
        (int)Cartridge::SC3000_FLAT64K,
        (int)Cartridge::SMS_SEGA_MAPPER,
        (int)Cartridge::COLECOVISION, (int)Cartridge::MSX_LINEAR, (int)Cartridge::SORD_M5_LINEAR,
        (int)Cartridge::SMS_SEGA_MAPPER};
    bool show_info = false;
    std::string recent_roms[config_max_recent_roms];
    std::string recent_discs[config_max_recent_discs];
    config_Tape sr1000_tape;
    config_Tape msx_tape;
    std::string bios_path;
    // [Machine.Coleco] BiosPath: the console ROM at $0000 (8 KiB).
    std::string coleco_bios_path;
    std::string msx_bios_path;
    std::string m5_bios_path;
    bool m5_em5_installed = false;
    bool msx_use_cbios;
    std::string disc_path;
    bool disc_write_protected = true;

    int savefiles_dir_option = 0;
    std::string savefiles_path;
    int savestates_dir_option = 0;
    std::string savestates_path;
    std::string last_open_path;
    int window_width = 770;
    int window_height = 600;
    //int spinner = 0;
    //int spinner_sensitivity = 0;
    bool capture_mouse = false;
    bool status_messages = false;
#if GEARSF7000_ENABLE_MCP
    int mcp_tcp_port = 7777;
    std::string mcp_http_address = "127.0.0.1";
#endif
};

// One mutually-exclusive state, not independent checkboxes - same reasoning
// as Overscan's single combo (see GetVerticalLineCounts's own history):
// None/Bilinear are the two sampler states that already existed,
// CRT_LOTTES/ADVANCED_SCALING each run their own live GPU pass instead of
// sampling emu_texture directly. Shared between config.h and renderer.cpp,
// which needs to branch on it too.
#define POSTPROCESSING_NONE 0
#define POSTPROCESSING_BILINEAR 1
#define POSTPROCESSING_CRT_LOTTES 2
// Sharper XY resampling than plain Bilinear (see shaders/crt_pass/
// advanced_scaling.frag) - crt_lottes.frag's own horizontal Gaussian
// "sharpen" kernel applied on both axes, plus a black-level crush/contrast
// stretch since it has no scanlines/mask of its own to give the picture
// punch.
#define POSTPROCESSING_ADVANCED_SCALING 3

// Frame pacing is two independent choices, so the constants are the two bits
// that make them:
//
//   align   run the machine's crystals at the display's rate (Gaming) or at
//           their real values (Accurate). Latched at Reset.
//   vsync   wait for the blank before presenting, or present immediately and
//           accept tearing. Takes effect at once.
//
// They really are independent. Accurate + VSync is a clean picture on the
// machine's own clock, which is what you want while watching something you
// also intend to measure; Accurate alone is the same timing with the lowest
// possible latency. Collapsing the two into one axis loses that.
#define PACING_VSYNC_BIT 1
#define PACING_ALIGN_BIT 2

#define PACING_ACCURATE 0
#define PACING_ACCURATE_VSYNC (PACING_VSYNC_BIT)
#define PACING_GAMING (PACING_ALIGN_BIT)
#define PACING_GAMING_VSYNC (PACING_ALIGN_BIT | PACING_VSYNC_BIT)

// Whether the swapchain waits for the blank.
bool config_video_vsync(void);

// Whether the crystals are aligned to the display. Read by the scheduler,
// which decides the alignment and the vblank lock from it.
bool config_video_pacing_aligned(void);

// "accurate", "accurate_vsync", "gaming", "gaming_vsync". The stable
// identifier, for the config file and the MCP surface; the menu spells it out
// with the rate attached. Stored by name rather than by number so that adding
// a mode - which has already happened once - cannot silently reinterpret an
// existing ini.
const char* config_video_pacing_name(void);
int config_video_pacing_from_name(const char* name, int fallback);

// Settings that describe how the emulated picture is presented.  There are
// two independent instances: config_video is the Computer-mode profile and
// config_debug.video is the Debug Output profile.
struct config_VideoOutput
{
    int scale = 0;
    int ratio = 1;
    int overscan = 1;
    int postprocessing = POSTPROCESSING_NONE;
    // Persisted CRT (Lottes) settings, independent of the transient copy
    // renderer_crt_lottes_params() exposes for the Debug > Video > Show CRT
    // Lottes Test window - that one is a scratch pad for testing, these are
    // the real saved defaults applied to live gameplay whenever
    // postprocessing == POSTPROCESSING_CRT_LOTTES. Field meanings match
    // RendererCrtLottesParams / crt_lottes.frag exactly.
    // Defaults tuned by eye and confirmed ("valori ok") after the bold-
    // dark-edges port landed - not derived from any reference, same as
    // every other parameter set in this file.
    float crt_lottes_mask_type = 0.0f;
    float crt_lottes_mask_intensity = 0.12f;
    float crt_lottes_scanline_thinness = 0.39f;
    float crt_lottes_scan_blur = 6.0f;
    float crt_lottes_gamma = 2.54f;
    // "Bold dark edges", ported from advanced_scaling.frag - see that
    // struct's own comment for what each one does.
    float crt_lottes_black_threshold = 0.15f;
    float crt_lottes_boldness = 0.42f;
    float crt_lottes_edge_radius = 0.41f;
    // Persisted Advanced Scaling settings (see advanced_scaling.frag).
    // sharpness is the positive UI-facing magnitude - the shader negates
    // it itself for the exp2() falloff exponent, same convention as
    // crt_lottes_scan_blur/"Sharpness" above.
    float advanced_scaling_sharpness = 4.0f;
    // "Bold dark edges" - see advanced_scaling.frag's own comment on why
    // this replaced a global black-level crush + contrast stretch (that
    // touched every color in the image, not just the dark lines it was
    // meant to thicken). blackThreshold: how dark a neighbor must be to
    // count as "an edge" (0 = only pure black triggers it). boldness: how
    // strongly a pixel near such an edge gets pulled toward black.
    // edgeRadius: how far away (in real output pixels) a dark neighbor can
    // be and still count - independent of boldness on purpose, a second
    // "the halo covers a whole character cell, not just the stroke"
    // version made clear strength and reach need separate knobs.
    float advanced_scaling_black_threshold = 0.15f;
    float advanced_scaling_boldness = 0.5f;
    float advanced_scaling_edge_radius = 1.0f;
};

struct config_Video : config_VideoOutput
{
    bool fps = false;
    bool frame_pacing_info = false;
    bool scanlines = true;
    float scanlines_intensity = 0.10f;
#ifdef SPRITE_EXPANDER
    bool sprite_limit = false;
#endif
    // Frame pacing. One choice where there used to be a vertical sync
    // checkbox doing two jobs at once - deciding when a picture reaches the
    // screen *and*, by accident, how fast the machine ran.
    //
    //   Accurate      the real crystals, 59.9227 / 50.1590, nothing resampled
    //   Gaming        crystals aligned to the display, presented immediately
    //   Gaming+VSync  aligned and waiting for the blank
    //
    // The alignment is latched at Reset by design, so switching between
    // Accurate and either Gaming mode takes effect on the next reset. Vertical
    // sync itself changes immediately, as it always did.
    int pacing = PACING_GAMING_VSYNC;
    int palette = 0;
    // The Mark III's VDP in a TMS mode: its own choice, so picking the
    // 315-5124's colours there leaves the SC-3000's palette alone.
    int sms_tms_palette = 0;
};

struct config_Audio
{
    bool enable = true;
#if GEARSF7000_ENABLE_AY
    // SGM-style AY-3-8910 / YM2149 expansion. Off by default: it is hardware
    // that does not exist unless you fit it.
    bool ay_enable = false;
    int  ay_chip = 0;               // 0 = AY-3-8910, 1 = YM2149
    int  ay_port_base = 0x20;       // see IsSC3000_AYport() for why $20
#endif
#if GEARSF7000_ENABLE_YM2413
    // Optional Sega FM Sound Unit. The selected machine decides whether this
    // attachment is meaningful; currently only Mark III installs it.
    bool ym2413_enable = false;
#endif
};

struct config_Input
{
    SDL_Scancode key_left;
    SDL_Scancode key_right;
    SDL_Scancode key_up;
    SDL_Scancode key_down;
    SDL_Scancode key_left_button;
    SDL_Scancode key_right_button;
    //SDL_Scancode key_blue;
    //SDL_Scancode key_purple;
    //SDL_Scancode key_0;
    //SDL_Scancode key_1;
    //SDL_Scancode key_2;
    //SDL_Scancode key_3;
    //SDL_Scancode key_4;
    //SDL_Scancode key_5;
    //SDL_Scancode key_6;
    //SDL_Scancode key_7;
    //SDL_Scancode key_8;
    //SDL_Scancode key_9;
    //SDL_Scancode key_asterisk;
    //SDL_Scancode key_hash;
    bool gamepad;
    int gamepad_directional;
    bool gamepad_invert_x_axis;
    bool gamepad_invert_y_axis;
    int gamepad_left_button;
    int gamepad_right_button;
    //int gamepad_blue;
    //int gamepad_purple;
    int gamepad_x_axis;
    int gamepad_y_axis;
    // Host scancode assigned to each key in sk1100_keyboard_layout.  This is
    // deliberately separate from the two joystick profiles above: keyboard
    // mode addresses the complete physical SK-1100 matrix.
    SDL_Scancode sk1100_key[64];
    // ColecoVision controller keypad, indexed like coleco_keypad_layout.
    SDL_Scancode keypad_key[12];
    // Master System: what is plugged into this player's controller port
    // (SystemControllerDevice), and the keys and gamepad buttons for a Mega
    // Drive pad's A, Start, X, Y, Z and Mode, indexed like md_pad_layout.
    // Unbound until the user binds them.
    int controller_device;
    SDL_Scancode md_key[6];
    int gamepad_md_button[6];
    // The Game Gear has one built-in Start button, separate from SMS/MD pads.
    SDL_Scancode gg_start_key;
    int gamepad_gg_start_button;
    //int gamepad_1;
    //int gamepad_2;
    //int gamepad_3;
    //int gamepad_4;
    //int gamepad_5;
    //int gamepad_6;
    //int gamepad_7;
    //int gamepad_8;
    //int gamepad_9;
    //int gamepad_0;
    //int gamepad_asterisk;
    //int gamepad_hash;
};

enum config_HotkeyIndex
{
    config_HotkeyIndex_OpenROM = 0,
    config_HotkeyIndex_ReloadROM,
    config_HotkeyIndex_Reset,
    config_HotkeyIndex_Pause,
    config_HotkeyIndex_FFWD,
    config_HotkeyIndex_SaveState,
    config_HotkeyIndex_LoadState,
    config_HotkeyIndex_Screenshot,
    config_HotkeyIndex_Fullscreen,
    config_HotkeyIndex_ShowMainMenu,
    config_HotkeyIndex_DebugStepInto,
    config_HotkeyIndex_DebugStepOver,
    config_HotkeyIndex_DebugStepLine,
    config_HotkeyIndex_DebugStepFrame,
    config_HotkeyIndex_DebugStepBack,
    config_HotkeyIndex_DebugContinue,
    config_HotkeyIndex_DebugContinueFromHere,
    config_HotkeyIndex_DebugBreak,
    config_HotkeyIndex_DebugRunToCursor,
    config_HotkeyIndex_DebugBreakpoint,
    config_HotkeyIndex_DebugGoBack,
    config_HotkeyIndex_DebugCopy,
    config_HotkeyIndex_DebugPaste,
    config_HotkeyIndex_COUNT
};

struct config_Hotkey
{
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
    SDL_Keymod mod = SDL_KMOD_NONE;
    char str[64] = {0};
};

struct config_Debug
{
    bool debug = false;
    // By default the debugger Output window follows the main Video profile.
    // Disable this to use the independent profile below.
    bool video_as_main_settings = true;
    // Independent presentation profile used when the link above is disabled.
    config_VideoOutput video = [] {
        config_VideoOutput output;
        output.scale = 1;
        output.ratio = 0;
        output.overscan = 0;
        output.postprocessing = POSTPROCESSING_NONE;
        return output;
    }();
    bool show_screen = true;
    bool show_disassembler = true;
    bool show_processor = true;
    bool show_memory = true;
    bool show_pattern_table = false;
    bool show_sprites = false;
    bool show_nametable = false;
    bool show_video_register_explorer = false;
    bool show_psg = true;
#if GEARSF7000_ENABLE_AY
    bool show_ay = false;
#endif
    // Kept in every build so the configuration round-trips unchanged.
    bool show_ym2413 = false;
    bool show_cassette = false;
    bool show_rewind = false;
    // Recorder capture is independent from whether its window is visible.
    // Keep it on by default so F5/F6 and the timeline work without a separate
    // start operation, as they do in Gearsystem.
    bool rewind_enabled = true;
    // Diagnostic log. 3 = Info, the lifecycle events worth keeping; 4 = Debug
    // turns on the legacy Log() firehose, which includes per-frame paths.
    int log_level = 3;
    int log_max_mb = 4;
    int rewind_seconds = 60;
    bool show_ppi_registers = true;
    bool show_sf7000 = false;
    bool show_events = false;
    // ROM map (DOCS/PROGRAM_ANALYSIS_AUDIT.md section 12): the window, and
    // whether the map accumulates. Off by default - off costs nothing.
    bool show_rom_map = false;
    bool show_disassembler_new = false;
    bool show_deep_search = false;
    bool show_to_check = false;
    bool show_palettes = false;
    bool rom_map_enabled = false;
    // Where the ROM map projects live (Debug > Analysis). Never left empty:
    // config_read() fills in config_default_projects_dir() when a config
    // from before this setting names none, so the value is always there to
    // see in gearconfig.ini and in the menu.
    std::string projects_dir;
    // Debug Events display filters - same class of bug as the grid/region
    // toggles above: plain statics that reset on every restart instead of
    // remembering how the list was left.
    int events_category_filter = -1;
    bool events_newest_first = true;
    bool events_group_by_pc = false;
    bool events_normalize_clock = false;
    bool show_memory_import = true;
    // ROM Inspector (DOCS/ROM_INSPECTOR_PLAN.md point 4) - offline external
    // ROM analysis, not tied to the currently loaded cartridge.
    bool show_rom_inspector = false;
    bool show_disc_explorer = false;
    // Area scale for the VDP debug windows (Background/Name Table, Pattern
    // Table, Sprites) - how large the rendered tile/sprite grid is drawn,
    // independent per window since each has a different native aspect
    // ratio. Not "zoom" (magnification of a fixed area) - this resizes the
    // whole drawn area, same convention/step values as Video > Scale.
    float vram_background_area_scale = 2.0f;
    float vram_tiles_area_scale = 2.0f;
    std::string vram_pattern_disabled_layer_options;
    float vram_sprites_area_scale = 4.0f;
    float vram_sprites_preview_area_scale = 1.0f;
    // 0 live SAT, 1 last rendered frame, 2 current fetch pipeline,
    // 3 one scanline from the last completed/rewind-restored frame.
    std::string vram_sprites_source_code = "live-sat";
    std::string vram_sprites_hidden_preview_parts;
    std::string vram_sprites_disabled_preview_options;
    int vram_sprites_scanline = 0;
    bool gray_inactive_sprite_attributes = true;
    // Grid/region overlays on the Name Table and Pattern Table views. These
    // used to be plain function-local statics, which meant they reset to
    // their default every restart - the whole point of a debug overlay
    // toggle is to stay how you left it.
    bool show_grid_background = true;
    bool show_viewport_background = true;
    bool show_regions_background = false;
    bool show_grid_tiles = true;
    // GPU render-to-texture test pass (horizontal blur), not a real CRT
    // decode - see shaders/crt_pass/README.md.
    bool show_crt_test_pass = false;
    // Real two-pass composite/S-Video decode, running on a captured GCRT
    // signal file - see composite_chroma.frag / composite_luma.frag.
    bool show_composite_decode_test = false;
    // Display-stage test (trimmed GearSystem crt_consumer.glsl port) -
    // see crt_consumer.frag.
    bool show_crt_consumer_test = false;
    // Second display-stage candidate (trimmed crt-lottes-fast.glsl port) -
    // see crt_lottes.frag.
    bool show_crt_lottes_test = false;
#if GEARSF7000_ENABLE_MCP
    bool show_mcp_server = false;
    bool show_watch_monitor = false;
#endif
    bool show_basic_typer = false;
    // Listings that carry graphic characters as \xNN need decoding before the
    // text is typed; plain ASCII listings must not be touched, since a lone
    // backslash is a real SC-3000 character.
    bool basic_typer_decode_escapes = false;
    // Index into the typing-speed presets in gui_debug_basic_typer.cpp. 0 is
    // the pace verified on a full listing; the other is margin.
    int basic_typer_speed = 0;
    // Address of BASIC's five-word pointer block, for the direct .bas loader.
    // 0 scans for it, which is the default because the address differs per
    // BASIC - $8160 on the Level III cartridge, $9954 on Disk BASIC - and a
    // port under development will move it again.
    int basic_pointer_block = 0;
    // Disassembler presentation state.  Keep it separate from the window
    // visibility flags so the debugger reopens exactly as the user left it.
    bool dis_follow_pc = true;
    bool dis_show_opcodes = true;
    bool dis_show_symbols = true;
    bool dis_show_segments = true;
    bool dis_show_bank = true;
    int font_size = 0;
    int ui_density = 0;
    float ui_scale = 1.0f;
    // 0 = debugger fixed font, 1 = Iosevka Condensed, 2 = Iosevka Light.
    // This is a global Memory Editor preference shared by ROM/RAM/VRAM tabs.
    int memory_data_font = 0;
    bool multi_viewport = false;
};

EXTERN mINI::INIStructure config_ini_data;
EXTERN char* config_root_path;
EXTERN char config_emu_file_path[4096];
EXTERN char config_imgui_file_path[4096];
EXTERN config_Emulator config_emulator;
EXTERN config_Video config_video;
EXTERN config_Audio config_audio;

// Cartridge type to impose on the core, derived from configuration. It lives
// here rather than in gui.cpp because both the GUI and application.cpp need
// it, and both used to hardcode CartridgeNotSupported - so a manual choice was
// ignored on ROM load and on reset alike.
inline Cartridge::CartridgeTypes config_forced_cartridge_type(void)
{
    return config_emulator.mapper_mode == 1
        ? (Cartridge::CartridgeTypes)config_emulator.mapper_type
        : Cartridge::CartridgeNotSupported;   // auto: GatherMetadata decides
}

// Only consulted in auto mode, and only when detection recognises nothing.
inline Cartridge::CartridgeTypes config_fallback_cartridge_type(void)
{
    if (config_emulator.mapper_mode == 1)
        return Cartridge::CartridgeNotSupported;

    SystemModel model = static_cast<SystemModel>(config_emulator.machine);
    if (!IsValidSystemModel(model))
        model = SystemModel::SC3000Export;
    return Cartridge::DefaultTypeForMachine(GetSystemMotherboard(model));
}

// The machine profile a model takes its mapper choice from.
enum config_MapperFamily
{
    config_MapperFamily_SG1000 = 0,   // SG-1000 and SC-3000: the same slot
    config_MapperFamily_SMS = 1,
    config_MapperFamily_Coleco = 2,
    config_MapperFamily_MSX = 3,
    config_MapperFamily_M5 = 4,
    config_MapperFamily_GameGear = 5,
    config_MapperFamily_COUNT = 6
};

inline config_MapperFamily config_mapper_family(SystemModel model)
{
    if (!IsValidSystemModel(model))
        return config_MapperFamily_SG1000;
    return IsGameGearSystem(model) ? config_MapperFamily_GameGear
        : IsSMSSystem(model) ? config_MapperFamily_SMS
        : IsColecoSystem(model) ? config_MapperFamily_Coleco
        : IsM5System(model) ? config_MapperFamily_M5
        : IsMSXSystem(model) ? config_MapperFamily_MSX : config_MapperFamily_SG1000;
}

// Keeps the live mapper choice in its family's slot: called before the
// configuration is written and before the machine changes family.
inline void config_mapper_store(void)
{
    const config_MapperFamily f =
        config_mapper_family(static_cast<SystemModel>(config_emulator.machine));
    config_emulator.mapper_mode_by_family[f] = config_emulator.mapper_mode;
    config_emulator.mapper_type_by_family[f] = config_emulator.mapper_type;
}

// The one way to change config_emulator.machine when the family may change:
// the mapper choice of the family being left is kept, the one of the family
// being entered comes back. Within a family (SG-1000 to SC-3000) nothing
// moves, so a manual choice survives.
inline std::string config_resolve_video_option(int legacy, const std::string& code)
{
    if (legacy == 0) return {};
    const auto* option = GetVideoCatalog().FindOption(code);
    if (option && option->legacyConfig == legacy) return code;
    SystemVideoDevice device;
    if (!GetSystemVideoDeviceFromConfigValue(legacy, device)) return {};
    option = GetVideoCatalog().FindLegacyOption(device);
    return option ? option->code : std::string{};
}

inline std::string config_selected_video_option()
{
    return config_resolve_video_option(config_emulator.vdp, config_emulator.video_option);
}

inline bool config_select_video_option(const std::string& code)
{
    if (code.empty()) {
        config_emulator.vdp = 0;
        config_emulator.video_option.clear();
        return true;
    }
    const auto model = static_cast<SystemModel>(config_emulator.machine);
    for (const auto* option : GetMachineVideoOptions(model))
        if (option->code == code) {
            config_emulator.video_option = code;
            config_emulator.vdp = option->legacyConfig;
            return true;
        }
    return false;
}

inline void config_select_machine(SystemModel model)
{
    if (!IsValidSystemModel(model)) return;
    const int old = config_emulator.machine;
    if (old == static_cast<int>(model)) return;
    config_mapper_store();
    config_emulator.last_board_by_machine[GetMachineMenuGroup(static_cast<SystemModel>(old)).code] = old;
    config_emulator.vdp_by_model[old] = config_emulator.vdp;
    config_emulator.video_option_by_model[old] = config_selected_video_option();
    config_emulator.last_model_by_family[config_mapper_family(static_cast<SystemModel>(old))] = old;
    config_emulator.vdp = config_emulator.vdp_by_model[static_cast<int>(model)];
    config_emulator.video_option = config_emulator.video_option_by_model[static_cast<int>(model)];
    if (IsColecoSystem(model)) config_emulator.coleco_sgm = HasSGM(model);
    config_emulator.machine = (int)model;
    const config_MapperFamily f = config_mapper_family(model);
    config_emulator.mapper_mode = config_emulator.mapper_mode_by_family[f];
    config_emulator.mapper_type = config_emulator.mapper_type_by_family[f];
}

inline SystemModel config_machine_group_selection(const MachineMenuGroup& group)
{
    const auto saved = config_emulator.last_board_by_machine.find(group.code);
    return ChooseMachineMenuModel(group, static_cast<SystemModel>(config_emulator.machine),
        saved == config_emulator.last_board_by_machine.end() ? group.defaultModel : static_cast<SystemModel>(saved->second));
}

EXTERN config_Input config_input[2];
EXTERN config_Hotkey config_hotkeys[config_HotkeyIndex_COUNT];
EXTERN config_Debug config_debug;

inline config_VideoOutput& config_video_output_for_mode(bool debug_mode)
{
    return debug_mode && !config_debug.video_as_main_settings
        ? config_debug.video : static_cast<config_VideoOutput&>(config_video);
}

// Optional explicit root for isolated integration tests / embedded desktop hosts.
EXTERN void config_init(const char* settings_root = nullptr);
// The headless instance's own settings folder, seeded once from the main one.
EXTERN void config_init_headless(void);
// The projects folder to use when the config names none. See config.cpp.
EXTERN std::string config_default_projects_dir(void);
EXTERN void config_destroy(void);
EXTERN const std::string& config_get_storage_error();
EXTERN const std::string& config_get_storage_notice();
EXTERN bool config_storage_writable();
EXTERN void config_read(void);
struct config_SaveResult {
    bool success;
    bool retry_allowed;
    std::string file;
    std::string operation;
    std::string error;
};
EXTERN config_SaveResult config_write(void);
EXTERN config_SaveResult config_get_storage_result();
EXTERN void config_update_hotkey_string(config_Hotkey* hotkey);
EXTERN const char* config_hotkey_name(config_HotkeyIndex index);
EXTERN const char* config_hotkey_label(config_HotkeyIndex index);
EXTERN config_Hotkey config_default_hotkey(config_HotkeyIndex index);
// Factory value of one joypad binding, found by which field of config_input it
// is: the Default button of the binding popup and config_init share it. False
// for a pointer that is not a joypad key or gamepad button.
EXTERN bool config_default_key_binding(const SDL_Scancode* binding, SDL_Scancode* value);
EXTERN bool config_default_button_binding(const int* binding, int* value);

#undef CONFIG_IMPORT
#undef EXTERN
#endif	/* CONFIG_H */
