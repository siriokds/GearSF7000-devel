#include "mcp_config.h"

#if GEARSF7000_ENABLE_MCP

#include "mcp_tool_definitions.h"

// Split into several functions because one function with dozens of nested
// nlohmann::json initializer-lists blows past MSVC's C1060 compiler heap
// limit (each part below compiles fine on its own).

static void AddMcpToolDefinitionsCore_ExecutionControl(json& tools)
{
    // Execution control tools
    tools.push_back({
        {"name", "debug_pause"},
        {"title", "Debug Pause"},
        {"description", "Pause execution at current instruction; enter debugger."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "debug_continue"},
        {"title", "Debug Continue"},
        {"description", "Resume emulator execution from pause or breakpoint."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "debug_step_into"},
        {"title", "Debug Step Into"},
        {"description", "Step next Z80 CPU instruction; enter CALL subroutines."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "debug_step_over"},
        {"title", "Debug Step Over"},
        {"description", "Step next Z80 CPU instruction; skip CALL subroutines."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "debug_step_out"},
        {"title", "Debug Step Out"},
        {"description", "NOT AVAILABLE in this build: it needs the CPU-independent call stack, which is not enabled yet, and every call returns an error. To leave a subroutine meanwhile, read the return address off SP and use debug_run_to_cursor."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "debug_step_frame"},
        {"title", "Debug Step Frame"},
        {"description", "Run one or more video frames to VBlank. Default mode is async; use mode sync to wait until all requested frames complete."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"frames", {
                    {"type", "integer"},
                    {"description", "Number of frames to step. Default 1."},
                    {"minimum", 1},
                    {"maximum", 1000}
                }},
                {"mode", {
                    {"type", "string"},
                    {"description", "async arms a counter and returns at once, one frame consumed per main-loop iteration, so it inherits whatever paces presentation. sync runs the frames before replying: far faster, and the reply is the confirmation. sync always leaves the machine paused and reports frames_completed, which is fewer than asked for when a breakpoint stopped it."},
                    {"enum", json::array({"async", "sync"})}
                }}
            }},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "debug_reset"},
        {"title", "Debug Reset"},
        {"description", "Reset the emulated system. With paused=true it stops at PC 0000 with nothing executed, so a breakpoint can be armed before the first instruction."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"paused", {
                    {"type", "boolean"},
                    {"description", "Stop at the reset vector (PC 0000) with zero instructions executed. Default false runs normally."}
                }}
            }},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_build_info"},
        {"title", "Get Build Info"},
        {"description", "Which build of the emulator this is: commit, compile timestamp, and the options compiled in. The timestamp is the reliable part - a commit ending in '-dirty' names every build made from it, and the .app bundle can be older than the binary in the source tree. Check this before reporting that a fix did not work."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "debug_get_status"},
        {"title", "Debug Get Status"},
        {"description", "Read debugger state: paused, breakpoint hit, current PC."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_mcp_status"},
        {"title", "Get MCP Status"},
        {"description", "Read bounded transport and queue diagnostics: listener state, active-request age, timeout/rejection counters and queue depths. Request identities and payloads are never exposed."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}}}
    });

    tools.push_back({
        {"name", "get_z80_clock"},
        {"title", "Get Z80 Clock"},
        {"description", "Read the Z80 elapsed counter used by the Z80 Status panel: ticks, microseconds and milliseconds."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "reset_z80_clock"},
        {"title", "Reset Z80 Clock"},
        {"description", "Reset the Z80 elapsed counter to zero, equivalent to the RESET button in Z80 Status."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });
}

static void AddMcpToolDefinitionsCore_Breakpoints(json& tools)
{
    // Breakpoint tools
    tools.push_back({
        {"name", "set_breakpoint"},
        {"title", "Set Breakpoint"},
        {"description", "Add execute/read/write breakpoint at logical ROM/RAM or VRAM address."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"address", {
                    {"type", "string"},
                    {"description", "Logical hex address; ranges: rom_ram 0000-FFFF, vram 0000-3FFF."}
                }},
                {"memory_area", {
                    {"type", "string"},
                    {"description", "Memory area: rom_ram (default) or vram."},
                    {"enum", json::array({"rom_ram", "vram"})}
                }},
                {"read", {
                    {"type", "boolean"},
                    {"description", "Read access breakpoint; PC stops after the access. Default false."}
                }},
                {"write", {
                    {"type", "boolean"},
                    {"description", "Write access breakpoint; PC stops after the access. Default false."}
                }},
                {"execute", {
                    {"type", "boolean"},
                    {"description", "Execution breakpoint; only valid for rom_ram. Default true."}
                }}
            }},
            {"required", json::array({"address"})}
        }}
    });

    tools.push_back({
        {"name", "set_breakpoint_range"},
        {"title", "Set Breakpoint Range"},
        {"description", "Add execute/read/write breakpoint over a logical address range."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"start_address", {
                    {"type", "string"},
                    {"description", "Start logical hex address; ranges: rom_ram 0000-FFFF, vram 0000-3FFF."}
                }},
                {"end_address", {
                    {"type", "string"},
                    {"description", "End logical hex address; same ranges as start_address."}
                }},
                {"memory_area", {
                    {"type", "string"},
                    {"description", "Memory area: rom_ram (default) or vram."},
                    {"enum", json::array({"rom_ram", "vram"})}
                }},
                {"read", {
                    {"type", "boolean"},
                    {"description", "Read access breakpoint; PC stops after the access. Default false."}
                }},
                {"write", {
                    {"type", "boolean"},
                    {"description", "Write access breakpoint; PC stops after the access. Default false."}
                }},
                {"execute", {
                    {"type", "boolean"},
                    {"description", "Execution breakpoint; only valid for rom_ram. Default true."}
                }}
            }},
            {"required", json::array({"start_address", "end_address"})}
        }}
    });

    tools.push_back({
        {"name", "remove_breakpoint"},
        {"title", "Remove Breakpoint"},
        {"description", "Remove matching single/range breakpoint by address, end_address, and memory area."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"address", {
                    {"type", "string"},
                    {"description", "Logical address hex; range removals use this as start."}
                }},
                {"end_address", {
                    {"type", "string"},
                    {"description", "Range end address hex; required only for range breakpoints."}
                }},
                {"memory_area", {
                    {"type", "string"},
                    {"description", "Memory area: rom_ram (default) or vram."},
                    {"enum", json::array({"rom_ram", "vram"})}
                }}
            }},
            {"required", json::array({"address"})}
        }}
    });

    tools.push_back({
        {"name", "list_breakpoints"},
        {"title", "List Breakpoints"},
        {"description", "List all execution/read/write breakpoints."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "add_debug_rule"},
        {"title", "Add Debug Event Rule"},
        {"description", "Add a common debug rule. I/O and PPI can trace CPU read/write attempts; PPI event means a decoded latch-state transition. Old/new predicates are accepted only when the selected core hook can provide both values."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                // Kept in step with ParseDebugEventCategory in the adapter: a
                // name accepted there but missing here cannot be reached, and
                // one advertised here but not parsed is rejected at call time.
                {"category", {{"type", "string"}, {"enum", json::array({"cpu_memory", "cpu_execute", "io", "vram", "vdp_register", "video_timing", "ppi", "fdc", "tape", "audio"
#if GEARSF7000_ENABLE_AY
                    , "ay_expansion"
#endif
                })}}},
                {"start_address", {{"type", "string"}, {"description", "Start address, hexadecimal (CPU 0000-FFFF; I/O 00-FF; VRAM 0000-3FFF; VDP register 00-FF; video-timing scanline 0000-FFFF)."}}},
                {"end_address", {{"type", "string"}, {"description", "End address, hexadecimal; defaults to start_address."}}},
                {"read", {{"type", "boolean"}}}, {"write", {{"type", "boolean"}}}, {"execute", {{"type", "boolean"}}},
                {"event", {{"type", "boolean"}, {"description", "Device state transition; used by video_timing, FDC, tape, audio and PPI latch-state events."}}},
                {"pause", {{"type", "boolean"}, {"description", "Pause at the selected matching hit. Default false."}}},
                {"log", {{"type", "boolean"}, {"description", "Append matching events to the fixed trace buffer. Default true."}}},
                {"value_mask", {{"type", "integer"}, {"minimum", 0}, {"maximum", 255}}},
                {"value_expected", {{"type", "integer"}, {"minimum", 0}, {"maximum", 255}, {"description", "Expected new/returned value; mask defaults to FF."}}},
                {"condition", {{"type", "string"}, {"enum", json::array({"any", "after_masked_equal", "before_masked_equal", "before_and_after_masked_equal", "changed", "after_less_than_before", "after_less_or_equal_before", "after_greater_than_before", "after_greater_or_equal_before", "masked_bits_changed", "before_masked_set", "after_masked_set", "before_masked_clear", "after_masked_clear"})}}},
                {"before_value_expected", {{"type", "integer"}, {"minimum", 0}, {"maximum", 255}, {"description", "Expected old value for before/exact-transition predicates; mask defaults to FF."}}},
                {"one_shot", {{"type", "boolean"}, {"description", "Disable the rule after it pauses successfully."}}},
                {"break_on_hit", {{"type", "integer"}, {"minimum", 1}, {"description", "Matching hit at which pause occurs; default 1."}}}
            }},
            {"required", json::array({"category", "start_address"})}
        }}
    });

    tools.push_back({
        {"name", "add_device_debug_rule"},
        {"title", "Add Device Debug Rule"},
        {"description", "Add a schema-v2 rule using stable device/space keys returned by list_debug_devices. Supports wide internal state such as the 14-bit TMS9918 VRAM Address Counter."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"device", {{"type", "string"}, {"description", "Stable device instance key."}}},
                {"space", {{"type", "string"}, {"description", "Stable space key, for example state, registers or memory."}}},
                {"start_target", {{"type", "string"}, {"description", "First target ID/address in hexadecimal."}}},
                {"end_target", {{"type", "string"}, {"description", "Last target ID/address in hexadecimal; defaults to start_target."}}},
                {"read", {{"type", "boolean"}}}, {"write", {{"type", "boolean"}}},
                {"execute", {{"type", "boolean"}}},
                {"event", {{"type", "boolean"}, {"description", "Internal device transition; default true for state space."}}},
                {"pause", {{"type", "boolean"}, {"description", "Pause on a matching hit; default false."}}},
                {"log", {{"type", "boolean"}, {"description", "Append matches to Debug Events; default true."}}},
                {"condition", {{"type", "string"}, {"enum", json::array({"any", "after_masked_equal", "before_masked_equal", "before_and_after_masked_equal", "changed", "after_less_than_before", "after_less_or_equal_before", "after_greater_than_before", "after_greater_or_equal_before", "masked_bits_changed", "before_masked_set", "after_masked_set", "before_masked_clear", "after_masked_clear"})}}},
                {"value_mask", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295ULL}}},
                {"value_expected", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295ULL}}},
                {"before_value_expected", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295ULL}}},
                {"one_shot", {{"type", "boolean"}}},
                {"break_on_hit", {{"type", "integer"}, {"minimum", 1}}}
            }},
            {"required", json::array({"device", "space", "start_target"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "list_debug_rules"}, {"title", "List Debug Event Rules"},
        {"description", "List common event rules and hit counters."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}}}
    });

    tools.push_back({
        {"name", "list_debug_devices"}, {"title", "List Debug Devices"},
        {"description", "Discover the hierarchical device instances, reusable hardware types, debug spaces and currently published targets."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}}}
    });

    tools.push_back({
        {"name", "remove_debug_rule"}, {"title", "Remove Debug Event Rule"},
        {"description", "Remove a common event rule by id."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {{"type", "object"}, {"properties", {{"id", {{"type", "integer"}, {"minimum", 1}}}}}, {"required", json::array({"id"})}}}
    });

    tools.push_back({
        {"name", "clear_debug_rules"}, {"title", "Clear Debug Event Rules"},
        {"description", "Remove all common event rules."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}}}
    });

    tools.push_back({
        {"name", "get_debug_events"}, {"title", "Get Debug Events"},
        {"description", "Read and filter recent events from the fixed common debugger trace buffer. The limit selects the most recent matching events; order controls only their returned order."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {{"type", "object"}, {"properties", {
            {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4096}}},
            {"order", {{"type", "string"}, {"enum", json::array({"oldest_first", "newest_first"})}}},
            {"sequence_from", {{"type", "integer"}, {"minimum", 0}}},
            {"sequence_to", {{"type", "integer"}, {"minimum", 0}}},
            {"clock_from", {{"type", "integer"}, {"minimum", 0}}},
            {"clock_to", {{"type", "integer"}, {"minimum", 0}}},
            {"pc_start", {{"type", "integer"}, {"minimum", 0}, {"maximum", 65535}}},
            {"pc_end", {{"type", "integer"}, {"minimum", 0}, {"maximum", 65535}}},
            {"target_start", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295ULL}}},
            {"target_end", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295ULL}}},
            {"category", {{"type", "string"}, {"enum", json::array({"cpu_memory", "cpu_execute", "io", "vram", "vdp_register", "video_timing", "ppi", "fdc", "tape", "audio", "ay_expansion", "device_state"})}}},
            {"access", {{"type", "string"}, {"enum", json::array({"read", "write", "execute", "event"})}}},
            {"device", {{"type", "string"}}},
            {"space", {{"type", "string"}}}
        }}, {"additionalProperties", false}}}
    });

    tools.push_back({
        {"name", "clear_debug_events"}, {"title", "Clear Debug Events"},
        {"description", "Clear the common debugger trace buffer."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}}}
    });

    tools.push_back({
        {"name", "toggle_irq_breakpoints"},
        {"title", "Toggle IRQ Breakpoints"},
        {"description", "Enable/disable interrupt breakpoints for RESET, NMI, INT."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"enabled", {
                    {"type", "boolean"},
                    {"description", "true breaks on IRQ, false disables."}
                }}
            }},
            {"required", json::array({"enabled"})}
        }}
    });
}

static void AddMcpToolDefinitionsCore_Memory(json& tools)
{
    // Memory tools
    tools.push_back({
        {"name", "list_memory_areas"},
        {"title", "List Memory Areas"},
        {"description", "List memory spaces/tabs: WRAM, VRAM, ROM banks; returns area IDs, sizes, offsets."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "read_memory"},
        {"title", "Read Memory"},
        {"description", "Read bytes from memory area/tab by physical 0-based offset."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"area", {
                    {"type", "integer"},
                    {"description", "Memory area ID from list_memory_areas."}
                }},
                {"offset", {
                    {"type", "string"},
                    {"description", "0-based hex offset in area, e.g. '0100'."}
                }},
                {"size", {
                    {"type", "integer"},
                    {"description", "Number of bytes to read."}
                }}
            }},
            {"required", json::array({"area", "offset", "size"})}
        }}
    });

    // Nested via local json variables, not a single literal, to keep MSVC's
    // compiler heap (C1060) from choking on too-deep initializer-list nesting.
    json atomic_snapshot_range_item_properties = json::object();
    atomic_snapshot_range_item_properties["area"] = {{"type", "integer"}, {"minimum", 0}, {"maximum", 4}};
    atomic_snapshot_range_item_properties["offset"] = {{"type", "integer"}, {"minimum", 0}};
    atomic_snapshot_range_item_properties["size"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 4096}};

    json atomic_snapshot_range_item = json::object();
    atomic_snapshot_range_item["type"] = "object";
    atomic_snapshot_range_item["properties"] = atomic_snapshot_range_item_properties;
    atomic_snapshot_range_item["required"] = json::array({"area", "offset", "size"});
    atomic_snapshot_range_item["additionalProperties"] = false;

    json atomic_snapshot_memory_ranges = json::object();
    atomic_snapshot_memory_ranges["type"] = "array";
    atomic_snapshot_memory_ranges["maxItems"] = 16;
    atomic_snapshot_memory_ranges["description"] = "Physical memory areas from list_memory_areas. At most 4096 bytes per range and 16384 bytes total.";
    atomic_snapshot_memory_ranges["items"] = atomic_snapshot_range_item;

    tools.push_back({
        {"name", "get_atomic_snapshot"},
        {"title", "Get Atomic Machine Snapshot"},
        {"description", "Capture CPU, VDP and selected memory ranges in one command on the emulation thread. The reply includes clock/frame markers proving that the component reads describe one scheduling boundary."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"cpu", {{"type", "boolean"}, {"description", "Include CPU registers and clock; default true."}}},
                {"vdp", {{"type", "boolean"}, {"description", "Include VDP registers and internal status; default true."}}},
                {"sf7000", {{"type", "boolean"}, {"description", "Include SF-7000 state; default false."}}},
                {"memory_ranges", atomic_snapshot_memory_ranges}
            }},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "write_memory"},
        {"title", "Write Memory"},
        {"description", "Write hex bytes to memory area/tab by physical 0-based offset."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"area", {
                    {"type", "integer"},
                    {"description", "Memory area ID from list_memory_areas."}
                }},
                {"offset", {
                    {"type", "string"},
                    {"description", "0-based hex offset in area, e.g. '0100'."}
                }},
                {"bytes", {
                    {"type", "string"},
                    {"description", "Hex bytes, spaces optional, e.g. 'A9 00 85 10'."}
                }}
            }},
            {"required", json::array({"area", "offset", "bytes"})}
        }}
    });

    // Register tools
    tools.push_back({
        {"name", "write_z80_register"},
        {"title", "Write Z80 Register"},
        {"description", "Write Z80 CPU register, alternate register, index register, SP/PC/WZ/I/R."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"name", {
                    {"type", "string"},
                    {"description", "Register: AF, BC, DE, HL, AF', BC', DE', HL', IX, IY, SP, PC, WZ, A, F, B, C, D, E, H, L, I, R."}
                }},
                {"value", {
                    {"type", "string"},
                    {"description", "Hex value."}
                }}
            }},
            {"required", json::array({"name", "value"})}
        }}
    });
}

static void AddMcpToolDefinitionsCore_DisassemblyAndMedia(json& tools)
{
    // Disassembly tool
    tools.push_back({
        {"name", "get_disassembly"},
        {"title", "Get Disassembly"},
        {"description", "Read recorded Z80 disassembly for logical range: bank, segment, mnemonic, bytes. Records exist after execution; max practical range 0x2000."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"start_address", {
                    {"type", "string"},
                    {"description", "Start logical address hex: 'E177', '0xE177', or '$E177'."}
                }},
                {"end_address", {
                    {"type", "string"},
                    {"description", "End logical address hex; must be >= start_address."}
                }},
                {"bank", {
                    {"type", "string"},
                    {"description", "Optional ROM bank 00-FF; read records from that bank instead of current map."}
                }},
                {"resolve_symbols", {
                    {"type", "boolean"},
                    {"description", "Resolve addresses to symbols when available. Default false."}
                }},
                {"detailed", {
                    {"type", "boolean"},
                    {"description", "Include opcode bytes, jump targets, IRQ metadata. Default false compact output."}
                }}
            }},
            {"required", json::array({"start_address", "end_address"})}
        }}
    });

    tools.push_back({
        {"name", "get_code_coverage"},
        {"title", "Get Executed Code Coverage"},
        {"description", "Export code that actually reached a Z80 instruction boundary. Unlike the disassembly view, debugger inspection does not count. Summary groups by segment/bank; detailed entries add logical and physical addresses, bytes, hit counts and first/last clocks."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"detailed", {{"type", "boolean"}, {"description", "Include individual executed instructions. Default false."}}},
                {"offset", {{"type", "integer"}, {"minimum", 0}, {"description", "First detailed record, for pagination."}}},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4096}, {"description", "Detailed records to return; default 512."}}}
            }},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "clear_code_coverage"},
        {"title", "Clear Executed Code Coverage"},
        {"description", "Reset execution hit counters without deleting disassembly or changing the emulated machine."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}}}
    });

    // Media info tool
    tools.push_back({
        {"name", "get_media_info"},
        {"title", "Get Media Info"},
        {"description", "Read loaded SC-3000/SG-1000 ROM path together with cassette and SF-7000 media state."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "list_recent_roms"},
        {"title", "List Recent ROMs"},
        {"description", "List recent ROMs with absolute local file_path values for load_rom."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "list_recent_tapes"},
        {"title", "List Recent Tapes"},
        {"description", "List recent cassette files with absolute local file_path values for load_tape."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "list_recent_disks"},
        {"title", "List Recent Disks"},
        {"description", "List recent SF-7000 disk images with absolute local file_path values for mount_disk."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });
}

void AddMcpToolDefinitionsCore(json& tools)
{
    AddMcpToolDefinitionsCore_ExecutionControl(tools);
    AddMcpToolDefinitionsCore_Breakpoints(tools);
    AddMcpToolDefinitionsCore_Memory(tools);
    AddMcpToolDefinitionsCore_DisassemblyAndMedia(tools);
}

#endif /* GEARSF7000_ENABLE_MCP */
