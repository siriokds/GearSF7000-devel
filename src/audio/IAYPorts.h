// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>

// External devices attached to the AY parallel pins, independent of audio
// synthesis or machine type. Port 0 is A (R14), port 1 is B (R15).
// Time is in the AY host CPU-cycle domain, relative to the current audio frame,
// just like Ay_Apu::write(). The owner supplies any absolute-time conversion.
//
// The attachment is owned by the machine, never by AY. MSX connects MSXTape;
// other current machines leave it null. A device may compose and describe all of
// them in GetDescription(); MCP get_ay8910_status exposes that string through
// io_attachment.description, with connected=false/description=null when absent.
// Input drivenMask selects supplied pins; the remaining pins read high.
// R7 bits 6/7 select input (0) or output (1); input writes only update the latch.
//
// Save states contain AY registers, not this pointer or the peripheral state.
// The machine must save/restore its peripheral separately and call SyncPorts
// only after committing a restore, never while validating a candidate state.
class IAYPorts
{
public:
    struct InputPins {
        std::uint8_t value;
        std::uint8_t drivenMask;
    };
    virtual ~IAYPorts() = default;
    // Human-readable description, also exposed by MCP. Must not perform I/O.
    virtual std::string GetDescription() const = 0;
    virtual InputPins PeekPort(unsigned port, std::int64_t time) const = 0;
    virtual InputPins ReadPort(unsigned port, std::int64_t time) {
        return PeekPort(port, time);
    }
    // drivenMask is FF for output, 00 for input/high impedance. value is
    // the output latch, including when a direction change releases the pins.
    virtual void WritePort(unsigned port, std::uint8_t value,
                           std::uint8_t drivenMask, std::int64_t time) = 0;
};
