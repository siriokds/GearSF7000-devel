/*
 * TMS9918B reference model - TMS9918B CPU access analysis
 *
 * Copyright 2026 Saverio Russo
 * SPDX-License-Identifier: Apache-2.0
 */

#include "TMS9918BAnalysis.h"

#include <algorithm>

namespace TMS9918BModel
{
    std::vector<std::uint16_t> CpuRasUnits(const Calendar& calendar)
    {
        std::vector<std::uint16_t> units;
        for (const Cycle& c : calendar.cycles)
            if (c.kind == Kind::Cpu)
                units.push_back(c.Unit());
        std::sort(units.begin(), units.end());
        return units;
    }

    Latency WorstLatency(const Calendar& calendar, Region region)
    {
        const std::vector<std::uint16_t> cpu = CpuRasUnits(calendar);
        std::vector<bool> isCpu(UnitsPerLine, false);
        for (std::uint16_t u : cpu) isCpu[u] = true;
        std::uint16_t worst = 0;
        for (std::uint16_t t = 0; t < UnitsPerLine; ++t)
        {
            const bool active = calendar.activeEnd > calendar.activeFirst && t >= calendar.activeFirst && t < calendar.activeEnd;
            if ((region == Region::ActiveArea && !active) || (region == Region::HorizontalBlanking && active))
                continue;
            std::uint16_t forward = 0;
            while (!isCpu[(t + forward) % UnitsPerLine]) ++forward;
            std::uint16_t back = 1;
            while (!isCpu[(t + UnitsPerLine - back) % UnitsPerLine]) ++back;
            const std::uint16_t gap = static_cast<std::uint16_t>(forward + back);
            worst = std::max(worst, std::min<std::uint16_t>(gap, static_cast<std::uint16_t>(forward + UnitsPerSlot)));
        }
        Latency l{};
        l.waitUnits = worst;
        l.totalUnits = static_cast<std::uint16_t>(PortLatencyUnits + worst);
        l.tStates = static_cast<std::uint16_t>((l.totalUnits + UnitsPerTState - 1u) / UnitsPerTState);
        l.microseconds = Ns(l.totalUnits) / 1000.0;
        return l;
    }

    double LossPercent(const std::vector<std::uint16_t>& cpu, std::uint32_t period, std::uint32_t writes)
    {
        std::vector<bool> isCpu(UnitsPerLine, false);
        for (std::uint16_t u : cpu) isCpu[u] = true;
        bool queued = false;
        std::uint64_t eligible = 0, lost = 0, next = 2000;
        std::uint32_t sent = 0;
        for (std::uint64_t t = 0; sent < writes || queued; ++t)
        {
            if (queued && t >= eligible && isCpu[t % UnitsPerLine]) queued = false;
            if (sent < writes && t == next)
            {
                if (queued) ++lost;
                queued = true;
                eligible = t + PortLatencyUnits;
                ++sent;
                next += period;
            }
        }
        return 100.0 * static_cast<double>(lost) / writes;
    }
}
