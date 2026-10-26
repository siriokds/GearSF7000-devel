#include "SystemRamRegions.h"
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#include "SystemBusMSX.h"
#include "Memory.h"
#include "Cartridge.h"
#include "Audio.h"
#include "MachineIOPorts.h"
#include "SaveStateStream.h"
#include <sstream>
#include <fstream>
#include "miniz/miniz.h"

SystemBusMSX::SystemBusMSX(Memory& memory, Cartridge& cart, Audio& audio, MachineIOPorts& ports)
    : m_memory(memory), m_cart(cart), m_audio(audio), m_ports(ports),
      m_ppi(*this, {"event.msx.ppi", "event.msx", "MSX PPI", 0xA8, true})
{
    m_slots.SetStateChangeCallback([this](u32 target, u8 before, u8 after) {
        if (m_slotsId) m_memory.CheckDeviceStateChange(
            {m_slotsId, DebugCommonSpace::State, target}, before, after);
    });
}

SystemBusMSX::~SystemBusMSX()
{
#if GEARSF7000_ENABLE_AY
    if (m_audio.GetAyApu()->GetPorts() == &m_joysticks)
        m_audio.GetAyApu()->SetPorts(nullptr);
#endif
}

// The AY's pins carry the joysticks, and port A bit 7 the cassette, which the
// joystick device passes through.
void SystemBusMSX::ConnectPeripherals()
{
#if GEARSF7000_ENABLE_SR1000 && GEARSF7000_ENABLE_AY
    m_joysticks.SetCassette(&m_tape);
    m_tape.SetMonitor([this](float sample, bool synthesized, float frequency) {
        m_audio.SR1000SpeakerWrite(sample, synthesized, frequency);
    });
#endif
#if GEARSF7000_ENABLE_AY
    m_audio.GetAyApu()->SetPorts(&m_joysticks);
#endif
}

bool SystemBusMSX::LoadOptionalFirmware(const std::string& logoPath,
    const std::string& basicPath, std::string& error)
{
    const auto load = [&](const std::string& path, const char* name, std::vector<u8>& image) {
        if (path.empty()) return true;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) { error = std::string(name) + ": cannot open " + path; return false; }
        if (file.tellg() != 0x4000) { error = std::string(name) + ": expected 16384 bytes"; return false; }
        image.resize(0x4000); file.seekg(0); file.read(reinterpret_cast<char*>(image.data()), image.size());
        if (!file) { error = std::string(name) + ": cannot read " + path; return false; }
        return true;
    };
    std::vector<u8> logo, basic;
    if (!load(logoPath, "Logo ROM", logo) || !load(basicPath, "BASIC ROM", basic)) return false;
    return LoadOptionalFirmwareFromBuffers(std::move(logo), std::move(basic), error);
}
bool SystemBusMSX::LoadOptionalFirmwareFromBuffers(std::vector<u8> logo, std::vector<u8> basic, std::string& error)
{
    if ((!logo.empty() && logo.size() != 0x4000) || (!basic.empty() && basic.size() != 0x4000)) {
        error = "Optional MSX firmware must contain 16384 bytes";
        return false;
    }
    if (!basic.empty() && (basic[0] != 'A' || basic[1] != 'B')) {
        error = "BASIC ROM must be an MSX cartridge image with an AB header (e.g. cbios_basic.rom)";
        return false;
    }
    error.clear();
    if (logo == m_logoImage && basic == m_basicImage) return true;
    m_memory.ResetRomDisassembledMemory();
    m_logoImage = std::move(logo); m_basicImage = std::move(basic);
    RebindMemory();
    return true;
}
void SystemBusMSX::CopyOptionalFirmware(const SystemBusMSX& other)
{
    m_logoImage = other.m_logoImage; m_basicImage = other.m_basicImage;
    RebindMemory();
}
u32 SystemBusMSX::LogoCRC() const { return m_logoImage.empty() ? 0 : mz_crc32(0, m_logoImage.data(), m_logoImage.size()); }
u32 SystemBusMSX::BasicCRC() const { return m_basicImage.empty() ? 0 : mz_crc32(0, m_basicImage.data(), m_basicImage.size()); }

void SystemBusMSX::RebindMemory()
{
    for (unsigned slot = 0; slot < 4; ++slot)
        for (unsigned page = 0; page < 4; ++page) m_slots.Bind(slot, 0, page, nullptr);
    m_bios.reset(); m_rom.reset(); m_ram.reset(); m_logo.reset(); m_basic.reset(); m_ascii16.reset();
    if (m_memory.GetBiosSize() == GetBiosSize())
        m_bios = std::make_unique<MSXLinearMemory>(MSXLinearMemory::Rom(
            {m_memory.GetBios(), GetBiosSize()}, 0, 0x8000));
    m_ram = std::make_unique<MSXLinearMemory>(MSXLinearMemory::Ram(
        {m_memory.GetInternalRam(), 0x10000}, 0, 0x10000));
    if (m_cart.IsReady() && m_cart.GetType() == Cartridge::MSX_LINEAR &&
        Cartridge::IsTypeCompatible(Cartridge::MSX_LINEAR, m_cart.GetROMSize(), nullptr))
    {
        // BASIC ROMs live only in page 2: mirroring their AB header into
        // page 1 makes the BIOS select the cartridge over its BASIC code.
        // Follow openMSX RomFactory's small-ROM header heuristic (Page2):
        // BASIC text in page 2, with no INIT or an immediate RET in page 2.
        const u8* data = m_cart.GetROM();
        const size_t size = static_cast<size_t>(m_cart.GetROMSize());
        bool page2 = false;
        if (size <= 0x4000 && data[0] == 'A' && data[1] == 'B')
        {
            const u16 init = data[2] | (data[3] << 8);
            const u16 text = data[8] | (data[9] << 8);
            page2 = (text & 0xC000) == 0x8000 &&
                (init == 0 || ((init & 0xC000) == 0x8000 && data[init % size] == 0xC9));
        }
        m_rom = std::make_unique<MSXLinearMemory>(MSXLinearMemory::Rom(
            {data, size}, page2 ? 0x8000 : 0x4000, page2 ? 0x4000 : 0x8000, true));
    }
    if (m_cart.IsReady() && m_cart.GetType() == Cartridge::MSX_ASCII16 &&
        Cartridge::IsTypeCompatible(Cartridge::MSX_ASCII16, m_cart.GetROMSize(), nullptr))
    {
        m_ascii16 = std::make_unique<MSXAscii16Rom>(std::span<const u8>(
            m_cart.GetROM(), static_cast<size_t>(m_cart.GetROMSize())));
        m_ascii16->SetStateChangeCallback([this](u32 target, u8 before, u8 after) {
            if (m_mapperId) m_memory.CheckDeviceStateChange(
                {m_mapperId, DebugCommonSpace::State, target}, before, after);
        });
    }
    if (!m_logoImage.empty())
        m_logo = std::make_unique<MSXLinearMemory>(MSXLinearMemory::Rom(m_logoImage, 0x8000, 0x4000));
    if (!m_basicImage.empty())
        m_basic = std::make_unique<MSXLinearMemory>(MSXLinearMemory::Rom(m_basicImage, 0x4000, 0x8000, true));
    for (unsigned page = 0; page < 4; ++page)
    {
        m_slots.Bind(0, 0, page, m_bios.get());
        m_slots.Bind(1, 0, page, m_ascii16 ? static_cast<MSXSlotDevice*>(m_ascii16.get()) : m_rom.get());
        m_slots.Bind(2, 0, page, m_basic.get());
        m_slots.Bind(3, 0, page, m_ram.get());
    }
    if (m_logo) m_slots.Bind(0, 0, 2, m_logo.get());
}

std::array<SystemBusMSX::PageMapping, 4> SystemBusMSX::GetPageMappings() const
{
    std::array<PageMapping, 4> result{};
    for (unsigned page = 0; page < 4; ++page)
    {
        const u16 start = page * 0x4000;
        const auto selected = m_slots.Inspect(start);
        const auto location = m_slots.Resolve(start);
        const char* device = !location.data ? "empty"
            : selected.device == m_bios.get() ? "BIOS"
            : selected.device == m_rom.get() ? "cartridge"
            : selected.device == m_logo.get() ? "Logo"
            : selected.device == m_basic.get() ? "BASIC" : "RAM";
        result[page] = {start, static_cast<u16>(start + 0x3FFF),
            selected.primary, selected.secondary,
            (m_slots.GetExpandedSlots() & (1 << selected.primary)) != 0,
            location.data != nullptr, location.writable, device, location.offset};
    }
    return result;
}

void SystemBusMSX::Reset()
{
#if GEARSF7000_ENABLE_SR1000
    m_tape.Reset(GetCpuPsgClockHz());
#endif
    RebindMemory();
    m_keyboard.Reset();
    m_joysticks.Reset();
    m_ppi.Reset();
    m_slots.Reset();
}

u8 SystemBusMSX::ReadMemory(u16 address) { return m_memory.Read(address); }
void SystemBusMSX::WriteMemory(u16 address, u8 value) { m_memory.Write(address, value); }
u8 SystemBusMSX::ReadIO(u64 time, u16 address)
{
    const u8 port = address;
    if (port == 0x98) return m_ports.GetVideoDevice()->GetDataPort(time);
    if (port == 0x99) return m_ports.GetVideoDevice()->GetStatusFlags();
#if GEARSF7000_ENABLE_AY
    if (port == 0xA2) return m_audio.AyReadData();
#endif
    if (port >= 0xA8 && port <= 0xAB) return m_ppi.ReadRegister(port & 3, time);
    return 0xFF;
}
void SystemBusMSX::WriteIO(u64 time, u16 address, u8 value)
{
    const u8 port = address;
    if (port == 0x98) m_ports.GetVideoDevice()->WriteData(time, value);
    else if (port == 0x99) m_ports.GetVideoDevice()->WriteControl(time, value);
#if GEARSF7000_ENABLE_AY
    else if (port == 0xA0) m_audio.AySelectRegister(value);
    else if (port == 0xA1)
    {
        const u8 before = m_audio.GetAyApu()->read(m_audio.GetAySelectedRegister());
        m_audio.AyWriteData(value);
        if (m_ayId) m_memory.CheckDeviceAccess(
            {m_ayId, DebugCommonSpace::Registers, m_audio.GetAySelectedRegister()},
            DebugEventAccess_Event, before, true,
            m_audio.GetAyApu()->read(m_audio.GetAySelectedRegister()), true);
    }
#endif
    else if (port >= 0xA8 && port <= 0xAB)
    {
        const auto before = m_ppi.GetSnapshot();
        m_ppi.WriteRegister(port & 3, value, time);
        // Only live bus writes reach the renderer. State-validation candidates
        // restore PPI pins too, but must never alter the running audio device.
        m_audio.WriteOneBitLevel(m_keyboardClick);
        const auto after = m_ppi.GetSnapshot();
        if (m_ppiId)
        {
            const unsigned index = port & 3;
            m_memory.CheckDeviceAccess({m_ppiId, DebugCommonSpace::Registers, index},
                DebugEventAccess_Write,
                index == 3 ? before.control : before.outputLatch[index], true,
                index == 3 ? after.control : after.outputLatch[index], true);
            for (unsigned i = 0; i < 3; ++i)
                m_memory.CheckDeviceStateChange({m_ppiId, DebugCommonSpace::State, 0xA8 + i},
                    before.outputLatch[i], after.outputLatch[i]);
            m_memory.CheckDeviceStateChange({m_ppiId, DebugCommonSpace::State, 0xAB}, before.control, after.control);
        }
    }
}

void SystemBusMSX::BindDebugEvents(const DebugDeviceRegistry& registry)
{
    const auto id = [&](const char* key) { auto* d = registry.FindDevice(key); return d ? d->id : 0; };
    const DebugDeviceId keyboardId = id("event.msx.keyboard");
    m_keyboard.SetRowChangeCallback([this, keyboardId](unsigned row, u8 before, u8 after) {
        if (keyboardId) m_memory.CheckDeviceStateChange(
            {keyboardId, DebugCommonSpace::CustomFirst, row}, before, after);
    });
    const DebugDeviceId joysticksId = id("event.msx.joysticks");
    m_joysticks.SetChangeCallback([this, joysticksId](unsigned player, u8 before, u8 after) {
        if (joysticksId) m_memory.CheckDeviceStateChange(
            {joysticksId, DebugCommonSpace::State, player}, before, after);
    });
    m_busId = id("event.msx.bus"); m_vdpId = id("event.msx.vdp");
    m_ayId = id("event.msx.ay"); m_ppiId = id("event.msx.ppi"); m_slotsId = id("event.msx.slots");
    m_mapperId = id("event.msx.cartridge_mapper");
#if GEARSF7000_ENABLE_SR1000
    const DebugDeviceId tapeId = id("event.msx.tape");
    m_tape.SetEvents([this, tapeId](unsigned target, u8 value) {
        if (tapeId && !m_restoringTape) m_memory.CheckDeviceAccess(
            {tapeId, DebugCommonSpace::State, target}, DebugEventAccess_Event, 0, false, value, true);
    });
#endif
}
DebugProbe SystemBusMSX::GetIOProbe(u16 address) const
{
    const u8 port = address;
    DebugProbe probe{m_busId, DebugCommonSpace::IoPorts, port};
    if (port == 0x98 || port == 0x99) probe.device = m_vdpId;
    else if (port >= 0xA0 && port <= 0xA2) probe.device = m_ayId;
    else if (port >= 0xA8 && port <= 0xAB) probe.device = m_ppiId;
    return probe;
}
DebugDeviceDefinition SystemBusMSX::DescribeDebugDevice() const
{
    DebugDeviceDefinition d;
    d.key = "event.msx.bus"; d.parentKey = "event.msx"; d.typeKey = "debug.system_bus"; d.name = "MSX System Bus";
    DebugSpaceDefinition io;
    io.id = DebugCommonSpace::IoPorts; io.key = "io_ports"; io.name = "I/O Ports"; io.targetMax = 0xFF;
    io.supportedAccesses = DebugEventAccess_Read | DebugEventAccess_Write; io.afterValueAccesses = io.supportedAccesses;
    d.spaces.push_back(std::move(io)); return d;
}
void SystemBusMSX::SaveState(std::ostream& stream) const
{
    StateWriter writer(stream); m_slots.SaveState(writer); m_ppi.SaveState(stream); m_keyboard.SaveState(stream);
    if (StateVersion() >= 3) {
        writer.U32(m_logoImage.size()); writer.U32(LogoCRC());
        writer.U32(m_basicImage.size()); writer.U32(BasicCRC());
    }
    // Version 5 says whether the tape follows; in version 4 it always does.
    bool tape = StateVersion() == 4;
    if (StateVersion() >= 5) {
        writer.U8(m_ascii16->GetBank(0)); writer.U8(m_ascii16->GetBank(1));
#if GEARSF7000_ENABLE_SR1000
        tape = m_tape.HasMedia();
#endif
        writer.Bool(tape);
    }
#if GEARSF7000_ENABLE_SR1000
    if (tape) m_tape.SaveState(stream);
#endif
}
bool SystemBusMSX::LoadState(std::istream& stream, unsigned version)
{
    if (version < 1 || version > 5) return false;
    // Version 5 is the ASCII 16 cartridge's, and that cartridge needs it.
    if ((version >= 5) != (m_ascii16 != nullptr)) return false;
    if (version < 3 && (!m_logoImage.empty() || !m_basicImage.empty())) return false;
#if GEARSF7000_ENABLE_SR1000
    auto& tapeSource = m_tapeValidationSource ? *m_tapeValidationSource : m_tape;
    if (version < 4 && tapeSource.HasMedia()) return false;
#else
    if (version == 4) return false; // a tape, which this build cannot hold
#endif
    MSXSlotMapper check;
    StateReader reader(stream);
    if (!check.LoadState(reader)) return false;
    SystemBusMSX candidate(m_memory, m_cart, m_audio, m_ports);
    if (!candidate.m_ppi.LoadState(stream)) return false;
    if (version >= 2 && !candidate.m_keyboard.LoadState(stream)) return false;
    if (version >= 3) {
        const u32 logoSize = reader.U32(), logoCRC = reader.U32();
        const u32 basicSize = reader.U32(), basicCRC = reader.U32();
        if (!reader.Ok() || logoSize != m_logoImage.size() || logoCRC != LogoCRC() ||
            basicSize != m_basicImage.size() || basicCRC != BasicCRC()) return false;
    }
    u8 banks[2] = {0, 0};
    bool tape = version == 4;
    if (version >= 5) {
        banks[0] = reader.U8(); banks[1] = reader.U8(); tape = reader.Bool();
        if (!reader.Ok()) return false;
    }
    const auto state = candidate.m_ppi.GetSnapshot();
    if ((state.drivenValue[0] & state.drivenMask[0]) != check.GetPrimarySlots()) return false;
#if GEARSF7000_ENABLE_SR1000
    std::string tapeBytes;
    if (version >= 5 && tape != tapeSource.HasMedia()) return false;
    if (tape) {
        tapeBytes.assign(std::istreambuf_iterator<char>(stream), {});
        std::istringstream checkTape(tapeBytes);
        if (!tapeSource.LoadState(checkTape, false) || checkTape.peek() != std::char_traits<char>::eof()) return false;
    }
    struct RestoreGuard { bool& flag; ~RestoreGuard() { flag = false; } } guard{m_restoringTape};
    m_restoringTape = true;
#else
    if (tape) return false;
#endif
    std::ostringstream saved;
    candidate.m_ppi.SaveState(saved);
    std::istringstream restored(saved.str());
    if (!m_ppi.LoadState(restored)) return false;
    DrivePortC(state.drivenValue[2], state.drivenMask[2], 0);
    std::ostringstream keyboardState; candidate.m_keyboard.SaveState(keyboardState);
    std::istringstream keyboardInput(keyboardState.str());
    if (!m_keyboard.LoadState(keyboardInput)) return false;
    // Restoring selectors is not an emulated bus write (no debug event).
    std::ostringstream selectors;
    StateWriter writer(selectors); check.SaveState(writer);
    std::istringstream selectorInput(selectors.str());
    StateReader selectorReader(selectorInput);
    if (!m_slots.LoadState(selectorReader)) return false;
    if (m_ascii16) m_ascii16->SetBanks(banks[0], banks[1]);
#if GEARSF7000_ENABLE_SR1000
    if (tape && !m_tapeValidationSource) {
        std::istringstream tapeState(tapeBytes);
        if (!m_tape.LoadState(tapeState)) return false;
    }
#endif
    return true;
}

RamRegions SystemBusMSX::GetRamRegions() const
{ return DescribeSystemRam(m_memory, GetMachineType()); }
