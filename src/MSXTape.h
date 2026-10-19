// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TapeRecorder.h"
#include "audio/IAYPorts.h"
#include "IDebugEventProvider.h"

// MSX cassette adapter. The bus owns this device and advances Tick with CPU
// cycles. AY reads only sample the level; their frame-relative timestamp must
// not be used to advance the transport again. PPI drives motor/output directly.
class MSXTape final : public TapeRecorder, public IAYPorts, public IDebugEventProvider
{
public:
    bool HasMedia() const { return Cassette != nullptr; }
    void SaveState(std::ostream& stream) const;
    bool LoadState(std::istream& stream, bool apply = true);
    DebugDeviceDefinition DescribeDebugDevice() const override {
        DebugDeviceDefinition device;
        device.key = "event.msx.tape";
        device.parentKey = "event.msx";
        device.typeKey = "msx.cassette";
        device.name = "MSX Tape";
        device.traits = DebugDeviceTrait_External;
        DebugSpaceDefinition state;
        state.id = DebugCommonSpace::State;
        state.key = "state"; state.name = "Tape State";
        state.targetMax = TapeEventMotor;
        state.supportedAccesses = DebugEventAccess_Event;
        state.afterValueAccesses = DebugEventAccess_Event;
        state.capabilities = DebugDescriptorCapability_EventsPublished;
        state.targets = {{TapeEventLoaded, "Loaded", 1}, {TapeEventPlaying, "Playing", 1},
            {TapeEventRewind, "Rewind", 1}, {TapeEventTargetSpeed, "Target Speed", 8},
            {TapeEventMotor, "Motor", 1}};
        device.spaces.push_back(std::move(state));
        return device;
    }
    std::string GetDescription() const override { return "MSX cassette input (AY port A, bit 7)"; }
    InputPins PeekPort(unsigned port, std::int64_t) const override {
        return port == 0 ? InputPins{std::uint8_t(GetSignal() ? 0x80 : 0), 0x80}
                         : InputPins{0xFF, 0};
    }
    // MSX cassette output is on the PPI, not on AY port B.
    void WritePort(unsigned, std::uint8_t, std::uint8_t, std::int64_t) override {}
    void SetOutput(bool high) { m_output = high; }
    bool GetOutput() const { return m_output; }
    void Reset(int masterClock) override {
        TapeRecorder::Reset(masterClock);
        m_output = false;
    }
private:
    bool m_output = false; // Line latch only; recording is not implemented yet.
};
