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

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "imgui/imgui.h"
#include "imgui/colors.h"
#include "config.h"
#include "emu.h"
#include "gui.h"
#include "gui_debug_basic_typer.h"

// Big enough for a screen or two of BASIC. Deliberately not sized for a whole
// program: see the header for why that is the cassette's job.
static const int k_buffer_size = 8 * 1024;

// Shared with the MCP tools, which is why this is not a local static inside
// the window function: external tools can load a fragment into the panel and
// clear it again without selecting the text by hand.
static char buffer[k_buffer_size] = "";
static std::string status;
static ImVec4 status_color = cyan;

// Undoes the \xNN escaping some of these listings carry, so that
// PRINT"\xE5" sends the graphic character rather than six literal ones. \\
// becomes one backslash; anything else after a backslash is left exactly as it
// was, because a lone backslash is a real SC-3000 character (the Yen key) and
// guessing at C escapes would corrupt text that never meant to be escaped.
static std::string decode_escapes(const char* in)
{
    std::string out;

    for (const char* p = in; *p; p++)
    {
        if (*p != '\\' || *(p + 1) == 0)
        {
            out.push_back(*p);
            continue;
        }

        const char next = *(p + 1);
        if (next == '\\')
        {
            out.push_back('\\');
            p++;
            continue;
        }
        if ((next == 'x' || next == 'X') && isxdigit((unsigned char)*(p + 2)) &&
            isxdigit((unsigned char)*(p + 3)))
        {
            const char digits[3] = { *(p + 2), *(p + 3), 0 };
            out.push_back((char)strtol(digits, 0, 16));
            p += 3;
            continue;
        }

        out.push_back(*p);
    }

    return out;
}

// Normalises what a paste gives us into what the SK-1100 can actually type.
//
// Tabs become spaces because the SK-1100 has no tab key. CRLF is left alone:
// QueueText collapses it to one Return itself, so that MCP's keyboard_text
// gets the same treatment without going through here.
//
// Everything else is checked against the matrix itself rather than against
// printable ASCII, which is neither a subset nor a superset of what this
// keyboard can produce. Such characters are dropped and counted, not refused,
// because QueueText rejects the whole string on the first one it cannot type -
// one stray byte would otherwise discard the entire paste with only an offset
// to explain it.
static std::string sanitise(const char* in, int* skipped, std::string* skipped_note)
{
    std::string out;
    int dropped = 0;
    char first_dropped = 0;

    const std::string decoded = config_debug.basic_typer_decode_escapes
        ? decode_escapes(in) : std::string(in);

    for (const char* p = decoded.c_str(); *p; p++)
    {
        const char c = *p;

        if (c == '\r')
        {
            out.push_back(c);
            continue;
        }
        if (c == '\t')
        {
            out.push_back(' ');
            continue;
        }
        if (c == '\n' || emu_keyboard_can_type(c))
        {
            out.push_back(c);
            continue;
        }

        if (dropped == 0)
            first_dropped = c;
        dropped++;
    }

    *skipped = dropped;
    if (dropped > 0)
    {
        char note[160];
        snprintf(note, sizeof(note),
                 "%d character%s dropped, first was $%02X%s",
                 dropped, dropped == 1 ? "" : "s", (unsigned char)first_dropped,
                 // Most of the graphic set is typable through GRAPH mode, but
                 // sixteen of the codes these listings use appear in no
                 // translation map at all - a program that defines its own
                 // patterns can print codes the keyboard was never able to
                 // produce. Say which case this is instead of leaving it
                 // looking like the decode failed.
                 (unsigned char)first_dropped >= 0x80
                     ? " (graphic code that no key produces - only CHR$ can)"
                     : "");
        *skipped_note = note;
    }
    else
        skipped_note->clear();

    return out;
}

// How hard to lean on the machine, in matrix polls. BASIC does not buffer
// keystrokes, so the settle value - the idle hold after Return, and before the
// first key - is what stops it losing the start of a line while it is busy
// tokenising the previous one.
//
// So the wait after Return is fixed and the presets only change the
// keystrokes, because that is where the time actually is. On a 314-character,
// eleven-line listing the keystrokes cost 942 polls at 2+1 against 1570 at
// 3+2, while the line holds cost 300 either way. Buying speed from the hold
// saves seconds and gives up the one thing protecting the start of each line -
// a run with it shortened to ten polls dropped characters exactly there.
//
// Forty polls is deliberately more than the twenty-five verified on a full
// listing from a settled prompt: it is margin for a line that *errors*, which
// takes BASIC longer than a line it simply stores, and which has not been
// measured. On a short fragment that margin costs a tenth of a second a line.
static const int k_newline_settle_polls = 40;

// A 2/1 preset existed and was dropped: Saverio lost characters on it twice.
// That run also had the line hold at ten polls, so 2/1 was never proven to be
// the culprit - but neither was it cleared, and in a tool whose job is to type
// a listing faithfully, ten seconds saved does not pay for a listing that has
// to be checked line by line afterwards.
struct t_typer_speed { const char* name; int press; int release; };
static const t_typer_speed k_speeds[] =
{
    { "Normal", 3, 2 },
    { "Gentle", 4, 3 },
};

static const t_typer_speed& current_speed(void)
{
    int index = config_debug.basic_typer_speed;
    if (index < 0 || index >= (int)(sizeof(k_speeds) / sizeof(k_speeds[0])))
        index = 0;
    return k_speeds[index];
}

// Filters the panel's text and queues it. Shared by the Send button and the
// MCP tool so both report the same thing and neither can bypass the filter.
static bool send_buffer(const char* source)
{
    int skipped = 0;
    std::string skipped_note;
    const std::string text = sanitise(buffer, &skipped, &skipped_note);

    if (text.empty())
    {
        status = "Nothing left to type after filtering";
        status_color = orange;
        return false;
    }

    const t_typer_speed& speed = current_speed();
    emu_keyboard_text_timing(speed.press, speed.release, k_newline_settle_polls);

    std::string error;
    if (!emu_keyboard_text(text.c_str(), source, &error))
    {
        status = error.empty() ? "Unable to queue text" : error;
        status_color = red;
        return false;
    }

    if (skipped > 0)
    {
        status = "Typing. " + skipped_note;
        status_color = orange;
    }
    else
    {
        status = "Typing.";
        status_color = cyan;
    }
    return true;
}

void gui_debug_basic_typer_set_text(const char* text)
{
    if (!text)
        return;
    snprintf(buffer, sizeof(buffer), "%s", text);
    status.clear();
}

void gui_debug_basic_typer_clear(void)
{
    buffer[0] = 0;
    status.clear();
}

const char* gui_debug_basic_typer_get_text(void)
{
    return buffer;
}

bool gui_debug_basic_typer_send(const char* source)
{
    return send_buffer(source ? source : "MCP");
}

void gui_debug_basic_typer_window(void)
{
    ImGui::SetNextWindowPos(ImVec2(220, 200), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520, 360), ImGuiCond_FirstUseEver);

    ImGui::Begin("BASIC Typer", &config_debug.show_basic_typer);
    ImGui::PushFont(gui_default_font);

    const bool busy = emu_keyboard_text_busy();

    ImGui::TextColored(cyan, "Text is typed through the SK-1100 matrix, as if from the keyboard.");
    ImGui::TextDisabled("For commands and short fragments. Load whole programs from cassette instead.");
    ImGui::Separator();

    // The text box takes whatever the window is not using for the rows below
    // it, so resizing the window resizes the editing area. Those rows are
    // measured rather than assumed, and the two that only appear while typing
    // are counted only when they are actually drawn - otherwise the box would
    // leave a permanent empty strip to hold space for them.
    const ImGuiStyle& style = ImGui::GetStyle();
    float footer =
        ImGui::GetFrameHeightWithSpacing() +         // count and escape checkbox
        style.ItemSpacing.y * 2.0f + 1.0f +          // separator
        ImGui::GetFrameHeightWithSpacing();          // Send / Stop / Clear
    if (busy)
    {
        footer += ImGui::GetFrameHeightWithSpacing();
        footer += ImGui::GetTextLineHeightWithSpacing();
        if (emu_is_paused() || emu_is_debugging())
            footer += ImGui::GetTextLineHeightWithSpacing();
    }
    if (!status.empty())
        footer += ImGui::GetTextLineHeightWithSpacing();

    float text_height = ImGui::GetContentRegionAvail().y - footer;
    const float min_height = ImGui::GetTextLineHeight() * 3.0f;
    if (text_height < min_height)
        text_height = min_height;

    // Locked while typing: the queue is built once from a snapshot of this
    // text, so editing mid-send would be misleading rather than useful.
    ImGui::BeginDisabled(busy);
    ImGui::InputTextMultiline("##basic_text", buffer, IM_ARRAYSIZE(buffer),
        ImVec2(-1.0f, text_height), ImGuiInputTextFlags_AllowTabInput);
    ImGui::EndDisabled();

    const int length = (int)strlen(buffer);
    int lines = length > 0 ? 1 : 0;
    for (const char* p = buffer; *p; p++)
        if (*p == '\n')
            lines++;

    // The matrix is polled 50 times a second, so the cost is arithmetic rather
    // than a guess: one keystroke per character plus the idle hold on each
    // line. Shifted and graphic characters cost more than this admits, which
    // is why it says "about".
    const t_typer_speed& speed = current_speed();
    const float polls = (float)length * (speed.press + speed.release) +
                        (float)(lines + 1) * k_newline_settle_polls;
    const float seconds = polls / 50.0f;

    ImGui::TextColored(cyan, "%d characters", length);
    ImGui::SameLine();
    ImGui::TextDisabled("  about %.0f s", seconds);

    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(110.0f);
    const char* names[] = { k_speeds[0].name, k_speeds[1].name };
    int speed_index = config_debug.basic_typer_speed;
    if (ImGui::Combo("##typer_speed", &speed_index, names, IM_ARRAYSIZE(names)))
        config_debug.basic_typer_speed = speed_index;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How long each key is held down.\n\n"
                          "The wait after Return is not adjustable and stays long:\n"
                          "BASIC does not scan the matrix while it tokenises a line,\n"
                          "so shortening that wait saves a couple of seconds and\n"
                          "costs the start of the next line.");

    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    ImGui::Checkbox("Decode \\xNN", &config_debug.basic_typer_decode_escapes);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Turn \\xE5 into the single character $E5 before typing,\n"
                          "and \\\\ into one backslash. Listings that carry graphic\n"
                          "characters this way need it; plain text does not.");

    ImGui::Separator();

    ImGui::BeginDisabled(busy || length == 0);
    if (ImGui::Button("Send", ImVec2(90, 0)))
        send_buffer("panel");
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!busy);
    if (ImGui::Button("Stop", ImVec2(90, 0)))
    {
        emu_keyboard_clear_text();
        status = "Stopped.";
        status_color = orange;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Clear", ImVec2(90, 0)))
        gui_debug_basic_typer_clear();
    ImGui::EndDisabled();

    if (busy)
    {
        const char* source = emu_keyboard_text_source();
        const bool from_panel = strcmp(source, "panel") == 0;
        // Named, not implied: this keyboard has two drivers, and text landing
        // in the machine while the user is doing something else is confusing
        // until you know which one sent it.
        ImGui::TextColored(from_panel ? cyan : orange, "Typing - sent from %s",
                           from_panel ? "this panel" : source);
        ImGui::ProgressBar(emu_keyboard_text_progress(), ImVec2(-1.0f, 0.0f));
        // The machine has to be running to scan the matrix, and it is easy to
        // start a send while paused at a breakpoint and conclude the feature
        // is broken.
        if (emu_is_paused() || emu_is_debugging())
            ImGui::TextColored(orange, "Machine is paused - nothing will be typed until it runs.");
    }

    if (!status.empty())
        ImGui::TextColored(status_color, "%s", status.c_str());

    ImGui::PopFont();
    ImGui::End();
}
