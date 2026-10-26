#include "mcp_config.h"

#if GEARSF7000_ENABLE_MCP

/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2021  Ignacio Sanchez
 * Copyright (C) 2026  Saverio Russo
 *
 * Derived from Gearsystem by Ignacio Sanchez. His code is free software: you
 * can redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * The changes and additions by Saverio Russo are free software: you can
 * redistribute them and/or modify them under the terms of the GNU Affero
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 * Section 13 of both licences permits the combination.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License and the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of both licences along with this
 * program.  If not, see http://www.gnu.org/licenses/
 *
 * SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later
 */

#include "mcp_tool_registry.h"
#include "../../../src/definitions.h"
#include <cctype>
#include <sstream>

static const char* json_type_name(const json& value)
{
    if (value.is_object()) return "object";
    if (value.is_array()) return "array";
    if (value.is_string()) return "string";
    if (value.is_boolean()) return "boolean";
    if (value.is_number_integer() || value.is_number_unsigned()) return "integer";
    if (value.is_number()) return "number";
    if (value.is_null()) return "null";
    return "unknown";
}

static bool json_type_matches(const json& value, const std::string& type)
{
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "boolean") return value.is_boolean();
    if (type == "integer") return value.is_number_integer() || value.is_number_unsigned();
    if (type == "number") return value.is_number();
    if (type == "null") return value.is_null();
    return true;
}

static std::string json_path_child(const std::string& path, const std::string& child)
{
    return path.empty() ? child : path + "." + child;
}

static bool validate_json_schema(const json& value, const json& schema, const std::string& path, std::string& error)
{
    if (!schema.is_object())
        return true;

    if (schema.contains("type") && schema["type"].is_string())
    {
        std::string type = schema["type"].get<std::string>();
        if (!json_type_matches(value, type))
        {
            error = "Parameter '" + path + "' must be " + type + ", got " + json_type_name(value);
            return false;
        }
    }

    if (schema.contains("enum") && schema["enum"].is_array())
    {
        bool found = false;
        for (json::const_iterator it = schema["enum"].begin(); it != schema["enum"].end(); ++it)
        {
            if (value == *it)
            {
                found = true;
                break;
            }
        }

        if (!found)
        {
            error = "Parameter '" + path + "' has an invalid value";
            return false;
        }
    }

    if (value.is_number())
    {
        double number = value.get<double>();
        if (schema.contains("minimum") && schema["minimum"].is_number() && number < schema["minimum"].get<double>())
        {
            error = "Parameter '" + path + "' is below the minimum";
            return false;
        }
        if (schema.contains("maximum") && schema["maximum"].is_number() && number > schema["maximum"].get<double>())
        {
            error = "Parameter '" + path + "' is above the maximum";
            return false;
        }
    }

    if (value.is_array())
    {
        if (schema.contains("minItems") && schema["minItems"].is_number_integer() && value.size() < schema["minItems"].get<size_t>())
        {
            error = "Parameter '" + path + "' has too few items";
            return false;
        }
        if (schema.contains("maxItems") && schema["maxItems"].is_number_integer() && value.size() > schema["maxItems"].get<size_t>())
        {
            error = "Parameter '" + path + "' has too many items";
            return false;
        }
        if (schema.contains("items") && schema["items"].is_object())
        {
            for (size_t i = 0; i < value.size(); i++)
            {
                std::ostringstream item_path;
                item_path << path << "[" << i << "]";
                if (!validate_json_schema(value[i], schema["items"], item_path.str(), error))
                    return false;
            }
        }
    }

    if (value.is_object())
    {
        if (schema.contains("required") && schema["required"].is_array())
        {
            for (json::const_iterator it = schema["required"].begin(); it != schema["required"].end(); ++it)
            {
                if (it->is_string() && !value.contains(it->get<std::string>()))
                {
                    error = "Missing required parameter '" + json_path_child(path, it->get<std::string>()) + "'";
                    return false;
                }
            }
        }

        const json* properties = NULL;
        if (schema.contains("properties") && schema["properties"].is_object())
            properties = &schema["properties"];

        for (json::const_iterator it = value.begin(); it != value.end(); ++it)
        {
            std::string child_path = json_path_child(path, it.key());
            if (properties && properties->contains(it.key()))
            {
                if (!validate_json_schema(it.value(), (*properties)[it.key()], child_path, error))
                    return false;
            }
            else if (schema.contains("additionalProperties") && schema["additionalProperties"].is_boolean() && !schema["additionalProperties"].get<bool>())
            {
                error = "Unexpected parameter '" + child_path + "'";
                return false;
            }
            else if (schema.contains("additionalProperties") && schema["additionalProperties"].is_object())
            {
                if (!validate_json_schema(it.value(), schema["additionalProperties"], child_path, error))
                    return false;
            }
        }
    }

    return true;
}

struct McpToolCategory
{
    const char* name;
    const char* title;
    const char* description;
};

struct McpToolCategoryTools
{
    const char* category;
    const char* const* tools;
    size_t count;
};

#define MCP_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const McpToolCategory kMcpToolCategories[] =
{
    {"execution", "Execution Control", "Pause, resume, step (into/over/out/frame), run to an address, reset, fast forward."},
    {"breakpoints", "Breakpoints", "Stop on execute, read or write of an address or range, or on interrupts."},
    {"memory", "Memory", "List memory areas, read/write bytes, atomic multi-area snapshots, select ranges, search memory, watches (freeze, conditions), bookmarks, export/import memory files."},
    {"cpu", "CPU", "Read and write Z80 registers and flags; the T-state clock."},
    {"disassembly", "Disassembly", "Disassembly of executed code, call stack, code coverage (what has actually run), the ROM map (per-ROM classification of every byte, coverage percent, persisted across sessions), bookmarks, save a listing to file."},
    {"symbols", "Symbols", "Add, remove, load, list, and look up debug symbols or labels."},
    {"hardware_video", "Video Hardware", "VDP registers, status and timing, video region, renderer source, framebuffer rows, slot history."},
#if GEARSF7000_ENABLE_YM2413
    {"hardware_audio", "Audio Hardware", "SN76489 PSG, onboard/expansion AY-3-8910, one-bit click and YM2413 FM state; connect/disconnect the FM unit; perceived loudness (LUFS) of the FM bus and final mix."},
#else
    {"hardware_audio", "Audio Hardware", "SN76489 PSG, onboard/expansion AY-3-8910 and one-bit click state."},
#endif
    {"hardware_m5", "Sord M5 Hardware", "M5 memory decode, CTC, keyboard and peripheral latches."},
    {"hardware_msx", "MSX Hardware", "MSX slot mapping, PPI, International keyboard, onboard AY and click audio."},
    {"hardware_sf7000", "SF-7000 Hardware", "Inspect SF-7000 expansion state: disk drive, FDC, serial port, and IPL ROM banking."},
    {"media", "Media", "Load, reload or eject ROMs; tapes (play, stop, rewind, speed); disks; start the SF-7000 IPL; load/save BASIC programs; recent files."},
    {"capture", "Capture", "Screenshots, sprite images and metadata, full-raster debug view, CRT signal dump, debug PNGs."},
    {"state", "Save States", "Save and load complete machine snapshots, in slots or files."},
    {"rewind", "Rewind", "Rewind recorder: recent frames are recorded continuously. See what it holds, seek back to any recorded frame, play/pause/step through the history, analyse a range. Also frame pacing settings."},
    {"input", "Input", "Joypad (press, release, macros), SK-1100 and MSX International keyboard keys, type text into SC-3000 BASIC, keyboard mode, NMI, current input state"
#if GEARSF7000_ENABLE_SMS
        "; on the Master System and Mark III the controller type of each port (SMS pad, Mega Drive 3 or 6 buttons), changeable at any time"
#endif
        "."},
#if GEARSF7000_ENABLE_YM2413
    {"trace", "Debug Events / Trace", "Debug Events: who touched what, and when. Devices form a tree (machine > device > space, e.g. event.sms > event.sms.fm > registers); list_debug_devices shows it with targets and presets, add_device_debug_rule attaches log-or-break rules to any node, set_trace_log records whole kinds unfiltered, get_debug_events reads the shared event ring."},
#else
    {"trace", "Debug Events / Trace", "Debug Events: who touched what, and when. Devices form a tree (machine > device > space, e.g. event.sc3000 > event.sc3000.ppi > event.sc3000.ppi.keyboard); list_debug_devices shows it with targets and presets, add_device_debug_rule attaches log-or-break rules to any node, set_trace_log records whole kinds unfiltered, get_debug_events reads the shared event ring."},
#endif
    {"tools", "Other Tools", "Build identity, MCP transport status and this help."}
};

static const char* const kMcpExecutionTools[] =
{
    "debug_pause", "debug_continue", "debug_step_into", "debug_step_over", "debug_step_out",
    "debug_step_frame", "debug_run_to_cursor", "debug_reset", "debug_get_status",
    "get_system_options", "set_system_options",
    "set_fast_forward_speed", "toggle_fast_forward"
};

static const char* const kMcpBreakpointTools[] =
{
    "set_breakpoint", "set_breakpoint_range", "remove_breakpoint", "list_breakpoints",
    "toggle_irq_breakpoints"
};

static const char* const kMcpMemoryTools[] =
{
    "list_memory_areas", "read_memory", "write_memory", "select_memory_range",
    "set_memory_selection_value", "get_memory_selection", "add_memory_bookmark",
    "remove_memory_bookmark", "list_memory_bookmarks", "watch_add", "watch_remove", "watch_list",
    "watch_freeze", "watch_unfreeze", "watch_set_condition", "watch_clear_condition",
    "watch_condition_status", "memory_search_capture", "memory_search", "memory_find_bytes",
    "memory_find_bytes_advanced", "get_atomic_snapshot", "export_memory_file", "import_memory_file"
};

static const char* const kMcpCpuTools[] =
{
    "get_z80_status", "get_z80_clock", "reset_z80_clock", "write_z80_register"
};

static const char* const kMcpDisassemblyTools[] =
{
    "get_disassembly", "add_disassembler_bookmark", "remove_disassembler_bookmark",
    "list_disassembler_bookmarks", "get_call_stack", "get_code_coverage",
    "clear_code_coverage", "save_disassembly_file", "get_rom_map",
    "set_rom_map_enabled", "save_rom_map", "reset_rom_map", "create_rom_map_project",
    "set_rom_map_projects_folder", "open_rom_map_project"
};

static const char* const kMcpSymbolTools[] =
{
    "add_symbol", "remove_symbol", "list_symbols", "load_symbols",
    "lookup_symbol_by_name", "lookup_symbol_at_address"
};

static const char* const kMcpVideoTools[] =
{
    "get_vdp_registers", "get_vdp_status", "set_renderer_source", "read_framebuffer", "set_region", "get_slot_history"
};

static const char* const kMcpAudioTools[] =
{
    "get_psg_status", "get_ay8910_status", "get_ym2413_status",
    "set_ym2413_enabled", "set_ym2413_loudness_enabled", "get_ym2413_loudness",
    "start_vgm_recording", "stop_vgm_recording", "get_vgm_recording_status",
    "start_wav_recording", "stop_wav_recording", "set_audio_mute"
};

static const char* const kMcpM5Tools[] = {"get_m5_status"};
static const char* const kMcpMSXTools[] = {"get_msx_status"};

static const char* const kMcpSF7000Tools[] =
{
    "get_sf7000_status"
};

static const char* const kMcpMediaTools[] =
{
    "load_rom", "eject_rom", "set_start_paused", "load_tape", "tape_play", "tape_stop", "tape_rewind", "set_tape_speed", "mount_disk", "start_sf7000", "get_media_info", "list_recent_roms", "list_recent_tapes", "list_recent_disks", "load_basic_program", "save_basic_program", "find_basic_blocks"
};

static const char* const kMcpCaptureTools[] =
{
    "get_screenshot", "list_sprites", "get_sprite_image",
    "get_sprite_pipeline", "get_sprite_scanline_history", "set_full_raster_debug_enabled",
    "get_full_raster_debug_screenshot", "dump_crt_signal", "save_debug_png"
};

static const char* const kMcpStateTools[] =
{
    "list_save_state_slots", "select_save_state_slot", "save_state", "load_state",
    "save_state_file", "load_state_file"
};

static const char* const kMcpRewindTools[] =
{
    "get_rewind_status", "rewind_seek", "configure_rewind", "rewind_transport",
    "get_rewind_position", "analyze_rewind_range", "get_sync_settings",
    "set_sync_settings"
};

static const char* const kMcpInputTools[] =
{
    "trigger_nmi", "keyboard_text", "cancel_keyboard_text", "keyboard_key", "controller_button", "set_controller_device", "controller_macro", "get_input_state", "get_keyboard_mode", "set_keyboard_mode",
    "basic_typer_set_text", "basic_typer_clear", "basic_typer_status"
};

static const char* const kMcpTraceTools[] =
{
    "get_trace_log", "set_trace_log", "list_debug_devices", "add_debug_rule", "add_device_debug_rule", "list_debug_rules",
    "remove_debug_rule", "clear_debug_rules", "get_debug_events", "clear_debug_events"
};

static const McpToolCategoryTools kMcpToolCategoryTools[] =
{
    {"execution", kMcpExecutionTools, MCP_ARRAY_COUNT(kMcpExecutionTools)},
    {"breakpoints", kMcpBreakpointTools, MCP_ARRAY_COUNT(kMcpBreakpointTools)},
    {"memory", kMcpMemoryTools, MCP_ARRAY_COUNT(kMcpMemoryTools)},
    {"cpu", kMcpCpuTools, MCP_ARRAY_COUNT(kMcpCpuTools)},
    {"disassembly", kMcpDisassemblyTools, MCP_ARRAY_COUNT(kMcpDisassemblyTools)},
    {"symbols", kMcpSymbolTools, MCP_ARRAY_COUNT(kMcpSymbolTools)},
    {"hardware_video", kMcpVideoTools, MCP_ARRAY_COUNT(kMcpVideoTools)},
    {"hardware_audio", kMcpAudioTools, MCP_ARRAY_COUNT(kMcpAudioTools)},
    {"hardware_m5", kMcpM5Tools, MCP_ARRAY_COUNT(kMcpM5Tools)},
    {"hardware_msx", kMcpMSXTools, MCP_ARRAY_COUNT(kMcpMSXTools)},
    {"hardware_sf7000", kMcpSF7000Tools, MCP_ARRAY_COUNT(kMcpSF7000Tools)},
    {"media", kMcpMediaTools, MCP_ARRAY_COUNT(kMcpMediaTools)},
    {"capture", kMcpCaptureTools, MCP_ARRAY_COUNT(kMcpCaptureTools)},
    {"state", kMcpStateTools, MCP_ARRAY_COUNT(kMcpStateTools)},
    {"rewind", kMcpRewindTools, MCP_ARRAY_COUNT(kMcpRewindTools)},
    {"input", kMcpInputTools, MCP_ARRAY_COUNT(kMcpInputTools)},
    {"trace", kMcpTraceTools, MCP_ARRAY_COUNT(kMcpTraceTools)}
};

const size_t kMcpSearchToolLimit = 20;

McpToolRegistry::McpToolRegistry()
{
    m_tools = json::array();
}

void McpToolRegistry::SetTools(const json& tools)
{
    m_tools = json::array();

    if (!tools.is_array())
        return;

    for (json::const_iterator it = tools.begin(); it != tools.end(); ++it)
    {
        if (!it->is_object() || !it->contains("name") || !(*it)["name"].is_string())
            continue;

        if (IsRouterToolName((*it)["name"].get<std::string>()))
            continue;

        m_tools.push_back(*it);
    }
}

bool McpToolRegistry::IsEmpty() const
{
    return !m_tools.is_array() || m_tools.empty();
}

bool McpToolRegistry::HasTool(const std::string& tool_name) const
{
    return FindTool(tool_name) != NULL;
}

bool McpToolRegistry::HasCategory(const std::string& category) const
{
    const size_t category_count = MCP_ARRAY_COUNT(kMcpToolCategories);

    for (size_t i = 0; i < category_count; i++)
    {
        if ((category == kMcpToolCategories[i].name) && HasToolInCategory(category, false))
            return true;
    }

    return false;
}

bool McpToolRegistry::ValidateArguments(const std::string& tool_name, const json& arguments, std::string& error) const
{
    const json* tool = FindTool(tool_name);
    if (!tool)
    {
        error = "Unknown tool '" + tool_name + "'";
        return false;
    }

    if (!tool->contains("inputSchema") || !(*tool)["inputSchema"].is_object())
    {
        error = "Tool has no valid input schema";
        return false;
    }

    return validate_json_schema(arguments, (*tool)["inputSchema"], "", error);
}

json McpToolRegistry::GetStats() const
{
    json categories = json::array();
    const size_t category_count = MCP_ARRAY_COUNT(kMcpToolCategories);
    int routed_count = 0;
    int direct_count = 0;

    for (json::const_iterator it = m_tools.begin(); it != m_tools.end(); ++it)
    {
        std::string name = (*it)["name"].get<std::string>();

        if (IsDirectToolName(name))
            direct_count++;
        else
            routed_count++;
    }

    for (size_t i = 0; i < category_count; i++)
    {
        int tool_count = CountToolsInCategory(kMcpToolCategories[i].name, false);

        if (tool_count == 0)
            continue;

        categories.push_back({
            {"name", kMcpToolCategories[i].name},
            {"tool_count", tool_count}
        });
    }

    return {
        {"total_categories", categories.size()},
        {"total_routed_tools", routed_count},
        {"total_direct_tools", direct_count},
        {"total_tools", routed_count + direct_count},
        {"categories", categories}
    };
}

json McpToolRegistry::GetCategories() const
{
    json categories = json::array();
    const size_t category_count = MCP_ARRAY_COUNT(kMcpToolCategories);

    for (size_t i = 0; i < category_count; i++)
    {
        if (!HasRoutedToolInCategory(kMcpToolCategories[i].name))
            continue;

        categories.push_back({
            {"name", kMcpToolCategories[i].name},
            {"title", kMcpToolCategories[i].title},
            {"description", kMcpToolCategories[i].description},
            {"tool_count", CountToolsInCategory(kMcpToolCategories[i].name, false)}
        });
    }

    return categories;
}

json McpToolRegistry::GetCategoryNames() const
{
    json categories = json::array();
    const size_t category_count = MCP_ARRAY_COUNT(kMcpToolCategories);

    for (size_t i = 0; i < category_count; i++)
    {
        if (HasRoutedToolInCategory(kMcpToolCategories[i].name))
            categories.push_back(kMcpToolCategories[i].name);
    }

    return categories;
}

json McpToolRegistry::GetDirectTools() const
{
    json tools = json::array();

    for (json::const_iterator it = m_tools.begin(); it != m_tools.end(); ++it)
    {
        std::string name = (*it)["name"].get<std::string>();
        if (IsDirectToolName(name))
            tools.push_back(*it);
    }

    return tools;
}

json McpToolRegistry::GetToolsInCategory(const std::string& category) const
{
    json tools = json::array();

    for (json::const_iterator it = m_tools.begin(); it != m_tools.end(); ++it)
    {
        std::string name = (*it)["name"].get<std::string>();

        if (IsDirectToolName(name))
            continue;

        if (ToolCategoryForName(name) == category)
            tools.push_back(ToolToSummaryJson(*it));
    }

    return tools;
}

json McpToolRegistry::GetToolInfo(const std::string& tool_name) const
{
    const json* tool = FindTool(tool_name);

    if (tool == NULL)
        return json::object();

    return ToolToInfoJson(*tool);
}

std::string McpToolRegistry::GetCategoryTitle(const std::string& category) const
{
    const size_t category_count = MCP_ARRAY_COUNT(kMcpToolCategories);

    for (size_t i = 0; i < category_count; i++)
    {
        if (category == kMcpToolCategories[i].name)
            return kMcpToolCategories[i].title;
    }

    return "";
}

std::string McpToolRegistry::GetCategoryDescription(const std::string& category) const
{
    const size_t category_count = MCP_ARRAY_COUNT(kMcpToolCategories);

    for (size_t i = 0; i < category_count; i++)
    {
        if (category == kMcpToolCategories[i].name)
            return kMcpToolCategories[i].description;
    }

    return "";
}

int McpToolRegistry::GetCategoryToolCount(const std::string& category) const
{
    return CountToolsInCategory(category, false);
}

json McpToolRegistry::GetHelp(const std::string& topic) const
{
    // Unlike the router helpers this lists direct tools too: help is the
    // manual for every tool, whichever listing mode the server runs in.
    const size_t category_count = MCP_ARRAY_COUNT(kMcpToolCategories);
    const std::string key = ToLower(topic);

    if (key.empty())
    {
        json sections = json::array();
        for (size_t i = 0; i < category_count; i++)
        {
            json names = json::array();
            for (json::const_iterator it = m_tools.begin(); it != m_tools.end(); ++it)
            {
                const std::string name = (*it)["name"].get<std::string>();
                if (ToolCategoryForName(name) == kMcpToolCategories[i].name)
                    names.push_back(name);
            }
            if (names.empty())
                continue;
            sections.push_back({{"category", kMcpToolCategories[i].name},
                                {"description", kMcpToolCategories[i].description},
                                {"tools", names}});
        }
        return {{"categories", sections},
                {"next", "help with a category name lists its tools with descriptions; "
                         "any other word searches every tool."}};
    }

    for (size_t i = 0; i < category_count; i++)
    {
        if (key != kMcpToolCategories[i].name)
            continue;
        json tools = json::array();
        for (json::const_iterator it = m_tools.begin(); it != m_tools.end(); ++it)
        {
            const std::string name = (*it)["name"].get<std::string>();
            if (ToolCategoryForName(name) == key)
                tools.push_back(ToolToSummaryJson(*it));
        }
        return {{"category", kMcpToolCategories[i].name},
                {"title", kMcpToolCategories[i].title},
                {"description", kMcpToolCategories[i].description},
                {"tools", tools}};
    }

    // Keyword: every tool whose name, description, category or aliases
    // contain all the words, grouped by category.
    json byCategory = json::object();
    int matches = 0;
    for (json::const_iterator it = m_tools.begin(); it != m_tools.end(); ++it)
    {
        const std::string name = (*it)["name"].get<std::string>();
        const std::string category = ToolCategoryForName(name);
        const std::string haystack = ToLower(name + " " + it->value("title", "") + " " +
            it->value("description", "") + " " + category + " " + AliasesForTool(name));
        std::istringstream terms(key);
        std::string term;
        bool all = true;
        while (terms >> term)
            all = all && haystack.find(term) != std::string::npos;
        if (!all)
            continue;
        byCategory[category].push_back(ToolToSummaryJson(*it));
        ++matches;
    }
    if (matches == 0)
        return {{"topic", topic}, {"matches", 0},
                {"categories", GetAllCategoryNames()},
                {"next", "No tool matched. Try a category name or a shorter word."}};
    return {{"topic", topic}, {"matches", matches}, {"by_category", byCategory}};
}

json McpToolRegistry::GetAllCategoryNames() const
{
    json names = json::array();
    const size_t category_count = MCP_ARRAY_COUNT(kMcpToolCategories);
    for (size_t i = 0; i < category_count; i++)
        if (CountToolsInCategory(kMcpToolCategories[i].name, true) > 0)
            names.push_back(kMcpToolCategories[i].name);
    return names;
}

std::string McpToolRegistry::GetCategoryNameList()
{
    std::string text;
    const size_t category_count = MCP_ARRAY_COUNT(kMcpToolCategories);
    for (size_t i = 0; i < category_count; i++)
    {
        if (!text.empty())
            text += ", ";
        text += kMcpToolCategories[i].name;
    }
    return text;
}

std::string McpToolRegistry::GetCategoryOverview() const
{
    std::string text;
    const size_t category_count = MCP_ARRAY_COUNT(kMcpToolCategories);
    for (size_t i = 0; i < category_count; i++)
    {
        if (CountToolsInCategory(kMcpToolCategories[i].name, true) == 0)
            continue;
        text += "- ";
        text += kMcpToolCategories[i].name;
        text += ": ";
        text += kMcpToolCategories[i].description;
        text += "\n";
    }
    return text;
}

json McpToolRegistry::SearchTools(const std::string& query) const
{
    json tools = json::array();
    std::string query_lower = ToLower(query);

    if (query_lower.empty())
        return tools;

    for (json::const_iterator it = m_tools.begin(); it != m_tools.end(); ++it)
    {
        std::string name = (*it)["name"].get<std::string>();

        std::string haystack = name + " ";
        haystack += it->value("title", "");
        haystack += " ";
        haystack += it->value("description", "");
        haystack += " ";
        haystack += ToolCategoryForName(name);
        haystack += " ";
        haystack += AliasesForTool(name);

        std::string haystack_lower = ToLower(haystack);
        std::istringstream terms(query_lower);
        std::string term;
        bool has_terms = false;
        bool matches = true;

        while (terms >> term)
        {
            has_terms = true;

            if (haystack_lower.find(term) == std::string::npos)
            {
                matches = false;
                break;
            }
        }

        if (has_terms && matches)
        {
            tools.push_back(ToolToSearchJson(*it));

            if (tools.size() >= kMcpSearchToolLimit)
                return tools;
        }
    }

    return tools;
}

bool McpToolRegistry::IsRouterTool(const std::string& tool_name) const
{
    return IsRouterToolName(tool_name);
}

bool McpToolRegistry::IsRouterTool(const std::string& tool_name, const std::string& router_tool_name) const
{
    return NormalizeToolName(tool_name) == router_tool_name;
}

size_t McpToolRegistry::GetSearchToolLimit() const
{
    return kMcpSearchToolLimit;
}

bool McpToolRegistry::IsRouterToolName(const std::string& tool_name) const
{
    std::string name = NormalizeToolName(tool_name);

    return (name == "list_tool_categories") ||
           (name == "get_category_tools") ||
           (name == "get_tool_info") ||
           (name == "search_tools") ||
           (name == "execute_tool");
}

std::string McpToolRegistry::NormalizeToolName(std::string tool_name) const
{
    size_t pos = 0;
    while ((pos = tool_name.find('.', pos)) != std::string::npos)
    {
        tool_name[pos] = '_';
        pos++;
    }

    return tool_name;
}

std::string McpToolRegistry::ToLower(const std::string& text) const
{
    std::string result = text;

    for (size_t i = 0; i < result.size(); i++)
        result[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(result[i])));

    return result;
}

bool McpToolRegistry::StringContains(const std::string& text, const std::string& needle) const
{
    return text.find(needle) != std::string::npos;
}

bool McpToolRegistry::ToolNameInList(const std::string& name, const char* const* tools, size_t count) const
{
    for (size_t i = 0; i < count; i++)
    {
        if (name == tools[i])
            return true;
    }

    return false;
}

bool McpToolRegistry::IsDirectToolName(const std::string& tool_name) const
{
    std::string name = NormalizeToolName(tool_name);

    return (name == "help_ai") ||
           (name == "load_rom") ||
           (name == "eject_rom") ||
           (name == "set_start_paused") ||
           (name == "load_tape") ||
           (name == "mount_disk") ||
           (name == "start_sf7000") ||
           (name == "get_media_info") ||
           (name == "get_system_info") ||
           (name == "list_recent_roms") ||
           (name == "list_recent_tapes") ||
           (name == "list_recent_disks") ||
           (name == "debug_pause") ||
           (name == "debug_continue") ||
           (name == "debug_step_into") ||
           (name == "get_z80_status") ||
           (name == "read_memory") ||
           (name == "write_memory") ||
           (name == "get_disassembly") ||
           (name == "set_breakpoint") ||
           (name == "get_screenshot") ||
           (name == "trigger_nmi") ||
           (name == "keyboard_text") ||
           (name == "cancel_keyboard_text") ||
           (name == "keyboard_key") ||
           (name == "controller_button");
}

std::string McpToolRegistry::ToolCategoryForName(const std::string& tool_name) const
{
    std::string name = NormalizeToolName(tool_name);
    const size_t category_count = MCP_ARRAY_COUNT(kMcpToolCategoryTools);

    for (size_t i = 0; i < category_count; i++)
    {
        if (ToolNameInList(name, kMcpToolCategoryTools[i].tools, kMcpToolCategoryTools[i].count))
            return kMcpToolCategoryTools[i].category;
    }

    return "tools";
}

std::string McpToolRegistry::AliasesForTool(const std::string& tool_name) const
{
    std::string name = NormalizeToolName(tool_name);
    std::string aliases = ToolCategoryForName(name);

    if (StringContains(name, "mikey"))
        aliases += " timers timer irq interrupt hblank vblank audio uart comlynx";
    if (StringContains(name, "suzy") || StringContains(name, "sprite"))
        aliases += " sprites sprite blitter math collision scb";
    if (StringContains(name, "vdp") || StringContains(name, "lcd") ||
        StringContains(name, "huc62"))
        aliases += " video display scanline hblank vblank palette tiles background";
    if (StringContains(name, "apu") || StringContains(name, "psg") ||
        StringContains(name, "audio") || StringContains(name, "ym2413") ||
        StringContains(name, "ay8910"))
        aliases += " sound audio channel tone noise volume";
    if (StringContains(name, "controller"))
        aliases += " pad joypad gamepad genesis megadrive mega drive 3-button 6-button"
                   " three six buttons start mode controller type port";
    if (StringContains(name, "loudness"))
        aliases += " lufs lkfs r128 bs1770 perceived level db decibel meter";
    if (StringContains(name, "sf7000") || StringContains(name, "fdc") ||
        StringContains(name, "floppy"))
        aliases += " disk floppy fdc765 drive motor index track sector ppi ipl";
    if (StringContains(name, "breakpoint"))
        aliases += " watchpoint stop read write execute irq interrupt";
    if (StringContains(name, "memory"))
        aliases += " ram rom vram bytes search watch bookmark selection";
    if (StringContains(name, "symbol"))
        aliases += " label labels names debug symbols";
    if (StringContains(name, "trace"))
        aliases += " log logger events cpu irq debug output";
    if (StringContains(name, "controller"))
        aliases += " input joypad gamepad button macro tap press release";
    if (StringContains(name, "state") || StringContains(name, "rewind"))
        aliases += " save savestate slot snapshot time travel history";
    if (StringContains(name, "cart") || StringContains(name, "eeprom"))
        aliases += " cartridge rom mapper bank save nonvolatile";
    if (StringContains(name, "cdrom") || StringContains(name, "adpcm"))
        aliases += " cd disc track audio pcm";

    return aliases;
}

const json* McpToolRegistry::FindTool(const std::string& tool_name) const
{
    std::string normalized_name = NormalizeToolName(tool_name);

    for (json::const_iterator it = m_tools.begin(); it != m_tools.end(); ++it)
    {
        std::string name = (*it)["name"].get<std::string>();
        if (NormalizeToolName(name) == normalized_name)
            return &(*it);
    }

    return NULL;
}

bool McpToolRegistry::HasToolInCategory(const std::string& category, bool include_direct) const
{
    for (json::const_iterator it = m_tools.begin(); it != m_tools.end(); ++it)
    {
        std::string name = (*it)["name"].get<std::string>();

        if (!include_direct && IsDirectToolName(name))
            continue;

        if (ToolCategoryForName(name) == category)
            return true;
    }

    return false;
}

bool McpToolRegistry::HasRoutedToolInCategory(const std::string& category) const
{
    return HasToolInCategory(category, false);
}

int McpToolRegistry::CountToolsInCategory(const std::string& category, bool include_direct) const
{
    int count = 0;

    for (json::const_iterator it = m_tools.begin(); it != m_tools.end(); ++it)
    {
        std::string name = (*it)["name"].get<std::string>();

        if (!include_direct && IsDirectToolName(name))
            continue;

        if (ToolCategoryForName(name) == category)
            count++;
    }

    return count;
}

json McpToolRegistry::ToolToSummaryJson(const json& tool) const
{
    json result;
    std::string name = tool.value("name", "");

    result["name"] = name;
    result["description"] = tool.value("description", "");

    return result;
}

json McpToolRegistry::ToolToSearchJson(const json& tool) const
{
    json result;
    std::string name = tool.value("name", "");

    result["category"] = ToolCategoryForName(name);
    result["tool"] = name;
    result["description"] = tool.value("description", "");

    if (IsDirectToolName(name))
        result["category"] = "direct";

    return result;
}

json McpToolRegistry::ToolToInfoJson(const json& tool) const
{
    json result;
    std::string name = tool.value("name", "");

    result["name"] = name;
    result["title"] = tool.value("title", name);
    result["description"] = tool.value("description", "");
    result["category"] = ToolCategoryForName(name);
    result["direct"] = IsDirectToolName(name);

    if (IsDirectToolName(name))
        result["category"] = "direct";

    if (tool.contains("inputSchema"))
        result["inputSchema"] = tool["inputSchema"];
    else
        result["inputSchema"] = json::object();

    if (tool.contains("annotations"))
        result["annotations"] = tool["annotations"];

    return result;
}
#endif /* GEARSF7000_ENABLE_MCP */
