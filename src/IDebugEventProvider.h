// SPDX-License-Identifier: GPL-3.0-or-later
// Component descriptions are produced during machine composition, never per cycle.
#ifndef I_DEBUG_EVENT_PROVIDER_H
#define I_DEBUG_EVENT_PROVIDER_H

#include "DebugEvents.h"

struct DebugTargetDefinition
{
    u32 id = 0;
    std::string name;
    u8 bitWidth = 8;
};

struct DebugSpaceDefinition
{
    DebugSpaceId id = DebugCommonSpace::Unknown;
    std::string key;
    std::string name;
    u32 targetMin = 0;
    u32 targetMax = 0;
    u8 supportedAccesses = DebugEventAccess_None;
    u8 beforeValueAccesses = DebugEventAccess_None;
    u8 afterValueAccesses = DebugEventAccess_None;
    u8 defaultBitWidth = 8;
    u8 capabilities = DebugDescriptorCapability_EventsPublished;
    std::vector<DebugTargetDefinition> targets;
};

// Ready-made rules supplied by a device. The preset addresses one of the
// provider's own spaces, so the catalog can bind it to any instance key.
struct DebugRulePresetDefinition
{
    std::string key;
    std::string name;
    std::string spaceKey;
    u32 targetStart = 0;
    u32 targetEnd = 0;
    u8 accessMask = DebugEventAccess_None;
    bool useValueCondition = false;
    DebugValueCondition valueCondition = DebugValueCondition::Any;
    u64 valueMask = 0;
    u64 beforeValueExpected = 0;
    u64 valueExpected = 0;
    u8 actions = DebugEventAction_Log;
};

struct DebugDeviceDefinition
{
    std::string key;
    std::string parentKey;
    std::string typeKey;
    std::string name;
    std::vector<DebugSpaceDefinition> spaces;
    std::vector<DebugRulePresetDefinition> presets;
    u8 traits = DebugDeviceTrait_None;
    DebugEventFormatter formatter = nullptr;
};

class IDebugEventProvider
{
public:
    virtual ~IDebugEventProvider() = default;
    // Values are owned: neither provider lifetime nor temporary labels can
    // invalidate the catalog once registration has copied the description.
    virtual DebugDeviceDefinition DescribeDebugDevice() const = 0;
};
#endif
