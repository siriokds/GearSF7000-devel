/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gui_debug_mem_import.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/colors.h"
#include "nfd_parented.h"
#include "config.h"
#include "emu.h"
#include "gui.h"
#include "gui_debug_memory.h"
#include "../../src/gearsf7000.h"
#include "../../src/IVideoDevice.h"
#include "../../src/IVideoDebugProvider.h"

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
            ? NFD_SaveDialogParented(&out_path, nullptr, 0, nullptr, nullptr)
            : NFD_OpenDialogParented(&out_path, nullptr, 0, nullptr);
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

bool read_file(const char* path, std::size_t maximumSize, std::vector<u8>& bytes)
{
    bytes.clear();
    if (!path || !path[0] || maximumSize == 0)
        return false;
    FILE* file = fopen(path, "rb");
    if (!file)
        return false;
    bytes.resize(maximumSize);
    const std::size_t count = fread(bytes.data(), 1, maximumSize, file);
    fclose(file);
    bytes.resize(count);
    return count != 0;
}

}

void gui_debug_mem_import_window(void)
{
    static char ram_import_path[1024] = "";
    static char ram_import_address[9] = "";
    static char ram_export_path[1024] = "";
    static char ram_export_address[9] = "";
    static char ram_export_length[9] = "";
    static char video_import_path[1024] = "";
    static char video_import_address[9] = "";
    static char video_export_path[1024] = "";
    static char video_export_address[9] = "";
    static char video_export_end[9] = "";
    static std::string selected_video_space;
    static int selected_view = 0;

    ImGui::SetNextWindowPos(ImVec2(160, 300), ImGuiCond_FirstUseEver);
    gui_set_next_window_size_em(38.9f, 25.6f);

    ImGui::Begin("Mem Import", &config_debug.show_memory_import);
    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

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
        float target_combo_width = 0.0f;
        for (int i = 0; i < view_count; ++i)
            target_combo_width = std::max(target_combo_width,
                gui_combo_width_for_text(views[i].title));
        ImGui::SetNextItemWidth(target_combo_width);
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
    ImGui::TextColored(yellow, "VIDEO MEMORY");

    GearSF7000Core* core = emu_get_core();
    const VideoDeviceHandle videoHandle = core->AcquireVideoDevice();
    IVideoDebugProvider* videoDebug = core->IsVideoDeviceHandleCurrent(videoHandle)
        ? videoHandle.device->GetDebugProvider() : nullptr;
    std::vector<GC_VideoMemorySpaceDescriptor> videoSpaces;
    if (videoDebug)
        for (std::size_t index = 0; index < videoDebug->GetMemorySpaceCount(); ++index)
        {
            GC_VideoMemorySpaceDescriptor space;
            if (videoDebug->DescribeMemorySpace(index, space) && space.code &&
                space.installed_size_bytes != 0 && (space.readable || space.writable))
                videoSpaces.push_back(space);
        }

    auto selectedSpace = videoSpaces.end();
    if (!selected_video_space.empty())
        selectedSpace = std::find_if(videoSpaces.begin(), videoSpaces.end(),
            [&](const GC_VideoMemorySpaceDescriptor& space)
            {
                return selected_video_space == space.code;
            });
    if (selectedSpace == videoSpaces.end() && !videoSpaces.empty())
    {
        selectedSpace = videoSpaces.begin();
        selected_video_space = selectedSpace->code;
    }

    if (selectedSpace == videoSpaces.end())
        ImGui::TextDisabled("The active video device exposes no memory space.");
    else
    {
        const char* selectedName = selectedSpace->name ? selectedSpace->name : selectedSpace->code;
        float comboWidth = gui_combo_width_for_text(selectedName);
        for (const GC_VideoMemorySpaceDescriptor& space : videoSpaces)
            comboWidth = std::max(comboWidth,
                gui_combo_width_for_text(space.name ? space.name : space.code));
        ImGui::SetNextItemWidth(comboWidth);
        if (ImGui::BeginCombo("Target##video_memory", selectedName))
        {
            for (const GC_VideoMemorySpaceDescriptor& space : videoSpaces)
            {
                const bool selected = selected_video_space == space.code;
                if (ImGui::Selectable(space.name ? space.name : space.code, selected))
                    selected_video_space = space.code;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextColored(cyan, "Size:"); ImGui::SameLine();
        ImGui::Text("$%X (%u bytes)", selectedSpace->installed_size_bytes,
            selectedSpace->installed_size_bytes);

        ImGui::Spacing();
        ImGui::TextColored(magenta, "FILE -> VIDEO MEMORY");
        path_field("##video_import_path", video_import_path,
            IM_ARRAYSIZE(video_import_path), false);
        ImGui::Text("Offset:"); ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::InputTextWithHint("##video_import_address", "XXXXXXXX",
            video_import_address, IM_ARRAYSIZE(video_import_address),
            ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        if (selectedSpace->writable && ImGui::Button("Import##video", ImVec2(90, 0)))
        {
            try
            {
                u32 offset = video_import_address[0]
                    ? static_cast<u32>(std::stoul(video_import_address, nullptr, 16)) : 0;
                if (offset < selectedSpace->installed_size_bytes)
                {
                    std::vector<u8> bytes;
                    if (read_file(video_import_path,
                            selectedSpace->installed_size_bytes - offset, bytes))
                        videoDebug->WriteMemorySpace(selectedSpace->code, offset,
                            bytes.data(), bytes.size());
                }
            }
            catch (const std::invalid_argument&) {}
            catch (const std::out_of_range&) {}
        }
        else if (!selectedSpace->writable)
            ImGui::TextDisabled("Read-only");

        ImGui::Spacing();
        ImGui::TextColored(magenta, "VIDEO MEMORY -> FILE");
        path_field("##video_export_path", video_export_path,
            IM_ARRAYSIZE(video_export_path), true);
        ImGui::Text("Address:"); ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::InputTextWithHint("##video_export_address", "XXXXXXXX",
            video_export_address, IM_ARRAYSIZE(video_export_address),
            ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        ImGui::Text("End:"); ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::InputTextWithHint("##video_export_end", "XXXXXXXX",
            video_export_end, IM_ARRAYSIZE(video_export_end),
            ImGuiInputTextFlags_CharsHexadecimal);
        if (selectedSpace->readable && ImGui::Button("Export##video", ImVec2(90, 0)))
        {
            try
            {
                u32 first = video_export_address[0]
                    ? static_cast<u32>(std::stoul(video_export_address, nullptr, 16)) : 0;
                u32 last = video_export_end[0]
                    ? static_cast<u32>(std::stoul(video_export_end, nullptr, 16))
                    : selectedSpace->installed_size_bytes - 1;
                if (first > last)
                    std::swap(first, last);
                if (first < selectedSpace->installed_size_bytes)
                {
                    last = std::min(last, selectedSpace->installed_size_bytes - 1);
                    std::vector<u8> bytes(static_cast<std::size_t>(last - first + 1));
                    if (videoDebug->ReadMemorySpace(selectedSpace->code, first,
                            bytes.data(), bytes.size()))
                        MemoryExport(video_export_path, bytes.data(), 0,
                            static_cast<int>(bytes.size()));
                }
            }
            catch (const std::invalid_argument&) {}
            catch (const std::out_of_range&) {}
        }
        else if (!selectedSpace->readable)
            ImGui::TextDisabled("Write-only");
    }

    ImGui::PopFont();
    ImGui::End();
}
