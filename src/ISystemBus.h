// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef I_SYSTEM_BUS_H
#define I_SYSTEM_BUS_H

#include "definitions.h"
#include <cstddef>
#include "IDebugEventProvider.h"
#include "VideoDeviceDescriptor.h"
#include "VdpLineType.h"
#include "SystemInputDescriptor.h"
#include "RamRegion.h"

// Motherboard identity, independent of Cartridge::CartridgeTypes and input mode.
enum class MachineType : u8
{
    SG1000,
    SC3000,
    SMS,
    // Appended: the values above are persisted in settings and states.
    Coleco,
    MSX,
    // Appended for Sord M5; earlier values remain stable.
    SordM5,
    // Appended for the separate handheld motherboard.
    GameGear
};

// Complete hardware composition. MachineType remains the motherboard
// identity because SC-3000 and SC-3000 + SF-7000 share the same onboard bus.
// Values 0, 1 and 2 intentionally preserve existing Emulator/Machine settings.
enum class SystemModel : u8
{
    SG1000 = static_cast<u8>(MachineType::SG1000),
    // Values 1 and 2 were the original SC-3000 choices.  Keep them assigned
    // to the Export board so existing configuration files retain the
    // emulator's historic 3.580000 MHz SC-3000 behaviour.
    SC3000Export = static_cast<u8>(MachineType::SC3000),
    SC3000ExportSF7000,
    SC3000Japan,
    SC3000JapanSF7000,
    SG1000SK1100,
    SMSNTSC,
    SMSPAL,
    SegaMarkIII,
    // Appended for the same reason as MachineType::Coleco.
    ColecoVision,
    ColecoVisionSGM,
    MSX1NTSC,
    MSX1PAL,
    SordM5NTSC,
    SordM5PAL,
    GameGearJapan,
    GameGearExport,

    // Source compatibility for code that predates explicit board variants.
    SC3000 = SC3000Export,
    SC3000SF7000 = SC3000ExportSF7000
};

struct SystemModelDescriptor
{
    SystemModel model;
    MachineType motherboard;
    const char* name;
    u32 cpuPsgClockHz;
    bool hasSK1100;
    bool hasSF7000;
    // The motherboard's own work RAM, as the CPU sees it: the RAM that
    // answers when no cartridge or expansion takes the cycle. Physical
    // chips can be larger (the Mark II A has a 2 KiB chip with A10 tied
    // low); this is the decoded size. Expansions such as the SF-7000 do
    // not change it - their RAM is theirs.
    u32 boardRamSize;
};

constexpr bool IsM5System(SystemModel model)
{ return model == SystemModel::SordM5NTSC || model == SystemModel::SordM5PAL; }

constexpr bool IsMSXSystem(SystemModel model)
{ return model == SystemModel::MSX1NTSC || model == SystemModel::MSX1PAL; }

constexpr bool IsGameGearSystem(SystemModel model)
{ return model == SystemModel::GameGearJapan || model == SystemModel::GameGearExport; }

constexpr bool IsValidSystemModel(SystemModel model)
{
    return model == SystemModel::SG1000 ||
           model == SystemModel::SG1000SK1100 ||
           model == SystemModel::SC3000Japan ||
           model == SystemModel::SC3000Export ||
           model == SystemModel::SC3000JapanSF7000 ||
           model == SystemModel::SC3000ExportSF7000 ||
           model == SystemModel::ColecoVision || model == SystemModel::ColecoVisionSGM || IsMSXSystem(model) || IsM5System(model)
#if GEARSF7000_ENABLE_SMS
           || model == SystemModel::SMSNTSC
           || model == SystemModel::SMSPAL
           || model == SystemModel::SegaMarkIII
#endif
#if GEARSF7000_ENABLE_GAMEGEAR
           || IsGameGearSystem(model)
#endif
           ;
}

constexpr bool IsColecoSystem(SystemModel model)
{
    return model == SystemModel::ColecoVision || model == SystemModel::ColecoVisionSGM;
}

constexpr bool HasSGM(SystemModel model) { return model == SystemModel::ColecoVisionSGM; }

constexpr bool IsSMSSystem(SystemModel model)
{
    return model == SystemModel::SMSNTSC ||
           model == SystemModel::SMSPAL ||
           model == SystemModel::SegaMarkIII;
}

constexpr bool IsJapaneseSMSSystem(SystemModel model)
{
    return model == SystemModel::SegaMarkIII;
}

constexpr bool IsSG1000System(SystemModel model)
{
    return model == SystemModel::SG1000 || model == SystemModel::SG1000SK1100;
}

constexpr bool IsSF7000System(SystemModel model)
{
    return model == SystemModel::SC3000JapanSF7000 ||
           model == SystemModel::SC3000ExportSF7000;
}

constexpr bool IsExportSystem(SystemModel model)
{
    return model == SystemModel::SC3000Export ||
           model == SystemModel::SC3000ExportSF7000;
}

constexpr bool HasSK1100(SystemModel model)
{
    return model == SystemModel::SG1000SK1100;
}

constexpr SystemModel WithoutSF7000(SystemModel model)
{
    return model == SystemModel::SC3000JapanSF7000
        ? SystemModel::SC3000Japan
        : model == SystemModel::SC3000ExportSF7000
            ? SystemModel::SC3000Export : model;
}

constexpr SystemModel WithSF7000(SystemModel model)
{
    return model == SystemModel::SC3000Japan ||
           model == SystemModel::SC3000JapanSF7000
        ? SystemModel::SC3000JapanSF7000
        : SystemModel::SC3000ExportSF7000;
}

constexpr SystemModelDescriptor GetSystemModelDescriptor(SystemModel model)
{
    switch (model)
    {
    case SystemModel::SG1000:
        return {model, MachineType::SG1000, "SG-1000 Mark I/II", 3'579'545,
                false, false, 0x400};
    case SystemModel::SG1000SK1100:
        return {model, MachineType::SG1000, "SG-1000 Mark I/II + SK-1100", 3'579'545,
                true, false, 0x400};
    case SystemModel::SC3000Japan:
        return {model, MachineType::SC3000, "SC-3000 Japan", 3'579'545,
                false, false, 0x800};
    case SystemModel::SC3000JapanSF7000:
        return {model, MachineType::SC3000, "SC-3000 Japan + SF-7000", 3'579'545,
                false, true, 0x800};
    case SystemModel::SC3000ExportSF7000:
        return {model, MachineType::SC3000, "SC-3000 Export + SF-7000", 3'580'000,
                false, true, 0x800};
    case SystemModel::SMSNTSC:
        return {model, MachineType::SMS, "Master System (NTSC)", 3'579'545,
                false, false, 0x2000};
    case SystemModel::SMSPAL:
        return {model, MachineType::SMS, "Master System (PAL)", 3'546'893,
                false, false, 0x2000};
    case SystemModel::SegaMarkIII:
        return {model, MachineType::SMS, "Mark III (Japan)", 3'579'545,
                false, false, 0x2000};
    case SystemModel::GameGearJapan:
    case SystemModel::GameGearExport:
        return {model, MachineType::GameGear,
                model == SystemModel::GameGearJapan ? "Game Gear (Japan)" : "Game Gear (Export)",
                3'579'545, false, false, 0x2000};
    case SystemModel::SordM5NTSC:
        return {model, MachineType::SordM5, "Sord M5 (NTSC)", 3'579'545,
                false, false, 0x1000};
    case SystemModel::SordM5PAL:
        return {model, MachineType::SordM5, "Sord M5 (PAL)", 3'579'545,
                false, false, 0x1000};
    case SystemModel::MSX1NTSC:
        return {model, MachineType::MSX, "MSX1 (NTSC)", 3'579'545,
                false, false, 0x10000};
    case SystemModel::MSX1PAL:
        return {model, MachineType::MSX, "MSX1 (PAL)", 3'579'545,
                false, false, 0x10000};
    case SystemModel::ColecoVision:
    case SystemModel::ColecoVisionSGM:
        return {model, MachineType::Coleco, HasSGM(model) ? "ColecoVision + SGM" : "ColecoVision", 3'579'545,
                false, false, 0x400};
    default:
        return {SystemModel::SC3000Export, MachineType::SC3000,
                "SC-3000 Export", 3'580'000, false, false, 0x800};
    }
}

constexpr MachineType GetSystemMotherboard(SystemModel model)
{
    return IsGameGearSystem(model) ? MachineType::GameGear
        : IsSMSSystem(model) ? MachineType::SMS
        : IsColecoSystem(model) ? MachineType::Coleco
        : IsM5System(model) ? MachineType::SordM5
        : IsMSXSystem(model) ? MachineType::MSX
        : IsSG1000System(model) ? MachineType::SG1000
        : MachineType::SC3000;
}

// Which machines can load each other's save states. The variants of one machine -
// region, keyboard, disk unit, expansion - share a family, and so do the SG-1000
// and the SC-3000, which are nearly interchangeable. Wider than the motherboard,
// and independent of the configuration profile used for mapper choices
// (config.h). Game Gear can use the SMS mapper but has its own family and
// configuration. What still differs
// inside a family (region, SF-7000, SGM, video device) keeps its own check.
// Persisted in save states: append only.
enum class SystemFamily : u8
{
    SegaTms = 0,      // SG-1000, SC-3000 and their variants
    MasterSystem = 1, // Master System NTSC/PAL, Mark III
    ColecoVision = 2, // with or without the SGM
    MSX1 = 3,
    SordM5 = 4,
    GameGear = 5
};

constexpr SystemFamily GetSystemFamily(SystemModel model)
{
    return IsGameGearSystem(model) ? SystemFamily::GameGear
        : IsSMSSystem(model) ? SystemFamily::MasterSystem
        : IsColecoSystem(model) ? SystemFamily::ColecoVision
        : IsM5System(model) ? SystemFamily::SordM5
        : IsMSXSystem(model) ? SystemFamily::MSX1 : SystemFamily::SegaTms;
}

constexpr const char* GetSystemFamilyName(SystemFamily family)
{
    switch (family)
    {
    case SystemFamily::SegaTms: return "SG-1000/SC-3000";
    case SystemFamily::MasterSystem: return "Master System";
    case SystemFamily::SordM5: return "Sord M5";
    case SystemFamily::MSX1: return "MSX1";
    case SystemFamily::ColecoVision: return "ColecoVision";
    case SystemFamily::GameGear: return "Game Gear";
    }
    return "unknown";
}

constexpr const char* GetSystemModelName(SystemModel model)
{
    return GetSystemModelDescriptor(model).name;
}

// A firmware address range [start, end); empty when start == end.
struct FirmwareRange
{
    u16 start;
    u16 end;
    bool Contains(u16 address) const { return address >= start && address < end; }
    bool Empty() const { return start == end; }
};

// Which I/O writes reach the SN76489 on a board: port & mask == match.
struct PsgPortDecode
{
    u8 mask;
    u8 match;
    bool Matches(u8 port) const { return (port & mask) == match; }
};

// Transaction boundary of the motherboard. Device ownership, physical POR and
// clock fan-out migrate separately; this contract does not reset every device.
// I/O addresses retain all Z80 address bits; each board decides what to decode.
class ISystemBus : public IDebugEventProvider
{
public:
    virtual ~ISystemBus() = default;
    virtual MachineType GetMachineType() const = 0;
    // Oscillator distributed by this motherboard to the Z80 and SN76489.
    // It is independent from the fitted VDP and its video standard.
    virtual u32 GetCpuPsgClockHz() const = 0;
    // Physical firmware storage required by this board configuration. Zero
    // means absent; otherwise a power of two. Mapping/mirroring is separate.
    virtual u32 GetBiosSize() const { return 0; }
    virtual RamRegions GetRamRegions() const { return {}; }
    // Optional output pin, driven by software. Board presence is reapplied at reset.
    virtual bool HasOneBitAudio() const { return false; }
    virtual u8 ReadMemory(u16 address) = 0;
    virtual void WriteMemory(u16 address, u8 value) = 0;
    virtual u8 ReadIO(u64 tstates, u16 address) = 0;
    virtual void WriteIO(u64 tstates, u16 address, u8 value) = 0;
    virtual unsigned int GetIOWaitStates(u64 tstates, u16 address, bool write) const = 0;
    // Wait states the board inserts into every cycle with /M1 active: opcode
    // fetches, prefix bytes and interrupt acknowledges. The circuit sits on
    // the motherboard, not in the Z80: the ColecoVision and MSX add one, the
    // Sega boards none.
    virtual unsigned int GetWaitCyclesM1() const = 0;
    // Where the VDP /INT output goes on this board.
    virtual VdpLineType GetVdpLineType() const = 0;
    // Player input this board offers; the Input menu is built from it.
    virtual SystemInputDescriptor DescribeInput() const = 0;
    // Where the board's own firmware sits, if any: the ColecoVision BIOS at
    // $0000-$1FFF, an MSX BIOS at $0000-$7FFF; nothing on a Sega console.
    // The ROM map records what a cartridge uses of it (entry points, data)
    // so an export can carry just those parts (DOCS/FIRMWARE_LINK_EXPORT.md).
    virtual FirmwareRange GetFirmwareRange() const { return {0, 0}; }
    // Where the PSG sits on the I/O map, for tools that watch the bus
    // (the ROM map learns sound routines from their PSG writes).
    virtual PsgPortDecode GetPsgPorts() const = 0;
    // Routines the board's own firmware provides for sound (the ColecoVision
    // OS 7 sound manager): entry points a program CALLs, known before any of
    // them writes the PSG. Most boards have none.
    virtual std::size_t GetFirmwareSoundEntries(const u16** entries) const
    {
        *entries = nullptr;
        return 0;
    }
    virtual u8 AcknowledgeInterrupt(u64 tstates) = 0;
    // Physical time and M1 snooping for boards with daisy-chain peripherals.
    // Deltas survive resetting the debugger's displayed elapsed counter.
    virtual void AdvanceCpuHalfCycles(u64) {}
    virtual void ObserveOpcodeFetch(u8) {}
    // New boards may publish their own raw I/O with a physical timestamp.
    virtual bool PublishesIOEvents() const { return false; }
    // Transitional bridge to the existing single CPU I/O publisher.
    virtual DebugProbe GetIOProbe(u16 address) const = 0;
};
#endif
