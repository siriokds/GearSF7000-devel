/*
 * GearSF7000 - Intel 8255A Programmable Peripheral Interface
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Behaviour was checked against the Intel programming model and the mature
 * MAME/openMSX implementations. This is a native GearSF7000 implementation:
 * the surrounding hardware is supplied through IIntel8255Pins.
 */

#include "Intel8255.h"
#include "SaveStateStream.h"

#include <utility>

namespace
{
constexpr std::uint8_t kModeSet = 0x80;
constexpr std::uint8_t kGroupAModeMask = 0x60;
constexpr std::uint8_t kPortAInput = 0x10;
constexpr std::uint8_t kPortCUpperInput = 0x08;
constexpr std::uint8_t kGroupBMode1 = 0x04;
constexpr std::uint8_t kPortBInput = 0x02;
constexpr std::uint8_t kPortCLowerInput = 0x01;
constexpr std::uint16_t kStateVersion = 1;
}

Intel8255::Intel8255(IIntel8255Pins& pins)
    : Intel8255(pins, Intel8255DebugConfiguration{})
{
}

Intel8255::Intel8255(IIntel8255Pins& pins,
                     Intel8255DebugConfiguration debug)
    : m_pins(pins), m_debug(std::move(debug))
{
    Reset();
}

DebugDeviceDefinition Intel8255::DescribeDebugDevice() const
{
    DebugDeviceDefinition device;
    device.key = m_debug.key;
    device.parentKey = m_debug.parentKey;
    device.typeKey = "intel.8255";
    device.name = m_debug.name.empty() ? "Intel 8255 PPI" : m_debug.name;

    const u8 readWrite = DebugEventAccess_Read | DebugEventAccess_Write;
    if (m_debug.hasIoPorts)
    {
        DebugSpaceDefinition ports;
        ports.id = DebugCommonSpace::IoPorts;
        ports.key = "io_ports";
        ports.name = "PPI Ports";
        ports.targetMin = m_debug.ioPortBase;
        ports.targetMax = static_cast<u32>(m_debug.ioPortBase + 3);
        ports.supportedAccesses = readWrite;
        ports.afterValueAccesses = readWrite;
        ports.capabilities = DebugDescriptorCapability_EventsPublished;
        ports.targets.push_back({m_debug.ioPortBase + 0u, "Port A", 8});
        ports.targets.push_back({m_debug.ioPortBase + 1u, "Port B", 8});
        ports.targets.push_back({m_debug.ioPortBase + 2u, "Port C", 8});
        ports.targets.push_back({m_debug.ioPortBase + 3u, "Control", 8});
        device.spaces.push_back(std::move(ports));

        DebugRulePresetDefinition ioPreset;
        ioPreset.key = "io_ports";
        ioPreset.name = device.name + " I/O";
        ioPreset.spaceKey = "io_ports";
        ioPreset.targetStart = m_debug.ioPortBase;
        ioPreset.targetEnd = static_cast<u32>(m_debug.ioPortBase + 3);
        ioPreset.accessMask = readWrite;
        device.presets.push_back(std::move(ioPreset));
    }

    DebugSpaceDefinition registers;
    registers.id = DebugCommonSpace::Registers;
    registers.key = "registers";
    registers.name = "Registers";
    registers.targetMax = 3;
    registers.supportedAccesses = DebugEventAccess_Write;
    registers.beforeValueAccesses = DebugEventAccess_Write;
    registers.afterValueAccesses = DebugEventAccess_Write;
    registers.capabilities = DebugDescriptorCapability_None;
    registers.targets.push_back({0, "Port A", 8});
    registers.targets.push_back({1, "Port B", 8});
    registers.targets.push_back({2, "Port C", 8});
    registers.targets.push_back({3, "Control", 8});
    device.spaces.push_back(std::move(registers));

    // The compatibility publisher currently identifies latch transitions by
    // their physical port. Keeping that range preserves saved rules while
    // the publisher itself is migrated to semantic register targets.
    DebugSpaceDefinition state;
    state.id = DebugCommonSpace::State;
    state.key = "state";
    state.name = "Latch State";
    state.targetMax = 0xFF;
    state.supportedAccesses = DebugEventAccess_Event;
    state.beforeValueAccesses = DebugEventAccess_Event;
    state.afterValueAccesses = DebugEventAccess_Event;
    state.capabilities = DebugDescriptorCapability_EventsPublished;
    device.spaces.push_back(std::move(state));

    DebugRulePresetDefinition statePreset;
    statePreset.key = "state";
    statePreset.name = device.name + " state";
    statePreset.spaceKey = "state";
    statePreset.targetEnd = 0xFF;
    statePreset.accessMask = DebugEventAccess_Event;
    device.presets.push_back(std::move(statePreset));
    return device;
}

void Intel8255::Reset(std::uint64_t timestamp)
{
    m_output[PortA] = m_output[PortB] = m_output[PortC] = 0;
    m_input[PortA] = m_input[PortB] = 0;
    m_ibf[PortA] = m_ibf[PortB] = false;
    m_obf[PortA] = m_obf[PortB] = true;
    m_inte[PortA] = m_inte[PortB] = false;
    m_inteAInput = m_inteAOutput = false;
    m_intr[PortA] = m_intr[PortB] = false;
    m_portCInputLines = 0xFF;
    m_control = 0x9B; // Mode 0; A, B, C upper and C lower are inputs.
    DriveAll(timestamp);
}

unsigned Intel8255::GroupAMode() const
{
    const unsigned encoded = (m_control & kGroupAModeMask) >> 5;
    return encoded >= 2 ? Mode2 : encoded;
}

unsigned Intel8255::GroupBMode() const
{
    return (m_control & kGroupBMode1) ? Mode1 : Mode0;
}

bool Intel8255::PortAIsInput() const { return (m_control & kPortAInput) != 0; }
bool Intel8255::PortBIsInput() const { return (m_control & kPortBInput) != 0; }
bool Intel8255::PortCLowerIsInput() const { return (m_control & kPortCLowerInput) != 0; }
bool Intel8255::PortCUpperIsInput() const { return (m_control & kPortCUpperInput) != 0; }

Intel8255PinSample Intel8255::ReadPins(Port port, std::uint64_t timestamp)
{
    switch (port)
    {
        case PortA: return m_pins.ReadPortA(timestamp);
        case PortB: return m_pins.ReadPortB(timestamp);
        case PortC: return m_pins.ReadPortC(timestamp);
    }
    return {};
}

Intel8255PinSample Intel8255::PeekPins(Port port,
                                       std::uint64_t timestamp) const
{
    switch (port)
    {
        case PortA: return m_pins.PeekPortA(timestamp);
        case PortB: return m_pins.PeekPortB(timestamp);
        case PortC: return m_pins.PeekPortC(timestamp);
    }
    return {};
}

std::uint8_t Intel8255::ReadRegister(std::uint8_t index,
                                     std::uint64_t timestamp)
{
    switch (index & 3)
    {
        case 0: return ReadPort(PortA, timestamp);
        case 1: return ReadPort(PortB, timestamp);
        case 2: return ComposePortC(ReadPins(PortC, timestamp).value);
        default:
            // A1=A0=1 with /RD active is an illegal/read-disabled 8255 bus
            // operation. The un-driven data bus is represented as open bus.
            return 0xFF;
    }
}

std::uint8_t Intel8255::PeekRegister(std::uint8_t index,
                                     std::uint64_t timestamp) const
{
    switch (index & 3)
    {
        case 0: return PeekPort(PortA, timestamp);
        case 1: return PeekPort(PortB, timestamp);
        case 2: return ComposePortC(PeekPins(PortC, timestamp).value);
        default: return 0xFF;
    }
}

std::uint8_t Intel8255::ReadPort(unsigned port, std::uint64_t timestamp)
{
    const bool input = port == PortA ? PortAIsInput() : PortBIsInput();
    const unsigned mode = port == PortA ? GroupAMode() : GroupBMode();

    if (mode == Mode0)
        return input ? ReadPins(static_cast<Port>(port), timestamp).value
                     : m_output[port];

    if (port == PortA && mode == Mode2)
    {
        const std::uint8_t value = m_input[PortA];
        SetInputBufferFull(PortA, false, timestamp);
        SetInterrupt(PortA, false, timestamp);
        m_input[PortA] = 0;
        return value;
    }

    if (!input)
        return m_output[port];

    const std::uint8_t value = m_input[port];
    SetInputBufferFull(port, false, timestamp);
    SetInterrupt(port, false, timestamp);
    m_input[port] = 0;
    return value;
}

std::uint8_t Intel8255::PeekPort(unsigned port,
                                 std::uint64_t timestamp) const
{
    const bool input = port == PortA ? PortAIsInput() : PortBIsInput();
    const unsigned mode = port == PortA ? GroupAMode() : GroupBMode();

    if (mode == Mode0)
        return input ? PeekPins(static_cast<Port>(port), timestamp).value
                     : m_output[port];
    if (port == PortA && mode == Mode2)
        return m_input[PortA];
    return input ? m_input[port] : m_output[port];
}

std::uint8_t Intel8255::ComposePortC(std::uint8_t pinValue) const
{
    std::uint8_t value = 0;
    std::uint8_t inputMask = 0;
    std::uint8_t lowerMask = 0x0F;

    switch (GroupAMode())
    {
        case Mode0:
            if (PortCUpperIsInput()) inputMask |= 0xF0;
            else value |= m_output[PortC] & 0xF0;
            break;

        case Mode1:
            value |= m_intr[PortA] ? 0x08 : 0;
            lowerMask = 0x07; // PC3 belongs to group A in modes 1 and 2.
            if (PortAIsInput())
            {
                value |= m_ibf[PortA] ? 0x20 : 0;
                value |= m_inte[PortA] ? 0x10 : 0;
                inputMask |= 0xC0;
            }
            else
            {
                value |= m_obf[PortA] ? 0x80 : 0;
                value |= m_inte[PortA] ? 0x40 : 0;
                inputMask |= 0x30;
            }
            if (!PortCUpperIsInput())
            {
                value |= m_output[PortC] & inputMask;
                inputMask = 0;
            }
            break;

        case Mode2:
            lowerMask = 0x07;
            value |= m_intr[PortA] ? 0x08 : 0;
            value |= m_inteAInput ? 0x10 : 0;
            value |= m_ibf[PortA] ? 0x20 : 0;
            value |= m_inteAOutput ? 0x40 : 0;
            value |= m_obf[PortA] ? 0x80 : 0;
            break;
    }

    if (GroupBMode() == Mode0)
    {
        if (PortCLowerIsInput()) inputMask |= lowerMask;
        else value |= m_output[PortC] & lowerMask;
    }
    else
    {
        value |= m_intr[PortB] ? 0x01 : 0;
        value |= (PortBIsInput() ? m_ibf[PortB] : m_obf[PortB]) ? 0x02 : 0;
        value |= m_inte[PortB] ? 0x04 : 0;
    }

    return value | (pinValue & inputMask);
}

void Intel8255::WriteRegister(std::uint8_t index, std::uint8_t value,
                              std::uint64_t timestamp)
{
    switch (index & 3)
    {
        case 0: WritePort(PortA, value, timestamp); break;
        case 1: WritePort(PortB, value, timestamp); break;
        case 2:
            m_output[PortC] = value;
            DrivePortC(timestamp);
            break;
        case 3: WriteControl(value, timestamp); break;
    }
}

void Intel8255::WritePort(unsigned port, std::uint8_t value,
                          std::uint64_t timestamp)
{
    const bool input = port == PortA ? PortAIsInput() : PortBIsInput();
    const unsigned mode = port == PortA ? GroupAMode() : GroupBMode();

    // The CPU always writes the output latch.  Input mode only disconnects
    // that latch from the pins; changing the direction later can therefore
    // expose the byte without another data-port write.
    m_output[port] = value;

    if (port == PortA && mode == Mode2)
    {
        SetOutputBufferFull(PortA, false, timestamp);
        DrivePortA(timestamp);
        SetInterrupt(PortA, false, timestamp);
        return;
    }

    if (input)
        return;

    if (port == PortA) DrivePortA(timestamp);
    else DrivePortB(timestamp);

    if (mode == Mode1)
    {
        SetOutputBufferFull(port, false, timestamp);
        SetInterrupt(port, false, timestamp);
    }
}

void Intel8255::WriteControl(std::uint8_t value, std::uint64_t timestamp)
{
    if (value & kModeSet)
        SetMode(value, timestamp);
    else
        SetPortCBit((value >> 1) & 7, (value & 1) != 0, timestamp);
}

void Intel8255::SetMode(std::uint8_t value, std::uint64_t timestamp)
{
    m_control = value;
    m_output[PortA] = m_output[PortB] = m_output[PortC] = 0;
    m_input[PortA] = m_input[PortB] = 0;
    m_ibf[PortA] = m_ibf[PortB] = false;
    m_obf[PortA] = m_obf[PortB] = true;
    m_inte[PortA] = m_inte[PortB] = false;
    m_inteAInput = m_inteAOutput = false;
    m_intr[PortA] = m_intr[PortB] = false;
    DriveAll(timestamp);
}

void Intel8255::SetPortCBit(unsigned bit, bool high,
                            std::uint64_t timestamp)
{
    const std::uint8_t mask = static_cast<std::uint8_t>(1u << (bit & 7));
    if (high) m_output[PortC] |= mask;
    else m_output[PortC] &= static_cast<std::uint8_t>(~mask);

    if (GroupAMode() == Mode1)
    {
        if (PortAIsInput() && bit == 4) m_inte[PortA] = high;
        if (!PortAIsInput() && bit == 6) m_inte[PortA] = high;
    }
    else if (GroupAMode() == Mode2)
    {
        if (bit == 4) m_inteAInput = high;
        if (bit == 6) m_inteAOutput = high;
    }

    if (GroupBMode() == Mode1 && bit == 2)
        m_inte[PortB] = high;

    CheckInterrupt(PortA, timestamp);
    CheckInterrupt(PortB, timestamp);
    DrivePortC(timestamp);
}

void Intel8255::DrivePortA(std::uint64_t timestamp)
{
    m_drivenValue[PortA] = m_output[PortA];
    m_drivenMask[PortA] = GroupAMode() == Mode2
        ? (m_obf[PortA] ? 0x00 : 0xFF)
        : (PortAIsInput() ? 0x00 : 0xFF);
    m_pins.DrivePortA(m_drivenValue[PortA], m_drivenMask[PortA], timestamp);
}

void Intel8255::DrivePortB(std::uint64_t timestamp)
{
    m_drivenValue[PortB] = m_output[PortB];
    m_drivenMask[PortB] = PortBIsInput() ? 0x00 : 0xFF;
    m_pins.DrivePortB(m_drivenValue[PortB], m_drivenMask[PortB], timestamp);
}

void Intel8255::DrivePortC(std::uint64_t timestamp)
{
    std::uint8_t value = 0;
    std::uint8_t mask = 0;
    std::uint8_t lowerMask = 0x0F;

    switch (GroupAMode())
    {
        case Mode0:
            if (!PortCUpperIsInput()) mask |= 0xF0;
            break;
        case Mode1:
            lowerMask = 0x07;
            mask |= 0x08; // INTR A
            value |= m_intr[PortA] ? 0x08 : 0;
            if (PortAIsInput())
            {
                mask |= 0x20; // IBF A; PC4 is /STB, PC6/7 are general I/O.
                value |= m_ibf[PortA] ? 0x20 : 0;
                if (!PortCUpperIsInput()) mask |= 0xC0;
            }
            else
            {
                mask |= 0x80; // /OBF A; PC6 is /ACK, PC4/5 are general I/O.
                value |= m_obf[PortA] ? 0x80 : 0;
                if (!PortCUpperIsInput()) mask |= 0x30;
            }
            break;
        case Mode2:
            lowerMask = 0x07;
            mask |= 0xA8; // /OBF A, IBF A and INTR A.
            value |= m_obf[PortA] ? 0x80 : 0;
            value |= m_ibf[PortA] ? 0x20 : 0;
            value |= m_intr[PortA] ? 0x08 : 0;
            break;
    }

    if (GroupBMode() == Mode0)
    {
        if (!PortCLowerIsInput()) mask |= lowerMask;
    }
    else
    {
        mask |= 0x03; // INTR B and IBF B or /OBF B; PC2 is strobe/ack.
        value |= m_intr[PortB] ? 0x01 : 0;
        value |= (PortBIsInput() ? m_ibf[PortB] : m_obf[PortB]) ? 0x02 : 0;
    }

    value |= m_output[PortC] & mask;
    // Status outputs override the user latch bits reserved for handshaking.
    if (GroupAMode() == Mode1 || GroupAMode() == Mode2)
    {
        value = static_cast<std::uint8_t>((value & ~0x08) |
            (m_intr[PortA] ? 0x08 : 0));
        if (PortAIsInput() || GroupAMode() == Mode2)
            value = static_cast<std::uint8_t>((value & ~0x20) |
                (m_ibf[PortA] ? 0x20 : 0));
        if (!PortAIsInput() || GroupAMode() == Mode2)
            value = static_cast<std::uint8_t>((value & ~0x80) |
                (m_obf[PortA] ? 0x80 : 0));
    }
    if (GroupBMode() == Mode1)
    {
        value = static_cast<std::uint8_t>((value & ~0x01) |
            (m_intr[PortB] ? 0x01 : 0));
        value = static_cast<std::uint8_t>((value & ~0x02) |
            ((PortBIsInput() ? m_ibf[PortB] : m_obf[PortB]) ? 0x02 : 0));
    }

    m_drivenValue[PortC] = value;
    m_drivenMask[PortC] = mask;
    m_pins.DrivePortC(value, mask, timestamp);
}

void Intel8255::DriveAll(std::uint64_t timestamp)
{
    DrivePortA(timestamp);
    DrivePortB(timestamp);
    DrivePortC(timestamp);
}

void Intel8255::CheckInterrupt(unsigned port, std::uint64_t timestamp)
{
    bool state = false;
    const unsigned mode = port == PortA ? GroupAMode() : GroupBMode();
    if (port == PortA && mode == Mode2)
        state = (m_inteAOutput && m_obf[PortA]) ||
                (m_inteAInput && m_ibf[PortA]);
    else if (mode == Mode1)
    {
        const bool input = port == PortA ? PortAIsInput() : PortBIsInput();
        state = m_inte[port] && (input ? m_ibf[port] : m_obf[port]);
    }

    SetInterrupt(port, state, timestamp);
}

void Intel8255::SetInputBufferFull(unsigned port, bool state,
                                   std::uint64_t timestamp)
{
    m_ibf[port] = state;
    CheckInterrupt(port, timestamp);
    DrivePortC(timestamp);
}

void Intel8255::SetOutputBufferFull(unsigned port, bool state,
                                    std::uint64_t timestamp)
{
    m_obf[port] = state;
    CheckInterrupt(port, timestamp);
    if (port == PortA && GroupAMode() == Mode2)
        DrivePortA(timestamp);
    DrivePortC(timestamp);
}

void Intel8255::SetInterrupt(unsigned port, bool state,
                             std::uint64_t timestamp)
{
    if (m_intr[port] == state)
        return;
    m_intr[port] = state;
    DrivePortC(timestamp);
}

void Intel8255::SetPortCInputLine(unsigned bit, bool high,
                                  std::uint64_t timestamp)
{
    if (bit != 2 && bit != 4 && bit != 6)
        return;
    const std::uint8_t mask = static_cast<std::uint8_t>(1u << bit);
    const bool wasHigh = (m_portCInputLines & mask) != 0;
    if (high) m_portCInputLines |= mask;
    else m_portCInputLines &= static_cast<std::uint8_t>(~mask);
    if (wasHigh == high || high)
        return;

    if (bit == 4 && (GroupAMode() == Mode2 ||
        (GroupAMode() == Mode1 && PortAIsInput())))
    {
        m_input[PortA] = ReadPins(PortA, timestamp).value;
        SetInputBufferFull(PortA, true, timestamp);
    }
    else if (bit == 6 && (GroupAMode() == Mode2 ||
             (GroupAMode() == Mode1 && !PortAIsInput())))
    {
        SetOutputBufferFull(PortA, true, timestamp);
    }
    else if (bit == 2 && GroupBMode() == Mode1)
    {
        if (PortBIsInput())
        {
            m_input[PortB] = ReadPins(PortB, timestamp).value;
            SetInputBufferFull(PortB, true, timestamp);
        }
        else
        {
            SetOutputBufferFull(PortB, true, timestamp);
        }
    }
}

Intel8255Snapshot Intel8255::GetSnapshot() const
{
    Intel8255Snapshot snapshot;
    snapshot.control = m_control;
    for (unsigned i = 0; i < 3; ++i)
    {
        snapshot.outputLatch[i] = m_output[i];
        snapshot.drivenValue[i] = m_drivenValue[i];
        snapshot.drivenMask[i] = m_drivenMask[i];
    }
    for (unsigned i = 0; i < 2; ++i)
    {
        snapshot.inputLatch[i] = m_input[i];
        snapshot.inputBufferFull[i] = m_ibf[i];
        snapshot.outputBufferFull[i] = m_obf[i];
        snapshot.interruptEnable[i] = m_inte[i];
        snapshot.interrupt[i] = m_intr[i];
    }
    snapshot.interruptEnableAInput = m_inteAInput;
    snapshot.interruptEnableAOutput = m_inteAOutput;
    return snapshot;
}

void Intel8255::SaveState(std::ostream& stream) const
{
    StateWriter writer(stream);
    writer.U16(kStateVersion);
    writer.U8(m_control);
    for (std::uint8_t value : m_output) writer.U8(value);
    for (std::uint8_t value : m_input) writer.U8(value);
    for (bool value : m_ibf) writer.Bool(value);
    for (bool value : m_obf) writer.Bool(value);
    for (bool value : m_inte) writer.Bool(value);
    writer.Bool(m_inteAInput);
    writer.Bool(m_inteAOutput);
    for (bool value : m_intr) writer.Bool(value);
    writer.U8(m_portCInputLines);
    for (std::uint8_t value : m_drivenValue) writer.U8(value);
    for (std::uint8_t value : m_drivenMask) writer.U8(value);
}

bool Intel8255::LoadState(std::istream& stream)
{
    StateReader reader(stream);
    if (reader.U16() != kStateVersion)
        return false;

    const std::uint8_t control = reader.U8();
    std::uint8_t output[3];
    std::uint8_t input[2];
    bool ibf[2], obf[2], inte[2], intr[2];
    for (auto& value : output) value = reader.U8();
    for (auto& value : input) value = reader.U8();
    for (auto& value : ibf) value = reader.Bool();
    for (auto& value : obf) value = reader.Bool();
    for (auto& value : inte) value = reader.Bool();
    const bool inteAInput = reader.Bool();
    const bool inteAOutput = reader.Bool();
    for (auto& value : intr) value = reader.Bool();
    const std::uint8_t portCInputLines = reader.U8();
    std::uint8_t drivenValue[3], drivenMask[3];
    for (auto& value : drivenValue) value = reader.U8();
    for (auto& value : drivenMask) value = reader.U8();
    if (!reader.Ok())
        return false;

    m_control = control;
    for (unsigned i = 0; i < 3; ++i)
    {
        m_output[i] = output[i];
        m_drivenValue[i] = drivenValue[i];
        m_drivenMask[i] = drivenMask[i];
    }
    for (unsigned i = 0; i < 2; ++i)
    {
        m_input[i] = input[i];
        m_ibf[i] = ibf[i];
        m_obf[i] = obf[i];
        m_inte[i] = inte[i];
        m_intr[i] = intr[i];
    }
    m_inteAInput = inteAInput;
    m_inteAOutput = inteAOutput;
    m_portCInputLines = portCInputLines;
    // Deliberately do not call DriveAll(): loading a chip state must not
    // create peripheral edges. The owner restores its wiring in its section.
    return true;
}
