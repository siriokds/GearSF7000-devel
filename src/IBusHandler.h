// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef I_BUS_HANDLER_H
#define I_BUS_HANDLER_H

#include "definitions.h"

enum class BusAddressSpace : u8
{
    Memory,
    IO
};

// A handler may leave the data bus alone. The eventual bus manager can then
// select another driver (or the machine's open-bus value). Writes are delivered
// to every selected listener, so they have no exclusive return value.
struct BusReadResult
{
    bool drivesDataBus = false;
    u8 value = 0xFF;
};

class IBusHandler
{
public:
    virtual ~IBusHandler() = default;
    virtual BusReadResult Read(BusAddressSpace space, u64 tstates,
                               u16 address) = 0;
    virtual void Write(BusAddressSpace space, u64 tstates,
                       u16 address, u8 value) = 0;
};

#endif
