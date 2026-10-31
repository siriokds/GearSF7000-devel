/*
 * GearSF7000 - Intel 8255A Programmable Peripheral Interface
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef GEARSF7000_INTEL8255_H
#define GEARSF7000_INTEL8255_H

#include "IDebugEventProvider.h"

#include <cstdint>
#include <iosfwd>
#include <string>

struct Intel8255PinSample
{
    // Electrical level after the wiring's pull-up/down policy has been
    // applied. drivenMask records which bits are actively driven externally;
    // it is diagnostic information used by the wiring to report contention.
    std::uint8_t value = 0xFF;
    std::uint8_t drivenMask = 0x00;
};

class IIntel8255Pins
{
public:
    virtual ~IIntel8255Pins() = default;

    virtual Intel8255PinSample ReadPortA(std::uint64_t timestamp) = 0;
    virtual Intel8255PinSample ReadPortB(std::uint64_t timestamp) = 0;
    virtual Intel8255PinSample ReadPortC(std::uint64_t timestamp) = 0;

    virtual Intel8255PinSample PeekPortA(std::uint64_t timestamp) const = 0;
    virtual Intel8255PinSample PeekPortB(std::uint64_t timestamp) const = 0;
    virtual Intel8255PinSample PeekPortC(std::uint64_t timestamp) const = 0;

    // drivenMask is zero for high-impedance pins. The wiring owns electrical
    // conflict detection because it alone knows its external drivers.
    virtual void DrivePortA(std::uint8_t value, std::uint8_t drivenMask,
                            std::uint64_t timestamp) = 0;
    virtual void DrivePortB(std::uint8_t value, std::uint8_t drivenMask,
                            std::uint64_t timestamp) = 0;
    virtual void DrivePortC(std::uint8_t value, std::uint8_t drivenMask,
                            std::uint64_t timestamp) = 0;
};

struct Intel8255Snapshot
{
    std::uint8_t control = 0x9B;
    std::uint8_t outputLatch[3] = {};
    std::uint8_t inputLatch[2] = {};
    std::uint8_t drivenValue[3] = {};
    std::uint8_t drivenMask[3] = {};
    bool inputBufferFull[2] = {};
    bool outputBufferFull[2] = {true, true};
    bool interruptEnable[2] = {};
    bool interruptEnableAInput = false;
    bool interruptEnableAOutput = false;
    bool interrupt[2] = {};
};

struct Intel8255DebugConfiguration
{
    std::string key;
    std::string parentKey;
    std::string name;
    std::uint16_t ioPortBase = 0;
    bool hasIoPorts = false;
};

class Intel8255 : public IDebugEventProvider
{
public:
    explicit Intel8255(IIntel8255Pins& pins);
    Intel8255(IIntel8255Pins& pins, Intel8255DebugConfiguration debug);

    // Called only while composing the machine's debug catalog. Register and
    // port names belong to the 8255; the instance identity and I/O window
    // belong to the board wiring supplied at construction.
    DebugDeviceDefinition DescribeDebugDevice() const override;

    // Hardware reset. The 8255 starts with all ports in Mode 0 input.
    void Reset(std::uint64_t timestamp = 0);

    std::uint8_t ReadRegister(std::uint8_t index,
                              std::uint64_t timestamp = 0);
    std::uint8_t PeekRegister(std::uint8_t index,
                              std::uint64_t timestamp = 0) const;
    void WriteRegister(std::uint8_t index, std::uint8_t value,
                       std::uint64_t timestamp = 0);

    // Peripheral-side handshake inputs. Only PC2, PC4 and PC6 have such a
    // role on a standard 8255. State changes are edge-sensitive.
    void SetPortCInputLine(unsigned bit, bool high,
                           std::uint64_t timestamp = 0);

    Intel8255Snapshot GetSnapshot() const;

    void SaveState(std::ostream& stream) const;
    bool LoadState(std::istream& stream);

private:
    enum Port : unsigned { PortA = 0, PortB = 1, PortC = 2 };
    enum Mode : unsigned { Mode0 = 0, Mode1 = 1, Mode2 = 2 };

    unsigned GroupAMode() const;
    unsigned GroupBMode() const;
    bool PortAIsInput() const;
    bool PortBIsInput() const;
    bool PortCLowerIsInput() const;
    bool PortCUpperIsInput() const;

    Intel8255PinSample ReadPins(Port port, std::uint64_t timestamp);
    Intel8255PinSample PeekPins(Port port, std::uint64_t timestamp) const;

    std::uint8_t ReadPort(unsigned port, std::uint64_t timestamp);
    std::uint8_t PeekPort(unsigned port, std::uint64_t timestamp) const;
    std::uint8_t ComposePortC(std::uint8_t pinValue) const;
    void WritePort(unsigned port, std::uint8_t value,
                   std::uint64_t timestamp);
    void WriteControl(std::uint8_t value, std::uint64_t timestamp);
    void SetMode(std::uint8_t value, std::uint64_t timestamp);
    void SetPortCBit(unsigned bit, bool high, std::uint64_t timestamp);

    void DrivePortA(std::uint64_t timestamp);
    void DrivePortB(std::uint64_t timestamp);
    void DrivePortC(std::uint64_t timestamp);
    void DriveAll(std::uint64_t timestamp);

    void CheckInterrupt(unsigned port, std::uint64_t timestamp);
    void SetInputBufferFull(unsigned port, bool state,
                            std::uint64_t timestamp);
    void SetOutputBufferFull(unsigned port, bool state,
                             std::uint64_t timestamp);
    void SetInterrupt(unsigned port, bool state, std::uint64_t timestamp);

    IIntel8255Pins& m_pins;
    std::uint8_t m_control = 0x9B;
    std::uint8_t m_output[3] = {};
    std::uint8_t m_input[2] = {};
    bool m_ibf[2] = {};
    bool m_obf[2] = {true, true};
    bool m_inte[2] = {};
    bool m_inteAInput = false;
    bool m_inteAOutput = false;
    bool m_intr[2] = {};
    std::uint8_t m_portCInputLines = 0xFF;
    std::uint8_t m_drivenValue[3] = {};
    std::uint8_t m_drivenMask[3] = {};
    Intel8255DebugConfiguration m_debug;
};

#endif
