// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Saverio Russo
// Behaviour adapted from MAME's Z80 CTC (Wilbert Pol; original Tatsuyuki Satoh).
// See DOCS/licenses/Z80CTC-BSD-3-Clause.txt.
#include "Z80CTC.h"
#include "SaveStateStream.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace
{
constexpr std::uint8_t Interrupt = 0x80, Counter = 0x40, Divide256 = 0x20;
constexpr std::uint8_t Rising = 0x10, ExternalStart = 0x08, ConstantFollows = 0x04;
constexpr std::uint8_t SoftwareReset = 0x02, ControlWord = 0x01;
constexpr std::uint16_t StateVersion = 1;
}

Z80CTC::Z80CTC(IZ80CTCPins* pins, Z80CTCDebugConfiguration debug)
    : m_pins(pins), m_debug(std::move(debug)) {}

unsigned Z80CTC::Prescaler(const Z80CTCChannelSnapshot& c)
{
    return (c.control & Divide256) ? 256 : 16;
}

void Z80CTC::Emit(Z80CTCEventKind kind, unsigned channel, std::uint8_t value)
{
    if (m_pins) m_pins->OnCTCEvent({kind, channel, value, m_state.timestamp});
}

void Z80CTC::Reset(std::uint64_t timestamp)
{
    const auto previous = m_state;
    m_state = {};
    m_state.timestamp = timestamp;
    m_state.interruptEnableIn = previous.interruptEnableIn;
    UpdateInterrupt();
    if (m_pins)
        for (unsigned i = 0; i < 3; ++i)
            if (previous.channels[i].zeroCountHigh)
                m_pins->SetZeroCount(i, false, timestamp);
    Emit(Z80CTCEventKind::Reset, 4, 0);
}

void Z80CTC::AdvanceTo(std::uint64_t timestamp)
{
    if (timestamp < m_state.timestamp)
        throw std::invalid_argument("Z80CTC: backward clock timestamp");
    while (m_state.timestamp < timestamp)
    {
        std::uint64_t step = timestamp - m_state.timestamp;
        for (const auto& c : m_state.channels)
        {
            if (c.zeroCountHigh) step = 1; // one input-clock-wide pulse
            if (c.running && !(c.control & Counter))
                step = std::min(step, std::uint64_t(c.counter) * Prescaler(c) - c.prescalerPhase);
        }
        bool expired[4] = {}, falling[4] = {};
        m_state.timestamp += step;
        for (unsigned i = 0; i < 4; ++i)
        {
            auto& c = m_state.channels[i];
            falling[i] = c.zeroCountHigh;
            c.zeroCountHigh = false;
            if (!c.running || (c.control & Counter)) continue;
            const auto phase = c.prescalerPhase + step;
            c.counter -= static_cast<std::uint16_t>(phase / Prescaler(c));
            c.prescalerPhase = static_cast<std::uint16_t>(phase % Prescaler(c));
            expired[i] = c.counter == 0;
            if (expired[i]) c.counter = c.constant;
        }
        // All channels share the same timestamp before any external callback.
        for (unsigned i = 0; i < 3; ++i)
            if (falling[i] && m_pins) m_pins->SetZeroCount(i, false, m_state.timestamp);
        for (unsigned i = 0; i < 4; ++i)
            if (expired[i]) TerminalCount(i);
    }
}

std::uint8_t Z80CTC::PeekRegister(unsigned channel) const
{
    return static_cast<std::uint8_t>(m_state.channels[channel & 3].counter);
}

std::uint8_t Z80CTC::ReadRegister(unsigned channel, std::uint64_t timestamp)
{
    AdvanceTo(timestamp);
    const auto result = PeekRegister(channel);
    Emit(Z80CTCEventKind::Read, channel & 3, result);
    return result;
}

void Z80CTC::WriteRegister(unsigned channel, std::uint8_t value, std::uint64_t timestamp)
{
    AdvanceTo(timestamp);
    channel &= 3;
    auto& c = m_state.channels[channel];
    if (c.control & ConstantFollows)
    {
        c.constant = value ? value : 256;
        c.control &= ~(ConstantFollows | SoftwareReset);
        c.loaded = true;
        // Zilog: changing a running channel's constant does not shorten its
        // current count; the new constant is used at the next terminal count.
        if (!c.running)
        {
            c.counter = c.constant;
            c.prescalerPhase = 0;
            c.waitingForTrigger = !(c.control & Counter) && (c.control & ExternalStart);
            c.running = !c.waitingForTrigger;
        }
    }
    else if (!(value & ControlWord))
    {
        if (channel == 0) m_state.vector = value & 0xF8;
    }
    else
    {
        const auto previous = c.control;
        c.control = value;
        if (value & SoftwareReset)
        {
            c.running = false;
            c.waitingForTrigger = false;
            c.loaded = false; // only a fresh constant can restart the channel
            c.prescalerPhase = 0;
        }
        else if ((previous ^ value) & (Counter | Divide256))
            c.prescalerPhase = 0;
        if (c.waitingForTrigger && ((value & Counter) || !(value & ExternalStart)))
        {
            c.waitingForTrigger = false;
            c.running = c.loaded;
        }
        // Software reset alone retains interrupt state. Disabling interrupts
        // clears a pending request, but not an interrupt already in service.
        if (!(value & Interrupt)) c.pending = false;
    }
    Emit(Z80CTCEventKind::Write, channel, value);
    UpdateInterrupt();
}

void Z80CTC::SetTrigger(unsigned channel, bool high, std::uint64_t timestamp)
{
    AdvanceTo(timestamp);
    channel &= 3;
    auto& c = m_state.channels[channel];
    if (high == c.triggerHigh) return;
    c.triggerHigh = high;
    if (high != bool(c.control & Rising) || !c.loaded || (c.control & SoftwareReset)) return;
    if (c.waitingForTrigger)
    {
        c.waitingForTrigger = false;
        c.running = true;
        c.prescalerPhase = 0;
    }
    else if (c.running && (c.control & Counter) && --c.counter == 0)
        TerminalCount(channel);
}

void Z80CTC::TerminalCount(unsigned channel)
{
    auto& c = m_state.channels[channel];
    c.counter = c.constant;
    if (c.control & Interrupt) c.pending = true;
    Emit(Z80CTCEventKind::TerminalCount, channel, static_cast<std::uint8_t>(c.constant));
    UpdateInterrupt();
    // Channel 3 has no ZC/TO pin on the Z8430.
    if (channel < 3 && !c.zeroCountHigh)
    {
        c.zeroCountHigh = true;
        if (m_pins) m_pins->SetZeroCount(channel, true, m_state.timestamp);
    }
}

int Z80CTC::EligibleInterrupt() const
{
    if (!m_state.interruptEnableIn) return -1;
    for (unsigned i = 0; i < 4; ++i)
    {
        const auto& c = m_state.channels[i];
        if (c.inService) break;
        if (c.pending) return static_cast<int>(i);
    }
    return -1;
}

bool Z80CTC::GetInterrupt() const { return EligibleInterrupt() >= 0; }

bool Z80CTC::GetInterruptEnableOut() const
{
    if (!m_state.interruptEnableIn) return false;
    for (const auto& c : m_state.channels)
        if (c.pending || c.inService) return false;
    return true;
}

void Z80CTC::UpdateInterrupt()
{
    const bool asserted = GetInterrupt();
    if (asserted == m_interrupt) return;
    m_interrupt = asserted;
    if (m_pins) m_pins->SetInterrupt(asserted, m_state.timestamp);
}

void Z80CTC::SetInterruptEnableIn(bool enabled, std::uint64_t timestamp)
{
    AdvanceTo(timestamp);
    m_state.interruptEnableIn = enabled;
    UpdateInterrupt();
}

bool Z80CTC::AcknowledgeInterrupt(std::uint8_t& vector, std::uint64_t timestamp)
{
    AdvanceTo(timestamp);
    const int index = EligibleInterrupt();
    if (index < 0) return false;
    auto& c = m_state.channels[index];
    c.pending = false;
    c.inService = true;
    vector = m_state.vector | (index << 1);
    Emit(Z80CTCEventKind::InterruptAcknowledge, index, vector);
    UpdateInterrupt();
    return true;
}

bool Z80CTC::Reti(std::uint64_t timestamp)
{
    AdvanceTo(timestamp);
    if (!m_state.interruptEnableIn) return false;
    for (unsigned i = 0; i < 4; ++i)
        if (m_state.channels[i].inService)
        {
            m_state.channels[i].inService = false;
            Emit(Z80CTCEventKind::Reti, i, 0);
            UpdateInterrupt();
            return true;
        }
    return false;
}

void Z80CTC::SaveState(std::ostream& stream) const
{
    StateWriter w(stream);
    w.U16(StateVersion);
    w.U64(m_state.timestamp);
    w.U8(m_state.vector);
    w.Bool(m_state.interruptEnableIn);
    for (const auto& c : m_state.channels)
    {
        w.U8(c.control); w.U16(c.constant); w.U16(c.counter); w.U16(c.prescalerPhase);
        w.Bool(c.loaded); w.Bool(c.running); w.Bool(c.waitingForTrigger);
        w.Bool(c.triggerHigh); w.Bool(c.pending); w.Bool(c.inService); w.Bool(c.zeroCountHigh);
    }
}

bool Z80CTC::LoadState(std::istream& stream)
{
    StateReader r(stream);
    if (r.U16() != StateVersion) return false;
    Z80CTCSnapshot candidate;
    candidate.timestamp = r.U64();
    candidate.vector = r.U8();
    bool valid = !(candidate.vector & 7);
    auto boolean = [&]() { const auto b = r.U8(); valid &= b <= 1; return b != 0; };
    candidate.interruptEnableIn = boolean();
    for (unsigned i = 0; i < 4; ++i)
    {
        auto& c = candidate.channels[i];
        c.control = r.U8(); c.constant = r.U16(); c.counter = r.U16(); c.prescalerPhase = r.U16();
        c.loaded = boolean(); c.running = boolean(); c.waitingForTrigger = boolean();
        c.triggerHigh = boolean(); c.pending = boolean(); c.inService = boolean(); c.zeroCountHigh = boolean();
        valid &= c.constant >= 1 && c.constant <= 256 && c.counter >= 1 && c.counter <= 256;
        valid &= c.prescalerPhase < Prescaler(c);
        valid &= !(c.running && (!c.loaded || c.waitingForTrigger || (c.control & SoftwareReset)));
        valid &= !(c.waitingForTrigger && (!c.loaded || (c.control & (Counter | SoftwareReset)) || !(c.control & ExternalStart)));
        valid &= !((c.control & Counter) && c.prescalerPhase);
        valid &= !(c.pending && !(c.control & Interrupt));
        valid &= !(i == 3 && c.zeroCountHigh);
    }
    if (!valid || !r.Ok()) return false;
    m_state = candidate;
    m_interrupt = GetInterrupt();
    return true;
}

DebugDeviceDefinition Z80CTC::DescribeDebugDevice() const
{
    DebugDeviceDefinition d;
    d.key = m_debug.key; d.parentKey = m_debug.parentKey;
    d.typeKey = "zilog.z80ctc";
    d.name = m_debug.name.empty() ? "Zilog Z80 CTC" : m_debug.name;
    DebugSpaceDefinition registers;
    registers.id = DebugCommonSpace::Registers;
    registers.key = "registers"; registers.name = "Channel ports"; registers.targetMax = 3;
    registers.supportedAccesses = DebugEventAccess_Read | DebugEventAccess_Write;
    registers.afterValueAccesses = registers.supportedAccesses;
    registers.capabilities = m_debug.eventsPublished ? DebugDescriptorCapability_EventsPublished : DebugDescriptorCapability_None;
    for (unsigned i = 0; i < 4; ++i)
        registers.targets.push_back({i, "Channel " + std::to_string(i), 8});
    d.spaces.push_back(registers);
    DebugSpaceDefinition state;
    state.id = DebugCommonSpace::State; state.key = "state"; state.name = "Channel events";
    state.targetMax = 4; state.supportedAccesses = DebugEventAccess_Event;
    state.afterValueAccesses = DebugEventAccess_Event; state.capabilities = registers.capabilities;
    state.targets = registers.targets;
    state.targets.push_back({4, "Chip reset", 8});
    d.spaces.push_back(std::move(state));
    return d;
}
