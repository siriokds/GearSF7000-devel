/*
 * GearSF7000 - TMS99xx CPU VRAM access timing
 *
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "TMS99xxCpuAccessTiming.h"

TMS99xxCpuAccessTiming::Schedule TMS99xxCpuAccessTiming::SelectSchedule(
    const State& state)
{
    if (!state.displayEnabled || !state.activeDisplayLine)
        return Schedule::Refresh;

    switch (state.mode)
    {
        case Mode::Text:       return Schedule::Text;
        case Mode::Multicolor: return Schedule::Multicolor;
        case Mode::Graphics:   return Schedule::Graphics;
    }

    return Schedule::Refresh;
}

TMS99xxCpuAccessTiming::Window TMS99xxCpuAccessTiming::GetCpuWindow(
    const State& state)
{
    Window result;
    result.schedule = SelectSchedule(state);
    result.mapped = true;
    result.currentSlot = TMS99xxSlotGrid::SlotAtPhase(state.phase);

    if (!result.mapped)
        return result;

    if (TMS99xxVramSlotSchedule::IsCpuSlot(result.schedule, result.currentSlot))
    {
        result.availableNow = true;
        result.nextSlot = result.currentSlot;
        return result;
    }

    for (std::uint16_t distance = 1; distance < SlotsPerLine; ++distance)
    {
        const std::uint16_t candidate = static_cast<std::uint16_t>(
            (result.currentSlot + distance) % SlotsPerLine);
        if (!TMS99xxVramSlotSchedule::IsCpuSlot(result.schedule, candidate))
            continue;

        result.nextSlot = candidate;
        const std::uint16_t start =
            TMS99xxSlotGrid::SlotStartPhase(candidate);
        result.phasesUntilNextSlot =
            TMS99xxSlotGrid::ForwardDistance(state.phase, start);
        return result;
    }

    // Every confirmed schedule has CPU slots. Keep a safe result for any
    // future malformed schedule instead of underflowing the timing math.
    result.nextSlot = result.currentSlot;
    result.phasesUntilNextSlot = PhasesPerLine;
    return result;
}
