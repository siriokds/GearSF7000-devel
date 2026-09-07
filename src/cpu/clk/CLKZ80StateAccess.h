/*
 * GearSF7000 - CLK Z80 internal state serialization
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_CLK_Z80_STATE_ACCESS_H
#define GEARSF7000_CLK_Z80_STATE_ACCESS_H

#include <cstddef>
#include <cstdint>
#include <iosfwd>

#include "Processors/Z80/Z80.hpp"

namespace CPU::Z80 {

// Captures and restores every internal field of a CLK Z80, including the
// micro-op scheduler, so that a snapshot taken part-way through an instruction
// resumes bit-for-bit rather than only at instruction boundaries.
//
// The scheduler is the reason this needs privileged access. CLK tracks its
// position with two raw pointers into vectors that are built at construction:
//
//     const MicroOp *scheduled_program_counter_;
//     InstructionPage *current_instruction_page_;
//
// Neither survives a round trip on its own. Both are converted here into a
// (program id, index) pair against a fixed enumeration of every micro-op array
// the core can schedule from, and rebuilt from that pair on load. The arrays
// are generated deterministically by install_default_instruction_set(), so the
// pair means the same thing in any run of the same binary; LayoutFingerprint()
// exists to reject a state written by a binary whose tables differ.
struct GearSF7000StateAccess
{
    // Bumped whenever the field list below changes.
    static constexpr std::uint16_t kVersion = 1;

    // 6 standalone programs + fetch_decode_execute and all_operations for each
    // of the 7 instruction pages.
    static constexpr int kPageCount = 7;
    static constexpr int kProgramCount = 6 + (2 * kPageCount);
    static constexpr std::uint8_t kNoProgram = 0xFF;

    // Every array assigned to scheduled_program_counter_ anywhere in
    // Z80Implementation.hpp, in a fixed order that defines the saved ids.
    static void CollectPrograms(const ProcessorStorage& storage,
                                const ProcessorStorage::MicroOp** bases,
                                std::size_t* counts);

    static void CollectPages(const ProcessorStorage& storage,
                             const ProcessorStorage::InstructionPage** pages);

    // Cheap structural hash of the generated tables. Two binaries agree on the
    // meaning of a (program id, index) pair only if they agree on this.
    static std::uint64_t LayoutFingerprint(const ProcessorStorage& storage);

    static bool Save(const ProcessorStorage& storage, std::ostream& stream);
    static bool Load(ProcessorStorage& storage, std::istream& stream);
};

}

#endif
