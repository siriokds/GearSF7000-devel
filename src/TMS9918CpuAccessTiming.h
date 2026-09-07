/*
 * GearSF7000 - TMS9918/TMS9929 CPU VRAM access timing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TMS9918_CPU_ACCESS_TIMING_H
#define TMS9918_CPU_ACCESS_TIMING_H

#include <cstdint>

#include "TMS9918SlotGrid.h"
#include "TMS9918VramSlotSchedule.h"

// Maps the documented 171 two-dot VRAM slots in a 342-dot TMS9918 scanline.
// It intentionally has no dependency on Video or a CPU: this makes the VDP
// arbitration rules testable without involving a CPU. The TMS9918/9929 has no
// READY/WAIT output: these windows schedule its internal port request and do
// not become Z80 wait states.
class TMS9918CpuAccessTiming final
{
public:
    static constexpr std::uint16_t PhasesPerLine =
        TMS9918SlotGrid::PhasesPerLine;
    static constexpr std::uint16_t SlotsPerLine =
        TMS9918SlotGrid::SlotsPerLine;
    static constexpr std::uint16_t PhasesPerSlot =
        TMS9918SlotGrid::PhasesPerSlot;

    enum class Mode : std::uint8_t
    {
        Graphics,
        Text,
        Multicolor
    };

    using Schedule = TMS9918VramSlotSchedule::Schedule;

    struct State
    {
        std::uint16_t phase = 0;
        bool activeDisplayLine = false;
        bool displayEnabled = false;
        Mode mode = Mode::Graphics;
    };

    struct Window
    {
        Schedule schedule = Schedule::Refresh;
        bool mapped = true;
        bool availableNow = false;
        std::uint16_t currentSlot = 0;
        std::uint16_t nextSlot = 0;
        std::uint16_t phasesUntilNextSlot = 0;
    };

    static Schedule SelectSchedule(const State& state);
    static Window GetCpuWindow(const State& state);

private:
};

#endif // TMS9918_CPU_ACCESS_TIMING_H
