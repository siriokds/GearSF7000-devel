/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026 Saverio Russo
 */

#include "gui_debug_memory.h"

#include <array>
#include <cstring>

#include "imgui/imgui.h"
#include "imgui/colors.h"
#include "imgui/memory_editor.h"
#include "config.h"
#include "emu.h"
#include "gui.h"
#include "../../src/gearsf7000.h"
#if GEARSF7000_ENABLE_SF7000
#include "../../src/SF7000.h"
#endif

namespace
{
MemEditor g_editors[GuiDebugMemoryEditorSlotCount];
struct ViewState
{
    const char* title = "";
    std::uint8_t* data = nullptr;
    int size = 0;
    int base_address = 0;
};
std::array<ViewState, GuiDebugMemoryEditorSlotCount> g_views;
int g_requested_selection = -1;
int g_current_editor = GuiDebugMemoryEditorSlotPrimary;

bool valid_slot(int slot)
{
    return slot >= 0 && slot < GuiDebugMemoryEditorSlotCount;
}
}

// Ported from GearSF7000, which has had this since before the fork. Its
// absence here is why bookmarks looked missing: MemEditor implements them in
// full, but the only way in was the right-click context menu, which nobody
// finds. Everything below drives APIs this editor already had.
static void draw_menu_bar(void)
{
    if (!ImGui::BeginMenuBar())
        return;

    MemEditor& editor = g_editors[valid_slot(g_current_editor)
        ? g_current_editor : GuiDebugMemoryEditorSlotPrimary];

    if (ImGui::BeginMenu("File"))
    {
        // This project keeps import/export in its own window rather than
        // behind file dialogs here, so the entry opens that instead.
        if (ImGui::MenuItem("Import / Export..."))
            config_debug.show_memory_import = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit"))
    {
        if (ImGui::MenuItem("Copy", "Ctrl+C"))
            gui_debug_memory_copy();
        if (ImGui::MenuItem("Paste", "Ctrl+V"))
            gui_debug_memory_paste();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Selection"))
    {
        if (ImGui::MenuItem("Select All", "Ctrl+A"))
            editor.SelectAll();
        if (ImGui::MenuItem("Clear Selection"))
            editor.ClearSelection();

        if (ImGui::BeginMenu("Set value"))
        {
            static char set_value_buffer[8] = {};
            const ImVec2 character_size = ImGui::CalcTextSize("X");
            const int word_bytes = editor.GetWordBytes();
            ImGui::SetNextItemWidth(((word_bytes * 2) + 1) * character_size.x);
            const bool entered = ImGui::InputTextWithHint("##set_value",
                word_bytes == 1 ? "XX" : "XXXX", set_value_buffer,
                IM_ARRAYSIZE(set_value_buffer),
                ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue
                | ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
            ImGui::SameLine();
            const bool pressed = ImGui::Button("Set!", ImVec2(40, 0));
            if (entered || pressed)
            {
                // memory_editor.cpp's parse_hex_string is a template with no
                // declaration in a header, so parse locally rather than widen
                // its visibility for one caller. Done by hand rather than with
                // sscanf, which MSVC reports as deprecated and which would be
                // the only use of it in the project.
                unsigned int value = 0;
                bool valid = set_value_buffer[0] != 0;
                for (const char* c = set_value_buffer; *c && valid; c++)
                {
                    const char d = *c;
                    if (d >= '0' && d <= '9')      value = (value << 4) | (unsigned)(d - '0');
                    else if (d >= 'A' && d <= 'F') value = (value << 4) | (unsigned)(d - 'A' + 10);
                    else if (d >= 'a' && d <= 'f') value = (value << 4) | (unsigned)(d - 'a' + 10);
                    else                            valid = false;
                }
                if (valid)
                {
                    editor.SetValueToSelection((int)value);
                    set_value_buffer[0] = 0;
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Bookmarks"))
    {
        if (ImGui::MenuItem("Add Bookmark"))
            editor.AddBookmark();
        if (ImGui::MenuItem("Clear All"))
            editor.RemoveBookmarks();

        std::vector<MemEditor::Bookmark>* bookmarks = editor.GetBookmarks();
        if (bookmarks->size() > 0)
            ImGui::Separator();

        for (size_t i = 0; i < bookmarks->size(); i++)
        {
            MemEditor::Bookmark* bookmark = &(*bookmarks)[i];
            char label[96];
            snprintf(label, sizeof(label), "$%04X: %s", bookmark->address, bookmark->name);
            if (ImGui::MenuItem(label))
                editor.JumpToAddress(bookmark->address);
        }
        ImGui::EndMenu();
    }

    char label[80];

    if (ImGui::BeginMenu("Watches"))
    {
        snprintf(label, sizeof(label), "Open %s Watch Window", editor.GetTitle());
        if (ImGui::MenuItem(label)) editor.OpenWatchWindow();
        snprintf(label, sizeof(label), "Add %s Watch", editor.GetTitle());
        if (ImGui::MenuItem(label)) editor.AddWatch();
        snprintf(label, sizeof(label), "Clear All %s Watches", editor.GetTitle());
        if (ImGui::MenuItem(label)) editor.RemoveWatches();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Search"))
    {
        snprintf(label, sizeof(label), "Open %s Search Window", editor.GetTitle());
        if (ImGui::MenuItem(label)) editor.OpenSearchWindow();
        snprintf(label, sizeof(label), "Find Bytes in %s...", editor.GetTitle());
        if (ImGui::MenuItem(label)) editor.OpenFindBytes();
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

void gui_debug_memory_window(void)
{
    // These two draw the modal popups that AddBookmark()/AddWatch() request.
    // Both only raise a flag; without something calling the popup each frame
    // the flag is set and nothing ever consumes it, which is why "Add
    // Bookmark" appeared to do nothing - from the menu bar and from the
    // right-click menu alike. They run for every editor, not just the active
    // tab, so a popup raised on one view survives switching to another.
    for (int i = 0; i < GuiDebugMemoryEditorSlotCount; i++)
    {
        g_editors[i].WatchPopup();
        g_editors[i].BookMarkPopup();
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::SetNextWindowPos(ImVec2(567, 249), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(324, 308), ImGuiCond_FirstUseEver);

    ImGui::Begin("Memory Editor", &config_debug.show_memory,
                 ImGuiWindowFlags_MenuBar);

    draw_menu_bar();

    GearSF7000Core* core = emu_get_core();
    Memory* memory = core->GetMemory();
    Cartridge* cartridge = core->GetCartridge();
    Video* video = core->GetVideo();
#if GEARSF7000_ENABLE_SF7000
    SF7000* sf7000 = core->GetSF7000();
#endif

    // The flat map has no ROM in the machine and no banking, so both the
    // "ROM" label and the BANK/ADDRESS pair would be stating zeroes about
    // something that does not exist. Only the mapper name is meaningful there.
    const bool flatMap = !memory->IsSF7000Enabled()
        && cartridge->GetType() == Cartridge::SC3000_FLAT64K;

    ImGui::PushFont(gui_default_font);
    ImGui::TextColored(cyan, "%s", memory->IsSF7000Enabled() ? "SF-7000"
                                 : flatMap ? "FLAT 64K" : "ROM");
    if (!flatMap)
    {
        ImGui::SameLine();
        ImGui::TextColored(magenta, " BANK");
        ImGui::SameLine();
        ImGui::Text("$%02X", memory->GetRomBank());
        ImGui::SameLine();
        ImGui::TextColored(magenta, " ADDRESS");
        ImGui::SameLine();
        ImGui::Text("$%05X", memory->GetRomBankAddress());
    }
    ImGui::SameLine();
    ImGui::TextColored(magenta, " CONFIG");
    ImGui::SameLine();
    char mapper_description[128] = {};
    cartridge->GetMapperDescr(mapper_description);
    ImGui::TextUnformatted(mapper_description);
#if GEARSF7000_ENABLE_SF7000
    if (memory->IsSF7000Enabled())
    {
        ImGui::SameLine();
        ImGui::TextColored(magenta, " IPL");
        ImGui::SameLine();
        ImGui::TextUnformatted(sf7000->IPL_Enabled() ? "ENABLED" : "DISABLED");
    }
#endif
    ImGui::PopFont();

    const auto draw_tab = [](const char* title, int slot, std::uint8_t* data,
                             int size, int base, ImFont* font)
    {
        const ImGuiTabItemFlags flags = gui_debug_memory_should_select(slot)
            ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem(title, nullptr, flags))
        {
            gui_debug_memory_draw_view(slot, title, data, size, base, font, true);
            ImGui::EndTabItem();
        }
    };

    if (ImGui::BeginTabBar("##memory_tabs", ImGuiTabBarFlags_None))
    {
        if (memory->IsSF7000Enabled())
        {
            // Physical IPL image.  The later logical CPU views will make the
            // read-only overlay and RAM write-through explicit.
            draw_tab("IPL ROM 8K", GuiDebugMemoryEditorSlotPrimary,
                     memory->GetBios(), 0x2000, 0x0000, gui_default_font);
            draw_tab("SF-7000 RAM 64K", GuiDebugMemoryEditorSlotSecondary,
                     memory->GetSGMRam(), 0x10000, 0x0000, gui_unifont_font);
        }
        else if (cartridge->GetType() == Cartridge::SC3000_FLAT64K)
        {
            // The flat map has no ROM in the machine at all: the image is
            // copied into the 64 KiB at reset and everything the CPU sees
            // afterwards is that RAM. Showing cartridge->GetROM() here would
            // show the file on disk, never the writes the program has made -
            // so the live buffer comes first, exactly as in the SF-7000 case
            // above, and the image is offered beside it for comparison.
            draw_tab("RAM 64K", GuiDebugMemoryEditorSlotPrimary,
                     memory->GetSGMRam(), 0x10000, 0x0000, gui_unifont_font);
            if (cartridge->GetROMSize() > 0)
                draw_tab("IMAGE", GuiDebugMemoryEditorSlotSecondary,
                         cartridge->GetROM(), cartridge->GetROMSize(), 0x0000,
                         gui_default_font);
        }
        else if (cartridge->GetType() == Cartridge::SC3000_ASC16L)
        {
            draw_tab("ROM FIXED", GuiDebugMemoryEditorSlotPrimary,
                     cartridge->GetROM(), 0x8000, 0x0000, gui_default_font);
            draw_tab("ROM PAGED", GuiDebugMemoryEditorSlotSecondary,
                     cartridge->GetROM() + memory->GetRomBankAddress(),
                     0x4000, 0x8000, gui_default_font);
            // ASC16L has a 16 KiB RAM window at $C000-$FFFF.  The backing
            // storage starts at $4000 in the shared 64 KiB allocation.
            draw_tab("RAM 16K", GuiDebugMemoryEditorSlotTertiary,
                     memory->GetSGMRam() + 0x4000, 0x4000, 0xC000,
                     gui_unifont_font);
        }
        else
        {
            draw_tab("ROM", GuiDebugMemoryEditorSlotPrimary,
                     cartridge->GetROM(), cartridge->GetROMSize(), 0x0000,
                     gui_default_font);

            switch (cartridge->GetType())
            {
                case Cartridge::SC3000_2K:
                    draw_tab("RAM 2K", GuiDebugMemoryEditorSlotSecondary,
                             memory->GetRam(), 0x0800, 0xC000,
                             gui_default_font);
                    break;
                case Cartridge::SC3000_32K:
                    draw_tab("RAM 32K", GuiDebugMemoryEditorSlotSecondary,
                             memory->GetSGMRam(), 0x8000, 0x8000,
                             gui_default_font);
                    break;
                case Cartridge::SG1000_1K:
                    draw_tab("RAM 1K", GuiDebugMemoryEditorSlotSecondary,
                             memory->GetRam(), 0x0400, 0xC000,
                             gui_default_font);
                    break;
                case Cartridge::SG1000_16K:
                    draw_tab("RAM 16K", GuiDebugMemoryEditorSlotTertiary,
                             memory->GetSGMRam(), 0x4000, 0xC000,
                             gui_unifont_font);
                    break;
                case Cartridge::SC3000_16K_AT_8000:
                    // Two tabs because they are two chips. Folding the 2 KB
                    // into a 16 KB view would just show eight copies of it.
                    draw_tab("CART RAM 16K", GuiDebugMemoryEditorSlotSecondary,
                             memory->GetSGMRam(), 0x4000, 0x8000,
                             gui_unifont_font);
                    draw_tab("INT RAM 2K", GuiDebugMemoryEditorSlotTertiary,
                             memory->GetRam(), 0x0800, 0xC000,
                             gui_default_font);
                    break;
                default:
                    break;
            }
        }

        draw_tab("VRAM", GuiDebugMemoryEditorSlotVram, video->GetVRAM(),
                 0x4000, 0x0000, gui_default_font);
        ImGui::EndTabBar();
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

bool gui_debug_memory_should_select(int slot)
{
    return valid_slot(slot) && g_requested_selection == slot;
}

void gui_debug_memory_draw_view(int slot, const char* title, std::uint8_t* data, int size,
                                int base_address, ImFont* font,
                                bool request_select)
{
    if (!valid_slot(slot) || data == nullptr || size <= 0)
        return;

    if (request_select && g_requested_selection == slot)
        g_requested_selection = -1;

    g_current_editor = slot;
    ViewState& view = g_views[slot];
    if (view.data != data || view.size != size ||
        view.base_address != base_address ||
        std::strcmp(view.title, title != nullptr ? title : "") != 0)
    {
        view.title = title != nullptr ? title : "";
        view.data = data;
        view.size = size;
        view.base_address = base_address;
        g_editors[slot].Reset(view.title, data, size, base_address);
    }

    // The setting is global to Memory Editor, rather than tied to a transient
    // tab object.  A font chosen in ROM therefore follows the user into RAM,
    // VRAM and the next application launch.
    (void)font;
    MemEditor::Options options = g_editors[slot].GetOptions();
    if (options.data_font != config_debug.memory_data_font)
    {
        options.data_font = config_debug.memory_data_font;
        g_editors[slot].SetOptions(options);
    }

    ImFont* data_font = gui_default_font;
    if (config_debug.memory_data_font == 1 && gui_memory_condensed_font != nullptr)
        data_font = gui_memory_condensed_font;
    else if (config_debug.memory_data_font == 2 && gui_memory_condensed_light_font != nullptr)
        data_font = gui_memory_condensed_light_font;
    g_editors[slot].SetGuiFont(data_font);
    g_editors[slot].Draw();

    // DrawOptions() may have changed the choice this frame; retain it before
    // another tab is drawn and before config_write() runs at shutdown.
    config_debug.memory_data_font = g_editors[slot].GetOptions().data_font;
}

void gui_debug_memory_goto(int slot, int address)
{
    if (!valid_slot(slot))
        return;

    g_requested_selection = slot;
    g_editors[slot].JumpToAddress(address);
}

void gui_debug_memory_copy(void)
{
    if (!valid_slot(g_current_editor))
        return;

    g_editors[g_current_editor].Copy();
}

void gui_debug_memory_paste(void)
{
    if (!valid_slot(g_current_editor))
        return;

    g_editors[g_current_editor].Paste();
}

// Deliberately duplicates gui_debug_memory_window()'s tab dispatch instead
// of sharing it - that function is already shipped/working and this is a
// read-only query used by unrelated tools (Mem Import/Export, MCP), so the
// lower-risk choice is a second small copy over a wider refactor of tested
// GUI code. Keep the two in sync by hand if the profile dispatch changes.
int gui_debug_memory_get_current_views(GuiDebugMemoryView* out_views, int max_views)
{
    if (out_views == nullptr || max_views <= 0)
        return 0;

    GearSF7000Core* core = emu_get_core();
    Memory* memory = core->GetMemory();
    Cartridge* cartridge = core->GetCartridge();

    int count = 0;
    const auto add_view = [&](const char* title, std::uint8_t* data, int size, int base_address)
    {
        if (count >= max_views)
            return;
        out_views[count].title = title;
        out_views[count].data = data;
        out_views[count].size = size;
        out_views[count].base_address = base_address;
        count++;
    };

    // The importer targets the real internal buffers directly, ignoring
    // whatever the currently active mapper/profile happens to expose at a
    // given CPU address - these four physical buffers exist unconditionally,
    // independent of cartridge type or SF-7000 mode, so the list is fixed
    // rather than dispatched per profile like gui_debug_memory_window() is.
    if (cartridge->GetROMSize() > 0)
        add_view("ROM", cartridge->GetROM(), cartridge->GetROMSize(), 0x0000);
    add_view("RAM 64K", memory->GetSGMRam(), 0x10000, 0x0000);
    add_view("RAM 2K", memory->GetRam(), 0x0800, 0x0000);
    add_view("IPL 8K", memory->GetBios(), 0x2000, 0x0000);

    return count;
}

// Search, Find Bytes and Watches each open their own top-level window, so they
// cannot be drawn inside the Memory Editor window - they are pumped alongside
// it. Unconditional and cheap: each returns immediately unless its editor has
// been asked to open it.
void gui_debug_memory_search_window(void)
{
    for (int i = 0; i < GuiDebugMemoryEditorSlotCount; i++)
    {
        ImGui::PushFont(gui_default_font);
        g_editors[i].DrawSearchWindow();
        ImGui::PopFont();
    }
}

void gui_debug_memory_find_bytes_window(void)
{
    for (int i = 0; i < GuiDebugMemoryEditorSlotCount; i++)
    {
        ImGui::PushFont(gui_default_font);
        g_editors[i].DrawFindBytesWindow();
        ImGui::PopFont();
    }
}

void gui_debug_memory_watches_window(void)
{
    for (int i = 0; i < GuiDebugMemoryEditorSlotCount; i++)
    {
        ImGui::PushFont(gui_default_font);
        g_editors[i].DrawWatchWindow();
        ImGui::PopFont();
    }
}
