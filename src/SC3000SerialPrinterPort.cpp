/*
 * GearSF7000 - SC-3000/SK-1100 serial printer DIN-5 connector
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SC3000SerialPrinterPort.h"

#include "DebugEvents.h"
#include "Memory.h"

#include <utility>

namespace
{
constexpr std::uint8_t kPrinterInputMask = 0x60;
constexpr std::uint8_t kPrinterOutputMask = 0xE0;

SegaSerialPrinterPort::PinState DecodePin(
    std::uint8_t value, std::uint8_t drivenMask, std::uint8_t bit)
{
    if ((drivenMask & bit) == 0)
        return SegaSerialPrinterPort::PinState::HighImpedance;
    return (value & bit) != 0
        ? SegaSerialPrinterPort::PinState::High
        : SegaSerialPrinterPort::PinState::Low;
}
}

SegaSerialPrinterPort::SegaSerialPrinterPort(
    const char* key, const char* parentKey, const char* name, Memory* memory)
    : m_key(key), m_parentKey(parentKey), m_name(name), m_memory(memory)
{
    m_signalState.fill(PinState::HighImpedance);
}

DebugDeviceDefinition
SegaSerialPrinterPort::DescribeDebugDevice() const
{
    DebugDeviceDefinition device;
    device.key = m_key;
    device.parentKey = m_parentKey;
    device.typeKey = "sega.serial_printer_port";
    device.name = m_name;

    DebugSpaceDefinition signals;
    signals.id = DebugCommonSpace::Signals;
    signals.key = "signals";
    signals.name = "Connector Signals";
    signals.targetMin = 0;
    signals.targetMax = SignalCount - 1;
    signals.supportedAccesses = DebugEventAccess_Event;
    signals.beforeValueAccesses = DebugEventAccess_Event;
    signals.afterValueAccesses = DebugEventAccess_Event;
    // 0=low, 1=high, 2=high impedance, 3=conflict.
    signals.defaultBitWidth = 2;
    signals.capabilities = DebugDescriptorCapability_EventsPublished |
                           DebugDescriptorCapability_SnapshotReadable;
    signals.targets = {
        {Data, "DATA (PC5)", 2},
        {ResetLine, "/RESET (PC6)", 2},
        {Feed, "/FEED (PC7)", 2},
        {Fault, "/FAULT (PB5)", 2},
        {Busy, "BUSY (PB6)", 2}
    };
    device.spaces.push_back(std::move(signals));
    return device;
}

void SegaSerialPrinterPort::BindDebugEvents(
    const DebugDeviceRegistry& registry)
{
    const DebugDeviceDescriptor* device = registry.FindDevice(m_key);
    m_debugDevice = device != nullptr ? device->id : 0;
}

void SegaSerialPrinterPort::Reset()
{
    for (unsigned i = 0; i < SignalCount; ++i)
        PublishSignal(static_cast<Signal>(i), PinState::HighImpedance);
    if (m_peripheral != nullptr)
        m_peripheral->Reset();
}

void SegaSerialPrinterPort::Attach(
    ISegaSerialPrinterPeripheral* peripheral)
{
    m_peripheral = peripheral;
    Reset();
}

Intel8255PinSample SegaSerialPrinterPort::ReadPortB(
    std::uint64_t timestamp)
{
    const Intel8255PinSample sample = m_peripheral != nullptr
        ? m_peripheral->ReadPortB(timestamp) : Intel8255PinSample{0x00, 0x00};
    PublishSignal(Fault, DecodePin(sample.value, sample.drivenMask, 0x20));
    PublishSignal(Busy, DecodePin(sample.value, sample.drivenMask, 0x40));
    return {static_cast<std::uint8_t>(sample.value & kPrinterInputMask),
            static_cast<std::uint8_t>(sample.drivenMask & kPrinterInputMask)};
}

Intel8255PinSample SegaSerialPrinterPort::PeekPortB(
    std::uint64_t timestamp) const
{
    if (m_peripheral == nullptr)
        return {0x00, 0x00};
    const Intel8255PinSample sample = m_peripheral->PeekPortB(timestamp);
    return {static_cast<std::uint8_t>(sample.value & kPrinterInputMask),
            static_cast<std::uint8_t>(sample.drivenMask & kPrinterInputMask)};
}

void SegaSerialPrinterPort::DrivePortC(
    std::uint8_t value, std::uint8_t drivenMask, std::uint64_t timestamp)
{
    const std::uint8_t mask = drivenMask & kPrinterOutputMask;
    PublishSignal(Data, DecodePin(value, mask, 0x20));
    PublishSignal(ResetLine, DecodePin(value, mask, 0x40));
    PublishSignal(Feed, DecodePin(value, mask, 0x80));
    if (m_peripheral != nullptr)
        m_peripheral->DrivePortC(value, mask, timestamp);
}

void SegaSerialPrinterPort::PublishSignal(
    Signal signal, PinState state)
{
    const PinState before = m_signalState[signal];
    if (before == state)
        return;
    m_signalState[signal] = state;
    if (m_memory == nullptr || m_debugDevice == 0)
        return;
    m_memory->CheckDeviceStateChange(
        {m_debugDevice, DebugCommonSpace::Signals, signal},
        static_cast<std::uint8_t>(before),
        static_cast<std::uint8_t>(state));
}
