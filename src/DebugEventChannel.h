// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DEBUG_EVENT_CHANNEL_H
#define DEBUG_EVENT_CHANNEL_H
#include "DebugEvents.h"

// Numeric publication endpoint bound once at composition. No descriptor
// building, string lookup or allocation is introduced in Publish.
class DebugEventChannel
{
public:
    DebugEventChannel(DebugEventManager& manager, DebugDeviceId device,
                      DebugSpaceId space)
        : m_manager(manager), m_device(device), m_space(space) {}

    bool Publish(u32 target, u8 access, u64 before, bool beforeKnown,
                 u64 after, bool afterKnown, u16 pc, u64 clock,
                 DebugEventSource source, DebugEventPhase phase,
                 const DebugEventRasterContext* raster = nullptr) const
    {
        return m_manager.EvaluateProbe({m_device, m_space, target}, access,
            before, beforeKnown, after, afterKnown, source, phase, pc, clock, raster);
    }
private:
    DebugEventManager& m_manager;
    DebugDeviceId m_device;
    DebugSpaceId m_space;
};
#endif
