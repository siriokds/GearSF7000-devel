/*
 * GearSF7000 - independent SC-3000 clock domains
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "cpu/clk/GearSF7000ClockDomains.h"

#include "SaveStateStream.h"

GearSF7000ClockDomains::GearSF7000ClockDomains(GearSF7000ClockRates rates)
    : m_rates(rates),
      m_cpuTStateConverter(2, 1),
      m_vdpMasterConverter(rates.cpuHz * 2u, rates.vdpMasterHz),
      m_fdcClockConverter(rates.cpuHz * 2u, rates.fdcHz)
{
}

bool GearSF7000ClockDomains::Configure(GearSF7000ClockRates rates)
{
    if (rates.cpuHz == 0 || rates.cpuHz > UINT32_MAX / 2u ||
        rates.vdpMasterHz == 0 || rates.fdcHz == 0)
    {
        return false;
    }

    m_rates = rates;
    m_cpuTStateConverter.SetRates(2, 1);
    m_vdpMasterConverter.SetRates(rates.cpuHz * 2u, rates.vdpMasterHz);
    m_fdcClockConverter.SetRates(rates.cpuHz * 2u, rates.fdcHz);
    Reset();
    return true;
}

void GearSF7000ClockDomains::Reset()
{
    m_cpuTStateConverter.Reset();
    m_vdpMasterConverter.Reset();
    m_fdcClockConverter.Reset();
    m_elapsedZ80HalfCycles = 0;
    m_elapsedCpuTStates = 0;
    m_elapsedVDPMasterClocks = 0;
    m_elapsedFDCClocks = 0;
    m_pendingCpuTStates = 0;
    m_pendingVDPMasterClocks = 0;
    m_pendingFDCClocks = 0;
}

void GearSF7000ClockDomains::AdvanceZ80HalfCycles(std::uint64_t halfCycles)
{
    const std::uint64_t cpuTStates = m_cpuTStateConverter.Advance(halfCycles);
    const std::uint64_t vdpMasterClocks = m_vdpMasterConverter.Advance(halfCycles);
    const std::uint64_t fdcClocks = m_fdcClockConverter.Advance(halfCycles);

    m_elapsedZ80HalfCycles += halfCycles;
    m_elapsedCpuTStates += cpuTStates;
    m_elapsedVDPMasterClocks += vdpMasterClocks;
    m_elapsedFDCClocks += fdcClocks;
    m_pendingCpuTStates += cpuTStates;
    m_pendingVDPMasterClocks += vdpMasterClocks;
    m_pendingFDCClocks += fdcClocks;
}

std::uint64_t GearSF7000ClockDomains::ConsumeCpuTStates()
{
    const std::uint64_t result = m_pendingCpuTStates;
    m_pendingCpuTStates = 0;
    return result;
}

std::uint64_t GearSF7000ClockDomains::ConsumeVDPMasterClocks()
{
    const std::uint64_t result = m_pendingVDPMasterClocks;
    m_pendingVDPMasterClocks = 0;
    return result;
}

std::uint64_t GearSF7000ClockDomains::ConsumeFDCClocks()
{
    const std::uint64_t result = m_pendingFDCClocks;
    m_pendingFDCClocks = 0;
    return result;
}

std::uint64_t GearSF7000ClockDomains::GetElapsedZ80HalfCycles() const
{
    return m_elapsedZ80HalfCycles;
}

std::uint64_t GearSF7000ClockDomains::GetElapsedCpuTStates() const
{
    return m_elapsedCpuTStates;
}

std::uint64_t GearSF7000ClockDomains::GetElapsedVDPMasterClocks() const
{
    return m_elapsedVDPMasterClocks;
}

std::uint64_t GearSF7000ClockDomains::GetElapsedFDCClocks() const
{
    return m_elapsedFDCClocks;
}

GearSF7000ClockRates GearSF7000ClockDomains::GetRates() const
{
    return m_rates;
}

void GearSF7000ClockDomains::SaveState(std::ostream& stream) const
{
    StateWriter w(stream);

    // The rates are saved so that a state captured in one region cannot be
    // applied to a machine configured for the other without being noticed.
    w.U32(m_rates.cpuHz);
    w.U32(m_rates.vdpMasterHz);
    w.U32(m_rates.fdcHz);

    w.U64(m_elapsedZ80HalfCycles);
    w.U64(m_elapsedCpuTStates);
    w.U64(m_elapsedVDPMasterClocks);
    w.U64(m_elapsedFDCClocks);
    w.U64(m_pendingCpuTStates);
    w.U64(m_pendingVDPMasterClocks);
    w.U64(m_pendingFDCClocks);

    w.U64(m_cpuTStateConverter.GetRemainder());
    w.U64(m_vdpMasterConverter.GetRemainder());
    w.U64(m_fdcClockConverter.GetRemainder());
}

bool GearSF7000ClockDomains::LoadState(std::istream& stream)
{
    StateReader r(stream);

    GearSF7000ClockRates rates{};
    rates.cpuHz = r.U32();
    rates.vdpMasterHz = r.U32();
    rates.fdcHz = r.U32();

    const std::uint64_t elapsedHalfCycles = r.U64();
    const std::uint64_t elapsedCpuTStates = r.U64();
    const std::uint64_t elapsedVDPMasterClocks = r.U64();
    const std::uint64_t elapsedFDCClocks = r.U64();
    const std::uint64_t pendingCpuTStates = r.U64();
    const std::uint64_t pendingVDPMasterClocks = r.U64();
    const std::uint64_t pendingFDCClocks = r.U64();

    const std::uint64_t cpuRemainder = r.U64();
    const std::uint64_t vdpRemainder = r.U64();
    const std::uint64_t fdcRemainder = r.U64();

    if (!r.Ok())
        return false;

    // Configure() reduces the rates and clears the remainders, so it has to
    // run before the remainders are put back.
    if (!Configure(rates))
        return false;

    if (!m_cpuTStateConverter.SetRemainder(cpuRemainder))
        return false;
    if (!m_vdpMasterConverter.SetRemainder(vdpRemainder))
        return false;
    if (!m_fdcClockConverter.SetRemainder(fdcRemainder))
        return false;

    m_elapsedZ80HalfCycles = elapsedHalfCycles;
    m_elapsedCpuTStates = elapsedCpuTStates;
    m_elapsedVDPMasterClocks = elapsedVDPMasterClocks;
    m_elapsedFDCClocks = elapsedFDCClocks;
    m_pendingCpuTStates = pendingCpuTStates;
    m_pendingVDPMasterClocks = pendingVDPMasterClocks;
    m_pendingFDCClocks = pendingFDCClocks;

    return true;
}
