// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#include "definitions.h"
#if GEARSF7000_ENABLE_GAMEGEAR
#include "GameGearInternalBusHandler.h"

#include "Audio.h"
#include "MachineIOPorts.h"
#include "SegaKeyboard.h"

#include <istream>
#include <ostream>

GameGearInternalBusHandler::GameGearInternalBusHandler(
    Audio& audio, MachineIOPorts& ports, SegaKeyboard& keyboard,
    bool exportRegion)
    : m_audio(audio), m_ports(ports), m_keyboard(keyboard),
      m_exportRegion(exportRegion)
{
    Reset();
}

void GameGearInternalBusHandler::Reset()
{
    // Gear-to-Gear registers $01-$05 with no cable attached.
    m_extPorts = {0x7F, 0xFF, 0x00, 0xFF, 0x00};
}

void GameGearInternalBusHandler::SaveState(std::ostream& stream) const
{
    stream.write(reinterpret_cast<const char*>(m_extPorts.data()), m_extPorts.size());
}

bool GameGearInternalBusHandler::LoadState(std::istream& stream)
{
    std::array<u8, 5> ports;
    stream.read(reinterpret_cast<char*>(ports.data()), ports.size());
    if (!stream)
        return false;
    m_extPorts = ports;
    return true;
}

BusReadResult GameGearInternalBusHandler::Read(
    BusAddressSpace space, u64 tstates, u16 address)
{
    if (space != BusAddressSpace::IO)
        return {};

    const u8 port = static_cast<u8>(address);
    if (port == 0x00)
    {
        const u8 start = m_startPressed ? 0x00 : 0x80;
        const u8 region = m_exportRegion ? 0x40 : 0x00;
        return {true, static_cast<u8>(start | region)};
    }
    if (port >= 0x01 && port <= 0x05)
        return {true, m_extPorts[port - 1]};

    if (port < 0x40)
        return {};  // $06-$3F are open bus ($FF with no other driver).

    if (port < 0xC0)
        return {true, m_ports.SMSIn(tstates, port)};

    if (port == 0xC0 || port == 0xDC)
    {
        // One built-in pad; the absent second controller's two lines read high.
        return {true, static_cast<u8>((m_keyboard.GetJoystickPortA() & 0x3F) | 0xC0)};
    }
    if (port == 0xC1 || port == 0xDD)
    {
        // The second controller and Reset are absent. TH levels come from the
        // I/O control latch written through the shared $07-$3F decoder.
        return {true, static_cast<u8>(0x3F | (m_ports.GetSMSIoControl() & 0xC0))};
    }

    // Other $C0-$FF locations have no selected internal driver.
    return {};
}

void GameGearInternalBusHandler::Write(
    BusAddressSpace space, u64 tstates, u16 address, u8 value)
{
    if (space != BusAddressSpace::IO)
        return;

    const u8 port = static_cast<u8>(address);
    if (port >= 0x01 && port <= 0x05)
        m_extPorts[port - 1] = value;
    else if (port == 0x06)
        m_audio.WriteStereoPanning(value);
    else if (port >= 0x07 && port < 0xC0)
        m_ports.SMSOut(tstates, port, value);
}

#endif /* GEARSF7000_ENABLE_GAMEGEAR */
