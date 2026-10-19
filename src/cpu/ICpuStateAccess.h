/*
 * GearSF7000 - CPU-independent state access
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_I_CPU_STATE_ACCESS_H
#define GEARSF7000_I_CPU_STATE_ACCESS_H

#include <cstdint>

class ICpuStateAccess
{
public:
    virtual ~ICpuStateAccess() = default;

    virtual std::uint16_t GetProgramCounter() const = 0;
};

#endif
