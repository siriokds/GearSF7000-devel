// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#ifndef GAME_GEAR_INTERNAL_BUS_HANDLER_H
#define GAME_GEAR_INTERNAL_BUS_HANDLER_H

#include "IBusHandler.h"

#include <array>
#include <iosfwd>

class Audio;
class MachineIOPorts;
class SegaKeyboard;

// The fixed Game Gear I/O decoder. Memory and cartridge selection belong to
// the remaining motherboard/slot composition, not to this port decoder.
class GameGearInternalBusHandler final : public IBusHandler
{
public:
    GameGearInternalBusHandler(Audio& audio, MachineIOPorts& ports,
                               SegaKeyboard& keyboard, bool exportRegion);

    BusReadResult Read(BusAddressSpace space, u64 tstates,
                       u16 address) override;
    void Write(BusAddressSpace space, u64 tstates,
               u16 address, u8 value) override;

    void Reset();
    void SetStartPressed(bool pressed) { m_startPressed = pressed; }
    bool IsStartPressed() const { return m_startPressed; }
    void SetExportRegion(bool exportRegion) { m_exportRegion = exportRegion; }
    void SaveState(std::ostream& stream) const;
    bool LoadState(std::istream& stream);

private:
    Audio& m_audio;
    MachineIOPorts& m_ports;
    SegaKeyboard& m_keyboard;
    bool m_exportRegion;
    bool m_startPressed = false;
    std::array<u8, 5> m_extPorts{};
};

#endif
