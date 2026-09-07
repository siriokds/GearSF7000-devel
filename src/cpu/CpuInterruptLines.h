/*
 * GearSF7000 - CPU-independent interrupt lines
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_CPU_INTERRUPT_LINES_H
#define GEARSF7000_CPU_INTERRUPT_LINES_H

class CpuInterruptLines
{
public:
    virtual ~CpuInterruptLines() = default;

    // Z80 INT is level-triggered; devices must explicitly assert/deassert it.
    virtual void SetMaskableInterruptLine(bool asserted) = 0;

    // The SC-3000 reset key is wired to NMI. This operation represents one
    // clean active edge, irrespective of the concrete Z80 implementation.
    virtual void PulseNonMaskableInterrupt() = 0;
};

#endif
