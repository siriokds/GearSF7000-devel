// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#include "ControllerDevice.h"

#include "Memory.h"
#include "SaveStateStream.h"

#include <utility>

const char* GetControllerDeviceName(SystemControllerDevice type)
{
    switch (type)
    {
    case SystemControllerDevice::SmsPad: return "Master System pad";
    case SystemControllerDevice::MegaDrive3: return "Mega Drive pad (3 buttons)";
    case SystemControllerDevice::MegaDrive6: return "Mega Drive pad (6 buttons)";
    }
    return "unknown";
}

std::unique_ptr<ControllerDevice> ControllerDevice::Create(SystemControllerDevice type)
{
    switch (type)
    {
    case SystemControllerDevice::MegaDrive3: return std::make_unique<MegaDrive3Device>();
    case SystemControllerDevice::MegaDrive6: return std::make_unique<MegaDrive6Device>();
    case SystemControllerDevice::SmsPad: break;
    }
    return std::make_unique<SmsPadDevice>();
}

u8 ControllerDevice::Read(u8 smsLines, bool th, u64 tstates)
{
    const u8 lines = static_cast<u8>(Lines(static_cast<u8>(smsLines & 0x3F), th, tstates) & 0x3F);
    if (lines != m_lastLines)
    {
        Publish(TargetLines, m_lastLines, lines);
        m_lastLines = lines;
    }
    return lines;
}

void ControllerDevice::OnTh(bool th, u64 tstates)
{
    if (!UsesTh() || th == m_th)
        return;
    Publish(TargetTh, m_th, th);
    ThChanged(th, tstates);
    m_th = th;
}

void ControllerDevice::SetButton(u8 buttons, bool pressed)
{
    const u8 before = m_buttons;
    m_buttons = static_cast<u8>(pressed ? (m_buttons | buttons) : (m_buttons & ~buttons));
    if (UsesTh() && before != m_buttons)
        Publish(TargetButtons, before, m_buttons);
}

void ControllerDevice::Reset()
{
    m_th = true;
    m_lastLines = 0x3F;
}

void ControllerDevice::SaveState(StateWriter& w) const
{
    w.Bool(m_th);
}

void ControllerDevice::LoadState(StateReader& r)
{
    m_th = r.Bool();
}

void ControllerDevice::SetDebugKeys(std::string key, std::string parentKey)
{
    m_debugKey = std::move(key);
    m_debugParentKey = std::move(parentKey);
}

void ControllerDevice::BindDebugEvents(const DebugDeviceRegistry& registry)
{
    const DebugDeviceDescriptor* device = registry.FindDevice(m_debugKey.c_str());
    m_debugDevice = device != nullptr ? device->id : 0;
}

void ControllerDevice::Publish(StateTarget target, u64 before, u64 after)
{
    if (m_memory != nullptr && m_debugDevice != 0)
        m_memory->CheckDeviceStateChange({m_debugDevice, DebugCommonSpace::State, target},
                                         before, after);
}

DebugDeviceDefinition ControllerDevice::DescribeDebugDevice() const
{
    DebugDeviceDefinition device;
    device.key = m_debugKey;
    device.parentKey = m_debugParentKey;
    device.typeKey = TypeKey();
    device.name = GetControllerDeviceName(GetType());

    DebugSpaceDefinition state;
    state.id = DebugCommonSpace::State;
    state.key = "state";
    state.name = "Controller State";
    state.supportedAccesses = DebugEventAccess_Event;
    state.beforeValueAccesses = DebugEventAccess_Event;
    state.afterValueAccesses = DebugEventAccess_Event;
    state.capabilities = DebugDescriptorCapability_EventsPublished;
    state.targets.push_back({TargetLines, "Lines read (U D L R TL TR, low = on)", 6});
    if (UsesTh())
    {
        state.targets.push_back({TargetTh, "TH", 1});
        state.targets.push_back({TargetButtons, "Buttons (A Start X Y Z Mode)", 6});
    }
    if (HasCounter())
        state.targets.push_back({TargetCounter, "Phase (TH rising edges)", 2});
    state.targetMax = state.targets.back().id;
    device.spaces.push_back(std::move(state));
    return device;
}

void MegaDrive6Device::Reset()
{
    MegaDrive3Device::Reset();
    SetPhase(0);
    m_timerArmed = false;
    m_timerStart = 0;
    if (!m_modeLatched)
    {
        m_sixButtons = (m_buttons & ButtonMode) == 0;
        m_modeLatched = true;
    }
}

void MegaDrive6Device::SaveState(StateWriter& w) const
{
    MegaDrive3Device::SaveState(w);
    w.U8(m_phase);
    w.Bool(m_sixButtons);
    w.Bool(m_timerArmed);
    w.U64(m_timerStart);
}

void MegaDrive6Device::LoadState(StateReader& r)
{
    MegaDrive3Device::LoadState(r);
    m_phase = static_cast<u8>(r.U8() & 3);
    m_sixButtons = r.Bool();
    m_timerArmed = r.Bool();
    m_timerStart = r.U64();
}

void MegaDrive6Device::Expire(u64 tstates)
{
    if (m_timerArmed && (tstates < m_timerStart || tstates - m_timerStart > kTimeout))
    {
        m_timerArmed = false;
        SetPhase(0);
    }
}

void MegaDrive6Device::SetPhase(u8 phase)
{
    if (phase != m_phase)
        Publish(TargetCounter, m_phase, phase);
    m_phase = phase;
}

void MegaDrive6Device::ThChanged(bool th, u64 tstates)
{
    Expire(tstates);
    if (!th)
    {
        m_timerArmed = false;
        return;
    }
    if (!m_sixButtons)
        return;
    m_timerArmed = true;
    m_timerStart = tstates;
    SetPhase(static_cast<u8>((m_phase + 1) & 3));
}

u8 MegaDrive6Device::Lines(u8 smsLines, bool th, u64 tstates)
{
    Expire(tstates);
    const u8 aStart = static_cast<u8>(Low(ButtonA, 4) | Low(ButtonStart, 5));
    if (m_phase == 2 && !th)
        return aStart;
    if (m_phase == 3)
        return th ? static_cast<u8>((smsLines & 0x30) | Low(ButtonZ, 0) | Low(ButtonY, 1) |
                                    Low(ButtonX, 2) | Low(ButtonMode, 3))
                  : static_cast<u8>(aStart | 0x0F);
    return MegaDrive3Device::Lines(smsLines, th, tstates);
}
