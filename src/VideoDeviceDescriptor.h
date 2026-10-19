/*
 * Static identity and capability catalogue for video devices.
 *
 * A descriptor says what hardware a selection represents. It deliberately
 * does not contain mutable frame geometry: the installed device publishes
 * that through GC_VideoFrameDescriptor as it changes video mode.
 */

#ifndef VIDEO_DEVICE_DESCRIPTOR_H
#define VIDEO_DEVICE_DESCRIPTOR_H

#include <cstddef>
#include <cstring>

#include "VideoFrame.h"

enum class GC_VideoDeviceAvailability : u8
{
    Implemented,
    Planned
};

// Stable hardware selection used by machine profiles, frontends and the
// device factory. Configuration files use an explicit conversion rather than
// depending on these numeric values.
enum class SystemVideoDevice : u8
{
    TMS9918,
    TMS9918B,
    TMS9928,
    TMS9929,
    Pico9918,
    PicoSMSVdp,
    SegaSMSVdpNTSC,
    SegaGameGearVdp
};

// Coarse timing family used by machine-side clock consumers. It has no
// geographical meaning; exact rates remain in nominal_refresh_rate.
enum GC_VideoFrequencyFamily : u8
{
    VDP_FREQ_50Hz,
    VDP_FREQ_60Hz,
    VDP_FREQ_VARIABLE
};

// The VDP selection always owns timing. Original chips have one fixed timing;
// replacement devices expose their own configurations. A cartridge never
// supplies this choice.
enum class GC_VideoMachineTimingBinding : u8
{
    FixedByDevice,
    ConfiguredByDevice
};

enum GC_VideoDeviceCapability : u64
{
    GC_VIDEO_CAPABILITY_NONE                 = 0,
    GC_VIDEO_CAPABILITY_TMS99XX_HOST_PORTS   = 1ull << 0,
    GC_VIDEO_CAPABILITY_LEGACY_TMS_MODES     = 1ull << 1,
    GC_VIDEO_CAPABILITY_PROGRAMMABLE_MODES   = 1ull << 2,
    GC_VIDEO_CAPABILITY_VRAM                 = 1ull << 3,
    GC_VIDEO_CAPABILITY_REGISTERS            = 1ull << 4,
    GC_VIDEO_CAPABILITY_SPRITES              = 1ull << 5,
    GC_VIDEO_CAPABILITY_LAYERS               = 1ull << 6,
    GC_VIDEO_CAPABILITY_VRAM_SLOT_TIMING     = 1ull << 7,
    GC_VIDEO_CAPABILITY_ANALOG_SIGNAL        = 1ull << 8,
    GC_VIDEO_CAPABILITY_VGA_OUTPUT           = 1ull << 9,
    GC_VIDEO_CAPABILITY_HDMI_OUTPUT          = 1ull << 10,
    GC_VIDEO_CAPABILITY_GPU_STATE            = 1ull << 11,
    GC_VIDEO_CAPABILITY_PALETTE_RAM          = 1ull << 12,
    GC_VIDEO_CAPABILITY_FULL_RASTER_DEBUG    = 1ull << 13
};

struct GC_VideoDeviceDescriptor
{
    const char* key;
    const char* display_name;
    const char* vendor;
    const char* model;
    const char* family;
    GC_VideoFrequencyFamily frequency_family;
    GC_VideoMachineTimingBinding machine_timing_binding;
    GC_VideoDeviceAvailability availability;
    GC_VideoSignalType native_signal;
    GC_VideoRational nominal_refresh_rate;

    // The native timing raster, when it is fixed and known. A zero dimension
    // means that this varies by output profile or still needs verification.
    int raster_width;
    int raster_height;

    // The common compatibility content mode, not a promise of the maximum
    // resolution. Programmable devices publish their current mode per frame.
    int compatibility_content_width;
    int compatibility_content_height;
    u16 register_count;
    u64 capability_mask;
};

inline constexpr GC_VideoDeviceDescriptor kVideoDeviceDescriptors[] = {
    {
        "TMS9918", "TMS9918 (NTSC)", "Texas Instruments", "TMS9918A",
        "TMS99xx", VDP_FREQ_60Hz,
        GC_VideoMachineTimingBinding::FixedByDevice,
        GC_VideoDeviceAvailability::Implemented, GC_VIDEO_SIGNAL_ANALOG_NTSC,
        {GC_VDP_MASTER_CLOCK_HZ,
         GC_VDP_MASTER_CLOCKS_PER_LINE * GC_LINES_PER_FRAME_NTSC},
        342, GC_LINES_PER_FRAME_NTSC, 256, 192, 8,
        GC_VIDEO_CAPABILITY_TMS99XX_HOST_PORTS |
        GC_VIDEO_CAPABILITY_LEGACY_TMS_MODES |
        GC_VIDEO_CAPABILITY_VRAM |
        GC_VIDEO_CAPABILITY_REGISTERS |
        GC_VIDEO_CAPABILITY_SPRITES |
        GC_VIDEO_CAPABILITY_VRAM_SLOT_TIMING |
        GC_VIDEO_CAPABILITY_ANALOG_SIGNAL |
        GC_VIDEO_CAPABILITY_FULL_RASTER_DEBUG
    },
    {
        "TMS9918B", "TMS9918B (NTSC)", "Saverio Russo", "TMS9918B",
        "TMS99xx", VDP_FREQ_60Hz,
        GC_VideoMachineTimingBinding::FixedByDevice,
        GC_VideoDeviceAvailability::Implemented, GC_VIDEO_SIGNAL_ANALOG_NTSC,
        {GC_VDP_MASTER_CLOCK_HZ,
         GC_VDP_MASTER_CLOCKS_PER_LINE * GC_LINES_PER_FRAME_NTSC},
        342, GC_LINES_PER_FRAME_NTSC, 256, 192, 16,
        GC_VIDEO_CAPABILITY_TMS99XX_HOST_PORTS |
        GC_VIDEO_CAPABILITY_LEGACY_TMS_MODES |
        GC_VIDEO_CAPABILITY_PROGRAMMABLE_MODES |
        GC_VIDEO_CAPABILITY_VRAM |
        GC_VIDEO_CAPABILITY_REGISTERS |
        GC_VIDEO_CAPABILITY_SPRITES |
        GC_VIDEO_CAPABILITY_LAYERS |
        GC_VIDEO_CAPABILITY_VRAM_SLOT_TIMING |
        GC_VIDEO_CAPABILITY_ANALOG_SIGNAL |
        GC_VIDEO_CAPABILITY_PALETTE_RAM |
        GC_VIDEO_CAPABILITY_FULL_RASTER_DEBUG
    },
    {
        "TMS9928", "TMS9928 (NTSC)", "Texas Instruments", "TMS9928A",
        "TMS99xx", VDP_FREQ_60Hz,
        GC_VideoMachineTimingBinding::FixedByDevice,
        GC_VideoDeviceAvailability::Implemented,
        GC_VIDEO_SIGNAL_ANALOG_COMPONENT,
        {GC_VDP_MASTER_CLOCK_HZ,
         GC_VDP_MASTER_CLOCKS_PER_LINE * GC_LINES_PER_FRAME_NTSC},
        342, GC_LINES_PER_FRAME_NTSC, 256, 192, 8,
        GC_VIDEO_CAPABILITY_TMS99XX_HOST_PORTS |
        GC_VIDEO_CAPABILITY_LEGACY_TMS_MODES |
        GC_VIDEO_CAPABILITY_VRAM |
        GC_VIDEO_CAPABILITY_REGISTERS |
        GC_VIDEO_CAPABILITY_SPRITES |
        GC_VIDEO_CAPABILITY_VRAM_SLOT_TIMING |
        GC_VIDEO_CAPABILITY_ANALOG_SIGNAL |
        GC_VIDEO_CAPABILITY_FULL_RASTER_DEBUG
    },
    {
        "TMS9929", "TMS9929 (PAL)", "Texas Instruments", "TMS9929A",
        "TMS99xx", VDP_FREQ_50Hz,
        GC_VideoMachineTimingBinding::FixedByDevice,
        GC_VideoDeviceAvailability::Implemented, GC_VIDEO_SIGNAL_ANALOG_PAL,
        {GC_VDP_MASTER_CLOCK_HZ,
         GC_VDP_MASTER_CLOCKS_PER_LINE * GC_LINES_PER_FRAME_PAL},
        342, GC_LINES_PER_FRAME_PAL, 256, 192, 8,
        GC_VIDEO_CAPABILITY_TMS99XX_HOST_PORTS |
        GC_VIDEO_CAPABILITY_LEGACY_TMS_MODES |
        GC_VIDEO_CAPABILITY_VRAM |
        GC_VIDEO_CAPABILITY_REGISTERS |
        GC_VIDEO_CAPABILITY_SPRITES |
        GC_VIDEO_CAPABILITY_VRAM_SLOT_TIMING |
        GC_VIDEO_CAPABILITY_ANALOG_SIGNAL |
        GC_VIDEO_CAPABILITY_FULL_RASTER_DEBUG
    },
    {
        "F18A", "F18A", "Matthew Hagerty", "F18A v1.9", "TMS99xx", VDP_FREQ_60Hz,
        GC_VideoMachineTimingBinding::ConfiguredByDevice,
        GC_VideoDeviceAvailability::Planned,
        GC_VIDEO_SIGNAL_VGA, {25000000, 794 * 525}, 794, 525, 256, 192, 64,
        GC_VIDEO_CAPABILITY_TMS99XX_HOST_PORTS |
        GC_VIDEO_CAPABILITY_LEGACY_TMS_MODES |
        GC_VIDEO_CAPABILITY_PROGRAMMABLE_MODES |
        GC_VIDEO_CAPABILITY_VRAM |
        GC_VIDEO_CAPABILITY_REGISTERS |
        GC_VIDEO_CAPABILITY_SPRITES |
        GC_VIDEO_CAPABILITY_LAYERS |
        GC_VIDEO_CAPABILITY_VGA_OUTPUT |
        GC_VIDEO_CAPABILITY_GPU_STATE |
        GC_VIDEO_CAPABILITY_PALETTE_RAM
    },
    {
        "PICO9918", "Pico9918", "Troy Schrapel", "Pico9918", "TMS99xx", VDP_FREQ_60Hz,
        GC_VideoMachineTimingBinding::ConfiguredByDevice,
        GEARSF7000_ENABLE_PICO9918
            ? GC_VideoDeviceAvailability::Implemented
            : GC_VideoDeviceAvailability::Planned,
        GC_VIDEO_SIGNAL_VGA, {84000000, 2668 * 525}, 2668, 525, 256, 192, 8,
        GC_VIDEO_CAPABILITY_TMS99XX_HOST_PORTS |
        GC_VIDEO_CAPABILITY_LEGACY_TMS_MODES |
        GC_VIDEO_CAPABILITY_VRAM |
        GC_VIDEO_CAPABILITY_REGISTERS |
        GC_VIDEO_CAPABILITY_SPRITES |
        GC_VIDEO_CAPABILITY_VGA_OUTPUT
    },
    {
        "PICOSMSVDP", "PicoSMSvdp (VGA)", "Siriokds", "PicoSMSvdp",
        "TMS99xx/Sega VDP", VDP_FREQ_60Hz,
        GC_VideoMachineTimingBinding::ConfiguredByDevice,
        GEARSF7000_ENABLE_PICO9918 && GEARSF7000_USE_PICOSMSVDP_CORE
            ? GC_VideoDeviceAvailability::Implemented
            : GC_VideoDeviceAvailability::Planned,
        GC_VIDEO_SIGNAL_VGA, {84000000, 2668 * 525}, 2668, 525,
        256, 192, 64,
        GC_VIDEO_CAPABILITY_TMS99XX_HOST_PORTS |
        GC_VIDEO_CAPABILITY_LEGACY_TMS_MODES |
        GC_VIDEO_CAPABILITY_PROGRAMMABLE_MODES |
        GC_VIDEO_CAPABILITY_VRAM |
        GC_VIDEO_CAPABILITY_REGISTERS |
        GC_VIDEO_CAPABILITY_SPRITES |
        GC_VIDEO_CAPABILITY_VGA_OUTPUT |
        GC_VIDEO_CAPABILITY_PALETTE_RAM
    },
    {
        "SEGASMSVDPNTSC", "Sega SMS VDP (NTSC)", "Sega", "SMS VDP",
        "Sega VDP", VDP_FREQ_60Hz,
        GC_VideoMachineTimingBinding::FixedByDevice,
        GEARSF7000_ENABLE_PICO9918 && GEARSF7000_USE_PICOSMSVDP_CORE
            ? GC_VideoDeviceAvailability::Implemented
            : GC_VideoDeviceAvailability::Planned,
        GC_VIDEO_SIGNAL_ANALOG_NTSC,
        {GC_VDP_MASTER_CLOCK_HZ,
         GC_VDP_MASTER_CLOCKS_PER_LINE * GC_LINES_PER_FRAME_NTSC},
        GC_VDP_DOTS_PER_LINE, GC_LINES_PER_FRAME_NTSC,
        256, 192, 64,
        GC_VIDEO_CAPABILITY_TMS99XX_HOST_PORTS |
        GC_VIDEO_CAPABILITY_LEGACY_TMS_MODES |
        GC_VIDEO_CAPABILITY_PROGRAMMABLE_MODES |
        GC_VIDEO_CAPABILITY_VRAM |
        GC_VIDEO_CAPABILITY_REGISTERS |
        GC_VIDEO_CAPABILITY_SPRITES |
        GC_VIDEO_CAPABILITY_ANALOG_SIGNAL |
        GC_VIDEO_CAPABILITY_PALETTE_RAM
    },
    {
        "SEGAGGVDP", "Sega Game Gear VDP (LCD)", "Sega", "Game Gear VDP",
        "Sega VDP", VDP_FREQ_60Hz,
        GC_VideoMachineTimingBinding::FixedByDevice,
        GEARSF7000_ENABLE_PICO9918 && GEARSF7000_USE_PICOSMSVDP_CORE
            ? GC_VideoDeviceAvailability::Implemented
            : GC_VideoDeviceAvailability::Planned,
        GC_VIDEO_SIGNAL_DIGITAL_RGB,
        {GC_VDP_MASTER_CLOCK_HZ,
         GC_VDP_MASTER_CLOCKS_PER_LINE * GC_LINES_PER_FRAME_NTSC},
        GC_VDP_DOTS_PER_LINE, GC_LINES_PER_FRAME_NTSC,
        160, 144, 64,
        GC_VIDEO_CAPABILITY_TMS99XX_HOST_PORTS |
        GC_VIDEO_CAPABILITY_LEGACY_TMS_MODES |
        GC_VIDEO_CAPABILITY_PROGRAMMABLE_MODES |
        GC_VIDEO_CAPABILITY_VRAM |
        GC_VIDEO_CAPABILITY_REGISTERS |
        GC_VIDEO_CAPABILITY_SPRITES |
        GC_VIDEO_CAPABILITY_PALETTE_RAM
    }
};

inline constexpr size_t kVideoDeviceDescriptorCount =
    sizeof(kVideoDeviceDescriptors) / sizeof(kVideoDeviceDescriptors[0]);

inline const GC_VideoDeviceDescriptor* FindVideoDeviceDescriptor(const char* key)
{
    if (key == NULL)
        return NULL;
    for (size_t i = 0; i < kVideoDeviceDescriptorCount; ++i)
        if (std::strcmp(kVideoDeviceDescriptors[i].key, key) == 0)
            return &kVideoDeviceDescriptors[i];
    return NULL;
}

inline constexpr const char* GetSystemVideoDeviceKey(SystemVideoDevice device)
{
    switch (device)
    {
    case SystemVideoDevice::TMS9918: return "TMS9918";
    case SystemVideoDevice::TMS9918B: return "TMS9918B";
    case SystemVideoDevice::TMS9928: return "TMS9928";
    case SystemVideoDevice::TMS9929: return "TMS9929";
    case SystemVideoDevice::Pico9918: return "PICO9918";
    case SystemVideoDevice::PicoSMSVdp: return "PICOSMSVDP";
    case SystemVideoDevice::SegaSMSVdpNTSC: return "SEGASMSVDPNTSC";
    case SystemVideoDevice::SegaGameGearVdp: return "SEGAGGVDP";
    }
    return nullptr;
}

inline const GC_VideoDeviceDescriptor* FindVideoDeviceDescriptor(
    SystemVideoDevice device)
{
    return FindVideoDeviceDescriptor(GetSystemVideoDeviceKey(device));
}

inline bool IsSystemVideoDeviceImplemented(SystemVideoDevice device)
{
    const GC_VideoDeviceDescriptor* descriptor =
        FindVideoDeviceDescriptor(device);
    return descriptor != nullptr &&
           descriptor->availability == GC_VideoDeviceAvailability::Implemented;
}

// Persistent frontend values. 1 and 2 preserve the historical NTSC/PAL
// setting while 3 adds the TMS9928 explicitly. Zero remains machine default.
inline constexpr int GetSystemVideoDeviceConfigValue(SystemVideoDevice device)
{
    switch (device)
    {
    case SystemVideoDevice::TMS9918: return 1;
    case SystemVideoDevice::TMS9929: return 2;
    case SystemVideoDevice::TMS9928: return 3;
    case SystemVideoDevice::Pico9918: return 4;
    case SystemVideoDevice::TMS9918B: return 5;
    case SystemVideoDevice::PicoSMSVdp: return 6;
    case SystemVideoDevice::SegaSMSVdpNTSC: return 7;
    case SystemVideoDevice::SegaGameGearVdp: return 8;
    }
    return 0;
}

inline constexpr bool GetSystemVideoDeviceFromConfigValue(
    int value, SystemVideoDevice& device)
{
    switch (value)
    {
    case 1: device = SystemVideoDevice::TMS9918; return true;
    case 2: device = SystemVideoDevice::TMS9929; return true;
    case 3: device = SystemVideoDevice::TMS9928; return true;
    case 4: device = SystemVideoDevice::Pico9918; return true;
    case 5: device = SystemVideoDevice::TMS9918B; return true;
    case 6: device = SystemVideoDevice::PicoSMSVdp; return true;
    case 7: device = SystemVideoDevice::SegaSMSVdpNTSC; return true;
    case 8: device = SystemVideoDevice::SegaGameGearVdp; return true;
    default: return false;
    }
}

#endif // VIDEO_DEVICE_DESCRIPTOR_H
