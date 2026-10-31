/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2021  Ignacio Sanchez
 * Copyright (C) 2024-2026  Saverio Russo
 *
 * Derived from Gearcoleco by Ignacio Sanchez. His code is free software: you
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

#include <math.h>
#include <inttypes.h>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <cfloat>
#include <unordered_map>
#include "imgui/imgui.h"
#include "imgui/colors.h"
#include "imgui/fonts/IconsMaterialDesign.h"
#include "config.h"
#include "emu.h"
#if GEARSF7000_ENABLE_MCP
#include "mcp/mcp_manager.h"
#endif
#include "renderer.h"
#include "crt_signal_capture_loader.h"
#include "gui_debug_audio.h"
#include "gui_debug_events.h"
#include "gui_debug_rom_map.h"
#include "gui_debug_disassembler_view.h"
#include "gui_debug_deep_search.h"
#include "gui_debug_palettes.h"
#include "../../src/gearsf7000.h"
#include "../../src/SystemBusMSX.h"
#if GEARSF7000_ENABLE_SF7000
#include "../../src/SF7000.h"
#endif
#if GEARSF7000_ENABLE_SR1000
#include "../../src/SR1000.h"
#endif
#include "gui.h"
#include "gui_debug_constants.h"
#include "gui_debug_memory.h"
#include "gui_debug_mem_import.h"
#include "gui_debug_basic_typer.h"
#include "gui_debug_disassembler_export.h"
#include "gui_debug_rom_inspector.h"
#include "gui_debug_nametable.h"
#include "gui_debug_pattern_table.h"
#include "gui_debug_sprites.h"
#include "gui_debug_video_registers.h"
#if GEARSF7000_ENABLE_SF7000
#include "gui_debug_disc_explorer.h"
#endif
#include "gui_debug_rewind.h"
#include "nfd_parented.h"

class SF7000;

#define GUI_DEBUG_IMPORT
#include "gui_debug.h"

struct DebugSymbol
{
    int bank;
    u16 address;
    std::string text;
};

struct DisassemblerLine
{
    bool is_symbol;
    bool is_breakpoint;
    Memory::stDisassembleRecord* record;
    std::string symbol;
};

static std::vector<DebugSymbol> symbols;
static Memory::stDisassembleRecord* selected_record = NULL;
static char brk_address_cpu[5] = "";
static char brk_address_mem[10] = "";
static bool brk_new_mem_read = true;
static bool brk_new_mem_write = true;


static char goto_address[5] = "";
static bool goto_address_requested = false;
static u16 goto_address_target = 0;
static bool goto_back_requested = false;
static int goto_back = 0;

namespace
{
// Same idea as gui.cpp's own ScopedFileDialogAudioPause - not exposed via a
// header, small enough to duplicate rather than plumb a shared one through.
struct ScopedDebugFileDialogAudioPause
{
    ScopedDebugFileDialogAudioPause() { emu_audio_suspend_for_file_dialog(); }
    ~ScopedDebugFileDialogAudioPause() { emu_audio_resume_after_file_dialog(); }
    ScopedDebugFileDialogAudioPause(const ScopedDebugFileDialogAudioPause&) = delete;
    ScopedDebugFileDialogAudioPause& operator=(const ScopedDebugFileDialogAudioPause&) = delete;
};

void save_disassembly_with_dialog(bool full, const char* default_name)
{
    ScopedDebugFileDialogAudioPause audioPause;
    nfdchar_t* out_path = nullptr;
    nfdresult_t result = NFD_SaveDialogParented(&out_path, nullptr, 0, nullptr, default_name);
    if (result == NFD_OKAY)
    {
        gui_debug_save_disassembly(out_path, full);
        NFD_FreePath(out_path);
    }
    else if (result != NFD_CANCEL)
    {
        Log("Save Disassembly Error: %s", NFD_GetError());
    }
}
}

static void debug_window_processor(void);
static void debug_window_memory(void);
static void debug_window_disassembler(void);
#if GEARSF7000_ENABLE_SF7000
static void debug_window_sf7000(void);
#endif
static void debug_window_crt_test_pass(void);
static void debug_window_composite_decode_test(void);
static void debug_window_crt_consumer_test(void);
static void debug_window_crt_lottes_test(void);
#if GEARSF7000_ENABLE_MCP
static void debug_window_mcp_server(void);
static void debug_window_watch_monitor(void);
#endif


#if GEARSF7000_ENABLE_SR1000
static void debug_window_cassette(void);
static void debug_window_cassette_controls(void);
#endif

static void add_symbol(const char* line);
static void add_breakpoint_cpu(void);
static void add_breakpoint_mem(void);

static bool is_return_instruction(u8 opcode1, u8 opcode2);
static void draw_disassembler_instruction(const char* instruction, const ImVec4& fallback,
                                          bool highlight);

static u8 high_byte(u16 value) { return static_cast<u8>(value >> 8); }
static u8 low_byte(u16 value) { return static_cast<u8>(value); }

static void draw_binary_bits(u8 value, bool space_every_bit = false)
{
    ImGui::BeginGroup();
    for (int bit = 7; bit >= 0; --bit)
    {
        if (bit != 7 && (space_every_bit || bit == 3))
        {
            ImGui::TextUnformatted(" ");
            ImGui::SameLine(0.0f, 0.0f);
        }
        ImGui::TextColored((value & (1u << bit)) ? green : gray, "%d",
                           (value >> bit) & 1u);
        if (bit != 0)
            ImGui::SameLine(0.0f, 0.0f);
    }
    ImGui::EndGroup();
}

static void draw_binary_word(u16 value)
{
    ImGui::BeginGroup();
    draw_binary_bits(high_byte(value));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextUnformatted(" ");
    ImGui::SameLine(0.0f, 0.0f);
    draw_binary_bits(low_byte(value));
    ImGui::EndGroup();
}

static void draw_disassembler_instruction(const char* instruction, const ImVec4& fallback,
                                          bool highlight)
{
    // The decoder uses the same compact semantic markers as GearSystem:
    // {n}=mnemonic, {o}=operand, {e}=relative-offset annotation, {s}=symbol.
    // Rendering
    // them here keeps the Z80 service independent from Dear ImGui.
    const ImVec4 operand = ImVec4(0.76f, 0.57f, 0.34f, 1.0f);
    ImVec4 current = fallback;
    const char* part = instruction;
    bool emitted = false;

    auto emit = [&](const char* end)
    {
        if (end == part)
            return;
        if (emitted)
            ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(current, "%.*s", static_cast<int>(end - part), part);
        emitted = true;
    };

    for (const char* cursor = instruction; *cursor != 0;)
    {
        if (cursor[0] == '{' && cursor[2] == '}' &&
            (cursor[1] == 'n' || cursor[1] == 'o' || cursor[1] == 'e' || cursor[1] == 's'))
        {
            emit(cursor);
            if (!highlight)
            {
                if (cursor[1] == 'n') current = white;
                else if (cursor[1] == 'o') current = operand;
                else current = blue;
                if (cursor[1] == 's') current = green;
            }
            part = cursor + 3;
            cursor += 3;
            continue;
        }
        ++cursor;
    }
    emit(part + std::strlen(part));
}

void gui_debug_draw_instruction(const char* instruction, const ImVec4& fallback,
                                bool highlight)
{
    draw_disassembler_instruction(instruction, fallback, highlight);
}

void gui_debug_windows(void)
{

    // Open debugger windows follow the active hardware composition. A pending
    // machine selection changes nothing until Reset; after the safe-point no
    // panel may keep polling a connector or device that is no longer present.
    GearSF7000Core* core = emu_get_core();
#if GEARSF7000_ENABLE_SR1000
    if (!core->HasCassetteInterface())
        config_debug.show_cassette = false;
#endif
    if (!core->HasKeyboardInterface())
        config_debug.show_ppi_registers = false;
    if (core->GetMachineType() == MachineType::MSX) {
        config_debug.show_psg = false;
        config_debug.show_ym2413 = false;
    }
#if GEARSF7000_ENABLE_SF7000
    if (!IsSF7000System(core->GetSystemModel()))
    {
        config_debug.show_sf7000 = false;
        config_debug.show_disc_explorer = false;
    }
#endif

#if GEARSF7000_ENABLE_YM2413
    core->GetAudio()->GetYm2413()->EnableDebug(config_debug.debug && config_debug.show_ym2413);
#endif
    // Unconditional, not just inside the PSG window: the tap has to switch
    // off the moment the panel closes or debug mode does, not linger armed.
    emu_get_core()->GetAudio()->EnablePsgDebug(config_debug.debug && config_debug.show_psg);
#if GEARSF7000_ENABLE_AY
    emu_get_core()->GetAudio()->EnableAyDebug(config_debug.debug && config_debug.show_ay);
#endif

#if GEARSF7000_ENABLE_SR1000
    if (config_debug.show_cassette)
        debug_window_cassette();
#endif
    if (config_debug.debug && config_debug.show_rewind)
        gui_debug_window_rewind();

    // These passes used to remain latched on when their windows stopped
    // being drawn. Ordinary video postprocessing is independent of them.
    if (!config_debug.debug)
    {
        renderer_set_crt_test_enabled(false);
        renderer_set_composite_decode_enabled(false);
        renderer_set_crt_consumer_enabled(false);
        renderer_set_crt_lottes_enabled(false);
    }

#ifdef DEBUG_TOOLS
    if (config_debug.debug)
    {
        if (config_debug.show_processor)
            debug_window_processor();
        if (config_debug.show_memory)
            debug_window_memory();
        // Owned by the memory editors but drawn as their own windows, so they
        // are pumped here rather than from inside the editor.
        gui_debug_memory_watches_window();
        gui_debug_memory_search_window();
        gui_debug_memory_find_bytes_window();
        if (config_debug.show_memory_import)
            gui_debug_mem_import_window();
        if (config_debug.show_basic_typer)
            gui_debug_basic_typer_window();
        if (config_debug.show_rom_inspector)
            gui_debug_rom_inspector_window();

#if GEARSF7000_ENABLE_SF7000
        if (config_debug.show_disc_explorer)
            gui_debug_disc_explorer_window();
#endif
        if (config_debug.show_disassembler)
            debug_window_disassembler();
        if (config_debug.show_disassembler_new)
            gui_debug_disassembler_view_window();
        if (config_debug.show_deep_search)
            gui_debug_deep_search_window();
        if (config_debug.show_to_check)
            gui_debug_to_check_window();
        if (config_debug.show_palettes)
            gui_debug_palettes_window();
        if (config_debug.show_pattern_table)
            debug_window_pattern_table();
        if (config_debug.show_sprites)
            debug_window_sprites();
        if (config_debug.show_nametable)
            debug_window_nametable();
        if (config_debug.show_video_register_explorer)
            gui_debug_video_registers_window();
        if (config_debug.show_ppi_registers && core->GetMachineType() == MachineType::MSX)
        {
            const auto& bus = *static_cast<const SystemBusMSX*>(core->GetSystemBus());
            const auto ppi = bus.PPI().GetSnapshot();
            gui_set_next_window_size_em(23.3f, 17.8f);
            if (ImGui::Begin("MSX PPI 8255A", &config_debug.show_ppi_registers))
            {
                ImGui::Text("Control $AB: $%02X", ppi.control);
                for (unsigned port = 0; port < 3; ++port)
                    ImGui::Text("%c ($%02X): latch $%02X  pins $%02X  output mask $%02X",
                        'A' + port, 0xA8 + port, ppi.outputLatch[port],
                        bus.PPI().PeekRegister(port), ppi.drivenMask[port]);
                ImGui::Separator();
                ImGui::Text("Primary slots: $%02X", bus.Slots().GetPrimarySlots());
                ImGui::Text("Keyboard row: %u, active-low input $%02X", bus.KeyboardRow(),
                    bus.Keyboard().ReadRow(bus.KeyboardRow()));
                ImGui::Text("PC7 keyboard click: %s", bus.KeyboardClickLevel() ? "HIGH" : "LOW");
                ImGui::TextUnformatted("International keyboard matrix (active low):");
                for (unsigned row = 0; row < MSXKeyboard::RowCount; ++row) {
                    ImGui::Text("%u: %02X", row, bus.Keyboard().ReadRow(row));
                    if (row % 4 != 3 && row + 1 != MSXKeyboard::RowCount) ImGui::SameLine();
                }
            }
            ImGui::End();
        }
        if (config_debug.show_psg)
            gui_debug_psg_window();
#if GEARSF7000_ENABLE_AY
        if (config_debug.show_ay)
            gui_debug_ay_window();
#endif
#if GEARSF7000_ENABLE_YM2413
        if (config_debug.show_ym2413)
            gui_debug_ym2413_window();
#endif
#if GEARSF7000_ENABLE_SF7000
        if (config_debug.show_sf7000)
            debug_window_sf7000();
#endif
        if (config_debug.show_events)
            gui_debug_events_window();
        if (config_debug.show_rom_map)
            gui_debug_rom_map_window();
        if (config_debug.show_crt_test_pass)
            debug_window_crt_test_pass();
        if (config_debug.show_composite_decode_test)
            debug_window_composite_decode_test();
        if (config_debug.show_crt_consumer_test)
            debug_window_crt_consumer_test();
        if (config_debug.show_crt_lottes_test)
            debug_window_crt_lottes_test();
#if GEARSF7000_ENABLE_MCP
        if (config_debug.show_mcp_server)
            debug_window_mcp_server();
        if (config_debug.show_watch_monitor)
            debug_window_watch_monitor();
#endif
    }
#endif
}

void gui_debug_reset(void)
{
    gui_debug_reset_breakpoints_cpu();
    gui_debug_reset_breakpoints_mem();
    gui_debug_reset_symbols();
    selected_record = NULL;
}

void gui_debug_reset_symbols(void)
{
    symbols.clear();
    
    GearSF7000Core* core = emu_get_core();
    Cartridge* cart = core->GetCartridge();

    if (core->GetMemory()->IsSF7000Enabled() || cart->IsSF7000IPL())
    {
        for (int i = 0; i < gui_debug_symbols_count; i++)
            add_symbol(gui_debug_symbols[i]);
    }
}

int gui_debug_symbol_count(void)
{
    return (int)symbols.size();
}

bool gui_debug_get_symbol(int index, int* bank, u16* address, const char** text)
{
    if (index < 0 || index >= (int)symbols.size())
        return false;
    const DebugSymbol& s = symbols[index];
    if (bank) *bank = s.bank;
    if (address) *address = s.address;
    if (text) *text = s.text.c_str();
    return true;
}

void gui_debug_set_symbol(int bank, u16 address, const char* text)
{
    if (text == NULL)
        return;

    // One name per (bank, address): overwrite rather than append, so adding
    // twice does not leave the disassembler with two labels on one line and
    // the second one unreachable.
    for (DebugSymbol& s : symbols)
    {
        if (s.bank == bank && s.address == address)
        {
            s.text = text;
            return;
        }
    }

    DebugSymbol s;
    s.bank = bank;
    s.address = address;
    s.text = text;
    symbols.push_back(s);
}

bool gui_debug_remove_symbol(int bank, u16 address)
{
    for (std::size_t i = 0; i < symbols.size(); i++)
    {
        if (symbols[i].bank == bank && symbols[i].address == address)
        {
            symbols.erase(symbols.begin() + i);
            return true;
        }
    }
    return false;
}

int gui_debug_load_symbols_file_counted(const char* path)
{
    if (path == NULL)
        return -1;

    // Reported rather than inferred: a .sym file whose lines the parser all
    // rejects loads "successfully" and changes nothing, which is exactly the
    // silent failure a caller needs told about. The count is the difference,
    // so it is right whatever the parser skips.
    {
        std::ifstream probe(path);
        if (!probe.is_open())
            return -1;
    }

    const int before = (int)symbols.size();
    gui_debug_load_symbols_file(path);
    return (int)symbols.size() - before;
}

void gui_debug_load_symbols_file(const char* path)
{
    Log("Loading symbol file %s", path);

    std::ifstream file(path);

    if (file.is_open())
    {
        std::string line;
        bool valid_section = true;

        while (std::getline(file, line))
        {
            size_t comment = line.find_first_of(';');
            if (comment != std::string::npos)
                line = line.substr(0, comment);
            line = line.erase(0, line.find_first_not_of(" \t\r\n"));
            line = line.erase(line.find_last_not_of(" \t\r\n") + 1);

            if (line.empty())
                continue;

            if (line.find("[") != std::string::npos)
            {
                valid_section = (line.find("[labels]") != std::string::npos);
                continue;
            }

            if (valid_section)
                add_symbol(line.c_str());
        }

        file.close();
    }
}

void gui_debug_toggle_breakpoint(void)
{
    if (IsValidPointer(selected_record))
    {
        bool found = false;
        std::vector<Memory::stDisassembleRecord*>* breakpoints = emu_get_core()->GetMemory()->GetBreakpointsCPU();

        for (long unsigned int b = 0; b < breakpoints->size(); b++)
        {
            if ((*breakpoints)[b] == selected_record)
            {
                found = true;
                 InitPointer((*breakpoints)[b]);
                break;
            }
        }

        if (!found)
        {
            breakpoints->push_back(selected_record);
        }
    }
}

void gui_debug_runtocursor(void)
{
    if (IsValidPointer(selected_record))
    {
        emu_get_core()->GetMemory()->ArmRunToAddress(selected_record->address);
        emu_debug_continue();
    }
}

void gui_debug_reset_breakpoints_cpu(void)
{
    emu_get_core()->GetMemory()->GetBreakpointsCPU()->clear();
    brk_address_cpu[0] = 0;
}

void gui_debug_reset_breakpoints_mem(void)
{
    emu_get_core()->GetMemory()->GetBreakpointsMem()->clear();
    brk_address_mem[0] = 0;
}

void gui_debug_go_back(void)
{
    goto_back_requested = true;
}

void gui_debug_copy_memory(void)
{
    gui_debug_memory_copy();
}

void gui_debug_paste_memory(void)
{
    gui_debug_memory_paste();
}



static void debug_window_memory(void)
{
    gui_debug_memory_window();
}

static void debug_window_disassembler(void)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::SetNextWindowPos(ImVec2(159, 31), ImGuiCond_FirstUseEver);
    gui_set_next_window_size_em(13.8f, 30.9f);

    ImGui::Begin("Disassembler", &config_debug.show_disassembler, ImGuiWindowFlags_MenuBar);

    GearSF7000Core* core = emu_get_core();
    const CpuStateSnapshot proc_state =
        core->GetCpuStateAccess()->GetCpuStateSnapshot();
    Memory* memory = core->GetMemory();
    std::vector<Memory::stDisassembleRecord*>* breakpoints_cpu = memory->GetBreakpointsCPU();
    std::vector<Memory::stMemoryBreakpoint>* breakpoints_mem = memory->GetBreakpointsMem();

    int pc = proc_state.pc;

    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Save Disassembly (Visible)..."))
                save_disassembly_with_dialog(false, "disassembly.asm");
            if (ImGui::MenuItem("Save Disassembly (Full)..."))
                save_disassembly_with_dialog(true, "disassembly_full.asm");
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Follow PC", NULL, &config_debug.dis_follow_pc);
            ImGui::Separator();
            ImGui::MenuItem("Opcodes", NULL, &config_debug.dis_show_opcodes);
            ImGui::MenuItem("Symbols", NULL, &config_debug.dis_show_symbols);
            ImGui::MenuItem("Segments", NULL, &config_debug.dis_show_segments);
            ImGui::MenuItem("Bank", NULL, &config_debug.dis_show_bank);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Go"))
        {
            if (ImGui::MenuItem("Back"))
            {
                goto_back_requested = true;
                config_debug.dis_follow_pc = false;
            }
            if (ImGui::MenuItem("Go To PC"))
            {
                gui_debug_request_goto_address(static_cast<u16>(pc));
                config_debug.dis_follow_pc = false;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Run"))
        {
            if (ImGui::MenuItem("Continue")) emu_debug_continue();
            if (ImGui::MenuItem("Pause")) emu_pause();
            ImGui::Separator();
            if (ImGui::MenuItem("Step Into")) emu_debug_step_into();
            if (ImGui::MenuItem("Step Over")) emu_debug_step_over();
            if (ImGui::MenuItem("Step Line")) emu_debug_step_line();
            if (ImGui::MenuItem("Step Frame")) emu_debug_next_frame();
            if (ImGui::MenuItem("Run To Cursor")) gui_debug_runtocursor();
            ImGui::Separator();
            if (ImGui::MenuItem("Reset")) gui_shortcut(gui_ShortcutReset);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    auto toolbar_button = [](const char* icon, const char* id, const char* tooltip)
    {
        char label[48];
        snprintf(label, sizeof(label), "%s##%s", icon, id);
        const bool pressed = ImGui::Button(label);
        // Do not use the deferred ItemTooltip helper here: the toolbar lives
        // in a compact child window and we want the description immediately.
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            // BeginTooltip inherits the current font stack.  Pop the icon
            // font before opening it, otherwise the description has no ASCII
            // glyphs and appears as an empty black rectangle.
            ImGui::PopFont();
            if (ImGui::BeginTooltip())
            {
                ImGui::TextUnformatted(tooltip);
                ImGui::EndTooltip();
            }
            ImGui::PushFont(gui_material_icons_font, 20.0f);
        }
        ImGui::SameLine();
        return pressed;
    };

    ImGui::PushFont(gui_material_icons_font, 20.0f);
    if (toolbar_button(ICON_MD_PLAY_ARROW, "debug_continue", "Start / Continue")) emu_debug_continue();
    if (toolbar_button(ICON_MD_PAUSE, "debug_pause", "Pause")) emu_pause();
    if (toolbar_button(ICON_MD_REDO, "debug_step_over", "Step Over")) emu_debug_step_over();
    if (toolbar_button(ICON_MD_ARROW_DROP_DOWN_CIRCLE, "debug_step_into", "Step Into")) emu_debug_step_into();
    if (toolbar_button(ICON_MD_VERTICAL_ALIGN_BOTTOM, "debug_step_line", "Step Line")) emu_debug_step_line();
    if (toolbar_button(ICON_MD_INPUT, "debug_step_frame", "Step Frame")) emu_debug_next_frame();
    if (toolbar_button(ICON_MD_KEYBOARD_TAB, "debug_run_to_cursor", "Run To Cursor")) gui_debug_runtocursor();
    if (toolbar_button(ICON_MD_REPLAY, "debug_reset", "Reset")) gui_shortcut(gui_ShortcutReset);
    if (config_debug.dis_follow_pc)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.48f, 0.90f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.56f, 1.00f, 1.0f));
    }
    const bool toggle_follow_pc = toolbar_button(ICON_MD_GPS_FIXED, "debug_follow_pc",
                                                  "Follow Program Counter");
    if (config_debug.dis_follow_pc)
        ImGui::PopStyleColor(2);
    if (toggle_follow_pc)
        config_debug.dis_follow_pc = !config_debug.dis_follow_pc;
    ImGui::PopFont();
    ImGui::TextColored(emu_is_execution_stopped() ? red : green,
                       emu_is_execution_stopped() ? "PAUSED" : "RUNNING");

    const bool& follow_pc = config_debug.dis_follow_pc;
    const bool& show_mem = config_debug.dis_show_opcodes;
    const bool& show_symbols = config_debug.dis_show_symbols;
    const bool& show_segment = config_debug.dis_show_segments;
    const bool& show_bank = config_debug.dis_show_bank;

    ImGui::Separator();

    ImGui::Text("Go To Address: ");
    ImGui::SameLine();
    ImGui::PushItemWidth(45);
    if (ImGui::InputTextWithHint("##goto_address", "XXXX", goto_address, IM_ARRAYSIZE(goto_address), ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue))
    {
        try
        {
            gui_debug_request_goto_address((u16)std::stoul(goto_address, 0, 16));
            config_debug.dis_follow_pc = false;
        }
        catch(const std::invalid_argument&)
        {
        }
        goto_address[0] = 0;
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Go", ImVec2(30, 0)))
    {
        try
        {
            gui_debug_request_goto_address((u16)std::stoul(goto_address, 0, 16));
            config_debug.dis_follow_pc = false;
        }
        catch(const std::invalid_argument&)
        {
        }
        goto_address[0] = 0;
    }

    ImGui::SameLine();
    if (ImGui::Button("Back", ImVec2(50, 0)))
    {
        goto_back_requested = true;
        config_debug.dis_follow_pc = false;
    }

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Processor Breakpoints"))
    {
        ImGui::Checkbox("Disable All##disable_all_cpu", &emu_debug_disable_breakpoints_cpu);

        ImGui::Columns(2, "breakpoints_cpu");
        ImGui::SetColumnOffset(1, 85);

        ImGui::Separator();

        if (IsValidPointer(selected_record))
            snprintf(brk_address_cpu, sizeof(brk_address_cpu), "%04X",
                selected_record->address);

        ImGui::PushItemWidth(70);
        if (ImGui::InputTextWithHint("##add_breakpoint_cpu", "XXXX", brk_address_cpu, IM_ARRAYSIZE(brk_address_cpu), ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue))
        {
            add_breakpoint_cpu();
        }
        ImGui::PopItemWidth();


        if (ImGui::Button("Add##add_cpu", ImVec2(70, 0)))
        {
            add_breakpoint_cpu();
        }

        if (ImGui::Button("Clear All##clear_all_cpu", ImVec2(70, 0)))
        {
            gui_debug_reset_breakpoints_cpu();
        }

        ImGui::NextColumn();

        ImGui::BeginChild("breakpoints_cpu", ImVec2(0, 80), false);

        int remove = -1;

        for (long unsigned int b = 0; b < breakpoints_cpu->size(); b++)
        {
            if (!IsValidPointer((*breakpoints_cpu)[b]))
                continue;

            ImGui::PushID(b);
            if (ImGui::SmallButton("X"))
            {
               remove = b;
               ImGui::PopID();
               continue;
            }

            ImGui::PopID();

            ImGui::PushFont(gui_default_font, gui_get_default_font_size());
            ImGui::SameLine();
            ImGui::TextColored(red, "%04X", (*breakpoints_cpu)[b]->address);
            ImGui::SameLine();
            ImGui::TextColored(gray, "%s", (*breakpoints_cpu)[b]->name);
            ImGui::PopFont();
        }

        if (remove >= 0)
        {
            breakpoints_cpu->erase(breakpoints_cpu->begin() + remove);
        }

        ImGui::EndChild();
        ImGui::Columns(1);
        ImGui::Separator();

    }

    if (ImGui::CollapsingHeader("Memory Breakpoints"))
    {
        ImGui::Checkbox("Disable All##diable_all_mem", &emu_debug_disable_breakpoints_mem);

        ImGui::Columns(2, "breakpoints_mem");
        ImGui::SetColumnOffset(1, 100);

        ImGui::Separator();

        ImGui::PushItemWidth(85);
        if (ImGui::InputTextWithHint("##add_breakpoint_mem", "XXXX-XXXX", brk_address_mem, IM_ARRAYSIZE(brk_address_mem), ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue))
        {
            add_breakpoint_mem();
        }
        ImGui::PopItemWidth();

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Use XXXX format for single addresses or XXXX-XXXX for address ranges");

        ImGui::Checkbox("Read", &brk_new_mem_read);
        ImGui::Checkbox("Write", &brk_new_mem_write);

        if (ImGui::Button("Add##add_mem", ImVec2(85, 0)))
        {
            add_breakpoint_mem();
        }

        if (ImGui::Button("Clear All##clear_all_mem", ImVec2(85, 0)))
        {
            gui_debug_reset_breakpoints_mem();
        }

        ImGui::NextColumn();

        ImGui::BeginChild("breakpoints_mem", ImVec2(0, 130), false);

        int remove = -1;

        for (long unsigned int b = 0; b < breakpoints_mem->size(); b++)
        {
            ImGui::PushID(10000 + b);
            if (ImGui::SmallButton("X"))
            {
               remove = b;
               ImGui::PopID();
               continue;
            }

            ImGui::PopID();

            ImGui::PushFont(gui_default_font, gui_get_default_font_size());
            ImGui::SameLine();
            if ((*breakpoints_mem)[b].range)
                ImGui::TextColored(red, "%04X-%04X", (*breakpoints_mem)[b].address1, (*breakpoints_mem)[b].address2);
            else
                ImGui::TextColored(red, "%04X", (*breakpoints_mem)[b].address1);
            if ((*breakpoints_mem)[b].read)
            {
                ImGui::SameLine(); ImGui::TextColored(gray, "R");
            }
            if ((*breakpoints_mem)[b].write)
            {
                ImGui::SameLine(); ImGui::TextColored(gray, "W");
            }
            ImGui::PopFont();
        }

        if (remove >= 0)
        {
            breakpoints_mem->erase(breakpoints_mem->begin() + remove);
        }

        ImGui::EndChild();
        ImGui::Columns(1);
        ImGui::Separator();
    }

    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    bool window_visible = ImGui::BeginChild("##dis", ImVec2(ImGui::GetContentRegionAvail().x, 0), true,
                                            ImGuiWindowFlags_HorizontalScrollbar);
    
    if (window_visible)
    {
        int dis_size = 0;
        int pc_pos = 0;
        int goto_address_pos = 0;
        
        std::vector<DisassemblerLine> vec(0x10000);
        
        for (int i = 0; i < 0x10000; i++)
        {
            Memory::stDisassembleRecord* record = memory->GetDisassembleRecord(i, false);

            if (IsValidPointer(record) && (record->name[0] != 0))
            {
                bool explicit_symbol = false;
                for (long unsigned int s = 0; s < symbols.size(); s++)
                {
                    if ((symbols[s].bank == record->bank) && (symbols[s].address == i) && show_symbols)
                    {
                        vec[dis_size].is_symbol = true;
                        vec[dis_size].symbol = symbols[s].text;
                        dis_size ++;
                        explicit_symbol = true;
                        break;
                    }
                }

                if (!explicit_symbol && show_symbols && record->auto_symbol[0] != 0)
                {
                    vec[dis_size].is_symbol = true;
                    vec[dis_size].symbol = record->auto_symbol;
                    dis_size ++;
                }

                vec[dis_size].is_symbol = false;
                vec[dis_size].record = record;

                if (vec[dis_size].record->address == pc)
                    pc_pos = dis_size;

                if (goto_address_requested && (vec[dis_size].record->address <= goto_address_target))
                    goto_address_pos = dis_size;

                vec[dis_size].is_breakpoint = false;

                for (long unsigned int b = 0; b < breakpoints_cpu->size(); b++)
                {
                    if ((*breakpoints_cpu)[b] == vec[dis_size].record)
                    {
                        vec[dis_size].is_breakpoint = true;
                        break;
                    }
                }

                dis_size++;
            }
        }

        if (follow_pc)
        {
            float window_offset = ImGui::GetWindowHeight() / 2.0f;
            float offset = window_offset - (ImGui::GetTextLineHeightWithSpacing() - 2.0f);
            ImGui::SetScrollY((pc_pos * ImGui::GetTextLineHeightWithSpacing()) - offset);
        }

        if (goto_address_requested)
        {
            goto_address_requested = false;
            goto_back = (int)ImGui::GetScrollY();
            ImGui::SetScrollY((goto_address_pos * ImGui::GetTextLineHeightWithSpacing()) + 2);
        }

        if (goto_back_requested)
        {
            goto_back_requested = false;
            ImGui::SetScrollY((float)goto_back);
        }

        ImGuiListClipper clipper;
        clipper.Begin(dis_size, ImGui::GetTextLineHeightWithSpacing());

        while (clipper.Step())
        {
            for (int item = clipper.DisplayStart; item < clipper.DisplayEnd; item++)
            {
                if (vec[item].is_symbol)
                {
                    ImGui::TextColored(green, "%s:", vec[item].symbol.c_str());
                    continue;
                }

                ImGui::PushID(item);

                bool is_selected = (selected_record == vec[item].record);

                if (ImGui::Selectable("", is_selected, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    if (ImGui::IsMouseDoubleClicked(0) && vec[item].record->jump)
                    {
                        config_debug.dis_follow_pc = false;
                        gui_debug_request_goto_address(vec[item].record->jump_address);
                    }
                    else if (is_selected)
                    {
                        InitPointer(selected_record);
                        brk_address_cpu[0] = 0;
                    }
                    else
                        selected_record = vec[item].record;
                }

                if (is_selected)
                    ImGui::SetItemDefaultFocus();

                ImVec4 color_segment = vec[item].is_breakpoint ? red : magenta;
                ImVec4 color_bank = vec[item].is_breakpoint ? red : violet;
                ImVec4 color_addr = vec[item].is_breakpoint ? red : cyan;
                ImVec4 color_opcodes = vec[item].is_breakpoint ? red : gray;
                ImVec4 color_name = vec[item].is_breakpoint ? red : white;

                if (show_segment)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(color_segment, "%s", vec[item].record->segment);
                }

                if (show_bank && memory->HasMapper())
                {
                    ImGui::SameLine();
                    const int display_bank = std::max(0, vec[item].record->bank);
                    ImGui::TextColored(color_bank, "%02X", display_bank);
                }

                ImGui::SameLine();
                ImGui::TextColored(color_addr, "%04X", vec[item].record->address);


                ImGui::SameLine();
                if (vec[item].record->address == pc)
                {
                    ImGui::TextColored(yellow, "->");
                    color_name = yellow;
                }
                else
                {
                    ImGui::TextColored(yellow, "  ");
                }

                ImGui::SameLine();
                const bool highlight_instruction = vec[item].is_breakpoint ||
                                                   vec[item].record->address == pc;
                std::string instruction = vec[item].record->name;
                if (show_symbols && vec[item].record->jump)
                {
                    Memory::stDisassembleRecord* target = memory->GetDisassembleRecord(
                        vec[item].record->jump_address, false);
                    const char* resolved_name = NULL;
                    int target_bank = IsValidPointer(target) ? target->bank : 0;

                    for (const DebugSymbol& symbol : symbols)
                    {
                        if (symbol.bank == target_bank &&
                            symbol.address == vec[item].record->jump_address)
                        {
                            resolved_name = symbol.text.c_str();
                            break;
                        }
                    }
                    if (!resolved_name && IsValidPointer(target) && target->auto_symbol[0] != 0)
                        resolved_name = target->auto_symbol;

                    if (resolved_name)
                    {
                        char address_text[8];
                        std::snprintf(address_text, sizeof(address_text), "$%04X",
                                      vec[item].record->jump_address);
                        const std::size_t pos = instruction.rfind(address_text);
                        if (pos != std::string::npos)
                            instruction.replace(pos, std::strlen(address_text),
                                                std::string("{s}") + resolved_name);
                    }
                }
                draw_disassembler_instruction(instruction.c_str(), color_name,
                                               highlight_instruction);

                if (show_mem)
                {
                    // This is deliberately a fixed column, not a responsive
                    // right edge.  A debugger must preserve line geometry:
                    // on a narrow window the line is clipped and revealed by
                    // the horizontal scrollbar instead of being compressed.
                    // Derive the fixed comment column from the active ImGui
                    // font.  No physical-pixel constants: this has the same
                    // logical geometry on Windows, macOS/Retina and a
                    // scaled Linux desktop.
                    const float line_start_x = ImGui::GetCursorStartPos().x;
                    const float address_columns = ImGui::CalcTextSize(
                        "ROM 00 0000 -> ").x;
                    const float widest_instruction = ImGui::CalcTextSize(
                        "JP NZ,TAG_00_FFFF  [+127]").x;
                    const float k_opcode_column_x = line_start_x + address_columns +
                                                     widest_instruction + 2.0f * ImGui::GetStyle().ItemSpacing.x;
                    ImGui::SameLine(k_opcode_column_x, 0.0f);
                    ImGui::TextColored(color_opcodes, ";%s", vec[item].record->bytes);
                    // Reserve the whole normal line width even when the
                    // current instruction is short, forcing a useful
                    // horizontal scrollbar in a narrow window.
                    ImGui::SameLine(k_opcode_column_x + ImGui::CalcTextSize("; DD CB 7F 00").x, 0.0f);
                    ImGui::Dummy(ImVec2(1.0f, 1.0f));
                }

                bool is_ret = is_return_instruction(vec[item].record->opcodes[0], vec[item].record->opcodes[1]);
                if (is_ret)
                {
                    ImGui::PushStyleColor(ImGuiCol_Separator, (vec[item].record->opcodes[0] == 0xC9) ? gray : dark_gray);
                    ImGui::Separator();
                    ImGui::PopStyleColor();
                }

                ImGui::PopID();
            }
        }
    }

    ImGui::EndChild();
    
    ImGui::PopFont();

    ImGui::End();

    ImGui::PopStyleVar();
}

// Previous full-height layout retained for reference, not drawn alongside
// the compact replacement. Same CPU-neutral snapshot, no alternate state.
[[maybe_unused]] static void debug_window_processor_legacy(void)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::SetNextWindowPos(ImVec2(6, 31), ImGuiCond_FirstUseEver);

    ImGui::Begin("Z80 Status", &config_debug.show_processor, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize);

    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    GearSF7000Core* core = emu_get_core();
    CpuStateAccess* processor = core->GetCpuStateAccess();
    const CpuStateSnapshot proc_state = processor->GetCpuStateSnapshot();


    const std::uint64_t elapsed_tS = processor->GetElapsedTStates();
    const int z80_clock_hz = core->GetClockDomains().GetRates().cpuHz;
    long double elapsed_uS = (long double)elapsed_tS / ((long double)z80_clock_hz / 1000000.0L);
    long double elapsed_mS = elapsed_uS / 1000;

    char elapsed_tk_text[32];
    char elapsed_us_text[48];
    char elapsed_ms_text[48];
    snprintf(elapsed_tk_text, sizeof(elapsed_tk_text), "TK: %016" PRIu64, elapsed_tS);
    snprintf(elapsed_us_text, sizeof(elapsed_us_text), "uS: %Lf", elapsed_uS);
    snprintf(elapsed_ms_text, sizeof(elapsed_ms_text), "mS: %Lf", elapsed_mS);

    const float clock_row_start = ImGui::GetCursorPosX();
    const float clock_block_width = std::max({
        ImGui::CalcTextSize(elapsed_tk_text).x,
        ImGui::CalcTextSize(elapsed_us_text).x,
        ImGui::CalcTextSize(elapsed_ms_text).x
    });
    ImGui::TextColored(magenta, "CLOCK");
    const float reset_button_width = ImGui::CalcTextSize("RESET").x
        + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(clock_row_start + clock_block_width - reset_button_width);
    const bool reset_clock = ImGui::SmallButton("RESET##reset_clock");
    const bool reset_clock_hovered = ImGui::IsItemHovered();
    if (reset_clock_hovered)
        ImGui::SetTooltip("Reset clock counters");

    ImGui::TextUnformatted(elapsed_tk_text);
    ImGui::TextUnformatted(elapsed_us_text);
    ImGui::TextUnformatted(elapsed_ms_text);

    if (reset_clock)
    {
        try
        {
            processor->SetElapsedTStates(0);
        }
        catch (const std::invalid_argument&)
        {
        }
    }



    ImGui::Separator();

    ImGui::TextColored(orange, "  S Z Y H X P N C");
    ImGui::TextUnformatted("  "); ImGui::SameLine(0.0f, 0.0f);
    draw_binary_bits(low_byte(proc_state.af), true);

    ImGui::Columns(2, "registers");
    ImGui::Separator();
    ImGui::TextColored(cyan, " A"); ImGui::SameLine();
    ImGui::Text(" $%02X", high_byte(proc_state.af));
    draw_binary_bits(high_byte(proc_state.af));

    ImGui::NextColumn();
    ImGui::TextColored(cyan, " F"); ImGui::SameLine();
    ImGui::Text(" $%02X", low_byte(proc_state.af));
    draw_binary_bits(low_byte(proc_state.af));

    ImGui::NextColumn();
    ImGui::Separator();
    ImGui::TextColored(cyan, " A'"); ImGui::SameLine();
    ImGui::Text("$%02X", high_byte(proc_state.af2));
    draw_binary_bits(high_byte(proc_state.af2));

    ImGui::NextColumn();
    ImGui::TextColored(cyan, " F'"); ImGui::SameLine();
    ImGui::Text("$%02X", low_byte(proc_state.af2));
    draw_binary_bits(low_byte(proc_state.af2));

    ImGui::NextColumn();
    ImGui::Separator();
    ImGui::TextColored(cyan, " B"); ImGui::SameLine();
    ImGui::Text(" $%02X", high_byte(proc_state.bc));
    draw_binary_bits(high_byte(proc_state.bc));

    ImGui::NextColumn();
    ImGui::TextColored(cyan, " C"); ImGui::SameLine();
    ImGui::Text(" $%02X", low_byte(proc_state.bc));
    draw_binary_bits(low_byte(proc_state.bc));

    ImGui::NextColumn();
    ImGui::Separator();
    ImGui::TextColored(cyan, " B'"); ImGui::SameLine();
    ImGui::Text("$%02X", high_byte(proc_state.bc2));
    draw_binary_bits(high_byte(proc_state.bc2));

    ImGui::NextColumn();
    ImGui::TextColored(cyan, " C'"); ImGui::SameLine();
    ImGui::Text("$%02X", low_byte(proc_state.bc2));
    draw_binary_bits(low_byte(proc_state.bc2));

    ImGui::NextColumn();
    ImGui::Separator();
    ImGui::TextColored(cyan, " D"); ImGui::SameLine();
    ImGui::Text(" $%02X", high_byte(proc_state.de));
    draw_binary_bits(high_byte(proc_state.de));

    ImGui::NextColumn();
    ImGui::TextColored(cyan, " E"); ImGui::SameLine();
    ImGui::Text(" $%02X", low_byte(proc_state.de));
    draw_binary_bits(low_byte(proc_state.de));

    ImGui::NextColumn();
    ImGui::Separator();
    ImGui::TextColored(cyan, " D'"); ImGui::SameLine();
    ImGui::Text("$%02X", high_byte(proc_state.de2));
    draw_binary_bits(high_byte(proc_state.de2));

    ImGui::NextColumn();
    ImGui::TextColored(cyan, " E'"); ImGui::SameLine();
    ImGui::Text("$%02X", low_byte(proc_state.de2));
    draw_binary_bits(low_byte(proc_state.de2));

    ImGui::NextColumn();
    ImGui::Separator();
    ImGui::TextColored(cyan, " H"); ImGui::SameLine();
    ImGui::Text(" $%02X", high_byte(proc_state.hl));
    draw_binary_bits(high_byte(proc_state.hl));

    ImGui::NextColumn();
    ImGui::TextColored(cyan, " L"); ImGui::SameLine();
    ImGui::Text(" $%02X", low_byte(proc_state.hl));
    draw_binary_bits(low_byte(proc_state.hl));

    ImGui::NextColumn();
    ImGui::Separator();
    ImGui::TextColored(cyan, " H'"); ImGui::SameLine();
    ImGui::Text("$%02X", high_byte(proc_state.hl2));
    draw_binary_bits(high_byte(proc_state.hl2));

    ImGui::NextColumn();
    ImGui::TextColored(cyan, " L'"); ImGui::SameLine();
    ImGui::Text("$%02X", low_byte(proc_state.hl2));
    draw_binary_bits(low_byte(proc_state.hl2));

    ImGui::NextColumn();
    ImGui::Separator();
    ImGui::TextColored(cyan, " I"); ImGui::SameLine();
    ImGui::Text(" $%02X", proc_state.i);
    draw_binary_bits(proc_state.i);

    ImGui::NextColumn();
    ImGui::TextColored(cyan, " R"); ImGui::SameLine();
    ImGui::Text(" $%02X", proc_state.r);
    draw_binary_bits(proc_state.r);

    ImGui::NextColumn();
    ImGui::Columns(1);

    ImGui::Separator();
    ImGui::TextColored(yellow, "    IX"); ImGui::SameLine();
    ImGui::Text("= $%04X", proc_state.ix);
    draw_binary_word(proc_state.ix);

    ImGui::Separator();
    ImGui::TextColored(yellow, "    IY"); ImGui::SameLine();
    ImGui::Text("= $%04X", proc_state.iy);
    draw_binary_word(proc_state.iy);

    ImGui::Separator();
    ImGui::TextColored(yellow, "    WZ"); ImGui::SameLine();
    ImGui::Text("= $%04X", proc_state.wz);
    draw_binary_word(proc_state.wz);

    ImGui::Separator();
    ImGui::TextColored(yellow, "    SP"); ImGui::SameLine();
    ImGui::Text("= $%04X", proc_state.sp);
    draw_binary_word(proc_state.sp);

    ImGui::Separator();
    ImGui::TextColored(yellow, "    PC"); ImGui::SameLine();
    ImGui::Text("= $%04X", proc_state.pc);
    draw_binary_word(proc_state.pc);

    ImGui::Separator();

    ImGui::TextColored(proc_state.iff1 ? green : gray, " IFF1"); ImGui::SameLine();
    ImGui::TextColored(proc_state.iff2 ? green : gray, " IFF2"); ImGui::SameLine();
    ImGui::TextColored(proc_state.halted ? green : gray, " HALT");
    
    ImGui::TextColored(proc_state.intLine ? green : gray, "    INT"); ImGui::SameLine();
    ImGui::TextColored(proc_state.nmiLine ? green : gray, "  NMI");

    ImGui::PopFont();

    ImGui::End();
    ImGui::PopStyleVar();
}


static void z80_value_details(const char* name, u16 value, bool word)
{
    ImGui::Text("%s: $%0*X", name, word ? 4 : 2, value);
    const int signedValue = word ? (value >= 0x8000 ? int(value) - 0x10000 : int(value))
                                 : (value >= 0x80 ? int(value) - 0x100 : int(value));
    ImGui::Text("Decimal: %u   Signed: %d", unsigned(value), signedValue);
    if (word) draw_binary_word(value);
    else draw_binary_bits(static_cast<u8>(value));
}

static void z80_register_value(const char* name, u16 value, bool word,
                               const char* pair = nullptr, u16 pairValue = 0)
{
    const float labelWidth = std::max(ImGui::CalcTextSize("A'").x,
                                      ImGui::CalcTextSize("WZ").x);
    const float valueSpacing = labelWidth - ImGui::CalcTextSize(name).x +
                               ImGui::GetStyle().ItemSpacing.x;
    ImGui::TextColored(word ? yellow : cyan, "%s", name);
    // Relative spacing avoids adding the table column offset a second time.
    ImGui::SameLine(0.0f, valueSpacing);
    ImGui::Text("$%0*X", word ? 4 : 2, value);
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        z80_value_details(name, value, word);
        if (pair)
        {
            ImGui::Separator();
            z80_value_details(pair, pairValue, true);
        }
        ImGui::EndTooltip();
    }
}

static void debug_window_processor(void)
{
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 19.0f,
                                  ImGui::GetFontSize() * 32.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Z80 Status", &config_debug.show_processor))
    {
        ImGui::End();
        return;
    }
    ImGui::PushFont(gui_default_font, gui_get_default_font_size());
    GearSF7000Core* core = emu_get_core();
    CpuStateAccess* processor = core->GetCpuStateAccess();
    const CpuStateSnapshot s = processor->GetCpuStateSnapshot();

    const std::uint64_t elapsed_tS = processor->GetElapsedTStates();
    const int z80_clock_hz = core->GetClockDomains().GetRates().cpuHz;
    long double elapsed_uS = (long double)elapsed_tS / ((long double)z80_clock_hz / 1000000.0L);
    long double elapsed_mS = elapsed_uS / 1000;

    char elapsed_tk_text[32];
    char elapsed_us_text[48];
    char elapsed_ms_text[48];
    snprintf(elapsed_tk_text, sizeof(elapsed_tk_text), "TK: %016" PRIu64, elapsed_tS);
    snprintf(elapsed_us_text, sizeof(elapsed_us_text), "uS: %Lf", elapsed_uS);
    snprintf(elapsed_ms_text, sizeof(elapsed_ms_text), "mS: %Lf", elapsed_mS);

    const float clock_row_start = ImGui::GetCursorPosX();
    const float clock_block_width = std::max({
        ImGui::CalcTextSize(elapsed_tk_text).x,
        ImGui::CalcTextSize(elapsed_us_text).x,
        ImGui::CalcTextSize(elapsed_ms_text).x
    });
    ImGui::TextColored(magenta, "CLOCK");
    const float reset_button_width = ImGui::CalcTextSize("RESET").x
        + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(clock_row_start + clock_block_width - reset_button_width);
    const bool reset_clock = ImGui::SmallButton("RESET##reset_clock");
    const bool reset_clock_hovered = ImGui::IsItemHovered();
    if (reset_clock_hovered)
        ImGui::SetTooltip("Reset clock counters");

    ImGui::TextUnformatted(elapsed_tk_text);
    ImGui::TextUnformatted(elapsed_us_text);
    ImGui::TextUnformatted(elapsed_ms_text);

    if (reset_clock)
    {
        try
        {
            processor->SetElapsedTStates(0);
        }
        catch (const std::invalid_argument&)
        {
        }
    }
    ImGui::Separator();
    const char* flagNames[] = {"S", "Z", "Y", "H", "X", "P/V", "N", "C"};
    for (int i = 0; i < 8; ++i)
    {
        if (i) ImGui::SameLine();
        const bool set = (s.af & (1u << (7 - i))) != 0;
        ImGui::TextColored(set ? green : gray, "%s", flagNames[i]);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s = %d", flagNames[i], set ? 1 : 0);
    }
    ImGui::Separator();
    if (ImGui::BeginTable("compact_z80_registers", 2, ImGuiTableFlags_SizingStretchSame))
    {
        const auto pair = [](const char* hi, const char* lo, const char* name, u16 value)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            z80_register_value(hi, high_byte(value), false, name, value);
            ImGui::TableNextColumn();
            z80_register_value(lo, low_byte(value), false, name, value);
        };
        pair("A", "F", "AF", s.af);
        pair("B", "C", "BC", s.bc);
        pair("D", "E", "DE", s.de);
        pair("H", "L", "HL", s.hl);
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Separator();
        ImGui::TableNextColumn(); ImGui::Separator();
        pair("A'", "F'", "AF'", s.af2);
        pair("B'", "C'", "BC'", s.bc2);
        pair("D'", "E'", "DE'", s.de2);
        pair("H'", "L'", "HL'", s.hl2);
        ImGui::TableNextRow(); ImGui::TableNextColumn();
        z80_register_value("I", s.i, false);
        ImGui::TableNextColumn(); z80_register_value("R", s.r, false);
        ImGui::TableNextRow(); ImGui::TableNextColumn();
        z80_register_value("IX", s.ix, true);
        ImGui::TableNextColumn(); z80_register_value("IY", s.iy, true);
        ImGui::EndTable();
    }
    z80_register_value("WZ", s.wz, true);
    ImGui::Separator();
    ImGui::TextColored(s.iff1 ? green : gray, "IFF1"); ImGui::SameLine();
    ImGui::TextColored(s.iff2 ? green : gray, "IFF2"); ImGui::SameLine();
    ImGui::TextColored(s.halted ? green : gray, "HALT");
    ImGui::TextColored(s.intLine ? green : gray, "INT"); ImGui::SameLine();
    ImGui::TextColored(s.nmiLine ? green : gray, "NMI");
    ImGui::Separator();
    z80_register_value("PC", s.pc, true);
    z80_register_value("SP", s.sp, true);
    if (ImGui::CollapsingHeader("Stack", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Raw little-endian words from SP, using the current CPU mapping.\n"
                "These values are not inferred return addresses. Double-click a value to inspect it in the disassembler.");
        if (ImGui::BeginChild("##z80_stack", ImVec2(0, std::max(
            ImGui::GetTextLineHeightWithSpacing() * 3.0f, ImGui::GetContentRegionAvail().y)),
            ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar))
        {
            ImGuiListClipper clipper;
            clipper.Begin(128);
            while (clipper.Step())
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    const u16 address = static_cast<u16>(s.sp + row * 2);
                    Memory* memory = core->GetMemory();
                    const u8 lo = memory->Peek(address);
                    const u8 hi = memory->Peek(static_cast<u16>(address + 1));
                    const u16 value = static_cast<u16>(lo | (hi << 8));
                    char label[64];
                    snprintf(label, sizeof(label), "%s $%04X  $%04X", row == 0 ? ">" : " ", address, value);
                    ImGui::PushID(row);
                    if (ImGui::Selectable(label, false, ImGuiSelectableFlags_AllowDoubleClick) &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        config_debug.show_disassembler = true;
                        gui_debug_request_goto_address(value);
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("SP + $%02X: $%04X\nBytes: $%02X $%02X (little-endian)",
                            row * 2, address, lo, hi);
                        z80_value_details("Word", value, true);
                        ImGui::EndTooltip();
                    }
                    ImGui::PopID();
                }
        }
        ImGui::EndChild();
    }
    ImGui::PopFont();
    ImGui::End();
}

#if GEARSF7000_ENABLE_SR1000
static void debug_window_cassette(void)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    // Geometry in em (the chrome font size), so the panel keeps its
    // proportions at any monitor DPI or interface scale. No NoScrollbar here:
    // scaled text can outgrow the box, and without a scrollbar the overflow
    // would be unreachable.
    const float em = ImGui::GetFontSize();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 available(std::max(1.0f, viewport->WorkSize.x),
                           std::max(1.0f, viewport->WorkSize.y));

    ImGui::SetNextWindowPos(ImVec2(31.5f * em, 31.0f * em), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(std::min(20.0f * em, available.x),
                                    std::min(16.0f * em, available.y)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(std::min(15.0f * em, available.x), std::min(9.0f * em, available.y)),
        available);

    ImGui::Begin("Cassette", &config_debug.show_cassette);

    debug_window_cassette_controls();

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleVar();
}
#endif // GEARSF7000_ENABLE_SR1000

#if GEARSF7000_ENABLE_SF7000
static const char* sf7000_disk_format_name(DiskImageFormat format)
{
    switch (format)
    {
        case DISK_FORMAT_SF7:  return "SF7";
        case DISK_FORMAT_EDSK: return "EDSK";
        case DISK_FORMAT_HFE:  return "HFE";
        case DISK_FORMAT_DS7:  return "DS7";
        default:               return "--";
    }
}

static const char* sf7000_fdc_phase_name(uint8_t phase)
{
    switch (phase)
    {
        case 0: return "IDLE";
        case 1: return "COMMAND";
        case 2: return "EXEC";
        case 3: return "RESULT";
        case 4: return "SEEK";
        default: return "?";
    }
}

static void sf7000_status_led(const char* label, bool active)
{
    // The debug font is a restricted bitmap font and has no Unicode circles.
    // Draw the LED ourselves so its meaning remains visible on every platform.
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float radius = 3.0f;
    const ImU32 color = ImGui::ColorConvertFloat4ToU32(active ? green : gray);
    drawList->AddCircleFilled(ImVec2(origin.x + radius, origin.y + ImGui::GetTextLineHeight() * 0.5f), radius, color);
    ImGui::Dummy(ImVec2(radius * 2.0f + 3.0f, ImGui::GetTextLineHeight()));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(active ? white : gray, "%s", label);
}

static void debug_window_sf7000(void)
{
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar;
    ImGui::SetNextWindowPos(ImVec2(670, 365), ImGuiCond_FirstUseEver);
    gui_set_next_window_size_em(24.2f, 14.6f);

    if (!ImGui::Begin("SF-7000", &config_debug.show_sf7000, flags))
    {
        ImGui::End();
        return;
    }

    GearSF7000Core* core = emu_get_core();
    SF7000* sf7000 = core->GetSF7000();
    if (sf7000 == NULL)
    {
        ImGui::TextColored(gray, "SF-7000 hardware is unavailable");
        ImGui::End();
        return;
    }
    NEC765DebugInfo fdc = {};
    DiskDebugInfo disk = {};
    sf7000->GetFDCDebugInfo(&fdc);
    sf7000->GetDiskDebugInfo(&disk);

    // DiskMotorOn is the effective emulated spindle state; it is deliberately
    // independent from the optional motor WAV player.
    const bool motor = sf7000->DiskMotorOn;
    const bool index = motor && sf7000->FDCIndexActive();
    const uint32_t bitsPerRevolution = sf7000->GetFDCBitsPerRevolution();
    const uint32_t rotationBits = sf7000->GetFDCRotationBits();
    const float rotation = sf7000->GetDiskRotationFraction();
    // Represent the index hole as the centre of the slider. The head moves
    // right during the first half of a revolution, wraps to the left halfway
    // around, then returns to centre at the next index pulse.
    float spindlePosition = rotation <= 0.5f ? rotation : rotation - 1.0f;
    const int trackScaleMax = (disk.tracks > 40 || fdc.currentTrack > 39 || fdc.destinationTrack > 39) ? 42 : 39;
    int displayedTrack = fdc.currentTrack > trackScaleMax ? trackScaleMax : fdc.currentTrack;
    const int sectorSize = fdc.sizeCode < 8 ? (128 << fdc.sizeCode) : 0;
    const uint8_t msr = fdc.mainStatus;

    ImGui::PushFont(gui_default_font, gui_get_default_font_size());
    ImGui::TextColored(cyan, "DRIVE 0"); ImGui::SameLine();
    sf7000_status_led("MOTOR", motor); ImGui::SameLine();
    sf7000_status_led("DISK", disk.present != 0); ImGui::SameLine();
    sf7000_status_led("WP", disk.readOnly != 0); ImGui::SameLine();
    sf7000_status_led("DIRTY", disk.dirty != 0);

    ImGui::TextColored(cyan, "HEAD"); ImGui::SameLine();
    ImGui::Text("Track %02u / %02d", static_cast<unsigned>(fdc.currentTrack), trackScaleMax); ImGui::SameLine();
    ImGui::TextColored(magenta, "SIDE"); ImGui::SameLine();
    ImGui::Text("%u", static_cast<unsigned>(fdc.side));
    ImGui::PushItemWidth(-54.0f);
    ImGui::BeginDisabled();
    ImGui::SliderInt("##sf7000_track", &displayedTrack, 0, trackScaleMax, "");
    ImGui::EndDisabled();
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::TextColored(fdc.currentTrack > 39 ? orange : cyan, "%02u", static_cast<unsigned>(fdc.currentTrack));

    ImGui::TextColored(cyan, "SPEED"); ImGui::SameLine();
    ImGui::Text("%u RPM", static_cast<unsigned>(disk.rpm ? disk.rpm : SF7_RPM)); ImGui::SameLine();
    ImGui::Text("  %.1f ms/rev", 60000.0f / static_cast<float>(disk.rpm ? disk.rpm : SF7_RPM)); ImGui::SameLine();
    sf7000_status_led("INDEX", index);
    ImGui::PushItemWidth(-1.0f);
    ImGui::BeginDisabled();
    ImGui::SliderFloat("##sf7000_rotation", &spindlePosition, -0.5f, 0.5f, "", ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    ImGui::PopItemWidth();
    ImGui::TextColored(gray, "index  %+.1f%%  ·  raw FDC %u / %u MFM bits", spindlePosition * 100.0f,
        rotationBits, bitsPerRevolution);

    ImGui::Separator();
    ImGui::TextColored(cyan, "FDC"); ImGui::SameLine();
    ImGui::Text("%.3f MHz  MSR $%02X",
        static_cast<double>(sf7000->GetFDCClockHz()) / 1000000.0, msr);
    ImGui::SameLine();
    sf7000_status_led("RQM", (msr & 0x80) != 0); ImGui::SameLine();
    sf7000_status_led("DIO", (msr & 0x40) != 0); ImGui::SameLine();
    sf7000_status_led("EXM", (msr & 0x20) != 0); ImGui::SameLine();
    sf7000_status_led("CB",  (msr & 0x10) != 0); ImGui::SameLine();
    sf7000_status_led("IRQ", fdc.interrupt != 0);
    ImGui::Text("CMD $%02X  %-7s  phase %s:%u", static_cast<unsigned>(fdc.commandCode),
        fdc.commandCode == 0x46 ? "READ" : fdc.commandCode == 0x45 ? "WRITE" :
        fdc.commandCode == 0x4D ? "FORMAT" : fdc.commandCode == 0x0F ? "SEEK" :
        fdc.commandCode == 0x07 ? "RECAL" : "",
        sf7000_fdc_phase_name(fdc.phase), static_cast<unsigned>(fdc.phaseStep));
    ImGui::Text("CHRN %02X:%u:%02X:%u  (%d B)  seek %02u -> %02u",
        static_cast<unsigned>(fdc.cylinder), static_cast<unsigned>(fdc.side),
        static_cast<unsigned>(fdc.sector), static_cast<unsigned>(fdc.sizeCode), sectorSize,
        static_cast<unsigned>(fdc.currentTrack), static_cast<unsigned>(fdc.destinationTrack));

    ImGui::Separator();
    ImGui::TextColored(cyan, "PPI2"); ImGui::SameLine();
    ImGui::Text("E4 $%02X  E5 $%02X  E6 $%02X  E7 $%02X",
        static_cast<unsigned>(sf7000->Port_E4), static_cast<unsigned>(sf7000->Port_E5),
        static_cast<unsigned>(sf7000->Port_E6), static_cast<unsigned>(sf7000->Port_E7));
    ImGui::TextColored(cyan, "IPL"); ImGui::SameLine();
    ImGui::TextColored(sf7000->IPL_Enabled() ? green : gray,
        "%s", sf7000->IPL_Enabled() ? "ENABLED" : "DISABLED"); ImGui::SameLine();
    ImGui::Text("  TC %s  RESET %s",
        (sf7000->Port_E6 & 0x04) ? "1" : "0", (sf7000->Port_E6 & 0x08) ? "1" : "0");

    if (disk.present)
    {
        const char* name = disk.fileName && disk.fileName[0] ? disk.fileName : "(unnamed image)";
        const char* slash = name;
        for (const char* p = name; *p; ++p)
            if (*p == '/' || *p == '\\') slash = p + 1;
        ImGui::TextColored(cyan, "DISK"); ImGui::SameLine();
        ImGui::Text("%s  %s · %d side · %d x %d B", slash,
            sf7000_disk_format_name(disk.format), disk.sides, disk.sectorsPerTrack, disk.sectorSize);
    }
    else
    {
        ImGui::TextColored(gray, "DISK  no image mounted");
    }

    ImGui::PopFont();
    ImGui::End();
}
#endif // GEARSF7000_ENABLE_SF7000



#if GEARSF7000_ENABLE_MCP
static void debug_window_mcp_server(void)
{
    static char http_address[64] = "";
    static bool initialized = false;
    if (!initialized)
    {
        strcpy(http_address, config_emulator.mcp_http_address.c_str());
        initialized = true;
    }

    const float chrome_em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(17.8f * chrome_em, 23.3f * chrome_em), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(16.7f * chrome_em, 8.9f * chrome_em), ImGuiCond_FirstUseEver);

    ImGui::Begin("MCP Server", &config_debug.show_mcp_server, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize);
    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    // AlwaysAutoResize fits the window to the widgets, but the widgets
    // themselves have to follow the font: a fixed pixel width would clip its
    // own label once the interface is scaled up. The start/stop button keeps
    // one width for both labels so the panel does not jump when it toggles.
    const float em = ImGui::GetFontSize();
    const float padding = ImGui::GetStyle().FramePadding.x * 2.0f;
    const float action_width = std::max({ 11.5f * em,
        ImGui::CalcTextSize("Start HTTP Server").x + padding,
        ImGui::CalcTextSize("Stop HTTP Server").x + padding });

    const bool mcp_running = emu_mcp_is_running();
    const int transport_mode = emu_mcp_get_transport_mode();
    const bool command_line_owned = emu_mcp_started_from_command_line();
    const bool http_running = mcp_running && (transport_mode == MCP_TRANSPORT_TCP);
    const bool stdio_running = mcp_running && (transport_mode == MCP_TRANSPORT_STDIO);

    if (!mcp_running)
    {
        if (ImGui::Button("Start HTTP Server", ImVec2(action_width, 0.0f)))
        {
            if (strlen(http_address) == 0)
                strcpy(http_address, "127.0.0.1");
            config_emulator.mcp_http_address = http_address;
            emu_mcp_set_transport(MCP_TRANSPORT_TCP, config_emulator.mcp_tcp_port, config_emulator.mcp_http_address.c_str());
            emu_mcp_start();
        }
    }
    else
    {
        ImGui::BeginDisabled(!http_running || command_line_owned);
        if (ImGui::Button("Stop HTTP Server", ImVec2(action_width, 0.0f)))
        {
            emu_mcp_stop();
        }
        ImGui::EndDisabled();
    }

    ImGui::Separator();

    if (stdio_running)
        ImGui::TextColored(emu_mcp_is_listening() ? orange : red,
                           emu_mcp_is_listening() ? "STDIO mode active" : "STDIO transport unavailable");
    else if (http_running && emu_mcp_is_listening())
        ImGui::TextColored(green, "Listening on %s:%d", emu_mcp_get_address(), emu_mcp_get_port());
    else if (http_running)
        ImGui::TextColored(red, "Not listening on %s:%d", emu_mcp_get_address(), emu_mcp_get_port());
    else
        ImGui::TextColored(red, "Stopped");

    if (mcp_running && command_line_owned)
        ImGui::TextColored(gray, "(started from the command line)");

    ImGui::Separator();

    ImGui::BeginDisabled(mcp_running);
    ImGui::Text("HTTP Address:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(9.2f * em,
        ImGui::CalcTextSize("255.255.255.255").x + padding));
    if (ImGui::InputText("##mcp_address", http_address, IM_ARRAYSIZE(http_address), ImGuiInputTextFlags_AutoSelectAll))
        config_emulator.mcp_http_address = http_address;

    ImGui::Text("HTTP Port:  ");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(5.4f * em,
        ImGui::CalcTextSize("65535").x + padding));
    if (ImGui::InputInt("##mcp_port", &config_emulator.mcp_tcp_port, 0, 0))
    {
        if (config_emulator.mcp_tcp_port < 1)
            config_emulator.mcp_tcp_port = 1;
        if (config_emulator.mcp_tcp_port > 65535)
            config_emulator.mcp_tcp_port = 65535;
    }
    ImGui::EndDisabled();

    ImGui::PopFont();
    ImGui::End();
}

// Live view/editor for the MCP watch/freeze/trigger system (mcp_debug_adapter's
// WatchAdd/List/Freeze/SetCondition family). Reads DebugAdapter's json results
// directly instead of duplicating that state in the GUI - WatchTick() already
// runs every frame from emu_update() regardless of this window being open, so
// freezes and the trigger condition keep working even while it's closed.
static void debug_window_watch_monitor(void)
{
    ImGui::SetNextWindowPos(ImVec2(320, 420), ImGuiCond_FirstUseEver);
    gui_set_next_window_size_em(25.2f, 22.0f);

    ImGui::Begin("Watch Monitor", &config_debug.show_watch_monitor);
    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    DebugAdapter* adapter = emu_mcp_get_adapter();
    if (!adapter)
    {
        ImGui::TextColored(red, "MCP adapter not available.");
        ImGui::PopFont();
        ImGui::End();
        return;
    }

    static const char* k_area_names[] = { "0: Z80 space", "1: RAM", "2: BIOS", "3: VRAM", "4: Cartridge ROM" };
    static const char* k_type_names[] = { "unsigned", "signed", "hex", "text" };
    static const char* k_op_names[] = { "==", "!=", ">", "<", ">=", "<=", "changed", "increased", "decreased" };

    // --- Search: find a candidate address before you know what to watch.
    // Reuses the same MemorySearchCapture/MemorySearch DebugAdapter methods
    // the memory_search_* MCP tools call (memory_editor.h's MemEditor, one
    // instance per area) - a "Watch" button on each result row hands the
    // address straight to WatchAdd instead of making you retype it.
    if (ImGui::CollapsingHeader("Search", ImGuiTreeNodeFlags_DefaultOpen))
    {
        static int search_area = 1;
        static const char* op_names[] = { "<", ">", "==", "!=", "<=", ">=" };
        static const char* compare_type_names[] = { "previous", "value", "address" };
        static const char* data_type_names[] = { "hex", "signed", "unsigned" };
        static int op_idx = 3;             // != previous, i.e. "anything that changed"
        static int compare_type_idx = 0;   // previous
        static int data_type_idx = 2;      // unsigned
        static char compare_value[16] = "0";
        static json search_result;
        static std::string search_error;
        static bool have_previous_results = false; // true once a scan this capture session found >=1 candidate

        ImGui::TextWrapped("1) Reset. 2) Change something in the game (lose a life, "
            "take a hit...). 3) Scan. Repeat 2-3: each Scan after the first "
            "narrows down the same candidates instead of starting over.");

        // Only two buttons on purpose: Reset always starts a new search
        // session, Scan always runs one step of it - which step (a full
        // area scan or a narrowing pass over the current results) is
        // decided automatically from have_previous_results and shown as a
        // status line underneath, not by renaming the button itself.
        ImGui::SetNextItemWidth(gui_combo_width_for_array(k_area_names, IM_ARRAYSIZE(k_area_names)));
        ImGui::Combo("Area##search_area", &search_area, k_area_names, IM_ARRAYSIZE(k_area_names));
        ImGui::SameLine();
        if (ImGui::Button("Reset"))
        {
            adapter->MemorySearchCapture(search_area);
            search_result = json();
            search_error.clear();
            have_previous_results = false;
        }

        ImGui::SetNextItemWidth(gui_combo_width_for_array(op_names, IM_ARRAYSIZE(op_names)));
        ImGui::Combo("Op##search_op", &op_idx, op_names, IM_ARRAYSIZE(op_names));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(gui_combo_width_for_array(compare_type_names, IM_ARRAYSIZE(compare_type_names)));
        ImGui::Combo("Compare to##search_compare_type", &compare_type_idx, compare_type_names, IM_ARRAYSIZE(compare_type_names));
        ImGui::SameLine();
        ImGui::BeginDisabled(compare_type_idx == 0); // "previous" needs no value
        ImGui::SetNextItemWidth(80);
        ImGui::InputText("Value##search_value", compare_value, IM_ARRAYSIZE(compare_value), ImGuiInputTextFlags_CharsDecimal);
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(gui_combo_width_for_array(data_type_names, IM_ARRAYSIZE(data_type_names)));
        ImGui::Combo("Data type##search_data_type", &data_type_idx, data_type_names, IM_ARRAYSIZE(data_type_names));
        ImGui::SameLine();
        if (ImGui::Button("Scan"))
        {
            const json result = adapter->MemorySearch(search_area, op_names[op_idx],
                compare_type_names[compare_type_idx], atoi(compare_value), data_type_names[data_type_idx],
                have_previous_results);
            if (result.contains("error"))
            {
                search_error = result["error"].get<std::string>();
                search_result = json();
            }
            else
            {
                search_error.clear();
                search_result = result;
                have_previous_results = result.value("count", 0) > 0;
            }
        }
        ImGui::SameLine();
        ImGui::TextColored(gray, have_previous_results ? "(will narrow current results)" : "(full area scan)");

        if (!search_error.empty())
            ImGui::TextColored(red, "%s", search_error.c_str());

        if (!search_result.is_null())
        {
            const int count = search_result.value("count", 0);
            ImGui::Text("%d match%s%s", count, count == 1 ? "" : "es",
                count > 1000 ? " (showing first 1000)" : "");

            if (ImGui::BeginTable("search_results", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(0, 160)))
            {
                ImGui::TableSetupColumn("Address");
                ImGui::TableSetupColumn("Value");
                ImGui::TableSetupColumn("Previous");
                ImGui::TableSetupColumn("");
                ImGui::TableHeadersRow();

                int row = 0;
                for (const auto& r : search_result.value("results", json::array()))
                {
                    const std::string addr = r.at(0).get<std::string>();
                    ImGui::PushID(row++);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("$%s", addr.c_str());
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%lld", r.at(1).get<long long>());
                    ImGui::TableSetColumnIndex(2); ImGui::Text("%lld", r.at(2).get<long long>());
                    ImGui::TableSetColumnIndex(3);
                    if (ImGui::SmallButton("Watch"))
                        adapter->WatchAdd(search_area, static_cast<u32>(strtoul(addr.c_str(), NULL, 16)), "unsigned", 1, "");
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
    }

    ImGui::Separator();

    // --- Add watch ---
    if (ImGui::CollapsingHeader("Add Watch", ImGuiTreeNodeFlags_DefaultOpen))
    {
        static int new_area = 1;
        static char new_address[8] = "0000";
        static int new_type = 0;
        static int new_size_choice = 0; // 0 = 1 byte, 1 = 2 bytes (numeric types only)
        static int new_text_size = 4;
        static char new_label[32] = "";

        ImGui::SetNextItemWidth(gui_combo_width_for_array(k_area_names, IM_ARRAYSIZE(k_area_names)));
        ImGui::Combo("Area##new_area", &new_area, k_area_names, IM_ARRAYSIZE(k_area_names));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputText("Address (hex)##new_address", new_address, IM_ARRAYSIZE(new_address), ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);

        ImGui::SetNextItemWidth(gui_combo_width_for_array(k_type_names, IM_ARRAYSIZE(k_type_names)));
        ImGui::Combo("Type##new_type", &new_type, k_type_names, IM_ARRAYSIZE(k_type_names));
        ImGui::SameLine();
        if (new_type == 3)
        {
            ImGui::SetNextItemWidth(80);
            ImGui::InputInt("Bytes##new_text_size", &new_text_size);
            if (new_text_size < 1) new_text_size = 1;
            if (new_text_size > 64) new_text_size = 64;
        }
        else
        {
            static const char* size_names[] = { "1 byte", "2 bytes" };
            ImGui::SetNextItemWidth(gui_combo_width_for_array(size_names, IM_ARRAYSIZE(size_names)));
            ImGui::Combo("Size##new_size", &new_size_choice, size_names, IM_ARRAYSIZE(size_names));
        }

        ImGui::SetNextItemWidth(180);
        ImGui::InputText("Label##new_label", new_label, IM_ARRAYSIZE(new_label));
        ImGui::SameLine();
        if (ImGui::Button("Add##add_watch"))
        {
            const u32 address = static_cast<u32>(strtoul(new_address, NULL, 16));
            const int size = (new_type == 3) ? new_text_size : (new_size_choice == 0 ? 1 : 2);
            adapter->WatchAdd(new_area, address, k_type_names[new_type], size, new_label);
            new_label[0] = '\0';
        }
    }

    ImGui::Separator();

    // --- Watch table ---
    const json watch_list = adapter->WatchList();
    std::vector<std::pair<int, std::string>> watch_ids; // for the condition builder below

    if (ImGui::BeginTable("watch_table", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label");
        ImGui::TableSetupColumn("Area");
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("Previous");
        ImGui::TableSetupColumn("Frozen");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();

        for (const auto& w : watch_list.value("watches", json::array()))
        {
            const int id = w.value("id", 0);
            const std::string label = w.value("label", "");
            watch_ids.push_back({ id, label.empty() ? ("#" + std::to_string(id)) : label });

            ImGui::PushID(id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(label.empty() ? "-" : label.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", w.value("area", 0));
            ImGui::TableSetColumnIndex(2); ImGui::Text("$%s", w.value("address", "").c_str());
            ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(w.value("data_type", "").c_str());

            const bool changed = w.value("changed", false);
            const json value_field = w.contains("value") ? w["value"] : json(nullptr);
            const std::string value_str = value_field.is_null() ? "-" : (value_field.is_string() ? value_field.get<std::string>() : value_field.dump());
            ImGui::TableSetColumnIndex(4);
            if (changed) ImGui::TextColored(green, "%s", value_str.c_str());
            else ImGui::TextUnformatted(value_str.c_str());

            const json prev_field = w.contains("previous_value") ? w["previous_value"] : json(nullptr);
            const std::string prev_str = prev_field.is_null() ? "-" : (prev_field.is_string() ? prev_field.get<std::string>() : prev_field.dump());
            ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(prev_str.c_str());

            ImGui::TableSetColumnIndex(6);
            bool frozen = w.value("frozen", false);
            if (ImGui::Checkbox("##frozen", &frozen))
            {
                if (frozen) adapter->WatchFreeze(id, value_field.is_null() ? json(0) : value_field);
                else adapter->WatchUnfreeze(id);
            }

            ImGui::TableSetColumnIndex(7);
            if (ImGui::SmallButton(ICON_MD_DELETE))
                adapter->WatchRemove(id);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Separator();

    // --- Trigger condition ---
    if (ImGui::CollapsingHeader("Trigger Condition", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (watch_ids.empty())
        {
            ImGui::TextColored(gray, "Add at least one watch to build a condition.");
        }
        else
        {
            constexpr int kMaxRows = 4;
            static bool row_enabled[kMaxRows] = {};
            static int row_watch[kMaxRows] = {};
            static int row_op[kMaxRows] = {};
            static char row_value[kMaxRows][32] = {};
            static int mode = 0; // 0 = AND, 1 = OR
            static std::string last_error;

            for (int i = 0; i < kMaxRows; i++)
            {
                ImGui::PushID(i);
                ImGui::Checkbox("##enabled", &row_enabled[i]);
                ImGui::SameLine();

                if (row_watch[i] >= static_cast<int>(watch_ids.size())) row_watch[i] = 0;
                float watch_combo_width = 0.0f;
                for (const auto& watch : watch_ids)
                    watch_combo_width = std::max(watch_combo_width,
                        gui_combo_width_for_text(watch.second.c_str()));
                ImGui::SetNextItemWidth(watch_combo_width);
                if (ImGui::BeginCombo("##watch", watch_ids[row_watch[i]].second.c_str()))
                {
                    for (int w = 0; w < static_cast<int>(watch_ids.size()); w++)
                        if (ImGui::Selectable(watch_ids[w].second.c_str(), row_watch[i] == w))
                            row_watch[i] = w;
                    ImGui::EndCombo();
                }
                ImGui::SameLine();

                ImGui::SetNextItemWidth(gui_combo_width_for_array(k_op_names, IM_ARRAYSIZE(k_op_names)));
                ImGui::Combo("##op", &row_op[i], k_op_names, IM_ARRAYSIZE(k_op_names));
                ImGui::SameLine();

                const bool is_transition_op = row_op[i] >= 6; // changed/increased/decreased
                ImGui::BeginDisabled(is_transition_op);
                ImGui::SetNextItemWidth(100);
                ImGui::InputText("##value", row_value[i], IM_ARRAYSIZE(row_value[i]));
                ImGui::EndDisabled();
                ImGui::PopID();
            }

            ImGui::RadioButton("AND", &mode, 0);
            ImGui::SameLine();
            ImGui::RadioButton("OR", &mode, 1);
            ImGui::SameLine();

            if (ImGui::Button("Apply"))
            {
                json conditions = json::array();
                for (int i = 0; i < kMaxRows; i++)
                {
                    if (!row_enabled[i]) continue;
                    json c;
                    c["id"] = watch_ids[row_watch[i]].first;
                    c["op"] = k_op_names[row_op[i]];
                    if (row_op[i] < 6)
                        c["value"] = std::string(row_value[i]);
                    conditions.push_back(c);
                }
                const json result = adapter->WatchSetCondition(conditions, mode == 0 ? "AND" : "OR");
                last_error = result.contains("error") ? result["error"].get<std::string>() : "";
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear"))
            {
                adapter->WatchClearCondition();
                last_error.clear();
            }

            if (!last_error.empty())
                ImGui::TextColored(red, "%s", last_error.c_str());

            const json status = adapter->WatchConditionStatus();
            if (status.value("active", false))
            {
                if (status.value("triggered", false))
                    ImGui::TextColored(orange, "Triggered - emulator paused.");
                else
                    ImGui::TextColored(green, "Active, watching...");
            }
            else
            {
                ImGui::TextColored(gray, "No condition set.");
            }
        }
    }

    ImGui::PopFont();
    ImGui::End();
}
#endif



// Proof-of-infrastructure only (see shaders/crt_pass/README.md): shows the
// output of a real second SDL_GPU pipeline reading the emulator texture -
// today just a horizontal blur, not a CRT decode. Side by side with the
// normal picture so the plumbing is visibly doing something, not a no-op.
static void debug_window_crt_test_pass(void)
{
    // Wide enough for two 2x-scaled 256-ish-wide pictures side by side
    // plus padding; ImGuiCond_FirstUseEver so a user's manual resize
    // still sticks across sessions.
    gui_set_next_window_size_em(65.6f, 34.4f);
    ImGui::Begin("CRT Test Pass (GPU)", &config_debug.show_crt_test_pass);

    // The window's visibility is the single source of truth for whether the
    // GPU pass runs - this function only executes while it is (or was,
    // this very frame - Begin applies a close-button click before
    // returning) visible, so syncing here covers every way the flag can
    // change: this checkbox, the window's own close button, and a
    // persisted-enabled config on startup.
    renderer_set_crt_test_enabled(config_debug.show_crt_test_pass);

    if (!renderer_crt_test_texture)
    {
        ImGui::TextWrapped("No texture yet - load a ROM first.");
        ImGui::End();
        return;
    }

    bool blur_enabled = renderer_crt_test_blur_enabled();
    if (ImGui::Checkbox("Enable Horizontal Blur", &blur_enabled))
        renderer_set_crt_test_blur_enabled(blur_enabled);

    float radius = renderer_crt_test_blur_radius();
    ImGui::BeginDisabled(!blur_enabled);
    ImGui::SetNextItemWidth(-1.0f);
    // 0.00-1.20 dots: past that the blur is already well beyond what a
    // real composite signal's chroma bandwidth produces (checked by hand
    // by dragging past 1.2 and seeing it turn into an unrealistic streak -
    // see blur.frag). Continuous (Gaussian sigma, not a rounded tap
    // count), so every position on the slider looks different, not just
    // whole-dot steps - snapped to a 0.05 grid after each drag so the
    // values it settles on stay easy to compare and repeat.
    if (ImGui::SliderFloat("##blur_radius", &radius, 0.0f, 1.2f, "Radius (dots) = %.2f"))
    {
        radius = roundf(radius / 0.05f) * 0.05f;
        renderer_set_crt_test_blur_radius(radius);
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::Spacing();

    GC_RuntimeInfo runtime;
    emu_get_runtime(runtime);
    const float scale = 2.0f;
    const ImVec2 image_size(static_cast<float>(runtime.screen_width) * scale,
                             static_cast<float>(runtime.screen_height) * scale);
    const RendererTextureRegion sourceRegion = renderer_get_emulator_texture_region();
    const RendererTextureRegion outputRegion = renderer_get_emulator_output_texture_region();

    if (ImGui::BeginTable("crt_test_pass_compare", 2, ImGuiTableFlags_SizingStretchSame))
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Source (emu_texture)");
        ImGui::Image(renderer_emu_texture, image_size,
            sourceRegion.uv0, sourceRegion.uv1);

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted("Blurred (GPU test pass output)");
        ImGui::Image(renderer_crt_test_texture, image_size,
            outputRegion.uv0, outputRegion.uv1);

        ImGui::EndTable();
    }

    ImGui::End();
}

// Real two-pass composite/S-Video decode (see shaders/crt_pass/
// composite_chroma.frag / composite_luma.frag) - a direct GLSL/MSL
// translation of tools/crt/bench_decode.cpp's DecodeFrame, not an
// approximation. Runs on a captured GCRT signal file (the same format
// dump_crt_signal writes over MCP), not on live gameplay - loading a
// capture is a deliberate user action here, not automatic.
static void debug_window_composite_decode_test(void)
{
    static char capture_path[1024] = "";
    static std::string last_message;
    static bool last_message_is_error = false;

    gui_set_next_window_size_em(38.9f, 38.9f);
    ImGui::Begin("CRT Composite Decode Test (GPU)", &config_debug.show_composite_decode_test);

    renderer_set_composite_decode_enabled(config_debug.show_composite_decode_test);

    ImGui::TextWrapped("Loads a GCRT capture (dump_crt_signal over MCP) and decodes it on the "
        "GPU - the same math as tools/crt/bench_decode.cpp, translated to a shader.");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(-90.0f);
    ImGui::InputText("##capture_path", capture_path, IM_ARRAYSIZE(capture_path));
    ImGui::SameLine();
    if (ImGui::Button("Load", ImVec2(80, 0)))
    {
        std::string error;
        if (renderer_load_composite_decode_capture(capture_path, &error))
        {
            last_message = "Loaded.";
            last_message_is_error = false;
        }
        else
        {
            last_message = error;
            last_message_is_error = true;
        }
    }
    if (!last_message.empty())
    {
        ImGui::TextColored(last_message_is_error ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
                                                   : ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
            "%s", last_message.c_str());
    }

    bool composite = renderer_composite_decode_mode();
    if (ImGui::RadioButton("Composite", composite)) { composite = true; renderer_set_composite_decode_mode(true); }
    ImGui::SameLine();
    if (ImGui::RadioButton("S-Video", !composite)) { renderer_set_composite_decode_mode(false); }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Composite: luma+chroma share a wire (dot crawl). S-Video: separate wires, no dot crawl but chroma still bleeds.");

    ImGui::Separator();
    ImGui::Spacing();

    if (!renderer_composite_decode_ready())
    {
        ImGui::TextWrapped("No capture loaded yet.");
        ImGui::End();
        return;
    }

    const float scale = 2.0f;
    // The visible raster is not square-pixelled - it fills a 4:3 screen,
    // so displaying it 1:1 looks squeezed horizontally compared to a real
    // PAL picture. 1.380 = (4/3) / (kPictureDots/kPictureLines), derived
    // from the actual raster geometry, not eyeballed - see tools/crt/
    // README.md's own note on this same factor in the offline decoder's
    // display stage. Display-only: the shaders and the texture itself
    // stay at native 284x294, only how wide we draw it here changes.
    const float aspectStretch = 1.380f;
    const ImVec2 image_size(
        static_cast<float>(CrtSignalCaptureLoader::kPictureDots) * scale * aspectStretch,
        static_cast<float>(CrtSignalCaptureLoader::kPictureLines) * scale);
    ImGui::Image(renderer_composite_decode_texture, image_size);

    ImGui::End();
}

// Display-stage test: trimmed GearSystem crt_consumer.glsl port (see
// shaders/crt_pass/crt_consumer.frag for what was kept - blur, scanline,
// mask, brightboost, gamma - and what was left out - curvature, glow,
// noise, vignette, color temperature, interlace flicker). Runs on
// renderer_emu_texture directly, so it works on live gameplay, not just a
// captured file. Parameter defaults are not derived from a reference -
// they are a starting point to tune by eye, same as the original shader.
static void debug_window_crt_consumer_test(void)
{
    gui_set_next_window_size_em(65.6f, 42.2f);
    ImGui::Begin("CRT Consumer Test (GPU)", &config_debug.show_crt_consumer_test);

    // Same single-source-of-truth pattern as the other two CRT test
    // windows: the window's visibility is what enables the GPU pass, so
    // sync it here every frame.
    renderer_set_crt_consumer_enabled(config_debug.show_crt_consumer_test);

    if (!renderer_crt_consumer_texture)
    {
        ImGui::TextWrapped("No texture yet - load a ROM first.");
        ImGui::End();
        return;
    }

    RendererCrtConsumerParams params = renderer_crt_consumer_params();
    bool changed = false;

    ImGui::TextWrapped("Trimmed port of GearSystem's crt_consumer.glsl - blur, scanline, "
        "mask, brightboost, gamma. Curvature and the other extras were left out.");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##blurx", &params.blurx, 0.0f, 1.0f, "Blur X = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##blury", &params.blury, 0.0f, 1.0f, "Blur Y = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##scanlow", &params.scanlow, 0.0f, 20.0f, "Scanline Weight Low = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##scanhigh", &params.scanhigh, 0.0f, 20.0f, "Scanline Weight High = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##beamlow", &params.beamlow, 0.0f, 4.0f, "Beam Shape Low = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##beamhigh", &params.beamhigh, 0.0f, 4.0f, "Beam Shape High = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##preserve", &params.preserve, 0.0f, 1.0f, "Mask/Luminance Preserve = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##brightboost1", &params.brightboost1, 0.5f, 2.0f, "Bright Boost (dark) = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##brightboost2", &params.brightboost2, 0.5f, 2.0f, "Bright Boost (bright) = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##gammaOut", &params.gammaOut, 1.0f, 3.0f, "Output Gamma = %.2f");

    if (changed)
        renderer_set_crt_consumer_params(params);

    ImGui::Spacing();
    int debugUpscale = renderer_crt_consumer_debug_upscale();
    ImGui::SetNextItemWidth(-1.0f);
    // Debug-only knob, not one of the shader's own tunables (see
    // renderer.h's comment on renderer_set_crt_consumer_debug_upscale) -
    // controls how many render-target rows this test pass allocates per
    // source row, i.e. how many distinct scanline-weight phases exist to
    // show. At 1 the scanline term is mathematically a no-op (every
    // fragment lands on the same texel-center phase) - push this up if the
    // banding below still isn't visible.
    // Range mirrors renderer.cpp's kCrtConsumerDebugUpscaleMin/Max (1..16) -
    // renderer_set_crt_consumer_debug_upscale clamps regardless, these are
    // just the slider's own bounds.
    if (ImGui::SliderInt("##debug_upscale", &debugUpscale, 1, 16,
        "Debug Upscale (rows per source row) = %d"))
        renderer_set_crt_consumer_debug_upscale(debugUpscale);

    ImGui::Separator();
    ImGui::Spacing();

    GC_RuntimeInfo runtime;
    emu_get_runtime(runtime);
    const float sourceScale = 2.0f;
    const ImVec2 sourceSize(static_cast<float>(runtime.screen_width) * sourceScale,
                             static_cast<float>(runtime.screen_height) * sourceScale);
    // Shown at its real, full-upscale pixel size (no extra ImGui-side
    // resampling) - a second, uncontrolled downsample on top of the GPU
    // pass is exactly what was hiding the scanline banding the first time
    // this window shipped: bilinear-sampling a 4x-tall render target down
    // to a 2x-tall ImGui image quietly averaged the very row-to-row
    // variation the debug-upscale control above exists to create. Each
    // pane scrolls independently since the two are no longer the same size.
    const ImVec2 outputSize(static_cast<float>(runtime.screen_width * debugUpscale),
                             static_cast<float>(runtime.screen_height * debugUpscale));
    const RendererTextureRegion sourceRegion = renderer_get_emulator_texture_region();
    const RendererTextureRegion outputRegion = renderer_get_emulator_output_texture_region();

    const float paneHeight = 620.0f;
    if (ImGui::BeginChild("crt_consumer_source_pane", ImVec2(sourceSize.x + 20.0f, paneHeight), true))
    {
        ImGui::TextUnformatted("Source (emu_texture)");
        ImGui::Image(renderer_emu_texture, sourceSize,
            sourceRegion.uv0, sourceRegion.uv1);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    if (ImGui::BeginChild("crt_consumer_output_pane", ImVec2(0.0f, paneHeight), true))
    {
        ImGui::TextUnformatted("crt_consumer output (native size - scroll to inspect)");
        ImGui::Image(renderer_crt_consumer_texture, outputSize,
            outputRegion.uv0, outputRegion.uv1);
    }
    ImGui::EndChild();

    ImGui::End();
}

// Display-stage test, second candidate: trimmed crt-lottes-fast.glsl port
// (see shaders/crt_pass/crt_lottes.frag for what was kept - scanline,
// optional phosphor mask, auto-exposure, gamma - and what was left out -
// curvature/warp and the vignette bundled inside it, contrast/saturation).
// Runs on renderer_emu_texture directly, same as CRT Consumer Test - the
// two are meant to be compared, not one replacing the other.
static void debug_window_crt_lottes_test(void)
{
    gui_set_next_window_size_em(65.6f, 42.2f);
    ImGui::Begin("CRT Lottes Test (GPU)", &config_debug.show_crt_lottes_test);

    // Same single-source-of-truth pattern as the other CRT test windows.
    renderer_set_crt_lottes_enabled(config_debug.show_crt_lottes_test);

    if (!renderer_crt_lottes_texture)
    {
        ImGui::TextWrapped("No texture yet - load a ROM first.");
        ImGui::End();
        return;
    }

    RendererCrtLottesParams params = renderer_crt_lottes_params();
    bool changed = false;

    ImGui::TextWrapped("Trimmed port of Timothy Lottes' crt-lottes-fast.glsl - scanline, "
        "optional phosphor mask, auto-exposure, gamma. Curvature/vignette and color "
        "grading extras were left out.");
    ImGui::Spacing();

    static const char* kMaskNames[] = {
        "No Mask", "Aperture Grille", "Aperture Grille (bright)", "Shadow Mask"
    };
    int maskIndex = static_cast<int>(params.maskType + 0.5f);
    ImGui::SetNextItemWidth(-1.0f);
    // Defaults to Off - compare against the other three by eye, as
    // requested, rather than assuming the mask should be on.
    if (ImGui::Combo("##maskType", &maskIndex, kMaskNames, IM_ARRAYSIZE(kMaskNames)))
    {
        params.maskType = static_cast<float>(maskIndex);
        changed = true;
    }
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##maskIntensity", &params.maskIntensity, 0.0f, 1.0f, "Mask Intensity = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##scanlineThinness", &params.scanlineThinness, 0.0f, 1.0f, "Scanline Intensity = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##scanBlur", &params.scanBlur, 1.0f, 6.0f, "Sharpness = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##crtGamma", &params.crtGamma, 1.0f, 3.0f, "CRT Gamma = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##blackThreshold", &params.blackThreshold, 0.0f, 0.5f, "Black Threshold = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##boldness", &params.boldness, 0.0f, 1.0f, "Boldness = %.2f");
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##edgeRadius", &params.edgeRadius, 0.25f, 4.0f, "Edge Radius = %.2f");

    if (changed)
        renderer_set_crt_lottes_params(params);

    ImGui::Spacing();
    int debugUpscale = renderer_crt_lottes_debug_upscale();
    ImGui::SetNextItemWidth(-1.0f);
    // Same debug-only knob as CRT Consumer Test's, independent value - see
    // renderer.h's comment on renderer_set_crt_consumer_debug_upscale for
    // why this exists and why the real final pass will never need it.
    if (ImGui::SliderInt("##debug_upscale", &debugUpscale, 1, 16,
        "Debug Upscale (rows per source row) = %d"))
        renderer_set_crt_lottes_debug_upscale(debugUpscale);

    ImGui::Separator();
    ImGui::Spacing();

    GC_RuntimeInfo runtime;
    emu_get_runtime(runtime);
    const float sourceScale = 2.0f;
    const ImVec2 sourceSize(static_cast<float>(runtime.screen_width) * sourceScale,
                             static_cast<float>(runtime.screen_height) * sourceScale);
    // Shown at real, full-upscale pixel size - see CRT Consumer Test's own
    // comment on why an extra ImGui-side resize would quietly hide the
    // scanline banding again.
    const ImVec2 outputSize(static_cast<float>(runtime.screen_width * debugUpscale),
                             static_cast<float>(runtime.screen_height * debugUpscale));
    const RendererTextureRegion sourceRegion = renderer_get_emulator_texture_region();
    const RendererTextureRegion outputRegion = renderer_get_emulator_output_texture_region();

    const float paneHeight = 620.0f;
    if (ImGui::BeginChild("crt_lottes_source_pane", ImVec2(sourceSize.x + 20.0f, paneHeight), true))
    {
        ImGui::TextUnformatted("Source (emu_texture)");
        ImGui::Image(renderer_emu_texture, sourceSize,
            sourceRegion.uv0, sourceRegion.uv1);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    if (ImGui::BeginChild("crt_lottes_output_pane", ImVec2(0.0f, paneHeight), true))
    {
        ImGui::TextUnformatted("crt_lottes output (native size - scroll to inspect)");
        ImGui::Image(renderer_crt_lottes_texture, outputSize,
            outputRegion.uv0, outputRegion.uv1);
    }
    ImGui::EndChild();

    ImGui::End();
}

#if GEARSF7000_ENABLE_SR1000
static void debug_window_cassette_controls(void)
{
    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    ITapeRecorder* recorder = emu_get_core()->GetCassette();
    SR1000Speaker* speaker = emu_get_core()->GetCassetteSpeaker();

    // State on the left, transport stacked on the right: the three buttons
    // cost one column instead of a whole row, and the split follows the font.
    const float em = ImGui::GetFontSize();
    const float transport_width = std::max(4.6f * em,
        ImGui::CalcTextSize("Rewind").x + (ImGui::GetStyle().FramePadding.x * 2.0f));

    // No sizing flag: both columns state their own policy below, and
    // SizingFixedFit would read as "shrink to contents" while doing nothing.
    if (!ImGui::BeginTable("##cassette_layout", 2, ImGuiTableFlags_None))
    {
        ImGui::PopFont();
        return;
    }

    ImGui::TableSetupColumn("##state", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##transport", ImGuiTableColumnFlags_WidthFixed, transport_width);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);

    ImGui::TextColored(yellow, "CASSETTE STATE:");

    ImGui::TextColored(cyan, " Loaded          "); ImGui::SameLine();
    recorder->IsLoaded() ? ImGui::TextColored(green, "YES ") : ImGui::TextColored(gray, "NO  ");

    ImGui::TextColored(cyan, " Playing         "); ImGui::SameLine();
    recorder->IsPlaying() ? ImGui::TextColored(green, "YES ") : ImGui::TextColored(gray, "NO  ");

    ImGui::TextColored(cyan, " Motor           "); ImGui::SameLine();
    recorder->IsMotorOn() ? ImGui::TextColored(green, "ON  ") : ImGui::TextColored(gray, "OFF ");

    ImGui::TextColored(cyan, " Signal          "); ImGui::SameLine();
    recorder->GetSignal() ? ImGui::TextColored(green, "^^^ ") : ImGui::TextColored(gray, "___ ");

    //ImGui::TextColored(cyan, " Percentage        %3d / 100", (int)recorder->GetPositionPercentage());
    ImGui::TextColored(cyan, " Counter          %3d / %3d", (int)recorder->GetCounter(), (int)recorder->GetCounterMax());

    float tapeSpeed = recorder->GetTapeSpeedPercent();
    ImGui::TextColored(cyan, " Tape speed");
    ImGui::SameLine();
    ImGui::PushItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##tape_speed", &tapeSpeed, -20.0f, 20.0f, "%+.0f%%"))
        recorder->SetTapeSpeedPercent(tapeSpeed);
    ImGui::PopItemWidth();
    ImGui::TextColored(gray, " Target %+.1f%%  effective %+.1f%%",
        recorder->GetTapeSpeedPercent(), recorder->GetEffectiveTapeSpeedPercent());

    //std::string info = recorder->GetInfo();
    std::string info = speaker->GetInfo();
    ImGui::TextColored(cyan, "%s", info.c_str());

    ImGui::TableSetColumnIndex(1);

    // -FLT_MIN fills the column, so the buttons stay as wide as the widest
    // label ("Rewind") however large the font gets.
    const ImVec2 transport_button(-FLT_MIN, 0.0f);

    if (ImGui::Button("Play", transport_button))
    {
        try
        {
            emu_cassette_play();
        }
        catch (const std::invalid_argument&)
        {
        }
    }

    if (ImGui::Button("Stop", transport_button))
    {
        try
        {
            emu_cassette_stop();
        }
        catch (const std::invalid_argument&)
        {
        }
    }

    if (ImGui::Button("Rewind", transport_button))
    {
        try
        {
            emu_cassette_rewind();
        }
        catch (const std::invalid_argument&)
        {
        }
    }

    ImGui::EndTable();

    ImGui::PopFont();
}
#endif // GEARSF7000_ENABLE_SR1000




static void add_symbol(const char* line)
{
    Log("Loading symbol %s", line);

    DebugSymbol s;

    std::string str(line);

    str.erase(std::remove(str.begin(), str.end(), '\r'), str.end());
    str.erase(std::remove(str.begin(), str.end(), '\n'), str.end());

    size_t first = str.find_first_not_of(' ');
    if (std::string::npos == first)
    {
        str = "";
    }
    else
    {
        size_t last = str.find_last_not_of(' ');
        str = str.substr(first, (last - first + 1));
    }

    std::size_t comment = str.find(";");

    if (comment != std::string::npos)
        str = str.substr(0 , comment);

    std::size_t space = str.find(" ");

    if (space != std::string::npos)
    {
        s.text = str.substr(space + 1 , std::string::npos);
        str = str.substr(0, space);

        std::size_t separator = str.find(":");

        try
        {
            if (separator != std::string::npos)
            {
                s.address = (u16)std::stoul(str.substr(separator + 1 , std::string::npos), 0, 16);

                s.bank = std::stoul(str.substr(0, separator), 0 , 16);
            }
            else
            {
                s.address = (u16)std::stoul(str, 0, 16);
                s.bank = 0;
            }

            symbols.push_back(s);
        }
        catch(const std::invalid_argument&)
        {
        }
    }
}

static void add_breakpoint_cpu(void)
{
    int input_len = (int)strlen(brk_address_cpu);
    u16 target_address = 0;
    try
    {
        if ((input_len == 7) && (brk_address_cpu[2] == ':'))
        {
            std::string str(brk_address_cpu);
            std::size_t separator = str.find(":");

            if (separator != std::string::npos)
            {
                target_address = (u16)std::stoul(str.substr(separator + 1 , std::string::npos), 0, 16);

                // Bank-qualified syntax is retained for compatibility. CPU
                // breakpoints currently operate on the visible Z80 address.
                // The value is discarded on purpose, but the call is not dead:
                // it throws on a bank that is not hex, and the catch below
                // then rejects the breakpoint instead of accepting "1X:4000".
                (void)std::stoul(str.substr(0, separator), 0, 16);
            }
        } 
        else if (input_len == 4)
        {
            target_address = (u16)std::stoul(brk_address_cpu, 0, 16);
        }
        else
        {
            return;
        }
    }
    catch(const std::invalid_argument&)
    {
        return;
    }

    Memory::stDisassembleRecord* record = emu_get_core()->GetMemory()->GetDisassembleRecord(target_address, true);

    brk_address_cpu[0] = 0;

    bool found = false;
    std::vector<Memory::stDisassembleRecord*>* breakpoints = emu_get_core()->GetMemory()->GetBreakpointsCPU();

    if (IsValidPointer(record))
    {
        for (long unsigned int b = 0; b < breakpoints->size(); b++)
        {
            if ((*breakpoints)[b] == record)
            {
                found = true;
                break;
            }
        }
    }

    if (!found)
    {
        breakpoints->push_back(record);
    }
}

static void add_breakpoint_mem(void)
{
    int input_len = (int)strlen(brk_address_mem);
    u16 address1 = 0;
    u16 address2 = 0;
    bool range = false;

    try
    {
        if ((input_len == 9) && (brk_address_mem[4] == '-'))
        {
            std::string str(brk_address_mem);
            std::size_t separator = str.find("-");

            if (separator != std::string::npos)
            {
                address1 = (u16)std::stoul(str.substr(0, separator), 0 , 16);
                address2 = (u16)std::stoul(str.substr(separator + 1 , std::string::npos), 0, 16);
                range = true;
            }
        }
        else if (input_len == 4)
        {
            address1 = (u16)std::stoul(brk_address_mem, 0, 16);
        }
        else
        {
            return;
        }
    }
    catch(const std::invalid_argument&)
    {
        return;
    }

    bool found = false;
    std::vector<Memory::stMemoryBreakpoint>* breakpoints = emu_get_core()->GetMemory()->GetBreakpointsMem();

    for (long unsigned int b = 0; b < breakpoints->size(); b++)
    {
        Memory::stMemoryBreakpoint temp = (*breakpoints)[b];
        if ((temp.address1 == address1) && (temp.address2 == address2) && (temp.range == range))
        {
            found = true;
            break;
        }
    }

    if (!found)
    {
        Memory::stMemoryBreakpoint new_breakpoint;
        new_breakpoint.address1 = address1;
        new_breakpoint.address2 = address2;
        new_breakpoint.range = range;
        new_breakpoint.read = brk_new_mem_read;
        new_breakpoint.write = brk_new_mem_write;

        breakpoints->push_back(new_breakpoint);
    }

    brk_address_mem[0] = 0;
}




void gui_debug_request_goto_address(u16 address)
{
    goto_address_requested = true;
    goto_address_target = address;
}

static bool is_return_instruction(u8 opcode1, u8 opcode2)
{
    switch (opcode1)
    {
        case 0xC9: // RET
        case 0xC0: // RET NZ
        case 0xC8: // RET Z
        case 0xD0: // RET NC
        case 0xD8: // RET C
        case 0xE0: // RET PO
        case 0xE8: // RET PE
        case 0xF0: // RET P
        case 0xF8: // RET M
            return true;
        case 0xED: // Extended instructions
            if (opcode2 == 0x45 || opcode2 == 0x4D) // RETN, RETI
                return true;
            else
                return false;
        default:
            return false;
    }
}
