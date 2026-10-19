/*
 * GearSF7000 - the new Disassembler window, built up next to the old one
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gui_debug_disassembler_view.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <future>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/colors.h"
#include "imgui/fonts/IconsMaterialDesign.h"
#include "config.h"
#include "emu.h"
#include "gui.h"
#include "gui_debug.h"
#include "gui_debug_rom_map.h"
#include "gui_debug_deep_search.h"
#include "rom_map_project.h"
#include "../../src/gearsf7000.h"
#include "../../src/JumpTables.h"
#include "../../src/RomMap.h"
#include "../../src/SourceListing.h"
#include "../../src/Z80Disassembler.h"
#include "../../src/opcode_timing.h"

// The new Disassembler window,
// grown one piece at a time beside the old window, which stays untouched
// until this one does everything it does. Step 1: the compact shell - the
// command bar, Go To with Back/Forward, and the listing of what the debugger
// has decoded, as today. What changes from the old window:
//   - the listing scrolls only when the PC leaves the 10%-90% band, putting
//     it back at 30% or 80%, as Emulicious does; it never recentres;
//   - a click, or scrolling by hand, suspends Follow PC and the selection
//     stays; the target button resumes following;
//   - the line list is rebuilt a few times a second, not every frame;
//   - a status bar with two fixed lines says what the selection (or the PC)
//     is: address and name, value, size and T-states, kind;
//   - where the decoded code is not contiguous, a gap row says how many
//     bytes in between were never decoded, and opens to show them raw;
//   - with a project, the cartridge is the whole ROM as source
//     (src/SourceListing.h: the export's rules and names), data in blocks
//     that fold - unknown ones closed, the rest open. BIOS and RAM stay as
//     the debugger decoded them.
// Its state is its own, not shared with the old window.
// Registers, stack, memory and events live in their own windows.

namespace
{
enum LineType : u8
{
    Line_Instruction,
    Line_Label,
    Line_Gap,       // bytes between two instructions nothing decoded, closed
    Line_GapBytes,  // up to 16 of those bytes, when the gap is open
    Line_Source,    // a line of the cartridge source (View::listing)
    Line_Blank,     // spacing in the source: before data, after a routine
    Line_More,      // "... N more bytes": the rest of a long data block
    Line_PreviewHead,        // "; try as code from 4CA" (click closes it)
    Line_PreviewInstruction, // an instruction of the preview
    Line_PreviewStop,        // "; stops: ..." and what argues against code
    Line_PreviewLabel,       // a label the preview would give, if kept
    Line_KeptHead,           // "; kept as code by you: 49F-4AF  [forget]"
    Line_KeptWordHead        // "; kept as a pointer by you: 2AC5  [forget]"
};

struct Line
{
    u16 address;
    u8 type;
    u16 count;  // Line_Gap: bytes in the gap; Line_GapBytes: bytes on the row
    int label;  // Line_Label: index into View::labels
    // Gap rows have no record of their own: where they sit, from the
    // instruction before them.
    char segment[5];
    int bank;
    int source = -1;    // Line_Source: index into View::listing lines
};

struct Range
{
    u16 start;
    u16 end;    // inclusive
};

// What a build takes along: copies, so nothing it reads changes under it.
struct SourceJob
{
    std::shared_ptr<std::vector<u8>> rom;
    std::shared_ptr<RomMap> map;
    std::shared_ptr<std::unordered_map<u16, std::string>> names;
    std::shared_ptr<std::unordered_map<u16, std::string>> ports;
    std::shared_ptr<std::vector<u8>> answers;
    std::vector<SourceListing::CodeRange> choices;
    std::vector<u32> keptWords;
    std::vector<JumpTables::Site> sites;
    u16 base = 0;
    u32 size = 0;
    u32 crc = 0;
    u64 generation = 0;
    u32 epoch = 0;
    int symbolCount = 0;
    const RomMap* origin = nullptr;
};

// Complete before View, which holds a std::future of it.
struct SourceResult
{
    SourceJob job;
    SourceListing listing;
    std::vector<JumpTables::Site> sites;
    int pending = 0;            // jumps no Deep Search has read yet
};

struct View
{
    bool follow = true;
    bool hasSelection = false;
    u16 selection = 0;

    bool showBytes = false;
    bool showAddresses = true;  // the source view's address column
    bool showMinimap = true;    // the code in miniature beside the listing
    bool showSymbols = true;
    bool showSegment = true;
    bool showBank = true;

    std::vector<Line> lines;
    std::vector<std::string> labels;
    // For each CPU address, the last line (not a label) starting at or
    // before it, -1 before the first.
    std::vector<int> lineAtOrBefore = std::vector<int>(0x10000, -1);
    // What was opened: a gap is open when it touches one of these, so the
    // pieces of a gap the code later runs into stay open.
    std::vector<Range> openGaps;
    bool built = false;
    bool forceRebuild = false;
    u64 builtTicks = 0;
    bool builtStopped = false;
    int builtPc = -1;
    bool builtSymbols = true;
    int builtSymbolCount = -1;

    // Positions visited with Go To / double-click; historyPos is where we are.
    std::vector<u16> history;
    int historyPos = -1;

    // A Go To asks for its line at 30% of the listing on the next draw.
    bool scrollRequested = false;
    u16 scrollTarget = 0;
    // Scrolling by hand while following suspends it: the listing remembers
    // where it left the scroll, and whether it moved it itself.
    float lastScrollY = -1.0f;
    bool scrolledByUs = false;

    char gotoText[40] = "";
    std::string message;

    // The cartridge as source, while the loaded ROM has a project.
    SourceListing listing;
    bool sourceActive = false;
    u16 sourceBase = 0;
    u32 sourceSize = 0;
    u32 sourceCrc = 0;
    u64 sourceGeneration = ~0ull;
    u64 sourceTicks = 0;
    int sourceSymbolCount = -1;
    std::string sourceNote;     // why there is no source view, when there is a project
    // How each data block is shown, by the ROM offset it starts at; a block
    // not in here shows its first rows (Block_Preview).
    std::unordered_map<u32, u8> blockState;
    // The project's map read from disk, when the Live Disassembler is not
    // collecting: looking at a project must not need collection on.
    std::unique_ptr<RomMap> diskMap;
    const RomMap* sourceMap = nullptr;  // the map the listing was built from
    // User symbols by address, for the listing: kept here because the
    // listing keeps the lookup and calls it again later.
    std::unordered_map<u16, std::string> sourceNames;

    // "Try as code": a preview decoded from one byte of a data block, shown
    // under its row. Nothing is written to the map.
    bool previewActive = false;
    u16 previewStart = 0;
    std::vector<std::string> previewTexts;
    bool openByteMenu = false;
    u16 byteMenuAddress = 0;
    bool openCodeMenu = false;
    u16 codeMenuAddress = 0;

    // What the user keeps as code, from the project's rommap/choices.txt;
    // the undo stack holds what it was before each change.
    std::vector<SourceListing::CodeRange> choices;
    // And the words the user keeps as pointers (ROM offsets), same file.
    std::vector<u32> keptWords;
    // The board's port names (a .ports file), keyed port | 0x100 when out.
    std::unordered_map<u16, std::string> portNames;
    std::string portsFile;
    std::string choicesPath;
    std::vector<std::pair<std::vector<SourceListing::CodeRange>, std::vector<u32>>> choicesUndo;
    bool sourceDirty = false;

    // Jump tables: what the analysis found at each executed jp (rr), kept in
    // rommap/jumptables.txt. A jump with no result, or whose code has changed
    // since (signature), is pending and gets analysed at the next pause -
    // never while the machine runs, and never left pending.
    std::vector<JumpTables::Site> jumpSites;
    std::string jumpPath;
    u64 jumpGeneration = ~0ull;

    // The listing is built - and pending jumps analysed - on another core,
    // from copies of the ROM, the map, the symbols and the choices taken when
    // it starts: the game never waits for this window (Saverio: use the
    // cores). The listing shown stays until the new one is ready.
    std::future<SourceResult> job;
    bool jobRunning = false;
    u32 jobEpoch = 0;                   // bumped when what the job was for goes
    const RomMap* sourceOrigin = nullptr;   // the live or disk map the listing came from
    std::shared_ptr<RomMap> heldMap;    // the copies the shown listing points into
    std::shared_ptr<std::vector<u8>> heldRom;
    std::shared_ptr<std::vector<u8>> answers;   // what answers at each address
    u32 answersCrc = 0;

    // Deep Search runs in its own panel (gui_debug_deep_search.cpp); here
    // only the count of the jumps it has not read yet, for the button.
    int deepPending = 0;
    int currentPc = 0;      // this frame's PC, for navigation started deep in drawing

    // Find (Cmd/Ctrl+F): text looked for in the lines as shown.
    char findText[64] = "";
    std::vector<int> findHits;
    int findIndex = -1;         // which hit is shown, into findHits
    std::string findFor;        // the text findHits were built for
    std::size_t findLines = 0;  // and the line count they were built on
    bool focusFind = false;
    // A link clicked while drawing, followed after the rows.
    bool linkClicked = false;
    u16 linkTarget = 0;
    u16 linkFrom = 0;           // the row the link is on: where Back returns
    u16 paintingAddress = 0;    // the row being painted
    // After a link's click, until the button is up: the release must not
    // select whatever row the jump has brought under the mouse.
    bool suppressRowClick = false;

    // The navigator (demo3's strip) in place of the vertical scrollbar, and
    // the horizontal scrollbar drawn under the listing: what the listing
    // child measured last frame, and the scroll asked of it for this one.
    float listScrollY = 0.0f, listScrollMaxY = 0.0f, listHeight = 0.0f;
    float listScrollX = 0.0f, listScrollMaxX = 0.0f, listWidth = 0.0f;
    float requestScrollY = -1.0f, requestScrollX = -1.0f;
    bool stripDragging = false;
    float stripGrab = 0.0f;     // where in the frame it was grabbed
    bool miniDragging = false;
    float miniGrab = 0.0f;
    float miniSlider = 0.0f;    // the minimap slider's opacity, fading in and out
    float miniScroll = 0.0f;    // the minimap's own scroll, moved only to keep the slider in
    int miniFirst = 0, miniRows = 0;    // the lines the minimap shows, for the ruler
    u32 diskMapCrc = 0;
    std::string diskMapPath;
};

View view;

// The line list is rebuilt this often at most while running.
const u64 kRebuildMs = 100;

u32 symbol_key(int bank, u16 address)
{
    return (static_cast<u32>(bank + 1) << 16) | address;
}

std::unordered_map<u32, std::string> collect_symbols(void)
{
    std::unordered_map<u32, std::string> result;
    const int count = gui_debug_symbol_count();
    for (int i = 0; i < count; i++)
    {
        int bank = 0;
        u16 address = 0;
        const char* text = NULL;
        if (gui_debug_get_symbol(i, &bank, &address, &text) && text)
            result[symbol_key(bank, address)] = text;
    }
    return result;
}

const char* symbol_for(const std::unordered_map<u32, std::string>& symbols,
                       Memory::stDisassembleRecord* record, u16 address)
{
    const int bank = IsValidPointer(record) ? record->bank : 0;
    auto it = symbols.find(symbol_key(bank, address));
    if (it != symbols.end())
        return it->second.c_str();
    if (IsValidPointer(record) && record->auto_symbol[0] != 0)
        return record->auto_symbol;
    return NULL;
}

bool gap_is_open(u16 start, u16 end)
{
    for (const Range& r : view.openGaps)
        if (r.start <= end && r.end >= start)
            return true;
    return false;
}

void set_gap_open(u16 start, u16 end, bool open)
{
    if (open)
        view.openGaps.push_back({start, end});
    else
        view.openGaps.erase(std::remove_if(view.openGaps.begin(), view.openGaps.end(),
            [&](const Range& r) { return r.start <= end && r.end >= start; }),
            view.openGaps.end());
    view.forceRebuild = true;
}

// The ROM listing is rebuilt at most this often while the map grows.
const u64 kSourceRebuildMs = 500;

// Every block starts open, unknown ones too (Saverio: what is not
// recognised is what he reads), but a long one shows only its first rows and
// a "... N more bytes" row: 8K of graphics must not bury the listing.
enum BlockState : u8
{
    Block_Folded,
    Block_Preview,
    Block_Full
};
const int kPreviewRows = 8;

u8 block_state(const SourceListing::Line& header)
{
    auto it = view.blockState.find(header.offset);
    return it != view.blockState.end() ? it->second : static_cast<u8>(Block_Preview);
}

bool block_is_open(const SourceListing::Line& header)
{
    return block_state(header) != Block_Folded;
}

void set_block_state(u32 offset, u8 state)
{
    view.blockState[offset] = state;
    view.forceRebuild = true;
}

void set_block_open(u32 offset, bool open)
{
    set_block_state(offset, open ? Block_Preview : Block_Folded);
}

int line_of(u16 address);

void start_preview(u16 address)
{
    // The whole block, so the preview is never inside a folded part.
    const int index = line_of(address);
    if (index >= 0 && view.sourceActive &&
        (view.lines[index].type == Line_Source || view.lines[index].type == Line_More))
    {
        const SourceListing::Line& source = view.listing.GetLines()[view.lines[index].source];
        const int header = source.type == SourceListing::Line_DataHeader ? view.lines[index].source
                                                                         : source.block;
        if (header >= 0)
            view.blockState[view.listing.GetLines()[header].offset] = Block_Full;
    }
    view.previewActive = true;
    view.previewStart = address;
    view.forceRebuild = true;
}

void close_preview(void)
{
    if (!view.previewActive)
        return;
    view.previewActive = false;
    view.forceRebuild = true;
}

// Where the cartridge sits in the CPU's view, when it is seen whole there
// (no banking): the address whose ROM offset is 0, and every 1K after it
// reading the matching offset.
bool find_flat_base(Memory* memory, u32 romSize, u16& base)
{
    u32 offset = 0;
    int found = -1;
    for (int a = 0; a < 0x10000; a += 0x400)
    {
        if (memory->TranslateToRomOffset(static_cast<u16>(a), offset) && offset == 0)
        {
            found = a;
            break;
        }
    }
    if (found < 0 || static_cast<u32>(found) + romSize > 0x10000)
        return false;
    for (u32 k = 0; k < romSize; k += 0x400)
    {
        if (!memory->TranslateToRomOffset(static_cast<u16>(found + k), offset) || offset != k)
            return false;
    }
    base = static_cast<u16>(found);
    return true;
}

// A byte of the map the listing shows.
u8 source_map_byte(u32 offset)
{
    return view.sourceMap && offset < view.sourceMap->GetBytes().size()
        ? view.sourceMap->GetBytes()[offset] : 0;
}

// The project's map file, loaded once per ROM for viewing only.
RomMap* disk_map(Memory* memory, Cartridge* cart, u32 romSize)
{
    const std::string dir = rom_map_project_dir();
    const std::string path = dir.empty() ? dir : dir + "rommap/rommap.bin";
    const u32 crc = cart->GetCRC();
    if (view.diskMap && view.diskMapCrc == crc && view.diskMapPath == path)
        return view.diskMap.get();
    view.diskMap.reset();
    if (path.empty() || romSize == 0)
        return NULL;
    std::unique_ptr<RomMap> loaded(new RomMap(memory));
    loaded->Attach(romSize, crc);
    loaded->SetEnabled(true);
    // The first layout kept the map loose in the project folder.
    if (!loaded->Load(path) && !loaded->Load(dir + "rommap.bin"))
        return NULL;
    loaded->LoadObservedJumps(dir + "rommap/jumps_seen.txt");
    view.diskMap = std::move(loaded);
    view.diskMapCrc = crc;
    view.diskMapPath = path;
    return view.diskMap.get();
}

// The board's .ports file, Emulicious' format ("<hex> <name> [in|out]"):
// the project's own if it has one, else the one shipped with the
// application (assets/ports).
std::string ports_file_name(void)
{
    switch (emu_get_core()->GetMachineType())
    {
        case MachineType::SG1000: return "SG-1000.ports";
        case MachineType::SC3000: return "SC-3000.ports";
        case MachineType::SMS: return "MasterSystem.ports";
        case MachineType::Coleco: return "ColecoVision.ports";
        case MachineType::MSX: return "MSX1.ports";
    }
    return "";
}

void load_ports(const std::string& projectDir)
{
    const std::string name = ports_file_name();
    std::vector<std::string> candidates;
    if (!projectDir.empty())
        candidates.push_back(projectDir + name);
    const char* base = SDL_GetBasePath();
    const std::string root = base ? base : "";
    for (const char* relative : {"../Resources/ports/", "../../desktop-shared/assets/ports/",
                                 "../desktop-shared/assets/ports/", "ports/"})
        candidates.push_back(root + relative + name);

    view.portNames.clear();
    view.portsFile.clear();
    for (const std::string& path : candidates)
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(std::filesystem::path(path), error))
            continue;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line))
        {
            const std::size_t comment = line.find(';');
            if (comment != std::string::npos)
                line.erase(comment);
            std::istringstream words(line);
            std::string port, portName, direction;
            if (!(words >> port >> portName))
                continue;
            words >> direction;
            const u16 value = static_cast<u16>(std::strtoul(port.c_str(), NULL, 16) & 0xFF);
            if (direction != "out")
                view.portNames[value] = portName;
            if (direction != "in")
                view.portNames[value | 0x100] = portName;
        }
        view.portsFile = path;
        return;
    }
}

// Writes a project file whole or not at all: to a temporary file, then a
// rename over the old one. An app closed in the middle leaves the old file,
// never half of the new.
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
    if (error)
    {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

// rommap/choices.txt: one line per range kept as code, ROM offsets, first
// and last byte, and one per word kept as a pointer - readable, and read by
// tools/game_export.py too.
void load_choices(const std::string& path)
{
    view.choices.clear();
    view.keptWords.clear();
    view.choicesUndo.clear();
    view.choicesPath = path;
    std::ifstream in(path);
    std::string line;
    if (!in || !std::getline(in, line) || line.rfind("GearSF7000 choices", 0) != 0)
        return;
    while (std::getline(in, line))
    {
        std::istringstream words(line);
        std::string what, range;
        if (!(words >> what >> range))
            continue;
        if (what == "word")
        {
            view.keptWords.push_back(static_cast<u32>(std::strtoul(range.c_str(), NULL, 16)));
            continue;
        }
        if (what != "code")
            continue;
        const std::size_t dash = range.find('-');
        if (dash == std::string::npos)
            continue;
        const u32 first = static_cast<u32>(std::strtoul(range.substr(0, dash).c_str(), NULL, 16));
        const u32 last = static_cast<u32>(std::strtoul(range.substr(dash + 1).c_str(), NULL, 16));
        if (last >= first)
            view.choices.push_back({first, last + 1});
    }
}

bool save_choices(void)
{
    if (view.choicesPath.empty())
        return false;
    std::ostringstream out;
    out << "GearSF7000 choices 1\n";
    out << "# Ranges the user keeps as code (ROM offsets, first-last). Only what ran is code\n";
    out << "# otherwise; these are the user's word for the rest. 'word AT': a pointer.\n";
    for (const SourceListing::CodeRange& range : view.choices)
    {
        char line[48];
        std::snprintf(line, sizeof(line), "code %04X-%04X\n", range.start, range.end - 1);
        out << line;
    }
    for (u32 at : view.keptWords)
    {
        char line[32];
        std::snprintf(line, sizeof(line), "word %04X\n", at);
        out << line;
    }
    return write_whole(view.choicesPath, out.str());
}

void change_choices(const std::vector<SourceListing::CodeRange>& next, const char* what)
{
    view.choicesUndo.push_back({view.choices, view.keptWords});
    view.choices = next;
    view.message = save_choices() ? what : "Could not write rommap/choices.txt.";
    view.sourceDirty = true;
}

void keep_as_code(u32 start, u32 end)
{
    // Each keep stays a piece of its own, so it can be forgotten alone
    // (Saverio kept 49F-4C9 a piece at a time); only pieces that overlap the
    // new one become one with it.
    std::vector<SourceListing::CodeRange> next;
    for (const SourceListing::CodeRange& range : view.choices)
    {
        if (range.end > start && range.start < end)
        {
            start = std::min(start, range.start);
            end = std::max(end, range.end);
        }
        else
            next.push_back(range);
    }
    next.push_back({start, end});
    std::sort(next.begin(), next.end(),
              [](const SourceListing::CodeRange& a, const SourceListing::CodeRange& b)
              { return a.start < b.start; });
    char message[80];
    std::snprintf(message, sizeof(message), "Kept %X-%X as code (Cmd/Ctrl+Z undoes it).",
                  view.sourceBase + start, view.sourceBase + end - 1);
    change_choices(next, message);
}

// The kept piece holding an offset, forgotten alone.
void forget_choice(u32 offset)
{
    std::vector<SourceListing::CodeRange> next;
    bool found = false;
    for (const SourceListing::CodeRange& range : view.choices)
    {
        if (!found && offset >= range.start && offset < range.end)
            found = true;
        else
            next.push_back(range);
    }
    change_choices(next, "Choice forgotten: those bytes are back to what the map saw.");
}

// A word kept as a pointer (Saverio: two loose bytes among pointers), and
// forgotten.
void change_words(const std::vector<u32>& next, const char* what)
{
    view.choicesUndo.push_back({view.choices, view.keptWords});
    view.keptWords = next;
    view.message = save_choices() ? what : "Could not write rommap/choices.txt.";
    view.sourceDirty = true;
}

void keep_as_pointer(u32 at)
{
    std::vector<u32> next;
    for (u32 w : view.keptWords)
        if (w + 1 < at || w > at + 1)      // one that overlaps it gives way
            next.push_back(w);
    next.push_back(at);
    std::sort(next.begin(), next.end());
    char message[80];
    std::snprintf(message, sizeof(message), "Kept %X as a pointer (Cmd/Ctrl+Z undoes it).",
                  view.sourceBase + at);
    change_words(next, message);
}

void forget_word(u32 at)
{
    std::vector<u32> next;
    for (u32 w : view.keptWords)
        if (w != at)
            next.push_back(w);
    change_words(next, "Pointer forgotten: those bytes are back to what the map saw.");
}

bool is_kept_word(u32 at)
{
    return std::find(view.keptWords.begin(), view.keptWords.end(), at) != view.keptWords.end();
}

void undo_choice(void)
{
    if (view.choicesUndo.empty())
    {
        view.message = "Nothing to undo.";
        return;
    }
    view.choices = view.choicesUndo.back().first;
    view.keptWords = view.choicesUndo.back().second;
    view.choicesUndo.pop_back();
    view.message = save_choices() ? "Undone." : "Could not write rommap/choices.txt.";
    view.sourceDirty = true;
}

void load_jump_tables(const std::string& path)
{
    view.jumpPath = path;
    view.jumpGeneration = ~0ull;
    JumpTables::Load(path, view.jumpSites);
}

// On the worker: the listing with every table known, and the count of the
// jumps a Deep Search would read - executed, with no result or a changed
// signature.
SourceResult run_source_job(SourceJob job)
{
    SourceResult result;
    result.sites = job.sites;
    const u8* rom = job.rom->data();
    for (u32 site : JumpTables::FindSites(rom, job.size, *job.map))
    {
        const u32 signature = JumpTables::Signature(rom, job.size, job.base, *job.map, site);
        bool known = false;
        for (const JumpTables::Site& s : result.sites)
            known = known || (s.offset == site && s.signature == signature);
        if (!known)
            result.pending++;
    }
    std::vector<SourceListing::JumpTable> tables;
    for (const JumpTables::Site& site : result.sites)
        for (const JumpTables::Table& t : site.tables)
            tables.push_back({t.base, t.entries, t.vector, t.index});
    auto names = job.names;
    auto ports = job.ports;
    auto answers = job.answers;
    result.listing.Build(rom, job.size, job.base, *job.map, [names](u16 address)
    {
        auto it = names->find(address);
        return it != names->end() ? it->second : std::string();
    }, [answers](u16 address) { return (*answers)[address] != 0; }, job.choices,
    [ports](u8 port, bool out)
    {
        auto it = ports->find(static_cast<u16>(port | (out ? 0x100 : 0)));
        return it != ports->end() ? it->second : std::string();
    }, tables, job.keptWords);
    result.job = std::move(job);
    return result;
}

bool update_source_inputs(Memory* memory);
bool collect_source_job(void);

// Decides whether the cartridge is shown as source and rebuilds the listing
// when the map, the symbols or the ROM moved. True when the lines changed.
bool update_source(Memory* memory)
{
    const bool installed = collect_source_job();
    return update_source_inputs(memory) || installed;
}

bool update_source_inputs(Memory* memory)
{
    GearSF7000Core* core = emu_get_core();
    RomMap* map = core->GetRomMap();
    Cartridge* cart = core->GetCartridge();
    const u32 romSize = static_cast<u32>(std::max(0, cart->GetROMSize()));
    const bool project = rom_map_project_active();

    // While collecting, the live map, which grows as the game runs;
    // otherwise the project's map as it was last saved.
    if (project && !(map && map->IsEnabled()))
        map = disk_map(memory, cart, romSize);
    else
        view.diskMap.reset();

    bool active = project && map && romSize > 0 && map->GetBytes().size() == romSize &&
                  cart->GetROM() != NULL;
    u16 base = 0;
    view.sourceNote.clear();
    if (active && !find_flat_base(memory, romSize, base))
    {
        active = false;
        view.sourceNote = "Banked ROM: the source view does flat ROMs only for now.";
    }
    else if (project && !active)
        view.sourceNote = "The project's map could not be read: showing what ran.";

    if (!active)
    {
        const bool changed = view.sourceActive;
        view.sourceActive = false;
        view.sourceMap = nullptr;
        view.sourceOrigin = nullptr;
        view.jobEpoch++;
        return changed;
    }

    const bool same = view.sourceActive && view.sourceOrigin == map && view.sourceBase == base &&
                      view.sourceSize == romSize && view.sourceCrc == map->GetCrc();
    const std::string dir = rom_map_project_dir();
    const std::string choicesPath = dir.empty() ? dir : dir + "rommap/choices.txt";
    if (choicesPath != view.choicesPath)
    {
        load_choices(choicesPath);
        view.sourceDirty = true;
    }
    const std::string jumpPath = dir.empty() ? dir : dir + "rommap/jumptables.txt";
    if (jumpPath != view.jumpPath)
    {
        load_jump_tables(jumpPath);
        view.sourceDirty = true;
    }
    static std::string portsFor;
    const std::string portsKey = dir + "|" + ports_file_name();
    if (portsKey != portsFor)
    {
        portsFor = portsKey;
        load_ports(dir);
        view.sourceDirty = true;
    }
    const bool symbolsMoved = view.sourceSymbolCount != gui_debug_symbol_count();
    if (same && !symbolsMoved && !view.sourceDirty)
    {
        if (map->GetGeneration() == view.sourceGeneration)
            return false;
        if (SDL_GetTicks() - view.sourceTicks < kSourceRebuildMs)
            return false;
    }
    if (!same && view.sourceActive)
    {
        // Another ROM or map: what a running build was for is gone.
        view.blockState.clear();
        view.jobEpoch++;
    }

    // A kept piece that has since run, even in part, gives way to what ran:
    // the live wins (Saverio fed the NMI with F1 and the pieces stayed). What
    // of it did not run goes back to what the map says until it does.
    {
        const std::vector<u8>& bytes = map->GetBytes();
        std::vector<SourceListing::CodeRange> kept;
        std::string ran;
        for (const SourceListing::CodeRange& range : view.choices)
        {
            bool any = false;
            for (u32 i = range.start; !any && i < range.end && i < bytes.size(); i++)
                any = (bytes[i] & RomMap::KindMask) == RomMap::Kind_Code;
            if (!any)
            {
                kept.push_back(range);
                continue;
            }
            char text[24];
            std::snprintf(text, sizeof(text), "%s%X-%X", ran.empty() ? "" : ", ",
                          base + range.start, base + range.end - 1);
            ran += text;
        }
        std::vector<u32> keptWords;
        for (u32 at : view.keptWords)
        {
            const bool any = at + 1 < bytes.size() && ((bytes[at] & RomMap::KindMask) == RomMap::Kind_Code ||
                                                       (bytes[at + 1] & RomMap::KindMask) == RomMap::Kind_Code);
            if (!any)
            {
                keptWords.push_back(at);
                continue;
            }
            char text[24];
            std::snprintf(text, sizeof(text), "%s%X", ran.empty() ? "" : ", ", base + at);
            ran += text;
        }
        if (!ran.empty())
        {
            view.choices = kept;
            view.keptWords = keptWords;
            view.choicesUndo.clear();
            view.message = save_choices()
                ? "Kept " + ran + ": it ran, the live wins - the choice is gone."
                : std::string("Could not write rommap/choices.txt.");
        }
    }

    // One build at a time; another is asked for when this one is in.
    if (view.jobRunning)
        return false;

    // What answers at each address, once per ROM: the job cannot ask the
    // live memory from its thread.
    if (!view.answers || view.answersCrc != map->GetCrc())
    {
        view.answers = std::make_shared<std::vector<u8>>(0x10000);
        for (int a = 0; a < 0x10000; a++)
            (*view.answers)[a] = memory->AnswersAt(static_cast<u16>(a)) ? 1 : 0;
        view.answersCrc = map->GetCrc();
    }

    // User symbols by address, whatever their bank: the listing is flat.
    std::unordered_map<u16, std::string>& names = view.sourceNames;
    names.clear();
    const int count = gui_debug_symbol_count();
    for (int i = 0; i < count; i++)
    {
        u16 address = 0;
        const char* text = NULL;
        if (gui_debug_get_symbol(i, NULL, &address, &text) && text && !names.count(address))
            names[address] = text;
    }

    SourceJob job;
    job.rom = std::make_shared<std::vector<u8>>(cart->GetROM(), cart->GetROM() + romSize);
    job.map = std::make_shared<RomMap>(*map);
    job.names = std::make_shared<std::unordered_map<u16, std::string>>(names);
    job.ports = std::make_shared<std::unordered_map<u16, std::string>>(view.portNames);
    job.answers = view.answers;
    job.choices = view.choices;
    job.keptWords = view.keptWords;
    job.sites = view.jumpSites;
    job.base = base;
    job.size = romSize;
    job.crc = map->GetCrc();
    job.generation = map->GetGeneration();
    job.epoch = view.jobEpoch;
    job.symbolCount = count;
    job.origin = map;
    view.job = std::async(std::launch::async, run_source_job, std::move(job));
    view.jobRunning = true;
    view.sourceDirty = false;
    view.sourceTicks = SDL_GetTicks();
    view.sourceGeneration = map->GetGeneration();
    view.sourceSymbolCount = count;
    return false;
}

// The finished build, if any, put in place of the listing shown: true when
// the lines changed.
bool collect_source_job(void)
{
    if (!view.jobRunning ||
        view.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return false;
    SourceResult result = view.job.get();
    view.jobRunning = false;
    if (result.job.epoch != view.jobEpoch)
        return false;       // built for what is no longer there
    view.deepPending = result.pending;
    view.listing = std::move(result.listing);
    view.heldMap = result.job.map;
    view.heldRom = result.job.rom;
    view.sourceMap = view.heldMap.get();
    view.sourceOrigin = result.job.origin;
    view.sourceActive = true;
    view.sourceBase = result.job.base;
    view.sourceSize = result.job.size;
    view.sourceCrc = result.job.crc;
    return true;
}

void start_preview(u16 address);
void close_preview(void);
bool ends_flow(const std::string& text);

// The preview, decoded linearly as a reader would try it: its instructions,
// where it ends and why. Computed once per rebuild, before the lines, since
// the data rows it covers must give way to it.
struct PreviewInstruction
{
    u16 address;
    u16 size;
    std::string text;
    bool jump;          // a branch with a target (not rst)
    u16 target;
};
std::vector<PreviewInstruction> previewInstructions;
std::vector<u16> previewLabels;
u32 previewEnd = 0;         // ROM offset past the last instruction
std::string previewStop;
bool previewWritten = false;

void compute_preview(void)
{
    previewInstructions.clear();
    previewWritten = false;
    const u8* rom = emu_get_core()->GetCartridge()->GetROM();
    const std::vector<u8>& kinds = view.sourceMap->GetBytes();
    const u32 start = static_cast<u32>(view.previewStart - view.sourceBase);
    std::vector<u32> targets;   // forward branches: the code goes on there
    std::string reason;
    char text[96];
    u32 offset = start;
    for (int count = 0; reason.empty(); count++)
    {
        const u16 address = static_cast<u16>(view.sourceBase + offset);
        if (offset >= view.sourceSize)
            reason = "end of the ROM";
        else if (count > 0 && (kinds[offset] & RomMap::KindMask) == RomMap::Kind_Code)
            std::snprintf(text, sizeof(text), "runs into code at %X", address), reason = text;
        // A code label (something jumps there) is where the flow goes on, as
        // after ret nz / jr nz (Saverio kept those a piece at a time): only
        // a name of data stops it.
        else if (count > 0 && view.listing.HasLabelAt(offset) &&
                 view.listing.Label(address).rfind("_DATA_", 0) == 0)
            reason = "next label " + view.listing.Label(address);
        else if (count >= 64)
            reason = "64 instructions, no end in sight";
        if (!reason.empty())
            break;

        u8 bytes[16] = {};
        for (u32 i = 0; i < 16 && offset + i < view.sourceSize; i++)
            bytes[i] = rom[offset + i];
        Z80Disassembler::Decoded decoded;
        Z80Disassembler::Decode(bytes, 16, address, decoded);
        const std::string name = decoded.name;
        if (decoded.size <= 0 || name.find("PARSE ERROR") != std::string::npos ||
            name.find("[UNDOC]") != std::string::npos)
        {
            std::snprintf(text, sizeof(text), "invalid opcode at %X", address);
            reason = text;
            break;
        }
        const std::string source =
            SourceListing::SourceText(decoded.name, decoded.rst, decoded.jumpAddress);
        previewInstructions.push_back({address, static_cast<u16>(decoded.size), source,
                                       decoded.jump && !decoded.rst, decoded.jumpAddress});
        if (decoded.jump && !decoded.rst && decoded.jumpAddress > address)
            targets.push_back(static_cast<u32>(decoded.jumpAddress - view.sourceBase));
        offset += static_cast<u32>(decoded.size);
        // After a jp or ret the flow goes on only if something jumped here.
        if (ends_flow(source) &&
            std::find(targets.begin(), targets.end(), offset) == targets.end())
            reason = "the flow ends here";
    }
    previewEnd = std::max(offset, start + 1);

    // The labels keeping it would give: its start, and every branch target
    // that is one of its instructions - named as the listing names code.
    std::vector<u16> labelled(1, view.previewStart);
    for (const PreviewInstruction& instruction : previewInstructions)
        if (instruction.jump)
            for (const PreviewInstruction& other : previewInstructions)
                if (other.address == instruction.target)
                    labelled.push_back(instruction.target);
    for (PreviewInstruction& instruction : previewInstructions)
    {
        if (!instruction.jump ||
            std::find(labelled.begin(), labelled.end(), instruction.target) == labelled.end())
            continue;
        char number[8];
        std::snprintf(number, sizeof(number), "$%04X", instruction.target);
        const std::size_t at = instruction.text.find(number);
        if (at != std::string::npos)
            instruction.text.replace(at, std::strlen(number),
                                     "{s}" + SourceListing::CodeLabelName(instruction.target) + "{o}");
    }
    previewLabels = labelled;

    previewStop = "; stops: " + reason;
    const std::string hint = view.listing.CandidateHint(start, previewEnd);
    if (hint.rfind("probably data", 0) == 0)
        previewStop += "   ; " + hint;
}

void append_preview_lines(void)
{
    if (previewWritten)
        return;
    previewWritten = true;
    auto push = [](u8 type, u16 address, u16 count, const std::string& text)
    {
        view.previewTexts.push_back(text);
        Line line = {address, type, count, static_cast<int>(view.previewTexts.size()) - 1, {}, 0};
        view.lines.push_back(line);
        if (type == Line_PreviewInstruction)
            view.lineAtOrBefore[address] = static_cast<int>(view.lines.size()) - 1;
    };
    char text[96];
    std::snprintf(text, sizeof(text), "; try as code from %X", view.previewStart);
    push(Line_PreviewHead, view.previewStart, 0, text);
    for (const PreviewInstruction& instruction : previewInstructions)
    {
        if (std::find(previewLabels.begin(), previewLabels.end(), instruction.address) !=
            previewLabels.end())
        {
            // An existing name (a symbol of yours) wins over the found one.
            const std::string existing = view.listing.Label(instruction.address);
            const bool named = !existing.empty() &&
                existing != SourceListing::DataLabelName(instruction.address);
            push(Line_PreviewLabel, instruction.address, 0,
                 named ? existing : SourceListing::CodeLabelName(instruction.address));
        }
        push(Line_PreviewInstruction, instruction.address, instruction.size, instruction.text);
    }
    push(Line_PreviewStop,
         static_cast<u16>(view.sourceBase + std::min(previewEnd, view.sourceSize) - 1), 0,
         previewStop);
}

// The text of a source instruction without markers, and whether it ends
// the flow: an unconditional jump or return, after which a new routine
// starts and gets a blank line above it.
bool ends_flow(const std::string& text)
{
    std::string plain;
    for (std::size_t i = 0; i < text.size(); i++)
    {
        if (text[i] == '{' && i + 2 < text.size() && text[i + 2] == '}')
        {
            i += 2;
            continue;
        }
        plain += text[i];
    }
    const std::size_t space = plain.find(' ');
    const std::string mnemonic = plain.substr(0, space);
    const bool operands = space != std::string::npos;
    if (mnemonic == "ret")
        return !operands;
    if (mnemonic == "reti" || mnemonic == "retn")
        return true;
    if (mnemonic == "jp" || mnemonic == "jr")
        return plain.find(',') == std::string::npos;
    return false;
}

// The cartridge's lines, folded blocks left out but their headers, with the
// blank lines demo3 has: before every block of data, before code that
// follows data, before a label that starts after the flow ended.
void append_source_lines(void)
{
    const std::vector<SourceListing::Line>& lines = view.listing.GetLines();
    const bool preview = view.previewActive && view.sourceMap;
    if (preview)
        compute_preview();
    const u32 previewFrom = preview ? static_cast<u32>(view.previewStart - view.sourceBase) : 0;
    std::vector<bool> keptHeadWritten(view.choices.size(), false);
    // The block being written: its header, how it is shown, how much of it
    // is out already.
    int block = -1;
    u8 state = Block_Full;
    int rows = 0;
    u32 bytesShown = 0;
    bool moreWritten = false;
    auto hidden = [&](const SourceListing::Line& data)
    {
        return data.block >= 0 && data.block == block &&
               (state == Block_Folded || (state == Block_Preview && rows >= kPreviewRows));
    };
    // What the last line pushed was: 0 nothing, 1 code, 2 code ending the
    // flow, 3 data, 4 blank.
    int last = 0;
    auto blank = [&]()
    {
        if (last == 0 || last == 4)
            return;
        Line line = {0, Line_Blank, 0, -1, {}, 0};
        line.address = view.lines.empty() ? 0 : view.lines.back().address;
        view.lines.push_back(line);
        last = 4;
    };
    for (std::size_t i = 0; i < lines.size(); i++)
    {
        const SourceListing::Line& source = lines[i];
        if (source.type == SourceListing::Line_DataHeader)
        {
            block = source.block;
            state = block_state(source);
            rows = 0;
            bytesShown = 0;
            moreWritten = false;
        }
        else if (source.block < 0 && source.type != SourceListing::Line_Label)
            block = -1;

        const bool dataRow = source.type == SourceListing::Line_Data ||
                             source.type == SourceListing::Line_Words;
        if (dataRow && hidden(source))
        {
            // The first row past the preview becomes "... N more bytes".
            if (state == Block_Preview && !moreWritten)
            {
                const u32 rest = lines[block].count > bytesShown ? lines[block].count - bytesShown : 0;
                Line more = {static_cast<u16>(view.sourceBase + source.offset), Line_More,
                             static_cast<u16>(std::min<u32>(rest, 0xFFFF)), -1, {}, 0};
                more.source = block;
                view.lines.push_back(more);
                view.lineAtOrBefore[more.address] = static_cast<int>(view.lines.size()) - 1;
                moreWritten = true;
                last = 3;
            }
            continue;
        }
        if ((source.type == SourceListing::Line_Label || source.type == SourceListing::Line_Comment) &&
            i + 1 < lines.size() && hidden(lines[i + 1]))
            continue;
        // A label inside what the preview decodes belongs to the preview.
        if (preview && source.type == SourceListing::Line_Label &&
            source.offset > previewFrom && source.offset < previewEnd)
            continue;

        // Data rows give way to the preview: what it decodes is shown once,
        // as instructions, and the bytes resume where it stops.
        if (preview && dataRow && source.offset < previewEnd &&
            source.offset + source.count > previewFrom)
        {
            if (source.block == block)
            {
                rows++;
                bytesShown += source.count;
            }
            auto piece = [&](u32 from, u32 to)
            {
                if (from >= to || source.type != SourceListing::Line_Data)
                    return;
                Line part = {static_cast<u16>(view.sourceBase + from), Line_Source,
                             static_cast<u16>(to - from), -1, {'R', 'O', 'M', 0, 0}, 0};
                part.source = static_cast<int>(i);
                view.lines.push_back(part);
                view.lineAtOrBefore[part.address] = static_cast<int>(view.lines.size()) - 1;
            };
            piece(source.offset, std::min(previewFrom, source.offset + source.count));
            if (source.offset + source.count > previewFrom)
                append_preview_lines();
            piece(std::max(previewEnd, source.offset), source.offset + source.count);
            last = 3;
            continue;
        }
        if (dataRow && source.block == block)
        {
            rows++;
            bytesShown += source.count;
        }

        // A label - or the comment over it - belongs to what follows it; the
        // blank line goes above the first of them.
        auto labelLike = [](const SourceListing::Line& l)
        {
            return l.type == SourceListing::Line_Label || l.type == SourceListing::Line_Comment;
        };
        const bool isLabel = labelLike(source);
        std::size_t j = i;
        while (j + 1 < lines.size() && labelLike(lines[j]))
            j++;
        const SourceListing::Line* next = isLabel ? &lines[j] : &source;
        const bool data = next->type != SourceListing::Line_Instruction;
        const bool afterLabel = i > 0 && labelLike(lines[i - 1]);

        if (source.type == SourceListing::Line_DataHeader && last != 4 && !afterLabel)
            blank();
        else if (isLabel && afterLabel)
            ;   // under its comment or another label: no blank in between
        else if (isLabel && data)
            blank();
        else if ((isLabel || source.type == SourceListing::Line_Instruction) && !data &&
                 (last == 3 || (isLabel && last == 2)))
            blank();

        // A kept piece opens with its own header line, above its label.
        if (source.type == SourceListing::Line_Label ||
            source.type == SourceListing::Line_Instruction)
        {
            for (std::size_t c = 0; c < view.choices.size(); c++)
            {
                if (view.choices[c].start != source.offset || keptHeadWritten[c])
                    continue;
                keptHeadWritten[c] = true;
                Line head = {static_cast<u16>(view.sourceBase + source.offset), Line_KeptHead,
                             0, static_cast<int>(c), {}, 0};
                view.lines.push_back(head);
            }
        }
        // And a kept pointer, the same way: its own line with forget.
        if (source.type == SourceListing::Line_Words && source.chosen)
        {
            Line head = {static_cast<u16>(view.sourceBase + source.offset), Line_KeptWordHead,
                         0, -1, {}, 0};
            view.lines.push_back(head);
        }

        Line line = {static_cast<u16>(view.sourceBase + source.offset), Line_Source,
                     source.count, -1, {'R', 'O', 'M', 0, 0}, 0};
        line.source = static_cast<int>(i);
        view.lines.push_back(line);
        if (!isLabel)
            view.lineAtOrBefore[line.address] = static_cast<int>(view.lines.size()) - 1;

        if (source.type == SourceListing::Line_Instruction)
            last = ends_flow(source.text) ? 2 : 1;
        else if (!isLabel)
            last = 3;
    }
}

void rebuild(Memory* memory, int pc)
{
    view.previewTexts.clear();
    const auto symbols = view.showSymbols ? collect_symbols()
                                          : std::unordered_map<u32, std::string>();
    view.lines.clear();
    view.labels.clear();
    std::fill(view.lineAtOrBefore.begin(), view.lineAtOrBefore.end(), -1);

    auto mark = [](u16 address)
    {
        view.lineAtOrBefore[address] = static_cast<int>(view.lines.size()) - 1;
    };

    Memory::stDisassembleRecord* previous = NULL;
    for (int a = 0; a < 0x10000; a++)
    {
        if (view.sourceActive && a == view.sourceBase)
        {
            append_source_lines();
            a = view.sourceBase + static_cast<int>(view.sourceSize) - 1;
            previous = NULL;
            continue;
        }
        const u16 address = static_cast<u16>(a);
        Memory::stDisassembleRecord* record = memory->GetDisassembleRecord(address, false);
        if (!IsValidPointer(record) || record->name[0] == 0)
            continue;

        // A gap only inside one piece of memory: between the ROM and the RAM
        // there is no code missing, just other things.
        if (IsValidPointer(previous) && previous->bank == record->bank &&
            std::strcmp(previous->segment, record->segment) == 0)
        {
            const int gapStart = previous->address + previous->size;
            if (gapStart < a)
            {
                const u16 start = static_cast<u16>(gapStart);
                const u16 end = static_cast<u16>(a - 1);
                Line gap = {start, Line_Gap, static_cast<u16>(a - gapStart), -1, {}, previous->bank};
                std::memcpy(gap.segment, previous->segment, sizeof(gap.segment));
                view.lines.push_back(gap);
                mark(start);
                if (gap_is_open(start, end))
                {
                    for (int b = gapStart; b < a; b += 16)
                    {
                        const u16 count = static_cast<u16>(std::min(16, a - b));
                        Line bytes = gap;
                        bytes.address = static_cast<u16>(b);
                        bytes.type = Line_GapBytes;
                        bytes.count = count;
                        view.lines.push_back(bytes);
                        mark(static_cast<u16>(b));
                    }
                }
            }
        }

        if (view.showSymbols)
        {
            const char* name = symbol_for(symbols, record, address);
            if (name)
            {
                view.labels.push_back(name);
                view.lines.push_back({address, Line_Label, 0,
                                      static_cast<int>(view.labels.size()) - 1, {}, 0});
            }
        }
        view.lines.push_back({address, Line_Instruction, 0, -1, {}, 0});
        mark(address);
        previous = record;
    }
    // Every address points at the last line starting at or before it.
    int last = -1;
    for (int a = 0; a < 0x10000; a++)
    {
        if (view.lineAtOrBefore[a] >= 0)
            last = view.lineAtOrBefore[a];
        view.lineAtOrBefore[a] = last;
    }
    view.forceRebuild = false;

    view.built = true;
    view.builtTicks = SDL_GetTicks();
    view.builtStopped = emu_is_execution_stopped();
    view.builtPc = pc;
    view.builtSymbols = view.showSymbols;
    view.builtSymbolCount = gui_debug_symbol_count();
}

bool needs_rebuild(int pc)
{
    if (!view.built || view.forceRebuild || view.builtSymbols != view.showSymbols ||
        view.builtSymbolCount != gui_debug_symbol_count())
        return true;
    const bool stopped = emu_is_execution_stopped();
    if (stopped != view.builtStopped || (stopped && pc != view.builtPc))
        return true;
    return SDL_GetTicks() - view.builtTicks >= kRebuildMs;
}

// An instruction, whether decoded by the debugger or from the ROM source.
bool is_instruction_line(const Line& line)
{
    if (line.type == Line_Instruction)
        return true;
    return line.type == Line_Source && view.sourceActive &&
           view.listing.GetLines()[line.source].type == SourceListing::Line_Instruction;
}

// The instruction line showing an address, or the nearest one before it.
int line_of(u16 address)
{
    return view.lineAtOrBefore[address];
}

u16 focus_address(int pc)
{
    return (!view.follow && view.hasSelection) ? view.selection : static_cast<u16>(pc);
}

void select(u16 address, bool running)
{
    if (view.follow)
    {
        view.follow = false;
        if (running)
            view.message = "Follow PC suspended: you picked a line. The target button resumes it.";
    }
    view.hasSelection = true;
    view.selection = address;
}

// Selects an address and scrolls to it, opening the gap it falls in.
void show_address(u16 address)
{
    const int index = line_of(address);
    if (index >= 0 && view.lines[index].type == Line_Gap)
    {
        const Line& gap = view.lines[index];
        set_gap_open(gap.address, static_cast<u16>(gap.address + gap.count - 1), true);
    }
    if (index >= 0 && view.sourceActive &&
        (view.lines[index].type == Line_Source || view.lines[index].type == Line_More))
    {
        // An address inside a folded block, or in the part of a long one the
        // preview leaves out: show the whole block.
        const SourceListing::Line& source = view.listing.GetLines()[view.lines[index].source];
        if (view.lines[index].type == Line_More ||
            (source.type == SourceListing::Line_DataHeader && !block_is_open(source) &&
             address != view.lines[index].address))
            set_block_state(view.lines[index].type == Line_More
                                ? view.listing.GetLines()[view.lines[index].source].offset
                                : source.offset, Block_Full);
        else if (source.type == SourceListing::Line_DataHeader && !block_is_open(source))
            set_block_open(source.offset, true);
    }
    view.follow = false;
    view.hasSelection = true;
    view.selection = address;
    view.scrollRequested = true;
    view.scrollTarget = address;
}

// Goes to an address, remembering where it came from for Back.
void go_to_from(u16 address, u16 from)
{
    if (view.historyPos >= 0)
        view.history.resize(view.historyPos + 1);
    if (view.history.empty() || view.history.back() != from)
        view.history.push_back(from);
    view.history.push_back(address);
    view.historyPos = static_cast<int>(view.history.size()) - 1;

    show_address(address);
}

void go_to(u16 address, int pc)
{
    go_to_from(address, focus_address(pc));
}

void history_move(int delta)
{
    const int next = view.historyPos + delta;
    if (next < 0 || next >= static_cast<int>(view.history.size()))
    {
        view.message = delta < 0 ? "No earlier position." : "No later position.";
        return;
    }
    view.historyPos = next;
    show_address(view.history[next]);
}

void resume_follow(void)
{
    view.follow = true;
    view.hasSelection = false;
    view.message.clear();
}

bool equals_ignore_case(const char* a, const char* b)
{
    for (; *a && *b; ++a, ++b)
        if (std::tolower(static_cast<unsigned char>(*a)) !=
            std::tolower(static_cast<unsigned char>(*b)))
            return false;
    return *a == *b;
}

std::string trimmed(const char* text)
{
    std::string result = text;
    result.erase(0, result.find_first_not_of(" \t"));
    result.erase(result.find_last_not_of(" \t") + 1);
    return result;
}

// An address in hex ($ optional) or a symbol name, explicit or automatic.
bool resolve_address(Memory* memory, const std::string& text, u16& address)
{
    if (text.empty())
        return false;
    std::string hex = text[0] == '$' ? text.substr(1) : text;
    if (!hex.empty() && hex.size() <= 4 &&
        hex.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
    {
        address = static_cast<u16>(std::stoul(hex, NULL, 16));
        return true;
    }

    const int count = gui_debug_symbol_count();
    for (int i = 0; i < count; i++)
    {
        const char* name = NULL;
        if (gui_debug_get_symbol(i, NULL, &address, &name) && name &&
            equals_ignore_case(name, text.c_str()))
            return true;
    }
    for (int a = 0; a < 0x10000; a++)
    {
        Memory::stDisassembleRecord* record =
            memory->GetDisassembleRecord(static_cast<u16>(a), false);
        if (IsValidPointer(record) && record->auto_symbol[0] != 0 &&
            equals_ignore_case(record->auto_symbol, text.c_str()))
        {
            address = static_cast<u16>(a);
            return true;
        }
    }
    return false;
}

void go_to_text(Memory* memory, int pc)
{
    const std::string text = trimmed(view.gotoText);
    view.gotoText[0] = 0;
    if (text.empty())
        return;
    u16 address = 0;
    if (resolve_address(memory, text, address))
        go_to(address, pc);
    else
        view.message = "Go To: no address or symbol called \"" + text + "\".";
}

// CPU breakpoints are the debugger's list of records, which the emulation
// thread walks while it runs: a removed one leaves an empty slot, as the old
// window's toggle does, and an added one takes the first empty slot, so the
// list only grows when it has to.
void set_breakpoint(Memory* memory, u16 address, bool on)
{
    std::vector<Memory::stDisassembleRecord*>* breakpoints = memory->GetBreakpointsCPU();
    Memory::stDisassembleRecord* record = memory->GetDisassembleRecord(address, on);
    if (!IsValidPointer(record))
        return;
    for (Memory::stDisassembleRecord*& b : *breakpoints)
    {
        if (b == record)
        {
            if (!on)
                InitPointer(b);
            return;
        }
    }
    if (!on)
        return;
    for (Memory::stDisassembleRecord*& b : *breakpoints)
    {
        if (!IsValidPointer(b))
        {
            b = record;
            return;
        }
    }
    breakpoints->push_back(record);
}

bool toolbar_button(const char* icon, const char* id, const char* tooltip, bool enabled = true)
{
    char label[48];
    snprintf(label, sizeof(label), "%s##%s", icon, id);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(label);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        // The tooltip needs the text font, not the icon font.
        ImGui::PopFont();
        if (ImGui::BeginTooltip())
        {
            ImGui::TextUnformatted(tooltip);
            ImGui::EndTooltip();
        }
        ImGui::PushFont(gui_material_icons_font, 20.0f);
    }
    ImGui::SameLine();
    return pressed;
}

void run_to_selection(void)
{
    if (!view.hasSelection)
        return;
    emu_get_core()->GetMemory()->ArmRunToAddress(view.selection);
    emu_debug_continue();
}

// Fixed width, centred: the bar must not move when RUNNING becomes PAUSED.
void run_state_label(bool stopped)
{
    const char* text = stopped ? "PAUSED" : "RUNNING";
    const float slot = ImGui::CalcTextSize("WWWWWWWWW").x;
    const float width = ImGui::CalcTextSize(text).x;
    const float x = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(x + (slot - width) * 0.5f);
    ImGui::TextColored(stopped ? red : green, "%s", text);
}

bool is_breakpoint(std::vector<Memory::stDisassembleRecord*>* breakpoints,
                   Memory::stDisassembleRecord* record)
{
    for (Memory::stDisassembleRecord* b : *breakpoints)
        if (b == record)
            return true;
    return false;
}

// The instruction bytes, between the address and the text, in a column as
// wide as the longest Z80 instruction. Padded with an item rather than placed
// at an offset: an offset from the line start is taken twice once the listing
// scrolls sideways, and the column then slides over the instructions.
void bytes_column(const char* text, const ImVec4& color)
{
    if (!view.showBytes)
        return;
    const float width = ImGui::CalcTextSize("DD CB 7F 00 ").x;
    ImGui::SameLine();
    const float used = ImGui::CalcTextSize(text).x;
    ImGui::TextColored(color, "%s", text);
    if (used < width)
    {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::Dummy(ImVec2(width - used, 1.0f));
    }
}

// The gutter left of every line: three slots, each with one meaning only -
// the breakpoint dot (a click there sets one), the PC mark, and a green dot
// on an instruction that has run at least once. Nothing else goes there: the
// fold arrows of blocks were triangles like the PC's and were mistaken for
// it, so they sit at the start of their header's text.
float breakpoint_margin_width(void)
{
    // Room for the four slots and a gap before the address: squeezed, the
    // jump's arrow touched the address (Saverio: "troppo schiacciati").
    return ImGui::GetTextLineHeight() * 4.6f;
}

float gutter_slot(int slot)
{
    const float h = ImGui::GetTextLineHeight();
    // 0 breakpoint, 1 PC, 2 ran / kept, 3 the line of the selected jump.
    static const float kSlots[] = {0.0f, 1.2f, 2.1f, 3.0f};
    return h * kSlots[slot < 0 ? 0 : (slot > 3 ? 3 : slot)];
}

bool breakpoint_margin(Memory* memory, u16 address, bool isBreakpoint, const ImVec2& row,
                       bool clicked);

const ImVec4 kPcBackground = ImVec4(0.42f, 0.14f, 0.14f, 1.0f);
const ImVec4 kExecutedDot = ImVec4(0.31f, 0.69f, 0.38f, 1.0f);

// The PC mark: a solid yellow bar, a shape no other mark in the gutter has.
void gutter_pc(const ImVec2& row)
{
    const float h = ImGui::GetTextLineHeight();
    const float x = row.x + gutter_slot(1) + h * 0.2f;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x, row.y), ImVec2(x + h * 0.3f, row.y + h),
                                              ImGui::GetColorU32(yellow));
}

// Ran at least once: this session's count when the debugger has one, or
// only the map's word for it (an earlier session).
void gutter_executed(const ImVec2& row, u64 count, bool hoverable)
{
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 centre(row.x + gutter_slot(2) + h * 0.35f, row.y + h * 0.5f);
    ImGui::GetWindowDrawList()->AddCircleFilled(centre, h * 0.17f,
                                                ImGui::GetColorU32(kExecutedDot));
    // The row's own item may not be the last one submitted here, so the
    // slot is tested as a rectangle.
    const ImVec2 min(row.x + gutter_slot(2), row.y);
    const ImVec2 max(row.x + gutter_slot(3), row.y + h);
    if (!hoverable || !ImGui::IsWindowHovered() || !ImGui::IsMouseHoveringRect(min, max))
        return;
    if (count > 0)
        ImGui::SetTooltip("Executed %llu time%s this session", static_cast<unsigned long long>(count),
                          count == 1 ? "" : "s");
    else
        ImGui::SetTooltip("Executed in an earlier session (the project's map)");
}

// Kept as code by the user: a hollow dot where a run shows a solid one; red
// when the map has since seen its bytes read as data.
void gutter_kept(const ImVec2& row, bool contradicted)
{
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 centre(row.x + gutter_slot(2) + h * 0.35f, row.y + h * 0.5f);
    ImGui::GetWindowDrawList()->AddCircle(centre, h * 0.18f,
        ImGui::GetColorU32(contradicted ? red : ImVec4(0.85f, 0.88f, 0.95f, 0.9f)), 0, 1.5f);
    const ImVec2 min(row.x + gutter_slot(2), row.y);
    const ImVec2 max(row.x + gutter_slot(3), row.y + h);
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, max))
        ImGui::SetTooltip(contradicted
            ? "Kept as code by you, but the map has since seen it read as data.\nRight-click: forget the choice."
            : "Kept as code by you; it has not run. Right-click: forget the choice.");
}

// A jump table entry seen in use: the same green dot as code that ran.
void gutter_seen_entry(const ImVec2& row)
{
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 centre(row.x + gutter_slot(2) + h * 0.35f, row.y + h * 0.5f);
    ImGui::GetWindowDrawList()->AddCircleFilled(centre, h * 0.17f, ImGui::GetColorU32(kExecutedDot));
    const ImVec2 min(row.x + gutter_slot(2), row.y), max(row.x + gutter_slot(3), row.y + h);
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, max))
        ImGui::SetTooltip("Seen in use: a jp (hl) went through this entry while the game ran");
}

const ImVec4 kSelectionOutline = ImVec4(0.85f, 0.88f, 0.95f, 0.9f);

// A clickable row. The PC row is red; the selected row has a light outline
// drawn over whatever is under it, so the PC stays red when it is also the
// selection - a filled selection covered it.
bool row_selectable(bool selected, bool pc, ImGuiSelectableFlags flags = 0)
{
    const bool clicked = ImGui::Selectable("", false, flags) && !view.suppressRowClick;
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (pc)
        draw->AddRectFilled(min, max, ImGui::GetColorU32(kPcBackground));
    if (selected)
        draw->AddRect(min, max, ImGui::GetColorU32(kSelectionOutline), 0.0f, 0, 1.5f);
    return clicked;
}

void draw_instruction_line(Memory* memory, const Line& line, int pc,
                           std::vector<Memory::stDisassembleRecord*>* breakpoints,
                           const std::unordered_map<u32, std::string>& symbols,
                           bool running)
{
    Memory::stDisassembleRecord* record = memory->GetDisassembleRecord(line.address, false);
    if (!IsValidPointer(record) || record->name[0] == 0)
    {
        ImGui::TextUnformatted("");
        return;
    }

    const bool isPc = record->address == pc;
    const bool isBreakpoint = is_breakpoint(breakpoints, record);
    const bool selected = !view.follow && view.hasSelection && view.selection == record->address;

    const ImVec2 row = ImGui::GetCursorScreenPos();
    const bool clicked = row_selectable(selected, isPc, ImGuiSelectableFlags_AllowDoubleClick);
    if (isPc)
        gutter_pc(row);
    if (record->execution_count > 0)
        gutter_executed(row, record->execution_count, true);
    if (!breakpoint_margin(memory, record->address, isBreakpoint, row, clicked) && clicked)
    {
        if (ImGui::IsMouseDoubleClicked(0) && record->jump)
            go_to(record->jump_address, pc);
        else
            select(record->address, running);
    }

    const ImVec4 colorSegment = isBreakpoint ? red : magenta;
    const ImVec4 colorBank = isBreakpoint ? red : violet;
    const ImVec4 colorAddress = isBreakpoint ? red : cyan;
    const ImVec4 colorBytes = isBreakpoint ? red : gray;
    ImVec4 colorName = isBreakpoint ? red : white;

    if (view.showSegment)
    {
        ImGui::SameLine();
        ImGui::TextColored(colorSegment, "%s", record->segment);
    }
    if (view.showBank && memory->HasMapper())
    {
        ImGui::SameLine();
        ImGui::TextColored(colorBank, "%02X", std::max(0, record->bank));
    }
    ImGui::SameLine();
    ImGui::TextColored(colorAddress, "%04X", record->address);
    bytes_column(record->bytes, colorBytes);

    // The PC is marked in the gutter; the two columns stay so the text lines
    // up with the gap rows.
    ImGui::SameLine();
    ImGui::TextUnformatted("  ");
    if (isPc)
        colorName = yellow;

    ImGui::SameLine();
    std::string instruction = record->name;
    if (view.showSymbols && record->jump)
    {
        Memory::stDisassembleRecord* target =
            memory->GetDisassembleRecord(record->jump_address, false);
        const char* name = symbol_for(symbols, target, record->jump_address);
        if (name)
        {
            char addressText[8];
            std::snprintf(addressText, sizeof(addressText), "$%04X", record->jump_address);
            const std::size_t pos = instruction.rfind(addressText);
            if (pos != std::string::npos)
                instruction.replace(pos, std::strlen(addressText), std::string("{s}") + name);
        }
    }
    gui_debug_draw_instruction(instruction.c_str(), colorName, isBreakpoint || isPc);
}

const ImVec4 kGapColor = ImVec4(0.85f, 0.55f, 0.20f, 1.0f);
const ImVec4 kGapDim = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);

// Segment, bank and address, as an instruction line starts, for a line with
// no breakpoint of its own.
void draw_gap_prefix(Memory* memory, const Line& line, const ImVec4& color)
{
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::Dummy(ImVec2(breakpoint_margin_width(), 1.0f));
    if (view.showSegment)
    {
        ImGui::SameLine();
        ImGui::TextColored(color, "%s", line.segment);
    }
    if (view.showBank && memory->HasMapper())
    {
        ImGui::SameLine();
        ImGui::TextColored(color, "%02X", std::max(0, line.bank));
    }
    ImGui::SameLine();
    ImGui::TextColored(color, "%04X", line.address);
    bytes_column("", color);
    ImGui::SameLine();
    ImGui::TextUnformatted("  ");
}

// The breakpoint margin of an instruction row: a dot when set, a faint one
// under the mouse, a click toggles. Draws after the row's Selectable.
bool breakpoint_margin(Memory* memory, u16 address, bool isBreakpoint, const ImVec2& row,
                       bool clicked)
{
    const float margin = breakpoint_margin_width();
    const float dotSlot = gutter_slot(1);
    const bool inMargin = clicked && ImGui::GetIO().MouseClickedPos[0].x < row.x + dotSlot;
    if (inMargin)
        set_breakpoint(memory, address, !isBreakpoint);
    const bool hovered = ImGui::IsItemHovered() && ImGui::GetIO().MousePos.x < row.x + dotSlot;
    if (isBreakpoint || hovered)
    {
        const float h = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(row.x + dotSlot * 0.5f, row.y + h * 0.5f), h * 0.3f,
            ImGui::GetColorU32(isBreakpoint ? red : ImVec4(red.x, red.y, red.z, 0.35f)));
    }
    if (hovered)
        ImGui::SetTooltip(isBreakpoint ? "Remove the breakpoint" : "Set a breakpoint here");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::Dummy(ImVec2(margin, 1.0f));
    return inMargin;
}

void draw_gap_line(Memory* memory, const Line& line)
{
    const u16 end = static_cast<u16>(line.address + line.count - 1);
    const bool open = gap_is_open(line.address, end);
    if (ImGui::Selectable("", false))
        set_gap_open(line.address, end, !open);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("$%04X-$%04X: %u bytes nothing has decoded yet.\nClick to %s them.",
                          line.address, end, line.count, open ? "hide" : "show");

    draw_gap_prefix(memory, line, kGapColor);
    ImGui::SameLine();
    // The open/closed arrow, drawn: the listing font has no triangle glyphs.
    const float size = ImGui::GetTextLineHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImGui::RenderArrow(ImGui::GetWindowDrawList(), ImVec2(pos.x, pos.y + size * 0.15f),
                       ImGui::GetColorU32(kGapColor), open ? ImGuiDir_Down : ImGuiDir_Right,
                       0.7f);
    ImGui::SameLine();
    ImGui::TextColored(kGapColor, "... %u bytes not seen", line.count);
}

void draw_gap_bytes_line(Memory* memory, const Line& line, bool running)
{
    const bool selected = !view.follow && view.hasSelection && view.selection >= line.address &&
                          view.selection < line.address + line.count;
    if (row_selectable(selected, false))
        select(line.address, running);

    draw_gap_prefix(memory, line, kGapDim);
    ImGui::SameLine();
    char text[16 * 4 + 8];
    int length = std::snprintf(text, sizeof(text), "db ");
    for (int i = 0; i < line.count; i++)
        length += std::snprintf(text + length, sizeof(text) - length, i ? ",$%02X" : "$%02X",
                                memory->Peek(static_cast<u16>(line.address + i)));
    ImGui::TextColored(kGapDim, "%s", text);
}

// demo3's colours for the source.
const ImVec4 kSrcMnemonic = ImVec4(0.44f, 0.66f, 1.00f, 1.0f);
const ImVec4 kSrcRegister = ImVec4(0.78f, 0.85f, 0.44f, 1.0f);
const ImVec4 kSrcNumber = ImVec4(0.69f, 0.55f, 1.00f, 1.0f);
// CODE?, unknown with a suggestion, on the ruler and the minimap (Saverio:
// red unknown, orange unknown with a suggestion).
const ImVec4 kHintColor = ImVec4(0.93f, 0.55f, 0.16f, 1.0f);
// Code you confirmed by hand: light green, beside the dark green of what ran.
const ImVec4 kKeptColor = ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
const ImVec4 kSrcCondition = ImVec4(0.94f, 0.58f, 0.31f, 1.0f);
const ImVec4 kSrcPunctuation = ImVec4(0.55f, 0.58f, 0.65f, 1.0f);
const ImVec4 kSrcName = ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
const ImVec4 kSrcComment = ImVec4(0.50f, 0.53f, 0.60f, 1.0f);
const ImVec4 kSrcLabel = ImVec4(0.96f, 0.96f, 0.98f, 1.0f);
const ImVec4 kSrcAddress = ImVec4(0.31f, 0.76f, 0.91f, 1.0f);
const ImVec4 kCandidateColor = ImVec4(0.90f, 0.84f, 0.66f, 1.0f);
const ImVec4 kCandidateMnemonic = ImVec4(0.84f, 0.71f, 0.42f, 1.0f);
bool is_register(const std::string& word)
{
    static const char* names[] = {"a", "b", "c", "d", "e", "h", "l", "af", "af'", "bc", "de",
                                  "hl", "sp", "ix", "iy", "i", "r", "ixh", "ixl", "iyh", "iyl"};
    for (const char* name : names)
        if (word == name)
            return true;
    return false;
}

bool is_condition(const std::string& word)
{
    return word == "nz" || word == "z" || word == "nc" || word == "c" || word == "po" ||
           word == "pe" || word == "p" || word == "m";
}

// Paints a source line's text: {n} the mnemonic, {o} operands split into
// registers, conditions, numbers and punctuation, {s} a name, {e} an aside.
// A comma gets a space after it, as demo3 writes it.
// Whether a name is drawn as a link: the found names and the user's symbols,
// cheaply, every frame; not port names, which go nowhere.
bool is_link(const std::string& name)
{
    if (name.compare(0, 7, "_LABEL_") == 0 || name.compare(0, 6, "_DATA_") == 0)
        return true;
    for (const auto& entry : view.sourceNames)
        if (entry.second == name)
            return true;
    return false;
}

// The address a name in the listing stands for: a found name carries it
// (_LABEL_A5A_, _DATA_1C3_), a symbol is looked up. Port names go nowhere.
bool name_address(const std::string& name, u16& address)
{
    for (const char* prefix : {"_LABEL_", "_DATA_"})
    {
        const std::size_t length = std::strlen(prefix);
        if (name.compare(0, length, prefix) == 0 && name.size() > length + 1 &&
            name.back() == '_')
        {
            const std::string hex = name.substr(length, name.size() - length - 1);
            if (!hex.empty() && hex.find_first_not_of("0123456789ABCDEFabcdef") == std::string::npos)
            {
                address = static_cast<u16>(std::strtoul(hex.c_str(), NULL, 16));
                return true;
            }
        }
    }
    for (const auto& entry : view.sourceNames)
        if (entry.second == name)
        {
            address = entry.first;
            return true;
        }
    for (const auto& entry : view.portNames)
        if (entry.second == name)
            return false;
    return resolve_address(emu_get_core()->GetMemory(), name, address);
}

void paint_source(const std::string& text, bool candidate, bool dim)
{
    bool first = true;
    auto put = [&](const std::string& piece, ImVec4 color)
    {
        if (piece.empty())
            return;
        if (dim)
            color.w = 0.6f;
        if (!first)
            ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(color, "%s", piece.c_str());
        first = false;
    };

    std::string mnemonic;
    char mode = 'o';
    bool firstOperand = true;
    std::size_t i = 0;
    while (i < text.size())
    {
        if (text[i] == '{' && i + 2 < text.size() && text[i + 2] == '}')
        {
            mode = text[i + 1];
            i += 3;
            continue;
        }
        if (mode == 'n')
        {
            std::size_t end = text.find('{', i);
            if (end == std::string::npos)
                end = text.size();
            std::string word = text.substr(i, end - i);
            mnemonic = word.substr(0, word.find(' '));
            put(word, candidate ? kCandidateMnemonic : kSrcMnemonic);
            i = end;
            continue;
        }
        if (mode == 's' || mode == 'e')
        {
            std::size_t end = text.find('{', i);
            if (end == std::string::npos)
                end = text.size();
            const std::string piece = text.substr(i, end - i);
            put(piece, mode == 's' ? kSrcName : kSrcComment);
            // A name that leads somewhere is a link, as in a web page: under
            // the mouse it is underlined - below the name, whose own
            // underscores a permanent line would blur - with the hand and a
            // tooltip, and a click goes there (Saverio: the jump is followed
            // where you work). The row takes the click as the active item,
            // so hovering is asked allowing for it.
            u16 target = 0;
            if (mode == 's' && is_link(piece))
            {
                const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                    ImGui::IsMouseHoveringRect(min, max) && name_address(piece, target))
                {
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, max.y + 1.0f),
                        ImVec2(max.x, max.y + 1.0f), ImGui::GetColorU32(kSrcName), 1.5f);
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("Click to go to %s (%X)", piece.c_str(), target);
                    if (ImGui::IsMouseClicked(0))
                    {
                        view.linkTarget = target;
                        view.linkFrom = view.paintingAddress;
                        view.linkClicked = true;
                        view.suppressRowClick = true;
                    }
                }
            }
            i = end;
            continue;
        }
        // Operands, one token at a time.
        const char c = text[i];
        if (c == '$')
        {
            std::size_t end = i + 1;
            while (end < text.size() && std::isxdigit(static_cast<unsigned char>(text[end])))
                end++;
            put(text.substr(i, end - i), candidate ? kCandidateColor : kSrcNumber);
            i = end;
        }
        else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
        {
            std::size_t end = i;
            while (end < text.size() &&
                   (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_' ||
                    text[end] == '\''))
                end++;
            const std::string word = text.substr(i, end - i);
            const bool branch = mnemonic == "jp" || mnemonic == "jr" || mnemonic == "call" ||
                                mnemonic == "ret";
            // "c" is the carry condition only as the first operand of a
            // branch that has a second one, or of ret.
            const bool condition = branch && firstOperand && is_condition(word) &&
                (word != "c" || mnemonic == "ret" || text.find(',', end) != std::string::npos);
            ImVec4 color = condition ? kSrcCondition
                         : is_register(word) ? kSrcRegister : kSrcName;
            if (candidate)
                color = kCandidateColor;
            put(word, color);
            i = end;
        }
        else if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+')
        {
            std::size_t end = i + 1;
            while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])))
                end++;
            const bool number = std::isdigit(static_cast<unsigned char>(c)) || end > i + 1;
            put(text.substr(i, end - i), number ? (candidate ? kCandidateColor : kSrcNumber)
                                                : kSrcPunctuation);
            i = end;
        }
        else
        {
            if (c == ',')
                firstOperand = false;
            put(c == ',' ? std::string(", ") : std::string(1, c),
                candidate ? kCandidateColor : kSrcPunctuation);
            i++;
        }
    }
}

// The optional address and bytes columns of the source view: decorations of
// the view, not part of the exported source.
void source_columns(u16 address, const char* bytes, bool dim)
{
    ImVec4 color = kSrcAddress;
    if (dim)
        color.w = 0.6f;
    if (view.showAddresses)
    {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(color, "%04X  ", address);
    }
    if (view.showBytes)
    {
        const float width = ImGui::CalcTextSize("DD CB 7F 00  ").x;
        const float used = ImGui::CalcTextSize(bytes).x;
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(gray, "%s", bytes);
        if (used < width)
        {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::Dummy(ImVec2(width - used, 1.0f));
        }
    }
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextUnformatted("    ");
}

// "... N more bytes": the rest of a long block, shown whole on a click.
void draw_more_line(const Line& line)
{
    const SourceListing::Line& header = view.listing.GetLines()[line.source];
    if (ImGui::Selectable("", false))
        set_block_state(header.offset, Block_Full);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Click to show the whole block; the arrow of its header folds it.");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::Dummy(ImVec2(breakpoint_margin_width(), 1.0f));
    source_columns(line.address, "", true);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(kSrcComment, "... %u more bytes", static_cast<unsigned>(line.count));
}

// A line of the cartridge source, laid out as demo3: gutter, optional
// address and bytes, then the source text indented under its labels.
void draw_source_line(Memory* memory, const Line& line, int pc,
                      std::vector<Memory::stDisassembleRecord*>* breakpoints, bool running)
{
    const SourceListing::Line& source = view.listing.GetLines()[line.source];
    const u8* rom = emu_get_core()->GetCartridge()->GetROM();
    const u16 address = line.address;
    const float margin = breakpoint_margin_width();

    if (source.type == SourceListing::Line_Label)
    {
        ImGui::Dummy(ImVec2(margin, 1.0f));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(kSrcLabel, "%s:", source.text.c_str());
        return;
    }
    if (source.type == SourceListing::Line_Comment)
    {
        ImGui::Dummy(ImVec2(margin, 1.0f));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(kSrcComment, "%s", source.text.c_str());
        return;
    }

    if (source.type == SourceListing::Line_Instruction)
    {
        Memory::stDisassembleRecord* record = memory->GetDisassembleRecord(address, false);
        const bool isBreakpoint = IsValidPointer(record) && is_breakpoint(breakpoints, record);
        const bool isPc = address == pc;
        const bool selected = !view.follow && view.hasSelection && view.selection == address;
        const ImVec2 row = ImGui::GetCursorScreenPos();
        const bool clicked = row_selectable(selected, isPc, ImGuiSelectableFlags_AllowDoubleClick);
        const bool rightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        // The margin helper draws the dot and leaves the cursor after the gutter.
        const bool marginClick = breakpoint_margin(memory, address, isBreakpoint, row, clicked);
        if (clicked && !marginClick)
        {
            Z80Disassembler::Decoded decoded;
            u8 bytes[16] = {};
            for (u32 i = 0; i < 16 && source.offset + i < view.sourceSize; i++)
                bytes[i] = rom[source.offset + i];
            Z80Disassembler::Decode(bytes, 16, address, decoded);
            if (ImGui::IsMouseDoubleClicked(0) && decoded.jump)
                go_to(decoded.jumpAddress, pc);
            else
                select(address, running);
        }
        if (isPc)
            gutter_pc(row);
        // What the map calls CODE ran at some point; kept code has a hollow
        // dot: code because you said so.
        if (source.chosen)
            gutter_kept(row, source.contradicted);
        else
            gutter_executed(row, IsValidPointer(record) ? record->execution_count : 0, true);
        if (source.chosen && rightClicked)
        {
            select(address, running);
            view.openCodeMenu = true;
            view.codeMenuAddress = address;
        }

        char bytesText[24] = "";
        int length = 0;
        for (int i = 0; i < source.count && i < 4; i++)
            length += std::snprintf(bytesText + length, sizeof(bytesText) - length, "%02X ",
                                    rom[source.offset + i]);
        source_columns(address, bytesText, false);
        ImGui::SameLine(0.0f, 0.0f);
        view.paintingAddress = address;
        paint_source(source.text, source.candidate, false);
        if (source.contradicted)
        {
            ImGui::SameLine();
            ImGui::TextColored(red, "; ! read as data since you kept it");
        }
        return;
    }

    const bool unknown = source.kind == RomMap::Kind_Unknown;

    if (source.type == SourceListing::Line_DataHeader)
    {
        const bool open = block_is_open(source);
        const ImVec2 row = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("", false))
            set_block_open(source.offset, !open);
        if (ImGui::IsItemHovered())
        {
            if (source.detail.empty())
                ImGui::SetTooltip("Click to %s the block.", open ? "fold" : "open");
            else
                ImGui::SetTooltip("%s\nClick to %s the block.", source.detail.c_str(),
                                  open ? "fold" : "open");
        }
        // The fold arrow starts the header's text, like a tree node.
        const float h = ImGui::GetTextLineHeight();
        const bool possibleCode = source.kind == RomMap::Kind_CodeCandidate;
        const ImVec4 color = unknown ? kGapColor
                           : possibleCode ? ImVec4(0.88f, 0.64f, 0.23f, 1.0f) : kSrcComment;
        ImGui::RenderArrow(ImGui::GetWindowDrawList(),
                           ImVec2(row.x + margin, row.y + h * 0.15f),
                           ImGui::GetColorU32(color), open ? ImGuiDir_Down : ImGuiDir_Right,
                           0.7f);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::Dummy(ImVec2(margin + h * 1.1f, 1.0f));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(color, "%s", source.text.c_str());
        return;
    }

    // dw rows: one pointer, selected whole; its two bytes go in the bytes
    // column, as an instruction's do (the name hides them). An entry seen in
    // use gets the green dot: proof, as for code that ran.
    if (source.type == SourceListing::Line_Words)
    {
        const bool selected = !view.follow && view.hasSelection && view.selection >= address &&
                              view.selection < address + source.count;
        const ImVec2 row = ImGui::GetCursorScreenPos();
        if (row_selectable(selected, false))
            select(address, running);
        // The right button: keep or forget it as a pointer, from the menu.
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            select(address, running);
            view.openByteMenu = true;
            view.byteMenuAddress = address;
        }
        if (source.seen)
            gutter_seen_entry(row);
        else if (source.chosen)
            gutter_kept(row, false);        // a pointer because you said so
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::Dummy(ImVec2(margin, 1.0f));
        char wordBytes[8];
        const u32 at = static_cast<u32>(address - view.sourceBase);
        std::snprintf(wordBytes, sizeof(wordBytes), "%02X %02X", rom[at],
                      at + 1 < view.sourceSize ? rom[at + 1] : 0);
        source_columns(address, wordBytes, unknown);
        ImGui::SameLine(0.0f, 0.0f);
        view.paintingAddress = address;
        paint_source(source.text, false, unknown);
        return;
    }

    // db rows: each byte is selectable on its own - the first byte of a
    // "try as code" - and the right button offers the preview.
    const bool clicked = row_selectable(false, false);
    const bool rightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::Dummy(ImVec2(margin, 1.0f));
    source_columns(address, "", unknown);
    ImGui::SameLine(0.0f, 0.0f);
    ImVec4 mnemonic = kSrcMnemonic;
    ImVec4 number = kSrcNumber;
    if (unknown)
        mnemonic.w = number.w = 0.6f;
    ImGui::TextColored(mnemonic, "db ");
    // The line's own range: a row the preview cut shows only its part.
    const u32 first = static_cast<u32>(address - view.sourceBase);
    const int count = std::min<int>(line.count, 16);
    float left[16], right[16];
    for (int i = 0; i < count; i++)
    {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(number, "$%02X", rom[first + i]);
        left[i] = ImGui::GetItemRectMin().x;
        right[i] = ImGui::GetItemRectMax().x;
        const u16 byteAddress = static_cast<u16>(address + i);
        if (!view.follow && view.hasSelection && view.selection == byteAddress)
            ImGui::GetWindowDrawList()->AddRect(
                ImVec2(left[i] - 2.0f, ImGui::GetItemRectMin().y),
                ImVec2(right[i] + 2.0f, ImGui::GetItemRectMax().y),
                ImGui::GetColorU32(kSelectionOutline), 0.0f, 0, 1.5f);
        if (i + 1 < count)
        {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextColored(kSrcPunctuation, ", ");
        }
    }
    if (clicked || rightClicked)
    {
        // The byte under the click, or the one before the gap it fell in.
        const float x = rightClicked ? ImGui::GetIO().MouseClickedPos[1].x
                                     : ImGui::GetIO().MouseClickedPos[0].x;
        int picked = 0;
        for (int i = 0; i < count; i++)
            if (x >= left[i])
                picked = i;
        select(static_cast<u16>(address + picked), running);
        if (rightClicked)
        {
            view.openByteMenu = true;
            view.byteMenuAddress = static_cast<u16>(address + picked);
        }
    }
}

void draw_kept_head(const Line& line)
{
    if (line.label < 0 || line.label >= static_cast<int>(view.choices.size()))
        return;
    const SourceListing::CodeRange& range = view.choices[line.label];
    ImGui::Dummy(ImVec2(breakpoint_margin_width() + ImGui::GetTextLineHeight() * 1.1f, 1.0f));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.95f, 0.9f), "; kept as code by you: %X-%X",
                       view.sourceBase + range.start, view.sourceBase + range.end - 1);
    ImGui::SameLine();
    char id[32];
    std::snprintf(id, sizeof(id), "forget##kept%d", line.label);
    if (ImGui::SmallButton(id))
        forget_choice(range.start);
    ImGui::SetItemTooltip("Back to what the map saw; Cmd/Ctrl+Z brings it back.");
}

void draw_kept_word_head(const Line& line)
{
    const u32 at = static_cast<u32>(line.address - view.sourceBase);
    ImGui::Dummy(ImVec2(breakpoint_margin_width() + ImGui::GetTextLineHeight() * 1.1f, 1.0f));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.95f, 0.9f), "; kept as a pointer by you: %X", line.address);
    ImGui::SameLine();
    char id[32];
    std::snprintf(id, sizeof(id), "forget##word%X", at);
    if (ImGui::SmallButton(id))
        forget_word(at);
    ImGui::SetItemTooltip("Back to what the map saw; Cmd/Ctrl+Z brings it back.");
}

void draw_preview_line(const Line& line, bool running)
{
    const std::string& text = view.previewTexts[line.label];
    const float margin = breakpoint_margin_width();
    const ImVec4 amber = ImVec4(0.88f, 0.64f, 0.23f, 1.0f);
    if (line.type == Line_PreviewHead)
    {
        // Indented like a block header, whose arrow it has not.
        ImGui::Dummy(ImVec2(margin + ImGui::GetTextLineHeight() * 1.1f, 1.0f));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(amber, "%s", text.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A preview only, until you keep it. Right-click an instruction\n"
                              "below to start again from it; Esc closes it.");
        ImGui::SameLine();
        if (ImGui::SmallButton("keep as code"))
        {
            const u32 start = static_cast<u32>(view.previewStart - view.sourceBase);
            keep_as_code(start, previewEnd);
            close_preview();
        }
        ImGui::SetItemTooltip("Saved in the project (rommap/choices.txt); Cmd/Ctrl+Z undoes it.");
        ImGui::SameLine();
        if (ImGui::SmallButton("close"))
            close_preview();
        return;
    }
    if (line.type == Line_PreviewLabel)
    {
        ImGui::Dummy(ImVec2(margin, 1.0f));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(amber, "%s:", text.c_str());
        return;
    }
    if (line.type == Line_PreviewStop)
    {
        ImGui::Dummy(ImVec2(margin + ImGui::GetTextLineHeight() * 1.1f, 1.0f));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(amber, "%s", text.c_str());
        return;
    }
    const bool selected = !view.follow && view.hasSelection && view.selection == line.address;
    if (row_selectable(selected, false))
        select(line.address, running);
    // Right button: start the preview again from this instruction.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    {
        select(line.address, running);
        view.openByteMenu = true;
        view.byteMenuAddress = line.address;
    }
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::Dummy(ImVec2(margin, 1.0f));
    const u8* rom = emu_get_core()->GetCartridge()->GetROM();
    char bytes[24] = "";
    int length = 0;
    for (int i = 0; i < line.count && i < 4; i++)
        length += std::snprintf(bytes + length, sizeof(bytes) - length, "%02X ",
                                rom[line.address - view.sourceBase + i]);
    source_columns(line.address, bytes, false);
    ImGui::SameLine(0.0f, 0.0f);
    view.paintingAddress = line.address;
    paint_source(text, true, false);
}

// T-states of one instruction from its bytes: the not-taken and taken
// counts (equal unless it branches or repeats), and how many M1 cycles it
// has, each of which pays the board's M1 wait.
struct Timing
{
    int min;
    int max;
    int m1;
};

Timing instruction_timing(const u8* op)
{
    Timing t = {0, 0, 1};
    if (op[0] == 0xDD || op[0] == 0xFD)
    {
        t.m1 = 2;
        if (op[1] == 0xCB)
            t.min = kOPCodeXYCBTStates[op[3]];
        else
        {
            t.min = kOPCodeXYTStates[op[1]];
            if (t.min == 0)
                t.min = 8;
            t.max = t.min + kOPCodeTStatesBranched[op[1]];
        }
    }
    else if (op[0] == 0xCB)
    {
        t.m1 = 2;
        t.min = kOPCodeCBTStates[op[1]];
    }
    else if (op[0] == 0xED)
    {
        t.m1 = 2;
        t.min = kOPCodeEDTStates[op[1]];
        // LDIR, CPIR, INIR, OTIR and the decrementing ones: 5 more while
        // they repeat.
        if ((op[1] & 0xF4) == 0xB0)
            t.max = t.min + 5;
    }
    else
    {
        t.min = kOPCodeTStates[op[0]];
        t.max = t.min + kOPCodeTStatesBranched[op[0]];
    }
    if (t.max < t.min)
        t.max = t.min;
    return t;
}

// The status bar speaks of the selection, or of the PC while following.
struct Subject
{
    bool valid = false;
    u16 address = 0;
    bool code = false;          // inside a decoded instruction
    u16 start = 0;              // that instruction's address
    int size = 0;
    Timing timing = {0, 0, 0};
    char segment[5] = "";
    int bank = 0;
    std::string name;           // nearest label at or before, with an offset
    int operandDigits = 0;      // 2 or 4 when the instruction has an operand
    u16 operand = 0;
    std::string kind = "not seen";
    ImVec4 kindColor = ImVec4(0.85f, 0.55f, 0.20f, 1.0f);
};

// The first $-number in the decoded text is the instruction's operand: an
// immediate, an address, a displacement or a branch target.
void find_operand(const char* text, Subject& subject)
{
    for (const char* p = std::strchr(text, '$'); p; p = std::strchr(p + 1, '$'))
    {
        int digits = 0;
        while (std::isxdigit(static_cast<unsigned char>(p[1 + digits])))
            digits++;
        if (digits == 2 || digits == 4)
        {
            subject.operandDigits = digits;
            subject.operand = static_cast<u16>(std::strtoul(p + 1, NULL, 16));
            return;
        }
    }
}

Subject find_subject(Memory* memory, int pc)
{
    Subject subject;
    subject.address = focus_address(pc);
    const int index = line_of(subject.address);
    if (index < 0)
        return subject;

    const Line& line = view.lines[index];
    if (line.type == Line_PreviewInstruction && view.sourceActive)
    {
        const u8* rom = emu_get_core()->GetCartridge()->GetROM();
        u8 bytes[16] = {};
        const u32 at = static_cast<u32>(line.address - view.sourceBase);
        for (u32 i = 0; i < 16 && at + i < view.sourceSize; i++)
            bytes[i] = rom[at + i];
        Z80Disassembler::Decoded decoded;
        Z80Disassembler::Decode(bytes, 16, line.address, decoded);
        std::memcpy(subject.segment, "ROM", 4);
        subject.valid = true;
        subject.code = true;
        subject.start = line.address;
        subject.size = decoded.size;
        subject.timing = instruction_timing(bytes);
        if (decoded.operandKind == Z80Disassembler::Decoded::Operand_Imm8 ||
            decoded.operandKind == Z80Disassembler::Decoded::Operand_Port)
            subject.operandDigits = 2, subject.operand = decoded.operand;
        else if (decoded.operandKind != Z80Disassembler::Decoded::Operand_None)
            subject.operandDigits = decoded.rst ? 2 : 4, subject.operand = decoded.operand;
        subject.kind = "preview";
        subject.kindColor = ImVec4(0.88f, 0.64f, 0.23f, 1.0f);
        return subject;
    }
    if (line.type == Line_Source)
    {
        const SourceListing::Line& source = view.listing.GetLines()[line.source];
        const u8* rom = emu_get_core()->GetCartridge()->GetROM();
        std::memcpy(subject.segment, "ROM", 4);
        subject.bank = 0;
        if (source.type == SourceListing::Line_Instruction)
        {
            u8 bytes[16] = {};
            for (u32 i = 0; i < 16 && source.offset + i < view.sourceSize; i++)
                bytes[i] = rom[source.offset + i];
            Z80Disassembler::Decoded decoded;
            Z80Disassembler::Decode(bytes, 16, line.address, decoded);
            subject.code = true;
            subject.start = line.address;
            subject.size = decoded.size;
            subject.timing = instruction_timing(bytes);
            if (decoded.operandKind == Z80Disassembler::Decoded::Operand_Imm8 ||
                decoded.operandKind == Z80Disassembler::Decoded::Operand_Port)
            {
                subject.operandDigits = 2;
                subject.operand = decoded.operand;
            }
            else if (decoded.operandKind != Z80Disassembler::Decoded::Operand_None)
            {
                subject.operandDigits = decoded.rst ? 2 : 4;
                subject.operand = decoded.operand;
            }
            subject.kind = source.candidate ? "CODE?" : "CODE";
            subject.kindColor = source.candidate ? ImVec4(0.85f, 0.65f, 0.25f, 1.0f) : green;
        }
        else
        {
            const u32 offset = subject.address - view.sourceBase;
            const u8 raw = offset < view.sourceSize ? source_map_byte(offset) : 0;
            RomMap::Range range = {offset, offset, static_cast<u8>(raw & RomMap::KindMask),
                                   (raw & RomMap::CandidateFlag) != 0};
            subject.kind = RomMap::RangeName(range);
            subject.kindColor = range.kind == RomMap::Kind_Unknown
                ? ImVec4(0.85f, 0.55f, 0.20f, 1.0f) : gui_debug_rom_map_kind_color(range.kind);
        }
    }
    else if (line.type == Line_Instruction)
    {
        Memory::stDisassembleRecord* record = memory->GetDisassembleRecord(line.address, false);
        if (!IsValidPointer(record) || record->name[0] == 0)
            return subject;
        if (subject.address >= line.address + record->size)
            return subject; // past the last instruction, nothing there
        subject.code = true;
        subject.start = line.address;
        subject.size = record->size;
        subject.timing = instruction_timing(record->opcodes);
        find_operand(record->name, subject);
        std::memcpy(subject.segment, record->segment, sizeof(subject.segment));
        subject.bank = record->bank;
        subject.kind = "CODE";
        subject.kindColor = green;
    }
    else
    {
        if (line.type == Line_Gap &&
            subject.address >= line.address + line.count)
            return subject;
        std::memcpy(subject.segment, line.segment, sizeof(subject.segment));
        subject.bank = line.bank;
    }
    subject.valid = true;

    for (int i = index; i >= 0; i--)
    {
        const Line& candidate = view.lines[i];
        const char* label = NULL;
        if (candidate.type == Line_Label)
            label = view.labels[candidate.label].c_str();
        else if (candidate.type == Line_Source &&
                 view.listing.GetLines()[candidate.source].type == SourceListing::Line_Label)
            label = view.listing.GetLines()[candidate.source].text.c_str();
        if (!label)
            continue;
        const int offset = subject.address - candidate.address;
        subject.name = label;
        if (offset > 0)
        {
            char hex[12];
            std::snprintf(hex, sizeof(hex), "+$%02X", offset);
            subject.name += hex;
        }
        break;
    }
    return subject;
}

void status_text(ImDrawList* draw, const ImVec2& pos, float width, float height,
                 const ImVec4& color, const char* text)
{
    const ImVec4 clip(pos.x, pos.y, pos.x + width, pos.y + height);
    draw->AddText(NULL, 0.0f, pos, ImGui::GetColorU32(color), text, NULL, 0.0f, &clip);
}

// Two lines, every cell at a fixed place and width, whatever is selected:
//   ROM 00:8C97  SUB_00_8C90+7          $CD  %11001101  205
//   3 bytes  T 7/12 +2 wait   CODE      <message>
void draw_status_bar(Memory* memory, int pc, float lineHeight)
{
    const Subject subject = find_subject(memory, pc);
    const float charWidth = ImGui::CalcTextSize("W").x;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(width, lineHeight * 2.0f));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    auto cell = [&](int line, int column, int chars, const ImVec4& color, const char* text)
    {
        const float x = origin.x + column * charWidth;
        const float right = std::min(origin.x + width, x + chars * charWidth);
        if (right > x)
            status_text(draw, ImVec2(x, origin.y + line * lineHeight), right - x,
                        lineHeight, color, text);
    };

    // A field is its name in grey and its value after it, as in demo3's
    // status bar ("Address: ... Value: ... Type: ..."): numbers alone did
    // not say what they were.
    auto field = [&](int line, int column, int chars, const char* name, const ImVec4& color,
                     const char* value)
    {
        const int nameChars = static_cast<int>(std::strlen(name));
        cell(line, column, chars, kSrcComment, name);
        cell(line, column + nameChars, chars - nameChars, color, value);
    };

    char text[96];
    if (!subject.valid)
    {
        field(0, 0, 40, "Address: ", gray, "-");
    }
    else
    {
        if (subject.name.empty())
            std::snprintf(text, sizeof(text), "%s%02X:%04X", subject.segment,
                          std::max(0, subject.bank), subject.address);
        else
            std::snprintf(text, sizeof(text), "%s%02X:%04X (%s)", subject.segment,
                          std::max(0, subject.bank), subject.address, subject.name.c_str());
        field(0, 0, 44, "Address: ", white, text);

        // On code, the operand; on a byte nothing decoded, the byte.
        const bool word = subject.code && subject.operandDigits == 4;
        const bool none = subject.code && subject.operandDigits == 0;
        const u16 value = subject.code ? subject.operand : memory->Peek(subject.address);
        const char* name = subject.code ? "Operand: " : "Value: ";
        if (none)
            std::snprintf(text, sizeof(text), "-");
        else if (word)
            std::snprintf(text, sizeof(text), "$%04X | %u", value, value);
        else
        {
            char bits[10];
            for (int b = 0; b < 8; b++)
                bits[b] = (value & (0x80 >> b)) ? '1' : '0';
            bits[8] = 0;
            std::snprintf(text, sizeof(text), "$%02X | %%%s | %u", value, bits, value);
        }
        field(0, 45, 36, name, white, text);

        if (subject.code)
        {
            const unsigned int wait =
                emu_get_core()->GetSystemBus()->GetWaitCyclesM1() * subject.timing.m1;
            char cycles[40];
            int length = subject.timing.max != subject.timing.min
                ? std::snprintf(cycles, sizeof(cycles), "%d / %d", subject.timing.min,
                                subject.timing.max)
                : std::snprintf(cycles, sizeof(cycles), "%d", subject.timing.min);
            if (wait)
                std::snprintf(cycles + length, sizeof(cycles) - length, " +%u wait", wait);
            std::snprintf(text, sizeof(text), "%d byte%s", subject.size,
                          subject.size == 1 ? "" : "s");
            field(1, 0, 14, "Size: ", white, text);
            field(1, 14, 30, "Cycles: ", white, cycles);
        }
        else
        {
            field(1, 0, 14, "Size: ", white, "1 byte");
            field(1, 14, 30, "Cycles: ", white, "-");
        }
        field(1, 45, 16, "Type: ", subject.kindColor, subject.kind.c_str());
    }

    // The message takes what is left of the second line; with none, why the
    // project has no source view, if that is the case.
    const std::string& message = view.message.empty() ? view.sourceNote : view.message;
    const float messageX = origin.x + 62 * charWidth;
    if (!message.empty() && messageX < origin.x + width)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, orange);
        const ImVec2 min(messageX, origin.y + lineHeight);
        const ImVec2 max(origin.x + width, origin.y + 2.0f * lineHeight);
        ImGui::RenderTextEllipsis(draw, min, max, max.x, message.c_str(), NULL, NULL);
        ImGui::PopStyleColor();
    }

    if (hovered)
    {
        ImGui::BeginTooltip();
        if (subject.valid && subject.code)
            ImGui::Text("Instruction at $%04X; the value is its operand.\n"
                        "T-states are the Z80's own: not taken / taken\n"
                        "for branches and repeats. The board adds %u wait per M1 cycle\n"
                        "(this instruction has %d).",
                        subject.start, emu_get_core()->GetSystemBus()->GetWaitCyclesM1(),
                        subject.timing.m1);
        else if (subject.valid)
            ImGui::TextUnformatted("A byte nothing has decoded yet: its value as the CPU sees it now.");
        else
            ImGui::TextUnformatted("Nothing decoded here yet.");
        if (!message.empty())
        {
            ImGui::Separator();
            ImGui::TextUnformatted(message.c_str());
        }
        ImGui::EndTooltip();
    }
}

struct BreakpointInput
{
    char cpu[40] = "";
    char mem[16] = "";
    bool memRead = true;
    bool memWrite = true;
};
BreakpointInput breakpointInput;

// A list of fixed height, so opening the panel is the only thing that moves.
const int kBreakpointRows = 4;

void add_memory_breakpoint(Memory* memory)
{
    const std::string text = trimmed(breakpointInput.mem);
    u16 first = 0, second = 0;
    bool range = false;
    const std::size_t dash = text.find('-');
    try
    {
        first = static_cast<u16>(std::stoul(text.substr(0, dash), NULL, 16));
        if (dash != std::string::npos)
        {
            second = static_cast<u16>(std::stoul(text.substr(dash + 1), NULL, 16));
            range = true;
        }
    }
    catch (const std::exception&)
    {
        view.message = "Memory breakpoint: use XXXX or XXXX-XXXX.";
        return;
    }
    if (!breakpointInput.memRead && !breakpointInput.memWrite)
    {
        view.message = "Memory breakpoint: choose read, write or both.";
        return;
    }
    breakpointInput.mem[0] = 0;
    for (const Memory::stMemoryBreakpoint& b : *memory->GetBreakpointsMem())
        if (b.address1 == first && b.address2 == second && b.range == range)
            return;
    Memory::stMemoryBreakpoint b;
    b.address1 = first;
    b.address2 = second;
    b.range = range;
    b.read = breakpointInput.memRead;
    b.write = breakpointInput.memWrite;
    memory->GetBreakpointsMem()->push_back(b);
}

void draw_breakpoints_panel(Memory* memory, int pc,
                            const std::unordered_map<u32, std::string>& symbols)
{
    int cpuCount = 0;
    for (Memory::stDisassembleRecord* b : *memory->GetBreakpointsCPU())
        if (IsValidPointer(b))
            cpuCount++;
    const int memCount = static_cast<int>(memory->GetBreakpointsMem()->size());

    char header[64];
    std::snprintf(header, sizeof(header), "Breakpoints  CPU %d  Memory %d###breakpoints",
                  cpuCount, memCount);
    if (!ImGui::CollapsingHeader(header))
        return;

    const float listHeight = kBreakpointRows * ImGui::GetTextLineHeightWithSpacing();
    if (!ImGui::BeginTable("##breakpoints", 2, ImGuiTableFlags_BordersInnerV))
        return;
    ImGui::TableNextColumn();
    {
        ImGui::Checkbox("Disable CPU", &emu_debug_disable_breakpoints_cpu);
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Add").x -
                                ImGui::GetStyle().FramePadding.x * 2.0f -
                                ImGui::GetStyle().ItemSpacing.x);
        bool add = ImGui::InputTextWithHint("##cpu", "address or symbol", breakpointInput.cpu,
            IM_ARRAYSIZE(breakpointInput.cpu),
            ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        add |= ImGui::Button("Add##cpu");
        if (add)
        {
            const std::string text = trimmed(breakpointInput.cpu);
            u16 address = 0;
            if (text.empty() && view.hasSelection)
                set_breakpoint(memory, view.selection, true);
            else if (resolve_address(memory, text, address))
                set_breakpoint(memory, address, true);
            else if (!text.empty())
                view.message = "Breakpoint: no address or symbol called \"" + text + "\".";
            breakpointInput.cpu[0] = 0;
        }
        ImGui::SetItemTooltip("Empty: the selected line");

        if (ImGui::BeginChild("##cpu_list", ImVec2(0, listHeight)))
        {
            std::vector<Memory::stDisassembleRecord*>* breakpoints = memory->GetBreakpointsCPU();
            for (std::size_t i = 0; i < breakpoints->size(); i++)
            {
                Memory::stDisassembleRecord* b = (*breakpoints)[i];
                if (!IsValidPointer(b))
                    continue;
                ImGui::PushID(static_cast<int>(i));
                const u16 address = b->address;
                if (ImGui::SmallButton("X"))
                    set_breakpoint(memory, address, false);
                ImGui::SameLine();
                const char* name = symbol_for(symbols, b, address);
                char text[64];
                std::snprintf(text, sizeof(text), "%04X  %s", address, name ? name : "");
                ImGui::PushStyleColor(ImGuiCol_Text, red);
                if (ImGui::Selectable(text))
                    go_to(address, pc);
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
    ImGui::TableNextColumn();
    {
        ImGui::Checkbox("Disable memory", &emu_debug_disable_breakpoints_mem);
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("XXXX-XXXXW").x);
        bool add = ImGui::InputTextWithHint("##mem", "XXXX-XXXX", breakpointInput.mem,
            IM_ARRAYSIZE(breakpointInput.mem),
            ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::Checkbox("R", &breakpointInput.memRead);
        ImGui::SameLine();
        ImGui::Checkbox("W", &breakpointInput.memWrite);
        ImGui::SameLine();
        add |= ImGui::Button("Add##mem");
        if (add)
            add_memory_breakpoint(memory);

        if (ImGui::BeginChild("##mem_list", ImVec2(0, listHeight)))
        {
            std::vector<Memory::stMemoryBreakpoint>* breakpoints = memory->GetBreakpointsMem();
            int remove = -1;
            for (std::size_t i = 0; i < breakpoints->size(); i++)
            {
                const Memory::stMemoryBreakpoint& b = (*breakpoints)[i];
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::SmallButton("X"))
                    remove = static_cast<int>(i);
                ImGui::SameLine();
                if (b.range)
                    ImGui::TextColored(red, "%04X-%04X", b.address1, b.address2);
                else
                    ImGui::TextColored(red, "%04X", b.address1);
                ImGui::SameLine();
                ImGui::TextColored(gray, "%s%s", b.read ? "R" : "", b.write ? "W" : "");
                ImGui::PopID();
            }
            if (remove >= 0)
                breakpoints->erase(breakpoints->begin() + remove);
        }
        ImGui::EndChild();
    }
    ImGui::EndTable();
}

// Places the PC line: nothing while it sits in the 10%-90% band, back to 30%
// above it and to 80% below it.
void keep_pc_in_band(int pcLine, float lineHeight)
{
    const float height = ImGui::GetWindowHeight();
    const float top = ImGui::GetScrollY();
    const float y = pcLine * lineHeight;
    float target;
    if (y < top + height * 0.10f)
        target = std::max(0.0f, y - height * 0.30f);
    else if (y > top + height * 0.90f - lineHeight)
        target = y - height * 0.80f;
    else
        return;
    // At the top of the listing the band cannot be reached: leave it.
    if (std::fabs(target - top) < 0.5f)
        return;
    ImGui::SetScrollY(target);
    view.scrolledByUs = true;
}
}

namespace
{
// The jump shown: the selected instruction's, or the PC's while following.
// Calls and rst are left out - they come back - as demo3 does.
bool jump_of_line(Memory* memory, int index, u16& target)
{
    const Line& line = view.lines[index];
    if (line.type == Line_Instruction)
    {
        Memory::stDisassembleRecord* record = memory->GetDisassembleRecord(line.address, false);
        if (!IsValidPointer(record) || !record->jump || record->subroutine)
            return false;
        target = record->jump_address;
        return true;
    }
    if (line.type == Line_Source && view.sourceActive &&
        view.listing.GetLines()[line.source].type == SourceListing::Line_Instruction)
    {
        const u8* rom = emu_get_core()->GetCartridge()->GetROM();
        u8 bytes[16] = {};
        const u32 at = static_cast<u32>(line.address - view.sourceBase);
        for (u32 i = 0; i < 16 && at + i < view.sourceSize; i++)
            bytes[i] = rom[at + i];
        Z80Disassembler::Decoded decoded;
        Z80Disassembler::Decode(bytes, 16, line.address, decoded);
        if (!decoded.jump || decoded.subroutine)
            return false;
        target = decoded.jumpAddress;
        return true;
    }
    return false;
}

// From the jump to its target, in the gutter's last slot: a green bracket
// with an arrow at the target. A jump backwards is a loop, and its body is
// tinted - only then, so the tint means one thing.
std::string without_markers(const std::string& text)
{
    std::string plain;
    for (std::size_t i = 0; i < text.size(); i++)
    {
        if (text[i] == '{' && i + 2 < text.size() && text[i + 2] == '}')
        {
            i += 2;
            continue;
        }
        plain += text[i];
    }
    return plain;
}

// A line as the search sees it: its address and its text, lower case.
std::string searchable_text(Memory* memory, const Line& line)
{
    std::string text;
    char address[8];
    std::snprintf(address, sizeof(address), "%04X ", line.address);
    switch (line.type)
    {
        case Line_Label:
            text = view.labels[line.label];
            break;
        case Line_Instruction:
        {
            Memory::stDisassembleRecord* record = memory->GetDisassembleRecord(line.address, false);
            if (IsValidPointer(record))
                text = address + without_markers(record->name);
            break;
        }
        case Line_Source:
        {
            const SourceListing::Line& source = view.listing.GetLines()[line.source];
            text = without_markers(source.text) + " " + source.detail;
            if (source.type != SourceListing::Line_Label)
                text = address + text;
            break;
        }
        case Line_PreviewHead:
        case Line_PreviewInstruction:
        case Line_PreviewStop:
        case Line_PreviewLabel:
            text = without_markers(view.previewTexts[line.label]);
            if (line.type == Line_PreviewInstruction)
                text = address + text;
            break;
        case Line_Gap:
            text = std::string(address) + "bytes not seen";
            break;
        default:
            break;
    }
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

void rebuild_find(Memory* memory)
{
    std::string needle = view.findText;
    for (char& c : needle)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (needle == view.findFor && view.findLines == view.lines.size())
        return;
    view.findFor = needle;
    view.findLines = view.lines.size();
    view.findHits.clear();
    view.findIndex = -1;
    if (needle.empty())
        return;
    for (int i = 0; i < static_cast<int>(view.lines.size()); i++)
        if (searchable_text(memory, view.lines[i]).find(needle) != std::string::npos)
            view.findHits.push_back(i);
}

// The next (or previous) hit after the line in view, wrapping around.
void find_step(Memory* memory, int pc, int direction)
{
    rebuild_find(memory);
    if (view.findHits.empty())
    {
        view.message = view.findFor.empty() ? "" : "Find: nothing matches.";
        return;
    }
    const int current = line_of(focus_address(pc));
    int next = -1;
    const int count = static_cast<int>(view.findHits.size());
    if (direction > 0)
    {
        for (int k = 0; k < count && next < 0; k++)
            if (view.findHits[k] > current)
                next = k;
        if (next < 0)
            next = 0;
    }
    else
    {
        for (int k = count - 1; k >= 0 && next < 0; k--)
            if (view.findHits[k] < current)
                next = k;
        if (next < 0)
            next = count - 1;
    }
    view.findIndex = next;
    show_address(view.lines[view.findHits[next]].address);
}

void draw_jump_line(int from, int to, int firstIndex, float firstY, float left, float lineHeight)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float h = ImGui::GetTextLineHeight();
    const ImU32 green = ImGui::GetColorU32(ImVec4(0.31f, 0.69f, 0.38f, 1.0f));
    const float yFrom = firstY + (from - firstIndex) * lineHeight + h * 0.5f;
    const float yTo = firstY + (to - firstIndex) * lineHeight + h * 0.5f;
    const float x = left + gutter_slot(3) + h * 0.15f;
    const float tip = left + gutter_slot(3) + h * 1.1f;
    if (to < from)
        draw->AddRectFilled(ImVec2(left, yTo - h * 0.5f),
                            ImVec2(left + 8000.0f, yFrom - h * 0.5f + lineHeight),
                            ImGui::GetColorU32(ImVec4(0.31f, 0.69f, 0.38f, 0.07f)));
    draw->AddLine(ImVec2(x, yFrom), ImVec2(x, yTo), green, 1.5f);
    draw->AddLine(ImVec2(x, yFrom), ImVec2(tip - h * 0.3f, yFrom), green, 1.5f);
    draw->AddLine(ImVec2(x, yTo), ImVec2(tip - h * 0.3f, yTo), green, 1.5f);
    draw->AddTriangleFilled(ImVec2(tip, yTo), ImVec2(tip - h * 0.35f, yTo - h * 0.25f),
                            ImVec2(tip - h * 0.35f, yTo + h * 0.25f), green);
}

}

namespace
{
// What a listing line is to the navigator: three areas only (Saverio) -
// code, data and unknown - and how long its text is, for the minimap's
// bars. Unknown takes possible code and the gaps too: nobody recognised
// them; it is the one that shows and juts out. Labels, blank lines and
// headers are neutral: the bytes around them speak.
enum StripArea : u8
{
    Area_None,
    Area_Code,
    Area_Kept,      // code you confirmed by hand: sure, not seen running (light green)
    Area_Data,
    Area_Hint,      // CODE?: unknown with a suggestion (Saverio: orange)
    Area_Unknown
};

StripArea strip_area(Memory* memory, const Line& line, int& length)
{
    (void)memory;
    length = 0;
    switch (line.type)
    {
        case Line_Instruction:
        case Line_PreviewInstruction:
            length = 14;
            return Area_Code;
        case Line_Gap:
        case Line_GapBytes:
            length = 30;
            return Area_Unknown;
        case Line_Source:
        {
            const SourceListing::Line& source = view.listing.GetLines()[line.source];
            if (source.type == SourceListing::Line_Label ||
                source.type == SourceListing::Line_DataHeader ||
                source.type == SourceListing::Line_Comment)
                return Area_None;
            length = static_cast<int>(without_markers(source.text).size());
            if (source.type == SourceListing::Line_Instruction)
                return source.chosen ? Area_Kept : Area_Code;
            if (source.kind == RomMap::Kind_CodeCandidate)
                return Area_Hint;
            if (source.kind == RomMap::Kind_Unknown)
                return Area_Unknown;
            return Area_Data;
        }
        default:
            return Area_None;
    }
}

bool strip_color(Memory* memory, const Line& line, ImU32& color, bool& juts)
{
    int length;
    const StripArea area = strip_area(memory, line, length);
    juts = area == Area_Unknown || area == Area_Hint;
    switch (area)
    {
        case Area_Code: color = ImGui::GetColorU32(ImVec4(0.22f, 0.40f, 0.27f, 1.0f)); return true;
        case Area_Hint: color = ImGui::GetColorU32(kHintColor); return true;
        case Area_Kept: color = ImGui::GetColorU32(kKeptColor); return true;
        case Area_Data: color = ImGui::GetColorU32(ImVec4(0.25f, 0.31f, 0.44f, 1.0f)); return true;
        case Area_Unknown: color = ImGui::GetColorU32(ImVec4(0.92f, 0.30f, 0.26f, 1.0f)); return true;
        default: return false;
    }
}

// Brings a listing line into view at 30% of the height and selects it: a
// click on the minimap or the ruler lands on a line you can see, not on a
// spot the view merely centres near.
void reveal_line(int index, float lineHeight)
{
    if (index < 0 || index >= static_cast<int>(view.lines.size()))
        return;
    view.requestScrollY = std::max(0.0f, std::min(view.listScrollMaxY,
        index * lineHeight - view.listHeight * 0.30f));
    view.follow = false;
    view.hasSelection = true;
    view.selection = view.lines[index].address;
}

// A line as the navigator's tooltip names it: its address and what it is.
std::string navigator_describe(Memory* memory, int index)
{
    // Labels and blank lines say nothing: the next line that does.
    ImU32 color;
    bool juts;
    for (int i = index; i < static_cast<int>(view.lines.size()) && i < index + 4; i++)
        if (strip_color(memory, view.lines[i], color, juts))
        {
            index = i;
            break;
        }
    const Line& line = view.lines[index];
    char text[160];
    std::snprintf(text, sizeof(text), "%04X: ", line.address);
    std::string what = text;
    switch (line.type)
    {
        case Line_Instruction:
            what += "code (the debugger decoded it)";
            break;
        case Line_Gap:
        case Line_GapBytes:
            what += "bytes not seen";
            break;
        case Line_Source:
        {
            const SourceListing::Line& source = view.listing.GetLines()[line.source];
            if (source.type == SourceListing::Line_Instruction)
            {
                what += source.chosen ? "code you kept" : "code that ran";
                break;
            }
            // The block's header says the rest.
            int header = source.type == SourceListing::Line_DataHeader ? line.source : source.block;
            if (header >= 0)
            {
                const std::string& h = view.listing.GetLines()[header].text;
                const std::size_t dash = h.find(" - ");
                if (source.kind == RomMap::Kind_Unknown)
                    what += "unknown";
                else
                    what += dash != std::string::npos ? h.substr(dash + 3) : "data";
            }
            break;
        }
        default:
            what += "-";
            break;
    }
    return what;
}

// The line a click on the ruler at pixel row y leads to. The unknown rows
// are often a single pixel high, too thin for the hand: within a few pixels
// of one the click snaps to it; otherwise it is the first line of the row.
int ruler_line(Memory* memory, int y, int rows, int count)
{
    auto range = [&](int r, int& from, int& to)
    {
        from = static_cast<int>(static_cast<long long>(r) * count / rows);
        to = static_cast<int>(static_cast<long long>(r + 1) * count / rows);
        if (to <= from)
            to = from + 1;
    };
    const int snap = 4;
    for (int d = 0; d <= snap; d++)
        for (int side = 0; side < (d == 0 ? 1 : 2); side++)
        {
            const int r = side == 0 ? y - d : y + d;
            if (r < 0 || r >= rows)
                continue;
            int from, to;
            range(r, from, to);
            for (int i = from; i < to && i < count; i++)
            {
                int length;
                const StripArea area = strip_area(memory, view.lines[i], length);
                if (area == Area_Unknown || area == Area_Hint)
                    return i;
            }
        }
    int from, to;
    range(std::max(0, std::min(rows - 1, y)), from, to);
    for (int i = from; i < to && i < count; i++)
    {
        int length;
        if (strip_area(memory, view.lines[i], length) != Area_None)
            return i;
    }
    return std::min(from, count - 1);
}

// The navigator: the whole listing in a thin column, by lines not bytes.
// The visible part is a hollow frame round it - the colours under it stay
// readable - that is grabbed and dragged; a click elsewhere goes there.
// Thin marks: yellow the PC, white the selection, red the breakpoints.
void draw_navigator(Memory* memory, int pc, float width, float height, float lineHeight)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##navigator", ImVec2(width, height));
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const int count = static_cast<int>(view.lines.size());
    const float strip = width * 0.80f;                // the column proper
    const float x0 = origin.x + (width - strip) * 0.5f;
    const float x1 = x0 + strip;
    // Nothing loaded, nothing drawn: an empty column read as a grey band.
    if (count == 0 || height < 4.0f)
        return;
    draw->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x1, origin.y + height),
                        ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.03f)));

    // One pixel row at a time, as VS Code's minimap: a bar as long as the
    // longest text among the lines it stands for, so the listing's shape
    // shows; unknown wins the row, fills it and juts out.
    const int rows = static_cast<int>(height);
    const ImU32 redRow = ImGui::GetColorU32(ImVec4(0.92f, 0.30f, 0.26f, 1.0f));
    for (int y = 0; y < rows; y++)
    {
        const int from = static_cast<int>(static_cast<long long>(y) * count / rows);
        int to = static_cast<int>(static_cast<long long>(y + 1) * count / rows);
        if (to <= from)
            to = from + 1;
        ImU32 color = 0;
        bool juts = false;
        int longest = 0;
        for (int i = from; i < to && i < count; i++)
        {
            ImU32 c;
            bool j;
            if (!strip_color(memory, view.lines[i], c, j))
                continue;
            int length = 0;
            // Unknown wins the row, then a hint: what is not sure shows.
            const StripArea area = strip_area(memory, view.lines[i], length);
            if (j && (!juts || (area == Area_Unknown && color != redRow)))
            {
                juts = true;
                color = area == Area_Unknown ? redRow : c;
            }
            else if (!juts && length >= longest)
                color = c;
            longest = std::max(longest, length);
        }
        if (color == 0)
            continue;
        float left = x0, right = x1;
        if (juts)
            left = origin.x + 1.0f, right = origin.x + width - 1.0f;
        else
            right = x0 + (x1 - x0) * std::min(1.0f, 0.25f + longest / 48.0f);
        draw->AddRectFilled(ImVec2(left, origin.y + y), ImVec2(right, origin.y + y + 1.0f), color);
    }

    // The frame: the listing's visible part. Bold, wider than the navigator
    // on both sides and never shorter than a line and a half, so it is found
    // at a glance however long the ROM (Saverio: "devo vederlo meglio").
    // It sits on the lines really shown - the colours at its centre are what
    // the listing shows - and a too thin slice grows around that centre.
    const float total = count * lineHeight;
    const float minFrame = ImGui::GetTextLineHeight() * 1.5f;
    const float slice0 = total > 0.0f ? view.listScrollY / total * height : 0.0f;
    const float slice1 = total > 0.0f ? (view.listScrollY + view.listHeight) / total * height : height;
    const float frameHeight = std::min(height, std::max(minFrame, slice1 - slice0));
    const float centre = (slice0 + slice1) * 0.5f;
    const float frameTop = origin.y +
        std::max(0.0f, std::min(height - frameHeight, centre - frameHeight * 0.5f));
    const float out = 3.0f;
    const ImVec2 fmin(origin.x - out, frameTop), fmax(origin.x + width + out, frameTop + frameHeight);
    // The minimap's span, when it is shown and does not hold the whole
    // listing: a faint band behind the frame, linking the two columns (the
    // frame is what the listing shows, the band what the minimap shows).
    if (view.showMinimap && view.miniRows > 0 && view.miniRows < count)
    {
        const float y0 = origin.y + view.miniFirst * height / count;
        const float y1 = origin.y + std::min(count, view.miniFirst + view.miniRows) * height / count;
        draw->AddRectFilled(ImVec2(origin.x, y0), ImVec2(origin.x + width, y1),
                            ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.10f)), 2.0f);
    }
    // Rounded and semi-transparent, less so than the minimap's slider, with
    // a faint outline: the bold white frame was too white (Saverio).
    // A bold white outline over a dark one, no fill: semi-transparent, as
    // the minimap's slider, it could not be seen on the ruler (Saverio tried).
    const ImU32 ink = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, hovered || active ? 1.0f : 0.92f));
    draw->AddRect(ImVec2(fmin.x - 1.5f, fmin.y - 1.5f), ImVec2(fmax.x + 1.5f, fmax.y + 1.5f),
                  ImGui::GetColorU32(ImVec4(0, 0, 0, 0.95f)), 2.0f, 0, 2.0f);
    draw->AddRect(fmin, fmax, ink, 2.0f, 0, 2.5f);

    // Marks.
    auto mark = [&](int index, const ImVec4& color, float thickness)
    {
        if (index < 0)
            return;
        const float y = origin.y + (index + 0.5f) * height / count;
        draw->AddRectFilled(ImVec2(origin.x, y - thickness * 0.5f),
                            ImVec2(origin.x + width, y + thickness * 0.5f), ImGui::GetColorU32(color));
    };
    for (Memory::stDisassembleRecord* b : *memory->GetBreakpointsCPU())
        if (IsValidPointer(b))
            mark(line_of(b->address), ImVec4(0.95f, 0.25f, 0.25f, 1.0f), 2.0f);
    // The selection only when it is out of the frame: inside, the listing
    // shows it already.
    if (!view.follow && view.hasSelection)
    {
        const int selected = line_of(view.selection);
        const float y = origin.y + (selected + 0.5f) * height / count;
        if (selected >= 0 && (y < fmin.y || y > fmax.y))
            mark(selected, ImVec4(1, 1, 1, 1), 2.0f);
    }
    mark(line_of(static_cast<u16>(pc)), ImVec4(1.0f, 0.85f, 0.2f, 1.0f), 2.0f);

    // Grab the frame and drag it, or click elsewhere to go there.
    const float mouseY = ImGui::GetIO().MousePos.y;
    if (ImGui::IsItemActivated())
    {
        const bool inFrame = mouseY >= fmin.y && mouseY <= fmax.y;
        view.stripGrab = inFrame ? mouseY - fmin.y : frameHeight * 0.5f;
        view.stripDragging = inFrame;
        if (!inFrame)
        {
            // A pixel row stands for several lines: go to the one it shows
            // for - the unknown one, snapping to it from a few pixels away.
            reveal_line(ruler_line(memory, static_cast<int>(mouseY - origin.y), rows, count),
                        lineHeight);
            if (!emu_is_execution_stopped())
                view.message = "Follow PC suspended: you moved the view from the navigator.";
        }
    }
    if (active && view.stripDragging)
    {
        // The frame's centre follows the mouse; the listing shows the lines
        // under it.
        const float wanted = mouseY - view.stripGrab - origin.y + frameHeight * 0.5f;
        const float scroll = wanted / height * total - view.listHeight * 0.5f;
        view.requestScrollY = std::max(0.0f, std::min(view.listScrollMaxY, scroll));
        if (view.follow)
        {
            view.follow = false;
            if (!emu_is_execution_stopped())
                view.message = "Follow PC suspended: you moved the view from the navigator.";
        }
    }
    if (!active)
        view.stripDragging = false;
    // The wheel over the ruler moves by pages.
    const float wheel = ImGui::GetIO().MouseWheel;
    if (hovered && wheel != 0.0f)
    {
        view.requestScrollY = std::max(0.0f, std::min(view.listScrollMaxY,
            view.listScrollY - wheel * view.listHeight * 0.8f));
        if (view.follow)
        {
            view.follow = false;
            if (!emu_is_execution_stopped())
                view.message = "Follow PC suspended: you scrolled the ruler.";
        }
    }
    if (hovered && !active)
    {
        // What is under the mouse, so a colour is read without going there.
        const int index = ruler_line(memory, static_cast<int>(mouseY - origin.y), rows, count);
        std::string what = navigator_describe(memory, index);
        ImGui::SetTooltip("%s\n\nDrag the frame, or click to go there.\n"
                          "Green code that ran, light green code you confirmed, blue data;\n"
                          "red unknown and orange unknown with a suggestion (code?) jut out.\n"
                          "Marks: yellow PC, white selection, red breakpoints.", what.c_str());
    }
}

// A listing line in miniature: its text as the minimap draws it, where it
// starts, and whether it is unknown bytes.
std::string minimap_text(Memory* memory, const Line& line, int& indent, bool& unknown)
{
    indent = 4;
    unknown = false;
    switch (line.type)
    {
        case Line_Label:
            indent = 0;
            return view.labels[line.label] + ":";
        case Line_Instruction:
        {
            indent = 0;
            Memory::stDisassembleRecord* record = memory->GetDisassembleRecord(line.address, false);
            return IsValidPointer(record) ? "        " + without_markers(record->name) : std::string();
        }
        case Line_Gap:
            unknown = true;
            return "... bytes not seen";
        case Line_GapBytes:
            unknown = true;
            return "db $00,$00,$00,$00,$00,$00,$00,$00";
        case Line_Source:
        {
            const SourceListing::Line& source = view.listing.GetLines()[line.source];
            if (source.type == SourceListing::Line_Label)
            {
                indent = 0;
                return source.text + ":";
            }
            if (source.type == SourceListing::Line_DataHeader)
                indent = 2;
            unknown = source.kind == RomMap::Kind_Unknown ||
                      source.kind == RomMap::Kind_CodeCandidate;
            return without_markers(source.text);
        }
        case Line_PreviewInstruction:
        case Line_PreviewLabel:
            return without_markers(view.previewTexts[line.label]);
        default:
            return std::string();
    }
}

// The code in miniature, VS Code's minimap: two pixels a line, a pixel a
// character, words in the listing's colours, faded. It shows the area
// round the view - scrolling with it, both ends meeting the listing's - and
// the visible lines under a light slider that is dragged; a click centres
// the listing there. It helps remember the shape of the code.
void draw_minimap(Memory* memory, int pc, float width, float height, float lineHeight)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##minimap", ImVec2(width, height));
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);
    const int count = static_cast<int>(view.lines.size());
    const float row = 2.0f, cell = 1.0f;
    const float total = count * row;
    // The minimap stays put and scrolls only as far as keeps the slider in
    // it: a click on it must leave what was clicked under the mouse (it
    // used to follow the listing's scroll and ran away with the spot).
    {
        const float sliderTop = view.listScrollY / lineHeight * row;
        const float sliderBottom = sliderTop + view.listHeight / lineHeight * row;
        if (sliderTop < view.miniScroll)
            view.miniScroll = sliderTop;
        else if (sliderBottom > view.miniScroll + height)
            view.miniScroll = sliderBottom - height;
        view.miniScroll = std::max(0.0f, std::min(std::max(0.0f, total - height), view.miniScroll));
    }
    const float scroll = view.miniScroll;
    view.miniFirst = static_cast<int>(scroll / row);
    view.miniRows = static_cast<int>(height / row);
    const int first = static_cast<int>(scroll / row);
    const int last = std::min(count, first + static_cast<int>(height / row) + 2);
    const float fade = 0.55f;
    auto faded = [&](ImVec4 c) { c.w = fade; return ImGui::GetColorU32(c); };
    const ImU32 unknownRule = ImGui::GetColorU32(ImVec4(0.92f, 0.30f, 0.26f, 0.9f));
    const ImU32 hintRule = ImGui::GetColorU32(kHintColor);
    const ImU32 keptRule = ImGui::GetColorU32(kKeptColor);

    for (int i = first; i < last; i++)
    {
        int indent;
        bool unknown;
        const std::string text = minimap_text(memory, view.lines[i], indent, unknown);
        if (text.empty())
            continue;
        const float y = origin.y + i * row - scroll;
        // Unknown bytes keep their colours; a thin red rule on the left
        // marks the zone without changing what the code looks like.
        // A code? suggestion: orange, as on the ruler.
        if (unknown)
        {
            const bool hint = view.lines[i].type == Line_Source &&
                              view.listing.GetLines()[view.lines[i].source].kind == RomMap::Kind_CodeCandidate;
            draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + 1.5f, y + row),
                                hint ? hintRule : unknownRule);
        }
        // Code you confirmed: light green, as on the ruler.
        else if (view.lines[i].type == Line_Source &&
                 view.listing.GetLines()[view.lines[i].source].type == SourceListing::Line_Instruction &&
                 view.listing.GetLines()[view.lines[i].source].chosen)
            draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + 1.5f, y + row), keptRule);
        if (view.lines[i].type == Line_Instruction || view.lines[i].type == Line_Source)
            if (view.lines[i].address == static_cast<u16>(pc) && is_instruction_line(view.lines[i]))
                draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + width, y + row),
                                    ImGui::GetColorU32(kPcBackground));
        // One rectangle a word; the first word is the mnemonic (or the
        // label, or the comment).
        const bool comment = text[0] == ';';
        const bool label = !text.empty() && text.back() == ':';
        int column = indent;
        bool firstWord = true;
        for (std::size_t k = 0; k < text.size();)
        {
            if (text[k] == ' ')
            {
                column++;
                k++;
                continue;
            }
            std::size_t end = k;
            while (end < text.size() && text[end] != ' ')
                end++;
            ImU32 color;
            if (comment)
                color = faded(kSrcComment);
            else if (label)
                color = faded(kSrcLabel);
            else if (firstWord)
                color = faded(kSrcMnemonic);
            else if (text[k] == '$' || std::isdigit(static_cast<unsigned char>(text[k])))
                color = faded(kSrcNumber);
            else if (text[k] == '_')
                color = faded(kSrcName);
            else
                color = faded(kSrcRegister);
            const float x = origin.x + 4.0f + column * cell;
            if (x > origin.x + width)
                break;
            draw->AddRectFilled(ImVec2(x, y), ImVec2(std::min(origin.x + width, x + (end - k) * cell), y + row - 0.5f), color);
            column += static_cast<int>(end - k);
            firstWord = false;
            k = end;
        }
    }

    // The slider over the lines the listing shows, as VS Code: it fades in
    // while the mouse is on the minimap, out when it leaves, and there is
    // none with nothing to frame.
    const float sliderTop = origin.y + view.listScrollY / lineHeight * row - scroll;
    const float sliderHeight = std::max(4.0f, view.listHeight / lineHeight * row);
    const ImVec2 smin(origin.x, sliderTop), smax(origin.x + width, sliderTop + sliderHeight);
    const float want = count > 0 && (hovered || active) ? (active ? 0.20f : 0.13f) : 0.0f;
    const float step = ImGui::GetIO().DeltaTime / 0.18f * 0.20f;
    view.miniSlider = want > view.miniSlider ? std::min(want, view.miniSlider + step)
                                             : std::max(want, view.miniSlider - step);
    if (view.miniSlider > 0.001f)
        draw->AddRectFilled(smin, smax, ImGui::GetColorU32(ImVec4(1, 1, 1, view.miniSlider)), 4.0f);

    // With the mouse on it, a discreet hint at an edge beyond which there is
    // more: a shade and a small arrow, fading in and out with the slider.
    const float more = view.miniSlider / 0.13f;
    if (more > 0.01f)
    {
        const float shade = std::min(1.0f, more);
        const float h = ImGui::GetTextLineHeight();
        const ImU32 dark = ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.55f * shade));
        const ImU32 clear = ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        const ImU32 arrow = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.75f * shade));
        const float cx = origin.x + width * 0.5f;
        if (scroll > 0.5f)
        {
            draw->AddRectFilledMultiColor(origin, ImVec2(origin.x + width, origin.y + h * 1.2f),
                                          dark, dark, clear, clear);
            draw->AddTriangleFilled(ImVec2(cx, origin.y + h * 0.15f),
                                    ImVec2(cx - h * 0.3f, origin.y + h * 0.5f),
                                    ImVec2(cx + h * 0.3f, origin.y + h * 0.5f), arrow);
        }
        if (scroll + height < total - 0.5f)
        {
            const float bottom = origin.y + height;
            draw->AddRectFilledMultiColor(ImVec2(origin.x, bottom - h * 1.2f),
                                          ImVec2(origin.x + width, bottom), clear, clear, dark, dark);
            draw->AddTriangleFilled(ImVec2(cx, bottom - h * 0.15f),
                                    ImVec2(cx - h * 0.3f, bottom - h * 0.5f),
                                    ImVec2(cx + h * 0.3f, bottom - h * 0.5f), arrow);
        }
    }
    draw->PopClipRect();

    // The wheel (two fingers on a trackpad) over the minimap scrolls the
    // listing in bigger steps - leafing through the miniature - and the
    // minimap follows.
    const float wheel = ImGui::GetIO().MouseWheel;
    if (hovered && wheel != 0.0f && count > 0)
    {
        view.requestScrollY = std::max(0.0f, std::min(view.listScrollMaxY,
            view.listScrollY - wheel * lineHeight * 12.0f));
        if (view.follow)
        {
            view.follow = false;
            if (!emu_is_execution_stopped())
                view.message = "Follow PC suspended: you scrolled the minimap.";
        }
    }

    const float mouseY = ImGui::GetIO().MousePos.y;
    if (ImGui::IsItemActivated())
    {
        const bool inSlider = mouseY >= smin.y && mouseY <= smax.y;
        view.miniGrab = inSlider ? mouseY - smin.y : sliderHeight * 0.5f;
        view.miniDragging = true;
        if (!inSlider)
        {
            // A click goes to the line under it and selects it.
            reveal_line(static_cast<int>((mouseY - origin.y + scroll) / row), lineHeight);
            view.miniDragging = false;
        }
        if (view.follow)
        {
            view.follow = false;
            if (!emu_is_execution_stopped())
                view.message = "Follow PC suspended: you moved the view from the minimap.";
        }
    }
    if (active && view.miniDragging)
    {
        // The slider travels the minimap's height as the listing its scroll.
        const float travel = std::max(1.0f, std::min(height, total) - sliderHeight);
        const float top = mouseY - view.miniGrab - origin.y;
        view.requestScrollY = std::max(0.0f, std::min(view.listScrollMaxY,
            top / travel * view.listScrollMaxY));
    }
    if (!active)
        view.miniDragging = false;
}

// The horizontal scrollbar under the listing, drawn by hand since the
// listing hides its own scrollbars; its room is always there, so nothing
// moves when it appears.
void draw_horizontal_scrollbar(float width, float height)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));
    if (view.listScrollMaxX <= 0.0f)
        return;
    const ImRect bb(origin, ImVec2(origin.x + width, origin.y + height));
    ImS64 scroll = static_cast<ImS64>(view.listScrollX);
    const ImS64 avail = static_cast<ImS64>(view.listWidth);
    const ImS64 contents = static_cast<ImS64>(view.listWidth + view.listScrollMaxX);
    if (ImGui::ScrollbarEx(bb, ImGui::GetID("##listing_hscroll"), ImGuiAxis_X, &scroll, avail, contents))
        view.requestScrollX = static_cast<float>(scroll);
}
}

// ---- To Check: every stretch not sure yet, to go and look at -------------

namespace
{
// A stretch of bytes not sure yet: UNKNOWN or CODE?, not kept by the user.
struct CheckRun
{
    u32 start;          // ROM offsets, end exclusive
    u32 end;
    bool guess;         // CODE? (a suggestion), else UNKNOWN
    std::string hint;
    u8 reference;       // the reference disassembly: 1 data, 2 code, 3 both, 0 none
};

struct CheckList
{
    const RomMap* key = nullptr;        // the listing's map the runs are for
    std::vector<CheckRun> runs;         // by address
    u32 bytes = 0;
    // Saverio: a second opinion from the disassembly in reference/ (an
    // Emulicious export: every data block is "; Data from X to Y", so the
    // rest is what it took for code). Per ROM offset: 0 none, 1 data, 2 code.
    std::vector<u8> reference;
    std::string referenceFile;
    std::string referenceFor;           // project dir | base | size it was read for
    bool sortBySize = false;
    bool referenceFirst = true;
};

CheckList check;

// What the user kept, as a mask over the ROM.
std::vector<bool> kept_mask(std::size_t size)
{
    std::vector<bool> kept(size, false);
    for (const SourceListing::CodeRange& range : view.choices)
        for (u32 o = range.start; o < range.end && o < size; o++)
            kept[o] = true;
    for (u32 at : view.keptWords)
        for (u32 o = at; o < at + 2 && o < size; o++)
            kept[o] = true;
    return kept;
}

void load_reference(void)
{
    const std::string dir = rom_map_project_dir();
    const std::string key = dir + "|" + std::to_string(view.sourceBase) + "|" + std::to_string(view.sourceSize);
    if (key == check.referenceFor)
        return;
    check.referenceFor = key;
    check.reference.clear();
    check.referenceFile.clear();
    check.key = nullptr;
    if (dir.empty() || view.sourceSize == 0)
        return;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(dir + "reference/", error))
    {
        if (entry.path().extension() != ".asm")
            continue;
        std::ifstream in(entry.path());
        std::string line;
        if (!std::getline(in, line) || line.find("Emulicious") == std::string::npos)
            continue;
        std::vector<u8> opinion(view.sourceSize, 2);
        while (std::getline(in, line))
        {
            unsigned from = 0, to = 0;
            if ((line.rfind("; Data from ", 0) != 0 && line.rfind("; Jump Table from ", 0) != 0) ||
                std::sscanf(line.c_str() + line.find("from ") + 5, "%x to %x", &from, &to) != 2)
                continue;
            for (unsigned a = from; a <= to; a++)
                if (a >= view.sourceBase && a - view.sourceBase < view.sourceSize)
                    opinion[a - view.sourceBase] = 1;
        }
        check.reference = std::move(opinion);
        check.referenceFile = entry.path().filename().string();
        return;
    }
}

void update_check(void)
{
    load_reference();
    if (!view.sourceActive || !view.sourceMap || check.key == view.sourceMap)
        return;
    check.key = view.sourceMap;
    check.runs.clear();
    check.bytes = 0;
    const std::vector<u8>& bytes = view.sourceMap->GetBytes();
    const std::vector<bool> kept = kept_mask(bytes.size());
    auto unsure = [&](u32 o, bool& guess)
    {
        const u8 kind = bytes[o] & RomMap::KindMask;
        guess = kind == RomMap::Kind_CodeCandidate;
        return !kept[o] && (kind == RomMap::Kind_Unknown || guess);
    };
    u32 o = 0;
    while (o < bytes.size())
    {
        bool guess = false;
        if (!unsure(o, guess))
        {
            o++;
            continue;
        }
        CheckRun run = {o, o + 1, guess, "", 0};
        bool g = false;
        while (run.end < bytes.size() && unsure(run.end, g) && g == guess)
            run.end++;
        const RomMap::DataHint* data = view.sourceMap->DataHintAt(run.start);
        run.hint = data ? data->text
                 : guess ? view.listing.CandidateHint(run.start, run.end)
                         : view.listing.PointerHint(run.start, run.end);
        if (!check.reference.empty())
            for (u32 i = run.start; i < run.end && i < check.reference.size(); i++)
                run.reference |= check.reference[i];
        check.bytes += run.end - run.start;
        check.runs.push_back(run);
        o = run.end;
    }
}

const char* reference_text(u8 reference)
{
    switch (reference)
    {
        case 1: return "data";
        case 2: return "code";
        case 3: return "code+data";
        default: return "";
    }
}

// F8 / Shift+F8: the next or previous stretch to check from where you are.
void check_step(int pc, int direction)
{
    update_check();
    if (check.runs.empty())
    {
        view.message = "Nothing left to check.";
        return;
    }
    const u32 at = static_cast<u32>(focus_address(pc) - view.sourceBase);
    const CheckRun* next = nullptr;
    if (direction > 0)
    {
        for (const CheckRun& run : check.runs)
            if (run.start > at)
            {
                next = &run;
                break;
            }
        if (!next)
            next = &check.runs.front();
    }
    else
    {
        for (auto it = check.runs.rbegin(); it != check.runs.rend(); ++it)
            if (it->end <= at)
            {
                next = &*it;
                break;
            }
        if (!next)
            next = &check.runs.back();
    }
    go_to(static_cast<u16>(view.sourceBase + next->start), pc);
}
}

void gui_debug_to_check_open(void)
{
    config_debug.show_to_check = true;
    ImGui::SetWindowFocus("To Check###to_check");
}

void gui_debug_to_check_window(void)
{
    gui_set_next_window_size_em(34.4f, 23.3f);
    if (!ImGui::Begin("To Check###to_check", &config_debug.show_to_check))
    {
        ImGui::End();
        return;
    }
    if (!view.sourceActive)
    {
        ImGui::TextColored(gray, "Open the Disassembler (new) on a project's ROM.");
        ImGui::End();
        return;
    }
    update_check();
    ImGui::Text("%u bytes not sure, in %zu places", check.bytes, check.runs.size());
    ImGui::SameLine();
    ImGui::TextColored(gray, "- unknown, or code? not confirmed. F8 / Shift+F8 in the Disassembler.");
    if (ImGui::RadioButton("By address", !check.sortBySize))
        check.sortBySize = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Longest first", check.sortBySize))
        check.sortBySize = true;
    if (!check.referenceFile.empty())
    {
        ImGui::SameLine();
        ImGui::Checkbox("Reference says code first", &check.referenceFirst);
        ImGui::SetItemTooltip("A second opinion, never proof: %s in reference/\n"
                              "(what it did not mark as data it took for code).",
                              check.referenceFile.c_str());
    }

    std::vector<const CheckRun*> order;
    for (const CheckRun& run : check.runs)
        order.push_back(&run);
    const bool byReference = check.referenceFirst && !check.referenceFile.empty();
    std::stable_sort(order.begin(), order.end(), [&](const CheckRun* a, const CheckRun* b)
    {
        if (byReference && ((a->reference & 2) != 0) != ((b->reference & 2) != 0))
            return (a->reference & 2) != 0;
        if (check.sortBySize)
            return a->end - a->start > b->end - b->start;
        return a->start < b->start;
    });

    const int columns = check.referenceFile.empty() ? 4 : 5;
    if (ImGui::BeginTable("##to_check", columns, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                                 ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000-0000").x);
        ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Bytes").x);
        ImGui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("unknown").x);
        if (columns == 5)
            ImGui::TableSetupColumn("Reference", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize("code+data").x);
        ImGui::TableSetupColumn("Hint");
        ImGui::TableHeadersRow();
        const u16 focus = focus_address(view.currentPc);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(order.size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
            {
                const CheckRun& run = *order[i];
                const u16 first = static_cast<u16>(view.sourceBase + run.start);
                const u16 last = static_cast<u16>(view.sourceBase + run.end - 1);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char where[32];
                if (first == last)
                    std::snprintf(where, sizeof(where), "%04X##%d", first, i);
                else
                    std::snprintf(where, sizeof(where), "%04X-%04X##%d", first, last, i);
                const bool here = focus >= first && focus <= last;
                if (ImGui::Selectable(where, here, ImGuiSelectableFlags_SpanAllColumns))
                    gui_debug_disassembler_view_goto(first);
                ImGui::TableNextColumn();
                ImGui::Text("%u", run.end - run.start);
                ImGui::TableNextColumn();
                ImGui::TextColored(run.guess ? kHintColor : ImVec4(0.92f, 0.30f, 0.26f, 1.0f), "%s",
                                   run.guess ? "code?" : "unknown");
                if (columns == 5)
                {
                    ImGui::TableNextColumn();
                    ImGui::TextColored((run.reference & 2) ? kKeptColor : gray, "%s",
                                       reference_text(run.reference));
                }
                ImGui::TableNextColumn();
                ImGui::TextColored(gray, "%s", run.hint.c_str());
            }
        ImGui::EndTable();
    }
    ImGui::End();
}

void gui_debug_disassembler_view_reload(void)
{
    view.diskMap.reset();
    view.jumpPath.clear();      // read again at the next update
    view.sourceDirty = true;
}

void gui_debug_disassembler_view_goto(u16 address)
{
    config_debug.show_disassembler_new = true;
    ImGui::SetWindowFocus("Disassembler (new)###disassembler_new");
    go_to(address, view.currentPc);
}

void gui_debug_disassembler_view_window(void)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::SetNextWindowPos(ImVec2(570, 31), ImGuiCond_FirstUseEver);
    gui_set_next_window_size_em(24.4f, 35.6f);

    ImGui::Begin("Disassembler (new)###disassembler_new",
                 &config_debug.show_disassembler_new, ImGuiWindowFlags_MenuBar);

    GearSF7000Core* core = emu_get_core();
    Memory* memory = core->GetMemory();
    const int pc = core->GetCpuStateAccess()->GetCpuStateSnapshot().pc;
    const bool stopped = emu_is_execution_stopped();
    std::vector<Memory::stDisassembleRecord*>* breakpoints = memory->GetBreakpointsCPU();

    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("View"))
        {
            bool follow = view.follow;
            if (ImGui::MenuItem("Follow PC", NULL, &follow))
            {
                if (follow)
                    resume_follow();
                else
                    view.follow = false;
            }
            ImGui::Separator();
            ImGui::MenuItem("Addresses", NULL, &view.showAddresses);
            ImGui::MenuItem("Minimap", NULL, &view.showMinimap);
            ImGui::MenuItem("Bytes", NULL, &view.showBytes);
            ImGui::MenuItem("Symbols", NULL, &view.showSymbols);
            ImGui::MenuItem("Segments", NULL, &view.showSegment);
            ImGui::MenuItem("Bank", NULL, &view.showBank);
            ImGui::Separator();
            if (ImGui::MenuItem("Show All Gaps"))
                set_gap_open(0x0000, 0xFFFF, true);
            if (ImGui::MenuItem("Hide All Gaps"))
                set_gap_open(0x0000, 0xFFFF, false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Go"))
        {
            if (ImGui::MenuItem("Back"))
                history_move(-1);
            if (ImGui::MenuItem("Forward"))
                history_move(1);
            if (ImGui::MenuItem("Go To PC"))
                resume_follow();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Run"))
        {
            if (ImGui::MenuItem("Continue")) emu_debug_continue();
            if (ImGui::MenuItem("Pause")) emu_pause();
            ImGui::Separator();
            if (ImGui::MenuItem("Step Into")) emu_debug_step_into();
            if (ImGui::MenuItem("Step Over")) emu_debug_step_over();
            if (ImGui::MenuItem("Step Line")) emu_debug_step_line();
            if (ImGui::MenuItem("Step Frame")) emu_debug_next_frame();
            if (ImGui::MenuItem("Run To Selection", NULL, false, view.hasSelection))
                run_to_selection();
            ImGui::Separator();
            if (ImGui::MenuItem("Reset")) gui_shortcut(gui_ShortcutReset);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // Command bar.
    ImGui::PushFont(gui_material_icons_font, 20.0f);
    if (toolbar_button(ICON_MD_PLAY_ARROW, "continue", "Start / Continue")) emu_debug_continue();
    if (toolbar_button(ICON_MD_PAUSE, "pause", "Pause")) emu_pause();
    if (toolbar_button(ICON_MD_REDO, "step_over", "Step Over")) emu_debug_step_over();
    if (toolbar_button(ICON_MD_ARROW_DROP_DOWN_CIRCLE, "step_into", "Step Into")) emu_debug_step_into();
    if (toolbar_button(ICON_MD_VERTICAL_ALIGN_BOTTOM, "step_line", "Step Line")) emu_debug_step_line();
    if (toolbar_button(ICON_MD_INPUT, "step_frame", "Step Frame")) emu_debug_next_frame();
    if (toolbar_button(ICON_MD_KEYBOARD_TAB, "run_to_selection", "Run To Selection",
                       view.hasSelection))
        run_to_selection();
    if (toolbar_button(ICON_MD_REPLAY, "reset", "Reset")) gui_shortcut(gui_ShortcutReset);
    const bool following = view.follow;
    if (following)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.48f, 0.90f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.56f, 1.00f, 1.0f));
    }
    const bool toggleFollow = toolbar_button(ICON_MD_GPS_FIXED, "follow_pc",
        following ? "Following the PC - click to stop" : "Follow the PC");
    if (following)
        ImGui::PopStyleColor(2);
    ImGui::PopFont();
    if (toggleFollow)
    {
        if (following)
            view.follow = false;
        else
            resume_follow();
    }
    run_state_label(stopped);

    // Go To.
    ImGui::PushItemWidth(ImGui::CalcTextSize("WWWWWWWWWWWWWWWW").x);
    bool goRequested = ImGui::InputTextWithHint("##goto", "address or symbol", view.gotoText,
        IM_ARRAYSIZE(view.gotoText),
        ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    goRequested |= ImGui::Button("Go");
    if (goRequested)
        go_to_text(memory, pc);
    ImGui::SameLine();
    ImGui::BeginDisabled(view.historyPos <= 0);
    if (ImGui::ArrowButton("##back", ImGuiDir_Left))
        history_move(-1);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Back");
    ImGui::SameLine();
    ImGui::BeginDisabled(view.historyPos < 0 ||
                         view.historyPos >= static_cast<int>(view.history.size()) - 1);
    if (ImGui::ArrowButton("##forward", ImGuiDir_Right))
        history_move(1);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Forward");
    ImGui::SameLine();
    ImGui::Checkbox("Bytes", &view.showBytes);
    ImGui::SetItemTooltip("Show the bytes of each instruction");
    // Always there, so the bar does not move; it only applies to the source.
    ImGui::SameLine();
    ImGui::BeginDisabled(!view.sourceActive);
    ImGui::Checkbox("Addr", &view.showAddresses);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Show the address of each source line (with a project)");

    // Find: a row of its own, always there. Cmd/Ctrl+F puts the cursor in it.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F))
        view.focusFind = true;
    if (view.focusFind)
    {
        ImGui::SetKeyboardFocusHere();
        view.focusFind = false;
    }
    ImGui::PushItemWidth(ImGui::CalcTextSize("WWWWWWWWWWWWWWWW").x);
    const bool findEnter = ImGui::InputTextWithHint("##find", "find (Cmd/Ctrl+F)", view.findText,
        IM_ARRAYSIZE(view.findText), ImGuiInputTextFlags_EnterReturnsTrue);
    const bool findActive = ImGui::IsItemActive();
    ImGui::PopItemWidth();
    if (findEnter)
    {
        find_step(memory, pc, ImGui::GetIO().KeyShift ? -1 : 1);
        view.focusFind = true;      // stay in the field for the next Enter
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##find_prev", ImGuiDir_Up))
        find_step(memory, pc, -1);
    ImGui::SetItemTooltip("Previous (Shift+Enter)");
    ImGui::SameLine();
    if (ImGui::ArrowButton("##find_next", ImGuiDir_Down))
        find_step(memory, pc, 1);
    ImGui::SetItemTooltip("Next (Enter)");
    ImGui::SameLine();
    if (view.findText[0] != 0 && !findActive)
        rebuild_find(memory);
    char findCount[24];
    if (view.findText[0] == 0)
        std::snprintf(findCount, sizeof(findCount), " ");
    else if (view.findIndex >= 0)
        std::snprintf(findCount, sizeof(findCount), "%d/%d", view.findIndex + 1,
                      static_cast<int>(view.findHits.size()));
    else
        std::snprintf(findCount, sizeof(findCount), "%d", static_cast<int>(view.findHits.size()));
    ImGui::TextColored(gray, "%-9s", findCount);
    ImGui::SetItemTooltip("Folded parts of blocks are not searched.");

    // Deep Search opens its panel; fixed width, the count of the jumps it
    // has not read yet inside.
    ImGui::SameLine();
    char deepLabel[48];
    if (view.deepPending > 0)
        std::snprintf(deepLabel, sizeof(deepLabel), "Open Deep Search \xc2\xb7 %d###deep", view.deepPending);
    else
        std::snprintf(deepLabel, sizeof(deepLabel), "Open Deep Search###deep");
    if (ImGui::Button(deepLabel, ImVec2(ImGui::CalcTextSize("Open Deep Search \xc2\xb7 000").x +
                                       ImGui::GetStyle().FramePadding.x * 2.0f, 0.0f)))
        gui_debug_deep_search_open();
    ImGui::SetItemTooltip("The heavy analysis of what the collection gathered: jump tables,\n"
                          "then the code reached from the known code, round after round.\n"
                          "The number: executed jumps through a table not read yet.");

    // The real count of what is not sure yet (Saverio): UNKNOWN and CODE? -
    // neither ran, a CODE? block reads as bytes like any other - less what
    // you kept as code or as pointers there. It goes down when you confirm
    // a block by hand; the Live Disassembler counts the map only.
    if (view.sourceActive && view.sourceMap)
    {
        const std::vector<u8>& bytes = view.sourceMap->GetBytes();
        std::vector<bool> kept(bytes.size(), false);
        for (const SourceListing::CodeRange& range : view.choices)
            for (u32 o = range.start; o < range.end && o < bytes.size(); o++)
                kept[o] = true;
        for (u32 at : view.keptWords)
            for (u32 o = at; o < at + 2 && o < bytes.size(); o++)
                kept[o] = true;
        u32 keptUnknown = 0, keptGuess = 0;
        for (u32 o = 0; o < bytes.size(); o++)
        {
            if (!kept[o])
                continue;
            const u8 kind = bytes[o] & RomMap::KindMask;
            keptUnknown += kind == RomMap::Kind_Unknown;
            keptGuess += kind == RomMap::Kind_CodeCandidate;
        }
        const u32 mapUnknown = view.sourceMap->GetCount(RomMap::Kind_Unknown);
        const u32 mapGuess = view.sourceMap->GetCount(RomMap::Kind_CodeCandidate);
        const u32 map = mapUnknown + mapGuess, keptAll = keptUnknown + keptGuess;
        ImGui::SameLine();
        ImGui::TextColored(white, "Unknown: %u", map - keptAll);
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextColored(gray, "(%u map - %u kept)", map, keptAll);
        ImGui::SetItemTooltip("Bytes not sure yet: neither run nor kept by you.\n"
                              "UNKNOWN %u - %u kept = %u\n"
                              "CODE?   %u - %u kept = %u  (guessed, never ran)\n"
                              "The Live Disassembler counts the map's UNKNOWN only.",
                              mapUnknown, keptUnknown, mapUnknown - keptUnknown,
                              mapGuess, keptGuess, mapGuess - keptGuess);
        ImGui::SameLine();
        if (ImGui::SmallButton("To Check"))
            gui_debug_to_check_open();
        ImGui::SetItemTooltip("Every stretch not sure yet, in a list (F8 / Shift+F8: next / previous).");
    }

    const auto symbols = view.showSymbols ? collect_symbols()
                                          : std::unordered_map<u32, std::string>();
    draw_breakpoints_panel(memory, pc, symbols);

    ImGui::Separator();

    ImGui::PushFont(gui_default_font, gui_get_default_font_size());
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    // The status bar below the listing is always there.
    const float footer = 2.0f * lineHeight + ImGui::GetStyle().ItemSpacing.y;

    view.currentPc = pc;
    if (update_source(memory))
        view.forceRebuild = true;
    if (needs_rebuild(pc))
        rebuild(memory, pc);

    // The ruler: the whole ROM; the minimap, optional, beside it.
    const float navigatorWidth = ImGui::GetTextLineHeight() * 1.6f;
    const float minimapWidth = view.showMinimap ? ImGui::GetTextLineHeight() * 6.0f : 0.0f;
    const float hbarHeight = ImGui::GetStyle().ScrollbarSize;
    const float listingHeight = ImGui::GetContentRegionAvail().y - footer - hbarHeight -
                                ImGui::GetStyle().ItemSpacing.y;
    const float listingWidth = ImGui::GetContentRegionAvail().x - navigatorWidth - minimapWidth -
                               ImGui::GetStyle().ItemSpacing.x * (view.showMinimap ? 2.0f : 1.0f);
    if (ImGui::BeginChild("##listing", ImVec2(listingWidth, listingHeight), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_HorizontalScrollbar))
    {
        // Scroll asked by the navigator or the horizontal bar last frame.
        if (view.requestScrollY >= 0.0f)
        {
            ImGui::SetScrollY(view.requestScrollY);
            view.requestScrollY = -1.0f;
            view.scrolledByUs = true;
        }
        if (view.requestScrollX >= 0.0f)
        {
            ImGui::SetScrollX(view.requestScrollX);
            view.requestScrollX = -1.0f;
        }
        // The wheel over the listing is the user moving away: while
        // following, that suspends it. (A scroll the listing merely clamped,
        // as when it is empty, is not; the minimap and the ruler suspend it
        // themselves.)
        const ImGuiIO& io = ImGui::GetIO();
        if (view.follow && !view.lines.empty() && ImGui::IsWindowHovered() &&
            (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f))
        {
            view.follow = false;
            if (!stopped)
                view.message = "Follow PC suspended: you scrolled away. The target button resumes it.";
        }
        view.scrolledByUs = false;

        if (view.scrollRequested)
        {
            view.scrollRequested = false;
            const int index = line_of(view.scrollTarget);
            if (index >= 0)
            {
                ImGui::SetScrollY(std::max(0.0f, index * lineHeight -
                                                 ImGui::GetWindowHeight() * 0.30f));
                view.scrolledByUs = true;
            }
        }
        else if (view.follow)
        {
            const int index = line_of(static_cast<u16>(pc));
            if (index >= 0 && is_instruction_line(view.lines[index]) &&
                view.lines[index].address == pc)
                keep_pc_in_band(index, lineHeight);
        }

        // Hovering a row barely shows; the selection is an outline and the
        // PC row is red (row_selectable).
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        // The jump to draw, found before the rows; placed after them, from
        // where the first drawn row landed.
        int jumpFrom = -1, jumpTo = -1;
        {
            const u16 focus = focus_address(pc);
            const int index = line_of(focus);
            u16 target = 0;
            if (index >= 0 && is_instruction_line(view.lines[index]) &&
                view.lines[index].address == focus && jump_of_line(memory, index, target))
            {
                const int to = line_of(target);
                if (to >= 0 && is_instruction_line(view.lines[to]) &&
                    view.lines[to].address == target)
                {
                    jumpFrom = index;
                    jumpTo = to;
                }
            }
        }
        int firstIndex = -1;
        float firstY = 0.0f, rowLeft = 0.0f;

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(view.lines.size()), lineHeight);
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
            {
                if (firstIndex < 0)
                {
                    firstIndex = i;
                    firstY = ImGui::GetCursorScreenPos().y;
                    rowLeft = ImGui::GetCursorScreenPos().x;
                }
                const Line& line = view.lines[i];
                if (line.type == Line_Label)
                {
                    ImGui::TextColored(green, "%s:", view.labels[line.label].c_str());
                    continue;
                }
                ImGui::PushID(i);
                if (line.type == Line_Blank)
                    ImGui::TextUnformatted("");
                else if (line.type == Line_More)
                    draw_more_line(line);
                else if (line.type == Line_KeptHead)
                    draw_kept_head(line);
                else if (line.type == Line_KeptWordHead)
                    draw_kept_word_head(line);
                else if (line.type == Line_PreviewHead || line.type == Line_PreviewInstruction ||
                         line.type == Line_PreviewStop || line.type == Line_PreviewLabel)
                    draw_preview_line(line, !stopped);

                else if (line.type == Line_Source)
                    draw_source_line(memory, line, pc, breakpoints, !stopped);
                else if (line.type == Line_Gap)
                    draw_gap_line(memory, line);
                else if (line.type == Line_GapBytes)
                    draw_gap_bytes_line(memory, line, !stopped);
                else
                    draw_instruction_line(memory, line, pc, breakpoints, symbols, !stopped);
                ImGui::PopID();
            }
        }
        ImGui::PopStyleColor(2);
        if (jumpFrom >= 0 && firstIndex >= 0)
            draw_jump_line(jumpFrom, jumpTo, firstIndex, firstY, rowLeft, lineHeight);
        if (view.linkClicked)
        {
            view.linkClicked = false;
            go_to_from(view.linkTarget, view.linkFrom);
        }
        if (!ImGui::IsMouseDown(0))
            view.suppressRowClick = false;

        // The data byte's menu, opened from its row but drawn here, outside
        // the row's ID scope.
        if (view.openByteMenu)
        {
            ImGui::OpenPopup("##byte_menu");
            view.openByteMenu = false;
        }
        if (ImGui::BeginPopup("##byte_menu"))
        {
            char item[48];
            std::snprintf(item, sizeof(item), "Try as code from %X", view.byteMenuAddress);
            if (ImGui::MenuItem(item))
                start_preview(view.byteMenuAddress);
            if (ImGui::MenuItem("Close preview", "Esc", false, view.previewActive))
                close_preview();
            // Two bytes as a pointer: the user's word, like keeping code.
            const u32 at = static_cast<u32>(view.byteMenuAddress - view.sourceBase);
            if (view.sourceActive && at + 1 < view.sourceSize && view.heldRom)
            {
                ImGui::Separator();
                if (is_kept_word(at))
                {
                    if (ImGui::MenuItem("Forget my pointer"))
                        forget_word(at);
                }
                else
                {
                    const std::vector<u8>& rom = *view.heldRom;
                    const u16 value = static_cast<u16>(rom[at] | (rom[at + 1] << 8));
                    const bool ran = ((source_map_byte(at) & RomMap::KindMask) == RomMap::Kind_Code) ||
                                     ((source_map_byte(at + 1) & RomMap::KindMask) == RomMap::Kind_Code);
                    std::snprintf(item, sizeof(item), "Keep %X as a pointer (dw $%04X)",
                                  view.byteMenuAddress, value);
                    if (ImGui::MenuItem(item, NULL, false, !ran))
                        keep_as_pointer(at);
                    if (ran)
                        ImGui::SetItemTooltip("It ran: the live wins.");
                }
            }
            ImGui::EndPopup();
        }
        if (view.openCodeMenu)
        {
            ImGui::OpenPopup("##code_menu");
            view.openCodeMenu = false;
        }
        if (ImGui::BeginPopup("##code_menu"))
        {
            if (ImGui::MenuItem("Forget my choice"))
                forget_choice(static_cast<u32>(view.codeMenuAddress - view.sourceBase));
            ImGui::EndPopup();
        }
        // Keys, while the listing has the focus and no field takes typing:
        // Enter follows the selected (or PC's) jump, Esc closes the preview
        // or goes back.
        const bool keys = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                          !ImGui::GetIO().WantTextInput;
        if (keys && ImGui::IsKeyPressed(ImGuiKey_F8))
            check_step(pc, ImGui::GetIO().KeyShift ? -1 : 1);
        if (keys && ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            if (view.previewActive)
                close_preview();
            else
                history_move(-1);
        }
        if (keys && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
        {
            const u16 focus = focus_address(pc);
            const int index = line_of(focus);
            u16 target = 0;
            if (index >= 0 && view.lines[index].address == focus &&
                jump_of_line(memory, index, target))
            {
                select(focus, !stopped);
                go_to(target, pc);
            }
        }
        if (view.sourceActive && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z))
            undo_choice();

        view.lastScrollY = view.scrolledByUs ? -1.0f : ImGui::GetScrollY();
        view.listScrollY = ImGui::GetScrollY();
        view.listScrollMaxY = ImGui::GetScrollMaxY();
        view.listScrollX = ImGui::GetScrollX();
        view.listScrollMaxX = ImGui::GetScrollMaxX();
        view.listHeight = ImGui::GetWindowHeight();
        view.listWidth = ImGui::GetWindowWidth();
    }
    ImGui::EndChild();
    if (view.showMinimap)
    {
        ImGui::SameLine();
        draw_minimap(memory, pc, minimapWidth, listingHeight, lineHeight);
    }
    ImGui::SameLine();
    draw_navigator(memory, pc, navigatorWidth, listingHeight, lineHeight);
    draw_horizontal_scrollbar(listingWidth, hbarHeight);

    draw_status_bar(memory, pc, lineHeight);
    ImGui::PopFont();

    ImGui::End();
    ImGui::PopStyleVar();
}
