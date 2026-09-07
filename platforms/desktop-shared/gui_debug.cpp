/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026  Saverio Russo

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 *
 */

#include <math.h>
#include <inttypes.h>
#include <cstring>
#include <sstream>
#include <algorithm>
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
#include "../../src/gearsf7000.h"
#if GEARSF7000_ENABLE_SF7000
#include "../../src/SF7000.h"
#endif
#if GEARSF7000_ENABLE_SR1000
#include "../../src/SR1000.h"
#endif
#include "../../src/Video.h"
#include "gui.h"
#include "gui_debug_constants.h"
#include "gui_debug_memory.h"
#include "gui_debug_mem_import.h"
#include "gui_debug_basic_typer.h"
#include "gui_debug_disassembler_export.h"
#include "gui_debug_rom_inspector.h"
#if GEARSF7000_ENABLE_SF7000
#include "gui_debug_disc_explorer.h"
#endif
#include "gui_debug_rewind.h"
#include "nfd/nfd.h"

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

static char brk_address_vram[10] = "";
static bool brk_new_vram_read = true;
static bool brk_new_vram_write = true;

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

// Right-click "Save As PNG..." on a debug tile/background/sprite view -
// point 6 of DOCS/ROM_INSPECTOR_PLAN.md. Ported from GearSF7000 upstream's
// gui_debug_tms9918.cpp context menus, adapted to NFD (this codebase's own
// save-dialog library everywhere else) instead of upstream's SDL3 native
// dialog calls - the two aren't interchangeable, so this isn't a copy.
// row_stride lets the caller crop a sub-region out of a larger backing
// buffer (e.g. the visible cols*rows out of a fixed 256x256 texture)
// without an intermediate copy - see emu_save_debug_png.
void save_debug_png_with_dialog(const u8* rgb_data, int width, int height, int row_stride, const char* default_name)
{
    ScopedDebugFileDialogAudioPause audioPause;
    nfdchar_t* out_path = nullptr;
    nfdfilteritem_t filter[1] = { { "PNG Files", "png" } };
    nfdresult_t result = NFD_SaveDialog(&out_path, filter, 1, nullptr, default_name);
    if (result == NFD_OKAY)
    {
        emu_save_debug_png(out_path, rgb_data, width, height, row_stride);
        NFD_FreePath(out_path);
    }
    else if (result != NFD_CANCEL)
    {
        Log("Save PNG Error: %s", NFD_GetError());
    }
}

void save_disassembly_with_dialog(bool full, const char* default_name)
{
    ScopedDebugFileDialogAudioPause audioPause;
    nfdchar_t* out_path = nullptr;
    nfdresult_t result = NFD_SaveDialog(&out_path, nullptr, 0, nullptr, default_name);
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
static void debug_window_vram_registers(void);
static void debug_window_vram(void);
static void debug_window_vram_background(void);
static void debug_window_vram_tiles(void);
static void debug_window_vram_sprites(void);
static void debug_window_vram_regs(void);
#if GEARSF7000_ENABLE_SF7000
static void debug_window_sf7000(void);
#endif
static void debug_window_events(void);
static void debug_window_crt_test_pass(void);
static void debug_window_composite_decode_test(void);
static void debug_window_crt_consumer_test(void);
static void debug_window_crt_lottes_test(void);
#if GEARSF7000_ENABLE_MCP
static void debug_window_mcp_server(void);
static void debug_window_watch_monitor(void);
#endif

static void debug_text_example(void);

#if GEARSF7000_ENABLE_SR1000
static void debug_window_cassette(void);
static void debug_window_cassette_controls(void);
#endif

static void add_symbol(const char* line);
static void add_breakpoint_cpu(void);
static void add_breakpoint_mem(void);
static void add_breakpoint_vram(void);
static void request_goto_address(u16 addr);
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

// Area scale for a VDP debug window's rendered tile/sprite grid - how big
// the whole grid is drawn, not a "zoom" into a fixed region. Every other
// measurement in these windows (image size, grid spacing, hit-test
// rectangles, column offsets) is already derived from this one value, so
// changing it adapts the rest of the window automatically.
static void area_scale_control(const char* label, float* scale)
{
    static const float steps[] = { 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f };
    char preview[16];
    snprintf(preview, sizeof(preview), "%.1fx", (double)*scale);
    ImGui::SetNextItemWidth(gui_combo_width_for_text("4.0x"));
    if (ImGui::BeginCombo(label, preview))
    {
        for (float step : steps)
        {
            char item[16];
            snprintf(item, sizeof(item), "%.1fx", (double)step);
            if (ImGui::Selectable(item, *scale == step))
                *scale = step;
        }
        ImGui::EndCombo();
    }
}

void gui_debug_windows(void)
{
//    debug_text_example();

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
    if (config_debug.show_rewind)
        gui_debug_window_rewind();

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
        if (config_debug.show_video)
            debug_window_vram();
        if (config_debug.show_video_registers)
            debug_window_vram_registers();
        if (config_debug.show_psg)
            gui_debug_psg_window();
#if GEARSF7000_ENABLE_AY
        if (config_debug.show_ay)
            gui_debug_ay_window();
#endif
#if GEARSF7000_ENABLE_SF7000
        if (config_debug.show_sf7000)
            debug_window_sf7000();
#endif
        if (config_debug.show_events)
            debug_window_events();
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

void gui_debug_reset_breakpoints_vram(void)
{
    emu_get_core()->GetMemory()->GetBreakpointsVRAM()->clear();
    brk_address_vram[0] = 0;
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


static void debug_text_example(void)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::SetNextWindowPos(ImVec2(567, 249), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(324, 308), ImGuiCond_FirstUseEver);

    ImGui::Begin("Text Example");
    ImGui::PushFont(gui_unifont_font, 16.0f);

    /*std::string text = "Prova";
    ImGui::Text("%s", text.c_str());*/

    //int offset = 0;
    
    //for (int row = 1; row < 8; row++)
    //{
    //    for (int col = 0; col < 32; col++)
    //    {
    //        ImGui::Text("%c", (row * 32) + col); ImGui::SameLine();
    //    }

    //    ImGui::Text("\n");
    //}

    ImGui::Text("%s\n", "  -------------------------------------   ");
    ImGui::Text("%s\n", "                                          ");
    ImGui::Text("%s\n", "   SEGA SC-3000 BASIC Level 3 ver 1.0     ");
    ImGui::Text("%s\n", "                                          ");
    ImGui::Text("%s\n", "      Export Version With Diereses        ");
    ImGui::Text("%s\n", "                                          ");
    ImGui::Text("%s\n", "       Copyright 1983 (C) by MITEC        ");
    ImGui::Text("%s\n", "   	                                   ");
    ImGui::Text("%s\n", "  -------------------------------------   ");
    ImGui::Text("%s\n", "                                          ");
    ImGui::Text("%s\n", "   26620 Bytes free                       ");
    ImGui::Text("%s\n", "  Ready                                   ");
    ImGui::Text("%c\n", "");

    ImGui::PopFont();

    ImGui::End();
    ImGui::PopStyleVar();
}

#if 0 // Retained only as migration reference; controller lives in gui_debug_memory.cpp.
static void debug_window_memory_legacy(void)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::SetNextWindowPos(ImVec2(567, 249), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(324, 308), ImGuiCond_FirstUseEver);

    ImGui::Begin("Memory Editor", &config_debug.show_memory);

    GearSF7000Core* core = emu_get_core();
    Memory* memory = core->GetMemory();
    Cartridge* cart = core->GetCartridge();
    Video* video = core->GetVideo();
    SF7000* sf7000 = core->GetSF7000();

    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    ImGui::TextColored(cyan, memory->IsSF7000Enabled() ? "  SF-7000: " : "  ROM: ");ImGui::SameLine();

    ImGui::TextColored(magenta, "BANK");ImGui::SameLine();
    ImGui::Text("$%02X", memory->GetRomBank()); ImGui::SameLine();
    
    ImGui::TextColored(magenta, "  ADDRESS");ImGui::SameLine();
    ImGui::Text("$%05X", memory->GetRomBankAddress()); ImGui::SameLine();
    ImGui::TextColored(magenta, "  CONFIG"); ImGui::SameLine();
    cart->GetMapperDescr(mapper_desc);
    ImGui::Text("%s", mapper_desc);  ImGui::SameLine();
    ImGui::TextColored(magenta, "  SF7000"); ImGui::SameLine();

    ImGui::Text("%s", sf7000->IPL_Enabled() ? "ENABLED" : "DISABLED");

    //core->GetSF7000()

	//sf7000->IPL_Enabled();
    //sf7000->
    //if (sf7000->IPL_Enabled())
    {
    }

   

    ImGui::PopFont();



    if (ImGui::BeginTabBar("##memory_tabs", ImGuiTabBarFlags_None))
    {
        // SF-7000 is an external memory/I/O configuration, not a cartridge
        // type. This branch must win as soon as Start SF-7000 enables it.
        if (memory->IsSF7000Enabled())
        {
            if (ImGui::BeginTabItem("IPL ROM 8K", NULL, mem_edit_select == 0 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
                ImGui::PushFont(gui_default_font, gui_get_default_font_size());
                if (mem_edit_select == 0)
                    mem_edit_select = -1;
                current_mem_edit = 0;
                // The physical 8 KiB IPL is mirrored across CPU reads
                // $0000-$3FFF while enabled; inspect its real image here.
                mem_edit[current_mem_edit].Draw(memory->GetBios(), 0x2000, 0x0000);
                ImGui::PopFont();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("SF-7000 RAM 64K", NULL, mem_edit_select == 1 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
                ImGui::PushFont(gui_unifont_font, 16.0f);
                if (mem_edit_select == 1)
                    mem_edit_select = -1;
                current_mem_edit = 1;
                mem_edit[current_mem_edit].Draw(memory->GetSGMRam(), 0x10000, 0x0000);
                ImGui::PopFont();
                ImGui::EndTabItem();
            }
        }
        // CASO SPECIFICO ASC16L
        else if (cart->GetType() == Cartridge::CartridgeTypes::SC3000_ASC16L)
        {
            // TAB 1: La parte fissa (Page 0 e 1)
            if (ImGui::BeginTabItem("ROM FIXED", NULL, mem_edit_select == 0 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
                ImGui::PushFont(gui_default_font, gui_get_default_font_size());
                if (mem_edit_select == 0) mem_edit_select = -1;
                current_mem_edit = 0;
                // Mostriamo i primi 32KB (fissi per ASC16L)
                mem_edit[current_mem_edit].Draw(cart->GetROM(), 0x8000, 0x0000);
                ImGui::PopFont();
                ImGui::EndTabItem();
            }

            // TAB 2: La vista della Page 2 (16KB)
            if (ImGui::BeginTabItem("ROM PAGED", NULL, mem_edit_select == 1 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
                ImGui::PushFont(gui_default_font, gui_get_default_font_size());
                if (mem_edit_select == 1) mem_edit_select = -1;
                current_mem_edit = 1;

                // Recuperiamo il puntatore al banco attivo calcolato dal mapper
                u8* pCurrentBank = cart->GetROM() + memory->GetRomBankAddress();
                // Disegniamo i 16KB visibili tra 0x8000 e 0xBFFF
                mem_edit[current_mem_edit].Draw(pCurrentBank, 0x4000, 0x8000);

                ImGui::PopFont();
                ImGui::EndTabItem();
            }

            //// TAB 3: Tutta la ROM fisica caricata (Dinamica)
            //if (ImGui::BeginTabItem("ROM PHYSICAL", NULL, mem_edit_select == 3 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            //{
            //    ImGui::PushFont(gui_default_font, gui_get_default_font_size());
            //    if (mem_edit_select == 3) mem_edit_select = -1;
            //    current_mem_edit = 3;
            //    // La dimensione qui è dinamica: 128K, 256K, o 512K
            //    mem_edit[current_mem_edit].Draw(cart->GetROM(), cart->GetROMSize(), 0x0000);
            //    ImGui::PopFont();
            //    ImGui::EndTabItem();
            //}

            // TAB RAM 32K (SGM)
            if (ImGui::BeginTabItem("RAM 32K", NULL, mem_edit_select == 2 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
                ImGui::PushFont(gui_default_font, gui_get_default_font_size());
                if (mem_edit_select == 2) mem_edit_select = -1;
                current_mem_edit = 2;
                mem_edit[current_mem_edit].Draw(memory->GetSGMRam(), 32768, 0x8000);
                ImGui::PopFont();
                ImGui::EndTabItem();
            }
        }
        else if (cart->GetType() == Cartridge::CartridgeTypes::SF7000IPL)
        {
            if (ImGui::BeginTabItem("IPL", NULL, mem_edit_select == 0 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
                ImGui::PushFont(gui_default_font, gui_get_default_font_size());
                if (mem_edit_select == 0)
                    mem_edit_select = -1;
                current_mem_edit = 0;
                mem_edit[current_mem_edit].Draw(memory->GetBios(), 0x2000, 0);
                ImGui::PopFont();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("TOP 16K", NULL, mem_edit_select == 0 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
                ImGui::PushFont(gui_default_font, gui_get_default_font_size());
                if (mem_edit_select == 1)
                    mem_edit_select = -1;
                current_mem_edit = 1;
                mem_edit[current_mem_edit].Draw(sf7000->IPL_Enabled() ? memory->GetBios() : memory->GetSGMRam(), 0x2000, 0);
                ImGui::PopFont();
                ImGui::EndTabItem();
            }


            if (ImGui::BeginTabItem("SF-7000 RAM", NULL, mem_edit_select == 2 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
                ImGui::PushFont(gui_default_font, gui_get_default_font_size());
                if (mem_edit_select == 2)
                    mem_edit_select = -1;
                current_mem_edit = 2;
                mem_edit[current_mem_edit].Draw(memory->GetSGMRam(), 0x10000, 0x0000);
                ImGui::PopFont();
                ImGui::EndTabItem();
            }
        }
        else if (cart->GetType() == Cartridge::CartridgeTypes::SC3000_2K)
        {
            if (ImGui::BeginTabItem("ROM", NULL, mem_edit_select == 0 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
                ImGui::PushFont(gui_default_font, gui_get_default_font_size());
                if (mem_edit_select == 0)
                    mem_edit_select = -1;
                current_mem_edit = 0;

                mem_edit[current_mem_edit].Draw(cart->GetROM(), cart->GetROMSize(), 0);
                ImGui::PopFont();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("RAM 2K", NULL, mem_edit_select == 2 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
                ImGui::PushFont(gui_default_font, gui_get_default_font_size());
                if (mem_edit_select == 1)
                    mem_edit_select = -1;
                current_mem_edit = 1;
                mem_edit[current_mem_edit].Draw(memory->GetRam(), 2048, 0xC000);
                ImGui::PopFont();
                ImGui::EndTabItem();
            }
        }
        else if (cart->GetType() == Cartridge::CartridgeTypes::SC3000_32K)
        {
            if (ImGui::BeginTabItem("ROM", NULL, mem_edit_select == 0 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
                ImGui::PushFont(gui_default_font, gui_get_default_font_size());
                if (mem_edit_select == 0)
                    mem_edit_select = -1;
                current_mem_edit = 0;

                mem_edit[current_mem_edit].Draw(cart->GetROM(), cart->GetROMSize(), 0);
                ImGui::PopFont();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("RAM 32K", NULL, mem_edit_select == 2 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
                ImGui::PushFont(gui_default_font, gui_get_default_font_size());
                if (mem_edit_select == 1)
                    mem_edit_select = -1;
                current_mem_edit = 1;
                mem_edit[current_mem_edit].Draw(memory->GetSGMRam(), 32768, 0x8000);
                ImGui::PopFont();
                ImGui::EndTabItem();
            }
        }
        else //if (cart->GetType() == Cartridge::CartridgeTypes::SG1000_1K)
        {
            if (ImGui::BeginTabItem("ROM", NULL, mem_edit_select == 0 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
                ImGui::PushFont(gui_default_font, gui_get_default_font_size());
                if (mem_edit_select == 0)
                    mem_edit_select = -1;
                current_mem_edit = 0;

                mem_edit[current_mem_edit].Draw(cart->GetROM(), cart->GetROMSize(), 0);
                ImGui::PopFont();
                ImGui::EndTabItem();
            }

            if (cart->GetType() == Cartridge::CartridgeTypes::SG1000_1K)
            {
                if (ImGui::BeginTabItem("RAM 1K", NULL, mem_edit_select == 2 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
                {
                    ImGui::PushFont(gui_default_font, gui_get_default_font_size());
                    if (mem_edit_select == 1)
                        mem_edit_select = -1;
                    current_mem_edit = 1;
                    mem_edit[current_mem_edit].Draw(memory->GetRam(), 1024, 0xC000);
                    ImGui::PopFont();
                    ImGui::EndTabItem();
                }
            }
            else //if (cart->GetType() == Cartridge::CartridgeTypes::SG1000_16K) 
            {
                if (ImGui::BeginTabItem("RAM 16K", NULL, mem_edit_select == 2 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
                {
                    ImGui::PushFont(gui_unifont_font, 16.0f);
                    if (mem_edit_select == 2)
                        mem_edit_select = -1;
                    current_mem_edit = 2;
                    mem_edit[current_mem_edit].Draw(memory->GetSGMRam(), 16384, 0xC000);
                    ImGui::PopFont();
                    ImGui::EndTabItem();
                }
            }
        }


        if (ImGui::BeginTabItem("VRAM", NULL, mem_edit_select == 4 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
        {
            ImGui::PushFont(gui_default_font, gui_get_default_font_size());
            if (mem_edit_select == 4)
                mem_edit_select = -1;
            current_mem_edit = 4;
            mem_edit[current_mem_edit].Draw(video->GetVRAM(), 0x4000, 0);
            ImGui::PopFont();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

#endif

static void debug_window_memory(void)
{
    gui_debug_memory_window();
}

static void debug_window_disassembler(void)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::SetNextWindowPos(ImVec2(159, 31), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(401, 641), ImGuiCond_FirstUseEver);

    ImGui::Begin("Disassembler", &config_debug.show_disassembler, ImGuiWindowFlags_MenuBar);

    GearSF7000Core* core = emu_get_core();
    const CpuStateSnapshot proc_state =
        core->GetCpuStateAccess()->GetCpuStateSnapshot();
    Memory* memory = core->GetMemory();
    std::vector<Memory::stDisassembleRecord*>* breakpoints_cpu = memory->GetBreakpointsCPU();
    std::vector<Memory::stMemoryBreakpoint>* breakpoints_mem = memory->GetBreakpointsMem();
    std::vector<Memory::stMemoryBreakpoint>* breakpoints_vram = memory->GetBreakpointsVRAM();

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
                request_goto_address(static_cast<u16>(pc));
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
            request_goto_address((u16)std::stoul(goto_address, 0, 16));
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
            request_goto_address((u16)std::stoul(goto_address, 0, 16));
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
            sprintf(brk_address_cpu, "%04X", selected_record->address);

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


    if (ImGui::CollapsingHeader("VRAM Breakpoints"))
    {
        ImGui::Checkbox("Disable All##diable_all_vram", &emu_debug_disable_breakpoints_vram);

        ImGui::Columns(2, "breakpoints_vram");
        ImGui::SetColumnOffset(1, 100);

        ImGui::Separator();

        ImGui::PushItemWidth(85);
        if (ImGui::InputTextWithHint("##add_breakpoint_vram", "XXXX-XXXX", brk_address_vram, IM_ARRAYSIZE(brk_address_vram), ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue))
        {
            add_breakpoint_vram();
        }
        ImGui::PopItemWidth();

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Use XXXX format for single addresses or XXXX-XXXX for ranges ($0000-$3FFF)");

        ImGui::Checkbox("Read", &brk_new_vram_read);
        ImGui::Checkbox("Write", &brk_new_vram_write);

        if (ImGui::Button("Add##add_vram", ImVec2(85, 0)))
        {
            add_breakpoint_vram();
        }

        if (ImGui::Button("Clear All##clear_all_vram", ImVec2(85, 0)))
        {
            gui_debug_reset_breakpoints_vram();
        }

        ImGui::NextColumn();

        ImGui::BeginChild("breakpoints_vram", ImVec2(0, 130), false);

        int remove = -1;

        for (long unsigned int b = 0; b < breakpoints_vram->size(); b++)
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
            if ((*breakpoints_vram)[b].range)
                ImGui::TextColored(red, "%04X-%04X", (*breakpoints_vram)[b].address1, (*breakpoints_vram)[b].address2);
            else
                ImGui::TextColored(red, "%04X", (*breakpoints_vram)[b].address1);
            if ((*breakpoints_vram)[b].read)
            {
                ImGui::SameLine(); ImGui::TextColored(gray, "R");
            }
            if ((*breakpoints_vram)[b].write)
            {
                ImGui::SameLine(); ImGui::TextColored(gray, "W");
            }
            ImGui::PopFont();
        }

        if (remove >= 0)
        {
            breakpoints_vram->erase(breakpoints_vram->begin() + remove);
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
                        request_goto_address(vec[item].record->jump_address);
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

static void debug_window_processor(void)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::SetNextWindowPos(ImVec2(6, 31), ImGuiCond_FirstUseEver);

    ImGui::Begin("Z80 Status", &config_debug.show_processor, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize);

    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    GearSF7000Core* core = emu_get_core();
    CpuStateAccess* processor = core->GetCpuStateAccess();
    const CpuStateSnapshot proc_state = processor->GetCpuStateSnapshot();


    const std::uint64_t elapsed_tS = processor->GetElapsedTStates();
    const int z80_clock_hz = core->GetVideo()->IsPAL() ? GC_MASTER_CLOCK_PAL : GC_MASTER_CLOCK_NTSC;
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

static void debug_window_vram_registers(void)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::SetNextWindowPos(ImVec2(567, 560), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(260, 329), ImGuiCond_FirstUseEver);

    ImGui::Begin("VDP Registers", &config_debug.show_video_registers);

    debug_window_vram_regs();

    ImGui::End();
    ImGui::PopStyleVar();
}


#if GEARSF7000_ENABLE_SR1000
static void debug_window_cassette(void)
{
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGui::SetNextWindowPos(ImVec2(567, 560), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(260, 360), ImGuiCond_FirstUseEver);

    ImGui::Begin("Cassette", &config_debug.show_cassette, flags);

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
    ImGui::SetNextWindowSize(ImVec2(430, 270), ImGuiCond_FirstUseEver);

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



static const char* debug_event_category_name(DebugEventCategory category)
{
    switch (category)
    {
    case DebugEventCategory::CpuExecute: return "CPU";
    case DebugEventCategory::CpuMemory: return "MEM";
    case DebugEventCategory::Vram: return "VDP";
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

static void debug_events_add_trace_rule(DebugEventManager* manager,
                                        DebugEventCategory category,
                                        u16 start, u16 end, u8 access)
{
    DebugRule rule;
    rule.category = category;
    rule.addressStart = start;
    rule.addressEnd = end;
    rule.accessMask = access;
    rule.actions = DebugEventAction_Log;
    manager->AddRule(rule);
}

static void debug_events_add_device_trace_rule(DebugEventManager* manager,
                                                const char* deviceKey,
                                                const char* spaceKey,
                                                u16 target)
{
    DebugRule rule;
    rule.useProbe = true;
    rule.deviceKey = deviceKey;
    rule.spaceKey = spaceKey;
    rule.category = DebugEventCategory::DeviceState;
    rule.addressStart = target;
    rule.addressEnd = target;
    rule.accessMask = DebugEventAccess_Event;
    rule.actions = DebugEventAction_Log;
    manager->AddRule(rule);
}

static bool debug_event_is_vdp_latch_replacement(const DebugEvent& event)
{
    const DebugDeviceDescriptor* device = FindDebugDeviceDescriptor(event.device);
    return device != NULL && device->typeKey != NULL
        && strcmp(device->typeKey, "ti.tms9918") == 0
        && event.space == DebugCommonSpace::State
        && event.targetV2 == Video::DebugCpuPortLatchReplacement;
}

static void debug_event_unpack_vdp_port_request(u64 packed, bool* write,
                                                u16* address, u8* value)
{
    *write = (packed & (1u << 22)) != 0;
    *address = static_cast<u16>((packed >> 8) & 0x3FFF);
    *value = static_cast<u8>(packed & 0xFF);
}

// Draws the PC as a link into the disassembler. The trace is the one place
// where an address is always worth following, so a single click is enough.
static void debug_event_pc_link(const DebugEvent& event, int id)
{
    ImGui::PushID(id);
    char label[16];
    snprintf(label, sizeof(label), "$%04X", event.pc);
    if (ImGui::Selectable(label, false, ImGuiSelectableFlags_AllowDoubleClick))
        request_goto_address(event.pc);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Go to $%04X in the disassembler.", event.pc);
    }
    ImGui::PopID();
}

// One line describing where on the raster the access landed and how much room
// the CPU actually left before it. Empty when the device published neither.
static void debug_event_format_raster(const DebugEvent& event, char* out,
                                      size_t size)
{
    const DebugEventRasterContext& r = event.raster;
    if (!r.rasterKnown && !r.gapKnown)
    {
        out[0] = 0;
        return;
    }

    char gap[64];
    if (r.gapKnown)
        snprintf(gap, sizeof(gap), "\n%uT since previous port access",
            r.tStatesSincePreviousAccess);
    else
        gap[0] = 0;

    if (r.rasterKnown)
        snprintf(out, size, "\nY=%u (%s)  X=%u dots (%s)  %s calendar%s",
            r.line, GetDebugRasterRegionName(r.verticalRegion),
            r.dot, GetDebugRasterRegionName(r.horizontalRegion),
            GetDebugVdpSlotCalendarName(r.calendar), gap);
    else
        snprintf(out, size, "%s", gap);
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

static void debug_window_events(void)
{
    ImGui::SetNextWindowPos(ImVec2(320, 420), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(620, 260), ImGuiCond_FirstUseEver);
    // Keep old saved ImGui layouts from opening this utility window larger
    // than the available desktop. These are constraints, not a fixed size:
    // the user can still resize it freely within this range.
    ImGui::SetNextWindowSizeConstraints(ImVec2(430, 180), ImVec2(1100, 700));

    GearSF7000Core* core = emu_get_core();
    DebugEventManager* manager = core->GetMemory()->GetDebugEventManager();
    int& categoryFilter = config_debug.events_category_filter;
    bool& newestFirst = config_debug.events_newest_first;
    bool& groupByPc = config_debug.events_group_by_pc;
    bool& normalizeClock = config_debug.events_normalize_clock;
    std::vector<DebugEvent> events;
    manager->CopyEvents(events);

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
        // This is an index into categoryNames, not a DebugEventCategory enum value.
        static int newCategory = 3; // VDP (CPU data-port / VRAM transfers)
        static char startText[5] = "3E0A";
        static char endText[5] = "3E0A";
        static bool onRead = false, onWrite = true, onEvent = false;
        static bool onPause = true, onLog = true, oneShot = false;
        static int condition = static_cast<int>(DebugValueCondition::Any), hit = 1;
        static char maskText[3] = "FF", beforeText[3] = "20", afterText[3] = "90";
        // Keep the three facets of one TMS9918 operation together: CPU/VRAM
        // accesses, register writes and the deferred CPU-port latch.
        const char* categoryNames[] = { "CPU EXEC", "MEM", "I/O", "VDP", "VDP REG", "VDP CPU LATCH", "VIDEO", "PPI", "FDC", "TAPE", "AUDIO", "AY" };
        const DebugEventCategory categoryValues[] = { DebugEventCategory::CpuExecute, DebugEventCategory::CpuMemory, DebugEventCategory::Io, DebugEventCategory::Vram, DebugEventCategory::VdpRegister, DebugEventCategory::DeviceState, DebugEventCategory::VideoTiming, DebugEventCategory::Ppi, DebugEventCategory::Fdc, DebugEventCategory::Tape, DebugEventCategory::Audio, DebugEventCategory::AyExpansion };
        const u16 categoryDefaultStart[] = { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, Video::DebugCpuPortLatchReplacement, 0x0000, 0x00DC, 0x0000, 0x0000, 0x0000, 0x0000 };
        const u16 categoryDefaultEnd[] = { 0x0000, 0x0000, 0x0000, 0x0000, 0x0007, Video::DebugCpuPortLatchReplacement, 0x0000, 0x00DF, 0x0003, 0x0004, 0x0007, 0x000F };
        const char* categoryTargetLabel[] = { "Address", "Address", "Port", "Address", "Register", "Event", "Scanline", "Port", "Event", "Event", "Register", "Register" };
        const char* categoryTargetHint[] = { "max $FFFF", "max $FFFF", "ports $00-$FF", "max $3FFF", "registers $00-$FF", "CPU-port latch replaced", "max $FFFF", "SC $DC-$DF; SF $E4-$E7", "events $00-$03", "events $00-$04", "registers $00-$07", "registers $00-$0F" };
        enum BreakpointAccessKind { AccessExecute, AccessReadWrite, AccessWrite, AccessEvent, AccessReadWriteEvent };
        const BreakpointAccessKind categoryAccess[] = { AccessExecute, AccessReadWrite, AccessReadWrite, AccessReadWrite, AccessWrite, AccessEvent, AccessEvent, AccessReadWriteEvent, AccessEvent, AccessEvent, AccessEvent, AccessEvent };
        const int vdpLatchCategory = 5;
        const auto isVdpLatchTarget = [&]() { return newCategory == vdpLatchCategory; };
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
        const bool needsMask = condition == 1 || condition == 2 || condition == 3
            || condition == 9 || condition == 10 || condition == 11
            || condition == 12 || condition == 13;
        const bool needsBefore = condition == 2 || condition == 3;
        const bool needsAfter = condition == 1 || condition == 3;
        bool addBreakpointRequested = false;
        DebugRule pendingRule;
        const char* validationMessage = NULL;
        const auto parseTargetRange = [&](u16* start, u16* end)
        {
            if (isVdpLatchTarget())
            {
                *start = Video::DebugCpuPortLatchReplacement;
                *end = Video::DebugCpuPortLatchReplacement;
                return true;
            }
            try
            {
                const u32 parsedStart = std::stoul(startText, 0, 16);
                const u32 parsedEnd = std::stoul(endText, 0, 16);
                const u16 targetMax = GetDebugEventCategoryCapabilities(categoryValues[newCategory]).targetMax;
                if (parsedStart > targetMax || parsedEnd > targetMax || parsedStart > parsedEnd)
                    return false;
                *start = static_cast<u16>(parsedStart);
                *end = static_cast<u16>(parsedEnd);
                return true;
            }
            catch (...) { return false; }
        };
        const auto selectedAccessMask = [&]() -> u8
        {
            switch (categoryAccess[newCategory])
            {
            case AccessExecute: return DebugEventAccess_Execute;
            case AccessReadWrite: return (onRead ? DebugEventAccess_Read : 0) | (onWrite ? DebugEventAccess_Write : 0);
            case AccessWrite: return DebugEventAccess_Write;
            case AccessEvent: return DebugEventAccess_Event;
            case AccessReadWriteEvent: return (onRead ? DebugEventAccess_Read : 0)
                | (onWrite ? DebugEventAccess_Write : 0)
                | (onEvent ? DebugEventAccess_Event : 0);
            }
            return DebugEventAccess_None;
        };
        const auto buildDraftRule = [&](DebugRule* rule) -> const char*
        {
            u16 start = 0, end = 0;
            if (!parseTargetRange(&start, &end))
                return "Invalid target range";
            try
            {
                const u32 mask = std::stoul(maskText, 0, 16);
                const u32 beforeExpected = std::stoul(beforeText, 0, 16);
                const u32 afterExpected = std::stoul(afterText, 0, 16);
                *rule = DebugRule();
                rule->category = categoryValues[newCategory];
                rule->addressStart = start;
                rule->addressEnd = end;
                rule->accessMask = selectedAccessMask();
                rule->valueCondition = conditionValues[condition];
                rule->valueMask = static_cast<u8>(mask);
                rule->beforeValueExpected = static_cast<u8>(beforeExpected);
                rule->valueExpected = static_cast<u8>(afterExpected);
                rule->breakOnHit = static_cast<u64>(std::max(1, hit));
                rule->oneShot = oneShot;
                rule->actions = (onLog ? DebugEventAction_Log : 0)
                    | (onPause ? DebugEventAction_Pause : 0);
                if (isVdpLatchTarget())
                {
                    const DebugDeviceRegistry& registry = GetGearDebugDeviceRegistry();
                    const DebugDeviceDescriptor* device =
                        registry.FindDevice("gearsf7000.sc3000.vdp");
                    const DebugSpaceDescriptor* space = device != NULL
                        ? registry.FindSpace(device->id, "state") : NULL;
                    if (device == NULL || space == NULL)
                        return "VDP latch debug target is unavailable";
                    rule->useProbe = true;
                    rule->device = device->id;
                    rule->space = space->id;
                    rule->deviceKey = device->key;
                    rule->spaceKey = space->key;
                    rule->category = DebugEventCategory::DeviceState;
                    rule->valueCondition = DebugValueCondition::Any;
                    rule->useValueCondition = false;
                }
                return ValidateDebugRule(*rule);
            }
            catch (...)
            {
                return "Invalid hexadecimal value";
            }
        };
        const auto drawTargetEditor = [&]()
        {
            ImGui::SetNextItemWidth(gui_combo_width_for_array(categoryNames, IM_ARRAYSIZE(categoryNames)));
            bool categoryChanged = false;
            if (ImGui::BeginCombo("##type", categoryNames[newCategory], ImGuiComboFlags_HeightLargest))
            {
                for (int categoryIndex = 0; categoryIndex < IM_ARRAYSIZE(categoryNames); ++categoryIndex)
                {
                    const bool selected = newCategory == categoryIndex;
                    if (ImGui::Selectable(categoryNames[categoryIndex], selected))
                    {
                        newCategory = categoryIndex;
                        categoryChanged = true;
                    }
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (categoryChanged)
            {
                snprintf(startText, IM_ARRAYSIZE(startText), "%04X", categoryDefaultStart[newCategory]);
                snprintf(endText, IM_ARRAYSIZE(endText), "%04X", categoryDefaultEnd[newCategory]);
                if (categoryAccess[newCategory] == AccessExecute || isVdpLatchTarget())
                    condition = 0;
            }
            if (isVdpLatchTarget())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("CPU-port latch replacement");
                return;
            }
            ImGui::SameLine(); ImGui::Text("%s", categoryTargetLabel[newCategory]); ImGui::SameLine();
            ImGui::SetNextItemWidth(48); ImGui::InputText("##start", startText, IM_ARRAYSIZE(startText), ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::SameLine(); ImGui::Text("to"); ImGui::SameLine();
            ImGui::SetNextItemWidth(48); ImGui::InputText("##end", endText, IM_ARRAYSIZE(endText), ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::SameLine();
            u16 validStart = 0, validEnd = 0;
            if (parseTargetRange(&validStart, &validEnd))
                ImGui::TextDisabled("%s", categoryTargetHint[newCategory]);
            else
                ImGui::TextColored(red, "invalid (max $%04X)",
                    GetDebugEventCategoryCapabilities(categoryValues[newCategory]).targetMax);
        };
        const auto drawAccessEditor = [&]()
        {
            switch (categoryAccess[newCategory])
            {
            case AccessExecute: ImGui::Text("Execute"); break;
            case AccessReadWrite: ImGui::Checkbox("Read", &onRead); ImGui::SameLine(); ImGui::Checkbox("Write", &onWrite); break;
            case AccessWrite: ImGui::Text("Write"); break;
            case AccessEvent: ImGui::Text("Event"); break;
            case AccessReadWriteEvent:
                ImGui::Checkbox("Read", &onRead); ImGui::SameLine();
                ImGui::Checkbox("Write", &onWrite); ImGui::SameLine();
                ImGui::Checkbox("State", &onEvent); break;
            }
        };
        const auto drawConditionEditor = [&]()
        {
            const bool hasValue = categoryAccess[newCategory] != AccessExecute
                && !isVdpLatchTarget();
            ImGui::BeginDisabled(!hasValue);
            ImGui::SetNextItemWidth(gui_combo_width_for_array(conditionNames, IM_ARRAYSIZE(conditionNames))); ImGui::Combo("##condition", &condition, conditionNames, IM_ARRAYSIZE(conditionNames));
            ImGui::EndDisabled();
        };
        const auto drawValuesEditor = [&]()
        {
            if (needsMask) { ImGui::Text("Mask"); ImGui::SameLine(); ImGui::SetNextItemWidth(32); ImGui::InputText("##mask", maskText, IM_ARRAYSIZE(maskText), ImGuiInputTextFlags_CharsHexadecimal); }
            if (needsBefore) { if (needsMask) ImGui::SameLine(); ImGui::Text("Old"); ImGui::SameLine(); ImGui::SetNextItemWidth(32); ImGui::InputText("##before", beforeText, IM_ARRAYSIZE(beforeText), ImGuiInputTextFlags_CharsHexadecimal); }
            if (needsAfter) { if (needsMask || needsBefore) ImGui::SameLine(); ImGui::Text("New"); ImGui::SameLine(); ImGui::SetNextItemWidth(32); ImGui::InputText("##after", afterText, IM_ARRAYSIZE(afterText), ImGuiInputTextFlags_CharsHexadecimal); }
            if (!needsMask && !needsBefore && !needsAfter) ImGui::TextDisabled("Not required");
        };
        const auto drawActionEditor = [&]()
        {
            ImGui::Checkbox("Log", &onLog); ImGui::SameLine(); ImGui::Checkbox("Pause", &onPause);
        };
        const auto drawTriggerEditor = [&]()
        {
            ImGui::Text("Mode"); ImGui::SameLine();
            const char* triggerModes[] = { "Repeat", "One-shot" };
            int triggerMode = oneShot ? 1 : 0;
            ImGui::SetNextItemWidth(gui_combo_width_for_array(triggerModes, IM_ARRAYSIZE(triggerModes))); if (ImGui::Combo("##trigger_mode", &triggerMode, triggerModes, IM_ARRAYSIZE(triggerModes))) oneShot = triggerMode == 1;
            ImGui::SameLine(); ImGui::Text("Pause from hit"); ImGui::SameLine(); ImGui::SetNextItemWidth(64); ImGui::InputInt("##hit", &hit); ImGui::SameLine();
            validationMessage = buildDraftRule(&pendingRule);
            const bool canAdd = validationMessage == NULL;
            ImGui::BeginDisabled(!canAdd);
            addBreakpointRequested = ImGui::Button("Add breakpoint");
            ImGui::EndDisabled();
        };

        const bool wideBreakpointEditor = ImGui::GetContentRegionAvail().x >= 900.0f;
        if (wideBreakpointEditor && ImGui::BeginTable("##breakpoint_editor_wide", 4,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
        {
            ImGui::TableSetupColumn("LabelLeft", ImGuiTableColumnFlags_WidthFixed, 82.0f);
            ImGui::TableSetupColumn("EditorLeft", ImGuiTableColumnFlags_WidthStretch, 0.54f);
            ImGui::TableSetupColumn("LabelRight", ImGuiTableColumnFlags_WidthFixed, 92.0f);
            ImGui::TableSetupColumn("EditorRight", ImGuiTableColumnFlags_WidthStretch, 0.46f);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::AlignTextToFramePadding(); ImGui::TextColored(cyan, "TARGET");
            ImGui::TableSetColumnIndex(1); drawTargetEditor();
            ImGui::TableSetColumnIndex(2); ImGui::AlignTextToFramePadding(); ImGui::TextColored(cyan, "ACCESS");
            ImGui::TableSetColumnIndex(3); drawAccessEditor();

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::AlignTextToFramePadding(); ImGui::TextColored(cyan, "BREAK WHEN");
            ImGui::TableSetColumnIndex(1); drawConditionEditor();
            ImGui::TableSetColumnIndex(2); ImGui::AlignTextToFramePadding(); ImGui::TextColored(cyan, "VALUES");
            ImGui::TableSetColumnIndex(3); drawValuesEditor();

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%s", conditionHelp[condition]);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::AlignTextToFramePadding(); ImGui::TextColored(cyan, "ACTION");
            ImGui::TableSetColumnIndex(1); drawActionEditor();
            ImGui::TableSetColumnIndex(2); ImGui::AlignTextToFramePadding(); ImGui::TextColored(cyan, "TRIGGER");
            ImGui::TableSetColumnIndex(3); drawTriggerEditor();
            ImGui::EndTable();
        }
        else if (!wideBreakpointEditor && ImGui::BeginTable("##breakpoint_editor_narrow", 2,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
        {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 92.0f);
            ImGui::TableSetupColumn("Editor", ImGuiTableColumnFlags_WidthStretch);
            const auto narrowRow = [&](const char* label, const auto& drawEditor)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::AlignTextToFramePadding(); ImGui::TextColored(cyan, "%s", label);
                ImGui::TableSetColumnIndex(1); drawEditor();
            };
            narrowRow("TARGET", drawTargetEditor);
            narrowRow("ACCESS", drawAccessEditor);
            narrowRow("BREAK WHEN", drawConditionEditor);
            if (needsMask || needsBefore || needsAfter) narrowRow("VALUES", drawValuesEditor);
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%s", conditionHelp[condition]);
            narrowRow("ACTION", drawActionEditor);
            narrowRow("TRIGGER", drawTriggerEditor);
            ImGui::EndTable();
        }
        if (validationMessage != NULL)
            ImGui::TextColored(red, "%s", validationMessage);
        if (addBreakpointRequested)
            manager->AddRule(pendingRule);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(cyan, "ACTIVE BREAKPOINTS"); ImGui::SameLine();
        ImGui::TextColored(gray, "(%d)", static_cast<int>(manager->GetRules().size()));
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear all##manager")) manager->ClearRules();
        const std::vector<DebugRule>& rules = manager->GetRules();
        if (!rules.empty() && ImGui::BeginTable("##active_breakpoints", 7,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("ON", ImGuiTableColumnFlags_WidthFixed, 28.0f);
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 34.0f);
            ImGui::TableSetupColumn("TARGET", ImGuiTableColumnFlags_WidthFixed, 155.0f);
            ImGui::TableSetupColumn("ACCESS", ImGuiTableColumnFlags_WidthFixed, 52.0f);
            ImGui::TableSetupColumn("CONDITION", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("MODE / HITS", ImGuiTableColumnFlags_WidthFixed, 108.0f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 76.0f);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < rules.size(); ++i)
            {
                const DebugRule& rule = rules[i];
                char conditionText[96];
                debug_rule_condition_text(rule, conditionText, sizeof(conditionText));
                ImGui::PushID(static_cast<int>(rule.id));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); bool enabled = rule.enabled; if (ImGui::Checkbox("##enabled_manager", &enabled)) manager->GetRules()[i].enabled = enabled;
                ImGui::TableSetColumnIndex(1); ImGui::Text("R%u", rule.id);
                ImGui::TableSetColumnIndex(2);
                if (rule.useProbe)
                    ImGui::TextColored(debug_event_category_color(DebugEventCategory::DeviceState),
                        "%s / %s $%X-$%X", rule.deviceKey.c_str(), rule.spaceKey.c_str(),
                        rule.addressStart, rule.addressEnd);
                else
                    ImGui::TextColored(debug_event_category_color(rule.category), "%s $%04X-$%04X",
                        debug_event_category_name(rule.category), rule.addressStart, rule.addressEnd);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%s", debug_event_access_name(rule.accessMask));
                ImGui::TableSetColumnIndex(4); ImGui::Text("%s", conditionText);
                ImGui::TableSetColumnIndex(5); ImGui::Text("%s / %llu", rule.oneShot ? "1-shot" : "repeat", rule.hitCount);
                ImGui::TableSetColumnIndex(6); if (ImGui::SmallButton("Reset")) manager->GetRules()[i].hitCount = 0; ImGui::SameLine(); if (ImGui::SmallButton("X")) { const u32 id = rule.id; ImGui::PopID(); manager->RemoveRule(id); break; }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Trace presets"))
    {
        if (ImGui::Button("PSG"))
            debug_events_add_trace_rule(manager, DebugEventCategory::Audio, 0, 7, DebugEventAccess_Event);
        ImGui::SameLine();
#if GEARSF7000_ENABLE_AY
        // All sixteen registers, unlike the PSG's eight logical ones.
        if (ImGui::Button("AY"))
            debug_events_add_trace_rule(manager, DebugEventCategory::AyExpansion, 0, 15, DebugEventAccess_Event);
        ImGui::SameLine();
#endif
        if (ImGui::Button("VDP latch"))
            debug_events_add_device_trace_rule(manager,
                "gearsf7000.sc3000.vdp", "state",
                Video::DebugCpuPortLatchReplacement);
        ImGui::SameLine();
        if (ImGui::Button("PPI"))
            debug_events_add_trace_rule(manager, DebugEventCategory::Ppi, 0xDC, 0xDF,
                DebugEventAccess_Read | DebugEventAccess_Write | DebugEventAccess_Event);
        ImGui::SameLine();
        if (ImGui::Button("FDC"))
            debug_events_add_trace_rule(manager, DebugEventCategory::Fdc, 0, 3, DebugEventAccess_Event);
        ImGui::SameLine();
        if (ImGui::Button("Tape"))
            debug_events_add_trace_rule(manager, DebugEventCategory::Tape, 0, 4, DebugEventAccess_Event);
        ImGui::SameLine();
        if (ImGui::Button("VBlank"))
        {
            DebugRule rule;
            rule.category = DebugEventCategory::VideoTiming;
            rule.addressStart = 192;
            rule.addressEnd = 192;
            rule.accessMask = DebugEventAccess_Event;
            rule.useValueCondition = true;
            rule.valueMask = 0x04;
            rule.valueExpected = 0x04;
            rule.actions = DebugEventAction_Log;
            manager->AddRule(rule);
        }
    }

    // Indexed by DebugEventCategory value plus one for the leading "All", so
    // this list has to track the enum's order exactly - a mismatch silently
    // filters by the wrong category rather than failing.
    const char* categories[] = { "All", "CPU", "MEM", "VDP", "I/O", "VDP REG", "FDC", "PPI", "VIDEO", "TAPE", "AUDIO", "AY", "STATE" };
    int categorySelection = categoryFilter + 1;
    ImGui::SetNextItemWidth(gui_combo_width_for_array(categories, IM_ARRAYSIZE(categories)));
    if (ImGui::Combo("Category", &categorySelection, categories, IM_ARRAYSIZE(categories)))
        categoryFilter = categorySelection - 1;

    u64 clockBase = 0;
    bool hasClockBase = false;
    if (normalizeClock)
    {
        for (const DebugEvent& event : events)
        {
            const DebugEventCategory category = static_cast<DebugEventCategory>(event.category);
            if (categoryFilter >= 0 && static_cast<int>(category) != categoryFilter)
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
            const DebugEventCategory category = static_cast<DebugEventCategory>(event.category);
            if (categoryFilter >= 0 && static_cast<int>(category) != categoryFilter)
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
            group.mixedCategory |= event.category != firstEvent.category;
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
                const DebugEventCategory category = static_cast<DebugEventCategory>(event.category);
                text << '$' << std::hex << std::uppercase << event.pc << std::dec << '\t'
                    << (group.mixedCategory ? "MIXED" : (debug_event_is_vdp_latch_replacement(event)
                        ? "VDP CPU LATCH" : debug_event_category_name(category))) << '\t'
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
            const DebugEventCategory category = static_cast<DebugEventCategory>(event.category);
            if (categoryFilter >= 0 && static_cast<int>(category) != categoryFilter)
                continue;
            char target[16];
            char value[24];
            sprintf(target, "$%04X", event.targetV2);
            if (event.valueKnown)
                sprintf(value, "$%0*llX", std::max(2, (event.valueWidth + 3) / 4),
                    static_cast<unsigned long long>(event.afterValueV2));
            else
                strcpy(value, "--");
            const u64 displayedClock = hasClockBase ? event.clock - clockBase : event.clock;
            text << displayedClock << '\t' << '$' << std::hex << std::uppercase
                << event.pc << std::dec << '\t' << debug_event_category_name(category)
                << '\t' << GetDebugDeviceName(event.device) << '\t'
                << GetDebugSpaceName(event.space) << '\t' << target << '\t' << value << '\t' << event.ruleId << '\t'
                << ((event.actions & DebugEventAction_Pause) ? "PAUSE" : "LOG") << '\n';
        }
        }
        ImGui::SetClipboardText(text.str().c_str());
    }

    ImGui::Separator();
    if (groupByPc)
    {
        ImGui::Columns(5, "debug_events_group_columns", false);
        ImGui::SetColumnWidth(0, 58.0f); ImGui::TextColored(gray, "PC"); ImGui::NextColumn();
        ImGui::SetColumnWidth(1, 112.0f); ImGui::TextColored(gray, "TYPE"); ImGui::NextColumn();
        ImGui::SetColumnWidth(2, 58.0f); ImGui::TextColored(gray, "ACCESS"); ImGui::NextColumn();
        ImGui::SetColumnWidth(3, 58.0f); ImGui::TextColored(gray, "COUNT"); ImGui::NextColumn();
        ImGui::TextColored(gray, "RULE / ACTION"); ImGui::NextColumn();
    }
    else
    {
        ImGui::Columns(9, "debug_events_columns", false);
        ImGui::SetColumnWidth(0, 72.0f); ImGui::TextColored(gray, "CLOCK"); ImGui::NextColumn();
        ImGui::SetColumnWidth(1, 58.0f); ImGui::TextColored(gray, "PC"); ImGui::NextColumn();
        ImGui::SetColumnWidth(2, 60.0f); ImGui::TextColored(gray, "TYPE"); ImGui::NextColumn();
        ImGui::SetColumnWidth(3, 52.0f); ImGui::TextColored(gray, "ACCESS"); ImGui::NextColumn();
        ImGui::SetColumnWidth(4, 65.0f); ImGui::TextColored(gray, "TARGET"); ImGui::NextColumn();
        ImGui::SetColumnWidth(5, 55.0f); ImGui::TextColored(gray, "BEFORE"); ImGui::NextColumn();
        ImGui::SetColumnWidth(6, 55.0f); ImGui::TextColored(gray, "AFTER"); ImGui::NextColumn();
        ImGui::SetColumnWidth(7, 140.0f); ImGui::TextColored(gray, "RASTER / GAP"); ImGui::NextColumn();
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
            const DebugEventCategory category = static_cast<DebugEventCategory>(event.category);
            const ImVec4 color = group.mixedCategory ? gray : debug_event_category_color(category);
            ImGui::Columns(5, "debug_events_group_rows", false);
            ImGui::SetColumnWidth(0, 58.0f); ImGui::Text("$%04X", event.pc); ImGui::NextColumn();
            ImGui::SetColumnWidth(1, 112.0f); ImGui::TextColored(color, "%s", group.mixedCategory ? "MIXED" : (debug_event_is_vdp_latch_replacement(event) ? "VDP CPU LATCH" : debug_event_category_name(category))); ImGui::NextColumn();
            ImGui::SetColumnWidth(2, 58.0f); ImGui::Text("%s", group.mixedAccess ? "MIXED" : debug_event_access_name(event.access)); ImGui::NextColumn();
            ImGui::SetColumnWidth(3, 58.0f); ImGui::Text("%u", group.count);
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
            const DebugEventCategory category = static_cast<DebugEventCategory>(event.category);
            if (categoryFilter >= 0 && static_cast<int>(category) != categoryFilter)
                continue;
            const ImVec4 color = debug_event_category_color(category);
            ImGui::Columns(9, "debug_events_rows", false);
            const u64 displayedClock = hasClockBase ? event.clock - clockBase : event.clock;
            ImGui::SetColumnWidth(0, 72.0f); ImGui::Text("%llu", displayedClock); ImGui::NextColumn();
            ImGui::SetColumnWidth(1, 58.0f); debug_event_pc_link(event, i); ImGui::NextColumn();
            ImGui::SetColumnWidth(2, 60.0f); ImGui::TextColored(color, "%s", debug_event_category_name(category));
            if (ImGui::IsItemHovered())
            {
                char raster[192];
                debug_event_format_raster(event, raster, sizeof(raster));
                if (debug_event_is_vdp_latch_replacement(event)
                    && event.beforeKnown && event.afterKnown)
                {
                    bool oldWrite = false, newWrite = false;
                    u16 oldAddress = 0, newAddress = 0;
                    u8 oldValue = 0, newValue = 0;
                    debug_event_unpack_vdp_port_request(event.beforeValueV2,
                        &oldWrite, &oldAddress, &oldValue);
                    debug_event_unpack_vdp_port_request(event.afterValueV2,
                        &newWrite, &newAddress, &newValue);
                    ImGui::SetTooltip(
                        "%s\n%s\n%s\nDiscarded: %s $%04X = $%02X\nReplacement: %s $%04X = $%02X%s",
                        GetDebugDeviceName(event.device), GetDebugSpaceName(event.space),
                        GetDebugEventPhaseName(event.phase), oldWrite ? "WRITE" : "READ",
                        oldAddress, oldValue, newWrite ? "WRITE" : "READ",
                        newAddress, newValue, raster);
                }
                else
                    ImGui::SetTooltip("%s\n%s\n%s%s", GetDebugDeviceName(event.device),
                        GetDebugSpaceName(event.space), GetDebugEventPhaseName(event.phase),
                        raster);
            }
            ImGui::NextColumn();
            ImGui::SetColumnWidth(3, 52.0f); ImGui::Text("%s", debug_event_access_name(event.access)); ImGui::NextColumn();
            const int valueDigits = std::max(2, (event.valueWidth + 3) / 4);
            ImGui::SetColumnWidth(4, 65.0f); ImGui::Text("$%04X", event.targetV2); ImGui::NextColumn();
            ImGui::SetColumnWidth(5, 55.0f); event.beforeKnown
                ? ImGui::Text("$%0*llX", valueDigits, static_cast<unsigned long long>(event.beforeValueV2))
                : ImGui::Text("--"); ImGui::NextColumn();
            ImGui::SetColumnWidth(6, 55.0f); event.afterKnown
                ? ImGui::Text("$%0*llX", valueDigits, static_cast<unsigned long long>(event.afterValueV2))
                : ImGui::Text("--"); ImGui::NextColumn();

            ImGui::SetColumnWidth(7, 140.0f);
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
                    ImGui::SetTooltip("Y %u (line): %s\nX %u (dots): %s\n%s calendar",
                        r.line, GetDebugRasterRegionName(r.verticalRegion),
                        r.dot, GetDebugRasterRegionName(r.horizontalRegion),
                        GetDebugVdpSlotCalendarName(r.calendar));
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

    ImGui::SetNextWindowPos(ImVec2(320, 420), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 160), ImGuiCond_FirstUseEver);

    ImGui::Begin("MCP Server", &config_debug.show_mcp_server, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize);
    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    const bool mcp_running = emu_mcp_is_running();
    const int transport_mode = emu_mcp_get_transport_mode();
    const bool command_line_owned = emu_mcp_started_from_command_line();
    const bool http_running = mcp_running && (transport_mode == MCP_TRANSPORT_TCP);
    const bool stdio_running = mcp_running && (transport_mode == MCP_TRANSPORT_STDIO);

    if (!mcp_running)
    {
        if (ImGui::Button("Start HTTP Server", ImVec2(150, 0)))
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
        if (ImGui::Button("Stop HTTP Server", ImVec2(150, 0)))
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
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputText("##mcp_address", http_address, IM_ARRAYSIZE(http_address), ImGuiInputTextFlags_AutoSelectAll))
        config_emulator.mcp_http_address = http_address;

    ImGui::Text("HTTP Port:  ");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
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
    ImGui::SetNextWindowSize(ImVec2(620, 480), ImGuiCond_FirstUseEver);

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
    ImGui::SetNextWindowSize(ImVec2(1180, 620), ImGuiCond_FirstUseEver);
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

    if (ImGui::BeginTable("crt_test_pass_compare", 2, ImGuiTableFlags_SizingStretchSame))
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Source (emu_texture)");
        ImGui::Image(renderer_emu_texture, image_size);

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted("Blurred (GPU test pass output)");
        ImGui::Image(renderer_crt_test_texture, image_size);

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

    ImGui::SetNextWindowSize(ImVec2(700, 700), ImGuiCond_FirstUseEver);
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
    ImGui::SetNextWindowSize(ImVec2(1180, 760), ImGuiCond_FirstUseEver);
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

    const float paneHeight = 620.0f;
    if (ImGui::BeginChild("crt_consumer_source_pane", ImVec2(sourceSize.x + 20.0f, paneHeight), true))
    {
        ImGui::TextUnformatted("Source (emu_texture)");
        ImGui::Image(renderer_emu_texture, sourceSize);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    if (ImGui::BeginChild("crt_consumer_output_pane", ImVec2(0.0f, paneHeight), true))
    {
        ImGui::TextUnformatted("crt_consumer output (native size - scroll to inspect)");
        ImGui::Image(renderer_crt_consumer_texture, outputSize);
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
    ImGui::SetNextWindowSize(ImVec2(1180, 760), ImGuiCond_FirstUseEver);
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

    const float paneHeight = 620.0f;
    if (ImGui::BeginChild("crt_lottes_source_pane", ImVec2(sourceSize.x + 20.0f, paneHeight), true))
    {
        ImGui::TextUnformatted("Source (emu_texture)");
        ImGui::Image(renderer_emu_texture, sourceSize);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    if (ImGui::BeginChild("crt_lottes_output_pane", ImVec2(0.0f, paneHeight), true))
    {
        ImGui::TextUnformatted("crt_lottes output (native size - scroll to inspect)");
        ImGui::Image(renderer_crt_lottes_texture, outputSize);
    }
    ImGui::EndChild();

    ImGui::End();
}

static void debug_window_vram(void)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::SetNextWindowPos(ImVec2(896, 31), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(668, 640), ImGuiCond_FirstUseEver);

    ImGui::Begin("VDP Viewer", &config_debug.show_video);

    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    Video* video = emu_get_core()->GetVideo();
    u8* regs = video->GetRegisters();

    ImGui::TextColored(cyan, "  VIDEO MODE:");ImGui::SameLine();
    ImGui::Text("$%02X", video->GetMode()); ImGui::SameLine();

    ImGui::TextColored(magenta, "  M1:");ImGui::SameLine();
    ImGui::Text("%d", (regs[1] >> 4) & 0x01); ImGui::SameLine();
    ImGui::TextColored(magenta, "  M2:");ImGui::SameLine();
    ImGui::Text("%d", (regs[0] >> 1) & 0x01); ImGui::SameLine();
    ImGui::TextColored(magenta, "  M3:");ImGui::SameLine();
    ImGui::Text("%d", (regs[1] >> 3) & 0x01);

    ImGui::PopFont();

    if (ImGui::BeginTabBar("##vram_tabs", ImGuiTabBarFlags_None))
    {
        if (ImGui::BeginTabItem("Name Table"))
        {
            debug_window_vram_background();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Pattern Table"))
        {
            debug_window_vram_tiles();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Sprites"))
        {
            debug_window_vram_sprites();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

static void debug_window_vram_background(void)
{
    Video* video = emu_get_core()->GetVideo();
    GC_RuntimeInfo runtime;
    emu_get_runtime(runtime);
    u8* regs = video->GetRegisters();
    u8* vram = video->GetVRAM();
    int mode = video->GetMode();

    bool& show_grid = config_debug.show_grid_background;
    bool& show_regions = config_debug.show_regions_background;
    int cols = (mode == 1) ? 40 : 32;
    int rows = 24;
    float& scale = config_debug.vram_background_area_scale;
    float tile_width = (mode == 1) ? 6.0f : 8.0f;
    float size_h = ((mode == 1) ? (6.0f * 40.0f) : 256.0f) * scale;
    float size_v = 8.0f * rows * scale;
    float spacing_h = ((mode == 1) ? 6.0f : 8.0f) * scale;
    float spacing_v = 8.0f * scale;
    float uv_h = (mode == 1) ? 0.0234375f : 1.0f / 32.0f;
    float uv_v = 1.0f / 32.0f;

    ImGui::Checkbox("Show Grid##grid_bg", &show_grid);
    ImGui::SameLine();
    area_scale_control("Scale##scale_bg", &scale);
    ImGui::SameLine();
    ImGui::Checkbox("Show Regions##regions_bg", &show_regions);

    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    ImGui::Columns(2, "bg", false);
    ImGui::SetColumnOffset(1, size_h + 10.0f);

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();

    renderer_begin_emulator_image();
    ImGui::Image((ImTextureID)(intptr_t)renderer_emu_debug_vram_background, ImVec2(size_h, size_v), ImVec2(0.0f, 0.0f), ImVec2(uv_h * cols, uv_v * rows));
    renderer_end_emulator_image();

    if (ImGui::BeginPopupContextItem("##bg_context"))
    {
        if (ImGui::Selectable("Save Background As PNG..."))
        {
            // Crop to the visible cols*rows, not the buffer's full fixed
            // 256x256 allocation - the rest is stale/unused for this mode.
            int crop_width = (mode == 1) ? (int)(6.0f * 40.0f) : 256;
            int crop_height = rows * 8;
            save_debug_png_with_dialog(emu_debug_background_buffer, crop_width, crop_height, 256 * 3, "background.png");
        }
        ImGui::EndPopup();
    }

    if (show_grid)
    {
        float x = p.x;
        for (int n = 0; n <= cols; n++)
        {
            draw_list->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + size_v), ImColor(grid_line), 1.0f);
            x += spacing_h;
        }

        float y = p.y;  
        for (int n = 0; n <= rows; n++)
        {
            draw_list->AddLine(ImVec2(p.x, y), ImVec2(p.x + size_h, y), ImColor(grid_line), 1.0f);
            y += spacing_v;
        }
    }

    if (show_regions)
    {
        // The 3 Graphics II regions, each 8 tile rows (64px unscaled) tall
        // - every region can point at a different pattern/color table bank.
        // Ported from SC3K-System's "Show Screen Rect" (same 2 dividers at
        // 64*scale/128*scale - SC3K also drew one at y=0, but that just
        // overlaps the image's own top edge, so only the 2 real dividers
        // are needed to show 3 regions). See DOCS/ROM_INSPECTOR_PLAN.md
        // point 2.
        float region_height = 8.0f * spacing_v;
        float y = p.y + region_height;
        for (int n = 0; n < 2; n++)
        {
            draw_list->AddLine(ImVec2(p.x, y), ImVec2(p.x + size_h, y), ImColor(blue), 2.0f);
            y += region_height;
        }
    }

    int name_table_addr = regs[2] << 10;
    int color_table_addr = regs[3] << 6;
    if (mode == 2)
        color_table_addr &= 0x2000;

    ImGui::TextColored(cyan, " Name Table Addr:"); ImGui::SameLine();
    ImGui::Text("$%04X", name_table_addr);

    float mouse_x = io.MousePos.x - p.x;
    float mouse_y = io.MousePos.y - p.y;

    int tile_x = -1;
    int tile_y = -1;
    if (ImGui::IsWindowHovered() && (mouse_x >= 0.0f) && (mouse_x < size_h) && (mouse_y >= 0.0f) && (mouse_y < size_v))
    {
        tile_x = (int)(mouse_x / spacing_h);
        tile_y = (int)(mouse_y / spacing_v);

        draw_list->AddRect(ImVec2(p.x + (tile_x * spacing_h), p.y + (tile_y * spacing_v)), ImVec2(p.x + ((tile_x + 1) * spacing_h), p.y + ((tile_y + 1) * spacing_v)), ImColor(cyan), 2.0f, ImDrawFlags_RoundCornersAll, 2.0f);

        ImGui::NextColumn();

        renderer_begin_emulator_image();
        ImGui::Image((ImTextureID)(intptr_t)renderer_emu_debug_vram_background, ImVec2(tile_width * 16.0f, 128.0f), ImVec2(uv_h * tile_x, uv_v * tile_y), ImVec2(uv_h * (tile_x + 1), uv_v * (tile_y + 1)));
        renderer_end_emulator_image();

        ImGui::TextColored(yellow, "INFO:");

        ImGui::TextColored(cyan, " X:"); ImGui::SameLine();
        ImGui::Text("$%02X", tile_x);
        ImGui::TextColored(cyan, " Y:"); ImGui::SameLine();
        ImGui::Text("$%02X", tile_y);

        int pattern_table_addr = regs[4] << 11;
        int region = (tile_y & 0x18) << 5;

        int tile_number = (tile_y * cols) + tile_x;
        int name_tile_addr = (name_table_addr + tile_number) & 0x3FFF;
        int name_tile = vram[name_tile_addr];

        if (mode == 2)
        {
            pattern_table_addr &= 0x2000;
            name_tile += region;
        }
        else if(mode == 4)
        {
            pattern_table_addr &= 0x2000;
        }

        int tile_addr = (pattern_table_addr + (name_tile << 3)) & 0x3FFF;

        int color_mask = ((regs[3] & 0x7F) << 3) | 0x07;

        int color_tile_addr = 0;

        if (mode == 2)
            color_tile_addr = color_table_addr + ((name_tile & color_mask) << 3);
        else if (mode == 0)
            color_tile_addr = color_table_addr + (name_tile >> 3);

        ImGui::TextColored(cyan, " Name Addr:"); ImGui::SameLine();
        ImGui::Text(" $%04X", name_tile_addr);
        ImGui::TextColored(cyan, " Tile Number:"); ImGui::SameLine();
        ImGui::Text("$%03X", name_tile);
        ImGui::TextColored(cyan, " Tile Addr:"); ImGui::SameLine();
        ImGui::Text(" $%04X", tile_addr);
        ImGui::TextColored(cyan, " Color Addr:"); ImGui::SameLine();
        ImGui::Text("$%04X", color_tile_addr);

        if (ImGui::IsMouseClicked(0))
        {
            gui_debug_memory_goto(GuiDebugMemoryEditorSlotVram, name_tile_addr);
        }
    }

    ImGui::Columns(1);

    ImGui::PopFont();
}

static void debug_window_vram_tiles(void)
{
    Video* video = emu_get_core()->GetVideo();
    u8* regs = video->GetRegisters();
    int mode = video->GetMode();

    bool& show_grid = config_debug.show_grid_tiles;
    int lines = 32;
    float& scale = config_debug.vram_tiles_area_scale;
    float width = 8.0f * 32.0f * scale;
    float height = 8.0f * lines * scale;
    float spacing = 8.0f * scale;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 p;

    ImGui::Checkbox("Show Grid##grid_tiles", &show_grid);
    ImGui::SameLine();
    area_scale_control("Scale##scale_tiles", &scale);

    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    ImGui::Columns(2, "tiles", false);
    ImGui::SetColumnOffset(1, width + 10.0f);

    p = ImGui::GetCursorScreenPos();

    renderer_begin_emulator_image();
    ImGui::Image((ImTextureID)(intptr_t)renderer_emu_debug_vram_tiles, ImVec2(width, height), ImVec2(0.0f, 0.0f), ImVec2(1.0f, (1.0f / 32.0f) * lines));
    renderer_end_emulator_image();

    if (ImGui::BeginPopupContextItem("##tiles_context"))
    {
        if (ImGui::Selectable("Save Pattern Table As PNG..."))
        {
            // lines is always 32, matching the buffer's full 256x256
            // allocation exactly - no crop needed here.
            save_debug_png_with_dialog(emu_debug_tile_buffer, 256, 256, 256 * 3, "pattern_table.png");
        }
        ImGui::EndPopup();
    }

    if (show_grid)
    {
        float x = p.x;
        for (int n = 0; n <= 32; n++)
        {
            draw_list->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + height), ImColor(grid_line), 1.0f);
            x += spacing;
        }

        float y = p.y;  
        for (int n = 0; n <= lines; n++)
        {
            draw_list->AddLine(ImVec2(p.x, y), ImVec2(p.x + width, y), ImColor(grid_line), 1.0f);
            y += spacing;
        }
    }

    int pattern_table_addr = (regs[4] & (mode == 2 ? 0x04 : 0x07)) << 11;

    ImGui::TextColored(cyan, " Pattern Table Addr:"); ImGui::SameLine();
    ImGui::Text("$%04X", pattern_table_addr);

    float mouse_x = io.MousePos.x - p.x;
    float mouse_y = io.MousePos.y - p.y;

    int tile_x = -1;
    int tile_y = -1;

    if (ImGui::IsWindowHovered() && (mouse_x >= 0.0f) && (mouse_x < width) && (mouse_y >= 0.0f) && (mouse_y < height))
    {
        tile_x = (int)(mouse_x / spacing);
        tile_y = (int)(mouse_y / spacing);

        draw_list->AddRect(ImVec2(p.x + (tile_x * spacing), p.y + (tile_y * spacing)), ImVec2(p.x + ((tile_x + 1) * spacing), p.y + ((tile_y + 1) * spacing)), ImColor(cyan), 2.0f, ImDrawFlags_RoundCornersAll, 2.0f);

        ImGui::NextColumn();

        renderer_begin_emulator_image();
        ImGui::Image((ImTextureID)(intptr_t)renderer_emu_debug_vram_tiles, ImVec2(128.0f, 128.0f), ImVec2((1.0f / 32.0f) * tile_x, (1.0f / 32.0f) * tile_y), ImVec2((1.0f / 32.0f) * (tile_x + 1), (1.0f / 32.0f) * (tile_y + 1)));
        renderer_end_emulator_image();

        ImGui::TextColored(yellow, "DETAILS:");

        int tile = (tile_y << 5) + tile_x;

        int tile_addr = (pattern_table_addr + (tile << 3)) & 0x3FFF;

        ImGui::TextColored(cyan, " Tile Number:"); ImGui::SameLine();
        ImGui::Text("$%03X", tile); 
        ImGui::TextColored(cyan, " Tile Addr:"); ImGui::SameLine();
        ImGui::Text("$%04X", tile_addr); 

        if (ImGui::IsMouseClicked(0))
        {
            gui_debug_memory_goto(GuiDebugMemoryEditorSlotVram, tile_addr);
        }
    }

    ImGui::Columns(1);

    ImGui::PopFont();
}

static void debug_window_vram_sprites(void)
{
    float& scale = config_debug.vram_sprites_area_scale;
    float size_8 = 8.0f * scale;
    float size_16 = 16.0f * scale;

    GearSF7000Core* core = emu_get_core();
    Video* video = core->GetVideo();
    u8* regs = video->GetRegisters();
    u8* vram = video->GetVRAM();
    GC_RuntimeInfo runtime;
    emu_get_runtime(runtime);
    const Video::RenderedSpriteDebugFrame& rendered =
        video->GetLastRenderedSpriteDebugFrame();
    const bool rendered_source = config_debug.vram_sprites_source == 1;
    const bool pipeline_source = config_debug.vram_sprites_source == 2;
    const bool scanline_source = config_debug.vram_sprites_source == 3;
    const Video::SpritePipelineDebugSnapshot pipeline =
        video->GetSpritePipelineDebugSnapshot();
    const Video::SpriteSelectionFrameHistory& history =
        video->GetSpriteSelectionFrameHistory();
    if (history.rasterLines > 0)
        config_debug.vram_sprites_scanline = std::clamp(
            config_debug.vram_sprites_scanline, 0,
            history.rasterLines - 1);
    const Video::SpriteSelectionHistoryRow* history_row =
        scanline_source && history.valid && history.rasterLines > 0
        ? &history.rows[config_debug.vram_sprites_scanline] : nullptr;
    bool sprites_16 = rendered_source && rendered.valid
        ? rendered.large : IsSetBit(regs[1], 1);
    if (history_row != nullptr)
        sprites_16 = (history_row->flags & 0x04) != 0;
    bool sprites_zoomed = rendered_source && rendered.valid
        ? rendered.magnified : IsSetBit(regs[1], 0);
    if (history_row != nullptr)
        sprites_zoomed = (history_row->flags & 0x08) != 0;

    float width = 0.0f;
    float height = 0.0f;

    width = sprites_16 ? size_16 : size_8;
    height = sprites_16 ? size_16 : size_8;

    ImVec2 p[64];

    ImGuiIO& io = ImGui::GetIO();

    const char* sprite_sources[] = {
        "Live SAT", "Last Rendered Frame", "Current Scanline Fetch",
        "Last Frame Scanline"
    };
    ImGui::SetNextItemWidth(gui_combo_width_for_array(sprite_sources, IM_ARRAYSIZE(sprite_sources)));
    ImGui::Combo("Source##sprite_source", &config_debug.vram_sprites_source,
        sprite_sources, IM_ARRAYSIZE(sprite_sources));
    ImGui::SameLine();
    area_scale_control("Grid Scale##scale_sprites", &scale);
    ImGui::SameLine();
    area_scale_control("Preview Scale##scale_sprites_preview", &config_debug.vram_sprites_preview_area_scale);
    if (rendered_source)
    {
        ImGui::SameLine();
        if (rendered.valid)
            ImGui::TextDisabled("Frame %llu", rendered.frameSerial);
        else
            ImGui::TextDisabled("No complete frame captured");
    }
    else if (pipeline_source)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("Raster %d dot %u slot %u%s%d",
            pipeline.rasterLine, pipeline.dot, pipeline.slot,
            pipeline.activeTargetValid ? " -> target " : "",
            pipeline.activeTargetValid ? pipeline.activeTargetLine : 0);
    }
    else if (scanline_source)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        const int maximum = history.rasterLines > 0
            ? history.rasterLines - 1 : 312;
        ImGui::SliderInt("Raster Y##sprite_history_line",
            &config_debug.vram_sprites_scanline, 0, maximum);
        ImGui::SameLine();
        if (history.valid)
            ImGui::TextDisabled("Frame %llu", history.frameSerial);
        else
            ImGui::TextDisabled("No recorded frame history");
    }

    if (rendered_source)
        ImGui::TextDisabled("Green: complete  Orange: partial  Gray: absent  Cyan: selected");
    else if (pipeline_source)
        ImGui::TextDisabled("Green: fully latched  Orange: fetch in progress  Red: fifth/overflow  Cyan: selected");
    else if (scanline_source)
    {
        ImGui::TextDisabled("Green: accepted by VDP  Red: fifth/overflow  Cyan: selected");
        if (history_row != nullptr && (history_row->flags & 0x20) != 0)
        {
            ImGui::SameLine();
            ImGui::TextColored(red, "Collision at X $%02X", history_row->collisionX);
        }
    }

    const Video::SpritePipelineLineDebug* active_pipeline_line = nullptr;
    for (const Video::SpritePipelineLineDebug& line : pipeline.lines)
    {
        if (!line.valid)
            continue;
        if (pipeline.activeTargetValid &&
            line.absoluteTargetLine == pipeline.activeAbsoluteTargetLine)
        {
            active_pipeline_line = &line;
            break;
        }
        if (active_pipeline_line == nullptr)
            active_pipeline_line = &line;
    }

    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    ImGui::Columns(2, "spr", false);
    // 4 thumbnails per row (see the `s % 4 < 3` SameLine below) plus the
    // child window's own item spacing/scrollbar/padding - proportional to
    // scale instead of the two magic constants this replaced, which only
    // matched the previous fixed scale (4.0x) by coincidence.
    float grid_column_width = (4.0f * width) + (3.0f * ImGui::GetStyle().ItemSpacing.x) + 40.0f;
    ImGui::SetColumnOffset(1, grid_column_width);

    ImGui::BeginChild("sprites", ImVec2(0, 0.0f), true);
    bool window_hovered = ImGui::IsWindowHovered();

    for (int s = 0; s < 32; s++)
    {
        p[s] = ImGui::GetCursorScreenPos();

        renderer_begin_emulator_image();
        ImGui::Image((ImTextureID)(intptr_t)renderer_emu_debug_vram_sprites[s], ImVec2(width, height), ImVec2(0.0f, 0.0f), ImVec2((1.0f / 16.0f) * (width / scale), (1.0f / 16.0f) * (height / scale)));
        renderer_end_emulator_image();

        if (rendered_source)
        {
            const Video::RenderedSpriteDebugEntry& entry =
                rendered.entries[s];
            const u16 complete_mask = sprites_16 ? 0xFFFFu : 0x00FFu;
            const ImU32 state_colour = !entry.seen
                ? ImColor(90, 90, 90)
                : ((entry.fetchedRows & complete_mask) == complete_mask
                    ? ImColor(green) : ImColor(orange));
            ImGui::GetWindowDrawList()->AddRect(
                p[s], ImVec2(p[s].x + width, p[s].y + height),
                state_colour, 0.0f, 0, 1.0f);
        }
        else if (pipeline_source && active_pipeline_line != nullptr)
        {
            ImU32 state_colour = ImColor(90, 90, 90);
            bool marked = false;
            for (int lane = 0; lane < active_pipeline_line->selectedCount &&
                 lane < 4; ++lane)
            {
                const Video::SpritePipelineLatchDebug& entry =
                    active_pipeline_line->selected[lane];
                if (entry.satIndex != s)
                    continue;
                const bool complete = entry.yValid && entry.xValid &&
                    entry.nameValid && entry.colourValid &&
                    entry.patternValid[0] &&
                    (!sprites_16 || entry.patternValid[1]);
                state_colour = complete ? ImColor(green) : ImColor(orange);
                marked = true;
                break;
            }
            if (active_pipeline_line->overflowRecorded &&
                active_pipeline_line->overflowSprite == s)
            {
                state_colour = ImColor(red);
                marked = true;
            }
            if (marked)
                ImGui::GetWindowDrawList()->AddRect(
                    p[s], ImVec2(p[s].x + width, p[s].y + height),
                    state_colour, 0.0f, 0, 2.0f);
        }
        else if (scanline_source && history_row != nullptr)
        {
            ImU32 state_colour = ImColor(90, 90, 90);
            bool marked = false;
            for (int lane = 0; lane < 5; ++lane)
            {
                if (history_row->rasterX[lane] == 0xFF)
                    continue;
                const auto scan = TMS9918VramSlotSchedule::GetSlot(
                    TMS9918VramSlotSchedule::Schedule::Graphics,
                    history_row->rasterX[lane]);
                if (scan.activity ==
                        TMS9918VramSlotSchedule::Activity::SpriteScanY &&
                    scan.index == s)
                {
                    state_colour = lane == 4 ? ImColor(red) : ImColor(green);
                    marked = true;
                    break;
                }
            }
            if (marked)
                ImGui::GetWindowDrawList()->AddRect(
                    p[s], ImVec2(p[s].x + width, p[s].y + height),
                    state_colour, 0.0f, 0, 2.0f);
        }

        ImGui::PushID(s);
        if (ImGui::BeginPopupContextItem("##spr_context"))
        {
            char label[32];
            snprintf(label, sizeof(label), "Save Sprite %d As PNG...", s);
            if (ImGui::Selectable(label))
            {
                // The 16x16 buffer only has real content in its top-left
                // sprite_size x sprite_size corner for 8x8 sprites - crop
                // to that, keep the buffer's own 16-wide row stride.
                int sprite_size = sprites_16 ? 16 : 8;
                char default_name[32];
                snprintf(default_name, sizeof(default_name), "sprite_%02d.png", s);
                save_debug_png_with_dialog(emu_debug_sprite_buffers[s], sprite_size, sprite_size, 16 * 3, default_name);
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();

        float mouse_x = io.MousePos.x - p[s].x;
        float mouse_y = io.MousePos.y - p[s].y;

        if (window_hovered && (mouse_x >= 0.0f) && (mouse_x < width) && (mouse_y >= 0.0f) && (mouse_y < height))
        {
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            draw_list->AddRect(ImVec2(p[s].x, p[s].y), ImVec2(p[s].x + width, p[s].y + height), ImColor(cyan), 2.0f, ImDrawFlags_RoundCornersAll, 3.0f);
        }

        if (s % 4 < 3)
            ImGui::SameLine();
    }

    ImGui::EndChild();

    ImGui::NextColumn();

    ImVec2 p_screen = ImGui::GetCursorScreenPos();

    float& screen_scale = config_debug.vram_sprites_preview_area_scale;
    renderer_begin_emulator_image();
    ImGui::Image((ImTextureID)(intptr_t)renderer_emu_texture, ImVec2(runtime.screen_width * screen_scale, runtime.screen_height * screen_scale));
    renderer_end_emulator_image();

    // SAT coordinates are relative to the active 256x192 display, but
    // runtime.screen_width/height (and this texture) are the packed frame,
    // which is wider/taller than that whenever overscan is on - GC_RESOLUTION_
    // WIDTH/HEIGHT sit inset inside it at content_area.x/y. Forgetting that
    // offset draws this rectangle at the sprite's position inside the active
    // area, which is short of its real position on screen by exactly the
    // border's thickness - up and to the left of the sprite it is meant to
    // mark, worse the wider the current overscan setting is.
    const GC_VideoFrameDescriptor frame_descriptor = core->GetVideoFrameDescriptor();

    for (int s = 0; s < 32; s++)
    {
        if ((p[s].x == 0) && (p[s].y == 0))
            continue;

        float mouse_x = io.MousePos.x - p[s].x;
        float mouse_y = io.MousePos.y - p[s].y;

        if (window_hovered && (mouse_x >= 0.0f) && (mouse_x < width) && (mouse_y >= 0.0f) && (mouse_y < height))
        {
            int x = 0;
            int y = 0;
            int tile = 0;
            int sprite_tile_addr = 0;
            int sprite_shift = 0;
            int sprite_color = 0;
            u16 fetched_rows = 0;
            bool fetched = true;
            int pipeline_lane = -1;
            int history_lane = -1;
            bool overflow_candidate = false;
            float real_x = 0.0f;
            float real_y = 0.0f;

            if (pipeline_source && active_pipeline_line != nullptr)
            {
                for (int lane = 0;
                     lane < active_pipeline_line->selectedCount && lane < 4;
                     ++lane)
                    if (active_pipeline_line->selected[lane].satIndex == s)
                        pipeline_lane = lane;
                overflow_candidate =
                    active_pipeline_line->overflowRecorded &&
                    active_pipeline_line->overflowSprite == s;
            }
            if (scanline_source && history_row != nullptr)
            {
                for (int lane = 0; lane < 5; ++lane)
                {
                    if (history_row->rasterX[lane] == 0xFF)
                        continue;
                    const auto scan = TMS9918VramSlotSchedule::GetSlot(
                        TMS9918VramSlotSchedule::Schedule::Graphics,
                        history_row->rasterX[lane]);
                    if (scan.activity ==
                            TMS9918VramSlotSchedule::Activity::SpriteScanY &&
                        scan.index == s)
                    {
                        history_lane = lane;
                        overflow_candidate = lane == 4;
                        break;
                    }
                }
            }

            u16 sprite_attribute_addr = (regs[5] & 0x7F) << 7;
            u16 sprite_pattern_addr = (regs[6] & 0x07) << 11;
            int sprite_attribute_offset = sprite_attribute_addr + (s << 2);
            if (rendered_source)
            {
                const Video::RenderedSpriteDebugEntry& entry =
                    rendered.entries[s];
                fetched = rendered.valid && entry.seen;
                x = entry.x;
                y = entry.rawY;
                tile = entry.name;
                sprite_shift = (entry.colour & 0x80) ? 32 : 0;
                sprite_color = entry.colour & 0x0F;
                fetched_rows = entry.fetchedRows;
                // The bytes are already in the VDP's fetch latches. Showing a
                // live VRAM address here would falsely claim that later writes
                // are the bytes that made this frame.
                sprite_tile_addr = -1;
            }
            else if (scanline_source && history_lane >= 0)
            {
                const u8* sat = history_row->sat[history_lane];
                x = sat[1];
                y = sat[0];
                tile = sat[2];
                sprite_shift = (sat[3] & 0x80) ? 32 : 0;
                sprite_color = sat[3] & 0x0F;
                fetched = history_lane < 4;
                sprite_tile_addr = -1;
            }
            else if (pipeline_source && pipeline_lane >= 0)
            {
                const Video::SpritePipelineLatchDebug& entry =
                    active_pipeline_line->selected[pipeline_lane];
                x = entry.x;
                y = entry.y;
                tile = entry.name;
                sprite_shift = (entry.colour & 0x80) ? 32 : 0;
                sprite_color = entry.colour & 0x0F;
                fetched = entry.yValid && entry.xValid && entry.nameValid &&
                    entry.colourValid && entry.patternValid[0] &&
                    (!sprites_16 || entry.patternValid[1]);
                sprite_tile_addr = -1;
            }
            else if (pipeline_source && overflow_candidate &&
                     active_pipeline_line->overflowSatValid)
            {
                const u8* sat = active_pipeline_line->overflowSat;
                x = sat[1];
                y = sat[0];
                tile = sat[2];
                sprite_shift = (sat[3] & 0x80) ? 32 : 0;
                sprite_color = sat[3] & 0x0F;
                fetched = false;
                sprite_tile_addr = -1;
            }
            else
            {
                tile = vram[sprite_attribute_offset + 2];
                sprite_tile_addr = sprite_pattern_addr + (tile << 3);
                sprite_shift = (vram[sprite_attribute_offset + 3] & 0x80) ? 32 : 0;
                sprite_color = vram[sprite_attribute_offset + 3] & 0x0F;
                x = vram[sprite_attribute_offset + 1];
                y = vram[sprite_attribute_offset];
            }

            int final_y = (y + 1) & 0xFF;

            if (final_y >= 0xE0)
                final_y = -(0x100 - final_y);

            real_x = (float)(x - sprite_shift) + (float)frame_descriptor.content_area.x;
            real_y = (float)final_y + (float)frame_descriptor.content_area.y;

            float max_width = 8.0f;
            float max_height = sprites_16 ? 16.0f : 8.0f;

            if (sprites_16)
                max_width = 16.0f;

            if (sprites_zoomed)
            {
                max_width *= 2.0f;
                max_height *= 2.0f;
            }

            float rectx_min = p_screen.x + (real_x * screen_scale);
            float rectx_max = p_screen.x + ((real_x + max_width) * screen_scale);
            float recty_min = p_screen.y + (real_y * screen_scale);
            float recty_max = p_screen.y + ((real_y + max_height) * screen_scale);

            rectx_min = fminf(fmaxf(rectx_min, p_screen.x), p_screen.x + (runtime.screen_width * screen_scale));
            rectx_max = fminf(fmaxf(rectx_max, p_screen.x), p_screen.x + (runtime.screen_width * screen_scale));
            recty_min = fminf(fmaxf(recty_min, p_screen.y), p_screen.y + (runtime.screen_height * screen_scale));
            recty_max = fminf(fmaxf(recty_max, p_screen.y), p_screen.y + (runtime.screen_height * screen_scale));
            
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            if (fetched)
                draw_list->AddRect(ImVec2(rectx_min, recty_min), ImVec2(rectx_max, recty_max), ImColor(cyan), 2.0f, ImDrawFlags_RoundCornersAll, 2.0f);

            ImGui::TextColored(yellow, "DETAILS:");
            ImGui::TextColored(cyan, " SAT Index:"); ImGui::SameLine();
            ImGui::Text("%d", s);
            if (!rendered_source && !scanline_source &&
                !(pipeline_source && (pipeline_lane >= 0 || overflow_candidate)))
            {
                ImGui::TextColored(cyan, " Attribute Addr:"); ImGui::SameLine();
                ImGui::Text("$%04X", sprite_attribute_offset);
            }

            ImGui::TextColored(cyan, " X:"); ImGui::SameLine();
            ImGui::Text("$%02X", x);
            ImGui::TextColored(cyan, " Y:"); ImGui::SameLine();
            ImGui::Text("$%02X", y);

            ImGui::TextColored(cyan, " Tile:"); ImGui::SameLine();
            ImGui::Text("$%02X", tile);

            if (sprite_tile_addr >= 0)
            {
                ImGui::TextColored(cyan, " Tile Addr:"); ImGui::SameLine();
                ImGui::Text("$%04X", sprite_tile_addr);
            }

            ImGui::TextColored(cyan, " Color:"); ImGui::SameLine();
            ImGui::Text("$%02X", sprite_color);

            ImGui::TextColored(cyan, " Early Clock:"); ImGui::SameLine();
            sprite_shift > 0 ? ImGui::TextColored(green, "ON ") : ImGui::TextColored(gray, "OFF");

            if (rendered_source)
            {
                const u16 complete_mask = sprites_16 ? 0xFFFFu : 0x00FFu;
                ImGui::TextColored(cyan, " Fetched Rows:"); ImGui::SameLine();
                ImGui::Text("$%04X", fetched_rows);
                ImGui::TextColored(cyan, " Frame Result:"); ImGui::SameLine();
                if (!fetched)
                    ImGui::TextColored(gray, "NOT FETCHED");
                else if ((fetched_rows & complete_mask) != complete_mask)
                    ImGui::TextColored(orange, "PARTIAL / OVERFLOW OR CLIPPED");
                else
                    ImGui::TextColored(green, "COMPLETE");
            }

            if (pipeline_source && (pipeline_lane >= 0 || overflow_candidate))
            {
                if (overflow_candidate)
                {
                    ImGui::TextColored(cyan, " Pipeline Result:"); ImGui::SameLine();
                    ImGui::TextColored(red, "FIFTH / REJECTED");
                    ImGui::TextDisabled("Only Y was fetched; remaining SAT fields are a diagnostic snapshot.");
                    ImGui::TextColored(cyan, " Raster X:"); ImGui::SameLine();
                    ImGui::Text("%u", active_pipeline_line->overflowRasterX);
                }
                else
                {
                    const Video::SpritePipelineLatchDebug& entry =
                        active_pipeline_line->selected[pipeline_lane];
                    ImGui::TextColored(cyan, " Lane / Raster X:"); ImGui::SameLine();
                    ImGui::Text("%d / %u", pipeline_lane, entry.rasterX);
                    ImGui::TextColored(cyan, " Latches Y X N C P0 P1:"); ImGui::SameLine();
                    ImGui::Text("%d %d %d %d %d %d", entry.yValid,
                        entry.xValid, entry.nameValid, entry.colourValid,
                        entry.patternValid[0], entry.patternValid[1]);
                    ImGui::TextColored(cyan, " Pattern Bytes:"); ImGui::SameLine();
                    ImGui::Text("$%02X $%02X", entry.pattern[0], entry.pattern[1]);
                }
                if (active_pipeline_line->collisionX >= 0)
                {
                    ImGui::TextColored(cyan, " Collision X:"); ImGui::SameLine();
                    ImGui::TextColored(red, "$%02X", active_pipeline_line->collisionX);
                }
            }

            if (scanline_source && history_lane >= 0)
            {
                ImGui::TextColored(cyan, " Lane / Raster X:"); ImGui::SameLine();
                ImGui::Text("%d / %u", history_lane,
                    history_row->rasterX[history_lane]);
                ImGui::TextColored(cyan, " Scanline Result:"); ImGui::SameLine();
                if (overflow_candidate)
                    ImGui::TextColored(red, "FIFTH / REJECTED");
                else
                {
                    ImGui::TextColored(green, "ACCEPTED");
                    ImGui::TextColored(cyan, " Pattern Bytes:"); ImGui::SameLine();
                    ImGui::Text("$%02X $%02X",
                        history_row->pattern[history_lane][0],
                        history_row->pattern[history_lane][1]);
                }
                if (overflow_candidate)
                    ImGui::TextDisabled("Only Y was fetched; remaining SAT fields are a diagnostic snapshot.");
            }

            if (sprite_tile_addr >= 0 && ImGui::IsMouseClicked(0))
            {
                gui_debug_memory_goto(GuiDebugMemoryEditorSlotVram, sprite_tile_addr);
            }
        }
    }

    ImGui::Columns(1);

    ImGui::PopFont();
}

static void debug_window_vram_regs(void)
{
    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    Video* video = emu_get_core()->GetVideo();
    u8* regs = video->GetRegisters();

    ImGui::TextColored(yellow, "VDP STATE:");

    ImGui::TextColored(cyan, " PAL (50Hz)       "); ImGui::SameLine();
    video->IsPAL() ? ImGui::TextColored(green, "YES ") : ImGui::TextColored(gray, "NO  ");
    ImGui::TextColored(cyan, " LATCH FIRST BYTE "); ImGui::SameLine();
    video->GetLatch() ? ImGui::TextColored(green, "YES ") : ImGui::TextColored(gray, "NO  ");
    ImGui::TextColored(cyan, " INTERNAL BUFFER  "); ImGui::SameLine();
    ImGui::Text("$%02X (" BYTE_TO_BINARY_PATTERN_SPACED ")", video->GetBufferReg(), BYTE_TO_BINARY(video->GetBufferReg()));
    ImGui::TextColored(cyan, " INTERNAL STATUS  "); ImGui::SameLine();
    ImGui::Text("$%02X (" BYTE_TO_BINARY_PATTERN_SPACED ")", video->GetStatusReg(), BYTE_TO_BINARY(video->GetStatusReg()));
    ImGui::TextColored(cyan, " INTERNAL ADDRESS "); ImGui::SameLine();
    ImGui::TextColored(yellow, "$%04X", video->GetAddressReg());
    ImGui::TextColored(cyan, " RENDER LINE      "); ImGui::SameLine();
    ImGui::Text("%d", video->GetRenderLine());
    ImGui::TextColored(cyan, " CYCLE COUNTER    "); ImGui::SameLine();
    ImGui::Text("%d", video->GetCycleCounter());

    ImGui::TextColored(yellow, " ");
    ImGui::TextColored(yellow, "VDP REGISTERS:");

    int mode = ((regs[0] & 0x06) << 8) | (regs[1] & 0x18);
    //u16 vramPtr = video->GetVRAMPtr();

    int name_table_addr;
    int pattern_table_addr;
    int color_table_addr;
    int sprite_attribute_addr = (regs[5] & 0x7F) << 7;
    int sprite_pattern_addr = (regs[6] & 0x07) << 11;

    switch (mode)
    {
    case 0x210: // Mode 1+2
    case 0x200: // Mode 2
        name_table_addr = (regs[2] & 0x0F) << 10;
        pattern_table_addr = (regs[4] & 0x04) << 11;
        color_table_addr = (regs[3] & 0x80) << 6;
        break;

    case 0x010: // Text Mode (0)
        name_table_addr = (regs[2] & 0x7F) << 10;
        pattern_table_addr = (regs[4] & 0x3F) << 11;
        color_table_addr = 0x3FFF;
        break;

    default:    // Mode 1
        name_table_addr = (regs[2] & 0x0F) << 10;
        pattern_table_addr = (regs[4] & 0x07) << 11;
        color_table_addr = regs[3] << 6;
        break;
    }

    const char* reg_desc[] = {"CONTROL 0   ", "CONTROL 1   ", "PATTERN NAME", "COLOR TABLE ", "PATTERN GEN ", "SPRITE ATTR ", "SPRITE GEN  ", "COLORS      "};

    //for (int i = 0; i < 8; i++)
    //{
    //    ImGui::TextColored(cyan, " $%01X ", i); ImGui::SameLine();
    //    ImGui::TextColored(magenta, "%s ", reg_desc[i]); ImGui::SameLine();
    //    ImGui::Text("$%02X (" BYTE_TO_BINARY_PATTERN_SPACED ")", regs[i], BYTE_TO_BINARY(regs[i]));
    //}


    for (int i = 0; i < 8; i++)
    {
        ImGui::TextColored(cyan, " REG $%01X ", i); ImGui::SameLine();
        ImGui::TextColored(magenta, "%s ", reg_desc[i]); ImGui::SameLine();



        switch (i)
        {
        case 1:
            {
                ImGui::Text(" $%02X  (" BYTE_TO_BINARY_PATTERN_SPACED ")", regs[i], BYTE_TO_BINARY(regs[i])); ImGui::SameLine();
                if (regs[1] & 0x80) ImGui::Text(" VRAM 16K"); else ImGui::Text(" VRAM 4K"); ImGui::SameLine();
                if (regs[1] & 0x40) ImGui::Text(" DISP ON"); else ImGui::Text(" DISP OFF"); ImGui::SameLine();
                if (regs[1] & 0x20) ImGui::Text(" INT. ON"); else ImGui::Text(" INT. OFF"); ImGui::SameLine();
                if (regs[1] & 0x02) ImGui::Text(" SPR. 16"); else ImGui::Text(" SPR. 8"); ImGui::SameLine();
                if (regs[1] & 0x01) ImGui::Text(" SPRZOOM"); else ImGui::Text("");
            }
            break;
        case 2:
            ImGui::Text(" $%02X  (" BYTE_TO_BINARY_PATTERN_SPACED "), Addr: $%04X", regs[i], BYTE_TO_BINARY(regs[i]), name_table_addr);
            break;
        case 3:
            ImGui::Text(" $%02X  (" BYTE_TO_BINARY_PATTERN_SPACED "), Addr: $%04X", regs[i], BYTE_TO_BINARY(regs[i]), color_table_addr);
            break;
        case 4:
            ImGui::Text(" $%02X  (" BYTE_TO_BINARY_PATTERN_SPACED "), Addr: $%04X", regs[i], BYTE_TO_BINARY(regs[i]), pattern_table_addr);
            break;
        case 5:
            ImGui::Text(" $%02X  (" BYTE_TO_BINARY_PATTERN_SPACED "), Addr: $%04X", regs[i], BYTE_TO_BINARY(regs[i]), sprite_attribute_addr);
            break;
        case 6:
            ImGui::Text(" $%02X  (" BYTE_TO_BINARY_PATTERN_SPACED "), Addr: $%04X", regs[i], BYTE_TO_BINARY(regs[i]), sprite_pattern_addr);
            break;
        default:
            ImGui::Text(" $%02X  (" BYTE_TO_BINARY_PATTERN_SPACED ")", regs[i], BYTE_TO_BINARY(regs[i]));
            break;
        }
    }

    //ImGui::TextColored(cyan, "VRAM PTR"); ImGui::SameLine(); ImGui::TextColored(yellow, "$%04X", vramPtr);


    ImGui::PopFont();
}




#if GEARSF7000_ENABLE_SR1000
static void debug_window_cassette_controls(void)
{
    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    SR1000* recorder = emu_get_core()->GetCassette();
    SR1000Speaker* speaker = emu_get_core()->GetCassetteSpeaker();
    
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
    ImGui::PushItemWidth(130.0f);
    if (ImGui::SliderFloat("##tape_speed", &tapeSpeed, -20.0f, 20.0f, "%+.0f%%"))
        recorder->SetTapeSpeedPercent(tapeSpeed);
    ImGui::PopItemWidth();
    ImGui::TextColored(gray, " Target %+.1f%%  effective %+.1f%%",
        recorder->GetTapeSpeedPercent(), recorder->GetEffectiveTapeSpeedPercent());

    //std::string info = recorder->GetInfo();
    std::string info = speaker->GetInfo();
    ImGui::TextColored(cyan, info.c_str());

    ImGui::Text("\n");

    if (ImGui::Button("Play", ImVec2(60, 0)))
    {
        try
        {
            emu_cassette_play();
        }
        catch (const std::invalid_argument&)
        {
        }
    }

    ImGui::SameLine();

    if (ImGui::Button("Stop", ImVec2(60, 0)))
    {
        try
        {
            emu_cassette_stop();
        }
        catch (const std::invalid_argument&)
        {
        }
    }

    ImGui::SameLine();

    if (ImGui::Button("Rewind", ImVec2(60, 0)))
    {
        try
        {
            emu_cassette_rewind();
        }
        catch (const std::invalid_argument&)
        {
        }
    }

    //u8* regs = video->GetRegisters();


    //ImGui::TextColored(cyan, " LATCH FIRST BYTE "); ImGui::SameLine();
    //video->GetLatch() ? ImGui::TextColored(green, "YES ") : ImGui::TextColored(gray, "NO  ");
    //ImGui::TextColored(cyan, " INTERNAL BUFFER  "); ImGui::SameLine();
    //ImGui::Text("$%02X (" BYTE_TO_BINARY_PATTERN_SPACED ")", video->GetBufferReg(), BYTE_TO_BINARY(video->GetBufferReg()));
    //ImGui::TextColored(cyan, " INTERNAL STATUS  "); ImGui::SameLine();
    //ImGui::Text("$%02X (" BYTE_TO_BINARY_PATTERN_SPACED ")", video->GetStatusReg(), BYTE_TO_BINARY(video->GetStatusReg()));
    //ImGui::TextColored(cyan, " INTERNAL ADDRESS "); ImGui::SameLine();
    //ImGui::Text("$%04X", video->GetAddressReg());
    //ImGui::TextColored(cyan, " RENDER LINE      "); ImGui::SameLine();
    //ImGui::Text("%d", video->GetRenderLine());
    //ImGui::TextColored(cyan, " CYCLE COUNTER    "); ImGui::SameLine();
    //ImGui::Text("%d", video->GetCycleCounter());

    //ImGui::TextColored(yellow, "VDP REGISTERS:");

    //const char* reg_desc[] = { "CONTROL 0   ", "CONTROL 1   ", "PATTERN NAME", "COLOR TABLE ", "PATTERN GEN ", "SPRITE ATTR ", "SPRITE GEN  ", "COLORS      " };

    //for (int i = 0; i < 8; i++)
    //{
    //    ImGui::TextColored(cyan, " $%01X ", i); ImGui::SameLine();
    //    ImGui::TextColored(magenta, "%s ", reg_desc[i]); ImGui::SameLine();
    //    ImGui::Text("$%02X (" BYTE_TO_BINARY_PATTERN_SPACED ")", regs[i], BYTE_TO_BINARY(regs[i]));
    //}

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
    int target_bank = 0;

    try
    {
        if ((input_len == 7) && (brk_address_cpu[2] == ':'))
        {
            std::string str(brk_address_cpu);
            std::size_t separator = str.find(":");

            if (separator != std::string::npos)
            {
                target_address = (u16)std::stoul(str.substr(separator + 1 , std::string::npos), 0, 16);

                target_bank = std::stoul(str.substr(0, separator), 0 , 16);
                target_bank &= 0xFF;
            }
        } 
        else if (input_len == 4)
        {
            target_bank = 0; 
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




static void add_breakpoint_vram(void)
{
    int input_len = (int)strlen(brk_address_vram);
    u16 address1 = 0;
    u16 address2 = 0;
    bool range = false;

    try
    {
        if ((input_len == 9) && (brk_address_vram[4] == '-'))
        {
            std::string str(brk_address_vram);
            std::size_t separator = str.find("-");

            if (separator != std::string::npos)
            {
                address1 = (u16)std::stoul(str.substr(0, separator), 0, 16);
                address2 = (u16)std::stoul(str.substr(separator + 1, std::string::npos), 0, 16);

                if (address1 > 0x3FFF || address2 > 0x3FFF || address1 > address2)
                    return;
                range = true;
            }
        }
        else if (input_len == 4)
        {
            address1 = (u16)std::stoul(brk_address_vram, 0, 16);
            if (address1 > 0x3FFF)
                return;
        }
        else
        {
            return;
        }
    }
    catch (const std::exception&)
    {
        return;
    }

    bool found = false;
    std::vector<Memory::stMemoryBreakpoint>* breakpoints = emu_get_core()->GetMemory()->GetBreakpointsVRAM();

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
        new_breakpoint.read = brk_new_vram_read;
        new_breakpoint.write = brk_new_vram_write;

        breakpoints->push_back(new_breakpoint);
    }

    brk_address_vram[0] = 0;
}


static void request_goto_address(u16 address)
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
