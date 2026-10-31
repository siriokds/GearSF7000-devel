#include "../../src/SystemBusM5.h"
/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2021  Ignacio Sanchez
 * Copyright (C) 2026  Saverio Russo
 *
 * Derived from Gearcoleco by Ignacio Sanchez (GPL-3.0-or-later); the changes
 * and additions by Saverio Russo are AGPL-3.0-or-later.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later
 */

#include "gui_debug_memory.h"

#include <array>
#include <cstring>
#include <string>

#include "imgui/imgui.h"
#include "imgui/colors.h"
#include "imgui/memory_editor.h"
#include "config.h"
#include "emu.h"
#include "gui.h"
#include "../../src/gearsf7000.h"
#include "../../src/IVideoDevice.h"
#include "../../src/SystemBusMSX.h"
#include "../../src/IVideoDebugProvider.h"
#if GEARSF7000_ENABLE_SF7000
#include "../../src/SF7000.h"
#endif

namespace
{
MemEditor g_editors[GuiDebugMemoryEditorSlotCount];
struct ViewState
{
    std::string title;
    std::string source_type_key;
    std::string source_space_code;
    std::uint8_t* data = nullptr;
    int size = 0;
    int base_address = 0;
    std::uint64_t source_generation = 0;
    std::uint64_t source_layout_revision = 0;
};
std::array<ViewState, GuiDebugMemoryEditorSlotCount> g_views;
int g_requested_selection = -1;
int g_current_editor = GuiDebugMemoryEditorSlotPrimary;

bool valid_slot(int slot)
{
    return slot >= 0 && slot < GuiDebugMemoryEditorSlotCount;
}

void invalidate_changed_ram_views()
{
    const auto* bus = emu_get_core()->GetSystemBus();
    const auto regions = bus ? bus->GetRamRegions() : RamRegions{};
    for (int slot = GuiDebugMemoryEditorSlotInternalRam; slot <= GuiDebugMemoryEditorSlotCartridgeRam; ++slot)
    {
        const auto& view = g_views[slot];
        if (!view.data) continue;
        bool matches = false;
        for (const auto& region : regions)
            if (slot == GuiDebugMemoryEditorSlotInternalRam + static_cast<int>(region.role))
                matches = region.showInEditor && view.data == region.data &&
                    view.size == static_cast<int>(region.size) &&
                    view.base_address == static_cast<int>(region.displayBase) &&
                    view.source_generation == emu_get_machine_generation();
        if (!matches) {
            g_editors[slot].Invalidate(); g_views[slot] = {};
            if (g_current_editor == slot) g_current_editor = GuiDebugMemoryEditorSlotPrimary;
            if (g_requested_selection == slot) g_requested_selection = -1;
        }
    }
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
    invalidate_changed_ram_views();
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
    gui_set_next_window_size_em(32.5f, 45.3f);

    ImGui::Begin("Memory Editor", &config_debug.show_memory,
                 ImGuiWindowFlags_MenuBar);

    draw_menu_bar();

    GearSF7000Core* core = emu_get_core();
    Memory* memory = core->GetMemory();
    Cartridge* cartridge = core->GetCartridge();
    const VideoDeviceHandle video_handle = core->AcquireVideoDevice();
    IVideoDebugProvider* video_debug =
        core->IsVideoDeviceHandleCurrent(video_handle)
        ? video_handle.device->GetDebugProvider() : nullptr;
#if GEARSF7000_ENABLE_SF7000
    SF7000* sf7000 = core->GetSF7000();
#endif

    // The flat map has no ROM in the machine and no banking, so both the
    // "ROM" label and the BANK/ADDRESS pair would be stating zeroes about
    // something that does not exist. Only the mapper name is meaningful there.
    const bool flatMap = !memory->IsSF7000Enabled()
        && cartridge->GetType() == Cartridge::SC3000_FLAT64K;

    ImGui::PushFont(gui_default_font, gui_get_default_font_size());
    if (core->GetMachineType() == MachineType::MSX)
    {
        const auto& msx = *static_cast<const SystemBusMSX*>(core->GetSystemBus());
        char heading[64];
        snprintf(heading, sizeof(heading), "MSX1 slots $%02X###msx_slots", msx.Slots().GetPrimarySlots());
        if (ImGui::TreeNode(heading))
        {
            for (const auto& page : msx.GetPageMappings())
            {
                ImGui::Text("$%04X-%04X  slot %u", page.start, page.end, page.primary);
                if (page.expanded) { ImGui::SameLine(); ImGui::Text(".%u", page.secondary); }
                ImGui::SameLine();
                if (page.mapped)
                    ImGui::Text("%s +$%05X (%s)", page.device, page.offset, page.writable ? "RW" : "RO");
                else ImGui::TextUnformatted("empty ($FF)");
            }
            ImGui::TreePop();
        }
    }
    else if (const auto* m5 = core->GetM5Bus()) {
        if (ImGui::TreeNode(m5->EM5Installed() ? "Sord M5 + EM-5 map" : "Sord M5 standard map")) {
            for (const auto& map : m5->Mapper().GetMappings()) {
                const char* name = map.area == M5MemoryMapper::Area::Bios ? "BIOS"
                    : map.area == M5MemoryMapper::Area::Cartridge ? "Cartridge"
                    : map.area == M5MemoryMapper::Area::InternalRam ? "Internal RAM"
                    : map.area == M5MemoryMapper::Area::ExternalRam ? "EM-5 RAM" : "Open bus";
                ImGui::Text("$%04X-%04X %s (%u bytes)", map.start, map.end, name, map.backingSize);
            }
            ImGui::TreePop();
        }
    }
    else
    {
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
    }
    ImGui::PopFont();

    const auto draw_tab = [](const char* title, int slot, std::uint8_t* data,
                             int size, int base, ImFont* font,
                             std::uint64_t source_generation = 0,
                             const char* source_type_key = nullptr,
                             std::uint64_t source_layout_revision = 0,
                             const char* source_space_code = nullptr)
    {
        const ImGuiTabItemFlags flags = gui_debug_memory_should_select(slot)
            ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem(title, nullptr, flags))
        {
            gui_debug_memory_draw_view(slot, title, data, size, base, font, true,
                source_generation, source_type_key, source_layout_revision,
                source_space_code);
            ImGui::EndTabItem();
        }
    };

    if (ImGui::BeginTabBar("##memory_tabs", ImGuiTabBarFlags_None))
    {
        if (memory->GetMSXSlots())
        {
            draw_tab("BIOS", GuiDebugMemoryEditorSlotPrimary,
                     memory->GetBios(), memory->GetBiosSize(), 0, gui_default_font);
            auto& bus = *static_cast<SystemBusMSX*>(const_cast<ISystemBus*>(core->GetSystemBus()));
            if (!bus.LogoROM().empty())
                draw_tab("Logo ROM", GuiDebugMemoryEditorSlotMsxLogo,
                         bus.LogoROM().data(), bus.LogoROM().size(), 0, gui_default_font);
            if (!bus.BasicROM().empty())
                draw_tab("BASIC ROM", GuiDebugMemoryEditorSlotMsxBasic,
                         bus.BasicROM().data(), bus.BasicROM().size(), 0, gui_default_font);

            if (cartridge->IsReady())
                draw_tab("ROM IMAGE", GuiDebugMemoryEditorSlotTertiary,
                         cartridge->GetROM(), cartridge->GetROMSize(), 0, gui_default_font);
        }
        else if (memory->IsSF7000Enabled())
        {

            draw_tab("IPL ROM", GuiDebugMemoryEditorSlotPrimary,
                     memory->GetBios(), memory->GetBiosSize(), 0x0000, gui_default_font);

        }
        else if (cartridge->GetType() == Cartridge::SC3000_FLAT64K)
        {

            if (cartridge->GetROMSize() > 0)
                draw_tab("IMAGE", GuiDebugMemoryEditorSlotSecondary,
                         cartridge->GetROM(), cartridge->GetROMSize(), 0x0000,
                         gui_default_font);
        }
        else if (cartridge->GetType() == Cartridge::SC3000_ASC16L
              || cartridge->GetType() == Cartridge::SC3000_ASC16LS)
        {
            draw_tab("ROM FIXED", GuiDebugMemoryEditorSlotPrimary,
                     cartridge->GetROM(), 0x8000, 0x0000, gui_default_font);
            draw_tab("ROM PAGED", GuiDebugMemoryEditorSlotSecondary,
                     cartridge->GetROM() + memory->GetRomBankAddress(),
                     0x4000, 0x8000, gui_default_font);

        }
        else if (cartridge->GetType() == Cartridge::SC3000_SURVIVORS_MULTICART
              || cartridge->GetType() == Cartridge::SC3000_SURVIVORS_MEGACART)
        {

            u8* active = cartridge->GetROM() + memory->GetRomBankAddress();
            draw_tab("ROM $0000-$3FFF", GuiDebugMemoryEditorSlotPrimary,
                     active, 0x4000, 0x0000, gui_default_font);
            draw_tab("ROM $4000-$7FFF", GuiDebugMemoryEditorSlotSecondary,
                     active + 0x4000, 0x4000, 0x4000, gui_default_font);

        }
        else if (Cartridge::UsesSega3155208(cartridge->GetType()))
        {
            const u8 page2 = memory->GetRomBank();
            draw_tab("ROM FIXED $0000-$7FFF", GuiDebugMemoryEditorSlotPrimary,
                     cartridge->GetROM(), 0x8000, 0x0000, gui_default_font);
            draw_tab("ROM PAGED $8000-$BFFF", GuiDebugMemoryEditorSlotSecondary,
                     cartridge->GetROM() + (static_cast<u32>(page2) << 14),
                     0x4000, 0x8000, gui_default_font);
            draw_tab("ROM IMAGE", GuiDebugMemoryEditorSlotTertiary,
                     cartridge->GetROM(), cartridge->GetROMSize(), 0x0000,
                     gui_default_font);

        }
        else if (emu_get_core()->GetMachineType() == MachineType::Coleco || emu_get_core()->GetMachineType() == MachineType::SordM5)
        {

            if (memory->IsBiosLoaded())
                draw_tab("BIOS", GuiDebugMemoryEditorSlotPrimary,
                         memory->GetBios(), memory->GetBiosSize(), 0x0000, gui_default_font);

            draw_tab("ROM", GuiDebugMemoryEditorSlotTertiary,
                     cartridge->GetROM(), cartridge->GetROMSize(), core->GetM5Bus() ? 0x2000 : 0x8000,
                     gui_default_font);
        }
        else if (memory->UsesSMSWorkRam())
        {
            draw_tab("ROM IMAGE", GuiDebugMemoryEditorSlotPrimary,
                     cartridge->GetROM(), cartridge->GetROMSize(), 0x0000,
                     gui_default_font);

        }
        else
        {
            draw_tab("ROM", GuiDebugMemoryEditorSlotPrimary,
                     cartridge->GetROM(), cartridge->GetROMSize(), 0x0000,
                     gui_default_font);

        }

        if (core->GetSystemBus()) for (const auto& region : core->GetSystemBus()->GetRamRegions())
        {
            if (!region.showInEditor) continue;
            const std::string label = std::string(region.name) + " " +
                std::to_string(region.size / 1024) + "KB";
            const int slot = GuiDebugMemoryEditorSlotInternalRam + static_cast<int>(region.role);
            draw_tab(label.c_str(), slot, region.data, region.size, region.displayBase, gui_default_font,
                     emu_get_machine_generation(), region.key);
        }

        bool described_video_space = false;
        if (video_debug != nullptr)
        {
            const std::size_t space_count = video_debug->GetMemorySpaceCount();
            for (std::size_t index = 0; index < space_count; ++index)
            {
                GC_VideoMemorySpaceDescriptor space;
                if (!video_debug->DescribeMemorySpace(index, space) ||
                    space.direct_data == nullptr || space.installed_size_bytes == 0)
                    continue;

                described_video_space = true;

                const char* title = space.name != nullptr ? space.name :
                    (space.code != nullptr ? space.code : "Video Memory");
                ImGui::PushID(space.code != nullptr ? space.code : title);
                draw_tab(title, GuiDebugMemoryEditorSlotVram, space.direct_data,
                    static_cast<int>(space.installed_size_bytes), 0x0000,
                    gui_default_font, video_handle.generation,
                    video_handle.type_key.c_str(), space.layout_revision,
                    space.code);
                ImGui::PopID();
            }
        }
        if (!described_video_space &&
            g_views[GuiDebugMemoryEditorSlotVram].data != nullptr)
        {
            g_editors[GuiDebugMemoryEditorSlotVram].Invalidate();
            g_views[GuiDebugMemoryEditorSlotVram] = {};
        }
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
                                bool request_select, std::uint64_t source_generation,
                                const char* source_type_key,
                                std::uint64_t source_layout_revision,
                                const char* source_space_code)
{
    if (!valid_slot(slot) || data == nullptr || size <= 0)
        return;

    if (request_select && g_requested_selection == slot)
        g_requested_selection = -1;

    g_current_editor = slot;
    ViewState& view = g_views[slot];
    const char* safe_title = title != nullptr ? title : "";
    const char* safe_type_key = source_type_key != nullptr ? source_type_key : "";
    const char* safe_space_code = source_space_code != nullptr ? source_space_code : "";
    if (view.data != nullptr && (view.source_generation != source_generation ||
        view.source_type_key != safe_type_key ||
        view.source_space_code != safe_space_code ||
        view.source_layout_revision != source_layout_revision))
        g_editors[slot].Invalidate();
    if (view.data != data || view.size != size ||
        view.base_address != base_address ||
        view.source_generation != source_generation ||
        view.source_layout_revision != source_layout_revision ||
        view.source_type_key != safe_type_key ||
        view.source_space_code != safe_space_code || view.title != safe_title)
    {
        view.title = safe_title;
        view.source_type_key = safe_type_key;
        view.source_space_code = safe_space_code;
        view.data = data;
        view.size = size;
        view.base_address = base_address;
        view.source_generation = source_generation;
        view.source_layout_revision = source_layout_revision;
        g_editors[slot].Reset(view.title.c_str(), data, size, base_address);
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
    const float data_font_size = config_debug.memory_data_font == 0
        ? gui_get_default_font_size() : 16.0f;
    g_editors[slot].SetGuiFont(data_font, data_font_size);
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

// Physical RAM views use the same bus declarations as the editor and MCP.
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

    // Import physical regions declared by the bus, with the same bounds as
    // the editor. CPU windows and allocation capacity do not define sizes.
    if (cartridge->GetROMSize() > 0)
        add_view("ROM", cartridge->GetROM(), cartridge->GetROMSize(), 0x0000);
    if (core->GetSystemBus()) for (const auto& region : core->GetSystemBus()->GetRamRegions())
        if (region.showInEditor)
            add_view(region.name, region.data, region.size, region.displayBase);
    if (memory->GetBiosSize())
        add_view("BIOS / IPL", memory->GetBios(), memory->GetBiosSize(), 0x0000);

    if (core->GetMachineType() == MachineType::MSX) {
        auto& bus = *static_cast<SystemBusMSX*>(const_cast<ISystemBus*>(core->GetSystemBus()));
        if (!bus.LogoROM().empty()) add_view("Logo ROM", bus.LogoROM().data(), bus.LogoROM().size(), 0);
        if (!bus.BasicROM().empty()) add_view("BASIC ROM", bus.BasicROM().data(), bus.BasicROM().size(), 0);
    }
    return count;
}

// Search, Find Bytes and Watches each open their own top-level window, so they
// cannot be drawn inside the Memory Editor window - they are pumped alongside
// it. Unconditional and cheap: each returns immediately unless its editor has
// been asked to open it.
void gui_debug_memory_search_window(void)
{
    invalidate_changed_ram_views();
    for (int i = 0; i < GuiDebugMemoryEditorSlotCount; i++)
    {
        ImGui::PushFont(gui_default_font, gui_get_default_font_size());
        g_editors[i].DrawSearchWindow();
        ImGui::PopFont();
    }
}

void gui_debug_memory_find_bytes_window(void)
{
    invalidate_changed_ram_views();
    for (int i = 0; i < GuiDebugMemoryEditorSlotCount; i++)
    {
        ImGui::PushFont(gui_default_font, gui_get_default_font_size());
        g_editors[i].DrawFindBytesWindow();
        ImGui::PopFont();
    }
}

void gui_debug_memory_watches_window(void)
{
    invalidate_changed_ram_views();
    for (int i = 0; i < GuiDebugMemoryEditorSlotCount; i++)
    {
        ImGui::PushFont(gui_default_font, gui_get_default_font_size());
        g_editors[i].DrawWatchWindow();
        ImGui::PopFont();
    }
}
