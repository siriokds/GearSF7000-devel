/*
 * GearSF7000 - TMS9918/TMS9929 VRAM phase sequencer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TMS9918_VRAM_SEQUENCER_H
#define TMS9918_VRAM_SEQUENCER_H

#include <cstdint>
#include <functional>
#include <iosfwd>

#include "TMS9918SlotGrid.h"
#include "TMS9918VramSlotSchedule.h"

// Minimum delay between the end of a CPU port access and the first internal
// VRAM slot in which that request may be issued. 22 native VDP phases are the
// ceiling of the 2 us nominal delay documented by TI at 10.738635 MHz. Keep
// it configurable so an SC-3000 hardware measurement can replace the nominal
// figure without changing the arbitration model.
#ifndef GEARSF7000_TMS9918_CPU_PORT_MIN_LATENCY_PHASES
#define GEARSF7000_TMS9918_CPU_PORT_MIN_LATENCY_PHASES 22
#endif

#if GEARSF7000_TMS9918_CPU_PORT_MIN_LATENCY_PHASES < 0
#error "GEARSF7000_TMS9918_CPU_PORT_MIN_LATENCY_PHASES must not be negative"
#endif

#ifndef GEARSF7000_TMS9918_VRAM_WRITE_COMMIT_PHASE_OFFSET
#define GEARSF7000_TMS9918_VRAM_WRITE_COMMIT_PHASE_OFFSET 1
#endif

#ifndef GEARSF7000_TMS9918_VRAM_READ_LATCH_PHASE_OFFSET
#define GEARSF7000_TMS9918_VRAM_READ_LATCH_PHASE_OFFSET 3
#endif

#if (GEARSF7000_TMS9918_VRAM_WRITE_COMMIT_PHASE_OFFSET < 1) || \
    (GEARSF7000_TMS9918_VRAM_WRITE_COMMIT_PHASE_OFFSET > 3)
#error "GEARSF7000_TMS9918_VRAM_WRITE_COMMIT_PHASE_OFFSET must be in [1, 3]"
#endif

#if (GEARSF7000_TMS9918_VRAM_READ_LATCH_PHASE_OFFSET < 1) || \
    (GEARSF7000_TMS9918_VRAM_READ_LATCH_PHASE_OFFSET > 3)
#error "GEARSF7000_TMS9918_VRAM_READ_LATCH_PHASE_OFFSET must be in [1, 3]"
#endif

// This class deliberately calls its 0..683 coordinate a VDP *phase* rather
// than a DRAM or oscillator clock. The TMS9918 slot map confirms the 684-phase
// line and the wrap at 683 -> 0. It does not identify a physical DRAM RAS edge
// with the logical origin of a slot, hence TMS9918SlotGrid keeps an explicit
// experimental offset. This class distinguishes both from write commit and
// read latch offsets.
class TMS9918VramSequencer final
{
public:
    static constexpr std::uint16_t PhasesPerLine =
        TMS9918SlotGrid::PhasesPerLine;
    static constexpr std::uint16_t PhasesPerSlot =
        TMS9918SlotGrid::PhasesPerSlot;
    static constexpr std::uint16_t RasPhaseOffset =
        TMS9918SlotGrid::RasPhaseOffset;
    // Datasheet TYP values: write R/W falling ~116 ns, read CAS rising ~270 ns.
    // Rounded to 93 ns internal phases relative to RAS falling.
    static constexpr std::uint16_t WriteCommitPhaseOffset =
        GEARSF7000_TMS9918_VRAM_WRITE_COMMIT_PHASE_OFFSET;
    static constexpr std::uint16_t ReadLatchPhaseOffset =
        GEARSF7000_TMS9918_VRAM_READ_LATCH_PHASE_OFFSET;
    static constexpr std::uint16_t CpuPortMinimumLatencyPhases =
        GEARSF7000_TMS9918_CPU_PORT_MIN_LATENCY_PHASES;

    using Schedule = TMS9918VramSlotSchedule::Schedule;

    enum class RequestKind : std::uint8_t
    {
        CpuRead,
        CpuWrite,
        VideoFetch,
        Refresh,
        Internal,
        // Keep the sprite path separate from generic VideoFetch so its
        // completion can update a per-scanline latch without relying on a
        // global "current fetch" side channel.
        SpriteFetch
    };

    struct Position
    {
        std::uint64_t line = 0;
        std::uint16_t phase = 0;
    };

    struct Request
    {
        RequestKind kind = RequestKind::Internal;
        std::uint16_t address = 0;
        std::uint8_t value = 0;
        bool write = false;
    };

    struct Completion
    {
        Request request;
        Position issuedAt;
        Position completedAt;
    };

    struct Statistics
    {
        std::uint64_t portAttempts = 0;
        std::uint64_t latchReplacements = 0;
        std::uint64_t issuedTransfers = 0;
        std::uint64_t completedTransfers = 0;
    };

    struct LatchReplacement
    {
        bool occurred = false;
        Request discardedRequest{};
        std::uint64_t discardedEligibleAt = 0;
        Request replacementRequest{};
        std::uint64_t replacementEligibleAt = 0;
        Position occurredAt{};
    };

    struct Snapshot
    {
        Position position{};
        Schedule schedule = Schedule::Refresh;
        Schedule videoSchedule = Schedule::Refresh;
        bool hasPendingRequest = false;
        Request pendingRequest{};
        Position pendingIssuedAt{};
        bool hasQueuedRequest = false;
        Request queuedRequest{};
        std::uint64_t queuedEligibleAt = 0;
        Statistics statistics{};
    };

    using CompletionCallback = std::function<void(const Completion&)>;
    using SlotCallback = std::function<void(
        const Position&, const TMS9918VramSlotSchedule::Slot&)>;

    void Reset();

    Position GetPosition() const;
    bool IsLogicalSlotStartPhase() const;
    bool IsRasPhase() const;
    bool HasPendingRequest() const;
    bool HasQueuedRequest() const;
    void SetCpuSchedule(Schedule schedule);
    Schedule GetCpuSchedule() const;
    // The CPU port can be fully available in blanking while the VDP still
    // performs a line-ahead fetch for the next active scanline. Keep the
    // callback calendar independent from CPU ownership.
    void SetVideoSchedule(Schedule schedule);
    Schedule GetVideoSchedule() const;
    Statistics GetStatistics() const;
    Snapshot GetSnapshot() const;

    // A request is accepted only at the configured RAS phase inside a slot.
    // Writes complete at WriteCommitPhaseOffset; reads latch at
    // ReadLatchPhaseOffset. The absolute RAS alignment is experimental.
    bool IssueRequest(const Request& request);

    // CPU ports can be reached at any phase. Queue one dated request in the
    // single overwriteable port latch. It becomes eligible after the minimum
    // internal delay and is issued only on a CPU-owned RAS in the schedule
    // selected by the VDP. A newer access replaces only this waiting latch;
    // an already issued DRAM transaction remains immutable.
    bool QueueRequest(const Request& request,
                      LatchReplacement* replacement = nullptr);

    void SaveState(std::ostream& stream) const;
    void LoadState(std::istream& stream);

    // Advances the phase clock. The callback is invoked only for a real
    // pending response, never once per phase; this keeps the class suitable
    // for an event-driven VDP implementation.
    void Advance(std::uint64_t phases,
                 const CompletionCallback& onCompletion = {},
                 const SlotCallback& onSlot = {});

private:
    void AdvanceOnePhase(const CompletionCallback& onCompletion);
    void AdvanceWithSlotEvents(std::uint64_t phases,
        const CompletionCallback& onCompletion, const SlotCallback& onSlot);
    void AdvancePosition(std::uint64_t phases);
    std::uint16_t PhasesUntilNextRas() const;
    void CompletePendingRequest(const CompletionCallback& onCompletion);
    bool CanIssueQueuedRequest() const;
    std::uint64_t GetAbsolutePhase() const;
    static constexpr std::uint16_t GetCommitDelay(const Request& request)
    {
        return request.write ? WriteCommitPhaseOffset : ReadLatchPhaseOffset;
    }

private:
    Position m_position{};
    Schedule m_cpuSchedule = Schedule::Refresh;
    Schedule m_videoSchedule = Schedule::Refresh;
    bool m_hasPendingRequest = false;
    Request m_pendingRequest{};
    Position m_pendingIssuedAt{};
    std::uint16_t m_pendingCommitDelay = 0;
    bool m_hasQueuedRequest = false;
    Request m_queuedRequest{};
    std::uint64_t m_queuedEligibleAt = 0;
    Statistics m_statistics{};
};

#endif // TMS9918_VRAM_SEQUENCER_H
