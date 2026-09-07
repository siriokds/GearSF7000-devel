/*
 * Compile-time switch for the optional GearSF7000 MCP server.
 *
 * The normal desktop emulator intentionally builds with MCP disabled. Enable
 * it only in an explicit MCP build with -DGEARSF7000_ENABLE_MCP=1.
 */

#ifndef GEARSF7000_ENABLE_MCP
#define GEARSF7000_ENABLE_MCP 0
#endif

#if (GEARSF7000_ENABLE_MCP != 0) && (GEARSF7000_ENABLE_MCP != 1)
#error "GEARSF7000_ENABLE_MCP must be 0 or 1"
#endif
