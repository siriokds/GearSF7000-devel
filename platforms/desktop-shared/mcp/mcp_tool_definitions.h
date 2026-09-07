#pragma once

#include "mcp_config.h"

#if GEARSF7000_ENABLE_MCP

#include "json.hpp"

using json = nlohmann::json;

void AddMcpToolDefinitionsCore(json& tools);
void AddMcpToolDefinitionsDebug(json& tools);
void AddMcpToolDefinitionsInput(json& tools);

#endif /* GEARSF7000_ENABLE_MCP */
