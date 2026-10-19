/*
 * GearSF7000 - the Deep Search panel
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gui_debug_deep_search.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/colors.h"
#include "config.h"
#include "emu.h"
#include "gui.h"
#include "gui_debug_disassembler_view.h"
#include "rom_map_project.h"
#include "../../src/gearsf7000.h"
#include "../../src/DeepSearch.h"
#include "../../src/JumpTables.h"
#include "../../src/RomMap.h"

// Deep Search: the heavy analysis of what the live collection gathered, run
// on request, on another core, in rounds until a round finds nothing new
// (src/DeepSearch.h). The panel runs it, shows each round as it goes, lists
// what it deduced - worst score first, the ones to look at - and keeps a log
// of every run in the project (rommap/deepsearch.log), appended, never
// cleared: what changed in the map, and when (Saverio: the log must stay,
// or the changes are lost).
//
// It pauses the machine if it runs, works on copies, then writes into the
// map - the live one when collecting, saved with the project, otherwise the
// project's map on disk - and into jumptables.txt and deduced.txt, from
// which the map redoes its static rules at every save.

namespace
{
struct Job
{
    std::shared_ptr<std::vector<u8>> rom;
    std::shared_ptr<RomMap> map;
    std::vector<u8> answers;
    std::vector<std::pair<u32, u32>> kept;
    std::vector<JumpTables::Site> sites;
    DeepSearch::Options options;
    u16 base = 0;
    u32 size = 0;
};

struct Panel
{
    DeepSearch::Options options;
    bool requested = false;
    std::future<DeepSearch::Result> job;
    bool running = false;
    std::atomic<int> round{0};
    std::atomic<bool> cancel{false};
    std::chrono::steady_clock::time_point started;
    DeepSearch::Result last;
    bool haveLast = false;
    u16 lastBase = 0;
    std::vector<std::string> log;
    std::string logPath;
    std::string status;
    bool sortByScore = true;
};

Panel panel;

std::string project_file(const char* name)
{
    const std::string dir = rom_map_project_dir();
    return dir.empty() || !rom_map_project_active() ? std::string() : dir + "rommap/" + name;
}

bool write_whole(const std::string& path, const std::string& text)
{
    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << text;
        out.flush();
        if (!out)
            return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}

void load_log(void)
{
    const std::string path = project_file("deepsearch.log");
    if (path == panel.logPath)
        return;
    panel.logPath = path;
    panel.log.clear();
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line))
        if (!line.empty() && line[0] != '#')
            panel.log.push_back(line);
}

// Appended, never rewritten: the log is the history of what the runs did.
void append_log(const std::vector<std::string>& lines)
{
    if (panel.logPath.empty())
        return;
    const bool fresh = !std::filesystem::exists(panel.logPath);
    std::ofstream out(panel.logPath, std::ios::app);
    if (fresh)
        out << "# Deep Search runs: what each found and what it changed in the map. Appended, never cleared.\n";
    for (const std::string& line : lines)
    {
        out << line << "\n";
        panel.log.push_back(line);
    }
}

// The pieces kept as code, from the project's choices.txt.
std::vector<std::pair<u32, u32>> load_kept(void)
{
    std::vector<std::pair<u32, u32>> kept;
    std::ifstream in(project_file("choices.txt"));
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream words(line);
        std::string what, range;
        if (!(words >> what >> range) || what != "code")
            continue;
        const std::size_t dash = range.find('-');
        if (dash == std::string::npos)
            continue;
        kept.push_back({static_cast<u32>(std::strtoul(range.substr(0, dash).c_str(), NULL, 16)),
                        static_cast<u32>(std::strtoul(range.substr(dash + 1).c_str(), NULL, 16)) + 1});
    }
    return kept;
}

// What is not sure yet, as the Disassembler counts it (Saverio: CODE? is
// only a hint, it has to be confirmed): UNKNOWN and CODE? bytes the user
// has not kept as code or as pointers.
u32 not_sure(const RomMap& map)
{
    const std::vector<u8>& bytes = map.GetBytes();
    std::vector<bool> kept(bytes.size(), false);
    for (const auto& piece : load_kept())
        for (u32 o = piece.first; o < piece.second && o < bytes.size(); o++)
            kept[o] = true;
    std::ifstream in(project_file("choices.txt"));
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream words(line);
        std::string what, at;
        if ((words >> what >> at) && what == "word")
        {
            const u32 o = static_cast<u32>(std::strtoul(at.c_str(), NULL, 16));
            for (u32 i = o; i < o + 2 && i < bytes.size(); i++)
                kept[i] = true;
        }
    }
    u32 n = 0;
    for (u32 o = 0; o < bytes.size(); o++)
    {
        const u8 kind = bytes[o] & RomMap::KindMask;
        if (!kept[o] && (kind == RomMap::Kind_Unknown || kind == RomMap::Kind_CodeCandidate))
            n++;
    }
    return n;
}

// The map to read and to write: the live one when collecting, else the
// project's on disk (loaded, and saved back after).
std::shared_ptr<RomMap> disk_map(GearSF7000Core* core, u32 size)
{
    auto map = std::make_shared<RomMap>(core->GetMemory());
    map->Attach(size, core->GetCartridge()->GetCRC());
    map->SetEnabled(true);
    const std::string dir = rom_map_project_dir();
    if (!map->Load(dir + "rommap/rommap.bin") && !map->Load(dir + "rommap.bin"))
        return nullptr;
    map->LoadObservedJumps(dir + "rommap/jumps_seen.txt");
    return map;
}

u16 flat_base(Memory* memory, u32 size)
{
    for (int a = 0; a < 0x10000; a += 0x400)
    {
        u32 offset = 0;
        if (memory->TranslateToRomOffset(static_cast<u16>(a), offset) && offset == 0)
        {
            for (u32 k = 0; k < size; k += 0x400)
                if (!memory->TranslateToRomOffset(static_cast<u16>(a + k), offset) || offset != k)
                    return 0xFFFF;
            return static_cast<u16>(a);
        }
    }
    return 0xFFFF;
}

void start(void)
{
    GearSF7000Core* core = emu_get_core();
    Cartridge* cart = core->GetCartridge();
    Memory* memory = core->GetMemory();
    const u32 size = static_cast<u32>(std::max(0, cart->GetROMSize()));
    const u16 base = flat_base(memory, size);
    if (size == 0 || cart->GetROM() == NULL || base == 0xFFFF)
    {
        panel.status = base == 0xFFFF ? "Deep Search does flat ROMs only for now." : "No ROM.";
        return;
    }
    RomMap* live = core->GetRomMap();
    std::shared_ptr<RomMap> source = (live && live->IsEnabled())
        ? std::make_shared<RomMap>(*live) : disk_map(core, size);
    if (!source || source->GetBytes().size() != size)
    {
        panel.status = "The project's map could not be read.";
        return;
    }
    Job job;
    job.rom = std::make_shared<std::vector<u8>>(cart->GetROM(), cart->GetROM() + size);
    job.map = source;
    job.answers.resize(0x10000);
    for (int a = 0; a < 0x10000; a++)
        job.answers[a] = memory->AnswersAt(static_cast<u16>(a)) ? 1 : 0;
    job.kept = load_kept();
    JumpTables::Load(project_file("jumptables.txt"), job.sites);
    job.options = panel.options;
    job.base = base;
    job.size = size;
    panel.lastBase = base;
    panel.round = 0;
    panel.cancel = false;
    panel.started = std::chrono::steady_clock::now();
    panel.running = true;
    panel.status = "Searching...";
    panel.job = std::async(std::launch::async, [job]() {
        return DeepSearch::Run(job.rom->data(), job.size, job.base, *job.map, job.answers, job.kept,
                               job.sites, job.options, &panel.round, &panel.cancel);
    });
}

// The result into the files and the map, while paused, and into the log.
void finish(DeepSearch::Result result)
{
    GearSF7000Core* core = emu_get_core();
    Cartridge* cart = core->GetCartridge();
    const u32 size = static_cast<u32>(std::max(0, cart->GetROMSize()));
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - panel.started).count();
    if (result.cancelled)
    {
        panel.status = "Cancelled: nothing was written.";
        append_log({"cancelled"});
        return;
    }

    // Files first: the map redoes its static rules from them.
    write_whole(project_file("jumptables.txt"), JumpTables::Format(result.sites));
    write_whole(project_file("deduced.txt"), DeepSearch::FormatDeduced(result.blocks));
    write_whole(project_file("datahints.txt"), DeepSearch::FormatDataHints(result.dataHints));

    RomMap* live = core->GetRomMap();
    const bool collecting = live && live->IsEnabled();
    std::shared_ptr<RomMap> diskCopy = collecting ? nullptr : disk_map(core, size);
    RomMap* map = collecting ? live : diskCopy.get();
    std::string changed = "map not written";
    if (map)
    {
        const u32 unsureBefore = not_sure(*map);
        const u32 guessBefore = map->GetCount(RomMap::Kind_CodeCandidate);
        rom_map_project_apply_jump_tables(map);     // both files, into the map
        map->MarkFill(cart->GetROM(), size);
        map->MarkStaticTables(cart->GetROM(), size);
        map->MarkBranchCandidates(cart->GetROM(), size);
        if (!collecting)
            rom_map_project_backup(rom_map_project_dir());
        const bool saved = collecting ? rom_map_project_save()
                                      : map->Save(rom_map_project_dir() + "rommap/rommap.bin");
        // Hints are not progress: Unknown moves only when a table is found
        // (its bytes are data); CODE? blocks are counted apart.
        const int hints = static_cast<int>(map->GetCount(RomMap::Kind_CodeCandidate)) -
                          static_cast<int>(guessBefore);
        char text[128];
        std::snprintf(text, sizeof(text), "Unknown %u -> %u, code? hints %+d%s", unsureBefore,
                      not_sure(*map), hints, saved ? "" : " (NOT SAVED)");
        changed = text;
    }
    gui_debug_disassembler_view_reload();

    // The log: a line for the run, one for each round.
    char stamp[32];
    const time_t now = time(nullptr);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", localtime(&now));
    int tables = 0, entries = 0, blocks = 0;
    u32 bytes = 0;
    for (const DeepSearch::Round& r : result.rounds)
    {
        tables += r.tables;
        entries += r.entries;
        blocks += r.blocks;
        bytes += r.bytes;
    }
    std::vector<std::string> lines;
    char line[384];
    u32 hinted = 0;
    for (const RomMap::DataHint& h : result.dataHints)
        hinted += h.end - h.start;
    std::snprintf(line, sizeof(line), "%s  %zu round%s in %.2f s: +%d jump table%s (%d entries), "
                  "+%d code? block%s (%u bytes), %zu data? hint%s (%u bytes) - %s", stamp,
                  result.rounds.size(), result.rounds.size() == 1 ? "" : "s", seconds, tables,
                  tables == 1 ? "" : "s", entries, blocks, blocks == 1 ? "" : "s", bytes,
                  result.dataHints.size(), result.dataHints.size() == 1 ? "" : "s", hinted, changed.c_str());
    lines.push_back(line);
    for (const DeepSearch::Round& r : result.rounds)
    {
        std::snprintf(line, sizeof(line), "    round %d: +%d tables (%d entries), +%d blocks (%u bytes), %d dropped",
                      r.round, r.tables, r.entries, r.blocks, r.bytes, r.dropped);
        lines.push_back(line);
    }
    append_log(lines);
    panel.status = lines.front().substr(std::strlen(stamp) + 2);
    panel.last = std::move(result);
    panel.haveLast = true;
}

void draw_blocks(void)
{
    if (!panel.haveLast || panel.last.blocks.empty())
    {
        ImGui::TextColored(gray, "No deduced code yet: run Deep Search.");
        return;
    }
    std::vector<const DeepSearch::Block*> order;
    for (const DeepSearch::Block& b : panel.last.blocks)
        order.push_back(&b);
    if (panel.sortByScore)
        std::stable_sort(order.begin(), order.end(),
                         [](const DeepSearch::Block* a, const DeepSearch::Block* b) { return a->score < b->score; });
    ImGui::Checkbox("Least like the game's code first", &panel.sortByScore);
    if (ImGui::BeginTable("##blocks", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_BordersInnerV,
                          ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 10.0f)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Code?", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000-0000").x);
        ImGui::TableSetupColumn("Like", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("100%").x);
        ImGui::TableSetupColumn("Why");
        ImGui::TableSetupColumn("Round", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Round").x);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(order.size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
            {
                const DeepSearch::Block& b = *order[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char range[24];
                std::snprintf(range, sizeof(range), "%04X-%04X##%d", panel.lastBase + b.start,
                              panel.lastBase + b.end - 1, i);
                if (ImGui::Selectable(range, false, ImGuiSelectableFlags_SpanAllColumns))
                    gui_debug_disassembler_view_goto(static_cast<u16>(panel.lastBase + b.start));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Click to see it in the Disassembler");
                ImGui::TableNextColumn();
                const ImVec4 tone = b.score >= 80 ? ImVec4(0.55f, 0.80f, 0.55f, 1.0f)
                                  : b.score >= 50 ? ImVec4(0.90f, 0.75f, 0.35f, 1.0f)
                                                  : ImVec4(0.95f, 0.40f, 0.35f, 1.0f);
                ImGui::TextColored(tone, "%d%%", b.score);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(b.reason.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%d", b.round);
            }
        ImGui::EndTable();
    }
}
}

void gui_debug_deep_search_open(void)
{
    config_debug.show_deep_search = true;
    ImGui::SetWindowFocus("Deep Search###deep_search");
}

void gui_debug_deep_search_window(void)
{
    gui_set_next_window_size_em(34.4f, 28.9f);
    if (!ImGui::Begin("Deep Search###deep_search", &config_debug.show_deep_search))
    {
        ImGui::End();
        return;
    }
    load_log();

    // A finished run: into the files and the map, and the log.
    if (panel.running && panel.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        panel.running = false;
        finish(panel.job.get());
    }
    // A requested run starts once the pause it asked for has landed.
    if (panel.requested && emu_is_execution_stopped() && !panel.running)
    {
        panel.requested = false;
        start();
    }

    const bool project = rom_map_project_active();
    if (!project)
    {
        ImGui::TextColored(gray, "Deep Search works inside a project: open the ROM's project first.");
        ImGui::End();
        return;
    }

    ImGui::TextColored(gray, "Works outward from what the live collection proved, round after round,");
    ImGui::TextColored(gray, "until a round finds nothing new. What it deduces is CODE?, never proof.");
    ImGui::Separator();
    ImGui::Checkbox("Jump tables", &panel.options.jumpTables);
    ImGui::SetItemTooltip("Reads the code before every jp (hl)/(ix)/(iy) for the table it jumps through");
    ImGui::SameLine();
    ImGui::Checkbox("Guided disassembly", &panel.options.guided);
    ImGui::SameLine();
    ImGui::Checkbox("Data", &panel.options.data);
    ImGui::SetItemTooltip("For the bytes still not sure: the code that names the table around them\n"
                          "says the size of an entry, the bytes read, the index and the count, and\n"
                          "whether the words are addresses; between pointers, words into the\n"
                          "cartridge are pointers too. Suggestions (data?), shown in To Check.");
    ImGui::SetItemTooltip("Follows the flow out of known code into unknown bytes, block by block,\n"
                          "dropping a block at the first sign it is not code");

    const bool busy = panel.running || panel.requested;
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Run Deep Search", ImVec2(ImGui::CalcTextSize("Run Deep Search").x + 24.0f, 0.0f)))
    {
        if (!emu_is_execution_stopped())
            emu_pause();
        panel.requested = true;
        panel.status = "Pausing...";
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Pauses the machine if it runs, then works on another core");
    ImGui::SameLine();
    if (panel.running)
    {
        // Shown for at least half a second, so a run is seen to happen.
        char text[48];
        std::snprintf(text, sizeof(text), "Round %d...", panel.round.load());
        ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(160.0f, 0.0f), text);
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel"))
            panel.cancel = true;
    }
    else
        ImGui::TextColored(panel.status.find("NOT SAVED") != std::string::npos ? red : cyan, "%s",
                           panel.status.c_str());

    ImGui::Separator();
    ImGui::TextColored(white, "Deduced code (CODE?)");
    draw_blocks();

    ImGui::Separator();
    ImGui::TextColored(white, "Log");
    ImGui::SameLine();
    ImGui::TextColored(gray, "rommap/deepsearch.log - kept, never cleared");
    if (ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders))
    {
        for (const std::string& line : panel.log)
            ImGui::TextColored(line.compare(0, 4, "    ") == 0 ? gray : white, "%s", line.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f)
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}
