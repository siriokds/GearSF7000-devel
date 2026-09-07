/* GearSF7000 MCP bridge: debugger APIs owned by the emulation thread. */
#include "mcp_config.h"
#if GEARSF7000_ENABLE_MCP
#ifndef MCP_DEBUG_ADAPTER_H
#define MCP_DEBUG_ADAPTER_H

#include <string>
#include <vector>
#include "json.hpp"
#include "../../../src/gearsf7000.h"

using json = nlohmann::json;

struct MemoryAreaInfo { int id; std::string name; u32 size; u32 display_base; u8* data; };
struct RegistersSnapshot { u16 AF, BC, DE, HL, AF2, BC2, DE2, HL2, IX, IY, SP, PC, WZ; u8 I, R; bool IFF1, IFF2, Halt; int InterruptMode; };
struct BreakpointInfo { bool enabled; int type; u16 address1, address2; bool read, write, execute, range; std::string type_name; };
struct DisasmLine { u32 address; u8 bank; std::string name, bytes, segment; int size; bool jump; u16 jump_address; u8 jump_bank; bool has_operand_address; u16 operand_address; bool subroutine; int irq; };

class DebugAdapter
{
public:
    explicit DebugAdapter(GearSF7000Core* core) : m_core(core) {}
    void Pause(); void Resume(); void StepInto(); void StepOver(); void StepOut(); void StepFrame(int frames = 1); json StepFrameSync(int frames); void Reset(bool paused = false);
    json GetDebugStatus(); json RunToAddress(u16 address);
    void SetBreakpoint(u16 address, int type, bool read, bool write, bool execute);
    void SetBreakpointRange(u16 start, u16 end, int type, bool read, bool write, bool execute);
    void ClearBreakpointByAddress(u16 address, int type, u16 end = 0); std::vector<BreakpointInfo> ListBreakpoints();
    RegistersSnapshot GetRegisters(); void SetRegister(const std::string& name, u32 value);
    std::vector<MemoryAreaInfo> ListMemoryAreas(); std::vector<u8> ReadMemoryArea(int area, u32 offset, size_t size); void WriteMemoryArea(int area, u32 offset, const std::vector<u8>& data);
    // MCP counterpart of the desktop "Mem Import" window (DOCS/ROM_INSPECTOR_PLAN.md
    // point 1) - same ReadMemoryArea/WriteMemoryArea areas as list_memory_areas,
    // plus the file on disk in one call instead of round-tripping bytes over JSON.
    json ExportMemoryToFile(int area, u32 offset, size_t length, const std::string& file_path);
    json ImportMemoryFromFile(int area, u32 offset, const std::string& file_path);
    // MCP counterpart of the Disassembler window's File > Save Disassembly
    // (DOCS/ROM_INSPECTOR_PLAN.md point 3). Shares gui_debug_save_disassembly()
    // with the GUI menu action instead of reimplementing the dump.
    json SaveDisassemblyFile(bool full, const std::string& file_path);
    // MCP counterpart of the VDP debug views' right-click "Save As PNG..."
    // (DOCS/ROM_INSPECTOR_PLAN.md point 6). view is "background"/"tiles"/
    // "sprite"; sprite_index only applies to "sprite" (0-31).
    json SaveDebugPng(const std::string& view, int sprite_index, const std::string& file_path);
    std::vector<DisasmLine> GetDisassembly(u16 start, u16 end, int bank = -1, bool resolve_symbols = false);
    json GetCodeCoverage(const json& query); json ClearCodeCoverage();
    json AddDebugRule(const std::string& category, u16 start, u16 end, bool read, bool write, bool execute, bool event,
                      bool pause, bool log, bool value_condition, u8 value_mask, u8 value_expected, u64 break_on_hit,
                      const std::string& condition_name, bool one_shot, u8 before_value_expected);
    json AddDeviceDebugRule(const std::string& device_key, const std::string& space_key,
                      u32 start, u32 end, bool read, bool write, bool execute, bool event,
                      bool pause, bool log, u64 value_mask, u64 value_expected,
                      u64 break_on_hit, const std::string& condition_name,
                      bool one_shot, u64 before_value_expected);
    json ListDebugRules(); json RemoveDebugRule(u32 id); json ClearDebugRules();
    json ListDebugDevices(); json GetDebugEvents(const json& query); json ClearDebugEvents();
    json GetZ80Status(); json GetZ80Clock(); json ResetZ80Clock(); json GetVDPRegisters(); json GetVDPStatus(); json GetAtomicSnapshot(const json& query); json SetRendererSource(const std::string& source); json SetRegion(const std::string& region); json GetSlotHistory(); json ReadFrameBuffer(const std::string& source, int y, int height); json GetPSGStatus(); json GetAY8910Status(); json GetSF7000Status(); json GetScreenshot(); json SetFullRasterDebugEnabled(bool enabled); json GetFullRasterDebugScreenshot(); json DumpCrtSignal(const std::string& file_path, int lines); json ListSprites(int line = -1, const std::string& source = "live"); json GetSpriteImage(int sprite_index, const std::string& source = "live"); json GetSpritePipeline(); json GetSpriteScanlineHistory(int startLine, int endLine, bool includeEmpty);
    json GetMediaInfo(); json ListRecentRoms(); json ListRecentTapes(); json ListRecentDisks(); json LoadRom(const std::string& file_path); json EjectRom(); json SetStartPaused(bool paused); json LoadTape(const std::string& file_path); json TapePlay(); json TapeStop(); json TapeRewind(); json SetTapeSpeed(float percent); json MountDisk(const std::string& file_path, bool write_protected); json StartSF7000();
    json ListSaveStateSlots(); json SelectSaveStateSlot(int slot); json SaveState(); json LoadState(); json SaveStateFile(const std::string& file_path); json LoadStateFile(const std::string& file_path); json SetFastForwardSpeed(int speed); json ToggleFastForward(bool enabled); json GetRewindStatus(); json ConfigureRewind(bool enabled, int seconds, int framesPerSnapshot, int memoryLimitMb); json RewindTransport(const std::string& action, int frames); json GetRewindPosition(); json GetSyncSettings(); json SetSyncSettings(const json& arguments); json RewindSeek(int snapshot); json AnalyzeRewindRange(const json& query);
    json TriggerNMI(); json KeyboardText(const std::string& text); json CancelKeyboardText();
    // The BASIC Typer panel: load text into it, clear it, read it back. Same
    // filtering as the Send button, so MCP cannot queue what the matrix
    // cannot type.
    json BasicTyperSetText(const std::string& text, bool send);
    json BasicTyperClear(bool cancel_queue);
    json BasicTyperStatus();
    // Writes a tokenised .bas straight into the program area. The typer's
    // counterpart, not a faster version of it: .basic source has to be
    // tokenised by BASIC itself.
    json LoadBasicProgram(const std::string& file_path, int pointer_block);
    json SaveBasicProgram(const std::string& file_path, int pointer_block);
    // Where a BASIC pointer block looks consistent, for a BASIC whose layout
    // is not known yet.
    json FindBasicBlocks();
 json KeyboardKey(const std::string& key, const std::string& action); json ControllerButton(int player, const std::string& button, const std::string& action); json GetInputState(); json GetKeyboardMode(); json SetKeyboardMode(bool enabled);
    json AddDisassemblerBookmark(u16 address, const std::string& name); json RemoveDisassemblerBookmark(u16 address); json ListDisassemblerBookmarks(); json AddSymbol(u8 bank, u16 address, const std::string& name); json RemoveSymbol(u8 bank, u16 address); json LoadSymbols(const std::string& file_path); json ListSymbols(); json LookupSymbolByName(const std::string& name); json LookupSymbolAtAddress(u8 bank, u16 address); json ListCallStack();
    json SelectMemoryRange(int area, int start, int end); json SetMemorySelectionValue(int area, u8 value); json GetMemorySelection(int area); json AddMemoryBookmark(int area, int address, const std::string& name); json RemoveMemoryBookmark(int area, int address); json ListMemoryBookmarks(int area); json MemorySearchCapture(int area); json MemorySearch(int area, const std::string& op, const std::string& compare_type, int compare_value, const std::string& data_type, bool narrow_previous = false); json MemoryFindBytes(int area, const std::string& hex_bytes); json MemoryFindBytesAdvanced(int area, const std::string& pattern, int wildcard_limit);
    json GetTraceLog(int start, int count); json SetTraceLog(bool enabled, u32 flags); json SetTraceLogDetailed(bool enabled, u32 flags, const json& arguments);

    // Multi-value live monitor: replaces the old unimplemented memory-watch
    // stubs (notes-only, one area at a time) with entries that carry a
    // current + previous value each frame, an optional per-frame freeze
    // (poke), and one shared AND/OR trigger condition that auto-pauses the
    // emulator. WatchTick() is not an MCP-invoked method - it is called once
    // per frame from emu_update() regardless of whether an MCP client is
    // connected, so freezes/conditions stay live between tool calls.
    json WatchAdd(int area, u32 address, const std::string& data_type, int size, const std::string& label);
    json WatchRemove(int id); json WatchList();
    json WatchFreeze(int id, const json& value); json WatchUnfreeze(int id);
    json WatchSetCondition(const json& conditions, const std::string& mode); json WatchClearCondition(); json WatchConditionStatus();
    bool WatchTick();

    GearSF7000Core* GetCore() { return m_core; }
private:
    GearSF7000Core* m_core;
    MemoryAreaInfo GetMemoryAreaInfo(int area);
};
#endif
#endif
