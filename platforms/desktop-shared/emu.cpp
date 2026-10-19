#include "../../src/SystemBusMSX.h"
#include "../../src/CBiosData.h"
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

#include <cstring>
#include <set>
#include <string>
#include "../../src/gearsf7000.h"
#include "../../src/SegaKeyboard.h"
#if GEARSF7000_ENABLE_AY
// For SetAyPortBase(): the AY window is decoded in the I/O port layer, which
// gearsf7000.h does not pull in.
#include "../../src/MachineIOPorts.h"
#endif
#include "../audio-shared/sound_queue.h"
#include "config.h"
#include "VideoOutputSettings.h"
#include "application.h"
#include "renderer.h"
#include "crt_signal_dump.h"
#include "../../src/IVideoDevice.h"
#include "../../src/Z80Disassembler.h"
#include "../../src/BasicProgramLoader.h"

#include "mcp/mcp_config.h"
#if GEARSF7000_ENABLE_MCP
#include "mcp/mcp_manager.h"
#endif

#define EMU_IMPORT
#include "emu.h"

namespace
{
// See emu_resync_host_input(): defined with the host input list below.
void resync_if_stale(void);
}
#include "rewind.h"
#if GEARSF7000_ENABLE_DEBUG_TOOLS
#include "rom_map_project.h"
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#ifdef _WIN32
#define STBIW_WINDOWS_UTF8
#endif
#include "stb/stb_image_write.h"


static GearSF7000Core* gearsf7000;
static SoundQueue* sound_queue;
static s16* audio_buffer;
static bool audio_enabled;
static bool audio_suspended_for_file_dialog = false;
static bool audio_output_paused = false;
static bool debugging = false;
static bool debug_step = false;
static bool debug_line_pending = false;
static int debug_frames_pending = 0;
static unsigned long long machine_generation = 0;
static u8* blank_frame_buffer = nullptr;
static size_t blank_frame_buffer_capacity = 0;
static bool video_output_blanked = false;
#if GEARSF7000_ENABLE_MCP
static McpManager* mcp_manager = NULL;
#endif

static void save_ram(void);
static void load_ram(void);
static void init_debug(void);
static void destroy_debug(void);
static void update_audio_pause_state(void);
static void ensure_blank_frame_buffer_capacity(void);
static void apply_configured_video_device(void);
static void apply_configured_video_output(void);

void emu_init(bool enable_audio)
{
#if GEARSF7000_ENABLE_DEBUG_TOOLS
    init_debug();
#endif

    gearsf7000 = new GearSF7000Core();
    if (!gearsf7000->SetSystemModel(static_cast<SystemModel>(config_emulator.machine))) {
        Log("Configured machine is unavailable in this build; using %s", GetSystemModelName(gearsf7000->GetSystemModel()));
        config_select_machine(gearsf7000->GetSystemModel());
    }
    gearsf7000->SetM5EM5Installed(config_emulator.m5_em5_installed);
    apply_configured_video_device();
    gearsf7000->Init();
    rewind_init();
#if GEARSF7000_ENABLE_DEBUG_TOOLS
    rom_map_project_init();
#endif
    gearsf7000->SetKeyboardMode(
        gearsf7000->GetMachineType() == MachineType::SC3000 &&
        config_emulator.keyboard_mode);
#if GEARSF7000_ENABLE_AY
    // Stage the persisted expansion settings here, before anything resets the
    // machine. Doing it later - from gui_init(), which runs after emu_init() -
    // meant the first reset had already happened, so a card configured as
    // present stayed absent until the user reset by hand.
    emu_set_ay_config(config_audio.ay_enable, config_audio.ay_chip,
                      config_audio.ay_port_base);
#endif
#if GEARSF7000_ENABLE_YM2413
    emu_set_ym2413_config(config_audio.ym2413_enable);
#endif
    for (int player = 0; player < 2; ++player)
        emu_set_controller_device(player, config_input[player].controller_device);

#if GEARSF7000_ENABLE_MCP
    mcp_manager = new McpManager();
    mcp_manager->Init(gearsf7000);
#endif

    if (enable_audio)
    {
        sound_queue = new SoundQueue();
        sound_queue->Start(GC_AUDIO_SAMPLE_RATE, GC_AUDIO_BUFFER_NUM);
    }
    else
    {
        sound_queue = NULL;
        DiagInfo("app", "audio device not opened (headless)");
    }

    audio_buffer = new s16[GC_AUDIO_BUFFER_SIZE];

    for (int i = 0; i < GC_AUDIO_BUFFER_SIZE; i++)
        audio_buffer[i] = 0;

    audio_enabled = enable_audio;
    emu_audio_sync = true;
    emu_debug_disable_breakpoints_cpu = false;
    emu_debug_disable_breakpoints_mem = false;
    emu_debug_tile_palette = 0;
    emu_savefiles_dir_option = 0;
    emu_savestates_dir_option = 0;
    emu_savefiles_path[0] = 0;
    emu_savestates_path[0] = 0;
}

void emu_prepare_shutdown(void)
{
#if GEARSF7000_ENABLE_DEBUG_TOOLS
    // The project folder hangs off config_root_path, which config_destroy()
    // frees: saving from emu_destroy() used a dangling pointer and lost the
    // final save. Found on the first real play session, 23/09/2026.
    rom_map_project_shutdown();
#endif
}

void emu_destroy(void)
{
    save_ram();
#if GEARSF7000_ENABLE_MCP
    SafeDelete(mcp_manager);
#endif
    SafeDeleteArray(audio_buffer);
    SafeDelete(sound_queue);
    SafeDelete(gearsf7000);
    SafeDeleteArray(blank_frame_buffer);
    blank_frame_buffer_capacity = 0;
#if GEARSF7000_ENABLE_DEBUG_TOOLS
    destroy_debug();
#endif
}

// One BIOS slot in Memory, one file per board: the SF-7000 IPL or the
// ColecoVision console ROM, from that machine's own section of the
// settings. A board without a configured BIOS gets an empty slot, never the
// other board's file.
static std::string machine_start_error;
static std::string applied_msx_main;
static bool applied_msx_cbios = false;
const char* emu_get_cbios_license() { return CBios::License; }
bool emu_is_cbios_active()
{
    return gearsf7000->GetMachineType() == MachineType::MSX &&
        gearsf7000->GetMemory()->IsBiosLoaded() && applied_msx_cbios;
}
bool emu_has_pending_msx_firmware()
{
    return applied_msx_cbios != config_emulator.msx_use_cbios ||
        (!config_emulator.msx_use_cbios && applied_msx_main != config_emulator.msx_bios_path);
}
const std::string& emu_get_machine_start_error() { return machine_start_error; }
static void apply_machine_bios(void)
{
    machine_start_error.clear();
    const SystemModel model = gearsf7000->GetSystemModel();
    Memory* memory = gearsf7000->GetMemory();
    applied_msx_cbios = false;
    if (IsMSXSystem(model)) {
        auto& bus = *static_cast<SystemBusMSX*>(const_cast<ISystemBus*>(gearsf7000->GetSystemBus()));
        std::string error;
        if (config_emulator.msx_use_cbios) {
            const bool pal = model == SystemModel::MSX1PAL;
            memory->LoadBiosFromBuffer(pal ? CBios::MainPAL : CBios::MainNTSC,
                sizeof(CBios::MainNTSC), pal ? "cbios_main_msx1_eu.rom" : "cbios_main_msx1.rom");
            bus.LoadOptionalFirmwareFromBuffers(std::vector<u8>(std::begin(CBios::Logo), std::end(CBios::Logo)), {}, error);
            applied_msx_cbios = memory->IsBiosLoaded();
        } else {
            memory->LoadBios(config_emulator.msx_bios_path.c_str());
            bus.LoadOptionalFirmwareFromBuffers({}, {}, error);
            if (!memory->IsBiosLoaded()) machine_start_error =
                "Missing or invalid MSX BIOS. Choose a readable 32 KB BIOS + BASIC image, or enable Use C-BIOS.";
        }
        applied_msx_main = config_emulator.msx_bios_path;
    } else if (memory->GetBiosSize()) {
        const auto& path = IsM5System(model) ? config_emulator.m5_bios_path : IsColecoSystem(model) ? config_emulator.coleco_bios_path : config_emulator.bios_path;
        memory->LoadBios(path.c_str());
        if (!memory->IsBiosLoaded()) machine_start_error = IsColecoSystem(model)
            ? "Missing or invalid ColecoVision BIOS. Choose a readable 8 KB BIOS image."
            : IsM5System(model) ? "Missing or invalid Sord M5 BIOS. Choose a readable 8 KB BIOS image."
            : "Missing or invalid SF-7000 IPL. Choose a readable 8 KB IPL image.";
    }
    if (!machine_start_error.empty()) DiagWarn("app", "%s", machine_start_error.c_str());
}

void emu_reload_machine_firmware()
{
    // Replacing firmware invalidates execution and recorder history. Prepare
    // the reset vector paused; Start or F5 is the explicit execution action.
    Cartridge::ForceConfiguration config;
    config.type = gearsf7000->GetCartridge()->GetType();
    emu_reset(config, true);
    gearsf7000->Pause(true);
}

bool emu_load_rom(const char* file_path, Cartridge::ForceConfiguration config, bool pauseAtResetVector)
{
    save_ram();
    // A normal cartridge and the SF-7000 I/O cartridge occupy the same slot.
    // Apply the model currently selected in the UI, rather than retaining the
    // previously running motherboard until a separate Reset. Only the
    // SF-7000 attachment is removed when a cartridge is inserted.
    SystemModel requested = static_cast<SystemModel>(config_emulator.machine);
    if (!IsValidSystemModel(requested))
        requested = gearsf7000->GetSystemModel();
    const SystemModel cartridgeSystem = WithoutSF7000(requested);
    if (gearsf7000->GetMachineType() == MachineType::MSX || gearsf7000->GetMachineType() == MachineType::SordM5 || IsMSXSystem(cartridgeSystem) || IsM5System(cartridgeSystem))
        emu_clear_matrix_host_input();
    config_select_machine(cartridgeSystem);
    if (!gearsf7000->ApplySystemModelAtReset(cartridgeSystem)) {
        config_select_machine(gearsf7000->GetSystemModel());
        return false;
    }
    apply_configured_video_device();
    apply_machine_bios();
    bool loaded = false;

    if (file_path != 0)
    {
        Log("gearsf7000->LoadROM: %s\n", file_path);

        loaded = gearsf7000->LoadROM(file_path, &config, pauseAtResetVector);
    }
    else
    {
        Log("gearsf7000->LoadROMNull\n");
        loaded = gearsf7000->LoadROMNull(&config, pauseAtResetVector);
    }
    if (loaded)
    {
        apply_configured_video_output();
        ++machine_generation;
        // Recorded before anything else touches the new machine, and with the
        // debugger state as the *outgoing* run left it: a bulk step still
        // armed here is the thing to look for after an unexplained exit.
        DiagInfo("media",
                 "ROM loaded, machine generation %llu: %s", machine_generation,
                 file_path ? file_path : "(null cartridge)");
        DiagInfo("media",
                 "  debugger carried in: debugging=%d step=%d frames_pending=%d",
                 debugging ? 1 : 0, debug_step ? 1 : 0, debug_frames_pending);
        DiagInfo("media", "  recorder: %s, %d frames held",
                 rewind_is_enabled() ? "on" : "off",
                 rewind_get_snapshot_count());

        load_ram();
#if GEARSF7000_ENABLE_DEBUG_TOOLS
        rom_map_project_on_media_changed();
#endif
        // A recording belongs to the machine that produced it: snapshots of
        // the outgoing cartridge cannot be applied to this one, and the state
        // header would refuse them anyway.
        rewind_reset();

        // The debugger has to start from a known state on the new machine.
        // This used to happen only through emu_debug_continue() below, which
        // is skipped when the caller wants the machine paused - so with
        // start_paused on, a bulk step armed against the outgoing cartridge
        // stayed armed against this one, and the main loop kept feeding it
        // frames of a machine that had just been rebuilt underneath it.
        debugging = pauseAtResetVector;
        debug_step = false;
        debug_line_pending = false;
        debug_frames_pending = 0;

        // gearsf7000->Reset() already left the machine paused at the reset
        // vector when requested; emu_debug_continue() would resume it.
        if (!pauseAtResetVector)
            emu_debug_continue();
    }
    return loaded;
}

// Runs one frame without touching the debugger's run state. The recorder
// uses it so that a seek updates the device-owned presentation surface:
// updates the screen: a snapshot holds the machine, not an image, and the
// only faithful way to see a frame - raster splits included - is to let the
// beam draw it.
void emu_render_current_frame(void)
{
    if (emu_is_empty())
        return;

    ensure_blank_frame_buffer_capacity();

    // RunToVBlank does nothing while the core is paused, and a seek is always
    // made from a paused machine - so without lifting the pause here the
    // screen would keep showing whatever was drawn before the seek. The pause
    // is put back immediately: the machine advances by the one frame it takes
    // to paint the picture, and no further.
    const bool was_paused = gearsf7000->IsPaused();
    if (was_paused)
        gearsf7000->Pause(false);

    int sampleCount = 0;
    gearsf7000->RunToVBlank(audio_buffer, &sampleCount, false, false);
    video_output_blanked = false;

    if (was_paused)
        gearsf7000->Pause(true);
}

int emu_debug_frames_pending(void)
{
    return debug_frames_pending;
}

// True while a step the user asked for is armed and has not run yet. Arming a
// step also sets `debugging`, which is what the scheduler treats as "this
// machine is not running" - so the scheduler has to ask this before deciding
// there is no frame to give, or the step would wait for a frame that its own
// flag prevents.
bool emu_debug_step_pending(void)
{
    return debug_step || debug_line_pending || (debug_frames_pending > 0);
}

unsigned long long emu_get_machine_generation(void)
{
    return machine_generation;
}

// Fast forward is one thing: a shorter frame period in the scheduler.
//
// It used to be three, and had to be. Going faster means producing more
// emulated frames per unit of real time, and back then the loop ran exactly
// one frame per iteration with no brake to release - what actually held the
// machine at its own rate was presentation waiting on the display refresh,
// and the audio queue blocking when full. So fast forward had to reach in and
// switch both of those off, save what they had been, and put them back
// afterwards, which also needed a paragraph explaining how it reconciled with
// the recorder doing the same thing for its own reasons.
//
// None of that is needed now. The scheduler decides how many frames are due
// from wall time against the machine's frame rate, fast forward divides that
// period by the speed setting, and neither vertical sync nor the audio queue
// paces anything. A user's video and audio settings are theirs.
void emu_set_fast_forward(bool enabled)
{
    if (enabled == config_emulator.ffwd)
        return;

    config_emulator.ffwd = enabled;
    DiagInfo("speed", "fast forward %s (speed setting %d)",
             enabled ? "on" : "off", config_emulator.ffwd_speed);
}

bool emu_get_fast_forward(void)
{
    return config_emulator.ffwd;
}

void emu_set_fast_forward_speed(int speed)
{
    if (speed < 0) speed = 0;
    if (speed > 4) speed = 4;
    config_emulator.ffwd_speed = speed;
    DiagInfo("speed", "fast forward speed setting %d", speed);
}

int emu_get_fast_forward_speed(void)
{
    return config_emulator.ffwd_speed;
}

double emu_get_fast_forward_multiplier(void)
{
    // Kept here rather than in either loop, because both the windowed one and
    // the headless one need the same numbers and would otherwise drift apart.
    static const double multiplier[] = { 1.5, 2.0, 2.5, 3.0 };
    const int setting = config_emulator.ffwd_speed;
    if (setting < 0 || setting >= (int)(sizeof(multiplier) / sizeof(multiplier[0])))
        return 0.0;   // unlimited
    return multiplier[setting];
}

int emu_debug_step_frames_now(int frames, bool* breakpoint_hit)
{
    if (breakpoint_hit)
        *breakpoint_hit = false;
    if (emu_is_empty() || frames < 1)
        return 0;

    DiagInfo("debug", "synchronous step of %d frames (generation %llu)",
             frames, machine_generation);

    // RunToVBlank does nothing while the core is paused, and this is normally
    // called from a paused machine.
    const bool wasPaused = gearsf7000->IsPaused();
    if (wasPaused)
        gearsf7000->Pause(false);

    // Any counter left armed for the main loop would keep running frames after
    // this returns, on top of the ones asked for here.
    debugging = true;
    debug_step = false;
    debug_frames_pending = 0;

    int completed = 0;
    bool hit = false;

    for (int i = 0; i < frames; ++i)
    {
        int sampleCount = 0;
        resync_if_stale();
        const GC_RunResult result = gearsf7000->RunToVBlank(
            audio_buffer, &sampleCount, false, true);
        if (result.DidRun())
            video_output_blanked = false;

        // Recorded exactly as the main loop would, so a burst and a run leave
        // the same history behind. The samples are dropped: hundreds of frames
        // of audio produced in a fraction of a second have nowhere to go.
        if (result.reachedVBlank)
        {
            ++completed;
            rewind_push();
        }

        if (result.BreakpointHit())
        {
            hit = true;
            DiagInfo("debug", "synchronous step stopped by a breakpoint after "
                              "%d of %d frames", completed, frames);
            break;
        }
    }

    // Always ends paused, whatever it was before: this is a debugger
    // operation, and reading the machine afterwards has to describe where it
    // stopped.
    gearsf7000->Pause(true);

    if (breakpoint_hit)
        *breakpoint_hit = hit;
    return completed;
}

// Machine-level settings that come from the configuration file.
//
// These used to live in gui_init(), for historical reasons rather than because
// any of them needs a window - and headless, which skips gui_init(), therefore
// booted without a BIOS while the same configuration in the windowed app
// booted the game. Keeping one copy is the point: two paths that must agree
// and are written twice will not stay in agreement.
void emu_apply_startup_config(void)
{
    emu_audio_mute(!config_audio.enable);

    // Capturing history is a persisted emulator policy, not a property of the
    // Recorder window. Configure it in the shared startup path so windowed
    // and headless runs agree and the default recording is already active
    // before the first emulated frame.
#if GEARSF7000_ENABLE_RECORDER
    RewindConfigResult rewind_result;
    if (!rewind_configure(config_debug.rewind_enabled,
                          config_debug.rewind_seconds, 1, 0,
                          &rewind_result))
    {
        config_debug.rewind_enabled = false;
        DiagWarn("recorder", "startup configuration failed: %s",
                 rewind_result.error.c_str());
    }
#else
    config_debug.rewind_enabled = false;
#endif

    if (gearsf7000->GetMachineType() == MachineType::MSX || gearsf7000->GetMachineType() == MachineType::Coleco || gearsf7000->GetMachineType() == MachineType::SordM5)
        emu_reload_machine_firmware();
    else
        apply_machine_bios();

    const auto startup_video = emu_get_effective_video_output(config_debug.debug);
    if (startup_video.overscan == 4)
        emu_set_full_raster_debug_enabled(true);
    else
        emu_set_overscan(startup_video.overscan);

#ifdef SPRITE_EXPANDER
    emu_video_no_sprite_limit(config_video.sprite_limit);
#endif

    // The palette belongs here too. It decides the colours written into the
    // frame buffer, not how they are drawn on screen, so a headless capture
    // gets the same picture the window would - which is the whole point of
    // being able to take one without a window.
    emu_predefined_palette(*emu_palette_setting());

    emu_writeprotect_disc(config_emulator.disc_write_protected);
}

// Frames per second of the machine as it is currently configured.
//
// Not 60 and not 50: the TMS9918 draws 342 dots a line at two master clocks
// each, from a 10.738635 MHz crystal, over 262 lines NTSC or 313 PAL.
//
//   NTSC  10738635 / (684 * 262) = 59.9226 Hz
//   PAL   10738635 / (684 * 313) = 50.1591 Hz
//
// Rounding those to 60 and 50 is a 0.13% error, which is the difference
// between a minute of recording holding 3600 frames and the 3595.4 the
// machine really produces. The recorder counts ages in these seconds and the
// scheduler paces frames by them, so both ask here rather than keeping their
// own copy of the arithmetic.
//
// Taken from the machine's own clock domains rather than from a copy of the
// crystal frequency: if the VDP clock is ever corrected - the PAL figure in
// particular is a hardware question, not a derived one - everything follows
// instead of quietly disagreeing with the emulation.
// Serial of the frame currently exposed by the video device. Advances only when the
// beam actually drew one, so a consumer can tell a fresh frame from the same
// one being presented again - which the scheduler does whenever the display
// runs faster than the machine.
unsigned long long emu_get_frame_serial(void)
{
    return gearsf7000 ? gearsf7000->GetVideoFrameSerial() : 0;
}

void emu_set_target_frame_rate(double fps)
{
    if (gearsf7000)
        gearsf7000->SetTargetFrameRate(fps);
}

double emu_get_native_frame_rate(void)
{
    return gearsf7000 ? gearsf7000->GetNativeFrameRate() : 59.9226;
}

void emu_set_audio_clock_trim(double ratio)
{
    if (gearsf7000 && gearsf7000->GetAudio())
        gearsf7000->GetAudio()->SetClockTrim(ratio);
}

double emu_get_machine_fps(void)
{
    constexpr double kFallbackNtsc = 59.9226;

    if (!gearsf7000)
        return kFallbackNtsc;
    const double fps = gearsf7000->GetEffectiveFrameRate();
    return fps > 1.0 ? fps : kFallbackNtsc;
}

// Split out of emu_update so the scheduler can keep servicing the host on
// iterations where the machine owes no frame. Tools have to answer while the
// emulator is paused or waiting for its next frame - a debugger that stops
// responding whenever the machine is stopped is not a debugger - so this
// half runs every iteration of the main loop, unconditionally.
bool emu_debug_runtime_requested(void)
{
    // A remote debugger is an explicit consumer even with the local UI off.
    bool debug_runtime = config_debug.debug;
#if GEARSF7000_ENABLE_MCP
    debug_runtime = debug_runtime || (mcp_manager && mcp_manager->IsRunning());
#endif
    return debug_runtime;
}

void emu_pump_host(void)
{
    if (gearsf7000)
        gearsf7000->SetDebugRuntimeActive(emu_debug_runtime_requested());
#if GEARSF7000_ENABLE_MCP
    if (mcp_manager)
        mcp_manager->PumpCommands();
#endif
    if (gearsf7000)
        gearsf7000->ApplyPendingDebugEventCommands();
    update_audio_pause_state();
}

// The other half: advance the machine by one frame. Call it only when a
// frame is actually due; calling it in a loop is what fast forward does.
void emu_run_frame(void)
{
    if (!emu_is_empty())
    {
        int sampleCount = 0;

        // These GUI toggles now filter only their corresponding legacy lists;
        // they cannot silence unrelated types or common/MCP DebugEvent rules.
        gearsf7000->GetMemory()->SetLegacyCpuBreakpointsEnabled(!emu_debug_disable_breakpoints_cpu);
        gearsf7000->GetMemory()->SetLegacyMemoryBreakpointsEnabled(!emu_debug_disable_breakpoints_mem);

        if (!debugging || debug_step || debug_line_pending || debug_frames_pending > 0)
        {
            // Per-category legacy filters live in their own hooks. Keep this
            // path active for MCP/common rules and Run To Cursor.
            bool breakpoints = true;

            resync_if_stale();
            const GC_RunResult runResult = gearsf7000->RunToVBlank(
                audio_buffer, &sampleCount, debug_step, breakpoints,
                debug_line_pending);
            if (runResult.DidRun())
                video_output_blanked = false;
            if (runResult.BreakpointHit())
            {
                if (debug_frames_pending > 0)
                    DiagInfo("debug", "bulk step cut short by a breakpoint, "
                                      "%d frames unrun (generation %llu)",
                             debug_frames_pending, machine_generation);
                debugging = true;
                debug_frames_pending = 0;
            }

            if (!runResult.BreakpointHit() && debug_frames_pending > 0)
            {
                --debug_frames_pending;
                if (debug_frames_pending == 0)
                    DiagInfo("debug", "bulk step finished (generation %llu)",
                             machine_generation);
            }
            debug_step = false;
            debug_line_pending = false;

            // One entry per frame that actually reached VBlank. Testing for
            // "no breakpoint" instead is not the same thing: while the core
            // is paused RunToVBlank returns immediately without running
            // anything, and recording that answered every GUI frame with a
            // fresh copy of the same stopped machine - which then evicted the
            // real history behind it. A pause has to leave the recording
            // alone.
            if (runResult.reachedVBlank)
                rewind_push();

#if GEARSF7000_ENABLE_MCP
            // Freezes/trigger condition run every frame regardless of
            // whether an MCP client is connected right now, so a freeze set
            // up earlier keeps holding and a condition can still fire.
            if (gearsf7000->GetDebugEventManager()->IsRuntimeActive() &&
                mcp_manager && mcp_manager->GetAdapter() && mcp_manager->GetAdapter()->WatchTick())
            {
                debugging = true;
                debug_frames_pending = 0;
            }
#endif
        }

        // RunToVBlank may itself enter a pause/breakpoint state.
        update_audio_pause_state();
        crt_signal_dump_tick();
#if GEARSF7000_ENABLE_DEBUG_TOOLS
        rom_map_project_tick();
#endif

        if ((sampleCount > 0) && !audio_output_paused && sound_queue)
        {
            sound_queue->Write(audio_buffer, sampleCount, emu_audio_sync);
        }
    }
}

// Kept for the headless front end, which has no presentation to decouple
// from and so has no reason to separate the two halves.
void emu_update(void)
{
    emu_pump_host();
    emu_run_frame();
}

void emu_pause_key_pressed()
{
    gearsf7000->PauseKeyPressed();
#if GEARSF7000_ENABLE_SR1000
    if (gearsf7000->HasCassetteInterface() && gearsf7000->GetMachineType() != MachineType::MSX)
        gearsf7000->GetCassette()->SetMotor(false);
#endif
    update_audio_pause_state();
}

// Who started the sequence that is currently being typed. Two things drive
// this keyboard - the BASIC Typer panel and MCP - and when characters appear
// on screen unbidden, the first question is which one. Keeping it here rather
// than in the panel means a plain keyboard_text call is labelled too, not just
// the ones that go through the panel's buffer.
static std::string keyboard_text_source;

bool emu_keyboard_text(const char* text, const char* source, std::string* error)
{
    if (!text || !gearsf7000->QueueKeyboardText(text, error))
        return false;
    keyboard_text_source = source ? source : "unknown";
    return true;
}

const char* emu_keyboard_text_source()
{
    return keyboard_text_source.c_str();
}

float emu_keyboard_text_progress()
{
    return gearsf7000 ? gearsf7000->GetKeyboardTextProgress() : 1.0f;
}

bool emu_keyboard_text_busy()
{
    return gearsf7000 && gearsf7000->GetKeyboardTextProgress() < 1.0f;
}

void emu_keyboard_clear_text()
{
    gearsf7000->ClearKeyboardText();
    keyboard_text_source.clear();
}

void emu_keyboard_text_timing(int press_polls, int release_polls, int newline_polls)
{
    if (gearsf7000)
        gearsf7000->SetKeyboardTextTiming(press_polls, release_polls, newline_polls);
}

bool emu_keyboard_can_type(char character)
{
    return SegaKeyboard::CanTypeChar(character);
}


void emu_enable_events(bool enabled)
{
    gearsf7000->EnableEvents(enabled);
}

void emu_set_keyboard_mode(bool enabled)
{
    // Kept in the config too, so that reading the mode back reports what the
    // machine is actually doing rather than what the menu was last set to.
    config_emulator.keyboard_mode = enabled;
    gearsf7000->SetKeyboardMode(enabled);
    if (!enabled && (gearsf7000->GetMachineType() == MachineType::MSX || gearsf7000->GetMachineType() == MachineType::SordM5)) emu_clear_matrix_host_input();
}

bool emu_get_keyboard_mode(void)
{
    return gearsf7000->HasKeyboardInterface() &&
        config_emulator.keyboard_mode;
}

//void emu_key_pressed(GC_Controllers controller, GC_Keys key)
//{
//    gearsf7000->KeyPressed(controller, key);
//}
//
//void emu_key_released(GC_Controllers controller, GC_Keys key)
//{
//    gearsf7000->KeyReleased(controller, key);
//}


void emu_input_event(SDL_Event event)
{
    gearsf7000->SetEvent(event);
}

void emu_input_pause(void)
{
    update_audio_pause_state();
}


// What is plugged into a Master System controller port. Kept in the
// configuration too, so the choice survives a restart.
bool emu_set_controller_device(int player, int device)
{
    if ((player != 0 && player != 1) || !IsValidControllerDevice(device))
        return false;
    config_input[player].controller_device = device;
    return gearsf7000->SetControllerDevice(player == 0 ? Controller_1 : Controller_2,
                                           static_cast<SystemControllerDevice>(device));
}

// What the host holds now, from every source that reaches the machine through
// here: keyboard and gamepad events, the on-screen SK-1100 keyboard and the MCP
// (controller_button, keyboard_key). A loaded state carries the inputs held when
// it was saved, and every source sends only changes, so a key released since
// would stay down in the machine. emu_resync_host_input() puts back this list.
namespace
{
bool host_joy[2][32] = {};
u16 host_matrix[8] = {};
u8 matrix_host_input[11] = {};
std::set<std::string> host_labels;
// A Recorder snapshot was loaded and still carries its own inputs; the next frame
// the machine actually runs puts back the host's first.
bool host_input_stale = false;

void resync_if_stale(void)
{
    if (host_input_stale)
        emu_resync_host_input();
}
}

void emu_mark_host_input_stale(void)
{
    host_input_stale = true;
}

void emu_clear_matrix_host_input(void)
{
    for (auto& row : matrix_host_input) row = 0;
    host_labels.clear();
    gearsf7000->ReleaseHostInput();
}

void emu_release_host_input(void)
{
    gearsf7000->ReleaseHostInput();
}

// After a state is loaded and live play takes over from it: release everything
// the state held, then press again what the host holds now. Scrubbing the
// Recorder does not come here - a snapshot being inspected shows its own inputs.
void emu_resync_host_input(void)
{
    host_input_stale = false;
    gearsf7000->ReleaseHostInput();
    for (int c = 0; c < 2; ++c)
        for (int key = 0; key < 32; ++key)
            if (host_joy[c][key])
                gearsf7000->JoystickPressed(static_cast<GC_Controllers>(c), static_cast<GC_Keys>(key));
    if (gearsf7000->GetMachineType() == MachineType::MSX || gearsf7000->GetMachineType() == MachineType::SordM5) {
        for (int row = 0; row < 11; ++row)
            if (matrix_host_input[row]) gearsf7000->KeyboardMatrixKey(row, matrix_host_input[row], true);
    }
    else for (int row = 0; row < 8; ++row)
        if (host_matrix[row])
            gearsf7000->KeyboardMatrixKey(row, host_matrix[row], true);
    for (const std::string& label : host_labels)
        gearsf7000->KeyboardKey(label.c_str(), true);
}

void emu_joy_pressed(GC_Controllers controller, GC_Keys key)
{
    if (key >= 0 && key < 32)
        host_joy[controller & 1][key] = true;
    gearsf7000->JoystickPressed(controller, key);
}

void emu_joy_released(GC_Controllers controller, GC_Keys key)
{
    if (key >= 0 && key < 32)
        host_joy[controller & 1][key] = false;
    gearsf7000->JoystickReleased(controller, key);
}

bool emu_keyboard_key(const char* label, bool pressed)
{
    const bool known = gearsf7000->KeyboardKey(label, pressed);
    if (known && label != nullptr)
    {
        if (pressed)
            host_labels.insert(label);
        else
            host_labels.erase(label);
    }
    return known;
}

void emu_keyboard_matrix_key(int row, int mask, bool pressed)
{
    if (gearsf7000->GetMachineType() == MachineType::MSX || gearsf7000->GetMachineType() == MachineType::SordM5) {
        if (row >= 0 && row < 11 && mask > 0 && mask <= 255)
            matrix_host_input[row] = pressed ? matrix_host_input[row] | mask : matrix_host_input[row] & ~mask;
    }
    else if (row >= 0 && row < 8)
        host_matrix[row] = static_cast<u16>(pressed ? (host_matrix[row] | mask)
                                                    : (host_matrix[row] & ~mask));
    gearsf7000->KeyboardMatrixKey(row, mask, pressed);
}

void emu_pause(void)
{
    gearsf7000->Pause(true);
    update_audio_pause_state();
}

void emu_resume(void)
{
    gearsf7000->Pause(false);
    update_audio_pause_state();
}

bool emu_is_paused(void)
{
    return gearsf7000->IsPaused();
}

bool emu_is_debugging(void)
{
    return debugging;
}

bool emu_is_execution_stopped(void)
{
    return emu_is_paused() || emu_is_debugging();
}

bool emu_is_empty(void)
{
    return !gearsf7000->IsMachineReady();
}

bool emu_is_bios_loaded(void)
{
    return gearsf7000->GetMemory()->IsBiosLoaded();
}

void emu_start_sf7000(bool pauseAtResetVector)
{
    save_ram();
    config_select_machine(WithSF7000(gearsf7000->GetSystemModel()));
    if (!gearsf7000->ApplySystemModelAtReset(
            static_cast<SystemModel>(config_emulator.machine))) {
        config_select_machine(gearsf7000->GetSystemModel());
        return;
    }
    apply_configured_video_device();
    apply_machine_bios();
    if (!emu_is_bios_loaded())
        return;
    gearsf7000->StartSF7000(pauseAtResetVector);
    apply_configured_video_output();
    load_ram();
#if GEARSF7000_ENABLE_DEBUG_TOOLS
    rom_map_project_on_media_changed();
#endif
    if (!pauseAtResetVector)
        emu_debug_continue();
}

void emu_reset(Cartridge::ForceConfiguration config, bool pauseAtResetVector)
{
    save_ram();
    const SystemModel requested = static_cast<SystemModel>(config_emulator.machine);
    if (gearsf7000->GetMachineType() == MachineType::MSX || gearsf7000->GetMachineType() == MachineType::SordM5 || IsMSXSystem(requested) || IsM5System(requested))
        emu_clear_matrix_host_input();
    if (!gearsf7000->ApplySystemModelAtReset(requested))
    {
        config_select_machine(gearsf7000->GetSystemModel());
        Log("System selection could not be applied; keeping the current configuration");
    }
    gearsf7000->SetM5EM5Installed(config_emulator.m5_em5_installed);
    apply_configured_video_device();
    apply_machine_bios();
    gearsf7000->SetKeyboardMode(config_emulator.keyboard_mode);
    if (IsSF7000System(gearsf7000->GetSystemModel()))
        gearsf7000->StartSF7000(pauseAtResetVector);
    else if (gearsf7000->GetCartridge()->IsReady() || gearsf7000->IsMachineReady())
        gearsf7000->ResetROM(&config, pauseAtResetVector);
    else
        gearsf7000->EjectCartridge();
    apply_configured_video_output();
#if GEARSF7000_ENABLE_SR1000
    if (gearsf7000->GetMachineType() == MachineType::MSX &&
        !gearsf7000->GetCassette()->IsLoaded() && !config_emulator.msx_tape.path.empty())
        application_load_tape(config_emulator.msx_tape.path.c_str());
#endif
    ++machine_generation;
    DiagInfo("media", "machine reset, generation %llu (paused=%d)",
             machine_generation, pauseAtResetVector ? 1 : 0);
    load_ram();
    // Everything before the reset leads to a machine that no longer exists.
    rewind_reset();
    debugging = pauseAtResetVector;
    debug_step = false;
    debug_line_pending = false;
    debug_frames_pending = 0;
}

static void apply_configured_video_device(void)
{
    if (config_emulator.vdp == 0)
    {
        gearsf7000->SelectDefaultVideoDevice();
    }
    else
    {
        if (!gearsf7000->SelectVideoOption(config_selected_video_option()))
        {
            // A motherboard change can invalidate a previously selected chip.
            // Fall back visibly to that machine's declared default.
            config_emulator.vdp = 0;
            gearsf7000->SelectDefaultVideoDevice();
        }
    }

    // Selection is installed by the ensuing reset/load, before output policy is applied.
}

static void apply_configured_video_output(void)
{
    // A machine reset can replace one VDP family with another, where the
    // concrete device cannot copy private presentation state. Reapply the
    // persisted semantic options so the Sega NTSC device immediately uses
    // the same border choice as the rest of the video pipeline.
    const auto output = emu_get_effective_video_output(config_debug.debug);
    if (output.overscan == 4)
        gearsf7000->SetVideoOption(GC_VideoOption::FullRaster, 1);
    else
    {
        gearsf7000->SetVideoOption(GC_VideoOption::FullRaster, 0);
        gearsf7000->SetVideoOption(GC_VideoOption::Border, output.overscan);
    }
    gearsf7000->SetVideoOption(GC_VideoOption::PredefinedPalette,
                               *emu_palette_setting());
}

void emu_eject_rom(void)
{
    // Flush the outgoing cartridge's own battery-backed SRAM before it is
    // gone; there is nothing to load_ram() afterward, since no cartridge is
    // inserted once this returns.
    save_ram();
    gearsf7000->EjectCartridge();
    ++machine_generation;
    DiagInfo("media", "cartridge ejected, generation %llu", machine_generation);
#if GEARSF7000_ENABLE_DEBUG_TOOLS
    rom_map_project_on_media_changed();
#endif
    rewind_reset();
}

void emu_dissasemble_rom(void)
{
    gearsf7000->SaveDisassembledROM();
}

void emu_audio_mute(bool mute)
{
    audio_enabled = !mute;
    gearsf7000->GetAudio()->Mute(mute);
}

#if GEARSF7000_ENABLE_AY
void emu_set_ay_config(bool enabled, int chip, int port_base)
{
    // The port window moves immediately - it is just where the card answers,
    // and nothing latches it. Presence and chip type are staged instead and
    // take hold in Audio::Reset(), because they are the equivalent of fitting
    // or swapping hardware and must not change under running software.
    SetAyPortBase(static_cast<u8>(port_base));

    Audio* audio = gearsf7000->GetAudio();
    if (IsValidPointer(audio))
    {
        audio->SetAyEnabled(enabled);
        audio->SetAyChip(chip == 1 ? Audio::AyChip::YM2149
                                   : Audio::AyChip::AY_3_8910);
    }
}
#endif

#if GEARSF7000_ENABLE_YM2413
void emu_set_ym2413_config(bool enabled)
{
    if (gearsf7000 && gearsf7000->GetAudio())
        gearsf7000->GetAudio()->SetYm2413Enabled(enabled);
}
#endif

void emu_audio_reset(void)
{
    // No audio device in headless runs.
    if (!sound_queue)
        return;

    sound_queue->Stop();
    sound_queue->ResetDiagnostics();
    if (!audio_output_paused)
        sound_queue->Start(GC_AUDIO_SAMPLE_RATE, GC_AUDIO_BUFFER_NUM);
}

bool emu_start_vgm_recording(const char* file_path)
{
#if GEARSF7000_ENABLE_RECORDER
    if (!gearsf7000->GetCartridge()->IsReady())
        return false;

    if (gearsf7000->GetAudio()->IsVgmRecording())
        emu_stop_vgm_recording();

    // Nominal motherboard clock, not the live one: SetTargetFrameRate can have
    // aligned the machine to the host display, and a VGM player replays
    // writes on the crystal a real machine would carry, not on whatever this
    // session's monitor happened to dictate.
    const bool isPAL =
        gearsf7000->GetVideoFrameDescriptor().region == Region_PAL;
    const int clockRate = static_cast<int>(
        gearsf7000->GetSystemBus()->GetCpuPsgClockHz());

    return gearsf7000->GetAudio()->StartVgmRecording(file_path, clockRate, isPAL);
#else
    (void)file_path;
    return false;
#endif
}

std::string emu_stop_vgm_recording(void)
{
#if GEARSF7000_ENABLE_RECORDER
    return gearsf7000->GetAudio()->StopVgmRecording();
#else
    return std::string();
#endif
}

bool emu_start_wav_recording(const char* file_path)
{
#if GEARSF7000_ENABLE_RECORDER
    if (!gearsf7000->IsMachineReady())
        return false;

    if (gearsf7000->GetAudio()->IsRecording())
        gearsf7000->GetAudio()->StopRecording();

    gearsf7000->GetAudio()->StartRecording(file_path);
    return gearsf7000->GetAudio()->IsRecording();
#else
    (void)file_path;
    return false;
#endif
}

bool emu_stop_wav_recording(void)
{
#if GEARSF7000_ENABLE_RECORDER
    if (!gearsf7000->GetAudio()->IsRecording())
        return false;
    gearsf7000->GetAudio()->StopRecording();
    return true;
#else
    return false;
#endif
}

bool emu_is_wav_recording(void)
{
#if GEARSF7000_ENABLE_RECORDER
    return gearsf7000->GetAudio()->IsRecording();
#else
    return false;
#endif
}

bool emu_is_vgm_recording(void)
{
#if GEARSF7000_ENABLE_RECORDER
    return gearsf7000->GetAudio()->IsVgmRecording();
#else
    return false;
#endif
}

static void update_audio_pause_state(void)
{
    // No audio device in headless runs.
    if (!sound_queue)
        return;

    const bool emulationPaused = gearsf7000 && gearsf7000->IsPaused();
    const bool shouldPauseOutput = emulationPaused || audio_suspended_for_file_dialog;
    if (!sound_queue || shouldPauseOutput == audio_output_paused)
        return;

    audio_output_paused = shouldPauseOutput;
    if (audio_output_paused)
    {
        // Pause is intentional: discard queued audio and make the diagnostic
        // counters describe only glitches while the emulation is running.
        sound_queue->Stop();
        sound_queue->ResetDiagnostics();
    }
    else
    {
        if (!sound_queue->Start(GC_AUDIO_SAMPLE_RATE, GC_AUDIO_BUFFER_NUM))
            Log("Audio: could not restart output after pause");
        sound_queue->ResetDiagnostics();
    }
}

void emu_audio_suspend_for_file_dialog(void)
{
    audio_suspended_for_file_dialog = true;
    update_audio_pause_state();
}

void emu_audio_resume_after_file_dialog(void)
{
    audio_suspended_for_file_dialog = false;
    update_audio_pause_state();
}



bool emu_is_audio_enabled(void)
{
    return audio_enabled;
}

EmuAudioQueueDiagnostics emu_get_audio_queue_diagnostics(void)
{
    if (!sound_queue)
        return EmuAudioQueueDiagnostics();

    EmuAudioQueueDiagnostics result = {};
    if (!sound_queue)
        return result;

    const SoundQueue::Diagnostics diagnostics = sound_queue->GetDiagnostics();
    result.callbacks = diagnostics.callbacks;
    result.underruns = diagnostics.underruns;
    result.zero_filled_samples = diagnostics.zero_filled_samples;
    result.producer_dropped_samples = diagnostics.producer_dropped_samples;
    result.variable_size_callbacks = diagnostics.variable_size_callbacks;
    result.min_callback_interval_us = diagnostics.min_callback_interval_us;
    result.max_callback_interval_us = diagnostics.max_callback_interval_us;
    result.client_sample_rate = diagnostics.client_sample_rate;
    result.device_sample_rate = diagnostics.device_sample_rate;
    result.min_requested_samples = diagnostics.min_requested_samples;
    result.max_requested_samples = diagnostics.max_requested_samples;
    result.queued_samples = diagnostics.queued_samples;
    result.ring_capacity_samples = diagnostics.ring_capacity_samples;
    return result;
}

void emu_reset_audio_queue_diagnostics(void)
{
    // No audio device in headless runs.
    if (!sound_queue)
        return;

    if (sound_queue)
        sound_queue->ResetDiagnostics();
}

// The setting the installed VDP's palette list belongs to. The Mark III's
// VDP lists its TMS-mode colours under "sms." codes and keeps them apart, so
// the two machines never overwrite each other's choice through the fallback
// below.
int* emu_palette_setting(void)
{
    GC_VideoColorProfileDescriptor first;
    if (gearsf7000 && gearsf7000->GetVideoColorProfileCount() > 0 &&
        gearsf7000->DescribeVideoColorProfile(0, first) && first.code &&
        std::strncmp(first.code, "sms.", 4) == 0)
        return &config_video.sms_tms_palette;
    return &config_video.palette;
}

void emu_predefined_palette(int palette)
{
    const std::size_t count = gearsf7000->GetVideoColorProfileCount();
    for (std::size_t index = 0; index < count; ++index)
    {
        GC_VideoColorProfileDescriptor profile;
        if (gearsf7000->DescribeVideoColorProfile(index, profile) &&
            profile.value == palette)
        {
            gearsf7000->SetVideoOption(
                GC_VideoOption::PredefinedPalette, profile.value);
            return;
        }
    }

    // This also migrates the old value 3 used by the removed custom editor.
    GC_VideoColorProfileDescriptor fallback;
    if (count > 0 && gearsf7000->DescribeVideoColorProfile(0, fallback))
    {
        *emu_palette_setting() = static_cast<int>(fallback.value);
        gearsf7000->SetVideoOption(
            GC_VideoOption::PredefinedPalette, fallback.value);
    }
}

bool emu_load_basic_program(const char* file_path, int pointer_block, std::string* status)
{
    if (!gearsf7000)
    {
        if (status) *status = "No machine running";
        return false;
    }

    BasicProgramLoader::Result result =
        BasicProgramLoader::Load(gearsf7000->GetMemory(), (u16)pointer_block, file_path ? file_path : "");

    if (status)
    {
        char text[256];
        if (result.success && result.detected)
            snprintf(text, sizeof(text),
                     "Loaded %d bytes at $%04X (pointer block found at $%04X), %d free to $%04X",
                     result.size, result.address, result.block,
                     (int)(result.free_end - result.address - result.size),
                     result.free_end);
        else if (result.success)
            snprintf(text, sizeof(text),
                     "Loaded %d bytes at $%04X, %d free to $%04X",
                     result.size, result.address,
                     (int)(result.free_end - result.address - result.size),
                     result.free_end);
        else
            snprintf(text, sizeof(text), "%s", result.error.c_str());
        *status = text;
    }

    return result.success;
}

bool emu_save_basic_program(const char* file_path, int pointer_block, std::string* status)
{
    if (!gearsf7000)
    {
        if (status) *status = "No machine running";
        return false;
    }

    BasicProgramLoader::Result result =
        BasicProgramLoader::Save(gearsf7000->GetMemory(), (u16)pointer_block, file_path ? file_path : "");

    if (status)
    {
        char text[256];
        if (result.success && result.detected)
            snprintf(text, sizeof(text), "Saved %d bytes from $%04X (pointer block found at $%04X)",
                     result.size, result.address, result.block);
        else if (result.success)
            snprintf(text, sizeof(text), "Saved %d bytes from $%04X", result.size, result.address);
        else
            snprintf(text, sizeof(text), "%s", result.error.c_str());
        *status = text;
    }

    return result.success;
}

int emu_find_basic_blocks(u16* out, int max_out)
{
    if (!gearsf7000 || !out || max_out <= 0)
        return 0;

    const std::vector<u16> found = BasicProgramLoader::FindCandidateBlocks(gearsf7000->GetMemory());
    int count = 0;
    for (size_t i = 0; i < found.size() && count < max_out; i++)
        out[count++] = found[i];
    return count;
}

void emu_save_ram(const char* file_path)
{
    if (!emu_is_empty())
        gearsf7000->SaveRam(file_path, true);
}

void emu_load_ram(const char* file_path, Cartridge::ForceConfiguration config)
{
    if (!emu_is_empty())
    {
        save_ram();
        gearsf7000->ResetROM(&config);
        apply_configured_video_output();
        gearsf7000->LoadRam(file_path, true);
    }
}

static const char* state_slot_directory()
{
    if (emu_savestates_dir_option == 0 && emu_savestates_path[0])
        return emu_savestates_path;
    // Built-in firmware has no file directory. Finder may launch with / as
    // cwd, so the ROM-directory preference must fall back to user settings.
    if (emu_is_cbios_active() && !gearsf7000->GetCartridge()->IsReady())
        return config_root_path;
    return nullptr;
}

bool emu_save_state_slot(int index)
{
    if (emu_is_empty())
        return false;

    return gearsf7000->SaveState(state_slot_directory(), index);
}

bool emu_load_state_slot(int index)
{
    if (emu_is_empty())
        return false;

    const bool loaded = gearsf7000->LoadState(state_slot_directory(), index);

    if (loaded)
    {
        rewind_abandon_history();
        emu_resync_host_input();
    }

    return loaded;
}

bool emu_save_state_file(const char* file_path, size_t* bytes_written)
{
    if (emu_is_empty())
        return false;

    return gearsf7000->SaveState(file_path, -1, bytes_written);
}

bool emu_load_state_file(const char* file_path)
{
    if (emu_is_empty())
        return false;

    const bool loaded = gearsf7000->LoadState(file_path, -1);

    if (loaded)
    {
        rewind_abandon_history();
        emu_resync_host_input();
    }

    return loaded;
}

void emu_get_runtime(GC_RuntimeInfo& runtime)
{
    gearsf7000->GetRuntimeInfo(runtime);
}

GC_VideoFrameDescriptor emu_get_video_frame_descriptor(void)
{
    return gearsf7000 ? gearsf7000->GetVideoFrameDescriptor()
                      : GC_VideoFrameDescriptor{};
}

GC_VideoFrameView emu_get_video_frame_view(void)
{
    if (!gearsf7000)
        return {};

    const GC_VideoFrameDescriptor descriptor =
        gearsf7000->GetVideoFrameDescriptor();
    if (!video_output_blanked)
        return gearsf7000->GetVideoFrameView();

    ensure_blank_frame_buffer_capacity();
    if (!blank_frame_buffer)
        return {};

    GC_VideoFrameView view;
    view.descriptor = descriptor;
    view.pixels = blank_frame_buffer;
    view.pixel_format = GC_PIXEL_RGB888;
    view.row_stride_bytes = descriptor.frame_width * 3;
    view.frame_serial = gearsf7000->GetVideoFrameSerial();
    return view;
}

void emu_clear_video_buffer(void)
{
    ensure_blank_frame_buffer_capacity();
    if (blank_frame_buffer && blank_frame_buffer_capacity > 0)
    {
        memset(blank_frame_buffer, 0, blank_frame_buffer_capacity);
        video_output_blanked = true;
    }
}

int emu_encode_png_rgba(const u8* pixels, int width, int height,
                        unsigned char** out_buffer)
{
    if (pixels == NULL || out_buffer == NULL || width <= 0 || height <= 0)
        return 0;
    int len = 0;
    *out_buffer = stbi_write_png_to_mem(pixels, width * 4, width, height, 4,
                                        &len);
    return (*out_buffer != NULL) ? len : 0;
}

int emu_get_screenshot_png(unsigned char** out_buffer, bool full_frame, GC_VideoRect* out_area)
{
    // Same readiness condition as GearSF7000Core::IsMachineReady(): SF-7000
    // IPL+RAM alone boot a complete machine with no SC-3000 cartridge
    // inserted at all, and that case has a frame buffer to capture too.
    if (!gearsf7000->IsMachineReady())
        return 0;

    const GC_VideoFrameView frame = emu_get_video_frame_view();
    if (!frame.IsValid() || frame.pixel_format != GC_PIXEL_RGB888)
        return 0;

    const GC_VideoFrameDescriptor& descriptor = frame.descriptor;
    const GC_VideoRect area = full_frame
        ? GC_VideoRect{0, 0, descriptor.frame_width, descriptor.frame_height}
        : descriptor.visible_area;
    const u8* areaPixels = frame.pixels +
        static_cast<size_t>(area.y) * frame.row_stride_bytes +
        static_cast<size_t>(area.x) * 3u;
    int len = 0;

    *out_buffer = stbi_write_png_to_mem(areaPixels,
        frame.row_stride_bytes, area.width, area.height, 3, &len);
    if (out_area)
        *out_area = area;

    return len;
}

// Capture runs across whole frames, so it cannot complete inside the call
// that starts it: the sink stays plugged in until it has seen the lines it
// was asked for, and crt_signal_dump_tick (called once per frame from
// emu_update) is what notices, writes the file and unplugs it.
static CrtSignalDumpSink* crt_signal_dump_sink = NULL;
static std::string crt_signal_dump_path;

bool emu_crt_signal_dump_start(const char* file_path, int lines)
{
    if (!gearsf7000 || !gearsf7000->IsMachineReady() || file_path == NULL)
        return false;
    if (crt_signal_dump_sink != NULL)
        return false;

    crt_signal_dump_path = file_path;
    crt_signal_dump_sink = new CrtSignalDumpSink(lines);
    if (!gearsf7000->AttachVideoCrtSignalSink(crt_signal_dump_sink))
    {
        SafeDelete(crt_signal_dump_sink);
        return false;
    }
    return true;
}

bool emu_crt_signal_dump_in_progress(void)
{
    return crt_signal_dump_sink != NULL;
}

void crt_signal_dump_tick(void)
{
    if (crt_signal_dump_sink == NULL || !crt_signal_dump_sink->IsComplete())
        return;

    crt_signal_dump_sink->WriteTo(crt_signal_dump_path.c_str());
    gearsf7000->AttachVideoCrtSignalSink(NULL);
    SafeDelete(crt_signal_dump_sink);
    Log("CRT signal dump written to %s", crt_signal_dump_path.c_str());
}

void emu_set_full_raster_debug_enabled(bool enabled)
{
    if (!gearsf7000)
        return;

    if (!gearsf7000->SetVideoOption(GC_VideoOption::FullRaster, enabled))
        return;

}

int emu_get_full_raster_debug_png(unsigned char** out_buffer)
{
    if (!gearsf7000 || !gearsf7000->IsMachineReady())
        return 0;

    const GC_VideoFrameView frame = gearsf7000->GetVideoFrameView();
    if (!frame.IsValid() || frame.pixel_format != GC_PIXEL_RGB888 ||
        frame.descriptor.surface_kind != GC_VIDEO_SURFACE_FULL_RASTER_DEBUG)
        return 0;

    int len = 0;
    *out_buffer = stbi_write_png_to_mem(frame.pixels,
        frame.row_stride_bytes, frame.descriptor.frame_width,
        frame.descriptor.frame_height, 3, &len);
    return len;
}

static void ensure_blank_frame_buffer_capacity(void)
{
    const GC_VideoFrameDescriptor descriptor = emu_get_video_frame_descriptor();
    if (!descriptor.IsValid())
        return;
    const size_t required = static_cast<size_t>(descriptor.buffer_width) *
        static_cast<size_t>(descriptor.buffer_height) * 3u;
    if (required == 0 || required <= blank_frame_buffer_capacity)
        return;

    u8* replacement = new u8[required]();
    SafeDeleteArray(blank_frame_buffer);
    blank_frame_buffer = replacement;
    blank_frame_buffer_capacity = required;
}

void emu_get_info(char* info)
{
    if (!emu_is_empty())
    {
        Cartridge* cart = gearsf7000->GetCartridge();
        GC_RuntimeInfo runtime;
        gearsf7000->GetRuntimeInfo(runtime);

        const char* filename = cart->GetFileName();
        const char* pal = runtime.region == Region_PAL ? "PAL" : "NTSC";
        const char* mapper = emu_get_mapper_name(cart->GetType());
        const char* selection = config_emulator.mapper_mode == 1
            ? "Manual"
            : (cart->IsTypeIdentified() ? "Auto (detected)" : "Auto (fallback)");
        const char* asc16l_signature = cart->GetType() == Cartridge::SC3000_ASC16LS
            ? ""
            : cart->HasASC16LSignature()
                ? "ASC16L Signature: Present at $3FF0\n"
                : "ASC16L Signature: Not present\n";
        const int rom_size = cart->GetROMSize();
        const int rom_pages = cart->GetROMBankCount();

        if (cart->GetType() == Cartridge::SC3000_SURVIVORS_MULTICART
            || cart->GetType() == Cartridge::SC3000_SURVIVORS_MEGACART)
        {
            snprintf(info, 512,
                "File Name: %s\nMapper: %s\nMapper Selection: %s\n"
                "%sCRC32: %08X\nROM Size: %d bytes (%d KiB)\n"
                "32 KiB ROM Blocks: %d\nRefresh Rate: %s\nScreen Resolution: %dx%d",
                filename, mapper, selection, asc16l_signature, cart->GetCRC(),
                rom_size, rom_size / 1024, rom_size / 0x8000, pal,
                runtime.screen_width, runtime.screen_height);
        }
        else
        {
            snprintf(info, 512,
                "File Name: %s\nMapper: %s\nMapper Selection: %s\n"
                "%sCRC32: %08X\nROM Size: %d bytes (%d KiB)\n"
                "16 KiB ROM Pages: %d\nRefresh Rate: %s\nScreen Resolution: %dx%d",
                filename, mapper, selection, asc16l_signature, cart->GetCRC(),
                rom_size, rom_size / 1024, rom_pages, pal,
                runtime.screen_width, runtime.screen_height);
        }
    }
    else
    {
        snprintf(info, 512, "There is no ROM loaded!");
    }
}

GearSF7000Core* emu_get_core(void)
{
    return gearsf7000;
}

#if GEARSF7000_ENABLE_MCP
void emu_mcp_start(void) { if (mcp_manager) mcp_manager->Start(MCP_START_GUI); }
void emu_mcp_start_from_command_line(void) { if (mcp_manager) mcp_manager->Start(MCP_START_COMMAND_LINE); }
void emu_mcp_stop(void) { if (mcp_manager) mcp_manager->Stop(); }
void emu_mcp_set_transport(int mode, int port, const char* address) { if (mcp_manager) mcp_manager->SetTransportMode((McpTransportMode)mode, port, address); }
bool emu_mcp_is_running(void) { return mcp_manager && mcp_manager->IsRunning(); }
int emu_mcp_get_transport_mode(void) { return mcp_manager ? mcp_manager->GetTransportMode() : -1; }
bool emu_mcp_started_from_command_line(void) { return mcp_manager && mcp_manager->GetStartSource() == MCP_START_COMMAND_LINE; }
bool emu_mcp_is_listening(void) { return mcp_manager && mcp_manager->GetTransportStatus().listening; }
int emu_mcp_get_port(void) { return mcp_manager ? mcp_manager->GetPort() : 0; }
const char* emu_mcp_get_address(void) { return mcp_manager ? mcp_manager->GetAddress().c_str() : ""; }
void emu_mcp_pump_commands(void) { if (mcp_manager) mcp_manager->PumpCommands(); }
DebugAdapter* emu_mcp_get_adapter(void) { return mcp_manager ? mcp_manager->GetAdapter() : NULL; }
#endif

void emu_debug_step_into(void)
{
    debugging = debug_step = true;
    debug_frames_pending = 0;
    gearsf7000->Pause(false);
}

void emu_debug_step(void)
{
    emu_debug_step_into();
}

void emu_debug_step_line(void)
{
    debugging = true;
    debug_step = false;
    debug_line_pending = true;
    debug_frames_pending = 0;
    gearsf7000->Pause(false);
}

namespace
{
bool is_z80_call_or_rst(Memory* memory, u16 pc)
{
    // DD/FD prefixes are ignored by CALL/RST, but are legal and contribute to
    // the return address. Skip them to classify the effective opcode.
    u16 opcodeAddress = pc;
    u8 opcode = memory->Peek(opcodeAddress);
    for (int prefixes = 0;
         prefixes < 16 && (opcode == 0xdd || opcode == 0xfd);
         ++prefixes)
    {
        opcode = memory->Peek(++opcodeAddress);
    }

    switch (opcode)
    {
        case 0xc4: case 0xcc: case 0xd4: case 0xdc:
        case 0xe4: case 0xec: case 0xf4: case 0xfc:
        case 0xcd:
        case 0xc7: case 0xcf: case 0xd7: case 0xdf:
        case 0xe7: case 0xef: case 0xf7: case 0xff:
            return true;
        default:
            return false;
    }
}
}

void emu_debug_step_over(void)
{
    GearSF7000Core* core = emu_get_core();
    const u16 pc = core->GetCpuStateAccess()->GetCpuStateSnapshot().pc;
    Memory* memory = core->GetMemory();

    core->GetDisassembler()->Disassemble(pc);
    Memory::stDisassembleRecord* record =
        memory->GetDisassembleRecord(pc, false);

    if (record && record->size > 0 && is_z80_call_or_rst(memory, pc))
    {
        memory->ArmRunToAddress(static_cast<u16>(pc + record->size));
        emu_debug_continue();
        return;
    }

    emu_debug_step_into();
}

void emu_debug_break(void)
{
    // Break is not a pause. Pausing freezes the host loop wherever it stands,
    // which as far as the machine is concerned can be the middle of an
    // instruction; the debugger would then be showing a PC that no
    // instruction ever started at. Break instead lets the core run and puts
    // it in single step, so it halts at the next instruction boundary with a
    // real PC on screen. That is the same operation as stepping one
    // instruction from a running machine, so it shares the implementation
    // rather than keeping a second copy of these flags in sync.
    emu_debug_step_into();
}

void emu_debug_continue(void)
{
    if (gearsf7000->GetMachineType() == MachineType::MSX &&
        (emu_is_empty() || emu_has_pending_msx_firmware())) {
        Cartridge::ForceConfiguration config;
        config.type = Cartridge::MSX_LINEAR;
        emu_reset(config, true);
    }
    if (emu_is_empty()) {
        if (!machine_start_error.empty()) DiagWarn("app", "%s", machine_start_error.c_str());
        return;
    }

    // Scrubbing the Recorder is an inspection operation. Continuing must
    // return to the live edge and append there; silently turning the inspected
    // historical frame into a new branch would destroy all newer evidence.
    // Keep rewind_push()'s branch guard as a safety net for low-level callers
    // that bypass the normal Continue path.
    if (rewind_get_seek_position() > 0)
    {
        if (!rewind_seek(0))
        {
            DiagWarn("recorder", "continue live could not restore the newest frame");
            return;
        }
    }

    debugging = debug_step = false;
    debug_line_pending = false;
    debug_frames_pending = 0;
    gearsf7000->Pause(false);
}

void emu_debug_next_frame(void)
{
    emu_debug_step_frames(1);
}

void emu_debug_step_frames(int frames)
{
    if (frames < 1)
        frames = 1;

    // Transitions only, never per frame: a bulk step of a thousand would
    // otherwise write a thousand lines and push everything else out of the
    // rotation.
    DiagInfo("debug", "bulk step armed: %d frames (generation %llu, %d were "
                      "still pending)",
             frames, machine_generation, debug_frames_pending);

    debugging = true;
    debug_step = false;
    debug_frames_pending = frames;
    gearsf7000->Pause(false);
}

void emu_load_bios(const char* file_path)
{
    gearsf7000->GetMemory()->LoadBios(file_path);
}


bool emu_load_disc(const char* file_path, bool* isReadOnly)
{
    return gearsf7000->DiskChange(0, file_path, isReadOnly) != 0;
}

void emu_eject_disc()
{
    gearsf7000->DiskEject(0);
}

void emu_writeprotect_disc(bool writeProtected)
{
    gearsf7000->DiskWriteProtect(0, writeProtected);
}



bool emu_load_cassette(const char* file_path, bool* isReadOnly)
{
    const bool loaded = gearsf7000->CassetteChange(file_path, isReadOnly) != 0;
    if (loaded) rewind_reset(); // Snapshots reference the currently mounted media.
    return loaded;
}

void emu_eject_cassette()
{
    gearsf7000->CassetteEject();
    rewind_reset();
}

void emu_writeprotect_cassette(bool writeProtected)
{
    gearsf7000->CassetteWriteProtect(writeProtected);
}

void emu_cassette_play()
{
    gearsf7000->CassettePlay();
}

void emu_cassette_stop()
{
    gearsf7000->CassetteStop();
}

void emu_cassette_rewind()
{
    gearsf7000->CassetteRewind();
}

#ifdef SPRITE_EXPANDER
void emu_video_no_sprite_limit(bool enabled)
{
    gearsf7000->SetVideoOption(GC_VideoOption::UnlimitedSprites, enabled);
}
#endif

void emu_set_overscan(int overscan)
{
    if (overscan < GC_VideoBorder::Disabled ||
        overscan > GC_VideoBorder::Full284)
        overscan = static_cast<int>(GC_VideoBorder::Disabled);
    gearsf7000->SetVideoOption(GC_VideoOption::Border, overscan);

}

void emu_save_screenshot(const char* file_path)
{
    if (!gearsf7000->IsMachineReady())
        return;

    const GC_VideoFrameView frame = emu_get_video_frame_view();
    if (!frame.IsValid() || frame.pixel_format != GC_PIXEL_RGB888)
        return;
    const GC_VideoFrameDescriptor& descriptor = frame.descriptor;
    const GC_VideoRect visible = descriptor.visible_area;
    const u8* visiblePixels = frame.pixels +
        static_cast<size_t>(visible.y) * frame.row_stride_bytes +
        static_cast<size_t>(visible.x) * 3u;

    Log("Saving screenshot to %s", file_path);

    stbi_write_png(file_path, visible.width, visible.height, 3,
        visiblePixels, frame.row_stride_bytes);

    Log("Screenshot saved!");
}

void emu_save_debug_png(const char* file_path, const u8* rgb_data, int width, int height, int row_stride)
{
    Log("Saving debug PNG to %s", file_path);
    stbi_write_png(file_path, width, height, 3, rgb_data, row_stride);
    Log("Debug PNG saved!");
}

static void save_ram(void)
{
#ifdef DEBUG_GEARSF7000
    emu_dissasemble_rom();
#endif

    if ((emu_savefiles_dir_option == 0) && (strcmp(emu_savefiles_path, "")))
        gearsf7000->SaveRam(emu_savefiles_path);
    else
        gearsf7000->SaveRam();
}

static void load_ram(void)
{
    if ((emu_savefiles_dir_option == 0) && (strcmp(emu_savefiles_path, "")))
        gearsf7000->LoadRam(emu_savefiles_path);
    else
        gearsf7000->LoadRam();
}

const char* emu_get_mapper_name(Cartridge::CartridgeTypes type)
{
    switch (type)
    {
        case Cartridge::SG1000_1K:
            return "SG-1000 standard (console RAM)";
        case Cartridge::SG1000_16K:
            return "SG-1000 (16KB)";
        case Cartridge::SC3000_2K:
            return "SC-3000 standard (console RAM at $8000)";
        case Cartridge::SC3000_32K:
            return "SC-3000 (32KB)";
        case Cartridge::SF7000IPL:
            return "SF-7000 (64KB)";
        case Cartridge::SC3000_ASC16L:
            return "ASCII 16 Light (32KB)";
        case Cartridge::SC3000_ASC16LS:
            return "Internal";
        case Cartridge::SC3000_16K_AT_8000:
            return "SC-3000 (16KB cartridge + console RAM)";
        case Cartridge::SC3000_FLAT64K:
            return "Flat 64KB RAM";
        case Cartridge::SG1000_DAHJEE_TYPE_A:
            return "SG-1000 Dahjee Type A (8KB external + console RAM)";
        case Cartridge::SG1000_DAHJEE_TYPE_B:
            return "SG-1000 Dahjee Type B (8KB adapter)";
        case Cartridge::SC3000_SURVIVORS_MULTICART:
            return "SC-3000 Survivors Multicart (2MB, 32KB pages)";
        case Cartridge::SC3000_SURVIVORS_MEGACART:
            return "SC-3000 Survivors Megacart (4MB, 32KB pages)";
        case Cartridge::SG1000_OTHELLO_2K:
            return "SG-1000 Othello (2KB cartridge RAM)";
        case Cartridge::SG1000_CASTLE_8K:
            return "SG-1000 The Castle (8KB cartridge + console RAM)";
        case Cartridge::SG1000_SEGA_315_5208:
            return "SG-1000 Sega 315-5208 (console RAM)";
        case Cartridge::SG1000_SEGA_315_5208_DAHJEE_B:
            return "SG-1000 Sega 315-5208 + Dahjee B (8KB adapter)";
        case Cartridge::SMS_SEGA_MAPPER:
            return "Master System Sega mapper";
        case Cartridge::SORD_M5_LINEAR:
            return "Sord M5 standard cartridge (up to 20 KB)";
        case Cartridge::MSX_LINEAR:
            return "MSX linear cartridge (8/16/32KB)";
        case Cartridge::COLECOVISION:
            return "ColecoVision standard cartridge (up to 32KB at $8000)";
        case Cartridge::COLECO_MEGACART:
            return "Coleco MegaCart (16KB banks)";
        case Cartridge::CartridgeNotSupported:
            return "Not Supported";
        default:
            return "Undefined";
        }
}

static void init_debug(void)
{
    const int rom_inspector_pixels = ROM_INSPECTOR_TEXTURE_SIZE * ROM_INSPECTOR_TEXTURE_SIZE;
    emu_debug_rom_inspector_buffer = new u8[rom_inspector_pixels * 3];
    for (int i = 0; i < (rom_inspector_pixels * 3); i++)
        emu_debug_rom_inspector_buffer[i] = 0;
}

static void destroy_debug(void)
{
    SafeDeleteArray(emu_debug_rom_inspector_buffer);

}

config_VideoOutput emu_get_effective_video_output(bool debug)
{
    return ResolveVideoOutput(config_video_output_for_mode(debug), gearsf7000->GetActiveVideoOptionCode(),
        debug && !config_debug.video_as_main_settings, emu_get_video_frame_descriptor());
}
