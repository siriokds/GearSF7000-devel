/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026  Saverio Russo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef IVIDEO_DEVICE_H
#define IVIDEO_DEVICE_H

#include <istream>
#include <ostream>
#include "IVideoPortDevice.h"
#include "VideoDeviceDescriptor.h"
#include "VideoDeviceOptions.h"
#include "VideoFrame.h"
#include "IDebugEventProvider.h"

class IVideoDebugProvider;

// Device contract used by the machine core. TMS-specific inspection APIs stay
// on TMS99xx until the debugger panels are migrated to capability-based views.
class IVideoDevice : public IVideoPortDevice, public IDebugEventProvider
{
public:
    virtual ~IVideoDevice() = default;

    virtual void Init() = 0;
    virtual void Reset() = 0;
    virtual bool TickDeviceClocks(unsigned int deviceClocks) = 0;
    virtual int GetRenderLine() const = 0;

    virtual void SaveState(std::ostream& stream) = 0;
    virtual void LoadState(std::istream& stream) = 0;
    virtual u16 GetStateFormatVersion() const = 0;

    virtual const GC_VideoDeviceDescriptor& GetDeviceDescriptor() const = 0;
    virtual IVideoDebugProvider* GetDebugProvider() = 0;
    virtual const IVideoDebugProvider* GetDebugProvider() const = 0;
    virtual u64 GetFrameSerial() const = 0;
    virtual GC_VideoFrameDescriptor GetFrameDescriptor() const = 0;
    virtual GC_VideoFrameView GetFrameView(GC_Color_Format pixelFormat) = 0;

    virtual std::size_t GetOptionCount() const = 0;
    virtual bool DescribeOption(std::size_t index,
        GC_VideoOptionDescriptor& descriptor) const = 0;
    virtual bool SetOption(const char* code, s64 value) = 0;
    virtual std::size_t GetColorProfileCount() const = 0;
    virtual bool DescribeColorProfile(std::size_t index,
        GC_VideoColorProfileDescriptor& descriptor) const = 0;
    virtual bool GetPaletteView(GC_VideoPaletteView& view) const = 0;
    virtual void CopyHostConfigurationFrom(const IVideoDevice& source) = 0;

};

#endif // IVIDEO_DEVICE_H
