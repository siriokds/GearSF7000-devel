// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#include "MSXKeyboard.h"
#include "SaveStateStream.h"
#include <cctype>

namespace {
// Row/bit wiring checked against openMSX Keyboard.cc. Bits are active low.
constexpr const char* labels[MSXKeyboard::RowCount][8] = {
    {"0", "1", "2", "3", "4", "5", "6", "7"},
    {"8", "9", "-", "=", "\\", "[", "]", ";"},
    {"'", "`", ",", ".", "/", "Accent", "A", "B"},
    {"C", "D", "E", "F", "G", "H", "I", "J"},
    {"K", "L", "M", "N", "O", "P", "Q", "R"},
    {"S", "T", "U", "V", "W", "X", "Y", "Z"},
    {"Shift", "Ctrl", "Graph", "Caps", "Code", "F1", "F2", "F3"},
    {"F4", "F5", "Esc", "Tab", "Stop", "Backspace", "Select", "Return"},
    {"Space", "Home", "Insert", "Delete", "Left", "Up", "Down", "Right"},
    {"KP*", "KP+", "KP/", "KP0", "KP1", "KP2", "KP3", "KP4"},
    {"KP5", "KP6", "KP7", "KP8", "KP9", "KP-", "KP,", "KP."}
};
std::string Lower(std::string text)
{
    for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}
}
const char* MSXKeyboard::KeyLabel(unsigned row, unsigned bit)
{ return row < RowCount && bit < 8 ? labels[row][bit] : ""; }
void MSXKeyboard::Reset() { m_host.fill(0xFF); m_labels.fill(0xFF); }
u8 MSXKeyboard::ReadRow(unsigned row) const
{ return row < RowCount ? m_host[row] & m_labels[row] : 0xFF; }
void MSXKeyboard::ReleaseHeldKeys()
{
    for (unsigned row = 0; row < RowCount; ++row) {
        const auto before = ReadRow(row);
        m_host[row] = m_labels[row] = 0xFF;
        if (m_change && before != 0xFF) m_change(row, before, 0xFF);
    }
}
bool MSXKeyboard::SetKey(std::array<u8, RowCount>& source, int row, int mask, bool pressed)
{
    if (row < 0 || row >= int(RowCount) || mask <= 0 || mask > 255) return false;
    const auto before = ReadRow(row);
    source[row] = pressed ? source[row] & ~mask : source[row] | mask;
    const auto after = ReadRow(row);
    if (m_change && before != after) m_change(row, before, after);
    return true;
}
bool MSXKeyboard::SetMatrixKeyState(int row, int mask, bool pressed)
{ return SetKey(m_host, row, mask, pressed); }
bool MSXKeyboard::PressMatrixKeyByLabel(const std::string& label, bool pressed)
{
    auto wanted = Lower(label);
    if (wanted == "escape") wanted = "esc";
    if (wanted == "enter") wanted = "return";
    if (wanted == "control") wanted = "ctrl";
    if (wanted == "capslock") wanted = "caps";
    if (wanted == "del") wanted = "delete";
    if (wanted == "ins") wanted = "insert";
    for (unsigned row = 0; row < RowCount; ++row)
        for (unsigned bit = 0; bit < 8; ++bit)
            if (wanted == Lower(labels[row][bit]))
                return SetKey(m_labels, row, 1 << bit, pressed);
    return false;
}
DebugDeviceDefinition MSXKeyboard::DescribeDebugDevice() const
{
    DebugDeviceDefinition device;
    device.key = "event.msx.keyboard"; device.parentKey = "event.msx";
    device.typeKey = "msx.keyboard"; device.name = "MSX Keyboard";
    DebugSpaceDefinition matrix;
    matrix.id = DebugCommonSpace::CustomFirst; matrix.key = "matrix_rows";
    matrix.name = "Keyboard Matrix Rows"; matrix.targetMax = RowCount - 1;
    matrix.supportedAccesses = matrix.beforeValueAccesses = matrix.afterValueAccesses = DebugEventAccess_Event;
    for (unsigned row = 0; row < RowCount; ++row)
        matrix.targets.push_back({row, "Row " + std::to_string(row), 8});
    device.spaces.push_back(std::move(matrix));
    DebugRulePresetDefinition preset;
    preset.key = "matrix"; preset.name = "Keyboard contacts"; preset.spaceKey = "matrix_rows";
    preset.targetEnd = RowCount - 1; preset.accessMask = DebugEventAccess_Event;
    device.presets.push_back(std::move(preset));
    return device;
}
void MSXKeyboard::SaveState(std::ostream& stream) const
{
    StateWriter writer(stream); writer.U16(1);
    for (auto value : m_host) writer.U8(value);
    for (auto value : m_labels) writer.U8(value);
}
bool MSXKeyboard::LoadState(std::istream& stream)
{
    StateReader reader(stream);
    if (reader.U16() != 1) return false;
    std::array<u8, RowCount> host, labels;
    for (auto& value : host) value = reader.U8();
    for (auto& value : labels) value = reader.U8();
    if (!reader.Ok()) return false;
    m_host = host; m_labels = labels;
    return true;
}
