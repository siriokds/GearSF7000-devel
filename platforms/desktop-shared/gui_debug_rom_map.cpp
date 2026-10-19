/*
 * GearSF7000 - Live Disassembler window (the ROM map, classified while the game runs)
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gui_debug_rom_map.h"
#include "gui_debug_deep_search.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "imgui/imgui.h"
#include "imgui/colors.h"
#include "config.h"
#include "emu.h"
#include "gui.h"
#include "rom_map_project.h"
#include "../../src/gearsf7000.h"
#include "../../src/RomMap.h"

// Laid out like Debug Events, and kept simple on purpose (Saverio: "non
// così complesso, ma comunque con delle informazioni"): the ROM and its
// project, one switch plus Reset with the coverage, then the ROM as ranges in
// address order. A ROM never seen is a single UNKNOWN range; play splits it.
// DOCS/PROGRAM_ANALYSIS_AUDIT.md section 12.

ImVec4 gui_debug_rom_map_kind_color(unsigned char kind)
{
    switch (kind)
    {
    case RomMap::Kind_Code: return green;
    case RomMap::Kind_Data: return white;
    case RomMap::Kind_GfxBg: return orange;
    case RomMap::Kind_GfxSpr: return magenta;
    case RomMap::Kind_Gfx: return yellow;
    case RomMap::Kind_Screen: return cyan;
    case RomMap::Kind_Sat: return violet;
    case RomMap::Kind_Vram: return blue;
    case RomMap::Kind_Sound: return red;
    case RomMap::Kind_Fill: return mid_gray;
    case RomMap::Kind_CodeCandidate: return ImVec4(0.55f, 0.80f, 0.55f, 1.0f); // pale green: a candidate, not executed
    case RomMap::Kind_Table: return ImVec4(0.95f, 0.75f, 0.45f, 1.0f);
    default: return gray;
    }
}

static char new_project_name[80] = "";

void gui_debug_rom_map_window(void)
{
    GearSF7000Core* core = emu_get_core();
    RomMap* map = core ? core->GetRomMap() : nullptr;

    // Geometry in em so the window follows the monitor DPI and the interface
    // scale; the upper bound is the work area rather than a 1100x1000 literal,
    // which on a 4K panel was smaller than the content wanted.
    const float chrome_em = ImGui::GetFontSize();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // The minimum is clamped to the work area too: at a large scale on a small
    // display an em-based minimum can exceed it, and a minimum above the
    // maximum is not a constraint ImGui can satisfy.
    const ImVec2 available(std::max(1.0f, viewport->WorkSize.x),
                           std::max(1.0f, viewport->WorkSize.y));
    ImGui::SetNextWindowPos(ImVec2(18.9f * chrome_em, 6.7f * chrome_em), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(28.9f * chrome_em, 23.3f * chrome_em), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(std::min(23.3f * chrome_em, available.x),
               std::min(11.1f * chrome_em, available.y)),
        available);
    ImGui::Begin("Live Disassembler", &config_debug.show_rom_map);
    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    // Inside the pushed debugger font, so this em differs from chrome_em.
    const float em = ImGui::GetFontSize();

    if (!map)
    {
        ImGui::TextColored(gray, "Not available in this build.");
        ImGui::PopFont();
        ImGui::End();
        return;
    }

    // --- the ROM and its project
    Cartridge* cartridge = core->GetCartridge();
    // The ROM is there whether or not a project holds it: without one the
    // window says so and offers to make it.
    const bool hasRom = cartridge->IsReady() && cartridge->GetROMSize() > 0 &&
                        !core->GetMemory()->IsSF7000Enabled();
    const bool hasProject = rom_map_project_active();
    if (hasRom)
    {
        // Three short lines instead of two long ones that ran out of the
        // window: the file, its identity, and the project folder by name only
        // (the full path is in the tooltip).
        ImGui::TextColored(cyan, "ROM");
        ImGui::SameLine();
        ImGui::TextUnformatted(cartridge->GetFileName());
        if (map->IsCollecting())
            ImGui::TextColored(gray, "%u bytes  |  CRC %08X  |  %s  |  sessions %u", map->GetRomSize(),
                               map->GetCrc(), GetSystemModelName(core->GetSystemModel()),
                               map->GetSessions());
        else
            ImGui::TextColored(gray, "%u bytes  |  CRC %08X  |  %s", static_cast<u32>(cartridge->GetROMSize()),
                               cartridge->GetCRC(), GetSystemModelName(core->GetSystemModel()));
        if (!hasProject)
        {
            ImGui::TextColored(yellow, "NO PROJECT");
            ImGui::SameLine();
            if (ImGui::SmallButton("New Project..."))
            {
                snprintf(new_project_name, sizeof(new_project_name), "%s", cartridge->GetFileName());
                char* dot = strrchr(new_project_name, '.');
                if (dot && dot != new_project_name)
                    *dot = 0;
                ImGui::OpenPopup("New Project");
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The Live Disassembler works inside a project.");
        }
        else
        {
        const std::string dir = rom_map_project_dir();
        std::string folder = dir;
        while (!folder.empty() && (folder.back() == '/' || folder.back() == '\\'))
            folder.pop_back();
        const size_t slash = folder.find_last_of("/\\");
        if (slash != std::string::npos)
            folder = folder.substr(slash + 1);
        ImGui::TextColored(gray, "project: %s", folder.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", dir.c_str());
        // Like Arduino IDE's Show Sketch Folder: the OS file manager opens the
        // folder (Finder, Explorer, the desktop's own). It exists from the first
        // save on.
        ImGui::SameLine();
        SDL_PathInfo info;
        const bool exists = !dir.empty() && SDL_GetPathInfo(dir.c_str(), &info) &&
                            info.type == SDL_PATHTYPE_DIRECTORY;
        ImGui::BeginDisabled(!exists);
        if (ImGui::SmallButton("Open Project Folder"))
            rom_map_project_open_folder(dir);
        ImGui::EndDisabled();
        if (!exists && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The folder is created at the first save.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Open Deep Search"))
            gui_debug_deep_search_open();
        }
    }
    else
    {
        ImGui::TextColored(cyan, "ROM");
        ImGui::SameLine();
        ImGui::TextColored(gray, "no cartridge ROM loaded");
    }

    if (ImGui::BeginPopupModal("New Project", NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("A project for this ROM, in the projects folder:");
        ImGui::TextColored(gray, "%s", rom_map_project_root().c_str());
        ImGui::SetNextItemWidth(27.7f * em);
        const bool enter = ImGui::InputText("Name", new_project_name, sizeof(new_project_name),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextColored(gray, "The ROM is copied into reference/. The Live Disassembler adds");
        ImGui::TextColored(gray, "what it observes to the project; nothing is ever taken away.");
        const char* why = rom_map_project_last_error();
        if (why && why[0] && !hasProject)
            ImGui::TextColored(red, "%s", why);
        if (ImGui::Button("Create", ImVec2(7.7f * em, 0.0f)) || enter)
        {
            if (rom_map_project_create(new_project_name))
                ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(7.7f * em, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // --- one switch: enabled means contributing to the project
    bool enabled = map->IsEnabled();
    ImGui::BeginDisabled(!hasProject && !enabled);
    if (ImGui::Checkbox("Contribute", &enabled))
        rom_map_project_set_enabled(enabled);
    ImGui::EndDisabled();
    if (!hasProject && !enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The Live Disassembler works inside a project: make one first.");
    ImGui::SameLine();
    if (map->IsCollecting())
        ImGui::TextColored(green, "ACCUMULATING");
    else if (enabled)
        ImGui::TextColored(yellow, hasRom ? "WAITING FOR A PROJECT" : "WAITING FOR A ROM");
    else
        ImGui::TextColored(gray, "OFF");
    ImGui::SameLine();
    ImGui::BeginDisabled(!map->IsCollecting());
    // Autosave runs every 30 s; this is for "those were minutes of play,
    // put them on disk now", with the time as the proof that it happened.
    if (ImGui::Button("Save now"))
        rom_map_project_save();
    ImGui::SameLine();
    if (ImGui::Button("Reset"))
        ImGui::OpenPopup("Reset ROM map?");
    ImGui::EndDisabled();
    if (map->IsCollecting())
    {
        ImGui::SameLine();
        const time_t saved = static_cast<time_t>(rom_map_project_last_save_time());
        if (saved != 0)
        {
            char when[16];
            strftime(when, sizeof(when), "%H:%M:%S", localtime(&saved));
            ImGui::TextColored(rom_map_project_is_dirty() ? yellow : gray,
                               rom_map_project_is_dirty() ? "saved %s, changed since" : "saved %s", when);
        }
        else
            ImGui::TextColored(yellow, "not saved yet");
    }

    if (ImGui::BeginPopupModal("Reset ROM map?", NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Back to 100% UNKNOWN, as a ROM never seen.");
        ImGui::TextColored(gray, "What the project holds is copied aside first, in rommap/.");
        if (ImGui::Button("Reset", ImVec2(7.7f * em, 0.0f)))
        {
            rom_map_project_reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(7.7f * em, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    const char* error = rom_map_project_last_error();
    if (error && error[0])
        ImGui::TextColored(red, "%s", error);

    if (!map->IsCollecting())
    {
        ImGui::Separator();
        if (hasRom && !hasProject)
            ImGui::TextColored(gray, "This ROM has no project. New Project makes one.");
        else
            ImGui::TextColored(gray, enabled ? "Load a cartridge ROM to start."
                                             : "Contribute to add what is observed to the project. What is there stays.");
        ImGui::PopFont();
        ImGui::End();
        return;
    }

    // --- coverage: 100% minus UNKNOWN and CODE?, then each kind. CODE? is
    // only a hint, to be confirmed: never covered (Saverio).
    const double size = static_cast<double>(map->GetRomSize());
    const double unknown = 100.0 * (map->GetCount(RomMap::Kind_Unknown) +
                                    map->GetCount(RomMap::Kind_CodeCandidate)) / size;
    ImGui::Separator();
    ImGui::TextColored(cyan, "COVERAGE %.1f%%", 100.0 - unknown);
    ImGui::SetItemTooltip("What the map knows: not UNKNOWN, not CODE? (a hint, never covered).\n"
                          "Code you kept by hand counts in the Disassembler's Unknown.");

    // One row per kind present, vertically: a single line of labels grew with
    // every kind added and wrapped unreadably (Saverio, on Champion Tennis).
    if (ImGui::CollapsingHeader("Summary", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const ImGuiTableFlags summaryFlags = ImGuiTableFlags_SizingFixedFit |
                                             ImGuiTableFlags_RowBg;
        if (ImGui::BeginTable("##rom_map_summary", 4, summaryFlags))
        {
            ImGui::TableSetupColumn("TYPE");
            ImGui::TableSetupColumn("BYTES");
            ImGui::TableSetupColumn("%");
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 12.3f * em);
            for (u8 kind = 0; kind < RomMap::Kind_Count; ++kind)
            {
                const u32 bytes = map->GetCount(static_cast<RomMap::Kind>(kind));
                if (bytes == 0 && kind != RomMap::Kind_Unknown)
                    continue;
                const float fraction = static_cast<float>(bytes / size);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(gui_debug_rom_map_kind_color(kind), "%s", RomMap::KindName(kind));
                ImGui::TableNextColumn();
                ImGui::Text("%6u", bytes);
                ImGui::TableNextColumn();
                ImGui::Text("%5.1f%%", 100.0f * fraction);
                ImGui::TableNextColumn();
                ImVec4 bar = gui_debug_rom_map_kind_color(kind);
                bar.w = 0.85f;
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar);
                ImGui::ProgressBar(fraction, ImVec2(160.0f, ImGui::GetTextLineHeight() * 0.7f), "");
                ImGui::PopStyleColor();
            }
            ImGui::EndTable();
        }
    }

    // --- ranges in address order. Rebuilt only when the map changed, and at
    // most twice a second: while playing it changes almost every frame at
    // first, and a list that jumps sixty times a second cannot be read.
    static std::vector<RomMap::Range> ranges;
    static u64 builtGeneration = ~0ull;
    static u64 builtTicks = 0;
    const u64 now = SDL_GetTicks();
    if (map->GetGeneration() != builtGeneration && (now - builtTicks >= 500 || ranges.empty()))
    {
        ranges = map->BuildRanges();
        builtGeneration = map->GetGeneration();
        builtTicks = now;
    }
    ImGui::TextColored(gray, "%d ranges", static_cast<int>(ranges.size()));

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##rom_map_ranges", 5, flags))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("START");
        ImGui::TableSetupColumn("END");
        ImGui::TableSetupColumn("BYTES");
        ImGui::TableSetupColumn("TYPE");
        ImGui::TableSetupColumn("EVIDENCE", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        const bool wide = map->GetRomSize() > 0x10000;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(ranges.size()));
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const RomMap::Range& range = ranges[row];
                ImVec4 color = gui_debug_rom_map_kind_color(range.kind);
                if (range.candidate)
                    color = ImVec4(color.x * 0.7f, color.y * 0.7f, color.z * 0.7f, color.w);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text(wide ? "$%06X" : "$%04X", range.start);
                ImGui::TableNextColumn();
                ImGui::Text(wide ? "$%06X" : "$%04X", range.end);
                ImGui::TableNextColumn();
                ImGui::Text("%u", range.end - range.start + 1);
                ImGui::TableNextColumn();
                ImGui::TextColored(color, "%s", RomMap::RangeName(range).c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(gray, "%s", map->DescribeEvidence(range).c_str());
            }
        }
        ImGui::EndTable();
    }

    ImGui::PopFont();
    ImGui::End();
}
