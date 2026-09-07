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

#include "mcp_config.h"

#if GEARSF7000_ENABLE_MCP

#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include "json.hpp"
#include "mcp_transport.h"
#include "mcp_debug_adapter.h"
#include "mcp_tool_registry.h"

using json = nlohmann::json;

#define MCP_MAX_PENDING_COMMANDS 64

// How the ~130 tools are presented to a client.
//
// false (the default): every tool is listed by tools/list, flat. A client
// that copes with a long list finds what it needs in one step.
//
// true: only the direct tools are listed, plus five navigation tools
// (list_tool_categories, get_category_tools, get_tool_info, search_tools,
// execute_tool) through which the rest are discovered by category. Costs a
// round trip, and is what a client with a cap on tool count - or one paying
// context for every description - needs instead.
//
// Presentation only: it decides what tools/list returns and whether the five
// navigation tools exist. HandleToolCall reaches the real tools the same way
// either way, so there are not two execution paths to keep in step.
//
// "Router" is Gearsystem's word for this, inherited with the mechanism; it is
// not an MCP concept. The command line says --mcp-tools flat|grouped, which
// explains itself to whoever types it.
extern bool g_mcp_router_enabled;

enum McpErrorCode
{
    MCP_ERROR_PARSE = -32700,
    MCP_ERROR_INVALID_REQUEST = -32600,
    MCP_ERROR_METHOD_NOT_FOUND = -32601,
    MCP_ERROR_INVALID_PARAMS = -32602,
    MCP_ERROR_INTERNAL = -32603,
    MCP_ERROR_RESOURCE_NOT_FOUND = -32002
};

struct ResourceInfo
{
    std::string uri;
    std::string title;
    std::string description;
    std::string mimeType;
    std::string category;
    std::string filePath;
};

struct DebugCommand
{
    json requestId;
    std::string toolName;
    json arguments;
    McpRequestToken requestToken = MCP_REQUEST_TOKEN_NONE;
};

struct DebugResponse
{
    json requestId;
    McpRequestToken requestToken = MCP_REQUEST_TOKEN_NONE;
    bool isError = false;
    bool isToolError = false;
    int errorCode = 0;
    std::string errorMessage;
    json result;
};

class CommandQueue
{
public:
    CommandQueue()
    {
        m_pending = 0;
    }

    bool Push(DebugCommand* cmd)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pending >= MCP_MAX_PENDING_COMMANDS)
            return false;

        m_queue.push(cmd);
        m_pending++;
        return true;
    }

    DebugCommand* Pop()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty())
            return NULL;
        DebugCommand* cmd = m_queue.front();
        m_queue.pop();
        return cmd;
    }

    DebugCommand* Pop(const char* tool_name)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        DebugCommand* match = NULL;
        size_t count = m_queue.size();

        for (size_t i = 0; i < count; i++)
        {
            DebugCommand* cmd = m_queue.front();
            m_queue.pop();

            std::string normalized_name = cmd->toolName;
            std::replace(normalized_name.begin(), normalized_name.end(), '.', '_');

            if (!match && normalized_name == tool_name)
                match = cmd;
            else
                m_queue.push(cmd);
        }

        return match;
    }

    void Complete()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pending > 0)
            m_pending--;
    }

    size_t Pending()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pending;
    }

    size_t Queued()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_queue.empty())
        {
            SafeDelete(m_queue.front());
            m_queue.pop();
        }
        m_pending = 0;
    }

private:
    std::queue<DebugCommand*> m_queue;
    std::mutex m_mutex;
    size_t m_pending;
};

class ResponseQueue
{
public:
    void Push(DebugResponse* resp)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(resp);
        m_cv.notify_one();
    }

    DebugResponse* WaitAndPop()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return !m_queue.empty() || !m_running; });

        if (m_queue.empty())
            return NULL;

        DebugResponse* resp = m_queue.front();
        m_queue.pop();
        return resp;
    }

    void Stop()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = false;
        while (!m_queue.empty())
        {
            SafeDelete(m_queue.front());
            m_queue.pop();
        }
        m_cv.notify_all();
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_queue.empty())
        {
            SafeDelete(m_queue.front());
            m_queue.pop();
        }
        m_running = true;
    }

    size_t Queued()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

private:
    std::queue<DebugResponse*> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_running = true;
};

class McpServer
{
public:
    McpServer(McpTransportInterface* transport,
              DebugAdapter& debugAdapter,
              CommandQueue& commandQueue,
              ResponseQueue& responseQueue)
        : m_debugAdapter(debugAdapter),
          m_commandQueue(commandQueue),
          m_responseQueue(responseQueue)
    {
        m_transport = transport;
        m_running = false;
        m_initialized = false;
    }

    ~McpServer()
    {
        Stop();
        SafeDelete(m_transport);
    }

    void Start()
    {
        if (m_running.load())
            return;

        LoadResources();
        m_initialized = false;
        m_running.store(true);
        m_readerThread = std::thread(&McpServer::ReaderLoop, this);
        m_thread = std::thread(&McpServer::Run, this);
    }

    void Stop()
    {
        std::lock_guard<std::mutex> lock(m_stopMutex);
        m_running.store(false);
        m_transport->close();
        m_responseQueue.Stop();

        if (m_readerThread.joinable())
            m_readerThread.join();
        if (m_thread.joinable())
            m_thread.join();
    }

    bool IsRunning() const
    {
        return m_running.load();
    }

    McpTransportStatus GetTransportStatus() const
    {
        return m_transport->get_status();
    }

    json ExecuteCommand(const std::string& toolName, const json& arguments);

    void ReaderLoop();

private:
    void Run();
    void HandleLine(const std::string& line, McpRequestToken request_token);
    void HandleInitialize(const json& request);
    void HandleToolsList(const json& request);
    void HandleToolsCall(const json& request);
    void HandleResourcesList(const json& request);
    void HandleResourceTemplatesList(const json& request);
    void HandleResourcesRead(const json& request);

    json BuildToolList();
    void EnsureToolRegistry();
    void AddRouterTools(json& tools);
    json HandleRouterListCategories();
    json HandleRouterGetCategoryTools(const json& arguments);
    json HandleRouterGetToolInfo(const json& arguments);
    json HandleRouterSearchTools(const json& arguments);
    void SendToolResult(const json& id, const json& result);

    void LoadResources();
    void LoadResourcesFromCategory(const std::string& category, const std::string& tocPath);
    bool ReadFileContents(const std::string& filePath, std::string& content);

    void SendResponse(const json& response, McpRequestToken request_token = MCP_REQUEST_TOKEN_NONE);
    void SendError(const json& id, int code, const std::string& message,
                   const json& data = json::object(), McpRequestToken request_token = MCP_REQUEST_TOKEN_NONE);

    McpTransportInterface* m_transport;
    DebugAdapter& m_debugAdapter;
    CommandQueue& m_commandQueue;
    ResponseQueue& m_responseQueue;
    std::thread m_thread;
    std::thread m_readerThread;
    std::mutex m_stopMutex;
    std::atomic<bool> m_running;
    std::atomic<bool> m_initialized;
    // Used only by ReaderLoop and its synchronous handlers. Asynchronous tool
    // responses carry their own token in DebugResponse instead.
    McpRequestToken m_readerRequestToken = MCP_REQUEST_TOKEN_NONE;
    McpToolRegistry m_toolRegistry;
    std::vector<ResourceInfo> m_resources;
    std::map<std::string, ResourceInfo> m_resourceMap;
};

#endif /* MCP_SERVER_H */

#endif /* GEARSF7000_ENABLE_MCP */
