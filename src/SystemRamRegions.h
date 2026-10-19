// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SYSTEM_RAM_REGIONS_H
#define SYSTEM_RAM_REGIONS_H
#include "ISystemBus.h"
#include "Memory.h"
#include "Cartridge.h"

// Shared declaration for existing buses. CPU decoding remains in their mappers.
// Return fresh descriptors: configuration/SGM presence can change at runtime.
inline RamRegions DescribeSystemRam(Memory& memory, MachineType machine)
{
    RamRegions result;
    const auto internal = [&](u32 size, u32 base, bool visible = true) {
        result.push_back({RamRole::Internal, "internal_ram", "Internal RAM",
            memory.GetInternalRam(), size, base, 0, visible});
    };
    const auto external = [&](u32 size, u32 base, u32 offset = 0) {
        result.push_back({RamRole::External, "external_ram", "External RAM",
            memory.GetExternalRam() + offset, size, base, offset, true});
    };
    // One source for the board's own RAM: the model table, through Memory.
    const u32 boardSize = memory.GetBoardRamSize();
    if (machine == MachineType::MSX) { internal(boardSize, 0); return result; }
    if (memory.IsSF7000Enabled()) {
        internal(boardSize, 0xC000, false); external(0x10000, 0); return result;
    }
    if (machine == MachineType::Coleco) {
        internal(boardSize, 0x6000);
        if (memory.GetSGMDevice() && memory.GetSGMDevice()->IsPresent()) external(0x8000, 0);
        return result;
    }
    if (memory.UsesSMSWorkRam()) {
        internal(boardSize, 0xC000);
        if (memory.GetSMSCartridgeRam())
            result.push_back({RamRole::Cartridge, "cartridge_ram", "Cartridge RAM",
                memory.GetSMSCartridgeRam(), 0x8000, 0x8000, 0, true});
        return result;
    }
    const auto type = memory.GetCartridge().GetType();
    switch (type) {
    case Cartridge::SC3000_FLAT64K:
        internal(boardSize, 0xC000, false); external(0x10000, 0); break;
    case Cartridge::SC3000_ASC16L:
    case Cartridge::SC3000_ASC16LS:
        internal(boardSize, 0xC000, false);
        external(memory.GetCartridge().GetASC16LRamBytes(), 0xC000, 0x4000); break;
    case Cartridge::SC3000_32K:
    case Cartridge::SC3000_SURVIVORS_MULTICART:
    case Cartridge::SC3000_SURVIVORS_MEGACART:
        internal(boardSize, 0xC000, false); external(0x8000, 0x8000); break;
    case Cartridge::SC3000_16K_AT_8000:
        internal(0x800, 0xC000); external(0x4000, 0x8000); break;
    case Cartridge::SG1000_16K:
        internal(boardSize, 0xC000, false); external(0x4000, 0xC000); break;
    case Cartridge::SG1000_DAHJEE_TYPE_A:
        internal(boardSize, 0xC000); external(0x2000, 0x2000); break;
    case Cartridge::SG1000_SEGA_315_5208_DAHJEE_B:
    case Cartridge::SG1000_DAHJEE_TYPE_B:
        internal(boardSize, 0xC000, false); external(0x2000, 0xC000); break;
    case Cartridge::SG1000_OTHELLO_2K:
        internal(boardSize, 0xC000); external(0x800, 0x8000); break;
    case Cartridge::SC3000_2K: internal(boardSize, 0xC000); break;
    case Cartridge::SG1000_CASTLE_8K:
        internal(boardSize, 0xC000); external(0x2000, 0x8000); break;
    default: internal(boardSize, 0xC000); break;
    }
    return result;
}
#endif
