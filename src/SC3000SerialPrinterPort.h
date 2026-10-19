/*
 * GearSF7000 - SC-3000/SK-1100 serial printer DIN-5 connector
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_SC3000_SERIAL_PRINTER_PORT_H
#define GEARSF7000_SC3000_SERIAL_PRINTER_PORT_H

#include "IDebugEventProvider.h"
#include "Intel8255.h"

#include <array>
#include <cstdint>

class DebugDeviceRegistry;
class Memory;

// Optional equipment connected to the SC-3000/SK-1100 DIN-5. Values retain
// their physical PPI bit positions: PB5=/FAULT, PB6=BUSY, PC5=DATA,
// PC6=/RESET and PC7=/FEED.
class ISegaSerialPrinterPeripheral
{
public:
    virtual ~ISegaSerialPrinterPeripheral() = default;
    virtual void Reset() = 0;
    virtual Intel8255PinSample ReadPortB(std::uint64_t timestamp) = 0;
    virtual Intel8255PinSample PeekPortB(std::uint64_t timestamp) const = 0;
    virtual void DrivePortC(std::uint8_t value, std::uint8_t drivenMask,
                            std::uint64_t timestamp) = 0;
};

// Shared electrical/debug behaviour of the identical ports fitted to the
// SC-3000 computer and the external SK-1100 keyboard. It is unrelated to the
// SF-7000 Centronics interface, which belongs to that unit's second 8255.
class SegaSerialPrinterPort : public IDebugEventProvider
{
public:
    enum Signal : std::uint8_t
    {
        Data = 0,
        ResetLine,
        Feed,
        Fault,
        Busy,
        SignalCount
    };

    enum class PinState : std::uint8_t
    {
        Low = 0,
        High = 1,
        HighImpedance = 2,
        Conflict = 3
    };

    DebugDeviceDefinition DescribeDebugDevice() const override;
    void BindDebugEvents(const DebugDeviceRegistry& registry);

    void Reset();
    // Non-owning; nullptr represents the physical connector with no
    // peripheral attached.
    void Attach(ISegaSerialPrinterPeripheral* peripheral);

    Intel8255PinSample ReadPortB(std::uint64_t timestamp = 0);
    Intel8255PinSample PeekPortB(std::uint64_t timestamp = 0) const;
    void DrivePortC(std::uint8_t value, std::uint8_t drivenMask,
                    std::uint64_t timestamp = 0);

    PinState GetSignalState(Signal signal) const
    {
        return m_signalState[signal];
    }

protected:
    SegaSerialPrinterPort(const char* key, const char* parentKey,
                          const char* name, Memory* memory);

private:
    void PublishSignal(Signal signal, PinState state);

    const char* m_key;
    const char* m_parentKey;
    const char* m_name;
    Memory* m_memory;
    ISegaSerialPrinterPeripheral* m_peripheral = nullptr;
    DebugDeviceId m_debugDevice = 0;
    std::array<PinState, SignalCount> m_signalState;
};

class SC3000SerialPrinterPort final : public SegaSerialPrinterPort
{
public:
    explicit SC3000SerialPrinterPort(Memory* memory)
        : SegaSerialPrinterPort(
              "event.sc3000.serial_printer_port",
              "event.sc3000", "Serial Printer Port (DIN-5)", memory)
    {
    }
};

// Constructed only by the future SG-1000 + SK-1100 machine composition.
class SK1100SerialPrinterPort final : public SegaSerialPrinterPort
{
public:
    explicit SK1100SerialPrinterPort(Memory* memory)
        : SegaSerialPrinterPort(
              "event.sg1000.sk1100.serial_printer_port",
              "event.sg1000.sk1100",
              "SK-1100 Serial Printer Port (DIN-5)", memory)
    {
    }
};

#endif
