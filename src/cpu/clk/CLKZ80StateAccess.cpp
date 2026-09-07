/*
 * GearSF7000 - CLK Z80 internal state serialization
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "cpu/clk/CLKZ80StateAccess.h"

#include "SaveStateStream.h"

namespace CPU::Z80 {

namespace {

constexpr std::uint32_t kMagic = 0x5A383053; // 'Z80S'

// FNV-1a. Only ever compared against itself, so the choice is about speed and
// avalanche, not about any cross-implementation agreement.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

inline std::uint64_t HashByte(std::uint64_t hash, std::uint8_t value)
{
    return (hash ^ value) * kFnvPrime;
}

inline std::uint64_t HashU64(std::uint64_t hash, std::uint64_t value)
{
    for (int i = 0; i < 8; ++i)
        hash = HashByte(hash, static_cast<std::uint8_t>(value >> (8 * i)));
    return hash;
}

}

void GearSF7000StateAccess::CollectPages(
    const ProcessorStorage& storage,
    const ProcessorStorage::InstructionPage** pages)
{
    pages[0] = &storage.base_page_;
    pages[1] = &storage.ed_page_;
    pages[2] = &storage.fd_page_;
    pages[3] = &storage.dd_page_;
    pages[4] = &storage.cb_page_;
    pages[5] = &storage.fdcb_page_;
    pages[6] = &storage.ddcb_page_;
}

void GearSF7000StateAccess::CollectPrograms(
    const ProcessorStorage& storage,
    const ProcessorStorage::MicroOp** bases,
    std::size_t* counts)
{
    const ProcessorStorage::InstructionPage* pages[kPageCount];
    CollectPages(storage, pages);

    const std::vector<ProcessorStorage::MicroOp>* standalone[6] = {
        &storage.reset_program_,
        &storage.nmi_program_,
        &storage.irq_program_[0],
        &storage.irq_program_[1],
        &storage.irq_program_[2],
        &storage.conditional_call_untaken_program_,
    };

    for (int i = 0; i < 6; ++i)
    {
        bases[i] = standalone[i]->data();
        counts[i] = standalone[i]->size();
    }

    for (int page = 0; page < kPageCount; ++page)
    {
        bases[6 + page] = pages[page]->fetch_decode_execute.data();
        counts[6 + page] = pages[page]->fetch_decode_execute.size();

        bases[6 + kPageCount + page] = pages[page]->all_operations.data();
        counts[6 + kPageCount + page] = pages[page]->all_operations.size();
    }
}

std::uint64_t GearSF7000StateAccess::LayoutFingerprint(
    const ProcessorStorage& storage)
{
    // install_default_instruction_set() builds identical tables for every
    // instance in a process, so this is computed once and reused. It walks
    // every micro-op, which is far too slow to repeat for each snapshot of a
    // per-frame recording.
    static const std::uint64_t cached = [&storage]() {
        const ProcessorStorage::MicroOp* bases[kProgramCount];
        std::size_t counts[kProgramCount];
        CollectPrograms(storage, bases, counts);

        std::uint64_t hash = kFnvOffset;
        hash = HashU64(hash, kVersion);
        hash = HashU64(hash, kProgramCount);

        for (int i = 0; i < kProgramCount; ++i)
        {
            hash = HashU64(hash, counts[i]);
            for (std::size_t op = 0; op < counts[i]; ++op)
            {
                hash = HashByte(hash,
                                static_cast<std::uint8_t>(bases[i][op].type));
                hash = HashU64(
                    hash,
                    static_cast<std::uint64_t>(
                        bases[i][op].machine_cycle.operation));
                hash = HashU64(
                    hash,
                    static_cast<std::uint64_t>(
                        bases[i][op].machine_cycle.length.as<std::int64_t>()));
            }
        }

        // The per-opcode dispatch table decides which micro-op an opcode jumps
        // to, so its shape belongs in the fingerprint as well.
        const ProcessorStorage::InstructionPage* pages[kPageCount];
        CollectPages(storage, pages);
        for (int page = 0; page < kPageCount; ++page)
        {
            hash = HashU64(hash, pages[page]->instructions.size());
            hash = HashByte(hash, pages[page]->is_indexed ? 1 : 0);
        }

        return hash;
    }();

    return cached;
}

bool GearSF7000StateAccess::Save(const ProcessorStorage& storage,
                                 std::ostream& stream)
{
    StateWriter w(stream);

    w.U32(kMagic);
    w.U16(kVersion);
    w.U64(LayoutFingerprint(storage));

    w.U8(storage.a_);
    w.U16(storage.bc_.full);
    w.U16(storage.de_.full);
    w.U16(storage.hl_.full);
    w.U16(storage.af_dash_.full);
    w.U16(storage.bc_dash_.full);
    w.U16(storage.de_dash_.full);
    w.U16(storage.hl_dash_.full);
    w.U16(storage.ix_.full);
    w.U16(storage.iy_.full);
    w.U16(storage.pc_.full);
    w.U16(storage.sp_.full);
    w.U16(storage.ir_.full);
    w.U16(storage.refresh_addr_.full);

    w.Bool(storage.iff1_);
    w.Bool(storage.iff2_);
    w.Bool(storage.ld_a_ir_interrupt_pv_quirk_);
    w.I32(static_cast<std::int32_t>(storage.interrupt_mode_));
    w.U16(storage.pc_increment_);

    // The flags live decomposed across these seven bytes rather than as a
    // packed F register; saving the decomposition avoids a lossy round trip
    // through get_flags()/set_flags().
    w.U8(storage.sign_result_);
    w.U8(storage.zero_result_);
    w.U8(storage.half_carry_result_);
    w.U8(storage.bit53_result_);
    w.U8(storage.parity_overflow_result_);
    w.U8(storage.subtract_flag_);
    w.U8(storage.carry_result_);
    w.U8(storage.halt_mask_);
    w.U32(static_cast<std::uint32_t>(storage.flag_adjustment_history_));
    w.U16(storage.last_address_bus_);

    // Signed: a bus handler that returns a wait penalty can push this past
    // zero before the next top-up.
    w.I64(storage.number_of_cycles_.as<std::int64_t>());

    w.U8(storage.request_status_);
    w.U8(storage.last_request_status_);
    w.Bool(storage.irq_line_);
    w.Bool(storage.nmi_line_);
    w.Bool(storage.bus_request_line_);
    w.Bool(storage.wait_line_);

    w.U8(storage.operation_);
    w.U16(storage.temp16_.full);
    w.U16(storage.memptr_.full);
    w.U8(storage.temp8_);

    // Scheduler position.
    const ProcessorStorage::MicroOp* bases[kProgramCount];
    std::size_t counts[kProgramCount];
    CollectPrograms(storage, bases, counts);

    std::uint8_t programId = kNoProgram;
    std::uint32_t programIndex = 0;

    if (storage.scheduled_program_counter_ != nullptr)
    {
        const ProcessorStorage::MicroOp* const p =
            storage.scheduled_program_counter_;

        // Strictly-inside first. Only if that fails is a one-past-the-end
        // pointer considered, because one array's end can share an address
        // with the next array's start and the interior match is the correct
        // reading whenever it exists.
        for (int i = 0; i < kProgramCount && programId == kNoProgram; ++i)
        {
            if (counts[i] == 0)
                continue;
            if (p >= bases[i] && p < bases[i] + counts[i])
            {
                programId = static_cast<std::uint8_t>(i);
                programIndex = static_cast<std::uint32_t>(p - bases[i]);
            }
        }

        for (int i = 0; i < kProgramCount && programId == kNoProgram; ++i)
        {
            if (counts[i] == 0)
                continue;
            if (p == bases[i] + counts[i])
            {
                programId = static_cast<std::uint8_t>(i);
                programIndex = static_cast<std::uint32_t>(counts[i]);
            }
        }

        // A pointer that belongs to no known program means the enumeration
        // above has fallen behind the core. Refuse rather than write a state
        // that would silently resume somewhere else.
        if (programId == kNoProgram)
            return false;
    }

    w.U8(programId);
    w.U32(programIndex);

    const ProcessorStorage::InstructionPage* pages[kPageCount];
    CollectPages(storage, pages);

    std::uint8_t pageId = kNoProgram;
    for (int page = 0; page < kPageCount; ++page)
    {
        if (storage.current_instruction_page_ == pages[page])
        {
            pageId = static_cast<std::uint8_t>(page);
            break;
        }
    }
    if (pageId == kNoProgram && storage.current_instruction_page_ != nullptr)
        return false;

    w.U8(pageId);

    return w.Ok();
}

bool GearSF7000StateAccess::Load(ProcessorStorage& storage,
                                 std::istream& stream)
{
    StateReader r(stream);

    if (r.U32() != kMagic)
        return false;
    if (r.U16() != kVersion)
        return false;
    if (r.U64() != LayoutFingerprint(storage))
        return false;

    storage.a_ = r.U8();
    storage.bc_.full = r.U16();
    storage.de_.full = r.U16();
    storage.hl_.full = r.U16();
    storage.af_dash_.full = r.U16();
    storage.bc_dash_.full = r.U16();
    storage.de_dash_.full = r.U16();
    storage.hl_dash_.full = r.U16();
    storage.ix_.full = r.U16();
    storage.iy_.full = r.U16();
    storage.pc_.full = r.U16();
    storage.sp_.full = r.U16();
    storage.ir_.full = r.U16();
    storage.refresh_addr_.full = r.U16();

    storage.iff1_ = r.Bool();
    storage.iff2_ = r.Bool();
    storage.ld_a_ir_interrupt_pv_quirk_ = r.Bool();
    storage.interrupt_mode_ = static_cast<int>(r.I32());
    storage.pc_increment_ = r.U16();

    storage.sign_result_ = r.U8();
    storage.zero_result_ = r.U8();
    storage.half_carry_result_ = r.U8();
    storage.bit53_result_ = r.U8();
    storage.parity_overflow_result_ = r.U8();
    storage.subtract_flag_ = r.U8();
    storage.carry_result_ = r.U8();
    storage.halt_mask_ = r.U8();
    storage.flag_adjustment_history_ =
        static_cast<unsigned int>(r.U32());
    storage.last_address_bus_ = r.U16();

    storage.number_of_cycles_ = HalfCycles(r.I64());

    storage.request_status_ = r.U8();
    storage.last_request_status_ = r.U8();
    storage.irq_line_ = r.Bool();
    storage.nmi_line_ = r.Bool();
    storage.bus_request_line_ = r.Bool();
    storage.wait_line_ = r.Bool();

    storage.operation_ = r.U8();
    storage.temp16_.full = r.U16();
    storage.memptr_.full = r.U16();
    storage.temp8_ = r.U8();

    const std::uint8_t programId = r.U8();
    const std::uint32_t programIndex = r.U32();
    const std::uint8_t pageId = r.U8();

    if (!r.Ok())
        return false;

    if (programId == kNoProgram)
    {
        storage.scheduled_program_counter_ = nullptr;
    }
    else
    {
        if (programId >= kProgramCount)
            return false;

        const ProcessorStorage::MicroOp* bases[kProgramCount];
        std::size_t counts[kProgramCount];
        CollectPrograms(storage, bases, counts);

        // A one-past-the-end index is legitimate, so the bound is inclusive.
        if (programIndex > counts[programId])
            return false;

        storage.scheduled_program_counter_ = bases[programId] + programIndex;
    }

    if (pageId == kNoProgram)
    {
        storage.current_instruction_page_ = nullptr;
    }
    else
    {
        if (pageId >= kPageCount)
            return false;

        ProcessorStorage::InstructionPage* pages[kPageCount] = {
            &storage.base_page_, &storage.ed_page_,  &storage.fd_page_,
            &storage.dd_page_,   &storage.cb_page_,  &storage.fdcb_page_,
            &storage.ddcb_page_};
        storage.current_instruction_page_ = pages[pageId];
    }

    return true;
}

}
