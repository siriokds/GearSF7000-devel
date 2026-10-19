/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026 Saverio Russo
 *
 * Generic video-device Name Table viewer. The active device publishes layer
 * geometry, RGBA surfaces and selected-cell fields through IVideoDebugProvider.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "imgui/imgui.h"
#include "imgui/colors.h"
#include "config.h"
#include "emu.h"
#include "renderer.h"
#include "gui.h"
#include "gui_debug_memory.h"
#include "gui_debug_nametable.h"
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

void save_layer_png_with_dialog(const GC_VideoDebugLayerSurface& surface,
                                int width, int height)
{
    std::vector<u8> rgb(static_cast<std::size_t>(width * height * 3));
    for (int row = 0; row < height; ++row)
        for (int column = 0; column < width; ++column)
        {
            const u8* pixel = surface.rgba + row * surface.row_pitch + column * 4;
            u8* output = &rgb[(row * width + column) * 3];
            output[0] = pixel[0];
            output[1] = pixel[1];
            output[2] = pixel[2];
        }
    ScopedDebugFileDialogAudioPause audioPause;
    nfdchar_t* out_path = nullptr;
    nfdfilteritem_t filter[1] = { { "PNG Files", "png" } };
    nfdresult_t result = NFD_SaveDialogParented(&out_path, filter, 1, nullptr, "nametable.png");
    if (result == NFD_OKAY)
    {
        emu_save_debug_png(out_path, rgb.data(), width, height, width * 3);
        NFD_FreePath(out_path);
    }
    else if (result != NFD_CANCEL)
    {
        Log("Save PNG Error: %s", NFD_GetError());
    }
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

void debug_address(const char* label, u64 address, u8 widthBits)
{
    const int digits = std::max(1, static_cast<int>((widthBits + 3) / 4));
    ImGui::TextColored(cyan, "%s", label);
    ImGui::SameLine();
    ImGui::Text("$%0*llX", digits, static_cast<unsigned long long>(address));
}

void debug_cell_field(const GC_VideoDebugGridCellFieldDescriptor& field)
{
    if (!field.label)
        return;
    if (field.format == GC_VideoDebugValueFormat::Address ||
        field.format == GC_VideoDebugValueFormat::Hexadecimal)
    {
        debug_address(field.label, field.value, field.width_bits);
        return;
    }

    ImGui::TextColored(cyan, "%s", field.label);
    ImGui::SameLine();
    switch (field.format)
    {
        case GC_VideoDebugValueFormat::Boolean:
            ImGui::TextUnformatted(field.value ? "YES" : "NO");
            break;
        case GC_VideoDebugValueFormat::Decimal:
            ImGui::Text("%llu", static_cast<unsigned long long>(field.value));
            break;
        case GC_VideoDebugValueFormat::Text:
            ImGui::TextUnformatted(field.text ? field.text : "");
            break;
        default:
            break;
    }
}

void draw_wrapped_viewport(ImDrawList* draw_list, const ImVec2& origin,
                           float content_width, float content_height,
                           float viewport_x, float viewport_y,
                           float viewport_width, float viewport_height)
{
    if (content_width <= 0.0f || content_height <= 0.0f ||
        viewport_width <= 0.0f || viewport_height <= 0.0f)
        return;

    const ImU32 colour = ImColor(0.20f, 1.0f, 0.35f, 0.62f);
    const float x0 = fmodf(viewport_x, content_width);
    const float y0 = fmodf(viewport_y, content_height);
    const float viewport_end_x = x0 + viewport_width;
    const float viewport_end_y = y0 + viewport_height;
    const float x1 = viewport_end_x <= content_width
        ? viewport_end_x : fmodf(viewport_end_x, content_width);
    const float y1 = viewport_end_y <= content_height
        ? viewport_end_y : fmodf(viewport_end_y, content_height);
    const float right = origin.x + content_width;
    const float bottom = origin.y + content_height;
    const float left_edge = origin.x + x0;
    const float right_edge = origin.x + x1;
    const float top_edge = origin.y + y0;
    const float bottom_edge = origin.y + y1;

    // Each edge wraps independently on the toroidal map. When a viewport is
    // as large as the map, opposite edges coincide and mark its scroll origin.
    const auto horizontal = [&](float y)
    {
        const float end = x0 + viewport_width;
        if (end <= content_width)
            draw_list->AddLine(ImVec2(left_edge, y), ImVec2(origin.x + end, y), colour, 2.0f);
        else
        {
            draw_list->AddLine(ImVec2(left_edge, y), ImVec2(right, y), colour, 2.0f);
            draw_list->AddLine(ImVec2(origin.x, y),
                               ImVec2(origin.x + fmodf(end, content_width), y), colour, 2.0f);
        }
    };
    const auto vertical = [&](float x)
    {
        const float end = y0 + viewport_height;
        if (end <= content_height)
            draw_list->AddLine(ImVec2(x, top_edge), ImVec2(x, origin.y + end), colour, 2.0f);
        else
        {
            draw_list->AddLine(ImVec2(x, top_edge), ImVec2(x, bottom), colour, 2.0f);
            draw_list->AddLine(ImVec2(x, origin.y),
                               ImVec2(x, origin.y + fmodf(end, content_height)), colour, 2.0f);
        }
    };

    horizontal(top_edge);
    horizontal(bottom_edge);
    vertical(left_edge);
    vertical(right_edge);
}
}

void debug_window_nametable(void)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::SetNextWindowPos(ImVec2(896, 31), ImGuiCond_FirstUseEver);
    gui_set_next_window_size_em(37.1f, 35.6f);
    if (!ImGui::Begin("Name Table", &config_debug.show_nametable))
    {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    GearSF7000Core* core = emu_get_core();
    const VideoDeviceHandle handle = core->AcquireVideoDevice();
    IVideoDebugProvider* provider = core->IsVideoDeviceHandleCurrent(handle)
        ? handle.device->GetDebugProvider() : nullptr;
    std::vector<std::size_t> layer_indices;
    if (provider)
        for (std::size_t index = 0; index < provider->GetDebugLayerCount(); ++index)
        {
            GC_VideoDebugLayerDescriptor candidate;
            if (provider->DescribeDebugLayer(index, candidate) &&
                candidate.kind == GC_VideoDebugLayerKind::TileGrid)
                layer_indices.push_back(index);
        }
    const std::size_t layer_count = layer_indices.size();
    static u64 selected_device_generation = 0;
    static std::size_t selected_layer_index = 0;
    if (selected_device_generation != handle.generation || selected_layer_index >= layer_count)
    {
        selected_device_generation = handle.generation;
        selected_layer_index = 0;
    }

    // A one-layer VDP is the useful limiting case: keep its existing direct
    // view clean. Enhanced devices select their independently-described maps
    // here; composition is deliberately a later decision.
    if (provider && layer_count > 1 && ImGui::BeginTabBar("NameTableLayers"))
    {
        for (std::size_t index = 0; index < layer_count; ++index)
        {
            GC_VideoDebugLayerDescriptor candidate;
            const std::size_t provider_index = layer_indices[index];
            if (!provider->DescribeDebugLayer(provider_index, candidate) || !candidate.code)
                continue;
            const char* name = candidate.name ? candidate.name : candidate.code;
            const std::string label = std::string(name) + "##" + candidate.code;
            if (ImGui::BeginTabItem(label.c_str()))
            {
                selected_layer_index = index;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    GC_VideoDebugSceneDescriptor scene;
    GC_VideoDebugLayerDescriptor layer;
    GC_VideoDebugLayerSurface surface;
    const std::size_t layer_index = layer_count > selected_layer_index
        ? layer_indices[selected_layer_index] : 0;
    if (!provider || layer_count <= layer_index ||
        !provider->DescribeDebugScene(scene) ||
        !provider->DescribeDebugLayer(layer_index, layer) || !layer.code || !layer.has_grid ||
        !provider->ReadDebugLayerSurface(layer_index, surface))
    {
        ImGui::TextDisabled("The active video device exposes no tile-grid layer.");
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    const ImTextureID layer_texture = renderer_get_debug_layer_texture(handle.generation,
                                                                        layer.code,
                                                                        0,
                                                                        surface.generation);
    if (layer_texture == ImTextureID_Invalid)
    {
        ImGui::TextDisabled("Waiting for the video layer texture.");
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    bool& show_grid = config_debug.show_grid_background;
    bool& show_viewport = config_debug.show_viewport_background;
    bool& show_regions = config_debug.show_regions_background;
    const std::size_t viewport_count =
        provider->GetDebugViewportRegionCount(layer_index);
    const int cols = static_cast<int>(layer.columns);
    const int rows = static_cast<int>(layer.rows);
    float& scale = config_debug.vram_background_area_scale;
    const float tile_width = static_cast<float>(layer.cell_width);
    const float presentation_x = static_cast<float>(layer.presentation_scale_x);
    const float presentation_y = static_cast<float>(layer.presentation_scale_y);
    const float display_x = presentation_x * scene.pixel_aspect_x;
    const float display_y = presentation_y * scene.pixel_aspect_y;
    const float size_h = static_cast<float>(layer.content_width) * display_x * scale;
    const float size_v = static_cast<float>(layer.content_height) * display_y * scale;
    const float spacing_h = static_cast<float>(layer.cell_width) * display_x * scale;
    const float spacing_v = static_cast<float>(layer.cell_height) * display_y * scale;
    const float uv_h = static_cast<float>(layer.cell_width) /
        static_cast<float>(surface.width);
    const float uv_v = static_cast<float>(layer.cell_height) /
        static_cast<float>(surface.height);

    ImGui::Checkbox("Show Grid##grid_bg", &show_grid);
    ImGui::SameLine();
    area_scale_control("Scale##scale_bg", &scale);
    ImGui::SameLine();
    if (viewport_count != 0)
    {
        ImGui::Checkbox("Show Screen Rect##viewport_bg", &show_viewport);
        ImGui::SameLine();
    }
    ImGui::Checkbox("Show Regions##regions_bg", &show_regions);

    ImGui::PushFont(gui_default_font, gui_get_default_font_size());
    ImGui::Columns(2, "bg", false);
    ImGui::SetColumnOffset(1, size_h + 10.0f);

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();

    renderer_begin_emulator_image();
    ImGui::Image(layer_texture, ImVec2(size_h, size_v),
                 ImVec2(0.0f, 0.0f), ImVec2(uv_h * cols, uv_v * rows));
    renderer_end_emulator_image();

    if (ImGui::BeginPopupContextItem("##bg_context"))
    {
        if (ImGui::Selectable("Save Layer As PNG..."))
        {
            save_layer_png_with_dialog(surface, static_cast<int>(layer.content_width),
                                       static_cast<int>(layer.content_height));
        }
        ImGui::EndPopup();
    }

    if (show_grid)
    {
        float x = p.x;
        for (int n = 0; n <= cols; n++)
        {
            draw_list->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + size_v),
                               ImColor(grid_line), 1.0f);
            x += spacing_h;
        }
        float y = p.y;
        for (int n = 0; n <= rows; n++)
        {
            draw_list->AddLine(ImVec2(p.x, y), ImVec2(p.x + size_h, y),
                               ImColor(grid_line), 1.0f);
            y += spacing_v;
        }
    }

    if (show_regions)
    {
        if (layer.rows_per_region != 0)
        {
            const float region_height = static_cast<float>(layer.rows_per_region) * spacing_v;
            for (float y = p.y + region_height; y < p.y + size_v; y += region_height)
                draw_list->AddLine(ImVec2(p.x, y), ImVec2(p.x + size_h, y),
                                   ImColor(blue), 2.0f);
        }
    }

    if (show_viewport)
    {
        for (std::size_t viewportIndex = 0; viewportIndex < viewport_count;
             ++viewportIndex)
        {
            GC_VideoDebugViewportRegionDescriptor viewport;
            if (!provider->DescribeDebugViewportRegion(
                    layer_index, viewportIndex, viewport))
                continue;
            draw_wrapped_viewport(draw_list, p, size_h, size_v,
                static_cast<float>(viewport.source_x) * display_x * scale,
                static_cast<float>(viewport.source_y) * display_y * scale,
                static_cast<float>(viewport.width) * display_x * scale,
                static_cast<float>(viewport.height) * display_y * scale);
        }
    }

    if (layer.primary_address_label && layer.address_width_bits != 0)
        debug_address(layer.primary_address_label, layer.primary_address,
                      layer.address_width_bits);

    float mouse_x = io.MousePos.x - p.x;
    float mouse_y = io.MousePos.y - p.y;
    if (ImGui::IsWindowHovered() && mouse_x >= 0.0f && mouse_x < size_h &&
        mouse_y >= 0.0f && mouse_y < size_v)
    {
        const int tile_x = static_cast<int>(mouse_x / spacing_h);
        const int tile_y = static_cast<int>(mouse_y / spacing_v);
        draw_list->AddRect(ImVec2(p.x + (tile_x * spacing_h), p.y + (tile_y * spacing_v)),
                           ImVec2(p.x + ((tile_x + 1) * spacing_h), p.y + ((tile_y + 1) * spacing_v)),
                           ImColor(cyan), 2.0f, ImDrawFlags_RoundCornersAll, 2.0f);

        ImGui::NextColumn();
        renderer_begin_emulator_image();
        ImGui::Image(layer_texture,
                     ImVec2(tile_width * display_x * 16.0f,
                            static_cast<float>(layer.cell_height) * display_y * 16.0f),
                     ImVec2(uv_h * tile_x, uv_v * tile_y),
                     ImVec2(uv_h * (tile_x + 1), uv_v * (tile_y + 1)));
        renderer_end_emulator_image();

        ImGui::TextColored(yellow, "INFO:");
        ImGui::TextColored(cyan, " X:"); ImGui::SameLine(); ImGui::Text("$%02X", tile_x);
        ImGui::TextColored(cyan, " Y:"); ImGui::SameLine(); ImGui::Text("$%02X", tile_y);

        GC_VideoDebugGridCell cell;
        if (provider->DescribeDebugGridCell(layer_index, static_cast<u32>(tile_x),
                                            static_cast<u32>(tile_y), cell) && cell.valid)
        {
            const std::size_t fieldCount = provider->GetDebugGridCellFieldCount(
                layer_index, static_cast<u32>(tile_x), static_cast<u32>(tile_y));
            for (std::size_t fieldIndex = 0; fieldIndex < fieldCount; ++fieldIndex)
            {
                GC_VideoDebugGridCellFieldDescriptor field;
                if (provider->DescribeDebugGridCellField(layer_index,
                        static_cast<u32>(tile_x), static_cast<u32>(tile_y),
                        fieldIndex, field))
                    debug_cell_field(field);
            }

            if (ImGui::IsMouseClicked(0))
                gui_debug_memory_goto(GuiDebugMemoryEditorSlotVram,
                                      cell.navigation_address);
        }
    }

    ImGui::Columns(1);
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar();
}
