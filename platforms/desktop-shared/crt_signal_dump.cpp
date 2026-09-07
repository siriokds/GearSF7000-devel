/*
 * GearSF7000 - CRT signal dump sink
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cstdio>

#include "crt_signal_dump.h"

// File layout, little-endian throughout:
//   "GCRT" magic, u8 version (1), then a flat run of records.
// Every record is: u8 type, u16 dots. RecordConfigure additionally carries
// u8 isPAL and u8 signalOutput; RecordData additionally carries u16 count
// followed by that many palette indices.
namespace
{
constexpr std::uint8_t kVersion = 1;
}

CrtSignalDumpSink::CrtSignalDumpSink(int linesToCapture)
    : m_LinesToCapture(linesToCapture)
{
    m_Buffer.push_back('G');
    m_Buffer.push_back('C');
    m_Buffer.push_back('R');
    m_Buffer.push_back('T');
    m_Buffer.push_back(kVersion);
}

bool CrtSignalDumpSink::IsComplete() const
{
    return m_LinesSeen >= m_LinesToCapture;
}

void CrtSignalDumpSink::AppendU16(int value)
{
    m_Buffer.push_back(static_cast<std::uint8_t>(value & 0xFF));
    m_Buffer.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void CrtSignalDumpSink::AppendRecord(RecordType type, int dots)
{
    m_Buffer.push_back(static_cast<std::uint8_t>(type));
    AppendU16(dots);
}

void CrtSignalDumpSink::ConfigureSignal(bool isPAL,
    TMS9918RasterTiming::SignalOutput signalOutput)
{
    AppendRecord(RecordConfigure, 0);
    m_Buffer.push_back(isPAL ? 1 : 0);
    m_Buffer.push_back(static_cast<std::uint8_t>(signalOutput));
}

void CrtSignalDumpSink::OutputSync(int dots)
{
    if (IsComplete())
        return;
    // One sync per line, so this is where lines get counted.
    m_LinesSeen++;
    AppendRecord(RecordSync, dots);
}

void CrtSignalDumpSink::OutputBlank(int dots)
{
    if (IsComplete())
        return;
    AppendRecord(RecordBlank, dots);
}

void CrtSignalDumpSink::OutputColourBurst(int dots)
{
    if (IsComplete())
        return;
    AppendRecord(RecordColourBurst, dots);
}

void CrtSignalDumpSink::OutputData(int dots,
    const std::uint8_t* colourIndices, std::size_t count)
{
    if (IsComplete())
        return;
    AppendRecord(RecordData, dots);
    AppendU16(static_cast<int>(count));
    m_Buffer.insert(m_Buffer.end(), colourIndices, colourIndices + count);
}

bool CrtSignalDumpSink::WriteTo(const char* path) const
{
    FILE* file = fopen(path, "wb");
    if (file == NULL)
        return false;
    if (!m_Buffer.empty())
        fwrite(m_Buffer.data(), 1, m_Buffer.size(), file);
    fclose(file);
    return true;
}
