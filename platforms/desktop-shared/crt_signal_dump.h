/*
 * GearSF7000 - CRT signal dump sink
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CRT_SIGNAL_DUMP_H
#define CRT_SIGNAL_DUMP_H

#include <cstdint>
#include <vector>

#include "../../src/ICrtSignalSink.h"

// An ICrtSignalSink that records the call sequence verbatim - one record per
// call, in emission order - instead of decoding it. Two uses: checking that
// the durations and ordering the VDP emits are what the raster tables say,
// and feeding an offline decoder that turns the signal into an image without
// any of it having to run inside the emulator.
//
// Capture is bounded: it stops on its own once the requested number of lines
// has been seen, so a capture cannot grow without limit if it is left
// plugged in. Every line emits exactly one sync record, which is what the
// line count counts.
class CrtSignalDumpSink final : public ICrtSignalSink
{
public:
    enum RecordType : std::uint8_t
    {
        RecordConfigure = 0,
        RecordSync = 1,
        RecordBlank = 2,
        RecordColourBurst = 3,
        RecordData = 4
    };

    explicit CrtSignalDumpSink(int linesToCapture);

    bool IsComplete() const;
    // Writes the capture to path. Returns false if the file cannot be
    // opened; a short or empty capture is still written, deliberately, so a
    // failed run can be inspected rather than silently discarded.
    bool WriteTo(const char* path) const;

    void ConfigureSignal(bool isPAL,
        TMS9918RasterTiming::SignalOutput signalOutput) override;
    void OutputSync(int dots) override;
    void OutputBlank(int dots) override;
    void OutputColourBurst(int dots) override;
    void OutputData(int dots, const std::uint8_t* colourIndices,
        std::size_t count) override;

private:
    void AppendRecord(RecordType type, int dots);
    void AppendU16(int value);

    std::vector<std::uint8_t> m_Buffer;
    int m_LinesToCapture;
    int m_LinesSeen = 0;
};

#endif // CRT_SIGNAL_DUMP_H
