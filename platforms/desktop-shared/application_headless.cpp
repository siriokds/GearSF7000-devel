/*
 * GearSF7000 - headless application
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "application_headless.h"

#include <csignal>
#include <SDL3/SDL.h>

#include "config.h"
#include "../../src/build_info.h"
#include "emu.h"
#include "rewind.h"
#include "DiagnosticLog.h"
#include "GearSF7000Core.h"
#include "application.h"

static volatile sig_atomic_t headless_running = 1;

static void headless_signal_handler(int)
{
    headless_running = 0;
}

int application_headless_init(const char* rom_file, const char* symbol_file,
                              bool enable_audio)
{
    config_init();
    config_read();

    if (DiagnosticLogOpen(config_root_path, "gearsf7000.log"))
    {
        DiagnosticLogSetLevel(static_cast<DiagLevel>(config_debug.log_level));
        DiagnosticLogSetMaxBytes(
            static_cast<std::size_t>(config_debug.log_max_mb) * 1024u * 1024u);
        DiagInfo("app", "GearSF7000 %s starting headless", build_info_full());
        DiagInfo("app", "log at %s", DiagnosticLogPath());
    }

    // No video, no gamepad, no window. Audio only if it was asked for, and the
    // timing functions need no subsystem at all.
    const SDL_InitFlags flags = enable_audio ? SDL_INIT_AUDIO : SDL_InitFlags(0);
    if (flags != 0 && !SDL_Init(flags))
    {
        DiagError("app", "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    emu_init(enable_audio);

    strcpy(emu_savefiles_path, config_emulator.savefiles_path.c_str());
    strcpy(emu_savestates_path, config_emulator.savestates_path.c_str());
    emu_savefiles_dir_option = config_emulator.savefiles_dir_option;
    emu_savestates_dir_option = config_emulator.savestates_dir_option;

    // The same machine-level setup the windowed front end applies. One
    // implementation, called by both, so the two cannot start different
    // machines from the same configuration file.
    emu_apply_startup_config();
    application_apply_startup_media();

    if (rom_file && rom_file[0])
    {
        DiagInfo("app", "loading %s", rom_file);
        if (!application_load_rom(rom_file))
            DiagError("app", "could not load %s", rom_file);
    }

    (void)symbol_file;

    signal(SIGINT, headless_signal_handler);
    signal(SIGTERM, headless_signal_handler);

    return 0;
}

void application_headless_mainloop(void)
{
    DiagInfo("app", "headless loop running");

    // Paced from the machine's own frame rate, not from a round 16.666 or
    // 20.0 ms: the TMS9918 makes 59.9227 frames a second NTSC and 50.1590 PAL,
    // and a tool that exists to be believed about timing should not drift by
    // 0.13% just to use a rounder number.
    //
    // Fast forward still applies, and it is the way to run an analysis faster
    // than the machine: there is no presentation here to pace against, so the
    // only limit is how fast the frames can be emulated.
    // Paced against a moving deadline rather than by sleeping a computed
    // amount each time round. Sleeping "the remainder" looks equivalent and is
    // not: SDL_Delay only guarantees *at least* the time asked for, so every
    // oversleep is lost for good and the error accumulates. Measured that way
    // the machine ran at 0.90x. A deadline that advances by exactly one frame
    // period absorbs an overslept frame in the next one, so the average comes
    // out right even though no single frame does.
    const Uint64 frequency = SDL_GetPerformanceFrequency();
    Uint64 deadline = SDL_GetPerformanceCounter();

    while (headless_running)
    {
        emu_update();

#if GEARSF7000_ENABLE_MCP
        if (!emu_mcp_is_running())
        {
            DiagInfo("app", "MCP server stopped, leaving headless loop");
            break;
        }
#endif

        if (emu_is_execution_stopped() || emu_is_empty())
        {
            // No frames are being produced, so there is nothing to pace. The
            // millisecond keeps a paused session from spinning a core while a
            // client is thinking, and the deadline is reset because the time
            // spent stopped is not a debt to catch up on.
            SDL_Delay(1);
            deadline = SDL_GetPerformanceCounter();
            continue;
        }

        double rate = emu_get_core()->GetEffectiveFrameRate();

        if (emu_get_fast_forward())
        {
            const double multiplier = emu_get_fast_forward_multiplier();
            if (multiplier <= 0.0)
            {
                // Unlimited: as fast as the frames can be made. Without a
                // window there is no presentation to pace against, so this is
                // the way to run an analysis faster than the machine.
                deadline = SDL_GetPerformanceCounter();
                continue;
            }
            // A chosen speed is still a speed, and the machine should run at
            // it rather than at whatever the host happens to manage.
            rate *= multiplier;
        }

        const Uint64 period =
            rate > 0.0 ? static_cast<Uint64>(frequency / rate) : frequency / 60;
        deadline += period;

        const Uint64 now = SDL_GetPerformanceCounter();
        if (now < deadline)
        {
            const double waitMs =
                static_cast<double>(deadline - now) * 1000.0 / frequency;
            if (waitMs > 1.5)
                SDL_Delay(static_cast<Uint32>(waitMs - 1.0));
            // The last millisecond is spun rather than slept: the scheduler
            // cannot be asked for less than about that, and overshooting it is
            // what the deadline exists to avoid.
            while (SDL_GetPerformanceCounter() < deadline)
                ;
        }
        else if (now - deadline > period * 8)
        {
            // Far behind - the host stalled, or the session was suspended.
            // Catching up frame by frame would run the machine at speed for as
            // long as it was away, so the debt is written off instead.
            deadline = now;
        }
    }
}

void application_headless_destroy(void)
{
    DiagInfo("app", "headless shutting down");
    config_write();
    config_destroy();
    emu_destroy();
    SDL_Quit();
}
