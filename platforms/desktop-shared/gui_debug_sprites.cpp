/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026 Saverio Russo
 *
 * Generic sprite debugger. The active video device supplies the available
 * views, atlas, state, screen rectangles and descriptive fields.
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
#include "gui_debug_sprites.h"
#include "renderer.h"
#include "nfd_parented.h"
#include "../../src/gearsf7000.h"
#include "../../src/IVideoDevice.h"
#include "../../src/IVideoDebugProvider.h"

namespace
{
struct ScopedDebugFileDialogAudioPause
{
    ScopedDebugFileDialogAudioPause() { emu_audio_suspend_for_file_dialog(); }
    ~ScopedDebugFileDialogAudioPause() { emu_audio_resume_after_file_dialog(); }
};

void save_debug_png_with_dialog(const u8* rgbData, int width, int height,
                                int rowStride, const char* defaultName)
{
    ScopedDebugFileDialogAudioPause audioPause;
    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filter[1] = { { "PNG Files", "png" } };
    const nfdresult_t result = NFD_SaveDialogParented(&outPath, filter, 1, nullptr, defaultName);
    if (result == NFD_OKAY)
    {
        emu_save_debug_png(outPath, rgbData, width, height, rowStride);
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

ImU32 sprite_state_colour(GC_VideoDebugSpriteVisualState state)
{
    switch (state)
    {
        case GC_VideoDebugSpriteVisualState::Absent: return ImColor(90, 90, 90);
        case GC_VideoDebugSpriteVisualState::Complete: return ImColor(green);
        case GC_VideoDebugSpriteVisualState::Partial: return ImColor(orange);
        case GC_VideoDebugSpriteVisualState::Overflow: return ImColor(red);
        default: return 0;
    }
}

void draw_debug_value(const GC_VideoDebugGridCellFieldDescriptor& field,
                      GC_VideoDebugSpriteVisualState state)
{
    if (!field.label)
        return;
    ImGui::TextColored(cyan, " %s:", field.label);
    ImGui::SameLine();
    switch (field.format)
    {
        case GC_VideoDebugValueFormat::Boolean:
            field.value ? ImGui::TextColored(green, "ON")
                        : ImGui::TextColored(gray, "OFF");
            break;
        case GC_VideoDebugValueFormat::Decimal:
            ImGui::Text("%llu", static_cast<unsigned long long>(field.value));
            break;
        case GC_VideoDebugValueFormat::Hexadecimal:
        case GC_VideoDebugValueFormat::Address:
        {
            const int digits = std::max(1, static_cast<int>((field.width_bits + 3) / 4));
            ImGui::Text("$%0*llX", digits,
                static_cast<unsigned long long>(field.value));
            break;
        }
        case GC_VideoDebugValueFormat::Text:
        {
            const ImVec4 colour = state == GC_VideoDebugSpriteVisualState::Overflow
                ? red : (state == GC_VideoDebugSpriteVisualState::Partial
                    ? orange : (state == GC_VideoDebugSpriteVisualState::Complete
                        ? green : white));
            ImGui::TextColored(colour, "%s", field.text ? field.text : "");
            break;
        }
    }
}

void draw_source_value_inline(const GC_VideoDebugGridCellFieldDescriptor& field)
{
    if (!field.label)
        return;
    ImGui::SameLine();
    if (field.format == GC_VideoDebugValueFormat::Text)
    {
        ImGui::TextDisabled("%s%s%s", field.label,
            field.text && field.text[0] ? ": " : "", field.text ? field.text : "");
    }
    else if (field.format == GC_VideoDebugValueFormat::Hexadecimal ||
             field.format == GC_VideoDebugValueFormat::Address)
    {
        const int digits = std::max(1, static_cast<int>((field.width_bits + 3) / 4));
        ImGui::TextDisabled("%s $%0*llX", field.label, digits,
            static_cast<unsigned long long>(field.value));
    }
    else
        ImGui::TextDisabled("%s %llu", field.label,
            static_cast<unsigned long long>(field.value));
}

bool preview_part_hidden(const char* code)
{
    if (!code || !code[0])
        return false;
    const std::string token = std::string("|") + code + "|";
    return config_debug.vram_sprites_hidden_preview_parts.find(token) !=
        std::string::npos;
}

void set_preview_part_hidden(const char* code, bool hidden)
{
    if (!code || !code[0])
        return;
    const std::string token = std::string("|") + code + "|";
    const std::size_t position =
        config_debug.vram_sprites_hidden_preview_parts.find(token);
    if (hidden && position == std::string::npos)
        config_debug.vram_sprites_hidden_preview_parts += token;
    else if (!hidden && position != std::string::npos)
        config_debug.vram_sprites_hidden_preview_parts.erase(position, token.size());
}

bool preview_option_disabled(const char* code)
{
    if (!code || !code[0])
        return false;
    const std::string token = std::string("|") + code + "|";
    return config_debug.vram_sprites_disabled_preview_options.find(token) !=
        std::string::npos;
}

void set_preview_option_disabled(const char* code, bool disabled)
{
    if (!code || !code[0])
        return;
    const std::string token = std::string("|") + code + "|";
    const std::size_t position =
        config_debug.vram_sprites_disabled_preview_options.find(token);
    if (disabled && position == std::string::npos)
        config_debug.vram_sprites_disabled_preview_options += token;
    else if (!disabled && position != std::string::npos)
        config_debug.vram_sprites_disabled_preview_options.erase(
            position, token.size());
}
}

static void draw_sprites_view(IVideoDebugProvider* provider,
    const VideoDeviceHandle& videoHandle)
{
    GC_VideoDebugSpriteSourceDescriptor selectedSource;
    bool selectedSourceValid = false;
    float sourceComboWidth = 0.0f;
    for (std::size_t index = 0; index < provider->GetDebugSpriteSourceCount(); ++index)
    {
        GC_VideoDebugSpriteSourceDescriptor source;
        if (!provider->DescribeDebugSpriteSource(index, source) || !source.code || !source.name)
            continue;
        sourceComboWidth = std::max(sourceComboWidth, gui_combo_width_for_text(source.name));
        if (config_debug.vram_sprites_source_code == source.code)
        {
            selectedSource = source;
            selectedSourceValid = true;
        }
    }
    if (!selectedSourceValid)
    {
        if (!provider->DescribeDebugSpriteSource(0, selectedSource) ||
            !selectedSource.code || !selectedSource.name)
            return;
        config_debug.vram_sprites_source_code = selectedSource.code;
    }

    ImGui::PushFont(gui_default_font, gui_get_default_font_size());
    ImGui::TextUnformatted("Source");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(sourceComboWidth);
    if (ImGui::BeginCombo("##sprite_source", selectedSource.name))
    {
        for (std::size_t index = 0; index < provider->GetDebugSpriteSourceCount(); ++index)
        {
            GC_VideoDebugSpriteSourceDescriptor source;
            if (!provider->DescribeDebugSpriteSource(index, source) || !source.code || !source.name)
                continue;
            const bool selected = config_debug.vram_sprites_source_code == source.code;
            if (ImGui::Selectable(source.name, selected))
            {
                config_debug.vram_sprites_source_code = source.code;
                selectedSource = source;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Grid Scale");
    ImGui::SameLine();
    area_scale_control("##scale_sprites", &config_debug.vram_sprites_area_scale);
    ImGui::SameLine();
    ImGui::TextUnformatted("Preview Scale");
    ImGui::SameLine();
    area_scale_control("##scale_sprites_preview",
        &config_debug.vram_sprites_preview_area_scale);

    u64 visiblePreviewParts = 0;
    const std::size_t previewPartCount = std::min<std::size_t>(
        provider->GetDebugPreviewPartCount(), 64);
    for (std::size_t index = 0; index < previewPartCount; ++index)
    {
        GC_VideoDebugPreviewPartDescriptor part;
        if (!provider->DescribeDebugPreviewPart(index, part) ||
            !part.code || !part.name)
            continue;
        bool visible = part.visible && !preview_part_hidden(part.code);
        ImGui::SameLine();
        ImGui::PushID(part.code);
        if (ImGui::Checkbox(part.name, &visible))
            set_preview_part_hidden(part.code, !visible);
        ImGui::PopID();
        if (visible)
            visiblePreviewParts |= 1ull << index;
    }
    u64 enabledPreviewOptions = 0;
    const std::size_t previewOptionCount = std::min<std::size_t>(
        provider->GetDebugPreviewOptionCount(), 64);
    for (std::size_t index = 0; index < previewOptionCount; ++index)
    {
        GC_VideoDebugPreviewOptionDescriptor option;
        if (!provider->DescribeDebugPreviewOption(index, option) ||
            !option.code || !option.name)
            continue;
        bool enabled = option.enabled && !preview_option_disabled(option.code);
        ImGui::SameLine();
        ImGui::PushID(option.code);
        if (ImGui::Checkbox(option.name, &enabled))
            set_preview_option_disabled(option.code, !enabled);
        ImGui::PopID();
        if (enabled)
            enabledPreviewOptions |= 1ull << index;
    }

    if (selectedSource.parameter_code && selectedSource.parameter_name)
    {
        int minimum = 0, maximum = 0;
        if (provider->DescribeDebugSpriteParameter(selectedSource.code, minimum, maximum))
        {
            config_debug.vram_sprites_scanline = std::clamp(
                config_debug.vram_sprites_scanline, minimum, maximum);
            ImGui::SameLine();
            ImGui::TextUnformatted(selectedSource.parameter_name);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150.0f);
            ImGui::SliderInt("##sprite_parameter", &config_debug.vram_sprites_scanline,
                minimum, maximum);
        }
    }
    for (std::size_t fieldIndex = 0;
         fieldIndex < provider->GetDebugSpriteSourceFieldCount(
             selectedSource.code, config_debug.vram_sprites_scanline);
         ++fieldIndex)
    {
        GC_VideoDebugGridCellFieldDescriptor field;
        if (provider->DescribeDebugSpriteSourceField(selectedSource.code,
                config_debug.vram_sprites_scanline, fieldIndex, field))
            draw_source_value_inline(field);
    }
    if (selectedSource.legend)
        ImGui::TextDisabled("%s", selectedSource.legend);

    GC_VideoDebugSpriteAtlasSurface atlas;
    if (!provider->ReadDebugSpriteAtlas(selectedSource.code,
            config_debug.vram_sprites_scanline, atlas) ||
        !atlas.rgba || !atlas.items || atlas.item_count == 0 ||
        atlas.width == 0 || atlas.height == 0)
    {
        ImGui::TextDisabled("The selected sprite source has no image.");
        ImGui::PopFont();
        return;
    }

    const float gridScale = config_debug.vram_sprites_area_scale;
    u32 maximumItemWidth = 1;
    for (u32 index = 0; index < atlas.item_count; ++index)
        maximumItemWidth = std::max(maximumItemWidth, atlas.items[index].width);
    const float gridColumnWidth = 4.0f * maximumItemWidth * gridScale +
        3.0f * ImGui::GetStyle().ItemSpacing.x + 40.0f;
    ImGui::Columns(2, "spr", false);
    ImGui::SetColumnOffset(1, gridColumnWidth);
    ImGui::BeginChild("sprites", ImVec2(0, 0.0f), true);
    const bool gridHovered = ImGui::IsWindowHovered();
    const ImGuiIO& io = ImGui::GetIO();
    std::vector<ImVec2> itemPositions(atlas.item_count);
    int hoveredSprite = -1;

    for (u32 index = 0; index < atlas.item_count; ++index)
    {
        const GC_VideoDebugSpriteAtlasItemDescriptor& item = atlas.items[index];
        const float itemWidth = item.width * gridScale;
        const float itemHeight = item.height * gridScale;
        itemPositions[index] = ImGui::GetCursorScreenPos();
        const ImVec2 uv0(static_cast<float>(item.atlas_x) / atlas.width,
            static_cast<float>(item.atlas_y) / atlas.height);
        const ImVec2 uv1(static_cast<float>(item.atlas_x + item.width) / atlas.width,
            static_cast<float>(item.atlas_y + item.height) / atlas.height);
        if (renderer_emu_debug_sprite_atlas)
        {
            renderer_begin_emulator_image();
            const float brightness = item.inactive ? 0.25f : 1.0f;
            ImGui::ImageWithBg(renderer_emu_debug_sprite_atlas,
                ImVec2(itemWidth, itemHeight), uv0, uv1, ImVec4(0, 0, 0, 0),
                ImVec4(brightness, brightness, brightness, 1.0f));
            renderer_end_emulator_image();
        }
        else
            ImGui::Dummy(ImVec2(itemWidth, itemHeight));

        const ImU32 stateColour = sprite_state_colour(item.visual_state);
        if (stateColour)
            ImGui::GetWindowDrawList()->AddRect(itemPositions[index],
                ImVec2(itemPositions[index].x + itemWidth,
                    itemPositions[index].y + itemHeight), stateColour, 0.0f, 0,
                item.visual_state == GC_VideoDebugSpriteVisualState::Absent ? 1.0f : 2.0f);

        const float mouseX = io.MousePos.x - itemPositions[index].x;
        const float mouseY = io.MousePos.y - itemPositions[index].y;
        if (gridHovered && mouseX >= 0.0f && mouseX < itemWidth &&
            mouseY >= 0.0f && mouseY < itemHeight)
        {
            hoveredSprite = static_cast<int>(index);
            ImGui::GetWindowDrawList()->AddRect(itemPositions[index],
                ImVec2(itemPositions[index].x + itemWidth,
                    itemPositions[index].y + itemHeight), ImColor(cyan), 2.0f,
                ImDrawFlags_RoundCornersAll, 3.0f);
        }

        ImGui::PushID(static_cast<int>(index));
        if (ImGui::BeginPopupContextItem("##spr_context"))
        {
            char menuLabel[40];
            snprintf(menuLabel, sizeof(menuLabel), "Save Sprite %u As PNG...", item.index);
            if (ImGui::Selectable(menuLabel))
            {
                char defaultName[32];
                snprintf(defaultName, sizeof(defaultName), "sprite_%02u.png", item.index);
                std::vector<u8> rgb(static_cast<std::size_t>(item.width * item.height * 3));
                for (u32 y = 0; y < item.height; ++y)
                    for (u32 x = 0; x < item.width; ++x)
                    {
                        const u8* rgba = atlas.rgba +
                            (item.atlas_y + y) * atlas.row_pitch +
                            (item.atlas_x + x) * 4;
                        u8* output = &rgb[(y * item.width + x) * 3];
                        output[0] = rgba[0]; output[1] = rgba[1]; output[2] = rgba[2];
                    }
                save_debug_png_with_dialog(rgb.data(), static_cast<int>(item.width),
                    static_cast<int>(item.height), static_cast<int>(item.width * 3),
                    defaultName);
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        if ((index & 3u) != 3u)
            ImGui::SameLine();
    }
    ImGui::EndChild();
    ImGui::NextColumn();

    const float previewScale = config_debug.vram_sprites_preview_area_scale;
    const float detailsMinimumWidth = 240.0f;
    GC_VideoDebugLayerSurface previewSurface;
    const bool showPreview = previewPartCount > 0 &&
        (selectedSource.capabilities & GC_VIDEO_DEBUG_SPRITE_CAP_SCREEN_PREVIEW) != 0 &&
        provider->ReadDebugPreviewSurface(visiblePreviewParts,
            enabledPreviewOptions, previewSurface) &&
        previewSurface.rgba && previewSurface.width > 0 && previewSurface.height > 0;
    const float previewWidth = showPreview ? previewSurface.width * previewScale : 0.0f;
    const float previewHeight = showPreview ? previewSurface.height * previewScale : 0.0f;
    const ImTextureID previewTexture = showPreview
        ? renderer_get_debug_preview_texture(videoHandle.generation,
            visiblePreviewParts, enabledPreviewOptions, previewSurface.generation)
        : ImTextureID_Invalid;
    if (ImGui::BeginTable("sprite_preview_and_details", showPreview ? 2 : 1,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX,
            ImVec2((showPreview ? previewWidth + ImGui::GetStyle().ItemSpacing.x : 0.0f) +
                detailsMinimumWidth, 0.0f)))
    {
        if (showPreview)
            ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed, previewWidth);
        ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthFixed,
            detailsMinimumWidth);
        ImGui::TableNextRow();
        ImVec2 previewPosition;
        if (showPreview)
        {
            ImGui::TableSetColumnIndex(0);
            previewPosition = ImGui::GetCursorScreenPos();
            const ImVec2 previewSize(previewWidth, previewHeight);
            if (previewTexture != ImTextureID_Invalid)
            {
                renderer_begin_emulator_image();
                ImGui::Image(previewTexture, previewSize);
                renderer_end_emulator_image();
            }
            else
            {
                ImGui::GetWindowDrawList()->AddRectFilled(previewPosition,
                    ImVec2(previewPosition.x + previewSize.x,
                        previewPosition.y + previewSize.y), IM_COL32(0, 0, 0, 255));
                ImGui::Dummy(previewSize);
            }
        }

        if (hoveredSprite >= 0)
        {
            const GC_VideoDebugSpriteAtlasItemDescriptor& item = atlas.items[hoveredSprite];
            if (showPreview && item.has_screen_rect)
            {
                float x0 = previewPosition.x + item.screen_x * previewScale;
                float y0 = previewPosition.y + item.screen_y * previewScale;
                float x1 = x0 + item.screen_width * previewScale;
                float y1 = y0 + item.screen_height * previewScale;
                x0 = std::clamp(x0, previewPosition.x, previewPosition.x +
                    previewWidth);
                x1 = std::clamp(x1, previewPosition.x, previewPosition.x +
                    previewWidth);
                y0 = std::clamp(y0, previewPosition.y, previewPosition.y +
                    previewHeight);
                y1 = std::clamp(y1, previewPosition.y, previewPosition.y +
                    previewHeight);
                if (x1 > x0 && y1 > y0 &&
                    item.visual_state != GC_VideoDebugSpriteVisualState::Overflow)
                    ImGui::GetWindowDrawList()->AddRect(ImVec2(x0, y0), ImVec2(x1, y1),
                        ImColor(cyan), 2.0f, ImDrawFlags_RoundCornersAll, 2.0f);
            }

            ImGui::TableSetColumnIndex(showPreview ? 1 : 0);
            ImGui::TextColored(yellow, "DETAILS:");
            for (std::size_t fieldIndex = 0;
                 fieldIndex < provider->GetDebugSpriteItemFieldCount(selectedSource.code,
                     config_debug.vram_sprites_scanline, item.index);
                 ++fieldIndex)
            {
                GC_VideoDebugGridCellFieldDescriptor field;
                if (provider->DescribeDebugSpriteItemField(selectedSource.code,
                        config_debug.vram_sprites_scanline, item.index,
                        fieldIndex, field))
                    draw_debug_value(field, item.visual_state);
            }
        }
        else
        {
            ImGui::TableSetColumnIndex(showPreview ? 1 : 0);
            const float hintWidth = std::max(96.0f,
                static_cast<float>(maximumItemWidth) * gridScale);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + hintWidth);
            ImGui::TextDisabled("Move over a sprite to inspect it.");
            ImGui::PopTextWrapPos();
        }
        ImGui::EndTable();
    }
    ImGui::Columns(1);
    ImGui::PopFont();
}

static void draw_sprite_attribute_table(IVideoDebugProvider* provider,
    std::size_t tableIndex)
{
    GC_VideoDebugSpriteAttributeTableDescriptor table;
    if (!provider->DescribeDebugSpriteAttributeTable(tableIndex, table) ||
        !table.code || table.column_count == 0)
        return;

    const int addressDigits = std::max(1,
        static_cast<int>((table.address_width_bits + 3) / 4));
    ImGui::TextColored(cyan, "%s Base:", table.name ? table.name : table.code);
    ImGui::SameLine();
    ImGui::Text("$%0*X", addressDigits, table.base_address);
    ImGui::SameLine();
    ImGui::TextDisabled("%u records x %u bytes", table.record_count,
        table.column_count);
    ImGui::SameLine();
    ImGui::Checkbox("Gray inactive##sprite_attributes",
        &config_debug.gray_inactive_sprite_attributes);

    ImGui::PushFont(gui_default_font, gui_get_default_font_size());
    const std::string tableId = std::string("##sprite_attributes_") + table.code;
    const float recordWidth = ImGui::CalcTextSize("SPR  $00  $00  $00  $00").x +
        ImGui::GetStyle().CellPadding.x * 10.0f;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const u32 recordsPerRow = availableWidth >= recordWidth * 4.0f ? 4u :
        (availableWidth >= recordWidth * 2.0f ? 2u : 1u);
    const u32 rowCount = (table.record_count + recordsPerRow - 1) / recordsPerRow;
    const int columnsPerRecord = static_cast<int>(table.column_count) + 1;
    const int guiColumnCount = static_cast<int>(recordsPerRow) * columnsPerRecord +
        static_cast<int>(recordsPerRow - 1);
    if (ImGui::BeginTable(tableId.c_str(), guiColumnCount,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
            ImVec2(0.0f, 0.0f)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (u32 group = 0; group < recordsPerRow; ++group)
        {
            if (group > 0)
            {
                std::string spacer = "##gap_" + std::to_string(group);
                ImGui::TableSetupColumn(spacer.c_str(),
                    ImGuiTableColumnFlags_WidthFixed, 10.0f);
            }
            std::string heading = "SPR##" + std::to_string(group);
            ImGui::TableSetupColumn(heading.c_str());
            for (u32 column = 0; column < table.column_count; ++column)
            {
                GC_VideoDebugSpriteAttributeColumnDescriptor descriptor;
                const char* name = "BYTE";
                if (provider->DescribeDebugSpriteAttributeColumn(tableIndex,
                        column, descriptor) && descriptor.name)
                    name = descriptor.name;
                heading = std::string(name) + "##" + std::to_string(group) + "_" +
                    std::to_string(column);
                ImGui::TableSetupColumn(heading.c_str());
            }
        }
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rowCount));
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                ImGui::TableNextRow();
                for (u32 group = 0; group < recordsPerRow; ++group)
                {
                    if (group > 0)
                        ImGui::TableNextColumn();
                    const u32 record = static_cast<u32>(row) + group * rowCount;
                    ImGui::TableNextColumn();
                    if (record >= table.record_count)
                    {
                        for (u32 column = 0; column < table.column_count; ++column)
                            ImGui::TableNextColumn();
                        continue;
                    }

                    GC_VideoDebugSpriteAttributeRecordDescriptor recordDescriptor;
                    const bool hasRecordDescriptor =
                        provider->DescribeDebugSpriteAttributeRecord(
                            tableIndex, record, recordDescriptor);
                    const bool grayRecord = config_debug.gray_inactive_sprite_attributes &&
                        hasRecordDescriptor && !recordDescriptor.visible;
                    ImGui::TextColored(grayRecord ? mid_gray : cyan, "%02X", record);
                    if (ImGui::IsItemHovered())
                    {
                        const u32 address = hasRecordDescriptor ? recordDescriptor.address :
                            table.base_address + record * table.record_stride;
                        ImGui::SetTooltip("Address: $%0*X", addressDigits, address);
                    }
                    for (u32 column = 0; column < table.column_count; ++column)
                    {
                        ImGui::TableNextColumn();
                        GC_VideoDebugSpriteAttributeColumnDescriptor descriptor;
                        u64 value = 0;
                        const u8 widthBits = provider->DescribeDebugSpriteAttributeColumn(
                            tableIndex, column, descriptor) ? descriptor.width_bits : 8;
                        if (provider->ReadDebugSpriteAttributeValue(tableIndex,
                                record, column, value))
                        {
                            const int digits = std::max(1,
                                static_cast<int>((widthBits + 3) / 4));
                            if (grayRecord)
                                ImGui::TextColored(mid_gray, "$%0*llX", digits,
                                    static_cast<unsigned long long>(value));
                            else
                                ImGui::Text("$%0*llX", digits,
                                    static_cast<unsigned long long>(value));
                        }
                        else
                            ImGui::TextDisabled("--");
                    }
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();
}

static void draw_sprite_attribute_tables(IVideoDebugProvider* provider)
{
    const std::size_t tableCount = provider->GetDebugSpriteAttributeTableCount();
    if (tableCount == 0)
    {
        ImGui::TextDisabled("The active video device exposes no sprite attribute table.");
        return;
    }
    if (tableCount == 1)
    {
        draw_sprite_attribute_table(provider, 0);
        return;
    }

    if (ImGui::BeginTabBar("##sprite_attribute_tables"))
    {
        for (std::size_t index = 0; index < tableCount; ++index)
        {
            GC_VideoDebugSpriteAttributeTableDescriptor table;
            if (!provider->DescribeDebugSpriteAttributeTable(index, table) || !table.code)
                continue;
            const char* name = table.name ? table.name : table.code;
            const std::string label = std::string(name) + "###" + table.code;
            if (ImGui::BeginTabItem(label.c_str()))
            {
                draw_sprite_attribute_table(provider, index);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
}

void debug_window_sprites(void)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::SetNextWindowPos(ImVec2(896, 31), ImGuiCond_FirstUseEver);
    gui_set_next_window_size_em(50.9f, 28.3f);
    if (ImGui::Begin("Sprites", &config_debug.show_sprites))
    {
        GearSF7000Core* core = emu_get_core();
        const VideoDeviceHandle videoHandle = core->AcquireVideoDevice();
        IVideoDebugProvider* provider = core->IsVideoDeviceHandleCurrent(videoHandle)
            ? videoHandle.device->GetDebugProvider() : nullptr;
        if (!provider)
            ImGui::TextDisabled("The active video device exposes no sprite data.");
        else if (ImGui::BeginTabBar("##sprite_tabs"))
        {
            if (provider->GetDebugSpriteSourceCount() > 0 &&
                ImGui::BeginTabItem("Sprites"))
            {
                draw_sprites_view(provider, videoHandle);
                ImGui::EndTabItem();
            }
            if (provider->GetDebugSpriteAttributeTableCount() > 0 &&
                ImGui::BeginTabItem("SAT"))
            {
                draw_sprite_attribute_tables(provider);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}
