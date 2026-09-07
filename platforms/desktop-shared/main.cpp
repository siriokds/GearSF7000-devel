/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026  Saverio Russo

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

#include "../../src/gearsf7000.h"
#include "../../src/build_info.h"
#include "application.h"
#include "application_headless.h"
#include "mcp/mcp_config.h"
#if GEARSF7000_ENABLE_MCP
#include "mcp/mcp_manager.h"
#include "emu.h"
#endif

int main(int argc, char* argv[])
{
    char* rom_file = NULL;
    char* symbol_file = NULL;
    bool show_usage = false;
    int ret = 0;
    bool headless = false;

#if GEARSF7000_ENABLE_MCP
    bool mcp_stdio = false;
    bool mcp_http = false;
    int mcp_port = 7777;
    const char* mcp_address = "127.0.0.1";
    // Matches g_mcp_router_enabled's default - see mcp_server.h for what the
    // two shapes mean. Stated here as a named value rather than left implicit
    // so the default is readable at the point it is decided.
    bool mcp_tools_grouped = false;
#endif

    for (int i = 1; i < argc; i++)
    {
        if ((strcmp(argv[i], "-h") == 0) || (strcmp(argv[i], "-?") == 0) ||
            (strcmp(argv[i], "--help") == 0) || (strcmp(argv[i], "/?") == 0))
        {
            show_usage = true;
            ret = 0;
        } 
        else if ((strcmp(argv[i], "-v") == 0) || (strcmp(argv[i], "--version") == 0))
        {
            printf("%s\n", GEARSF7000_TITLE_ASCII);
            printf("Build: %s\n", build_info_version());
            printf("Compiled: %s\n", build_info_timestamp());
            printf("Author: Saverio Russo\n");
            return 0;
        }
#if GEARSF7000_ENABLE_MCP
        else if (strcmp(argv[i], "--headless") == 0)
        {
            headless = true;
        }
        else if (strcmp(argv[i], "--mcp-stdio") == 0)
        {
            mcp_stdio = true;
            // MCP stdio is a protocol stream: suppress debug logging before
            // application_init has a chance to print anything.
            g_mcp_stdio_mode = true;
        }
        else if (strcmp(argv[i], "--mcp-http") == 0)
        {
            mcp_http = true;
        }
        else if (strcmp(argv[i], "--mcp-http-port") == 0)
        {
            if (++i >= argc)
            {
                fprintf(stderr, "Missing value for --mcp-http-port\n");
                return -1;
            }
            char* end = NULL;
            long value = strtol(argv[i], &end, 10);
            if (!end || *end != '\0' || value < 1 || value > 65535)
            {
                fprintf(stderr, "Invalid MCP HTTP port: %s\n", argv[i]);
                return -1;
            }
            mcp_port = (int)value;
        }
        else if (strcmp(argv[i], "--mcp-tools") == 0)
        {
            if (++i >= argc)
            {
                fprintf(stderr, "--mcp-tools needs a value: flat or grouped\n");
                return -1;
            }
            if (strcmp(argv[i], "flat") == 0)
                mcp_tools_grouped = false;
            else if (strcmp(argv[i], "grouped") == 0)
                mcp_tools_grouped = true;
            else
            {
                fprintf(stderr, "Unknown --mcp-tools value: %s (use flat or grouped)\n",
                        argv[i]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--mcp-http-address") == 0)
        {
            if (++i >= argc || argv[i][0] == '\0')
            {
                fprintf(stderr, "Missing value for --mcp-http-address\n");
                return -1;
            }
            mcp_address = argv[i];
        }
#endif
        else if (argv[i][0] == '-')
        {
            show_usage = true;
            ret = -1;
        }
        else if (!rom_file)
        {
            rom_file = argv[i];
        }
        else if (!symbol_file)
        {
            symbol_file = argv[i];
        }
        else
        {
            show_usage = true;
            ret = -1;
        }
    }

#if GEARSF7000_ENABLE_MCP
    if (mcp_stdio && mcp_http)
    {
        fprintf(stderr, "Use either --mcp-stdio or --mcp-http, not both.\n");
        return -1;
    }

    // Once, before either path can start a server - headless and windowed
    // both do, further down, and the server reads this when it answers
    // tools/list.
    g_mcp_router_enabled = mcp_tools_grouped;
#endif

    if (show_usage)
    {
        printf("Usage: %s [options] [rom_file] [symbol_file]\n", argv[0]);
#if GEARSF7000_ENABLE_MCP
        printf("      --mcp-stdio                 Start MCP over standard input/output\n");
        printf("      --mcp-http                  Start MCP HTTP endpoint\n");
        printf("      --mcp-http-address ADDRESS  Bind address (default 127.0.0.1)\n");
        printf("      --mcp-http-port PORT        HTTP port (default 7777)\n");
        printf("      --mcp-tools flat|grouped    List every tool, or group them behind\n");
        printf("                                  categories (default flat)\n");
        printf("      --headless                  Run with no window (needs --mcp-stdio or --mcp-http)\n");
#endif
        return ret;
    }

#if GEARSF7000_ENABLE_MCP
    if (headless && !mcp_stdio && !mcp_http)
    {
        printf("--headless needs an MCP transport: add --mcp-stdio or --mcp-http.\n");
        printf("Without one there would be no way to talk to the emulator.\n");
        return 1;
    }
#else
    if (headless)
    {
        printf("--headless needs MCP, and this build was made without it.\n");
        return 1;
    }
#endif

    if (headless)
    {
        // No window, so no audio device either: nobody is listening, and an
        // unattended run should not take over the sound hardware.
        ret = application_headless_init(rom_file, symbol_file, false);

        if (ret == 0)
        {
#if GEARSF7000_ENABLE_MCP
            emu_mcp_set_transport(mcp_http ? MCP_TRANSPORT_TCP : MCP_TRANSPORT_STDIO, mcp_port, mcp_address);
            emu_mcp_start_from_command_line();
#endif
            application_headless_mainloop();
        }

        application_headless_destroy();
        return ret;
    }

    ret = application_init(rom_file, symbol_file);

    if (ret == 0)
    {
#if GEARSF7000_ENABLE_MCP
        if (mcp_stdio || mcp_http)
        {
            emu_mcp_set_transport(mcp_http ? MCP_TRANSPORT_TCP : MCP_TRANSPORT_STDIO, mcp_port, mcp_address);
            emu_mcp_start_from_command_line();
        }
#endif
        application_mainloop();
    }

    application_destroy();    

    return ret;
}
