/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026  Saverio Russo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#ifndef IVIDEO_CRT_SIGNAL_PROVIDER_H
#define IVIDEO_CRT_SIGNAL_PROVIDER_H

class ICrtSignalSink;

// Optional capability for video devices that can expose their signal before
// the ordinary RGB frame. Devices without a signal-level model do not need to
// know about CRT capture at all.
class IVideoCrtSignalProvider
{
public:
    virtual ~IVideoCrtSignalProvider() = default;
    virtual bool AttachCrtSignalSink(ICrtSignalSink* sink) = 0;
};

#endif // IVIDEO_CRT_SIGNAL_PROVIDER_H
