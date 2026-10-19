/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026 Saverio Russo
 *
 * Generic Pattern Table viewer. The active video device owns tile decoding,
 * source areas and tile details; this panel only draws the published surface.
 */

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/colors.h"
#include "config.h"
#include "emu.h"
#include "gui.h"
#include "gui_debug_memory.h"
#include "gui_debug_pattern_table.h"
#include "renderer.h"
#include "nfd_parented.h"
#include "../../src/IVideoDebugProvider.h"
#include "../../src/IVideoDevice.h"

namespace
{
struct ScopedDebugFileDialogAudioPause
{
    ScopedDebugFileDialogAudioPause() { emu_audio_suspend_for_file_dialog(); }
    ~ScopedDebugFileDialogAudioPause() { emu_audio_resume_after_file_dialog(); }
    ScopedDebugFileDialogAudioPause(const ScopedDebugFileDialogAudioPause&) = delete;
    ScopedDebugFileDialogAudioPause& operator=(const ScopedDebugFileDialogAudioPause&) = delete;
};

void save_catalog_png_with_dialog(const GC_VideoDebugLayerSurface& surface)
{
    std::vector<u8> rgb(static_cast<std::size_t>(surface.width) * surface.height * 3);
    for (u32 row = 0; row < surface.height; ++row)
        for (u32 column = 0; column < surface.width; ++column)
        {
            const u8* rgba = surface.rgba + row * surface.row_pitch + column * 4;
            u8* destination = &rgb[(row * surface.width + column) * 3];
            destination[0] = rgba[0];
            destination[1] = rgba[1];
            destination[2] = rgba[2];
        }
    ScopedDebugFileDialogAudioPause audioPause;
    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filter[1] = { { "PNG Files", "png" } };
    const nfdresult_t result = NFD_SaveDialogParented(&outPath, filter, 1, nullptr, "pattern_table.png");
    if (result == NFD_OKAY)
    {
        emu_save_debug_png(outPath, rgb.data(), static_cast<int>(surface.width),
                           static_cast<int>(surface.height), static_cast<int>(surface.width * 3));
        NFD_FreePath(outPath);
    }
    else if (result != NFD_CANCEL)
        Log("Save PNG Error: %s", NFD_GetError());
}

void area_scale_control(const char* label, float* scale)
{
    static const float steps[] = { 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f };
    char preview[16];
    snprintf(preview, sizeof(preview), "%.1fx", static_cast<double>(*scale));
    ImGui::SetNextItemWidth(gui_combo_width_for_text("4.0x"));
    if (ImGui::BeginCombo(label, preview))
    {
        for (float step : steps)
        {
            char item[16];
            snprintf(item, sizeof(item), "%.1fx", static_cast<double>(step));
            if (ImGui::Selectable(item, *scale == step))
                *scale = step;
        }
        ImGui::EndCombo();
    }
}

void draw_address(const char* label, u64 address, u8 widthBits)
{
    const int digits = std::max(1, static_cast<int>((widthBits + 3) / 4));
    ImGui::TextColored(cyan, "%s", label);
    ImGui::SameLine();
    ImGui::Text("$%0*llX", digits, static_cast<unsigned long long>(address));
}

void draw_field(const GC_VideoDebugGridCellFieldDescriptor& field)
{
    ImGui::TextColored(cyan, "%s", field.label ? field.label : field.code);
    ImGui::SameLine();
    if (field.has_source_address)
    {
        const int addressDigits = std::max(1,
            static_cast<int>((field.source_address_width_bits + 3) / 4));
        const int valueDigits = std::max(1, static_cast<int>((field.width_bits + 3) / 4));
        ImGui::Text("$%0*llX: $%0*llX", addressDigits,
            static_cast<unsigned long long>(field.source_address), valueDigits,
            static_cast<unsigned long long>(field.value));
    }
    else if (field.format == GC_VideoDebugValueFormat::Text)
        ImGui::TextUnformatted(field.text ? field.text : "");
    else if (field.format == GC_VideoDebugValueFormat::Address)
    {
        const int digits = std::max(1, static_cast<int>((field.width_bits + 3) / 4));
        ImGui::Text("$%0*llX", digits, static_cast<unsigned long long>(field.value));
    }
    else
        ImGui::Text("$%llX", static_cast<unsigned long long>(field.value));
}

bool layer_option_disabled(const char* code)
{
    if (!code || !code[0])
        return false;
    const std::string token = std::string("|") + code + "|";
    return config_debug.vram_pattern_disabled_layer_options.find(token) !=
        std::string::npos;
}

void set_layer_option_disabled(const char* code, bool disabled)
{
    if (!code || !code[0])
        return;
    const std::string token = std::string("|") + code + "|";
    const std::size_t position =
        config_debug.vram_pattern_disabled_layer_options.find(token);
    if (disabled && position == std::string::npos)
        config_debug.vram_pattern_disabled_layer_options += token;
    else if (!disabled && position != std::string::npos)
        config_debug.vram_pattern_disabled_layer_options.erase(position, token.size());
}
}

static void draw_pattern_catalog(IVideoDebugProvider* provider,
    const VideoDeviceHandle& handle, std::size_t layerIndex,
    const GC_VideoDebugLayerDescriptor& layer)
{
    if (layer.columns == 0 || layer.rows == 0 ||
        layer.cell_width == 0 || layer.cell_height == 0)
    {
        ImGui::TextDisabled("The active video device exposes no tile catalogue.");
        return;
    }

    bool& showGrid = config_debug.show_grid_tiles;
    float& scale = config_debug.vram_tiles_area_scale;
    const int columns = static_cast<int>(layer.columns);
    const int rows = static_cast<int>(layer.rows);
    const float width = static_cast<float>(layer.content_width) * scale;
    const float height = static_cast<float>(layer.content_height) * scale;
    const float spacingX = static_cast<float>(layer.cell_width) * scale;
    const float spacingY = static_cast<float>(layer.cell_height) * scale;
    const u32 activeRows = std::min(layer.rows,
        (layer.active_cell_count + layer.columns - 1) / layer.columns);
    const float activeHeight = static_cast<float>(activeRows) * spacingY;

    ImGui::Checkbox("Show Grid##pattern_grid", &showGrid);
    ImGui::SameLine();
    area_scale_control("Scale##pattern_scale", &scale);

    u64 optionMask = 0;
    const std::size_t optionCount = std::min<std::size_t>(
        provider->GetDebugLayerOptionCount(layerIndex), 64);
    for (std::size_t optionIndex = 0; optionIndex < optionCount; ++optionIndex)
    {
        GC_VideoDebugLayerOptionDescriptor option;
        if (!provider->DescribeDebugLayerOption(layerIndex, optionIndex, option) ||
            !option.code || !option.name)
            continue;
        bool enabled = option.enabled && !layer_option_disabled(option.code);
        ImGui::SameLine();
        ImGui::PushID(option.code);
        if (ImGui::Checkbox(option.name, &enabled))
            set_layer_option_disabled(option.code, !enabled);
        ImGui::PopID();
        if (enabled)
            optionMask |= 1ull << optionIndex;
    }

    GC_VideoDebugLayerSurface surface;
    if (!provider->ReadDebugLayerSurfaceWithOptions(layerIndex, optionMask, surface) ||
        !surface.rgba)
    {
        ImGui::TextDisabled("The active video device exposes no tile catalogue.");
        return;
    }
    const ImTextureID texture = renderer_get_debug_layer_texture(handle.generation,
        layer.code, optionMask, surface.generation);
    if (texture == ImTextureID_Invalid)
    {
        ImGui::TextDisabled("Waiting for the Pattern Table texture.");
        return;
    }

    ImGui::PushFont(gui_default_font, gui_get_default_font_size());
    ImGui::Columns(2, "pattern_table", false);
    ImGui::SetColumnOffset(1, width + 10.0f);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    renderer_begin_emulator_image();
    ImGui::Image(texture, ImVec2(width, height));
    renderer_end_emulator_image();
    if (ImGui::BeginPopupContextItem("##pattern_table_context"))
    {
        if (ImGui::Selectable("Save Pattern Table As PNG..."))
            save_catalog_png_with_dialog(surface);
        ImGui::EndPopup();
    }

    if (showGrid)
    {
        for (int column = 0; column <= columns; ++column)
            drawList->AddLine(ImVec2(origin.x + column * spacingX, origin.y),
                              ImVec2(origin.x + column * spacingX, origin.y + activeHeight),
                              ImColor(grid_line), 1.0f);
        for (u32 row = 0; row <= activeRows; ++row)
            drawList->AddLine(ImVec2(origin.x, origin.y + row * spacingY),
                              ImVec2(origin.x + width, origin.y + row * spacingY),
                              ImColor(grid_line), 1.0f);
    }

    // These belong to the catalogue column. Keeping them here prevents the
    // taller hover preview in the detail column from moving the footer.
    ImGui::TextColored(cyan, "Tiles:");
    ImGui::SameLine();
    ImGui::Text("%u", layer.active_cell_count);
    for (std::size_t areaIndex = 0; areaIndex < provider->GetDebugLayerAreaCount(layerIndex); ++areaIndex)
    {
        GC_VideoDebugLayerAreaDescriptor area;
        if (!provider->DescribeDebugLayerArea(layerIndex, areaIndex, area) || !area.label)
            continue;
        char label[96];
        snprintf(label, sizeof(label), "%s:", area.label);
        draw_address(label, area.address, area.address_width_bits);
        if (area.size > 1)
        {
            ImGui::SameLine();
            const int digits = std::max(1, static_cast<int>((area.address_width_bits + 3) / 4));
            ImGui::TextDisabled("- $%0*X  (%u bytes)", digits,
                area.address + area.size - 1, area.size);
        }
    }

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float mouseX = mouse.x - origin.x;
    const float mouseY = mouse.y - origin.y;
    if (ImGui::IsWindowHovered() && mouseX >= 0.0f && mouseX < width &&
        mouseY >= 0.0f && mouseY < height)
    {
        const int column = static_cast<int>(mouseX / spacingX);
        const int row = static_cast<int>(mouseY / spacingY);
        GC_VideoDebugGridCell cell;
        if (provider->DescribeDebugGridCell(layerIndex, static_cast<u32>(column),
                static_cast<u32>(row), cell) && cell.valid)
        {
            drawList->AddRect(ImVec2(origin.x + column * spacingX, origin.y + row * spacingY),
                              ImVec2(origin.x + (column + 1) * spacingX,
                                     origin.y + (row + 1) * spacingY),
                              ImColor(cyan), 2.0f, ImDrawFlags_RoundCornersAll, 2.0f);
            ImGui::NextColumn();
            renderer_begin_emulator_image();
            ImGui::Image(texture, ImVec2(128.0f, 128.0f),
                ImVec2(static_cast<float>(column) / columns, static_cast<float>(row) / rows),
                ImVec2(static_cast<float>(column + 1) / columns,
                       static_cast<float>(row + 1) / rows));
            renderer_end_emulator_image();
            ImGui::TextColored(yellow, "DETAILS:");
            const std::size_t fieldCount = provider->GetDebugGridCellFieldCount(
                layerIndex, static_cast<u32>(column), static_cast<u32>(row));
            for (std::size_t fieldIndex = 0; fieldIndex < fieldCount; ++fieldIndex)
            {
                GC_VideoDebugGridCellFieldDescriptor field;
                if (provider->DescribeDebugGridCellField(layerIndex,
                        static_cast<u32>(column), static_cast<u32>(row), fieldIndex, field))
                    draw_field(field);
            }
            if (ImGui::IsMouseClicked(0))
                gui_debug_memory_goto(GuiDebugMemoryEditorSlotVram, cell.navigation_address);
        }
    }

    ImGui::Columns(1);
    ImGui::PopFont();
}

static void draw_pattern_table_contents(void)
{
    GearSF7000Core* core = emu_get_core();
    const VideoDeviceHandle handle = core->AcquireVideoDevice();
    IVideoDebugProvider* provider = core->IsVideoDeviceHandleCurrent(handle)
        ? handle.device->GetDebugProvider() : nullptr;
    if (!provider)
    {
        ImGui::TextDisabled("The active video device exposes no Pattern Table.");
        return;
    }

    struct Catalog
    {
        std::size_t index;
        GC_VideoDebugLayerDescriptor descriptor;
    };
    std::vector<Catalog> catalogs;
    for (std::size_t index = 0; index < provider->GetDebugLayerCount(); ++index)
    {
        GC_VideoDebugLayerDescriptor descriptor;
        if (provider->DescribeDebugLayer(index, descriptor) && descriptor.code &&
            descriptor.kind == GC_VideoDebugLayerKind::TileCatalog)
            catalogs.push_back({ index, descriptor });
    }

    if (catalogs.empty())
    {
        ImGui::TextDisabled("The active video device exposes no tile catalogue.");
        return;
    }
    if (catalogs.size() == 1)
    {
        draw_pattern_catalog(provider, handle, catalogs[0].index, catalogs[0].descriptor);
        return;
    }

    if (ImGui::BeginTabBar("##pattern_catalogs"))
    {
        for (const Catalog& catalog : catalogs)
        {
            const char* name = catalog.descriptor.name ? catalog.descriptor.name : catalog.descriptor.code;
            const std::string label = std::string(name) + "###" + catalog.descriptor.code;
            if (ImGui::BeginTabItem(label.c_str()))
            {
                draw_pattern_catalog(provider, handle, catalog.index, catalog.descriptor);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
}

void debug_window_pattern_table(void)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::SetNextWindowPos(ImVec2(896, 31), ImGuiCond_FirstUseEver);
    gui_set_next_window_size_em(21.4f, 15.8f);

    if (ImGui::Begin("Pattern Table", &config_debug.show_pattern_table))
        draw_pattern_table_contents();

    ImGui::End();
    ImGui::PopStyleVar();
}
