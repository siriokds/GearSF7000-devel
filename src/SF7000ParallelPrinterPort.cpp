/*
 * GearSF7000 - SF-7000 Centronics parallel printer connector
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SF7000ParallelPrinterPort.h"

#include "DebugEvents.h"
#include "Memory.h"

#include <utility>

namespace
{
constexpr std::uint8_t kBusyAtPeripheral = 0x01;
constexpr std::uint8_t kBusyAtPPI = 0x02;
constexpr std::uint8_t kStrobeAtPPI = 0x80;
}

SF7000ParallelPrinterPort::SF7000ParallelPrinterPort(Memory* memory)
    : m_memory(memory)
{
}

DebugDeviceDefinition SF7000ParallelPrinterPort::DescribeDebugDevice() const
{
    DebugDeviceDefinition device;
    device.key = "event.sc3000.sf7000.parallel_printer_port";
    device.parentKey = "event.sc3000.sf7000";
    device.typeKey = "centronics.parallel_printer_port";
    device.name = "Parallel Printer Port (Centronics)";

    DebugSpaceDefinition signals;
    signals.id = DebugCommonSpace::Signals;
    signals.key = "signals";
    signals.name = "Connector Signals";
    signals.targetMin = 0;
    signals.targetMax = SignalCount - 1;
    signals.supportedAccesses = DebugEventAccess_Event;
    signals.beforeValueAccesses = DebugEventAccess_Event;
    signals.afterValueAccesses = DebugEventAccess_Event;
    signals.defaultBitWidth = 8;
    signals.capabilities = DebugDescriptorCapability_EventsPublished |
                           DebugDescriptorCapability_SnapshotReadable;
    signals.targets = {
        {Data, "DATA1..DATA8", 8},
        {DataDrivenMask, "DATA driven mask", 8},
        {DataConflictMask, "DATA conflict mask", 8},
        {Strobe, "/STROBE", 2},
        {Busy, "BUSY", 2}
    };
    device.spaces.push_back(std::move(signals));
    return device;
}

void SF7000ParallelPrinterPort::BindDebugEvents(
    const DebugDeviceRegistry& registry)
{
    const DebugDeviceDescriptor* device = registry.FindDevice(
        "event.sc3000.sf7000.parallel_printer_port");
    m_debugDevice = device != nullptr ? device->id : 0;
}

void SF7000ParallelPrinterPort::Reset()
{
    if (m_peripheral != nullptr)
        m_peripheral->Reset();

    DriveData(0, 0, 0);
    DriveStrobe(0, 0, 0);
    DriveBusyFromPPI(0, 0, 0);
}

void SF7000ParallelPrinterPort::Attach(
    ISF7000ParallelPrinterPeripheral* peripheral)
{
    m_peripheral = peripheral;
    Reset();
}

SF7000ParallelPrinterPort::PinState
SF7000ParallelPrinterPort::DecodePin(
    std::uint8_t value, std::uint8_t drivenMask, std::uint8_t bit)
{
    if ((drivenMask & bit) == 0)
        return PinState::HighImpedance;
    return (value & bit) != 0 ? PinState::High : PinState::Low;
}

Intel8255PinSample SF7000ParallelPrinterPort::ReadBusy(
    std::uint64_t timestamp)
{
    const Intel8255PinSample sample = m_peripheral != nullptr
        ? m_peripheral->ReadBusy(timestamp) : Intel8255PinSample{0, 0};
    PublishBusy(sample);
    return {
        static_cast<std::uint8_t>((sample.value & kBusyAtPeripheral) != 0
            ? kBusyAtPPI : 0),
        static_cast<std::uint8_t>((sample.drivenMask & kBusyAtPeripheral) != 0
            ? kBusyAtPPI : 0)
    };
}

Intel8255PinSample SF7000ParallelPrinterPort::ReadData(
    std::uint64_t timestamp)
{
    const Intel8255PinSample sample = m_peripheral != nullptr
        ? m_peripheral->ReadData(timestamp) : Intel8255PinSample{0, 0};
    ResolveData(sample);
    return sample;
}

Intel8255PinSample SF7000ParallelPrinterPort::PeekData(
    std::uint64_t timestamp) const
{
    return m_peripheral != nullptr
        ? m_peripheral->PeekData(timestamp) : Intel8255PinSample{0, 0};
}

Intel8255PinSample SF7000ParallelPrinterPort::PeekBusy(
    std::uint64_t timestamp) const
{
    if (m_peripheral == nullptr)
        return {0, 0};
    const Intel8255PinSample sample = m_peripheral->PeekBusy(timestamp);
    return {
        static_cast<std::uint8_t>((sample.value & kBusyAtPeripheral) != 0
            ? kBusyAtPPI : 0),
        static_cast<std::uint8_t>((sample.drivenMask & kBusyAtPeripheral) != 0
            ? kBusyAtPPI : 0)
    };
}

void SF7000ParallelPrinterPort::DriveBusyFromPPI(
    std::uint8_t value, std::uint8_t drivenMask, std::uint64_t timestamp)
{
    m_ppiBusy = DecodePin(value, drivenMask, kBusyAtPPI);
    const Intel8255PinSample printer = m_peripheral != nullptr
        ? m_peripheral->PeekBusy(timestamp) : Intel8255PinSample{0, 0};
    PublishBusy(printer);
}

void SF7000ParallelPrinterPort::DriveData(
    std::uint8_t value, std::uint8_t drivenMask, std::uint64_t timestamp)
{
    m_ppiData = value;
    m_ppiDataDrivenMask = drivenMask;
    if (m_peripheral != nullptr)
        m_peripheral->DriveData(value, drivenMask, timestamp);
    ResolveData(PeekData(timestamp));
}

void SF7000ParallelPrinterPort::DriveStrobe(
    std::uint8_t value, std::uint8_t drivenMask, std::uint64_t timestamp)
{
    const PinState before = m_strobe;
    m_strobe = DecodePin(value, drivenMask, kStrobeAtPPI);
    PublishValue(Strobe, static_cast<std::uint8_t>(before),
                 static_cast<std::uint8_t>(m_strobe));
    if (m_peripheral != nullptr)
        m_peripheral->DriveStrobe(
            static_cast<std::uint8_t>((value & kStrobeAtPPI) != 0),
            static_cast<std::uint8_t>((drivenMask & kStrobeAtPPI) != 0),
            timestamp);
}

void SF7000ParallelPrinterPort::PublishValue(
    Signal signal, std::uint8_t before, std::uint8_t after)
{
    if (before == after || m_memory == nullptr || m_debugDevice == 0)
        return;
    m_memory->CheckDeviceStateChange(
        {m_debugDevice, DebugCommonSpace::Signals, signal}, before, after);
}

void SF7000ParallelPrinterPort::PublishBusy(
    const Intel8255PinSample& printerSample)
{
    const PinState printerBusy = DecodePin(
        printerSample.value, printerSample.drivenMask, kBusyAtPeripheral);
    PinState resolved = printerBusy;
    if (m_ppiBusy != PinState::HighImpedance)
    {
        if (printerBusy == PinState::HighImpedance)
            resolved = m_ppiBusy;
        else if (printerBusy != m_ppiBusy)
            resolved = PinState::Conflict;
    }

    const PinState before = m_busy;
    m_busy = resolved;
    PublishValue(Busy, static_cast<std::uint8_t>(before),
                 static_cast<std::uint8_t>(m_busy));
}

void SF7000ParallelPrinterPort::ResolveData(
    const Intel8255PinSample& peripheralSample)
{
    const std::uint8_t beforeData = m_data;
    const std::uint8_t beforeMask = m_dataDrivenMask;
    const std::uint8_t beforeConflict = m_dataConflictMask;
    const std::uint8_t both = static_cast<std::uint8_t>(
        m_ppiDataDrivenMask & peripheralSample.drivenMask);

    m_dataConflictMask = static_cast<std::uint8_t>(
        (m_ppiData ^ peripheralSample.value) & both);
    m_dataDrivenMask = static_cast<std::uint8_t>(
        m_ppiDataDrivenMask | peripheralSample.drivenMask);
    m_data = static_cast<std::uint8_t>(
        (m_ppiData & m_ppiDataDrivenMask) |
        (peripheralSample.value & peripheralSample.drivenMask &
         ~m_ppiDataDrivenMask) |
        m_dataConflictMask);

    PublishValue(Data, beforeData, m_data);
    PublishValue(DataDrivenMask, beforeMask, m_dataDrivenMask);
    PublishValue(DataConflictMask, beforeConflict, m_dataConflictMask);
}
