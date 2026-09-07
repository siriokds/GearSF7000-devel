/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026  Saverio Russo

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 *
 */

#ifndef IOPORTS_H
#define	IOPORTS_H

#include "definitions.h"
#include "DebugEvents.h"

class IOPorts
{
public:
    IOPorts() { };
    virtual ~IOPorts() { };
    virtual void Reset() = 0;
    virtual u8 In(uint64_t tstates, u8 port) = 0;
    virtual void Out(uint64_t tstates, u8 port, u8 value) = 0;
    // Returns additional Z80 T-states imposed by the addressed hardware.
    // Keeping this policy on the machine bus lets both the legacy CPU and a
    // cycle-accurate CPU apply the same wait without an I/O device reaching
    // back into a particular processor implementation.
    virtual unsigned int GetWaitStates(u8 port, bool write) const
    {
        (void)port;
        (void)write;
        return 0;
    }
    // Dynamic waits are sampled only once the normal I/O machine cycle has
    // reached its transfer boundary and peripherals have been synchronized to
    // that instant. This is reserved for devices whose real READY output can
    // depend on their current state. The legacy overload remains the default
    // for fixed-latency devices such as the PSG.
    virtual unsigned int GetWaitStatesAt(uint64_t tstates, u8 port,
                                         bool write) const
    {
        (void)tstates;
        return GetWaitStates(port, write);
    }
    // Supplies the byte present on the Z80 data bus during an interrupt
    // acknowledge. IM 1 ignores it, IM 0 decodes it as an opcode and IM 2
    // uses it as the low byte of the vector. $FF is the conventional idle
    // bus value and therefore an IM 0 RST $38 fallback.
    virtual u8 AcknowledgeInterrupt(uint64_t tstates)
    {
        (void)tstates;
        return 0xff;
    }
    // The CPU owns publication of the bus access. Each machine-specific I/O
    // decoder only supplies the addressed device and canonical port.
    virtual DebugProbe GetDebugProbe(u8 port) const = 0;
};

#endif	/* IOPORTS_H */
