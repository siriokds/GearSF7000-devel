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
#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl3.h"
#include "../../src/build_info.h"
#include "emu.h"
#include "gui.h"
#include "gui_debug.h"
#include "gui_debug_rewind.h"
#include "Video.h"
#include "rewind.h"
#include "DiagnosticLog.h"
#include <typeinfo>
#include "config.h"
#include "renderer.h"
#include "scheduler.h"
#include "sk1100_keyboard_layout.h"
#include <filesystem>

#define APPLICATION_IMPORT
#include "application.h"

static SDL_Window* sdl_window;
static SDL_GPUDevice* gpu_device;
static bool running = true;
static bool paused_when_focus_lost = false;

static int sdl_init(void);
static void sdl_destroy(void);
static void sdl_events(void);
static void sdl_events_emu(const SDL_Event* event);
static bool sdl_shortcuts_gui(const SDL_Event* event);
static void keyboard_event_to_machine(SDL_Scancode scancode, bool pressed);
static void handle_mouse_cursor(void);
static void run_emulator(void);
static void render(void);
static void save_window_size(void);
static void push_recent_rom(const char* path);
static void push_recent_disc(const char* path);
static void push_recent_cassette(const char* path);
static Cartridge::CartridgeRegions application_cartridge_region(int index);

#ifdef __AVX2__
#include <iostream>

#ifdef _WIN32
#include <intrin.h> // Per _cpuid e _xgetbv

bool supportsAVX2() {
    int cpuInfo[4]; // Array per memorizzare i registri EAX, EBX, ECX, EDX

    // Chiamata CPUID con funzione 7 (extended features)
    __cpuidex(cpuInfo, 7, 0); // Chiamata CPUID con leaf 7, subleaf 0

    // Bit 5 di EBX indica il supporto AVX2
    if (cpuInfo[1] & (1 << 5)) { // EBX è cpuInfo[1]
        // Verifica che i registri di controllo della CPU supportino AVX (XGETBV)
        int xcr0 = _xgetbv(0); // Controlla il registro XCR0
        if ((xcr0 & 6) == 6) { // AVX richiede che i bit 1 (SSE) e 2 (AVX) siano impostati
            return true;
        }
    }
    return false; // AVX2 non supportato
}
#else
#include <cpuid.h>

bool supportsAVX2() {
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        return (ebx & (1 << 5)); // AVX2 è il bit 5 di EBX
    }
    return false;
}
#endif
#endif

int application_init(const char* rom_file, const char* symbol_file)
{
    // The drawable/framebuffer ratio is supplied by the SDL3 ImGui backend.
    // Keep a separate logical-content scale for the ImGui font system.
    application_content_scale = 1.0f;

    Log("\n%s", GEARSF7000_TITLE_ASCII);
    Log("%s %s Desktop App", GEARSF7000_TITLE, build_info_version());
    // First line of every log: which binary is actually running.
    Log("Compiled: %s", build_info_timestamp());
    Log("By Saverio Russo");

#ifdef __AVX2__
    if (!supportsAVX2())
    {
        Log("This CPU doesn't support AVX2 extensions!"); 
        return -1000000;
    }
#endif

    config_init();
    config_read();

    // Opened as early as the configuration allows, because the interesting
    // failures are the ones that happen before anyone is watching. It lives
    // next to config.ini - SDL's preferences directory, or the application's
    // own folder in portable mode - so it travels with the settings.
    if (DiagnosticLogOpen(config_root_path, "gearsf7000.log"))
    {
        DiagnosticLogSetLevel(static_cast<DiagLevel>(config_debug.log_level));
        DiagnosticLogSetMaxBytes(
            static_cast<std::size_t>(config_debug.log_max_mb) * 1024u * 1024u);
        DiagInfo("app", "GearSF7000 %s starting", build_info_full());
        DiagInfo("app", "log at %s", DiagnosticLogPath());
    }

    int ret = sdl_init();
    emu_init();

    strcpy(emu_savefiles_path, config_emulator.savefiles_path.c_str());
    strcpy(emu_savestates_path, config_emulator.savestates_path.c_str());
    emu_savefiles_dir_option = config_emulator.savefiles_dir_option;
    emu_savestates_dir_option = config_emulator.savestates_dir_option;
    
    gui_init();
    // After emu_init and gui_init: it reads the machine frame rate and the
    // window's display, and neither exists before those.
    scheduler_init();

    ImGui_ImplSDL3_InitForSDLGPU(sdl_window);

    if (!renderer_init(gpu_device, sdl_window))
        return 1;

    if (config_emulator.fullscreen)
        application_trigger_fullscreen(true);

    if (IsValidPointer(rom_file) && (strlen(rom_file) > 0))
    {
        Log ("Rom file argument: %s", rom_file);
        gui_load_rom(rom_file);
    }
    if (IsValidPointer(symbol_file) && (strlen(symbol_file) > 0))
    {
        Log ("Symbol file argument: %s", symbol_file);
        gui_debug_reset_symbols();
        gui_debug_load_symbols_file(symbol_file);
    }

    return ret;
}

void application_destroy(void)
{
    DiagInfo("app", "GearSF7000 shutting down");
    save_window_size();
    config_write();
    config_destroy();
    renderer_destroy();
    ImGui_ImplSDL3_Shutdown();
    gui_destroy();
    emu_destroy();
    sdl_destroy();
}

// Everything worth knowing about where the machine was when something went
// wrong, in one place so a fatal path does not have to remember to ask.
static void application_log_machine_context(void)
{
    Log("  machine generation : %llu",
        (unsigned long long)emu_get_machine_generation());
    Log("  rom                : %s",
        emu_is_empty() ? "(none)"
                       : emu_get_core()->GetCartridge()->GetFileName());
    Log("  paused             : core=%d debugger=%d",
        emu_is_paused() ? 1 : 0, emu_is_debugging() ? 1 : 0);
    Log("  debug step frames  : %d pending", emu_debug_frames_pending());

    if (!emu_is_empty())
    {
        const CpuStateSnapshot cpu =
            emu_get_core()->GetCpuStateAccess()->GetCpuStateSnapshot();
        Log("  pc                 : %04X", cpu.pc);
        if (Video* video = emu_get_core()->GetVideo())
            Log("  frame serial       : %llu",
                (unsigned long long)
                    video->GetFrameRenderDiagnostics().frameSerial);
    }

    Log("  recorder           : %s, %d frames held",
        rewind_is_enabled() ? "on" : "off", rewind_get_snapshot_count());
}

void application_mainloop(void)
{
    // Last resort. An exception escaping this loop reaches std::terminate,
    // which aborts without unwinding: the process vanishes and leaves nothing
    // to read. Catching it here does not make the emulator survive - it still
    // stops, because carrying on from an unknown failure would be worse - but
    // it stops having said what happened and where.
    //
    // Tools have their own guard in McpManager::PumpCommands, closer to the
    // cause; this one covers everything else.
    try
    {
        // Three separate jobs, deliberately not one. The host and its tools
        // are serviced every iteration; how many frames the machine owes is
        // decided by the wall clock against the machine's own frame rate;
        // presentation happens once, whether or not a frame was produced.
        // Nothing here derives the emulation rate from the display's.
        while (running)
        {
            sdl_events();
            handle_mouse_cursor();
            emu_pump_host();

            scheduler_begin_iteration();
            run_emulator();

            render();
            scheduler_wait_for_present_slot();
        }
    }
    catch (const std::exception& exception)
    {
        Log("FATAL: main loop aborted by %s: %s", typeid(exception).name(),
            exception.what());
        application_log_machine_context();
        throw;
    }
    catch (...)
    {
        Log("FATAL: main loop aborted by a non-standard exception");
        application_log_machine_context();
        throw;
    }
}

void application_trigger_quit(void)
{
    SDL_Event event;
    event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&event);
}

double application_get_display_refresh_hz(void)
{
    if (!sdl_window)
        return 0.0;

    const SDL_DisplayID display = SDL_GetDisplayForWindow(sdl_window);
    if (display == 0)
        return 0.0;

    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
    if (!mode || mode->refresh_rate <= 0.0f)
        return 0.0;

    return static_cast<double>(mode->refresh_rate);
}

void application_trigger_fullscreen(bool fullscreen)
{
    SDL_SetWindowFullscreen(sdl_window, fullscreen);
}

void application_trigger_fit_to_content(int width, int height)
{
    SDL_SetWindowSize(sdl_window, width, height);
}

void application_apply_startup_media(void)
{
    if (!config_emulator.disc_path.empty())
    {
        DiagInfo("media", "mounting configured disc: %s",
                 config_emulator.disc_path.c_str());
        application_mount_disk(config_emulator.disc_path.c_str(),
                               config_emulator.disc_write_protected);
    }

    if (!config_emulator.cassette_path.empty())
    {
        DiagInfo("media", "loading configured cassette: %s",
                 config_emulator.cassette_path.c_str());
        application_load_tape(config_emulator.cassette_path.c_str());
    }
}

bool application_load_rom(const char* path)
{
    if (!path || !path[0])
        return false;

    std::error_code pathError;
    if (!std::filesystem::is_regular_file(std::filesystem::path(path), pathError))
    {
        Log("application_load_rom invalid path: %s", path);
        return false;
    }

    Log("application_load_rom %s", path);

    Cartridge::ForceConfiguration config;
    config.region = application_cartridge_region(config_emulator.region);
    config.type = config_forced_cartridge_type();
    config.fallbackType = config_fallback_cartridge_type();

    // Keep this sequence identical to the historic GUI command: the action
    // moved here only so every front-end reaches the same code path.
    const bool wasPaused = emu_is_paused();
    emu_resume();
    if (!emu_load_rom(path, config, config_emulator.start_paused))
    {
        Log("application_load_rom failed: %s", path);
        if (wasPaused)
            emu_pause();
        return false;
    }
    push_recent_rom(path);
    gui_debug_reset();

    std::string symbolPath(path);
    symbolPath = symbolPath.substr(0, symbolPath.find_last_of("."));
    symbolPath += ".sym";
    gui_debug_load_symbols_file(symbolPath.c_str());

    if (config_emulator.start_paused)
        emu_clear_video_buffer();

    return !emu_is_empty();
}

bool application_load_tape(const char* path)
{
    if (!path || !path[0])
        return false;

    std::error_code pathError;
    if (!std::filesystem::is_regular_file(std::filesystem::path(path), pathError))
    {
        Log("application_load_tape invalid path: %s", path);
        return false;
    }

    bool readOnly = true;
    if (!emu_load_cassette(path, &readOnly))
    {
        Log("application_load_tape failed: %s", path);
        return false;
    }

    config_emulator.cassette_path = path;
    config_emulator.cassette_write_protected = readOnly;
    emu_writeprotect_cassette(readOnly);
    push_recent_cassette(path);
    config_write();
    return true;
}

bool application_mount_disk(const char* path, bool write_protected)
{
    if (!path || !path[0])
        return false;

    std::error_code pathError;
    if (!std::filesystem::is_regular_file(std::filesystem::path(path), pathError))
    {
        Log("application_mount_disk invalid path: %s", path);
        return false;
    }

    // The disk layer may detect an intrinsically read-only image.  Preserve
    // that constraint while still honouring an explicit user write-protect.
    bool detectedReadOnly = false;
    if (!emu_load_disc(path, &detectedReadOnly))
    {
        Log("application_mount_disk failed: %s", path);
        return false;
    }

    const bool effectiveWriteProtect = write_protected || detectedReadOnly;
    config_emulator.disc_path = path;
    config_emulator.disc_write_protected = effectiveWriteProtect;
    emu_writeprotect_disc(effectiveWriteProtect);
    push_recent_disc(path);
    config_write();
    return true;
}

bool application_start_sf7000(void)
{
    if (!emu_is_bios_loaded())
    {
        if (config_emulator.bios_path.empty())
        {
            Log("application_start_sf7000: no IPL configured");
            return false;
        }

        std::error_code pathError;
        if (!std::filesystem::is_regular_file(std::filesystem::path(config_emulator.bios_path), pathError))
        {
            Log("application_start_sf7000 invalid IPL path: %s", config_emulator.bios_path.c_str());
            return false;
        }

        emu_load_bios(config_emulator.bios_path.c_str());
        if (!emu_is_bios_loaded())
        {
            Log("application_start_sf7000 failed to load IPL: %s", config_emulator.bios_path.c_str());
            return false;
        }
    }

    emu_resume();
    emu_start_sf7000(config_emulator.start_paused);

    gui_debug_reset();
    if (config_emulator.start_paused)
        emu_clear_video_buffer();
    return emu_is_bios_loaded();
}

static void push_recent_rom(const char* path)
{
    const std::string value(path);
    int slot = 0;
    for (; slot < config_max_recent_roms; ++slot)
        if (config_emulator.recent_roms[slot] == value)
            break;

    slot = std::min(slot, config_max_recent_roms - 1);
    for (int i = slot; i > 0; --i)
        config_emulator.recent_roms[i] = config_emulator.recent_roms[i - 1];
    config_emulator.recent_roms[0] = value;
}

static void push_recent_disc(const char* path)
{
    const std::string value(path);
    int slot = 0;
    for (; slot < config_max_recent_discs; ++slot)
        if (config_emulator.recent_discs[slot] == value)
            break;
    slot = std::min(slot, config_max_recent_discs - 1);
    for (int i = slot; i > 0; --i)
        config_emulator.recent_discs[i] = config_emulator.recent_discs[i - 1];
    config_emulator.recent_discs[0] = value;
}

static void push_recent_cassette(const char* path)
{
    const std::string value(path);
    int slot = 0;
    for (; slot < config_max_recent_cassettes; ++slot)
        if (config_emulator.recent_cassettes[slot] == value)
            break;
    slot = std::min(slot, config_max_recent_cassettes - 1);
    for (int i = slot; i > 0; --i)
        config_emulator.recent_cassettes[i] = config_emulator.recent_cassettes[i - 1];
    config_emulator.recent_cassettes[0] = value;
}

static Cartridge::CartridgeRegions application_cartridge_region(int index)
{
    switch (index)
    {
    case 1: return Cartridge::CartridgeNTSC;
    case 2: return Cartridge::CartridgePAL;
    default: return Cartridge::CartridgeUnknownRegion;
    }
}

// SDL_WINDOWPOS_CENTERED centers the client area and ignores the window frame,
// so a window nearly as tall as the display ends up with its title bar above the
// top edge. Center the whole frame instead, and never let it start off-screen.
static void center_window(SDL_Window* window)
{
    SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    if (display == 0)
        display = SDL_GetPrimaryDisplay();

    SDL_Rect usable;
    if (!SDL_GetDisplayUsableBounds(display, &usable))
        return;

    int width, height;
    SDL_GetWindowSize(window, &width, &height);

    int top = 0, left = 0, bottom = 0, right = 0;
    SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right);

    // SDL window coordinates address the client area, so offset by the border.
    int x = usable.x + (usable.w - (width + left + right)) / 2 + left;
    int y = usable.y + (usable.h - (height + top + bottom)) / 2 + top;

    x = std::max(x, usable.x + left);
    y = std::max(y, usable.y + top);

    SDL_SetWindowPosition(window, x, y);
}

static int sdl_init(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        Log("Error: %s\n", SDL_GetError());
        return 1;
    }

    application_sdl_version = SDL_VERSION;

    const SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    char window_title[128];
    snprintf(window_title, sizeof(window_title), "%s %s",
             GEARSF7000_TITLE, build_info_version());
    sdl_window = SDL_CreateWindow(window_title,
                                  config_emulator.window_width, config_emulator.window_height, window_flags);
    if (!sdl_window)
    {
        Log("SDL window creation failed: %s\n", SDL_GetError());
        return 1;
    }
    // SDL3 dropped the x/y arguments from SDL_CreateWindow and defaults to
    // SDL_WINDOWPOS_UNDEFINED, so place the window explicitly.
    center_window(sdl_window);

    // Every pass now ships bytecode for each of these, so SDL is free to pick
    // whichever driver it considers optimal - see shaders/crt_pass/README.md.
    // The debug_mode argument is deliberately false: it turns on validation
    // layers, which is not what vsync (set through the swapchain present mode
    // in renderer.cpp) should ever have been controlling.
    gpu_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL |
                                     SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB,
                                     false, nullptr);
    if (!gpu_device || !SDL_ClaimWindowForGPUDevice(gpu_device, sdl_window))
    {
        Log("SDL_GPU initialization failed: %s\n", SDL_GetError());
        if (gpu_device) SDL_DestroyGPUDevice(gpu_device);
        SDL_DestroyWindow(sdl_window);
        sdl_window = nullptr;
        return 1;
    }
    SDL_SetWindowMinimumSize(sdl_window, 500, 300);

    application_gamepad_mappings = SDL_AddGamepadMappingsFromFile("gamecontrollerdb.txt");

    if (application_gamepad_mappings > 0)
    {
        Log("Succesfuly loaded %d game controller mappings", application_gamepad_mappings);
    }
    else
    {
        Log("Game controller database not found!");
    }


    int gamepads_found = 0;
    int gamepad_count = 0;
    SDL_JoystickID* gamepads = SDL_GetGamepads(&gamepad_count);
    for (int i = 0; gamepads && i < gamepad_count && gamepads_found < 2; ++i)
    {
        application_gamepad[gamepads_found] = SDL_OpenGamepad(gamepads[i]);
        if (!application_gamepad[gamepads_found])
            Log("Warning: Unable to open gamepad %d: %s\n", i, SDL_GetError());
        else
            ++gamepads_found;
    }
    SDL_free(gamepads);

    SDL_DisplayID display = SDL_GetDisplayForWindow(sdl_window);
    if (display == 0)
        display = SDL_GetPrimaryDisplay();
    application_content_scale = display != 0
        ? SDL_GetDisplayContentScale(display) : 1.0f;
    if (application_content_scale <= 0.0f)
        application_content_scale = 1.0f;

    return 0;
}

static void sdl_destroy(void)
{
    if (application_gamepad[0]) SDL_CloseGamepad(application_gamepad[0]);
    if (application_gamepad[1]) SDL_CloseGamepad(application_gamepad[1]);
    if (gpu_device && sdl_window) SDL_ReleaseWindowFromGPUDevice(gpu_device, sdl_window);
    if (gpu_device) SDL_DestroyGPUDevice(gpu_device);
    SDL_DestroyWindow(sdl_window);
    SDL_Quit();
}

static void handle_mouse_cursor(void)
{
    bool hide_cursor = false;

    if (gui_main_window_hovered && !config_debug.debug)
        hide_cursor = true;

    if (!config_emulator.show_menu && !config_debug.debug)
        hide_cursor = true;

    if (hide_cursor)
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    else
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);

    SDL_SetWindowRelativeMouseMode(sdl_window, config_emulator.capture_mouse);
}

static void sdl_events(void)
{
    SDL_Event event;
        
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_EVENT_QUIT)
        {
            running = false;
            break;
        }

        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(sdl_window))
        {
            running = false;
            break;
        }

        ImGui_ImplSDL3_ProcessEvent(&event);

        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
            gui_sk1100_release_virtual_keys();

        // Binding capture sees raw SDL events even while the modal owns the
        // GUI. A consumed key must never leak into BASIC or a debugger
        // shortcut while it is being assigned.
        if (gui_input_capture_event(&event))
            continue;

        if (!gui_in_use)
        {
            // Frontend hotkeys own an exact key+modifier chord. Dispatch
            // them before machine input so Ctrl+D cannot also type into
            // BASIC and a debugger F-key cannot reach the SK-1100 matrix.
            if (sdl_shortcuts_gui(&event))
                continue;
            sdl_events_emu(&event);
        }
    }
}

static void sdl_events_emu(const SDL_Event* event)
{
    switch(event->type)
    {
        case SDL_EVENT_DROP_FILE:
        {
            gui_load_rom(event->drop.data);
        }
        break;

        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        {
            switch (event->type)
            {
                case SDL_EVENT_WINDOW_FOCUS_GAINED:
                {
                    //if (!paused_when_focus_lost)
                    //    emu_resume();
                }
                break;

                case SDL_EVENT_WINDOW_FOCUS_LOST:
                {
                    //paused_when_focus_lost = emu_is_paused();
                    //emu_pause();
                }
                break;
            }
        }
        break;

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        {
            for (int i = 0; i < 2; i++)
            {
                GC_Controllers controller = (i == 0) ? Controller_1 : Controller_2;
                if (!application_gamepad[i])
                    continue;
                SDL_JoystickID id = SDL_GetGamepadID(application_gamepad[i]);

                if (!config_input[i].gamepad)
                    continue;

                if (event->gbutton.which != id)
                    continue;

                if (event->gbutton.button == config_input[i].gamepad_left_button)
                    emu_joy_pressed(controller, Key_Left_Button);
                else if (event->gbutton.button == config_input[i].gamepad_right_button)
                    emu_joy_pressed(controller, Key_Right_Button);

                if (config_input[i].gamepad_directional == 1)
                    continue;
                
                if (event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP)
                    emu_joy_pressed(controller, Key_Up);
                else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN)
                    emu_joy_pressed(controller, Key_Down);
                else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT)
                    emu_joy_pressed(controller, Key_Left);
                else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT)
                    emu_joy_pressed(controller, Key_Right);
            }
        }
        break;

        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        {
            for (int i = 0; i < 2; i++)
            {
                GC_Controllers controller = (i == 0) ? Controller_1 : Controller_2;
                if (!application_gamepad[i])
                    continue;
                SDL_JoystickID id = SDL_GetGamepadID(application_gamepad[i]);

                if (!config_input[i].gamepad)
                    continue;

                if (event->gbutton.which != id)
                    continue;

                if (event->gbutton.button == config_input[i].gamepad_left_button)
                    emu_joy_released(controller, Key_Left_Button);
                else if (event->gbutton.button == config_input[i].gamepad_right_button)
                    emu_joy_released(controller, Key_Right_Button);

                if (config_input[i].gamepad_directional == 1)
                    continue;
                
                if (event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP)
                    emu_joy_released(controller, Key_Up);
                else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN)
                    emu_joy_released(controller, Key_Down);
                else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT)
                    emu_joy_released(controller, Key_Left);
                else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT)
                    emu_joy_released(controller, Key_Right);
            }
        }
        break;

        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        {
            for (int i = 0; i < 2; i++)
            {
                GC_Controllers controller = (i == 0) ? Controller_1 : Controller_2;
                if (!application_gamepad[i])
                    continue;
                SDL_JoystickID id = SDL_GetGamepadID(application_gamepad[i]);

                if (!config_input[i].gamepad)
                    continue;

                if (config_input[i].gamepad_directional == 0)
                    continue;

                if (event->gaxis.which != id)
                    continue;

                const int STICK_DEAD_ZONE = 8000;
                    
                if (event->gaxis.axis == config_input[i].gamepad_x_axis)
                {
                    int x_motion = event->gaxis.value * (config_input[i].gamepad_invert_x_axis ? -1 : 1);

                    if (x_motion < -STICK_DEAD_ZONE)
                    {
                        emu_joy_pressed(controller, Key_Left);
                        emu_joy_released(controller, Key_Right);
                    }
                    else if (x_motion > STICK_DEAD_ZONE)
                    {
                        emu_joy_pressed(controller, Key_Right);
                        emu_joy_released(controller, Key_Left);
                    }
                    else
                    {
                        emu_joy_released(controller, Key_Left);
                        emu_joy_released(controller, Key_Right);
                    }
                }
                else if(event->gaxis.axis == config_input[i].gamepad_y_axis)
                {
                    int y_motion = event->gaxis.value * (config_input[i].gamepad_invert_y_axis ? -1 : 1);

                    if (y_motion < -STICK_DEAD_ZONE)
                    {
                        emu_joy_pressed(controller, Key_Up);
                        emu_joy_released(controller, Key_Down);
                    }
                    else if (y_motion > STICK_DEAD_ZONE)
                    {
                        emu_joy_pressed(controller, Key_Down);
                        emu_joy_released(controller, Key_Up);
                    }
                    else
                    {
                        emu_joy_released(controller, Key_Up);
                        emu_joy_released(controller, Key_Down);
                    }
                }
            }
        }
        break;

        case SDL_EVENT_KEY_DOWN:
        {
            if (event->key.repeat != 0)
                break;

            //if (event->key.keysym.mod & KMOD_CTRL)
            //    break;

            int key = event->key.scancode;


            //if (key == SDL_SCANCODE_ESCAPE)
            //{
            //    //application_trigger_quit();
            //    break;
            //}

            // Fullscreen used to be F11. It moved to F12 when F11 became
            // step into, which is where Gearsystem and most debuggers put it.




            if (key == SDL_SCANCODE_F1 && !config_emulator.keyboard_mode)
            {
                emu_pause_key_pressed();
                //config_emulator.capture_mouse = !config_emulator.capture_mouse;
                break;
            }

            keyboard_event_to_machine((SDL_Scancode)key, true);

            //for (int i = 0; i < 2; i++)
            //{
            //    GC_Controllers controller = (i == 0) ? Controller_1 : Controller_2;

            //    if (key == config_input[i].key_left)
            //        emu_key_pressed(controller, Key_Left);
            //    else if (key == config_input[i].key_right)
            //        emu_key_pressed(controller, Key_Right);
            //    else if (key == config_input[i].key_up)
            //        emu_key_pressed(controller, Key_Up);
            //    else if (key == config_input[i].key_down)
            //        emu_key_pressed(controller, Key_Down);
            //    else if (key == config_input[i].key_left_button)
            //        emu_key_pressed(controller, Key_Left_Button);
            //    else if (key == config_input[i].key_right_button)
            //        emu_key_pressed(controller, Key_Right_Button);
            //    else if (key == config_input[i].key_blue)
            //        emu_key_pressed(controller, Key_Blue);
            //    else if (key == config_input[i].key_purple)
            //        emu_key_pressed(controller, Key_Purple);
            //    else if (key == config_input[i].key_0)
            //        emu_key_pressed(controller, Keypad_0);
            //    else if (key == config_input[i].key_1)
            //        emu_key_pressed(controller, Keypad_1);
            //    else if (key == config_input[i].key_2)
            //        emu_key_pressed(controller, Keypad_2);
            //    else if (key == config_input[i].key_3)
            //        emu_key_pressed(controller, Keypad_3);
            //    else if (key == config_input[i].key_4)
            //        emu_key_pressed(controller, Keypad_4);
            //    else if (key == config_input[i].key_5)
            //        emu_key_pressed(controller, Keypad_5);
            //    else if (key == config_input[i].key_6)
            //        emu_key_pressed(controller, Keypad_6);
            //    else if (key == config_input[i].key_7)
            //        emu_key_pressed(controller, Keypad_7);
            //    else if (key == config_input[i].key_8)
            //        emu_key_pressed(controller, Keypad_8);
            //    else if (key == config_input[i].key_9)
            //        emu_key_pressed(controller, Keypad_9);
            //    else if (key == config_input[i].key_asterisk)
            //        emu_key_pressed(controller, Keypad_Asterisk);
            //    else if (key == config_input[i].key_hash)
            //        emu_key_pressed(controller, Keypad_Hash);
            //}
        }
        break;

        case SDL_EVENT_KEY_UP:
        {
            int key = event->key.scancode;

            keyboard_event_to_machine((SDL_Scancode)key, false);

            //for (int i = 0; i < 2; i++)
            //{
            //    GC_Controllers controller = (i == 0) ? Controller_1 : Controller_2;

            //    if (key == config_input[i].key_left)
            //        emu_key_released(controller, Key_Left);
            //    else if (key == config_input[i].key_right)
            //        emu_key_released(controller, Key_Right);
            //    else if (key == config_input[i].key_up)
            //        emu_key_released(controller, Key_Up);
            //    else if (key == config_input[i].key_down)
            //        emu_key_released(controller, Key_Down);
            //    else if (key == config_input[i].key_left_button)
            //        emu_key_released(controller, Key_Left_Button);
            //    else if (key == config_input[i].key_right_button)
            //        emu_key_released(controller, Key_Right_Button);
            //    else if (key == config_input[i].key_blue)
            //        emu_key_released(controller, Key_Blue);
            //    else if (key == config_input[i].key_purple)
            //        emu_key_released(controller, Key_Purple);
            //    else if (key == config_input[i].key_0)
            //        emu_key_released(controller, Keypad_0);
            //    else if (key == config_input[i].key_1)
            //        emu_key_released(controller, Keypad_1);
            //    else if (key == config_input[i].key_2)
            //        emu_key_released(controller, Keypad_2);
            //    else if (key == config_input[i].key_3)
            //        emu_key_released(controller, Keypad_3);
            //    else if (key == config_input[i].key_4)
            //        emu_key_released(controller, Keypad_4);
            //    else if (key == config_input[i].key_5)
            //        emu_key_released(controller, Keypad_5);
            //    else if (key == config_input[i].key_6)
            //        emu_key_released(controller, Keypad_6);
            //    else if (key == config_input[i].key_7)
            //        emu_key_released(controller, Keypad_7);
            //    else if (key == config_input[i].key_8)
            //        emu_key_released(controller, Keypad_8);
            //    else if (key == config_input[i].key_9)
            //        emu_key_released(controller, Keypad_9);
            //    else if (key == config_input[i].key_asterisk)
            //        emu_key_released(controller, Keypad_Asterisk);
            //    else if (key == config_input[i].key_hash)
            //        emu_key_released(controller, Keypad_Hash);
            //}
        }
        break;
    }
}

static void keyboard_event_to_machine(SDL_Scancode scancode, bool pressed)
{
    if (config_emulator.keyboard_mode)
    {
        for (int key = 0; key < SK1100_KEYBOARD_KEY_COUNT; ++key)
        {
            const SK1100KeyboardKeyLayout& layout = sk1100_keyboard_layout[key];
            if (config_input[0].sk1100_key[key] != scancode)
                continue;
            if (layout.row >= 0)
            {
                // Physical and on-screen keyboards are independent sources.
                // Releasing either one must not release a matrix contact that
                // is still held by the other.
                if (pressed || !gui_sk1100_virtual_matrix_down(layout.row, layout.mask))
                    emu_keyboard_matrix_key(layout.row, layout.mask, pressed);
            }
            else if (pressed)
                // RESET is not a matrix contact. On the SC-3000 it is the
                // keyboard unit's dedicated NMI source, so use the existing
                // hardware path instead of resetting the emulator itself.
                emu_pause_key_pressed();
        }
        return;
    }

    // In joystick mode the keyboard profiles address the same six active-low
    // bits as a physical controller. These settings existed in the menu for
    // years, but their event path was commented out and therefore inert.
    for (int player = 0; player < 2; ++player)
    {
        const GC_Controllers controller = player == 0 ? Controller_1 : Controller_2;
        const config_Input& input = config_input[player];
        const GC_Keys actions[6] = {
            Key_Left, Key_Right, Key_Up, Key_Down,
            Key_Left_Button, Key_Right_Button
        };
        const SDL_Scancode bindings[6] = {
            input.key_left, input.key_right, input.key_up, input.key_down,
            input.key_left_button, input.key_right_button
        };
        for (int action = 0; action < 6; ++action)
        {
            if (bindings[action] != scancode)
                continue;
            if (pressed)
                emu_joy_pressed(controller, actions[action]);
            else
                emu_joy_released(controller, actions[action]);
        }
    }
}

static bool sdl_shortcuts_gui(const SDL_Event* event)
{
    if (event->type != SDL_EVENT_KEY_DOWN || event->key.repeat != 0)
        return false;

    SDL_Keymod mods = SDL_KMOD_NONE;
    const SDL_Keymod raw = event->key.mod;
    if (raw & (SDL_KMOD_LCTRL | SDL_KMOD_RCTRL)) mods = (SDL_Keymod)(mods | SDL_KMOD_CTRL);
    if (raw & (SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT)) mods = (SDL_Keymod)(mods | SDL_KMOD_SHIFT);
    if (raw & (SDL_KMOD_LALT | SDL_KMOD_RALT)) mods = (SDL_Keymod)(mods | SDL_KMOD_ALT);
    if (raw & (SDL_KMOD_LGUI | SDL_KMOD_RGUI)) mods = (SDL_Keymod)(mods | SDL_KMOD_GUI);

    int match = -1;
    for (int i = 0; i < config_HotkeyIndex_COUNT; ++i)
    {
        if (config_hotkeys[i].key == event->key.scancode && config_hotkeys[i].mod == mods)
        {
            match = i;
            break;
        }
    }
    if (match < 0)
        return false;

    switch ((config_HotkeyIndex)match)
    {
        case config_HotkeyIndex_OpenROM: gui_shortcut(gui_ShortcutOpenROM); break;
        case config_HotkeyIndex_ReloadROM: gui_shortcut(gui_ShortcutReloadROM); break;
        case config_HotkeyIndex_Reset: gui_shortcut(gui_ShortcutReset); break;
        case config_HotkeyIndex_Pause: gui_shortcut(gui_ShortcutPause); break;
        case config_HotkeyIndex_FFWD: gui_shortcut(gui_ShortcutFFWD); break;
        case config_HotkeyIndex_SaveState: gui_shortcut(gui_ShortcutSaveState); break;
        case config_HotkeyIndex_LoadState: gui_shortcut(gui_ShortcutLoadState); break;
        case config_HotkeyIndex_Screenshot: gui_shortcut(gui_ShortcutScreenshot); break;
        case config_HotkeyIndex_Fullscreen: gui_shortcut(gui_ShortcutFullscreen); break;
        case config_HotkeyIndex_ShowMainMenu: gui_shortcut(gui_ShortcutShowMainMenu); break;
        case config_HotkeyIndex_DebugStepInto: gui_shortcut(gui_ShortcutDebugStepInto); break;
        case config_HotkeyIndex_DebugStepOver: gui_shortcut(gui_ShortcutDebugStepOver); break;
        case config_HotkeyIndex_DebugStepLine: gui_shortcut(gui_ShortcutDebugStepLine); break;
        case config_HotkeyIndex_DebugStepFrame:
            if (!gui_debug_rewind_step_forward())
                gui_shortcut(gui_ShortcutDebugNextFrame);
            break;
        case config_HotkeyIndex_DebugStepBack: gui_shortcut(gui_ShortcutDebugPreviousFrame); break;
        case config_HotkeyIndex_DebugContinue: gui_shortcut(gui_ShortcutDebugContinue); break;
        case config_HotkeyIndex_DebugContinueFromHere:
            gui_shortcut(gui_ShortcutDebugContinueFromHere);
            break;
        case config_HotkeyIndex_DebugBreak: gui_shortcut(gui_ShortcutDebugBreak); break;
        case config_HotkeyIndex_DebugRunToCursor: gui_shortcut(gui_ShortcutDebugRuntocursor); break;
        case config_HotkeyIndex_DebugBreakpoint: gui_shortcut(gui_ShortcutDebugBreakpoint); break;
        case config_HotkeyIndex_DebugGoBack: gui_shortcut(gui_ShortcutDebugGoBack); break;
        case config_HotkeyIndex_DebugCopy: gui_shortcut(gui_ShortcutDebugCopy); break;
        case config_HotkeyIndex_DebugPaste: gui_shortcut(gui_ShortcutDebugPaste); break;
        default: return false;
    }
    return true;
}

// Ceiling on one iteration's wall-clock time during unlimited fast forward.
// It is what keeps the interface alive: however fast the host is, a single
// presentation never spends longer than this running frames. The frame count
// itself is the scheduler's business now - fast forward there is a shorter
// frame period, not a second counter kept here.
static constexpr double kFfwdBudgetMs = 12.0;

static void run_emulator(void)
{
    if (!emu_is_empty())
    {
        static int i = 0;
        i++;

        if (i > 20)
        {
            i = 0;

            char title[256];
            snprintf(title, sizeof(title), "%s %s - %s", GEARSF7000_TITLE, build_info_version(), emu_get_core()->GetCartridge()->GetFileName());
            SDL_SetWindowTitle(sdl_window, title);
        }
    }
    config_emulator.paused = emu_is_execution_stopped();

    // The audio queue must not block the emulation any more. It used to: a
    // full ring made Write() wait, and that wait was one of the two things
    // actually setting the machine's speed - the other being vertical sync.
    // Neither was the machine's own clock. The scheduler is the pacer now,
    // and it keeps the buffer near half full by nudging the frame period
    // instead, so the audio device's drift is absorbed without the device
    // dictating when a frame runs.
    //
    // The setting that used to control it is gone with the mechanism: there
    // is nothing left for "sync the audio to the emulator" to mean once the
    // emulator is not paced by the audio.
    emu_audio_sync = false;

    // How many frames are due is the scheduler's decision, taken from wall
    // time against the machine's frame rate. Fast forward is a shorter frame
    // period there, not a separate counter here.
    const Uint64 burst_start = SDL_GetPerformanceCounter();
    while (scheduler_take_frame())
    {
        emu_run_frame();

        // A breakpoint during a burst must not be run past: the rest of the
        // frames belong to a machine the user has just asked to look at.
        if (emu_is_paused() || emu_is_debugging())
        {
            scheduler_reset();
            break;
        }

        // Unlimited fast forward has no period to bound it, so the wall-clock
        // budget is what keeps one iteration from swallowing the whole loop
        // and freezing the interface. Measured between frames, not predicted
        // before them: how long a frame takes depends on the host and the ROM.
        if (config_emulator.ffwd)
        {
            const double elapsed_ms =
                (double)(SDL_GetPerformanceCounter() - burst_start) * 1000.0 /
                SDL_GetPerformanceFrequency();
            if (elapsed_ms >= kFfwdBudgetMs)
                break;
        }
    }
}

static void render(void)
{
    renderer_begin_render();
    ImGui_ImplSDL3_NewFrame();
    bool mainWindowFocused = gui_render();
    emu_enable_events(mainWindowFocused);

    renderer_render();
    renderer_end_render();
}

static void save_window_size(void)
{
    if (!config_emulator.fullscreen)
    {
        int width, height;
        SDL_GetWindowSize(sdl_window, &width, &height);
        config_emulator.window_width = width;
        config_emulator.window_height = height;
    }
}
