/*
 * GearSF7000 - exact rational clock-rate conversion
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_EXACT_CLOCK_RATE_CONVERTER_H
#define GEARSF7000_EXACT_CLOCK_RATE_CONVERTER_H

#include <cstdint>
#include <numeric>

// Converts integral source ticks to integral destination ticks without
// floating point and without discarding the fractional remainder. Rates are
// reduced when configured, so advancing in many small chunks produces exactly
// the same result as advancing once by their sum.
class ExactClockRateConverter
{
public:
    ExactClockRateConverter(std::uint32_t sourceRate = 1,
                            std::uint32_t destinationRate = 1)
    {
        SetRates(sourceRate, destinationRate);
    }

    bool SetRates(std::uint32_t sourceRate,
                  std::uint32_t destinationRate)
    {
        if (sourceRate == 0 || destinationRate == 0)
            return false;

        const std::uint32_t divisor = std::gcd(sourceRate, destinationRate);
        m_sourceRate = sourceRate / divisor;
        m_destinationRate = destinationRate / divisor;
        m_remainder = 0;
        return true;
    }

    void Reset()
    {
        m_remainder = 0;
    }

    std::uint64_t Advance(std::uint64_t sourceTicks)
    {
        // Splitting the input avoids multiplying a long-running absolute tick
        // count by the destination rate. The partial product is bounded by two
        // reduced 32-bit rates and therefore fits in uint64_t.
        const std::uint64_t wholePeriods = sourceTicks / m_sourceRate;
        const std::uint64_t partialTicks = sourceTicks % m_sourceRate;
        const std::uint64_t scaledPartial =
            partialTicks * m_destinationRate + m_remainder;

        const std::uint64_t destinationTicks =
            wholePeriods * m_destinationRate +
            scaledPartial / m_sourceRate;
        m_remainder = scaledPartial % m_sourceRate;
        return destinationTicks;
    }

    std::uint32_t GetSourceRate() const { return m_sourceRate; }
    std::uint32_t GetDestinationRate() const { return m_destinationRate; }
    std::uint64_t GetRemainder() const { return m_remainder; }

    // For save-state restore only. The remainder is the fraction of a
    // destination tick already accumulated; dropping it makes a reloaded
    // machine drift against the run it was captured from, so it has to come
    // back exactly as it was rather than being recomputed.
    bool SetRemainder(std::uint64_t remainder)
    {
        if (remainder >= m_sourceRate)
            return false;
        m_remainder = remainder;
        return true;
    }

private:
    std::uint32_t m_sourceRate = 1;
    std::uint32_t m_destinationRate = 1;
    std::uint64_t m_remainder = 0;
};

#endif
