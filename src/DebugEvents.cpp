#include "DebugEvents.h"
#include "TMS99xxDebugEvents.h"
#include "SN76489DebugEvents.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace
{
const u8 kReadWrite = DebugEventAccess_Read | DebugEventAccess_Write;
const u8 kPublished = DebugDescriptorCapability_EventsPublished;

// Catalog-local runtime handles. Stable external identity comes from each
// descriptor key, so these values are not part of the persistence protocol.
namespace GearDevice
{
    const DebugDeviceId Machine = 1;
    const DebugDeviceId SC3000 = 2;
    const DebugDeviceId Z80 = 3;
    const DebugDeviceId SystemBus = 4;
    const DebugDeviceId TMS9918 = 5;
    const DebugDeviceId SN76489 = 6;
    const DebugDeviceId PPI_SC3000 = 7;
    const DebugDeviceId Tape_SR1000 = 8;
    const DebugDeviceId SF7000 = 9;
    const DebugDeviceId PPI2_SF7000 = 10;
    const DebugDeviceId FDC_uPD765 = 11;
    const DebugDeviceId Drive_SF7000 = 12;
    const DebugDeviceId USART_8251 = 13;
    const DebugDeviceId AY_Expansion = 14;
}





const DebugTargetDescriptor kPpiRegisterTargets[] = {
    { 0, "Port A", 8 },
    { 1, "Port B", 8 },
    { 2, "Port C", 8 },
    { 3, "Control", 8 }
};

const DebugTargetDescriptor kPpiScPortTargets[] = {
    { 0xDC, "Port A", 8 },
    { 0xDD, "Port B", 8 },
    { 0xDE, "Port C", 8 },
    { 0xDF, "Control", 8 }
};

const DebugTargetDescriptor kPpiSfPortTargets[] = {
    { 0xE4, "Port A", 8 },
    { 0xE5, "Port B", 8 },
    { 0xE6, "Port C", 8 },
    { 0xE7, "Control", 8 }
};

// AY-3-8910 / YM2149 on the SGM-style expansion. Register names follow the
// chip, not the port: the card exposes a latch/data pair, so what matters when
// reading a trace is which of the sixteen registers was hit.
const DebugTargetDescriptor kAyRegisterTargets[] = {
    { 0,  "A Tone Fine", 8 },     { 1,  "A Tone Coarse", 4 },
    { 2,  "B Tone Fine", 8 },     { 3,  "B Tone Coarse", 4 },
    { 4,  "C Tone Fine", 8 },     { 5,  "C Tone Coarse", 4 },
    { 6,  "Noise Period", 5 },    { 7,  "Mixer", 8 },
    { 8,  "A Volume", 5 },        { 9,  "B Volume", 5 },
    { 10, "C Volume", 5 },        { 11, "Env Period Fine", 8 },
    { 12, "Env Period Coarse", 8 }, { 13, "Env Shape", 4 },
    { 14, "I/O Port A", 8 },      { 15, "I/O Port B", 8 }
};

const DebugTargetDescriptor kFdcStateTargets[] = {
    { 0, "Command", 8 },
    { 1, "Phase", 8 },
    { 2, "IRQ", 1 },
    { 3, "Track", 8 }
};

const DebugTargetDescriptor kTapeStateTargets[] = {
    { 0, "Loaded", 1 },
    { 1, "Playing", 1 },
    { 2, "Rewind", 1 },
    { 3, "Target Speed", 8 },
    { 4, "Motor", 1 }
};

const DebugSpaceDescriptor kZ80Spaces[] = {
    { DebugCommonSpace::Execute, "execute", "Execute", 0x0000, 0xFFFF,
      DebugEventAccess_Execute, DebugEventAccess_None, DebugEventAccess_None,
      0, kPublished, NULL, 0 },
    { DebugCommonSpace::Memory, "memory", "CPU Memory", 0x0000, 0xFFFF,
      kReadWrite, kReadWrite, kReadWrite, 8, kPublished, NULL, 0 }
};

const DebugSpaceDescriptor kSystemBusSpaces[] = {
    { DebugCommonSpace::IoPorts, "io_ports", "I/O Ports", 0x0000, 0x00FF,
      kReadWrite, DebugEventAccess_None, kReadWrite, 8, kPublished, NULL, 0 }
};


// The I/O window is not fixed the way the PSG's $7F is: the expansion's base
// is configurable because the board it models is still being designed. The
// range here covers the offered bases; the register space is what rules will
// normally target anyway.
const DebugSpaceDescriptor kAySpaces[] = {
    { DebugCommonSpace::IoPorts, "io_ports", "I/O Ports", 0x0020, 0x003F,
      kReadWrite, DebugEventAccess_None, kReadWrite,
      8, DebugDescriptorCapability_None, NULL, 0 },
    { DebugCommonSpace::Registers, "registers", "Registers", 0x0000, 0x000F,
      DebugEventAccess_Event, DebugEventAccess_None, DebugEventAccess_Event,
      8, kPublished, kAyRegisterTargets, sizeof(kAyRegisterTargets) / sizeof(kAyRegisterTargets[0]) }
};

const DebugSpaceDescriptor kPpiScSpaces[] = {
    { DebugCommonSpace::IoPorts, "io_ports", "PPI Ports", 0x00DC, 0x00DF,
      kReadWrite, DebugEventAccess_None, kReadWrite, 8, kPublished,
      kPpiScPortTargets, sizeof(kPpiScPortTargets) / sizeof(kPpiScPortTargets[0]) },
    { DebugCommonSpace::Registers, "registers", "Registers", 0x0000, 0x0003,
      DebugEventAccess_Write, DebugEventAccess_Write, DebugEventAccess_Write,
      8, DebugDescriptorCapability_None,
      kPpiRegisterTargets, sizeof(kPpiRegisterTargets) / sizeof(kPpiRegisterTargets[0]) },
    // Legacy PPI state events still use the physical port as target.
    { DebugCommonSpace::State, "state", "Latch State", 0x0000, 0x00FF,
      DebugEventAccess_Event, DebugEventAccess_Event, DebugEventAccess_Event,
      8, kPublished, NULL, 0 }
};

const DebugSpaceDescriptor kTapeSpaces[] = {
    { DebugCommonSpace::State, "state", "Tape State", 0x0000, 0x0004,
      DebugEventAccess_Event, DebugEventAccess_None, DebugEventAccess_Event,
      8, kPublished, kTapeStateTargets, sizeof(kTapeStateTargets) / sizeof(kTapeStateTargets[0]) }
};

const DebugSpaceDescriptor kPpiSfSpaces[] = {
    { DebugCommonSpace::IoPorts, "io_ports", "PPI2 Ports", 0x00E4, 0x00E7,
      kReadWrite, DebugEventAccess_None, kReadWrite, 8, kPublished,
      kPpiSfPortTargets, sizeof(kPpiSfPortTargets) / sizeof(kPpiSfPortTargets[0]) },
    { DebugCommonSpace::Registers, "registers", "Registers", 0x0000, 0x0003,
      DebugEventAccess_Write, DebugEventAccess_Write, DebugEventAccess_Write,
      8, DebugDescriptorCapability_None,
      kPpiRegisterTargets, sizeof(kPpiRegisterTargets) / sizeof(kPpiRegisterTargets[0]) },
    { DebugCommonSpace::State, "state", "Latch State", 0x0000, 0x00FF,
      DebugEventAccess_Event, DebugEventAccess_Event, DebugEventAccess_Event,
      8, kPublished, NULL, 0 }
};

const DebugSpaceDescriptor kFdcSpaces[] = {
    { DebugCommonSpace::IoPorts, "io_ports", "I/O Ports", 0x00E0, 0x00E1,
      kReadWrite, DebugEventAccess_None, kReadWrite, 8,
      DebugDescriptorCapability_None, NULL, 0 },
    { DebugCommonSpace::State, "state", "Controller State", 0x0000, 0x0003,
      DebugEventAccess_Event, DebugEventAccess_Event, DebugEventAccess_Event,
      8, kPublished, kFdcStateTargets, sizeof(kFdcStateTargets) / sizeof(kFdcStateTargets[0]) }
};

const DebugSpaceDescriptor kDriveSpaces[] = {
    { DebugCommonSpace::State, "state", "Mechanical State", 0x0000, 0x00FF,
      DebugEventAccess_Event, DebugEventAccess_Event, DebugEventAccess_Event,
      16, DebugDescriptorCapability_None, NULL, 0 }
};

const DebugSpaceDescriptor kUsartSpaces[] = {
    { DebugCommonSpace::IoPorts, "io_ports", "I/O Ports", 0x00E8, 0x00E9,
      kReadWrite, DebugEventAccess_None, kReadWrite, 8,
      DebugDescriptorCapability_None, NULL, 0 },
    { DebugCommonSpace::Registers, "registers", "Registers", 0x0000, 0x0002,
      kReadWrite, kReadWrite, kReadWrite, 8,
      DebugDescriptorCapability_None, NULL, 0 }
};

const DebugDeviceDescriptor kDebugDevices[] = {
    { GearDevice::Machine, 0, "event", "emulator.machine", "GearSF7000", NULL, 0 },
    { GearDevice::SC3000, GearDevice::Machine, "event.sc3000", "sega.sc3000", "SC-3000", NULL, 0 },
    { GearDevice::Z80, GearDevice::SC3000, "event.sc3000.cpu", "zilog.z80", "Z80", kZ80Spaces, sizeof(kZ80Spaces) / sizeof(kZ80Spaces[0]) },
    { GearDevice::SystemBus, GearDevice::SC3000, "event.sc3000.bus", "debug.system_bus", "System Bus", kSystemBusSpaces, sizeof(kSystemBusSpaces) / sizeof(kSystemBusSpaces[0]) },
    { GearDevice::TMS9918, GearDevice::SC3000, "event.sc3000.vdp", "ti.tms99xx", "TMS9918 VDP", kTms99xxDebugSpaces, kTms99xxDebugSpaceCount },
    { GearDevice::SN76489, GearDevice::SC3000, "event.sc3000.psg", "ti.sn76489", "SN76489 PSG", kSn76489DebugSpaces, kSn76489DebugSpaceCount },
    { GearDevice::PPI_SC3000, GearDevice::SC3000, "event.sc3000.ppi", "intel.8255", "SC-3000 PPI", kPpiScSpaces, sizeof(kPpiScSpaces) / sizeof(kPpiScSpaces[0]) },
    { GearDevice::Tape_SR1000, GearDevice::SC3000, "event.sc3000.tape", "sega.sr1000", "SR-1000 Tape", kTapeSpaces, sizeof(kTapeSpaces) / sizeof(kTapeSpaces[0]), DebugDeviceTrait_External },
    { GearDevice::SF7000, GearDevice::SC3000, "event.sc3000.sf7000", "sega.sf7000", "SF-7000 Control Station", NULL, 0, DebugDeviceTrait_External },
    { GearDevice::PPI2_SF7000, GearDevice::SF7000, "event.sc3000.sf7000.ppi2", "intel.8255", "SF-7000 PPI2", kPpiSfSpaces, sizeof(kPpiSfSpaces) / sizeof(kPpiSfSpaces[0]) },
    { GearDevice::FDC_uPD765, GearDevice::SF7000, "event.sc3000.sf7000.fdc", "nec.upd765", "uPD765 FDC", kFdcSpaces, sizeof(kFdcSpaces) / sizeof(kFdcSpaces[0]) },
    { GearDevice::Drive_SF7000, GearDevice::SF7000, "event.sc3000.sf7000.drive0", "floppy.drive", "SF-7000 Drive", kDriveSpaces, sizeof(kDriveSpaces) / sizeof(kDriveSpaces[0]) },
    { GearDevice::USART_8251, GearDevice::SF7000, "event.sc3000.sf7000.usart", "intel.8251", "8251 USART", kUsartSpaces, sizeof(kUsartSpaces) / sizeof(kUsartSpaces[0]) },
    // Hangs off the SC-3000 rather than the SF-7000: it is a cartridge-port
    // expansion, present or not independently of the disk station.
    { GearDevice::AY_Expansion, GearDevice::SC3000, "event.sc3000.ay", "gi.ay_3_8910", "AY Expansion", kAySpaces, sizeof(kAySpaces) / sizeof(kAySpaces[0]), DebugDeviceTrait_External }
};

u8 GetProbeValueWidth(const DebugDeviceRegistry& registry, const DebugProbe& probe)
{
    const DebugSpaceDescriptor* space = registry.FindSpace(probe.device, probe.space);
    if (space == NULL)
        return 8;
    for (size_t i = 0; i < space->targetCount; ++i)
        if (space->targets[i].id == probe.target)
            return space->targets[i].bitWidth;
    return space->defaultBitWidth;
}

}

DebugDeviceRegistry::DebugDeviceRegistry(const DebugDeviceDescriptor* devices, size_t count)
    : m_devices(devices)
    , m_count(count)
{
}

void DebugDeviceRegistry::SetCatalog(const DebugDeviceDescriptor* devices, size_t count)
{
    m_devices = devices;
    m_count = count;
}

const DebugDeviceDescriptor* DebugDeviceRegistry::GetDevices(size_t* count) const
{
    if (count != NULL)
        *count = m_count;
    return m_devices;
}

const DebugDeviceDescriptor* DebugDeviceRegistry::FindDevice(DebugDeviceId device) const
{
    for (size_t i = 0; i < m_count; ++i)
        if (m_devices[i].id == device)
            return &m_devices[i];
    return NULL;
}

const DebugDeviceDescriptor* DebugDeviceRegistry::FindDevice(const char* stableKey) const
{
    if (stableKey == NULL)
        return NULL;
    for (size_t i = 0; i < m_count; ++i)
        if (m_devices[i].key != NULL && std::strcmp(m_devices[i].key, stableKey) == 0)
            return &m_devices[i];

    // Rules created before the component-provider detach used the emulator
    // product name as their namespace. Resolve those persisted keys against
    // the hardware-oriented event.* tree without retaining duplicate devices.
    constexpr const char* oldRoot = "gearsf7000";
    constexpr size_t oldRootLength = 10;
    if (std::strncmp(stableKey, oldRoot, oldRootLength) == 0 &&
        (stableKey[oldRootLength] == '\0' || stableKey[oldRootLength] == '.'))
    {
        const std::string canonical = std::string("event") +
            (stableKey + oldRootLength);
        for (size_t i = 0; i < m_count; ++i)
            if (m_devices[i].key != NULL &&
                canonical == m_devices[i].key)
                return &m_devices[i];
    }
    return NULL;
}

const DebugDeviceDescriptor* DebugDeviceRegistry::FindDeviceByType(
    const char* typeKey) const
{
    if (typeKey == NULL)
        return NULL;
    for (size_t i = 0; i < m_count; ++i)
        if (m_devices[i].typeKey != NULL &&
            std::strcmp(m_devices[i].typeKey, typeKey) == 0)
            return &m_devices[i];
    return NULL;
}

const DebugDeviceDescriptor* DebugDeviceRegistry::GetParent(DebugDeviceId device) const
{
    const DebugDeviceDescriptor* descriptor = FindDevice(device);
    return descriptor != NULL && descriptor->parentId != 0
        ? FindDevice(descriptor->parentId) : NULL;
}

size_t DebugDeviceRegistry::GetChildCount(DebugDeviceId parent) const
{
    size_t count = 0;
    for (size_t i = 0; i < m_count; ++i)
        if (m_devices[i].parentId == parent)
            ++count;
    return count;
}

const DebugDeviceDescriptor* DebugDeviceRegistry::GetChild(DebugDeviceId parent, size_t index) const
{
    for (size_t i = 0; i < m_count; ++i)
    {
        if (m_devices[i].parentId != parent)
            continue;
        if (index == 0)
            return &m_devices[i];
        --index;
    }
    return NULL;
}

const DebugSpaceDescriptor* DebugDeviceRegistry::FindSpace(DebugDeviceId device, DebugSpaceId space) const
{
    const DebugDeviceDescriptor* descriptor = FindDevice(device);
    if (descriptor == NULL)
        return NULL;
    for (size_t i = 0; i < descriptor->spaceCount; ++i)
        if (descriptor->spaces[i].id == space)
            return &descriptor->spaces[i];
    return NULL;
}

const DebugSpaceDescriptor* DebugDeviceRegistry::FindSpace(DebugDeviceId device, const char* stableKey) const
{
    const DebugDeviceDescriptor* descriptor = FindDevice(device);
    if (descriptor == NULL || stableKey == NULL)
        return NULL;
    for (size_t i = 0; i < descriptor->spaceCount; ++i)
        if (descriptor->spaces[i].key != NULL && std::strcmp(descriptor->spaces[i].key, stableKey) == 0)
            return &descriptor->spaces[i];
    return NULL;
}

const DebugDeviceRegistry& GetGearDebugDeviceRegistry()
{
    static const DebugDeviceRegistry registry(kDebugDevices,
        sizeof(kDebugDevices) / sizeof(kDebugDevices[0]));
    return registry;
}

const DebugDeviceDescriptor* FindDebugDeviceDescriptor(DebugDeviceId device)
{
    return GetGearDebugDeviceRegistry().FindDevice(device);
}

const DebugSpaceDescriptor* FindDebugSpaceDescriptor(DebugDeviceId device, DebugSpaceId space)
{
    return GetGearDebugDeviceRegistry().FindSpace(device, space);
}

const char* GetDebugDeviceName(DebugDeviceId device)
{
    const DebugDeviceDescriptor* descriptor = FindDebugDeviceDescriptor(device);
    return descriptor != NULL ? descriptor->key : "unknown";
}

const char* GetDebugDeviceName(const DebugDeviceRegistry& registry,
                               DebugDeviceId device)
{
    const auto* descriptor = registry.FindDevice(device);
    return descriptor ? descriptor->key : "unknown";
}

const char* GetDebugSpaceName(const DebugDeviceRegistry& registry,
                              DebugDeviceId device, DebugSpaceId space)
{
    const auto* descriptor = registry.FindSpace(device, space);
    return descriptor ? descriptor->key : GetDebugSpaceName(space);
}

const char* GetDebugSpaceName(DebugSpaceId space)
{
    switch (space)
    {
    case DebugCommonSpace::Execute: return "execute";
    case DebugCommonSpace::Memory: return "memory";
    case DebugCommonSpace::IoPorts: return "io_ports";
    case DebugCommonSpace::Registers: return "registers";
    case DebugCommonSpace::State: return "state";
    case DebugCommonSpace::Signals: return "signals";
    case DebugCommonSpace::Raster: return "raster";
    default: return "unknown";
    }
}

bool FormatDebugRasterContext(const DebugEventRasterContext& raster,
                              char* output, size_t outputSize)
{
    if (output == nullptr || outputSize == 0)
        return false;
    output[0] = 0;
    if (!raster.rasterKnown && !raster.gapKnown)
        return false;

    if (raster.rasterKnown && raster.gapKnown)
        std::snprintf(output, outputSize,
            "Y=%u (%s)  X=%u dots (%s)  %s calendar\n"
            "%uT since previous port access",
            raster.line, GetDebugRasterRegionName(raster.verticalRegion),
            raster.dot, GetDebugRasterRegionName(raster.horizontalRegion),
            GetDebugVdpSlotCalendarName(raster.calendar),
            raster.tStatesSincePreviousAccess);
    else if (raster.rasterKnown)
        std::snprintf(output, outputSize,
            "Y=%u (%s)  X=%u dots (%s)  %s calendar",
            raster.line, GetDebugRasterRegionName(raster.verticalRegion),
            raster.dot, GetDebugRasterRegionName(raster.horizontalRegion),
            GetDebugVdpSlotCalendarName(raster.calendar));
    else
        std::snprintf(output, outputSize,
            "%uT since previous port access",
            raster.tStatesSincePreviousAccess);
    return true;
}

bool FormatDebugEventPresentation(const DebugDeviceRegistry& registry,
                                  const DebugEvent& event,
                                  DebugEventPresentation& presentation)
{
    presentation = {};
    const DebugDeviceDescriptor* device = registry.FindDevice(event.device);
    if (device == nullptr || device->formatter == nullptr)
        return false;
    const bool formatted = device->formatter(event, presentation);
    presentation.attributeCount = std::min(
        presentation.attributeCount,
        DebugEventPresentation::MaxAttributes);
    return formatted;
}

const char* GetDebugEventSourceName(DebugEventSource source)
{
    switch (source)
    {
    case DebugEventSource::LegacyHook: return "legacy_hook";
    case DebugEventSource::CpuInstruction: return "cpu_instruction";
    case DebugEventSource::Reset: return "reset";
    case DebugEventSource::AutoIncrement: return "auto_increment";
    case DebugEventSource::DeviceInternal: return "device_internal";
    case DebugEventSource::GuiDebugger: return "gui_debugger";
    case DebugEventSource::Mcp: return "mcp";
    default: return "unknown";
    }
}

const char* GetDebugEventPhaseName(DebugEventPhase phase)
{
    switch (phase)
    {
    case DebugEventPhase::BeforeCommit: return "before_commit";
    case DebugEventPhase::AfterCommitInstructionBoundary: return "after_commit_instruction_boundary";
    case DebugEventPhase::AfterCommitDeviceBoundary: return "after_commit_device_boundary";
    default: return "unknown";
    }
}

const char* GetDebugRasterRegionName(DebugRasterRegion region)
{
    switch (region)
    {
    case DebugRasterRegion::ActiveDisplay: return "ACTIVE";
    case DebugRasterRegion::Border: return "BORDER";
    case DebugRasterRegion::Blanking: return "BLANK";
    case DebugRasterRegion::Sync: return "SYNC";
    default: return "?";
    }
}

const char* GetDebugVdpSlotCalendarName(DebugVdpSlotCalendar calendar)
{
    switch (calendar)
    {
    case DebugVdpSlotCalendar::Refresh: return "Refresh";
    case DebugVdpSlotCalendar::Graphics: return "Graphics";
    case DebugVdpSlotCalendar::Text: return "Text";
    case DebugVdpSlotCalendar::Multicolor: return "Multicolor";
    default: return "?";
    }
}

DebugProbe AdaptLegacyDebugProbe(DebugEventCategory category, u8 access, u16 target)
{
    DebugProbe probe;
    probe.target = target;
    switch (category)
    {
    case DebugEventCategory::CpuExecute:
        probe.device = GearDevice::Z80; probe.space = DebugCommonSpace::Execute; break;
    case DebugEventCategory::CpuMemory:
        probe.device = GearDevice::Z80; probe.space = DebugCommonSpace::Memory; break;
    case DebugEventCategory::Vram:
        probe.device = GearDevice::TMS9918; probe.space = DebugCommonSpace::Memory; break;
    case DebugEventCategory::Io:
        probe.device = GearDevice::SystemBus; probe.space = DebugCommonSpace::IoPorts; break;
    case DebugEventCategory::VdpRegister:
        probe.device = GearDevice::TMS9918; probe.space = DebugCommonSpace::Registers; break;
    case DebugEventCategory::Fdc:
        probe.device = GearDevice::FDC_uPD765; probe.space = DebugCommonSpace::State; break;
    case DebugEventCategory::Ppi:
        probe.device = target >= 0xE4 && target <= 0xE7
            ? GearDevice::PPI2_SF7000 : GearDevice::PPI_SC3000;
        probe.space = (access & DebugEventAccess_Event) != 0
            ? DebugCommonSpace::State : DebugCommonSpace::IoPorts;
        break;
    case DebugEventCategory::VideoTiming:
        probe.device = GearDevice::TMS9918; probe.space = DebugCommonSpace::Raster; break;
    case DebugEventCategory::Tape:
        probe.device = GearDevice::Tape_SR1000; probe.space = DebugCommonSpace::State; break;
    case DebugEventCategory::Audio:
        probe.device = GearDevice::SN76489; probe.space = DebugCommonSpace::Registers; break;
    case DebugEventCategory::AyExpansion:
        probe.device = GearDevice::AY_Expansion; probe.space = DebugCommonSpace::Registers; break;
    case DebugEventCategory::DeviceState:
        break;
    }
    return probe;
}

DebugProbe DecodeGearIoPort(u16 rawPort)
{
    DebugProbe probe;
    probe.device = GearDevice::SystemBus;
    probe.space = DebugCommonSpace::IoPorts;
    probe.target = rawPort & 0x00FF;

    const u8 port = static_cast<u8>(rawPort);
    if (port >= 0x40 && port < 0x80)
    {
        // The SC-3000 decodes only the upper address lines for the PSG.
        probe.device = GearDevice::SN76489;
        probe.target = 0x7F;
    }
    else if ((port & 0xC0) == 0x80)
    {
        // All even/odd aliases select the VDP data/control port.
        probe.device = GearDevice::TMS9918;
        probe.target = (port & 1) == 0 ? 0xBE : 0xBF;
    }
    else if (port >= 0xDC && port <= 0xDF)
    {
        probe.device = GearDevice::PPI_SC3000;
    }
    else if (port == 0xE0 || port == 0xE1)
    {
        probe.device = GearDevice::FDC_uPD765;
    }
    else if (port >= 0xE4 && port <= 0xE7)
    {
        probe.device = GearDevice::PPI2_SF7000;
    }
    else if (port == 0xE8 || port == 0xE9)
    {
        probe.device = GearDevice::USART_8251;
    }
    return probe;
}

DebugProbe GetGearVdpStateProbe(u32 target)
{
    DebugProbe probe;
    probe.device = GearDevice::TMS9918;
    probe.space = DebugCommonSpace::State;
    probe.target = target;
    return probe;
}

DebugEventCategoryCapabilities GetDebugEventCategoryCapabilities(DebugEventCategory category)
{
    const u8 readWrite = DebugEventAccess_Read | DebugEventAccess_Write;
    switch (category)
    {
    case DebugEventCategory::CpuExecute:
        return { 0xFFFF, DebugEventAccess_Execute, DebugEventAccess_None, DebugEventAccess_None };
    case DebugEventCategory::CpuMemory:
        return { 0xFFFF, readWrite, readWrite, readWrite };
    case DebugEventCategory::Vram:
        return { 0x3FFF, readWrite, readWrite, readWrite };
    case DebugEventCategory::Io:
        return { 0x00FF, readWrite, DebugEventAccess_None, readWrite };
    case DebugEventCategory::VdpRegister:
        return { 0x00FF, DebugEventAccess_Write, DebugEventAccess_Write, DebugEventAccess_Write };
    case DebugEventCategory::Fdc:
        return { 0x0003, DebugEventAccess_Event, DebugEventAccess_Event, DebugEventAccess_Event };
    case DebugEventCategory::Ppi:
        // CPU reads/writes publish the transferred byte.  Derived latch
        // transitions publish Event with both the old and new state.
        return { 0x00FF,
            static_cast<u8>(readWrite | DebugEventAccess_Event),
            DebugEventAccess_Event,
            static_cast<u8>(readWrite | DebugEventAccess_Event) };
    case DebugEventCategory::VideoTiming:
        return { 0xFFFF, DebugEventAccess_Event, DebugEventAccess_None, DebugEventAccess_Event };
    case DebugEventCategory::Tape:
        return { 0x0004, DebugEventAccess_Event, DebugEventAccess_None, DebugEventAccess_Event };
    case DebugEventCategory::Audio:
        return { 0x0007, DebugEventAccess_Event, DebugEventAccess_None, DebugEventAccess_Event };
    case DebugEventCategory::AyExpansion:
        return { 0x000F, DebugEventAccess_Event, DebugEventAccess_None, DebugEventAccess_Event };
    case DebugEventCategory::DeviceState:
        return { 0, DebugEventAccess_None, DebugEventAccess_None, DebugEventAccess_None };
    }
    return { 0, DebugEventAccess_None, DebugEventAccess_None, DebugEventAccess_None };
}

bool DebugValueConditionNeedsBefore(DebugValueCondition condition)
{
    switch (condition)
    {
    case DebugValueCondition::BeforeMaskedEqual:
    case DebugValueCondition::BeforeAndAfterMaskedEqual:
    case DebugValueCondition::BeforeNotEqualAfter:
    case DebugValueCondition::AfterLessThanBefore:
    case DebugValueCondition::AfterLessOrEqualBefore:
    case DebugValueCondition::AfterGreaterThanBefore:
    case DebugValueCondition::AfterGreaterOrEqualBefore:
    case DebugValueCondition::MaskedBitsChanged:
    case DebugValueCondition::BeforeMaskedSet:
    case DebugValueCondition::BeforeMaskedClear:
        return true;
    default:
        return false;
    }
}

bool DebugValueConditionNeedsAfter(DebugValueCondition condition)
{
    switch (condition)
    {
    case DebugValueCondition::AfterMaskedEqual:
    case DebugValueCondition::BeforeAndAfterMaskedEqual:
    case DebugValueCondition::BeforeNotEqualAfter:
    case DebugValueCondition::AfterLessThanBefore:
    case DebugValueCondition::AfterLessOrEqualBefore:
    case DebugValueCondition::AfterGreaterThanBefore:
    case DebugValueCondition::AfterGreaterOrEqualBefore:
    case DebugValueCondition::MaskedBitsChanged:
    case DebugValueCondition::AfterMaskedSet:
    case DebugValueCondition::AfterMaskedClear:
        return true;
    default:
        return false;
    }
}

const char* ValidateDebugRule(const DebugRule& rule)
{
    return ValidateDebugRule(rule, GetGearDebugDeviceRegistry());
}

const char* ValidateDebugRule(const DebugRule& rule, const DebugDeviceRegistry& registry)
{
    u32 targetMax = 0;
    u8 supportedAccesses = DebugEventAccess_None;
    u8 beforeValueAccesses = DebugEventAccess_None;
    u8 afterValueAccesses = DebugEventAccess_None;

    if (rule.useProbe)
    {
        const DebugDeviceDescriptor* device = registry.FindDevice(rule.device);
        if (device == NULL)
            return "Unknown debug device";
        const DebugSpaceDescriptor* space = registry.FindSpace(rule.device, rule.space);
        if (space == NULL)
            return "Unknown debug space for selected device";
        if ((space->capabilities & DebugDescriptorCapability_EventsPublished) == 0)
            return "Selected device space is described but does not publish events yet";
        targetMax = space->targetMax;
        supportedAccesses = space->supportedAccesses;
        beforeValueAccesses = space->beforeValueAccesses;
        afterValueAccesses = space->afterValueAccesses;
    }
    else
    {
        const DebugEventCategoryCapabilities capabilities =
            GetDebugEventCategoryCapabilities(rule.category);
        targetMax = capabilities.targetMax;
        supportedAccesses = capabilities.supportedAccesses;
        beforeValueAccesses = capabilities.beforeValueAccesses;
        afterValueAccesses = capabilities.afterValueAccesses;
    }

    if (rule.addressStart > rule.addressEnd)
        return "Target range start is greater than end";
    if (rule.addressEnd > targetMax)
        return "Target range exceeds the selected space";
    if (rule.accessMask == DebugEventAccess_None)
        return "No access type selected";
    if ((rule.accessMask & ~supportedAccesses) != 0)
        return "Access type is not published by the selected target";
    if (DebugValueConditionNeedsBefore(rule.valueCondition)
        && (rule.accessMask & ~beforeValueAccesses) != 0)
        return "Old value is unavailable for one or more selected access types";
    if (DebugValueConditionNeedsAfter(rule.valueCondition)
        && (rule.accessMask & ~afterValueAccesses) != 0)
        return "New value is unavailable for one or more selected access types";
    if ((rule.actions & (DebugEventAction_Log | DebugEventAction_Pause)) == 0)
        return "No action selected";
    if (rule.breakOnHit == 0 && (rule.actions & DebugEventAction_Log) == 0)
        return "Pause is disabled by hit zero and the rule does not log";
    return NULL;
}

DebugEventManager::DebugEventManager(const DebugDeviceRegistry* registry)
    : m_events(DefaultEventCapacity)
    , m_eventHead(0)
    , m_eventCount(0)
    , m_nextRuleId(1)
    , m_nextSequence(1)
    , m_droppedEvents(0)
    , m_eventGeneration(1)
    , m_registry(registry != NULL ? registry : &GetGearDebugDeviceRegistry())
    , m_bankProvider(NULL)
    , m_bankContext(NULL)
{
    m_pendingRuleCommands.reserve(64);
    m_applyingRuleCommands.reserve(64);
}

bool DebugEventManager::SetRegistry(const DebugDeviceRegistry& registry)
{
    if (!m_rules.empty() || m_nextSequence != 1 || m_nextRuleId.load() != 1)
        return false;
    m_registry = &registry;
    return true;
}

void DebugEventManager::RebindRegistry(const DebugDeviceRegistry& registry)
{
    ClearEvents();
    m_registry = &registry;
    for (DebugRule& rule : m_rules)
    {
        if (!rule.useProbe)
            continue;
        rule.device = 0;
        rule.space = DebugCommonSpace::Unknown;
        const auto* device = registry.FindDevice(rule.deviceKey.c_str());
        if (!device)
            continue;
        const auto* space = registry.FindSpace(device->id, rule.spaceKey.c_str());
        if (!space)
            continue;
        rule.device = device->id;
        rule.space = space->id;
        if (ValidateDebugRule(rule, registry) != NULL)
        {
            rule.device = 0;
            rule.space = DebugCommonSpace::Unknown;
        }
        else
        {
            rule.deviceKey = device->key;
            rule.spaceKey = space->key;
        }
    }
}

u32 DebugEventManager::AddRule(const DebugRule& source)
{
    DebugRule rule = source;
    if (!PrepareRule(rule))
        return 0;
    return InsertPreparedRule(std::move(rule), ReserveRuleId());
}

u32 DebugEventManager::ReserveRuleId()
{
    return static_cast<u32>(m_nextRuleId.fetch_add(1));
}

bool DebugEventManager::PrepareRule(DebugRule& rule) const
{
    if (rule.useProbe)
    {
        const DebugDeviceRegistry& registry = *m_registry;
        const DebugDeviceDescriptor* device = rule.device != 0
            ? registry.FindDevice(rule.device) : registry.FindDevice(rule.deviceKey.c_str());
        if (device == NULL)
            return false;
        const DebugSpaceDescriptor* space = rule.space != DebugCommonSpace::Unknown
            ? registry.FindSpace(device->id, rule.space)
            : registry.FindSpace(device->id, rule.spaceKey.c_str());
        if (space == NULL)
            return false;
        rule.device = device->id;
        rule.space = space->id;
        rule.deviceKey = device->key;
        rule.spaceKey = space->key;
    }
    // Rules saved/created by the first Debug Events implementation use only
    // useValueCondition + mask + expected.  Keep their exact meaning.
    if (rule.valueCondition == DebugValueCondition::Any && rule.useValueCondition)
        rule.valueCondition = DebugValueCondition::AfterMaskedEqual;
    rule.useValueCondition = rule.valueCondition != DebugValueCondition::Any;
    if (ValidateDebugRule(rule, *m_registry) != NULL)
        return false;
    return true;
}

u32 DebugEventManager::InsertPreparedRule(DebugRule&& rule, u32 id)
{
    rule.id = id;
    rule.hitCount = 0;
    m_rules.push_back(std::move(rule));
    return id;
}

u32 DebugEventManager::AddRuleWithId(const DebugRule& source, u32 id)
{
    DebugRule rule = source;
    // Runtime handles belong to the catalog current at application time. A
    // queued command may have crossed a stopped-machine recomposition.
    if (rule.useProbe)
    {
        rule.device = 0;
        rule.space = DebugCommonSpace::Unknown;
    }
    if (!PrepareRule(rule))
        return 0;
    return InsertPreparedRule(std::move(rule), id);
}

bool DebugEventManager::RemoveRule(u32 id)
{
    for (std::vector<DebugRule>::iterator it = m_rules.begin(); it != m_rules.end(); ++it)
    {
        if (it->id == id)
        {
            m_rules.erase(it);
            return true;
        }
    }
    return false;
}

void DebugEventManager::ClearRules()
{
    m_rules.clear();
}

bool DebugEventManager::SetRuleEnabled(u32 id, bool enabled)
{
    for (DebugRule& rule : m_rules)
    {
        if (rule.id == id)
        {
            rule.enabled = enabled;
            return true;
        }
    }
    return false;
}

bool DebugEventManager::ResetRuleHits(u32 id)
{
    for (DebugRule& rule : m_rules)
    {
        if (rule.id == id)
        {
            rule.hitCount = 0;
            return true;
        }
    }
    return false;
}

const std::vector<DebugRule>& DebugEventManager::GetRules() const
{
    return m_rules;
}

void DebugEventManager::QueueRuleCommand(RuleCommand&& command)
{
    std::lock_guard<std::mutex> lock(m_ruleCommandMutex);
    m_pendingRuleCommands.push_back(std::move(command));
}

u32 DebugEventManager::QueueAddRule(const DebugRule& rule)
{
    RuleCommand command;
    command.type = RuleCommandType::Add;
    command.rule = rule;
    if (!PrepareRule(command.rule))
        return 0;
    command.id = ReserveRuleId();
    const u32 id = command.id;
    QueueRuleCommand(std::move(command));
    return id;
}

void DebugEventManager::QueueRemoveRule(u32 id)
{
    RuleCommand command;
    command.type = RuleCommandType::Remove;
    command.id = id;
    QueueRuleCommand(std::move(command));
}

void DebugEventManager::QueueClearRules()
{
    RuleCommand command;
    command.type = RuleCommandType::Clear;
    QueueRuleCommand(std::move(command));
}

void DebugEventManager::QueueSetRuleEnabled(u32 id, bool enabled)
{
    RuleCommand command;
    command.type = RuleCommandType::SetEnabled;
    command.id = id;
    command.enabled = enabled;
    QueueRuleCommand(std::move(command));
}

void DebugEventManager::QueueResetRuleHits(u32 id)
{
    RuleCommand command;
    command.type = RuleCommandType::ResetHits;
    command.id = id;
    QueueRuleCommand(std::move(command));
}

size_t DebugEventManager::ApplyPendingRuleCommands()
{
    {
        std::lock_guard<std::mutex> lock(m_ruleCommandMutex);
        m_pendingRuleCommands.swap(m_applyingRuleCommands);
    }

    const size_t count = m_applyingRuleCommands.size();
    for (const RuleCommand& command : m_applyingRuleCommands)
    {
        switch (command.type)
        {
        case RuleCommandType::Add:
            AddRuleWithId(command.rule, command.id);
            break;
        case RuleCommandType::Remove:
            RemoveRule(command.id);
            break;
        case RuleCommandType::Clear:
            ClearRules();
            break;
        case RuleCommandType::SetEnabled:
            SetRuleEnabled(command.id, command.enabled);
            break;
        case RuleCommandType::ResetHits:
            ResetRuleHits(command.id);
            break;
        }
    }
    m_applyingRuleCommands.clear();
    return count;
}

bool DebugEventManager::Evaluate(DebugEventCategory category, u8 access, u16 target,
                                 u8 value, bool valueKnown, u16 pc, u64 clock)
{
    return Evaluate(category, access, target, 0, false, value, valueKnown, pc, clock);
}

bool DebugEventManager::RuleMatchesValue(const DebugRule& rule, u64 beforeValue, bool beforeKnown,
                                         u64 afterValue, bool afterKnown) const
{
    switch (rule.valueCondition)
    {
    case DebugValueCondition::Any:
        return true;
    case DebugValueCondition::AfterMaskedEqual:
        return afterKnown && ((afterValue & rule.valueMask) == rule.valueExpected);
    case DebugValueCondition::BeforeMaskedEqual:
        return beforeKnown && ((beforeValue & rule.valueMask) == rule.beforeValueExpected);
    case DebugValueCondition::BeforeAndAfterMaskedEqual:
        return beforeKnown && afterKnown && ((beforeValue & rule.valueMask) == rule.beforeValueExpected)
            && ((afterValue & rule.valueMask) == rule.valueExpected);
    case DebugValueCondition::BeforeNotEqualAfter:
        return beforeKnown && afterKnown && beforeValue != afterValue;
    case DebugValueCondition::AfterLessThanBefore:
        return beforeKnown && afterKnown && afterValue < beforeValue;
    case DebugValueCondition::AfterLessOrEqualBefore:
        return beforeKnown && afterKnown && afterValue <= beforeValue;
    case DebugValueCondition::AfterGreaterThanBefore:
        return beforeKnown && afterKnown && afterValue > beforeValue;
    case DebugValueCondition::AfterGreaterOrEqualBefore:
        return beforeKnown && afterKnown && afterValue >= beforeValue;
    case DebugValueCondition::MaskedBitsChanged:
        return beforeKnown && afterKnown && ((beforeValue & rule.valueMask) != (afterValue & rule.valueMask));
    case DebugValueCondition::BeforeMaskedSet:
        return beforeKnown && (beforeValue & rule.valueMask) == rule.valueMask;
    case DebugValueCondition::AfterMaskedSet:
        return afterKnown && (afterValue & rule.valueMask) == rule.valueMask;
    case DebugValueCondition::BeforeMaskedClear:
        return beforeKnown && (beforeValue & rule.valueMask) == 0;
    case DebugValueCondition::AfterMaskedClear:
        return afterKnown && (afterValue & rule.valueMask) == 0;
    }
    return false;
}

bool DebugEventManager::Evaluate(DebugEventCategory category, u8 access, u16 target,
                                 u8 beforeValue, bool beforeKnown,
                                 u8 afterValue, bool afterKnown, u16 pc, u64 clock,
                                 const DebugEventRasterContext* raster)
{
    const DebugProbe probe = ResolveLegacyProbe(category, access, target);
    return EvaluateWithProbe(category, access, target, beforeValue, beforeKnown,
        afterValue, afterKnown, pc, clock, &probe, NULL,
        DebugEventSource::Unknown, DebugEventPhase::Unknown, raster);
}

bool DebugEventManager::EvaluateIoPort(const DebugProbe& decodedProbe, u16 rawPort,
                                       u8 access, u8 value, bool valueKnown,
                                       u16 pc, u64 clock)
{
    DebugProbe probe = decodedProbe;
    if (probe.device == 0)
        probe = ResolveLegacyProbe(DebugEventCategory::Io, access, rawPort);
    probe.space = DebugCommonSpace::IoPorts;

    // Legacy generic I/O rules keep matching the raw Z80 port. The v2 event
    // identifies the decoded device and its canonical port.
    bool pause = EvaluateWithProbe(DebugEventCategory::Io, access, rawPort,
        0, false, value, valueKnown, pc, clock, &probe);

    // PPI Ports is a qualified view of that same CPU transaction. There is no
    // second publisher in the 8255 implementation; only matching legacy PPI
    // rules are evaluated here for compatibility with existing presets.
    const DebugDeviceDescriptor* decodedDevice = m_registry->FindDevice(probe.device);
    if (decodedDevice != NULL && decodedDevice->typeKey != NULL &&
        std::strcmp(decodedDevice->typeKey, "intel.8255") == 0)
        pause = EvaluateWithProbe(DebugEventCategory::Ppi, access,
            static_cast<u16>(probe.target), 0, false, value, valueKnown,
            pc, clock, &probe) || pause;
    return pause;
}

bool DebugEventManager::EvaluateMemoryAccess(
    const DebugProbe& primaryProbe, const DebugProbe& compatibilityProbe,
    u8 access, u64 beforeValue, bool beforeKnown,
    u64 afterValue, bool afterKnown, u16 pc, u64 clock)
{
    return EvaluateWithProbe(DebugEventCategory::CpuMemory, access,
        static_cast<u16>(primaryProbe.target), beforeValue, beforeKnown,
        afterValue, afterKnown, pc, clock, &primaryProbe, &compatibilityProbe,
        DebugEventSource::CpuInstruction,
        DebugEventPhase::AfterCommitDeviceBoundary);
}

bool DebugEventManager::EvaluateProbe(const DebugProbe& probe, u8 access,
                                      u64 beforeValue, bool beforeKnown,
                                      u64 afterValue, bool afterKnown,
                                      DebugEventSource source, DebugEventPhase phase,
                                      u16 pc, u64 clock,
                                      const DebugEventRasterContext* raster)
{
    return EvaluateWithProbe(DebugEventCategory::DeviceState, access,
        static_cast<u16>(probe.target), beforeValue, beforeKnown,
        afterValue, afterKnown, pc, clock, &probe, NULL, source, phase, raster);
}

bool DebugEventManager::EvaluateDeviceProbe(
    DebugEventCategory compatibilityCategory, const DebugProbe& probe,
    u8 access, u64 beforeValue, bool beforeKnown,
    u64 afterValue, bool afterKnown,
    DebugEventSource source, DebugEventPhase phase,
    u16 pc, u64 clock, const DebugEventRasterContext* raster)
{
    return EvaluateWithProbe(compatibilityCategory, access,
        static_cast<u16>(probe.target), beforeValue, beforeKnown,
        afterValue, afterKnown, pc, clock, &probe, NULL, source, phase, raster);
}

bool DebugEventManager::EvaluateWithProbe(DebugEventCategory category, u8 access, u16 target,
                                          u64 beforeValue, bool beforeKnown,
                                          u64 afterValue, bool afterKnown,
                                          u16 pc, u64 clock,
                                          const DebugProbe* probeOverride,
                                          const DebugProbe* compatibilityProbe,
                                          DebugEventSource source,
                                          DebugEventPhase phase,
                                          const DebugEventRasterContext* raster)
{
    if (!m_runtimeActive)
        return false;
    bool pause = false;

    for (std::vector<DebugRule>::iterator it = m_rules.begin(); it != m_rules.end(); ++it)
    {
        DebugRule& rule = *it;
        if (!rule.enabled)
            continue;
        u32 matchedTarget = target;
        if (rule.useProbe)
        {
            const bool primaryMatch = probeOverride != NULL
                && rule.device == probeOverride->device
                && rule.space == probeOverride->space;
            const bool compatibilityMatch = compatibilityProbe != NULL
                && rule.device == compatibilityProbe->device
                && rule.space == compatibilityProbe->space;
            if (!primaryMatch && !compatibilityMatch)
                continue;
            matchedTarget = primaryMatch ? probeOverride->target
                                         : compatibilityProbe->target;
        }
        else if (rule.category != category)
            continue;
        if ((rule.accessMask & access) == 0)
            continue;
        if (matchedTarget < rule.addressStart || matchedTarget > rule.addressEnd)
            continue;
        if (!RuleMatchesValue(rule, beforeValue, beforeKnown, afterValue, afterKnown))
            continue;

        ++rule.hitCount;
        const bool pauseThisHit = (rule.actions & DebugEventAction_Pause) != 0
            // breakOnHit is an ignore count threshold: a repeat rule must
            // pause on every matching access from hit N onward.  Equality
            // here made a normal repeat breakpoint stop only once.
            && rule.breakOnHit != 0 && rule.hitCount >= rule.breakOnHit;
        u8 actions = rule.actions;
        if (!pauseThisHit)
            actions &= ~DebugEventAction_Pause;

        // A pause must always leave evidence in the trace, even for a rule
        // configured without explicit logging.
        if ((rule.actions & DebugEventAction_Log) != 0 || pauseThisHit)
        {
            if (probeOverride != NULL)
            {
                const DebugProbe& eventProbe = rule.useProbe
                    && compatibilityProbe != NULL
                    && rule.device == compatibilityProbe->device
                    && rule.space == compatibilityProbe->space
                    ? *compatibilityProbe : *probeOverride;
                PushEventWithProbe(rule.id, category, access, target,
                    beforeValue, beforeKnown, afterValue, afterKnown,
                    pc, clock, actions, eventProbe, source, phase, raster);
            }
            else
                PushEvent(rule.id, category, access, target,
                          static_cast<u8>(beforeValue), beforeKnown,
                          static_cast<u8>(afterValue), afterKnown, pc, clock, actions);
        }

        if (pauseThisHit && rule.oneShot)
            rule.enabled = false;

        pause = pause || pauseThisHit;
    }

    return pause;
}

void DebugEventManager::RecordLegacyHit(DebugEventCategory category, u8 access, u16 target,
                                        u8 value, bool valueKnown, u16 pc, u64 clock)
{
    if (!m_runtimeActive)
        return;
    PushEvent(0, category, access, target, value, valueKnown, pc, clock,
              DebugEventAction_Log | DebugEventAction_Pause);
}

void DebugEventManager::SetBankProvider(BankProvider provider, void* context)
{
    m_bankProvider = provider;
    m_bankContext = context;
}

size_t DebugEventManager::SetEventCapacity(size_t capacity)
{
    // Clamped rather than refused: a caller asking for a million events wants
    // depth, not an error, and one asking for none has almost certainly made a
    // mistake. Each event is a little over a hundred bytes, so the ceiling is
    // what keeps an over-enthusiastic trace from eating the machine.
    static const size_t kMinimum = 64;
    static const size_t kMaximum = 1000000;

    if (capacity < kMinimum)
        capacity = kMinimum;
    if (capacity > kMaximum)
        capacity = kMaximum;

    if (capacity != m_events.size())
    {
        m_events.assign(capacity, DebugEvent());
        m_eventHead = 0;
        m_eventCount = 0;
        m_droppedEvents = 0;
        ++m_eventGeneration;
    }

    return m_events.size();
}

size_t DebugEventManager::GetEventCapacity() const
{
    return m_events.size();
}

void DebugEventManager::ClearEvents()
{
    m_eventHead = 0;
    m_eventCount = 0;
    m_droppedEvents = 0;
    ++m_eventGeneration;
}

size_t DebugEventManager::GetEventCount() const
{
    return m_eventCount;
}

u64 DebugEventManager::GetDroppedEventCount() const
{
    return m_droppedEvents;
}

u64 DebugEventManager::GetEventGeneration() const
{
    return m_eventGeneration;
}

void DebugEventManager::CopyEvents(std::vector<DebugEvent>& out) const
{
    out.clear();
    const size_t capacity = m_events.size();
    out.reserve(capacity);
    if (capacity == 0)
        return;
    const size_t first = (m_eventHead + capacity - m_eventCount) % capacity;
    for (size_t i = 0; i < m_eventCount; ++i)
        out.push_back(m_events[(first + i) % capacity]);
}

bool DebugEventManager::CopyEventsIfChanged(
    u64& generation, std::vector<DebugEvent>& out) const
{
    if (generation == m_eventGeneration)
        return false;
    CopyEvents(out);
    generation = m_eventGeneration;
    return true;
}

void DebugEventManager::PushEvent(u32 ruleId, DebugEventCategory category, u8 access,
                                  u16 target, u8 value, bool valueKnown, u16 pc, u64 clock,
                                  u8 actions)
{
    PushEvent(ruleId, category, access, target, 0, false, value, valueKnown,
              pc, clock, actions);
}

void DebugEventManager::PushEvent(u32 ruleId, DebugEventCategory category, u8 access,
                                  u16 target, u8 beforeValue, bool beforeKnown,
                                  u8 afterValue, bool afterKnown, u16 pc, u64 clock,
                                  u8 actions)
{
    const DebugProbe probe = ResolveLegacyProbe(category, access, target);
    PushEventWithProbe(ruleId, category, access, target, beforeValue, beforeKnown,
        afterValue, afterKnown, pc, clock, actions, probe);
}

DebugProbe DebugEventManager::ResolveLegacyProbe(
    DebugEventCategory category, u8 access, u16 target) const
{
    DebugProbe probe;
    probe.target = target;
    const char* typeKey = NULL;
    DebugSpaceId space = DebugCommonSpace::Unknown;
    switch (category)
    {
    case DebugEventCategory::CpuExecute:
        typeKey = "zilog.z80"; space = DebugCommonSpace::Execute; break;
    case DebugEventCategory::CpuMemory:
        typeKey = "zilog.z80"; space = DebugCommonSpace::Memory; break;
    case DebugEventCategory::Vram:
        typeKey = "ti.tms99xx"; space = DebugCommonSpace::Memory; break;
    case DebugEventCategory::Io:
        typeKey = "debug.system_bus"; space = DebugCommonSpace::IoPorts; break;
    case DebugEventCategory::VdpRegister:
        typeKey = "ti.tms99xx"; space = DebugCommonSpace::Registers; break;
    case DebugEventCategory::Fdc:
        typeKey = "nec.upd765"; space = DebugCommonSpace::State; break;
    case DebugEventCategory::Ppi:
        space = (access & DebugEventAccess_Event) != 0
            ? DebugCommonSpace::State : DebugCommonSpace::IoPorts;
        if (target >= 0xE4 && target <= 0xE7)
        {
            const auto* sfPpi = m_registry->FindDevice(
                "event.sc3000.sf7000.ppi2");
            if (sfPpi != NULL)
                probe.device = sfPpi->id;
        }
        typeKey = "intel.8255";
        break;
    case DebugEventCategory::VideoTiming:
        typeKey = "ti.tms99xx"; space = DebugCommonSpace::Raster; break;
    case DebugEventCategory::Tape:
        typeKey = "sega.sr1000"; space = DebugCommonSpace::State; break;
    case DebugEventCategory::Audio:
        typeKey = "ti.sn76489"; space = DebugCommonSpace::Registers; break;
    case DebugEventCategory::AyExpansion:
        typeKey = "gi.ay_3_8910"; space = DebugCommonSpace::Registers; break;
    case DebugEventCategory::DeviceState:
        break;
    }
    if (probe.device == 0 && typeKey != NULL)
    {
        const auto* device = m_registry->FindDeviceByType(typeKey);
        if (device != NULL)
            probe.device = device->id;
    }
    probe.space = space;
    return probe;
}

void DebugEventManager::PushEventWithProbe(u32 ruleId, DebugEventCategory category, u8 access,
                                           u16 target, u8 beforeValue, bool beforeKnown,
                                           u8 afterValue, bool afterKnown, u16 pc, u64 clock,
                                           u8 actions, const DebugProbe& probe)
{
    const DebugEventSource source = (access & DebugEventAccess_Event) != 0
        ? DebugEventSource::DeviceInternal : DebugEventSource::CpuInstruction;
    const DebugEventPhase phase = (access & DebugEventAccess_Event) != 0
        ? DebugEventPhase::AfterCommitDeviceBoundary
        : DebugEventPhase::AfterCommitInstructionBoundary;
    PushEventWithProbe(ruleId, category, access, target,
        static_cast<u64>(beforeValue), beforeKnown,
        static_cast<u64>(afterValue), afterKnown, pc, clock,
        actions, probe, source, phase);
}

void DebugEventManager::PushEventWithProbe(u32 ruleId, DebugEventCategory category, u8 access,
                                           u16 target, u64 beforeValue, bool beforeKnown,
                                           u64 afterValue, bool afterKnown, u16 pc, u64 clock,
                                           u8 actions, const DebugProbe& probe,
                                           DebugEventSource source, DebugEventPhase phase,
                                           const DebugEventRasterContext* raster)
{
    DebugEvent& event = m_events[m_eventHead];
    event.sequence = m_nextSequence++;
    event.clock = clock;
    event.ruleId = ruleId;
    event.pc = pc;
    event.bankKnown = m_bankProvider != NULL;
    event.bank = event.bankKnown ? m_bankProvider(m_bankContext) : 0;
    event.target = target;
    event.value = static_cast<u8>(afterValue);
    event.beforeValue = static_cast<u8>(beforeValue);
    event.afterValue = static_cast<u8>(afterValue);
    event.category = static_cast<u8>(category);
    event.access = access;
    event.actions = actions;
    event.valueKnown = afterKnown;
    event.beforeKnown = beforeKnown;
    event.afterKnown = afterKnown;

    event.schemaVersion = DebugEvent::SchemaVersion;
    event.device = probe.device;
    event.space = probe.space;
    event.source = source == DebugEventSource::Unknown
        ? ((access & DebugEventAccess_Event) != 0
            ? DebugEventSource::DeviceInternal : DebugEventSource::CpuInstruction)
        : source;
    event.phase = phase == DebugEventPhase::Unknown
        ? ((access & DebugEventAccess_Event) != 0
            ? DebugEventPhase::AfterCommitDeviceBoundary
            : DebugEventPhase::AfterCommitInstructionBoundary)
        : phase;
    event.valueWidth = GetProbeValueWidth(*m_registry, probe);
    event.targetV2 = probe.target;
    event.beforeValueV2 = beforeValue;
    event.afterValueV2 = afterValue;
    event.raster = raster != NULL ? *raster : DebugEventRasterContext();

    m_eventHead = (m_eventHead + 1) % m_events.size();
    if (m_eventCount < m_events.size())
        ++m_eventCount;
    else
        ++m_droppedEvents;
    ++m_eventGeneration;
}
