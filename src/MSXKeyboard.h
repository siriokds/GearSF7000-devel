// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#ifndef MSX_KEYBOARD_H
#define MSX_KEYBOARD_H
#include "IDebugEventProvider.h"
#include <array>
#include <functional>
#include <iosfwd>

// Standard international MSX matrix. Electrical contacts only; host layout
// policy stays in the frontend, as with SegaKeyboard::SetMatrixKeyState.
class MSXKeyboard final : public IDebugEventProvider
{
public:
    static constexpr unsigned RowCount = 11;
    using RowChange = std::function<void(unsigned, u8, u8)>;
    MSXKeyboard() { Reset(); }
    void Reset();
    void ReleaseHeldKeys();
    u8 ReadRow(unsigned row) const;
    // Independent physical-host and labelled (MCP) input sources.
    bool SetMatrixKeyState(int row, int mask, bool pressed);
    bool PressMatrixKeyByLabel(const std::string& label, bool pressed);
    void SetRowChangeCallback(RowChange callback) { m_change = std::move(callback); }
    DebugDeviceDefinition DescribeDebugDevice() const override;
    void SaveState(std::ostream& stream) const;
    bool LoadState(std::istream& stream);
    static const char* KeyLabel(unsigned row, unsigned bit);
private:
    bool SetKey(std::array<u8, RowCount>& source, int row, int mask, bool pressed);
    std::array<u8, RowCount> m_host, m_labels;
    RowChange m_change;
};
#endif
