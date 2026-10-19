/*
 * GearSF7000 - dynamic Debug Events window
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gui_debug_events.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/colors.h"
#include "config.h"
#include "emu.h"
#include "gui.h"
#include "gui_debug.h"
#include "../../src/gearsf7000.h"

static const char* debug_event_category_name(DebugEventCategory category)
{
    switch (category)
    {
    case DebugEventCategory::CpuExecute: return "CPU";
    case DebugEventCategory::CpuMemory: return "MEM";
    case DebugEventCategory::Vram: return "VRAM";
    case DebugEventCategory::Io: return "I/O";
    case DebugEventCategory::VdpRegister: return "VDP REG";
    case DebugEventCategory::Fdc: return "FDC";
    case DebugEventCategory::Ppi: return "PPI PORTS";
    case DebugEventCategory::VideoTiming: return "VIDEO";
    case DebugEventCategory::Tape: return "TAPE";
    case DebugEventCategory::Audio: return "AUDIO";
    case DebugEventCategory::AyExpansion: return "AY";
    case DebugEventCategory::DeviceState: return "STATE";
    default: return "?";
    }
}

static ImVec4 debug_event_category_color(DebugEventCategory category)
{
    switch (category)
    {
    case DebugEventCategory::CpuExecute: return yellow;
    case DebugEventCategory::CpuMemory: return orange;
    case DebugEventCategory::Vram: return cyan;
    case DebugEventCategory::Io: return green;
    case DebugEventCategory::VdpRegister: return magenta;
    case DebugEventCategory::Fdc: return ImVec4(0.95f, 0.60f, 0.20f, 1.0f);
    case DebugEventCategory::Ppi: return ImVec4(0.40f, 0.90f, 0.80f, 1.0f);
    case DebugEventCategory::VideoTiming: return ImVec4(0.55f, 0.70f, 1.0f, 1.0f);
    case DebugEventCategory::Tape: return ImVec4(0.95f, 0.75f, 0.45f, 1.0f);
    case DebugEventCategory::Audio: return ImVec4(0.85f, 0.55f, 0.95f, 1.0f);
    // Close to the PSG's violet, since both are sound chips, but clearly
    // distinct so a mixed trace can be read at a glance.
    case DebugEventCategory::AyExpansion: return ImVec4(0.70f, 0.60f, 1.0f, 1.0f);
    case DebugEventCategory::DeviceState: return ImVec4(0.50f, 0.85f, 1.0f, 1.0f);
    default: return gray;
    }
}

static ImVec4 debug_event_device_color(DebugDeviceId device)
{
    static const ImVec4 palette[] = {
        cyan, green, yellow, orange, magenta,
        ImVec4(0.40f, 0.90f, 0.80f, 1.0f),
        ImVec4(0.55f, 0.70f, 1.0f, 1.0f),
        ImVec4(0.95f, 0.75f, 0.45f, 1.0f)
    };
    return device == 0 ? gray : palette[(device - 1) % IM_ARRAYSIZE(palette)];
}

static bool debug_event_matches_filter(const DebugEvent& event,
                                       DebugDeviceId device,
                                       DebugSpaceId space)
{
    return device == 0 || (event.device == device && event.space == space);
}

static const char* debug_event_type_name(
    const DebugDeviceRegistry& registry, const DebugEvent& event,
    const DebugEventPresentation& presentation)
{
    if (presentation.typeLabel[0] != 0)
        return presentation.typeLabel;
    const DebugSpaceDescriptor* space = registry.FindSpace(event.device, event.space);
    return space != NULL ? space->name : "Legacy";
}

// Derive control and column sizes from the font and style currently selected
// in UI Settings. Literal pixel widths become stale as soon as either changes.
static float debug_events_item_width(const char* sample)
{
    return ImGui::CalcTextSize(sample).x
        + ImGui::GetStyle().FramePadding.x * 2.0f;
}

static float debug_events_column_width(const char* header, const char* sample)
{
    return std::max(ImGui::CalcTextSize(header).x,
                    ImGui::CalcTextSize(sample).x)
        + ImGui::GetStyle().CellPadding.x * 2.0f;
}

static void debug_events_add_device_preset(
    DebugEventManager* manager, const DebugDeviceDescriptor& device,
    const DebugRulePresetDescriptor& preset)
{
    DebugRule rule;
    rule.useProbe = true;
    rule.deviceKey = device.key;
    rule.spaceKey = preset.spaceKey;
    rule.category = DebugEventCategory::DeviceState;
    rule.addressStart = preset.targetStart;
    rule.addressEnd = preset.targetEnd;
    rule.accessMask = preset.accessMask;
    rule.useValueCondition = preset.useValueCondition;
    rule.valueCondition = preset.valueCondition;
    rule.valueMask = preset.valueMask;
    rule.beforeValueExpected = preset.beforeValueExpected;
    rule.valueExpected = preset.valueExpected;
    rule.actions = preset.actions;
    manager->QueueAddRule(rule);
}

// Draws the PC as a link into the disassembler. The trace is the one place
// where an address is always worth following, so a single click is enough.
static void debug_event_pc_link(const DebugEvent& event, int id)
{
    ImGui::PushID(id);
    char label[16];
    snprintf(label, sizeof(label), "$%04X", event.pc);
    if (ImGui::Selectable(label, false, ImGuiSelectableFlags_AllowDoubleClick))
        gui_debug_request_goto_address(event.pc);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Go to $%04X in the disassembler.", event.pc);
    }
    ImGui::PopID();
}

static const char* debug_event_access_name(u8 access)
{
    if ((access & (DebugEventAccess_Read | DebugEventAccess_Write | DebugEventAccess_Event))
        == (DebugEventAccess_Read | DebugEventAccess_Write | DebugEventAccess_Event)) return "R/W/STATE";
    if ((access & DebugEventAccess_Read) && (access & DebugEventAccess_Event)) return "R/STATE";
    if ((access & DebugEventAccess_Write) && (access & DebugEventAccess_Event)) return "W/STATE";
    if ((access & DebugEventAccess_Read) && (access & DebugEventAccess_Write)) return "R/W";
    if (access & DebugEventAccess_Read) return "READ";
    if (access & DebugEventAccess_Write) return "WRITE";
    if (access & DebugEventAccess_Execute) return "EXEC";
    if (access & DebugEventAccess_Event) return "EVENT";
    return "-";
}

static void debug_rule_condition_text(const DebugRule& rule, char* text, size_t textSize)
{
    switch (rule.valueCondition)
    {
    case DebugValueCondition::Any:
        snprintf(text, textSize, "Every access");
        break;
    case DebugValueCondition::AfterMaskedEqual:
        snprintf(text, textSize, "New=$%llX  mask $%llX",
            static_cast<unsigned long long>(rule.valueExpected),
            static_cast<unsigned long long>(rule.valueMask));
        break;
    case DebugValueCondition::BeforeMaskedEqual:
        snprintf(text, textSize, "Old=$%llX  mask $%llX",
            static_cast<unsigned long long>(rule.beforeValueExpected),
            static_cast<unsigned long long>(rule.valueMask));
        break;
    case DebugValueCondition::BeforeAndAfterMaskedEqual:
        snprintf(text, textSize, "$%llX -> $%llX  mask $%llX",
            static_cast<unsigned long long>(rule.beforeValueExpected),
            static_cast<unsigned long long>(rule.valueExpected),
            static_cast<unsigned long long>(rule.valueMask));
        break;
    case DebugValueCondition::BeforeNotEqualAfter:
        snprintf(text, textSize, "Value changes");
        break;
    case DebugValueCondition::AfterLessThanBefore:
        snprintf(text, textSize, "New < Old");
        break;
    case DebugValueCondition::AfterLessOrEqualBefore:
        snprintf(text, textSize, "New <= Old");
        break;
    case DebugValueCondition::AfterGreaterThanBefore:
        snprintf(text, textSize, "New > Old");
        break;
    case DebugValueCondition::AfterGreaterOrEqualBefore:
        snprintf(text, textSize, "New >= Old");
        break;
    case DebugValueCondition::MaskedBitsChanged:
        snprintf(text, textSize, "Bits $%llX change", static_cast<unsigned long long>(rule.valueMask));
        break;
    case DebugValueCondition::BeforeMaskedSet:
        snprintf(text, textSize, "Old bits $%llX set", static_cast<unsigned long long>(rule.valueMask));
        break;
    case DebugValueCondition::AfterMaskedSet:
        snprintf(text, textSize, "New bits $%llX set", static_cast<unsigned long long>(rule.valueMask));
        break;
    case DebugValueCondition::BeforeMaskedClear:
        snprintf(text, textSize, "Old bits $%llX clear", static_cast<unsigned long long>(rule.valueMask));
        break;
    case DebugValueCondition::AfterMaskedClear:
        snprintf(text, textSize, "New bits $%llX clear", static_cast<unsigned long long>(rule.valueMask));
        break;
    }
}

static const char* debug_key_segment(const char* key)
{
    if (key == NULL)
        return "";
    const char* separator = strrchr(key, '.');
    return separator != NULL ? separator + 1 : key;
}

static bool debug_target_tree_node(const DebugDeviceRegistry& registry,
                                   const DebugDeviceDescriptor& device,
                                   DebugDeviceId selectedDevice,
                                   DebugSpaceId selectedSpace,
                                   const DebugDeviceDescriptor** chosenDevice,
                                   const DebugSpaceDescriptor** chosenSpace)
{
    bool changed = false;
    ImGui::PushID(device.key);
    const bool open = ImGui::TreeNodeEx("##device",
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow |
        ImGuiTreeNodeFlags_SpanAvailWidth, "%s", debug_key_segment(device.key));
    if (open)
    {
        // Tree nodes reserve horizontal space for their arrow. Selectable
        // leaves do not, so add one explicit level to keep spaces visibly
        // below their owning device instead of aligned with its label.
        ImGui::Indent();
        for (size_t s = 0; s < device.spaceCount; ++s)
        {
            const DebugSpaceDescriptor& space = device.spaces[s];
            if (space.supportedAccesses == DebugEventAccess_None)
                continue;
            ImGui::PushID(space.key);
            const bool selected = selectedDevice == device.id &&
                                  selectedSpace == space.id;
            if (ImGui::Selectable(space.key, selected))
            {
                *chosenDevice = &device;
                *chosenSpace = &space;
                changed = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::Unindent();
        const size_t childCount = registry.GetChildCount(device.id);
        // Providers declare whether they are physically external. Keep the
        // catalog's stable order inside each group, but present motherboard
        // components before expansions regardless of registration order.
        for (int externalPass = 0; externalPass < 2; ++externalPass)
            for (size_t child = 0; child < childCount; ++child)
            {
                const DebugDeviceDescriptor* childDevice =
                    registry.GetChild(device.id, child);
                if (childDevice == NULL ||
                    (((childDevice->traits & DebugDeviceTrait_External) != 0)
                        != (externalPass != 0)))
                    continue;
                changed |= debug_target_tree_node(registry, *childDevice,
                    selectedDevice, selectedSpace, chosenDevice, chosenSpace);
            }
        ImGui::TreePop();
    }
    ImGui::PopID();
    return changed;
}

void gui_debug_events_window(void)
{
    ImGui::SetNextWindowPos(ImVec2(320, 420), ImGuiCond_FirstUseEver);
    gui_set_next_window_size_em(23.7f, 14.8f);
    // Keep old saved ImGui layouts from opening this utility window larger
    // than the available desktop. These are constraints, not a fixed size:
    // the user can still resize it freely within this range.
    ImGui::SetNextWindowSizeConstraints(ImVec2(430, 180), ImVec2(1100, 700));

    GearSF7000Core* core = emu_get_core();
    DebugEventManager* manager = core->GetDebugEventManager();
    const DebugDeviceRegistry& registry = manager->GetRegistry();
    (void)config_debug.events_category_filter;
    static std::string filterDeviceKey;
    static std::string filterSpaceKey;
    bool& newestFirst = config_debug.events_newest_first;
    bool& groupByPc = config_debug.events_group_by_pc;
    bool& normalizeClock = config_debug.events_normalize_clock;
    // The window owns one reusable chronological view of the manager's ring.
    // A stable trace therefore costs no copy and no allocation per GUI frame.
    static DebugEventManager* snapshotManager = NULL;
    static u64 snapshotGeneration = 0;
    static std::vector<DebugEvent> events;
    if (snapshotManager != manager)
    {
        snapshotManager = manager;
        snapshotGeneration = 0;
    }
    manager->CopyEventsIfChanged(snapshotGeneration, events);

    ImGui::Begin("Debug Events", &config_debug.show_events);
    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    // The depth is chosen at runtime now, so it is read rather than assumed.
    ImGui::TextColored(cyan, "EVENTS %d/%d", static_cast<int>(events.size()),
        static_cast<int>(manager->GetEventCapacity()));
    ImGui::SameLine();
    ImGui::TextColored(gray, "dropped %llu", manager->GetDroppedEventCount());
    ImGui::SameLine();
    if (ImGui::Button("Clear trace"))
        manager->ClearEvents();
    ImGui::SameLine();
    ImGui::Checkbox("Newest first", &newestFirst);
    ImGui::SameLine();
    ImGui::Checkbox("Group by PC", &groupByPc);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("One row per Z80 source PC; Count is the number of matching raw events.");
    ImGui::SameLine();
    ImGui::Checkbox("Norm. clock", &normalizeClock);
    ImGui::SameLine();
    if (ImGui::Button("Reset layout"))
    {
        ImGui::SetWindowPos(ImVec2(320, 420), ImGuiCond_Always);
        ImGui::SetWindowSize(ImVec2(620, 260), ImGuiCond_Always);
    }

    if (ImGui::CollapsingHeader("Breakpoint Manager", ImGuiTreeNodeFlags_DefaultOpen))
    {
        // Stable keys survive catalog rebuilds. If a machine switch removes
        // the selected instance, retain the type/space preference and bind it
        // to the equivalent device in the new composition.
        static std::string selectedDeviceKey;
        static std::string selectedDeviceType;
        static std::string selectedSpaceKey;
        static char startText[9] = "00000000";
        static char endText[9] = "00000000";
        static bool onRead = true, onWrite = true, onExecute = false, onEvent = false;
        static bool onPause = true, onLog = true, oneShot = false;
        static int condition = static_cast<int>(DebugValueCondition::Any), hit = 1;
        static char maskText[17] = "FF", beforeText[17] = "20", afterText[17] = "90";

        size_t deviceCount = 0;
        const DebugDeviceDescriptor* devices = registry.GetDevices(&deviceCount);
        const DebugDeviceDescriptor* selectedDevice =
            registry.FindDevice(selectedDeviceKey.c_str());
        const DebugSpaceDescriptor* selectedSpace = selectedDevice != NULL
            ? registry.FindSpace(selectedDevice->id, selectedSpaceKey.c_str()) : NULL;

        if (selectedSpace == NULL)
        {
            selectedDevice = NULL;
            // First try the same kind of component and logical space.
            for (size_t i = 0; i < deviceCount && selectedDevice == NULL; ++i)
            {
                if (devices[i].typeKey == NULL ||
                    selectedDeviceType != devices[i].typeKey)
                    continue;
                const DebugSpaceDescriptor* candidate =
                    registry.FindSpace(devices[i].id, selectedSpaceKey.c_str());
                if (candidate != NULL)
                {
                    selectedDevice = &devices[i];
                    selectedSpace = candidate;
                }
            }
            // Then choose the first event-capable space in the live machine.
            for (size_t i = 0; i < deviceCount && selectedDevice == NULL; ++i)
                for (size_t s = 0; s < devices[i].spaceCount; ++s)
                    if (devices[i].spaces[s].supportedAccesses != DebugEventAccess_None)
                    {
                        selectedDevice = &devices[i];
                        selectedSpace = &devices[i].spaces[s];
                        break;
                    }
            if (selectedDevice != NULL)
            {
                selectedDeviceKey = selectedDevice->key;
                selectedDeviceType = selectedDevice->typeKey != NULL
                    ? selectedDevice->typeKey : "";
                selectedSpaceKey = selectedSpace->key;
                snprintf(startText, sizeof(startText), "%08X", selectedSpace->targetMin);
                snprintf(endText, sizeof(endText), "%08X", selectedSpace->targetMin);
            }
        }

        char preview[256] = {};
        if (selectedDevice != NULL && selectedSpace != NULL)
        {
            const char* deviceKey = selectedDevice->key;
            constexpr const char* hiddenRoot = "event.";
            if (strncmp(deviceKey, hiddenRoot, strlen(hiddenRoot)) == 0)
                deviceKey += strlen(hiddenRoot);
            snprintf(preview, sizeof(preview), "%s.%s",
                     deviceKey, selectedSpace->key);
        }
        else
            snprintf(preview, sizeof(preview), "No debug target");
        ImGui::SetNextItemWidth(std::min(
            debug_events_item_width("sc3000.sf7000.parallel_printer_port.signals"),
            ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("Target##debug_target", preview,
                              ImGuiComboFlags_HeightLargest))
        {
            const DebugDeviceDescriptor* chosenDevice = NULL;
            const DebugSpaceDescriptor* chosenSpace = NULL;
            for (size_t i = 0; i < deviceCount; ++i)
            {
                if (devices[i].parentId != 0)
                    continue;
                // Omit a catalog-wide container such as "GearSF7000" while
                // preserving every machine/device relationship below it.
                if (devices[i].spaceCount == 0 &&
                    registry.GetChildCount(devices[i].id) == 1)
                {
                    const DebugDeviceDescriptor* child =
                        registry.GetChild(devices[i].id, 0);
                    if (child != NULL)
                        debug_target_tree_node(registry, *child,
                            selectedDevice != NULL ? selectedDevice->id : 0,
                            selectedSpace != NULL ? selectedSpace->id : 0,
                            &chosenDevice, &chosenSpace);
                }
                else
                    debug_target_tree_node(registry, devices[i],
                        selectedDevice != NULL ? selectedDevice->id : 0,
                        selectedSpace != NULL ? selectedSpace->id : 0,
                        &chosenDevice, &chosenSpace);
            }
            if (chosenDevice != NULL && chosenSpace != NULL)
            {
                selectedDevice = chosenDevice;
                selectedSpace = chosenSpace;
                selectedDeviceKey = chosenDevice->key;
                selectedDeviceType = chosenDevice->typeKey != NULL
                    ? chosenDevice->typeKey : "";
                selectedSpaceKey = chosenSpace->key;
                snprintf(startText, sizeof(startText), "%08X", chosenSpace->targetMin);
                snprintf(endText, sizeof(endText), "%08X", chosenSpace->targetMin);
                onRead = (chosenSpace->supportedAccesses & DebugEventAccess_Read) != 0;
                onWrite = (chosenSpace->supportedAccesses & DebugEventAccess_Write) != 0;
                onExecute = (chosenSpace->supportedAccesses & DebugEventAccess_Execute) != 0;
                onEvent = (chosenSpace->supportedAccesses & DebugEventAccess_Event) != 0;
                condition = static_cast<int>(DebugValueCondition::Any);
            }
            ImGui::EndCombo();
        }

        if (selectedSpace != NULL)
        {
            ImGui::Text("Range");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(debug_events_item_width("00000000"));
            ImGui::InputText("##target_start", startText, IM_ARRAYSIZE(startText),
                             ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::SameLine();
            ImGui::Text("to");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(debug_events_item_width("00000000"));
            ImGui::InputText("##target_end", endText, IM_ARRAYSIZE(endText),
                             ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::SameLine();
            ImGui::TextDisabled("$%X-$%X", selectedSpace->targetMin,
                                selectedSpace->targetMax);

            if (selectedSpace->targetCount > 0)
            {
                ImGui::SameLine();
                if (ImGui::BeginCombo("##named_target", "Named target"))
                {
                    for (size_t t = 0; t < selectedSpace->targetCount; ++t)
                    {
                        const DebugTargetDescriptor& target =
                            selectedSpace->targets[t];
                        char label[160];
                        snprintf(label, sizeof(label), "%s ($%X)",
                                 target.name, target.id);
                        if (ImGui::Selectable(label))
                        {
                            snprintf(startText, sizeof(startText), "%08X", target.id);
                            snprintf(endText, sizeof(endText), "%08X", target.id);
                        }
                    }
                    ImGui::EndCombo();
                }
            }
        }

        ImGui::Text("Access");
        ImGui::SameLine();
        const u8 supported = selectedSpace != NULL
            ? selectedSpace->supportedAccesses : DebugEventAccess_None;
        if (supported & DebugEventAccess_Read)
        {
            ImGui::Checkbox("Read", &onRead);
            ImGui::SameLine();
        }
        if (supported & DebugEventAccess_Write)
        {
            ImGui::Checkbox("Write", &onWrite);
            ImGui::SameLine();
        }
        if (supported & DebugEventAccess_Execute)
        {
            ImGui::Checkbox("Execute", &onExecute);
            ImGui::SameLine();
        }
        if (supported & DebugEventAccess_Event)
        {
            ImGui::Checkbox("Event", &onEvent);
            ImGui::SameLine();
        }
        if (supported == DebugEventAccess_None)
            ImGui::TextDisabled("Unavailable");
        else
            ImGui::NewLine();

        const char* conditionNames[] = {
            "Every selected access", "New value is exactly", "Old value was exactly",
            "Exact old -> exact new", "Value changes", "New value < old",
            "New value <= old", "New value > old", "New value >= old",
            "Selected bits change", "Old selected bits are set",
            "New selected bits are set", "Old selected bits are clear",
            "New selected bits are clear"
        };
        const DebugValueCondition conditionValues[] = {
            DebugValueCondition::Any, DebugValueCondition::AfterMaskedEqual,
            DebugValueCondition::BeforeMaskedEqual, DebugValueCondition::BeforeAndAfterMaskedEqual,
            DebugValueCondition::BeforeNotEqualAfter, DebugValueCondition::AfterLessThanBefore,
            DebugValueCondition::AfterLessOrEqualBefore, DebugValueCondition::AfterGreaterThanBefore,
            DebugValueCondition::AfterGreaterOrEqualBefore, DebugValueCondition::MaskedBitsChanged,
            DebugValueCondition::BeforeMaskedSet, DebugValueCondition::AfterMaskedSet,
            DebugValueCondition::BeforeMaskedClear, DebugValueCondition::AfterMaskedClear
        };
        const char* conditionHelp[] = {
            "Break on every selected access.",
            "Break when the written/read value equals New after applying Mask.",
            "Break when the old value equals Old after applying Mask.",
            "Break only when this exact masked value transition occurs.",
            "Break when Old and New differ.", "Break when New is numerically lower than Old.",
            "Break when New is lower than or equal to Old.",
            "Break when New is numerically higher than Old.",
            "Break when New is higher than or equal to Old.",
            "Break when one or more bits selected by Mask change.",
            "Break when every bit selected by Mask was set before the access.",
            "Break when every bit selected by Mask is set after the access.",
            "Break when every bit selected by Mask was clear before the access.",
            "Break when every bit selected by Mask is clear after the access."
        };
        const bool hasValues = selectedSpace != NULL &&
            (selectedSpace->beforeValueAccesses != DebugEventAccess_None ||
             selectedSpace->afterValueAccesses != DebugEventAccess_None);
        if (!hasValues)
            condition = static_cast<int>(DebugValueCondition::Any);
        ImGui::Text("Break when");
        ImGui::SameLine();
        ImGui::BeginDisabled(!hasValues);
        ImGui::SetNextItemWidth(gui_combo_width_for_array(
            conditionNames, IM_ARRAYSIZE(conditionNames)));
        ImGui::Combo("##condition", &condition, conditionNames,
                     IM_ARRAYSIZE(conditionNames));
        ImGui::EndDisabled();

        const bool needsMask = condition == 1 || condition == 2 || condition == 3
            || condition == 9 || condition == 10 || condition == 11
            || condition == 12 || condition == 13;
        const bool needsBefore = condition == 2 || condition == 3;
        const bool needsAfter = condition == 1 || condition == 3;
        if (needsMask || needsBefore || needsAfter)
        {
            ImGui::SameLine();
            if (needsMask)
            {
                ImGui::Text("Mask"); ImGui::SameLine();
                ImGui::SetNextItemWidth(
                    debug_events_item_width("0000000000000000"));
                ImGui::InputText("##mask", maskText, IM_ARRAYSIZE(maskText),
                                 ImGuiInputTextFlags_CharsHexadecimal);
            }
            if (needsBefore)
            {
                ImGui::SameLine(); ImGui::Text("Old"); ImGui::SameLine();
                ImGui::SetNextItemWidth(
                    debug_events_item_width("0000000000000000"));
                ImGui::InputText("##before", beforeText, IM_ARRAYSIZE(beforeText),
                                 ImGuiInputTextFlags_CharsHexadecimal);
            }
            if (needsAfter)
            {
                ImGui::SameLine(); ImGui::Text("New"); ImGui::SameLine();
                ImGui::SetNextItemWidth(
                    debug_events_item_width("0000000000000000"));
                ImGui::InputText("##after", afterText, IM_ARRAYSIZE(afterText),
                                 ImGuiInputTextFlags_CharsHexadecimal);
            }
        }
        ImGui::TextDisabled("%s", conditionHelp[condition]);

        DebugRule pendingRule;
        const char* validationMessage = NULL;
        try
        {
            const unsigned long parsedStart = std::stoul(startText, 0, 16);
            const unsigned long parsedEnd = std::stoul(endText, 0, 16);
            if (selectedDevice == NULL || selectedSpace == NULL)
                validationMessage = "No target is available";
            else if (parsedStart < selectedSpace->targetMin ||
                     parsedStart > selectedSpace->targetMax ||
                     parsedEnd < selectedSpace->targetMin ||
                     parsedEnd > selectedSpace->targetMax ||
                     parsedStart > parsedEnd)
                validationMessage = "Invalid target range";
            else
            {
                pendingRule.useProbe = true;
                pendingRule.device = selectedDevice->id;
                pendingRule.space = selectedSpace->id;
                pendingRule.deviceKey = selectedDevice->key;
                pendingRule.spaceKey = selectedSpace->key;
                pendingRule.category = DebugEventCategory::DeviceState;
                pendingRule.addressStart = static_cast<u32>(parsedStart);
                pendingRule.addressEnd = static_cast<u32>(parsedEnd);
                pendingRule.accessMask =
                    ((supported & DebugEventAccess_Read) && onRead ? DebugEventAccess_Read : 0) |
                    ((supported & DebugEventAccess_Write) && onWrite ? DebugEventAccess_Write : 0) |
                    ((supported & DebugEventAccess_Execute) && onExecute ? DebugEventAccess_Execute : 0) |
                    ((supported & DebugEventAccess_Event) && onEvent ? DebugEventAccess_Event : 0);
                pendingRule.valueCondition = conditionValues[condition];
                pendingRule.useValueCondition =
                    pendingRule.valueCondition != DebugValueCondition::Any;
                pendingRule.valueMask = std::stoull(maskText, 0, 16);
                pendingRule.beforeValueExpected = std::stoull(beforeText, 0, 16);
                pendingRule.valueExpected = std::stoull(afterText, 0, 16);
                pendingRule.breakOnHit = static_cast<u64>(std::max(1, hit));
                pendingRule.oneShot = oneShot;
                pendingRule.actions = (onLog ? DebugEventAction_Log : 0)
                    | (onPause ? DebugEventAction_Pause : 0);
                validationMessage = ValidateDebugRule(pendingRule, registry);
            }
        }
        catch (...)
        {
            validationMessage = "Invalid hexadecimal value";
        }
        ImGui::Checkbox("Log", &onLog);
        ImGui::SameLine();
        ImGui::Checkbox("Pause", &onPause);
        ImGui::SameLine();
        ImGui::Checkbox("One-shot", &oneShot);
        ImGui::SameLine();
        ImGui::Text("Pause from hit");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(debug_events_item_width("0000"));
        ImGui::InputInt("##hit", &hit);
        ImGui::SameLine();
        ImGui::BeginDisabled(validationMessage != NULL);
        const bool addRequested = ImGui::Button("Add breakpoint");
        ImGui::EndDisabled();
        if (validationMessage != NULL)
        {
            ImGui::SameLine();
            ImGui::TextColored(red, "%s", validationMessage);
        }
        if (addRequested)
            manager->QueueAddRule(pendingRule);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(cyan, "ACTIVE BREAKPOINTS"); ImGui::SameLine();
        ImGui::TextColored(gray, "(%d)", static_cast<int>(manager->GetRules().size()));
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear all##manager")) manager->QueueClearRules();
        const std::vector<DebugRule>& rules = manager->GetRules();
        if (!rules.empty() && ImGui::BeginTable("##active_breakpoints", 7,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("ON", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("TARGET", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("ACCESS", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("CONDITION", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("MODE / HITS", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < rules.size(); ++i)
            {
                const DebugRule& rule = rules[i];
                char conditionText[96];
                debug_rule_condition_text(rule, conditionText, sizeof(conditionText));
                ImGui::PushID(static_cast<int>(rule.id));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                bool enabled = rule.enabled;
                if (ImGui::Checkbox("##enabled_manager", &enabled))
                    manager->QueueSetRuleEnabled(rule.id, enabled);
                ImGui::TableSetColumnIndex(1); ImGui::Text("R%u", rule.id);
                ImGui::TableSetColumnIndex(2);
                if (rule.useProbe)
                {
                    const char* deviceKey = rule.deviceKey.c_str();
                    if (std::strncmp(deviceKey, "event.", 6) == 0)
                        deviceKey += 6;
                    ImGui::TextColored(debug_event_category_color(DebugEventCategory::DeviceState),
                        "%s.%s $%X-$%X", deviceKey,
                        rule.spaceKey.c_str(), rule.addressStart, rule.addressEnd);
                }
                else
                    ImGui::TextColored(debug_event_category_color(rule.category),
                        "%s $%04X-$%04X", debug_event_category_name(rule.category),
                        rule.addressStart, rule.addressEnd);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s", debug_event_access_name(rule.accessMask));
                ImGui::TableSetColumnIndex(4); ImGui::Text("%s", conditionText);
                ImGui::TableSetColumnIndex(5); ImGui::Text("%s / %llu",
                    rule.oneShot ? "1-shot" : "repeat", rule.hitCount);
                ImGui::TableSetColumnIndex(6);
                if (ImGui::SmallButton("Reset"))
                    manager->QueueResetRuleHits(rule.id);
                ImGui::SameLine();
                if (ImGui::SmallButton("X"))
                {
                    const u32 id = rule.id;
                    ImGui::PopID();
                    manager->QueueRemoveRule(id);
                    break;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Trace presets"))
    {
        const DebugDeviceRegistry& registry = manager->GetRegistry();
        size_t presetDeviceCount = 0;
        const DebugDeviceDescriptor* presetDevices =
            registry.GetDevices(&presetDeviceCount);
        bool hasPreviousPreset = false;
        for (size_t d = 0; d < presetDeviceCount; ++d)
        {
            for (size_t p = 0; p < presetDevices[d].presetCount; ++p)
            {
                const DebugRulePresetDescriptor& preset =
                    presetDevices[d].presets[p];
                if (hasPreviousPreset)
                {
                    const ImGuiStyle& style = ImGui::GetStyle();
                    const float buttonWidth = ImGui::CalcTextSize(preset.name).x
                        + style.FramePadding.x * 2.0f;
                    const float contentRight = ImGui::GetWindowPos().x
                        + ImGui::GetWindowContentRegionMax().x;
                    if (ImGui::GetItemRectMax().x + style.ItemSpacing.x
                        + buttonWidth <= contentRight)
                        ImGui::SameLine();
                }
                if (ImGui::Button(preset.name))
                    debug_events_add_device_preset(
                        manager, presetDevices[d], preset);
                hasPreviousPreset = true;
            }
        }
    }

    const DebugDeviceDescriptor* filterDevice =
        registry.FindDevice(filterDeviceKey.c_str());
    const DebugSpaceDescriptor* filterSpace = filterDevice != NULL
        ? registry.FindSpace(filterDevice->id, filterSpaceKey.c_str()) : NULL;
    if (filterSpace == NULL)
    {
        filterDevice = NULL;
        filterSpace = NULL;
        filterDeviceKey.clear();
        filterSpaceKey.clear();
    }
    DebugDeviceId filterDeviceId = filterDevice != NULL ? filterDevice->id : 0;
    DebugSpaceId filterSpaceId = filterSpace != NULL
        ? filterSpace->id : DebugCommonSpace::Unknown;
    char filterPreview[256] = "All";
    if (filterDevice != NULL && filterSpace != NULL)
    {
        const char* key = filterDevice->key;
        if (std::strncmp(key, "event.", 6) == 0)
            key += 6;
        std::snprintf(filterPreview, sizeof(filterPreview), "%s.%s",
                      key, filterSpace->key);
    }
    ImGui::SetNextItemWidth(std::min(
        debug_events_item_width("sc3000.sf7000.parallel_printer_port.signals"),
        ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("Filter", filterPreview, ImGuiComboFlags_HeightLargest))
    {
        if (ImGui::Selectable("All", filterDeviceId == 0))
        {
            filterDeviceKey.clear();
            filterSpaceKey.clear();
            filterDeviceId = 0;
            filterSpaceId = DebugCommonSpace::Unknown;
        }
        size_t filterDeviceCount = 0;
        const DebugDeviceDescriptor* filterDevices =
            registry.GetDevices(&filterDeviceCount);
        for (size_t d = 0; d < filterDeviceCount; ++d)
            for (size_t s = 0; s < filterDevices[d].spaceCount; ++s)
            {
                const DebugSpaceDescriptor& candidate = filterDevices[d].spaces[s];
                if (candidate.supportedAccesses == DebugEventAccess_None)
                    continue;
                const char* key = filterDevices[d].key;
                if (std::strncmp(key, "event.", 6) == 0)
                    key += 6;
                char label[256];
                std::snprintf(label, sizeof(label), "%s.%s", key, candidate.key);
                const bool selected = filterDeviceId == filterDevices[d].id &&
                    filterSpaceId == candidate.id;
                if (ImGui::Selectable(label, selected))
                {
                    filterDeviceKey = filterDevices[d].key;
                    filterSpaceKey = candidate.key;
                    filterDeviceId = filterDevices[d].id;
                    filterSpaceId = candidate.id;
                }
            }
        ImGui::EndCombo();
    }

    u64 clockBase = 0;
    bool hasClockBase = false;
    if (normalizeClock)
    {
        for (const DebugEvent& event : events)
        {
            if (!debug_event_matches_filter(event, filterDeviceId, filterSpaceId))
                continue;
            if (!hasClockBase || event.clock < clockBase)
            {
                clockBase = event.clock;
                hasClockBase = true;
            }
        }
    }
    ImGui::SameLine();
    struct DebugEventPcGroup
    {
        const DebugEvent* representative;
        u32 count;
        bool mixedCategory;
        bool mixedAccess;
        bool mixedRule;
        bool mixedAction;
    };
    std::vector<DebugEventPcGroup> pcGroups;
    if (groupByPc)
    {
        std::unordered_map<u16, size_t> groupByAddress;
        const int groupFirst = newestFirst ? static_cast<int>(events.size()) - 1 : 0;
        const int groupLast = newestFirst ? -1 : static_cast<int>(events.size());
        const int groupDelta = newestFirst ? -1 : 1;
        for (int i = groupFirst; i != groupLast; i += groupDelta)
        {
            const DebugEvent& event = events[static_cast<size_t>(i)];
            if (!debug_event_matches_filter(event, filterDeviceId, filterSpaceId))
                continue;
            const auto found = groupByAddress.find(event.pc);
            if (found == groupByAddress.end())
            {
                groupByAddress[event.pc] = pcGroups.size();
                pcGroups.push_back({ &event, 1, false, false, false, false });
                continue;
            }
            DebugEventPcGroup& group = pcGroups[found->second];
            const DebugEvent& firstEvent = *group.representative;
            ++group.count;
            group.mixedCategory |= event.device != firstEvent.device ||
                event.space != firstEvent.space;
            group.mixedAccess |= event.access != firstEvent.access;
            group.mixedRule |= event.ruleId != firstEvent.ruleId;
            group.mixedAction |= event.actions != firstEvent.actions;
        }
        std::sort(pcGroups.begin(), pcGroups.end(),
            [](const DebugEventPcGroup& left, const DebugEventPcGroup& right)
            {
                return left.representative->pc < right.representative->pc;
            });
    }

    if (ImGui::Button("Copy visible"))
    {
        std::ostringstream text;
        if (groupByPc)
        {
            text << "PC\tTYPE\tACCESS\tCOUNT\tRULE\tACTION\n";
            for (const DebugEventPcGroup& group : pcGroups)
            {
                const DebugEvent& event = *group.representative;
                DebugEventPresentation presentation;
                FormatDebugEventPresentation(registry, event, presentation);
                const char* typeLabel = debug_event_type_name(registry, event, presentation);
                text << '$' << std::hex << std::uppercase << event.pc << std::dec << '\t'
                    << (group.mixedCategory ? "MIXED" : typeLabel) << '\t'
                    << (group.mixedAccess ? "MIXED" : debug_event_access_name(event.access)) << '\t'
                    << group.count << '\t'
                    << (group.mixedRule ? "MIXED" : ("R" + std::to_string(event.ruleId))) << '\t'
                    << (group.mixedAction ? "MIXED" : ((event.actions & DebugEventAction_Pause) ? "PAUSE" : "LOG"))
                    << '\n';
            }
        }
        else
        {
            text << "CLOCK\tPC\tTYPE\tDEVICE\tSPACE\tTARGET\tVALUE\tRULE\tACTION\n";
        const int copyFirst = newestFirst ? static_cast<int>(events.size()) - 1 : 0;
        const int copyLast = newestFirst ? -1 : static_cast<int>(events.size());
        const int copyDelta = newestFirst ? -1 : 1;
        for (int i = copyFirst; i != copyLast; i += copyDelta)
        {
            const DebugEvent& event = events[static_cast<size_t>(i)];
            if (!debug_event_matches_filter(event, filterDeviceId, filterSpaceId))
                continue;
            char target[16];
            char value[24];
            snprintf(target, sizeof(target), "$%04X", event.targetV2);
            if (event.valueKnown)
                snprintf(value, sizeof(value), "$%0*llX", std::max(2, (event.valueWidth + 3) / 4),
                    static_cast<unsigned long long>(event.afterValueV2));
            else
                strcpy(value, "--");
            DebugEventPresentation presentation;
            FormatDebugEventPresentation(registry, event, presentation);
            const char* typeLabel = debug_event_type_name(registry, event, presentation);
            const u64 displayedClock = hasClockBase ? event.clock - clockBase : event.clock;
            text << displayedClock << '\t' << '$' << std::hex << std::uppercase
                << event.pc << std::dec << '\t' << typeLabel
                << '\t' << GetDebugDeviceName(registry, event.device) << '\t'
                << GetDebugSpaceName(registry, event.device, event.space) << '\t' << target << '\t' << value << '\t' << event.ruleId << '\t'
                << ((event.actions & DebugEventAction_Pause) ? "PAUSE" : "LOG") << '\n';
        }
        }
        ImGui::SetClipboardText(text.str().c_str());
    }

    ImGui::Separator();
    if (groupByPc)
    {
        ImGui::Columns(5, "debug_events_group_columns", false);
        ImGui::SetColumnWidth(0, debug_events_column_width("PC", "$FFFF")); ImGui::TextColored(gray, "PC"); ImGui::NextColumn();
        ImGui::SetColumnWidth(1, debug_events_column_width("TYPE", "000000000000000")); ImGui::TextColored(gray, "TYPE"); ImGui::NextColumn();
        ImGui::SetColumnWidth(2, debug_events_column_width("ACCESS", "EXECUTE")); ImGui::TextColored(gray, "ACCESS"); ImGui::NextColumn();
        ImGui::SetColumnWidth(3, debug_events_column_width("COUNT", "000000")); ImGui::TextColored(gray, "COUNT"); ImGui::NextColumn();
        ImGui::TextColored(gray, "RULE / ACTION"); ImGui::NextColumn();
    }
    else
    {
        ImGui::Columns(9, "debug_events_columns", false);
        ImGui::SetColumnWidth(0, debug_events_column_width("CLOCK", "0000000000")); ImGui::TextColored(gray, "CLOCK"); ImGui::NextColumn();
        ImGui::SetColumnWidth(1, debug_events_column_width("PC", "$FFFF")); ImGui::TextColored(gray, "PC"); ImGui::NextColumn();
        ImGui::SetColumnWidth(2, debug_events_column_width("TYPE", "VDP REG")); ImGui::TextColored(gray, "TYPE"); ImGui::NextColumn();
        ImGui::SetColumnWidth(3, debug_events_column_width("ACCESS", "EXECUTE")); ImGui::TextColored(gray, "ACCESS"); ImGui::NextColumn();
        ImGui::SetColumnWidth(4, debug_events_column_width("TARGET", "$FFFF")); ImGui::TextColored(gray, "TARGET"); ImGui::NextColumn();
        ImGui::SetColumnWidth(5, debug_events_column_width("BEFORE", "$FFFFFFFF")); ImGui::TextColored(gray, "BEFORE"); ImGui::NextColumn();
        ImGui::SetColumnWidth(6, debug_events_column_width("AFTER", "$FFFFFFFF")); ImGui::TextColored(gray, "AFTER"); ImGui::NextColumn();
        ImGui::SetColumnWidth(7, debug_events_column_width("RASTER / GAP", "Y000 X000 000000T")); ImGui::TextColored(gray, "RASTER / GAP"); ImGui::NextColumn();
        ImGui::TextColored(gray, "RULE / ACTION"); ImGui::NextColumn();
    }
    ImGui::Columns(1);
    ImGui::Separator();

    ImGui::BeginChild("debug_events_trace", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    if (groupByPc)
    {
        for (const DebugEventPcGroup& group : pcGroups)
        {
            const DebugEvent& event = *group.representative;
            const ImVec4 color = group.mixedCategory ? gray :
                debug_event_device_color(event.device);
            DebugEventPresentation presentation;
            FormatDebugEventPresentation(registry, event, presentation);
            const char* typeLabel = debug_event_type_name(registry, event, presentation);
            ImGui::Columns(5, "debug_events_group_rows", false);
            ImGui::SetColumnWidth(0, debug_events_column_width("PC", "$FFFF")); ImGui::Text("$%04X", event.pc); ImGui::NextColumn();
            ImGui::SetColumnWidth(1, debug_events_column_width("TYPE", "000000000000000")); ImGui::TextColored(color, "%s", group.mixedCategory ? "MIXED" : typeLabel); ImGui::NextColumn();
            ImGui::SetColumnWidth(2, debug_events_column_width("ACCESS", "EXECUTE")); ImGui::Text("%s", group.mixedAccess ? "MIXED" : debug_event_access_name(event.access)); ImGui::NextColumn();
            ImGui::SetColumnWidth(3, debug_events_column_width("COUNT", "000000")); ImGui::Text("%u", group.count);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%u raw event%s from this PC.", group.count,
                    group.count == 1 ? "" : "s");
            ImGui::NextColumn();
            ImGui::Text("%s %s", group.mixedAction ? "MIXED" : ((event.actions & DebugEventAction_Pause) ? "PAUSE" : "LOG"),
                group.mixedRule ? "R*" : ("R" + std::to_string(event.ruleId)).c_str()); ImGui::NextColumn();
            ImGui::Columns(1);
        }
    }
    else
    {
        const int first = newestFirst ? static_cast<int>(events.size()) - 1 : 0;
        const int last = newestFirst ? -1 : static_cast<int>(events.size());
        const int delta = newestFirst ? -1 : 1;
        for (int i = first; i != last; i += delta)
        {
            const DebugEvent& event = events[static_cast<size_t>(i)];
            if (!debug_event_matches_filter(event, filterDeviceId, filterSpaceId))
                continue;
            const ImVec4 color = debug_event_device_color(event.device);
            DebugEventPresentation presentation;
            FormatDebugEventPresentation(registry, event, presentation);
            const char* typeLabel = debug_event_type_name(registry, event, presentation);
            ImGui::Columns(9, "debug_events_rows", false);
            const u64 displayedClock = hasClockBase ? event.clock - clockBase : event.clock;
            ImGui::SetColumnWidth(0, debug_events_column_width("CLOCK", "0000000000")); ImGui::Text("%llu", displayedClock); ImGui::NextColumn();
            ImGui::SetColumnWidth(1, debug_events_column_width("PC", "$FFFF")); debug_event_pc_link(event, i); ImGui::NextColumn();
            ImGui::SetColumnWidth(2, debug_events_column_width("TYPE", "000000000000000")); ImGui::TextColored(color, "%s", typeLabel);
            if (ImGui::IsItemHovered())
            {
                if (presentation.details[0] != 0)
                    ImGui::SetTooltip(
                        "%s\n%s\n%s\n%s",
                        GetDebugDeviceName(registry, event.device),
                        GetDebugSpaceName(registry, event.device, event.space),
                        GetDebugEventPhaseName(event.phase), presentation.details);
                else
                    ImGui::SetTooltip("%s\n%s\n%s",
                        GetDebugDeviceName(registry, event.device),
                        GetDebugSpaceName(registry, event.device, event.space),
                        GetDebugEventPhaseName(event.phase));
            }
            ImGui::NextColumn();
            ImGui::SetColumnWidth(3, debug_events_column_width("ACCESS", "EXECUTE")); ImGui::Text("%s", debug_event_access_name(event.access)); ImGui::NextColumn();
            const int valueDigits = std::max(2, (event.valueWidth + 3) / 4);
            ImGui::SetColumnWidth(4, debug_events_column_width("TARGET", "$FFFF")); ImGui::Text("$%04X", event.targetV2); ImGui::NextColumn();
            ImGui::SetColumnWidth(5, debug_events_column_width("BEFORE", "$FFFFFFFF")); event.beforeKnown
                ? ImGui::Text("$%0*llX", valueDigits, static_cast<unsigned long long>(event.beforeValueV2))
                : ImGui::Text("--"); ImGui::NextColumn();
            ImGui::SetColumnWidth(6, debug_events_column_width("AFTER", "$FFFFFFFF")); event.afterKnown
                ? ImGui::Text("$%0*llX", valueDigits, static_cast<unsigned long long>(event.afterValueV2))
                : ImGui::Text("--"); ImGui::NextColumn();

            ImGui::SetColumnWidth(7, debug_events_column_width("RASTER / GAP", "Y000 X000 000000T"));
            const DebugEventRasterContext& r = event.raster;
            if (r.rasterKnown)
            {
                ImGui::Text("Y%u X%u", r.line, r.dot);
                if (r.gapKnown)
                {
                    // No threshold colouring here on purpose. A latch
                    // replacement row already means the VDP refused the
                    // access, so a second verdict from a hardcoded T-state
                    // limit would only add a wrong one: the limit differs
                    // per mode (29T Graphics, 13T Multicolor, 12T Text).
                    ImGui::SameLine();
                    ImGui::TextColored(gray, "%uT",
                        r.tStatesSincePreviousAccess);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", presentation.details);
            }
            else
                ImGui::TextColored(gray, "--");
            ImGui::NextColumn();

            ImGui::Text("%s R%u", (event.actions & DebugEventAction_Pause) ? "PAUSE" : "LOG", event.ruleId); ImGui::NextColumn();
            ImGui::Columns(1);
        }
    }
    ImGui::EndChild();
    ImGui::PopFont();
    ImGui::End();
}
