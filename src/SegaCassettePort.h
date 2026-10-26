/*
 * GearSF7000 - SC-3000/SK-1100 cassette connector signals
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef GEARSF7000_SEGA_CASSETTE_PORT_H
#define GEARSF7000_SEGA_CASSETTE_PORT_H

#include "IDebugEventProvider.h"

#include <array>
#include <cstdint>

class DebugDeviceRegistry;
class Memory;

class SegaCassettePort : public IDebugEventProvider
{
public:
    enum Signal : std::uint8_t
    {
        TapeIn = 0,
        TapeOut,
        VirtualMotor,
        SignalCount
    };

    enum class PinState : std::uint8_t
    {
        Low = 0,
        High = 1,
        HighImpedance = 2
    };

    DebugDeviceDefinition DescribeDebugDevice() const override;
    void BindDebugEvents(const DebugDeviceRegistry& registry);
    void Reset();

    // PB7 is the HIC-1 conditioned cassette input. PC4 drives TAPE OUT;
    // PC3 is the emulator's documented virtual motor control used by BASIC.
    void ObserveTapeInput(bool high);
    void DrivePortC(std::uint8_t value, std::uint8_t drivenMask);

    PinState GetSignalState(Signal signal) const
    {
        return m_signalState[signal];
    }

protected:
    SegaCassettePort(const char* key, const char* parentKey,
                     const char* name, Memory* memory);

private:
    void PublishSignal(Signal signal, PinState state);

    const char* m_key;
    const char* m_parentKey;
    const char* m_name;
    Memory* m_memory;
    DebugDeviceId m_debugDevice = 0;
    std::array<PinState, SignalCount> m_signalState;
};

class SC3000CassettePort final : public SegaCassettePort
{
public:
    explicit SC3000CassettePort(Memory* memory)
        : SegaCassettePort("event.sc3000.ppi.cassette",
                           "event.sc3000.ppi",
                           "Cassette Interface", memory)
    {
    }
};

class SK1100CassettePort final : public SegaCassettePort
{
public:
    explicit SK1100CassettePort(Memory* memory)
        : SegaCassettePort("event.sg1000.sk1100.ppi.cassette",
                           "event.sg1000.sk1100.ppi",
                           "SK-1100 Cassette Interface", memory)
    {
    }
};

#endif
