// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TMS99XX_DEBUG_EVENTS_H
#define TMS99XX_DEBUG_EVENTS_H
#include "IDebugEventProvider.h"
// Shared schema for both original TMS variants, including unrestricted raster targets.
inline constexpr size_t kTms99xxDebugSpaceCount = 5;
extern const DebugSpaceDescriptor kTms99xxDebugSpaces[kTms99xxDebugSpaceCount];

namespace Tms99xxDebugTarget
{
    enum State : u16
    {
        AddressCounter = 0,
        ControlLatch,
        ReadBuffer,
        Status,
        IRQ,
        VideoMode,
        CpuPortLatchReplacement,
        ControlLatchValue
    };
}

DebugDeviceDefinition DescribeTms99xxDebugEvents();
#endif
