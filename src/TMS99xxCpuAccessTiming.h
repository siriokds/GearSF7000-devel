/*
 * GearSF7000 - TMS99xx CPU VRAM access timing
 *
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef TMS99XX_CPU_ACCESS_TIMING_H
#define TMS99XX_CPU_ACCESS_TIMING_H

#include <cstdint>

#include "TMS99xxSlotGrid.h"
#include "TMS99xxVramSlotSchedule.h"

// Maps the documented 171 two-dot VRAM slots in a 342-dot TMS99xx scanline.
// It intentionally has no dependency on a concrete VDP or CPU, which keeps
// the family arbitration rules independently testable. These chips have no
// READY/WAIT output: the windows schedule an internal port request and do not
// become Z80 wait states.
class TMS99xxCpuAccessTiming final
{
public:
    static constexpr std::uint16_t PhasesPerLine =
        TMS99xxSlotGrid::PhasesPerLine;
    static constexpr std::uint16_t SlotsPerLine =
        TMS99xxSlotGrid::SlotsPerLine;
    static constexpr std::uint16_t PhasesPerSlot =
        TMS99xxSlotGrid::PhasesPerSlot;

    enum class Mode : std::uint8_t
    {
        Graphics,
        Text,
        Multicolor
    };

    using Schedule = TMS99xxVramSlotSchedule::Schedule;

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

#endif // TMS99XX_CPU_ACCESS_TIMING_H
