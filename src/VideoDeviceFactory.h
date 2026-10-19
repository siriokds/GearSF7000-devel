/* Production video-device composition boundary. */
#ifndef VIDEO_DEVICE_FACTORY_H
#define VIDEO_DEVICE_FACTORY_H

#include <memory>

#include "VideoDeviceDescriptor.h"

class CpuInterruptLines;
class IVideoDevice;
class Memory;
struct VideoMachineOption;

using VideoDeviceCreateFunction = std::unique_ptr<IVideoDevice> (*)(
    SystemVideoDevice, Memory*, CpuInterruptLines*);

std::unique_ptr<IVideoDevice> CreateVideoDevice(
    SystemVideoDevice device, Memory* memory,
    CpuInterruptLines* interruptLines);

std::unique_ptr<IVideoDevice> CreateVideoOption(
    const VideoMachineOption& option, Memory* memory, CpuInterruptLines* interruptLines);

#endif // VIDEO_DEVICE_FACTORY_H
