/* GearSF7000 MCP manager. Network/stdio threads never access the core. */
#include "mcp_config.h"
#if GEARSF7000_ENABLE_MCP
#ifndef MCP_MANAGER_H
#define MCP_MANAGER_H

#include "mcp_server.h"
#include "mcp_transport.h"
#include "mcp_debug_adapter.h"

#include <typeinfo>

inline bool g_mcp_stdio_mode = false;

enum McpTransportMode { MCP_TRANSPORT_STDIO = 0, MCP_TRANSPORT_TCP = 1 };
enum McpStartSource { MCP_START_NONE = 0, MCP_START_GUI = 1, MCP_START_COMMAND_LINE = 2 };

class McpManager
{
public:
    McpManager() : m_adapter(NULL), m_server(NULL), m_mode(MCP_TRANSPORT_STDIO), m_startSource(MCP_START_NONE), m_port(7777), m_address("127.0.0.1") {}
    ~McpManager() { Stop(); SafeDelete(m_adapter); }

    void Init(GearSF7000Core* core) { m_adapter = new DebugAdapter(core); }
    DebugAdapter* GetAdapter() const { return m_adapter; }
    void SetTransportMode(McpTransportMode mode, int port = 7777, const char* address = "127.0.0.1")
    {
        m_mode = mode;
        m_port = port > 0 && port <= 65535 ? port : 7777;
        m_address = address && address[0] ? address : "127.0.0.1";
    }
    void Start(McpStartSource source = MCP_START_GUI)
    {
        if (m_server && m_server->IsRunning()) return;
        SafeDelete(m_server);
        m_commands.Clear();
        m_responses.Reset();
        McpTransportInterface* transport = NULL;
        g_mcp_stdio_mode = m_mode == MCP_TRANSPORT_STDIO;
        if (m_mode == MCP_TRANSPORT_TCP) transport = new HttpTransport(m_address, m_port);
        else transport = new StdioTransport();
        m_server = new McpServer(transport, *m_adapter, m_commands, m_responses);
        m_startSource = source;
        m_server->Start();
    }
    void Stop()
    {
        SafeDelete(m_server);
        m_commands.Clear();
        g_mcp_stdio_mode = false;
        m_startSource = MCP_START_NONE;
    }
    bool IsRunning() const { return m_server && m_server->IsRunning(); }
    int GetTransportMode() const { return (int)m_mode; }
    McpStartSource GetStartSource() const { return m_startSource; }
    int GetPort() const { return m_port; }
    const std::string& GetAddress() const { return m_address; }
    McpTransportStatus GetTransportStatus() const
    {
        return m_server ? m_server->GetTransportStatus() : McpTransportStatus();
    }
    // Tools run here, on the emulation thread. The network thread guards
    // itself against exceptions with an explicit try - but HandleLine only
    // queues, so nothing that a tool throws was ever caught: it unwound out of
    // emu_update(), through the main loop, and into std::terminate, which
    // aborts the process. The port disappears and there is nothing to read
    // afterwards. A malformed argument, a json type mismatch or a failed
    // allocation must not be able to do that.
    void PumpCommands()
    {
        if (!m_server) return;
        DebugCommand* command = NULL;
        while ((command = m_commands.Pop()) != NULL)
        {
            DebugResponse* response = new DebugResponse();
            response->requestId = command->requestId;
            response->requestToken = command->requestToken;

            try
            {
                response->result = m_server->ExecuteCommand(command->toolName,
                                                            command->arguments);
            }
            catch (const std::exception& exception)
            {
                // Logged as well as returned: a client sees the failure, and
                // whoever is watching the emulator sees which tool did it and
                // with what, which is what a crash left no trace of.
                Log("[MCP] Tool '%s' threw %s: %s",
                    command->toolName.c_str(), typeid(exception).name(),
                    exception.what());
                Log("[MCP] Arguments were: %s",
                    command->arguments.dump().c_str());
                response->result = {
                    {"error", "tool_threw_exception"},
                    {"tool", command->toolName},
                    {"exception", exception.what()}
                };
            }
            catch (...)
            {
                Log("[MCP] Tool '%s' threw a non-standard exception",
                    command->toolName.c_str());
                Log("[MCP] Arguments were: %s",
                    command->arguments.dump().c_str());
                response->result = {
                    {"error", "tool_threw_unknown_exception"},
                    {"tool", command->toolName}
                };
            }

            response->isToolError = response->result.contains("error");
            if (response->isToolError) response->errorMessage = response->result.value("error", "MCP tool failed");
            m_responses.Push(response);
            SafeDelete(command);
        }
    }
private:
    DebugAdapter* m_adapter;
    McpServer* m_server;
    CommandQueue m_commands;
    ResponseQueue m_responses;
    McpTransportMode m_mode;
    McpStartSource m_startSource;
    int m_port;
    std::string m_address;
};
#endif
#endif
