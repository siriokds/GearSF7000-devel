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

#include "../../src/gearsf7000.h"
#include "../../src/sk1100.h"
#if GEARSF7000_ENABLE_AY
// For SetAyPortBase(): the AY window is decoded in the I/O port layer, which
// gearsf7000.h does not pull in.
#include "../../src/MachineIOPorts.h"
#endif
#include "../audio-shared/sound_queue.h"
#include "config.h"
#include "renderer.h"
#include "crt_signal_dump.h"
#include "../../src/TMS9918RasterTiming.h"
#include "../../src/Z80Disassembler.h"
#include "../../src/BasicProgramLoader.h"

#include "mcp/mcp_config.h"
#if GEARSF7000_ENABLE_MCP
#include "mcp/mcp_manager.h"
#endif

#define EMU_IMPORT
#include "emu.h"
#include "rewind.h"

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
static size_t emu_frame_buffer_capacity = 0;
#if GEARSF7000_ENABLE_MCP
static McpManager* mcp_manager = NULL;
#endif

u16* debug_background_buffer;
u16* debug_tile_buffer;
u16* debug_sprite_buffers[64];

static void save_ram(void);
static void load_ram(void);
static const char* get_mapper(Cartridge::CartridgeTypes type);
static void init_debug(void);
static void destroy_debug(void);
static void update_audio_pause_state(void);
static void update_debug(void);
static void update_debug_background_buffer(void);
static void update_debug_tile_buffer(void);
static void update_debug_sprite_buffers(void);
static void ensure_video_frame_buffer_capacity(void);

void emu_init(bool enable_audio)
{
#if GEARSF7000_ENABLE_DEBUG_TOOLS
    init_debug();
#endif

    gearsf7000 = new GearSF7000Core();
    gearsf7000->Init();
    rewind_init();
    gearsf7000->SetKeyboardMode(config_emulator.keyboard_mode);
    ensure_video_frame_buffer_capacity();

#if GEARSF7000_ENABLE_AY
    // Stage the persisted expansion settings here, before anything resets the
    // machine. Doing it later - from gui_init(), which runs after emu_init() -
    // meant the first reset had already happened, so a card configured as
    // present stayed absent until the user reset by hand.
    emu_set_ay_config(config_audio.ay_enable, config_audio.ay_chip,
                      config_audio.ay_port_base);
#endif

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
    emu_debug_disable_breakpoints_vram = false;
    emu_debug_tile_palette = 0;
    emu_savefiles_dir_option = 0;
    emu_savestates_dir_option = 0;
    emu_savefiles_path[0] = 0;
    emu_savestates_path[0] = 0;
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
    SafeDeleteArray(emu_frame_buffer);
    emu_frame_buffer_capacity = 0;
#if GEARSF7000_ENABLE_DEBUG_TOOLS
    destroy_debug();
#endif
}

bool emu_load_rom(const char* file_path, Cartridge::ForceConfiguration config, bool pauseAtResetVector)
{
    save_ram();
    // A normal cartridge load disconnects the external SF-7000 I/O cartridge
    // mapping, just as physically removing that external configuration would.
    gearsf7000->GetMemory()->EnableSF7000(false);
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
        debug_frames_pending = 0;

        // gearsf7000->Reset() already left the machine paused at the reset
        // vector when requested; emu_debug_continue() would resume it.
        if (!pauseAtResetVector)
            emu_debug_continue();
    }
    return loaded;
}

// Runs one frame and leaves the picture in emu_frame_buffer, without
// touching the debugger's run state. The recorder uses it so that a seek
// updates the screen: a snapshot holds the machine, not an image, and the
// only faithful way to see a frame - raster splits included - is to let the
// beam draw it.
void emu_render_current_frame(void)
{
    if (emu_is_empty())
        return;

    ensure_video_frame_buffer_capacity();

    // RunToVBlank does nothing while the core is paused, and a seek is always
    // made from a paused machine - so without lifting the pause here the
    // screen would keep showing whatever was drawn before the seek. The pause
    // is put back immediately: the machine advances by the one frame it takes
    // to paint the picture, and no further.
    const bool was_paused = gearsf7000->IsPaused();
    if (was_paused)
        gearsf7000->Pause(false);

    int sampleCount = 0;
    gearsf7000->RunToVBlank(emu_frame_buffer, audio_buffer, &sampleCount,
                            false, false);

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

    ensure_video_frame_buffer_capacity();

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
        const GC_RunResult result = gearsf7000->RunToVBlank(
            emu_frame_buffer, audio_buffer, &sampleCount, false, true);

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

    if (!config_emulator.bios_path.empty())
    {
        DiagInfo("app", "BIOS: %s", config_emulator.bios_path.c_str());
        emu_load_bios(config_emulator.bios_path.c_str());
    }
    else
    {
        DiagWarn("app", "no BIOS configured; a machine that needs one will "
                        "boot to the NO BIOS screen");
    }

    const config_VideoOutput& startup_video = config_debug.debug
        ? config_debug.video : static_cast<const config_VideoOutput&>(config_video);
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
    if (config_video.palette == 3)
        emu_palette(config_video.color);
    else
        emu_predefined_palette(config_video.palette);

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
// Serial of the frame currently in emu_frame_buffer. Advances only when the
// beam actually drew one, so a consumer can tell a fresh frame from the same
// one being presented again - which the scheduler does whenever the display
// runs faster than the machine.
unsigned long long emu_get_frame_serial(void)
{
    if (!gearsf7000 || !gearsf7000->GetVideo())
        return 0;
    return gearsf7000->GetVideo()->GetFrameRenderDiagnostics().frameSerial;
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
    constexpr double kMasterClocksPerLine =
        TMS9918RasterTiming::DotsPerLine * 2.0;
    constexpr double kFallbackNtsc = 59.9226;

    if (!gearsf7000 || !gearsf7000->GetVideo())
        return kFallbackNtsc;

    const double vdpMasterHz =
        static_cast<double>(gearsf7000->GetClockDomains().GetRates().vdpMasterHz);
    if (vdpMasterHz <= 0.0)
        return kFallbackNtsc;

    const double lines = gearsf7000->GetVideo()->IsPAL() ? GC_LINES_PER_FRAME_PAL
                                                         : GC_LINES_PER_FRAME_NTSC;

    return vdpMasterHz / (kMasterClocksPerLine * lines);
}

// Split out of emu_update so the scheduler can keep servicing the host on
// iterations where the machine owes no frame. Tools have to answer while the
// emulator is paused or waiting for its next frame - a debugger that stops
// responding whenever the machine is stopped is not a debugger - so this
// half runs every iteration of the main loop, unconditionally.
void emu_pump_host(void)
{
#if GEARSF7000_ENABLE_MCP
    if (mcp_manager)
        mcp_manager->PumpCommands();
#endif
    update_audio_pause_state();
}

// The other half: advance the machine by one frame. Call it only when a
// frame is actually due; calling it in a loop is what fast forward does.
void emu_run_frame(void)
{
    if (!emu_is_empty())
    {
        ensure_video_frame_buffer_capacity();
        int sampleCount = 0;

        // These GUI toggles now filter only their corresponding legacy lists;
        // they cannot silence unrelated types or common/MCP DebugEvent rules.
        gearsf7000->GetMemory()->SetLegacyCpuBreakpointsEnabled(!emu_debug_disable_breakpoints_cpu);
        gearsf7000->GetMemory()->SetLegacyMemoryBreakpointsEnabled(!emu_debug_disable_breakpoints_mem);
        gearsf7000->GetMemory()->SetLegacyVRAMBreakpointsEnabled(!emu_debug_disable_breakpoints_vram);

        if (!debugging || debug_step || debug_line_pending || debug_frames_pending > 0)
        {
            // Per-category legacy filters live in their own hooks. Keep this
            // path active for MCP/common rules and Run To Cursor.
            bool breakpoints = true;

            const GC_RunResult runResult = gearsf7000->RunToVBlank(
                emu_frame_buffer, audio_buffer, &sampleCount,
                debug_step, breakpoints, debug_line_pending);
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
            if (mcp_manager && mcp_manager->GetAdapter() && mcp_manager->GetAdapter()->WatchTick())
            {
                debugging = true;
                debug_frames_pending = 0;
            }
#endif
        }

#if GEARSF7000_ENABLE_DEBUG_TOOLS
        update_debug();
#endif
        // RunToVBlank may itself enter a pause/breakpoint state.
        update_audio_pause_state();
        crt_signal_dump_tick();

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
    return SK1100::CanTypeChar(character);
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
}

bool emu_get_keyboard_mode(void)
{
    return config_emulator.keyboard_mode;
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


void emu_joy_pressed(GC_Controllers controller, GC_Keys key)
{
    gearsf7000->JoystickPressed(controller, key);
}

void emu_joy_released(GC_Controllers controller, GC_Keys key)
{
    gearsf7000->JoystickReleased(controller, key);
}

bool emu_keyboard_key(const char* label, bool pressed)
{
    return gearsf7000->KeyboardKey(label, pressed);
}

void emu_keyboard_matrix_key(int row, int mask, bool pressed)
{
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
    gearsf7000->StartSF7000(pauseAtResetVector);
    load_ram();
    if (!pauseAtResetVector)
        emu_debug_continue();
}

void emu_reset(Cartridge::ForceConfiguration config, bool pauseAtResetVector)
{
    save_ram();
    gearsf7000->ResetROM(&config, pauseAtResetVector);
    ++machine_generation;
    DiagInfo("media", "machine reset, generation %llu (paused=%d)",
             machine_generation, pauseAtResetVector ? 1 : 0);
    load_ram();
    // Everything before the reset leads to a machine that no longer exists.
    rewind_reset();
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

    // Nominal regional clock, not the live one: SetTargetFrameRate can have
    // aligned the machine to the host display, and a VGM player replays
    // writes on the crystal a real machine would carry, not on whatever this
    // session's monitor happened to dictate.
    const bool isPAL = gearsf7000->GetVideo()->IsPAL();
    const int clockRate = isPAL ? GC_MASTER_CLOCK_PAL : GC_MASTER_CLOCK_NTSC;

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

void emu_palette(GC_Color* palette)
{
    gearsf7000->GetVideo()->SetCustomPalette(palette);
}

void emu_predefined_palette(int palette)
{
    gearsf7000->GetVideo()->SetPredefinedPalette(palette);
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
        gearsf7000->LoadRam(file_path, true);
    }
}

bool emu_save_state_slot(int index)
{
    if (emu_is_empty())
        return false;

    if ((emu_savestates_dir_option == 0) && (strcmp(emu_savestates_path, "")))
        return gearsf7000->SaveState(emu_savestates_path, index);

    return gearsf7000->SaveState(index);
}

bool emu_load_state_slot(int index)
{
    if (emu_is_empty())
        return false;

    const bool loaded =
        ((emu_savestates_dir_option == 0) && (strcmp(emu_savestates_path, "")))
            ? gearsf7000->LoadState(emu_savestates_path, index)
            : gearsf7000->LoadState(index);

    if (loaded)
        rewind_abandon_history();

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
        rewind_abandon_history();

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

void emu_clear_video_buffer(void)
{
    if (emu_frame_buffer && emu_frame_buffer_capacity > 0)
        memset(emu_frame_buffer, 0, emu_frame_buffer_capacity);
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

int emu_get_screenshot_png(unsigned char** out_buffer)
{
    // Same readiness condition as GearSF7000Core::IsMachineReady(): SF-7000
    // IPL+RAM alone boot a complete machine with no SC-3000 cartridge
    // inserted at all, and that case has a frame buffer to capture too.
    if (!gearsf7000->IsMachineReady())
        return 0;

    // emu_frame_buffer is filled by RunToVBlank every frame regardless of
    // whether a GUI is presenting it, so this works the same headless.
    GC_RuntimeInfo runtime;
    emu_get_runtime(runtime);

    const GC_Color_Format format = gearsf7000->GetPixelFormat();
    const int channels =
        (format == GC_PIXEL_RGB888 || format == GC_PIXEL_BGR888) ? 3 : 4;
    const int stride = runtime.screen_width * channels;
    int len = 0;

    *out_buffer = stbi_write_png_to_mem(emu_frame_buffer, stride,
        runtime.screen_width, runtime.screen_height, channels, &len);

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
    gearsf7000->GetVideo()->AttachCrtSignalSink(crt_signal_dump_sink);
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
    gearsf7000->GetVideo()->AttachCrtSignalSink(NULL);
    SafeDelete(crt_signal_dump_sink);
    Log("CRT signal dump written to %s", crt_signal_dump_path.c_str());
}

void emu_set_full_raster_debug_enabled(bool enabled)
{
    if (!gearsf7000)
        return;

    gearsf7000->GetVideo()->SetFullRasterDebugEnabled(enabled);

    // Same reason as emu_set_overscan: this changes the output geometry
    // without advancing the machine, so repack the current frame now.
    if (gearsf7000->IsMachineReady())
    {
        ensure_video_frame_buffer_capacity();
        gearsf7000->RenderCurrentFrame(emu_frame_buffer);
    }
}

int emu_get_full_raster_debug_png(unsigned char** out_buffer)
{
    if (!gearsf7000 || !gearsf7000->IsMachineReady())
        return 0;

    Video* video = gearsf7000->GetVideo();
    if (!video->IsFullRasterDebugEnabled())
        return 0;

    int width = 0, height = 0;
    const u8* buffer = video->GetFullRasterDebugBuffer(width, height);
    if (!buffer || width <= 0 || height <= 0)
        return 0;

    int len = 0;
    *out_buffer = stbi_write_png_to_mem(buffer, width * 3, width, height, 3, &len);
    return len;
}

static void ensure_video_frame_buffer_capacity(void)
{
    const GC_VideoFrameDescriptor descriptor = emu_get_video_frame_descriptor();
    if (!descriptor.IsValid())
        return;
    const size_t required = static_cast<size_t>(descriptor.buffer_width) *
        static_cast<size_t>(descriptor.buffer_height) * 3u;
    if (required == 0 || required <= emu_frame_buffer_capacity)
        return;

    u8* replacement = new u8[required]();
    SafeDeleteArray(emu_frame_buffer);
    emu_frame_buffer = replacement;
    emu_frame_buffer_capacity = required;
}

void emu_get_info(char* info)
{
    if (!emu_is_empty())
    {
        Cartridge* cart = gearsf7000->GetCartridge();
        GC_RuntimeInfo runtime;
        gearsf7000->GetRuntimeInfo(runtime);

        const char* filename = cart->GetFileName();
        const char* pal = cart->IsPAL() ? "PAL" : "NTSC";
        const char* checksum = cart->IsValidROM() ? "VALID" : "FAILED";
        int rom_banks = cart->GetROMBankCount();
        const char* mapper = get_mapper(cart->GetType());

        sprintf(info, "File Name: %s\nMapper: %s\nRefresh Rate: %s\nCartridge Header: %s\nROM Banks: %d\nScreen Resolution: %dx%d", filename, mapper, pal, checksum, rom_banks, runtime.screen_width, runtime.screen_height);
    }
    else
    {
        sprintf(info, "There is no ROM loaded!");
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
    u8 opcode = memory->Read(opcodeAddress);
    for (int prefixes = 0;
         prefixes < 16 && (opcode == 0xdd || opcode == 0xfd);
         ++prefixes)
    {
        opcode = memory->Read(++opcodeAddress);
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
    // Scrubbing the Recorder is an inspection operation. Continuing must
    // return to the live edge and append there; silently turning the inspected
    // historical frame into a new branch would destroy all newer evidence.
    // Keep rewind_push()'s branch guard as a safety net for low-level callers
    // that bypass the normal Continue path.
    if (rewind_get_seek_position() > 0 && !rewind_seek(0))
    {
        DiagWarn("recorder", "continue live could not restore the newest frame");
        return;
    }

    debugging = debug_step = false;
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
    return gearsf7000->CassetteChange(file_path, isReadOnly) != 0;
}

void emu_eject_cassette()
{
    gearsf7000->CassetteEject();
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
    gearsf7000->GetVideo()->SetNoSpriteLimit(enabled);
}
#endif

void emu_set_overscan(int overscan)
{
    switch (overscan)
    {
        case 0:
            gearsf7000->GetVideo()->SetOverscan(Video::OverscanDisabled);
            break;
        case 1:
            gearsf7000->GetVideo()->SetOverscan(Video::OverscanTopBottom);
            break;
        case 2:
            gearsf7000->GetVideo()->SetOverscan(Video::OverscanFull272);
            break;
        case 3:
            gearsf7000->GetVideo()->SetOverscan(Video::OverscanFull284);
            break;
        //case 4:
        //    gearsf7000->GetVideo()->SetOverscan(Video::OverscanFull320);
        //    break;
        default:
            gearsf7000->GetVideo()->SetOverscan(Video::OverscanDisabled);
    }

    // Changing the output geometry does not advance the emulated machine.
    // Repack the already-rendered VDP image immediately so a paused machine,
    // startup configuration, or debugger view never exposes pixels laid out
    // using the previous descriptor.
    ensure_video_frame_buffer_capacity();
    gearsf7000->RenderCurrentFrame(emu_frame_buffer);
}

void emu_save_screenshot(const char* file_path)
{
    if (!gearsf7000->IsMachineReady())
        return;

    GC_RuntimeInfo runtime;
    emu_get_runtime(runtime);

    Log("Saving screenshot to %s", file_path);

    stbi_write_png(file_path, runtime.screen_width, runtime.screen_height, 3, emu_frame_buffer, runtime.screen_width * 3);

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

static const char* get_mapper(Cartridge::CartridgeTypes type)
{
    switch (type)
    {
        case Cartridge::SG1000_1K:
            return "SG-1000 (1KB)";
        case Cartridge::SG1000_16K:
            return "SG-1000 (16KB)";
        case Cartridge::SC3000_2K:
            return "SC-3000 (2KB)";
        case Cartridge::SC3000_32K:
            return "SC-3000 (32KB)";
        case Cartridge::SF7000IPL:
            return "SF-7000 (64KB)";
        case Cartridge::SC3000_ASC16L:
            return "ASCII 16 Light (32KB)";
        case Cartridge::CartridgeNotSupported:
            return "Not Supported";
        default:
            return "Undefined";
        }
}

static void init_debug(void)
{
    emu_debug_background_buffer = new u8[256 * 256 * 3];
    emu_debug_tile_buffer = new u8[32 * 32 * 64 * 3];
    debug_background_buffer = new u16[256 * 256];    
    debug_tile_buffer = new u16[32 * 32 * 64];

    for (int i=0,j=0; i < (32 * 32 * 64); i++,j+=3)
    {
        debug_tile_buffer[i] = 0;
        emu_debug_tile_buffer[j] = 0;
        emu_debug_tile_buffer[j+1] = 0;
        emu_debug_tile_buffer[j+2] = 0;
    }

    for (int s = 0; s < 64; s++)
    {
        emu_debug_sprite_buffers[s] = new u8[16 * 16 * 3];
        debug_sprite_buffers[s] = new u16[16 * 16];

        for (int i=0,j=0; i < (16 * 16); i++,j+=3)
        {
            debug_sprite_buffers[s][i] = 0;
            emu_debug_sprite_buffers[s][j] = 0;
            emu_debug_sprite_buffers[s][j+1] = 0;
            emu_debug_sprite_buffers[s][j+2] = 0;
        }
    }

    for (int i=0,j=0; i < (256 * 256); i++,j+=3)
    {
        debug_background_buffer[i] = 0;
        emu_debug_background_buffer[j] = 0;
        emu_debug_background_buffer[j+1] = 0;
        emu_debug_background_buffer[j+2] = 0;
    }

    const int rom_inspector_pixels = ROM_INSPECTOR_TEXTURE_SIZE * ROM_INSPECTOR_TEXTURE_SIZE;
    emu_debug_rom_inspector_buffer = new u8[rom_inspector_pixels * 3];
    for (int i = 0; i < (rom_inspector_pixels * 3); i++)
        emu_debug_rom_inspector_buffer[i] = 0;
}

static void destroy_debug(void)
{
    SafeDeleteArray(emu_debug_background_buffer);
    SafeDeleteArray(emu_debug_tile_buffer);
    SafeDeleteArray(debug_background_buffer);
    SafeDeleteArray(debug_tile_buffer);
    SafeDeleteArray(emu_debug_rom_inspector_buffer);

    for (int s = 0; s < 64; s++)
    {
        SafeDeleteArray(emu_debug_sprite_buffers[s]);
        SafeDeleteArray(debug_sprite_buffers[s]);
    }
}

static void update_debug(void)
{

    update_debug_background_buffer();
    update_debug_tile_buffer();
    update_debug_sprite_buffers();

    Video* video = gearsf7000->GetVideo();

    video->Render24bit(debug_background_buffer, emu_debug_background_buffer, GC_PIXEL_RGB888, 256 * 256);
    video->Render24bit(debug_tile_buffer, emu_debug_tile_buffer, GC_PIXEL_RGB888, 32 * 32 * 64);

    for (int s = 0; s < 64; s++)
    {
        video->Render24bit(debug_sprite_buffers[s], emu_debug_sprite_buffers[s], GC_PIXEL_RGB888, 16 * 16);
    }
}

static void update_debug_background_buffer(void)
{
    Video* video = gearsf7000->GetVideo();
    u8* vram = video->GetVRAM();
    u8* regs = video->GetRegisters();
    int mode = video->GetMode();

    int name_table_addr = regs[2] << 10;
    int color_table_addr = regs[3] << 6;
    int pattern_table_addr = regs[4] << 11;
    int region_mask = ((regs[4] & 0x03) << 8) | 0xFF;
    int color_mask = ((regs[3] & 0x7F) << 3) | 0x07;
    int backdrop_color = regs[7] & 0x0F;
    backdrop_color = (backdrop_color > 0) ? backdrop_color : 1;
    int region = 0;

    switch (mode)
    {
        case 1:
        {
            int fg_color = (regs[7] >> 4) & 0x0F;
            int bg_color = backdrop_color;
            fg_color = (fg_color > 0) ? fg_color : backdrop_color;

            for (int line = 0; line < 192; line++)
            {
                int line_offset = line * GC_RESOLUTION_WIDTH;
                int tile_y = line >> 3;
                int tile_y_offset = line & 7;

                for (int tile_x = 0; tile_x < 40; tile_x++)
                {
                    int tile_number = (tile_y * 40) + tile_x;
                    int name_tile_addr = name_table_addr + tile_number;
                    int name_tile = vram[name_tile_addr];
                    u8 pattern_line = vram[pattern_table_addr + (name_tile << 3) + tile_y_offset];

                    int screen_offset = line_offset + (tile_x * 6);

                    for (int tile_pixel = 0; tile_pixel < 6; tile_pixel++)
                    {
                        int pixel = screen_offset + tile_pixel;
                        debug_background_buffer[pixel] = IsSetBit(pattern_line, 7 - tile_pixel) ? fg_color : bg_color;
                    }
                }
            }
            return;
        }
        case 2:
        {
            pattern_table_addr &= 0x2000;
            color_table_addr &= 0x2000;
            break;
        }
        case 4:
        {
            pattern_table_addr &= 0x2000;
            break;
        }
    }

    for (int line = 0; line < 192; line++)
    {
        int line_offset = line * GC_RESOLUTION_WIDTH;
        int tile_y = line >> 3;
        int tile_y_offset = line & 7;
        region = (tile_y & 0x18) << 5;

        for (int tile_x = 0; tile_x < 32; tile_x++)
        {
            int tile_number = (tile_y << 5) + tile_x;
            int name_tile_addr = name_table_addr + tile_number;
            int name_tile = vram[name_tile_addr];
            u8 pattern_line = 0;
            u8 color_line = 0;

            if (mode == 4)
            {
                int offset_color = pattern_table_addr + (name_tile << 3) + ((tile_y & 0x03) << 1) + (line & 0x04 ? 1 : 0);
                color_line = vram[offset_color];

                int left_color = color_line >> 4;
                int right_color = color_line & 0x0F;
                left_color = (left_color > 0) ? left_color : backdrop_color;
                right_color = (right_color > 0) ? right_color : backdrop_color;

                int screen_offset = line_offset + (tile_x << 3);

                for (int tile_pixel = 0; tile_pixel < 4; tile_pixel++)
                {
                    int pixel = screen_offset + tile_pixel;
                    debug_background_buffer[pixel] = left_color;
                }

                for (int tile_pixel = 4; tile_pixel < 8; tile_pixel++)
                {
                    int pixel = screen_offset + tile_pixel;
                    debug_background_buffer[pixel] = right_color;
                }

                continue;
            }
            else if (mode == 0)
            {
                pattern_line = vram[pattern_table_addr + (name_tile << 3) + tile_y_offset];
                color_line = vram[color_table_addr + (name_tile >> 3)];
            }
            else if (mode == 2)
            {
                name_tile += region;
                pattern_line = vram[pattern_table_addr + ((name_tile & region_mask) << 3) + tile_y_offset];
                color_line = vram[color_table_addr + ((name_tile & color_mask) << 3) + tile_y_offset];
            }

            int fg_color = color_line >> 4;
            int bg_color = color_line & 0x0F;
            fg_color = (fg_color > 0) ? fg_color : backdrop_color;
            bg_color = (bg_color > 0) ? bg_color : backdrop_color;

            int screen_offset = line_offset + (tile_x << 3);

            for (int tile_pixel = 0; tile_pixel < 8; tile_pixel++)
            {
                int pixel = screen_offset + tile_pixel;
                debug_background_buffer[pixel] = IsSetBit(pattern_line, 7 - tile_pixel) ? fg_color : bg_color;
            }
        }
    }
}

static void update_debug_tile_buffer(void)
{
    Video* video = gearsf7000->GetVideo();
    u8* vram = video->GetVRAM();
    u8* regs = video->GetRegisters();
    int mode = video->GetMode();

    int pattern_table_addr = (regs[4] & ((mode == 2) ? 0x04 : 0x07)) << 11;
    int color_table_addr = regs[3] << 6;
    if (mode == 2)
        color_table_addr &= 0x2000;

    int backdrop_color = regs[7] & 0x0F;
    backdrop_color = (backdrop_color > 0) ? backdrop_color : 1;

    // Text Mode has no per-tile color table at all - fixed fg/bg from R7,
    // same substitution rule update_debug_background_buffer already uses.
    int text_fg = (regs[7] >> 4) & 0x0F;
    int text_bg = backdrop_color;
    text_fg = (text_fg > 0) ? text_fg : backdrop_color;

    for (int y = 0; y < 256; y++)
    {
        int width_y = (y * 256);
        int tile_y = y / 8;
        int offset_y = y & 0x7;

        for (int x = 0; x < 256; x++)
        {
            int tile_x = x / 8;
            int offset_x = 7 - (x & 0x7);
            int pixel = width_y + x;

            int tile_number = (tile_y * 32) + tile_x;

            int tile_data_addr = (pattern_table_addr + (tile_number * 8) + (1 * offset_y)) & 0x3FFF;
            bool bit_set = IsSetBit(vram[tile_data_addr], offset_x);

            int fg_color = 15;
            int bg_color = 0;

            if (mode == 1)
            {
                fg_color = text_fg;
                bg_color = text_bg;
            }
            else if ((mode == 0) || (mode == 2))
            {
                // Graphics I and II both address their color table with the
                // exact same coordinates used for the pattern table above
                // (tile_number for II - one color byte per pattern row,
                // tile_number>>3 for I - one byte per 8 consecutive
                // patterns), so this grid of tiles is never ambiguous about
                // which color entry belongs to it: no name-table position
                // or "which Graphics II region" guess needed. Ported from
                // SC3K-System's update_debug_tile_buffer_sg1000_mode2/
                // update_debug_tile_buffer_sg1000 (see
                // DOCS/ROM_INSPECTOR_PLAN.md point 2).
                int tile_color_addr = (mode == 2)
                    ? (color_table_addr + (tile_number * 8) + offset_y) & 0x3FFF
                    : (color_table_addr + (tile_number >> 3)) & 0x3FFF;
                int color_line = vram[tile_color_addr];
                fg_color = color_line >> 4;
                bg_color = color_line & 0x0F;
                fg_color = (fg_color > 0) ? fg_color : backdrop_color;
                bg_color = (bg_color > 0) ? bg_color : backdrop_color;
            }
            // Multicolor (mode 4): color is embedded directly in the
            // pattern data itself, not looked up from a separate table -
            // a genuinely different decode, not just an added lookup.
            // Left as plain black/white for now, same scope boundary
            // SC3K-System itself stopped at.

            debug_tile_buffer[pixel] = bit_set ? fg_color : bg_color;
        }
    }
}

static void update_debug_sprite_buffers(void)
{
    GearSF7000Core* core = emu_get_core();
    Video* video = core->GetVideo();
    u8* regs = video->GetRegisters();
    u8* vram = video->GetVRAM();
    GC_RuntimeInfo runtime;
    emu_get_runtime(runtime);

    const bool rendered_source = config_debug.vram_sprites_source == 1;
    const bool pipeline_source = config_debug.vram_sprites_source == 2;
    const bool scanline_source = config_debug.vram_sprites_source == 3;
    const Video::RenderedSpriteDebugFrame& rendered =
        video->GetLastRenderedSpriteDebugFrame();
    int sprite_size = rendered_source && rendered.valid
        ? (rendered.large ? 16 : 8)
        : (IsSetBit(regs[1], 1) ? 16 : 8);
    u16 sprite_attribute_addr = (regs[5] & 0x7F) << 7;
    u16 sprite_pattern_addr = (regs[6] & 0x07) << 11;

    for (int s = 0; s < 32; s++)
    {
        for (int pixel = 0; pixel < 16 * 16; ++pixel)
            debug_sprite_buffers[s][pixel] = 0;

        if (rendered_source)
        {
            if (!rendered.valid)
                continue;
            for (int pixel_y = 0; pixel_y < sprite_size; ++pixel_y)
                for (int pixel_x = 0; pixel_x < sprite_size; ++pixel_x)
                {
                    const int pixel = pixel_y * 16 + pixel_x;
                    debug_sprite_buffers[s][pixel] =
                        rendered.entries[s].pixels[pixel];
                }
            continue;
        }

        if (scanline_source)
        {
            const Video::SpriteSelectionFrameHistory& history =
                video->GetSpriteSelectionFrameHistory();
            const int line = config_debug.vram_sprites_scanline;
            if (!history.valid || line < 0 || line >= history.rasterLines)
                continue;
            const Video::SpriteSelectionHistoryRow& row = history.rows[line];
            for (int lane = 0; lane < 4; ++lane)
            {
                if (row.rasterX[lane] == 0xFF)
                    continue;
                const auto scan = TMS9918VramSlotSchedule::GetSlot(
                    TMS9918VramSlotSchedule::Schedule::Graphics,
                    row.rasterX[lane]);
                if (scan.activity !=
                        TMS9918VramSlotSchedule::Activity::SpriteScanY ||
                    scan.index != s)
                    continue;
                int visible_y = (row.sat[lane][0] + 1) & 0xFF;
                if (visible_y >= 0xE0)
                    visible_y -= 0x100;
                const bool zoomed = (row.flags & 0x08) != 0;
                const int source_y = (line - visible_y) >> (zoomed ? 1 : 0);
                const int source_size = (row.flags & 0x04) != 0 ? 16 : 8;
                if (source_y < 0 || source_y >= source_size)
                    continue;
                const int colour = row.sat[lane][3] & 0x0F;
                for (int x = 0; x < source_size; ++x)
                {
                    const u8 pattern = x < 8
                        ? row.pattern[lane][0] : row.pattern[lane][1];
                    const int bit = x < 8 ? 7 - x : 15 - x;
                    if (colour != 0 && IsSetBit(pattern, bit & 7))
                        debug_sprite_buffers[s][source_y * 16 + x] = colour;
                }
            }
            continue;
        }

        if (pipeline_source)
        {
            const Video::SpritePipelineDebugSnapshot pipeline =
                video->GetSpritePipelineDebugSnapshot();
            const Video::SpritePipelineLineDebug* active = nullptr;
            for (const Video::SpritePipelineLineDebug& line : pipeline.lines)
            {
                if (!line.valid)
                    continue;
                if (pipeline.activeTargetValid &&
                    line.absoluteTargetLine == pipeline.activeAbsoluteTargetLine)
                {
                    active = &line;
                    break;
                }
                if (active == nullptr)
                    active = &line;
            }
            if (active == nullptr)
                continue;
            for (int lane = 0; lane < active->selectedCount && lane < 4;
                 ++lane)
            {
                const Video::SpritePipelineLatchDebug& entry =
                    active->selected[lane];
                if (entry.satIndex != s || !entry.yValid || !entry.colourValid ||
                    !entry.patternValid[0])
                    continue;
                int visible_y = (entry.y + 1) & 0xFF;
                if (visible_y >= 0xE0)
                    visible_y -= 0x100;
                const bool zoomed = IsSetBit(regs[1], 0);
                const int source_y = (active->targetLine - visible_y) >>
                    (zoomed ? 1 : 0);
                if (source_y < 0 || source_y >= sprite_size)
                    continue;
                const int colour = entry.colour & 0x0F;
                for (int x = 0; x < sprite_size; ++x)
                {
                    if (x >= 8 && !entry.patternValid[1])
                        continue;
                    const u8 pattern = x < 8
                        ? entry.pattern[0] : entry.pattern[1];
                    const int bit = x < 8 ? 7 - x : 15 - x;
                    if (colour != 0 && IsSetBit(pattern, bit & 7))
                        debug_sprite_buffers[s][source_y * 16 + x] = colour;
                }
            }
            continue;
        }

        int sprite_attribute_offset = sprite_attribute_addr + (s << 2);
        int sprite_color = vram[sprite_attribute_offset + 3] & 0x0F;
        int sprite_tile = vram[sprite_attribute_offset + 2];
        sprite_tile &= (sprite_size == 16) ? 0xFC : 0xFF;

        for (int pixel_y = 0; pixel_y < sprite_size; pixel_y++)
        {
            int sprite_line_addr = sprite_pattern_addr + (sprite_tile << 3) + pixel_y;

            for (int pixel_x = 0; pixel_x < 16; pixel_x++)
            {
                if ((sprite_size == 8) && (pixel_x == 8))
                    break;

                int pixel = (pixel_y * 16) + pixel_x;

                bool sprite_pixel = false;

                if (pixel_x < 8)
                    sprite_pixel = IsSetBit(vram[sprite_line_addr], 7 - pixel_x);
                else
                    sprite_pixel = IsSetBit(vram[sprite_line_addr + 16], 15 - pixel_x);

                debug_sprite_buffers[s][pixel] = sprite_pixel ? sprite_color : 0;
            }
        }
    }
}

void emu_refresh_debug_sprite_buffers(void)
{
    if (emu_is_empty())
        return;

    update_debug_sprite_buffers();
    Video* video = gearsf7000->GetVideo();
    for (int s = 0; s < 32; ++s)
        video->Render24bit(debug_sprite_buffers[s],
            emu_debug_sprite_buffers[s], GC_PIXEL_RGB888, 16 * 16);
}
