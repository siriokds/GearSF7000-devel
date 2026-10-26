/*
 * GearSF7000 - SF-7000 Centronics parallel printer connector
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef GEARSF7000_SF7000_PARALLEL_PRINTER_PORT_H
#define GEARSF7000_SF7000_PARALLEL_PRINTER_PORT_H

#include "IDebugEventProvider.h"
#include "Intel8255.h"

#include <array>
#include <cstdint>

class DebugDeviceRegistry;
class Memory;

// Optional printer connected to the SF-7000 Amphenol 14-pin connector.
// The connector is unidirectional apart from BUSY: the SF-7000 drives the
// eight data pins and /STROBE, while the printer drives BUSY.
class ISF7000ParallelPrinterPeripheral
{
public:
    virtual ~ISF7000ParallelPrinterPeripheral() = default;
    virtual void Reset() = 0;
    virtual Intel8255PinSample ReadData(std::uint64_t timestamp) = 0;
    virtual Intel8255PinSample PeekData(std::uint64_t timestamp) const = 0;
    virtual Intel8255PinSample ReadBusy(std::uint64_t timestamp) = 0;
    virtual Intel8255PinSample PeekBusy(std::uint64_t timestamp) const = 0;
    virtual void DriveData(std::uint8_t value, std::uint8_t drivenMask,
                           std::uint64_t timestamp) = 0;
    virtual void DriveStrobe(std::uint8_t value, std::uint8_t drivenMask,
                             std::uint64_t timestamp) = 0;
};

class SF7000ParallelPrinterPort final : public IDebugEventProvider
{
public:
    enum Signal : std::uint8_t
    {
        Data = 0,
        DataDrivenMask,
        DataConflictMask,
        Strobe,
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

    explicit SF7000ParallelPrinterPort(Memory* memory);

    DebugDeviceDefinition DescribeDebugDevice() const override;
    void BindDebugEvents(const DebugDeviceRegistry& registry);

    void Reset();
    // Non-owning; nullptr represents the connector with no printer attached.
    void Attach(ISF7000ParallelPrinterPeripheral* peripheral);

    Intel8255PinSample ReadBusy(std::uint64_t timestamp = 0);
    Intel8255PinSample PeekBusy(std::uint64_t timestamp = 0) const;
    Intel8255PinSample ReadData(std::uint64_t timestamp = 0);
    Intel8255PinSample PeekData(std::uint64_t timestamp = 0) const;
    void DriveBusyFromPPI(std::uint8_t value, std::uint8_t drivenMask,
                          std::uint64_t timestamp = 0);
    void DriveData(std::uint8_t value, std::uint8_t drivenMask,
                   std::uint64_t timestamp = 0);
    void DriveStrobe(std::uint8_t value, std::uint8_t drivenMask,
                     std::uint64_t timestamp = 0);

    std::uint8_t GetData() const { return m_data; }
    std::uint8_t GetDataDrivenMask() const { return m_dataDrivenMask; }
    std::uint8_t GetDataConflictMask() const { return m_dataConflictMask; }
    PinState GetStrobeState() const { return m_strobe; }
    PinState GetBusyState() const { return m_busy; }

private:
    static PinState DecodePin(std::uint8_t value, std::uint8_t drivenMask,
                              std::uint8_t bit);
    void PublishValue(Signal signal, std::uint8_t before,
                      std::uint8_t after);
    void PublishBusy(const Intel8255PinSample& printerSample);
    void ResolveData(const Intel8255PinSample& peripheralSample);

    Memory* m_memory;
    ISF7000ParallelPrinterPeripheral* m_peripheral = nullptr;
    DebugDeviceId m_debugDevice = 0;
    std::uint8_t m_data = 0;
    std::uint8_t m_dataDrivenMask = 0;
    std::uint8_t m_dataConflictMask = 0;
    std::uint8_t m_ppiData = 0;
    std::uint8_t m_ppiDataDrivenMask = 0;
    PinState m_strobe = PinState::HighImpedance;
    PinState m_busy = PinState::HighImpedance;
    PinState m_ppiBusy = PinState::HighImpedance;
};

#endif
