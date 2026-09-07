/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026 Saverio Russo
 */

#include "gui_debug_disc_explorer.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/colors.h"
#include "nfd/nfd.h"
#include "config.h"
#include "emu.h"
#include "gui.h"
#include "../../src/GearSF7000Core.h"
#include "../../src/SF7000.h"
#include "../../src/sf7fs/SF7Disc.h"

namespace
{

// Same idea as gui.cpp's own ScopedFileDialogAudioPause - not exposed via a
// header, small enough to duplicate rather than plumb a shared one through.
struct ScopedAudioPause
{
    ScopedAudioPause() { emu_audio_suspend_for_file_dialog(); }
    ~ScopedAudioPause() { emu_audio_resume_after_file_dialog(); }
    ScopedAudioPause(const ScopedAudioPause&) = delete;
    ScopedAudioPause& operator=(const ScopedAudioPause&) = delete;
};

SF7Image g_image;
bool  g_loaded = false;
char  g_status[256] = "";
bool  g_status_is_error = false;

char  g_disc_path[1024] = "";
char  g_filter[64] = "";
char  g_import_path[1024] = "";
char  g_import_name[16] = "";

bool  g_show_size = true;
float g_listing_height = 220.0f;   // dragged by the splitter below the table
bool  g_show_raw = false;

int   g_selected = -1;                 // index into g_entries
std::vector<DirectoryItem> g_entries;
std::map<int, uint8_t> g_fat;
std::vector<uint8_t> g_selected_chain; // clusters of the selected file

void set_status(bool error, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, args);
    va_end(args);
    g_status_is_error = error;
}

// Re-reads directory and FAT from the in-memory image. Called after every
// change so the view never disagrees with the image behind it.
void refresh(void)
{
    g_entries.clear();
    g_fat.clear();
    g_selected_chain.clear();

    if (!g_loaded)
        return;

    g_image.readDirectory(&g_entries);
    g_image.readFAT(&g_fat);

    if (g_selected >= static_cast<int>(g_entries.size()))
        g_selected = -1;

    if (g_selected >= 0)
    {
        int size = 0;
        sf7GetClusterChainFromFAT(g_fat, g_entries[g_selected].initialCluster,
                                  g_selected_chain, &size);
    }
}

void open_path(const char* path)
{
    FATResult r = g_image.load(path);
    if (r != FATResult::SUCCESS)
    {
        g_loaded = false;
        set_status(true, "%s", sf7FATErrorText(r).c_str());
        refresh();
        return;
    }

    g_loaded = true;
    g_selected = -1;
    refresh();

    // The verdict is shown, never enforced: an image that is not a Disk BASIC
    // volume still opens, which is usually why it is worth opening.
    FATResult verdict = g_image.validate();
    if (verdict == FATResult::SUCCESS)
        set_status(false, "%d entries", static_cast<int>(g_entries.size()));
    else
        set_status(true, "%s Showing it anyway.", sf7FATErrorText(verdict).c_str());
}

void browse_button(const char* id, char* buf, size_t size, bool save)
{
    ImGui::PushID(id);
    if (ImGui::Button("Browse..."))
    {
        ScopedAudioPause audioPause;
        nfdchar_t* out = nullptr;
        nfdresult_t result = save
            ? NFD_SaveDialog(&out, nullptr, 0, nullptr, nullptr)
            : NFD_OpenDialog(&out, nullptr, 0, nullptr);
        if (result == NFD_OKAY)
        {
            strncpy(buf, out, size - 1);
            buf[size - 1] = 0;
            NFD_FreePath(out);
        }
        else if (result != NFD_CANCEL)
            Log("Disc Explorer Browse Error: %s", NFD_GetError());
    }
    ImGui::PopID();
}


void draw_toolbar(void)
{
    ImGui::PushItemWidth(-220.0f);
    ImGui::InputText("##disc", g_disc_path, sizeof(g_disc_path));
    ImGui::PopItemWidth();

    ImGui::SameLine();
    browse_button("disc", g_disc_path, sizeof(g_disc_path), false);

    ImGui::SameLine();
    if (ImGui::Button("Open") && g_disc_path[0])
        open_path(g_disc_path);

    ImGui::SameLine();
    // Fills in the path of whatever the emulator has inserted. The file is
    // then read from disc like any other: the mounted image is not touched.
    if (ImGui::Button("Mounted disc"))
    {
        GearSF7000Core* core = emu_get_core();
        SF7000* sf7000 = core ? core->GetSF7000() : NULL;
        DiskDebugInfo disk = {};
        if (sf7000 != NULL)
            sf7000->GetDiskDebugInfo(&disk);

        if (disk.present && disk.fileName && disk.fileName[0])
        {
            strncpy(g_disc_path, disk.fileName, sizeof(g_disc_path) - 1);
            g_disc_path[sizeof(g_disc_path) - 1] = 0;
            open_path(g_disc_path);
        }
        else
            set_status(true, "No disc inserted in the emulated drive");
    }

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fill in the path of the disc currently inserted, and open it");
}

void draw_header(void)
{
    if (!g_loaded)
        return;

    std::string label = g_image.discLabel();
    ImGui::TextColored(label.empty() ? gray : cyan, "%s",
                       label.empty() ? "(not a system disc)" : label.c_str());

    const int freeClusters = sf7CountFreeClusters(g_fat);
    ImGui::SameLine();
    ImGui::TextColored(gray, "  |  %d free clusters, %d bytes",
                       freeClusters, freeClusters * DISC_CLUSTER_SIZE_BYTES);

    if (g_image.isDirty())
    {
        ImGui::SameLine();
        ImGui::TextColored(orange, "  |  modified, not saved");
    }
}

// The stored name is a fixed 8.3 field padded with spaces, which reads badly
// in a column ("CLI     .COM"). Trim both halves for display and keep the case
// as written -- the raw 16 bytes are a checkbox away when they matter.
std::string display_name(const DirectoryItem& e)
{
    auto take = [&](int from, int count) {
        std::string out;
        for (int i = 0; i < count; i++)
        {
            unsigned char c = static_cast<unsigned char>(e.filename[from + i]);
            if (c == 0x00) break;
            out += (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?';
        }
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out;
    };

    const std::string name = take(0, MAX_FILENAME_LENGTH);
    const std::string ext = take(MAX_FILENAME_LENGTH + 1, MAX_EXTENSION_LENGTH);

    if (ext.empty())
        return name;
    return name + "." + ext;
}

void draw_listing(void)
{
    ImGui::PushItemWidth(120.0f);
    ImGui::InputTextWithHint("##filter", "*.BAS", g_filter, sizeof(g_filter));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::Checkbox("Size", &g_show_size);
    ImGui::SameLine();
    ImGui::Checkbox("Raw entry", &g_show_raw);

    const int columns = 5 + (g_show_size ? 1 : 0) + (g_show_raw ? 1 : 0);

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;

    if (!ImGui::BeginTable("entries", columns, flags, ImVec2(0.0f, g_listing_height)))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Cluster");
    ImGui::TableSetupColumn("Attr");
    ImGui::TableSetupColumn("Type");
    ImGui::TableSetupColumn("Flags");
    if (g_show_size) ImGui::TableSetupColumn("Size");
    if (g_show_raw)  ImGui::TableSetupColumn("Raw entry", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (int i = 0; i < static_cast<int>(g_entries.size()); i++)
    {
        const DirectoryItem& e = g_entries[i];
        const std::string name = sf7NormalizeFilename(e.filename);

        if (!sf7MatchWildcard(name, g_filter))
            continue;

        ImGui::TableNextRow();
        ImGui::TableNextColumn();

        // Reserved padding entries are listed like any other: nothing is
        // hidden, only the field padding is trimmed for reading.
        const std::string shown = display_name(e);

        ImGui::PushID(i);
        const bool clicked = ImGui::Selectable(shown.c_str(), g_selected == i,
                                               ImGuiSelectableFlags_SpanAllColumns);
        ImGui::PopID();
        if (clicked)
        {
            g_selected = i;
            g_selected_chain.clear();
            int size = 0;
            sf7GetClusterChainFromFAT(g_fat, e.initialCluster, g_selected_chain, &size);
            snprintf(g_import_name, sizeof(g_import_name), "%s", name.c_str());
        }

        ImGui::TableNextColumn();
        ImGui::Text("$%02X", e.initialCluster);

        ImGui::TableNextColumn();
        ImGui::Text("$%02X", e.attribute);

        // Type in the low three bits, then the flags. SC-DOS adds HIDDEN and
        // SYSTEM on top of Disk BASIC's read-only, and shows them here because
        // BASIC ignores them and would give no hint they are set.
        ImGui::TableNextColumn();
        ImGui::TextColored(white, "%s", sf7AttributeTypeText(e.attribute));

        ImGui::TableNextColumn();
        const std::string flags = sf7AttributeFlagsText(e.attribute);
        if (!flags.empty())
            ImGui::TextColored(orange, "%s", flags.c_str());
        else
            ImGui::TextColored(gray, "-");

        if (g_show_size)
        {
            ImGui::TableNextColumn();
            if (e.initialCluster == SF7_CLUSTER_NONE)
                ImGui::TextColored(gray, "0");
            else
            {
                int size = 0;
                FATResult r = g_image.fileSize(e, &size);
                if (r == FATResult::SUCCESS)
                    ImGui::Text("%d", size);
                else
                    ImGui::TextColored(red, "-");
            }
        }

        if (g_show_raw)
        {
            ImGui::TableNextColumn();
            const uint8_t* raw = reinterpret_cast<const uint8_t*>(&e);
            char hex[16 * 3 + 1];
            for (int b = 0; b < 16; b++)
                snprintf(hex + b * 3, 4, "%02X ", raw[b]);
            ImGui::TextColored(gray, "%s", hex);
        }
    }

    ImGui::EndTable();

    // Splitter: drag the bar under the table to give the listing more or less
    // room, at the expense of the FAT map below it.
    ImGui::InvisibleButton("##listing_splitter", ImVec2(-1.0f, 6.0f));
    if (ImGui::IsItemActive())
        g_listing_height += ImGui::GetIO().MouseDelta.y;
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    if (g_listing_height < 60.0f)  g_listing_height = 60.0f;
    if (g_listing_height > 2000.0f) g_listing_height = 2000.0f;

    const ImVec2 barMin = ImGui::GetItemRectMin();
    const ImVec2 barMax = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(barMin.x, barMin.y + 2.0f), ImVec2(barMax.x, barMax.y - 2.0f),
        ImGui::GetColorU32((ImGui::IsItemHovered() || ImGui::IsItemActive())
                               ? ImGuiCol_SeparatorActive
                               : ImGuiCol_Separator));
}

void draw_actions(void)
{
    const bool hasSelection = g_selected >= 0 &&
                              g_selected < static_cast<int>(g_entries.size());

    if (!hasSelection)
        ImGui::BeginDisabled();

    if (ImGui::Button("Export...") && hasSelection)
    {
        const std::string name = sf7NormalizeFilename(g_entries[g_selected].filename);
        std::vector<uint8_t> data;
        FATResult r = g_image.exportFile(name, data);
        if (r != FATResult::SUCCESS)
            set_status(true, "Export: %s", sf7FATErrorText(r).c_str());
        else
        {
            char out[1024] = "";
            {
                ScopedAudioPause audioPause;
                nfdchar_t* chosen = nullptr;
                if (NFD_SaveDialog(&chosen, nullptr, 0, nullptr, name.c_str()) == NFD_OKAY)
                {
                    snprintf(out, sizeof(out), "%s", chosen);
                    NFD_FreePath(chosen);
                }
            }
            if (out[0])
            {
                FATResult w = sf7WriteHostFile(out, data);
                set_status(w != FATResult::SUCCESS, "%s",
                           w == FATResult::SUCCESS
                               ? (name + ": " + std::to_string(data.size()) + " bytes written").c_str()
                               : sf7FATErrorText(w).c_str());
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Delete") && hasSelection)
    {
        const std::string name = sf7NormalizeFilename(g_entries[g_selected].filename);
        FATResult r = g_image.deleteFile(name);
        set_status(r != FATResult::SUCCESS, "%s",
                   r == FATResult::SUCCESS ? (name + " deleted, not yet saved").c_str()
                                           : sf7FATErrorText(r).c_str());
        g_selected = -1;
        refresh();
    }

    if (!hasSelection)
        ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextColored(gray, "|");
    ImGui::SameLine();

    if (ImGui::Button("Save") && g_image.isDirty())
    {
        FATResult r = g_image.save(g_disc_path);
        set_status(r != FATResult::SUCCESS, "%s",
                   r == FATResult::SUCCESS ? "Image written" : sf7FATErrorText(r).c_str());
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Writes the image back. Nothing reaches the file until you press this.");

    // Import
    ImGui::PushItemWidth(-260.0f);
    ImGui::InputTextWithHint("##import", "file to insert", g_import_path, sizeof(g_import_path));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    browse_button("import", g_import_path, sizeof(g_import_path), false);

    ImGui::SameLine();
    ImGui::PushItemWidth(100.0f);
    ImGui::InputTextWithHint("##name", "NAME.EXT", g_import_name, sizeof(g_import_name));
    ImGui::PopItemWidth();

    ImGui::SameLine();
    if (ImGui::Button("Import") && g_import_path[0] && g_import_name[0])
    {
        std::vector<uint8_t> data;
        FATResult r = sf7ReadHostFile(g_import_path, data);
        if (r != FATResult::SUCCESS)
            set_status(true, "%s", sf7FATErrorText(r).c_str());
        else
        {
            r = g_image.importFile(g_import_name, data);
            set_status(r != FATResult::SUCCESS, "%s",
                       r == FATResult::SUCCESS
                           ? (std::string(g_import_name) + " inserted, not yet saved").c_str()
                           : sf7FATErrorText(r).c_str());
            refresh();
        }
    }
}

// The 160-cluster map, the same information sfdisc's --fat prints: reserved,
// free, a link to the next cluster, or an end marker whose low nibble is the
// sector count. The selected file's chain is highlighted.
void draw_fat(void)
{
    if (!ImGui::CollapsingHeader("FAT", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::TextColored(gray, "SYS reserved   free   in use   end of chain");

    if (!ImGui::BeginTable("fat", 16,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
        return;

    for (int cluster = 0; cluster < DISC_CLUSTERS_NUM; cluster++)
    {
        ImGui::TableNextColumn();

        auto it = g_fat.find(cluster);
        const uint8_t v = (it == g_fat.end()) ? 0 : it->second;

        bool inChain = false;
        for (uint8_t c : g_selected_chain)
            if (c == cluster) { inChain = true; break; }

        ImVec4 color = white;
        const char* text = "--";
        char buf[8];

        if (v == FAT_RESERVED_CLUSTER)      { color = gray;  text = "SYS"; }
        else if (v == FAT_FREE_CLUSTER)     { color = green; text = "."; }
        else if (v >= FAT_LAST_CLUSTER_CODE_MIN && v <= FAT_LAST_CLUSTER_CODE_MAX)
        {
            color = orange;
            snprintf(buf, sizeof(buf), "E%d", v & 0x0F);
            text = buf;
        }
        else
        {
            color = cyan;
            snprintf(buf, sizeof(buf), "%02X", v);
            text = buf;
        }

        if (inChain)
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                                   ImGui::GetColorU32(ImVec4(0.25f, 0.35f, 0.55f, 1.0f)));

        ImGui::TextColored(color, "%s", text);

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("cluster $%02X = $%02X\ntrack %d sector %d",
                              cluster, v,
                              (cluster * DISC_CLUSTER_NUM_SECTORS) / DISC_SECTORS_PER_TRACK,
                              ((cluster * DISC_CLUSTER_NUM_SECTORS) % DISC_SECTORS_PER_TRACK) + 1);
    }

    ImGui::EndTable();
}

} // namespace

void gui_debug_disc_explorer_window(void)
{
    ImGui::SetNextWindowPos(ImVec2(120, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(760, 620), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Disc Explorer", &config_debug.show_disc_explorer))
    {
        ImGui::End();
        return;
    }

    draw_toolbar();
    ImGui::Separator();

    if (g_status[0])
        ImGui::TextColored(g_status_is_error ? red : gray, "%s", g_status);

    if (g_loaded)
    {
        draw_header();
        ImGui::Separator();
        draw_listing();
        draw_actions();
        ImGui::Separator();
        draw_fat();
    }

    ImGui::End();
}
