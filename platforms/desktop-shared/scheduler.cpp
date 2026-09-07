/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026 Saverio Russo
 */

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

#include "scheduler.h"
#include "config.h"
#include "emu.h"
#include "renderer.h"
#include "application.h"
#include "DiagnosticLog.h"

namespace
{

// A stall longer than this is written off rather than replayed. Dragging the
// window, a modal file dialog, a breakpoint held for a minute: none of those
// are time the machine owes, and trying to catch up on them produces a burst
// of fast-forward the user never asked for.
constexpr double kMaxCatchUpSeconds = 0.25;

// Ceiling on frames per iteration during normal play. The debt survives, so
// a genuine short stall still gets caught up - just spread over the next few
// iterations instead of all at once, which keeps the picture moving.
constexpr int kMaxFramesPerIteration = 4;

// Fast forward asks for many more, and unlimited asks for as many as fit.
// What actually stops unlimited is the caller's own time budget.
constexpr int kMaxFramesFastForward = 32;

// How fast the iteration-time estimate follows the real one. Slow enough to
// remove presentation jitter, fast enough to follow a display that actually
// changed - a window dragged to a monitor with a different refresh.
constexpr double kElapsedSmoothing = 0.05;

// Audio buffer level the correction aims for. Half full leaves equal room to
// absorb the machine running briefly ahead or behind.
constexpr double kAudioTargetFill = 0.5;

// The correction's response is cubic in the fill error, which gives two
// behaviours out of one expression instead of two regimes with a seam:
//
//   error 0.05 (buffer 55% full)  ->  0.03% period change, inaudible
//   error 0.50 (buffer empty)     ->  25% - fills in well under a second
//
// Both are needed, and for different reasons. The small end is drift: the
// audio device runs on its own crystal and the machine on the emulated one,
// so over minutes they separate and the buffer would slowly empty into
// clicks or fill until writes are dropped. The large end is priming: with
// the queue no longer blocking the producer, nothing else fills the buffer
// at startup - production and consumption match exactly, so an empty buffer
// stays empty and underruns forever. It has to be run down deliberately.
constexpr double kAudioCorrectionGain = 2.0;
// Far tighter than when this bent the frame period. It is a pitch change now,
// and a level problem is not its job: a buffer far from target is refilled by
// the scheduler's own catch-up frames, or was primed when the device opened.
// At the crystal error actually measured - around 200 ppm - it sits two
// orders of magnitude inside this.
constexpr double kMaxAudioCorrection = 0.01;

// Fraction of the way to the target the correction moves per iteration, so a
// momentary wobble in the queue does not step the pitch.
constexpr double kAudioCorrectionSlew = 0.1;

// How fast the smoothed buffer level follows the real one. At display rates
// this is about a second of averaging - long against the device's drain
// ripple, short against the drift being corrected.
constexpr double kAudioFillSmoothing = 0.02;

// Used instead while the level is far from target, where following slowly is
// what causes overshoot rather than what prevents it. About a tenth of a
// second.
constexpr double kAudioFillFastSmoothing = 0.3;

double machine_fps = 59.9226;
double machine_period = 1.0 / 59.9226;
double display_hz = 60.0;
double present_period = 1.0 / 60.0;

double debt_seconds = 0.0;
double smoothed_elapsed = 0.0;
// Where we are in the presentation cycle while locked, and whether this
// iteration is the one that advances the machine.
int lock_phase = 0;
bool lock_due = false;
double audio_correction = 0.0;
// Negative until the first reading, so the filter starts at the real level
// instead of ramping up from zero and briefly asking for a full prime.
double fill_smoothed = -1.0;

unsigned long long last_generation = 0;

Uint64 last_tick = 0;
Uint64 last_present_tick = 0;
bool started = false;

int frames_this_iteration = 0;
unsigned long long catch_up_clamps = 0;

// Smoothed, because the raw per-iteration number is either 0 or 1 and says
// nothing on its own.

double seconds_since(Uint64 from, Uint64 to)
{
    const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
    if (freq <= 0.0)
        return 0.0;
    return static_cast<double>(to - from) / freq;
}

// Defined further down, with the counters it reads.
double measured_refresh_hz(void);

// The machine advances only when it is actually running. Everything else -
// no cartridge, paused, sitting at a breakpoint - owes no frames, and must
// not build up a debt that pays out the moment it resumes.
bool machine_is_running(void)
{
    return !emu_is_empty() && !emu_is_paused() && !emu_is_debugging();
}

// How many presentations per emulated frame, or 0 to pace from the clock.
//
// Two requirements that cannot both be met, so both exist and vertical sync
// chooses between them - it is the "wait vblank" of the machine being
// emulated, and turning it on is asking for the display's cadence.
//
//   locked   one frame per N presentations. No skips, no repeats, nothing to
//            see: the machine advances exactly when the screen does. It then
//            runs at display_hz/N, which for a 60.00 Hz monitor is 0.13%
//            above 59.9227. That is what you want while playing.
//
//   clock    frames due from wall time against the machine's own rate. Exact
//            against a real clock, at the cost of repeating one frame every
//            775 - unavoidable when the two rates differ at all. That is what
//            you want while measuring.
//
// Locking is only offered when the display really is a whole multiple of the
// machine's rate. Gearsystem rounds the ratio to an integer unconditionally,
// which at 144 Hz gives 2 and runs the machine at 72 fps; 144/59.9227 is
// 2.403 and is not a multiple of anything. Outside the tolerance the
// accumulator takes over, which is the honest answer for those displays.
int display_lock_divisor(void)
{
    // Locking needs both bits. The crystals have to be aligned, or there is
    // no whole ratio to lock at; and there has to be a blank to lock to.
    // Accurate + VSync deliberately does not lock: it waits for the blank but
    // keeps the machine's own rate, which is the whole point of Accurate.
    if (config_video.pacing != PACING_GAMING_VSYNC || config_emulator.ffwd)
        return 0;
    const double refresh = measured_refresh_hz();
    if (machine_fps <= 1.0 || refresh <= 1.0)
        return 0;

    const double ratio = refresh / machine_fps;
    const int n = static_cast<int>(ratio + 0.5);
    if (n < 1 || n > 8)
        return 0;
    // 60/59.9227 = 1.0013 and 120/59.9227 = 2.0026 lock; 144/59.9227 = 2.403
    // does not, and neither does 75/59.9227 = 1.252.
    if (std::fabs(ratio - static_cast<double>(n)) > 0.02)
        return 0;
    return n;
}

// Fast forward is a shorter frame period, not a separate frame counter. That
// keeps one mechanism instead of two, and makes 1.5x mean 1.5x rather than
// "one extra frame every other iteration, plus whatever the throttle did".
double effective_period(void)
{
    // The machine's own period, and nothing else in it. The audio drift
    // correction used to be applied here, which meant the frame rate moved to
    // suit the sound card - measured at about 200 ppm below the machine's
    // real rate. For a tool built to be believed about timing that is the
    // wrong place to absorb it: 59.9227 has to mean 59.9227 against a real
    // clock. The correction now trims the audio resampler instead, where the
    // crystal difference actually is.
    double period = machine_period;

    if (config_emulator.ffwd && machine_is_running())
    {
        const double speed = emu_get_fast_forward_multiplier();
        if (speed > 0.0)
            period /= speed;
        else
            period = 0.0;  // unlimited: always due, capped by frame count
    }

    return period;
}

// The rate the machine is actually being driven at, which while locked is the
// display's cadence and not its own. One function because there are two
// readers - the overlay and the pacing log - and having each work it out
// separately is how the log ended up reporting 59.9227 for a machine visibly
// running at 60.0000.
double target_fps(void)
{
    const int lock = display_lock_divisor();
    if (lock > 0)
        return measured_refresh_hz() / static_cast<double>(lock);

    const double period = effective_period();
    return (period > 0.0) ? (1.0 / period) : 0.0;
}

// Pull the audio buffer back towards half full by lengthening or shortening
// the frame period slightly. Only while the machine is genuinely free-running
// with audio open: during fast forward the buffer is meant to be starved, and
// while stepping there is no steady state to hold.
void update_audio_correction(void)
{
    if (!machine_is_running() || config_emulator.ffwd || !config_audio.enable)
    {
        if (audio_correction != 0.0)
            emu_set_audio_clock_trim(0.0);
        audio_correction = 0.0;
        fill_smoothed = -1.0;
        return;
    }

    const EmuAudioQueueDiagnostics audio = emu_get_audio_queue_diagnostics();
    if (audio.ring_capacity_samples == 0 || audio.callbacks == 0)
    {
        audio_correction = 0.0;
        return;
    }

    const double fill =
        static_cast<double>(audio.queued_samples) /
        static_cast<double>(audio.ring_capacity_samples);

    // Smoothed hard before it is used, because the instantaneous level is a
    // sawtooth, not a measurement. The device drains in fixed blocks - 2048
    // samples against a 12288-sample ring - so the level drops by a sixth of
    // capacity at a time and climbs back frame by frame. Feeding that
    // straight into the correction made it chase a 40 ms ripple instead of
    // the drift it is for, and the target rate visibly wandered.
    //
    // The two are far enough apart in frequency that filtering is free:
    // crystal drift moves over minutes, the ripple over milliseconds. About
    // a second of averaging removes one and leaves the other untouched.
    if (fill_smoothed < 0.0)
        fill_smoothed = fill;   // first reading: start where we are

    // Adaptive, because one time constant cannot do both jobs. Near the
    // target the filter has to be slow to reject the drain ripple. Far from
    // it, being slow is the whole problem: the correction acts on a level
    // that is a second out of date, keeps pushing after the buffer has
    // already recovered, and overshoots. That is what a measured run showed -
    // the buffer driven from empty to 95% full, then samples dropped.
    //
    // Queue priming means this branch should now be rare; it is kept because
    // recovering from a long stall lands in the same place.
    const double raw_error = std::fabs(fill - kAudioTargetFill);
    const double smoothing =
        (raw_error > 0.15) ? kAudioFillFastSmoothing : kAudioFillSmoothing;
    fill_smoothed += (fill - fill_smoothed) * smoothing;

    // Above the target the machine is ahead of the device, so frames should
    // take longer; below, shorter. The sign follows from period, not rate.
    const double error = fill_smoothed - kAudioTargetFill;
    double target = error * error * error * kAudioCorrectionGain;
    target = std::max(-kMaxAudioCorrection,
                      std::min(kMaxAudioCorrection, target));

    audio_correction += (target - audio_correction) * kAudioCorrectionSlew;

    // Applied to the resampler, not to the frame period: the machine keeps
    // its exact rate and the audio absorbs the device's crystal error.
    emu_set_audio_clock_trim(audio_correction);
}

// Tells the core what to align its crystals to, so that whenever the machine
// is next reset it comes up matching the display instead of compensating for
// it. Pushed every iteration because it is only a stored double; the core
// latches it at Reset, which is the only place the rates reach SystemClock,
// the clock domains, Audio, the SR-1000 and the SF-7000 without disturbing
// anything that is running.
//
// The decision is made against the *native* rate, never the current one:
// asking "is the display a multiple of what the machine runs at" after the
// machine has already been scaled to the display would answer yes to
// anything.
//
// The rate a Gaming mode would produce on this display, or 0 when the display
// is not a whole multiple of the machine and there is nothing to align to.
// Deliberately free of the pacing and fast-forward gates, so the menu can put
// the number in a label for a mode that is not the current one.
double aligned_frame_rate(void)
{
    const double native = emu_get_native_frame_rate();
    const double refresh = measured_refresh_hz();
    if (native <= 1.0 || refresh <= 1.0)
        return 0.0;

    const double ratio = refresh / native;
    const int n = static_cast<int>(ratio + 0.5);
    if (n < 1 || n > 8 || std::fabs(ratio - static_cast<double>(n)) > 0.02)
        return 0.0;
    return refresh / static_cast<double>(n);
}

void update_clock_alignment(void)
{
    // The align bit, and nothing about vertical sync. Gaming without the
    // blank still wants the aligned crystals - it is asking to run at the
    // display's rate, just without waiting to show it.
    const bool align = config_video_pacing_aligned() && !config_emulator.ffwd;

    emu_set_target_frame_rate(align ? aligned_frame_rate() : 0.0);
}

void refresh_display_rate(void)
{
    const double hz = application_get_display_refresh_hz();
    display_hz = (hz > 1.0) ? hz : 60.0;
    present_period = 1.0 / display_hz;
}

// Periodic trace of what the pacing actually did, for runs long enough that
// watching the overlay is not an option.
//
// The cumulative rate is the number that settles the question. Any windowed
// average carries the noise of its own endpoints; frames counted since the
// machine started running, divided by the time since, converges on the truth
// and keeps getting sharper the longer it runs. If it reads 59.9226 after ten
// minutes, the machine is on its own clocks and nothing else needs arguing.
//
// The min/max pairs are there because averages hide exactly the intermittent
// events worth finding: a debt that touched two frames, a buffer that came
// near empty, a correction that saturated.
constexpr double kLogIntervalSeconds = 10.0;

Uint64 log_last_tick = 0;
Uint64 run_start_tick = 0;
unsigned long long run_frames = 0;
unsigned long long run_iterations = 0;
// Time the machine was actually running, accumulated per iteration.
//
// Not "now minus when the run started": iterations are only counted while
// the machine is advancing, so any pause inflates that denominator without
// touching the numerator and every rate derived from it reads low. It showed
// up as the measured refresh disagreeing with itself between two runs on the
// same monitor - 60.0000 and then 59.9838 - and as a cumulative rate that
// never quite reached its target.
double run_active_seconds = 0.0;
Uint64 run_last_tick = 0;
unsigned long long interval_frames = 0;
unsigned long long interval_iterations = 0;
unsigned long long interval_idle_iterations = 0;
double interval_debt_min = 0.0;
double interval_debt_max = 0.0;
double interval_fill_min = 0.0;
double interval_fill_max = 0.0;
double interval_drift_min = 0.0;
double interval_drift_max = 0.0;
bool interval_has_samples = false;
unsigned long long logged_underruns = 0;
unsigned long long logged_dropped = 0;
unsigned long long logged_conversion_presentations = 0;
unsigned long long logged_conversion_converted = 0;

// The refresh rate the display is really running at, measured, not asked for.
//
// SDL reports the mode's nominal figure and a panel sold as 60 Hz is often
// 59.94 or 60.002. That is close enough to ignore while it only decides how
// long to sleep, and not close enough once it decides what to scale the
// machine's crystals to: aligning to a claimed 60.000 on a panel that really
// runs 59.94 would leave the machine wrong by 0.1%, which is worse than not
// aligning at all.
//
// With vertical sync on, the loop iterates once per refresh, so counting
// iterations against elapsed time measures the panel directly and keeps
// sharpening. Below a couple of seconds there is not enough of it to trust,
// and the nominal figure is the better guess.
double measured_refresh_hz(void)
{
    if (run_iterations < 120 || run_active_seconds < 2.0)
        return display_hz;

    const double measured =
        static_cast<double>(run_iterations) / run_active_seconds;

    // A measurement wildly off the mode is not a lying panel, it is a loop
    // that has been blocked; the nominal figure is the safer answer.
    if (measured < display_hz * 0.5 || measured > display_hz * 1.5)
        return display_hz;

    return measured;
}

void reset_interval(void)
{
    interval_frames = 0;
    interval_iterations = 0;
    interval_idle_iterations = 0;
    interval_has_samples = false;
}

void log_tick(Uint64 now)
{
    if (!machine_is_running())
        return;

    if (run_start_tick == 0)
    {
        run_start_tick = now;
        log_last_tick = now;
        run_last_tick = now;
        reset_interval();
        return;
    }

    // Only the time between two iterations that both ran. A gap across a
    // pause is skipped by the guard above, so it never enters the total.
    //
    // Capped the same way the debt is, and for the same reason. A screenshot,
    // a dragged window or a file dialog blocks the loop while the machine is
    // still nominally running, so the gap does land here - and left uncapped
    // it stays in the average for good: one stall took the reported rate to
    // 54.07 for a machine that had not slowed down at all. The debt writes
    // those off already; a measurement that does not would be telling a
    // different story about the same event.
    double delta = seconds_since(run_last_tick, now);
    if (delta > kMaxCatchUpSeconds)
        delta = kMaxCatchUpSeconds;
    run_active_seconds += delta;
    run_last_tick = now;

    // Sampled every iteration so the extremes are real extremes, not
    // whatever happened to be true at the moment the interval closed.
    const EmuAudioQueueDiagnostics audio = emu_get_audio_queue_diagnostics();
    const double fill =
        audio.ring_capacity_samples > 0
            ? static_cast<double>(audio.queued_samples) /
                  static_cast<double>(audio.ring_capacity_samples)
            : 0.0;
    const double debt_frames =
        machine_period > 0.0 ? debt_seconds / machine_period : 0.0;

    ++interval_iterations;
    if (frames_this_iteration == 0)
        ++interval_idle_iterations;
    interval_frames += static_cast<unsigned>(frames_this_iteration);
    run_frames += static_cast<unsigned>(frames_this_iteration);
    ++run_iterations;

    if (!interval_has_samples)
    {
        interval_debt_min = interval_debt_max = debt_frames;
        interval_fill_min = interval_fill_max = fill;
        interval_drift_min = interval_drift_max = audio_correction;
        interval_has_samples = true;
    }
    else
    {
        interval_debt_min = std::min(interval_debt_min, debt_frames);
        interval_debt_max = std::max(interval_debt_max, debt_frames);
        interval_fill_min = std::min(interval_fill_min, fill);
        interval_fill_max = std::max(interval_fill_max, fill);
        interval_drift_min = std::min(interval_drift_min, audio_correction);
        interval_drift_max = std::max(interval_drift_max, audio_correction);
    }

    const double since_log = seconds_since(log_last_tick, now);
    if (since_log < kLogIntervalSeconds)
        return;

    const double run_seconds = run_active_seconds;
    const double interval_fps =
        since_log > 0.0 ? static_cast<double>(interval_frames) / since_log : 0.0;
    const double run_fps =
        run_seconds > 0.0 ? static_cast<double>(run_frames) / run_seconds : 0.0;

    DiagInfo("pacing",
             "%.0fs  fps interval %.4f  cumulative %.4f  target %.4f  "
             "machine %.4f  display %.2f",
             run_seconds, interval_fps, run_fps, target_fps(),
             machine_fps, display_hz);
    DiagInfo("pacing",
             "     frames %llu  iterations %llu  idle %llu  "
             "debt %.3f..%.3f fr  drift %+.4f%%..%+.4f%%",
             interval_frames, interval_iterations, interval_idle_iterations,
             interval_debt_min, interval_debt_max,
             interval_drift_min * 100.0, interval_drift_max * 100.0);
    DiagInfo("pacing",
             "     audio fill %.1f%%..%.1f%%  underruns +%llu  dropped +%llu  "
             "clamps %llu",
             interval_fill_min * 100.0, interval_fill_max * 100.0,
             audio.underruns - logged_underruns,
             audio.producer_dropped_samples - logged_dropped,
             catch_up_clamps);

    // What the overlay shows, so a log from a test run answers the same
    // question without needing someone watching the screen at the time.
    const RendererConversionStats conv = renderer_get_conversion_stats();
    const unsigned long long presentations_delta =
        conv.presentations - logged_conversion_presentations;
    const unsigned long long converted_delta =
        conv.converted - logged_conversion_converted;
    const double converted_pct =
        presentations_delta > 0
            ? 100.0 * static_cast<double>(converted_delta) /
                  static_cast<double>(presentations_delta)
            : 0.0;
    DiagInfo("pacing",
             "     pacing %s  vsync %s  locked %s  converter %.0f%% "
             "(%llu/%llu frames)",
             config_video_pacing_name(), config_video_vsync() ? "on" : "off",
             (display_lock_divisor() > 0) ? "yes" : "no", converted_pct,
             converted_delta, presentations_delta);

    logged_underruns = audio.underruns;
    logged_dropped = audio.producer_dropped_samples;
    logged_conversion_presentations = conv.presentations;
    logged_conversion_converted = conv.converted;
    log_last_tick = now;
    reset_interval();
}

}  // namespace

void scheduler_init(void)
{
    machine_fps = emu_get_machine_fps();
    if (machine_fps <= 1.0)
        machine_fps = 59.9226;
    machine_period = 1.0 / machine_fps;

    refresh_display_rate();
    scheduler_reset();
}

void scheduler_reset(void)
{
    last_generation = emu_get_machine_generation();
    debt_seconds = 0.0;
    smoothed_elapsed = 0.0;
    lock_phase = 0;
    lock_due = false;
    audio_correction = 0.0;
    fill_smoothed = -1.0;
    last_tick = SDL_GetPerformanceCounter();
    last_present_tick = last_tick;
    started = true;
    frames_this_iteration = 0;

    // Cumulative statistics belong to one run of one machine: carrying them
    // across a ROM load would average two different things together.
    run_start_tick = 0;
    run_last_tick = 0;
    run_active_seconds = 0.0;
    run_frames = 0;
    run_iterations = 0;
    log_last_tick = 0;
    reset_interval();
}

void scheduler_begin_iteration(void)
{
    const Uint64 now = SDL_GetPerformanceCounter();

    if (!started)
    {
        scheduler_reset();
        return;
    }

    // Before frames_this_iteration is cleared: it describes the iteration
    // that has just finished, which is the one worth recording.
    log_tick(now);

    double elapsed = seconds_since(last_tick, now);
    last_tick = now;

    frames_this_iteration = 0;

    // A new machine starts with no debt. Loading a ROM takes long enough -
    // reading the file, resetting, rebuilding - that charging it as elapsed
    // time would open the cartridge with a burst of catch-up frames. Noticed
    // here rather than announced by the loader: the generation counter
    // already exists and cannot be forgotten at a call site.
    const unsigned long long generation = emu_get_machine_generation();
    if (generation != last_generation)
    {
        last_generation = generation;
        scheduler_reset();
        return;
    }

    // The machine's rate can change under us - a PAL/NTSC switch, a different
    // cartridge - and the period is cheap to re-read.
    const double fps = emu_get_machine_fps();
    if (fps > 1.0 && std::fabs(fps - machine_fps) > 0.0001)
    {
        machine_fps = fps;
        machine_period = 1.0 / fps;
    }
    refresh_display_rate();
    update_clock_alignment();
    update_audio_correction();

    if (!machine_is_running())
    {
        // Not a stall to be caught up on later: the machine was not supposed
        // to be advancing.
        debt_seconds = 0.0;
        return;
    }

    if (elapsed > kMaxCatchUpSeconds)
    {
        elapsed = kMaxCatchUpSeconds;
        ++catch_up_clamps;
    }

    const int lock = display_lock_divisor();
    if (lock > 0)
    {
        // One frame per N presentations, and the wall clock does not come
        // into it: the display is the clock. Debt is cleared rather than
        // accumulated so that switching back to the other mode does not
        // start with a pile of it.
        lock_due = (lock_phase == 0);
        lock_phase = (lock_phase + 1) % lock;
        debt_seconds = 0.0;
        return;
    }
    lock_phase = 0;
    lock_due = false;

    // The debt is charged the smoothed iteration time, not the raw one.
    //
    // The loop is paced by presentation, so its period is steady in the mean
    // and jittery in every individual sample - half a millisecond either way
    // from vertical sync and the host's own scheduling. Feeding that straight
    // in makes the debt cross the one-frame threshold erratically instead of
    // regularly, and every crossing is a skipped frame followed by a doubled
    // one. Measured: six idle iterations and five doubles per six hundred,
    // about one visible hiccup a second, where the arithmetic only calls for
    // one skip every 775 iterations - a 60.00 Hz display against a 59.9227
    // Hz machine has to repeat a frame that often and no more.
    //
    // Smoothing is unbiased, so the long-run total is still real time and the
    // rate stays exact; it only stops the crossings from bunching.
    //
    // A genuinely late iteration is different from jitter and must not be
    // averaged away: a real stall is time the machine owes. Anything far
    // outside the running mean is taken as it is.
    if (smoothed_elapsed <= 0.0)
        smoothed_elapsed = elapsed;
    else if (std::fabs(elapsed - smoothed_elapsed) > smoothed_elapsed * 0.5)
        smoothed_elapsed = elapsed;
    else
        smoothed_elapsed += (elapsed - smoothed_elapsed) * kElapsedSmoothing;

    debt_seconds += smoothed_elapsed;
}

bool scheduler_take_frame(void)
{
    // An armed step is not paced. It was asked for explicitly, so it does not
    // wait for wall-clock debt, and it must be handed a frame even though
    // arming it set the very flag machine_is_running() reads as "stopped".
    // Without this the frame that would consume the step is never run, the
    // step stays armed forever, and the button looks dead. The frame itself
    // still ends on an instruction boundary: that is RunToVBlank's business,
    // not the scheduler's, and this changes only whether it gets to run.
    if (emu_debug_step_pending() && !emu_is_empty() && !emu_is_paused())
    {
        if (frames_this_iteration >= kMaxFramesPerIteration)
            return false;
        ++frames_this_iteration;
        return true;
    }

    if (!machine_is_running())
        return false;

    const int cap = config_emulator.ffwd ? kMaxFramesFastForward
                                         : kMaxFramesPerIteration;
    if (frames_this_iteration >= cap)
        return false;

    if (display_lock_divisor() > 0)
    {
        if (!lock_due || frames_this_iteration > 0)
            return false;
        ++frames_this_iteration;
        return true;
    }

    const double period = effective_period();

    // Unlimited fast forward: no period to wait for, the frame count is the
    // only limit.
    if (period <= 0.0)
    {
        ++frames_this_iteration;
        return true;
    }

    if (debt_seconds < period)
        return false;

    debt_seconds -= period;
    ++frames_this_iteration;

    // Nothing is measured here any more. The reported rate is the cumulative
    // one - frames since this machine started running, over the time since -
    // computed where those counters already live, in log_tick.
    //
    // Two windowed measurements were tried and both were too noisy to read,
    // for the same underlying reason: frames are quantized against a loop
    // that iterates at the display's rate. A fixed-time window reads 60
    // almost always and 59 occasionally, a 1.7% dip. A fixed-frame window is
    // no better - 120 frames take either 120 iterations or 121 depending on
    // whether the scheduler skipped one, which is 60.00 against 59.50, and
    // an average that is correct made of samples that swing half a frame
    // either side is not something anyone can read off a screen.
    //
    // The skip is not an error to be smoothed away: it is how the rate is
    // held correct. So the honest number is the one that integrates over all
    // of them, and it sharpens the longer it runs instead of jittering
    // forever.

    return true;
}

void scheduler_wait_for_present_slot(void)
{
    const Uint64 now = SDL_GetPerformanceCounter();

    // With vertical sync on, the present call has already blocked until the
    // display was ready. Sleeping here as well would be a second limiter on
    // the same loop - exactly the mistake this scheduler exists to undo.
    if (renderer_get_vsync())
    {
        last_present_tick = now;
        return;
    }

    // Unlimited fast forward is asking to go as fast as the host allows, so
    // there is nothing to wait for.
    if (config_emulator.ffwd && emu_get_fast_forward_multiplier() <= 0.0)
    {
        last_present_tick = now;
        return;
    }

    // Otherwise hold the loop to the display's rate. Not the machine's: this
    // paces *presentation*, and running the loop faster than the display can
    // show only burns the GPU. How many frames the machine ran in between is
    // already settled, and is not this function's business.
    const double elapsed = seconds_since(last_present_tick, now);
    const double remaining = present_period - elapsed;

    if (remaining > 0.0)
    {
        // Sleep most of it and spin the rest: SDL_Delay rounds to whole
        // milliseconds and routinely overshoots, which at 144 Hz is most of
        // a frame. Same shape as the headless loop's pacing.
        if (remaining > 0.0015)
            SDL_Delay(static_cast<Uint32>((remaining - 0.001) * 1000.0));

        const Uint64 deadline =
            last_present_tick +
            static_cast<Uint64>(present_period *
                                static_cast<double>(SDL_GetPerformanceFrequency()));
        while (SDL_GetPerformanceCounter() < deadline)
            ;
        last_present_tick = deadline;
        return;
    }

    last_present_tick = now;
}

SchedulerDiagnostics scheduler_get_diagnostics(void)
{
    SchedulerDiagnostics d;
    d.machine_fps = machine_fps;
    d.display_hz = display_hz;
    // Cumulative, not windowed: frames since this machine started running
    // over the time since. Reset when the machine changes, so it always
    // describes the run in front of you.
    d.measured_fps = run_active_seconds > 0.0
                         ? static_cast<double>(run_frames) / run_active_seconds
                         : 0.0;
    // While locked this is the display's cadence, not the machine's own, and
    // saying otherwise would hide exactly the trade-off that mode makes.
    d.effective_fps = target_fps();
    d.debt_frames = (machine_period > 0.0) ? (debt_seconds / machine_period) : 0.0;
    d.audio_correction = audio_correction;
    d.catch_up_clamps = catch_up_clamps;
    d.frames_last_iteration = frames_this_iteration;
    d.aligned_fps = aligned_frame_rate();
    d.measured_refresh = measured_refresh_hz();
    d.vsync = renderer_get_vsync();
    d.display_locked = (display_lock_divisor() > 0);
    // Against the presentation period, not the machine's: the filter
    // integrates over what the screen showed, and that is how long a
    // presentation lasts.
    //
    // Measured, like every other decision that has to be right about the real
    // cadence. present_period is built from the figure SDL reports for the
    // mode, and a panel sold as 60 Hz is often 59.94: taking the nominal one
    // here would bias every weight by that difference, on a value that is a
    // fraction of an interval that really happened. The measurement is
    // already there and costs nothing to use.
    const double measured_period = 1.0 / measured_refresh_hz();
    const double weight = (measured_period > 0.0)
                              ? (debt_seconds / measured_period)
                              : 1.0;
    d.blend_weight = weight < 0.0 ? 0.0 : (weight > 1.0 ? 1.0 : weight);
    // Scaling is latched at Reset, so after switching vertical sync off the
    // machine keeps running at the aligned rate until it is reset. That is
    // the intended behaviour and completely invisible without saying so.
    const double native = emu_get_native_frame_rate();
    d.clock_scale = (native > 1.0) ? (machine_fps / native) : 1.0;
    return d;
}
