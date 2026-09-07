/*
 * GearSF7000 - CPU-independent debugger context
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_CPU_DEBUG_CONTEXT_H
#define GEARSF7000_CPU_DEBUG_CONTEXT_H

#include "definitions.h"

struct CpuDebugSnapshot
{
    u16 programCounter = 0;
    u64 elapsedTStates = 0;
};

// Minimal contract needed by memory and device event producers. It keeps the
// debugger separate from private details of the CLK Z80 template.
class CpuDebugContext
{
public:
    virtual ~CpuDebugContext() = default;
    virtual CpuDebugSnapshot GetCpuDebugSnapshot() const = 0;
    virtual void RequestCpuDebugPause() = 0;
};

#endif
