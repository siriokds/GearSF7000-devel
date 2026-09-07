/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026 Saverio Russo
 */

#include "gui_debug_rom_inspector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/colors.h"
#include "nfd/nfd.h"
#include "config.h"
#include "emu.h"
#include "gui.h"
#include "renderer.h"
#include "../../src/definitions.h"
#include "../../src/Video.h"

namespace
{

// Max scratch buffer size - large enough for the biggest ASC16L multicart
// image this project has seen (see DOCS/ROM_INSPECTOR_PLAN.md point 4).
constexpr size_t kMaxRomSize = 4 * 1024 * 1024;
// ROM_INSPECTOR_TEXTURE_SIZE (emu.h) is 512px = 64 tiles - the hard cap on
// both tiles-per-row and visible tile-rows, since the backing texture is a
// fixed square and never gets recreated.
constexpr int kMaxTilesPerAxis = ROM_INSPECTOR_TEXTURE_SIZE / 8;
constexpr float kScale = 2.0f;
constexpr float kTilePx = 8.0f * kScale;

struct ScopedAudioPause
{
    ScopedAudioPause() { emu_audio_suspend_for_file_dialog(); }
    ~ScopedAudioPause() { emu_audio_resume_after_file_dialog(); }
    ScopedAudioPause(const ScopedAudioPause&) = delete;
    ScopedAudioPause& operator=(const ScopedAudioPause&) = delete;
};

std::vector<u8> g_rom_data;
char g_file_path[1024] = "";
// Fine, sub-tile alignment bias (0-7) - graphics in an arbitrary ROM file
// aren't guaranteed to start on an 8-byte tile boundary the way real VRAM
// pattern data always does. Coarse navigation through the file is the
// scroll position below instead.
int g_tile_offset = 0;
int g_tiles_per_row = 32;
int g_colortable_mode = GuiDebugRomInspectorColorTableNone;
int g_colortable_offset = 0;
// Authoritative scroll position (whole tile-rows from the start of the
// file) - the GUI syncs this from ImGui::GetScrollY() every frame the
// window is open, and gui_debug_rom_inspector_set_scroll_row() (MCP) sets
// it directly. g_scroll_dirty means the GUI scrollbar needs to snap to
// match a value that was just set programmatically rather than by dragging.
int g_scroll_top_row = 0;
bool g_scroll_dirty = false;

// Mode 2 color table auto-search: a scored, non-overlapping candidate list
// the user picks from, rather than a single guess. See
// search_colortable_candidates() for the heuristic.
struct ColorTableCandidate
{
    int offset;
    float score;
};
std::vector<ColorTableCandidate> g_colortable_candidates;
bool g_colortable_search_ran = false;
// One screen-third: the real TMS9918 Mode 2 color table size, and a large
// enough sample to make the two signals below statistically meaningful.
constexpr int kColorTableSearchWindowBytes = 6144;
constexpr int kColorTableMaxCandidates = 20;

int compute_total_rows()
{
    int total_tiles = g_rom_data.empty() ? 0 : (int)((g_rom_data.size() + 7) / 8);
    return (g_tiles_per_row > 0) ? (total_tiles + g_tiles_per_row - 1) / g_tiles_per_row : 0;
}

int clamp_top_row(int row)
{
    int total_rows = compute_total_rows();
    int max_top_row = total_rows > kMaxTilesPerAxis ? total_rows - kMaxTilesPerAxis : 0;
    if (row < 0)
        row = 0;
    if (row > max_top_row)
        row = max_top_row;
    return row;
}

bool load_rom_file(const char* path)
{
    FILE* file = fopen(path, "rb");
    if (file == nullptr)
    {
        Log("ROM Inspector: could not open %s", path);
        return false;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size <= 0)
    {
        fclose(file);
        return false;
    }

    size_t read_size = (size_t)file_size;
    if (read_size > kMaxRomSize)
        read_size = kMaxRomSize;

    g_rom_data.resize(read_size);
    fread(g_rom_data.data(), 1, read_size, file);
    fclose(file);

    strncpy(g_file_path, path, sizeof(g_file_path) - 1);
    g_file_path[sizeof(g_file_path) - 1] = 0;

    g_tile_offset = 0;
    g_scroll_top_row = 0;
    g_scroll_dirty = true;
    g_colortable_candidates.clear();
    g_colortable_search_ran = false;
    return true;
}

void open_rom_file_dialog()
{
    ScopedAudioPause audioPause;
    nfdchar_t* out_path = nullptr;
    // No extension filter - source files can be ROMs from any of the
    // TMS9918-based systems this tool targets (ColecoVision/MSX/
    // CreatiVision/SC-3000), with whatever extension their own tooling uses.
    nfdresult_t result = NFD_OpenDialog(&out_path, nullptr, 0, nullptr);
    if (result != NFD_OKAY)
    {
        if (result != NFD_CANCEL)
            Log("ROM Inspector Open Error: %s", NFD_GetError());
        return;
    }

    load_rom_file(out_path);
    NFD_FreePath(out_path);
}

// Reads one byte at a possibly-out-of-range offset - the user can type any
// pattern/color table address by hand, including past the end of a small
// loaded file. Returns 0 (rendered as background) rather than reading out
// of the vector's bounds.
u8 read_byte(int offset)
{
    if ((offset < 0) || ((size_t)offset >= g_rom_data.size()))
        return 0;
    return g_rom_data[(size_t)offset];
}

// base_offset is the byte position of tile (row 0, col 0) of what's
// currently visible. g_tile_offset then biases every read by 0-7 bytes on
// top of that, independent of scroll position, for sub-tile alignment.
void decode_rom_inspector_buffer(int base_offset)
{
    int tiles_per_row = g_tiles_per_row;
    if (tiles_per_row < 1)
        tiles_per_row = 1;
    if (tiles_per_row > kMaxTilesPerAxis)
        tiles_per_row = kMaxTilesPerAxis;

    for (int tile_row = 0; tile_row < kMaxTilesPerAxis; tile_row++)
    {
        for (int tile_col = 0; tile_col < kMaxTilesPerAxis; tile_col++)
        {
            bool active_column = tile_col < tiles_per_row;
            int tile_number = active_column ? ((tile_row * tiles_per_row) + tile_col) : 0;

            for (int offset_y = 0; offset_y < 8; offset_y++)
            {
                u8 pattern_byte = active_column
                    ? read_byte(base_offset + g_tile_offset + (tile_number * 8) + offset_y)
                    : 0;

                int fg_color = 15;
                int bg_color = 0;

                if (active_column && (g_colortable_mode != GuiDebugRomInspectorColorTableNone))
                {
                    int color_addr = (g_colortable_mode == GuiDebugRomInspectorColorTableMode2)
                        ? (g_colortable_offset + (tile_number * 8) + offset_y)
                        : (g_colortable_offset + (tile_number >> 3));
                    u8 color_byte = read_byte(color_addr);
                    fg_color = color_byte >> 4;
                    bg_color = color_byte & 0x0F;
                }

                int pixel_y = (tile_row * 8) + offset_y;

                for (int offset_x = 0; offset_x < 8; offset_x++)
                {
                    int pixel_x = (tile_col * 8) + offset_x;
                    int pixel = (pixel_y * ROM_INSPECTOR_TEXTURE_SIZE) + pixel_x;
                    int byte_offset = pixel * 3;

                    int color_index;
                    if (!active_column)
                        color_index = 0;
                    else
                        color_index = IsSetBit(pattern_byte, 7 - offset_x) ? fg_color : bg_color;

                    // Same TMS9918 palette table Video.cpp defaults to
                    // (kPalette_888_tms9918_analog) - a real color LUT, not
                    // an approximation, and independent of any live Video
                    // instance since it's a plain static array.
                    const u8* rgb = &kPalette_888_tms9918_analog[color_index * 3];
                    emu_debug_rom_inspector_buffer[byte_offset] = rgb[0];
                    emu_debug_rom_inspector_buffer[byte_offset + 1] = rgb[1];
                    emu_debug_rom_inspector_buffer[byte_offset + 2] = rgb[2];
                }
            }
        }
    }
}

// Scores every 8-byte-aligned offset as a candidate Mode 2 color table
// start, on two signals a real color map shows and arbitrary ROM data
// usually doesn't:
//
//  - "flatness": within one tile's 8 color bytes, a sprite or font glyph
//    rarely swaps fg/bg mid-character, so consecutive bytes tend to repeat
//    rather than change on every row.
//  - "reuse": across the whole window the table draws from a modest,
//    non-random set of byte values (a small palette reused everywhere),
//    unlike a single constant fill (unused ROM space, scored low) or
//    high-entropy data such as code or the pattern table itself (also
//    scored low).
//
// There is no structural relationship between a pattern table and its color
// table in an arbitrary ROM the way there is in a fixed VRAM layout, so this
// ranks likely candidates for a human to pick from - it does not claim to
// find "the" answer. Non-overlapping top candidates only: without that, the
// list would be dominated by many near-identical offsets from the same
// region since the window slides one block (8 bytes) at a time.
void search_colortable_candidates()
{
    g_colortable_candidates.clear();
    g_colortable_search_ran = true;

    const int total_bytes = (int)g_rom_data.size();
    const int nblocks = total_bytes / 8;
    const int window_blocks = std::min(kColorTableSearchWindowBytes / 8, nblocks);

    if (window_blocks < 8 || nblocks <= window_blocks)
        return;

    std::vector<u8> flatness((size_t)nblocks);
    for (int b = 0; b < nblocks; b++)
    {
        const u8* block = &g_rom_data[(size_t)b * 8];
        int transitions = 0;
        for (int k = 0; k < 7; k++)
            if (block[k] != block[k + 1])
                transitions++;
        flatness[(size_t)b] = (u8)(7 - transitions);
    }

    int freq[256] = { 0 };
    int distinct_count = 0;
    long long flatness_sum = 0;

    auto add_byte = [&](u8 v) { if (freq[v]++ == 0) distinct_count++; };
    auto remove_byte = [&](u8 v) { if (--freq[v] == 0) distinct_count--; };

    for (int b = 0; b < window_blocks; b++)
    {
        flatness_sum += flatness[(size_t)b];
        const u8* block = &g_rom_data[(size_t)b * 8];
        for (int k = 0; k < 8; k++)
            add_byte(block[k]);
    }

    const int window_bytes = window_blocks * 8;
    // A reused palette in a window this size plausibly has on the order of
    // a couple percent distinct byte values - not a measured constant, a
    // starting point for the bell curve below.
    const float target_distinct = std::max(2.0f, (float)window_bytes * 0.02f);
    const float log_target = std::log2(target_distinct);

    auto score_for = [&]() -> float
    {
        float flat_norm = (float)flatness_sum / (float)(window_blocks * 7);
        float log_d = std::log2((float)std::max(1, distinct_count));
        float diff = (log_d - log_target) / 2.0f;
        float reuse_score = std::exp(-0.5f * diff * diff);
        return flat_norm * 0.5f + reuse_score * 0.5f;
    };

    std::vector<ColorTableCandidate> all;
    all.reserve((size_t)(nblocks - window_blocks + 1));
    all.push_back({ 0, score_for() });

    for (int c = 1; c <= nblocks - window_blocks; c++)
    {
        int leaving_block = c - 1;
        int entering_block = c + window_blocks - 1;

        flatness_sum -= flatness[(size_t)leaving_block];
        const u8* leave = &g_rom_data[(size_t)leaving_block * 8];
        for (int k = 0; k < 8; k++)
            remove_byte(leave[k]);

        flatness_sum += flatness[(size_t)entering_block];
        const u8* enter = &g_rom_data[(size_t)entering_block * 8];
        for (int k = 0; k < 8; k++)
            add_byte(enter[k]);

        all.push_back({ c * 8, score_for() });
    }

    std::sort(all.begin(), all.end(), [](const ColorTableCandidate& a, const ColorTableCandidate& b)
    {
        return a.score > b.score;
    });

    for (const ColorTableCandidate& cand : all)
    {
        if ((int)g_colortable_candidates.size() >= kColorTableMaxCandidates)
            break;

        bool overlaps = false;
        for (const ColorTableCandidate& picked : g_colortable_candidates)
        {
            if (std::abs(cand.offset - picked.offset) < window_bytes)
            {
                overlaps = true;
                break;
            }
        }
        if (!overlaps)
            g_colortable_candidates.push_back(cand);
    }
}

}

bool gui_debug_rom_inspector_load_file(const char* file_path)
{
    return load_rom_file(file_path);
}

void gui_debug_rom_inspector_set_tile_offset(int value)
{
    if (value < 0) value = 0;
    if (value > 7) value = 7;
    g_tile_offset = value;
}

void gui_debug_rom_inspector_set_tiles_per_row(int value)
{
    if (value < 1) value = 1;
    if (value > kMaxTilesPerAxis) value = kMaxTilesPerAxis;
    g_tiles_per_row = value;
}

void gui_debug_rom_inspector_set_colortable(int mode, int offset)
{
    if ((mode < GuiDebugRomInspectorColorTableNone) || (mode > GuiDebugRomInspectorColorTableMode2))
        return;
    g_colortable_mode = mode;
    g_colortable_offset = offset < 0 ? 0 : offset;
}

void gui_debug_rom_inspector_set_scroll_row(int top_row)
{
    g_scroll_top_row = clamp_top_row(top_row);
    g_scroll_dirty = true;
}

GuiDebugRomInspectorStatus gui_debug_rom_inspector_get_status(void)
{
    GuiDebugRomInspectorStatus status;
    status.file_path = g_file_path;
    status.file_size = (int)g_rom_data.size();
    status.tile_offset = g_tile_offset;
    status.tiles_per_row = g_tiles_per_row;
    status.colortable_mode = g_colortable_mode;
    status.colortable_offset = g_colortable_offset;
    status.scroll_row = g_scroll_top_row;
    status.total_rows = compute_total_rows();
    return status;
}

void gui_debug_rom_inspector_render_visible(int* out_width, int* out_height)
{
    g_scroll_top_row = clamp_top_row(g_scroll_top_row);
    int base_offset = (g_scroll_top_row * g_tiles_per_row) * 8;
    decode_rom_inspector_buffer(base_offset);

    int total_rows = compute_total_rows();
    int visible_rows = total_rows - g_scroll_top_row;
    if (visible_rows > kMaxTilesPerAxis) visible_rows = kMaxTilesPerAxis;
    if (visible_rows < 0) visible_rows = 0;

    if (out_width) *out_width = g_tiles_per_row * 8;
    if (out_height) *out_height = visible_rows * 8;
}

void gui_debug_rom_inspector_window(void)
{
    ImGui::SetNextWindowPos(ImVec2(200, 200), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(700, 640), ImGuiCond_FirstUseEver);

    ImGui::Begin("ROM Inspector", &config_debug.show_rom_inspector);
    ImGui::PushFont(gui_default_font);

    if (ImGui::Button("Open..."))
        open_rom_file_dialog();
    ImGui::SameLine();
    ImGui::TextColored(cyan, "File:"); ImGui::SameLine();
    ImGui::TextUnformatted(g_file_path[0] ? g_file_path : "(none)");

    ImGui::TextColored(cyan, "Size:"); ImGui::SameLine();
    ImGui::Text("$%X (%zu bytes)", (unsigned int)g_rom_data.size(), g_rom_data.size());

    ImGui::Separator();

    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Tile Offset (fine, 0-7)", &g_tile_offset, 0, 7);
    if (g_tile_offset < 0)
        g_tile_offset = 0;
    if (g_tile_offset > 7)
        g_tile_offset = 7;

    ImGui::SetNextItemWidth(350);
    ImGui::SliderInt("Tiles Per Row", &g_tiles_per_row, 1, kMaxTilesPerAxis);
    if (g_tiles_per_row < 1)
        g_tiles_per_row = 1;
    if (g_tiles_per_row > kMaxTilesPerAxis)
        g_tiles_per_row = kMaxTilesPerAxis;

    ImGui::TextColored(cyan, "Color Table:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    const char* colortable_labels[3] = { "None", "Mode 1 (Graphics I)", "Mode 2 (Graphics II)" };
    if (ImGui::BeginCombo("##colortable_mode", colortable_labels[g_colortable_mode]))
    {
        for (int i = 0; i < 3; i++)
        {
            if (ImGui::Selectable(colortable_labels[i], g_colortable_mode == i))
                g_colortable_mode = i;
        }
        ImGui::EndCombo();
    }

    if (g_colortable_mode != GuiDebugRomInspectorColorTableNone)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::InputInt("Color Table Offset", &g_colortable_offset, 1, 8, ImGuiInputTextFlags_CharsHexadecimal);
        if (g_colortable_offset < 0)
            g_colortable_offset = 0;

        if (g_colortable_mode == GuiDebugRomInspectorColorTableMode2)
        {
            ImGui::SameLine();
            if (ImGui::Button("Auto Search"))
                search_colortable_candidates();

            if (g_colortable_search_ran)
            {
                if (g_colortable_candidates.empty())
                {
                    ImGui::TextColored(mid_gray, "No candidates (file smaller than the %d-byte search window)", kColorTableSearchWindowBytes);
                }
                else
                {
                    ImGui::TextColored(mid_gray, "Candidates, best first - click to use:");
                    if (ImGui::BeginTable("##colortable_candidates", 2,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
                        ImVec2(300, ImGui::GetTextLineHeightWithSpacing() * 6)))
                    {
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableSetupColumn("ADDRESS");
                        ImGui::TableSetupColumn("SCORE");
                        ImGui::TableHeadersRow();

                        for (const ColorTableCandidate& cand : g_colortable_candidates)
                        {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            char label[32];
                            snprintf(label, sizeof(label), "$%06X", cand.offset);
                            if (ImGui::Selectable(label, g_colortable_offset == cand.offset, ImGuiSelectableFlags_SpanAllColumns))
                                g_colortable_offset = cand.offset;
                            ImGui::TableNextColumn();
                            ImGui::Text("%.2f", (double)cand.score);
                        }
                        ImGui::EndTable();
                    }
                }
            }
        }
    }

    ImGui::Separator();

    const int total_rows = compute_total_rows();
    const float content_width = (float)(g_tiles_per_row * 8) * kScale;
    const float content_height = (float)total_rows * kTilePx;

    ImGui::BeginChild("##rom_inspector_scroll", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);

    // The scrollbar navigates the *entire* loaded ROM, not just the fixed
    // texture: this Dummy establishes the true scroll range (every tile
    // row in the file). g_scroll_dirty means MCP just set a position
    // programmatically - snap the real ImGui scrollbar to match before
    // reading it back, instead of the usual GUI-drag -> GetScrollY() flow.
    ImVec2 origin = ImGui::GetCursorPos();
    ImGui::Dummy(ImVec2(content_width, content_height));

    if (g_scroll_dirty)
    {
        ImGui::SetScrollY(g_scroll_top_row * kTilePx);
        g_scroll_dirty = false;
    }
    else
    {
        g_scroll_top_row = clamp_top_row((int)(ImGui::GetScrollY() / kTilePx));
    }

    int width = 0, height = 0;
    gui_debug_rom_inspector_render_visible(&width, &height);

    float image_width = (float)width * kScale;
    float image_height = (float)height * kScale;
    float uv_x = (float)g_tiles_per_row / (float)kMaxTilesPerAxis;
    float uv_y = (float)height / (float)(kMaxTilesPerAxis * 8);

    ImGui::SetCursorPos(ImVec2(origin.x, origin.y + ((float)g_scroll_top_row * kTilePx)));
    renderer_begin_emulator_image();
    ImGui::Image((ImTextureID)(intptr_t)renderer_emu_debug_rom_inspector, ImVec2(image_width, image_height),
                 ImVec2(0.0f, 0.0f), ImVec2(uv_x, uv_y));
    renderer_end_emulator_image();
    ImGui::EndChild();

    ImGui::PopFont();
    ImGui::End();
}
