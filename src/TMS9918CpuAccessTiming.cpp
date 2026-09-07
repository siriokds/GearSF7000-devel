/*
 * GearSF7000 - TMS9918/TMS9929 CPU VRAM access timing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TMS9918CpuAccessTiming.h"

TMS9918CpuAccessTiming::Schedule TMS9918CpuAccessTiming::SelectSchedule(
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

TMS9918CpuAccessTiming::Window TMS9918CpuAccessTiming::GetCpuWindow(
    const State& state)
{
    Window result;
    result.schedule = SelectSchedule(state);
    result.mapped = true;
    result.currentSlot = TMS9918SlotGrid::SlotAtPhase(state.phase);

    if (!result.mapped)
        return result;

    if (TMS9918VramSlotSchedule::IsCpuSlot(result.schedule, result.currentSlot))
    {
        result.availableNow = true;
        result.nextSlot = result.currentSlot;
        return result;
    }

    for (std::uint16_t distance = 1; distance < SlotsPerLine; ++distance)
    {
        const std::uint16_t candidate = static_cast<std::uint16_t>(
            (result.currentSlot + distance) % SlotsPerLine);
        if (!TMS9918VramSlotSchedule::IsCpuSlot(result.schedule, candidate))
            continue;

        result.nextSlot = candidate;
        const std::uint16_t start =
            TMS9918SlotGrid::SlotStartPhase(candidate);
        result.phasesUntilNextSlot =
            TMS9918SlotGrid::ForwardDistance(state.phase, start);
        return result;
    }

    // Every confirmed schedule has CPU slots. Keep a safe result for any
    // future malformed schedule instead of underflowing the timing math.
    result.nextSlot = result.currentSlot;
    result.phasesUntilNextSlot = PhasesPerLine;
    return result;
}
