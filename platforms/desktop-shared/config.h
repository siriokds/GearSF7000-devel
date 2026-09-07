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
    int region = 0;
    // 0 = auto (signature and CRC decide), 1 = manual (mapper_type wins).
    // Manual performs no signature or checksum check at all.
    int mapper_mode = 0;
    // Only meaningful when mapper_mode == 1. Holds a Cartridge::CartridgeTypes.
    int mapper_type = (int)Cartridge::SC3000_FLAT64K;
    // Used in auto mode when nothing identifies the image. The historic
    // behaviour was SG-1000 16K, hardcoded; keeping it as the default means
    // auto behaves exactly as before unless this is changed.
    int mapper_fallback = (int)Cartridge::SG1000_16K;
    bool show_info = false;
    std::string recent_roms[config_max_recent_roms];
    std::string recent_discs[config_max_recent_discs];
    std::string recent_cassettes[config_max_recent_cassettes];
    std::string bios_path;
    std::string disc_path;
    bool disc_write_protected = true;
    std::string cassette_path;
    bool cassette_write_protected = true;

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
    GC_Color color[16] = {
        {0, 0, 0}, {0, 0, 0}, {33, 200, 66}, {94, 20, 120},
        {84, 85, 237}, {112, 118, 252}, {212, 82, 77}, {66, 235, 245},
        {252, 85, 84}, {255, 121, 120}, {212, 193, 84}, {230, 206, 128},
        {33, 176, 59}, {201, 91, 186}, {204, 204, 204}, {255, 255, 255}
    };
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
    // Independent presentation profile for the debugger's Output window.
    // Conservative defaults are intentional for old config files which do
    // not have a [DebugVideo] section yet: native picture, square pixels and
    // no display shader. config_write() creates the complete section.
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
    bool show_video = false;
    bool show_video_registers = true;
    bool show_psg = true;
#if GEARSF7000_ENABLE_AY
    bool show_ay = false;
#endif
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
    float vram_sprites_area_scale = 4.0f;
    float vram_sprites_preview_area_scale = 1.0f;
    // 0 live SAT, 1 last rendered frame, 2 current fetch pipeline,
    // 3 one scanline from the last completed/rewind-restored frame.
    int vram_sprites_source = 0;
    int vram_sprites_scanline = 0;
    // Grid/region overlays on the Name Table and Pattern Table views. These
    // used to be plain function-local statics, which meant they reset to
    // their default every restart - the whole point of a debug overlay
    // toggle is to stay how you left it.
    bool show_grid_background = true;
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

EXTERN mINI::INIFile* config_ini_file;
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
    return config_emulator.mapper_mode == 1
        ? Cartridge::CartridgeNotSupported
        : (Cartridge::CartridgeTypes)config_emulator.mapper_fallback;
}

EXTERN config_Input config_input[2];
EXTERN config_Hotkey config_hotkeys[config_HotkeyIndex_COUNT];
EXTERN config_Debug config_debug;

EXTERN void config_init(void);
EXTERN void config_destroy(void);
EXTERN void config_read(void);
EXTERN void config_write(void);
EXTERN void config_update_hotkey_string(config_Hotkey* hotkey);
EXTERN const char* config_hotkey_name(config_HotkeyIndex index);
EXTERN const char* config_hotkey_label(config_HotkeyIndex index);
EXTERN config_Hotkey config_default_hotkey(config_HotkeyIndex index);

#undef CONFIG_IMPORT
#undef EXTERN
#endif	/* CONFIG_H */
