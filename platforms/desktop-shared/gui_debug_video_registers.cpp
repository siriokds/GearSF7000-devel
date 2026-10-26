/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gui_debug_video_registers.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "emu.h"
#include "gui.h"
#include "imgui/imgui.h"
#include "imgui/colors.h"
#include "../../src/gearsf7000.h"
#include "../../src/IVideoDevice.h"
#include "../../src/IVideoDebugProvider.h"

static const char* debug_register_short_code(const char* code)
{
    if (code == nullptr)
        return "?";
    const char* separator = std::strrchr(code, '.');
    return separator != nullptr && separator[1] != '\0' ? separator + 1 : code;
}

void gui_debug_video_registers_window(void)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::SetNextWindowPos(ImVec2(840, 80), ImGuiCond_FirstUseEver);
    const float em = gui_get_default_font_size();
    ImGui::SetNextWindowSize(ImVec2(em * 36.0f, em * 32.0f),
        ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("VDP Register Explorer",
        &config_debug.show_video_register_explorer))
    {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    GearSF7000Core* core = emu_get_core();
    const VideoDeviceHandle handle = core->AcquireVideoDevice();
    if (!core->IsVideoDeviceHandleCurrent(handle))
    {
        ImGui::TextDisabled("No video device available.");
        ImGui::PopFont();
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    IVideoDebugProvider* provider = handle.device->GetDebugProvider();
    if (provider == nullptr)
    {
        ImGui::TextDisabled("This video device does not expose registers.");
        ImGui::PopFont();
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    const GC_VideoDeviceDescriptor& device = handle.device->GetDeviceDescriptor();
    ImGui::TextColored(cyan, "%s",
        device.display_name != nullptr ? device.display_name : handle.type_key.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("read-only snapshot");
    ImGui::Separator();

    ImGui::PushID(handle.type_key.c_str());
    if (ImGui::BeginChild("##register_tree", ImVec2(0.0f, 0.0f), true))
    {
        if (ImGui::TreeNodeEx("General Info",
            ImGuiTreeNodeFlags_DefaultOpen))
        {
            const ImGuiTableFlags infoFlags = ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit |
                ImGuiTableFlags_NoSavedSettings;
            if (ImGui::BeginTable("##general_info", 3, infoFlags))
            {
                ImGui::TableSetupColumn("Name",
                    ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Val",
                    ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Description",
                    ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableHeadersRow();

                const std::size_t stateCount = provider->GetStateValueCount();
                for (std::size_t stateIndex = 0;
                    stateIndex < stateCount; ++stateIndex)
                {
                    GC_VideoStateValueDescriptor state;
                    if (!provider->DescribeStateValue(stateIndex, state) ||
                        state.code == nullptr)
                        continue;

                    ImGui::PushID(state.code);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(cyan, "%s",
                        state.name != nullptr ? state.name : state.code);
                    ImGui::TableSetColumnIndex(1);

                    if (state.format == GC_VideoDebugValueFormat::Text)
                    {
                        const char* textValue = nullptr;
                        if (provider->ReadStateTextSnapshot(stateIndex,
                            textValue) && textValue != nullptr)
                            ImGui::TextColored(green, "%s", textValue);
                    }
                    else
                    {
                        u64 value = 0;
                        if (provider->ReadStateValueSnapshot(stateIndex, value))
                        {
                            switch (state.format)
                            {
                                case GC_VideoDebugValueFormat::Boolean:
                                    ImGui::TextColored(value != 0 ? green : gray,
                                        "%s", value != 0 ? "YES" : "NO");
                                    break;
                                case GC_VideoDebugValueFormat::Hexadecimal:
                                case GC_VideoDebugValueFormat::Address:
                                {
                                    const unsigned minimumDigits = state.format ==
                                        GC_VideoDebugValueFormat::Address ? 4u : 2u;
                                    const unsigned digits = std::max(minimumDigits,
                                        (static_cast<unsigned>(state.width_bits) + 3u) / 4u);
                                    ImGui::TextColored(state.format ==
                                        GC_VideoDebugValueFormat::Address
                                            ? yellow : green,
                                        "$%0*llX", static_cast<int>(digits),
                                        static_cast<unsigned long long>(value));
                                    break;
                                }
                                case GC_VideoDebugValueFormat::Decimal:
                                    ImGui::TextColored(green, "%llu",
                                        static_cast<unsigned long long>(value));
                                    break;
                                case GC_VideoDebugValueFormat::Text:
                                    break;
                            }
                        }
                    }
                    ImGui::TableSetColumnIndex(2);
                    const char* dynamicDescription = nullptr;
                    if (state.format != GC_VideoDebugValueFormat::Text &&
                        provider->ReadStateTextSnapshot(stateIndex,
                            dynamicDescription) && dynamicDescription != nullptr)
                        ImGui::TextUnformatted(dynamicDescription);
                    else
                        ImGui::TextUnformatted(state.description != nullptr
                            ? state.description : "");
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }

        const std::size_t bankCount = provider->GetRegisterBankCount();
        if (bankCount == 0)
            ImGui::TextDisabled("No register banks exposed by this device.");

        for (std::size_t bankIndex = 0; bankIndex < bankCount; ++bankIndex)
        {
            GC_VideoRegisterBankDescriptor bank;
            if (!provider->DescribeRegisterBank(bankIndex, bank) ||
                bank.code == nullptr)
                continue;

            ImGui::PushID(bank.code);
            const char* bankName = bank.name != nullptr ? bank.name : bank.code;
            char bankLabel[192];
            std::snprintf(bankLabel, sizeof(bankLabel), "%s (%u)##bank",
                bankName, static_cast<unsigned>(bank.register_count));
            const ImGuiTreeNodeFlags bankFlags = bankIndex == 0
                ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
            if (ImGui::TreeNodeEx(bankLabel, bankFlags))
            {
                const ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerV |
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                    ImGuiTableFlags_SizingFixedFit |
                    ImGuiTableFlags_NoSavedSettings;
                if (ImGui::BeginTable("##registers", 5, tableFlags))
                {
                    ImGui::TableSetupColumn("Reg",
                        ImGuiTableColumnFlags_WidthFixed);
                    ImGui::TableSetupColumn("Name",
                        ImGuiTableColumnFlags_WidthFixed);
                    ImGui::TableSetupColumn("Val",
                        ImGuiTableColumnFlags_WidthFixed);
                    ImGui::TableSetupColumn("Addr",
                        ImGuiTableColumnFlags_WidthFixed);
                    ImGui::TableSetupColumn("Description",
                        ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableHeadersRow();

                    const std::size_t registerCount =
                        provider->GetRegisterCount(bank.code);
                    for (std::size_t registerIndex = 0;
                        registerIndex < registerCount; ++registerIndex)
                    {
                        GC_VideoRegisterDescriptor reg;
                        u32 value = 0;
                        if (!provider->DescribeRegister(bank.code,
                                registerIndex, reg) ||
                            !provider->ReadRegisterSnapshot(bank.code,
                                static_cast<u32>(registerIndex), value))
                            continue;

                        const unsigned widthBits = reg.width_bits != 0
                            ? reg.width_bits : bank.register_width_bits;
                        const unsigned hexDigits = std::max(2u,
                            (widthBits + 3u) / 4u);
                        const std::size_t fieldCount =
                            provider->GetRegisterFieldCount(bank.code,
                                reg.register_index);
                        char registerLabel[96];
                        std::snprintf(registerLabel, sizeof(registerLabel),
                            "%s##register_%u",
                            debug_register_short_code(reg.code),
                            static_cast<unsigned>(reg.register_index));

                        ImGui::PushID(static_cast<int>(registerIndex));
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        const ImGuiTreeNodeFlags registerFlags = fieldCount == 0
                            ? ImGuiTreeNodeFlags_Leaf |
                                ImGuiTreeNodeFlags_NoTreePushOnOpen
                            : ImGuiTreeNodeFlags_None;
                        ImGui::PushStyleColor(ImGuiCol_Text, cyan);
                        const bool registerOpen = ImGui::TreeNodeEx(
                            registerLabel, registerFlags);
                        ImGui::PopStyleColor();
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextColored(magenta, "%s",
                            reg.short_name != nullptr ? reg.short_name : "");
                        ImGui::TableSetColumnIndex(2);
                        char valueText[32];
                        std::snprintf(valueText, sizeof(valueText), "$%0*X",
                            static_cast<int>(hexDigits), value);
                        ImGui::TextColored(green, "%s", valueText);
                        ImGui::TableSetColumnIndex(3);
                        u32 derivedAddress = 0;
                        if (reg.has_derived_address &&
                            provider->ReadRegisterDerivedAddressSnapshot(
                                bank.code, reg.register_index, derivedAddress))
                        {
                            const unsigned addressDigits = std::max(4u,
                                (static_cast<unsigned>(
                                    reg.derived_address_width_bits) + 3u) / 4u);
                            ImGui::TextColored(yellow, "$%0*X",
                                static_cast<int>(addressDigits), derivedAddress);
                        }
                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextUnformatted(reg.description != nullptr
                            ? reg.description : "");

                        if (fieldCount != 0 && registerOpen)
                        {
                            for (std::size_t fieldIndex = 0;
                                fieldIndex < fieldCount; ++fieldIndex)
                            {
                                GC_VideoRegisterFieldDescriptor field;
                                if (!provider->DescribeRegisterField(bank.code,
                                    reg.register_index, fieldIndex, field))
                                    continue;
                                const u32 decoded =
                                    (value & field.mask) >> field.shift;
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextDisabled("field");
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextColored(magenta, "%s",
                                    debug_register_short_code(field.code));
                                ImGui::TableSetColumnIndex(2);
                                char fieldValueText[32];
                                std::snprintf(fieldValueText,
                                    sizeof(fieldValueText), "%u", decoded);
                                ImGui::TextColored(green, "%s", fieldValueText);
                                ImGui::TableSetColumnIndex(4);
                                ImGui::TextUnformatted(field.description != nullptr
                                    ? field.description
                                    : debug_register_short_code(field.code));
                                ImGui::SameLine();
                                ImGui::TextDisabled("$%0*X >> %u",
                                    static_cast<int>(hexDigits), field.mask,
                                    static_cast<unsigned>(field.shift));
                            }
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::PopID();

    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar();
}
