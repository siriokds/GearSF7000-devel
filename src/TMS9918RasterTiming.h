/*
 * GearSF7000 - TMS9918/TMS9929 raster coordinate helpers
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TMS9918_RASTER_TIMING_H
#define TMS9918_RASTER_TIMING_H

#include <cstdint>

#include "TMS9918SlotGrid.h"

// Coordinates use dot 0 at the start of horizontal sync. A dot lasts two VDP
// master phases. This is output-raster timing, deliberately independent from
// the VRAM RAS/CAS alignment inside a two-dot access slot.
class TMS9918RasterTiming final
{
public:
    static constexpr std::uint16_t DotsPerLine = 342;
    static constexpr std::uint16_t PhasesPerDot = 2;
    static constexpr std::uint16_t PhasesPerLine =
        TMS9918SlotGrid::PhasesPerLine;

    enum class DisplayWidth : std::uint16_t
    {
        Graphics = 256,
        Text = 240
    };

    enum class SignalOutput : std::uint8_t
    {
        // TMS9918A: composite NTSC encoder, borders 13 / 15 dot.
        Composite9918,
        // TMS9928A/TMS9929A: component output, borders 15 / 13 dot.
        Component9928_9929
    };

    enum class HorizontalRegion : std::uint8_t
    {
        HSync,
        LeftBlankA,
        ColorBurst,
        LeftBlankB,
        LeftBorder,
        Active,
        RightBorder,
        RightBlank
    };

    enum class VerticalRegion : std::uint8_t
    {
        Active,
        BottomBorder,
        BottomBlanking,
        VerticalSync,
        TopBlanking,
        TopBorder
    };

    // Line counts per region, indexed by line relative to line 0 = the
    // first active display line (matching this codebase's own
    // m_iRenderLine convention, not the datasheet's own presentation
    // order which starts from the top border instead - see
    // GetVerticalDisplayRow for the remap between the two).
    //
    // Every value here is read straight off the TMS9918/28/29 pixel-clock
    // tables (VERTICAL NTSC and VERTICAL PAL); none of it is inferred.
    //   NTSC: 192 + 24 + 3 + 3 + 13 + 27 = 262, visible area 243.
    //   PAL:  192 + 50 + 3 + 3 + 13 + 52 = 313, visible area 294.
    // Both totals and both visible areas match the tables exactly, and the
    // blanking/sync segments (3/3/13) are identical across the two
    // standards - PAL's extra 51 lines all go into the borders, split
    // near-symmetrically just like NTSC's 27/24.
    struct VerticalLineCounts
    {
        std::uint16_t active;
        std::uint16_t bottomBorder;
        std::uint16_t bottomBlanking;
        std::uint16_t verticalSync;
        std::uint16_t topBlanking;
        std::uint16_t topBorder;
    };

    static constexpr VerticalLineCounts GetVerticalLineCounts(bool isPAL)
    {
        if (isPAL)
            return {192, 50, 3, 3, 13, 52};
        return {192, 24, 3, 3, 13, 27};
    }

    static constexpr std::uint16_t GetLinesPerFrame(bool isPAL)
    {
        const VerticalLineCounts counts = GetVerticalLineCounts(isPAL);
        return static_cast<std::uint16_t>(counts.active + counts.bottomBorder +
            counts.bottomBlanking + counts.verticalSync + counts.topBlanking +
            counts.topBorder);
    }

    static constexpr VerticalRegion GetVerticalRegion(
        std::uint16_t line, bool isPAL)
    {
        const VerticalLineCounts c = GetVerticalLineCounts(isPAL);
        const std::uint16_t normalized = line % GetLinesPerFrame(isPAL);
        std::uint16_t boundary = c.active;
        if (normalized < boundary) return VerticalRegion::Active;
        boundary += c.bottomBorder;
        if (normalized < boundary) return VerticalRegion::BottomBorder;
        boundary += c.bottomBlanking;
        if (normalized < boundary) return VerticalRegion::BottomBlanking;
        boundary += c.verticalSync;
        if (normalized < boundary) return VerticalRegion::VerticalSync;
        boundary += c.topBlanking;
        if (normalized < boundary) return VerticalRegion::TopBlanking;
        return VerticalRegion::TopBorder;
    }

    // Remaps a line (0 = first active line, this codebase's convention)
    // to a display row where row 0 is the first row of the top border -
    // so a debug image written in display-row order shows top border at
    // the top, active picture below it, and the rest of the vertical
    // blanking cluster at the very bottom, matching how a real raster
    // actually looks stacked top-to-bottom instead of wrapping the top
    // border to the end of the buffer.
    static constexpr std::uint16_t GetVerticalDisplayRow(
        std::uint16_t line, bool isPAL)
    {
        const VerticalLineCounts c = GetVerticalLineCounts(isPAL);
        return static_cast<std::uint16_t>(
            (line + c.topBorder) % GetLinesPerFrame(isPAL));
    }

    struct DotPosition
    {
        std::uint16_t dot = 0;
        std::uint8_t phaseInDot = 0;
    };

    static constexpr DotPosition GetDotPosition(std::uint16_t phase)
    {
        const std::uint16_t normalized = phase % PhasesPerLine;
        return {static_cast<std::uint16_t>(normalized / PhasesPerDot),
                static_cast<std::uint8_t>(normalized % PhasesPerDot)};
    }

    static constexpr std::uint16_t GetDot(std::uint16_t phase)
    {
        return GetDotPosition(phase).dot;
    }

    // Distance from the current phase to the next pixel/dot transition.
    // At a transition itself it returns two: that dot has just begun.
    static constexpr std::uint8_t PhasesUntilNextDot(std::uint16_t phase)
    {
        return static_cast<std::uint8_t>(PhasesPerDot -
            (phase % PhasesPerDot));
    }

    static constexpr std::uint16_t GetActiveStart(
        DisplayWidth width, SignalOutput output)
    {
        // Text positioning is not specified by the TMS9918 datasheet; 69 is
        // retained from the documented 240-dot timing diagram. Graphics is
        // specified: 13/15 composite borders versus 15/13 component borders.
        if (width == DisplayWidth::Text)
            return 69;
        return output == SignalOutput::Component9928_9929 ? 65 : 63;
    }

    static constexpr std::uint16_t GetActiveWidth(DisplayWidth width)
    {
        return static_cast<std::uint16_t>(width);
    }

    static constexpr std::uint16_t GetActiveEnd(
        DisplayWidth width, SignalOutput output)
    {
        return GetActiveStart(width, output) + GetActiveWidth(width) - 1;
    }

    static constexpr bool IsActiveDot(std::uint16_t dot, DisplayWidth width,
                                      SignalOutput output)
    {
        return dot >= GetActiveStart(width, output) &&
            dot <= GetActiveEnd(width, output);
    }

    static constexpr HorizontalRegion GetHorizontalRegion(
        std::uint16_t dot, DisplayWidth width, SignalOutput output)
    {
        // The horizontal breakdown is one and the same for both signal
        // types: the pixel-clock table gives a single HORIZONTAL (NTSC/PAL)
        // column set - sync 26, left blanking A 2, colour burst 14, left
        // blanking B 8, right blanking 8 - and the *only* thing that
        // differs between composite and component is the left/right border
        // split (13/15 against 15/13), which GetActiveStart already
        // handles. In particular the colour burst is present for both: the
        // datasheet's Table 2-3 lists distinct COLOR BURST R-Y/B-Y values
        // for the 28A and the 29A, and CLK emits it unconditionally too.
        // Component output used to collapse dots 26-49 into a single
        // burst-less back porch here, which left a signal-level consumer
        // with no phase reference on PAL.
        const std::uint16_t normalized = dot % DotsPerLine;
        if (normalized < 26)
            return HorizontalRegion::HSync;
        if (normalized < 28)
            return HorizontalRegion::LeftBlankA;
        if (normalized < 42)
            return HorizontalRegion::ColorBurst;
        if (normalized < 50)
            return HorizontalRegion::LeftBlankB;
        if (normalized < GetActiveStart(width, output))
            return HorizontalRegion::LeftBorder;
        if (normalized <= GetActiveEnd(width, output))
            return HorizontalRegion::Active;
        if (normalized < 334)
            return HorizontalRegion::RightBorder;
        return HorizontalRegion::RightBlank;
    }
};

static_assert(TMS9918RasterTiming::PhasesPerLine == 684,
              "TMS9918 raster geometry must remain 342 dots / 684 phases");

#endif // TMS9918_RASTER_TIMING_H
