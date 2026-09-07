/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026 Saverio Russo
 */

#include "gui_debug_mem_import.h"

#include <cstring>
#include <string>
#include <utility>

#include "imgui/imgui.h"
#include "imgui/colors.h"
#include "nfd/nfd.h"
#include "config.h"
#include "emu.h"
#include "gui.h"
#include "gui_debug_memory.h"
#include "../../src/gearsf7000.h"

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

void browse_button(const char* id, char* path_buf, size_t path_buf_size, bool save)
{
    ImGui::PushID(id);
    if (ImGui::Button("Browse..."))
    {
        ScopedAudioPause audioPause;
        nfdchar_t* out_path = nullptr;
        nfdresult_t result = save
            ? NFD_SaveDialog(&out_path, nullptr, 0, nullptr, nullptr)
            : NFD_OpenDialog(&out_path, nullptr, 0, nullptr);
        if (result == NFD_OKAY)
        {
            strncpy(path_buf, out_path, path_buf_size - 1);
            path_buf[path_buf_size - 1] = 0;
            NFD_FreePath(out_path);
        }
        else if (result != NFD_CANCEL)
        {
            Log("Mem Import Browse Error: %s", NFD_GetError());
        }
    }
    ImGui::PopID();
}

// One text field, meant to be reused with a click: the user edits the
// target file externally, then clicks the action button again with the
// same path already typed - see gui_debug_mem_import.h's own comment on
// why this isn't a picker-only flow.
void path_field(const char* label, char* buf, size_t buf_size, bool save)
{
    ImGui::SetNextItemWidth(560);
    ImGui::InputTextWithHint(label, "Full path", buf, buf_size);
    ImGui::SameLine();
    browse_button(label, buf, buf_size, save);
}

}

void gui_debug_mem_import_window(void)
{
    static char ram_import_path[1024] = "";
    static char ram_import_address[9] = "";
    static char ram_export_path[1024] = "";
    static char ram_export_address[9] = "";
    static char ram_export_length[9] = "";
    static char vram_import_path[1024] = "";
    static char vram_export_path[1024] = "";
    static char vram_export_address[5] = "";
    static char vram_export_end[5] = "";
    static int selected_view = 0;

    ImGui::SetNextWindowPos(ImVec2(160, 300), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(700, 460), ImGuiCond_FirstUseEver);

    ImGui::Begin("Mem Import", &config_debug.show_memory_import);
    ImGui::PushFont(gui_default_font);

    // RAM/ROM section - view picker instead of one flat 64 KiB buffer: what
    // buffers actually exist (and at what CPU base) depends on the active
    // machine profile (SF-7000 vs SC-3000 cartridge type), same list the
    // Memory Editor's own tabs show. See gui_debug_memory.h.
    GuiDebugMemoryView views[GuiDebugMemoryEditorSlotCount];
    int view_count = gui_debug_memory_get_current_views(views, GuiDebugMemoryEditorSlotCount);
    if (selected_view >= view_count)
        selected_view = 0;

    ImGui::TextColored(yellow, "RAM / ROM");

    if (view_count == 0)
    {
        ImGui::TextWrapped("No RAM/ROM buffer available - load a ROM first.");
    }
    else
    {
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo("Target", views[selected_view].title))
        {
            for (int i = 0; i < view_count; i++)
            {
                bool is_selected = (i == selected_view);
                if (ImGui::Selectable(views[i].title, is_selected))
                    selected_view = i;
                if (is_selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        // Raw internal buffer, not a CPU-address view - no mapper/bank is
        // consulted, so there is no "base address" to show, only a size.
        ImGui::TextColored(cyan, "  Size: "); ImGui::SameLine();
        ImGui::Text("$%X (%d bytes)", views[selected_view].size, views[selected_view].size);

        const GuiDebugMemoryView& view = views[selected_view];

        ImGui::Spacing();
        ImGui::TextColored(magenta, "FILE -> RAM/ROM");
        path_field("##ram_import_path", ram_import_path, IM_ARRAYSIZE(ram_import_path), false);
        ImGui::Text("Offset:"); ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::InputTextWithHint("##ram_import_address", "XXXXXXXX", ram_import_address, IM_ARRAYSIZE(ram_import_address),
            ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        if (ImGui::Button("Import##ram", ImVec2(90, 0)))
        {
            try
            {
                u32 offset = ram_import_address[0] ? (u32)std::stoul(ram_import_address, 0, 16) : 0;
                MemoryImport(ram_import_path, view.data, offset, view.size);
            }
            catch (const std::invalid_argument&)
            {
            }
        }

        ImGui::Spacing();
        ImGui::TextColored(magenta, "RAM/ROM -> FILE");
        path_field("##ram_export_path", ram_export_path, IM_ARRAYSIZE(ram_export_path), true);
        ImGui::Text("Offset:"); ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::InputTextWithHint("##ram_export_address", "XXXXXXXX", ram_export_address, IM_ARRAYSIZE(ram_export_address),
            ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        ImGui::Text("  Length:"); ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::InputTextWithHint("##ram_export_length", "XXXXXXXX", ram_export_length, IM_ARRAYSIZE(ram_export_length),
            ImGuiInputTextFlags_CharsHexadecimal);
        if (ImGui::Button("Export##ram", ImVec2(90, 0)))
        {
            try
            {
                u32 offset = ram_export_address[0] ? (u32)std::stoul(ram_export_address, 0, 16) : 0;
                u32 len = ram_export_length[0] ? (u32)std::stoul(ram_export_length, 0, 16) : (u32)view.size;
                if (offset >= (u32)view.size)
                    offset = 0;
                if (offset + len > (u32)view.size)
                    len = (u32)view.size - offset;
                MemoryExport(ram_export_path, view.data, offset, (int)len);
            }
            catch (const std::invalid_argument&)
            {
            }
        }
    }

    ImGui::Separator();
    ImGui::TextColored(yellow, "VRAM");

    Video* video = emu_get_core()->GetVideo();

    ImGui::Spacing();
    ImGui::TextColored(magenta, "FILE -> VRAM");
    path_field("##vram_import_path", vram_import_path, IM_ARRAYSIZE(vram_import_path), false);
    if (ImGui::Button("Import##vram", ImVec2(90, 0)))
    {
        MemoryImport(vram_import_path, video->GetVRAM(), 0, 0x4000);
    }

    ImGui::Spacing();
    ImGui::TextColored(magenta, "VRAM -> FILE");
    path_field("##vram_export_path", vram_export_path, IM_ARRAYSIZE(vram_export_path), true);
    ImGui::Text("Address:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(50);
    ImGui::InputTextWithHint("##vram_export_address", "XXXX", vram_export_address, IM_ARRAYSIZE(vram_export_address),
        ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::Text("  End:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(50);
    ImGui::InputTextWithHint("##vram_export_end", "XXXX", vram_export_end, IM_ARRAYSIZE(vram_export_end),
        ImGuiInputTextFlags_CharsHexadecimal);
    if (ImGui::Button("Export##vram", ImVec2(90, 0)))
    {
        try
        {
            u16 addr = vram_export_address[0] ? (u16)std::stoul(vram_export_address, 0, 16) : 0;
            u16 end = vram_export_end[0] ? (u16)std::stoul(vram_export_end, 0, 16) : 0x3FFF;
            addr &= 0x3FFF;
            end &= 0x3FFF;
            if (addr > end)
                std::swap(addr, end);
            MemoryExport(vram_export_path, video->GetVRAM(), addr, (end - addr) + 1);
        }
        catch (const std::invalid_argument&)
        {
        }
    }

    ImGui::PopFont();
    ImGui::End();
}
