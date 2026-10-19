// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SGM_DEVICE_H
#define SGM_DEVICE_H

#include "IDebugEventProvider.h"
#include "definitions.h"

// Control state only: Memory owns the RAM and Audio owns the AY renderer.
class SGMDevice final : public IDebugEventProvider
{
public:
    void Configure(bool present) { m_present = present; Reset(); }
    void Reset() { m_lower = m_upper = false; }
    bool IsPresent() const { return m_present; }
    bool LowerEnabled() const { return m_present && m_lower; }
    bool UpperEnabled() const { return m_present && m_upper; }
    bool ResolvesRam(u16 address) const
    {
        return address < 0x2000 ? LowerEnabled()
            : address < 0x8000 && UpperEnabled();
    }
    void Restore(bool lower, bool upper) { m_lower = lower; m_upper = upper; }
    bool WriteControl(u8 port, u8 value)
    {
        if (!m_present) return false;
        if (port == 0x53) m_upper = (value & 1) != 0;
        else if (port == 0x7F) m_lower = (value & 2) == 0;
        else return false;
        return true;
    }
    DebugDeviceDefinition DescribeDebugDevice() const override
    {
        DebugDeviceDefinition device;
        device.key = "event.coleco.sgm";
        device.parentKey = "event.coleco";
        device.typeKey = "opcode.sgm";
        device.name = "Super Game Module";
        device.traits = DebugDeviceTrait_External;
        DebugSpaceDefinition state;
        state.id = DebugCommonSpace::State;
        state.key = "state";
        state.name = "RAM mapping";
        state.targetMax = 1;
        state.supportedAccesses = DebugEventAccess_Event;
        state.beforeValueAccesses = state.afterValueAccesses = DebugEventAccess_Event;
        state.targets.push_back({0, "Lower RAM (BIOS overlay)", 1});
        state.targets.push_back({1, "Upper RAM", 1});
        device.spaces.push_back(std::move(state));
        DebugSpaceDefinition ram;
        ram.id = DebugCommonSpace::Memory;
        ram.key = "ram";
        ram.name = "SGM RAM (physical)";
        ram.targetMax = 0x7FFF;
        ram.supportedAccesses = DebugEventAccess_Read | DebugEventAccess_Write;
        ram.beforeValueAccesses = DebugEventAccess_Write;
        ram.afterValueAccesses = ram.supportedAccesses;
        device.spaces.push_back(std::move(ram));
        return device;
    }
private:
    bool m_present = false;
    bool m_lower = false;
    bool m_upper = false;
};
#endif
