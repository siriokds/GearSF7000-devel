/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026 Saverio Russo
 *
 * Frame pacing. Decides how many emulated frames are due, from the machine's
 * own frame rate and a wall clock - not from how often the host happens to
 * present a picture.
 *
 * Why this exists (DOCS/18 D2, DOCS/17): the loop used to run exactly one
 * emulated frame per iteration, and the iteration rate was whatever vertical
 * sync and the audio queue produced between them. So the machine ran at the
 * monitor's refresh rate, or the audio device's rate, whichever bound first
 * - never at its own. On a 144 Hz display with the audio queue not blocking,
 * an NTSC machine ran 144 frames a second instead of 59.9227.
 *
 * The rule here is the opposite one: wall time decides how many frames the
 * machine owes, presentation is free to happen more or less often than that,
 * and neither one is derived from the other.
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

struct SchedulerDiagnostics
{
    double machine_fps;          // what the emulated machine's clocks produce
    double display_hz;           // the display the window is currently on
    double measured_fps;         // emulated frames per wall second, smoothed
    double effective_fps;        // machine_fps after fast forward and drift
    double debt_frames;          // unpaid time, in frames
    double audio_correction;     // drift nudge as a ratio, 0 when inactive
    unsigned long long catch_up_clamps;  // times a stall was written off
    int frames_last_iteration;
    // 1.0 when the machine runs on its real crystals; anything else means
    // they have been scaled to the display and a reset is what changes it.
    double clock_scale;
    // True while one emulated frame is being produced per N presentations.
    // Nothing repeats and nothing is dropped, so anything that exists to
    // hide a repeat has nothing to do and should stay out of the way.
    bool display_locked;
    // Weight of the newest frame in a temporal box filter over the
    // presentation interval that just elapsed - mpv's "oversample" tscale.
    //
    // 1 means that interval fell entirely inside one emulated frame, so
    // there is nothing to blend and the picture is left exactly as the VDP
    // drew it. Below 1, a frame boundary fell inside the interval and this
    // is how much of it the newest frame covered.
    //
    // It is min(1, debt / presentation period): the debt is the time since
    // the last frame's boundary, which is precisely where that boundary sits
    // inside the interval. Nothing extra has to be measured for it.
    double blend_weight;
    // The rate a Gaming mode would run at on this display, or 0 when the
    // display is not a whole multiple of the machine and Gaming would leave
    // the crystals alone. Reported whatever the current mode is, so the menu
    // can label a mode that is not the one in force.
    double aligned_fps;
    // The presentation rate as measured, not as the display mode claims. A
    // panel sold as 60 Hz is often 59.94, and this is the number the frame
    // converter divides by.
    double measured_refresh;
    bool vsync;
};

// Once at startup, and whenever the machine's frame rate may have changed
// (region switch, ROM load): re-reads the machine rate and the display.
void scheduler_init(void);

// Forget any unpaid time. Call when the machine legitimately stopped
// advancing - pause, breakpoint, ROM load, recorder seek - so the pause does
// not come back as a burst of catch-up frames the moment it ends.
void scheduler_reset(void);

// Start of a main-loop iteration: charges elapsed wall time to the debt.
void scheduler_begin_iteration(void);

// Takes one frame off the debt if one is owed. Loop on it: that is what
// fast forward is, and what catch-up after a stall is.
bool scheduler_take_frame(void);

// After presenting. Sleeps only if nothing else in the iteration did - with
// vertical sync on, the present call already waited, and sleeping again here
// would pace the loop twice.
void scheduler_wait_for_present_slot(void);

SchedulerDiagnostics scheduler_get_diagnostics(void);

#endif /* SCHEDULER_H */
