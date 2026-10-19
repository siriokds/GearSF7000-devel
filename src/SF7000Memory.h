/*
 * GearSF7000 - SF-7000 memory subsystem debug description
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_SF7000_MEMORY_H
#define GEARSF7000_SF7000_MEMORY_H

#include "IDebugEventProvider.h"

class SF7000MemoryDevice final : public IDebugEventProvider
{
public:
    enum Space : DebugSpaceId
    {
        RamSpace = DebugCommonSpace::CustomFirst,
        IplSpace
    };

    enum State : u8
    {
        IplVisible = 0,
        StateCount
    };

    DebugDeviceDefinition DescribeDebugDevice() const override;
};

#endif
