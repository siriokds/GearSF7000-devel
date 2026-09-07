/*
 * GearSF7000 - CRT signal sink interface
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ICRT_SIGNAL_SINK_H
#define ICRT_SIGNAL_SINK_H

#include <cstddef>
#include <cstdint>

#include "TMS9918RasterTiming.h"

// Optional per-line output for a real signal-level CRT simulator (e.g. a
// bridge to Clock Signal's Outputs::CRT::CRT), kept entirely separate from
// the normal palette-index -> RGB rendering path in Video.cpp. Video calls
// these only when a sink is attached (see Video::AttachCrtSignalSink);
// with no sink attached the calls never happen, so this costs nothing when
// unused.
//
// The sequence per scanline mirrors TMS9918RasterTiming::HorizontalRegion
// in dot order, the same for both signal types: HSync -> LeftBlankA ->
// ColorBurst -> LeftBlankB -> LeftBorder -> Active -> RightBorder ->
// RightBlank. Border dots are real displayed pixels (the backdrop color)
// and are reported through OutputData like the active region, not as
// blanking.
//
// Durations are in TMS9918 dots (TMS9918RasterTiming::DotsPerLine == 342
// dots per line) - a concrete sink converts to whatever cycle unit its own
// signal-level model expects, that conversion is not this interface's
// concern.
class ICrtSignalSink
{
public:
    virtual ~ICrtSignalSink() = default;

    // Called once, whenever the signal type or region (PAL/NTSC) changes -
    // not per line. Mirrors TMS9918RasterTiming::SignalOutput: composite
    // (TMS9918A) has a color burst and no separate R-Y/B-Y outputs;
    // component (TMS9928A/TMS9929A) does not encode a burst into the same
    // signal the sink receives (see OutputColourBurst).
    virtual void ConfigureSignal(bool isPAL,
        TMS9918RasterTiming::SignalOutput signalOutput) = 0;

    // Sync pulse. Carries both kinds, distinguished only by duration -
    // there is deliberately no separate vertical-sync call:
    //   horizontal, every line: dots == HorizontalRegion::HSync's width (26)
    //   vertical, on a VerticalRegion::VerticalSync line: dots == DotsPerLine
    //     (342), i.e. the whole line is one pulse and none of the other
    //     Output* calls are made for that line.
    // A signal-level receiver recovers the field boundary from that
    // abnormally long pulse, exactly as real hardware does - CLK's
    // Flywheel works this way, and its own TMS9918 emits vertical sync as
    // output_sync(CyclesPerLine) with no vertical-specific API
    // (Components/9918/Implementation/9918.cpp, "Vertical sync" branch).
    //
    // Do not copy CLK's vertical *numbers*, only this mechanism: it puts
    // vsync about 8 lines later than the TI table and holds it for 4 lines
    // (its own comment says "2.5 or 3", so it is approximate by its own
    // admission), and it does not model the border/blanking split at all.
    // GetVerticalLineCounts is the accurate source here.
    virtual void OutputSync(int dots) = 0;

    // Blanking with no picture content and no color burst
    // (LeftBlankA, LeftBlankB, RightBlank).
    virtual void OutputBlank(int dots) = 0;

    // The color burst reference (ColorBurst region). Present for both
    // signal types, not composite-only: the TMS9928A/TMS9929A datasheet's
    // own Table 2-3 lists distinct COLOR BURST R-Y/B-Y values for both
    // chips, and CLK's own TMS9918 model (Components/9918/9918.hpp -
    // "TMS9918A, // includes the 9928 and 9929") emits this call
    // unconditionally regardless of composite/component output. Video
    // calls this for both SignalOutput values.
    virtual void OutputColourBurst(int dots) = 0;

    // Displayed pixels, border and active area alike (LeftBorder, Active,
    // RightBorder). colourIndices holds one TMS9918 palette index (0-15)
    // per dot, count entries long; the sink is responsible for whatever
    // color-to-signal encoding it needs (e.g. the calibrated Y/Pr/Pb table
    // in memory, not this interface's concern).
    virtual void OutputData(int dots, const std::uint8_t* colourIndices,
        std::size_t count) = 0;
};

#endif // ICRT_SIGNAL_SINK_H
