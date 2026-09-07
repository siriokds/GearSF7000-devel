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
#include "mcp_tool_definitions.h"
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

    AddMcpToolDefinitionsCore(tools);
    AddMcpToolDefinitionsDebug(tools);
    AddMcpToolDefinitionsInput(tools);

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
