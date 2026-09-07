/*
 * GearSF7000 - optional CPU save-state capability
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_CPU_STATE_PERSISTENCE_H
#define GEARSF7000_CPU_STATE_PERSISTENCE_H

#include <iosfwd>

// Kept separate from CpuExecution because a cycle-accurate CPU may execute
// correctly before its complete micro-operation state can be serialized.
class CpuStatePersistence
{
public:
    virtual ~CpuStatePersistence() = default;

    virtual bool SaveCpuState(std::ostream& stream) = 0;
    virtual bool LoadCpuState(std::istream& stream) = 0;
};

#endif
