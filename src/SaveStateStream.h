/*
 * GearSF7000 - save-state primitives
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_SAVE_STATE_STREAM_H
#define GEARSF7000_SAVE_STATE_STREAM_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <istream>
#include <ostream>
#include <string>

// Fixed-width little-endian primitives for the save-state format.
//
// Everything goes through these rather than writing structs directly, for two
// reasons: a state written on one of the development machines has to load on
// the other, and a field list that is read back in the same explicit order it
// was written cannot silently drift when a struct gains padding.
//
// Both classes latch a failure flag instead of throwing. A truncated or
// malformed state therefore surfaces as Ok() == false at the end of a section,
// which is what the section reader checks, rather than as a partially applied
// device state.
class StateWriter
{
public:
    explicit StateWriter(std::ostream& stream) : m_stream(stream) {}

    void U8(std::uint8_t value) { Raw(&value, 1); }
    void Bool(bool value) { U8(value ? 1 : 0); }

    void U16(std::uint16_t value)
    {
        std::uint8_t bytes[2] = {
            static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(value >> 8)};
        Raw(bytes, sizeof(bytes));
    }

    void U32(std::uint32_t value)
    {
        std::uint8_t bytes[4];
        for (int i = 0; i < 4; ++i)
            bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
        Raw(bytes, sizeof(bytes));
    }

    void U64(std::uint64_t value)
    {
        std::uint8_t bytes[8];
        for (int i = 0; i < 8; ++i)
            bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
        Raw(bytes, sizeof(bytes));
    }

    void I8(std::int8_t value) { U8(static_cast<std::uint8_t>(value)); }
    void I16(std::int16_t value) { U16(static_cast<std::uint16_t>(value)); }
    void I32(std::int32_t value) { U32(static_cast<std::uint32_t>(value)); }
    void I64(std::int64_t value) { U64(static_cast<std::uint64_t>(value)); }

    // Host-endian blob. Reserved for byte arrays such as RAM and VRAM, where
    // there is no element width to swap.
    void Bytes(const void* data, std::size_t size) { Raw(data, size); }

    void String(const std::string& value)
    {
        U32(static_cast<std::uint32_t>(value.size()));
        Raw(value.data(), value.size());
    }

    bool Ok() const { return m_ok && m_stream.good(); }

private:
    void Raw(const void* data, std::size_t size)
    {
        if (!m_ok)
            return;
        m_stream.write(static_cast<const char*>(data),
                       static_cast<std::streamsize>(size));
        if (!m_stream)
            m_ok = false;
    }

    std::ostream& m_stream;
    bool m_ok = true;
};

class StateReader
{
public:
    explicit StateReader(std::istream& stream) : m_stream(stream) {}

    std::uint8_t U8()
    {
        std::uint8_t value = 0;
        Raw(&value, 1);
        return value;
    }

    bool Bool() { return U8() != 0; }

    std::uint16_t U16()
    {
        std::uint8_t bytes[2] = {0, 0};
        Raw(bytes, sizeof(bytes));
        return static_cast<std::uint16_t>(bytes[0] |
                                          (std::uint16_t(bytes[1]) << 8));
    }

    std::uint32_t U32()
    {
        std::uint8_t bytes[4] = {0, 0, 0, 0};
        Raw(bytes, sizeof(bytes));
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i)
            value |= std::uint32_t(bytes[i]) << (8 * i);
        return value;
    }

    std::uint64_t U64()
    {
        std::uint8_t bytes[8] = {};
        Raw(bytes, sizeof(bytes));
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i)
            value |= std::uint64_t(bytes[i]) << (8 * i);
        return value;
    }

    std::int8_t I8() { return static_cast<std::int8_t>(U8()); }
    std::int16_t I16() { return static_cast<std::int16_t>(U16()); }
    std::int32_t I32() { return static_cast<std::int32_t>(U32()); }
    std::int64_t I64() { return static_cast<std::int64_t>(U64()); }

    void Bytes(void* data, std::size_t size) { Raw(data, size); }

    // Discards bytes without needing a destination; used to step over a
    // section this build does not understand.
    void Skip(std::size_t size)
    {
        if (!m_ok)
            return;
        m_stream.ignore(static_cast<std::streamsize>(size));
        if (!m_stream)
            m_ok = false;
    }

    std::string String()
    {
        const std::uint32_t size = U32();
        std::string value;
        if (!m_ok)
            return value;
        value.resize(size);
        if (size > 0)
            Raw(&value[0], size);
        return value;
    }

    bool Ok() const { return m_ok; }

private:
    void Raw(void* data, std::size_t size)
    {
        if (!m_ok)
            return;
        m_stream.read(static_cast<char*>(data),
                      static_cast<std::streamsize>(size));
        if (m_stream.gcount() != static_cast<std::streamsize>(size))
            m_ok = false;
    }

    std::istream& m_stream;
    bool m_ok = true;
};

#endif
