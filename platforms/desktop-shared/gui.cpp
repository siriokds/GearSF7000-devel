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

#include <math.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Before windows.h: it would otherwise pull in the winsock 1.1 declarations,
// which clash with the winsock2 ones mcp_transport.h brings in further down.
#include <winsock2.h>
#include <windows.h>
#include "../windows/resource.h"
#endif
#include "imgui/imgui.h"
// For CloseButton(): the recent lists reuse the same cross ImGui draws to
// close a window, rather than an X glyph that would depend on the font.
#include "imgui/imgui_internal.h"
#include "implot/implot.h"
#include "scheduler.h"
#include "imgui/fonts/Roboto-Light.h"
#include "imgui/fonts/RobotoMedium.h"
#include "imgui/fonts/MaterialIcons.h"
#include "imgui/fonts/IconsMaterialDesign.h"
#include "imgui/fonts/SC3000Basic.h"
#include "imgui/fonts/unifont.h"
#include "nfd_parented.h"
#include "config.h"
#include "VideoMenuOptions.h"
#include "VideoOutputSettings.h"
#include "rewind.h"
#include "emu.h"
#include "../../src/gearsf7000.h"
#include "../../src/build_info.h"
#include "renderer.h"
#include "application.h"
#include "license.h"
#include "backers.h"
#include "gui_debug.h"
#include "gui_debug_rewind.h"
#include "rom_map_project.h"
#include "sk1100_keyboard_layout.h"

#define GUI_IMPORT
#include "gui.h"
#include "ISystemBus.h"
#include "coleco_keypad_layout.h"
#include "md_pad_layout.h"
#if GEARSF7000_ENABLE_MCP
#include "mcp/mcp_manager.h"
#endif
#include "imgui/colors.h"

#if defined(_WIN32)
struct WindowsResourceView
{
    const unsigned char* data = nullptr;
    size_t size = 0;
};

static bool load_windows_resource(int id, WindowsResourceView* view)
{
    if (!view)
        return false;
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC handle = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!handle)
        return false;
    HGLOBAL loaded = LoadResource(module, handle);
    const DWORD size = SizeofResource(module, handle);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data || size == 0)
        return false;
    view->data = static_cast<const unsigned char*>(data);
    view->size = static_cast<size_t>(size);
    return true;
}
#endif

static int main_menu_height;
static bool dialog_in_use = false;
static SDL_Scancode* configured_key;
static int* configured_button;
static config_Hotkey* configured_hotkey;
static int configured_hotkey_index = -1;
static int configured_player = -1;
static const char* configured_action = NULL;
static int configured_sk1100_key = -1;

enum InputCaptureKind
{
    InputCapture_None = 0,
    InputCapture_Keyboard,
    InputCapture_Gamepad,
    InputCapture_Hotkey
};

enum InputCapturePhase
{
    InputCapture_Waiting = 0,
    InputCapture_Confirm,
    InputCapture_Conflict,
    InputCapture_Complete,
    InputCapture_Cancelled
};

static InputCaptureKind input_capture_kind = InputCapture_None;
static InputCapturePhase input_capture_phase = InputCapture_Waiting;
static bool input_capture_popup_requested = false;
static SDL_Scancode input_capture_key = SDL_SCANCODE_UNKNOWN;
static SDL_Keymod input_capture_mod = SDL_KMOD_NONE;
static int input_capture_button = SDL_GAMEPAD_BUTTON_INVALID;
static SDL_Scancode* input_capture_conflicting_key = NULL;
static int input_capture_conflicting_hotkey = -1;
static int* input_capture_conflicting_button = NULL;
static const char* input_capture_conflicting_action = NULL;
static int input_capture_conflicting_player = -1;
static char input_capture_message[160] = "";
static bool show_sk1100_keyboard_config = false;
static bool show_hotkey_config = false;
static bool show_interface_settings = false;
static bool apply_interface_settings = false;
static bool restore_interface_settings_position = false;
static ImVec2 interface_settings_position;
static float pending_interface_scale = 1.0f;
static int pending_interface_font = 0;
static int pending_interface_density = 0;
static ImTextureID sk1100_keyboard_texture = ImTextureID_Invalid;
static int sk1100_keyboard_texture_width = 0;
static int sk1100_keyboard_texture_height = 0;
static int sk1100_selected_key = -1;
static bool sk1100_keyboard_configure_mode = false;
static bool sk1100_virtual_key_down[SK1100_KEYBOARD_KEY_COUNT] = {};
static int sk1100_mouse_key = -1;
static bool shortcut_open_rom = false;
static ImFont* default_font[4];
static ImFont* roboto_fonts[4];
static ImFont* compact_chrome_font;
static ImFont* condensed_chrome_font;
static char bios_path[4096] = "";
static char disc_path[4096] = "";
static char cassette_path[4096] = "";
static char savefiles_path[4096] = "";
static char savestates_path[4096] = "";
static int main_window_width = 0;
static int main_window_height = 0;
static bool status_message_active = false;
static char status_message[4096] = "";
static u32 status_message_start_time = 0;
static u32 status_message_duration = 0;

// "Configure..." window for the CRT (Lottes) postprocessing mode - a real
// Output panel, not a diagnostic test tool, so it lives here rather than in
// gui_debug.cpp and isn't gated by config_debug.debug. No embedded preview
// image: once the selected profile uses CRT (Lottes), its Output view
// already shows the live result behind this panel, so a second copy would
// be redundant (unlike the Debug > Video > Show CRT Lottes Test window,
// which needs one because the pass isn't otherwise visible anywhere).
static bool show_crt_lottes_setup = false;
// Same reasoning as show_crt_lottes_setup - a real user-facing panel for
// Advanced Scaling in either presentation profile, no preview needed.
static bool show_advanced_scaling_setup = false;
// The setup window remembers which menu opened it. This matters while the
// debugger is enabled: Video still edits the Computer profile, whereas
// Debug > Video edits the Debug Output profile.
static config_VideoOutput* video_setup_profile = nullptr;



static void main_menu(void);
#if GEARSF7000_ENABLE_AY
static void gui_apply_ay_config(void);
#endif
static bool main_window(void);
static void file_dialog_open_rom(void);
static void file_dialog_load_ram(void);
static void file_dialog_load_basic_program(void);
static void file_dialog_save_basic_program(void);
static void file_dialog_save_ram(void);
static void file_dialog_load_state(void);
static void file_dialog_save_state(void);
#if GEARSF7000_ENABLE_RECORDER
static void file_dialog_save_vgm(void);
#endif
static void file_dialog_choose_savestate_path(void);
static void file_dialog_choose_projects_folder(void);
static void projects_folder_use(const std::string& path);
static void file_dialog_load_bios(void);
static void file_dialog_load_coleco_bios(void);
static void file_dialog_load_m5_bios(void);
static void file_dialog_load_msx_bios(void);
static void file_dialog_load_disc(void);
static void file_dialog_load_cassette(void);
static void file_dialog_select_msx_cassette(void);
static void file_dialog_load_symbols(void);
static void file_dialog_save_screenshot(void);
static void keyboard_configuration_item(const char* text, SDL_Scancode* key, int player);
static void gamepad_configuration_item(const char* text, int* button, int player);
static void popup_modal_input_binding(void);
static void window_sk1100_keyboard_config(void);
static void window_hotkey_config(void);
static void window_interface_settings(void);
static void popup_modal_about(void);
static void popup_modal_bios(void);
static void update_palette(void);
static void draw_menu_bar_status(void);
static void menu_reset(void);
static void menu_continue(void);
static void menu_reset_paused(void);
static void menu_eject(void);
static void menu_pause(void);
static void menu_ffwd(void);
static void window_crt_lottes_setup(void);
static void window_advanced_scaling_setup(void);
static void show_info(void);
static void show_fps(const ImVec2& content_origin);
static void show_status_message(void);
static Cartridge::ForceConfiguration get_force_config(void);
static void call_save_screenshot(const char* path);


#define SCALE_INTEGER               0
#define SCALE_1X                    1
#define SCALE_2X                    2
#define SCALE_3X                    3
#define SCALE_4X                    4
#define SCALE_5X                    5
#define SCALE_WIN_HEIGHT            6
#define SCALE_WIN_WIDTH_HEIGHT      7
#define SCALE_1_5X                  8
#define SCALE_2_5X                  9
#define SCALE_3_5X                 10
#define SCALE_4_5X                 11
// Fills the window height like SCALE_WIN_HEIGHT, but snaps the resulting
// multiplier to the nearest 0.5 instead of the raw continuous fraction the
// window happens to be - a cleaner, more predictable geometry while still
// staying close to a full-window fit. Doesn't dodge the CRT (Lottes)
// scanline resonance at exactly 2x (0.5 steps land there too) - see
// gearsf7000-open-items memory for that, this is a separate quality
// improvement, not a workaround for it.
#define SCALE_WIN_HEIGHT_HALF      12

static float explicit_scale_multiplier(int scale)
{
    switch (scale)
    {
        case SCALE_1X: return 1.0f;
        case SCALE_1_5X: return 1.5f;
        case SCALE_2X: return 2.0f;
        case SCALE_2_5X: return 2.5f;
        case SCALE_3X: return 3.0f;
        case SCALE_3_5X: return 3.5f;
        case SCALE_4X: return 4.0f;
        case SCALE_4_5X: return 4.5f;
        case SCALE_5X: return 5.0f;
        default: return 0.0f;
    }
}

static const VideoOutputPolicy& video_output_policy(const config_VideoOutput& output, bool debug)
{
    return GetVideoCatalog().OutputPolicy(emu_get_core()->GetActiveVideoOptionCode(), debug, output.overscan == 4);
}

static void apply_video_output_source(const config_VideoOutput& preference)
{
    const auto output = ResolveVideoOutput(preference, emu_get_core()->GetActiveVideoOptionCode(),
        &preference == &config_debug.video, emu_get_video_frame_descriptor());
    const bool full_frame = output.overscan == 4;
    emu_set_full_raster_debug_enabled(full_frame);
    if (!full_frame)
        emu_set_overscan(output.overscan);
}

static void menu_video_output_scale(config_VideoOutput& output, bool debug_output)
{
    const char* preview = "Integer Scale (Auto)";
    switch (video_output_policy(output, debug_output).Resolve("SCALE", output.scale))
    {
        case SCALE_1X: preview = "Integer Scale (1x)"; break;
        case SCALE_1_5X: preview = "Integer Scale (1.5x)"; break;
        case SCALE_2X: preview = "Integer Scale (2x)"; break;
        case SCALE_2_5X: preview = "Integer Scale (2.5x)"; break;
        case SCALE_3X: preview = "Integer Scale (3x)"; break;
        case SCALE_3_5X: preview = "Integer Scale (3.5x)"; break;
        case SCALE_4X: preview = "Integer Scale (4x)"; break;
        case SCALE_4_5X: preview = "Integer Scale (4.5x)"; break;
        case SCALE_5X: preview = "Integer Scale (5x)"; break;
        case SCALE_WIN_HEIGHT: preview = "Scale to Window Height"; break;
        case SCALE_WIN_HEIGHT_HALF: preview = "Scale to Window Height (Half-Steps)"; break;
        case SCALE_WIN_WIDTH_HEIGHT: preview = "Scale to Window Width & Height"; break;
    }

    ImGui::SetNextItemWidth(gui_combo_width_for_text("Scale to Window Height (Half-Steps)"));
    if (!ImGui::BeginCombo("##scale", preview))
        return;

    const auto& policy = video_output_policy(output, debug_output);
    const auto item = [&output, &policy](const char* label, int value)
    {
        if (!policy.Allows("SCALE", value)) return;
        if (ImGui::Selectable(label, policy.Resolve("SCALE", output.scale) == value))
            output.scale = value;
    };
    if (!debug_output)
        item("Integer Scale (Auto)", SCALE_INTEGER);
    item("Integer Scale (1x)", SCALE_1X);
    item("Integer Scale (1.5x)", SCALE_1_5X);
    item("Integer Scale (2x)", SCALE_2X);
    item("Integer Scale (2.5x)", SCALE_2_5X);
    item("Integer Scale (3x)", SCALE_3X);
    item("Integer Scale (3.5x)", SCALE_3_5X);
    item("Integer Scale (4x)", SCALE_4X);
    item("Integer Scale (4.5x)", SCALE_4_5X);
    item("Integer Scale (5x)", SCALE_5X);
    if (!debug_output)
    {
        ImGui::Separator();
        item("Scale to Window Height", SCALE_WIN_HEIGHT);
        item("Scale to Window Height (Half-Steps)", SCALE_WIN_HEIGHT_HALF);
        item("Scale to Window Width & Height", SCALE_WIN_WIDTH_HEIGHT);
    }
    ImGui::EndCombo();
}

static void menu_video_output_overscan(config_VideoOutput& output, bool debug)
{
    const auto& policy = video_output_policy(output, debug);
    if (!policy.values.count("OVERSCAN")) return;
    if (!ImGui::BeginMenu("Overscan")) return;
    static const char* labels[] = {"Disabled", "Top+Bottom", "Full (narrow border)", "Full (wide border)", "Full Frame"};
    for (int value = 0; value < 5; ++value)
        if (policy.Allows("OVERSCAN", value) && ImGui::MenuItem(labels[value], "", policy.Resolve("OVERSCAN", output.overscan) == value)) {
            output.overscan = value;
            if (debug || !config_debug.debug || config_debug.video_as_main_settings) apply_video_output_source(output);
        }
    ImGui::EndMenu();
}

static void menu_video_output_aspect(config_VideoOutput& output, bool debug)
{
    const auto& policy = video_output_policy(output, debug);
    if (!policy.values.count("ASPECT_RATIO") || (!debug && policy.Resolve("SCALE", output.scale) == SCALE_WIN_WIDTH_HEIGHT)) return;
    if (!ImGui::BeginMenu("Aspect Ratio")) return;
    static const char* labels[] = {"Device Pixel Aspect Ratio", "Standard (4:3 DAR)", "Wide (16:9 DAR)"};
    for (int value = 0; value < 3; ++value)
        if (policy.Allows("ASPECT_RATIO", value) && ImGui::MenuItem(labels[value], "", policy.Resolve("ASPECT_RATIO", output.ratio) == value)) output.ratio = value;
    ImGui::EndMenu();
}

static void menu_video_output_postprocessing(config_VideoOutput& output)
{
    const bool debug = &output == &config_debug.video;
    const auto& policy = video_output_policy(output, debug);
    const auto effective = ResolveVideoOutput(output, emu_get_core()->GetActiveVideoOptionCode(), debug, emu_get_video_frame_descriptor());
    const auto supported = [&](int value) { return policy.Allows("POST_PROCESSING", value) && renderer_postprocessing_supported(value); };
    const auto item = [&](const char* label, int value) {
        if (supported(value) && ImGui::MenuItem(label, "", effective.postprocessing == value)) output.postprocessing = value;
    };
    item("None", POSTPROCESSING_NONE);
    item("Bilinear", POSTPROCESSING_BILINEAR);
    item("Advanced Scaling", POSTPROCESSING_ADVANCED_SCALING);
    item("CRT (Lottes)", POSTPROCESSING_CRT_LOTTES);
    if (effective.postprocessing != output.postprocessing)
        ImGui::TextDisabled("Saved filter is unavailable for this output");
    ImGui::Separator();
    if (supported(POSTPROCESSING_ADVANCED_SCALING) && ImGui::MenuItem("Configure Advanced Scaling...")) {
        video_setup_profile = &output;
        show_advanced_scaling_setup = true;
    }
    if (supported(POSTPROCESSING_CRT_LOTTES) && ImGui::MenuItem("Configure CRT (Lottes)...")) {
        video_setup_profile = &output;
        show_crt_lottes_setup = true;
    }
}


static bool endsWith(std::string const& str, std::string const& suffix) {
    if (str.length() < suffix.length()) {
        return false;
    }
    return str.compare(str.length() - suffix.length(), suffix.length(), suffix) == 0;
}


void MemoryExport(const char* dstFilename, u8* memoryPtr, u32 srcOffset, int length)
{
    u8* src = memoryPtr + srcOffset;

    std::string pathstr(dstFilename);

    if (src != 0 && !endsWith(pathstr, "\\") && !endsWith(pathstr, "//"))
    {
        if (FILE* file = fopen(pathstr.c_str(), "wb"))
        {
            fwrite(src, 1, length, file);
            fclose(file);
        }
    }

}


int MemoryImport(const char* fname, u8* memoryPtr, u32 dstOffset, int maxLength)
{
    // dstOffset >= maxLength must be rejected before the subtraction below:
    // with dstOffset now a wider u32 (large ROMs need offsets past 0xFFFF),
    // an out-of-range offset would make maxLength - dstOffset wrap around in
    // unsigned arithmetic instead of going negative, which used to be caught
    // by the (harmless, u16-only) sign of the old int-vs-u16 subtraction.
    if (maxLength <= 0 || dstOffset >= (u32)maxLength)
        return 0;

    u8* dst = memoryPtr + dstOffset;

    int maxSize = maxLength - (int)dstOffset;
    int fileSize = 0;

    std::string pathstr(fname);

    if (dst != 0 && maxSize > 0 && !endsWith(pathstr, "\\") && !endsWith(pathstr, "//"))
    {
        if (FILE* file = fopen(pathstr.c_str(), "rb"))
        {
            fseek(file, 0, 2);
            fileSize = (int)ftell(file);
            fseek(file, 0, 0);

            if (fileSize > 0)
            {
                if (fileSize > maxSize) fileSize = maxSize;
                fread(dst, 1, fileSize, file);
            }
            fclose(file);
        }
    }

    return fileSize;

}


static void set_style(void)
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImGuiIO& io = ImGui::GetIO();

    // Rebuild every style field from ImGui defaults before applying the
    // selected scale. StyleColorsDark() only resets colors; using it alone
    // would leave newer spacing/size fields scaled and compound subsequent
    // adjustments made while the UI menu is open.
    style = ImGuiStyle();
    ImGui::StyleColorsDark(&style);

    // The default ImGui font draws window titles, menus, tabs and controls.
    // Debugger data explicitly pushes gui_default_font/gui_unifont_font and
    // therefore stays at its selected, readable fixed size.
    if (config_debug.ui_density == 1 && compact_chrome_font)
        io.FontDefault = compact_chrome_font;
    else if (config_debug.ui_density == 2 && condensed_chrome_font)
        io.FontDefault = condensed_chrome_font;
    else
        io.FontDefault = gui_roboto_font;

    style.Alpha = 1.0f;
    style.DisabledAlpha = 0.6000000238418579f;
    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.WindowRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowMinSize = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 4.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = ImVec2(4.0f, 3.0f);
    style.FrameRounding = 2.5f;
    style.FrameBorderSize = 0.0f;
    style.ItemSpacing = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    style.CellPadding = ImVec2(4.0f, 2.0f);
    style.IndentSpacing = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    style.ScrollbarSize = 11.0f;
    style.ScrollbarRounding = 2.5f;
    style.GrabMinSize = 10.0f;
    style.GrabRounding = 2.0f;
    style.TabRounding = 3.5f;
    style.TabBorderSize = 0.0f;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.0f);

    // Compact debug UI deliberately keeps the fonts unchanged: the
    // monospace debugger data and the emulated SC-3000 output remain at
    // their readable sizes. Only the surrounding ImGui chrome is condensed.
    if (config_debug.ui_density == 1)
    {
        style.WindowPadding = ImVec2(5.0f, 5.0f);
        style.WindowRounding = 3.0f;
        style.FramePadding = ImVec2(3.0f, 1.0f);
        style.FrameRounding = 2.0f;
        style.ItemSpacing = ImVec2(6.0f, 2.0f);
        style.ItemInnerSpacing = ImVec2(3.0f, 2.0f);
        style.CellPadding = ImVec2(3.0f, 1.0f);
        style.IndentSpacing = 17.0f;
        style.ColumnsMinSpacing = 4.0f;
        style.ScrollbarSize = 9.0f;
        style.GrabMinSize = 8.0f;
        style.TabRounding = 2.0f;
    }
    else if (config_debug.ui_density == 2)
    {
        style.WindowPadding = ImVec2(6.5f, 6.5f);
        style.WindowRounding = 3.5f;
        style.FramePadding = ImVec2(3.5f, 2.0f);
        style.FrameRounding = 2.25f;
        style.ItemSpacing = ImVec2(7.0f, 3.0f);
        style.ItemInnerSpacing = ImVec2(3.5f, 3.0f);
        style.CellPadding = ImVec2(3.5f, 1.5f);
        style.IndentSpacing = 19.0f;
        style.ColumnsMinSpacing = 5.0f;
        style.ScrollbarSize = 10.0f;
        style.GrabMinSize = 9.0f;
        style.TabRounding = 2.75f;
    }

    // SDL3 supplies framebuffer scaling to the renderer. Dear ImGui needs
    // the separate logical content scale for its fonts and style geometry.
    const float ui_scale = application_content_scale * config_debug.ui_scale;
    style.ScaleAllSizes(ui_scale);
    style.FontScaleDpi = application_content_scale;
    style.FontScaleMain = config_debug.ui_scale;

    //style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    //style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.5921568870544434f, 0.5921568870544434f, 0.5921568870544434f, 1.0f);
    //style.Colors[ImGuiCol_WindowBg] = ImVec4(0.060085229575634f, 0.060085229575634f, 0.06008583307266235f, 1.0f);
    //style.Colors[ImGuiCol_ChildBg] = ImVec4(0.05882352963089943f, 0.05882352963089943f, 0.05882352963089943f, 1.0f);
    //style.Colors[ImGuiCol_PopupBg] = ImVec4(0.1176470592617989f, 0.1176470592617989f, 0.1176470592617989f, 1.0f);
    //style.Colors[ImGuiCol_Border] = ImVec4(0.1802574992179871f, 0.1802556961774826f, 0.1802556961774826f, 1.0f);
    //style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.3058823645114899f, 0.3058823645114899f, 0.3058823645114899f, 1.0f);
    //style.Colors[ImGuiCol_FrameBg] = ImVec4(0.1843137294054031f, 0.1843137294054031f, 0.1843137294054031f, 1.0f);
    //style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.270386278629303f, 0.2703835666179657f, 0.2703848779201508f, 1.0f);
    //style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.8745098114013672f, 0.007843137718737125f, 0.3882353007793427f, 1.0f);
    //style.Colors[ImGuiCol_TitleBg] = ImVec4(0.1450980454683304f, 0.1450980454683304f, 0.1490196138620377f, 1.0f);
    //style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.8745098114013672f, 0.007843137718737125f, 0.3882353007793427f, 1.0f);
    //style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.1450980454683304f, 0.1450980454683304f, 0.1490196138620377f, 1.0f);
    //style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.1176470592617989f, 0.1176470592617989f, 0.1176470592617989f, 1.0f);
    //style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.1176470592617989f, 0.1176470592617989f, 0.1176470592617989f, 1.0f);
    //style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.6266094446182251f, 0.6266031861305237f, 0.6266063451766968f, 1.0f);
    //style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.9999899864196777f, 0.9999899864196777f, 1.0f, 1.0f);
    //style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.9999899864196777f, 0.9999899864196777f, 1.0f, 1.0f);
    //style.Colors[ImGuiCol_CheckMark] = ImVec4(0.8745098114013672f, 0.007843137718737125f, 0.3882353007793427f, 1.0f);
    //style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.8745098114013672f, 0.007843137718737125f, 0.3882353007793427f, 1.0f);
    //style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.8745098114013672f, 0.007843137718737125f, 0.3882353007793427f, 1.0f);
    //style.Colors[ImGuiCol_Button] = ImVec4(0.184547483921051f, 0.184547483921051f, 0.1845493316650391f, 1.0f);
    //style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.8745098114013672f, 0.007843137718737125f, 0.3882353007793427f, 1.0f);
    //style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.8745098114013672f, 0.007843137718737125f, 0.3882353007793427f, 1.0f);
    //style.Colors[ImGuiCol_Header] = ImVec4(0.1843137294054031f, 0.1843137294054031f, 0.1843137294054031f, 1.0f);
    //style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.8745098114013672f, 0.007843137718737125f, 0.3882353007793427f, 1.0f);
    //style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.8745098114013672f, 0.007843137718737125f, 0.3882353007793427f, 1.0f);
    //style.Colors[ImGuiCol_Separator] = ImVec4(0.1803921610116959f, 0.1803921610116959f, 0.1803921610116959f, 1.0f);
    //style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.1803921610116959f, 0.1803921610116959f, 0.1803921610116959f, 1.0f);
    //style.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.1803921610116959f, 0.1803921610116959f, 0.1803921610116959f, 1.0f);
    //style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.2489270567893982f, 0.2489245682954788f, 0.2489245682954788f, 1.0f);
    //style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(1.0f, 0.9999899864196777f, 0.9999899864196777f, 1.0f);
    //style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(1.0f, 0.9999899864196777f, 0.9999899864196777f, 1.0f);
    //style.Colors[ImGuiCol_Tab] = ImVec4(0.1450980454683304f, 0.1450980454683304f, 0.1490196138620377f, 1.0f);
    //style.Colors[ImGuiCol_TabHovered] = ImVec4(0.8745098114013672f, 0.007843137718737125f, 0.3882353007793427f, 1.0f);
    //style.Colors[ImGuiCol_TabActive] = ImVec4(0.8755365014076233f, 0.00751531170681119f, 0.3875076174736023f, 1.0f);
    //style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.1450980454683304f, 0.1450980454683304f, 0.1490196138620377f, 1.0f);
    //style.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.8745098114013672f, 0.007843137718737125f, 0.3882353007793427f, 1.0f);
    //style.Colors[ImGuiCol_PlotLines] = ImVec4(0.8745098114013672f, 0.007843137718737125f, 0.3882353007793427f, 1.0f);
    //style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.8745098114013672f, 0.007843137718737125f, 0.3882353007793427f, 1.0f);
    //style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.8745098114013672f, 0.007843137718737125f, 0.3882353007793427f, 1.0f);
    //style.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.8745098114013672f, 0.007843137718737125f, 0.3882353007793427f, 1.0f);
    //style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.1882352977991104f, 0.1882352977991104f, 0.2000000029802322f, 1.0f);
    //style.Colors[ImGuiCol_TableBorderStrong] = ImVec4(0.3098039329051971f, 0.3098039329051971f, 0.3490196168422699f, 1.0f);
    //style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.2274509817361832f, 0.2274509817361832f, 0.2470588237047195f, 1.0f);
    //style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    //style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.05999999865889549f);
    //style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.8745098114013672f, 0.007843137718737125f, 0.3882353007793427f, 1.0f);
    //style.Colors[ImGuiCol_DragDropTarget] = ImVec4(0.1450980454683304f, 0.1450980454683304f, 0.1490196138620377f, 1.0f);
    //style.Colors[ImGuiCol_NavHighlight] = ImVec4(0.1450980454683304f, 0.1450980454683304f, 0.1490196138620377f, 1.0f);
    //style.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.699999988079071f);
    //style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.800000011920929f, 0.800000011920929f, 0.800000011920929f, 0.2000000029802322f);
    //style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.1450980454683304f, 0.1450980454683304f, 0.1490196138620377f, 0.7f);

    //style.Colors[ImGuiCol_DockingPreview] = style.Colors[ImGuiCol_HeaderActive] * ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    //style.Colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    //style.Colors[ImGuiCol_TabHovered] = style.Colors[ImGuiCol_HeaderHovered];
    ////style.Colors[ImGuiCol_Tab] = lerp(style.Colors[ImGuiCol_Header], style.Colors[ImGuiCol_TitleBgActive], 0.80f);
    //style.Colors[ImGuiCol_TabSelected] = lerp(style.Colors[ImGuiCol_HeaderActive], style.Colors[ImGuiCol_TitleBgActive], 0.60f);
    //style.Colors[ImGuiCol_TabSelectedOverline] = style.Colors[ImGuiCol_HeaderActive];
    //style.Colors[ImGuiCol_TabDimmed] = lerp(style.Colors[ImGuiCol_Tab], style.Colors[ImGuiCol_TitleBg], 0.80f);
    //style.Colors[ImGuiCol_TabDimmedSelected] = lerp(style.Colors[ImGuiCol_TabSelected], style.Colors[ImGuiCol_TitleBg], 0.40f);
    //style.Colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
}

static void gui_load_disc(const char* disc_path)
{
    application_mount_disk(disc_path, config_emulator.disc_write_protected);
}


static void gui_load_cassette(const char* cassette_path)
{
    application_load_tape(cassette_path);
}


ImFont* gui_get_font(int index)
{
    return default_font[index & 3];
}

float gui_get_default_font_size(void)
{
    return 13.0f + (static_cast<float>(config_debug.font_size & 3) * 3.0f);
}

void gui_set_next_window_size_em(float width_em, float height_em, ImGuiCond cond)
{
    const float em = ImGui::GetFontSize();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float max_width = std::max(1.0f, viewport->WorkSize.x);
    const float max_height = std::max(1.0f, viewport->WorkSize.y);
    ImGui::SetNextWindowSize(ImVec2(std::min(width_em * em, max_width),
                                    std::min(height_em * em, max_height)), cond);
}

float gui_combo_width_for_text(const char* longest_item)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float text_width = longest_item ? ImGui::CalcTextSize(longest_item).x : 0.0f;
    return text_width + ImGui::GetFrameHeight() + (style.FramePadding.x * 2.0f)
        + (ImGui::GetFontSize() * 0.75f);
}

float gui_combo_width_for_items(const char* items_separated_by_zeros)
{
    float width = 0.0f;
    if (items_separated_by_zeros)
    {
        for (const char* item = items_separated_by_zeros; *item; item += std::strlen(item) + 1)
            width = std::max(width, gui_combo_width_for_text(item));
    }
    return width;
}

float gui_combo_width_for_array(const char* const items[], int item_count)
{
    float width = 0.0f;
    for (int i = 0; items && i < item_count; ++i)
        width = std::max(width, gui_combo_width_for_text(items[i]));
    return width;
}


void gui_init(void)
{
    if (NFD_Init() != NFD_OKAY)
    {
        Log("NFD Error: %s", NFD_GetError());
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();

    io.IniFilename = config_imgui_file_path;

    // Panels dock into tabs on Shift+drag; no multi-viewport (that would need
    // ImGui_ImplSDLGPU3 to render into separate OS windows, which it does not
    // support yet). This is why the vendored ImGui.h/.cpp/_internal.h/
    // _widgets.cpp/_tables.cpp/_draw.cpp/imstb_textedit.h are the
    // v1.92.5-docking tag rather than plain v1.92.5: the docking
    // implementation itself is ~6000 lines inside imgui.cpp, not a flag on
    // top of the regular build.
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDockingWithShift = true;

    gui_roboto_font = io.Fonts->AddFontFromMemoryCompressedTTF(RobotoLight_compressed_data, RobotoLight_compressed_size, 18.0f);
    compact_chrome_font = io.Fonts->AddFontFromMemoryCompressedTTF(RobotoLight_compressed_data, RobotoLight_compressed_size, 15.0f);
    condensed_chrome_font = io.Fonts->AddFontFromMemoryCompressedTTF(RobotoLight_compressed_data, RobotoLight_compressed_size, 16.5f);
    gui_sc3000_font = io.Fonts->AddFontFromMemoryCompressedTTF(SC3000Basic_compressed_data, SC3000Basic_compressed_size, 16.0f);
    gui_unifont_font = io.Fonts->AddFontFromMemoryCompressedTTF(unifont_compressed_data, unifont_compressed_size, 16.0f);

    // Same compact Material Icons set used by the current GearSystem
    // debugger.  Keep it as an independent font: debugger data remains in
    // the selected fixed font while toolbars stay compact at every density.
    const float icon_font_size = 20.0f;
    static const ImWchar icon_ranges[] = { ICON_MIN_MD, ICON_MAX_16_MD, 0 };
    ImFontConfig icon_config;
    icon_config.PixelSnapH = true;
    icon_config.GlyphMinAdvanceX = icon_font_size;
    icon_config.GlyphOffset = ImVec2(0.0f, 2.0f);
    gui_material_icons_font = io.Fonts->AddFontFromMemoryCompressedTTF(
        MaterialIcons_compressed_data, MaterialIcons_compressed_size,
        icon_font_size, &icon_config, icon_ranges);

    ImFontConfig font_cfg;

    for (int i = 0; i < 4; i++)
    {
        font_cfg.SizePixels = (13.0f + (i * 3));
        default_font[i] = io.Fonts->AddFontDefault(&font_cfg);
    }

    gui_default_font = default_font[config_debug.font_size];

    // A dedicated, genuinely condensed data font for the memory editor.
    // It is kept separate from the UI font: menus, buttons and popups still
    // follow the Normal / Compact / Minimal density setting.
#if defined(_WIN32)
    WindowsResourceView memory_font;
    WindowsResourceView memory_light_font;
    if (load_windows_resource(IDR_MEMORY_CONDENSED_TTF, &memory_font))
    {
        ImFontConfig memory_font_config;
        memory_font_config.PixelSnapH = true;
        memory_font_config.FontDataOwnedByAtlas = false;
        gui_memory_condensed_font = io.Fonts->AddFontFromMemoryTTF(
            const_cast<unsigned char*>(memory_font.data),
            static_cast<int>(memory_font.size),
            16.0f, &memory_font_config);
    }
    if (load_windows_resource(IDR_MEMORY_CONDENSED_LIGHT_TTF,
                              &memory_light_font))
    {
        ImFontConfig memory_light_font_config;
        memory_light_font_config.PixelSnapH = true;
        memory_light_font_config.FontDataOwnedByAtlas = false;
        gui_memory_condensed_light_font = io.Fonts->AddFontFromMemoryTTF(
            const_cast<unsigned char*>(memory_light_font.data),
            static_cast<int>(memory_light_font.size),
            16.0f, &memory_light_font_config);
    }
#else
    const char* base_path = SDL_GetBasePath();
    if (base_path != NULL)
    {
        char memory_font_path[4096];
        char memory_light_font_path[4096];
#if defined(__APPLE__)
        const int path_length = SDL_snprintf(memory_font_path, sizeof(memory_font_path),
            "%s../Resources/GearSF7000Mono-Condensed.ttf", base_path);
        const int light_path_length = SDL_snprintf(memory_light_font_path, sizeof(memory_light_font_path),
            "%s../Resources/GearSF7000Mono-CondensedLight.ttf", base_path);
#else
        const int path_length = SDL_snprintf(memory_font_path, sizeof(memory_font_path),
            "%sGearSF7000Mono-Condensed.ttf", base_path);
        const int light_path_length = SDL_snprintf(memory_light_font_path, sizeof(memory_light_font_path),
            "%sGearSF7000Mono-CondensedLight.ttf", base_path);
#endif
        if (path_length >= 0 && static_cast<size_t>(path_length) < sizeof(memory_font_path))
        {
            ImFontConfig memory_font_config;
            memory_font_config.PixelSnapH = true;
            gui_memory_condensed_font = io.Fonts->AddFontFromFileTTF(
                memory_font_path, 16.0f, &memory_font_config);
        }
        if (light_path_length >= 0 && static_cast<size_t>(light_path_length) < sizeof(memory_light_font_path))
        {
            ImFontConfig memory_light_font_config;
            memory_light_font_config.PixelSnapH = true;
            gui_memory_condensed_light_font = io.Fonts->AddFontFromFileTTF(
                memory_light_font_path, 16.0f, &memory_light_font_config);
        }
    }
#endif


    set_style();

    // The AY expansion is staged in emu_init(), not here: gui_init() runs
    // after it, and the machine can already have reset by this point.

    // BIOS, overscan, disc write protection and the audio mute now live in
    // emu_apply_startup_config(), so the headless front end starts the same
    // machine this one does.
    emu_apply_startup_config();

    strcpy(bios_path, config_emulator.bios_path.c_str());
    strcpy(disc_path, config_emulator.disc_path.c_str());
    strcpy(cassette_path, config_emulator.sr1000_tape.path.c_str());
    strcpy(savefiles_path, config_emulator.savefiles_path.c_str());
    strcpy(savestates_path, config_emulator.savestates_path.c_str());

    // No renderer_set_crt_test_enabled call here either - see the Debug
    // menu's "Show CRT Test Pass" comment. If it was persisted enabled,
    // the debug window dispatcher already calls debug_window_crt_test_pass
    // (and so syncs it) on the very first frame.

    // Mounting the configured media moved to
    // application_apply_startup_media(), so a headless session inserts the
    // same disc and cassette this one does.
    application_apply_startup_media();
    
}

void gui_destroy(void)
{
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    NFD_Quit();
}

bool gui_render(void)
{
    // Rebuild style/font defaults between frames, with no active windows or
    // style stacks. Editing the settings never changes the controls in flight.
    if (apply_interface_settings)
    {
        config_debug.ui_scale = pending_interface_scale;
        config_debug.font_size = pending_interface_font;
        config_debug.ui_density = pending_interface_density;
        gui_default_font = default_font[config_debug.font_size];
        set_style();
        apply_interface_settings = false;
        restore_interface_settings_position = true;
    }
    ImGui::NewFrame();

    gui_in_use = dialog_in_use;
    
    main_menu();

    gui_main_window_hovered = false;

    bool mainWindowfocused = false;
    if ((!config_debug.debug && !emu_is_empty()) || (config_debug.debug && config_debug.show_screen))
    {
        mainWindowfocused = main_window();
    }

    gui_debug_windows();

    if (config_emulator.show_info)
        show_info();

    window_crt_lottes_setup();
    window_advanced_scaling_setup();

    // Configuration dialogs belong above the emulated screen and debugger
    // windows. Drawing this before main_window() made the screen cover most
    // of the keyboard even though the dialog itself was open.
    if (show_sk1100_keyboard_config)
        window_sk1100_keyboard_config();
    if (show_hotkey_config)
        window_hotkey_config();
    if (show_interface_settings)
        window_interface_settings();
    popup_modal_input_binding();

    show_status_message();

    ImGui::Render();

    return mainWindowfocused;
}

void gui_shortcut(gui_ShortCutEvent event)
{
    switch (event)
    {  
    case gui_ShortcutOpenROM:
        shortcut_open_rom = true;
        break;
    case gui_ShortcutReloadROM:
    {
        const char* path = emu_get_core()->GetCartridge()->GetFilePath();
        if (path && path[0])
        {
            const std::string current_path(path);
            if (application_load_rom(current_path.c_str()))
                gui_set_status_message("ROM reloaded", 3000);
            else
                gui_set_status_message("ROM reload failed", 3000);
        }
        else
            gui_set_status_message("No ROM to reload", 3000);
        break;
    }
    case gui_ShortcutReset:
        menu_reset();
        break;
    case gui_ShortcutPause:
        menu_pause();
        break;
    case gui_ShortcutFFWD:
        emu_set_fast_forward(!config_emulator.ffwd);
        menu_ffwd();
        break;
    case gui_ShortcutSaveState:
    {
        std::string message("Saving state to slot ");
        message += std::to_string(config_emulator.save_slot + 1);
        gui_set_status_message(message.c_str(), 3000);
        emu_save_state_slot(config_emulator.save_slot + 1);
        break;
    }
    case gui_ShortcutLoadState:
    {
        std::string message("Loading state from slot ");
        message += std::to_string(config_emulator.save_slot + 1);
        gui_set_status_message(message.c_str(), 3000);
        emu_load_state_slot(config_emulator.save_slot + 1);
        break;
    }
    case gui_ShortcutScreenshot:
        call_save_screenshot(NULL);
        break;
    case gui_ShortcutFullscreen:
        config_emulator.fullscreen = !config_emulator.fullscreen;
        application_trigger_fullscreen(config_emulator.fullscreen);
        break;
#if GEARSF7000_ENABLE_DEBUG_TOOLS
    case gui_ShortcutDebugStepInto:
        if (config_debug.debug)
            emu_debug_step_into();
        break;
    case gui_ShortcutDebugStepOver:
        if (config_debug.debug)
            emu_debug_step_over();
        break;
    case gui_ShortcutDebugStepLine:
        if (config_debug.debug)
            emu_debug_step_line();
        break;
    case gui_ShortcutDebugBreak:
        if (config_debug.debug)
            emu_debug_break();
        break;
    case gui_ShortcutDebugContinue:
        if (config_debug.debug || emu_get_core()->GetMachineType() == MachineType::SordM5 || emu_get_core()->GetMachineType() == MachineType::MSX ||
            emu_get_core()->GetMachineType() == MachineType::Coleco)
            menu_continue();
        break;
    case gui_ShortcutDebugContinueFromHere:
        if (config_debug.debug)
            gui_debug_rewind_resume_from_here();
        break;
    case gui_ShortcutDebugNextFrame:
        if (config_debug.debug)
            emu_debug_next_frame();
        break;
    case gui_ShortcutDebugPreviousFrame:
        if (config_debug.debug)
            gui_debug_rewind_step_back();
        break;
    case gui_ShortcutDebugBreakpoint:
        if (config_debug.debug)
            gui_debug_toggle_breakpoint();
        break;
    case gui_ShortcutDebugRuntocursor:
        if (config_debug.debug)
            gui_debug_runtocursor();
        break;
    case gui_ShortcutDebugGoBack:
        if (config_debug.debug)
            gui_debug_go_back();
        break;
    case gui_ShortcutDebugCopy:
        gui_debug_copy_memory();
        break;
    case gui_ShortcutDebugPaste:
        gui_debug_paste_memory();
        break;
#endif
    case gui_ShortcutShowMainMenu:
        config_emulator.show_menu = !config_emulator.show_menu;
        break;
    default:
        break;
    }
}

void gui_load_rom(const char* path)
{
    application_load_rom(path);
}


void gui_load_rom_none()
{
    application_start_sf7000();
}

void gui_set_status_message(const char* message, u32 milliseconds)
{
    if (config_emulator.status_messages)
    {
        strcpy(status_message, message);
        status_message_active = true;
        status_message_start_time = SDL_GetTicks();
        status_message_duration = milliseconds;
    }
}

#if GEARSF7000_ENABLE_AY
static void gui_apply_ay_config(void)
{
    emu_set_ay_config(config_audio.ay_enable, config_audio.ay_chip,
                      config_audio.ay_port_base);
}
#endif
#if GEARSF7000_ENABLE_YM2413
static void gui_apply_ym2413_config(void)
{
    emu_set_ym2413_config(config_audio.ym2413_enable);
}
#endif

// Drops one entry from a recent list and closes the gap, so the list keeps its
// most-recent-first order with the empty slots at the end - the same shape
// push_recent_* maintains and config_write expects.
static void recent_list_remove(std::string* entries, int count, int index)
{
    for (int i = index; i < count - 1; i++)
        entries[i] = entries[i + 1];
    entries[count - 1].clear();
    // Persist straight away: forgetting a path is an explicit instruction, and
    // it should survive even if the emulator never gets a clean shutdown.
    config_write();
}

// Preserve the distinguishing tail of a path when a recent-file submenu
// would otherwise occupy most of the display. Cuts only at a path separator,
// following the same rule used by Dual's recent-path popup.
static std::string recent_path_label(const std::string& path, float max_width)
{
    if (ImGui::CalcTextSize(path.c_str()).x <= max_width)
        return path;

    // Inside the user's home, start from its first meaningful directory:
    // /Users/name/Documents/... becomes /Documents/... . On a drive-letter
    // path keep the drive root and apply the same rule.
    std::string visible_path = path;
    if (const char* home_folder = SDL_GetUserFolder(SDL_FOLDER_HOME))
    {
        std::string home(home_folder);
        if (!home.empty() && visible_path.compare(0, home.size(), home) == 0)
        {
            const std::string relative = visible_path.substr(home.size());
            if (home.size() >= 3 && home[1] == ':' &&
                (home[2] == '/' || home[2] == '\\'))
                visible_path = home.substr(0, 3) + relative;
            else
                visible_path = "/" + relative;
        }
    }

    size_t component_start = 0;
    if (!visible_path.empty() && (visible_path[0] == '/' || visible_path[0] == '\\'))
        component_start = 1;
    else if (visible_path.size() >= 3 && visible_path[1] == ':' &&
             (visible_path[2] == '/' || visible_path[2] == '\\'))
        component_start = 3;

    const size_t first_separator = visible_path.find_first_of("/\\", component_start);
    if (first_separator == std::string::npos)
        return visible_path;

    const std::string prefix = visible_path.substr(0, first_separator);
    const char separator_char = visible_path[first_separator];
    for (size_t separator = first_separator;
         separator != std::string::npos;
         separator = visible_path.find_first_of("/\\", separator + 1))
    {
        const std::string candidate = prefix + separator_char + "..." +
            visible_path.substr(separator);
        if (ImGui::CalcTextSize(candidate.c_str()).x <= max_width)
            return candidate;
    }

    // A single very long filename cannot be shortened at a component
    // boundary. Keep it intact rather than cutting a UTF-8 sequence.
    const size_t last_separator = visible_path.find_last_of("/\\");
    return last_separator == std::string::npos
        ? visible_path
        : prefix + separator_char + "..." + visible_path.substr(last_separator);
}

// Renders one "Open Recent" list: each row is the path plus a right-aligned
// delete button. Returns the index the user chose to open, or -1.
//
// The rows are laid out from the widest path rather than from the available
// content region, because a menu window sizes itself from the previous frame's
// content and would make the button jump on the first frame it is opened.
static int recent_list_menu(std::string* entries, int count)
{
    const float row_height = ImGui::GetFrameHeight();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = style.ItemSpacing.x;
    const float menu_width = ImGui::GetMainViewport()->WorkSize.x * 0.50f;
    const float menu_chrome_width = row_height + spacing +
        (style.WindowPadding.x * 2.0f);
    const float max_label_width = std::max(ImGui::GetFontSize() * 12.0f,
        menu_width - menu_chrome_width);

    std::vector<std::string> labels(static_cast<size_t>(count));

    float label_width = 0.0f;
    for (int i = 0; i < count; i++)
        if (entries[i].length() > 0)
        {
            labels[static_cast<size_t>(i)] = recent_path_label(entries[i], max_label_width);
            label_width = std::max(label_width,
                ImGui::CalcTextSize(labels[static_cast<size_t>(i)].c_str()).x);
        }

    int chosen = -1;
    int remove = -1;

    for (int i = 0; i < count; i++)
    {
        if (entries[i].length() == 0)
            continue;

        ImGui::PushID(i);
        const ImVec2 row_min = ImGui::GetCursorScreenPos();

        // Fixed row size, so neither the menu width nor the row height can
        // change when the button appears: the list would shift under the
        // pointer that is trying to aim at it.
        const std::string& label = labels[static_cast<size_t>(i)];
        if (ImGui::Selectable("##recent_entry", false,
                ImGuiSelectableFlags_AllowOverlap,
                ImVec2(label_width, row_height)))
            chosen = i;
        const float text_y = row_min.y + (row_height - ImGui::GetFontSize()) * 0.5f;
        ImGui::GetWindowDrawList()->AddText(ImVec2(row_min.x, text_y),
            ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
        if (label != entries[i] && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", entries[i].c_str());

        const ImVec2 row_max(row_min.x + label_width + spacing + row_height,
                             row_min.y + row_height);
        const bool row_hovered = ImGui::IsMouseHoveringRect(row_min, row_max);

        ImGui::SameLine(label_width + spacing);
        // Reserve the square unconditionally so the menu keeps its width when
        // the cross is not being drawn.
        const ImVec2 button_pos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(row_height, row_height));

        if (row_hovered)
        {
            // CloseButton draws its cross from the draw list at exactly
            // FontSize square, so centre it inside the reserved row.
            const float inset = (row_height - ImGui::GetFontSize()) * 0.5f;
            // A close button does not dismiss the menu, so several entries can
            // be forgotten in one visit.
            if (ImGui::CloseButton(ImGui::GetID("##forget"),
                    ImVec2(button_pos.x + inset, button_pos.y + inset)))
                remove = i;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Remove from the recent list.\n"
                                  "The file itself is not touched.");
        }

        ImGui::PopID();
    }

    if (remove >= 0)
    {
        recent_list_remove(entries, count, remove);
        // The row the click landed on has just been replaced by the next one.
        chosen = -1;
    }

    return chosen;
}

// One line per open window in the Windows menu below. Focuses the
// window instead of the hide/show round trip that was the only way to find
// a panel buried under others - docking helps for panels grouped as tabs,
// but nothing stops a floating window from still covering another.
static void window_menu_item(const char* label, bool is_open, const char* window_id)
{
    if (!is_open)
        return;
    if (ImGui::MenuItem(label))
        ImGui::SetWindowFocus(window_id);
}

// Informational content is deliberately smaller and muted, below a separator.
static void firmware_note(const char* text)
{
    ImGui::PushFont(gui_default_font, gui_get_default_font_size() * 0.85f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 360.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}
static void firmware_file_label(const std::string& path, const char* emptyLabel = "No external BIOS selected")
{
    const auto slash = path.find_last_of("/\\");
    std::string name = path.empty() ? emptyLabel : path.substr(slash == std::string::npos ? 0 : slash + 1);
    const float maxWidth = 360.0f;
    bool trimmed = false;
    while (name.size() > 1 && ImGui::CalcTextSize(name.c_str()).x > maxWidth) {
        name.erase(0, 1);
        while (!name.empty() && (static_cast<unsigned char>(name[0]) & 0xC0) == 0x80) name.erase(0, 1);
        trimmed = true;
    }
    if (trimmed) name = "..." + name;
    ImGui::TextDisabled("%s", name.c_str());
    if (!path.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
}

static void main_menu(void)
{
    bool open_rom = false;

    bool open_ram = false;
    bool open_basic_program = false;
    bool save_basic_program = false;
    bool save_ram = false;
    bool open_state = false;
    bool save_state = false;
    bool open_about = false;
    bool open_symbols = false;
    bool save_screenshot = false;
    bool choose_savestates_path = false;
    bool open_bios = false;
    bool open_bios_warning = false;
    bool open_coleco_bios = false;
    bool open_m5_bios = false;
    bool open_msx_bios = false;
    bool open_disc = false;
    bool open_cassette = false;
    bool select_msx_cassette = false;

    gui_main_menu_hovered = false;

    if (config_emulator.show_menu && ImGui::BeginMainMenuBar())
    {
        gui_main_menu_hovered = ImGui::IsWindowHovered();

        //if (ImGui::BeginMenu(GEARSF7000_TITLE))
        if (ImGui::BeginMenu("File"))
        {
            gui_in_use = true;

            if (ImGui::MenuItem("Open ROM...", config_hotkeys[config_HotkeyIndex_OpenROM].str))
            {
                open_rom = true;
            }

            if (ImGui::MenuItem("Reload Current ROM",
                                config_hotkeys[config_HotkeyIndex_ReloadROM].str,
                                false, !emu_is_empty()))
                gui_shortcut(gui_ShortcutReloadROM);



            if (ImGui::BeginMenu("Open Recent"))
            {
                const int chosen = recent_list_menu(config_emulator.recent_roms,
                                                    config_max_recent_roms);
                if (chosen >= 0)
                {
                    char rom_path[4096];
                    snprintf(rom_path, sizeof(rom_path), "%s",
                             config_emulator.recent_roms[chosen].c_str());
                    gui_load_rom(rom_path);
                }

                ImGui::EndMenu();
            }

            if (ImGui::MenuItem("Eject", "", false, !emu_is_empty()))
            {
                menu_eject();
            }

            ImGui::Separator();

            if (emu_get_core()->HasKeyboardInterface() && emu_get_core()->GetMachineType() != MachineType::MSX && emu_get_core()->GetMachineType() != MachineType::SordM5 && ImGui::BeginMenu("BASIC"))
            {
                // A .bas is already tokenised, so it can go straight into the
                // program area. A .basic is source text, which BASIC has to
                // tokenise itself - that is what the typer and the cassette are
                // for.
                if (ImGui::MenuItem("Load BASIC Program...", "", false, !emu_is_empty()))
                {
                    open_basic_program = true;
                }

                if (ImGui::MenuItem("Save BASIC Program As...", "", false, !emu_is_empty()))
                {
                    save_basic_program = true;
                }

                // Where BASIC keeps its pointer block differs per BASIC, so it is
                // picked here rather than assumed. Auto scans for it and proceeds
                // only on a single match, which covers both known layouts without
                // the user having to look anything up; the named entries are for
                // pinning a choice, and the scan results are listed so a BASIC
                // nobody has measured yet can be found from here too.
                if (ImGui::BeginMenu("BASIC Pointer Block", !emu_is_empty()))
                {
                    int& chosen = config_debug.basic_pointer_block;

                    if (ImGui::MenuItem("Auto (single match)", "", chosen == 0))
                        chosen = 0;
                    ImGui::Separator();
                    if (ImGui::MenuItem("$8160  Level III cartridge", "", chosen == 0x8160))
                        chosen = 0x8160;
                    if (ImGui::MenuItem("$9954  Disk BASIC", "", chosen == 0x9954))
                        chosen = 0x9954;

                    u16 found[8];
                    const int count = emu_find_basic_blocks(found, 8);
                    bool header = false;
                    for (int i = 0; i < count; i++)
                    {
                        if (found[i] == 0x8160 || found[i] == 0x9954)
                            continue;
                        if (!header)
                        {
                            ImGui::Separator();
                            ImGui::TextDisabled("Found in this machine");
                            header = true;
                        }
                        char label[32];
                        snprintf(label, sizeof(label), "$%04X", found[i]);
                        if (ImGui::MenuItem(label, "", chosen == found[i]))
                            chosen = found[i];
                    }

                    if (count == 0)
                    {
                        ImGui::Separator();
                        ImGui::TextDisabled("Nothing consistent found right now");
                    }

                    ImGui::EndMenu();
                }

                ImGui::EndMenu();
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Reset", config_hotkeys[config_HotkeyIndex_Reset].str))
            {
                menu_reset();
            }

            if (emu_get_core()->GetMachineType() != MachineType::MSX && emu_get_core()->GetMachineType() != MachineType::SordM5 &&
                ImGui::MenuItem("Soft Reset (NMI)", "F1"))
            {
                emu_pause_key_pressed();
            }

            if (ImGui::MenuItem("Pause", config_hotkeys[config_HotkeyIndex_Pause].str, &config_emulator.paused))
            {
                menu_pause();
            }

            ImGui::Separator();

            bool ffwd_menu = config_emulator.ffwd;
            if (ImGui::MenuItem("Fast Forward", config_hotkeys[config_HotkeyIndex_FFWD].str, &ffwd_menu))
            {
                emu_set_fast_forward(ffwd_menu);
                menu_ffwd();
            }

            if (ImGui::BeginMenu("Fast Forward Speed"))
            {
                ImGui::PushItemWidth(gui_combo_width_for_text("Unlimited"));
                ImGui::Combo("##fwd", &config_emulator.ffwd_speed, "X 1.5\0X 2\0X 2.5\0X 3\0Unlimited\0\0");
                ImGui::PopItemWidth();
                ImGui::EndMenu();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Save State As...")) 
            {
                save_state = true;
            }

            if (ImGui::MenuItem("Load State From..."))
            {
                open_state = true;
            }

            ImGui::Separator();

            if (ImGui::BeginMenu("Save State Slot"))
            {
                ImGui::PushItemWidth(gui_combo_width_for_text("Slot 5"));
                ImGui::Combo("##slot", &config_emulator.save_slot, "Slot 1\0Slot 2\0Slot 3\0Slot 4\0Slot 5\0\0");
                ImGui::PopItemWidth();
                ImGui::EndMenu();
            }

            if (ImGui::MenuItem("Save State", "Ctrl+S")) 
            {
                std::string message("Saving state to slot ");
                message += std::to_string(config_emulator.save_slot + 1);
                gui_set_status_message(message.c_str(), 3000);
                emu_save_state_slot(config_emulator.save_slot + 1);
            }

            if (ImGui::MenuItem("Load State", "Ctrl+L"))
            {
                std::string message("Loading state from slot ");
                message += std::to_string(config_emulator.save_slot + 1);
                gui_set_status_message(message.c_str(), 3000);
                emu_load_state_slot(config_emulator.save_slot + 1);
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Save Screenshot As..."))
            {
                save_screenshot = true;
            }

            if (ImGui::MenuItem("Save Screenshot", "Ctrl+X"))
            {
                call_save_screenshot(NULL);
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Interface Settings..."))
            {
                pending_interface_scale = config_debug.ui_scale;
                pending_interface_font = config_debug.font_size;
                pending_interface_density = config_debug.ui_density;
                show_interface_settings = true;
                ImGui::SetWindowFocus("Interface Settings");
            }

#ifdef DEBUG_TOOLS
            if (ImGui::MenuItem("Debug Mode", "", &config_debug.debug))
            {
                apply_video_output_source(config_video_output_for_mode(config_debug.debug));
                if (config_debug.debug)
                    emu_debug_step();
                else
                    menu_continue();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Show debugging tools and enable local analysis.\n"
                    "When off, collection is suspended unless MCP is running.\n"
                    "Recording starts a new history after an unrecorded interval.");
#endif

            ImGui::Separator();

            if (ImGui::MenuItem("Quit", ""))
            {
                application_trigger_quit();
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Emulator"))
        {
            gui_in_use = true;

            if (ImGui::BeginMenu("Machine"))
            {
                const auto running = emu_get_core()->GetSystemModel();
                ImGui::TextDisabled("Active: %s", GetMachineMenuGroup(running).label);
                ImGui::Separator();
                for (const auto& group : GetMachineMenuGroups()) {
                    if (!MachineMenuGroupAvailable(group)) continue;
                    const auto target = config_machine_group_selection(group);
                    if (ImGui::MenuItem(group.label, "", MachineGroupContains(group, static_cast<SystemModel>(config_emulator.machine)),
                            MachineMenuModelAvailable(target))) {
                        config_select_machine(target);
                        menu_reset();
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Board configuration"))
            {
                const auto running = emu_get_core()->GetSystemModel();
                ImGui::TextDisabled("Active: %s", GetSystemModelName(running));
                ImGui::TextDisabled("VDP is selected separately");
                ImGui::Separator();
                const auto& group = GetMachineMenuGroup(static_cast<SystemModel>(config_emulator.machine));
                for (const auto model : group.models) {
                    if (!MachineMenuModelAvailable(model)) continue;
                    if (ImGui::MenuItem(GetSystemModelName(model), model == running ? "Active" : "Restart",
                            config_emulator.machine == static_cast<int>(model))) {
                        config_select_machine(model);
                        menu_reset();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Board CPU/PSG clock: %.6f MHz",
                        GetSystemModelDescriptor(model).cpuPsgClockHz / 1000000.0);
                }
                ImGui::EndMenu();
            }

            // The selected VDP determines the video standard and its native
            // timing. It is not a geographical region setting.
            if (ImGui::BeginMenu("VDP"))
            {
                GearSF7000Core* core = emu_get_core();
                const auto* active = GetVideoCatalog().FindOption(core->GetActiveVideoOptionCode());
                ImGui::TextDisabled("Active: %s", active ? active->label.c_str() : "Unknown");
                ImGui::Separator();

                SystemModel requested = static_cast<SystemModel>(config_emulator.machine);
                if (!IsValidSystemModel(requested)) requested = core->GetSystemModel();
                const auto rows = BuildVideoMenuOptions(GetMachineVideoOptions(requested),
                    GetDefaultMachineVideoOption(requested), config_selected_video_option(),
                    core->GetActiveVideoOptionCode(), core->IsUsingDefaultVideoDevice(),
                    requested == core->GetSystemModel());
                if (rows.empty()) ImGui::TextDisabled("No video core available in this build");
                for (const auto& row : rows)
                {
                    ImGui::PushID(row.code.empty() ? "machine-default" : row.code.c_str());
                    if (ImGui::MenuItem(row.label.c_str(), row.applied ? "Active" : "Reset to apply", row.selected))
                    {
                        if (config_select_video_option(row.code)) config_emulator.ffwd = false;
                    }
                    ImGui::PopID();
                    if (row.code.empty()) ImGui::Separator();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Mapper"))
            {
                const auto* cart = emu_get_core()->GetCartridge();
                ImGui::TextDisabled("System: %s", GetSystemModelName(emu_get_core()->GetSystemModel()));
                if (cart->IsReady())
                {
                    ImGui::TextDisabled("Active: %s", emu_get_mapper_name(cart->GetType()));
                    const char* reason = nullptr;
                    if (!Cartridge::IsTypeCompatible(cart->GetType(), cart->GetROMSize(), &reason))
                        ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", reason);
                    if (cart->GetType() == Cartridge::COLECO_MEGACART)
                        ImGui::TextDisabled("%d KiB, %d banks; selected: %d",
                            cart->GetROMSize() / 1024, cart->GetROMBankCount(),
                            emu_get_core()->GetMemory()->GetRomBank());
                }
                ImGui::TextDisabled("SGM is an expansion: select it under System");
                ImGui::Separator();
                ImGui::PushItemWidth(gui_combo_width_for_text("Auto (detect)"));
                ImGui::Combo("##mapper_mode", &config_emulator.mapper_mode,
                    "Auto (detect)\0Manual\0\0");
                ImGui::PopItemWidth();

                ImGui::Separator();

                static const struct { Cartridge::CartridgeTypes type; const char* name; } kTypes[] = {
                    { Cartridge::SC3000_FLAT64K, "Flat 64K (RAM)" },
                    { Cartridge::SC3000_ASC16L,  "ASC16L" },
                    { Cartridge::SC3000_ASC16LS, "Internal" },
                    { Cartridge::SC3000_32K,     "SC-3000 (32K RAM)" },
                    { Cartridge::SC3000_16K_AT_8000, "SC-3000 (16K cart + console RAM)" },
                    { Cartridge::SC3000_2K,      "SC-3000 standard (console RAM at 8000)" },
                    { Cartridge::SG1000_16K,     "SG-1000 (16K RAM)" },
                    { Cartridge::SG1000_1K,      "SG-1000 standard (console RAM)" },
                    { Cartridge::SG1000_DAHJEE_TYPE_A, "SG-1000 Dahjee A (8K ext + console RAM)" },
                    { Cartridge::SG1000_DAHJEE_TYPE_B, "SG-1000 Dahjee B (8K at C000)" },
                    { Cartridge::SC3000_SURVIVORS_MULTICART, "SC-3000 Survivors Multicart (2MB)" },
                    { Cartridge::SC3000_SURVIVORS_MEGACART, "SC-3000 Survivors Megacart (4MB)" },
                    { Cartridge::SG1000_OTHELLO_2K, "SG-1000 Othello (2K cart RAM)" },
                    { Cartridge::SG1000_CASTLE_8K, "SG-1000 The Castle (8K cart + console RAM)" },
                    { Cartridge::SG1000_SEGA_315_5208, "SG-1000 Sega 315-5208 (console RAM)" },
                    { Cartridge::SG1000_SEGA_315_5208_DAHJEE_B, "SG-1000 Sega 315-5208 + Dahjee B (8K)" },
                    { Cartridge::SMS_SEGA_MAPPER, "Master System Sega mapper" },
                    { Cartridge::MSX_LINEAR, "MSX linear (8/16/32K)" },
                    { Cartridge::SORD_M5_LINEAR, "Sord M5 standard (up to 20 KB)" },
                    { Cartridge::COLECOVISION,   "ColecoVision standard (32K at 8000)" },
                    { Cartridge::COLECO_MEGACART, "Coleco MegaCart (128K-1MB)" },
                };

                if (config_emulator.mapper_mode == 0)
                {
                    ImGui::TextDisabled("Signatures, CRC database and mapper heuristics decide");
                    const Cartridge::CartridgeTypes fallback =
                        config_fallback_cartridge_type();
                    const char* fallbackName = "Unknown";
                    for (const auto& entry : kTypes)
                        if (entry.type == fallback)
                            fallbackName = entry.name;
                    ImGui::TextDisabled("Machine default: %s", fallbackName);
                }
                else
                {
                    // The loaded ROM's size, if there is one, decides which
                    // entries make sense. Incompatible ones stay visible with
                    // the reason beside them: a greyed-out item says nothing
                    // about why it is greyed out.
                    GearSF7000Core* core = emu_get_core();
                    Cartridge* cart = core ? core->GetCartridge() : NULL;
                    const int romSize = (cart && cart->IsReady()) ? cart->GetROMSize() : 0;

                    for (const auto& entry : kTypes)
                    {
                        if (IsMSXSystem(static_cast<SystemModel>(config_emulator.machine)) && entry.type != Cartridge::MSX_LINEAR) continue;
                        if (IsM5System(static_cast<SystemModel>(config_emulator.machine)) && entry.type != Cartridge::SORD_M5_LINEAR) continue;
                        if (!Cartridge::IsTypeBuilt(entry.type)) continue;
                        const char* reason = NULL;
                        const bool fits = romSize == 0
                            || Cartridge::IsTypeCompatible(entry.type, romSize, &reason);

                        if (ImGui::MenuItem(entry.name, "",
                                config_emulator.mapper_type == (int)entry.type))
                            config_emulator.mapper_type = (int)entry.type;

                        if (!fits && reason)
                        {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "  %s", reason);
                        }
                    }

                    ImGui::Separator();
                    ImGui::TextDisabled("no signature or checksum check");
                }

                ImGui::TextDisabled("applies on load or reset");
                ImGui::EndMenu();
            }

            //ImGui::Separator();

            //if (ImGui::BeginMenu("BIOS"))
            //{
            //    if (ImGui::MenuItem("Load BIOS..."))
            //    {
            //        open_bios = true;
            //    }
            //    ImGui::PushItemWidth(350);
            //    if (ImGui::InputText("##bios_path", bios_path, IM_ARRAYSIZE(bios_path), ImGuiInputTextFlags_AutoSelectAll))
            //    {
            //        config_emulator.bios_path.assign(bios_path);
            //        emu_load_bios(bios_path);
            //    }
            //    ImGui::PopItemWidth();
            //    ImGui::EndMenu();
            //}

            ImGui::Separator();

            if (ImGui::BeginMenu("Save State Location"))
            {
                ImGui::PushItemWidth(gui_combo_width_for_text("Savestates In Custom Folder"));
                if (ImGui::Combo("##savestate_option", &config_emulator.savestates_dir_option, "Savestates In Custom Folder\0Savestates In ROM Folder\0\0"))
                {
                    emu_savestates_dir_option = config_emulator.savestates_dir_option;
                }

                if (config_emulator.savestates_dir_option == 0)
                {
                    if (ImGui::MenuItem("Choose Savestate Folder..."))
                    {
                        choose_savestates_path = true;
                    }

                    ImGui::PushItemWidth(350);
                    if (ImGui::InputText("##savestate_path", savestates_path, IM_ARRAYSIZE(savestates_path), ImGuiInputTextFlags_AutoSelectAll))
                    {
                        config_emulator.savestates_path.assign(savestates_path);
                        strcpy(emu_savestates_path, savestates_path);
                    }
                    ImGui::PopItemWidth();
                }

                ImGui::EndMenu();
            }

            ImGui::Separator();

            ImGui::MenuItem("Show ROM info", "", &config_emulator.show_info);

            ImGui::MenuItem("Status Messages", "", &config_emulator.status_messages);

            ImGui::EndMenu();
        }




        // Menus of the emulated machine's own hardware: present when the
        // running board has the device, absent otherwise, never greyed.
#if GEARSF7000_ENABLE_SR1000
        if (emu_get_core()->HasCassetteInterface() &&
            emu_get_core()->GetMachineType() != MachineType::MSX && emu_get_core()->GetMachineType() != MachineType::SordM5 &&
            ImGui::BeginMenu("SR-1000"))
        {
            gui_in_use = true;

            // Tape transport is a normal computer peripheral, not a debugger.
            ImGui::MenuItem("Cassette Controls", "", &config_debug.show_cassette);

            if (ImGui::BeginMenu("Cassette"))
            {
                if (ImGui::MenuItem("Load Cassette..."))
                {
                    open_cassette = true;
                }

                ImGui::PushItemWidth(550);
                if (ImGui::InputText("##cassette_path", cassette_path, IM_ARRAYSIZE(cassette_path), ImGuiInputTextFlags_AutoSelectAll))
                {
                    gui_load_cassette(cassette_path);
                }
                ImGui::PopItemWidth();

                if (ImGui::MenuItem("Cassette write protected", "", &config_emulator.sr1000_tape.write_protected))
                {
                    emu_writeprotect_cassette(config_emulator.sr1000_tape.write_protected);
                }


                if (ImGui::MenuItem("Eject Cassette"))
                {
                    strcpy(cassette_path, "");
                    config_emulator.sr1000_tape.path.assign(cassette_path);
                    emu_eject_cassette();

                    //WavPlayer *player = emu_get_core()->GetAudio()->GetWavPlayer();
                    //player->SetSampleOnChannel(0, 3);
                    //player->PlayChannel(0);
                }

                if (ImGui::BeginMenu("Open Recent"))
                {
                    const int chosen = recent_list_menu(
                        config_emulator.sr1000_tape.recent,
                        config_max_recent_cassettes);
                    if (chosen >= 0)
                    {
                        snprintf(cassette_path, sizeof(cassette_path), "%s",
                                 config_emulator.sr1000_tape.recent[chosen].c_str());
                        gui_load_cassette(cassette_path);
                    }

                    ImGui::EndMenu();
                }




                ImGui::EndMenu();
            }

            ImGui::EndMenu();
        }
#endif // GEARSF7000_ENABLE_SR1000




#if GEARSF7000_ENABLE_SF7000
        if (emu_get_core()->CanAttachSF7000() &&
            ImGui::BeginMenu("SF-7000"))
        {
            gui_in_use = true;

            if (ImGui::MenuItem("Start SF-7000"))
            {
                if (!application_start_sf7000())
                    open_bios_warning = true;
            }

            ImGui::Separator();

            if (ImGui::BeginMenu("IPL"))
            {
                if (ImGui::MenuItem("Load IPL..."))
                {
                    open_bios = true;
                }
                ImGui::PushItemWidth(450);
                if (ImGui::InputText("##bios_path", bios_path, IM_ARRAYSIZE(bios_path), ImGuiInputTextFlags_AutoSelectAll))
                {
                    config_emulator.bios_path.assign(bios_path);
                    emu_load_bios(bios_path);
                }
                ImGui::PopItemWidth();
                ImGui::EndMenu();
            }

            ImGui::Separator();

            if (ImGui::BeginMenu("Disc"))
            {
                if (ImGui::MenuItem("Load Disc..."))
                {
                    open_disc = true;
                }

                ImGui::PushItemWidth(550);
                if (ImGui::InputText("##disc_path", disc_path, IM_ARRAYSIZE(disc_path), ImGuiInputTextFlags_AutoSelectAll))
                {
                    config_emulator.disc_path.assign(disc_path);
//                    emu_load_disc(disc_path);

                    gui_load_disc(disc_path);

                }
                ImGui::PopItemWidth();

                if (ImGui::MenuItem("Disc write protected", "", &config_emulator.disc_write_protected))
                {
                    emu_writeprotect_disc(config_emulator.disc_write_protected);
                }


                if (ImGui::MenuItem("Eject Disc"))
                {
                    strcpy(disc_path, "");
                    config_emulator.disc_path.assign(disc_path);
                    emu_eject_disc();

                    //WavPlayer *player = emu_get_core()->GetAudio()->GetWavPlayer();
                    //player->SetSampleOnChannel(0, 3);
                    //player->PlayChannel(0);
                }

                if (ImGui::BeginMenu("Open Recent"))
                {
                    const int chosen = recent_list_menu(
                        config_emulator.recent_discs, config_max_recent_discs);
                    if (chosen >= 0)
                    {
                        snprintf(disc_path, sizeof(disc_path), "%s",
                                 config_emulator.recent_discs[chosen].c_str());
                        gui_load_disc(disc_path);
                    }

                    ImGui::EndMenu();
                }




                ImGui::EndMenu();
            }

            ImGui::EndMenu();
        }
#endif // GEARSF7000_ENABLE_SF7000

        const bool msxMachine = emu_get_core()->GetMachineType() == MachineType::MSX;
        const bool m5Machine = emu_get_core()->GetMachineType() == MachineType::SordM5;
        const bool colecoMachine = emu_get_core()->GetMachineType() == MachineType::Coleco;
        if ((msxMachine || colecoMachine || m5Machine) && ImGui::BeginMenu(msxMachine ? "MSX1" : m5Machine ? "Sord M5" : "ColecoVision"))
        {
            gui_in_use = true;
            if (ImGui::MenuItem(msxMachine ? "Start MSX1" : m5Machine ? "Start Sord M5" : "Start ColecoVision")) {
                menu_reset();
                gui_debug_reset();
                if (!config_emulator.start_paused || !emu_debug_runtime_requested()) menu_continue();
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("BIOS")) {
                if (ImGui::MenuItem("Load BIOS...")) {
                    if (msxMachine) open_msx_bios = true;
                    else if (m5Machine) open_m5_bios = true;
                    else open_coleco_bios = true;
                }
                if (msxMachine && ImGui::MenuItem("Use C-BIOS", nullptr, &config_emulator.msx_use_cbios)) {
                    emu_reload_machine_firmware();
                    gui_debug_reset();
                }
                ImGui::Separator();
                firmware_file_label(msxMachine ? config_emulator.msx_bios_path : m5Machine ? config_emulator.m5_bios_path : config_emulator.coleco_bios_path);
                firmware_note(msxMachine && config_emulator.msx_use_cbios
                    ? "Active: built-in C-BIOS. Runs compatible cartridges; no full BASIC."
                    : msxMachine ? "External BIOS: 32 KB, normally BIOS + BASIC."
                    : m5Machine ? "External BIOS: 8 KB (e.g. sordint.ic21). BASIC requires a cartridge."
                    : "External BIOS: 8 KB. Can start without a cartridge.");
                if (!emu_get_machine_start_error().empty())
                    firmware_note(emu_get_machine_start_error().c_str());
                ImGui::EndMenu();
            }
            if (m5Machine) {
                ImGui::Separator();
                if (ImGui::MenuItem("EM-5 RAM (32 KB)", nullptr, &config_emulator.m5_em5_installed)) {
                    menu_reset();
                    gui_debug_reset();
                }
            }
            ImGui::EndMenu();
        }

#if GEARSF7000_ENABLE_SR1000
        // Machine menu first, followed by its peripherals.
        if (emu_get_core()->GetMachineType() == MachineType::MSX &&
            ImGui::BeginMenu("MSX Tape"))
        {
            gui_in_use = true;
            auto& tape = config_emulator.msx_tape;
            ImGui::MenuItem("Cassette Controls", nullptr, &config_debug.show_cassette);
            auto* recorder = emu_get_core()->GetCassette();
            if (ImGui::MenuItem("Play", nullptr, false, recorder->IsLoaded())) emu_cassette_play();
            if (ImGui::MenuItem("Stop", nullptr, false, recorder->IsLoaded())) emu_cassette_stop();
            if (ImGui::MenuItem("Rewind", nullptr, false, recorder->IsLoaded())) emu_cassette_rewind();
            if (ImGui::BeginMenu("Cassette"))
            {
                if (ImGui::MenuItem("Load Cassette..."))
                    select_msx_cassette = true;
                ImGui::Separator();
                firmware_file_label(tape.path, "No cassette selected");
                ImGui::Separator();
                ImGui::MenuItem("Cassette write protected", nullptr, true, false);
                if (ImGui::MenuItem("Eject Cassette", nullptr, false, !tape.path.empty() || recorder->IsLoaded()))
                {
                    emu_eject_cassette();
                    tape.path.clear();
                    config_write();
                }
                if (ImGui::BeginMenu("Open Recent"))
                {
                    const int chosen = recent_list_menu(tape.recent, config_max_recent_cassettes);
                    if (chosen >= 0 && !application_load_tape(tape.recent[chosen].c_str()))
                        gui_set_status_message("Unable to load MSX tape (WAV/MP3 supported)", 4000);
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            firmware_note("Start loading in BASIC, then press Play. Requires an external BIOS with BASIC.");
            ImGui::EndMenu();
        }
#endif // GEARSF7000_ENABLE_SR1000

        if (ImGui::BeginMenu("Video"))
        {
            gui_in_use = true;

            if (ImGui::MenuItem("Full Screen", "F12", &config_emulator.fullscreen))
            {
                application_trigger_fullscreen(config_emulator.fullscreen);
            }

            ImGui::MenuItem("Show Menu", "Ctrl+M", &config_emulator.show_menu);

            if (ImGui::MenuItem("Resize Window to Content"))
            {
                if (
                    !config_debug.debug && 
                    (config_video.ratio != 3))
                {
                    application_trigger_fit_to_content(main_window_width, main_window_height + main_menu_height);
                }
            }

            ImGui::Separator();

            if (ImGui::BeginMenu("Scale"))
            {
                menu_video_output_scale(config_video, false);
                ImGui::EndMenu();
            }

            menu_video_output_aspect(config_video, false);
            menu_video_output_overscan(config_video, false);

            ImGui::Separator();

            if (ImGui::BeginMenu("Frame Pacing"))
            {
                // The four combinations of two independent choices, laid out
                // the way they divide: the crystals (Accurate / Gaming) down
                // the list, the blank (off / on) across the separator. This
                // replaces a Vertical Sync checkbox that decided both at once
                // and a Screen Ghosting toggle that only ever made sense in
                // one of the four.
                //
                // A rate on every entry was the first attempt and it was
                // wrong twice over. It only ever takes two values - the rate
                // belongs to the clock axis, and VSync never changes it - so
                // four labels carrying it implied a variation that does not
                // exist. And in the case that matters most, PAL on a 60 Hz
                // panel, there is no whole ratio at all: Gaming cannot align,
                // so all four read 50.1590 Hz with nothing to say why.
                //
                // So the two rates are stated once, at the top, next to each
                // other, where the fact that they can coincide is visible and
                // explained instead of being a puzzle spread over four rows.
                const SchedulerDiagnostics pacing = scheduler_get_diagnostics();
                const double native = emu_get_native_frame_rate();
                const bool can_align = pacing.aligned_fps > 1.0;
                const double gaming = can_align ? pacing.aligned_fps : native;

                // What it is doing right now, in sentences.
                //
                // Every earlier attempt at this menu described the *choices* -
                // names, rates in parentheses - and none of them described the
                // state. So the same entry meant three different things
                // depending on region and monitor, silently: Gaming + VSync
                // locks in NTSC on 60 Hz, converts in PAL on the same panel,
                // and does neither at 144 Hz. Nothing on screen said which.
                //
                // The entries below are the question. This is the answer.
                const double refresh = pacing.measured_refresh > 1.0
                                           ? pacing.measured_refresh
                                           : pacing.display_hz;
                const bool converting =
                    config_video_pacing_aligned() && !pacing.display_locked;

                // The signal path, left to right: what the VDP produces, what
                // the monitor takes, and which of the two stages between them
                // are running. Green is on, grey is off - so "off" and "not
                // mentioned" cannot be confused, which they would be if the
                // tags only appeared when active.
                const ImVec4 on_colour(0.35f, 0.85f, 0.40f, 1.0f);

                // effective_fps, not machine_fps: what the engine is being
                // paced to right now, fast forward included. The two agree
                // during normal play and must not disagree in the one line
                // whose job is to say what is happening.
                //
                // With the deviation from the crystals, because "is the clock
                // being bent, and by how much" was otherwise only answerable
                // from a debug-only line that appears when it is bent and says
                // nothing when it is not - which is half the question.
                const double bend = (pacing.clock_scale - 1.0) * 100.0;
                if (std::fabs(bend) > 0.001)
                    ImGui::TextDisabled("TMS %.4f Hz (%+.2f%%)",
                                        pacing.effective_fps, bend);
                else
                    ImGui::TextDisabled("TMS %.4f Hz (crystals)",
                                        pacing.effective_fps);
                ImGui::SameLine();
                ImGui::TextDisabled("=>");
                ImGui::SameLine();
                ImGui::TextDisabled("Monitor %.2f Hz", refresh);
                ImGui::SameLine();
                if (converting)
                    ImGui::TextColored(on_colour, "  Converter");
                else
                    ImGui::TextDisabled("  Converter");
                ImGui::SameLine();
                if (config_video_vsync())
                    ImGui::TextColored(on_colour, "VSync");
                else
                    ImGui::TextDisabled("VSync");

                ImGui::Separator();

                struct PacingChoice
                {
                    int mode;
                    const char* name;
                    const char* tooltip;
                };

                const PacingChoice choices[] = {
                    { PACING_ACCURATE,       "Accurate",         "Real crystals. Exact timing, can tear." },
                    { PACING_GAMING,         "Gaming",           "Rate follows the display. Can tear." },
                    { -1, NULL, NULL },
                    { PACING_ACCURATE_VSYNC, "Accurate + VSync", "Real crystals. Exact timing, no tearing." },
                    { PACING_GAMING_VSYNC,   "Gaming + VSync",   "Rate follows the display. No tearing." },
                };

                const int previous = config_video.pacing;

                for (const PacingChoice& choice : choices)
                {
                    if (choice.name == NULL)
                    {
                        ImGui::Separator();
                        continue;
                    }

                    const bool aligned = (choice.mode & PACING_ALIGN_BIT) != 0;
                    const bool entry_vsync =
                        (choice.mode & PACING_VSYNC_BIT) != 0;
                    const double rate = aligned ? gaming : native;
                    const double gap = std::fabs(refresh - rate);

                    // The name carries VSync, so you can see which entries give
                    // it before choosing one. Nothing carried the converter,
                    // and it cannot go in the name: whether it runs depends on
                    // the display as well as the mode - Gaming + VSync locks in
                    // NTSC on 60 Hz and converts in PAL on the same panel, so a
                    // fixed name would be wrong in one of the two.
                    //
                    // So it is computed per entry and printed in the right-hand
                    // column: what this choice would actually do, here, read
                    // before making it rather than discovered after.
                    // One question per column, and this column's question is
                    // "does this entry turn the converter on". An interval in
                    // seconds on the Accurate rows answered a different one and
                    // left the reader to translate it into a judgement; the
                    // answer for those rows is simply no.
                    char effect[40];
                    if (aligned && entry_vsync && can_align)
                        // The only combination that locks, and the only one
                        // where the converter has nothing left to do.
                        snprintf(effect, sizeof(effect), "Locked 1:%d",
                                 (int)(refresh / rate + 0.5));
                    else if (aligned)
                        snprintf(effect, sizeof(effect), "Converter");
                    else
                        snprintf(effect, sizeof(effect), "No converter");

                    if (ImGui::MenuItem(choice.name, effect,
                                        config_video.pacing == choice.mode))
                        config_video.pacing = choice.mode;

                    // The cost of the choice, for whoever wants it: how often
                    // a presentation finds no new frame and shows the previous
                    // one again. Every 13 s is invisible, every 0.1 s is the
                    // staircase scrolling. Detail, so it lives on hover.
                    if (ImGui::IsItemHovered())
                    {
                        const double entry_bend =
                            (native > 1.0) ? (rate / native - 1.0) * 100.0 : 0.0;
                        if (!aligned && gap > 0.0001)
                            ImGui::SetTooltip("%s\nEngine %.4f Hz (crystals) - shows a frame "
                                              "twice every %.2f s",
                                              choice.tooltip, rate, 1.0 / gap);
                        else if (std::fabs(entry_bend) > 0.001)
                            ImGui::SetTooltip("%s\nEngine %.4f Hz (%+.2f%% from the crystals)",
                                              choice.tooltip, rate, entry_bend);
                        else
                            ImGui::SetTooltip("%s\nEngine %.4f Hz (crystals)",
                                              choice.tooltip, rate);
                    }
                }

                if (config_video.pacing != previous)
                {
                    // Vertical sync is presentation and takes effect now. The
                    // clock alignment is latched at Reset by design - every
                    // domain moves together or none does - so this only asks
                    // for it and the next reset applies it.
                    renderer_set_vsync(config_video_vsync());
                }

                // Only when a reset would actually change something, and
                // pointing at the Reset that already exists rather than
                // repeating it here - it is the same Ctrl+R, and a second
                // copy of a command is a second place to keep it correct.
                //
                // A permanent "applies at the next reset" would be noise in
                // the PAL case anyway, where the requested rate and the
                // running one are the same number and no reset changes that.
                const double requested =
                    config_video_pacing_aligned() ? gaming : native;
                if (std::fabs(requested - pacing.machine_fps) > 0.0001)
                {
                    ImGui::Separator();
                    ImGui::TextDisabled("Running at %.4f Hz - Ctrl+R to apply %.4f Hz",
                                        pacing.machine_fps, requested);
                }

                ImGui::EndMenu();
            }

            ImGui::MenuItem("Show FPS", "", &config_video.fps);
            ImGui::MenuItem("Show Frame Pacing Info", "", &config_video.frame_pacing_info);

            ImGui::Separator();

            if (ImGui::BeginMenu("Postprocessing"))
            {
                menu_video_output_postprocessing(config_video);
                ImGui::EndMenu();
            }
#ifdef SPRITE_EXPANDER
            if (ImGui::MenuItem("Disable Sprite Limit", "", &config_video.sprite_limit))
            {
                emu_video_no_sprite_limit(config_video.sprite_limit);
            }
#endif



            ImGui::Separator();

            const std::size_t colorProfileCount =
                emu_get_core()->GetVideoColorProfileCount();
            if (colorProfileCount > 0 && ImGui::BeginMenu("Palette"))
            {
                for (std::size_t index = 0; index < colorProfileCount; ++index)
                {
                    GC_VideoColorProfileDescriptor profile;
                    if (!emu_get_core()->DescribeVideoColorProfile(index, profile))
                        continue;
                    int* setting = emu_palette_setting();
                    const bool selected = *setting == profile.value;
                    if (ImGui::MenuItem(profile.label, nullptr, selected))
                    {
                        *setting = static_cast<int>(profile.value);
                        update_palette();
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Input"))
        {
            gui_in_use = true;

            // The menu is the board's: what the running system bus says it
            // offers is what appears. A board without a keyboard has no
            // Keyboard Mode to show, and the saved choice waits for a board
            // that has one.
            const ISystemBus* bus = emu_get_core()->GetSystemBus();
            const SystemInputDescriptor input =
                bus ? bus->DescribeInput() : kSegaJoypadInput;
            if (input.machineKeyboard)
            {
                bool keyboardMode = config_emulator.keyboard_mode;
                if (ImGui::MenuItem("Keyboard Mode", "", &keyboardMode))
                    emu_set_keyboard_mode(keyboardMode);
                if (emu_get_core()->GetMachineType() == MachineType::SordM5 && ImGui::BeginMenu("Sord M5 Keyboard")) {
                    firmware_note("Physical layout. Tab: FUNC; Escape: RESET; F5: Run.");
                    ImGui::Separator();
                    firmware_note("Left/right Shift are separate contacts. ISO < > maps to underscore.");
                    firmware_note("Fire 1/2 share the contacts of 1/2 (P1) and 5/6 (P2).");
                    ImGui::EndMenu();
                }
                if (emu_get_core()->GetMachineType() == MachineType::MSX &&
                    ImGui::BeginMenu("MSX International Keyboard"))
                {
                    ImGui::TextUnformatted("Physical key layout; key remapping is not available yet.");
                    ImGui::Separator();
                    ImGui::TextUnformatted("F1-F4 -> F1-F4; F5 has no host binding");
#ifdef __APPLE__
                    ImGui::TextUnformatted("Left Option -> GRAPH; Right Option -> CODE");
#else
                    ImGui::TextUnformatted("Left Alt -> GRAPH; Right Alt / AltGr -> CODE");
#endif
                    ImGui::TextUnformatted("ISO < > (left of Z) -> accent / DEAD");
                    ImGui::TextUnformatted("Home -> HOME; End / Insert -> INS");
                    ImGui::TextUnformatted("Page Up -> SELECT; Page Down -> STOP");
                    ImGui::TextUnformatted("Forward Delete -> DEL; Backspace -> BS");
#ifdef __APPLE__
                    ImGui::TextUnformatted("Mac Fn + arrows: Home / End / Page Up / Page Down");
                    ImGui::TextUnformatted("Mac Fn + Delete: Forward Delete");
                    ImGui::TextDisabled("Physical Mac Fn combinations still need verification.");
#endif
                    ImGui::EndMenu();
                }
                if (emu_get_core()->GetMachineType() == MachineType::MSX)
                    ImGui::TextDisabled("MSX joysticks are not implemented yet.");
                if (emu_get_core()->GetMachineType() != MachineType::MSX && emu_get_core()->GetMachineType() != MachineType::SordM5)
                    ImGui::MenuItem("SC-3000 Keyboard...", NULL,
                                &show_sk1100_keyboard_config);
            }

            ImGui::MenuItem("Configure Emulator Hotkeys...", NULL,
                            &show_hotkey_config);

            // What each controller port holds. Only the Master System family
            // drives TH, which a Mega Drive pad needs; the SG-1000 and SC-3000
            // keep their own joysticks.
            const bool smsPorts = emu_get_core()->GetMachineType() == MachineType::SMS;
            if (smsPorts && ImGui::BeginMenu("Controller Type"))
            {
                for (int player = 0; player < 2; ++player)
                {
                    if (!ImGui::BeginMenu(player == 0 ? "Player 1 (Port A)" : "Player 2 (Port B)"))
                        continue;
                    for (int d = 0; d <= static_cast<int>(SystemControllerDevice::MegaDrive6); ++d)
                        if (ImGui::MenuItem(GetControllerDeviceName(static_cast<SystemControllerDevice>(d)),
                                            "", config_input[player].controller_device == d))
                        {
                            emu_set_controller_device(player, d);
                            config_write();
                        }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();
            // The board names its two buttons; a Mega Drive pad in a port calls
            // the same two lines B and C, so that player's menus say both.
            char fire[2][2][48];
            for (int player = 0; player < 2; ++player)
            {
                const bool megaDrive = smsPorts && config_input[player].controller_device != 0;
                // Short: the label column is 100 pixels, and the board's own
                // names ("Fire 1 (Left)") already fill it.
                if (megaDrive)
                {
                    snprintf(fire[player][0], sizeof(fire[player][0]), "B (Fire 1):");
                    snprintf(fire[player][1], sizeof(fire[player][1]), "C (Fire 2):");
                }
                else
                {
                    snprintf(fire[player][0], sizeof(fire[player][0]), "%s:", input.fire1Name);
                    snprintf(fire[player][1], sizeof(fire[player][1]), "%s:", input.fire2Name);
                }
            }

            if (input.joypads > 0 && ImGui::BeginMenu("Keyboard"))
            {
                if (ImGui::BeginMenu("Player 1"))
                {
                    keyboard_configuration_item("Left:", &config_input[0].key_left, 0);
                    keyboard_configuration_item("Right:", &config_input[0].key_right, 0);
                    keyboard_configuration_item("Up:", &config_input[0].key_up, 0);
                    keyboard_configuration_item("Down:", &config_input[0].key_down, 0);
                    if (input.fire1Name[0]) keyboard_configuration_item(fire[0][0], &config_input[0].key_left_button, 0);
                    if (input.fire2Name[0]) keyboard_configuration_item(fire[0][1], &config_input[0].key_right_button, 0);
                    if (input.startName[0])
                        keyboard_configuration_item("Start:", &config_input[0].gg_start_key, 0);
                    if (smsPorts && config_input[0].controller_device != 0)
                    {
                        // The Mega Drive pad's buttons beyond B and C.
                        ImGui::Separator();
                        static char mdLabel[MD_PAD_BUTTON_COUNT][16];
                        for (int b = 0; b < MD_PAD_BUTTON_COUNT; ++b)
                        {
                            snprintf(mdLabel[b], sizeof(mdLabel[b]), "%s:", md_pad_layout[b].label);
                            keyboard_configuration_item(mdLabel[b], &config_input[0].md_key[b], 0);
                        }
                    }
                    if (input.joypadKeypad)
                    {
                        // The keypad is part of this controller (ColecoVision).
                        ImGui::Separator();
                        static char keypadLabel[COLECO_KEYPAD_KEY_COUNT][16];
                        for (int k = 0; k < COLECO_KEYPAD_KEY_COUNT; ++k)
                        {
                            snprintf(keypadLabel[k], sizeof(keypadLabel[k]), "Keypad %s:",
                                     coleco_keypad_layout[k].label);
                            keyboard_configuration_item(keypadLabel[k],
                                &config_input[0].keypad_key[k], 0);
                        }
                    }
                    //keyboard_configuration_item("Purple:", &config_input[0].key_purple, 0);
                    //keyboard_configuration_item("Blue:", &config_input[0].key_blue, 0);
                    //keyboard_configuration_item("Keypad 0:", &config_input[0].key_0, 0);
                    //keyboard_configuration_item("Keypad 1:", &config_input[0].key_1, 0);
                    //keyboard_configuration_item("Keypad 2:", &config_input[0].key_2, 0);
                    //keyboard_configuration_item("Keypad 3:", &config_input[0].key_3, 0);
                    //keyboard_configuration_item("Keypad 4:", &config_input[0].key_4, 0);
                    //keyboard_configuration_item("Keypad 5:", &config_input[0].key_5, 0);
                    //keyboard_configuration_item("Keypad 6:", &config_input[0].key_6, 0);
                    //keyboard_configuration_item("Keypad 7:", &config_input[0].key_7, 0);
                    //keyboard_configuration_item("Keypad 8:", &config_input[0].key_8, 0);
                    //keyboard_configuration_item("Keypad 9:", &config_input[0].key_9, 0);
                    //keyboard_configuration_item("Keypad *:", &config_input[0].key_asterisk, 0);
                    //keyboard_configuration_item("Keypad #:", &config_input[0].key_hash, 0);

                    ImGui::EndMenu();
                }

                if (input.joypads > 1 && ImGui::BeginMenu("Player 2"))
                {
                    keyboard_configuration_item("Left:", &config_input[1].key_left, 1);
                    keyboard_configuration_item("Right:", &config_input[1].key_right, 1);
                    keyboard_configuration_item("Up:", &config_input[1].key_up, 1);
                    keyboard_configuration_item("Down:", &config_input[1].key_down, 1);
                    if (input.fire1Name[0]) keyboard_configuration_item(fire[1][0], &config_input[1].key_left_button, 1);
                    if (input.fire2Name[0]) keyboard_configuration_item(fire[1][1], &config_input[1].key_right_button, 1);
                    if (smsPorts && config_input[1].controller_device != 0)
                    {
                        // The Mega Drive pad's buttons beyond B and C.
                        ImGui::Separator();
                        static char mdLabel[MD_PAD_BUTTON_COUNT][16];
                        for (int b = 0; b < MD_PAD_BUTTON_COUNT; ++b)
                        {
                            snprintf(mdLabel[b], sizeof(mdLabel[b]), "%s:", md_pad_layout[b].label);
                            keyboard_configuration_item(mdLabel[b], &config_input[1].md_key[b], 1);
                        }
                    }
                    if (input.joypadKeypad)
                    {
                        // The keypad is part of this controller (ColecoVision).
                        ImGui::Separator();
                        static char keypadLabel[COLECO_KEYPAD_KEY_COUNT][16];
                        for (int k = 0; k < COLECO_KEYPAD_KEY_COUNT; ++k)
                        {
                            snprintf(keypadLabel[k], sizeof(keypadLabel[k]), "Keypad %s:",
                                     coleco_keypad_layout[k].label);
                            keyboard_configuration_item(keypadLabel[k],
                                &config_input[1].keypad_key[k], 1);
                        }
                    }
                    //keyboard_configuration_item("Purple:", &config_input[1].key_purple, 1);
                    //keyboard_configuration_item("Blue:", &config_input[1].key_blue, 1);
                    //keyboard_configuration_item("Keypad 0:", &config_input[1].key_0, 1);
                    //keyboard_configuration_item("Keypad 1:", &config_input[1].key_1, 1);
                    //keyboard_configuration_item("Keypad 2:", &config_input[1].key_2, 1);
                    //keyboard_configuration_item("Keypad 3:", &config_input[1].key_3, 1);
                    //keyboard_configuration_item("Keypad 4:", &config_input[1].key_4, 1);
                    //keyboard_configuration_item("Keypad 5:", &config_input[1].key_5, 1);
                    //keyboard_configuration_item("Keypad 6:", &config_input[1].key_6, 1);
                    //keyboard_configuration_item("Keypad 7:", &config_input[1].key_7, 1);
                    //keyboard_configuration_item("Keypad 8:", &config_input[1].key_8, 1);
                    //keyboard_configuration_item("Keypad 9:", &config_input[1].key_9, 1);
                    //keyboard_configuration_item("Keypad *:", &config_input[1].key_asterisk, 1);
                    //keyboard_configuration_item("Keypad #", &config_input[1].key_hash, 1);

                    ImGui::EndMenu();
                }

                ImGui::EndMenu();
            }

            if (input.joypads > 0 && ImGui::BeginMenu("Gamepads"))
            {
                if (ImGui::BeginMenu("Player 1"))
                {
                    ImGui::MenuItem("Enable Gamepad P1", "", &config_input[0].gamepad);

                    if (ImGui::BeginMenu("Directional Controls"))
                    {
                        ImGui::PushItemWidth(gui_combo_width_for_text("Left Analog Stick"));
                        ImGui::Combo("##directional", &config_input[0].gamepad_directional, "D-pad\0Left Analog Stick\0\0");
                        ImGui::PopItemWidth();
                        ImGui::EndMenu();
                    }

                    if (ImGui::BeginMenu("Button Configuration"))
                    {
                        if (input.fire1Name[0]) gamepad_configuration_item(fire[0][0], &config_input[0].gamepad_left_button, 0);
                        if (input.fire2Name[0]) gamepad_configuration_item(fire[0][1], &config_input[0].gamepad_right_button, 0);
                        if (input.startName[0])
                            gamepad_configuration_item("Start:", &config_input[0].gamepad_gg_start_button, 0);
                        if (smsPorts && config_input[0].controller_device != 0)
                        {
                            ImGui::Separator();
                            static char mdLabel[MD_PAD_BUTTON_COUNT][16];
                            for (int b = 0; b < MD_PAD_BUTTON_COUNT; ++b)
                            {
                                snprintf(mdLabel[b], sizeof(mdLabel[b]), "%s:", md_pad_layout[b].label);
                                gamepad_configuration_item(mdLabel[b], &config_input[0].gamepad_md_button[b], 0);
                            }
                        }
                        //gamepad_configuration_item("Purple:", &config_input[0].gamepad_purple, 0);
                        //gamepad_configuration_item("Blue:", &config_input[0].gamepad_blue, 0);
                        //gamepad_configuration_item("Keypad 0:", &config_input[0].gamepad_0, 0);
                        //gamepad_configuration_item("Keypad 1:", &config_input[0].gamepad_1, 0);
                        //gamepad_configuration_item("Keypad 2:", &config_input[0].gamepad_2, 0);
                        //gamepad_configuration_item("Keypad 3:", &config_input[0].gamepad_3, 0);
                        //gamepad_configuration_item("Keypad 4:", &config_input[0].gamepad_4, 0);
                        //gamepad_configuration_item("Keypad 5:", &config_input[0].gamepad_5, 0);
                        //gamepad_configuration_item("Keypad 6:", &config_input[0].gamepad_6, 0);
                        //gamepad_configuration_item("Keypad 7:", &config_input[0].gamepad_7, 0);
                        //gamepad_configuration_item("Keypad 8:", &config_input[0].gamepad_8, 0);
                        //gamepad_configuration_item("Keypad 9:", &config_input[0].gamepad_9, 0);
                        //gamepad_configuration_item("Asterisk:", &config_input[0].gamepad_asterisk, 0);
                        //gamepad_configuration_item("Hash:", &config_input[0].gamepad_hash, 0);

                        ImGui::EndMenu();
                    }

                    ImGui::EndMenu();
                }

                if (input.joypads > 1 && ImGui::BeginMenu("Player 2"))
                {
                    ImGui::MenuItem("Enable Gamepad P2", "", &config_input[1].gamepad);

                    if (ImGui::BeginMenu("Directional Controls"))
                    {
                        ImGui::PushItemWidth(gui_combo_width_for_text("Left Analog Stick"));
                        ImGui::Combo("##directional", &config_input[1].gamepad_directional, "D-pad\0Left Analog Stick\0\0");
                        ImGui::PopItemWidth();
                        ImGui::EndMenu();
                    }

                    if (ImGui::BeginMenu("Button Configuration"))
                    {
                        if (input.fire1Name[0]) gamepad_configuration_item(fire[1][0], &config_input[1].gamepad_left_button, 1);
                        if (input.fire2Name[0]) gamepad_configuration_item(fire[1][1], &config_input[1].gamepad_right_button, 1);
                        if (smsPorts && config_input[1].controller_device != 0)
                        {
                            ImGui::Separator();
                            static char mdLabel[MD_PAD_BUTTON_COUNT][16];
                            for (int b = 0; b < MD_PAD_BUTTON_COUNT; ++b)
                            {
                                snprintf(mdLabel[b], sizeof(mdLabel[b]), "%s:", md_pad_layout[b].label);
                                gamepad_configuration_item(mdLabel[b], &config_input[1].gamepad_md_button[b], 1);
                            }
                        }
                        //gamepad_configuration_item("Purple:", &config_input[1].gamepad_purple, 1);
                        //gamepad_configuration_item("Blue:", &config_input[1].gamepad_blue, 1);
                        //gamepad_configuration_item("Keypad 0:", &config_input[1].gamepad_0, 1);
                        //gamepad_configuration_item("Keypad 1:", &config_input[1].gamepad_1, 1);
                        //gamepad_configuration_item("Keypad 2:", &config_input[1].gamepad_2, 1);
                        //gamepad_configuration_item("Keypad 3:", &config_input[1].gamepad_3, 1);
                        //gamepad_configuration_item("Keypad 4:", &config_input[1].gamepad_4, 1);
                        //gamepad_configuration_item("Keypad 5:", &config_input[1].gamepad_5, 1);
                        //gamepad_configuration_item("Keypad 6:", &config_input[1].gamepad_6, 1);
                        //gamepad_configuration_item("Keypad 7:", &config_input[1].gamepad_7, 1);
                        //gamepad_configuration_item("Keypad 8:", &config_input[1].gamepad_8, 1);
                        //gamepad_configuration_item("Keypad 9:", &config_input[1].gamepad_9, 1);
                        //gamepad_configuration_item("Asterisk:", &config_input[1].gamepad_asterisk, 1);
                        //gamepad_configuration_item("Hash:", &config_input[1].gamepad_hash, 1);

                        ImGui::EndMenu();
                    }

                    ImGui::EndMenu();
                }

                ImGui::EndMenu();
            }

            //if (ImGui::BeginMenu("Spinners"))
            //{
            //    ImGui::MenuItem("Capture Mouse", "F12", &config_emulator.capture_mouse);
            //    if (ImGui::IsItemHovered())
            //    {
            //        ImGui::SetTooltip("When enabled, the mouse will be captured inside\nthe emulator window to use spinners freely.\nPress F12 to release the mouse.");
            //    }

            //    ImGui::Combo("##spinner", &config_emulator.spinner, "Disabled\0Super Action Controller\0Steering Wheel\0Roller Controller\0\0", 4);
            //    if (ImGui::IsItemHovered())
            //    {
            //        ImGui::SetTooltip("· SAC Spinner for P1 is controlled with mouse movement.\n· SAC Spinner for P2 is controlled with mouse wheel.\n· Steering Wheel is controlled with mouse movement.\n· Roller Controller is controlled with mouse movement and mouse buttons.");
            //    }
            //    ImGui::SliderInt("##spinner_sensitivity", &config_emulator.spinner_sensitivity, 1, 10, "Sensitivity = %d");

            //    ImGui::EndMenu();
            //}

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Audio"))
        {
            gui_in_use = true;

            if (ImGui::MenuItem("Enable Audio", "", &config_audio.enable))
            {
                emu_audio_mute(!config_audio.enable);
            }

            ImGui::Separator();
            ImGui::TextDisabled("PSG: %s",
                emu_get_core()->GetAudio()->GetApu()->GetVariantName());

#if GEARSF7000_ENABLE_RECORDER
            // Register-write log, not a sample recording - see
            // emu_start_vgm_recording's comment. In the menu, not the debug
            // panel: Gearsystem puts it here too, and it is a capture someone
            // starts and later opens in a player, not something read live
            // while it runs.
            const bool vgm_recording = emu_is_vgm_recording();

            if (ImGui::MenuItem("Start VGM Recording...", "", false,
                                !vgm_recording && !emu_is_empty()))
            {
                file_dialog_save_vgm();
            }

            if (ImGui::MenuItem("Stop VGM Recording", "", false, vgm_recording))
            {
                const std::string path = emu_stop_vgm_recording();
                std::string message = "VGM recording stopped: " + path;
                gui_set_status_message(message.c_str(), 3000);
            }
#endif

#if GEARSF7000_ENABLE_AY
            ImGui::Separator();

            if (ImGui::BeginMenu("AY Expansion", emu_get_core()->GetMachineType() == MachineType::SC3000))
            {
                // The expansion is hardware, not an effect: fitting it,
                // removing it or swapping the chip only takes hold at the next
                // reset, so software which has already probed the port never
                // sees it change underneath. Audio latches the pending values
                // in Audio::Reset().
                if (ImGui::MenuItem("Enabled", "", &config_audio.ay_enable))
                    gui_apply_ay_config();

                ImGui::Separator();

                if (ImGui::MenuItem("AY-3-8910", "", config_audio.ay_chip == 0))
                {
                    config_audio.ay_chip = 0;
                    gui_apply_ay_config();
                }
                if (ImGui::MenuItem("YM2149", "", config_audio.ay_chip == 1))
                {
                    config_audio.ay_chip = 1;
                    gui_apply_ay_config();
                }

                ImGui::Separator();

                if (ImGui::BeginMenu("Port Base"))
                {
                    // $20 is the only block on a real SC-3000 bus where no
                    // machine IC is selected, and it keeps the MSX register
                    // layout with bit 7 cleared. The others are offered
                    // because the board this has to match is still being
                    // designed.
                    static const int bases[] = { 0x20, 0x24, 0x28, 0x2C, 0x30, 0x38 };
                    for (int base : bases)
                    {
                        char label[16];
                        snprintf(label, sizeof(label), "$%02X", base);
                        if (ImGui::MenuItem(label, "", config_audio.ay_port_base == base))
                        {
                            config_audio.ay_port_base = base;
                            gui_apply_ay_config();
                        }
                    }
                    ImGui::EndMenu();
                }

                ImGui::Separator();
                ImGui::TextDisabled("presence and chip apply at reset");

                ImGui::EndMenu();
            }
#endif

#if GEARSF7000_ENABLE_YM2413
            ImGui::Separator();
            if (ImGui::BeginMenu("YM2413 FM Sound Unit"))
            {
                if (ImGui::MenuItem("Connected", "",
                                    &config_audio.ym2413_enable))
                    gui_apply_ym2413_config();
                ImGui::Separator();
                ImGui::TextDisabled("Mark III / SMS FM interface; reset required");
                ImGui::TextDisabled("$F2 selects PSG or FM");
                ImGui::EndMenu();
            }
#endif

            ImGui::EndMenu();
        }

#ifdef DEBUG_TOOLS
        if (config_debug.debug && ImGui::BeginMenu("Debug"))
        {
            gui_in_use = true;

            ImGui::MenuItem("Start Paused", "", &config_emulator.start_paused);

            if (ImGui::MenuItem("Reset (Paused at 0000)", "", false, !emu_is_empty()))
            {
                menu_reset_paused();
            }

            ImGui::Separator();

            // Grouped by what they are for (Saverio: the flat menu had grown
            // too long): running the CPU, the core views, then one submenu per
            // family of tools.
            if (ImGui::BeginMenu("Execution", config_debug.debug))
            {
                if (ImGui::MenuItem("Step Into", "F11", (void*)0, config_debug.debug))
                {
                    emu_debug_step_into();
                }

                if (ImGui::MenuItem("Step Over", "F10", (void*)0, config_debug.debug))
                {
                    emu_debug_step_over();
                }

                if (ImGui::MenuItem("Step Line",
                                    config_hotkeys[config_HotkeyIndex_DebugStepLine].str,
                                    (void*)0, config_debug.debug))
                {
                    emu_debug_step_line();
                }

                if (ImGui::MenuItem("Step Frame", "F6", (void*)0, config_debug.debug))
                {
                    emu_debug_next_frame();
                }

                if (ImGui::MenuItem("Break", "F7", (void*)0, config_debug.debug))
                {
                    gui_shortcut(gui_ShortcutDebugBreak);
                }

                if (ImGui::MenuItem("Continue Live",
                                    config_hotkeys[config_HotkeyIndex_DebugContinue].str,
                                    (void*)0, config_debug.debug))
                {
                    menu_continue();
                }

                if (ImGui::MenuItem(
                        "Continue From Here",
                        config_hotkeys[config_HotkeyIndex_DebugContinueFromHere].str,
                        (void*)0, config_debug.debug))
                {
                    gui_debug_rewind_resume_from_here();
                }

                if (ImGui::MenuItem("Run To Cursor", "F8", (void*)0, config_debug.debug))
                {
                    gui_debug_runtocursor();
                }

                ImGui::Separator();
                if (ImGui::MenuItem("Go Back", "Ctrl+Backspace", (void*)0, config_debug.debug))
                {
                    gui_debug_go_back();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Return to the address you were at before\nthe last JP/JR/CALL double-click or Goto,\nlike a browser back button.");
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();

            if (ImGui::BeginMenu("Breakpoints", config_debug.debug))
            {
                if (ImGui::MenuItem("Toggle Breakpoint", "F9"))
                {
                    gui_debug_toggle_breakpoint();
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Clear All Processor Breakpoints"))
                {
                    gui_debug_reset_breakpoints_cpu();
                }

                if (ImGui::MenuItem("Clear All Memory Breakpoints"))
                {
                    gui_debug_reset_breakpoints_mem();
                }

                ImGui::Separator();

                ImGui::MenuItem("Disable All Processor Breakpoints", 0, &emu_debug_disable_breakpoints_cpu);
                ImGui::MenuItem("Disable All Memory Breakpoints", 0, &emu_debug_disable_breakpoints_mem);
                ImGui::EndMenu();
            }

            ImGui::Separator();

            ImGui::MenuItem("Show Output Screen", "", &config_debug.show_screen, config_debug.debug);

            ImGui::MenuItem("Show Disassembler", "", &config_debug.show_disassembler, config_debug.debug);

            ImGui::MenuItem("Show Disassembler (new)", "", &config_debug.show_disassembler_new, config_debug.debug);

            ImGui::MenuItem("Show Deep Search", "", &config_debug.show_deep_search, config_debug.debug);

            ImGui::MenuItem("Show To Check", "", &config_debug.show_to_check, config_debug.debug);

            ImGui::MenuItem("Show Z80 Status", "", &config_debug.show_processor, config_debug.debug);

            ImGui::MenuItem("Show Memory Editor", "", &config_debug.show_memory, config_debug.debug);

            if (ImGui::BeginMenu("Video", config_debug.debug))
            {
                if (ImGui::MenuItem("As Main Video Settings", "",
                    &config_debug.video_as_main_settings))
                {
                    video_setup_profile = config_debug.video_as_main_settings
                        ? static_cast<config_VideoOutput*>(&config_video)
                        : &config_debug.video;
                    apply_video_output_source(config_video_output_for_mode(true));
                }
                ImGui::Separator();
                ImGui::BeginDisabled(config_debug.video_as_main_settings);

                if (ImGui::BeginMenu("Scale"))
                {
                    menu_video_output_scale(config_debug.video, true);
                    ImGui::EndMenu();
                }

                menu_video_output_aspect(config_debug.video, true);
                menu_video_output_overscan(config_debug.video, true);

                if (ImGui::BeginMenu("Postprocessing"))
                {
                    menu_video_output_postprocessing(config_debug.video);
                    ImGui::EndMenu();
                }

                ImGui::EndDisabled();

                ImGui::Separator();
                ImGui::MenuItem("Show Pattern Table", "", &config_debug.show_pattern_table);
                ImGui::MenuItem("Show Sprites", "", &config_debug.show_sprites);
                ImGui::MenuItem("Show Name Table", "", &config_debug.show_nametable);
                ImGui::MenuItem("Show Palettes", "", &config_debug.show_palettes);
                ImGui::MenuItem("Show VDP Register Explorer", "", &config_debug.show_video_register_explorer);
                ImGui::Separator();
                if (ImGui::BeginMenu("CRT Experiments"))
                {
                    ImGui::MenuItem("Show CRT Test Pass", "", &config_debug.show_crt_test_pass);
                    // No renderer_set_crt_test_enabled call here: the window
                    // itself is the single source of truth for whether the GPU
                    // pass runs, synced right after ImGui::Begin in
                    // debug_window_crt_test_pass - that covers this checkbox,
                    // the window's own close button, and startup alike, so
                    // there is nowhere else this can get out of sync.
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("GPU render-to-texture proof of concept (horizontal blur), not a real CRT decode.");
                    ImGui::MenuItem("Show CRT Composite Decode", "", &config_debug.show_composite_decode_test);
                    // Same reasoning as CRT Test Pass above: no call here, the
                    // window syncs renderer_set_composite_decode_enabled itself.
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Real composite/S-Video decode (quadrature demod + PAL delay line), run on a captured GCRT signal file.");
                    ImGui::MenuItem("Show CRT Consumer Test", "", &config_debug.show_crt_consumer_test);
                    // Same reasoning as CRT Test Pass above: no call here, the
                    // window syncs renderer_set_crt_consumer_enabled itself.
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Display-stage test: trimmed GearSystem crt_consumer.glsl port (blur+scanline+mask+brightboost+gamma), run on live output.");
                    ImGui::MenuItem("Show CRT Lottes Test", "", &config_debug.show_crt_lottes_test);
                    // Same reasoning as CRT Test Pass above: no call here, the
                    // window syncs renderer_set_crt_lottes_enabled itself.
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Display-stage test, second candidate: trimmed crt-lottes-fast.glsl port (scanline+optional mask+auto-exposure+gamma), run on live output.");
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Peripherals", config_debug.debug))
            {
                if (emu_get_core()->GetMachineType() != MachineType::MSX && emu_get_core()->GetMachineType() != MachineType::SordM5)
                    ImGui::MenuItem("SN76489A/SMS PSG", "", &config_debug.show_psg);
#if GEARSF7000_ENABLE_AY
                ImGui::MenuItem("AY-3-8910 / YM2149", "", &config_debug.show_ay);
#endif
#if GEARSF7000_ENABLE_YM2413
                if (emu_get_core()->GetMachineType() != MachineType::MSX && emu_get_core()->GetMachineType() != MachineType::SordM5)
                    ImGui::MenuItem("YM2413 FM Sound Unit", "", &config_debug.show_ym2413);
#endif
#if GEARSF7000_ENABLE_SR1000
                if (emu_get_core()->HasCassetteInterface())
                    ImGui::MenuItem("Show Cassette", "", &config_debug.show_cassette);
#endif
                if (emu_get_core()->HasKeyboardPPI() || emu_get_core()->GetMachineType() == MachineType::MSX)
                    ImGui::MenuItem("Show PPI 8255A", "", &config_debug.show_ppi_registers);
#if GEARSF7000_ENABLE_SF7000
                if (IsSF7000System(emu_get_core()->GetSystemModel()))
                    ImGui::MenuItem("Show SF-7000", "", &config_debug.show_sf7000);
#endif
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Recorder"))
            {
                ImGui::Checkbox("Show Recorder", &config_debug.show_rewind);
                ImGui::SameLine();
                bool recording = rewind_is_enabled();
                if (ImGui::Checkbox("Enabled", &recording))
                {
                    RewindConfigResult r;
                    if (rewind_configure(recording,
                                         config_debug.rewind_seconds,
                                         1, 0, &r))
                    {
                        config_debug.rewind_enabled = recording;
                        gui_set_status_message(
                            recording ? "Recorder enabled"
                                      : "Recorder disabled; data cleared",
                            3000);
                    }
                    else
                    {
                        gui_set_status_message(r.error.c_str(), 6000);
                    }
                    // A failed enable request must not grey the duration field
                    // for the rest of this frame as though it had succeeded.
                    recording = rewind_is_enabled();
                }

                ImGui::Separator();
                ImGui::BeginDisabled(rewind_get_snapshot_count() == 0);
                if (ImGui::MenuItem("Reset Data"))
                {
                    gui_debug_rewind_reset_data();
                    gui_set_status_message("Recorder data cleared", 3000);
                }
                ImGui::EndDisabled();

                ImGui::Separator();
                ImGui::BeginDisabled(recording);
                ImGui::SetNextItemWidth(90);
                int rewind_seconds = config_debug.rewind_seconds;
                if (ImGui::InputInt("Seconds##menu_rw", &rewind_seconds, 5, 30))
                {
                    if (rewind_seconds < 1) rewind_seconds = 1;
                    if (rewind_seconds > 600) rewind_seconds = 600;
                    config_debug.rewind_seconds = rewind_seconds;
                }
                ImGui::EndDisabled();
                if (recording)
                    ImGui::TextDisabled("disable to change the length");

                ImGui::EndMenu();
            }
            // Reverse engineering a ROM: what it is made of and what it does.
            if (ImGui::BeginMenu("Analysis", config_debug.debug))
            {
                ImGui::MenuItem("Show Live Disassembler", "", &config_debug.show_rom_map);
                ImGui::MenuItem("Show Debug Events", "", &config_debug.show_events);
                ImGui::MenuItem("Show ROM Inspector", "", &config_debug.show_rom_inspector);
#if GEARSF7000_ENABLE_MCP
                // The watch/freeze/trigger system is reachable over MCP but is
                // not about MCP: WatchTick() runs from emu_update() whether or
                // not a client is connected, so the window belongs with the
                // other analysis tools. It stays behind the MCP flag because
                // that is what compiles the adapter holding the watch state.
                ImGui::MenuItem("Show Watch Monitor", "", &config_debug.show_watch_monitor);
#endif

                // Where the ROM map projects are saved. Shown, not just kept:
                // a folder one has to hunt for is a folder one loses.
                ImGui::Separator();
                const std::string projects_root = rom_map_project_root();
                std::string projects_shown = projects_root;
                if (projects_shown.size() > 44)
                    projects_shown = "..." + projects_shown.substr(projects_shown.size() - 41);
                ImGui::TextDisabled("Projects folder");
                ImGui::TextDisabled("%s", projects_shown.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", projects_root.c_str());
                if (ImGui::MenuItem("Change Projects Folder..."))
                    file_dialog_choose_projects_folder();
                if (ImGui::MenuItem("Open Projects Folder"))
                    rom_map_project_open_folder(projects_root);
                if (ImGui::MenuItem("Use Default Projects Folder", "", false, !rom_map_project_root_is_default()))
                    projects_folder_use(std::string());
                ImGui::EndMenu();
            }

            // Getting programs and data in and out of the machine.
            if (ImGui::BeginMenu("Tools", config_debug.debug))
            {
                // Not behind the MCP flag: it drives the SK-1100 injector
                // directly, which every build has.
                ImGui::MenuItem("Show BASIC Typer", "", &config_debug.show_basic_typer);
#ifdef   SC3KSYSTEM_MEM_IMPORT_EXPORT
                ImGui::MenuItem("Show Memory Import", "", &config_debug.show_memory_import);
#endif
#if GEARSF7000_ENABLE_SF7000
                ImGui::MenuItem("Show Disc Explorer", "", &config_debug.show_disc_explorer);
#endif
                ImGui::EndMenu();
            }

#if GEARSF7000_ENABLE_MCP
            if (ImGui::BeginMenu("MCP", config_debug.debug))
            {
                // Watch Monitor lives under Analysis: this menu is the
                // transport itself, not what the tools inspect.
                ImGui::MenuItem("Show MCP Server", "", &config_debug.show_mcp_server);
                ImGui::EndMenu();
            }
#endif

            ImGui::Separator();

#if defined(__APPLE__) || defined(_WIN32)
            ImGui::MenuItem("Multi-Viewport (Restart required)", "", &config_debug.multi_viewport, config_debug.debug);
            ImGui::Separator();
#endif

            if (ImGui::BeginMenu("Symbols", config_debug.debug))
            {
                if (ImGui::MenuItem("Load Symbols..."))
                {
                    open_symbols = true;
                }

                if (ImGui::MenuItem("Clear Symbols"))
                {
                    gui_debug_reset_symbols();
                }
                ImGui::EndMenu();
            }


            ImGui::EndMenu();
        }

#else
        config_debug.debug = false;
#endif

        // Window navigation belongs to both emulator and debugger modes.
        if (ImGui::BeginMenu("Windows"))
        {
            gui_in_use = true;
            window_menu_item("ROM Info", config_emulator.show_info, "ROM Info");
            window_menu_item("Interface Settings", show_interface_settings, "Interface Settings");
            window_menu_item("Emulator Hotkeys", show_hotkey_config, "Emulator Hotkeys");
            window_menu_item("SC-3000 Keyboard", show_sk1100_keyboard_config, "SC-3000 Keyboard");
            window_menu_item("CRT (Lottes) Setup", show_crt_lottes_setup, "CRT (Lottes) Setup");
            window_menu_item("Advanced Scaling Setup", show_advanced_scaling_setup, "Advanced Scaling Setup");
#if GEARSF7000_ENABLE_SR1000
            window_menu_item("Cassette", config_debug.show_cassette &&
                emu_get_core()->HasCassetteInterface(), "Cassette");
#endif
#ifdef DEBUG_TOOLS
            if (config_debug.debug)
            {
                window_menu_item("Output", config_debug.show_screen, "Output###debug_output");
                window_menu_item("Disassembler", config_debug.show_disassembler, "Disassembler");
                window_menu_item("Disassembler (new)", config_debug.show_disassembler_new, "Disassembler (new)###disassembler_new");
                window_menu_item("Deep Search", config_debug.show_deep_search, "Deep Search###deep_search");
                window_menu_item("To Check", config_debug.show_to_check, "To Check###to_check");
                window_menu_item("Z80 Status", config_debug.show_processor, "Z80 Status");
                window_menu_item("Memory Editor", config_debug.show_memory, "Memory Editor");
                window_menu_item("Pattern Table", config_debug.show_pattern_table, "Pattern Table");
                window_menu_item("Sprites", config_debug.show_sprites, "Sprites");
                window_menu_item("Name Table", config_debug.show_nametable, "Name Table");
                window_menu_item("Palettes", config_debug.show_palettes, "Palettes###palettes");
                window_menu_item("VDP Register Explorer", config_debug.show_video_register_explorer, "VDP Register Explorer");
                window_menu_item("PSG", config_debug.show_psg, "PSG");
#if GEARSF7000_ENABLE_AY
                window_menu_item("AY-3-8910 / YM2149", config_debug.show_ay, "AY-3-8910 / YM2149");
#endif
#if GEARSF7000_ENABLE_YM2413
                window_menu_item("YM2413 FM Sound Unit", config_debug.show_ym2413,
                                 "YM2413 FM Sound Unit");
#endif
#if GEARSF7000_ENABLE_SF7000
                window_menu_item("SF-7000", config_debug.show_sf7000, "SF-7000");
#endif
                window_menu_item("Recorder", config_debug.show_rewind, "Recorder");
                window_menu_item("Debug Events", config_debug.show_events, "Debug Events");
                window_menu_item("Live Disassembler", config_debug.show_rom_map, "Live Disassembler");
                window_menu_item("BASIC Typer", config_debug.show_basic_typer, "BASIC Typer");
                window_menu_item("Mem Import", config_debug.show_memory_import, "Mem Import");
                window_menu_item("ROM Inspector", config_debug.show_rom_inspector, "ROM Inspector");
#if GEARSF7000_ENABLE_SF7000
                window_menu_item("Disc Explorer", config_debug.show_disc_explorer, "Disc Explorer");
#endif
                window_menu_item("CRT Test Pass (GPU)", config_debug.show_crt_test_pass, "CRT Test Pass (GPU)");
                window_menu_item("CRT Composite Decode Test (GPU)", config_debug.show_composite_decode_test, "CRT Composite Decode Test (GPU)");
                window_menu_item("CRT Consumer Test (GPU)", config_debug.show_crt_consumer_test, "CRT Consumer Test (GPU)");
                window_menu_item("CRT Lottes Test (GPU)", config_debug.show_crt_lottes_test, "CRT Lottes Test (GPU)");
#if GEARSF7000_ENABLE_MCP
                window_menu_item("MCP Server", config_debug.show_mcp_server, "MCP Server");
                window_menu_item("Watch Monitor", config_debug.show_watch_monitor, "Watch Monitor");
#endif
            }
#endif
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("About"))
        {
            gui_in_use = true;

            char about_label[96];
            snprintf(about_label, sizeof(about_label), "About %s", GEARSF7000_TITLE);
            if (ImGui::MenuItem(about_label))
            {
               open_about = true;
            }
            ImGui::EndMenu();
        }

        draw_menu_bar_status();

        main_menu_height = (int)ImGui::GetWindowSize().y;

        ImGui::EndMainMenuBar();       
    }

    if (open_rom || shortcut_open_rom)
    {
        shortcut_open_rom = false;
        file_dialog_open_rom();
    }

    if (open_ram)
        file_dialog_load_ram();

    if (open_basic_program)
        file_dialog_load_basic_program();

    if (save_basic_program)
        file_dialog_save_basic_program();

    if (save_ram)
        file_dialog_save_ram();

    if (open_state)
        file_dialog_load_state();
    
    if (save_state)
        file_dialog_save_state();

    if (save_screenshot)
        file_dialog_save_screenshot();

    if (choose_savestates_path)
        file_dialog_choose_savestate_path();

    if (open_bios)
        file_dialog_load_bios();

    if (open_m5_bios) file_dialog_load_m5_bios();
    if (open_coleco_bios)
        file_dialog_load_coleco_bios();
    if (open_msx_bios)
        file_dialog_load_msx_bios();

    if (open_disc)
        file_dialog_load_disc();

    if (open_cassette)
        file_dialog_load_cassette();
    if (select_msx_cassette)
        file_dialog_select_msx_cassette();


    if (open_symbols)
        file_dialog_load_symbols();

    if (open_about)
    {
        dialog_in_use = true;
        ImGui::OpenPopup("About " GEARSF7000_TITLE);
    }

    if (open_bios_warning)
    {
        dialog_in_use = true;
        ImGui::OpenPopup("IPL");
    }

    popup_modal_bios();
    popup_modal_about();

}

static bool main_window(void)
{
    bool hasFocus = true;

    const bool config_isdebug = config_debug.debug;
    const auto video_output = emu_get_effective_video_output(config_isdebug);

    GC_RuntimeInfo runtime;
    emu_get_runtime(runtime);
    const GC_VideoFrameDescriptor frameDescriptor =
        emu_get_video_frame_descriptor();

    // 1920x994
    int w = (int)ImGui::GetIO().DisplaySize.x;
    int h = (int)ImGui::GetIO().DisplaySize.y - (config_emulator.show_menu ? main_menu_height : 0);

    int selected_ratio = video_output.ratio;
    float ratio = 0;

    switch (selected_ratio)
    {
        case 1:
            ratio = 4.0f / 3.0f;
            break;
        case 2:
            ratio = 16.0f / 9.0f;
            break;
        default:
            ratio = frameDescriptor.IsValid()
                ? static_cast<float>(frameDescriptor.presentation_width) /
                  static_cast<float>(frameDescriptor.presentation_height)
                : (float)runtime.screen_width / (float)runtime.screen_height;
    }

    if (!config_isdebug && video_output.scale == SCALE_WIN_WIDTH_HEIGHT)
    {
        ratio = (float)w / (float)h;
    }

    // Explicit scale choices describe desktop occupancy, not framebuffer
    // pixels. Pico9918, for example, emits 640x480 but presents at a 320x240
    // basis, so the user's existing 2x choice remains a 640x480 window. The
    // aspect ratio is applied to the presentation basis as a whole and never
    // changes the geometry of an individual video mode inside the frame.
    const int presentationHeight = frameDescriptor.IsValid()
        ? frameDescriptor.presentation_height
        : runtime.screen_height;
    int w_corrected = frameDescriptor.IsValid() && selected_ratio == 0
        ? frameDescriptor.presentation_width
        : (int)roundf(presentationHeight * ratio);
    int h_corrected = presentationHeight;
    float scale_multiplier = 0.0f;
    const float selected_scale = explicit_scale_multiplier(video_output.scale);

    if (config_isdebug)
    {
        if (selected_scale > 0.0f)
            scale_multiplier = selected_scale;
        else
            scale_multiplier = 1;
    }
    else
    {
        if (selected_scale > 0.0f)
        {
            scale_multiplier = selected_scale;
        }
        else if (video_output.scale == SCALE_INTEGER)
        {
            int factor_w = w / w_corrected;
            int factor_h = h / h_corrected;
            scale_multiplier = (factor_w < factor_h) ? factor_w : factor_h;
        }
        else if (video_output.scale == SCALE_WIN_HEIGHT)
        {
            scale_multiplier = 1;
            h_corrected = h;
            w_corrected = h * ratio;
        }
        else if (video_output.scale == SCALE_WIN_HEIGHT_HALF)
        {
            // Same "fill the window height" goal as SCALE_WIN_HEIGHT, but
            // snapped to the nearest 0.5 instead of the raw continuous
            // fraction the window happens to be - cleaner, more predictable
            // geometry while staying close to a full-window fit.
            // w_corrected/h_corrected keep their native-resolution values
            // from above (unlike SCALE_WIN_HEIGHT, which overrides them to
            // the window's own pixel size directly) - scale_multiplier does
            // all the work here instead.
            float rawMultiplier = h_corrected > 0 ? (float)h / (float)h_corrected : 1.0f;
            scale_multiplier = roundf(rawMultiplier * 2.0f) / 2.0f;
            if (scale_multiplier < 0.5f)
                scale_multiplier = 0.5f;
        }
        else if (video_output.scale == SCALE_WIN_WIDTH_HEIGHT)
        {
            scale_multiplier = 1;
            w_corrected = w;
            h_corrected = h;
        }
    }

    main_window_width = static_cast<int>(roundf(w_corrected * scale_multiplier));
    main_window_height = static_cast<int>(roundf(h_corrected * scale_multiplier));

    // Must run before renderer_render() executes the GPU passes for this
    // same frame (it does - renderer_begin_render/gui_render/renderer_render
    // is the per-frame order in application.cpp) so the live CRT pass has
    // the real on-screen size ready. A no-op when unchanged from last frame.
    // renderer_ensure_crt_lottes_pipeline() is checked here too (not just
    // texture existence) so a backend with no bytecode for this shader
    // (see shaders/crt_pass/README.md's "No DXBC yet") falls back to
    // drawing plain emu_texture below instead of a never-written, blank
    // renderer_crt_lottes_live_texture.
    bool crt_lottes_ready = false;
    if (video_output.postprocessing == POSTPROCESSING_CRT_LOTTES &&
        renderer_postprocessing_supported(POSTPROCESSING_CRT_LOTTES))
    {
        renderer_set_crt_lottes_output_size(main_window_width, main_window_height);
        crt_lottes_ready = renderer_ensure_crt_lottes_pipeline();
    }
    bool advanced_scaling_ready = false;
    if (video_output.postprocessing == POSTPROCESSING_ADVANCED_SCALING &&
        renderer_postprocessing_supported(POSTPROCESSING_ADVANCED_SCALING))
    {
        renderer_set_advanced_scaling_output_size(main_window_width, main_window_height);
        advanced_scaling_ready = renderer_ensure_advanced_scaling_pipeline();
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar;
    
    if (config_debug.debug)
    {
        //int window_x = (w - (w_corrected * scale_multiplier)) / 2;
        //int window_y = ((h - (h_corrected * scale_multiplier)) / 2) + (config_emulator.show_menu ? main_menu_height : 0);

        //ImGui::SetNextWindowSize(ImVec2((float)main_window_width, (float)main_window_height));
        //ImGui::SetNextWindowPos(ImVec2((float)window_x, (float)window_y));


        flags |= ImGuiWindowFlags_AlwaysAutoResize;

        ImGui::SetNextWindowPos(ImVec2(568, 31), ImGuiCond_FirstUseEver);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

        ImGui::Begin("Output###debug_output", &config_debug.show_screen, flags);
        gui_main_window_hovered = ImGui::IsWindowHovered();
    }
    else
    {
        int window_x = (w - (w_corrected * scale_multiplier)) / 2;
        int window_y = ((h - (h_corrected * scale_multiplier)) / 2) + (config_emulator.show_menu ? main_menu_height : 0);

        ImGui::SetNextWindowSize(ImVec2((float)main_window_width, (float)main_window_height));
        ImGui::SetNextWindowPos(ImVec2((float)window_x, (float)window_y));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

        flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;

        ImGui::Begin(GEARSF7000_TITLE, 0, flags);
        gui_main_window_hovered = ImGui::IsWindowHovered();
    }

    const ImVec2 output_content_origin = ImGui::GetCursorPos();
    renderer_begin_emulator_image();
    // CRT (Lottes) draws its own already-final-resolution output instead of
    // emu_texture directly - see renderer_crt_lottes_live_texture's own
    // comment for why it must already be exactly this size (no further
    // resampling here, which previously hid the scanline banding twice
    // this session when it happened by accident in a debug preview).
    const bool useCrtLottesLive = crt_lottes_ready
        && renderer_crt_lottes_live_texture != ImTextureID_Invalid;
    const bool useAdvancedScalingLive = advanced_scaling_ready
        && renderer_advanced_scaling_live_texture != ImTextureID_Invalid;
    ImTextureID mainImageTexture = useCrtLottesLive
        ? renderer_crt_lottes_live_texture
        : useAdvancedScalingLive
            ? renderer_advanced_scaling_live_texture
            : (ImTextureID)(intptr_t)renderer_emu_texture;
    const RendererTextureRegion sourceRegion = renderer_get_emulator_texture_region();
    const ImVec2 imageUv0 = (useCrtLottesLive || useAdvancedScalingLive)
        ? ImVec2(0.0f, 0.0f) : sourceRegion.uv0;
    const ImVec2 imageUv1 = (useCrtLottesLive || useAdvancedScalingLive)
        ? ImVec2(1.0f, 1.0f) : sourceRegion.uv1;
    ImGui::Image(mainImageTexture,
        ImVec2((float)main_window_width, (float)main_window_height),
        imageUv0, imageUv1);
    renderer_end_emulator_image();

    if (config_video.fps || config_video.frame_pacing_info)
        show_fps(output_content_origin);

    if ((config_debug.debug) && !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootWindow)) {
        hasFocus = false;
    }

    ImGui::End();

    ImGui::PopStyleVar();
    ImGui::PopStyleVar();

    //if (!config_debug.debug)
    {
        ImGui::PopStyleVar();
    }

    return hasFocus;
}

class ScopedFileDialogAudioPause
{
public:
    ScopedFileDialogAudioPause() { emu_audio_suspend_for_file_dialog(); }
    ~ScopedFileDialogAudioPause() { emu_audio_resume_after_file_dialog(); }
    ScopedFileDialogAudioPause(const ScopedFileDialogAudioPause&) = delete;
    ScopedFileDialogAudioPause& operator=(const ScopedFileDialogAudioPause&) = delete;
};

static void file_dialog_open_rom(void)
{
    ScopedFileDialogAudioPause audioPause;
    nfdchar_t *outPath;
    // Only the file types of machines this build has.
    static const std::string kRomExtensions = std::string("sg,sc")
#if GEARSF7000_ENABLE_SMS
        + ",sms"
#endif
#if GEARSF7000_ENABLE_GAMEGEAR
        + ",gg"
#endif
#if !GEARSF7000_PRODUCT_SC3000
        + ",col,cv,mx1"
#endif
        + ",rom,asc,asls,bin,zip";
    nfdfilteritem_t filterItem[1] = { { "ROM Files", kRomExtensions.c_str() } };
    nfdresult_t result = NFD_OpenDialogParented(
        &outPath, filterItem, 1, config_emulator.last_open_path.c_str());
    if (result == NFD_OKAY)
    {
        std::string path = outPath;
        std::string::size_type pos = path.find_last_of("\\/");
        config_emulator.last_open_path.assign(path.substr(0, pos));
        gui_load_rom(outPath);
        NFD_FreePath(outPath);
    }
    else if (result != NFD_CANCEL)
    {
        Log("Open ROM Error: %s", NFD_GetError());
    }
}

static void file_dialog_load_ram(void)
{
    ScopedFileDialogAudioPause audioPause;
    nfdchar_t *outPath;
    nfdfilteritem_t filterItem[1] = { { "RAM Files", "sav" } };
    nfdresult_t result = NFD_OpenDialogParented(&outPath, filterItem, 1, NULL);
    if (result == NFD_OKAY)
    {
        Cartridge::ForceConfiguration config = get_force_config();

        emu_load_ram(outPath, config);
        NFD_FreePath(outPath);
    }
    else if (result != NFD_CANCEL)
    {
        Log("Load RAM Error: %s", NFD_GetError());
    }
}

static void file_dialog_save_basic_program(void)
{
    ScopedFileDialogAudioPause audioPause;
    nfdchar_t *outPath;
    nfdfilteritem_t filterItem[1] = { { "Tokenised BASIC", "bas" } };
    nfdresult_t result = NFD_SaveDialogParented(&outPath, filterItem, 1, config_emulator.last_open_path.c_str(), "program.bas");
    if (result == NFD_OKAY)
    {
        std::string status;
        const bool saved = emu_save_basic_program(outPath, config_debug.basic_pointer_block, &status);
        Log("Save BASIC Program: %s", status.c_str());
        gui_set_status_message(status.c_str(), saved ? 3000 : 6000);
        NFD_FreePath(outPath);
    }
    else if (result != NFD_CANCEL)
    {
        Log("Save BASIC Program Error: %s", NFD_GetError());
    }
}

static void file_dialog_load_basic_program(void)
{
    ScopedFileDialogAudioPause audioPause;
    nfdchar_t *outPath;
    nfdfilteritem_t filterItem[1] = { { "Tokenised BASIC", "bas" } };
    nfdresult_t result = NFD_OpenDialogParented(&outPath, filterItem, 1, config_emulator.last_open_path.c_str());
    if (result == NFD_OKAY)
    {
        std::string status;
        const bool loaded = emu_load_basic_program(outPath, config_debug.basic_pointer_block, &status);
        // Said out loud either way: a load that quietly did nothing because
        // BASIC was not initialised looks exactly like one that worked, right
        // up until the user types LIST.
        Log("Load BASIC Program: %s", status.c_str());
        gui_set_status_message(status.c_str(), loaded ? 3000 : 6000);
        NFD_FreePath(outPath);
    }
    else if (result != NFD_CANCEL)
    {
        Log("Load BASIC Program Error: %s", NFD_GetError());
    }
}

static void file_dialog_save_ram(void)
{
    ScopedFileDialogAudioPause audioPause;
    nfdchar_t *outPath;
    nfdfilteritem_t filterItem[1] = { { "RAM Files", "sav" } };
    nfdresult_t result = NFD_SaveDialogParented(&outPath, filterItem, 1, NULL, NULL);
    if (result == NFD_OKAY)
    {
        emu_save_ram(outPath);
        NFD_FreePath(outPath);
    }
    else if (result != NFD_CANCEL)
    {
        Log("Save RAM Error: %s", NFD_GetError());
    }
}

static void file_dialog_load_state(void)
{
    ScopedFileDialogAudioPause audioPause;
    nfdchar_t *outPath;
    nfdfilteritem_t filterItem[1] = { { "Save State Files", "state" } };
    nfdresult_t result = NFD_OpenDialogParented(&outPath, filterItem, 1, NULL);
    if (result == NFD_OKAY)
    {
        std::string message("Loading state from ");
        message += outPath;
        gui_set_status_message(message.c_str(), 3000);
        emu_load_state_file(outPath);
        NFD_FreePath(outPath);
    }
    else if (result != NFD_CANCEL)
    {
        Log("Load State Error: %s", NFD_GetError());
    }
}

#if GEARSF7000_ENABLE_RECORDER
static void file_dialog_save_vgm(void)
{
    ScopedFileDialogAudioPause audioPause;
    nfdchar_t *outPath;
    nfdfilteritem_t filterItem[1] = { { "VGM Files", "vgm" } };
    nfdresult_t result = NFD_SaveDialogParented(&outPath, filterItem, 1, config_emulator.last_open_path.c_str(), "recording.vgm");
    if (result == NFD_OKAY)
    {
        if (emu_start_vgm_recording(outPath))
            gui_set_status_message("VGM recording started", 3000);
        else
            gui_set_status_message("Could not start VGM recording", 3000);
        NFD_FreePath(outPath);
    }
    else if (result != NFD_CANCEL)
    {
        Log("Save VGM Error: %s", NFD_GetError());
    }
}
#endif

static void file_dialog_save_state(void)
{
    ScopedFileDialogAudioPause audioPause;
    nfdchar_t *outPath;
    nfdfilteritem_t filterItem[1] = { { "Save State Files", "state" } };
    nfdresult_t result = NFD_SaveDialogParented(&outPath, filterItem, 1, NULL, NULL);
    if (result == NFD_OKAY)
    {
        std::string message("Saving state to ");
        message += outPath;
        gui_set_status_message(message.c_str(), 3000);
        emu_save_state_file(outPath);
        NFD_FreePath(outPath);
    }
    else if (result != NFD_CANCEL)
    {
        Log("Save State Error: %s", NFD_GetError());
    }
}

static void file_dialog_choose_savestate_path(void)
{
    ScopedFileDialogAudioPause audioPause;
    nfdchar_t *outPath;
    nfdresult_t result = NFD_PickFolderParented(&outPath, savestates_path);
    if (result == NFD_OKAY)
    {
        strcpy(savestates_path, outPath);
        config_emulator.savestates_path.assign(outPath);
        NFD_FreePath(outPath);
    }
    else if (result != NFD_CANCEL)
    {
        Log("Savestate Path Error: %s", NFD_GetError());
    }
}

static void projects_folder_use(const std::string& path)
{
    if (rom_map_project_set_root(path))
    {
        std::string message("Projects folder: ");
        message += rom_map_project_root();
        gui_set_status_message(message.c_str(), 4000);
    }
    else
    {
        std::string message("Projects folder not changed: ");
        message += rom_map_project_last_error();
        Log("%s", message.c_str());
        gui_set_status_message(message.c_str(), 6000);
    }
}

static void file_dialog_choose_projects_folder(void)
{
    ScopedFileDialogAudioPause audioPause;
    const std::string current = rom_map_project_root();
    nfdchar_t *outPath;
    nfdresult_t result = NFD_PickFolderParented(&outPath, current.c_str());
    if (result == NFD_OKAY)
    {
        const std::string chosen(outPath);
        NFD_FreePath(outPath);
        projects_folder_use(chosen);
    }
    else if (result != NFD_CANCEL)
    {
        Log("Projects Folder Error: %s", NFD_GetError());
    }
}

static void file_dialog_load_m5_bios(void)
{
    ScopedFileDialogAudioPause audioPause;
    nfdchar_t *outPath;
    nfdfilteritem_t filterItem[1] = { { "Sord M5 BIOS", "col,rom,bin,bios" } };
    nfdresult_t result = NFD_OpenDialogParented(&outPath, filterItem, 1, NULL);
    if (result == NFD_OKAY)
    {
        config_emulator.m5_bios_path.assign(outPath);
        NFD_FreePath(outPath);
        emu_reload_machine_firmware();
        gui_debug_reset();
    }
    else if (result != NFD_CANCEL)
    {
        Log("Load Sord M5 BIOS Error: %s", NFD_GetError());
    }
}

static void file_dialog_load_coleco_bios(void)
{
    ScopedFileDialogAudioPause audioPause;
    nfdchar_t *outPath;
    nfdfilteritem_t filterItem[1] = { { "ColecoVision BIOS", "col,rom,bin,bios" } };
    nfdresult_t result = NFD_OpenDialogParented(&outPath, filterItem, 1, NULL);
    if (result == NFD_OKAY)
    {
        config_emulator.coleco_bios_path.assign(outPath);
        NFD_FreePath(outPath);
        emu_reload_machine_firmware();
        gui_debug_reset();
    }
    else if (result != NFD_CANCEL)
    {
        Log("Load ColecoVision BIOS Error: %s", NFD_GetError());
    }
}

static void file_dialog_load_msx_bios(void)
{
    ScopedFileDialogAudioPause audioPause;
    nfdchar_t *outPath;
    nfdfilteritem_t filterItem[1] = { { "MSX firmware ROM", "rom,bin,bios" } };
    nfdresult_t result = NFD_OpenDialogParented(&outPath, filterItem, 1, NULL);
    if (result == NFD_OKAY)
    {
        config_emulator.msx_bios_path.assign(outPath);
        config_emulator.msx_use_cbios = false;
        NFD_FreePath(outPath);
        emu_reload_machine_firmware();
        gui_debug_reset();
    }
    else if (result != NFD_CANCEL)
    {
        Log("Load MSX1 BIOS (32 KB) Error: %s", NFD_GetError());
    }
}

static void file_dialog_load_bios(void)
{
    ScopedFileDialogAudioPause audioPause;
    nfdchar_t *outPath;
    nfdfilteritem_t filterItem[1] = { { "BIOS Files", "sg,sc,bin,rom,bios,ipl" } };
    nfdresult_t result = NFD_OpenDialogParented(&outPath, filterItem, 1, NULL);
    if (result == NFD_OKAY)
    {
        strcpy(bios_path, outPath);
        config_emulator.bios_path.assign(outPath);
        emu_load_bios(bios_path);
        NFD_FreePath(outPath);
    }
    else if (result != NFD_CANCEL)
    {
        Log("Load IPL Error: %s", NFD_GetError());
    }
}


static void file_dialog_load_disc(void)
{
    ScopedFileDialogAudioPause audioPause;
    nfdchar_t* outPath;
    nfdfilteritem_t filterItem[1] = { { "Disk Files", "sf7,dsk,hfe,ds7" } };
    nfdresult_t result = NFD_OpenDialogParented(&outPath, filterItem, 1, NULL);
    if (result == NFD_OKAY)
    {
        strcpy(disc_path, outPath);
        gui_load_disc(disc_path);

        NFD_FreePath(outPath);
    }
    else if (result != NFD_CANCEL)
    {
        Log("Load Disc Error: %s", NFD_GetError());
    }
}





static void file_dialog_select_msx_cassette(void)
{
    ScopedFileDialogAudioPause audioPause;
    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filter = { "MSX Audio Tapes", "wav,mp3" };
    const auto parent = std::filesystem::path(config_emulator.msx_tape.path).parent_path().string();
    const nfdresult_t result = NFD_OpenDialogParented(&outPath, &filter, 1,
        parent.empty() ? nullptr : parent.c_str());
    if (result == NFD_OKAY)
    {
        if (!application_load_tape(outPath))
            gui_set_status_message("Unable to load MSX tape (WAV/MP3 supported)", 4000);
        NFD_FreePath(outPath);
    }
    else if (result != NFD_CANCEL)
        Log("Select MSX Tape Error: %s", NFD_GetError());
}

static void file_dialog_load_cassette(void)
{
    ScopedFileDialogAudioPause audioPause;
    nfdchar_t* outPath;
    nfdfilteritem_t filterItem[1] = { { "Tape Files", "bit,bas,basic,wav,mp3" } };
    nfdresult_t result = NFD_OpenDialogParented(&outPath, filterItem, 1, NULL);
    if (result == NFD_OKAY)
    {
        strcpy(cassette_path, outPath);
        gui_load_cassette(cassette_path);

        NFD_FreePath(outPath);
    }
    else if (result != NFD_CANCEL)
    {
        Log("Load Tape Error: %s", NFD_GetError());
    }
}



static void file_dialog_load_symbols(void)
{
    ScopedFileDialogAudioPause audioPause;
    nfdchar_t *outPath;
    nfdfilteritem_t filterItem[1] = { { "Symbol Files", "sym" } };
    nfdresult_t result = NFD_OpenDialogParented(&outPath, filterItem, 1, NULL);
    if (result == NFD_OKAY)
    {
        gui_debug_reset_symbols();
        gui_debug_load_symbols_file(outPath);
        NFD_FreePath(outPath);
    }
    else if (result != NFD_CANCEL)
    {
        Log("Load Symbols Error: %s", NFD_GetError());
    }
}

static void file_dialog_save_screenshot(void)
{
    ScopedFileDialogAudioPause audioPause;
    nfdchar_t *outPath;
    nfdfilteritem_t filterItem[1] = { { "PNG Files", "png" } };
    nfdresult_t result = NFD_SaveDialogParented(&outPath, filterItem, 1, NULL, NULL);
    if (result == NFD_OKAY)
    {
        call_save_screenshot(outPath);
        NFD_FreePath(outPath);
    }
    else if (result != NFD_CANCEL)
    {
        Log("Save Screenshot Error: %s", NFD_GetError());
    }
}

static void keyboard_configuration_item(const char* text, SDL_Scancode* key, int player)
{
    ImGui::Text("%s", text);
    ImGui::SameLine(100);

    char button_label[256];
    const char* name = *key == SDL_SCANCODE_UNKNOWN ? "Unassigned" : SDL_GetScancodeName(*key);
    sprintf(button_label, "%s##%s%d", name, text, player);

    if (ImGui::Button(button_label, ImVec2(90,0)))
    {
        configured_key = key;
        configured_hotkey = NULL;
        configured_hotkey_index = -1;
        configured_button = NULL;
        configured_player = player;
        configured_action = text;
        configured_sk1100_key = -1;
        input_capture_kind = InputCapture_Keyboard;
        input_capture_phase = InputCapture_Waiting;
        input_capture_message[0] = 0;
        input_capture_popup_requested = true;
    }
}

static void gamepad_configuration_item(const char* text, int* button, int player)
{
    ImGui::Text("%s", text);
    ImGui::SameLine(100);

    static const char* gamepad_names[] = {"A", "B", "X" ,"Y", "BACK", "GUID", "START", "L3", "R3", "L1", "R1", "UP", "DOWN", "LEFT", "RIGHT", "MISC"};

    char button_label[256];
    const char* name = (*button >= 0 && *button < (int)(sizeof(gamepad_names) / sizeof(gamepad_names[0])))
        ? gamepad_names[*button] : "Unassigned";
    sprintf(button_label, "%s##%s%d", name, text, player);

    if (ImGui::Button(button_label, ImVec2(70,0)))
    {
        configured_button = button;
        configured_key = NULL;
        configured_hotkey = NULL;
        configured_hotkey_index = -1;
        configured_player = player;
        configured_action = text;
        configured_sk1100_key = -1;
        input_capture_kind = InputCapture_Gamepad;
        input_capture_phase = InputCapture_Waiting;
        input_capture_message[0] = 0;
        input_capture_popup_requested = true;
    }
}

static void finish_input_capture(bool save)
{
    if (save)
    {
        // Player 0 is canonical for the one physical machine keyboard.
        for (int key = 0; key < SK1100_KEYBOARD_KEY_COUNT; ++key)
            config_input[1].sk1100_key[key] = config_input[0].sk1100_key[key];
        config_write();
    }
    if (configured_sk1100_key >= 0)
        sk1100_selected_key = -1;
    input_capture_phase = save ? InputCapture_Complete : InputCapture_Cancelled;
}

static void apply_pending_input_capture(void)
{
    if (input_capture_kind == InputCapture_Keyboard)
        *configured_key = input_capture_key;
    else if (input_capture_kind == InputCapture_Gamepad)
        *configured_button = input_capture_button;
    else if (input_capture_kind == InputCapture_Hotkey)
    {
        configured_hotkey->key = input_capture_key;
        configured_hotkey->mod = input_capture_mod;
        config_update_hotkey_string(configured_hotkey);
    }
    finish_input_capture(true);
}

static SDL_Keymod normalized_hotkey_mod(SDL_Keymod raw)
{
    SDL_Keymod mods = SDL_KMOD_NONE;
    if (raw & (SDL_KMOD_LCTRL | SDL_KMOD_RCTRL)) mods = (SDL_Keymod)(mods | SDL_KMOD_CTRL);
    if (raw & (SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT)) mods = (SDL_Keymod)(mods | SDL_KMOD_SHIFT);
    if (raw & (SDL_KMOD_LALT | SDL_KMOD_RALT)) mods = (SDL_Keymod)(mods | SDL_KMOD_ALT);
    if (raw & (SDL_KMOD_LGUI | SDL_KMOD_RGUI)) mods = (SDL_Keymod)(mods | SDL_KMOD_GUI);
    return mods;
}

static bool modifier_scancode(SDL_Scancode scancode)
{
    return scancode == SDL_SCANCODE_LCTRL || scancode == SDL_SCANCODE_RCTRL ||
           scancode == SDL_SCANCODE_LSHIFT || scancode == SDL_SCANCODE_RSHIFT ||
           scancode == SDL_SCANCODE_LALT || scancode == SDL_SCANCODE_RALT ||
           scancode == SDL_SCANCODE_LGUI || scancode == SDL_SCANCODE_RGUI;
}

static int find_hotkey_conflict(SDL_Scancode scancode, SDL_Keymod mods)
{
    for (int i = 0; i < config_HotkeyIndex_COUNT; ++i)
        if (i != configured_hotkey_index && config_hotkeys[i].key == scancode &&
            config_hotkeys[i].mod == mods)
            return i;
    return -1;
}

static int find_plain_hotkey(SDL_Scancode scancode)
{
    for (int i = 0; i < config_HotkeyIndex_COUNT; ++i)
        if (config_hotkeys[i].key == scancode && config_hotkeys[i].mod == SDL_KMOD_NONE)
            return i;
    return -1;
}

// "Keypad 5", stable storage for the conflict message.
static const char* keypad_action_name(int key)
{
    static char names[COLECO_KEYPAD_KEY_COUNT][16];
    snprintf(names[key], sizeof(names[key]), "Keypad %s", coleco_keypad_layout[key].label);
    return names[key];
}

static SDL_Scancode* find_keyboard_conflict(SDL_Scancode scancode,
                                             const char** action, int* player)
{
    const int service = find_plain_hotkey(scancode);
    if (service >= 0)
    {
        *action = config_hotkey_label((config_HotkeyIndex)service);
        *player = -1;
        input_capture_conflicting_hotkey = service;
        return &config_hotkeys[service].key;
    }

    if (configured_sk1100_key >= 0)
    {
        for (int key = 0; key < SK1100_KEYBOARD_KEY_COUNT; ++key)
        {
            if (key != configured_sk1100_key && config_input[0].sk1100_key[key] == scancode)
            {
                *action = sk1100_keyboard_layout[key].name;
                *player = -1;
                return &config_input[0].sk1100_key[key];
            }
        }
        return NULL;
    }

    static const char* names[6] = {"Left", "Right", "Up", "Down", "Fire 1", "Fire 2"};
    for (int p = 0; p < 2; ++p)
    {
        SDL_Scancode* bindings[6] = {
            &config_input[p].key_left, &config_input[p].key_right,
            &config_input[p].key_up, &config_input[p].key_down,
            &config_input[p].key_left_button, &config_input[p].key_right_button
        };
        for (int i = 0; i < 6; ++i)
        {
            if (bindings[i] != configured_key && *bindings[i] == scancode)
            {
                *action = names[i];
                *player = p;
                return bindings[i];
            }
        }
        for (int k = 0; k < COLECO_KEYPAD_KEY_COUNT; ++k)
        {
            SDL_Scancode* binding = &config_input[p].keypad_key[k];
            if (binding != configured_key && *binding == scancode)
            {
                *action = keypad_action_name(k);
                *player = p;
                return binding;
            }
        }
    }
    return NULL;
}

static SDL_Scancode* find_machine_binding(SDL_Scancode scancode,
                                           const char** action, int* player)
{
    for (int key = 0; key < SK1100_KEYBOARD_KEY_COUNT; ++key)
    {
        if (config_input[0].sk1100_key[key] == scancode)
        {
            *action = sk1100_keyboard_layout[key].name;
            *player = -1;
            return &config_input[0].sk1100_key[key];
        }
    }
    static const char* names[6] = {"Left", "Right", "Up", "Down", "Fire 1", "Fire 2"};
    for (int p = 0; p < 2; ++p)
    {
        SDL_Scancode* bindings[6] = {
            &config_input[p].key_left, &config_input[p].key_right,
            &config_input[p].key_up, &config_input[p].key_down,
            &config_input[p].key_left_button, &config_input[p].key_right_button
        };
        for (int i = 0; i < 6; ++i)
        {
            if (*bindings[i] == scancode)
            {
                *action = names[i];
                *player = p;
                return bindings[i];
            }
        }
        for (int k = 0; k < COLECO_KEYPAD_KEY_COUNT; ++k)
        {
            if (config_input[p].keypad_key[k] == scancode)
            {
                *action = keypad_action_name(k);
                *player = p;
                return &config_input[p].keypad_key[k];
            }
        }
    }
    return NULL;
}

static int* find_gamepad_conflict(int button, const char** action)
{
    int* bindings[2] = {
        &config_input[configured_player].gamepad_left_button,
        &config_input[configured_player].gamepad_right_button
    };
    static const char* names[2] = {"Fire 1", "Fire 2"};
    for (int i = 0; i < 2; ++i)
    {
        if (bindings[i] != configured_button && *bindings[i] == button)
        {
            *action = names[i];
            return bindings[i];
        }
    }
    return NULL;
}

bool gui_input_capture_event(const SDL_Event* event)
{
    if (input_capture_kind == InputCapture_None)
        return false;

    if (event->type == SDL_EVENT_KEY_DOWN && event->key.repeat == 0)
    {
        if (input_capture_kind == InputCapture_Keyboard)
        {
            if (event->key.scancode == SDL_SCANCODE_LGUI ||
                event->key.scancode == SDL_SCANCODE_RGUI)
            {
                SDL_snprintf(input_capture_message, sizeof(input_capture_message),
                             "Command is reserved for emulator and macOS shortcuts.");
                return true;
            }
            input_capture_message[0] = 0;
            input_capture_conflicting_hotkey = -1;
            input_capture_key = event->key.scancode;
            input_capture_conflicting_key = find_keyboard_conflict(
                input_capture_key, &input_capture_conflicting_action,
                &input_capture_conflicting_player);
            if (input_capture_conflicting_key)
                input_capture_phase = InputCapture_Conflict;
            else
                input_capture_phase = InputCapture_Confirm;
        }
        else if (input_capture_kind == InputCapture_Hotkey)
        {
            // Modifier keys build a chord; they are not useful service keys
            // on their own. The following non-modifier event carries their
            // state in event.key.mod on every SDL backend.
            if (modifier_scancode(event->key.scancode))
                return true;

            input_capture_message[0] = 0;
            input_capture_key = event->key.scancode;
            input_capture_mod = normalized_hotkey_mod((SDL_Keymod)event->key.mod);
            input_capture_conflicting_hotkey = find_hotkey_conflict(
                input_capture_key, input_capture_mod);
            if (input_capture_conflicting_hotkey >= 0)
            {
                input_capture_conflicting_action = config_hotkey_label(
                    (config_HotkeyIndex)input_capture_conflicting_hotkey);
                input_capture_conflicting_player = -1;
                input_capture_phase = InputCapture_Conflict;
            }
            else if (input_capture_mod == SDL_KMOD_NONE &&
                     (input_capture_conflicting_key = find_machine_binding(
                         input_capture_key, &input_capture_conflicting_action,
                         &input_capture_conflicting_player)))
            {
                // The service-key panel owns this decision. Replace may
                // explicitly unassign the machine key; SC-3000 capture can
                // never alter a service binding in the opposite direction.
                input_capture_conflicting_hotkey = -2;
                input_capture_phase = InputCapture_Conflict;
            }
            else
                input_capture_phase = InputCapture_Confirm;
        }
        return true;
    }
    if (event->type == SDL_EVENT_KEY_UP)
        return true;

    if (input_capture_kind == InputCapture_Gamepad &&
        event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
    {
        if (configured_player < 0 || configured_player >= 2 ||
            !application_gamepad[configured_player])
            return true;
        const SDL_JoystickID id = SDL_GetGamepadID(application_gamepad[configured_player]);
        if (event->gbutton.which != id)
            return true;
        input_capture_button = event->gbutton.button;
        input_capture_conflicting_button = find_gamepad_conflict(
            input_capture_button, &input_capture_conflicting_action);
        input_capture_conflicting_player = configured_player;
        if (input_capture_conflicting_button)
            input_capture_phase = InputCapture_Conflict;
        else
            input_capture_phase = InputCapture_Confirm;
        return true;
    }
    return event->type == SDL_EVENT_GAMEPAD_BUTTON_UP;
}

static void popup_modal_input_binding(void)
{
    if (input_capture_popup_requested)
    {
        ImGui::OpenPopup("Input Binding");
        input_capture_popup_requested = false;
    }
    if (!ImGui::BeginPopupModal("Input Binding", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    gui_in_use = true;
    if (input_capture_phase == InputCapture_Complete ||
        input_capture_phase == InputCapture_Cancelled)
    {
        ImGui::CloseCurrentPopup();
        input_capture_kind = InputCapture_None;
        ImGui::EndPopup();
        return;
    }

    char category[32];
    if (input_capture_kind == InputCapture_Hotkey)
        SDL_snprintf(category, sizeof(category), "Emulator");
    else if (input_capture_kind == InputCapture_Keyboard && configured_sk1100_key >= 0)
        SDL_snprintf(category, sizeof(category), "SC-3000");
    else
        SDL_snprintf(category, sizeof(category), "Player %d%s", configured_player + 1,
                     input_capture_kind == InputCapture_Gamepad ? " gamepad" : "");
    ImGui::Text("%s: %s", category, configured_action ? configured_action : "Input");
    if (input_capture_phase == InputCapture_Waiting)
    {
        if (input_capture_kind == InputCapture_Gamepad &&
            (configured_player < 0 || !application_gamepad[configured_player]))
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.25f, 1.0f), "No gamepad detected for Player %d.", configured_player + 1);
        else if (input_capture_kind == InputCapture_Hotkey)
            ImGui::TextUnformatted("Press a key or key combination to assign...");
        else
            ImGui::Text("Press a %s to assign...", input_capture_kind == InputCapture_Keyboard ? "key" : "gamepad button");
        if (input_capture_message[0])
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.25f, 1.0f), "%s", input_capture_message);
        ImGui::TextDisabled("Every key, including Escape, can be assigned.");
    }
    else if (input_capture_phase == InputCapture_Confirm)
    {
        config_Hotkey candidate;
        candidate.key = input_capture_key;
        candidate.mod = input_capture_mod;
        config_update_hotkey_string(&candidate);
        const char* host_name = input_capture_kind == InputCapture_Hotkey
            ? candidate.str : input_capture_kind == InputCapture_Keyboard
            ? SDL_GetScancodeName(input_capture_key) : "gamepad button";
        ImGui::Text("New binding: %s", host_name);
    }
    else if (input_capture_phase == InputCapture_Conflict)
    {
        config_Hotkey candidate;
        candidate.key = input_capture_key;
        candidate.mod = input_capture_mod;
        config_update_hotkey_string(&candidate);
        const char* host_name = input_capture_kind == InputCapture_Hotkey
            ? candidate.str : input_capture_kind == InputCapture_Keyboard
            ? SDL_GetScancodeName(input_capture_key) : "gamepad button";
        if (input_capture_conflicting_player >= 0)
            ImGui::Text("%s is already assigned to Player %d %s.", host_name,
                        input_capture_conflicting_player + 1,
                        input_capture_conflicting_action);
        else
            ImGui::Text("%s is already assigned to %s.", host_name,
                        input_capture_conflicting_action);
        if (input_capture_kind == InputCapture_Keyboard &&
            input_capture_conflicting_hotkey >= 0)
            ImGui::TextWrapped("This is an emulator service key. Change it from Configure Emulator Hotkeys; the SC-3000 keyboard cannot replace it.");
        if (input_capture_conflicting_hotkey >= 0 || input_capture_kind == InputCapture_Hotkey)
            if (input_capture_conflicting_hotkey >= 0 || input_capture_kind == InputCapture_Hotkey)
            ImGui::TextDisabled("Press another key, or Cancel.");
        else
            ImGui::TextDisabled("Swap the two, Replace (the other becomes unassigned),\npress another key, or Cancel.");
        else
            ImGui::TextDisabled("Swap the two, Replace (the other becomes unassigned),\npress another key, or Cancel.");
    }

    ImGui::Separator();
    // A conflict with another machine binding (a joypad key, a gamepad button,
    // an SK-1100 key) can be settled here: Swap gives the other action the key
    // this one had, Replace leaves the other action unassigned. A service key
    // stays blocked: the machine bindings cannot take one away.
    const bool machine_conflict = input_capture_phase == InputCapture_Conflict &&
        input_capture_kind != InputCapture_Hotkey && input_capture_conflicting_hotkey < 0 &&
        (input_capture_conflicting_key != NULL || input_capture_conflicting_button != NULL);
    if (machine_conflict)
    {
        if (ImGui::Button("Swap", ImVec2(100, 0)))
        {
            if (input_capture_kind == InputCapture_Keyboard)
            {
                *input_capture_conflicting_key = *configured_key;
                *configured_key = input_capture_key;
            }
            else
            {
                *input_capture_conflicting_button = *configured_button;
                *configured_button = input_capture_button;
            }
            finish_input_capture(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Replace", ImVec2(100, 0)))
        {
            if (input_capture_kind == InputCapture_Keyboard)
            {
                *input_capture_conflicting_key = SDL_SCANCODE_UNKNOWN;
                *configured_key = input_capture_key;
            }
            else
            {
                *input_capture_conflicting_button = SDL_GAMEPAD_BUTTON_INVALID;
                *configured_button = input_capture_button;
            }
            finish_input_capture(true);
        }
        ImGui::SameLine();
    }
    else
    {
        if (input_capture_phase != InputCapture_Confirm)
            ImGui::BeginDisabled();
        if (ImGui::Button("OK", ImVec2(100, 0)))
            apply_pending_input_capture();
        if (input_capture_phase != InputCapture_Confirm)
            ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (ImGui::Button("Cancel", ImVec2(100, 0)))
        finish_input_capture(false);

    // Clear and Default, as the emulator hotkeys already had. Escape is an
    // assignable key here, so emptying a binding needs its own button.
    if (input_capture_kind != InputCapture_Hotkey)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (ImGui::Button("Clear", ImVec2(70, 0)))
        {
            if (input_capture_kind == InputCapture_Keyboard)
                *configured_key = SDL_SCANCODE_UNKNOWN;
            else
                *configured_button = SDL_GAMEPAD_BUTTON_INVALID;
            finish_input_capture(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Default", ImVec2(70, 0)))
        {
            // Offered as a candidate, like a pressed key: if the default is
            // taken by another action the conflict choices appear.
            if (input_capture_kind == InputCapture_Keyboard)
            {
                SDL_Scancode value = SDL_SCANCODE_UNKNOWN;
                if (config_default_key_binding(configured_key, &value))
                {
                    input_capture_key = value;
                    input_capture_conflicting_hotkey = -1;
                    input_capture_conflicting_key = value == SDL_SCANCODE_UNKNOWN ? NULL :
                        find_keyboard_conflict(value, &input_capture_conflicting_action,
                                               &input_capture_conflicting_player);
                    input_capture_phase = input_capture_conflicting_key
                        ? InputCapture_Conflict : InputCapture_Confirm;
                }
            }
            else
            {
                int value = SDL_GAMEPAD_BUTTON_INVALID;
                if (config_default_button_binding(configured_button, &value))
                {
                    input_capture_button = value;
                    input_capture_conflicting_button = find_gamepad_conflict(
                        value, &input_capture_conflicting_action);
                    input_capture_conflicting_player = configured_player;
                    input_capture_phase = input_capture_conflicting_button
                        ? InputCapture_Conflict : InputCapture_Confirm;
                }
            }
        }
    }
    ImGui::EndPopup();
}

static bool load_sk1100_keyboard_texture(void)
{
    if (sk1100_keyboard_texture != ImTextureID_Invalid)
        return true;

#if defined(_WIN32)
    WindowsResourceView keyboard_png;
    if (load_windows_resource(IDR_SC3000_KEYBOARD_PNG, &keyboard_png))
    {
        sk1100_keyboard_texture = renderer_load_png_texture_memory(
            keyboard_png.data, keyboard_png.size,
            &sk1100_keyboard_texture_width,
            &sk1100_keyboard_texture_height);
        if (sk1100_keyboard_texture != ImTextureID_Invalid)
            return true;
    }
#endif

    // Development fallback and the normal macOS bundle/resource path.
    const char* base_path = SDL_GetBasePath();
    const char* relative_paths[] = {
        "../Resources/sc3000-keyboard-compact.png",
        "../../desktop-shared/assets/sc3000-keyboard-compact.png",
        "../desktop-shared/assets/sc3000-keyboard-compact.png",
        "sc3000-keyboard-compact.png"
    };
    for (const char* relative : relative_paths)
    {
        std::string path = base_path ? base_path : "";
        path += relative;
        sk1100_keyboard_texture = renderer_load_png_texture(
            path.c_str(), &sk1100_keyboard_texture_width,
            &sk1100_keyboard_texture_height);
        if (sk1100_keyboard_texture != ImTextureID_Invalid)
            return true;
    }
    return false;
}

static void begin_sk1100_key_capture(int key)
{
    sk1100_selected_key = key;
    configured_sk1100_key = key;
    configured_key = &config_input[0].sk1100_key[key];
    configured_button = NULL;
    configured_hotkey = NULL;
    configured_hotkey_index = -1;
    configured_player = -1;
    configured_action = sk1100_keyboard_layout[key].name;
    input_capture_kind = InputCapture_Keyboard;
    input_capture_phase = InputCapture_Waiting;
    input_capture_conflicting_key = NULL;
    input_capture_conflicting_hotkey = -1;
    input_capture_conflicting_button = NULL;
    input_capture_conflicting_action = NULL;
    input_capture_conflicting_player = -1;
    input_capture_message[0] = 0;
    input_capture_popup_requested = true;
}

static void begin_hotkey_capture(int index)
{
    configured_hotkey_index = index;
    configured_hotkey = &config_hotkeys[index];
    configured_key = NULL;
    configured_button = NULL;
    configured_player = -1;
    configured_sk1100_key = -1;
    configured_action = config_hotkey_label((config_HotkeyIndex)index);
    input_capture_kind = InputCapture_Hotkey;
    input_capture_phase = InputCapture_Waiting;
    input_capture_key = SDL_SCANCODE_UNKNOWN;
    input_capture_mod = SDL_KMOD_NONE;
    input_capture_conflicting_key = NULL;
    input_capture_conflicting_button = NULL;
    input_capture_conflicting_hotkey = -1;
    input_capture_conflicting_action = NULL;
    input_capture_conflicting_player = -1;
    input_capture_message[0] = 0;
    input_capture_popup_requested = true;
}

static void hotkey_row(int index)
{
    ImGui::PushID(index);
    ImGui::TextUnformatted(config_hotkey_label((config_HotkeyIndex)index));
    ImGui::SameLine(210.0f);
    if (ImGui::Button(config_hotkeys[index].str, ImVec2(145.0f, 0.0f)))
        begin_hotkey_capture(index);
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
    {
        config_hotkeys[index].key = SDL_SCANCODE_UNKNOWN;
        config_hotkeys[index].mod = SDL_KMOD_NONE;
        config_update_hotkey_string(&config_hotkeys[index]);
        config_write();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Default"))
    {
        config_hotkeys[index] = config_default_hotkey((config_HotkeyIndex)index);
        config_write();
    }
    ImGui::PopID();
}

static void window_hotkey_config(void)
{
    gui_set_next_window_size_em(29.7f, 36.1f);
    if (!ImGui::Begin("Emulator Hotkeys", &show_hotkey_config))
    {
        ImGui::End();
        return;
    }
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) ||
        ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
        gui_in_use = true;

    ImGui::TextUnformatted("Click a binding, then press its new key combination.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset all"))
        ImGui::OpenPopup("Reset all hotkeys?");

    ImGui::SeparatorText("Emulator");
    for (int i = 0; i < config_HotkeyIndex_DebugStepInto; ++i)
        hotkey_row(i);
    ImGui::SeparatorText("Debugger");
    for (int i = config_HotkeyIndex_DebugStepInto; i < config_HotkeyIndex_COUNT; ++i)
        hotkey_row(i);

    if (ImGui::BeginPopupModal("Reset all hotkeys?", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Restore every emulator and debugger hotkey to its default?");
        if (ImGui::Button("Reset", ImVec2(100, 0)))
        {
            for (int i = 0; i < config_HotkeyIndex_COUNT; ++i)
                config_hotkeys[i] = config_default_hotkey((config_HotkeyIndex)i);
            config_write();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}

static bool sk1100_is_latching_key(int key)
{
    const char* id = sk1100_keyboard_layout[key].id;
    return strcmp(id, "func") == 0 || strcmp(id, "ctrl") == 0 ||
           strcmp(id, "shift_l") == 0 || strcmp(id, "shift_r") == 0 ||
           strcmp(id, "graph") == 0;
}

bool gui_sk1100_virtual_matrix_down(int row, int mask)
{
    for (int key = 0; key < SK1100_KEYBOARD_KEY_COUNT; ++key)
    {
        const SK1100KeyboardKeyLayout& layout = sk1100_keyboard_layout[key];
        if (sk1100_virtual_key_down[key] && layout.row == row &&
            (layout.mask & mask) != 0)
            return true;
    }
    return false;
}

static bool sk1100_physical_matrix_down(int row, int mask)
{
    const bool* keyboard_state = SDL_GetKeyboardState(NULL);
    if (!keyboard_state)
        return false;
    for (int key = 0; key < SK1100_KEYBOARD_KEY_COUNT; ++key)
    {
        const SK1100KeyboardKeyLayout& layout = sk1100_keyboard_layout[key];
        const SDL_Scancode host = config_input[0].sk1100_key[key];
        if (layout.row == row && (layout.mask & mask) != 0 &&
            host != SDL_SCANCODE_UNKNOWN && keyboard_state[host])
            return true;
    }
    return false;
}

static void sk1100_set_virtual_key(int key, bool pressed)
{
    if (key < 0 || key >= SK1100_KEYBOARD_KEY_COUNT ||
        sk1100_virtual_key_down[key] == pressed)
        return;

    const SK1100KeyboardKeyLayout& layout = sk1100_keyboard_layout[key];
    sk1100_virtual_key_down[key] = pressed;
    if (layout.row < 0)
    {
        if (pressed)
            emu_pause_key_pressed();
        // RESET/NMI is an edge, never a matrix contact or a latched key.
        sk1100_virtual_key_down[key] = false;
        return;
    }

    if (pressed || (!gui_sk1100_virtual_matrix_down(layout.row, layout.mask) &&
                    !sk1100_physical_matrix_down(layout.row, layout.mask)))
        emu_keyboard_matrix_key(layout.row, layout.mask, pressed);
}

void gui_sk1100_release_virtual_keys(void)
{
    sk1100_mouse_key = -1;
    for (int key = 0; key < SK1100_KEYBOARD_KEY_COUNT; ++key)
        if (sk1100_virtual_key_down[key])
            sk1100_set_virtual_key(key, false);
}

static void window_sk1100_keyboard_config(void)
{
    if (emu_get_core()->GetMachineType() == MachineType::MSX || emu_get_core()->GetMachineType() == MachineType::SordM5) {
        show_sk1100_keyboard_config = false;
        return;
    }
    const float chrome_em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(44.4f * chrome_em, 17.8f * chrome_em), ImGuiCond_Appearing);
    const bool window_visible = ImGui::Begin("SC-3000 Keyboard", &show_sk1100_keyboard_config);
    if (!show_sk1100_keyboard_config)
        gui_sk1100_release_virtual_keys();
    if (!window_visible)
    {
        ImGui::End();
        return;
    }

    if (sk1100_keyboard_configure_mode &&
        (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) ||
         ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)))
        gui_in_use = true;

    ImGui::TextUnformatted("Mode:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Use", !sk1100_keyboard_configure_mode))
    {
        sk1100_keyboard_configure_mode = false;
        sk1100_selected_key = -1;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Configure", sk1100_keyboard_configure_mode))
    {
        gui_sk1100_release_virtual_keys();
        sk1100_keyboard_configure_mode = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled(sk1100_keyboard_configure_mode
        ? "Select a key, then press its host key."
        : "Click keys; Shift, Ctrl, Func and Graph latch.");
    if (sk1100_keyboard_configure_mode)
        ImGui::SameLine();
    if (sk1100_keyboard_configure_mode && ImGui::SmallButton("Reset all"))
        ImGui::OpenPopup("Reset SC-3000 keyboard?");

    if (ImGui::BeginPopupModal("Reset SC-3000 keyboard?", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Restore every SC-3000 host-key assignment to its default?");
        if (ImGui::Button("Reset", ImVec2(100, 0)))
        {
            for (int key = 0; key < SK1100_KEYBOARD_KEY_COUNT; ++key)
            {
                config_input[0].sk1100_key[key] = sk1100_keyboard_layout[key].default_scancode;
                config_input[1].sk1100_key[key] = sk1100_keyboard_layout[key].default_scancode;
            }
            config_write();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (!load_sk1100_keyboard_texture())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                           "Keyboard image resource could not be loaded.");
        ImGui::End();
        return;
    }

    const float available_width = ImGui::GetContentRegionAvail().x;
    // The source stays at 2080x656 for a crisp Retina texture. The binding
    // dialog only needs a compact overview, so its default presentation is
    // 768x240: halfway between the initial large view and the 50% test, using
    // classic resolution-friendly dimensions. X/Y hitboxes scale separately,
    // so the sub-1% aspect adjustment does not disturb key alignment.
    // That 768 is expressed in em (42.7 x the 18px chrome font): as a raw
    // pixel cap the artwork stayed put while the surrounding text grew with
    // the monitor DPI, which made the keyboard look shrunken on a 4K panel.
    const float image_width = std::min(42.7f * ImGui::GetFontSize(), available_width);
    const float image_height = image_width * (240.0f / 768.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Image(sk1100_keyboard_texture, ImVec2(image_width, image_height));

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float scale_x = image_width / (float)SK1100_KEYBOARD_IMAGE_WIDTH;
    const float scale_y = image_height / (float)SK1100_KEYBOARD_IMAGE_HEIGHT;
    // The original SC-3000 power lamp is green. The source art contains a
    // static red placeholder, so replace it with live emulator state. SVG
    // coordinates (2089,390) are relative to viewBox origin (103,321).
    const ImVec2 led_center(origin.x + 1986.0f * scale_x,
                            origin.y + 69.0f * scale_y);
    const float led_radius = 16.0f * std::min(scale_x, scale_y);
    const bool power_on = !emu_is_empty();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        led_center, led_radius,
        power_on ? IM_COL32(45, 220, 85, 255) : IM_COL32(25, 55, 35, 255));
    ImGui::GetWindowDrawList()->AddCircle(
        led_center, led_radius, IM_COL32(10, 20, 14, 255), 0, 1.5f);
    const bool* keyboard_state = SDL_GetKeyboardState(NULL);
    int hovered_key = -1;
    for (int key = 0; key < SK1100_KEYBOARD_KEY_COUNT; ++key)
    {
        const SK1100KeyboardKeyLayout& layout = sk1100_keyboard_layout[key];
        const ImVec2 min(origin.x + layout.x * scale_x, origin.y + layout.y * scale_y);
        const ImVec2 max(min.x + layout.w * scale_x, min.y + layout.h * scale_y);
        if (mouse.x >= min.x && mouse.x < max.x && mouse.y >= min.y && mouse.y < max.y)
            hovered_key = key;

        ImU32 color = 0;
        float thickness = 2.0f;
        if (sk1100_keyboard_configure_mode && key == sk1100_selected_key)
            color = IM_COL32(20, 220, 235, 255);
        if (sk1100_keyboard_configure_mode &&
            input_capture_phase == InputCapture_Conflict &&
            config_input[0].sk1100_key[key] == input_capture_key)
        {
            color = IM_COL32(255, 75, 55, 255);
            thickness = 3.0f;
        }
        if (key == hovered_key)
        {
            color = IM_COL32(255, 215, 45, 255);
            thickness = 3.0f;
        }
        const SDL_Scancode host_key = config_input[0].sk1100_key[key];
        if (sk1100_virtual_key_down[key] ||
            (keyboard_state && host_key != SDL_SCANCODE_UNKNOWN && keyboard_state[host_key]))
        {
            color = IM_COL32(50, 235, 100, 255);
            thickness = 3.0f;
        }
        if (color)
            ImGui::GetWindowDrawList()->AddRect(min, max, color, 4.0f, 0, thickness);
    }

    if (hovered_key >= 0)
    {
        const SDL_Scancode binding = config_input[0].sk1100_key[hovered_key];
        if (sk1100_keyboard_configure_mode)
        {
            ImGui::SetTooltip("%s\nHost: %s", sk1100_keyboard_layout[hovered_key].name,
                              binding == SDL_SCANCODE_UNKNOWN ? "Unassigned" : SDL_GetScancodeName(binding));
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                input_capture_kind == InputCapture_None)
                begin_sk1100_key_capture(hovered_key);
        }
        else
        {
            const bool latch = sk1100_is_latching_key(hovered_key);
            ImGui::SetTooltip("%s%s", sk1100_keyboard_layout[hovered_key].name,
                              latch ? " (click to latch/release)" : "");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                if (latch)
                    sk1100_set_virtual_key(hovered_key,
                                           !sk1100_virtual_key_down[hovered_key]);
                else
                {
                    sk1100_mouse_key = hovered_key;
                    sk1100_set_virtual_key(hovered_key, true);
                }
            }
        }
    }

    // Releasing outside the key or even outside the image must still release
    // the ordinary key that received the original mouse-down.
    if (!sk1100_keyboard_configure_mode && sk1100_mouse_key >= 0 &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        sk1100_set_virtual_key(sk1100_mouse_key, false);
        sk1100_mouse_key = -1;
    }

    ImGui::End();
}

static void popup_modal_about(void)
{
    if (ImGui::BeginPopupModal("About " GEARSF7000_TITLE, NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("%s %s", GEARSF7000_TITLE, build_info_version());
        ImGui::Text("Build: %s", build_info_revision());
        // The line that says whether this is the binary you just made, or one
        // from yesterday still sitting in the .app bundle.
        ImGui::Text("Compiled: %s", build_info_timestamp());
        
        ImGui::Separator();
        
        ImGui::Text("By Saverio Russo, based on Gearcoleco by Ignacio Sanchez");
        ImGui::Text("%s is licensed under the AGPL-3.0 License, see LICENSE for more information.\n"
                    "The parts from Gearcoleco and Gearsystem remain under the GPL-3.0 License.", GEARSF7000_TITLE);
        
        ImGui::Separator();

        if (ImGui::BeginTabBar("##Tabs", ImGuiTabBarFlags_None))
        {
            if (ImGui::BeginTabItem("Special thanks to"))
            {
                ImGui::BeginChild("backers", ImVec2(0, 100), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                ImGui::Text("%s", BACKERS_STR);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("C-BIOS"))
            {
                ImGui::BeginChild("cbios_license", ImVec2(0, 250), true);
                ImGui::TextWrapped("%s", emu_get_cbios_license());
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("LICENSE"))
            {
                ImGui::BeginChild("license", ImVec2(0, 100), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                ImGui::TextUnformatted(AGPL_LICENSE_STR);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("GPL-3.0"))
            {
                ImGui::BeginChild("gpl_license", ImVec2(0, 100), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                ImGui::TextUnformatted(GPL_LICENSE_STR);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Separator();

        #if defined(_M_ARM64)
        ImGui::Text("Windows ARM64 build");
        #endif
        #if defined(_M_X64)
        ImGui::Text("Windows 64 bit build");
        #endif
        #if defined(_M_IX86)
        ImGui::Text("Windows 32 bit build");
        #endif
        #if defined(__linux__) && defined(__x86_64__)
        ImGui::Text("Linux 64 bit build");
        #endif
        #if defined(__linux__) && defined(__i386__)
        ImGui::Text("Linux 32 bit build");
        #endif
        #if defined(__linux__) && defined(__arm__)
        ImGui::Text("Linux ARM build");
        #endif
        #if defined(__linux__) && defined(__aarch64__)
        ImGui::Text("Linux ARM64 build");
        #endif
        #if defined(__APPLE__) && defined(__arm64__ )
        ImGui::Text("macOS build (Apple Silicon)");
        #endif
        #if defined(__APPLE__) && defined(__x86_64__)
        ImGui::Text("macOS build (Intel)");
        #endif
        #if defined(_MSC_FULL_VER)
        ImGui::Text("Microsoft C++ %d", _MSC_FULL_VER);
        #endif
        #if defined(__CLR_VER)
        ImGui::Text("CLR version: %d", __CLR_VER);
        #endif
        #if defined(__MINGW32__)
        ImGui::Text("MinGW 32 bit (%d.%d)", __MINGW32_MAJOR_VERSION, __MINGW32_MINOR_VERSION);
        #endif
        #if defined(__MINGW64__)
        ImGui::Text("MinGW 64 bit (%d.%d)", __MINGW64_VERSION_MAJOR, __MINGW64_VERSION_MINOR);
        #endif
        #if defined(__GNUC__) && !defined(__llvm__) && !defined(__INTEL_COMPILER)
        ImGui::Text("GCC %d.%d.%d", (int)__GNUC__, (int)__GNUC_MINOR__, (int)__GNUC_PATCHLEVEL__);
        #endif
        #if defined(__clang_version__)
        ImGui::Text("Clang %s", __clang_version__);
        #endif
        #if defined(__TIMESTAMP__)
        ImGui::Text("Generated on: %s", __TIMESTAMP__);
        #endif

        ImGui::Separator();

        #ifdef DEBUG
        ImGui::Text("define: DEBUG");
        #endif
        #ifdef DEBUG_GEARSF7000
        ImGui::Text("define: DEBUG_GEARSF7000");
        #endif
        #ifdef __cplusplus
        ImGui::Text("define: __cplusplus = %d", (int)__cplusplus);
        #endif
        #ifdef __STDC__
        ImGui::Text("define: __STDC__ = %d", (int)__STDC__);
        #endif
        #ifdef __STDC_VERSION__
        ImGui::Text("define: __STDC_VERSION__ = %d", (int)__STDC_VERSION__);
        #endif
        
        ImGui::Separator();

        ImGui::Text("SDL3 %d.%d.%d", SDL_VERSIONNUM_MAJOR(application_sdl_version), SDL_VERSIONNUM_MINOR(application_sdl_version), SDL_VERSIONNUM_MICRO(application_sdl_version));
        ImGui::Text("SDL_GPU %s", renderer_gpu_driver ? renderer_gpu_driver : "unknown");
        #if defined(GLEW_VERSION)
        ImGui::Text("GLEW %s", renderer_glew_version);
        #endif
        ImGui::Text("Dear ImGui %s (%d)", IMGUI_VERSION, IMGUI_VERSION_NUM);

        ImGui::Separator();

        for (int i = 0; i < 2; i++)
        {
            if (application_gamepad[i])
                ImGui::Text("Gamepad detected for Player %d", i+1);
            else
                ImGui::Text("No gamepad detected for Player %d", i+1);
        }

        if (application_gamepad_mappings > 0)
            ImGui::Text("%d gamepad mappings loaded", application_gamepad_mappings);
        else
            ImGui::Text("Gamepad database not found");

        ImGui::Separator();

        if (ImGui::Button("OK", ImVec2(120, 0))) 
        {
            ImGui::CloseCurrentPopup();
            dialog_in_use = false;
        }
        ImGui::SetItemDefaultFocus();

        ImGui::EndPopup();
    }
}

static void popup_modal_bios(void)
{
    if (ImGui::BeginPopupModal("IPL", NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {      
        ImGui::Text("IMPORTANT! IPL ROM is required to run SF-7000.");
        ImGui::Text(" ");
        ImGui::Text("Load a BIOS file using the \"SF-7000 -> Load IPL... \" menu option.");
        ImGui::Text(" ");
        
        ImGui::Separator();

        if (ImGui::Button("OK", ImVec2(120, 0))) 
        {
            ImGui::CloseCurrentPopup();
            dialog_in_use = false;
        }
        ImGui::SetItemDefaultFocus();

        ImGui::EndPopup();
    }
}

static void update_palette(void)
{
    emu_predefined_palette(*emu_palette_setting());
}

// Short board names for the measured status area. The full name is in the
// tooltip and the window title; no fixed glyph-count width is assumed.
static const char* board_short_name(SystemModel model)
{
    switch (model)
    {
        case SystemModel::SG1000:             return "SG1K";
        case SystemModel::SG1000SK1100:       return "SG1K+KB";
        case SystemModel::SC3000Japan:        return "SC3K-J";
        case SystemModel::SC3000Export:       return "SC3K-E";
        case SystemModel::SC3000JapanSF7000:  return "SF7K-J";
        case SystemModel::SC3000ExportSF7000: return "SF7K-E";
        case SystemModel::SMSNTSC:            return "SMS-NTSC";
        case SystemModel::SMSPAL:             return "SMS-PAL";
        case SystemModel::SegaMarkIII:        return "MARK-III";
        case SystemModel::MSX1NTSC: return "MSX1-NTSC";
        case SystemModel::MSX1PAL: return "MSX1-PAL";
        case SystemModel::SordM5NTSC: return "M5-NTSC";
        case SystemModel::SordM5PAL: return "M5-PAL";
        case SystemModel::GameGearJapan: return "GG-J";
        case SystemModel::GameGearExport: return "GG-E";
        case SystemModel::ColecoVision:       return "COLECO";
        case SystemModel::ColecoVisionSGM:    return "COLECO+SGM";
        default:                              return "?";
    }
}

// The cartridge mapper actually in use - with Auto that is what detection
// chose, which the Mapper menu does not say.
static const char* mapper_short_name(Cartridge::CartridgeTypes type)
{
    switch (type)
    {
        case Cartridge::SG1000_1K:                  return "SG1K-1K";
        case Cartridge::SG1000_16K:                 return "SG1K-16K";
        case Cartridge::SC3000_2K:                  return "SC3K-2K";
        case Cartridge::SC3000_32K:                 return "SC3K-32K";
        case Cartridge::SC3000_16K_AT_8000:         return "SC3K-16K";
        case Cartridge::SF7000IPL:                  return "SF7K-IPL";
        case Cartridge::SC3000_ASC16L:              return "ASC16L";
        case Cartridge::SC3000_ASC16LS:             return "INTERNAL";
        case Cartridge::SC3000_FLAT64K:             return "FLAT64K";
        case Cartridge::SG1000_DAHJEE_TYPE_A:       return "DAHJEE-A";
        case Cartridge::SG1000_DAHJEE_TYPE_B:       return "DAHJEE-B";
        case Cartridge::SC3000_SURVIVORS_MULTICART: return "SURV-MUL";
        case Cartridge::SC3000_SURVIVORS_MEGACART:  return "SURV-MEG";
        case Cartridge::SG1000_OTHELLO_2K:          return "OTHELLO";
        case Cartridge::SG1000_CASTLE_8K:           return "CASTLE";
        case Cartridge::SG1000_SEGA_315_5208:       return "315-5208";
        case Cartridge::SG1000_SEGA_315_5208_DAHJEE_B: return "5208+DJB";
        case Cartridge::SMS_SEGA_MAPPER:            return "SMS-SEGA";
        case Cartridge::MSX_LINEAR: return "MSX-LINEAR";
        case Cartridge::SORD_M5_LINEAR: return "M5-LINEAR";
        case Cartridge::COLECOVISION:               return "CV-STD";
        case Cartridge::COLECO_MEGACART:            return "CV-MEGA";
        default:                                    return "--------";
    }
}

// One measured group: proportional fonts and long board names cannot overlap.
// Drop optional details before sacrificing access to the actual menus.
static void draw_menu_bar_status(void)
{
    const auto model = emu_get_core()->GetSystemModel();
    const Cartridge* cart = emu_get_core()->GetCartridge();
    const bool ready = cart && cart->IsReady();
    const std::string board = board_short_name(model);
    const std::string mapper = ready ? mapper_short_name(cart->GetType()) : "";
    std::string mcp;
    bool mcpRunning = false;
#if GEARSF7000_ENABLE_MCP
    mcpRunning = emu_mcp_is_running();
    if (mcpRunning)
        mcp = emu_mcp_get_transport_mode() == MCP_TRANSPORT_STDIO
            ? "MCP stdio" : "MCP :" + std::to_string(emu_mcp_get_port());
    else if (config_debug.debug)
        mcp = "MCP off";
#endif
    const float gap = ImGui::GetStyle().ItemSpacing.x * 2.0f;
    // Menu bars may have zero WindowPadding. Keep a font-scaled inset even then.
    const float right = ImGui::GetWindowWidth() - ImGui::GetFontSize();
    const float available = right - ImGui::GetCursorPosX() - gap;
    const auto* memory = emu_get_core()->GetMemory();
    const bool hasFirmware = memory->GetBiosSize() != 0;
    const bool biosOK = memory->IsBiosLoaded();
    const std::string bios = !hasFirmware ? "" : !biosOK ? "BIOS: ERROR"
        : emu_is_cbios_active() ? "BIOS: C-BIOS" : "BIOS: OK";
    struct Part { std::string text; ImVec4 color; };
    const ImVec4 muted = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    const ImVec4 green(0.25f, 0.85f, 0.35f, 1.0f);
    std::vector<Part> parts;
    const bool configError = !config_get_storage_error().empty();
    if (configError) parts.push_back({"CONFIG: ERROR", ImVec4(1.0f, 0.25f, 0.25f, 1.0f)});
    parts.push_back({board, muted});
    if (config_debug.debug && !mapper.empty()) parts.push_back({mapper, muted});
    if (!bios.empty()) parts.push_back({bios, biosOK ? muted : ImVec4(1.0f, 0.25f, 0.25f, 1.0f)});
    if (!mcp.empty()) parts.push_back({mcp, mcpRunning ? green : muted});
    auto widthOfParts = [&]() {
        float width = 0;
        for (const auto& part : parts) width += ImGui::CalcTextSize(part.text.c_str()).x;
        return width + (parts.size() - 1) * ImGui::CalcTextSize("  |  ").x;
    };
    // Preserve the firmware status when the menu bar gets narrow.
    while (parts.size() > 1 && widthOfParts() > available) {
        auto it = std::find_if(parts.begin(), parts.end(), [&](const Part& part) { return part.text == mapper; });
        if (it == parts.end()) it = std::find_if(parts.begin(), parts.end(), [&](const Part& part) { return part.text != (configError ? "CONFIG: ERROR" : bios); });
        parts.erase(it);
    }
    const float width = widthOfParts();
    if (width > available) return;
    ImGui::SameLine(right - width);
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) {
            ImGui::SameLine(0, 0); ImGui::TextDisabled("  |  "); ImGui::SameLine(0, 0);
        }
        ImGui::TextColored(parts[i].color, "%s", parts[i].text.c_str());
    }
    ImGui::EndGroup();
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(GetSystemModelName(model));
        ImGui::TextUnformatted(ready ? emu_get_mapper_name(cart->GetType()) : "No cartridge loaded");
        if (hasFirmware) {
            ImGui::TextUnformatted(bios.c_str());
            if (biosOK) ImGui::TextUnformatted(memory->GetBiosFilePath().c_str());
            if (!emu_get_machine_start_error().empty()) ImGui::TextUnformatted(emu_get_machine_start_error().c_str());
        }
        if (!mcp.empty()) ImGui::TextUnformatted(mcp.c_str());
        if (configError) {
            ImGui::Separator();
            ImGui::TextUnformatted("Settings could not be loaded or saved:");
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
            ImGui::TextUnformatted(config_get_storage_error().c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndTooltip();
    }
}

static void menu_continue(void)
{
    if (emu_get_core()->GetMachineType() == MachineType::MSX &&
        (emu_is_empty() || emu_has_pending_msx_firmware()))
        menu_reset();
    if (emu_is_empty()) {
        gui_set_status_message(emu_get_machine_start_error().empty()
            ? "Cannot start: no bootable cartridge or firmware is loaded."
            : emu_get_machine_start_error().c_str(), 8000);
        return;
    }
    emu_debug_continue();
}

static void menu_reset(void)
{
    //gui_set_status_message("Resetting...", 3000);

    emu_resume();

    Cartridge::ForceConfiguration config = get_force_config();

    const bool startPaused = config_emulator.start_paused && emu_debug_runtime_requested();
    emu_reset(config, startPaused);

    if (startPaused)
        emu_clear_video_buffer();
    char status[96];
    snprintf(status, sizeof(status), "Reset: %s active",
             GetSystemModelName(emu_get_core()->GetSystemModel()));
    gui_set_status_message(emu_get_machine_start_error().empty() ? status : emu_get_machine_start_error().c_str(), 8000);
}

static void menu_reset_paused(void)
{
    // One-shot action: always lands paused at the reset vector regardless
    // of the persistent "start paused" setting, so a breakpoint at 0000 can
    // be armed before the machine takes a single step.
    emu_resume();

    Cartridge::ForceConfiguration config = get_force_config();

    emu_reset(config, true);
    emu_clear_video_buffer();
    gui_debug_reset();
    char status[112];
    snprintf(status, sizeof(status), "Reset: %s active, paused at 0000",
             GetSystemModelName(emu_get_core()->GetSystemModel()));
    gui_set_status_message(status, 3000);
}

static void menu_eject(void)
{
    emu_eject_rom();
    emu_clear_video_buffer();
    gui_debug_reset();
    gui_set_status_message("Ejected", 3000);
}

static void menu_pause(void)
{
    if (emu_is_execution_stopped())
    {
        // Also clears a debugger stop. emu_resume() alone only releases the
        // core pause bit and used to leave a stepped machine apparently stuck.
        menu_continue();
        if (!emu_is_execution_stopped()) gui_set_status_message("Resumed", 3000);
    }
    else
    {
        gui_set_status_message("Paused", 3000);
        emu_pause();
    }
}

// The switching itself moved to emu_set_fast_forward(), so the menu, the
// keyboard shortcut and MCP all go through one implementation rather than
// three copies of the same three steps. What stays here is the part that only
// makes sense with a window: telling the user.
static void menu_ffwd(void)
{
    gui_set_status_message(config_emulator.ffwd ? "Fast Forward ON"
                                                : "Fast Forward OFF", 3000);
}

static void window_interface_settings(void)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float em = ImGui::GetFontSize();
    const ImVec2 available(std::max(1.0f, viewport->WorkSize.x),
                           std::max(1.0f, viewport->WorkSize.y));
    ImGui::SetNextWindowSize(ImVec2(std::min(28.0f * em, available.x),
                                  std::min(19.0f * em, available.y)), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(std::min(22.0f * em, available.x), std::min(12.0f * em, available.y)),
        available);
    if (restore_interface_settings_position)
    {
        ImGui::SetNextWindowPos(interface_settings_position);
        restore_interface_settings_position = false;
    }
    else
        ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::Begin("Interface Settings", &show_interface_settings,
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking))
    {
        gui_in_use = true;
        ImGui::TextWrapped("Changes take effect when you press Apply.");
        ImGui::Spacing();
        ImGui::TextUnformatted("Global scale");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputFloat("##interface_scale", &pending_interface_scale, 0.025f, 0.10f, "%.3f"))
            pending_interface_scale = std::clamp(pending_interface_scale, 0.75f, 2.0f);
        ImGui::TextUnformatted("Font size");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::Combo("##interface_font", &pending_interface_font,
                     "Very Small\0Small\0Medium\0Large\0\0");
        ImGui::TextUnformatted("Density");
        ImGui::SetNextItemWidth(-1.0f);
        // Configuration IDs are historical: 1 = Minimal, 2 = Compact.
        ImGui::Combo("##interface_density", &pending_interface_density,
                     "Normal\0Minimal\0Compact\0\0");
        ImGui::Separator();
        if (ImGui::Button("Restore Defaults"))
        {
            pending_interface_scale = 1.0f;
            pending_interface_font = 0;
            pending_interface_density = 0;
        }
        const bool changed = pending_interface_scale != config_debug.ui_scale
            || pending_interface_font != config_debug.font_size
            || pending_interface_density != config_debug.ui_density;
        ImGui::BeginDisabled(!changed);
        if (ImGui::Button("Apply"))
        {
            interface_settings_position = ImGui::GetWindowPos();
            apply_interface_settings = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close"))
            show_interface_settings = false;
        if (changed)
            ImGui::TextWrapped("Unapplied changes are discarded when you close this panel.");
    }
    ImGui::End();
}

static bool shader_slider(const char* id, float* value, const char* key, const char* format)
{
    const auto* parameter = GetVideoCatalog().ShaderParameter(key);
    return parameter && ImGui::SliderFloat(id, value, parameter->minimum, parameter->maximum, format);
}

static void window_crt_lottes_setup(void)
{
    if (!show_crt_lottes_setup)
        return;

    config_VideoOutput& output = video_setup_profile
        ? *video_setup_profile : config_video_output_for_mode(config_debug.debug);

    // Not AlwaysAutoResize: combined with the SetNextItemWidth(-1.0f)
    // sliders below, auto-resize's width feeds back into itself frame over
    // frame (fill-available-width depends on last frame's auto-fit result)
    // and the window visibly shrinks toward nothing - a fixed initial size
    // like the other CRT windows in this session avoids the loop entirely.
    gui_set_next_window_size_em(18.9f, 18.9f);
    ImGui::Begin("CRT (Lottes) Setup", &show_crt_lottes_setup);

    if (output.postprocessing != POSTPROCESSING_CRT_LOTTES)
        ImGui::TextWrapped("Postprocessing is not set to CRT (Lottes) - these settings are saved "
            "but won't be visible until you select it in this profile's Postprocessing menu.");

    // The scanline term needs the real output to have more rows than
    // emu_texture (see crt_lottes.frag's own OutputSize comment) - at
    // exactly 1x it mathematically cannot produce any banding, same as the
    // debug-upscale bug found earlier this session, just triggered here by
    // a Scale/window-size combination that happens to land on 1x instead of
    // a manufactured factor. Shown so that's visible at a glance instead of
    // guessed at.
    GC_RuntimeInfo crt_lottes_runtime;
    emu_get_runtime(crt_lottes_runtime);
    if (crt_lottes_runtime.screen_height > 0)
    {
        const float verticalScale = static_cast<float>(main_window_height) / static_cast<float>(crt_lottes_runtime.screen_height);
        ImGui::Text("Output: %dx%d (%.2fx native height)", main_window_width, main_window_height, verticalScale);
        if (verticalScale < 1.5f)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                "Too close to 1x - scanlines need real vertical headroom to show. "
                "Try a bigger window or a higher Scale setting.");
        // Measured, not assumed: at exactly 2x there are only 2 output rows
        // per source row, and their phases always land at 0.25/0.75 - with
        // scanline_thinness above ~1/3 (thin above ~0.667) the cosine
        // window's own clamp zeroes one of the two contributions entirely,
        // so every output row ends up at nearly the same total brightness
        // even though the shader is computing real, different values. Not a
        // bug - the model assumes far more than 2 samples per source row
        // (real monitors are usually 4-8x an emulated console's
        // resolution), 2x just falls outside where it was designed to work.
        else if (verticalScale < 2.5f && output.crt_lottes_scanline_thinness > 0.33f)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                "Near 2x with this Scanline Intensity, the banding can flatten to near-"
                "invisible even though the pass is running correctly (the cosine window's "
                "own clamp - only 2 samples per source row at 2x). Try Scale 3x+, or drop "
                "Scanline Intensity below ~0.30 at this size.");
    }

    const auto* mask = GetVideoCatalog().ShaderParameter("crt_lottes_mask_type");
    const int maskIndex = static_cast<int>(output.crt_lottes_mask_type + 0.5f);
    const auto maskName = [](int value) {
        static const char* names[] = {"No Mask", "Aperture Grille", "Aperture Grille (bright)", "Shadow Mask"};
        return value >= 0 && value < 4 ? names[value] : "Unknown";
    };
    const char* maskLabel = maskName(maskIndex);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##crt_lottes_mask", maskLabel)) {
        for (const auto& choice : mask->choices)
            if (ImGui::Selectable(maskName(choice.second), maskIndex == choice.second)) output.crt_lottes_mask_type = static_cast<float>(choice.second);
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(-1.0f);
    shader_slider("##crt_lottes_mask_intensity", &output.crt_lottes_mask_intensity, "crt_lottes_mask_intensity", "Mask Intensity = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    shader_slider("##crt_lottes_scanline", &output.crt_lottes_scanline_thinness, "crt_lottes_scanline_thinness", "Scanline Intensity = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    shader_slider("##crt_lottes_sharpness", &output.crt_lottes_scan_blur, "crt_lottes_scan_blur", "Sharpness = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    shader_slider("##crt_lottes_gamma", &output.crt_lottes_gamma, "crt_lottes_gamma", "CRT Gamma = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    shader_slider("##crt_lottes_black_threshold", &output.crt_lottes_black_threshold, "crt_lottes_black_threshold", "Black Threshold = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    shader_slider("##crt_lottes_boldness", &output.crt_lottes_boldness, "crt_lottes_boldness", "Boldness = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    shader_slider("##crt_lottes_edge_radius", &output.crt_lottes_edge_radius, "crt_lottes_edge_radius", "Edge Radius = %.2f");

    ImGui::End();
}

static void window_advanced_scaling_setup(void)
{
    if (!show_advanced_scaling_setup)
        return;

    config_VideoOutput& output = video_setup_profile
        ? *video_setup_profile : config_video_output_for_mode(config_debug.debug);

    // Same AlwaysAutoResize/SetNextItemWidth feedback loop as
    // window_crt_lottes_setup - fixed initial size avoids it.
    gui_set_next_window_size_em(18.9f, 9.4f);
    ImGui::Begin("Advanced Scaling Setup", &show_advanced_scaling_setup);

    if (output.postprocessing != POSTPROCESSING_ADVANCED_SCALING)
        ImGui::TextWrapped("Postprocessing is not set to Advanced Scaling - these settings are saved "
            "but won't be visible until you select it in this profile's Postprocessing menu.");

    ImGui::SetNextItemWidth(-1.0f);
    shader_slider("##advanced_scaling_sharpness", &output.advanced_scaling_sharpness, "advanced_scaling_sharpness", "Sharpness = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    shader_slider("##advanced_scaling_black_threshold", &output.advanced_scaling_black_threshold, "advanced_scaling_black_threshold", "Black Threshold = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    shader_slider("##advanced_scaling_boldness", &output.advanced_scaling_boldness, "advanced_scaling_boldness", "Boldness = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    shader_slider("##advanced_scaling_edge_radius", &output.advanced_scaling_edge_radius, "advanced_scaling_edge_radius", "Edge Radius = %.2f");

    ImGui::End();
}

static void show_info(void)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::Begin("ROM Info", &config_emulator.show_info, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize);

    static char info[512];
    emu_get_info(info);

    ImGui::PushFont(gui_default_font, gui_get_default_font_size());
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f,0.502f,0.957f,1.0f));
    ImGui::SetCursorPosX(5.0f);
    ImGui::Text("%s", info);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::End();
    ImGui::PopStyleVar();
}

static void show_fps(const ImVec2& content_origin)
{
    // Two rates, because they are two different things now and showing only
    // one hid which. MACHINE is how fast the emulated SC-3000 is running,
    // paced from its own clocks; DISPLAY is how often a picture is presented,
    // which follows the monitor. They are no longer required to match, and
    // seeing them disagree - 59.92 against 144.00 - is the scheduler working,
    // not a fault.
    const SchedulerDiagnostics pacing = scheduler_get_diagnostics();

    ImGui::PushFont(gui_default_font, gui_get_default_font_size());
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f,1.00f,0.0f,1.0f));
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::SetCursorPos(ImVec2(content_origin.x + style.FramePadding.x,
                              content_origin.y + style.FramePadding.y));
    // Four decimals on the machine line, two on the display: 59.9227 against
    // a round 60 is a 0.13% difference, and at two decimals both read 59.92.
    // The whole point of the line is that the first number reaches the second
    // and stays there, which cannot be seen without the digits to see it in.
    if (config_video.fps)
    {
        ImGui::Text("MACHINE: %.4f / %.4f fps\nDISPLAY: %.2f fps  (%.2f ms)",
                    pacing.measured_fps, pacing.effective_fps,
                    ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
    }

    // The same signal path the Frame Pacing menu shows, in one line: source,
    // destination, and which of the two stages between them are running.
    // Neither stage is visible in the two rates above - that is the whole
    // reason this line exists.
    if (config_video.frame_pacing_info)
    {
        const bool converting =
            config_video_pacing_aligned() && !pacing.display_locked;
        const double out = pacing.measured_refresh > 1.0 ? pacing.measured_refresh
                                                         : pacing.display_hz;
        const double bend = (pacing.clock_scale - 1.0) * 100.0;
        if (std::fabs(bend) > 0.001)
            ImGui::Text("TMS %.4f Hz (%+.2f%%)  =>  Monitor %.2f Hz",
                        pacing.effective_fps, bend, out);
        else
            ImGui::Text("TMS %.4f Hz (crystals)  =>  Monitor %.2f Hz",
                        pacing.effective_fps, out);
        ImGui::SameLine();
        if (converting)
            ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.40f, 1.0f), "  Converter");
        else
            ImGui::TextDisabled("  Converter");
        ImGui::SameLine();
        if (config_video_vsync())
            ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.40f, 1.0f), "VSync");
        else
            ImGui::TextDisabled("VSync");
    }

    // The scheduler's own state, only where a debug session can see it. If
    // the measured rate is tracking DISPLAY rather than the target, these
    // three say which part is not doing its job: debt should hover under one
    // frame, drift stays near zero once the buffer settles, and frames is 1
    // most iterations with an occasional 0 - a steady 1 means nothing is
    // being held back.
    if (config_debug.debug && config_video.frame_pacing_info)
    {
        const EmuAudioQueueDiagnostics q = emu_get_audio_queue_diagnostics();
        ImGui::Text("PACING: debt %+.3f fr  drift %+.3f%%  frames %d  buf %d/%d",
                    pacing.debt_frames, pacing.audio_correction * 100.0,
                    pacing.frames_last_iteration,
                    (int)q.queued_samples, (int)q.ring_capacity_samples);

        // Only when it is not 1: a machine on its own crystals should not
        // have to say so, and one that has been scaled to the display must,
        // because switching vertical sync off does not undo it - the scale
        // is latched at reset, so until then the machine keeps running
        // aligned and nothing else on screen would reveal it.
        if (pacing.clock_scale < 0.9999 || pacing.clock_scale > 1.0001)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.15f, 1.0f),
                               "CLOCKS: x%.6f  (aligned to display; reset to "
                               "return to %.4f)",
                               pacing.clock_scale, emu_get_native_frame_rate());
        }
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

static void show_status_message(void)
{
    if (status_message_active)
    {
        u32 current_time = SDL_GetTicks();
        if ((current_time - status_message_start_time) > status_message_duration)
            status_message_active = false;
        else
            ImGui::OpenPopup("Status");
    }

    if (status_message_active)
    {
        ImGui::SetNextWindowPos(ImVec2(0.0f, config_emulator.show_menu ? main_menu_height : 0.0f));
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.9f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav;

        if (ImGui::BeginPopup("Status", flags))
        {
            ImGui::PushFont(gui_default_font, gui_get_default_font_size());
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.1f,0.9f,0.1f,1.0f));
            ImGui::TextWrapped("%s", status_message);
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::EndPopup();
        }

        ImGui::PopStyleVar();
    }
}

// The single place user configuration becomes what the core receives. Each
// caller used to build its own ForceConfiguration with type hardcoded to
// CartridgeNotSupported, so choosing a mapper and then pressing Reset went
// quietly back to auto-detection.
static Cartridge::ForceConfiguration get_force_config(void)
{
    Cartridge::ForceConfiguration config;
    config.type = config_forced_cartridge_type();
    config.fallbackType = config_fallback_cartridge_type();
    return config;
}

static void call_save_screenshot(const char* path)
{
    using namespace std;

    if (!emu_get_core()->IsMachineReady())
        return;

    time_t now = time(0);
    tm* ltm = localtime(&now);

    string date_time = to_string(1900 + ltm->tm_year) + "-" + to_string(1 + ltm->tm_mon) + "-" + to_string(ltm->tm_mday) + " " + to_string(ltm->tm_hour) + to_string(ltm->tm_min) + to_string(ltm->tm_sec);

    string file_path;

    if (path != NULL)
        file_path = path;
    else if ((emu_savestates_dir_option == 0) && (strcmp(emu_savestates_path, "")))
         file_path = file_path.assign(emu_savestates_path)+ "/" + string(emu_get_core()->GetCartridge()->GetFileName()) + " - " + date_time + ".png";
    else
         file_path = file_path.assign(emu_get_core()->GetCartridge()->GetFilePath()) + " - " + date_time + ".png";

    emu_save_screenshot(file_path.c_str());

    string message = "Screenshot saved to " + file_path;
    gui_set_status_message(message.c_str(), 3000);
}
