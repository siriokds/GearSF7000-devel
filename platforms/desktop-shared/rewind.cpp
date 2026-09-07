/*
 * GearSF7000 - frame recorder
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rewind.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <sstream>

#include "emu.h"
#include "application.h"
#include "renderer.h"
#include "config.h"
#include "DiagnosticLog.h"
#include "gearsf7000.h"
#include "Video.h"
#include "TMS9918RasterTiming.h"
#include "Memory.h"
#include "miniz/miniz.h"

namespace
{

// One group is about a second of play. Small enough that eviction granularity
// is not noticeable, large enough that the keyframes it forces are a small
// share of the total.
constexpr int kFramesPerGroup = 60;
constexpr int kMaxSeconds = 600;
constexpr std::size_t kDefaultMemoryLimit = 512u * 1024u * 1024u;

struct Frame
{
    std::vector<std::uint8_t> stored;  // deflated keyframe or XOR delta
    std::uint64_t frame_serial = 0;
    bool keyframe = false;
};

struct Group
{
    std::deque<Frame> frames;
    std::size_t bytes = 0;
};

struct Recorder
{
    bool enabled = false;
    int seconds = 60;
    int frames_per_snapshot = 1;
    std::size_t memory_limit = kDefaultMemoryLimit;

    std::deque<Group> groups;
    std::size_t bytes = 0;
    int count = 0;

    int frame_accum = 0;
    int seek_position = -1;

    // Raw image of the frame most recently pushed. Deltas are formed against
    // this, so it has to be exactly what the previous push serialized.
    std::vector<std::uint8_t> previous_raw;
    std::size_t raw_size = 0;

    // The recorder turned vertical sync off. Reported so it is never a
    // surprise; not undone on stop.
    bool turned_vsync_off = false;

    int dropped = 0;
    int failures = 0;
    int groups_dropped_for_memory = 0;
    double capture_ms_total = 0.0;
    // Smoothed interval between captures in wall-clock milliseconds, for
    // reporting how fast the machine is actually being driven.
    double wall_interval_ms = 0.0;
    std::chrono::steady_clock::time_point last_push{};
    bool has_last_push = false;
    double capture_ms_worst = 0.0;
    int capture_samples = 0;

    // Section sizes of the most recent snapshot, for reporting where the
    // weight is.
    std::vector<RewindSectionWeight> sections;
};

Recorder g;

// See rewind_take_vgm_stop_notice() in rewind.h.
std::string g_VgmStopNotice;

// Frames per second of the machine as it is currently configured. The ring is
// specified in seconds, so this is what turns that into a frame count, and it
// counts in the machine's own seconds - 59.9226 NTSC, 50.1591 PAL, not 60 and
// 50. The derivation and the reason it matters live on emu_get_machine_fps;
// the scheduler paces frames by the same number, and two copies of it would
// be two things to correct when the PAL clock is settled.
double FramesPerSecond()
{
    return emu_get_machine_fps();
}

int TargetCapacity()
{
    const double fps = FramesPerSecond();
    const int per = g.frames_per_snapshot < 1 ? 1 : g.frames_per_snapshot;

    // "Sixty seconds of recording" means being able to go sixty seconds back,
    // not holding sixty seconds' worth of states. With N frames the ages run
    // 0..N-1, so the reach is N-1 *intervals*: sizing to N alone leaves the
    // oldest reachable frame a few milliseconds short of the promise. Hence
    // the +1, on top of rounding up.
    const double frames = g.seconds * fps / per;
    const int target = static_cast<int>(std::ceil(frames - 1e-9)) + 1;
    return target < 1 ? 1 : target;
}

// How many frames can actually be seeked to, which is one fewer than are
// held.
//
// Displaying frame N means letting the beam draw it, which means starting
// from N-1 and running one frame. The oldest frame in the ring has nothing
// before it, so it is kept as that predecessor and never offered as a
// destination: offering it would either show a stale picture or, worse, land
// the machine on N+1 while claiming to be on N.
int ExposedCount()
{
    if (g.count <= 1)
        return 0;
    const int exposed = g.count - 1;
    const int capacity = TargetCapacity();
    return exposed < capacity ? exposed : capacity;
}

// Frame serial at an age counted back from the newest.
std::uint64_t FrameSerialAt(int age)
{
    if (age < 0 || age >= g.count)
        return 0;

    int forward = g.count - 1 - age;
    for (const Group& group : g.groups)
    {
        const int size = static_cast<int>(group.frames.size());
        if (forward >= size)
        {
            forward -= size;
            continue;
        }
        return group.frames[forward].frame_serial;
    }
    return 0;
}

bool Deflate(const std::uint8_t* data, std::size_t size,
             std::vector<std::uint8_t>& out)
{
    mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(size));
    out.resize(bound);
    mz_ulong written = bound;
    // Speed over ratio: this runs inside the frame budget, and the delta has
    // already removed most of the redundancy that a higher level would find.
    const int status = mz_compress2(out.data(), &written, data,
                                    static_cast<mz_ulong>(size),
                                    MZ_BEST_SPEED);
    if (status != MZ_OK)
        return false;
    out.resize(written);
    return true;
}

bool Inflate(const std::vector<std::uint8_t>& in, std::size_t expected,
             std::vector<std::uint8_t>& out)
{
    out.resize(expected);
    mz_ulong written = static_cast<mz_ulong>(expected);
    const int status = mz_uncompress(out.data(), &written, in.data(),
                                     static_cast<mz_ulong>(in.size()));
    return status == MZ_OK && written == expected;
}

// Walks the section list of a snapshot so the status can say where its bytes
// went. Mirrors the container written by GearSF7000Core::SaveState.
void DescribeSections(const std::vector<std::uint8_t>& raw)
{
    g.sections.clear();
    // magic(4) version(2) media(1) region(1) type(4) mapper(4) crc(4) serial(8)
    std::size_t p = 28;
    auto u32 = [&](std::size_t at) -> std::uint32_t {
        return std::uint32_t(raw[at]) | (std::uint32_t(raw[at + 1]) << 8) |
               (std::uint32_t(raw[at + 2]) << 16) |
               (std::uint32_t(raw[at + 3]) << 24);
    };

    while (p + 10 <= raw.size())
    {
        const std::uint32_t id = u32(p);
        if (id == 0)
            break;
        const std::uint32_t length = u32(p + 6);
        char name[5] = {char(id & 0xFF), char((id >> 8) & 0xFF),
                        char((id >> 16) & 0xFF), char((id >> 24) & 0xFF), 0};
        for (int i = 0; i < 4; ++i)
            if (name[i] == ' ')
                name[i] = 0;
        g.sections.push_back({std::string(name), length});
        p += 10 + length;
    }
}

void DropOldestGroup()
{
    if (g.groups.empty())
        return;
    g.bytes -= g.groups.front().bytes;
    g.count -= static_cast<int>(g.groups.front().frames.size());
    g.groups.pop_front();
}

void Trim()
{
    // A delta means nothing without the chain in front of it, so eviction
    // takes a whole group - up to 60 frames - rather than a single frame.
    // That granularity is why the test is written against what would remain
    // *after* dropping the front group, and not against the current count:
    // trimming as soon as count exceeds capacity left the window oscillating
    // between 59 and 60 seconds, delivering less than was asked for.
    //
    // The +1 is the hidden predecessor that ExposedCount() reserves.
    const int required = TargetCapacity() + 1;
    while (g.groups.size() > 1 &&
           g.count - static_cast<int>(g.groups.front().frames.size()) >= required)
        DropOldestGroup();

    // The memory ceiling is allowed to win over the promised duration - there
    // is no alternative - but it is recorded so the shortfall is visible in
    // the status rather than silent.
    while (g.bytes > g.memory_limit && g.groups.size() > 1)
    {
        DropOldestGroup();
        ++g.groups_dropped_for_memory;
    }
}

bool ReconstructRaw(int age, std::vector<std::uint8_t>& out)
{
    if (age < 0 || age >= g.count)
        return false;

    // age counts back from the newest; turn it into a forward index.
    int forward = g.count - 1 - age;

    for (const Group& group : g.groups)
    {
        const int size = static_cast<int>(group.frames.size());
        if (forward >= size)
        {
            forward -= size;
            continue;
        }

        if (!group.frames.front().keyframe)
            return false;

        if (!Inflate(group.frames.front().stored, g.raw_size, out))
            return false;

        std::vector<std::uint8_t> delta;
        for (int i = 1; i <= forward; ++i)
        {
            if (!Inflate(group.frames[i].stored, g.raw_size, delta))
                return false;
            for (std::size_t b = 0; b < g.raw_size; ++b)
                out[b] ^= delta[b];
        }
        return true;
    }

    return false;
}

}

bool rewind_init(void)
{
    rewind_reset();
    return true;
}

void rewind_destroy(void)
{
    rewind_reset();
    g.enabled = false;
}

void rewind_reset(void)
{
    // Swapped against empty containers rather than cleared: clear() keeps a
    // deque's block list and a vector's buffer, so switching the recorder off
    // would leave its memory allocated. Turning it off has to give the memory
    // back.
    std::deque<Group>().swap(g.groups);
    std::vector<std::uint8_t>().swap(g.previous_raw);

    g.bytes = 0;
    g.count = 0;
    g.frame_accum = 0;
    g.seek_position = -1;
    g.raw_size = 0;
    g.dropped = 0;
    g.failures = 0;
    g.groups_dropped_for_memory = 0;
    g.capture_ms_total = 0.0;
    g.capture_ms_worst = 0.0;
    g.capture_samples = 0;
    g.wall_interval_ms = 0.0;
    g.has_last_push = false;
    g.sections.clear();
}

void rewind_abandon_history(void)
{
    if (!g.enabled)
        return;

    rewind_reset();
}

void rewind_push(void)
{
    if (!g.enabled || emu_is_empty())
        return;

    // A frame has been produced while the cursor was parked in the past, so
    // execution has resumed from an old point and the frames after it belong
    // to a future that is not going to happen. Dropping them here, rather
    // than in each resume path, is what makes every way of restarting -
    // rewind_transport play, debug_continue, step frame, run to cursor, the
    // window's play button - end up with one coherent branch instead of a
    // ring holding two versions of the same moment.
    if (g.seek_position > 0)
        rewind_commit_seek();

    if (++g.frame_accum < g.frames_per_snapshot)
        return;
    g.frame_accum = 0;

    const auto started = std::chrono::steady_clock::now();

    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    std::size_t size = 0;
    if (!emu_get_core()->SaveState(stream, size))
    {
        ++g.failures;
        return;
    }

    const std::string text = stream.str();
    const std::uint8_t* raw = reinterpret_cast<const std::uint8_t*>(text.data());
    const std::size_t rawSize = text.size();

    // A change of size means the machine was rewired underneath the ring
    // (different media, different mapper). Start again rather than XOR two
    // things that are not the same shape.
    if (g.raw_size != 0 && rawSize != g.raw_size)
        rewind_reset();

    if (g.raw_size == 0)
    {
        g.raw_size = rawSize;
        DescribeSections(std::vector<std::uint8_t>(raw, raw + rawSize));
    }

    const bool needKeyframe =
        g.groups.empty() ||
        static_cast<int>(g.groups.back().frames.size()) >= kFramesPerGroup ||
        g.previous_raw.size() != rawSize;

    std::vector<std::uint8_t> payload;
    bool ok = false;

    if (needKeyframe)
    {
        ok = Deflate(raw, rawSize, payload);
    }
    else
    {
        std::vector<std::uint8_t> delta(rawSize);
        for (std::size_t b = 0; b < rawSize; ++b)
            delta[b] = raw[b] ^ g.previous_raw[b];
        ok = Deflate(delta.data(), rawSize, payload);
    }

    if (!ok)
    {
        ++g.dropped;
        return;
    }

    if (needKeyframe)
        g.groups.push_back(Group());

    Frame frame;
    frame.stored = std::move(payload);
    frame.keyframe = needKeyframe;
    frame.frame_serial = emu_get_core()->GetVideo()
                             ? emu_get_core()->GetVideo()
                                   ->GetFrameRenderDiagnostics().frameSerial
                             : 0;

    g.bytes += frame.stored.size();
    g.groups.back().bytes += frame.stored.size();
    g.groups.back().frames.push_back(std::move(frame));
    ++g.count;

    g.previous_raw.assign(raw, raw + rawSize);

    Trim();

    const auto finished = std::chrono::steady_clock::now();

    if (g.has_last_push)
    {
        const double gap =
            std::chrono::duration<double, std::milli>(finished - g.last_push)
                .count();
        // A long gap means the machine was not running - a pause, a
        // breakpoint, a load - rather than running slowly, so it would
        // poison the average.
        if (gap > 0.0 && gap < 250.0)
            g.wall_interval_ms = g.wall_interval_ms > 0.0
                                     ? g.wall_interval_ms * 0.98 + gap * 0.02
                                     : gap;
    }
    g.last_push = finished;
    g.has_last_push = true;

    const double ms = std::chrono::duration<double, std::milli>(
                          finished - started).count();
    g.capture_ms_total += ms;
    g.capture_ms_worst = std::max(g.capture_ms_worst, ms);
    ++g.capture_samples;

    // A new frame invalidates any scrub position: the newest frame is now
    // somewhere else.
    g.seek_position = -1;
}

bool rewind_seek(int age)
{
    if (!g.enabled || emu_is_empty())
        return false;

    // A snapshot holds the machine, not a picture. To put frame `age` on the
    // screen the beam has to draw it, so the frame *before* it is loaded and
    // one frame is run: that lands the machine exactly on `age` and paints it
    // the way it really looked, mid-frame register changes included. Storing
    // an image instead would be about 96 KB a frame - more than the entire
    // state - and would show a split-screen ROM only as far as it had been
    // drawn when the snapshot was taken.
    //
    // The predecessor always exists because ExposedCount() never offers the
    // oldest held frame as a destination. It used to be special-cased by
    // loading the oldest frame and running one anyway, which landed the
    // machine on the frame after the one that was asked for.
    if (age < 0 || age >= ExposedCount())
        return false;

    std::vector<std::uint8_t> raw;
    if (!ReconstructRaw(age + 1, raw))
        return false;

    if (!emu_get_core()->LoadState(raw.data(), raw.size()))
        return false;

    emu_render_current_frame();

    // A seek always lands paused. Scrubbing a timeline that is still running
    // would show a frame that has already been left behind.
    emu_pause();

    g.seek_position = age;

    // A VGM recorder is a linear, append-only log with no notion of a frame
    // or a branch - unlike this ring, which exists precisely to throw away a
    // branch that stops happening (see rewind_commit_seek()). Resuming from
    // here would re-issue PSG/AY writes the recorder already appended for the
    // future this seek just abandoned, splicing the old branch and the new
    // one into one file with no way to tell them apart. Stopping here keeps
    // everything already written - which really happened, in that order -
    // and turns a corrupt file into a short but valid one.
    if (emu_is_vgm_recording())
    {
        g_VgmStopNotice = emu_stop_vgm_recording();
        DiagInfo("rewind", "VGM recording stopped by seek: %s",
                 g_VgmStopNotice.c_str());
    }

    return true;
}

int rewind_get_seek_position(void)
{
    return g.seek_position;
}

bool rewind_read_snapshot(int age, std::vector<std::uint8_t>& raw,
                          std::uint64_t* frame_serial)
{
    if (!g.enabled || age < 0 || age >= ExposedCount())
        return false;
    if (!ReconstructRaw(age, raw))
        return false;
    if (frame_serial)
        *frame_serial = FrameSerialAt(age);
    return true;
}

std::string rewind_take_vgm_stop_notice(void)
{
    std::string notice;
    notice.swap(g_VgmStopNotice);
    return notice;
}

void rewind_commit_seek(void)
{
    if (g.seek_position <= 0)
    {
        g.seek_position = -1;
        return;
    }

    // Everything newer than where we are now belongs to a future that is not
    // going to happen.
    int toDrop = g.seek_position;
    while (toDrop > 0 && !g.groups.empty())
    {
        Group& last = g.groups.back();
        if (static_cast<int>(last.frames.size()) <= toDrop)
        {
            toDrop -= static_cast<int>(last.frames.size());
            g.bytes -= last.bytes;
            g.count -= static_cast<int>(last.frames.size());
            g.groups.pop_back();
        }
        else
        {
            for (int i = 0; i < toDrop; ++i)
            {
                g.bytes -= last.frames.back().stored.size();
                last.bytes -= last.frames.back().stored.size();
                last.frames.pop_back();
                --g.count;
            }
            toDrop = 0;
        }
    }

    // The chain the next delta would be built on is gone with it.
    g.previous_raw.clear();
    g.seek_position = -1;
}

bool rewind_configure(bool enabled, int seconds, int frames_per_snapshot,
                      std::size_t memory_limit_bytes,
                      RewindConfigResult* result)
{
    RewindConfigResult local;
    local.requested_seconds = seconds;
    local.memory_limit_bytes = memory_limit_bytes;

    if (seconds < 1 || seconds > kMaxSeconds)
    {
        local.error = "seconds_out_of_range";
        if (result) *result = local;
        return false;
    }

    if (frames_per_snapshot < 1)
    {
        local.error = "frames_per_snapshot_out_of_range";
        if (result) *result = local;
        return false;
    }

    if (memory_limit_bytes == 0)
        memory_limit_bytes = kDefaultMemoryLimit;

    // Estimate before committing, from what this machine has actually been
    // costing. Reducing the requested duration silently is exactly what the
    // caller does not want, so an impossible request is refused with the
    // figure it would need.
    const double fps = FramesPerSecond();
    const int capacity = std::max(
        1, static_cast<int>(std::ceil(seconds * fps / frames_per_snapshot -
                                      1e-9)));

    std::size_t perFrame = 0;
    if (g.count > 0)
        perFrame = g.bytes / static_cast<std::size_t>(g.count);
    else if (g.raw_size > 0)
        perFrame = g.raw_size / 8;   // deltas typically land near this
    if (perFrame == 0)
        perFrame = 12u * 1024u;      // first configure, before any capture

    local.required_memory_bytes =
        perFrame * static_cast<std::size_t>(capacity);

    if (enabled && local.required_memory_bytes > memory_limit_bytes)
    {
        local.error = "rewind_memory_limit_too_small";
        if (result) *result = local;
        return false;
    }

    // The length of the recording is only allowed to change while stopped.
    // Changing it underneath a running ring would either throw away frames
    // the user is still looking at, or leave the buffer describing a duration
    // it does not hold.
    const bool shapeChanged = (seconds != g.seconds) ||
                              (frames_per_snapshot != g.frames_per_snapshot);
    if (g.enabled && enabled && shapeChanged)
    {
        local.error = "stop_the_recorder_before_changing_its_length";
        if (result) *result = local;
        return false;
    }

    g.seconds = seconds;
    g.frames_per_snapshot = frames_per_snapshot;
    g.memory_limit = memory_limit_bytes;

    if (!enabled)
    {
        // Stopping gives the memory back. Nothing else to undo: recording no
        // longer borrows the vertical sync setting - see below.
        rewind_reset();
        g.enabled = false;
    }
    else
    {
        // Recording used to switch vertical sync off here, because with it on
        // the machine really did advance one frame per refresh and every
        // duration in a recording measured the monitor instead of the
        // SC-3000. That is fixed at the source: the scheduler paces frames
        // from the machine's own rate and vertical sync is a presentation
        // setting again, so there is nothing left to borrow. Taking a user's
        // setting away to work around a bug that no longer exists would be
        // the surprising behaviour now.
        g.turned_vsync_off = false;

        if (shapeChanged)
            rewind_reset();
        else
            Trim();
        g.enabled = true;
    }

    local.success = true;
    if (result) *result = local;
    return true;
}

RewindStatus rewind_get_status(void)
{
    RewindStatus s;

    s.format_version = GearSF7000Core::GetSaveStateFormatVersion();
    s.enabled = g.enabled;

    if (emu_is_empty())
    {
        s.available = false;
        s.unavailable_reason = "no_media";
        return s;
    }

    GearSF7000Core* core = emu_get_core();
    if (!core->IsCpuStatePersistenceAvailable())
    {
        s.available = false;
        s.unavailable_reason = "cpu_backend_cannot_serialize";
        return s;
    }

    s.available = true;
    s.media_mode = core->GetMemory()->IsSF7000Enabled() ? "sf7000"
                                                        : "cartridge_rom";
    s.region = core->GetVideo() && core->GetVideo()->IsPAL() ? "PAL"
                                                             : "NTSC";

    const double fps = FramesPerSecond();
    const int exposed = ExposedCount();
    s.snapshot_count = exposed;
    s.capacity = TargetCapacity();
    s.frames_per_snapshot = g.frames_per_snapshot;
    s.buffer_seconds = g.seconds;
    // Two readings, because they answer different questions and differ by one
    // interval: how much is held, and how far back one can actually go.
    s.buffered_seconds =
        static_cast<double>(exposed * g.frames_per_snapshot) / fps;
    s.max_age_seconds =
        exposed > 0 ? static_cast<double>((exposed - 1) * g.frames_per_snapshot) / fps
                    : 0.0;
    s.frames_per_second = fps;
    s.vertical_sync_suspended = g.turned_vsync_off;
    s.display_refresh_hz = application_get_display_refresh_hz();
    if (g.wall_interval_ms > 0.0)
    {
        s.wall_clock_fps = 1000.0 / g.wall_interval_ms;
        s.speed_ratio = s.wall_clock_fps / fps;
    }

    if (exposed > 0)
    {
        // The oldest *reachable* frame, not the oldest one held: the frame
        // behind it is the hidden predecessor and cannot be seeked to.
        s.oldest_frame_serial = FrameSerialAt(exposed - 1);
        s.newest_frame_serial = FrameSerialAt(0);
    }

    s.snapshot_raw_bytes = g.raw_size;
    s.memory_bytes = g.bytes;
    if (g.count > 0)
    {
        // Averaged over every frame actually stored, hidden predecessor
        // included: that is what the memory figure is made of.
        s.average_compressed_bytes =
            static_cast<double>(g.bytes) / g.count;
        if (s.average_compressed_bytes > 0.0)
            s.compression_ratio =
                static_cast<double>(g.raw_size) / s.average_compressed_bytes;
        s.projected_full_bytes = static_cast<std::size_t>(
            s.average_compressed_bytes * (s.capacity + 1));
    }
    s.memory_limit_bytes = g.memory_limit;
    s.memory_limited = g.groups_dropped_for_memory > 0;
    s.groups_dropped_for_memory = g.groups_dropped_for_memory;

    s.sections = g.sections;

    s.snapshots_dropped = g.dropped;
    s.serialization_failures = g.failures;
    if (g.capture_samples > 0)
        s.average_capture_ms = g.capture_ms_total / g.capture_samples;
    s.worst_capture_ms = g.capture_ms_worst;

    return s;
}

int rewind_get_snapshot_count(void)
{
    return ExposedCount();
}

bool rewind_is_enabled(void)
{
    return g.enabled;
}
