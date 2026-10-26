/*
 * GearSF7000 - TMS99xx VRAM phase sequencer
 *
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "TMS99xxVramSequencer.h"

#include <istream>
#include <ostream>

namespace
{
void SavePosition(std::ostream& stream, const TMS99xxVramSequencer::Position& value)
{
    stream.write(reinterpret_cast<const char*>(&value.line), sizeof(value.line));
    stream.write(reinterpret_cast<const char*>(&value.phase), sizeof(value.phase));
}

void LoadPosition(std::istream& stream, TMS99xxVramSequencer::Position& value)
{
    stream.read(reinterpret_cast<char*>(&value.line), sizeof(value.line));
    stream.read(reinterpret_cast<char*>(&value.phase), sizeof(value.phase));
}

void SaveRequest(std::ostream& stream, const TMS99xxVramSequencer::Request& value)
{
    const std::uint8_t kind = static_cast<std::uint8_t>(value.kind);
    stream.write(reinterpret_cast<const char*>(&kind), sizeof(kind));
    stream.write(reinterpret_cast<const char*>(&value.address), sizeof(value.address));
    stream.write(reinterpret_cast<const char*>(&value.value), sizeof(value.value));
    stream.write(reinterpret_cast<const char*>(&value.write), sizeof(value.write));
}

void LoadRequest(std::istream& stream, TMS99xxVramSequencer::Request& value)
{
    std::uint8_t kind = 0;
    stream.read(reinterpret_cast<char*>(&kind), sizeof(kind));
    value.kind = static_cast<TMS99xxVramSequencer::RequestKind>(kind);
    stream.read(reinterpret_cast<char*>(&value.address), sizeof(value.address));
    stream.read(reinterpret_cast<char*>(&value.value), sizeof(value.value));
    stream.read(reinterpret_cast<char*>(&value.write), sizeof(value.write));
}
}

void TMS99xxVramSequencer::Reset()
{
    m_position = {};
    m_cpuSchedule = Schedule::Refresh;
    m_videoSchedule = Schedule::Refresh;
    m_cpuPhaseMap = nullptr;
    m_hasCpuPhaseMap = false;
    m_hasPendingRequest = false;
    m_pendingRequest = {};
    m_pendingIssuedAt = {};
    m_pendingCommitDelay = 0;
    m_hasQueuedRequest = false;
    m_queuedRequest = {};
    m_queuedEligibleAt = 0;
    m_statistics = {};
}

TMS99xxVramSequencer::Position TMS99xxVramSequencer::GetPosition() const
{
    return m_position;
}

bool TMS99xxVramSequencer::IsLogicalSlotStartPhase() const
{
    return TMS99xxSlotGrid::IsSlotStartPhase(m_position.phase);
}

bool TMS99xxVramSequencer::IsRasPhase() const
{
    return TMS99xxSlotGrid::IsRasPhase(m_position.phase);
}

bool TMS99xxVramSequencer::HasPendingRequest() const
{
    return m_hasPendingRequest;
}

bool TMS99xxVramSequencer::HasQueuedRequest() const
{
    return m_hasQueuedRequest;
}

void TMS99xxVramSequencer::SetCpuSchedule(Schedule schedule)
{
    m_cpuSchedule = schedule;
}

TMS99xxVramSequencer::Schedule TMS99xxVramSequencer::GetCpuSchedule() const
{
    return m_cpuSchedule;
}

void TMS99xxVramSequencer::SetCpuPhaseMap(
    const std::array<bool, PhasesPerLine>& phases)
{
    m_cpuPhaseMap = &phases;
    m_hasCpuPhaseMap = true;
}

void TMS99xxVramSequencer::ClearCpuPhaseMap()
{
    m_hasCpuPhaseMap = false;
    m_cpuPhaseMap = nullptr;
}

void TMS99xxVramSequencer::SetVideoSchedule(Schedule schedule)
{
    m_videoSchedule = schedule;
}

TMS99xxVramSequencer::Schedule TMS99xxVramSequencer::GetVideoSchedule() const
{
    return m_videoSchedule;
}

TMS99xxVramSequencer::Statistics TMS99xxVramSequencer::GetStatistics() const
{
    return m_statistics;
}

TMS99xxVramSequencer::Snapshot TMS99xxVramSequencer::GetSnapshot() const
{
    return {m_position, m_cpuSchedule, m_videoSchedule,
            m_hasPendingRequest, m_pendingRequest, m_pendingIssuedAt,
            m_hasQueuedRequest, m_queuedRequest, m_queuedEligibleAt,
            m_statistics};
}

bool TMS99xxVramSequencer::IssueRequest(const Request& request)
{
    const bool requestPhase = IsRasPhase() ||
        (m_hasCpuPhaseMap && (*m_cpuPhaseMap)[m_position.phase]);
    if (!requestPhase || m_hasPendingRequest)
        return false;

    m_pendingRequest = request;
    m_pendingIssuedAt = m_position;
    m_pendingCommitDelay = GetCommitDelay(request);
    m_hasPendingRequest = true;
    ++m_statistics.issuedTransfers;
    return true;
}

bool TMS99xxVramSequencer::QueueRequest(const Request& request,
                                        LatchReplacement* replacement)
{
    ++m_statistics.portAttempts;

    if (replacement != nullptr)
        *replacement = LatchReplacement{};

    // The VDP has one port latch, not an unbounded FIFO. A request that has
    // already reached a DRAM slot remains physically in flight, while a newer
    // port operation replaces the single request still waiting for RAS.
    if (m_hasQueuedRequest)
    {
        if (replacement != nullptr)
        {
            replacement->occurred = true;
            replacement->discardedRequest = m_queuedRequest;
            replacement->discardedEligibleAt = m_queuedEligibleAt;
            replacement->replacementRequest = request;
            replacement->replacementEligibleAt =
                GetAbsolutePhase() + CpuPortMinimumLatencyPhases;
            replacement->occurredAt = m_position;
        }
        m_queuedRequest = request;
        m_queuedEligibleAt = GetAbsolutePhase() + CpuPortMinimumLatencyPhases;
        ++m_statistics.latchReplacements;
        return true;
    }

    // Even if no DRAM transaction is in flight, the port latch still has the
    // documented internal delay. It must not bypass that delay merely because
    // the CPU cycle happened to end on a RAS phase.
    m_queuedRequest = request;
    m_hasQueuedRequest = true;
    m_queuedEligibleAt = GetAbsolutePhase() + CpuPortMinimumLatencyPhases;
    return true;
}

void TMS99xxVramSequencer::SaveState(std::ostream& stream) const
{
    SavePosition(stream, m_position);
    const std::uint8_t schedule = static_cast<std::uint8_t>(m_cpuSchedule);
    stream.write(reinterpret_cast<const char*>(&schedule), sizeof(schedule));
    stream.write(reinterpret_cast<const char*>(&m_hasPendingRequest),
                 sizeof(m_hasPendingRequest));
    SaveRequest(stream, m_pendingRequest);
    SavePosition(stream, m_pendingIssuedAt);
    stream.write(reinterpret_cast<const char*>(&m_pendingCommitDelay),
                 sizeof(m_pendingCommitDelay));
    stream.write(reinterpret_cast<const char*>(&m_hasQueuedRequest),
                 sizeof(m_hasQueuedRequest));
    SaveRequest(stream, m_queuedRequest);
    stream.write(reinterpret_cast<const char*>(&m_queuedEligibleAt),
                 sizeof(m_queuedEligibleAt));
    stream.write(reinterpret_cast<const char*>(&m_statistics.portAttempts),
                 sizeof(m_statistics.portAttempts));
    stream.write(reinterpret_cast<const char*>(&m_statistics.latchReplacements),
                 sizeof(m_statistics.latchReplacements));
    stream.write(reinterpret_cast<const char*>(&m_statistics.issuedTransfers),
                 sizeof(m_statistics.issuedTransfers));
    stream.write(reinterpret_cast<const char*>(&m_statistics.completedTransfers),
                 sizeof(m_statistics.completedTransfers));
}

void TMS99xxVramSequencer::LoadState(std::istream& stream)
{
    LoadPosition(stream, m_position);
    std::uint8_t schedule = 0;
    stream.read(reinterpret_cast<char*>(&schedule), sizeof(schedule));
    m_cpuSchedule = static_cast<Schedule>(schedule);
    stream.read(reinterpret_cast<char*>(&m_hasPendingRequest),
                sizeof(m_hasPendingRequest));
    LoadRequest(stream, m_pendingRequest);
    LoadPosition(stream, m_pendingIssuedAt);
    stream.read(reinterpret_cast<char*>(&m_pendingCommitDelay),
                sizeof(m_pendingCommitDelay));
    stream.read(reinterpret_cast<char*>(&m_hasQueuedRequest),
                sizeof(m_hasQueuedRequest));
    LoadRequest(stream, m_queuedRequest);
    stream.read(reinterpret_cast<char*>(&m_queuedEligibleAt),
                sizeof(m_queuedEligibleAt));
    stream.read(reinterpret_cast<char*>(&m_statistics.portAttempts),
                sizeof(m_statistics.portAttempts));
    stream.read(reinterpret_cast<char*>(&m_statistics.latchReplacements),
                sizeof(m_statistics.latchReplacements));
    stream.read(reinterpret_cast<char*>(&m_statistics.issuedTransfers),
                sizeof(m_statistics.issuedTransfers));
    stream.read(reinterpret_cast<char*>(&m_statistics.completedTransfers),
                sizeof(m_statistics.completedTransfers));
}

void TMS99xxVramSequencer::Advance(
    std::uint64_t phases, const CompletionCallback& onCompletion,
    const SlotCallback& onSlot)
{
    if (onSlot)
    {
        AdvanceWithSlotEvents(phases, onCompletion, onSlot);
        return;
    }

    // Queued/pending operations occupy no more than one slot plus its commit
    // delay. Once idle, use exact
    // arithmetic rather than iterating over every VDP phase.
    while (phases != 0 && (m_hasQueuedRequest || m_hasPendingRequest))
    {
        AdvanceOnePhase(onCompletion);
        --phases;
    }

    if (phases == 0)
        return;

    const std::uint64_t total =
        static_cast<std::uint64_t>(m_position.phase) + phases;
    m_position.line += total / PhasesPerLine;
    m_position.phase = static_cast<std::uint16_t>(total % PhasesPerLine);
}

void TMS99xxVramSequencer::AdvanceWithSlotEvents(
    std::uint64_t phases, const CompletionCallback& onCompletion,
    const SlotCallback& onSlot)
{
    // A streaming renderer needs one callback per DRAM slot, not one per
    // 684-phase raster line. Leap to the next RAS or pending completion; the
    // maximum number of callback boundaries is therefore 171 per line.
    while (phases != 0)
    {
        const std::uint16_t toRas = PhasesUntilNextRas();
        const std::uint16_t toCpu = m_hasCpuPhaseMap
            ? PhasesUntilNextCpuPhase() : UINT16_MAX;
        const std::uint16_t toCompletion = m_hasPendingRequest
            ? m_pendingCommitDelay : UINT16_MAX;
        const std::uint64_t step = std::min<std::uint64_t>(
            phases, std::min({toRas, toCpu, toCompletion}));

        AdvancePosition(step);
        phases -= step;

        const bool atRas = step == toRas;
        const bool atCpu = m_hasCpuPhaseMap && step == toCpu;
        const bool completionDue = m_hasPendingRequest &&
            step == toCompletion;

        if (atRas)
        {
            const std::uint16_t slot =
                TMS99xxSlotGrid::SlotAtPhase(m_position.phase);
            onSlot(m_position,
                TMS99xxVramSlotSchedule::GetSlot(m_videoSchedule, slot));

        }

        if ((atRas || atCpu) && CanIssueQueuedRequest())
        {
            const Request request = m_queuedRequest;
            m_hasQueuedRequest = false;
            const bool accepted = IssueRequest(request);
            (void)accepted;
        }

        if (completionDue)
            CompletePendingRequest(onCompletion);
    }
}

void TMS99xxVramSequencer::AdvancePosition(std::uint64_t phases)
{
    if (phases == 0)
        return;

    const std::uint64_t total =
        static_cast<std::uint64_t>(m_position.phase) + phases;
    m_position.line += total / PhasesPerLine;
    m_position.phase = static_cast<std::uint16_t>(total % PhasesPerLine);

    if (m_hasPendingRequest)
        m_pendingCommitDelay = static_cast<std::uint16_t>(
            m_pendingCommitDelay - phases);
}

std::uint16_t TMS99xxVramSequencer::PhasesUntilNextRas() const
{
    // A slot is four phases wide, so this bounded search is clearer and more
    // robust than duplicating the configurable RAS offset arithmetic.
    for (std::uint16_t distance = 1; distance <= PhasesPerSlot; ++distance)
    {
        const std::uint16_t candidate = static_cast<std::uint16_t>(
            (m_position.phase + distance) % PhasesPerLine);
        if (TMS99xxSlotGrid::IsRasPhase(candidate))
            return distance;
    }
    return PhasesPerSlot;
}

std::uint16_t TMS99xxVramSequencer::PhasesUntilNextCpuPhase() const
{
    for (std::uint16_t distance = 1; distance <= PhasesPerLine; ++distance)
    {
        const std::uint16_t candidate = static_cast<std::uint16_t>(
            (m_position.phase + distance) % PhasesPerLine);
        if ((*m_cpuPhaseMap)[candidate])
            return distance;
    }
    return UINT16_MAX;
}

void TMS99xxVramSequencer::CompletePendingRequest(
    const CompletionCallback& onCompletion)
{
    if (!m_hasPendingRequest || m_pendingCommitDelay != 0)
        return;

    Completion completion{m_pendingRequest, m_pendingIssuedAt, m_position};
    m_hasPendingRequest = false;
    ++m_statistics.completedTransfers;
    if (onCompletion)
        onCompletion(completion);
}

void TMS99xxVramSequencer::AdvanceOnePhase(
    const CompletionCallback& onCompletion)
{
    ++m_position.phase;
    if (m_position.phase == PhasesPerLine)
    {
        m_position.phase = 0;
        ++m_position.line;
    }

    if (CanIssueQueuedRequest())
    {
        const Request request = m_queuedRequest;
        m_hasQueuedRequest = false;
        // The current phase is the configured RAS phase and no request can
        // still be pending here.
        const bool accepted = IssueRequest(request);
        (void)accepted;
        return;
    }

    if (!m_hasPendingRequest)
        return;

    if (m_pendingCommitDelay != 0)
        --m_pendingCommitDelay;

    // A write commits near R/W falling; a read latches near CAS rising. Their
    // relative offsets come from the TMS9918 datasheet, while the global RAS
    // phase remains a configurable hypothesis.
    CompletePendingRequest(onCompletion);
}

bool TMS99xxVramSequencer::CanIssueQueuedRequest() const
{
    const bool ownedPhase = m_hasCpuPhaseMap
        ? (*m_cpuPhaseMap)[m_position.phase]
        : (IsRasPhase() && TMS99xxVramSlotSchedule::IsCpuSlot(
              m_cpuSchedule,
              TMS99xxSlotGrid::SlotAtPhase(m_position.phase)));
    if (!m_hasQueuedRequest || m_hasPendingRequest || !ownedPhase ||
        GetAbsolutePhase() < m_queuedEligibleAt)
    {
        return false;
    }

    return true;
}

std::uint64_t TMS99xxVramSequencer::GetAbsolutePhase() const
{
    return m_position.line * static_cast<std::uint64_t>(PhasesPerLine) +
           m_position.phase;
}
