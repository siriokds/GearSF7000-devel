/*
 * TMS9918B reference model - TMS9918B CPU access analysis
 *
 * Copyright 2026 Saverio Russo
 * SPDX-License-Identifier: Apache-2.0
 *
 * Same rules as GearSF7000's TMS99xxVramSequencer, expressed in units:
 * a port write becomes eligible 44 units (22 phases) later, is issued on the
 * RAS of the next CPU-owned cycle, and replaces a request still waiting.
 *
 * Latency convention (matches the TI "CPU to VDP access times" table):
 * a request arriving at unit t waits for the first CPU cycle c >= t; the
 * charged wait is min(c - p, c - t + 8) where p is the previous CPU cycle,
 * i.e. the gap measured from the start of the slot that contains t.
 */

#ifndef TMS9918B_ANALYSIS_H
#define TMS9918B_ANALYSIS_H

#include <cstdint>
#include <vector>

#include "TMS9918BCalendar.h"

namespace TMS9918BModel
{
    enum class Region : std::uint8_t { ActiveArea, HorizontalBlanking, WholeLine };

    struct Latency
    {
        std::uint16_t waitUnits;
        std::uint16_t totalUnits;
        std::uint16_t tStates;
        double microseconds;
    };

    std::vector<std::uint16_t> CpuRasUnits(const Calendar& calendar);
    Latency WorstLatency(const Calendar& calendar, Region region);
    double LossPercent(const std::vector<std::uint16_t>& cpuRasUnits, std::uint32_t periodUnits, std::uint32_t writes = 60000);
}

#endif // TMS9918B_ANALYSIS_H
