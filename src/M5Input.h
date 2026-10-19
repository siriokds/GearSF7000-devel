// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef M5_INPUT_H
#define M5_INPUT_H
#include "IDebugEventProvider.h"
#include <array>
#include <functional>
#include <iosfwd>

// Passive, active-high contacts. The owner supplies absolute CPU T-states;
// this device neither advances time nor resets the CPU itself.
class M5Input final : public IDebugEventProvider
{
public:
    static constexpr unsigned RowCount = 7;
    static constexpr unsigned JoystickTarget = 7, ResetTarget = 8, TargetCount = 9;
    enum class Source { Host, Synthetic, Controller };
    enum class Direction { Right, Up, Left, Down };
    using Change = std::function<void(unsigned, u8, u8, u64)>;
    void Reset(); // Silent power/reset initialization; callbacks remain installed.
    void Release(Source source, u64 clock);
    u8 ReadRow(unsigned row) const;
    u8 ReadJoystick() const { return Read(JoystickTarget); }
    u8 ReadResetStatus() const { return Read(ResetTarget); } // bit 7 only
    bool SetMatrixKeyState(Source source, unsigned row, unsigned mask, bool pressed, u64 clock);
    bool SetKeyByLabel(Source source, const std::string& label, bool pressed, u64 clock);
    bool SetJoystickDirection(Source source, unsigned player, Direction direction, bool pressed, u64 clock);
    bool SetJoystickFire(Source source, unsigned player, unsigned button, bool pressed, u64 clock);
    bool SetResetKey(Source source, bool pressed, u64 clock);
    // eventsPublished is an explicit promise by the owner to publish the callback.
    void SetChangeCallback(Change callback, bool eventsPublished = false);
    DebugDeviceDefinition DescribeDebugDevice() const override;
    void SaveState(std::ostream& stream) const;
    bool LoadState(std::istream& stream); // Transactional and silent.
    static const char* KeyLabel(unsigned row, unsigned bit);
private:
    using Contacts = std::array<u8, TargetCount>;
    bool Set(Source source, unsigned target, unsigned mask, bool pressed, u64 clock);
    u8 Read(unsigned target) const { return m_host[target] | m_synthetic[target] | m_controller[target]; }
    Contacts m_host{}, m_synthetic{}, m_controller{};
    Change m_change;
    bool m_eventsPublished = false;
};
#endif
