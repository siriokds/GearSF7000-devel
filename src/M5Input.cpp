// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#include "M5Input.h"
#include "SaveStateStream.h"
#include <cctype>
#include <utility>

namespace {
// Hardware contact assignment cross-checked against MAME m5.cpp (Curt Coder,
// Ales Dlabac), commit 1e1b6ce78a1645805bb5eef4049e3f1d3f926194.
constexpr const char* labels[M5Input::RowCount][8] = {
    {"Ctrl", "Func", "LeftShift", "RightShift", "", "", "Space", "Return"},
    {"1", "2", "3", "4", "5", "6", "7", "8"},
    {"Q", "W", "E", "R", "T", "Y", "U", "I"},
    {"A", "S", "D", "F", "G", "H", "J", "K"},
    {"Z", "X", "C", "V", "B", "N", "M", ","},
    {"9", "0", "-", "^", ".", "/", "_", "\\"},
    {"O", "P", "@", "[", "L", ";", ":", "]"}
};
constexpr u8 ValidMask(unsigned target)
{ return target == 0 ? 0xCF : target == M5Input::ResetTarget ? 0x80 : 0xFF; }
std::string Lower(std::string text)
{
    for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}
}
const char* M5Input::KeyLabel(unsigned row, unsigned bit)
{ return row < RowCount && bit < 8 ? labels[row][bit] : ""; }
void M5Input::Reset() { m_host.fill(0); m_synthetic.fill(0); m_controller.fill(0); }
u8 M5Input::ReadRow(unsigned row) const { return row < RowCount ? Read(row) : 0; }
void M5Input::Release(Source source, u64 clock)
{
    if (source != Source::Host && source != Source::Synthetic && source != Source::Controller) return;
    for (unsigned target = 0; target < TargetCount; ++target)
        Set(source, target, ValidMask(target), false, clock);
}
bool M5Input::Set(Source source, unsigned target, unsigned mask, bool pressed, u64 clock)
{
    if ((source != Source::Host && source != Source::Synthetic && source != Source::Controller) || target >= TargetCount ||
        !mask || (mask & ~unsigned(ValidMask(target)))) return false;
    auto& contacts = source == Source::Host ? m_host : source == Source::Synthetic ? m_synthetic : m_controller;
    const auto before = Read(target);
    contacts[target] = pressed ? contacts[target] | mask : contacts[target] & ~mask;
    const auto after = Read(target);
    if (m_change && before != after) m_change(target, before, after, clock);
    return true;
}
bool M5Input::SetMatrixKeyState(Source source, unsigned row, unsigned mask, bool pressed, u64 clock)
{ return row < RowCount && Set(source, row, mask, pressed, clock); }
bool M5Input::SetKeyByLabel(Source source, const std::string& label, bool pressed, u64 clock)
{
    auto wanted = Lower(label);
    if (wanted.empty()) return false;
    if (wanted == "enter") wanted = "return";
    if (wanted == "control") wanted = "ctrl";
    if (wanted == "reset") return SetResetKey(source, pressed, clock);
    for (unsigned row = 0; row < RowCount; ++row)
        for (unsigned bit = 0; bit < 8; ++bit)
            if (wanted == Lower(labels[row][bit]))
                return Set(source, row, 1u << bit, pressed, clock);
    return false;
}
bool M5Input::SetJoystickDirection(Source source, unsigned player, Direction direction, bool pressed, u64 clock)
{
    const auto bit = static_cast<unsigned>(direction);
    return player < 2 && bit < 4 && Set(source, JoystickTarget, 1u << (player * 4 + bit), pressed, clock);
}
bool M5Input::SetJoystickFire(Source source, unsigned player, unsigned button, bool pressed, u64 clock)
{
    // Fire contacts are keyboard Y1: P1 = 1/2, P2 = 5/6.
    if (player >= 2 || button >= 2) return false;
    return Set(source, 1, 1u << (player * 4 + button), pressed, clock);
}
bool M5Input::SetResetKey(Source source, bool pressed, u64 clock)
{ return Set(source, ResetTarget, 0x80, pressed, clock); }
void M5Input::SetChangeCallback(Change callback, bool eventsPublished)
{ m_change = std::move(callback); m_eventsPublished = bool(m_change) && eventsPublished; }
DebugDeviceDefinition M5Input::DescribeDebugDevice() const
{
    DebugDeviceDefinition device;
    device.key = "event.m5.input"; device.parentKey = "event.m5";
    device.typeKey = "m5.input"; device.name = "Sord M5 Input";
    DebugSpaceDefinition space;
    space.id = DebugCommonSpace::CustomFirst; space.key = "contacts";
    space.name = "Input contacts"; space.targetMax = TargetCount - 1;
    space.capabilities = m_eventsPublished ? DebugDescriptorCapability_EventsPublished : 0;
    space.supportedAccesses = space.beforeValueAccesses = space.afterValueAccesses = DebugEventAccess_Event;
    for (unsigned row = 0; row < RowCount; ++row)
        space.targets.push_back({row, "Keyboard Y" + std::to_string(row), 8});
    space.targets.push_back({JoystickTarget, "Joystick directions P1/P2", 8});
    space.targets.push_back({ResetTarget, "Reset key (status bit 7)", 8});
    device.spaces.push_back(std::move(space));
    DebugRulePresetDefinition preset;
    preset.key = "contacts"; preset.name = "Input contacts"; preset.spaceKey = "contacts";
    preset.targetEnd = TargetCount - 1; preset.accessMask = DebugEventAccess_Event;
    device.presets.push_back(std::move(preset));
    return device;
}
void M5Input::SaveState(std::ostream& stream) const
{
    StateWriter writer(stream); writer.U16(2);
    for (auto value : m_host) writer.U8(value);
    for (auto value : m_synthetic) writer.U8(value);
    for (auto value : m_controller) writer.U8(value);
}
bool M5Input::LoadState(std::istream& stream)
{
    StateReader reader(stream);
    const u16 version = reader.U16();
    if (version != 1 && version != 2) return false;
    Contacts host{}, synthetic{}, controller{};
    for (auto& value : host) value = reader.U8();
    for (auto& value : synthetic) value = reader.U8();
    if (version == 2) for (auto& value : controller) value = reader.U8();
    if (!reader.Ok()) return false;
    for (unsigned target = 0; target < TargetCount; ++target)
        if ((host[target] | synthetic[target] | controller[target]) & ~ValidMask(target)) return false;
    m_host = host; m_synthetic = synthetic; m_controller = controller;
    return true;
}
