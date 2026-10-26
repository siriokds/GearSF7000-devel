// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#ifndef RAM_REGION_H
#define RAM_REGION_H
#include "definitions.h"
#include <vector>

// Allocation capacity is not the amount of RAM fitted to a machine.
enum class RamRole : u8 { Internal, External, Cartridge };
struct RamRegion
{
    RamRole role;
    const char* key;
    const char* name;
    u8* data;
    u32 size;             // Installed bytes, excluding mirrors.
    u32 displayBase;      // A debugger display convention, not CPU decoding.
    u32 backingOffset;
    bool showInEditor;    // Hidden motherboard RAM is still discoverable.
};
using RamRegions = std::vector<RamRegion>;
#endif
