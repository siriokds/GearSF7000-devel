// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#pragma once
#include "definitions.h"
#include "audio/IAYPorts.h"
#include "IDebugEventProvider.h"
#include <array>
#include <functional>
#include <string>

// The two MSX joystick ports, on the AY's parallel pins. Register 15 (port B,
// an output) selects the port read with bit 6 and drives pins 6 and 7 of each
// port through open collectors: bits 0-1 for port 1, bits 2-3 for port 2.
// Register 14 (port A, an input) reads up, down, left, right in bits 0-3 and
// pins 6 and 7, the two triggers, in bits 4-5, all active low; a trigger also
// reads low while its output latch is low. Bit 7 is the cassette input, which
// this device passes through from the cassette attachment.
class MSXJoysticks final : public IAYPorts, public IDebugEventProvider
{
public:
    enum Line : u8 { Up = 0x01, Down = 0x02, Left = 0x04, Right = 0x08, TriggerA = 0x10, TriggerB = 0x20 };
    using Change = std::function<void(unsigned player, u8 before, u8 after)>;

    void SetCassette(IAYPorts* cassette) { m_cassette = cassette; }
    void SetChangeCallback(Change change) { m_change = std::move(change); }
    // Contacts released; port B as at power on, pins pulled high.
    void Reset() { m_contacts = {}; m_portB = 0xFF; }
    void ReleaseAll() { for (unsigned player = 0; player < 2; ++player) Set(player, m_contacts[player], false); }
    // Contacts are active high here: a set bit is a closed switch.
    u8 Contacts(unsigned player) const { return m_contacts[player & 1]; }
    bool Set(unsigned player, u8 lines, bool pressed)
    {
        if (player > 1 || (lines & ~0x3F)) return false;
        const u8 before = m_contacts[player];
        const u8 after = pressed ? before | lines : before & ~lines;
        m_contacts[player] = after;
        if (after != before && m_change) m_change(player, before, after);
        return true;
    }

    std::string GetDescription() const override
    {
        return m_cassette ? "MSX joystick ports (AY port A bits 0-5, port B bit 6 selects) and " +
                            m_cassette->GetDescription()
                          : "MSX joystick ports (AY port A bits 0-5, port B bit 6 selects)";
    }
    InputPins PeekPort(unsigned port, std::int64_t time) const override
    {
        return port == 0 ? Compose(m_cassette ? m_cassette->PeekPort(0, time) : InputPins{0xFF, 0})
                         : InputPins{0xFF, 0};
    }
    InputPins ReadPort(unsigned port, std::int64_t time) override
    {
        return port == 0 ? Compose(m_cassette ? m_cassette->ReadPort(0, time) : InputPins{0xFF, 0})
                         : InputPins{0xFF, 0};
    }
    void WritePort(unsigned port, std::uint8_t value, std::uint8_t drivenMask, std::int64_t time) override
    {
        if (port == 1) m_portB = u8((value & drivenMask) | ~drivenMask);
        if (m_cassette) m_cassette->WritePort(port, value, drivenMask, time);
    }

    DebugDeviceDefinition DescribeDebugDevice() const override
    {
        DebugDeviceDefinition device;
        device.key = "event.msx.joysticks"; device.parentKey = "event.msx";
        device.typeKey = "msx.joysticks"; device.name = "MSX Joysticks";
        device.traits = DebugDeviceTrait_External;
        DebugSpaceDefinition contacts;
        contacts.id = DebugCommonSpace::State; contacts.key = "contacts";
        contacts.name = "Joystick Contacts"; contacts.targetMax = 1;
        contacts.supportedAccesses = contacts.beforeValueAccesses = contacts.afterValueAccesses = DebugEventAccess_Event;
        contacts.capabilities = DebugDescriptorCapability_EventsPublished;
        contacts.targets = {{0, "Port 1", 8}, {1, "Port 2", 8}};
        device.spaces.push_back(std::move(contacts));
        DebugRulePresetDefinition preset;
        preset.key = "contacts"; preset.name = "Joystick contacts"; preset.spaceKey = "contacts";
        preset.targetEnd = 1; preset.accessMask = DebugEventAccess_Event;
        device.presets.push_back(std::move(preset));
        return device;
    }

private:
    InputPins Compose(InputPins cassette) const
    {
        const unsigned port = (m_portB & 0x40) ? 1 : 0;
        const u8 contacts = m_contacts[port];
        const u8 latches = u8(m_portB >> (port * 2)) & 3;     // pins 6 and 7
        u8 value = u8(~contacts & 0x0F);
        if ((latches & 1) && !(contacts & TriggerA)) value |= 0x10;
        if ((latches & 2) && !(contacts & TriggerB)) value |= 0x20;
        const u8 passed = cassette.drivenMask & 0xC0;
        return {u8(value | (cassette.value & passed)), u8(0x3F | passed)};
    }

    std::array<u8, 2> m_contacts{};
    u8 m_portB = 0xFF;
    IAYPorts* m_cassette = nullptr;
    Change m_change;
};
