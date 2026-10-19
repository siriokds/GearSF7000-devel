// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CONTROLLER_DEVICE_H
#define CONTROLLER_DEVICE_H

#include "definitions.h"
#include "IDebugEventProvider.h"

#include <memory>
#include <string>

class Memory;
class StateReader;
class StateWriter;

// What is plugged into a Master System controller port. Persisted in the
// configuration: append only.
enum class SystemControllerDevice : u8
{
    SmsPad = 0,     // the Master System's own two-button pad
    MegaDrive3 = 1, // Mega Drive pad, three buttons plus Start
    MegaDrive6 = 2  // Mega Drive pad, six buttons plus Start and Mode
};

constexpr bool IsValidControllerDevice(int value)
{
    return value >= 0 && value <= static_cast<int>(SystemControllerDevice::MegaDrive6);
}
const char* GetControllerDeviceName(SystemControllerDevice type);

// A controller port's six lines are up, down, left, right, TL and TR, active low,
// plus TH, which the Master System drives through port $3F. The directions, TL (B,
// button 1) and TR (C, button 2) are the SMS pad's lines as the joystick row holds
// them; the buttons only a Mega Drive pad has are held here.
//
// Each device is its own debugger device under the machine: what the port read
// from it, and on a Mega Drive pad TH, its extra buttons and (six buttons) the
// counter, are published as state changes.
class ControllerDevice : public IDebugEventProvider
{
public:
    // A Mega Drive pad's buttons beyond the SMS pad's two.
    enum Button : u8
    {
        ButtonA = 1u << 0,
        ButtonStart = 1u << 1,
        ButtonX = 1u << 2,
        ButtonY = 1u << 3,
        ButtonZ = 1u << 4,
        ButtonMode = 1u << 5
    };

    // Targets of the device's "state" space.
    enum StateTarget : u32
    {
        TargetLines = 0,   // the six lines the port last read, active low
        TargetTh = 1,      // TH as the port drives it
        TargetButtons = 2, // the extra buttons held, active high
        TargetCounter = 3  // six buttons: the phase, 0-3
    };

    static std::unique_ptr<ControllerDevice> Create(SystemControllerDevice type);

    ~ControllerDevice() override = default;
    virtual SystemControllerDevice GetType() const = 0;

    // The six lines the port reads (bits 0-5, active low), given the SMS pad's lines
    // for this player and TH as the port drives it now.
    u8 Read(u8 smsLines, bool th, u64 tstates);
    // TH as the port drives it, on every port $3F write.
    void OnTh(bool th, u64 tstates);
    void SetButton(u8 buttons, bool pressed);
    u8 GetButtons() const { return m_buttons; }
    bool GetTh() const { return m_th; }
    virtual void Reset();

    // Buttons held and the device type are not state: the host holds the first,
    // the configuration the second.
    virtual void SaveState(StateWriter& w) const;
    virtual void LoadState(StateReader& r);

    void SetDebugKeys(std::string key, std::string parentKey);
    void SetMemory(Memory* memory) { m_memory = memory; }
    void BindDebugEvents(const DebugDeviceRegistry& registry);
    DebugDeviceDefinition DescribeDebugDevice() const override;

protected:
    virtual u8 Lines(u8 smsLines, bool th, u64 tstates) = 0;
    virtual void ThChanged(bool th, u64 tstates) { (void)th; (void)tstates; }
    virtual const char* TypeKey() const = 0;
    virtual bool UsesTh() const { return true; }
    virtual bool HasCounter() const { return false; }
    void Publish(StateTarget target, u64 before, u64 after);

    // A button's line: low while it is pressed.
    u8 Low(u8 button, int bit) const
    {
        return static_cast<u8>((m_buttons & button) ? 0 : (1u << bit));
    }

    u8 m_buttons = 0;
    bool m_th = true;

private:
    Memory* m_memory = nullptr;
    std::string m_debugKey;
    std::string m_debugParentKey;
    DebugDeviceId m_debugDevice = 0;
    u8 m_lastLines = 0x3F;
};

// The Master System pad: its lines as they are, TH not connected.
class SmsPadDevice final : public ControllerDevice
{
public:
    SystemControllerDevice GetType() const override { return SystemControllerDevice::SmsPad; }

protected:
    u8 Lines(u8 smsLines, bool, u64) override { return smsLines; }
    const char* TypeKey() const override { return "sega.sms_pad"; }
    bool UsesTh() const override { return false; }
};

// The three-button Mega Drive pad: a 74HC157 multiplexer, TH picking which half
// of the buttons reaches the lines. No memory, no timing.
//   TH high: up down left right  B  C
//   TH low:  up down 0    0      A  Start   (left and right low: a Mega Drive pad)
class MegaDrive3Device : public ControllerDevice
{
public:
    SystemControllerDevice GetType() const override { return SystemControllerDevice::MegaDrive3; }

protected:
    u8 Lines(u8 smsLines, bool th, u64) override
    {
        if (th)
            return smsLines;
        return static_cast<u8>(Low(ButtonA, 4) | Low(ButtonStart, 5) | (smsLines & 0x03));
    }
    const char* TypeKey() const override { return "sega.md_pad3"; }
};

// The six-button Mega Drive pad, as MAME models it on the Master System port
// (src/devices/bus/sms_ctrl/md6bt.cpp). Sega's chip steps a phase, 0-3, on each
// low-to-high edge of TH. A rising edge also starts a 1.5 ms timer, stopped while
// TH is low; if it runs out the phase returns to 0, so a read once a frame starts
// from the top. Phases 0 and 1 are the 3-button multiplexer; then:
//   phase 2, TH low:  0 0 0 0      A  Start   (all four low: six buttons)
//   phase 3, TH high: Z Y X Mode   B  C
//   phase 3, TH low:  1 1 1 1      A  Start
// Mode held at the first reset puts it in 3-button mode for good, for games a
// six-button pad confuses (MAME latches it once, at device_reset).
class MegaDrive6Device final : public MegaDrive3Device
{
public:
    // 1.5 ms at the Master System's 3.58 MHz CPU clock.
    static constexpr u64 kTimeout = 5369;

    SystemControllerDevice GetType() const override { return SystemControllerDevice::MegaDrive6; }
    u8 GetPhase() const { return m_phase; }
    bool IsSixButtonMode() const { return m_sixButtons; }
    void Reset() override;
    void SaveState(StateWriter& w) const override;
    void LoadState(StateReader& r) override;

protected:
    u8 Lines(u8 smsLines, bool th, u64 tstates) override;
    void ThChanged(bool th, u64 tstates) override;
    const char* TypeKey() const override { return "sega.md_pad6"; }
    bool HasCounter() const override { return true; }

private:
    // The timer, run lazily: whenever the pad is looked at, a timer that would
    // have fired by now has.
    void Expire(u64 tstates);
    void SetPhase(u8 phase);

    u8 m_phase = 0;
    bool m_sixButtons = true;
    bool m_modeLatched = false;
    bool m_timerArmed = false;
    u64 m_timerStart = 0;
};

#endif
