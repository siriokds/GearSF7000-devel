#pragma once

#include "mcp_config.h"
// The feature switches decide which tools exist; IDE builds get them only here.
#include "../../../src/definitions.h"

#if GEARSF7000_ENABLE_MCP

#include "json.hpp"

using json = nlohmann::json;

void AddMcpToolDefinitionsCore(json& tools);
void AddMcpToolDefinitionsDebug(json& tools);
void AddMcpToolDefinitionsInput(json& tools);

#endif /* GEARSF7000_ENABLE_MCP */
