#include "mcp_config.h"

#if GEARSF7000_ENABLE_MCP

#include "mcp_tool_definitions.h"

void AddMcpToolDefinitionsDebug(json& tools)
{
    // Chip status tools
    tools.push_back({
        {"name", "get_z80_status"},
        {"title", "Get Z80 Status"},
        {"description", "Read Z80 CPU state: registers, flags, interrupts, HALT, interrupt mode."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_vdp_registers"},
        {"title", "Get VDP Registers"},
        {"description", "Read TMS9918 VDP registers R0-R7 with hex values and decoded meanings."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_vdp_status"},
        {"title", "Get VDP Status"},
        {"description", "Read TMS9918 VDP state, raster position, CPU-port VRAM latch, in-flight transfer and arbitration counters."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "set_region"},
        {"title", "Set Region"},
        {"description", "Switch the machine between PAL (313 lines, 50.16 Hz) and NTSC (262 lines, 59.92 Hz), or back to whatever the cartridge implies. Resets the machine to apply it."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"region", {{"type", "string"},
                    {"enum", json::array({"pal", "ntsc", "auto"})}}}
            }},
            {"required", json::array({"region"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_slot_history"},
        {"title", "Get Slot History"},
        {"description", "Live, per-scanline map of every one of the 171 DRAM slots in the current raster line (342 dots, blanking included), keyed by physical beam position rather than which logical line a fetch belongs to. Shows what the schedule assigned each slot and, for CPU slots, whether the CPU actually used it and with what address/value. Analogous to C64 Debugger's VIC-II bus access view."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "set_renderer_source"},
        {"title", "Set Renderer Source"},
        {"description", "Choose which of the two parallel renderers reaches the screen: the legacy per-line renderer or the continuous streaming renderer. Both keep running and being compared either way."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"source", {
                    {"type", "string"},
                    {"enum", json::array({"legacy", "streaming"})},
                    {"description", "Renderer to present. Defaults to legacy at startup."}
                }}
            }},
            {"required", json::array({"source"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "read_framebuffer"},
        {"title", "Read Frame Buffer"},
        {"description", "Read composed frame pixels as TMS9918 palette indices, one hex nibble per pixel, 64 characters per 256 pixel line. Reads the presented buffer, or either renderer directly for comparison."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"source", {
                    {"type", "string"},
                    {"enum", json::array({"presented", "legacy", "streaming"})},
                    {"description", "Which buffer to read. Default presented."}
                }},
                {"y", {{"type", "integer"}, {"description", "First line, 0-191. Default 0."}}},
                {"height", {{"type", "integer"}, {"description", "Number of lines. Default all remaining."}}}
            }},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_psg_status"},
        {"title", "Get PSG Status"},
        {"description", "Read SN76489 PSG audio state: 3 tone channels, noise, volume, period, frequency."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_sf7000_status"},
        {"title", "Get SF-7000 Status"},
        {"description", "Read SF-7000 drive, uPD765 FDC, PPI2, IPL overlay and disk rotation state."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_ay8910_status"},
        {"title", "Get AY-3-8910 Status"},
        {"description", "Read AY-3-8910 SGM audio state: 3 channels, mixer, noise, envelope, registers, mute."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_screenshot"},
        {"title", "Get Screenshot"},
        {"description", "Capture current screen/frame/video output as PNG screenshot image."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "set_full_raster_debug_enabled"},
        {"title", "Set Full Raster Debug Enabled"},
        {"description", "Full Frame view: the whole 342-dot TMS9918 raster (sync/blank/color burst/border/active), 342x262 on NTSC or 342x313 on PAL, instead of the ~256x192 picture the normal screen shows. Blanking and color burst are true black; the horizontal and vertical sync bands are a dark slate (22,24,38) so they can be told apart from the blanking around them; border and active carry the real backdrop/content color. Same thing as Debug > Video > Overscan > Full Frame; it is deliberately unavailable in Computer mode. Off by default; costs nothing when disabled."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"enabled", {{"type", "boolean"}, {"description", "true to render and expose the full-raster debug buffer, false to disable it again."}}}
            }},
            {"required", json::array({"enabled"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "dump_crt_signal"},
        {"title", "Dump CRT Signal"},
        {"description", "Records what the VDP emits on its second, signal-level video output (ICrtSignalSink) to a binary file: the sync/blank/color-burst/data call sequence with durations in dots and palette indices for the displayed span, in raster order. For feeding an offline CRT decoder or checking the emitted timing against the raster tables. Capture spans frames and is not finished when this returns - wait before reading the file. Format: 'GCRT' magic, u8 version, then records of u8 type (0=configure, 1=sync, 2=blank, 3=burst, 4=data) + u16 dots, where configure adds u8 isPAL + u8 signalOutput and data adds u16 count + that many indices."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"file_path", {{"type", "string"}, {"description", "Absolute path to write the capture to."}}},
                {"lines", {{"type", "integer"}, {"description", "How many scanlines to capture. One frame is 262 (NTSC) or 313 (PAL); capturing two frames' worth lets a decoder find a field boundary."}}}
            }},
            {"required", json::array({"file_path", "lines"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_full_raster_debug_screenshot"},
        {"title", "Get Full Raster Debug Screenshot"},
        {"description", "PNG of the full-raster debug buffer (342 x 262/313 depending on region), see set_full_raster_debug_enabled. Errors if that hasn't been enabled yet."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "export_memory_file"},
        {"title", "Export Memory To File"},
        {"description", "MCP counterpart of the desktop Debug > Memory > Show Memory Import window's RAM/ROM/VRAM export - dumps a range from one of the areas list_memory_areas reports (area 0 is the real Z80 address space, respects bank switching; area 3 is VRAM) straight to a binary file. Use list_memory_areas first if unsure which area/offset covers what."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", true}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"area", {{"type", "integer"}, {"description", "Memory area id from list_memory_areas (0=Z80 address space, 3=VRAM, etc)."}}},
                {"offset", {{"type", "integer"}, {"description", "Byte offset within the area to start reading from."}}},
                {"length", {{"type", "integer"}, {"description", "How many bytes to export."}}},
                {"file_path", {{"type", "string"}, {"description", "Absolute path to write the dump to."}}}
            }},
            {"required", json::array({"area", "offset", "length", "file_path"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "import_memory_file"},
        {"title", "Import Memory From File"},
        {"description", "MCP counterpart of the desktop Debug > Memory > Show Memory Import window's RAM/ROM/VRAM import - writes a binary file straight into one of the areas list_memory_areas reports (area 0 is the real Z80 address space, respects bank switching; area 3 is VRAM), starting at offset. Writes past the area's own size are silently truncated, same as WriteMemoryArea/write_memory."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", true}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"area", {{"type", "integer"}, {"description", "Memory area id from list_memory_areas (0=Z80 address space, 3=VRAM, etc)."}}},
                {"offset", {{"type", "integer"}, {"description", "Byte offset within the area to start writing at."}}},
                {"file_path", {{"type", "string"}, {"description", "Absolute path to read the file from."}}}
            }},
            {"required", json::array({"area", "offset", "file_path"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "save_disassembly_file"},
        {"title", "Save Disassembly To File"},
        {"description", "MCP counterpart of the Disassembler window's File > Save Disassembly - writes a gapless disassembly listing to a plain-text file (SC3K-System style: unknown bytes become `db` directives instead of being skipped, so the output covers the whole range start to end, every line labelled). full=false ('Visible') dumps the disassembler's current live view (GetDisassembleRecord() for every CPU address, respecting whatever mapper bank/IPL overlay is active right now). full=true ('Full') dumps every physical disassembly map (BIOS+RAM+SGM+ROM) unconditionally, independent of which bank/mode is currently active - same mapper-agnostic principle as export_memory_file's raw buffers."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", true}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"full", {{"type", "boolean"}, {"description", "false = current live view only (default), true = every physical map unconditionally."}}},
                {"file_path", {{"type", "string"}, {"description", "Absolute path to write the disassembly text to."}}}
            }},
            {"required", json::array({"file_path"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "save_debug_png"},
        {"title", "Save Debug View As PNG"},
        {"description", "MCP counterpart of the VDP debug views' right-click \"Save As PNG...\" (Background/Name Table, Pattern Table, Sprites) - writes the already-colored RGB888 debug render to a PNG file, cropped to the meaningful area (not the fixed 256x256 backing buffer's unused padding). Complementary to export_memory_file's raw VRAM dump: this is for documentation/visual sharing, the raw dump is what a ROM-conversion pipeline actually needs."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", true}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"view", {{"type", "string"}, {"description", "\"background\", \"tiles\", or \"sprite\"."}}},
                {"sprite_index", {{"type", "integer"}, {"description", "0-31, only used when view=\"sprite\"."}}},
                {"file_path", {{"type", "string"}, {"description", "Absolute path to write the PNG to."}}}
            }},
            {"required", json::array({"view", "file_path"})},
            {"additionalProperties", false}
        }}
    });

    // Media and state management tools
    tools.push_back({
        {"name", "load_rom"},
        {"title", "Load ROM"},
        {"description", "Load an absolute local SC-3000/SG-1000 ROM through the same command used by the desktop menu: reset, recent ROM, debugger reset and automatic matching .sym load."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", true}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"file_path", {
                    {"type", "string"},
                    {"description", "Absolute local ROM file path. Network URLs are not supported."}
                }}
            }},
            {"required", json::array({"file_path"})}
        }}
    });

    tools.push_back({
        {"name", "set_start_paused"},
        {"title", "Set Start Paused"},
        {"description", "Toggle whether load_rom/start_sf7000 pause the machine immediately after loading, before any instruction executes. Set true to arm a breakpoint at 0000 before the ROM's first instruction runs, then debug_continue to start it with the breakpoint live. Applies to the next load, not to whatever is already running."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"paused", {{"type", "boolean"}}}
            }},
            {"required", json::array({"paused"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "eject_rom"},
        {"title", "Eject ROM"},
        {"description", "Remove the currently inserted cartridge and bring the machine back to the same idle state it is in before anything is ever loaded. Flushes the outgoing cartridge's own battery-backed SRAM first. Does not touch SF-7000/disk state."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "load_tape"},
        {"title", "Load Tape"},
        {"description", "Load an absolute local .bit, .bas, .basic or .wav cassette through the same command used by the SR-1000 cassette menu. It does not reset or start a ROM."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", true}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"file_path", {{"type", "string"}, {"description", "Absolute local tape file path. Network URLs are not supported."}}}
            }},
            {"required", json::array({"file_path"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "set_tape_speed"},
        {"title", "Set Tape Speed"},
        {"description", "Adjust SR-1000 tape playback speed as a percentage offset from nominal (-20.0 to +20.0). +15.0 is a reliable faster-load value. Resets to 0.0 on tape rewind or eject."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"percent", {{"type", "number"}, {"description", "Speed offset in percent, -20.0 to +20.0. 0.0 = nominal speed."}}}
            }},
            {"required", json::array({"percent"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "tape_play"},
        {"title", "Tape Play"},
        {"description", "Press the Play button on the SR-1000 cassette player. Only has effect when a tape is loaded; the motor must be requested by the running ROM (e.g. after a BASIC LOAD command) for data to flow."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}}}
    });

    tools.push_back({
        {"name", "tape_stop"},
        {"title", "Tape Stop"},
        {"description", "Press the Stop button on the SR-1000 cassette player, pausing tape playback."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}}}
    });

    tools.push_back({
        {"name", "tape_rewind"},
        {"title", "Tape Rewind"},
        {"description", "Rewind the SR-1000 cassette to the beginning."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}}}
    });

    tools.push_back({
        {"name", "mount_disk"},
        {"title", "Mount SF-7000 Disk"},
        {"description", "Mount an absolute local SF-7000 disk image through the same command used by the Disc menu. This intentionally does not start or reset SF-7000; call start_sf7000 separately to boot its IPL."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", true}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"file_path", {{"type", "string"}, {"description", "Absolute local .sf7, .dsk, .hfe or .ds7 disk image path."}}},
                {"write_protected", {{"type", "boolean"}, {"description", "Request write protection. Defaults to the current saved SF-7000 setting."}}}
            }},
            {"required", json::array({"file_path"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "start_sf7000"},
        {"title", "Start SF-7000"},
        {"description", "Perform the desktop menu's Start SF-7000 action. If needed it automatically loads the configured 8 KB IPL, then resets into the SF-7000 boot sequence. It does not mount a disk."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "load_symbols"},
        {"title", "Load Symbols"},
        {"description", "Load .sym debug symbols (BANK:ADDRESS LABEL); append to symbol table."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", true}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"file_path", {
                    {"type", "string"},
                    {"description", "Absolute symbol file path."}
                }}
            }},
            {"required", json::array({"file_path"})}
        }}
    });

    tools.push_back({
        {"name", "list_save_state_slots"},
        {"title", "List Save State Slots"},
        {"description", "List save-state slots: slot, ROM name, timestamp, screenshot flag."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", true}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "select_save_state_slot"},
        {"title", "Select Save State Slot"},
        {"description", "Select active save-state slot 1-5 for save_state/load_state."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"slot", {
                    {"type", "integer"},
                    {"description", "Slot number 1-5."},
                    {"minimum", 1},
                    {"maximum", 5}
                }}
            }},
            {"required", json::array({"slot"})}
        }}
    });

    tools.push_back({
        {"name", "save_state"},
        {"title", "Save State"},
        {"description", "Save emulator state to active save-state slot."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", true}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "load_state"},
        {"title", "Load State"},
        {"description", "Load emulator state from active save-state slot."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", true}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "save_state_file"},
        {"title", "Save State File"},
        {"description", "Save emulator state to an explicit file path."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", true}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"file_path", {
                    {"type", "string"},
                    {"description", "Absolute destination file path."}
                }}
            }},
            {"required", json::array({"file_path"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "load_state_file"},
        {"title", "Load State File"},
        {"description", "Load emulator state from an explicit file path."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", true}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"file_path", {
                    {"type", "string"},
                    {"description", "Absolute save-state file path."}
                }}
            }},
            {"required", json::array({"file_path"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "set_fast_forward_speed"},
        {"title", "Set Fast Forward Speed"},
        {"description", "Set fast-forward speed index: 0=1.5x, 1=2x, 2=2.5x, 3=3x, 4=unlimited."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"speed", {
                    {"type", "integer"},
                    {"description", "Speed index 0-4."},
                    {"minimum", 0},
                    {"maximum", 4}
                }}
            }},
            {"required", json::array({"speed"})}
        }}
    });

    tools.push_back({
        {"name", "toggle_fast_forward"},
        {"title", "Toggle Fast Forward"},
        {"description", "Enable/disable fast-forward mode at configured speed."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"enabled", {
                    {"type", "boolean"},
                    {"description", "true enables fast forward; false disables."}
                }}
            }},
            {"required", json::array({"enabled"})}
        }}
    });

    tools.push_back({
        {"name", "get_rewind_status"},
        {"title", "Get Rewind Status"},
        {"description", "Read the frame recorder: how many frames are held, what they cost, which sections carry the weight, and whether any were dropped."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    tools.push_back({
        {"name", "configure_rewind"},
        {"title", "Configure Rewind"},
        {"description", "Turn the frame recorder on or off and set how many seconds it keeps. A duration that will not fit the memory limit is refused with the figure it needs, rather than being quietly shortened."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"enabled", {{"type", "boolean"}, {"description", "Whether to record."}}},
                {"seconds", {{"type", "integer"}, {"description", "Seconds of play to keep. Default 60."}, {"minimum", 1}, {"maximum", 600}}},
                {"frames_per_snapshot", {{"type", "integer"}, {"description", "1 records every frame, which is what frame-exact work needs."}, {"minimum", 1}}},
                {"memory_limit_mb", {{"type", "integer"}, {"description", "Ceiling used to validate the request. It does not shorten it."}, {"minimum", 1}}}
            }},
            {"required", json::array({"enabled"})}
        }}
    });

    tools.push_back({
        {"name", "get_sync_settings"},
        {"title", "Get Sync Settings"},
        {"description", "Current frame pacing mode (accurate, accurate_vsync, gaming or gaming_vsync), plus the derived rates: whether the clocks are aligned to the host display, whether presentation waits for the blank, the machine's native and effective frame rates, and the measured display refresh."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "set_sync_settings"},
        {"title", "Set Sync Settings"},
        {"description", "Choose how frames are paced. Two independent things: whether the crystals run at their real values ('accurate*', 59.9227 NTSC / 50.1590 PAL - use it for anything being measured) or aligned to the host display ('gaming*', smoother motion), and whether presenting waits for the blank ('*_vsync', no tearing) or happens immediately (lowest latency). The alignment is latched at Reset, so moving between accurate and gaming takes effect on the next reset; vertical sync changes immediately."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"pacing", {{"type", "string"}, {"enum", {"accurate", "accurate_vsync", "gaming", "gaming_vsync"}}, {"description", "Frame pacing mode."}}}
            }},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_rewind_position"},
        {"title", "Get Rewind Position"},
        {"description", "Where the recorder's cursor is now and whether the machine is running or paused. Reports the position inside the buffer, how far back it is in frames and seconds, and the machine's own frame number."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "analyze_rewind_range"},
        {"title", "Analyze Recorder Frame Range"},
        {"description", "Collect CPU, VDP and selected memory data across a Recorder interval in one request. Positions use the Recorder counter (1 oldest, snapshot_count newest). Memory is delta encoded; the live state and Recorder cursor are restored, and no emulated frame is executed."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"start_position", {{"type", "integer"}, {"minimum", 1}}},
                {"end_position", {{"type", "integer"}, {"minimum", 1}}},
                {"step", {{"type", "integer"}, {"minimum", 1}, {"description", "Sample every N Recorder snapshots; the end position is always included. Default 1."}}},
                {"cpu", {{"type", "boolean"}, {"description", "Include compact CPU state. Default true."}}},
                {"vdp", {{"type", "boolean"}, {"description", "Include VDP registers, status, hashes, overflow and collision. Default true."}}},
                {"changes_only", {{"type", "boolean"}, {"description", "Return a memory baseline then changed spans only. Default true."}}},
                {"memory_ranges", {
                    {"type", "array"}, {"maxItems", 8},
                    {"description", "Physical areas from list_memory_areas; 4096 bytes each, 16384 total."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"area", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4}}},
                            {"offset", {{"type", "integer"}, {"minimum", 0}}},
                            {"size", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4096}}}
                        }},
                        {"required", json::array({"area", "offset", "size"})},
                        {"additionalProperties", false}
                    }}
                }}
            }},
            {"required", json::array({"start_position", "end_position"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "rewind_transport"},
        {"title", "Rewind Transport"},
        {"description", "The recorder's transport controls: play, pause, one frame back or forward, jump to the oldest or newest frame held. Every action except play leaves the machine paused."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"action", {{"type", "string"},
                            {"enum", json::array({"play", "play_from_here", "pause", "step_back", "step_forward", "oldest", "newest"})},
                            {"description", "Which control to press."}}},
                {"frames", {{"type", "integer"}, {"description", "How many frames step_back/step_forward move. Default 1."}, {"minimum", 1}}}
            }},
            {"required", json::array({"action"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "rewind_seek"},
        {"title", "Rewind Seek"},
        {"description", "Go to a recorded frame and stay paused there. Snapshot 1 is the oldest frame held and snapshot_count the newest; age_frames counts backwards from the newest instead, if that is easier."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"snapshot", {
                    {"type", "integer"},
                    {"description", "1 is the oldest frame held, snapshot_count the newest."},
                    {"minimum", 1}
                }},
                {"age_frames", {
                    {"type", "integer"},
                    {"description", "Frames back from the newest; 0 is the newest. Use this or snapshot, not both."},
                    {"minimum", 0}
                }}
            }}
        }}
    });

}

#endif /* GEARSF7000_ENABLE_MCP */
