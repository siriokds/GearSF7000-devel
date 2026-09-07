/*
 * GearSF7000 - CPU-independent Z80 state access
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_CPU_STATE_ACCESS_H
#define GEARSF7000_CPU_STATE_ACCESS_H

#include <cstdint>

enum class CpuRegister
{
    AF, BC, DE, HL,
    AF2, BC2, DE2, HL2,
    IX, IY, SP, PC, WZ,
    I, R, IFF1, IFF2, InterruptMode
};

struct CpuStateSnapshot
{
    std::uint16_t af = 0;
    std::uint16_t bc = 0;
    std::uint16_t de = 0;
    std::uint16_t hl = 0;
    std::uint16_t af2 = 0;
    std::uint16_t bc2 = 0;
    std::uint16_t de2 = 0;
    std::uint16_t hl2 = 0;
    std::uint16_t ix = 0;
    std::uint16_t iy = 0;
    std::uint16_t sp = 0;
    std::uint16_t pc = 0;
    std::uint16_t wz = 0;
    std::uint8_t i = 0;
    std::uint8_t r = 0;
    bool iff1 = false;
    bool iff2 = false;
    bool halted = false;
    bool intLine = false;
    bool nmiLine = false;
    int interruptMode = 0;
};

class CpuStateAccess
{
public:
    virtual ~CpuStateAccess() = default;

    virtual CpuStateSnapshot GetCpuStateSnapshot() const = 0;
    virtual bool SetCpuRegister(CpuRegister reg, std::uint16_t value) = 0;
    virtual std::uint64_t GetElapsedTStates() const = 0;
    virtual void SetElapsedTStates(std::uint64_t value) = 0;
};

#endif
