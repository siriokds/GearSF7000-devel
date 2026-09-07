/*
 * GearSF7000 - independent SC-3000 clock domains
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_CLK_CLOCK_DOMAINS_H
#define GEARSF7000_CLK_CLOCK_DOMAINS_H

#include <cstdint>
#include <iosfwd>

#include "../../definitions.h"
#include "cpu/clk/ExactClockRateConverter.h"
#include "cpu/clk/GearSF7000BusHandler.h"

struct GearSF7000ClockRates
{
    std::uint32_t cpuHz;
    std::uint32_t vdpMasterHz;
    std::uint32_t fdcHz;
};

// Frequencies are named after the physical domains, not a synthetic global
// machine clock. On PAL SC-3000 hardware CPU/PSG and TMS9929 are asynchronous.
namespace GearSF7000ClockRate
{
constexpr std::uint32_t SF7000FDC = 8'000'000;
constexpr GearSF7000ClockRates SC3000PAL{
    3'580'000, GC_VDP_MASTER_CLOCK_HZ, SF7000FDC};
constexpr GearSF7000ClockRates SC3000NTSC{
    3'579'545, GC_VDP_MASTER_CLOCK_HZ, SF7000FDC};
}

// Clock bridge for the CLK integration. It receives objective Z80
// half-cycles from the bus and accounts independently for:
//   * whole Z80 T-states, used by CPU/PSG-domain devices;
//   * TMS9918/9929 master clocks (two master clocks per 342-dot raster dot).
//   * NEC765 input clocks from the SF-7000's independent 8 MHz oscillator.
// CPU-domain devices, VDP and NEC765 consume their native counts independently.
class GearSF7000ClockDomains final : public GearSF7000BusClockSink
{
public:
    explicit GearSF7000ClockDomains(
        GearSF7000ClockRates rates = GearSF7000ClockRate::SC3000NTSC);

    bool Configure(GearSF7000ClockRates rates);
    void Reset();
    void AdvanceZ80HalfCycles(std::uint64_t halfCycles) override;

    std::uint64_t ConsumeCpuTStates();
    std::uint64_t ConsumeVDPMasterClocks();
    std::uint64_t ConsumeFDCClocks();

    std::uint64_t GetElapsedZ80HalfCycles() const;
    std::uint64_t GetElapsedCpuTStates() const;
    std::uint64_t GetElapsedVDPMasterClocks() const;
    std::uint64_t GetElapsedFDCClocks() const;
    GearSF7000ClockRates GetRates() const;

    // Includes the fractional remainder of each rational converter. Without
    // it two runs can start from the same integer counts and still diverge.
    void SaveState(std::ostream& stream) const;
    bool LoadState(std::istream& stream);

private:
    GearSF7000ClockRates m_rates;
    ExactClockRateConverter m_cpuTStateConverter;
    ExactClockRateConverter m_vdpMasterConverter;
    ExactClockRateConverter m_fdcClockConverter;
    std::uint64_t m_elapsedZ80HalfCycles = 0;
    std::uint64_t m_elapsedCpuTStates = 0;
    std::uint64_t m_elapsedVDPMasterClocks = 0;
    std::uint64_t m_elapsedFDCClocks = 0;
    std::uint64_t m_pendingCpuTStates = 0;
    std::uint64_t m_pendingVDPMasterClocks = 0;
    std::uint64_t m_pendingFDCClocks = 0;
};

#endif
