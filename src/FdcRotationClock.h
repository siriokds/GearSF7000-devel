/*
 * GearSF7000 - deterministic floppy rotation clock divider
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_FDC_ROTATION_CLOCK_H
#define GEARSF7000_FDC_ROTATION_CLOCK_H

#include <cstdint>
#include <limits>

// Converts controller input clocks into raw on-disk bit positions without
// rounding each scheduler block independently. The ratio is intentionally
// expressed per revolution rather than as integer clocks per bit.
struct FdcRotationClock
{
    std::uint64_t numeratorRemainder = 0;

    void Reset()
    {
        numeratorRemainder = 0;
    }

    std::uint64_t Advance(std::uint64_t inputClocks,
                          std::uint32_t bitsPerRevolution,
                          std::uint64_t clocksPerRevolution)
    {
        if (bitsPerRevolution == 0 || clocksPerRevolution == 0)
            return 0;

        // Remove complete revolutions before multiplying. Besides keeping the
        // fractional state independent of Tick() segmentation, this prevents
        // a large (for example resumed or malformed) clock delta from
        // overflowing merely because it was delivered in one block.
        const std::uint64_t completeRevolutions =
            inputClocks / clocksPerRevolution;
        const std::uint64_t partialClocks =
            inputClocks % clocksPerRevolution;
        const std::uint64_t bitsPerRev = bitsPerRevolution;

        if (completeRevolutions >
            std::numeric_limits<std::uint64_t>::max() / bitsPerRev)
        {
            // This represents centuries of unconsumed emulated time with the
            // real SF-7000 rates. Keep the fractional phase deterministic and
            // report a saturated distance instead of wrapping backwards.
            numeratorRemainder = 0;
            return std::numeric_limits<std::uint64_t>::max();
        }

        if (partialClocks >
            (std::numeric_limits<std::uint64_t>::max() - numeratorRemainder) /
                bitsPerRev)
        {
            numeratorRemainder = 0;
            return std::numeric_limits<std::uint64_t>::max();
        }

        const std::uint64_t numerator = numeratorRemainder +
            partialClocks * bitsPerRev;
        const std::uint64_t bits = numerator / clocksPerRevolution;
        numeratorRemainder = numerator % clocksPerRevolution;
        const std::uint64_t completeBits =
            completeRevolutions * bitsPerRev;
        if (bits > std::numeric_limits<std::uint64_t>::max() - completeBits)
            return std::numeric_limits<std::uint64_t>::max();
        return completeBits + bits;
    }

    std::uint64_t PreviewAdvance(std::uint64_t inputClocks,
                                 std::uint32_t bitsPerRevolution,
                                 std::uint64_t clocksPerRevolution) const
    {
        FdcRotationClock preview = *this;
        return preview.Advance(inputClocks, bitsPerRevolution,
                               clocksPerRevolution);
    }

    std::uint64_t ClocksUntilBits(std::uint64_t rawBits,
                                  std::uint32_t bitsPerRevolution,
                                  std::uint64_t clocksPerRevolution) const
    {
        if (rawBits == 0 || bitsPerRevolution == 0 ||
            clocksPerRevolution == 0)
            return 0;

        // Runtime FDC deadlines never exceed one revolution, so this product
        // is tightly bounded (100000 * 1600000 at 8 MHz). Reject impossible
        // input rather than allowing a wrapped deadline to fire immediately.
        if (rawBits >
            std::numeric_limits<std::uint64_t>::max() /
                clocksPerRevolution)
            return std::numeric_limits<std::uint64_t>::max();

        const std::uint64_t targetNumerator =
            rawBits * clocksPerRevolution;
        if (targetNumerator <= numeratorRemainder)
            return 0;

        const std::uint64_t missing =
            targetNumerator - numeratorRemainder;
        return missing / bitsPerRevolution +
            ((missing % bitsPerRevolution) != 0 ? 1u : 0u);
    }
};

#endif
