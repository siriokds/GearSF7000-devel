#include "mcp_config.h"

#if GEARSF7000_ENABLE_MCP

/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2021  Ignacio Sanchez

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

#include "mcp_server.h"
#include "../../../src/build_info.h"
#include "../config.h"
#include "../emu.h"
#include <sstream>
#include <iomanip>
#include <fstream>
#include <limits>
#include <cstdlib>
#include "../rewind.h"

bool g_mcp_router_enabled = false;

template<typename T>
static bool parse_mcp_hex_with_prefix(const std::string& hex_str, T* result)
{
    const char* str = hex_str.c_str();
    size_t len = hex_str.length();

    if (len >= 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X'))
    {
        str += 2;
        len -= 2;
    }
    else if (len >= 1 && str[0] == '$')
    {
        str += 1;
        len -= 1;
    }

    if (len == 0)
        return false;

    char* end = NULL;
    unsigned long value = strtoul(str, &end, 16);

    if (!end || *end != '\0')
        return false;
    if (value > (unsigned long)std::numeric_limits<T>::max())
        return false;

    *result = (T)value;
    return true;
}

void McpServer::ReaderLoop()
{
    while (m_running.load())
    {
        std::string line;
        bool received = false;
        McpRequestToken request_token = MCP_REQUEST_TOKEN_NONE;

        // Parsing a request is itself network-driven work: a malformed header
        // or an oversized body must cost the client its request, never take the
        // whole reader thread down with it.
        try
        {
            received = m_transport->recv(line, request_token);
        }
        catch (const std::exception& exception)
        {
            Error("[MCP] Dropping connection after transport exception: %s", exception.what());
            m_transport->abort_request(request_token);
            continue;
        }
        catch (...)
        {
            Error("[MCP] Dropping connection after unknown transport exception");
            m_transport->abort_request(request_token);
            continue;
        }

        if (received)
        {
            if (!line.empty())
            {
                // Network input must not be able to unwind through std::thread
                // (which would otherwise call std::terminate and kill the UI).
                try
                {
                    m_readerRequestToken = request_token;
                    HandleLine(line, request_token);
                    m_readerRequestToken = MCP_REQUEST_TOKEN_NONE;
                }
                catch (const std::exception& exception)
                {
                    Error("[MCP] Request rejected after exception: %s", exception.what());
                    m_transport->reject_notification(request_token);
                    m_readerRequestToken = MCP_REQUEST_TOKEN_NONE;
                }
                catch (...)
                {
                    Error("[MCP] Request rejected after unknown exception");
                    m_transport->reject_notification(request_token);
                    m_readerRequestToken = MCP_REQUEST_TOKEN_NONE;
                }
            }
        }
        else
        {
            m_running.store(false);
            m_responseQueue.Stop();
            break;
        }
    }
}

void McpServer::Run()
{
    while (m_running.load())
    {
        DebugResponse* resp = m_responseQueue.WaitAndPop();
        if (resp == NULL)
            break;

        try
        {
            if (resp->isError)
            {
                SendError(resp->requestId, resp->errorCode, resp->errorMessage,
                          json::object(), resp->requestToken);
            }
            else
            {
                json mcpResult;
                mcpResult["content"] = json::array();

                if (resp->result.contains("__mcp_image") && resp->result["__mcp_image"] == true)
                {
                    mcpResult["content"].push_back({
                        {"type", "image"},
                        {"data", resp->result["data"]},
                        {"mimeType", resp->result["mimeType"]}
                    });
                }
                else
                {
                    std::ostringstream result_ss;
                    result_ss << resp->result.dump(2, ' ', false, json::error_handler_t::replace);

                    mcpResult["content"].push_back({
                        {"type", "text"},
                        {"text", result_ss.str()}
                    });
                }

                json response;
                response["jsonrpc"] = "2.0";
                response["id"] = resp->requestId;
                mcpResult["isError"] = resp->isToolError;
                response["result"] = mcpResult;

                SendResponse(response, resp->requestToken);
            }
        }
        catch (const std::exception& exception)
        {
            // The client is waiting on a response that will now never be built.
            // Leaving it open would stop the accept loop for good.
            Error("[MCP] Response delivery failed: %s", exception.what());
            m_transport->abort_request(resp->requestToken);
        }
        catch (...)
        {
            Error("[MCP] Response delivery failed with an unknown exception");
            m_transport->abort_request(resp->requestToken);
        }

        SafeDelete(resp);
        m_commandQueue.Complete();
    }
}

void McpServer::HandleLine(const std::string& line, McpRequestToken request_token)
{
    json request;

    if (!json::accept(line))
    {
        if (!m_transport->validate_protocol_version("", request_token))
            return;
        SendError(json(), MCP_ERROR_PARSE, "Parse error: Invalid JSON");
        return;
    }

    request = json::parse(line);

    if (!request.is_object())
    {
        if (!m_transport->validate_protocol_version("", request_token))
            return;
        SendError(json(), MCP_ERROR_INVALID_REQUEST, "Invalid Request: expected an object");
        return;
    }

    std::string method;
    if (request.contains("method") && request["method"].is_string())
        method = request["method"];

    if (!m_transport->validate_protocol_version(method, request_token))
        return;

    bool is_notification = !request.contains("id");

    const auto reject_or_send_error = [this, is_notification, request_token](const json& id, int code, const std::string& message)
    {
        if (is_notification)
            m_transport->reject_notification(request_token);
        else
            SendError(id, code, message);
    };

    if (request.contains("id") && !request["id"].is_string() &&
        !request["id"].is_number_integer() && !request["id"].is_number_unsigned())
    {
        reject_or_send_error(json(), MCP_ERROR_INVALID_REQUEST, "Invalid Request: id must be a string or integer");
        return;
    }

    json request_id = request.contains("id") ? request["id"] : json();

    if (!request.contains("jsonrpc") || request["jsonrpc"] != "2.0")
    {
        reject_or_send_error(json(), MCP_ERROR_INVALID_REQUEST, "Invalid Request: missing or invalid jsonrpc version");
        return;
    }

    if (!request.contains("method") || !request["method"].is_string())
    {
        reject_or_send_error(json(), MCP_ERROR_INVALID_REQUEST, "Invalid Request: missing method");
        return;
    }

    method = request["method"];

    if (request.contains("params") && !request["params"].is_object())
    {
        reject_or_send_error(request_id, MCP_ERROR_INVALID_PARAMS, "Invalid params: expected an object");
        return;
    }

    if (method == "initialize" && is_notification)
    {
        reject_or_send_error(json(), MCP_ERROR_INVALID_REQUEST, "Initialize must be a request");
        return;
    }

    if (!m_initialized && method != "initialize" && method != "ping")
    {
        reject_or_send_error(request_id, MCP_ERROR_INVALID_REQUEST, "Server not initialized");
        return;
    }

    if (is_notification)
    {
        m_transport->acknowledge_notification(request_token);
        return;
    }

    if (method == "initialize")
    {
        HandleInitialize(request);
    }
    else if (method == "ping")
    {
        json response;
        response["jsonrpc"] = "2.0";
        response["id"] = request_id;
        response["result"] = json::object();
        SendResponse(response);
    }
    else if (method == "tools/list")
    {
        HandleToolsList(request);
    }
    else if (method == "tools/call")
    {
        HandleToolsCall(request);
    }
    else if (method == "resources/list")
    {
        HandleResourcesList(request);
    }
    else if (method == "resources/templates/list")
    {
        HandleResourceTemplatesList(request);
    }
    else if (method == "resources/read")
    {
        HandleResourcesRead(request);
    }
    else
    {
        SendError(request_id, MCP_ERROR_METHOD_NOT_FOUND, "Method not found: " + method);
    }
}

static bool ValidateInitializeParams(const json& params, std::string& error)
{
    if (!params.contains("protocolVersion"))
    {
        error = "Missing required parameter 'protocolVersion'";
        return false;
    }
    if (!params["protocolVersion"].is_string())
    {
        error = "Parameter 'protocolVersion' must be a string";
        return false;
    }

    if (!params.contains("capabilities"))
    {
        error = "Missing required parameter 'capabilities'";
        return false;
    }
    if (!params["capabilities"].is_object())
    {
        error = "Parameter 'capabilities' must be an object";
        return false;
    }

    if (!params.contains("clientInfo"))
    {
        error = "Missing required parameter 'clientInfo'";
        return false;
    }
    if (!params["clientInfo"].is_object())
    {
        error = "Parameter 'clientInfo' must be an object";
        return false;
    }

    const json& client_info = params["clientInfo"];
    if (!client_info.contains("name"))
    {
        error = "Missing required parameter 'clientInfo.name'";
        return false;
    }
    if (!client_info["name"].is_string())
    {
        error = "Parameter 'clientInfo.name' must be a string";
        return false;
    }
    if (!client_info.contains("version"))
    {
        error = "Missing required parameter 'clientInfo.version'";
        return false;
    }
    if (!client_info["version"].is_string())
    {
        error = "Parameter 'clientInfo.version' must be a string";
        return false;
    }

    return true;
}

void McpServer::HandleInitialize(const json& request)
{
    const json& id = request["id"];

    if (!request.contains("params"))
    {
        SendError(id, MCP_ERROR_INVALID_PARAMS, "Invalid params: missing params");
        return;
    }

    std::string validation_error;
    if (!ValidateInitializeParams(request["params"], validation_error))
    {
        SendError(id, MCP_ERROR_INVALID_PARAMS, "Invalid params: " + validation_error);
        return;
    }

    std::string protocolVersion = MCP_PROTOCOL_VERSION;

    json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = {
        {"protocolVersion", protocolVersion},
        {"capabilities", {
            {"tools", json::object()},
            {"resources", json::object()}
        }},
        {"serverInfo", {
            {"name", "gearsf7000-mcp-server"},
            {"title", GEARSF7000_TITLE " MCP Server"},
            {"description", "Debug/control " GEARSF7000_TITLE ": SC-3000/SF-7000 execution, Z80, memory, TMS9918 VDP, SN76489 PSG, cassette and save states."},
            {"version", build_info_version()},
            // This identifies the binary currently installed in the app bundle.
            {"compiled", build_info_timestamp()}
        }}
    };

    response["result"]["instructions"] =
        "Use this server for SC-3000 and SF-7000 debugging: execution, Z80 inspection, memory, "
        "breakpoints, TMS9918 VDP, SN76489 PSG, cassette/media information and save states.";

    if (g_mcp_router_enabled)
    {
        response["result"]["instructions"] =
            response["result"]["instructions"].get<std::string>() +
            " The tool router is enabled. Common tools are directly callable. Advanced tools are routed: "
            "call search_tools to find a tool, call get_tool_info to obtain its exact input schema, then "
            "call execute_tool with the returned tool name and arguments. Never call a routed tool directly.";
    }

    m_initialized = true;
    m_transport->set_protocol_version(protocolVersion);
    SendResponse(response);
}

json McpServer::BuildToolList()
{
    json tools = json::array();

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
                {"memory_ranges", {
                    {"type", "array"}, {"maxItems", 16},
                    {"description", "Physical memory areas from list_memory_areas. At most 4096 bytes per range and 16384 bytes total."},
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

    for (json::iterator it = tools.begin(); it != tools.end(); ++it)
    {
        if (it->contains("inputSchema") && (*it)["inputSchema"].is_object() &&
            !(*it)["inputSchema"].contains("additionalProperties"))
        {
            (*it)["inputSchema"]["additionalProperties"] = false;
        }
    }

    return tools;
}

void McpServer::HandleToolsList(const json& request)
{
    const json& id = request["id"];

    json tools = BuildToolList();

    m_toolRegistry.SetTools(tools);

    if (g_mcp_router_enabled)
    {
        json visibleTools = m_toolRegistry.GetDirectTools();
        AddRouterTools(visibleTools);
        tools = visibleTools;
    }

    json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = {
        {"tools", tools}
    };

    SendResponse(response);
}

void McpServer::EnsureToolRegistry()
{
    if (!m_toolRegistry.IsEmpty())
        return;

    m_toolRegistry.SetTools(BuildToolList());
}


void McpServer::AddRouterTools(json& tools)
{
    tools.push_back({
        {"name", "list_tool_categories"},
        {"title", "List Tool Categories"},
        {"description", "List routed MCP tool categories with descriptions and tool counts. Use this first to discover advanced emulator/debugger tools."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_category_tools"},
        {"title", "Get Category Tools"},
        {"description", "List routed tools in a category with compact descriptions. Use category names returned by list_tool_categories, then call get_tool_info for one tool's input schema."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"category", {{"type", "string"}}}
            }},
            {"required", json::array({"category"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_tool_info"},
        {"title", "Get Tool Info"},
        {"description", "Return one MCP tool's title, description, category, direct/routed status, and real input schema. Use this after search_tools or get_category_tools before execute_tool."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"name", {{"type", "string"}}}
            }},
            {"required", json::array({"name"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "search_tools"},
        {"title", "Search Tools"},
        {"description", "Search direct and routed MCP tools by keyword, category, title, description, and aliases. Use this when you know what you want to do but not the tool name."},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"query", {{"type", "string"}}}
            }},
            {"required", json::array({"query"})},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "execute_tool"},
        {"title", "Execute Routed Tool"},
        {"description", "Execute a routed MCP tool by name. First use search_tools or get_category_tools to discover the tool, then call get_tool_info to obtain its exact input schema."},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", true}}},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"name", {{"type", "string"}}},
                {"arguments", {
                    {"type", "object"},
                    {"additionalProperties", true}
                }}
            }},
            {"required", json::array({"name"})},
            {"additionalProperties", false}
        }}
    });

}

json McpServer::HandleRouterListCategories()
{
    EnsureToolRegistry();

    json stats = m_toolRegistry.GetStats();
    stats["categories"] = m_toolRegistry.GetCategories();

    return stats;
}

json McpServer::HandleRouterGetCategoryTools(const json& arguments)
{
    EnsureToolRegistry();

    std::string category = arguments.value("category", "");

    if (!m_toolRegistry.HasCategory(category))
    {
        return {
            {"error", "Unknown category"},
            {"category", category},
            {"available_categories", m_toolRegistry.GetCategoryNames()}
        };
    }

    return {
        {"category", category},
        {"title", m_toolRegistry.GetCategoryTitle(category)},
        {"description", m_toolRegistry.GetCategoryDescription(category)},
        {"tool_count", m_toolRegistry.GetCategoryToolCount(category)},
        {"tools", m_toolRegistry.GetToolsInCategory(category)}
    };
}

json McpServer::HandleRouterSearchTools(const json& arguments)
{
    EnsureToolRegistry();

    std::string query = arguments.value("query", "");
    json tools = m_toolRegistry.SearchTools(query);

    return {
        {"query", query},
        {"count", tools.size()},
        {"limit", m_toolRegistry.GetSearchToolLimit()},
        {"matches", tools}
    };
}

json McpServer::HandleRouterGetToolInfo(const json& arguments)
{
    EnsureToolRegistry();

    std::string tool_name = arguments.value("name", "");
    json tool = m_toolRegistry.GetToolInfo(tool_name);

    if (tool.empty())
    {
        return {
            {"error", "Unknown tool"},
            {"name", tool_name},
            {"hint", "Use search_tools or get_category_tools to discover available tool names."}
        };
    }

    return tool;
}

void McpServer::SendToolResult(const json& id, const json& result)
{
    json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = {
        {"content", json::array({
            {
                {"type", "text"},
                {"text", result.dump(2, ' ', false, json::error_handler_t::replace)}
            }
        })}
    };
    response["result"]["isError"] = result.contains("error");

    SendResponse(response);
}

void McpServer::HandleToolsCall(const json& request)
{
    const json& id = request["id"];

    if (!request.contains("params") || !request["params"].contains("name") || !request["params"]["name"].is_string())
    {
        SendError(id, MCP_ERROR_INVALID_PARAMS, "Invalid params: missing tool name");
        return;
    }

    std::string toolName = request["params"]["name"];
    if (request["params"].contains("arguments") && !request["params"]["arguments"].is_object())
    {
        SendError(id, MCP_ERROR_INVALID_PARAMS, "Invalid params: arguments must be an object");
        return;
    }

    json arguments = request["params"].contains("arguments") ? request["params"]["arguments"] : json::object();

    EnsureToolRegistry();

    if (g_mcp_router_enabled && m_toolRegistry.IsRouterTool(toolName, "list_tool_categories"))
    {
        if (!arguments.empty())
        {
            SendError(id, MCP_ERROR_INVALID_PARAMS, "Invalid params: list_tool_categories takes no arguments");
            return;
        }
        SendToolResult(id, HandleRouterListCategories());
        return;
    }

    if (g_mcp_router_enabled && m_toolRegistry.IsRouterTool(toolName, "get_category_tools"))
    {
        if (arguments.size() != 1 || !arguments.contains("category") || !arguments["category"].is_string())
        {
            SendError(id, MCP_ERROR_INVALID_PARAMS, "Invalid params: category must be a string");
            return;
        }
        SendToolResult(id, HandleRouterGetCategoryTools(arguments));
        return;
    }

    if (g_mcp_router_enabled && m_toolRegistry.IsRouterTool(toolName, "get_tool_info"))
    {
        if (arguments.size() != 1 || !arguments.contains("name") || !arguments["name"].is_string())
        {
            SendError(id, MCP_ERROR_INVALID_PARAMS, "Invalid params: name must be a string");
            return;
        }
        SendToolResult(id, HandleRouterGetToolInfo(arguments));
        return;
    }

    if (g_mcp_router_enabled && m_toolRegistry.IsRouterTool(toolName, "search_tools"))
    {
        if (arguments.size() != 1 || !arguments.contains("query") || !arguments["query"].is_string())
        {
            SendError(id, MCP_ERROR_INVALID_PARAMS, "Invalid params: query must be a string");
            return;
        }
        SendToolResult(id, HandleRouterSearchTools(arguments));
        return;
    }

    if (g_mcp_router_enabled && m_toolRegistry.IsRouterTool(toolName, "execute_tool"))
    {
        if (arguments.size() > 2 || !arguments.contains("name") || !arguments["name"].is_string() ||
            (arguments.size() == 2 && !arguments.contains("arguments")))
        {
            SendError(id, MCP_ERROR_INVALID_PARAMS, "Invalid params: execute_tool accepts only name and arguments");
            return;
        }

        toolName = arguments["name"].get<std::string>();

        if (!m_toolRegistry.HasTool(toolName))
        {
            SendError(id, MCP_ERROR_INVALID_PARAMS, "Invalid params: unknown routed tool '" + toolName + "'");
            return;
        }

        if (arguments.contains("arguments") && !arguments["arguments"].is_object())
        {
            SendError(id, MCP_ERROR_INVALID_PARAMS, "Invalid params: routed arguments must be an object");
            return;
        }

        if (arguments.contains("arguments"))
            arguments = arguments["arguments"];
        else
            arguments = json::object();
    }

    std::string validation_error;
    if (!m_toolRegistry.ValidateArguments(toolName, arguments, validation_error))
    {
        SendError(id, MCP_ERROR_INVALID_PARAMS, "Invalid params: " + validation_error);
        return;
    }

    DebugCommand* cmd = new DebugCommand();
    cmd->requestId = id;
    cmd->toolName = toolName;
    cmd->arguments = arguments;
    cmd->requestToken = m_readerRequestToken;
    if (!m_commandQueue.Push(cmd))
    {
        SafeDelete(cmd);
        SendError(id, MCP_ERROR_INTERNAL, "Server busy");
    }
}

static int GetBreakpointTypeFromString(const std::string& memory_area)
{
    if (memory_area == "vram") return 1;
    return 0;
}

static bool IsBreakpointMemoryAreaValid(const std::string& memory_area)
{
    return memory_area == "rom_ram" || memory_area == "vram";
}

json McpServer::ExecuteCommand(const std::string& toolName, const json& arguments)
{
    // Normalize tool name: VS Code converts underscores to dots
    std::string normalizedTool = toolName;
    size_t pos = 0;
    while ((pos = normalizedTool.find('.', pos)) != std::string::npos) {
        normalizedTool[pos] = '_';
        pos++;
    }

    // Execution control
    if (normalizedTool == "debug_pause")
    {
        m_debugAdapter.Pause();
        return {{"success", true}};
    }
    if (normalizedTool == "debug_continue")
    {
        m_debugAdapter.Resume();
        return {{"success", true}};
    }
    if (normalizedTool == "debug_step_into")
    {
        m_debugAdapter.StepInto();
        return {{"success", true}};
    }
    if (normalizedTool == "debug_step_over")
    {
        m_debugAdapter.StepOver();
        return {{"success", true}};
    }
    if (normalizedTool == "debug_step_out")
    {
        return {{"error", "Step Out is not available until the CPU-independent call stack is enabled"}};
    }
    if (normalizedTool == "debug_step_frame")
    {
        int frames = arguments.value("frames", 1);

        if (frames < 1 || frames > 1000)
            return {{"error", "Invalid frames value (must be 1-1000)"}};

        // mode was advertised in the schema and ignored by the dispatch, so
        // the tool promised a behaviour it did not have. It is honoured now.
        const std::string mode = arguments.value("mode", "async");
        if (mode == "sync")
            return m_debugAdapter.StepFrameSync(frames);

        m_debugAdapter.StepFrame(frames);
        return {{"success", true}, {"mode", "async"}, {"pending", true}, {"frames", frames}};
    }
    if (normalizedTool == "debug_reset")
    {
        const bool paused = arguments.value("paused", false);
        m_debugAdapter.Reset(paused);
        return {{"success", true}, {"paused", paused}};
    }
    if (normalizedTool == "debug_get_status")
    {
        return m_debugAdapter.GetDebugStatus();
    }
    if (normalizedTool == "get_build_info")
    {
        return {
            {"version", build_info_version()},
            {"compiled", build_info_timestamp()},
            {"summary", build_info_full()},
            // What is compiled in decides which tools exist at all, so clients
            // can distinguish "not built" from "not supported".
            {"options", {
                {"mcp", true},
#if GEARSF7000_ENABLE_AY
                {"ay_expansion", true}
#else
                {"ay_expansion", false}
#endif
            }}
        };
    }
    if (normalizedTool == "get_mcp_status")
    {
        const McpTransportStatus status = m_transport->get_status();
        return {
            {"running", m_running.load()},
            {"initialized", m_initialized.load()},
            // Repeated from get_build_info on purpose: this is the tool
            // reached for when something is behaving unexpectedly, and
            // "which binary is this" is the first thing to rule out.
            {"build", {
                {"version", build_info_version()},
                {"compiled", build_info_timestamp()}
            }},
            {"transport", {
                {"mode", status.mode}, {"bind_address", status.bindAddress},
                {"port", status.port}, {"listening", status.listening},
                {"authentication_enabled", status.authenticationEnabled},
                {"active_request", status.activeRequest},
                {"active_request_age_ms", status.activeRequestAgeMs}
            }},
            {"queues", {
                {"commands_pending", m_commandQueue.Pending()},
                {"commands_queued", m_commandQueue.Queued()},
                {"responses_queued", m_responseQueue.Queued()}
            }},
            {"counters", {
                {"accepted_requests", status.acceptedRequests},
                {"stall_timeouts", status.stallTimeouts},
                {"receive_timeouts", status.receiveTimeouts},
                {"client_disconnects", status.clientDisconnects},
                {"invalid_framing", status.invalidFraming},
                {"oversized_headers", status.oversizedHeaders},
                {"oversized_bodies", status.oversizedBodies},
                {"authentication_rejects", status.authenticationRejects},
                {"discarded_responses", status.discardedResponses},
                {"send_failures", status.sendFailures}
            }}
        };
    }
    // Breakpoints
    if (normalizedTool == "set_breakpoint")
    {
        std::string addrStr = arguments["address"];
        u16 address;
        if (!parse_mcp_hex_with_prefix(addrStr, &address))
            return {{"error", "Invalid address format"}};

        std::string memory_area = arguments.value("memory_area", "rom_ram");
        if (!IsBreakpointMemoryAreaValid(memory_area))
            return {{"error", "Invalid memory_area (must be: rom_ram or vram)"}};
        int breakpoint_type = GetBreakpointTypeFromString(memory_area);

        bool read = arguments.value("read", false);
        bool write = arguments.value("write", false);
        bool execute = arguments.value("execute", true);

        if (breakpoint_type != 0)
            execute = false;

        if (!read && !write && !execute)
            return {{"error", "At least one of read, write, or execute must be true"}};

        u16 max_address = 0xFFFF;
        if (breakpoint_type == 1)
            max_address = 0x3FFF;
        else if (breakpoint_type == 2)
            max_address = 0x0007;
        if (address > max_address)
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "Address 0x%04X out of range for %s (max: 0x%04X)", address, memory_area.c_str(), max_address);
            return {{"error", msg}};
        }

        m_debugAdapter.SetBreakpoint(address, breakpoint_type, read, write, execute);
        return {{"success", true}, {"address", addrStr}, {"memory_area", memory_area}};
    }
    if (normalizedTool == "set_breakpoint_range")
    {
        std::string startAddrStr = arguments["start_address"];
        std::string endAddrStr = arguments["end_address"];
        u16 start_address, end_address;

        if (!parse_mcp_hex_with_prefix(startAddrStr, &start_address))
            return {{"error", "Invalid start_address format"}};
        if (!parse_mcp_hex_with_prefix(endAddrStr, &end_address))
            return {{"error", "Invalid end_address format"}};
        if (start_address > end_address)
            return {{"error", "start_address must be <= end_address"}};

        std::string memory_area = arguments.value("memory_area", "rom_ram");
        if (!IsBreakpointMemoryAreaValid(memory_area))
            return {{"error", "Invalid memory_area (must be: rom_ram or vram)"}};
        int breakpoint_type = GetBreakpointTypeFromString(memory_area);

        bool read = arguments.value("read", false);
        bool write = arguments.value("write", false);
        bool execute = arguments.value("execute", true);

        if (breakpoint_type != 0)
            execute = false;

        if (!read && !write && !execute)
            return {{"error", "At least one of read, write, or execute must be true"}};

        u16 max_address = 0xFFFF;
        if (breakpoint_type == 1)
            max_address = 0x3FFF;
        else if (breakpoint_type == 2)
            max_address = 0x0007;
        if (start_address > max_address || end_address > max_address)
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "Address out of range for %s (max: 0x%04X)", memory_area.c_str(), max_address);
            return {{"error", msg}};
        }

        m_debugAdapter.SetBreakpointRange(start_address, end_address, breakpoint_type,
                                         read, write, execute);
        return {{"success", true}, {"start_address", startAddrStr}, {"end_address", endAddrStr}, {"memory_area", memory_area}};
    }
    if (normalizedTool == "remove_breakpoint")
    {
        std::string addrStr = arguments["address"];
        u16 address;
        if (!parse_mcp_hex_with_prefix(addrStr, &address))
            return {{"error", "Invalid address format"}};

        std::string memory_area = arguments.value("memory_area", "rom_ram");
        if (!IsBreakpointMemoryAreaValid(memory_area))
            return {{"error", "Invalid memory_area (must be: rom_ram or vram)"}};
        int breakpoint_type = GetBreakpointTypeFromString(memory_area);

        u16 end_address = 0;
        if (arguments.contains("end_address"))
        {
            std::string endAddrStr = arguments["end_address"];
            if (!parse_mcp_hex_with_prefix(endAddrStr, &end_address))
                return {{"error", "Invalid end_address format"}};
        }

        m_debugAdapter.ClearBreakpointByAddress(address, breakpoint_type, end_address);
        return {{"success", true}, {"address", addrStr}, {"memory_area", memory_area}};
    }
    if (normalizedTool == "list_breakpoints")
    {
        std::vector<BreakpointInfo> breakpoints = m_debugAdapter.ListBreakpoints();
        json bpArray = json::array();
        for (const BreakpointInfo& bp : breakpoints)
        {
            json bpObj;
            bpObj["enabled"] = bp.enabled;
            bpObj["type"] = bp.type_name;

            std::ostringstream addr_ss;
            addr_ss << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << bp.address1;
            bpObj["address"] = addr_ss.str();

            if (bp.range)
            {
                std::ostringstream addr2_ss;
                addr2_ss << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << bp.address2;
                bpObj["address2"] = addr2_ss.str();
            }

            bpObj["read"] = bp.read;
            bpObj["write"] = bp.write;
            bpObj["execute"] = bp.execute;
            bpArray.push_back(bpObj);
        }
        return {{"breakpoints", bpArray}};
    }
    if (normalizedTool == "add_debug_rule")
    {
        const std::string category = arguments["category"];
        const std::string startText = arguments["start_address"];
        const std::string endText = arguments.value("end_address", startText);
        u16 start = 0;
        u16 end = 0;
        if (!parse_mcp_hex_with_prefix(startText, &start) || !parse_mcp_hex_with_prefix(endText, &end) || start > end)
            return {{"error", "Invalid start_address/end_address"}};

        bool read = arguments.value("read", false);
        bool write = arguments.value("write", false);
        bool execute = arguments.value("execute", category == "cpu_execute");
        bool event = arguments.value("event", category == "video_timing");
        if (category == "cpu_execute")
        {
            read = false;
            write = false;
            execute = true;
        }

        const bool pause = arguments.value("pause", false);
        const bool log = arguments.value("log", true);
        const bool hasMask = arguments.contains("value_mask");
        const bool hasExpected = arguments.contains("value_expected");

        const u8 valueMask = static_cast<u8>(arguments.value("value_mask", 255));
        const u8 valueExpected = static_cast<u8>(arguments.value("value_expected", 0));
        const u64 breakOnHit = static_cast<u64>(arguments.value("break_on_hit", 1));
        const std::string condition = arguments.value("condition",
            (hasMask || hasExpected) ? "after_masked_equal" : "any");
        const bool oneShot = arguments.value("one_shot", false);
        const u8 beforeValueExpected = static_cast<u8>(arguments.value("before_value_expected", 0));
        return m_debugAdapter.AddDebugRule(category, start, end, read, write, execute, event,
            pause, log, hasMask || hasExpected, valueMask, valueExpected, breakOnHit, condition, oneShot, beforeValueExpected);
    }
    if (normalizedTool == "add_device_debug_rule")
    {
        const std::string device = arguments.value("device", "");
        const std::string space = arguments.value("space", "");
        const std::string startText = arguments.value("start_target", "");
        const std::string endText = arguments.value("end_target", startText);
        u32 start = 0;
        u32 end = 0;
        if (!parse_mcp_hex_with_prefix(startText, &start)
            || !parse_mcp_hex_with_prefix(endText, &end) || start > end)
            return {{"error", "Invalid start_target/end_target"}};
        const bool read = arguments.value("read", false);
        const bool write = arguments.value("write", false);
        const bool execute = arguments.value("execute", false);
        const bool event = arguments.value("event", space == "state" || space == "signals" || space == "raster");
        const bool pause = arguments.value("pause", false);
        const bool log = arguments.value("log", true);
        const u64 valueMask = arguments.value("value_mask", static_cast<u64>(~0ULL));
        const u64 valueExpected = arguments.value("value_expected", static_cast<u64>(0));
        const u64 beforeValueExpected = arguments.value("before_value_expected", static_cast<u64>(0));
        const u64 breakOnHit = arguments.value("break_on_hit", static_cast<u64>(1));
        const std::string condition = arguments.value("condition",
            arguments.contains("value_expected") ? "after_masked_equal" : "any");
        const bool oneShot = arguments.value("one_shot", false);
        return m_debugAdapter.AddDeviceDebugRule(device, space, start, end,
            read, write, execute, event, pause, log, valueMask, valueExpected,
            breakOnHit, condition, oneShot, beforeValueExpected);
    }
    if (normalizedTool == "list_debug_rules")
    {
        return m_debugAdapter.ListDebugRules();
    }
    if (normalizedTool == "list_debug_devices")
    {
        return m_debugAdapter.ListDebugDevices();
    }
    if (normalizedTool == "remove_debug_rule")
    {
        return m_debugAdapter.RemoveDebugRule(static_cast<u32>(arguments["id"]));
    }
    if (normalizedTool == "clear_debug_rules")
    {
        return m_debugAdapter.ClearDebugRules();
    }
    if (normalizedTool == "get_debug_events")
    {
        return m_debugAdapter.GetDebugEvents(arguments);
    }
    if (normalizedTool == "clear_debug_events")
    {
        return m_debugAdapter.ClearDebugEvents();
    }
    if (normalizedTool == "toggle_irq_breakpoints")
    {
        bool enabled = arguments["enabled"];
        (void)enabled;
        return {{"error", "IRQ breakpoints are not available in GearSF7000"}};
    }
    // Memory
    if (normalizedTool == "list_memory_areas")
    {
        std::vector<MemoryAreaInfo> areas = m_debugAdapter.ListMemoryAreas();
        json areaArray = json::array();
        for (const MemoryAreaInfo& area : areas)
        {
            json areaObj;
            areaObj["id"] = area.id;
            areaObj["name"] = area.name;
            areaObj["size"] = area.size;
            areaObj["display_base"] = area.display_base;
            areaArray.push_back(areaObj);
        }
        return {{"areas", areaArray}};
    }
    if (normalizedTool == "read_memory")
    {
        int area = arguments["area"];
        std::string offsetStr = arguments["offset"];
        u32 offset;
        if (!parse_mcp_hex_with_prefix(offsetStr, &offset))
            return {{"error", "Invalid offset format"}};

        size_t size = arguments["size"];
        std::vector<u8> data = m_debugAdapter.ReadMemoryArea(area, offset, size);

        std::ostringstream hex_ss;
        for (size_t i = 0; i < data.size(); i++)
        {
            hex_ss << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << (int)data[i];
            if (i < data.size() - 1)
                hex_ss << " ";
        }

        return {{"area", area}, {"offset", offsetStr}, {"data", hex_ss.str()}};
    }
    if (normalizedTool == "get_atomic_snapshot")
    {
        return m_debugAdapter.GetAtomicSnapshot(arguments);
    }
    if (normalizedTool == "write_memory")
    {
        if (!arguments.contains("area") || !arguments["area"].is_number_integer())
            return {{"error", "area is required"}};
        if (!arguments.contains("offset") || !arguments["offset"].is_string())
            return {{"error", "offset is required"}};
        if (!arguments.contains("bytes") || !arguments["bytes"].is_string())
            return {{"error", "bytes is required"}};

        int area = arguments["area"].get<int>();
        std::string offsetStr = arguments["offset"].get<std::string>();
        u32 offset;
        if (!parse_mcp_hex_with_prefix(offsetStr, &offset))
            return {{"error", "Invalid offset format"}};

        std::string bytesStr = arguments["bytes"].get<std::string>();
        std::vector<u8> data;

        std::istringstream iss(bytesStr);
        std::string byteStr;
        while (iss >> byteStr)
        {
            u8 byte;
            if (!parse_mcp_hex_with_prefix(byteStr, &byte))
                return {{"error", "Invalid byte format"}};
            data.push_back(byte);
        }

        m_debugAdapter.WriteMemoryArea(area, offset, data);
        return {{"success", true}, {"area", area}, {"offset", offsetStr}, {"bytes_written", data.size()}};
    }
    // Registers
    if (normalizedTool == "write_z80_register")
    {
        std::string name = arguments["name"];
        std::string valueStr = arguments["value"];
        u32 value;
        if (!parse_mcp_hex_with_prefix(valueStr, &value))
            return {{"error", "Invalid value format"}};

        m_debugAdapter.SetRegister(name, value);
        return {{"success", true}, {"register", name}, {"value", valueStr}};
    }
    // Disassembly
    if (normalizedTool == "get_disassembly")
    {
        if (!arguments.contains("start_address"))
            return {{"error", "start_address is required"}};
        if (!arguments.contains("end_address"))
            return {{"error", "end_address is required"}};

        std::string startAddrStr = arguments["start_address"];
        std::string endAddrStr = arguments["end_address"];
        u16 start_address, end_address;

        if (!parse_mcp_hex_with_prefix(startAddrStr, &start_address))
            return {{"error", "Invalid start_address format"}};
        if (!parse_mcp_hex_with_prefix(endAddrStr, &end_address))
            return {{"error", "Invalid end_address format"}};
        if (start_address > end_address)
            return {{"error", "start_address must be <= end_address"}};

        u32 range_size = (u32)end_address - (u32)start_address + 1;
        if (range_size > 0x2000)
            return {{"error", "Address range too large. Maximum range is 0x2000 (8KB). Use smaller ranges for disassembly."}};

        // Optional bank parameter (-1 means use current mapper mappings)
        int bank = -1;
        if (arguments.contains("bank"))
        {
            std::string bankStr = arguments["bank"];
            u8 bank_value;
            if (!parse_mcp_hex_with_prefix(bankStr, &bank_value))
                return {{"error", "Invalid bank format (must be 00-FF in hex)"}};
            bank = bank_value;
        }

        bool resolve_symbols = false;
        if (arguments.contains("resolve_symbols") && arguments["resolve_symbols"].is_boolean())
            resolve_symbols = arguments["resolve_symbols"].get<bool>();

        bool detailed = false;
        if (arguments.contains("detailed") && arguments["detailed"].is_boolean())
            detailed = arguments["detailed"].get<bool>();

        std::vector<DisasmLine> lines = m_debugAdapter.GetDisassembly(start_address, end_address, bank, resolve_symbols);

        json result;

        if (detailed)
        {
            json instructions = json::array();

            for (const DisasmLine& line : lines)
            {
                json instr;
                std::ostringstream addr_ss, bank_ss, jump_ss;

                addr_ss << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << line.address;
                bank_ss << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << (int)line.bank;

                instr["address"] = addr_ss.str();
                instr["bank"] = bank_ss.str();
                instr["segment"] = line.segment;
                instr["instruction"] = line.name;
                instr["bytes"] = line.bytes;
                instr["size"] = line.size;

                if (line.jump)
                {
                    jump_ss << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << line.jump_address;
                    instr["jump_target"] = jump_ss.str();
                    instr["is_subroutine"] = line.subroutine;
                }

                if (line.irq > 0)
                {
                    instr["irq"] = line.irq;
                }

                instructions.push_back(instr);
            }

            result["instructions"] = instructions;
        }
        else
        {
            json instructions = json::array();

            for (const DisasmLine& line : lines)
            {
                std::ostringstream ss;
                ss << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << (int)line.bank;
                ss << ":" << std::setw(4) << line.address;
                ss << "  " << line.name;
                instructions.push_back(ss.str());
            }

            result["instructions"] = instructions;
        }

        result["count"] = lines.size();
        result["start_address"] = startAddrStr;
        result["end_address"] = endAddrStr;
        if (bank >= 0)
        {
            std::ostringstream bank_ss;
            bank_ss << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << bank;
            result["bank"] = bank_ss.str();
        }

        if (lines.empty())
        {
            result["note"] = "No disassembly records found. You may have asked for code that has not been executed yet. Code is only disassembled as it is executed.";
        }

        return result;
    }
    if (normalizedTool == "get_code_coverage")
    {
        return m_debugAdapter.GetCodeCoverage(arguments);
    }
    if (normalizedTool == "clear_code_coverage")
    {
        return m_debugAdapter.ClearCodeCoverage();
    }
    // Media info
    if (normalizedTool == "get_media_info")
    {
        return m_debugAdapter.GetMediaInfo();
    }
    if (normalizedTool == "list_recent_roms")
    {
        return m_debugAdapter.ListRecentRoms();
    }
    if (normalizedTool == "list_recent_tapes")
    {
        return m_debugAdapter.ListRecentTapes();
    }
    if (normalizedTool == "list_recent_disks")
    {
        return m_debugAdapter.ListRecentDisks();
    }
    // Chip status
    if (normalizedTool == "get_z80_status")
    {
        return m_debugAdapter.GetZ80Status();
    }
    if (normalizedTool == "get_z80_clock")
    {
        return m_debugAdapter.GetZ80Clock();
    }
    if (normalizedTool == "reset_z80_clock")
    {
        return m_debugAdapter.ResetZ80Clock();
    }
    if (normalizedTool == "get_vdp_registers")
    {
        return m_debugAdapter.GetVDPRegisters();
    }
    if (normalizedTool == "get_vdp_status")
    {
        return m_debugAdapter.GetVDPStatus();
    }
    if (normalizedTool == "set_region")
    {
        return m_debugAdapter.SetRegion(arguments.value("region", ""));
    }
    if (normalizedTool == "get_slot_history")
    {
        return m_debugAdapter.GetSlotHistory();
    }
    if (normalizedTool == "set_renderer_source")
    {
        return m_debugAdapter.SetRendererSource(arguments.value("source", ""));
    }
    if (normalizedTool == "read_framebuffer")
    {
        return m_debugAdapter.ReadFrameBuffer(
            arguments.value("source", "presented"),
            arguments.value("y", 0),
            arguments.value("height", 0));
    }
    if (normalizedTool == "get_psg_status")
    {
        return m_debugAdapter.GetPSGStatus();
    }
    if (normalizedTool == "get_sf7000_status")
    {
        return m_debugAdapter.GetSF7000Status();
    }
    if (normalizedTool == "get_ay8910_status")
    {
        return m_debugAdapter.GetAY8910Status();
    }
    if (normalizedTool == "get_screenshot")
    {
        return m_debugAdapter.GetScreenshot();
    }
    if (normalizedTool == "set_full_raster_debug_enabled")
    {
        return m_debugAdapter.SetFullRasterDebugEnabled(arguments["enabled"]);
    }
    if (normalizedTool == "get_full_raster_debug_screenshot")
    {
        return m_debugAdapter.GetFullRasterDebugScreenshot();
    }
    if (normalizedTool == "dump_crt_signal")
    {
        return m_debugAdapter.DumpCrtSignal(
            arguments.value("file_path", ""), arguments.value("lines", 0));
    }
    if (normalizedTool == "export_memory_file")
    {
        return m_debugAdapter.ExportMemoryToFile(
            arguments.value("area", 0), arguments.value("offset", 0u),
            arguments.value("length", (size_t)0), arguments.value("file_path", ""));
    }
    if (normalizedTool == "import_memory_file")
    {
        return m_debugAdapter.ImportMemoryFromFile(
            arguments.value("area", 0), arguments.value("offset", 0u), arguments.value("file_path", ""));
    }
    if (normalizedTool == "save_disassembly_file")
    {
        return m_debugAdapter.SaveDisassemblyFile(
            arguments.value("full", false), arguments.value("file_path", ""));
    }
    if (normalizedTool == "save_debug_png")
    {
        return m_debugAdapter.SaveDebugPng(
            arguments.value("view", ""), arguments.value("sprite_index", 0), arguments.value("file_path", ""));
    }
    // Media and state management
    if (normalizedTool == "load_rom")
    {
        return m_debugAdapter.LoadRom(arguments.value("file_path", ""));
    }
    if (normalizedTool == "set_start_paused")
    {
        return m_debugAdapter.SetStartPaused(arguments.value("paused", false));
    }
    if (normalizedTool == "eject_rom")
    {
        return m_debugAdapter.EjectRom();
    }
    if (normalizedTool == "load_tape")
    {
        return m_debugAdapter.LoadTape(arguments.value("file_path", ""));
    }
    if (normalizedTool == "set_tape_speed")
    {
        return m_debugAdapter.SetTapeSpeed(arguments.value("percent", 0.0f));
    }
    if (normalizedTool == "tape_play")
    {
        return m_debugAdapter.TapePlay();
    }
    if (normalizedTool == "tape_stop")
    {
        return m_debugAdapter.TapeStop();
    }
    if (normalizedTool == "tape_rewind")
    {
        return m_debugAdapter.TapeRewind();
    }
    if (normalizedTool == "mount_disk")
    {
        return m_debugAdapter.MountDisk(arguments.value("file_path", ""), arguments.value("write_protected", config_emulator.disc_write_protected));
    }
    if (normalizedTool == "start_sf7000")
    {
        return m_debugAdapter.StartSF7000();
    }
    if (normalizedTool == "load_symbols")
    {
        std::string file_path = arguments["file_path"];
        return m_debugAdapter.LoadSymbols(file_path);
    }
    if (normalizedTool == "list_save_state_slots")
    {
        return m_debugAdapter.ListSaveStateSlots();
    }
    if (normalizedTool == "select_save_state_slot")
    {
        int slot = arguments["slot"];
        return m_debugAdapter.SelectSaveStateSlot(slot);
    }
    if (normalizedTool == "save_state")
    {
        return m_debugAdapter.SaveState();
    }
    if (normalizedTool == "load_state")
    {
        return m_debugAdapter.LoadState();
    }
    if (normalizedTool == "save_state_file")
    {
        if (!arguments.contains("file_path") || !arguments["file_path"].is_string())
            return {{"error", "File path is required"}};

        std::string file_path = arguments["file_path"];
        return m_debugAdapter.SaveStateFile(file_path);
    }
    if (normalizedTool == "load_state_file")
    {
        if (!arguments.contains("file_path") || !arguments["file_path"].is_string())
            return {{"error", "File path is required"}};

        std::string file_path = arguments["file_path"];
        return m_debugAdapter.LoadStateFile(file_path);
    }
    if (normalizedTool == "set_fast_forward_speed")
    {
        int speed = arguments["speed"];
        return m_debugAdapter.SetFastForwardSpeed(speed);
    }
    if (normalizedTool == "toggle_fast_forward")
    {
        bool enabled = arguments["enabled"];
        return m_debugAdapter.ToggleFastForward(enabled);
    }
    if (normalizedTool == "get_rewind_status")
    {
        return m_debugAdapter.GetRewindStatus();
    }
    if (normalizedTool == "configure_rewind")
    {
        const bool enabled = arguments.value("enabled", true);
        const int seconds = arguments.value("seconds", 60);
        const int perSnapshot = arguments.value("frames_per_snapshot", 1);
        const int limitMb = arguments.value("memory_limit_mb", 0);
        return m_debugAdapter.ConfigureRewind(enabled, seconds, perSnapshot,
                                              limitMb);
    }
    if (normalizedTool == "get_sync_settings")
    {
        return m_debugAdapter.GetSyncSettings();
    }
    if (normalizedTool == "set_sync_settings")
    {
        return m_debugAdapter.SetSyncSettings(arguments);
    }
    if (normalizedTool == "get_rewind_position")
    {
        return m_debugAdapter.GetRewindPosition();
    }
    if (normalizedTool == "analyze_rewind_range")
    {
        return m_debugAdapter.AnalyzeRewindRange(arguments);
    }
    if (normalizedTool == "rewind_transport")
    {
        const std::string action = arguments.value("action", "");
        const int frames = arguments.value("frames", 1);
        return m_debugAdapter.RewindTransport(action, frames);
    }
    if (normalizedTool == "rewind_seek")
    {
        // Either addressing mode is accepted; age_frames is converted to the
        // snapshot numbering the adapter speaks.
        if (arguments.contains("age_frames"))
        {
            const int age = arguments["age_frames"];
            const int count = rewind_get_snapshot_count();
            return m_debugAdapter.RewindSeek(count - age);
        }
        if (!arguments.contains("snapshot"))
            return {{"error", "rewind_seek needs snapshot or age_frames"}};
        int snapshot = arguments["snapshot"];
        return m_debugAdapter.RewindSeek(snapshot);
    }
    if (normalizedTool == "controller_button")
    {
        int player = arguments["player"];
        std::string button = arguments["button"];
        std::string action = arguments["action"];
        return m_debugAdapter.ControllerButton(player, button, action);
    }
    if (normalizedTool == "trigger_nmi")
    {
        return m_debugAdapter.TriggerNMI();
    }
    if (normalizedTool == "keyboard_text")
    {
        return m_debugAdapter.KeyboardText(arguments.value("text", ""));
    }
    if (normalizedTool == "cancel_keyboard_text")
    {
        return m_debugAdapter.CancelKeyboardText();
    }
    if (normalizedTool == "basic_typer_set_text")
    {
        return m_debugAdapter.BasicTyperSetText(arguments.value("text", ""), arguments.value("send", false));
    }
    if (normalizedTool == "basic_typer_clear")
    {
        return m_debugAdapter.BasicTyperClear(arguments.value("cancel_queue", true));
    }
    if (normalizedTool == "basic_typer_status")
    {
        return m_debugAdapter.BasicTyperStatus();
    }
    if (normalizedTool == "load_basic_program")
    {
        return m_debugAdapter.LoadBasicProgram(arguments.value("file_path", ""), arguments.value("pointer_block", -1));
    }
    if (normalizedTool == "save_basic_program")
    {
        return m_debugAdapter.SaveBasicProgram(arguments.value("file_path", ""), arguments.value("pointer_block", -1));
    }
    if (normalizedTool == "find_basic_blocks")
    {
        return m_debugAdapter.FindBasicBlocks();
    }
    if (normalizedTool == "keyboard_key")
    {
        std::string key = arguments.value("key", "");
        std::string action = arguments.value("action", "");
        return m_debugAdapter.KeyboardKey(key, action);
    }
    if (normalizedTool == "get_input_state")
    {
        return m_debugAdapter.GetInputState();
    }
    if (normalizedTool == "get_keyboard_mode")
    {
        return m_debugAdapter.GetKeyboardMode();
    }
    if (normalizedTool == "set_keyboard_mode")
    {
        return m_debugAdapter.SetKeyboardMode(arguments.value("enabled", true));
    }
    if (normalizedTool == "controller_macro")
    {
        return {{"error", "controller_macro must be handled by the MCP manager"}};
    }
    if (normalizedTool == "list_sprites")
    {
        return m_debugAdapter.ListSprites(arguments.value("line", -1),
            arguments.value("source", "live"));
    }
    if (normalizedTool == "get_sprite_image")
    {
        int sprite_index = arguments.value("sprite_index", 0);
        return m_debugAdapter.GetSpriteImage(sprite_index,
            arguments.value("source", "live"));
    }
    if (normalizedTool == "get_sprite_pipeline")
    {
        return m_debugAdapter.GetSpritePipeline();
    }
    if (normalizedTool == "get_sprite_scanline_history")
    {
        return m_debugAdapter.GetSpriteScanlineHistory(
            arguments.value("start_line", 0),
            arguments.value("end_line", -1),
            arguments.value("include_empty", false));
    }
    // Disassembler operations
    if (normalizedTool == "debug_run_to_cursor")
    {
        std::string addrStr = arguments["address"];
        u16 address;
        if (!parse_mcp_hex_with_prefix(addrStr, &address))
            return {{"error", "Invalid address format"}};
        return m_debugAdapter.RunToAddress(address);
    }
    if (normalizedTool == "add_disassembler_bookmark")
    {
        std::string addrStr = arguments["address"];
        u16 address;
        if (!parse_mcp_hex_with_prefix(addrStr, &address))
            return {{"error", "Invalid address format"}};
        std::string name = arguments.value("name", "");
        return m_debugAdapter.AddDisassemblerBookmark(address, name);
    }
    if (normalizedTool == "remove_disassembler_bookmark")
    {
        std::string addrStr = arguments["address"];
        u16 address;
        if (!parse_mcp_hex_with_prefix(addrStr, &address))
            return {{"error", "Invalid address format"}};
        return m_debugAdapter.RemoveDisassemblerBookmark(address);
    }
    if (normalizedTool == "add_symbol")
    {
        std::string bankStr = arguments["bank"];
        std::string addrStr = arguments["address"];
        std::string name = arguments["name"];
        u8 bank;
        u16 address;
        if (!parse_mcp_hex_with_prefix(bankStr, &bank))
            return {{"error", "Invalid bank format"}};
        if (!parse_mcp_hex_with_prefix(addrStr, &address))
            return {{"error", "Invalid address format"}};
        return m_debugAdapter.AddSymbol(bank, address, name);
    }
    if (normalizedTool == "remove_symbol")
    {
        std::string bankStr = arguments["bank"];
        std::string addrStr = arguments["address"];
        u8 bank;
        u16 address;
        if (!parse_mcp_hex_with_prefix(bankStr, &bank))
            return {{"error", "Invalid bank format"}};
        if (!parse_mcp_hex_with_prefix(addrStr, &address))
            return {{"error", "Invalid address format"}};
        return m_debugAdapter.RemoveSymbol(bank, address);
    }
    // Memory editor operations
    if (normalizedTool == "select_memory_range")
    {
        int editor = arguments["area"];
        std::string startStr = arguments["start_address"];
        std::string endStr = arguments["end_address"];
        u32 start_address, end_address;
        if (!parse_mcp_hex_with_prefix(startStr, &start_address))
            return {{"error", "Invalid start_address format"}};
        if (!parse_mcp_hex_with_prefix(endStr, &end_address))
            return {{"error", "Invalid end_address format"}};
        return m_debugAdapter.SelectMemoryRange(editor, start_address, end_address);
    }
    if (normalizedTool == "set_memory_selection_value")
    {
        int editor = arguments["area"];
        std::string valueStr = arguments["value"];
        u8 value;
        if (!parse_mcp_hex_with_prefix(valueStr, &value))
            return {{"error", "Invalid value format"}};
        return m_debugAdapter.SetMemorySelectionValue(editor, value);
    }
    if (normalizedTool == "add_memory_bookmark")
    {
        int editor = arguments["area"];
        std::string addrStr = arguments["address"];
        std::string name = arguments.value("name", "");
        u32 address;
        if (!parse_mcp_hex_with_prefix(addrStr, &address))
            return {{"error", "Invalid address format"}};
        return m_debugAdapter.AddMemoryBookmark(editor, address, name);
    }
    if (normalizedTool == "remove_memory_bookmark")
    {
        int editor = arguments["area"];
        std::string addrStr = arguments["address"];
        u32 address;
        if (!parse_mcp_hex_with_prefix(addrStr, &address))
            return {{"error", "Invalid address format"}};
        return m_debugAdapter.RemoveMemoryBookmark(editor, address);
    }
    if (normalizedTool == "watch_add")
    {
        int area = arguments["area"];
        std::string addrStr = arguments["address"];
        std::string data_type = arguments["data_type"];
        int size = arguments["size"];
        std::string label = arguments.value("label", "");
        u32 address;
        if (!parse_mcp_hex_with_prefix(addrStr, &address))
            return {{"error", "Invalid address format"}};
        return m_debugAdapter.WatchAdd(area, address, data_type, size, label);
    }
    if (normalizedTool == "watch_remove")
    {
        return m_debugAdapter.WatchRemove(arguments["id"]);
    }
    if (normalizedTool == "watch_freeze")
    {
        return m_debugAdapter.WatchFreeze(arguments["id"], arguments["value"]);
    }
    if (normalizedTool == "watch_unfreeze")
    {
        return m_debugAdapter.WatchUnfreeze(arguments["id"]);
    }
    if (normalizedTool == "watch_set_condition")
    {
        return m_debugAdapter.WatchSetCondition(arguments["conditions"], arguments["mode"]);
    }
    if (normalizedTool == "watch_clear_condition")
    {
        return m_debugAdapter.WatchClearCondition();
    }
    if (normalizedTool == "watch_condition_status")
    {
        return m_debugAdapter.WatchConditionStatus();
    }
    if (normalizedTool == "list_disassembler_bookmarks")
    {
        return m_debugAdapter.ListDisassemblerBookmarks();
    }
    if (normalizedTool == "list_symbols")
    {
        return m_debugAdapter.ListSymbols();
    }
    if (normalizedTool == "lookup_symbol_by_name")
    {
        return m_debugAdapter.LookupSymbolByName(arguments["name"]);
    }
    if (normalizedTool == "lookup_symbol_at_address")
    {
        std::string bank_str = arguments["bank"];
        std::string address_str = arguments["address"];
        u8 bank;
        u16 address;
        if (!parse_mcp_hex_with_prefix(bank_str, &bank))
            return {{"error", "Invalid bank format"}};
        if (!parse_mcp_hex_with_prefix(address_str, &address))
            return {{"error", "Invalid address format"}};
        return m_debugAdapter.LookupSymbolAtAddress(bank, address);
    }
    if (normalizedTool == "get_call_stack")
    {
        return m_debugAdapter.ListCallStack();
    }
    if (normalizedTool == "list_memory_bookmarks")
    {
        int area = arguments["area"];
        return m_debugAdapter.ListMemoryBookmarks(area);
    }
    if (normalizedTool == "watch_list")
    {
        return m_debugAdapter.WatchList();
    }
    if (normalizedTool == "get_memory_selection")
    {
        int area = arguments["area"];
        return m_debugAdapter.GetMemorySelection(area);
    }
    if (normalizedTool == "memory_search_capture")
    {
        int area = arguments["area"];
        return m_debugAdapter.MemorySearchCapture(area);
    }
    if (normalizedTool == "memory_search")
    {
        int area = arguments["area"];
        std::string op = arguments["operator"];
        std::string compare_type = arguments["compare_type"];
        int compare_value = arguments.value("compare_value", 0);
        std::string data_type = arguments.value("data_type", "unsigned");
        bool narrow_previous = arguments.value("narrow_previous", false);
        return m_debugAdapter.MemorySearch(area, op, compare_type, compare_value, data_type, narrow_previous);
    }
    if (normalizedTool == "memory_find_bytes")
    {
        if (!arguments.contains("area") || !arguments["area"].is_number_integer())
            return {{"error", "area is required"}};
        if (!arguments.contains("hex_bytes") || !arguments["hex_bytes"].is_string())
            return {{"error", "hex_bytes is required"}};

        int area = arguments["area"].get<int>();
        std::string hex_bytes = arguments["hex_bytes"].get<std::string>();
        return m_debugAdapter.MemoryFindBytes(area, hex_bytes);
    }
    if (normalizedTool == "memory_find_bytes_advanced")
    {
        if (!arguments.contains("area") || !arguments["area"].is_number_integer())
            return {{"error", "area is required"}};
        if (!arguments.contains("pattern") || !arguments["pattern"].is_string())
            return {{"error", "pattern is required"}};

        int area = arguments["area"].get<int>();
        std::string pattern = arguments["pattern"].get<std::string>();
        int wildcard_limit = arguments.value("wildcard_limit", 4);
        return m_debugAdapter.MemoryFindBytesAdvanced(area, pattern, wildcard_limit);
    }
    if (normalizedTool == "get_trace_log")
    {
        int start = arguments.value("start", 0);
        int count = arguments.value("count", 100);
        return m_debugAdapter.GetTraceLog(start, count);
    }
    if (normalizedTool == "set_trace_log")
    {
        bool enabled = arguments.value("enabled", true);
        u32 flags = arguments.value("flags", 0xFFu);
        return m_debugAdapter.SetTraceLogDetailed(enabled, flags, arguments);
    }

    return {{"error", "Unknown tool: " + toolName}};
}

void McpServer::SendResponse(const json& response, McpRequestToken request_token)
{
    std::string line = response.dump(-1, ' ', false, json::error_handler_t::replace);
    if (request_token == MCP_REQUEST_TOKEN_NONE)
        request_token = m_readerRequestToken;
    m_transport->send(line, request_token);
}

void McpServer::SendError(const json& id, int code, const std::string& message,
                          const json& data, McpRequestToken request_token)
{
    json error;
    error["jsonrpc"] = "2.0";
    error["id"] = id;
    error["error"] = {
        {"code", code},
        {"message", message}
    };

    if (!data.empty() && !data.is_null())
    {
        error["error"]["data"] = data;
    }

    if (!g_mcp_stdio_mode)
        Log("[MCP] Sending error: %s", error.dump().c_str());

    SendResponse(error, request_token);
}

void McpServer::LoadResources()
{
    // GearSF7000 initially exposes its tool schemas directly. Hardware
    // resource files will be added with the SC-3000-specific skill pack.
}

static bool IsValidResourceName(const std::string& name)
{
    if (name.empty() || name == "." || name == "..")
        return false;

    for (size_t i = 0; i < name.size(); i++)
    {
        unsigned char character = (unsigned char)name[i];
        if (character < 0x20 || character == 0x7F || character == '/' || character == '\\')
            return false;
    }

    return true;
}

void McpServer::LoadResourcesFromCategory(const std::string& category, const std::string& tocPath)
{
    std::ifstream file(tocPath);
    if (!file.is_open())
    {
        Log("[MCP] Warning: Resources TOC file not found: %s", tocPath.c_str());
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    bool read_error = file.bad();
    file.close();

    if (read_error)
    {
        Log("[MCP] Warning: Failed to read resources TOC file: %s", tocPath.c_str());
        return;
    }

    if (!json::accept(content))
    {
        Log("[MCP] Warning: Invalid JSON in resources TOC file: %s", tocPath.c_str());
        return;
    }

    json toc = json::parse(content);

    if (!toc.contains("toc") || !toc["toc"].is_array())
    {
        Log("[MCP] Warning: Invalid TOC format in resources TOC file: %s", tocPath.c_str());
        return;
    }

    std::string tocDir = tocPath.substr(0, tocPath.find_last_of("/\\"));

    for (size_t i = 0; i < toc["toc"].size(); i++)
    {
        const json& item = toc["toc"][i];
        if (!item.is_object() || !item.contains("uri") || !item["uri"].is_string() ||
            !item.contains("title") || !item["title"].is_string() ||
            (item.contains("description") && !item["description"].is_string()) ||
            (item.contains("mimeType") && !item["mimeType"].is_string()))
        {
            Log("[MCP] Warning: Invalid resource entry %d in TOC file: %s", (int)i, tocPath.c_str());
            continue;
        }

        std::string name = item["uri"].get<std::string>();
        if (!IsValidResourceName(name))
        {
            Log("[MCP] Warning: Invalid resource name in TOC file: %s", tocPath.c_str());
            continue;
        }

        ResourceInfo resource;
        resource.uri = "gearsf7000://" + category + "/" + name;
        resource.title = item["title"].get<std::string>();
        resource.description = item.contains("description") ? item["description"].get<std::string>() : "";
        resource.mimeType = item.contains("mimeType") ? item["mimeType"].get<std::string>() : "text/plain";
        resource.category = category;
        resource.filePath = tocDir + "/" + name + ".md";

        if (m_resourceMap.find(resource.uri) != m_resourceMap.end())
        {
            Log("[MCP] Warning: Duplicate resource URI in TOC file: %s", resource.uri.c_str());
            continue;
        }

        m_resources.push_back(resource);
        m_resourceMap[resource.uri] = resource;
    }
}

bool McpServer::ReadFileContents(const std::string& filePath, std::string& content)
{
    content.clear();
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        Log("[MCP] Warning: Failed to open resource file: %s", filePath.c_str());
        return false;
    }

    std::streamoff file_size = file.tellg();
    if (file_size < 0)
    {
        Log("[MCP] Warning: Failed to read resource file: %s", filePath.c_str());
        return false;
    }

    content.resize((size_t)file_size);
    file.seekg(0, std::ios::beg);
    if (!file || (!content.empty() && !file.read(&content[0], (std::streamsize)content.size())))
    {
        Log("[MCP] Warning: Failed to read resource file: %s", filePath.c_str());
        content.clear();
        return false;
    }

    return true;
}

void McpServer::HandleResourcesList(const json& request)
{
    const json& id = request["id"];

    json resources = json::array();

    for (const ResourceInfo& resource : m_resources)
    {
        json resourceJson;
        resourceJson["uri"] = resource.uri;
        resourceJson["name"] = resource.title;
        resourceJson["title"] = resource.title;
        resourceJson["description"] = resource.description;
        resourceJson["mimeType"] = resource.mimeType;

        resources.push_back(resourceJson);
    }

    json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = {
        {"resources", resources}
    };

    SendResponse(response);
}

void McpServer::HandleResourceTemplatesList(const json& request)
{
    json response;
    response["jsonrpc"] = "2.0";
    response["id"] = request["id"];
    response["result"] = {
        {"resourceTemplates", json::array()}
    };

    SendResponse(response);
}

void McpServer::HandleResourcesRead(const json& request)
{
    const json& id = request["id"];

    if (!request.contains("params") || !request["params"].contains("uri") || !request["params"]["uri"].is_string())
    {
        SendError(id, MCP_ERROR_INVALID_PARAMS, "Invalid params: uri must be a string");
        return;
    }

    std::string uri = request["params"]["uri"];

    std::map<std::string, ResourceInfo>::const_iterator it = m_resourceMap.find(uri);
    if (it == m_resourceMap.end())
    {
        SendError(id, MCP_ERROR_RESOURCE_NOT_FOUND, "Resource not found", {{"uri", uri}});
        return;
    }

    const ResourceInfo& resource = it->second;
    std::string content;

    if (!ReadFileContents(resource.filePath, content))
    {
        SendError(id, MCP_ERROR_INTERNAL, "Failed to read resource", {{"uri", uri}});
        return;
    }

    json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = {
        {"contents", json::array({
            {
                {"uri", resource.uri},
                {"mimeType", resource.mimeType},
                {"text", content}
            }
        })}
    };

    SendResponse(response);
}

#endif /* GEARSF7000_ENABLE_MCP */
