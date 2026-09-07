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

#ifndef EMU_H
#define	EMU_H

#include "../../src/gearsf7000.h"

#ifdef EMU_IMPORT
    #define EXTERN
#else
    #define EXTERN extern
#endif

EXTERN u8* emu_frame_buffer;
EXTERN u8* emu_debug_background_buffer;
EXTERN u8* emu_debug_tile_buffer;
EXTERN u8* emu_debug_sprite_buffers[64];
void emu_refresh_debug_sprite_buffers(void);
// ROM Inspector (DOCS/ROM_INSPECTOR_PLAN.md point 4): decodes an arbitrary
// external scratch buffer at a user-chosen offset/tiles-per-row, not live
// VRAM. Fixed-size square buffer (up to 64 tiles/row, 64 tile-rows visible
// at once) so the GPU texture backing it never needs recreating when the
// user changes tiles-per-row - only how much of it is actually shown does.
#define ROM_INSPECTOR_TEXTURE_SIZE 512
EXTERN u8* emu_debug_rom_inspector_buffer;

EXTERN bool emu_audio_sync;

struct EmuAudioQueueDiagnostics
{
    unsigned long long callbacks;
    unsigned long long underruns;
    unsigned long long zero_filled_samples;
    unsigned long long producer_dropped_samples;
    unsigned long long variable_size_callbacks;
    unsigned long long min_callback_interval_us;
    unsigned long long max_callback_interval_us;
    unsigned long long client_sample_rate;
    unsigned long long device_sample_rate;
    unsigned long long min_requested_samples;
    unsigned long long max_requested_samples;
    unsigned long long queued_samples;
    unsigned long long ring_capacity_samples;
};
EXTERN bool emu_debug_disable_breakpoints_cpu;
EXTERN bool emu_debug_disable_breakpoints_mem;
EXTERN bool emu_debug_disable_breakpoints_vram;
EXTERN int emu_debug_tile_palette;
EXTERN bool emu_savefiles_dir_option;
EXTERN bool emu_savestates_dir_option;
EXTERN char emu_savefiles_path[4096];
EXTERN char emu_savestates_path[4096];

// enable_audio is false for headless runs: there is nobody to hear it, an
// unattended analysis should not take over the sound device, and at nine times
// speed the samples have nowhere to go anyway.
EXTERN void emu_init(bool enable_audio = true);
// Applies the machine-level settings held in the configuration: BIOS,
// overscan, disc write protection, audio mute. Called by every front end, so
// that a windowed session and a headless one start the same machine.
EXTERN void emu_apply_startup_config(void);
EXTERN void emu_destroy(void);
EXTERN void emu_update(void);
// The two halves of emu_update, for front ends that pace presentation
// separately from emulation. See the comments on their definitions.
EXTERN double emu_get_machine_fps(void);
// Absorbs the audio device's crystal error in the resampler instead of in the
// machine's frame rate. See Audio::SetClockTrim.
EXTERN void emu_set_audio_clock_trim(double ratio);
// Frame rate the machine's crystals should be scaled to reach, or 0 for the
// real ones. Latched at the next machine reset - see
// GearSF7000Core::SetTargetFrameRate for why it is not applied live.
EXTERN unsigned long long emu_get_frame_serial(void);
EXTERN void emu_set_target_frame_rate(double fps);
// The real crystals' rate, unscaled. The decision of what to align to has to
// be made against this and not against the current rate, which already
// contains the answer.
EXTERN double emu_get_native_frame_rate(void);
EXTERN void emu_pump_host(void);
EXTERN void emu_run_frame(void);
EXTERN void emu_render_current_frame(void);
// pauseAtResetVector: leave the machine paused with PC already at the reset
// vector, having executed nothing, instead of running normally.
EXTERN bool emu_load_rom(const char* file_path, Cartridge::ForceConfiguration config, bool pauseAtResetVector = false);
EXTERN void emu_start_sf7000(bool pauseAtResetVector = false);

EXTERN bool emu_load_disc(const char* file_path, bool* isReadOnly);
EXTERN void emu_eject_disc();
EXTERN void emu_writeprotect_disc(bool);

EXTERN bool emu_load_cassette(const char* file_path, bool* isReadOnly);
EXTERN void emu_eject_cassette();
EXTERN void emu_writeprotect_cassette(bool);
EXTERN void emu_cassette_play();
EXTERN void emu_cassette_stop();
EXTERN void emu_cassette_rewind();

EXTERN void emu_enable_events(bool enabled);
EXTERN void emu_set_keyboard_mode(bool enabled);
// With the SK-1100 in keyboard mode the matrix answers the PPI; turning it
// off hands the same port back to the joystick, which is what a game in
// attract mode expects.
EXTERN bool emu_get_keyboard_mode(void);
EXTERN void emu_input_event(SDL_Event event);
EXTERN void emu_joy_pressed(GC_Controllers controller, GC_Keys key);
EXTERN void emu_joy_released(GC_Controllers controller, GC_Keys key);
// Direct press/release of a single non-printable SK-1100 keyboard matrix key
// (e.g. "Up"/"Down"/"Left"/"Right" cursor keys) that emu_keyboard_text can't
// reach. Returns false if label isn't in the keyboard matrix mapping.
EXTERN bool emu_keyboard_key(const char* label, bool pressed);
EXTERN void emu_keyboard_matrix_key(int row, int mask, bool pressed);
EXTERN void emu_pause_key_pressed();
// source names who asked for this: "panel" or "MCP". It is shown in the BASIC
// Typer while the sequence runs, so text arriving unexpectedly can be traced
// to whoever sent it without having to ask.
EXTERN bool emu_keyboard_text(const char* text, const char* source, std::string* error = 0);
// Who started the sequence being typed now. Empty when nothing is.
EXTERN const char* emu_keyboard_text_source();
EXTERN void emu_keyboard_clear_text();
// Whether the SK-1100 matrix can produce this character. emu_keyboard_text
// rejects the whole string on the first one it cannot.
EXTERN bool emu_keyboard_can_type(char character);
// Matrix polls per keystroke, and the idle hold after Return that stops BASIC
// losing the start of the next line. Applies to the next sequence queued.
EXTERN void emu_keyboard_text_timing(int press_polls, int release_polls, int newline_polls);
// 0..1 while text is being typed, 1 when idle.
EXTERN float emu_keyboard_text_progress();
EXTERN bool emu_keyboard_text_busy();
EXTERN void emu_pause(void);
EXTERN void emu_resume(void);
EXTERN bool emu_is_paused(void);
// True when the desktop debugger has halted execution (for example at a breakpoint).
EXTERN bool emu_is_debugging(void);
// User-facing execution state. A debugger stop is just as stopped as a core
// pause, even though the two mechanisms intentionally remain separate.
EXTERN bool emu_is_execution_stopped(void);
// How many frames a bulk step still has to run. Exposed so a diagnostic dump
// can say whether something was left armed across a reload.
EXTERN int emu_debug_frames_pending(void);
EXTERN bool emu_debug_step_pending(void);
// Runs the frames here and now, rather than arming a counter for the main loop
// to work through one frame per iteration. The machine ends paused. Returns
// how many frames completed, which is fewer than asked for when a breakpoint
// stopped it.
//
// The point is speed: stepping through the main loop inherits whatever paces
// presentation, so a few hundred frames take as many sixtieths of a second in
// real time. Executed here it is bounded only by how fast the machine can be
// emulated.
EXTERN int emu_debug_step_frames_now(int frames, bool* breakpoint_hit);

// Fast forward. Releases the two brakes that actually pace the machine -
// presentation and the audio queue - and lets the throttle's floor become the
// speed limit. Speed setting: 0=1.5x, 1=2x, 2=2.5x, 3=3x, 4=unlimited.
EXTERN void emu_set_fast_forward(bool enabled);
EXTERN bool emu_get_fast_forward(void);
EXTERN void emu_set_fast_forward_speed(int speed);
EXTERN int emu_get_fast_forward_speed(void);
// The speed setting as a multiplier of the machine's own rate; 0 means
// unlimited, which is a request to go as fast as the frames can be made.
EXTERN double emu_get_fast_forward_multiplier(void);
// Incremented every time the machine is rebuilt - ROM load, reset, eject. Any
// log line carrying it makes it obvious when something belonging to a previous
// machine is still acting on the current one, which is the shape of bug that
// a reload crash usually turns out to be.
EXTERN unsigned long long emu_get_machine_generation(void);
EXTERN bool emu_is_empty(void);
EXTERN bool emu_is_bios_loaded(void);
EXTERN void emu_reset(Cartridge::ForceConfiguration config, bool pauseAtResetVector = false);
EXTERN void emu_eject_rom(void);
EXTERN void emu_dissasemble_rom(void);
EXTERN void emu_audio_mute(bool mute);
EXTERN void emu_audio_reset(void);
// VGM export: the SN76489/AY-3-8910 register-write log, not a sample
// recording. Needs a cartridge loaded - see StartVgmRecording's own comment
// in Audio.h for why the clock recorded is the nominal regional one.
EXTERN bool emu_start_vgm_recording(const char* file_path);
// Returns the path written, or empty if nothing was recording.
EXTERN std::string emu_stop_vgm_recording(void);
EXTERN bool emu_is_vgm_recording(void);
#if GEARSF7000_ENABLE_AY
// Stages the SGM-style AY expansion. The port window moves at once; presence
// and chip type take effect at the next machine reset.
EXTERN void emu_set_ay_config(bool enabled, int chip, int port_base);
#endif
EXTERN void emu_audio_suspend_for_file_dialog(void);
EXTERN void emu_audio_resume_after_file_dialog(void);
EXTERN bool emu_is_audio_enabled(void);
EXTERN EmuAudioQueueDiagnostics emu_get_audio_queue_diagnostics(void);
EXTERN void emu_reset_audio_queue_diagnostics(void);
EXTERN void emu_palette(GC_Color* palette);
EXTERN void emu_predefined_palette(int palette);
EXTERN void emu_save_ram(const char* file_path);
EXTERN void emu_load_ram(const char* file_path, Cartridge::ForceConfiguration config);
// Writes a tokenised .bas image into the BASIC program area and rebuilds the
// pointers around it, the way a cassette LOAD does. Not for .basic source
// text, which BASIC has to tokenise itself - that goes through the typer or
// the cassette. Fills status with what happened, either way.
EXTERN bool emu_load_basic_program(const char* file_path, int pointer_block, std::string* status);
// The mirror: writes [TXTBGN, ARYBGN) out to file_path, which is what the
// ROM's own SAVE calls the program.
EXTERN bool emu_save_basic_program(const char* file_path, int pointer_block, std::string* status);
// Addresses where a BASIC pointer block looks consistent, for finding the
// layout of a BASIC that has none documented. Returns how many were written.
EXTERN int emu_find_basic_blocks(u16* out, int max_out);
// Report whether the state was actually written or applied, so a caller
// cannot announce a save that never happened.
EXTERN bool emu_save_state_slot(int index);
EXTERN bool emu_load_state_slot(int index);
EXTERN bool emu_save_state_file(const char* file_path, size_t* bytes_written = NULL);
EXTERN bool emu_load_state_file(const char* file_path);
EXTERN void emu_get_runtime(GC_RuntimeInfo& runtime);
EXTERN GC_VideoFrameDescriptor emu_get_video_frame_descriptor(void);
EXTERN void emu_clear_video_buffer(void);
// PNG encode of the live frame buffer, filled every frame by RunToVBlank
// regardless of whether a GUI is presenting it. Caller frees *out_buffer with
// free(). Returns 0 on failure.
EXTERN int emu_get_screenshot_png(unsigned char** out_buffer);
// Encodes RGBA pixels as a PNG in memory; the caller frees *out_buffer with
// free(). Here rather than at the call site because stb_image_write's
// to_mem entry point is only visible in the translation unit that carries
// STB_IMAGE_WRITE_IMPLEMENTATION, which is this one.
EXTERN int emu_encode_png_rgba(const u8* pixels, int width, int height,
                               unsigned char** out_buffer);
// Debug-only visualization of the full 342-dot raster (sync/blank/burst/
// border/active), see Video::SetFullRasterDebugEnabled. Off by default.
EXTERN void emu_set_full_raster_debug_enabled(bool enabled);
// Plugs a recording sink into the VDP's second video output and captures
// `lines` scanlines' worth of signal calls to file_path. Capture spans
// frames, so it is not finished when this returns - poll
// emu_crt_signal_dump_in_progress().
EXTERN bool emu_crt_signal_dump_start(const char* file_path, int lines);
EXTERN bool emu_crt_signal_dump_in_progress(void);
EXTERN void crt_signal_dump_tick(void);
// PNG encode of the full-raster debug buffer. Returns 0 if disabled, no
// machine ready, or encoding failed. Caller frees *out_buffer with free().
EXTERN int emu_get_full_raster_debug_png(unsigned char** out_buffer);
EXTERN void emu_get_info(char* info);
EXTERN GearSF7000Core* emu_get_core(void);
#if GEARSF7000_ENABLE_MCP
class DebugAdapter;
EXTERN void emu_mcp_start(void);
EXTERN void emu_mcp_start_from_command_line(void);
EXTERN void emu_mcp_stop(void);
EXTERN void emu_mcp_set_transport(int mode, int port, const char* address);
EXTERN bool emu_mcp_is_running(void);
EXTERN int emu_mcp_get_transport_mode(void);
EXTERN bool emu_mcp_started_from_command_line(void);
EXTERN bool emu_mcp_is_listening(void);
EXTERN int emu_mcp_get_port(void);
EXTERN const char* emu_mcp_get_address(void);
EXTERN void emu_mcp_pump_commands(void);
// Direct access for GUI windows (the watch/freeze monitor) that call
// DebugAdapter methods themselves instead of going through the MCP
// request queue - forward-declared here so emu.h doesn't need to pull in
// json.hpp; callers that need the full type already include
// mcp/mcp_manager.h (or mcp_debug_adapter.h) themselves.
EXTERN DebugAdapter* emu_mcp_get_adapter(void);
#endif
EXTERN void emu_debug_step_into(void);
EXTERN void emu_debug_step_over(void);
// Compatibility name retained for older front-end code. It is Step Into.
EXTERN void emu_debug_step(void);
// Runs until the VDP's current render line changes (or a breakpoint/VBlank
// intervenes), stopping right after a mid-frame raster write instead of
// running past it to the next full frame like Step Frame does.
EXTERN void emu_debug_step_line(void);
EXTERN void emu_debug_break(void);
EXTERN void emu_debug_continue(void);
EXTERN void emu_debug_next_frame(void);
EXTERN void emu_debug_step_frames(int frames);
EXTERN void emu_load_bios(const char* file_path);
EXTERN void emu_load_bios(const char* file_path);
#ifdef SPRITE_EXPANDER
EXTERN void emu_video_no_sprite_limit(bool enabled);
#endif
EXTERN void emu_set_overscan(int overscan);
EXTERN void emu_save_screenshot(const char* file_path);
// Parameterized twin of emu_save_screenshot - writes any RGB888 buffer
// (debug tile/background/sprite views) to PNG. row_stride lets the caller
// crop a sub-region out of a larger backing buffer (e.g. the visible
// cols*rows out of the debug views' fixed 256x256 texture buffers)
// without an intermediate copy.
EXTERN void emu_save_debug_png(const char* file_path, const u8* rgb_data, int width, int height, int row_stride);

#undef EMU_IMPORT
#undef EXTERN
#endif	/* EMU_H */
