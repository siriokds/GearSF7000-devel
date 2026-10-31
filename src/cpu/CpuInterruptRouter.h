/*
 * GearSF7000 - switchable CPU interrupt-line router
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef GEARSF7000_CPU_INTERRUPT_ROUTER_H
#define GEARSF7000_CPU_INTERRUPT_ROUTER_H

#include "cpu/CpuInterruptLines.h"

// Gives machine devices a stable interrupt endpoint while allowing the core
// to replace the CPU implementation behind it. The router owns no target.
class CpuInterruptRouter final : public CpuInterruptLines
{
public:
    void Attach(CpuInterruptLines* target)
    {
        if (target == m_target)
            return;
        if (m_target)
        {
            m_target->SetMaskableInterruptLine(false);
            m_target->SetNonMaskableInterruptLevel(false);
        }
        m_target = target;
        if (m_target)
        {
            m_target->SetMaskableInterruptLine(m_intLine);
            m_target->SetNonMaskableInterruptLevel(m_nmiLevel);
        }
    }

    CpuInterruptLines* GetTarget() const
    {
        return m_target;
    }

    void SetMaskableInterruptLine(bool asserted) override
    {
        m_intLine = asserted;
        if (m_target)
            m_target->SetMaskableInterruptLine(asserted);
    }

    void PulseNonMaskableInterrupt() override
    {
        if (m_target)
            m_target->PulseNonMaskableInterrupt();
    }

    void SetNonMaskableInterruptLevel(bool asserted) override
    {
        m_nmiLevel = asserted;
        if (m_target)
            m_target->SetNonMaskableInterruptLevel(asserted);
    }

private:
    CpuInterruptLines* m_target = nullptr;
    bool m_intLine = false;
    bool m_nmiLevel = false;
};

#endif
