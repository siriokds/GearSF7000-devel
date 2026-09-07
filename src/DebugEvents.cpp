#include "DebugEvents.h"
#include <cstring>

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

const DebugTargetDescriptor kVdpPortTargets[] = {
    { 0xBE, "Data", 8 },
    { 0xBF, "Control / Status", 8 }
};

const DebugTargetDescriptor kVdpRegisterTargets[] = {
    { 0, "R0 Mode Control 1", 8 },
    { 1, "R1 Mode Control 2", 8 },
    { 2, "R2 Name Table", 8 },
    { 3, "R3 Color Table", 8 },
    { 4, "R4 Pattern Table", 8 },
    { 5, "R5 Sprite Attribute Table", 8 },
    { 6, "R6 Sprite Pattern Table", 8 },
    { 7, "R7 Text / Backdrop Color", 8 }
};

const DebugTargetDescriptor kVdpStateTargets[] = {
    { 0, "VRAM Address Counter", 14 },
    { 1, "Control Sequence (Awaiting First Byte)", 1 },
    { 2, "Read Buffer", 8 },
    { 3, "Status", 8 },
    { 4, "IRQ", 1 },
    { 5, "Video Mode", 3 },
    { 6, "CPU Port Latch Replacement", 23 },
    { 7, "Control Latch Byte", 8 }
};

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

const DebugTargetDescriptor kPsgRegisterTargets[] = {
    { 0, "Tone 0", 10 }, { 1, "Volume 0", 4 },
    { 2, "Tone 1", 10 }, { 3, "Volume 1", 4 },
    { 4, "Tone 2", 10 }, { 5, "Volume 2", 4 },
    { 6, "Noise", 3 },   { 7, "Volume 3", 4 }
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

const DebugSpaceDescriptor kVdpSpaces[] = {
    // Device-qualified port events will be enabled when the System Bus owns
    // canonical port decoding (implementation plan step 2).
    { DebugCommonSpace::IoPorts, "io_ports", "I/O Ports", 0x00BE, 0x00BF,
      kReadWrite, DebugEventAccess_None, kReadWrite, 8,
      DebugDescriptorCapability_None, kVdpPortTargets, sizeof(kVdpPortTargets) / sizeof(kVdpPortTargets[0]) },
    { DebugCommonSpace::Registers, "registers", "Registers", 0x0000, 0x00FF,
      DebugEventAccess_Write, DebugEventAccess_Write, DebugEventAccess_Write,
      8, kPublished, kVdpRegisterTargets, sizeof(kVdpRegisterTargets) / sizeof(kVdpRegisterTargets[0]) },
    { DebugCommonSpace::Memory, "memory", "VRAM", 0x0000, 0x3FFF,
      kReadWrite, kReadWrite, kReadWrite, 8, kPublished, NULL, 0 },
    { DebugCommonSpace::State, "state", "Internal State", 0x0000, 0x0007,
      DebugEventAccess_Event, DebugEventAccess_Event, DebugEventAccess_Event,
      8, static_cast<u8>(kPublished | DebugDescriptorCapability_SnapshotReadable),
      kVdpStateTargets, sizeof(kVdpStateTargets) / sizeof(kVdpStateTargets[0]) },
    { DebugCommonSpace::Raster, "raster", "Raster", 0x0000, 0xFFFF,
      DebugEventAccess_Event, DebugEventAccess_None, DebugEventAccess_Event,
      16, kPublished, NULL, 0 }
};

const DebugSpaceDescriptor kPsgSpaces[] = {
    { DebugCommonSpace::IoPorts, "io_ports", "I/O Ports", 0x007F, 0x007F,
      DebugEventAccess_Write, DebugEventAccess_None, DebugEventAccess_Write,
      8, DebugDescriptorCapability_None, NULL, 0 },
    { DebugCommonSpace::Registers, "registers", "Logical Registers", 0x0000, 0x0007,
      DebugEventAccess_Event, DebugEventAccess_None, DebugEventAccess_Event,
      8, kPublished, kPsgRegisterTargets, sizeof(kPsgRegisterTargets) / sizeof(kPsgRegisterTargets[0]) }
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
    { DebugCommonSpace::State, "state", "Transport State", 0x0000, 0x0004,
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
    { GearDevice::Machine, 0, "gearsf7000", "emulator.machine", "GearSF7000", NULL, 0 },
    { GearDevice::SC3000, GearDevice::Machine, "gearsf7000.sc3000", "sega.sc3000", "SC-3000", NULL, 0 },
    { GearDevice::Z80, GearDevice::SC3000, "gearsf7000.sc3000.cpu", "zilog.z80", "Z80", kZ80Spaces, sizeof(kZ80Spaces) / sizeof(kZ80Spaces[0]) },
    { GearDevice::SystemBus, GearDevice::SC3000, "gearsf7000.sc3000.bus", "debug.system_bus", "System Bus", kSystemBusSpaces, sizeof(kSystemBusSpaces) / sizeof(kSystemBusSpaces[0]) },
    { GearDevice::TMS9918, GearDevice::SC3000, "gearsf7000.sc3000.vdp", "ti.tms9918", "TMS9918 VDP", kVdpSpaces, sizeof(kVdpSpaces) / sizeof(kVdpSpaces[0]) },
    { GearDevice::SN76489, GearDevice::SC3000, "gearsf7000.sc3000.psg", "ti.sn76489", "SN76489 PSG", kPsgSpaces, sizeof(kPsgSpaces) / sizeof(kPsgSpaces[0]) },
    { GearDevice::PPI_SC3000, GearDevice::SC3000, "gearsf7000.sc3000.ppi", "intel.8255", "SC-3000 PPI", kPpiScSpaces, sizeof(kPpiScSpaces) / sizeof(kPpiScSpaces[0]) },
    { GearDevice::Tape_SR1000, GearDevice::SC3000, "gearsf7000.sc3000.tape", "sega.sr1000", "SR-1000 Tape", kTapeSpaces, sizeof(kTapeSpaces) / sizeof(kTapeSpaces[0]) },
    { GearDevice::SF7000, GearDevice::SC3000, "gearsf7000.sc3000.sf7000", "sega.sf7000", "SF-7000 Control Station", NULL, 0 },
    { GearDevice::PPI2_SF7000, GearDevice::SF7000, "gearsf7000.sc3000.sf7000.ppi2", "intel.8255", "SF-7000 PPI2", kPpiSfSpaces, sizeof(kPpiSfSpaces) / sizeof(kPpiSfSpaces[0]) },
    { GearDevice::FDC_uPD765, GearDevice::SF7000, "gearsf7000.sc3000.sf7000.fdc", "nec.upd765", "uPD765 FDC", kFdcSpaces, sizeof(kFdcSpaces) / sizeof(kFdcSpaces[0]) },
    { GearDevice::Drive_SF7000, GearDevice::SF7000, "gearsf7000.sc3000.sf7000.drive0", "floppy.drive", "SF-7000 Drive", kDriveSpaces, sizeof(kDriveSpaces) / sizeof(kDriveSpaces[0]) },
    { GearDevice::USART_8251, GearDevice::SF7000, "gearsf7000.sc3000.sf7000.usart", "intel.8251", "8251 USART", kUsartSpaces, sizeof(kUsartSpaces) / sizeof(kUsartSpaces[0]) },
    // Hangs off the SC-3000 rather than the SF-7000: it is a cartridge-port
    // expansion, present or not independently of the disk station.
    { GearDevice::AY_Expansion, GearDevice::SC3000, "gearsf7000.sc3000.ay", "gi.ay_3_8910", "AY Expansion", kAySpaces, sizeof(kAySpaces) / sizeof(kAySpaces[0]) }
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

bool IsPpiDevice(DebugDeviceId device)
{
    return device == GearDevice::PPI_SC3000 || device == GearDevice::PPI2_SF7000;
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
    , m_registry(registry != NULL ? registry : &GetGearDebugDeviceRegistry())
    , m_bankProvider(NULL)
    , m_bankContext(NULL)
{
}

u32 DebugEventManager::AddRule(const DebugRule& source)
{
    DebugRule rule = source;
    if (rule.useProbe)
    {
        const DebugDeviceRegistry& registry = *m_registry;
        const DebugDeviceDescriptor* device = rule.device != 0
            ? registry.FindDevice(rule.device) : registry.FindDevice(rule.deviceKey.c_str());
        if (device == NULL)
            return 0;
        const DebugSpaceDescriptor* space = rule.space != DebugCommonSpace::Unknown
            ? registry.FindSpace(device->id, rule.space)
            : registry.FindSpace(device->id, rule.spaceKey.c_str());
        if (space == NULL)
            return 0;
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
        return 0;
    rule.id = static_cast<u32>(m_nextRuleId++);
    rule.hitCount = 0;
    m_rules.push_back(rule);
    return rule.id;
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

std::vector<DebugRule>& DebugEventManager::GetRules()
{
    return m_rules;
}

const std::vector<DebugRule>& DebugEventManager::GetRules() const
{
    return m_rules;
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
    const DebugProbe probe = AdaptLegacyDebugProbe(category, access, target);
    return EvaluateWithProbe(category, access, target, beforeValue, beforeKnown,
        afterValue, afterKnown, pc, clock, &probe,
        DebugEventSource::Unknown, DebugEventPhase::Unknown, raster);
}

bool DebugEventManager::EvaluateIoPort(const DebugProbe& decodedProbe, u16 rawPort,
                                       u8 access, u8 value, bool valueKnown,
                                       u16 pc, u64 clock)
{
    DebugProbe probe = decodedProbe;
    if (probe.device == 0)
        probe = AdaptLegacyDebugProbe(DebugEventCategory::Io, access, rawPort);
    probe.space = DebugCommonSpace::IoPorts;

    // Legacy generic I/O rules keep matching the raw Z80 port. The v2 event
    // identifies the decoded device and its canonical port.
    bool pause = EvaluateWithProbe(DebugEventCategory::Io, access, rawPort,
        0, false, value, valueKnown, pc, clock, &probe);

    // PPI Ports is a qualified view of that same CPU transaction. There is no
    // second publisher in the 8255 implementation; only matching legacy PPI
    // rules are evaluated here for compatibility with existing presets.
    if (IsPpiDevice(probe.device))
        pause = EvaluateWithProbe(DebugEventCategory::Ppi, access,
            static_cast<u16>(probe.target), 0, false, value, valueKnown,
            pc, clock, &probe) || pause;
    return pause;
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
        afterValue, afterKnown, pc, clock, &probe, source, phase, raster);
}

bool DebugEventManager::EvaluateWithProbe(DebugEventCategory category, u8 access, u16 target,
                                          u64 beforeValue, bool beforeKnown,
                                          u64 afterValue, bool afterKnown,
                                          u16 pc, u64 clock,
                                          const DebugProbe* probeOverride,
                                          DebugEventSource source,
                                          DebugEventPhase phase,
                                          const DebugEventRasterContext* raster)
{
    bool pause = false;

    for (std::vector<DebugRule>::iterator it = m_rules.begin(); it != m_rules.end(); ++it)
    {
        DebugRule& rule = *it;
        if (!rule.enabled)
            continue;
        u32 matchedTarget = target;
        if (rule.useProbe)
        {
            if (probeOverride == NULL || rule.device != probeOverride->device
                || rule.space != probeOverride->space)
                continue;
            matchedTarget = probeOverride->target;
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
                PushEventWithProbe(rule.id, category, access, target,
                    beforeValue, beforeKnown, afterValue, afterKnown,
                    pc, clock, actions, *probeOverride, source, phase, raster);
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
}

size_t DebugEventManager::GetEventCount() const
{
    return m_eventCount;
}

u64 DebugEventManager::GetDroppedEventCount() const
{
    return m_droppedEvents;
}

void DebugEventManager::CopyEvents(std::vector<DebugEvent>& out) const
{
    out.clear();
    out.reserve(m_eventCount);
    const size_t capacity = m_events.size();
    if (capacity == 0)
        return;
    const size_t first = (m_eventHead + capacity - m_eventCount) % capacity;
    for (size_t i = 0; i < m_eventCount; ++i)
        out.push_back(m_events[(first + i) % capacity]);
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
    const DebugProbe probe = AdaptLegacyDebugProbe(category, access, target);
    PushEventWithProbe(ruleId, category, access, target, beforeValue, beforeKnown,
        afterValue, afterKnown, pc, clock, actions, probe);
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
}
