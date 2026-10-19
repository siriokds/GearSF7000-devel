/*
 * GearSF7000 - VDP interrupt output to CPU pin adapter
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_CPU_VDP_INTERRUPT_ADAPTER_H
#define GEARSF7000_CPU_VDP_INTERRUPT_ADAPTER_H

#include "cpu/CpuInterruptLines.h"
#include "VdpLineType.h"

// The VDP drives one level on its /INT output and never learns which CPU pin
// it reaches. This is the wire on the motherboard: the system bus says
// whether it ends on /INT (Sega boards, MSX) or /NMI (ColecoVision), and the
// adapter delivers the same level to that pin through the shared router.
class VdpInterruptAdapter final : public CpuInterruptLines
{
public:
    void Attach(CpuInterruptLines* target)
    {
        if (target == m_target)
            return;
        // Replacing the CPU does not toggle a device-owned input (CTC).
        if (m_type != VdpLineType::VDP_DEVICE) Drive(false);
        m_target = target;
        if (m_type != VdpLineType::VDP_DEVICE) Drive(m_level);
    }

    void SetLineType(VdpLineType type)
    {
        if (type == m_type)
            return;
        // Move the level from the old pin to the new one without leaving a
        // request behind on the pin that is no longer wired.
        Drive(false);
        m_type = type;
        Drive(m_level);
    }

    // Device target is owned by the board. Install before selecting VDP_DEVICE.
    void AttachDevice(CpuInterruptLines* target)
    {
        if (target == m_deviceTarget) return;
        if (m_type == VdpLineType::VDP_DEVICE) Drive(false);
        m_deviceTarget = target;
        if (m_type == VdpLineType::VDP_DEVICE) Drive(m_level);
    }

    VdpLineType GetLineType() const { return m_type; }
    bool GetLevel() const { return m_level; }

    // Machine reset: the VDP starts with its output released.
    void Reset()
    {
        m_level = false;
        Drive(false);
    }

    // After a state load the CPU already holds the restored pin; only the
    // remembered level has to agree with it.
    void SyncLevel(bool level) { m_level = level; }

    // The VDP calls this; the name is the VDP's view of its own output.
    void SetMaskableInterruptLine(bool asserted) override
    {
        m_level = asserted;
        Drive(asserted);
    }

    void PulseNonMaskableInterrupt() override
    {
        if (m_target)
            m_target->PulseNonMaskableInterrupt();
    }

    void SetNonMaskableInterruptLevel(bool asserted) override
    {
        if (m_target)
            m_target->SetNonMaskableInterruptLevel(asserted);
    }

private:
    void Drive(bool level)
    {
        if (m_type == VdpLineType::VDP_DEVICE)
        {
            if (m_deviceTarget) m_deviceTarget->SetMaskableInterruptLine(level);
            return;
        }
        if (!m_target) return;
        if (m_type == VdpLineType::VDP_NMI)
            m_target->SetNonMaskableInterruptLevel(level);
        else
            m_target->SetMaskableInterruptLine(level);
    }

    CpuInterruptLines* m_target = nullptr;
    CpuInterruptLines* m_deviceTarget = nullptr;
    VdpLineType m_type = VdpLineType::VDP_INT;
    bool m_level = false;
};

#endif
