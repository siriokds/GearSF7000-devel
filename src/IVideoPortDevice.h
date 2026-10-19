/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026  Saverio Russo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#ifndef IVIDEO_PORT_DEVICE_H
#define IVIDEO_PORT_DEVICE_H

#include <cstdint>
#include "definitions.h"

// The motherboard only knows that its video socket owns $BE/$BF. Timing,
// registers, VRAM and the rendered frame belong to the concrete video device.
class IVideoPortDevice
{
public:
    virtual ~IVideoPortDevice() = default;

    virtual u8 GetDataPort(std::uint64_t tstates) = 0;
    virtual u8 GetStatusFlags() = 0;
    virtual void WriteData(std::uint64_t tstates, u8 data) = 0;
    virtual void WriteControl(std::uint64_t tstates, u8 control) = 0;

    // SMS-only counter ports.  Keeping these optional lets the original
    // TMS99xx devices continue to implement the common data/control socket.
    // The timestamp is the CPU bus time; an SMS-capable VDP converts it to
    // its native 342-clock raster independently of the host VGA output.
    virtual bool SupportsSmsCounters() const { return false; }
    virtual void ConfigureSmsRaster(bool pal) { (void)pal; }
    virtual u8 ReadSmsVCounter(std::uint64_t tstates)
    {
        (void)tstates;
        return 0xFF;
    }
    virtual u8 ReadSmsHCounter(std::uint64_t tstates)
    {
        (void)tstates;
        return 0xFF;
    }
    virtual void LatchSmsHCounter(std::uint64_t tstates) { (void)tstates; }
};

#endif // IVIDEO_PORT_DEVICE_H
