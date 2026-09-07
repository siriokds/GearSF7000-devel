#include "mcp_config.h"

#if GEARSF7000_ENABLE_MCP

#include "mcp_tool_definitions.h"

void AddMcpToolDefinitionsInput(json& tools)
{
    // Controller input tools
    tools.push_back({
        {"name", "trigger_nmi"},
        {"title", "Trigger NMI / Reset Key"},
        {"description", "Pulse the emulated SC-3000 reset line through the exact F1/Soft Reset (NMI) path. This requests a Z80 NMI; software decides whether it means pause, soft reset, or another action."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "keyboard_text"},
        {"title", "Type SC-3000 Text"},
        {"description", "Queue text through the SC-3000 SK-1100 matrix. Each key is held for two emulated polls then released for one, so BASIC can scan it reliably. Supports A-Z, 0-9, space, punctuation present on the matrix, Return/newline and double quote."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {{"text", {{"type", "string"}, {"description", "Text to type. Use \\n for Return."}}}}},
            {"required", json::array({"text"})}
        }}
    });

    tools.push_back({
        {"name", "cancel_keyboard_text"},
        {"title", "Cancel SC-3000 Text"},
        {"description", "Cancel the pending synthetic SC-3000 keyboard sequence and release its held key."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}}}
    });

    tools.push_back({
        {"name", "basic_typer_set_text"},
        {"title", "Load BASIC Typer"},
        {"description", "Put text into the BASIC Typer panel, optionally sending it straight away. Same filter as the panel's Send button: characters the SK-1100 cannot type are dropped rather than rejecting the whole text, and CRLF collapses to one Return. For fragments - a whole program belongs on a cassette."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"text", {{"type", "string"}, {"description", "Text to load. Use \\n for Return."}}},
                {"send", {{"type", "boolean"}, {"description", "Queue it immediately as well as loading it. Default false."}}}
            }},
            {"required", json::array({"text"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "basic_typer_clear"},
        {"title", "Clear BASIC Typer"},
        {"description", "Empty the BASIC Typer panel's text box. With cancel_queue, also abort any sequence still being typed and release its held key - the same as cancel_keyboard_text."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"cancel_queue", {{"type", "boolean"}, {"description", "Also cancel a sequence in progress. Default true."}}}
            }},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "basic_typer_status"},
        {"title", "BASIC Typer Status"},
        {"description", "What the BASIC Typer panel holds, whether it is typing, and how far along."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}}}
    });

    tools.push_back({
        {"name", "load_basic_program"},
        {"title", "Load Tokenised BASIC"},
        {"description", "Write a tokenised .bas image into the BASIC program area at TXTBGN and rebuild the array/variable/free pointers, the way a cassette LOAD does. Requires BASIC to be running and initialised. Not for .basic source text, which BASIC has to tokenise itself - type that instead."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"file_path", {{"type", "string"}, {"description", "Absolute path to the .bas image."}}},
                {"pointer_block", {{"type", "integer"}, {"description", "Address of BASIC's five-word pointer block. Omit to follow the GUI setting; pass 0 to scan for it. $8160 is the Level III cartridge, $9954 Disk BASIC - find_basic_blocks reports what a running machine has."}}}
            }},
            {"required", json::array({"file_path"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "save_basic_program"},
        {"title", "Save Tokenised BASIC"},
        {"description", "Write the program currently in memory out as a .bas image - the bytes from TXTBGN to ARYBGN, which is what the ROM's own SAVE puts on tape. Refuses on an empty program the same way it does."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"file_path", {{"type", "string"}, {"description", "Absolute path to write."}}},
                {"pointer_block", {{"type", "integer"}, {"description", "Address of BASIC's pointer block. Omit to follow the GUI setting; pass 0 to scan for it."}}}
            }},
            {"required", json::array({"file_path"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "find_basic_blocks"},
        {"title", "Find BASIC Pointer Block"},
        {"description", "Scan RAM for a BASIC pointer block whose program/array/variable/free/limit words are consistent with each other and carry the terminators BASIC writes. For working out where an unfamiliar BASIC keeps its layout: over 32 KB of a live Disk BASIC it matched in exactly one place, $9954."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}}}
    });

    tools.push_back({
        {"name", "keyboard_key"},
        {"title", "SC-3000 Non-Printable Key"},
        {"description", "Press/release a single SK-1100 keyboard matrix key that keyboard_text can't type - cursor keys, Ins/Del, Home/Clear, Func - not the joystick port. Send press, let at least one frame pass, then release - like controller_button, this cannot hold a key across the two edges itself."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"key", {
                    {"type", "string"},
                    {"description", "Matrix key label."},
                    {"enum", json::array({"Up", "Down", "Left", "Right", "Ins/Del", "Home/Clear", "Func"})}
                }},
                {"action", {
                    {"type", "string"},
                    {"description", "press or release."},
                    {"enum", json::array({"press", "release"})}
                }}
            }},
            {"required", json::array({"key", "action"})}
        }}
    });

    tools.push_back({
        {"name", "controller_button"},
        {"title", "Controller Button"},
        {"description", "Press/release one SC-3000 joystick input for players 1-2: four directions and two fire buttons. The ColecoVision keypad (0-9, *, #, blue, purple) does not exist on this machine and is not accepted - see get_input_state for what can be read back."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"player", {
                    {"type", "integer"},
                    {"description", "Player number 1-2."},
                    {"minimum", 1},
                    {"maximum", 2}
                }},
                {"button", {
                    {"type", "string"},
                    {"description", "Direction, or a fire button by any of its aliases."},
                    {"enum", json::array({"up", "down", "left", "right", "left_button", "right_button", "fire1", "fire2", "button1", "button2", "yellow", "red", "fire"})}
                }},
                {"action", {
                    {"type", "string"},
                    {"description", "Action; press_and_release auto-releases after several frames."},
                    {"enum", json::array({"press", "release", "press_and_release"})}
                }}
            }},
            {"required", json::array({"player", "button", "action"})}
        }}
    });

    tools.push_back({
        {"name", "controller_macro"},
        {"title", "Controller Macro"},
        {"description", "Run a frame-based controller macro. Commands are tap, press, release, and wait; player defaults to 1."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"player", {
                    {"type", "integer"},
                    {"description", "Default player number 1-2."},
                    {"minimum", 1},
                    {"maximum", 2}
                }},
                {"commands", {
                    {"type", "array"},
                    {"description", "Ordered macro commands, e.g. [{\"tap\":\"1\"},{\"wait\":30},{\"press\":\"right\"},{\"wait\":60},{\"release\":\"right\"}]."},
                    {"minItems", 1},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"tap", {
                                {"type", "string"},
                                {"description", "Tap button for one frame."},
                                {"enum", json::array({"up", "down", "left", "right", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "asterisk", "*", "hash", "#", "left_button", "right_button", "yellow", "red", "fire1", "fire2", "blue", "purple"})}
                            }},
                            {"press", {
                                {"type", "string"},
                                {"description", "Press and hold button."},
                                {"enum", json::array({"up", "down", "left", "right", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "asterisk", "*", "hash", "#", "left_button", "right_button", "yellow", "red", "fire1", "fire2", "blue", "purple"})}
                            }},
                            {"release", {
                                {"type", "string"},
                                {"description", "Release button."},
                                {"enum", json::array({"up", "down", "left", "right", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "asterisk", "*", "hash", "#", "left_button", "right_button", "yellow", "red", "fire1", "fire2", "blue", "purple"})}
                            }},
                            {"wait", {
                                {"type", "integer"},
                                {"description", "Frames to wait."},
                                {"minimum", 1},
                                {"maximum", 1000}
                            }},
                            {"player", {
                                {"type", "integer"},
                                {"description", "Player override for this command, 1-2."},
                                {"minimum", 1},
                                {"maximum", 2}
                            }}
                        }},
                        {"additionalProperties", false}
                    }}
                }}
            }},
            {"required", json::array({"commands"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_input_state"},
        {"title", "Get Input State"},
        {"description", "The six joystick bits per player, read back from the SK-1100 matrix: up, down, left, right, button1, button2. These are exactly what controller_button can set."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_keyboard_mode"},
        {"title", "Get Keyboard Mode"},
        {"description", "Whether the SK-1100 keyboard matrix is answering the PPI. With it off the same port reads as the joystick, which is what a cartridge game expects."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "set_keyboard_mode"},
        {"title", "Set Keyboard Mode"},
        {"description", "Turn the SK-1100 keyboard matrix on or off. Turn it off to let a cartridge game read the joystick on the same PPI port."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"enabled", {{"type", "boolean"}, {"description", "true for keyboard, false for joystick."}}}
            }},
            {"required", json::array({"enabled"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "list_sprites"},
        {"title", "List Sprites"},
        {"description", "List hardware sprites, including TMS9918 early-clock/effective X and, when the requested line is the line currently held by the VDP, which four sprites were selected and which fifth sprite caused overflow."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"line", {{"type", "integer"}, {"minimum", 0}, {"maximum", 312},
                          {"description", "Optional raster line. Omit for the current render line. Per-line selection is valid only while the VDP still holds that line."}}},
                {"source", {{"type", "string"}, {"enum", json::array({"live", "rendered"})},
                            {"description", "live reads the current SAT; rendered reports rows actually fetched for the last complete frame."}}}
            }},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_sprite_image"},
        {"title", "Get Sprite Image"},
        {"description", "Capture one hardware sprite as PNG from the live SAT or from rows actually fetched for the last complete frame."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"sprite_index", {
                    {"type", "integer"},
                    {"description", "Sprite index 0-31."}
                }},
                {"source", {{"type", "string"}, {"enum", json::array({"live", "rendered"})},
                            {"description", "live reads current SAT/SPRPAT; rendered uses the timed fetch-latch capture."}}}
            }},
            {"required", json::array({"sprite_index"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_sprite_pipeline"},
        {"title", "Get Current Sprite Pipeline"},
        {"description", "Read the cycle-current TMS9918 sprite scan/fetch pipeline: raster position, active target line, four selected SAT entries, per-field latch validity and fifth-sprite overflow. Intended for instruction or cycle stepping."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}}}
    });

    tools.push_back({
        {"name", "get_sprite_scanline_history"},
        {"title", "Get Sprite Scanline History"},
        {"description", "Read a bounded range from the last completed or rewind-restored frame. Each raster line contains four accepted sprite records, the fifth overflow candidate, full SAT tuples, actual fetched pattern bytes, sprite mode and collision information."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"start_line", {{"type", "integer"}, {"minimum", 0}, {"maximum", 312}, {"description", "First raster line, inclusive. Default 0."}}},
                {"end_line", {{"type", "integer"}, {"minimum", 0}, {"maximum", 312}, {"description", "Last raster line, inclusive. Default is the final NTSC/PAL raster line."}}},
                {"include_empty", {{"type", "boolean"}, {"description", "Include lines with no selected sprite, overflow or collision. Default false."}}}
            }},
            {"additionalProperties", false}
        }}
    });

    // Disassembler tools
    tools.push_back({
        {"name", "debug_run_to_cursor"},
        {"title", "Debug Run To Cursor"},
        {"description", "Continue execution until logical address."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"address", {
                    {"type", "string"},
                    {"description", "Logical hex address, e.g. 'E177'."}
                }}
            }},
            {"required", json::array({"address"})}
        }}
    });

    tools.push_back({
        {"name", "add_disassembler_bookmark"},
        {"title", "Add Disassembler Bookmark"},
        {"description", "Add disassembler bookmark at logical address."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"address", {
                    {"type", "string"},
                    {"description", "Logical hex address, e.g. 'E177'."}
                }},
                {"name", {
                    {"type", "string"},
                    {"description", "Bookmark name; optional, auto-generated if omitted."}
                }}
            }},
            {"required", json::array({"address"})}
        }}
    });

    tools.push_back({
        {"name", "remove_disassembler_bookmark"},
        {"title", "Remove Disassembler Bookmark"},
        {"description", "Remove disassembler bookmark at logical address."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"address", {
                    {"type", "string"},
                    {"description", "Logical hex address, e.g. 'E177'."}
                }}
            }},
            {"required", json::array({"address"})}
        }}
    });

    tools.push_back({
        {"name", "add_symbol"},
        {"title", "Add Symbol"},
        {"description", "Add disassembler symbol/label at bank:logical address."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"bank", {
                    {"type", "string"},
                    {"description", "Bank hex byte, e.g. '00'."}
                }},
                {"address", {
                    {"type", "string"},
                    {"description", "Logical hex address, e.g. 'E177'."}
                }},
                {"name", {
                    {"type", "string"},
                    {"description", "Symbol/label name."}
                }}
            }},
            {"required", json::array({"bank", "address", "name"})}
        }}
    });

    tools.push_back({
        {"name", "remove_symbol"},
        {"title", "Remove Symbol"},
        {"description", "Remove disassembler symbol/label at bank:logical address."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"bank", {
                    {"type", "string"},
                    {"description", "Bank hex byte, e.g. '00'."}
                }},
                {"address", {
                    {"type", "string"},
                    {"description", "Logical hex address, e.g. 'E177'."}
                }}
            }},
            {"required", json::array({"bank", "address"})}
        }}
    });

    // Memory editor tools
    tools.push_back({
        {"name", "select_memory_range"},
        {"title", "Select Memory Range"},
        {"description", "Select memory editor range by area and 0-based offsets."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"area", {
                    {"type", "integer"},
                    {"description", "Memory area ID from list_memory_areas."}
                }},
                {"start_address", {
                    {"type", "string"},
                    {"description", "Start 0-based hex offset, e.g. '0100'."}
                }},
                {"end_address", {
                    {"type", "string"},
                    {"description", "End 0-based hex offset, e.g. '01FF'."}
                }}
            }},
            {"required", json::array({"area", "start_address", "end_address"})}
        }}
    });

    tools.push_back({
        {"name", "set_memory_selection_value"},
        {"title", "Set Memory Selection Value"},
        {"description", "Fill current memory selection with byte value; use select_memory_range first."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"area", {
                    {"type", "integer"},
                    {"description", "Memory area ID from list_memory_areas."}
                }},
                {"value", {
                    {"type", "string"},
                    {"description", "Byte hex value, e.g. 'FF' or '00'."}
                }}
            }},
            {"required", json::array({"area", "value"})}
        }}
    });

    tools.push_back({
        {"name", "add_memory_bookmark"},
        {"title", "Add Memory Bookmark"},
        {"description", "Add memory bookmark at area offset."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"area", {
                    {"type", "integer"},
                    {"description", "Memory area ID from list_memory_areas."}
                }},
                {"address", {
                    {"type", "string"},
                    {"description", "0-based hex offset in physical memory area."}
                }},
                {"name", {
                    {"type", "string"},
                    {"description", "Bookmark name; optional."}
                }}
            }},
            {"required", json::array({"area", "address"})}
        }}
    });

    tools.push_back({
        {"name", "remove_memory_bookmark"},
        {"title", "Remove Memory Bookmark"},
        {"description", "Remove memory bookmark at area offset."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"area", {
                    {"type", "integer"},
                    {"description", "Memory area ID from list_memory_areas."}
                }},
                {"address", {
                    {"type", "string"},
                    {"description", "0-based hex offset in physical memory area."}
                }}
            }},
            {"required", json::array({"area", "address"})}
        }}
    });

    tools.push_back({
        {"name", "watch_add"},
        {"title", "Add Watch"},
        {"description", "Add a live monitor entry (multi-value watch). Read with watch_list; each entry keeps current and previous value so you can see e.g. lives dropping from 3 to 2 between polls."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"area", {
                    {"type", "integer"},
                    {"description", "Memory area ID from list_memory_areas."}
                }},
                {"address", {
                    {"type", "string"},
                    {"description", "0-based hex offset in physical memory area."}
                }},
                {"data_type", {
                    {"type", "string"},
                    {"description", "unsigned/signed/hex are 1 or 2 raw bytes (little-endian); text reads size raw bytes as ASCII."},
                    {"enum", {"unsigned", "signed", "hex", "text"}}
                }},
                {"size", {
                    {"type", "integer"},
                    {"description", "Bytes: 1 or 2 for unsigned/signed/hex; 1-64 for text."}
                }},
                {"label", {
                    {"type", "string"},
                    {"description", "Free-form name, e.g. \"lives\" or \"energy\"; optional."}
                }}
            }},
            {"required", json::array({"area", "address", "data_type", "size"})}
        }}
    });

    tools.push_back({
        {"name", "watch_remove"},
        {"title", "Remove Watch"},
        {"description", "Remove a watch entry by id. If it is used by the current trigger condition, the condition is cleared too."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"id", {{"type", "integer"}, {"description", "Watch id returned by watch_add."}}}
            }},
            {"required", json::array({"id"})}
        }}
    });

    tools.push_back({
        {"name", "watch_freeze"},
        {"title", "Freeze Watch"},
        {"description", "Pin a watch's value: rewritten to memory every frame until watch_unfreeze. Number for unsigned/signed/hex (hex also accepts a \"0x..\" string); string for text."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"id", {{"type", "integer"}, {"description", "Watch id returned by watch_add."}}},
                {"value", {{"description", "Value to hold; number or hex/decimal string for numeric types, plain string for text."}}}
            }},
            {"required", json::array({"id", "value"})}
        }}
    });

    tools.push_back({
        {"name", "watch_unfreeze"},
        {"title", "Unfreeze Watch"},
        {"description", "Stop rewriting a watch's value every frame."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"id", {{"type", "integer"}, {"description", "Watch id returned by watch_add."}}}
            }},
            {"required", json::array({"id"})}
        }}
    });

    tools.push_back({
        {"name", "watch_set_condition"},
        {"title", "Set Watch Trigger Condition"},
        {"description", "Define one AND/OR condition over existing watches; the emulator auto-pauses the first frame it becomes true. Replaces any previous condition. Constant ops (==, !=, >, <, >=, <=) compare against value and need it; text watches only support == and != of those. \"changed\"/\"increased\"/\"decreased\" instead compare this tick's value against last tick's - e.g. lives dropping, regardless of the exact before/after numbers - and take no value (\"increased\"/\"decreased\" are numeric-only, \"changed\" also works on text)."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"conditions", {
                    {"type", "array"},
                    {"description", "List of {id, op, value?}, one per watch to test. value is required for ==, !=, >, <, >=, <= and ignored/omitted for changed, increased, decreased."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"id", {{"type", "integer"}, {"description", "Watch id."}}},
                            {"op", {{"type", "string"}, {"enum", {"==", "!=", ">", "<", ">=", "<=", "changed", "increased", "decreased"}}}},
                            {"value", {{"description", "Comparison value; required for ==, !=, >, <, >=, <=, omit for changed/increased/decreased."}}}
                        }},
                        {"required", json::array({"id", "op"})}
                    }}
                }},
                {"mode", {
                    {"type", "string"},
                    {"description", "Combine all conditions with AND or OR."},
                    {"enum", {"AND", "OR"}}
                }}
            }},
            {"required", json::array({"conditions", "mode"})}
        }}
    });

    tools.push_back({
        {"name", "watch_clear_condition"},
        {"title", "Clear Watch Trigger Condition"},
        {"description", "Remove the current trigger condition, if any."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "watch_condition_status"},
        {"title", "Get Watch Trigger Condition Status"},
        {"description", "Report whether a trigger condition is active and whether it has fired (and paused the emulator) yet."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "list_disassembler_bookmarks"},
        {"title", "List Disassembler Bookmarks"},
        {"description", "List disassembler bookmarks."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "list_symbols"},
        {"title", "List Symbols"},
        {"description", "List disassembler symbols/labels."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "lookup_symbol_by_name"},
        {"title", "Lookup Symbol by Name"},
        {"description", "Find exact symbol name; return all matches."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"name", {
                    {"type", "string"},
                    {"description", "Exact symbol name."}
                }}
            }},
            {"required", json::array({"name"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "lookup_symbol_at_address"},
        {"title", "Lookup Symbol at Address"},
        {"description", "Find symbol at bank/address."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"bank", {
                    {"type", "string"},
                    {"description", "Hex bank, 00-FF."}
                }},
                {"address", {
                    {"type", "string"},
                    {"description", "Hex address, 0000-FFFF."}
                }}
            }},
            {"required", json::array({"bank", "address"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_call_stack"},
        {"title", "Get Call Stack"},
        {"description", "List current call stack/subroutine hierarchy."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "list_memory_bookmarks"},
        {"title", "List Memory Bookmarks"},
        {"description", "List bookmarks for memory area."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"area", {
                    {"type", "integer"},
                    {"description", "Memory area ID from list_memory_areas."}
                }}
            }},
            {"required", json::array({"area"})}
        }}
    });

    tools.push_back({
        {"name", "watch_list"},
        {"title", "List Watches"},
        {"description", "Read every watch entry's current value in one call - the multi-value monitor (lives, energy, score, etc. together)."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_memory_selection"},
        {"title", "Get Memory Selection"},
        {"description", "Read current memory selection range for area."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"area", {
                    {"type", "integer"},
                    {"description", "Memory area ID from list_memory_areas."}
                }}
            }},
            {"required", json::array({"area"})}
        }}
    });

    tools.push_back({
        {"name", "memory_search_capture"},
        {"title", "Memory Search Capture"},
        {"description", "Snapshot memory area for later value-change search."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"area", {
                    {"type", "integer"},
                    {"description", "Memory area ID from list_memory_areas."}
                }}
            }},
            {"required", json::array({"area"})}
        }}
    });

    tools.push_back({
        {"name", "memory_search"},
        {"title", "Memory Search"},
        {"description", "Search memory values by comparison against snapshot, constant value, or address. Set narrow_previous=true to intersect with the previous memory_search's results instead of rescanning the whole area - the progressive Cheat Engine-style workflow: first call with narrow_previous=false (or omitted) after memory_search_capture, then repeat calls with narrow_previous=true after each further gameplay change to keep filtering the same candidate set down."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"area", {
                    {"type", "integer"},
                    {"description", "Memory area ID from list_memory_areas"}
                }},
                {"operator", {
                    {"type", "string"},
                    {"description", "Comparison operator: <, >, ==, !=, <=, >=."},
                    {"enum", json::array({"<", ">", "==", "!=", "<=", ">="})}
                }},
                {"compare_type", {
                    {"type", "string"},
                    {"description", "Compare against previous snapshot, constant value, or value at address. With narrow_previous=true, \"previous\" means each surviving candidate's own value from the last search, not the original capture."},
                    {"enum", json::array({"previous", "value", "address"})}
                }},
                {"compare_value", {
                    {"type", "integer"},
                    {"description", "Search value or address used for compare_type value/address."}
                }},
                {"data_type", {
                    {"type", "string"},
                    {"description", "Value type: unsigned default, signed, or hex."},
                    {"enum", json::array({"unsigned", "signed", "hex"})}
                }},
                {"narrow_previous", {
                    {"type", "boolean"},
                    {"description", "true = filter the previous memory_search's own results instead of rescanning the whole area; false (default) = a fresh full-area scan against the memory_search_capture snapshot. No-op (falls back to full scan) if there is no previous result set yet."}
                }}
            }},
            {"required", json::array({"area", "operator", "compare_type"})}
        }}
    });

    tools.push_back({
        {"name", "memory_find_bytes"},
        {"title", "Find Byte Sequence in Memory"},
        {"description", "Find consecutive hex byte sequence in memory; return addresses."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"area", {
                    {"type", "integer"},
                    {"description", "Memory area ID from list_memory_areas."}
                }},
                {"hex_bytes", {
                    {"type", "string"},
                    {"description", "Hex byte pairs to find, e.g. '04E5FF32' (spaces optional)"}
                }}
            }},
            {"required", json::array({"area", "hex_bytes"})}
        }}
    });

    tools.push_back({
        {"name", "memory_find_bytes_advanced"},
        {"title", "Find Byte Sequence in Memory (Wildcards)"},
        {"description", "Find a byte sequence in memory with wildcards, for locating a routine that was relocated (opcodes identical, an embedded address operand moved). '\?\?' matches exactly one byte; '*' matches a run of 0..wildcard_limit bytes. Returns each match's address and actual matched length (varies when the pattern has '*')."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"area", {
                    {"type", "integer"},
                    {"description", "Memory area ID from list_memory_areas."}
                }},
                {"pattern", {
                    {"type", "string"},
                    {"description", "Hex byte pairs with wildcards, e.g. '3A 5B ?? ?? 1F' or '3A 5B * 1F' (spaces optional between hex pairs)."}
                }},
                {"wildcard_limit", {
                    {"type", "integer"},
                    {"description", "Max bytes a single '*' may consume, 0-64. Default 4 (a 16-bit address operand)."}
                }}
            }},
            {"required", json::array({"area", "pattern"})}
        }}
    });

    // Tracing tools
    tools.push_back({
        {"name", "get_trace_log"},
        {"title", "Get Trace Log"},
        {"description", "Read back what the trace recorded, newest last. Each entry carries the sequence number, the T-state clock, the logical PC and the physical bank it was in, and - for anything the VDP published - the raster line, dot and slot the beam was on. Reads are windowed: the ring can hold far more than one reply should carry."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"start", {
                    {"type", "integer"},
                    {"description", "Start index; 0 oldest, default 0."},
                    {"minimum", 0}
                }},
                {"count", {
                    {"type", "integer"},
                    {"description", "How many entries to return, counting back from the newest. Default 100. The ceiling is deliberately well below the ring's depth: an unfiltered CPU trace is thousands of events per frame, and a reply carrying all of them helps nobody."},
                    {"minimum", 1},
                    {"maximum", 20000}
                }}
            }}
        }}
    });

    tools.push_back({
        {"name", "set_trace_log"},
        {"title", "Set Trace Log"},
        {"description", "Record everything of the chosen kinds, unfiltered. This is the debug rule system with its filters removed rather than a second mechanism: each kind becomes one rule over the whole address range that only logs and never pauses, so a trace and your own rules share one ring. Name the kinds you want; with none named, all of them are armed."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"enabled", {
                    {"type", "boolean"},
                    {"description", "true starts tracing, false stops."}
                }},
                {"flags", {
                    {"type", "integer"},
                    {"description", "Legacy bitmask, kept working. Prefer the named booleans below, which say what they mean."},
                    {"minimum", 0},
                    {"maximum", 255}
                }},
                {"clear", {{"type", "boolean"}, {"description", "Empty the ring first, so a measurement starts from nothing."}}},
                {"capacity", {{"type", "integer"}, {"description", "Events the ring holds. The default 4096 suits rules, which are selective; an unfiltered CPU trace is about sixty thousand events a frame, so raise it. Changing it clears the ring."}, {"minimum", 64}, {"maximum", 1000000}}},
                {"cpu", {{"type", "boolean"}, {"description", "Every instruction boundary."}}},
                {"memory", {{"type", "boolean"}, {"description", "CPU memory reads and writes."}}},
                {"vram", {{"type", "boolean"}}},
                {"vdp_register", {{"type", "boolean"}}},
                {"io_port", {{"type", "boolean"}}},
                {"ppi", {{"type", "boolean"}}},
                {"fdc", {{"type", "boolean"}}},
                {"tape", {{"type", "boolean"}}},
                {"psg", {{"type", "boolean"}, {"description", "SN76489."}}},
                {"ay", {{"type", "boolean"}, {"description", "AY-3-8910 / YM2149 expansion."}}},
                {"video_timing", {{"type", "boolean"}}},
                {"device_state", {{"type", "boolean"}}}
            }},
            {"required", json::array({"enabled"})}
        }}
    });

}

#endif /* GEARSF7000_ENABLE_MCP */
