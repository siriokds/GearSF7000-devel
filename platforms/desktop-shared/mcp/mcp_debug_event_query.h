#ifndef MCP_DEBUG_EVENT_QUERY_H
#define MCP_DEBUG_EVENT_QUERY_H

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>
#include "../../../src/DebugEvents.h"

struct McpDebugEventQuery
{
    size_t limit = 128;
    bool newestFirst = false;
    bool hasSequenceFrom = false; u64 sequenceFrom = 0;
    bool hasSequenceTo = false; u64 sequenceTo = 0;
    bool hasClockFrom = false; u64 clockFrom = 0;
    bool hasClockTo = false; u64 clockTo = 0;
    bool hasPcStart = false; u16 pcStart = 0;
    bool hasPcEnd = false; u16 pcEnd = 0;
    bool hasTargetStart = false; u32 targetStart = 0;
    bool hasTargetEnd = false; u32 targetEnd = 0;
    int category = -1;
    u8 accessMask = DebugEventAccess_None;
    std::string device;
    std::string space;
};

inline bool McpDebugEventMatches(const DebugEvent& event, const McpDebugEventQuery& query)
{
    if (query.hasSequenceFrom && event.sequence < query.sequenceFrom) return false;
    if (query.hasSequenceTo && event.sequence > query.sequenceTo) return false;
    if (query.hasClockFrom && event.clock < query.clockFrom) return false;
    if (query.hasClockTo && event.clock > query.clockTo) return false;
    if (query.hasPcStart && event.pc < query.pcStart) return false;
    if (query.hasPcEnd && event.pc > query.pcEnd) return false;
    const u32 target = event.schemaVersion >= 2 ? event.targetV2 : event.target;
    if (query.hasTargetStart && target < query.targetStart) return false;
    if (query.hasTargetEnd && target > query.targetEnd) return false;
    if (query.category >= 0 && event.category != static_cast<u8>(query.category)) return false;
    if (query.accessMask != DebugEventAccess_None && (event.access & query.accessMask) == 0) return false;
    if (!query.device.empty() && query.device != GetDebugDeviceName(event.device)) return false;
    if (!query.space.empty() && query.space != GetDebugSpaceName(event.space)) return false;
    return true;
}

inline size_t McpFilterDebugEvents(const std::vector<DebugEvent>& source,
                                   const McpDebugEventQuery& query,
                                   std::vector<DebugEvent>& output)
{
    output.clear();
    for (const DebugEvent& event : source)
        if (McpDebugEventMatches(event, query))
            output.push_back(event);

    const size_t matched = output.size();
    if (query.limit > 0 && output.size() > query.limit)
        output.erase(output.begin(), output.end() - static_cast<std::ptrdiff_t>(query.limit));
    if (query.newestFirst)
        std::reverse(output.begin(), output.end());
    return matched;
}

#endif
