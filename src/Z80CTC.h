// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Saverio Russo
// Behaviour adapted from MAME's Z80 CTC (Wilbert Pol; original Tatsuyuki Satoh).
// See DOCS/licenses/Z80CTC-BSD-3-Clause.txt.
#ifndef GEARSF7000_Z80CTC_H
#define GEARSF7000_Z80CTC_H

#include "IDebugEventProvider.h"
#include <array>
#include <cstdint>
#include <iosfwd>

enum class Z80CTCEventKind : std::uint8_t
{
    Read, Write, TerminalCount, InterruptAcknowledge, Reti, Reset
};

struct Z80CTCEvent
{
    Z80CTCEventKind kind;
    unsigned channel; // 0..3; 4 for a chip-wide event
    std::uint8_t value;
    std::uint64_t timestamp;
};

// Logical asserted levels, not active-low electrical voltages. No owner needed
// for an isolated device. Callbacks must not reset/reprogram/advance the chip;
// ZC/TO -> SetTrigger at the SAME timestamp is supported for channel chaining.
class IZ80CTCPins
{
public:
    virtual ~IZ80CTCPins() = default;
    virtual void SetInterrupt(bool asserted, std::uint64_t timestamp) = 0;
    virtual void SetZeroCount(unsigned channel, bool high, std::uint64_t timestamp) = 0;
    virtual void OnCTCEvent(const Z80CTCEvent&) {}
};

struct Z80CTCChannelSnapshot
{
    std::uint8_t control = 0x02;
    std::uint16_t constant = 256;
    std::uint16_t counter = 256; // zero on the data bus represents 256
    std::uint16_t prescalerPhase = 0;
    bool loaded = false;
    bool running = false;
    bool waitingForTrigger = false;
    bool triggerHigh = false;
    bool pending = false;
    bool inService = false;
    bool zeroCountHigh = false; // physical outputs exist on channels 0..2 only
};

struct Z80CTCSnapshot
{
    std::uint64_t timestamp = 0;
    std::uint8_t vector = 0;
    bool interruptEnableIn = true;
    std::array<Z80CTCChannelSnapshot, 4> channels{};
};

struct Z80CTCDebugConfiguration
{
    std::string key;
    std::string parentKey;
    std::string name;
    // Set only when the owner actually routes OnCTCEvent to Debug Events.
    bool eventsPublished = false;
};

class Z80CTC : public IDebugEventProvider
{
public:
    explicit Z80CTC(IZ80CTCPins* pins = nullptr, Z80CTCDebugConfiguration debug = {});
    DebugDeviceDefinition DescribeDebugDevice() const override;
    void Reset(std::uint64_t timestamp = 0);

    // Absolute CTC input-clock ticks. The bus converts CPU time if clocks differ.
    // Operations advance first; edges expiring at t precede a CPU access at t.
    // Backward time is a caller error (throws); LoadState/Reset can restore time.
    void AdvanceTo(std::uint64_t timestamp);
    std::uint8_t ReadRegister(unsigned channel, std::uint64_t timestamp);
    std::uint8_t PeekRegister(unsigned channel) const; // current time, no effects
    void WriteRegister(unsigned channel, std::uint8_t value, std::uint64_t timestamp);
    void SetTrigger(unsigned channel, bool high, std::uint64_t timestamp);

    void SetInterruptEnableIn(bool enabled, std::uint64_t timestamp);
    bool GetInterrupt() const;
    bool GetInterruptEnableOut() const;
    // False means this device did not respond; vector is left untouched.
    bool AcknowledgeInterrupt(std::uint8_t& vector, std::uint64_t timestamp);
    bool Reti(std::uint64_t timestamp);

    Z80CTCSnapshot GetSnapshot() const { return m_state; }
    void SaveState(std::ostream& stream) const;
    // Transactional, validated, and silent: owner restores/reconciles wiring.
    bool LoadState(std::istream& stream);

private:
    int EligibleInterrupt() const;
    void UpdateInterrupt();
    void TerminalCount(unsigned channel);
    void Emit(Z80CTCEventKind kind, unsigned channel, std::uint8_t value);
    static unsigned Prescaler(const Z80CTCChannelSnapshot& channel);
    Z80CTCSnapshot m_state;
    IZ80CTCPins* m_pins;
    Z80CTCDebugConfiguration m_debug;
    bool m_interrupt = false;
};
#endif
