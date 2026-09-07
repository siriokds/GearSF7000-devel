/*
 * GearSF7000 - shared TMS9918/TMS9929 VRAM slot geometry
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TMS9918_SLOT_GRID_H
#define TMS9918_SLOT_GRID_H

#include <cstdint>

// The slot diagram fixes the logical 171-cell grid but does not identify the
// electrical RAS edge inside each four-phase cell. Keep that hypothesis as an
// offset from the cell origin instead of encoding it as a second global grid.
#ifndef GEARSF7000_TMS9918_VRAM_RAS_PHASE_OFFSET
#define GEARSF7000_TMS9918_VRAM_RAS_PHASE_OFFSET 0
#endif

#if (GEARSF7000_TMS9918_VRAM_RAS_PHASE_OFFSET < 0) || \
    (GEARSF7000_TMS9918_VRAM_RAS_PHASE_OFFSET > 3)
#error "GEARSF7000_TMS9918_VRAM_RAS_PHASE_OFFSET must be in [0, 3]"
#endif

class TMS9918SlotGrid final
{
public:
    static constexpr std::uint16_t PhasesPerLine = 684;
    static constexpr std::uint16_t SlotsPerLine = 171;
    static constexpr std::uint16_t PhasesPerSlot = 4;
    static constexpr std::uint16_t SlotZeroStartPhase = 682;
    static constexpr std::uint16_t RasPhaseOffset =
        GEARSF7000_TMS9918_VRAM_RAS_PHASE_OFFSET;

    static constexpr std::uint16_t NormalizePhase(std::uint32_t phase)
    {
        return static_cast<std::uint16_t>(phase % PhasesPerLine);
    }

    // Slot 0 spans phases 682, 683, 0 and 1. Slots 1..170 begin at
    // 2, 6, 10 ... 678 respectively.
    static constexpr std::uint16_t SlotAtPhase(std::uint32_t phase)
    {
        return static_cast<std::uint16_t>(
            ((NormalizePhase(phase) + 2u) / PhasesPerSlot) % SlotsPerLine);
    }

    static constexpr std::uint16_t SlotStartPhase(std::uint32_t slot)
    {
        return static_cast<std::uint16_t>(
            ((slot % SlotsPerLine) * PhasesPerSlot + SlotZeroStartPhase) %
            PhasesPerLine);
    }

    static constexpr std::uint16_t PhaseOffsetInSlot(std::uint32_t phase)
    {
        return static_cast<std::uint16_t>(
            (NormalizePhase(phase) + 2u) % PhasesPerSlot);
    }

    static constexpr bool IsSlotStartPhase(std::uint32_t phase)
    {
        return PhaseOffsetInSlot(phase) == 0;
    }

    static constexpr std::uint16_t RasPhase(std::uint32_t slot)
    {
        return NormalizePhase(SlotStartPhase(slot) + RasPhaseOffset);
    }

    static constexpr bool IsRasPhase(std::uint32_t phase)
    {
        return PhaseOffsetInSlot(phase) == RasPhaseOffset;
    }

    static constexpr std::uint16_t ForwardDistance(std::uint32_t fromPhase,
                                                   std::uint32_t toPhase)
    {
        return static_cast<std::uint16_t>(
            (NormalizePhase(toPhase) + PhasesPerLine -
             NormalizePhase(fromPhase)) % PhasesPerLine);
    }
};

static_assert(TMS9918SlotGrid::SlotsPerLine *
                  TMS9918SlotGrid::PhasesPerSlot ==
              TMS9918SlotGrid::PhasesPerLine);
static_assert(TMS9918SlotGrid::SlotAtPhase(682) == 0);
static_assert(TMS9918SlotGrid::SlotAtPhase(683) == 0);
static_assert(TMS9918SlotGrid::SlotAtPhase(0) == 0);
static_assert(TMS9918SlotGrid::SlotAtPhase(1) == 0);
static_assert(TMS9918SlotGrid::SlotStartPhase(1) == 2);
static_assert(TMS9918SlotGrid::SlotStartPhase(170) == 678);

#endif // TMS9918_SLOT_GRID_H
