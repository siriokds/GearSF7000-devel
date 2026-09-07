/*
 * GearSF7000 - frame recorder
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_REWIND_H
#define GEARSF7000_REWIND_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// A rolling recording of the machine, one snapshot per frame.
//
// A minute of SC-3000 at 60 Hz is 3600 snapshots of roughly 90 KB, which is
// 300 MB if kept verbatim. Almost none of that is new information: between
// two consecutive frames most of the 64 KB of RAM, all of the ROM-derived
// state and usually most of VRAM are unchanged. So frames are grouped, the
// first frame of each group is stored whole, and the rest are stored as the
// XOR difference against the frame before them. Every stored block is then
// deflated.
//
// Grouping is what makes eviction possible: a delta is meaningless without
// the chain behind it, so the oldest whole group is dropped at once rather
// than individual frames.
struct RewindSectionWeight
{
    std::string name;   // four-character section id, as written in the state
    std::size_t bytes;  // uncompressed size in one snapshot
};

struct RewindStatus
{
    bool available = false;
    bool enabled = false;
    std::string unavailable_reason;

    std::string media_mode;      // "cartridge_rom" or "sf7000"
    std::string region;          // "NTSC" or "PAL"

    int snapshot_count = 0;
    int capacity = 0;
    int frames_per_snapshot = 1;
    int buffer_seconds = 0;
    double buffered_seconds = 0.0;
    // How far back a seek can actually reach, which is one frame interval less
    // than buffered_seconds: with N frames held the oldest is N-1 intervals
    // old, not N. This is the number the requested duration is measured
    // against.
    double max_age_seconds = 0.0;
    // The machine's real frame rate, which is neither 60 nor 50: see
    // FramesPerSecond() in rewind.cpp. Reported so a client converting frames
    // to seconds uses the same figure this does.
    double frames_per_second = 0.0;
    // Frames actually captured per second of wall-clock time, measured. The
    // recorder counts machine frames, so this never changes what is held -
    // but it is how you find out that the machine is not running at its own
    // speed. With vertical sync on there is no other frame limiter while a
    // ROM is running, so the emulator advances one frame per display refresh:
    // on a 60 Hz panel that is 0.13% fast, and on a 120 Hz one it is double.
    double wall_clock_fps = 0.0;
    double speed_ratio = 0.0;   // wall_clock_fps / frames_per_second

    // The recorder turned vertical sync off, because with it on the machine
    // is paced by the display instead of by its own crystals. It is left off
    // when recording stops: switching it back on is the user's call.
    // Always false now. Recording used to switch vertical sync off to stop
    // the display pacing the machine; the scheduler fixed that at the source.
    // Kept so MCP clients reading the field keep working.
    bool vertical_sync_suspended = false;
    double display_refresh_hz = 0.0;

    std::uint64_t oldest_frame_serial = 0;
    std::uint64_t newest_frame_serial = 0;

    // What the recording actually costs.
    std::size_t snapshot_raw_bytes = 0;      // one frame, before compression
    std::size_t memory_bytes = 0;            // everything currently held
    double average_compressed_bytes = 0.0;   // per frame, as stored
    double compression_ratio = 0.0;          // raw / stored
    std::size_t projected_full_bytes = 0;    // memory at full capacity
    std::size_t memory_limit_bytes = 0;
    bool memory_limited = false;             // capacity cut short by the limit
    int groups_dropped_for_memory = 0;

    // Where the weight is, so it is obvious what to leave out.
    std::vector<RewindSectionWeight> sections;

    // Health.
    int snapshots_dropped = 0;               // must stay zero
    int serialization_failures = 0;
    double average_capture_ms = 0.0;
    double worst_capture_ms = 0.0;
    int format_version = 0;
};

struct RewindConfigResult
{
    bool success = false;
    std::string error;
    int requested_seconds = 0;
    std::size_t required_memory_bytes = 0;
    std::size_t memory_limit_bytes = 0;
};

bool rewind_init(void);
void rewind_destroy(void);
// Drops everything. Called when the media changes: a recording of one
// cartridge cannot be applied to another.
void rewind_reset(void);

// One frame completed. Cheap and does nothing when disabled.
void rewind_push(void);

// A state was loaded from outside the recorder - a file or a slot - so the
// machine is no longer where the recording says it is. Everything held
// describes a line that has been left, and the delta chain would splice a new
// history onto a frame from that old line. The frames go; the configuration
// and the running state stay.
//
// Deliberately not called by rewind_seek(), which loads through a different
// path precisely so that scrubbing does not destroy what it is scrubbing.
void rewind_abandon_history(void);

// age 0 is the newest frame, age snapshot_count-1 the oldest. Leaves the
// emulator paused, always.
bool rewind_seek(int age);
int  rewind_get_seek_position(void);

// Empty unless the seek that just succeeded also stopped a VGM recording -
// see rewind_seek()'s own comment for why it has to. Read-and-clear: call
// once per successful seek and show it if not empty, so the same notice
// cannot be reported twice from two different callers.
std::string rewind_take_vgm_stop_notice(void);
// Throws away the branch that is no longer going to happen, so that resuming
// from an old frame does not leave a future that never occurred in the ring.
void rewind_commit_seek(void);

bool rewind_configure(bool enabled, int seconds, int frames_per_snapshot,
                      std::size_t memory_limit_bytes,
                      RewindConfigResult* result);

RewindStatus rewind_get_status(void);

int rewind_get_snapshot_count(void);
bool rewind_is_enabled(void);

// Non-mutating access for bounded MCP analysis. Unlike rewind_seek(), this
// reconstructs the state stored at age directly: it neither runs a frame nor
// moves the Recorder cursor. age 0 is the newest exposed snapshot.
bool rewind_read_snapshot(int age, std::vector<std::uint8_t>& raw,
                          std::uint64_t* frame_serial = nullptr);

#endif
