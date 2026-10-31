// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#include "VideoDeviceFactory.h"
#include "VideoCatalog.h"

#include "IVideoDevice.h"
#include "TMS9918B.h"
#include "TMS99xx.h"
#if GEARSF7000_ENABLE_PICO9918
#include "Pico9918.h"
#endif

std::unique_ptr<IVideoDevice> CreateVideoDevice(
    SystemVideoDevice device, Memory* memory,
    CpuInterruptLines* interruptLines)
{
    switch (device)
    {
    case SystemVideoDevice::TMS9918:
        return std::make_unique<TMS9918>(memory, interruptLines);
    case SystemVideoDevice::TMS9918B:
        return std::make_unique<TMS9918B>(memory, interruptLines);
    case SystemVideoDevice::TMS9928:
        return std::make_unique<TMS9928>(memory, interruptLines);
    case SystemVideoDevice::TMS9929:
        return std::make_unique<TMS9929>(memory, interruptLines);
#if GEARSF7000_ENABLE_PICO9918
    case SystemVideoDevice::Pico9918:
        return std::make_unique<Pico9918>(memory, interruptLines);
#if GEARSF7000_USE_PICOSMSVDP_CORE
    case SystemVideoDevice::PicoSMSVdp:
        return std::make_unique<Pico9918>(memory, interruptLines,
            SystemVideoDevice::PicoSMSVdp);
    case SystemVideoDevice::SegaSMSVdpNTSC:
    case SystemVideoDevice::SegaGameGearVdp:
        return std::make_unique<Pico9918>(memory, interruptLines,
            device);
#endif
#endif
    default:
        return nullptr;
    }
}

std::unique_ptr<IVideoDevice> CreateVideoOption(
    const VideoMachineOption& option, Memory* memory, CpuInterruptLines* interruptLines)
{
    // Only resolved, registered combinations can reach the production factory.
    const auto* registered = GetVideoCatalog().FindOption(option.code);
    if (!registered || !registered->available || registered->device != option.device ||
        registered->variant != option.variant || registered->core != option.core)
        return nullptr;
    return CreateVideoDevice(registered->legacyDevice, memory, interruptLines);
}
