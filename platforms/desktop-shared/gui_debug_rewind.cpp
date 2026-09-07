/*
 * GearSF7000 - frame recorder window
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gui_debug_rewind.h"

#include <cmath>
#include <cstdio>
#include <string>

#include "imgui/imgui.h"
#include "imgui/colors.h"
#include "imgui/fonts/IconsMaterialDesign.h"

#include "config.h"
#include "emu.h"
#include "gui.h"
#include "rewind.h"

namespace
{

// How long the timeline has to be still before a seek is issued. Seeking on
// every pixel of a drag would ask the machine to rebuild and redraw a frame
// far faster than it can, and the slider would lag behind the mouse.
constexpr double kSettleSeconds = 0.25;

int  pending_position = -1;   // where the slider is, not yet applied
double pending_since = 0.0;
int  applied_position = 0;    // where the machine actually is
char last_error[96] = {0};
bool grabbed = false;
bool playback_active = false;
int playback_speed_index = 2;
double playback_last_time = 0.0;
double playback_frames_due = 0.0;

constexpr double kPlaybackSpeeds[] = {0.25, 0.5, 1.0, 2.0, 4.0};

void StopPlayback()
{
    playback_active = false;
    playback_frames_due = 0.0;
}

// A VGM recording active during the seek does not survive it - see
// rewind_seek()'s own comment for why. Reported here rather than left to be
// discovered from a truncated file.
void ReportVgmStopIfAny()
{
    const std::string stopped = rewind_take_vgm_stop_notice();
    if (!stopped.empty())
    {
        const std::string message = "VGM recording stopped by rewind: " + stopped;
        gui_set_status_message(message.c_str(), 4000);
    }
}

void ApplyPendingSeek()
{
    if (pending_position < 0)
        return;
    if (ImGui::GetTime() - pending_since < kSettleSeconds)
        return;

    if (rewind_seek(pending_position))
    {
        applied_position = pending_position;
        ReportVgmStopIfAny();
    }
    pending_position = -1;
}

bool SeekNow(int age)
{
    const int count = rewind_get_snapshot_count();
    if (age < 0 || age >= count)
        return false;
    if (!rewind_seek(age))
        return false;
    applied_position = age;
    pending_position = -1;
    ReportVgmStopIfAny();
    return true;
}

// Tooltips have to leave the icon font, which has no ASCII glyphs: written
// inside it the text renders as an empty rectangle.
void IconTooltip(const char* text)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        return;
    ImGui::PopFont();
    if (ImGui::BeginTooltip())
    {
        ImGui::TextUnformatted(text);
        ImGui::EndTooltip();
    }
    ImGui::PushFont(gui_material_icons_font);
}

void DrawTransport(int count, bool can_scrub)
{
    const bool stopped = emu_is_execution_stopped();
    const int position = pending_position >= 0 ? pending_position
                                               : applied_position;
    const bool at_newest = position <= 0;
    const bool at_oldest = position >= count - 1;

    ImGui::PushFont(gui_material_icons_font);

    if (playback_active)
    {
        if (ImGui::Button(ICON_MD_PAUSE "##rw_recording_pause"))
            StopPlayback();
        IconTooltip("pause recorded playback");
    }
    else
    {
        ImGui::BeginDisabled(!can_scrub || at_newest);
        if (ImGui::Button(ICON_MD_PLAY_ARROW "##rw_recording_play"))
        {
            // This is playback of stored states, not live emulation. The core
            // remains paused and no new machine frame is executed.
            if (!emu_is_paused())
                emu_pause();
            playback_active = true;
            playback_last_time = ImGui::GetTime();
            playback_frames_due = 0.0;
        }
        ImGui::EndDisabled();
        IconTooltip("play the recording to its newest frame");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!can_scrub || at_oldest);
    if (ImGui::Button(ICON_MD_SKIP_PREVIOUS "##rw_oldest"))
    {
        StopPlayback();
        SeekNow(count - 1);
    }
    ImGui::EndDisabled();

    // Exactly one frame each, whatever the timeline's pixel granularity is.
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_scrub || at_oldest);
    if (ImGui::Button(ICON_MD_FAST_REWIND "##rw_back"))
    {
        StopPlayback();
        SeekNow(position + 1);
    }
    ImGui::EndDisabled();
    IconTooltip("one frame back  (Shift+F6)");

    ImGui::SameLine();
    ImGui::BeginDisabled(!can_scrub || at_newest);
    if (ImGui::Button(ICON_MD_FAST_FORWARD "##rw_fwd"))
    {
        StopPlayback();
        SeekNow(position - 1);
    }
    ImGui::EndDisabled();
    IconTooltip("one frame forward  (F6)");

    ImGui::SameLine();
    ImGui::BeginDisabled(!can_scrub || at_newest);
    if (ImGui::Button(ICON_MD_SKIP_NEXT "##rw_newest"))
    {
        StopPlayback();
        SeekNow(0);
    }
    ImGui::EndDisabled();

    ImGui::PopFont();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(72);
    ImGui::Combo("##rw_playback_speed", &playback_speed_index,
                 "0.25x\0 0.5x\0   1x\0   2x\0   4x\0\0");

    ImGui::SameLine();
    ImGui::BeginDisabled(!stopped || emu_is_empty());
    if (ImGui::Button("Resume live"))
    {
        StopPlayback();
        // Analysis is non-destructive: return to the newest captured state
        // before execution continues, then append new frames normally.
        emu_debug_continue();
        if (!emu_is_execution_stopped())
        {
            applied_position = 0;
            pending_position = -1;
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!stopped || emu_is_empty());
    if (ImGui::Button("Resume from here"))
        gui_debug_rewind_resume_from_here();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Ctrl+F5 - discard newer recorded frames when viewing history");

    ImGui::SameLine();
    if (playback_active)
        ImGui::TextColored(orange, "PLAYBACK");
    else
        ImGui::TextColored(stopped ? red : green,
                           stopped ? "PAUSED" : "RUNNING");
}

void AdvancePlayback(const RewindStatus& status, int count)
{
    if (!playback_active)
        return;
    if (!status.available || !status.enabled || count <= 0 ||
        emu_is_empty() || !emu_is_execution_stopped())
    {
        StopPlayback();
        return;
    }

    const int position = pending_position >= 0 ? pending_position
                                               : applied_position;
    if (position <= 0)
    {
        StopPlayback();
        return;
    }

    const double now = ImGui::GetTime();
    double elapsed = now - playback_last_time;
    playback_last_time = now;
    if (elapsed < 0.0)
        elapsed = 0.0;
    // A dragged window or breakpoint in the host must not make playback jump
    // seconds ahead when drawing resumes.
    if (elapsed > 0.25)
        elapsed = 0.25;

    const double fps = status.frames_per_second > 0.0
                           ? status.frames_per_second
                           : (status.region == "PAL" ? 50.0 : 60.0);
    const int stride = status.frames_per_snapshot > 0
                           ? status.frames_per_snapshot
                           : 1;
    playback_frames_due += elapsed * fps *
                           kPlaybackSpeeds[playback_speed_index] / stride;
    const int advance = static_cast<int>(std::floor(playback_frames_due));
    if (advance <= 0)
        return;

    playback_frames_due -= advance;
    const int target = position > advance ? position - advance : 0;
    if (!SeekNow(target) || target == 0)
        StopPlayback();
}

void DrawTimeline(const RewindStatus& status, int count, bool can_scrub)
{
    if (!can_scrub)
    {
        ImGui::BeginDisabled(true);
        int dummy = 0;
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##rw_timeline", &dummy, 0, 0,
                         count > 0 ? "pause to scrub" : "nothing recorded yet");
        ImGui::EndDisabled();
        return;
    }

    const int oldest = count - 1;
    int position = pending_position >= 0 ? pending_position : applied_position;
    if (position > oldest)
        position = oldest;

    // Drawn left-to-right as time, so the oldest frame is at the left end.
    int slider = oldest - position;

    const double fps = (status.region == "PAL") ? 50.0 : 60.0;
    const double ago =
        position * status.frames_per_snapshot / (fps > 0 ? fps : 60.0);

    // The whole recording is mapped across the width of the bar, so how much
    // time one pixel is worth depends on how long the recording is. Say it,
    // because it decides whether dragging can land on a single frame or only
    // near it - the < and > buttons are what land exactly.
    ImGui::SetNextItemWidth(-1);
    const float bar_width = ImGui::CalcItemWidth();
    const double frames_per_pixel =
        bar_width > 1.0f ? static_cast<double>(count) / bar_width : 1.0;

    char label[128];
    char grain[48];
    if (frames_per_pixel <= 1.0)
        snprintf(grain, sizeof(grain), "1px = 1 frame");
    else
        snprintf(grain, sizeof(grain), "1px = %.0f frames", frames_per_pixel);

    if (pending_position >= 0)
        snprintf(label, sizeof(label), "-%.2fs   frame %d / %d   %s  (seeking)",
                 ago, slider + 1, count, grain);
    else
        snprintf(label, sizeof(label), "-%.2fs   frame %d / %d   %s", ago,
                 slider + 1, count, grain);

    const bool changed =
        ImGui::SliderInt("##rw_timeline", &slider, 0, oldest, label);

    // Taking hold of the timeline stops the machine straight away, so that
    // what is on screen is the frame under the cursor and not one that has
    // already gone past.
    if (ImGui::IsItemActivated())
    {
        StopPlayback();
        grabbed = true;
        if (!emu_is_paused())
            emu_pause();
    }
    if (ImGui::IsItemDeactivated())
        grabbed = false;

    if (changed)
    {
        StopPlayback();
        pending_position = oldest - slider;
        pending_since = ImGui::GetTime();
    }
}

void DrawSettings(const RewindStatus& status)
{
    // Enabled lives beside Show Recorder in the Debug menu. Keeping the
    // capture switch away from the transport avoids confusing machine Pause
    // with the old Stop Recording operation. Reset Data starts a fresh
    // baseline while leaving capture enabled.
    ImGui::BeginDisabled(rewind_get_snapshot_count() == 0);
    if (ImGui::Button("Reset Data", ImVec2(150, 0)))
        gui_debug_rewind_reset_data();
    ImGui::EndDisabled();

    ImGui::Separator();

    if (!status.available)
        ImGui::TextColored(red, "Unavailable: %s",
                           status.unavailable_reason.c_str());
    else if (status.enabled)
        ImGui::TextColored(green, "Recording  %.1fs of %ds   %.2f MB",
                           status.buffered_seconds, status.buffer_seconds,
                           status.memory_bytes / (1024.0 * 1024.0));
    else
        ImGui::TextColored(red, "Disabled  (enable from Debug > Recorder)");

    if (status.available && status.enabled)
        ImGui::TextColored(gray, "%s, %s   %d bytes a frame raw, %.0f stored, %.0fx",
                           status.media_mode.c_str(), status.region.c_str(),
                           (int)status.snapshot_raw_bytes,
                           status.average_compressed_bytes,
                           status.compression_ratio);

    ImGui::Separator();

    // Length is only editable while stopped: changing it under a running ring
    // would either discard frames being looked at or leave the buffer holding
    // a different duration from the one it reports.
    ImGui::BeginDisabled(status.enabled);
    ImGui::Text("Seconds:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    int seconds = config_debug.rewind_seconds;
    if (ImGui::InputInt("##rw_seconds", &seconds, 5, 30))
    {
        if (seconds < 1) seconds = 1;
        if (seconds > 600) seconds = 600;
        config_debug.rewind_seconds = seconds;
    }
    ImGui::EndDisabled();

    if (status.enabled)
    {
        ImGui::SameLine();
        ImGui::TextColored(gray, "(disable to change)");
    }

    if (last_error[0])
        ImGui::TextColored(red, "%s", last_error);

    if (status.snapshots_dropped > 0 || status.serialization_failures > 0)
        ImGui::TextColored(red, "dropped %d, failed %d",
                           status.snapshots_dropped,
                           status.serialization_failures);

}

}

bool gui_debug_rewind_step_back(void)
{
    StopPlayback();
    if (!emu_is_paused())
        emu_pause();
    return SeekNow((pending_position >= 0 ? pending_position
                                          : applied_position) + 1);
}

bool gui_debug_rewind_step_forward(void)
{
    StopPlayback();
    const int position =
        pending_position >= 0 ? pending_position : applied_position;
    if (position <= 0)
        return false;
    if (!emu_is_paused())
        emu_pause();
    return SeekNow(position - 1);
}

bool gui_debug_rewind_resume_from_here(void)
{
    StopPlayback();
    ApplyPendingSeek();

    // With capture disabled, or while already at the live edge, there is no
    // recorded future to discard. The shortcut still acts as Continue rather
    // than becoming mysteriously inert.
    if (!rewind_is_enabled() || rewind_get_seek_position() <= 0)
    {
        emu_debug_continue();
        return false;
    }

    rewind_commit_seek();
    applied_position = 0;
    pending_position = -1;
    grabbed = false;
    emu_debug_continue();
    return true;
}

void gui_debug_rewind_reset_data(void)
{
    StopPlayback();
    rewind_reset();
    applied_position = 0;
    pending_position = -1;
    grabbed = false;
    last_error[0] = 0;
}

void gui_debug_window_rewind(void)
{
    // Applied here rather than from the slider callback: the point of the
    // delay is that nothing happens while the cursor is still moving.
    if (!grabbed || (ImGui::GetTime() - pending_since >= kSettleSeconds))
        ApplyPendingSeek();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::SetNextWindowPos(ImVec2(180, 300), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_FirstUseEver);
    // "Recorder", not "Rewind": rewinding is one of the things it does, and the
    // menu, the buttons and the documentation all say Recorder.
    ImGui::Begin("Recorder", &config_debug.show_rewind,
                 ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::Dummy(ImVec2(360, 0));

    const RewindStatus status = rewind_get_status();
    const int count = rewind_get_snapshot_count();
    const int actual_position = rewind_get_seek_position();
    if (actual_position >= 0 && pending_position < 0)
        applied_position = actual_position;
    else if (actual_position < 0 && !emu_is_execution_stopped())
        applied_position = 0;
    AdvancePlayback(status, count);
    const bool can_scrub = status.available && status.enabled && count > 0 &&
                           emu_is_execution_stopped() && !emu_is_empty();

    DrawTransport(count, can_scrub);
    ImGui::Spacing();
    DrawTimeline(status, count, can_scrub);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    DrawSettings(status);

    ImGui::End();
    ImGui::PopStyleVar();
}
