// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef I_FM_PORT_DEVICE_H
#define I_FM_PORT_DEVICE_H

#include "definitions.h"

// Optional FM expansion endpoint.  The chip implementation is deliberately
// independent of the motherboard decoder: an SMS routes it at $F0-$F2 while
// an SG-1000/SC-3000 expansion can install the same endpoint elsewhere.
struct FmAccessTiming
{
    bool violation = false;
    u64 elapsed = 0;
    u32 required = 0;
};

class IFmPortDevice
{
public:
    virtual ~IFmPortDevice() = default;
    virtual void SelectRegister(u8 value) = 0;
    virtual void WriteData(u8 value) = 0;
    virtual void WriteControl(u8 value) = 0;
    virtual u8 ReadControl() const = 0;
    virtual u8 GetSelectedRegister() const = 0;
    // Diagnostic only. Caller supplies absolute chip clocks, not frame time.
    virtual FmAccessTiming ObserveAccess(u64, bool) { return {}; }
};

struct FmPortMap
{
    u8 select = 0xF0;
    u8 data = 0xF1;
    u8 control = 0xF2;

    bool Contains(u8 port) const
    {
        return port == select || port == data || port == control;
    }
};

#endif
