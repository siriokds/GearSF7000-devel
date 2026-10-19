/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026  Saverio Russo

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 *
 */

#include <iostream>
#include <iomanip>
#include <fstream>
#include "definitions.h"
#include "Memory.h"
#include "miniz/miniz.h"
#include <memory>
#include <algorithm>
#ifndef GEARSF7000_DISABLE_DISASSEMBLER
#include "RomMap.h"
#endif
#include <sstream>
#include "SaveStateStream.h"

namespace
{
constexpr std::uint16_t kMemoryStateVersion = 2;
}
#include "Cartridge.h"
#include "ASC16LMapper.h"
#include "ASC16LSMapper.h"
#include "SC3000SurvivorsMapper.h"
#include "Sega3155208SG1000Mapper.h"
#include "SegaSMSMapper.h"
#include "ColecoMegaCartMapper.h"
#include "SF7000Memory.h"



Memory::Memory(Cartridge* pCartridge, DebugEventManager& debugEvents)
    : m_DebugEvents(debugEvents)
{
    m_pCartridge = pCartridge;
    m_pMapper = NULL; // Inizializzazione di sicurezza
    m_debugMemoryDevice = 0;
    m_debugMapperDevice = 0;
    m_debugSF7000MemoryDevice = 0;
    m_debugVideoDevice = 0;
    m_debugPsgDevice = 0;
    m_LegacyMemoryBreakpointsEnabled = true;
    m_LegacyCpuBreakpointsEnabled = true;
    InitPointer(m_cpuDebugContext);
    InitPointer(m_pDisassembledRomMap);
    InitPointer(m_pDisassembledInternalRamMap);
    InitPointer(m_pDisassembledBiosMap);
    InitPointer(m_pDisassembledExternalRamMap);
    m_runToAddressActive = false;
    m_runToAddress = 0;
    InitPointer(m_pBios);
    InitPointer(m_pInternalRam);
    InitPointer(m_pExternalRam);
    m_bBiosLoaded = false;
    m_bSF7000Enabled = false;
    m_bSGMUpper = false;
    m_bIPLRomDisabled = false;
    m_RomBankAddress = 0;
    m_RomBank = 0;
}



bool Memory::UsesSMSWorkRam() const
{
    return !m_bSF7000Enabled &&
        (m_pCartridge->GetType() == Cartridge::SMS_SEGA_MAPPER ||
         (m_smsMotherboard && m_pCartridge->GetType() == Cartridge::SG1000_1K));
}

Memory::~Memory()
{
    for (auto& [data, records] : m_additionalRomMaps)
        for (auto* record : records) delete record;
    SafeDeleteArray(m_pBios);
    SafeDeleteArray(m_pInternalRam);
    SafeDeleteArray(m_pExternalRam);
    SafeDelete(m_pMapper);

    if (IsValidPointer(m_pDisassembledRomMap))
    {
        for (int i = 0; i < MAX_ROM_SIZE; i++)
        {
            SafeDelete(m_pDisassembledRomMap[i]);
        }
        SafeDeleteArray(m_pDisassembledRomMap);
    }

    if (IsValidPointer(m_pDisassembledInternalRamMap))
    {
        for (int i = 0; i < RamCapacity; i++)
        {
            SafeDelete(m_pDisassembledInternalRamMap[i]);
        }
        SafeDeleteArray(m_pDisassembledInternalRamMap);
    }

    if (IsValidPointer(m_pDisassembledBiosMap))
    {
        for (u32 i = 0; i < m_biosSize; i++)
        {
            SafeDelete(m_pDisassembledBiosMap[i]);
        }
        SafeDeleteArray(m_pDisassembledBiosMap);
    }

    if (IsValidPointer(m_pDisassembledExternalRamMap))
    {
        for (int i = 0; i < 0x10000; i++)
        {
            SafeDelete(m_pDisassembledExternalRamMap[i]);
        }
        SafeDeleteArray(m_pDisassembledExternalRamMap);
    }
}

void Memory::SetCpuDebugContext(CpuDebugContext* context)
{
    m_cpuDebugContext = context;
}

CpuDebugSnapshot Memory::GetCpuDebugSnapshot() const
{
    return m_cpuDebugContext ? m_cpuDebugContext->GetCpuDebugSnapshot()
                             : CpuDebugSnapshot{};
}

void Memory::RequestCpuDebugPause()
{
    if (m_cpuDebugContext)
        m_cpuDebugContext->RequestCpuDebugPause();
}

void Memory::CheckIOPortAccess(const DebugProbe& probe, u16 rawPort, u8 access,
                               u8 value, bool valueKnown)
{
    if (!m_DebugEvents.IsRuntimeActive())
        return;
    const CpuDebugSnapshot cpu = GetCpuDebugSnapshot();
    if (m_DebugEvents.EvaluateIoPort(probe, rawPort, access, value, valueKnown,
                                    cpu.programCounter, cpu.elapsedTStates))
    {
        RequestCpuDebugPause();
    }
}

void Memory::OnCpuInstructionBoundary(u16 nextProgramCounter,
                                      u64 elapsedTStates)
{
    bool pause = m_DebugEvents.Evaluate(DebugEventCategory::CpuExecute,
                                        DebugEventAccess_Execute,
                                        nextProgramCounter, 0, false,
                                        nextProgramCounter, elapsedTStates);

    bool legacyHit = false;
    if (m_runToAddressActive)
    {
        if (m_runToAddress == nextProgramCounter)
        {
            m_runToAddressActive = false;
            legacyHit = true;
        }
    }
    else if (m_LegacyCpuBreakpointsEnabled)
    {
        stDisassembleRecord* currentRecord =
            GetDisassembleRecord(nextProgramCounter, false);
        for (stDisassembleRecord* breakpoint : m_BreakpointsCPU)
        {
            if (breakpoint == currentRecord)
            {
                legacyHit = true;
                break;
            }
        }
    }

    if (legacyHit)
    {
        m_DebugEvents.RecordLegacyHit(DebugEventCategory::CpuExecute,
                                      DebugEventAccess_Execute,
                                      nextProgramCounter, 0, false,
                                      nextProgramCounter, elapsedTStates);
        pause = true;
    }

    if (pause)
    {
        RequestCpuDebugPause();
    }
}

void Memory::Init()
{
    m_pInternalRam = new u8[RamCapacity]{};
    m_pExternalRam = new u8[0x10000]();

#ifndef GEARSF7000_DISABLE_DISASSEMBLER
    m_pDisassembledRomMap = new stDisassembleRecord*[MAX_ROM_SIZE];
    for (int i = 0; i < MAX_ROM_SIZE; i++)
    {
        InitPointer(m_pDisassembledRomMap[i]);
    }

    m_pDisassembledInternalRamMap = new stDisassembleRecord*[RamCapacity];
    for (int i = 0; i < RamCapacity; i++)
    {
        InitPointer(m_pDisassembledInternalRamMap[i]);
    }

    m_pDisassembledExternalRamMap = new stDisassembleRecord*[0x10000];
    for (int i = 0; i < 0x10000; i++)
    {
        InitPointer(m_pDisassembledExternalRamMap[i]);
    }
#endif

    m_BreakpointsCPU.clear();
    m_BreakpointsMem.clear();

    // The event stream records which physical page a logical PC was in, and
    // only Memory can answer that - it is the mapper's bank, not the cached
    // copy the debugger reads.
    m_DebugEvents.SetBankProvider(
        [](void* context) -> u8 {
            return static_cast<Memory*>(context)->GetRomBank();
        },
        this);

    m_runToAddressActive = false;
    m_runToAddress = 0;

    Reset();
}

void Memory::Reset()
{
    CancelRunToAddress();
    m_bSGMUpper = false;
    SetIPLRomDisabled(false);
    m_RomBank = 0;
    m_RomBankAddress = 0x0000;

    for (int i = 0; i < RamCapacity; i++)
    {
        // Power-on policy follows the board, not the cartridge: the small
        // SRAM of SG-1000, SC-3000 and ColecoVision boards comes up as
        // noise; the SMS-family, MSX and M5 boards start zeroed. Only the
        // RAM the board decodes is filled.
        const bool noisyBoard = !m_msxSlots && !m_m5Mapper && !m_smsMotherboard;
        m_pInternalRam[i] = (noisyBoard && i < static_cast<int>(m_boardRamSize))
            ? rand() % 256 : 0;
    }

    for (int i = 0; i < 0x10000; i++)
    {
        //m_pExternalRam[i] = rand() % 256;
        // SGM reset changes its latches, not its RAM (openMSX contract).
        if (!(m_sgm && m_sgm->IsPresent() && i < 0x8000))
            m_pExternalRam[i] = 0;
    }

    // Flat map: the image is poured into the 64 KB and everything is RAM from
    // there on, exactly as on the SF-7000 with its IPL switched off. The copy
    // lives here rather than in SetupMapper() because SetupMapper() runs first
    // and Reset() then zeroes m_pExternalRam, which would wipe a freshly written
    // image. Doing it on every reset is also the useful behaviour: a reset
    // gets the clean image back instead of whatever the program left behind.
    if (IsValidPointer(m_pCartridge)
        && m_pCartridge->GetType() == Cartridge::SC3000_FLAT64K
        && m_pCartridge->IsReady())
    {
        const u8* pRom = m_pCartridge->GetROM();
        if (IsValidPointer(pRom))
        {
            const int romSize = m_pCartridge->GetROMSize();
            const int copySize = romSize > 0x10000 ? 0x10000 : romSize;
            for (int i = 0; i < copySize; i++)
                m_pExternalRam[i] = pRom[i];
            Log("Flat 64K: loaded %d KB of image into RAM", copySize / 1024);
        }
    }
}

u32 Memory::GetMapperId() const
{
    return IsValidPointer(m_pMapper) ? m_pMapper->GetMapperId() : 0u;
}

u8* Memory::GetSMSCartridgeRam()
{
    SegaSMSMapper* mapper = dynamic_cast<SegaSMSMapper*>(m_pMapper);
    return mapper ? mapper->GetCartridgeRam() : NULL;
}

void Memory::SaveState(std::ostream& stream)
{
    StateWriter w(stream);

    w.U16(kMemoryStateVersion);

    w.Bytes(m_pInternalRam, RamCapacity);
    w.Bytes(m_pExternalRam, 0x10000);
    w.Bool(m_bSGMUpper);
    w.Bool(m_bIPLRomDisabled);

    // How the address space is decoded, not just what is in it.
    w.Bool(m_bBiosLoaded);
    w.Bool(m_bSF7000Enabled);

    // Debugger-facing cache of the bank. Kept so a reloaded state shows the
    // same numbers in the UI, but it is not what the CPU reads through: the
    // mapper below is authoritative.
    w.U32(m_RomBankAddress);
    w.U8(m_RomBank);

    // Mapper payload. Mappers do not share a shape - one has a single bank
    // register, another has three slot registers and a RAM-enable flag - so
    // the payload is written blind, behind its own id, version and byte
    // count. A loader that meets an id it does not have installed can then
    // step over exactly the right number of bytes instead of losing sync.
    const bool hasMapper = IsValidPointer(m_pMapper);
    w.Bool(hasMapper);
    if (hasMapper)
    {
        std::stringstream payload(std::ios::in | std::ios::out |
                                  std::ios::binary);
        m_pMapper->SaveState(payload);
        const std::string bytes = payload.str();

        w.U32(m_pMapper->GetMapperId());
        w.U16(m_pMapper->GetStateVersion());
        w.U32(static_cast<std::uint32_t>(bytes.size()));
        w.Bytes(bytes.data(), bytes.size());
    }
}

namespace
{
struct SavedMemory
{
    u16 version = 0;
    std::vector<u8> internal = std::vector<u8>(Memory::RamCapacity, 0);
    std::vector<u8> external = std::vector<u8>(Memory::RamCapacity, 0);
    bool sgm = false, iplDisabled = false, bios = false, sf7000 = false;
    u32 bankAddress = 0;
    u8 bank = 0;
    bool hasMapper = false;
    u32 mapperId = 0;
    u16 mapperVersion = 0;
    std::string mapperBytes;
};
bool ReadSavedMemory(std::istream& stream, SavedMemory& state, Mapper* mapper)
{
    StateReader r(stream);
    state.version = r.U16();
    if (state.version != 1 && state.version != kMemoryStateVersion) return false;
    r.Bytes(state.internal.data(), state.version == 1 ? Memory::LegacyInternalRamSize : Memory::RamCapacity);
    r.Bytes(state.external.data(), Memory::RamCapacity);
    const auto boolean = [&]() {
        const u8 value = r.U8();
        if (value > 1) stream.setstate(std::ios::failbit);
        return value != 0;
    };
    state.sgm = boolean(); state.iplDisabled = boolean();
    state.bios = boolean(); state.sf7000 = boolean();
    state.bankAddress = r.U32(); state.bank = r.U8(); state.hasMapper = boolean();
    if (state.hasMapper != (mapper != nullptr)) return false;
    if (state.hasMapper) {
        state.mapperId = r.U32(); state.mapperVersion = r.U16();
        const u32 size = r.U32();
        if (!r.Ok() || !mapper || state.mapperId != mapper->GetMapperId() ||
            state.mapperVersion != mapper->GetStateVersion()) return false;
        // All existing mapper payloads have a fixed shape for their version.
        // Derive its size rather than trusting an unbounded input allocation.
        std::ostringstream current(std::ios::binary);
        mapper->SaveState(current);
        if (size != current.str().size()) return false;
        state.mapperBytes.resize(size);
        if (size) r.Bytes(&state.mapperBytes[0], size);
    }
    return r.Ok() && !stream.fail();
}
}

bool Memory::ValidateState(std::istream& stream) const
{
    SavedMemory state;
    return ReadSavedMemory(stream, state, m_pMapper);
}

void Memory::LoadState(std::istream& stream)
{
    SavedMemory state;
    if (!ReadSavedMemory(stream, state, m_pMapper)) {
        stream.setstate(std::ios::failbit); return;
    }
    if (state.hasMapper) {
        std::ostringstream backup(std::ios::binary);
        m_pMapper->SaveState(backup);
        std::istringstream payload(state.mapperBytes, std::ios::binary);
        m_pMapper->LoadState(payload);
        if (payload.fail() || payload.peek() != std::char_traits<char>::eof()) {
            std::istringstream restore(backup.str(), std::ios::binary);
            m_pMapper->LoadState(restore);
            stream.setstate(std::ios::failbit); return;
        }
    }
    // v1 stored MSX and SMS motherboard RAM in the external allocation.
    if (state.version == 1) {
        if (m_msxSlots) std::copy(state.external.begin(), state.external.end(), state.internal.begin());
        else if (UsesSMSWorkRam()) std::copy_n(state.external.begin(), 0x2000, state.internal.begin());
    }
    std::copy(state.internal.begin(), state.internal.end(), m_pInternalRam);
    std::copy(state.external.begin(), state.external.end(), m_pExternalRam);
    m_bSGMUpper = state.sgm;
    m_bIPLRomDisabled = state.iplDisabled;
    // Snapshots do not contain firmware and cannot manufacture a BIOS.
    m_bBiosLoaded = state.bios && m_bBiosLoaded && m_biosSize != 0;
    m_bSF7000Enabled = state.sf7000;
    m_RomBankAddress = state.bankAddress;
    m_RomBank = state.bank;
}

std::vector<Memory::stDisassembleRecord*>* Memory::GetBreakpointsCPU()
{
    return &m_BreakpointsCPU;
}

std::vector<Memory::stMemoryBreakpoint>* Memory::GetBreakpointsMem()
{
    return &m_BreakpointsMem;
}


bool Memory::ArmRunToAddress(u16 address)
{
    m_runToAddress = address;
    m_runToAddressActive = true;
    return true;
}

void Memory::CancelRunToAddress()
{
    m_runToAddressActive = false;
}

bool Memory::ConfigureBiosStorage(u32 size)
{
    if (size && (size & (size - 1)))
        return false;
    if (size == m_biosSize)
        return true;

    // Allocate first: an allocation failure leaves the old storage intact.
    std::unique_ptr<u8[]> bytes(size ? new u8[size] : nullptr);
    if (size) std::fill_n(bytes.get(), size, 0xFF);
#ifndef GEARSF7000_DISABLE_DISASSEMBLER
    std::unique_ptr<stDisassembleRecord*[]> records(
        size ? new stDisassembleRecord*[size]() : nullptr);
    for (u32 i = 0; i < m_biosSize; ++i)
    {
        auto* record = m_pDisassembledBiosMap[i];
        m_BreakpointsCPU.erase(std::remove(m_BreakpointsCPU.begin(),
            m_BreakpointsCPU.end(), record), m_BreakpointsCPU.end());
        delete record;
    }
    delete[] m_pDisassembledBiosMap;
    m_pDisassembledBiosMap = records.release();
#endif
    delete[] m_pBios;
    m_pBios = bytes.release();
    m_biosSize = size;
    m_biosFilePath.clear();
    m_bBiosLoaded = false;
    return true;
}

bool Memory::LoadBiosFromBuffer(const u8* data, u32 size, const std::string& identity)
{
    m_bBiosLoaded = false;
    m_biosFilePath.clear();
    if (!data || !m_biosSize || size != m_biosSize) {
        if (m_pBios) memset(m_pBios, 0xFF, m_biosSize);
        return false;
    }
    memcpy(m_pBios, data, size);
    m_biosFilePath = identity;
    m_bBiosLoaded = true;
    return true;
}

void Memory::LoadBios(const char* path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || !m_biosSize || file.tellg() != static_cast<std::streamoff>(m_biosSize)) {
        LoadBiosFromBuffer(nullptr, 0, "");
        Log("Missing or invalid BIOS: %s", path);
        return;
    }
    std::vector<u8> bytes(m_biosSize);
    file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    LoadBiosFromBuffer(file ? bytes.data() : nullptr, bytes.size(), path);
}

u8* Memory::GetInternalRam()
{
    return m_pInternalRam;
}

u8* Memory::GetExternalRam()
{
    return m_pExternalRam;
}


u32 Memory::GetBiosCRC() const
{
    // Recalculate from current bytes: the Memory Editor may patch firmware.
    return m_bBiosLoaded ? static_cast<u32>(mz_crc32(0, m_pBios, m_biosSize)) : 0;
}

u8* Memory::GetBios()
{
    return m_pBios;
}

u8 Memory::GetRomBank()
{
    if (m_pMapper != NULL)
    {
        m_RomBank = m_pMapper->GetRomBank();
    }
    return m_RomBank;
}

u32 Memory::GetRomBankAddress()
{
    if (m_pMapper != NULL)
    {
        m_RomBankAddress = m_pMapper->GetRomBankAddress();
    }
    return m_RomBankAddress;
}

void Memory::WriteMapperPort(u8 port, u8 value)
{
    // The SF-7000 interface owns this I/O range when fitted. A Survivors
    // cartridge cannot answer behind that overlay.
    if (!m_bSF7000Enabled && IsValidPointer(m_pMapper))
    {
        const u8 beforeBank = m_pMapper->GetRomBank();
        m_pMapper->WriteIO(port, value);
        PublishMapperBankChange(beforeBank, m_pMapper->GetRomBank());
    }
}

void Memory::WriteMapperMemory(u16 address, u8 value)
{
    if (!IsValidPointer(m_pMapper))
        return;
    const u8 beforeBank = m_pMapper->GetRomBank();
    m_pMapper->Write(address, value);
    PublishMapperBankChange(beforeBank, m_pMapper->GetRomBank());
}

void Memory::PublishMapperBankChange(u8 beforeBank, u8 afterBank)
{
    if (beforeBank == afterBank || m_debugMapperDevice == 0)
        return;
    const CpuDebugSnapshot cpu = GetCpuDebugSnapshot();
    if (m_DebugEvents.EvaluateProbe(
            {m_debugMapperDevice, DebugCommonSpace::State, 0},
            DebugEventAccess_Event, beforeBank, true, afterBank, true,
            DebugEventSource::DeviceInternal,
            DebugEventPhase::AfterCommitDeviceBoundary,
            cpu.programCounter, cpu.elapsedTStates))
        RequestCpuDebugPause();
}

u8 Memory::ReadMegaCart(u16 address, bool publish)
{
    auto* mapper = static_cast<ColecoMegaCartMapper*>(m_pMapper);
    if (!mapper) return 0xFF;
    if (!publish) return mapper->Peek(address);
    const u8 before = mapper->GetRomBank();
    const u8 value = mapper->Read(address);
    PublishMapperBankChange(before, mapper->GetRomBank());
    return value;
}

DebugDeviceDefinition Memory::DescribeDebugDevice() const
{
    DebugDeviceDefinition device;
    device.key = m_debugMachineKey + ".memory";
    device.parentKey = m_debugMachineKey;
    device.typeKey = "memory.controller";
    device.name = "Memory Controller";
    DebugSpaceDefinition space;
    space.id = DebugCommonSpace::Memory;
    space.key = "address_space";
    space.name = "CPU Address Space";
    space.targetMax = 0xffff;
    space.supportedAccesses = DebugEventAccess_Read | DebugEventAccess_Write;
    space.beforeValueAccesses = DebugEventAccess_Write;
    space.afterValueAccesses = space.supportedAccesses;
    device.spaces.push_back(std::move(space));
    return device;
}

void Memory::SetDebugMachineKey(const std::string& key)
{
    m_debugMachineKey = key;
    m_debugVideoDevice = 0;
    m_debugPsgDevice = 0;
    if (IsValidPointer(m_pMapper))
        m_pMapper->SetDebugParentKey(m_debugMachineKey + ".memory");
}

void Memory::BindDebugEventProviders(const DebugDeviceRegistry& registry)
{
    const auto definition = DescribeDebugDevice();
    const auto* memory = registry.FindDevice(definition.key.c_str());
    m_debugMemoryDevice = memory ? memory->id : 0;
    m_debugMapperDevice = 0;
    const auto* sf7000Memory = registry.FindDevice(
        "event.sc3000.sf7000.memory");
    m_debugSF7000MemoryDevice = sf7000Memory ? sf7000Memory->id : 0;
    const auto* video = registry.FindDevice(
        (m_debugMachineKey + ".vdp").c_str());
    m_debugVideoDevice = video ? video->id : 0;
    // The PSG by the machine's key, as the VDP: its type key differs between
    // boards (ti.sn76489, sega.sms_psg), so a lookup by type misses some.
    const auto* psg = registry.FindDevice(
        (m_debugMachineKey + ".psg").c_str());
    m_debugPsgDevice = psg ? psg->id : 0;
    if (IsValidPointer(m_pMapper))
    {
        const auto definition = m_pMapper->DescribeDebugDevice();
        const auto* mapper = registry.FindDevice(definition.key.c_str());
        m_debugMapperDevice = mapper ? mapper->id : 0;
    }
}

bool Memory::IsBiosLoaded() const
{
    return m_bBiosLoaded;
}

void Memory::EnableSGMUpper(bool enable)
{
    m_bSGMUpper = enable;
}

void Memory::SetIPLRomDisabled(bool disabled)
{
    if (m_bIPLRomDisabled == disabled)
        return;
    const bool beforeVisible = !m_bIPLRomDisabled;
    m_bIPLRomDisabled = disabled;
    if (m_debugSF7000MemoryDevice != 0)
        CheckDeviceStateChange(
            {m_debugSF7000MemoryDevice, DebugCommonSpace::State,
             SF7000MemoryDevice::IplVisible},
            beforeVisible ? 1 : 0, disabled ? 0 : 1);
}

void Memory::EnableSF7000(bool enable)
{
    if (m_bSF7000Enabled == enable)
        return;
    m_bSF7000Enabled = enable;
}

bool Memory::IsSF7000Enabled() const
{
    return m_bSF7000Enabled;
}


void Memory::SetupMapper()
{
    SafeDelete(m_pMapper);

    switch (m_pCartridge->GetType())
    {
    case Cartridge::SC3000_ASC16L:
        m_pMapper = new ASC16LMapper(m_pCartridge);
        break;
    case Cartridge::SC3000_ASC16LS:
        m_pMapper = new ASC16LSMapper(m_pCartridge);
        break;
    case Cartridge::SC3000_SURVIVORS_MULTICART:
        m_pMapper = new SC3000SurvivorsMapper(
            m_pCartridge, SC3000SurvivorsMapper::Multicart);
        break;
    case Cartridge::SC3000_SURVIVORS_MEGACART:
        m_pMapper = new SC3000SurvivorsMapper(
            m_pCartridge, SC3000SurvivorsMapper::Megacart);
        break;
    case Cartridge::SG1000_SEGA_315_5208:
    case Cartridge::SG1000_SEGA_315_5208_DAHJEE_B:
        m_pMapper = new Sega3155208SG1000Mapper(m_pCartridge);
        break;
    case Cartridge::SMS_SEGA_MAPPER:
        m_pMapper = new SegaSMSMapper(m_pCartridge);
        break;
    case Cartridge::COLECO_MEGACART:
        m_pMapper = new ColecoMegaCartMapper(m_pCartridge);
        break;
    default:
        m_pMapper = NULL; // new StandardMapper(m_pCartridge);
        break;
    }

    if (IsValidPointer(m_pMapper))
    {
        m_pMapper->SetDebugParentKey(m_debugMachineKey + ".memory");
        m_pMapper->Reset();
    }
}

// In Memory.h (privato)
void Memory::InternalBreakpointsCheck(DebugEventCategory category, u16 address, bool write,
                                      const std::vector<stMemoryBreakpoint>& breakpoints)
{
    if (!m_DebugEvents.IsRuntimeActive())
        return;
    for (const auto& bp : breakpoints)
    {
        if (write && !bp.write) continue;
        if (!write && !bp.read) continue;

        bool hit = false;
        if (bp.range) {
            hit = (address >= bp.address1 && address <= bp.address2);
        }
        else {
            hit = (bp.address1 == address);
        }

        if (hit) {
            const CpuDebugSnapshot state = GetCpuDebugSnapshot();
            m_DebugEvents.RecordLegacyHit(category,
                write ? DebugEventAccess_Write : DebugEventAccess_Read,
                address, 0, false, state.programCounter, state.elapsedTStates);
            RequestCpuDebugPause();
            return; // Esci subito appena ne trovi uno
        }
    }
}

void Memory::CheckBreakpoints(u16 address, bool write) {
    if (m_LegacyMemoryBreakpointsEnabled)
        InternalBreakpointsCheck(DebugEventCategory::CpuMemory, address, write, m_BreakpointsMem);
}

void Memory::CheckBreakpoints(u16 address, bool write, u8 value, bool valueKnown)
{
    CheckBreakpoints(address, write, 0, false, value, valueKnown);
}

void Memory::CheckBreakpoints(u16 address, bool write, u8 beforeValue, bool beforeKnown, u8 afterValue, bool afterKnown)
{
    if (!m_DebugEvents.IsRuntimeActive())
        return;
    const CpuDebugSnapshot state = GetCpuDebugSnapshot();
    const u8 access = write ? DebugEventAccess_Write : DebugEventAccess_Read;
    const DebugProbe compatibility = m_DebugEvents.ResolveLegacyProbe(
        DebugEventCategory::CpuMemory, access, address);
    const DebugProbe primary = m_debugMemoryDevice != 0
        ? DebugProbe{m_debugMemoryDevice, DebugCommonSpace::Memory, address}
        : compatibility;
    const bool pause = m_DebugEvents.EvaluateMemoryAccess(
        primary, compatibility,
        access, beforeValue, beforeKnown, afterValue, afterKnown,
        state.programCounter, state.elapsedTStates);
    bool physicalPause = false;
    if (m_sgm && m_sgm->ResolvesRam(address))
    {
        const auto* device = m_DebugEvents.GetRegistry().FindDevice("event.coleco.sgm");
        if (device)
            physicalPause = m_DebugEvents.EvaluateProbe(
                {device->id, DebugCommonSpace::Memory, address}, access,
                beforeValue, beforeKnown, afterValue, afterKnown,
                DebugEventSource::CpuInstruction, DebugEventPhase::AfterCommitDeviceBoundary,
                state.programCounter, state.elapsedTStates);
    }
    if (m_bSF7000Enabled && m_debugSF7000MemoryDevice != 0)
    {
        const bool iplRead = !write && !m_bIPLRomDisabled &&
                             address < 0x4000;
        const DebugSpaceId space = iplRead
            ? SF7000MemoryDevice::IplSpace
            : SF7000MemoryDevice::RamSpace;
        const u32 physicalAddress = iplRead ? (address & 0x1FFF) : address;
        physicalPause = m_DebugEvents.EvaluateProbe(
            {m_debugSF7000MemoryDevice, space, physicalAddress},
            access, beforeValue, beforeKnown, afterValue, afterKnown,
            DebugEventSource::CpuInstruction,
            DebugEventPhase::AfterCommitDeviceBoundary,
            state.programCounter, state.elapsedTStates);
    }
    if (m_LegacyMemoryBreakpointsEnabled)
        InternalBreakpointsCheck(DebugEventCategory::CpuMemory, address, write, m_BreakpointsMem);
    if (pause || physicalPause)
        RequestCpuDebugPause();
}

void Memory::CheckVRAMAccess(u16 address, bool write, u8 value, bool valueKnown)
{
    CheckVRAMAccess(address, write, 0, false, value, valueKnown);
}

void Memory::CheckVRAMAccess(u16 address, bool write, u8 beforeValue, bool beforeKnown,
                             u8 afterValue, bool afterKnown,
                             const DebugEventRasterContext* raster)
{
#ifndef GEARSF7000_DISABLE_DISASSEMBLER
    if (write && afterKnown && m_pRomMap)
        m_pRomMap->OnVramWrite(static_cast<u16>(address & 0x3FFF), afterValue);
#endif
    const CpuDebugSnapshot state = GetCpuDebugSnapshot();
    const u8 access = write ? DebugEventAccess_Write : DebugEventAccess_Read;
    const DebugProbe probe{m_debugVideoDevice, DebugCommonSpace::Memory,
        static_cast<u32>(address & 0x3FFF)};
    const bool pause = m_debugVideoDevice != 0
        ? m_DebugEvents.EvaluateDeviceProbe(DebugEventCategory::Vram,
            probe, access,
            beforeValue, beforeKnown, afterValue, afterKnown,
            DebugEventSource::CpuInstruction,
            DebugEventPhase::AfterCommitDeviceBoundary,
            state.programCounter, state.elapsedTStates, raster)
        : m_DebugEvents.Evaluate(DebugEventCategory::Vram, access,
            address & 0x3FFF, beforeValue, beforeKnown,
            afterValue, afterKnown, state.programCounter,
            state.elapsedTStates, raster);
    if (pause)
        RequestCpuDebugPause();
}

void Memory::CheckVDPRegisterWrite(u8 reg, u8 beforeValue, u8 afterValue)
{
#ifndef GEARSF7000_DISABLE_DISASSEMBLER
    if (m_pRomMap)
        m_pRomMap->OnVdpLayoutChanged();
#endif
    const CpuDebugSnapshot state = GetCpuDebugSnapshot();
    const DebugProbe probe{m_debugVideoDevice, DebugCommonSpace::Registers, reg};
    const bool pause = m_debugVideoDevice != 0
        ? m_DebugEvents.EvaluateDeviceProbe(DebugEventCategory::VdpRegister,
            probe, DebugEventAccess_Write,
            beforeValue, true, afterValue, true,
            DebugEventSource::CpuInstruction,
            DebugEventPhase::AfterCommitDeviceBoundary,
            state.programCounter, state.elapsedTStates)
        : m_DebugEvents.Evaluate(DebugEventCategory::VdpRegister,
            DebugEventAccess_Write, reg, beforeValue, true,
            afterValue, true, state.programCounter, state.elapsedTStates);
    if (pause)
        RequestCpuDebugPause();
}

void Memory::CheckVDPStateChange(u16 stateTarget, u64 beforeValue, u64 afterValue,
                                 DebugEventSource source,
                                 const DebugEventRasterContext* raster)
{
    const CpuDebugSnapshot state = GetCpuDebugSnapshot();
    DebugProbe probe{m_debugVideoDevice, DebugCommonSpace::State, stateTarget};
    if (probe.device == 0)
    {
        probe = m_DebugEvents.ResolveLegacyProbe(
            DebugEventCategory::VdpRegister, DebugEventAccess_Event, stateTarget);
        probe.space = DebugCommonSpace::State;
    }
    const bool pause = m_DebugEvents.EvaluateDeviceProbe(
        DebugEventCategory::VdpRegister, probe, DebugEventAccess_Event,
        beforeValue, true, afterValue, true,
        source, DebugEventPhase::AfterCommitDeviceBoundary,
        state.programCounter, state.elapsedTStates, raster);
    if (pause)
        RequestCpuDebugPause();
}

void Memory::CheckVideoTimingEvent(u16 scanline, u8 eventFlags)
{
    const CpuDebugSnapshot state = GetCpuDebugSnapshot();
    const DebugProbe probe{m_debugVideoDevice, DebugCommonSpace::Raster,
        scanline};
    const bool pause = m_debugVideoDevice != 0
        ? m_DebugEvents.EvaluateDeviceProbe(DebugEventCategory::VideoTiming,
            probe, DebugEventAccess_Event,
            eventFlags, true, eventFlags, true,
            DebugEventSource::DeviceInternal,
            DebugEventPhase::AfterCommitDeviceBoundary,
            state.programCounter, state.elapsedTStates)
        : m_DebugEvents.Evaluate(DebugEventCategory::VideoTiming,
            DebugEventAccess_Event, scanline, eventFlags, true,
            state.programCounter, state.elapsedTStates);
    if (pause)
        RequestCpuDebugPause();
}

void Memory::CheckPPIStateChange(u16 port, u8 beforeValue, u8 afterValue)
{
    const CpuDebugSnapshot state = GetCpuDebugSnapshot();
    const bool pause = m_DebugEvents.Evaluate(DebugEventCategory::Ppi,
        DebugEventAccess_Event, port, beforeValue, true, afterValue, true,
        state.programCounter, state.elapsedTStates);
    if (pause)
        RequestCpuDebugPause();
}

void Memory::CheckFDCEvent(u16 eventType, u8 beforeValue, u8 afterValue)
{
    const CpuDebugSnapshot state = GetCpuDebugSnapshot();
    const bool pause = m_DebugEvents.Evaluate(DebugEventCategory::Fdc,
        DebugEventAccess_Event, eventType, beforeValue, true, afterValue, true,
        state.programCounter, state.elapsedTStates);
    if (pause)
        RequestCpuDebugPause();
}

void Memory::CheckAudioEvent(u16 reg, u8 value)
{
    const CpuDebugSnapshot state = GetCpuDebugSnapshot();
    const DebugProbe probe{m_debugPsgDevice, DebugCommonSpace::Registers, reg};
    const bool pause = m_debugPsgDevice != 0
        ? m_DebugEvents.EvaluateDeviceProbe(DebugEventCategory::Audio,
            probe, DebugEventAccess_Event, 0, false, value, true,
            DebugEventSource::CpuInstruction,
            DebugEventPhase::AfterCommitDeviceBoundary,
            state.programCounter, state.elapsedTStates)
        : m_DebugEvents.Evaluate(DebugEventCategory::Audio,
            DebugEventAccess_Event, reg, value, true,
            state.programCounter, state.elapsedTStates);
    if (pause)
        RequestCpuDebugPause();
}

void Memory::CheckAyEvent(u16 reg, u8 value)
{
    const CpuDebugSnapshot state = GetCpuDebugSnapshot();
    if (m_sgm && m_sgm->IsPresent())
    {
        const auto* device = m_DebugEvents.GetRegistry().FindDevice("event.coleco.sgm.ay");
        if (device && m_DebugEvents.EvaluateDeviceProbe(DebugEventCategory::AyExpansion,
            {device->id, DebugCommonSpace::Registers, reg}, DebugEventAccess_Event,
            0, false, value, true, DebugEventSource::CpuInstruction,
            DebugEventPhase::AfterCommitDeviceBoundary, state.programCounter, state.elapsedTStates))
            RequestCpuDebugPause();
        return;
    }
    const bool pause = m_DebugEvents.Evaluate(DebugEventCategory::AyExpansion,
        DebugEventAccess_Event, reg, value, true,
        state.programCounter, state.elapsedTStates);
    if (pause)
        RequestCpuDebugPause();
}

void Memory::CheckTapeEvent(u16 eventType, u8 value)
{
    const CpuDebugSnapshot state = GetCpuDebugSnapshot();
    const bool pause = m_DebugEvents.Evaluate(DebugEventCategory::Tape,
        DebugEventAccess_Event, eventType, value, true,
        state.programCounter, state.elapsedTStates);
    if (pause)
        RequestCpuDebugPause();
}

void Memory::CheckDeviceStateChange(const DebugProbe& probe,
                                    u64 beforeValue, u64 afterValue)
{
    const CpuDebugSnapshot state = GetCpuDebugSnapshot();
    const bool pause = m_DebugEvents.EvaluateProbe(
        probe, DebugEventAccess_Event,
        beforeValue, true, afterValue, true,
        DebugEventSource::DeviceInternal,
        DebugEventPhase::AfterCommitDeviceBoundary,
        state.programCounter, state.elapsedTStates);
    if (pause)
        RequestCpuDebugPause();
}

void Memory::CheckDeviceAccess(const DebugProbe& probe, u8 access,
                               u64 beforeValue, bool beforeKnown,
                               u64 afterValue, bool afterKnown)
{
    const CpuDebugSnapshot state = GetCpuDebugSnapshot();
    const bool pause = m_DebugEvents.EvaluateProbe(
        probe, access, beforeValue, beforeKnown, afterValue, afterKnown,
        DebugEventSource::CpuInstruction,
        DebugEventPhase::AfterCommitDeviceBoundary,
        state.programCounter, state.elapsedTStates);
    if (pause)
        RequestCpuDebugPause();
}

//void Memory::CheckBreakpoints(u16 address, bool write)
//{
//    size_t size = m_BreakpointsMem.size();
//
//    for (size_t b = 0; b < size; b++)
//    {
//        if (write && !m_BreakpointsMem[b].write)
//            continue;
//
//        if (!write && !m_BreakpointsMem[b].read)
//            continue;
//
//        bool proceed = false;
//
//        if (m_BreakpointsMem[b].range)
//        {
//            if ((address >= m_BreakpointsMem[b].address1) && (address <= m_BreakpointsMem[b].address2))
//            {
//                proceed = true;
//            }
//        }
//        else
//        {
//            if (m_BreakpointsMem[b].address1 == address)
//            {
//                proceed = true;
//            }
//        }
//
//        if (proceed)
//        {
//            RequestCpuDebugPause();
//            break;
//        }
//    }
//}

void Memory::ResetRomDisassembledMemory()
{
    #ifndef GEARSF7000_DISABLE_DISASSEMBLER

    m_BreakpointsCPU.clear();
    for (auto& [data, records] : m_additionalRomMaps)
        for (auto* record : records) delete record;
    m_additionalRomMaps.clear();

    if (IsValidPointer(m_pDisassembledRomMap))
    {
        for (int i = 0; i < MAX_ROM_SIZE; i++)
        {
            SafeDelete(m_pDisassembledRomMap[i]);
        }
    }

    if (IsValidPointer(m_pDisassembledInternalRamMap))
    {
        for (int i = 0; i < RamCapacity; i++)
        {
            SafeDelete(m_pDisassembledInternalRamMap[i]);
        }
    }

    if (IsValidPointer(m_pDisassembledBiosMap))
    {
        for (u32 i = 0; i < m_biosSize; i++)
        {
            SafeDelete(m_pDisassembledBiosMap[i]);
        }
    }

    if (IsValidPointer(m_pDisassembledExternalRamMap))
    {
        for (int i = 0; i < 0x10000; i++)
        {
            SafeDelete(m_pDisassembledExternalRamMap[i]);
        }
    }

    #endif
}

#ifndef GEARSF7000_DISABLE_DISASSEMBLER
// Where a CPU address lands right now: which disassembly map (BIOS, RAM,
// SRAM, ROM), at what offset in it, with the segment code and ROM bank. The
// one place that knows every mapper; the disassembler and the ROM map both
// go through it.
void Memory::ResolveLocation(u16 address, stDisassembleRecord**& map,
                             int& offset, int& segment, int& bank)
{
    map = NULL;
    offset = address;
    segment = 0;
    bank = -1;

    if (m_m5Mapper) {
        const auto location = m_m5Mapper->Resolve(address);
        offset = location.offset;
        switch (location.area) {
        case M5MemoryMapper::Area::Bios: map = m_pDisassembledBiosMap; segment = 5; break;
        case M5MemoryMapper::Area::Cartridge: map = m_pDisassembledRomMap; segment = 3; break;
        case M5MemoryMapper::Area::InternalRam: map = m_pDisassembledInternalRamMap; segment = 2; break;
        case M5MemoryMapper::Area::ExternalRam: map = m_pDisassembledExternalRamMap; segment = 4; break;
        default: break;
        }
    }
    else if (m_msxSlots)
    {
        const auto location = m_msxSlots->Resolve(address);
        offset = location.offset;
        if (location.data == m_pBios && location.data) { map = m_pDisassembledBiosMap; segment = 5; }
        else if (location.data == m_pInternalRam) { map = m_pDisassembledInternalRamMap; segment = 2; }
        else if (location.data == m_pCartridge->GetROM() && location.data) { map = m_pDisassembledRomMap; segment = 3; }
        else if (location.data && location.size && !location.writable) {
            auto& records = m_additionalRomMaps[location.data];
            if (records.empty()) records.resize(location.size, nullptr);
            map = records.data(); segment = 5;
        }
    }
    else if (m_sgm && m_sgm->ResolvesRam(address))
    {
        map = m_pDisassembledExternalRamMap;
        offset = address;
        segment = 1;
    }
    else if (m_bSF7000Enabled)
    {
        if (address < 0x4000)
        {
            offset = m_bIPLRomDisabled ? address : address & GetBiosMask();
            map = m_bIPLRomDisabled ? m_pDisassembledExternalRamMap : m_pDisassembledBiosMap;
            segment = m_bIPLRomDisabled ? 1 : 0;
        }
        else
        {
            offset = address;
            map = m_pDisassembledExternalRamMap;
            segment = 1;
        }
    }
    else if (address >= 0xC000 && UsesSMSWorkRam())
    {
        map = m_pDisassembledInternalRamMap;
        offset = BoardRamOffset(address);
        segment = 2;
    }
    else if (m_pCartridge->GetType() == Cartridge::SMS_SEGA_MAPPER)
    {
        if (address >= 0xC000)
        {
            map = m_pDisassembledInternalRamMap;
            offset = BoardRamOffset(address);
            segment = 2;
        }
        else if (address >= 0x8000 &&
                 (m_pMapper->GetBankReg(0) & 0x08) != 0)
        {
            map = m_pDisassembledExternalRamMap;
            offset = ((m_pMapper->GetBankReg(0) >> 2) & 1) * 0x4000
                   + (address & 0x3FFF);
            segment = 6; // Cartridge SRAM, distinct from system work RAM
        }
        else
        {
            const int frame = address >> 14;
            const int bankCount = m_pCartridge->GetROMBankCount();
            int selected = m_pMapper->GetBankReg(frame + 1);
            if (address < 0x0400)
                selected = 0;
            if (bankCount > 0)
                selected %= bankCount;
            map = m_pDisassembledRomMap;
            offset = selected * 0x4000 + (address & 0x3FFF);
            bank = selected;
            segment = 4;
        }
    }
    else if (m_colecoMotherboard)
    {
        // Same decode as Memory::Read. The BIOS has its own map so it never
        // lands in the cartridge's ROM map (the live disassembler's project
        // is the cartridge); the cartridge's physical offset starts at $8000.
        switch (address & 0xE000)
        {
        case 0x0000:
            map = m_pDisassembledBiosMap;
            offset = address;
            segment = 5;
            break;
        case 0x6000:
            map = m_pDisassembledInternalRamMap;
            offset = BoardRamOffset(address);
            segment = 2;
            break;
        case 0x2000:
        case 0x4000:
            map = NULL;
            break;
        default:
            offset = address - 0x8000;
            if (m_pCartridge->GetType() == Cartridge::COLECO_MEGACART && m_pMapper)
            {
                offset = static_cast<ColecoMegaCartMapper*>(m_pMapper)->PhysicalOffset(address);
                bank = offset >> 14;
            }
            map = offset < m_pCartridge->GetROMSize() ? m_pDisassembledRomMap : NULL;
            segment = 3;
            break;
        }
    }
    else if (m_pCartridge->GetType() == Cartridge::SG1000_CASTLE_8K &&
             address >= 0x8000)
    {
        if (address >= 0xC000)
        {
            map = m_pDisassembledInternalRamMap;
            offset = BoardRamOffset(address);
            segment = 2;
        }
        else
        {
            map = m_pDisassembledExternalRamMap;
            offset = address & 0x1FFF;
            segment = 1;
        }
    }
    else if (m_pCartridge->GetType() == Cartridge::SG1000_OTHELLO_2K)
    {
        if (address >= 0x8000 && address < 0xA000)
        {
            map = m_pDisassembledExternalRamMap;
            offset = address & 0x07FF;
            segment = 1;
        }
        else if (address >= 0xC000)
        {
            map = m_pDisassembledInternalRamMap;
            offset = BoardRamOffset(address);
            segment = 2;
        }
        else
        {
            const int romSize = m_pCartridge->GetROMSize();
            map = m_pDisassembledRomMap;
            offset = romSize > 0 ? address % romSize : 0;
            segment = 3;
        }
    }
    else if (m_pCartridge->GetType() == Cartridge::SC3000_SURVIVORS_MULTICART
          || m_pCartridge->GetType() == Cartridge::SC3000_SURVIVORS_MEGACART)
    {
        if (address < 0x8000)
        {
            map = m_pDisassembledRomMap;
            offset = static_cast<int>(m_pMapper->GetRomBankAddress()) + address;
            bank = m_pMapper->GetRomBank();
            segment = 4;
        }
        else
        {
            map = m_pDisassembledExternalRamMap;
            offset = address & 0x7FFF;
            segment = 1;
        }
    }
    else if (Cartridge::UsesSega3155208(m_pCartridge->GetType()))
    {
        if (address < 0xC000)
        {
            map = m_pDisassembledRomMap;
            if (address < 0x8000)
            {
                offset = address;
                bank = address >> 14;
            }
            else
            {
                bank = m_pMapper->GetRomBank();
                offset = (bank << 14) + (address & 0x3FFF);
            }
            segment = 4;
        }
        else
        {
            if (m_pCartridge->GetType() == Cartridge::SG1000_SEGA_315_5208_DAHJEE_B)
            {
                map = m_pDisassembledExternalRamMap;
                offset = address & 0x1FFF;
                segment = 1;
            }
            else
            {
                map = m_pDisassembledInternalRamMap;
                offset = BoardRamOffset(address);
                segment = 2;
            }
        }
    }
    else
    {
        switch (address & 0xE000)
        {
        case 0x0000:
        case 0x4000:
        case 0x6000:
        {
            const int romSize = m_pCartridge->GetROMSize();
            offset = (m_pCartridge->GetType() == Cartridge::SG1000_DAHJEE_TYPE_A
                   || m_pCartridge->GetType() == Cartridge::SG1000_DAHJEE_TYPE_B)
                && romSize > 0 ? address % romSize : address;
            map = m_pDisassembledRomMap;
            segment = 3; // ROM standard
            break;
        }
        case 0x2000:
        {
            if (m_pCartridge->GetType() == Cartridge::SG1000_DAHJEE_TYPE_A)
            {
                offset = address & 0x1FFF;
                map = m_pDisassembledExternalRamMap;
                segment = 1;
            }
            else
            {
                const int romSize = m_pCartridge->GetROMSize();
                offset = m_pCartridge->GetType() == Cartridge::SG1000_DAHJEE_TYPE_B
                    && romSize > 0 ? address % romSize : address;
                map = m_pDisassembledRomMap;
                segment = 3;
            }
            break;
        }
        case 0x8000:
        case 0xA000:
        {
            map = m_pDisassembledRomMap;
            // Se il mapper ASC16L è attivo, calcoliamo l'offset reale e il banco
            if (IsValidPointer(m_pMapper)
                && (m_pCartridge->GetType() == Cartridge::SC3000_ASC16L
                    || m_pCartridge->GetType() == Cartridge::SC3000_ASC16LS))
            {
                offset = m_pMapper->GetRomBankAddress() + (address & 0x3FFF);
                bank = m_pMapper->GetRomBank();
                segment = 4; // MROM (Visualizza il banco nel debugger)
            }
            else
            {
                const int romSize = m_pCartridge->GetROMSize();
                offset = (m_pCartridge->GetType() == Cartridge::SG1000_DAHJEE_TYPE_A
                       || m_pCartridge->GetType() == Cartridge::SG1000_DAHJEE_TYPE_B)
                    && romSize > 0 ? address % romSize : address;
                segment = 3; // ROM standard
            }
            break;
        }
        default:
        {
            if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SG1000_1K)
            {
                offset = BoardRamOffset(address);
                map = m_pDisassembledInternalRamMap;
                segment = 2;
            }
            else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SC3000_2K
                  || m_pCartridge->GetType() == Cartridge::CartridgeTypes::SC3000_16K_AT_8000)
            {
                offset = BoardRamOffset(address);
                map = m_pDisassembledInternalRamMap;
                segment = 2;
            }
            else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SG1000_16K)
            {
                offset = address & 0x3FFF;
                map = m_pDisassembledExternalRamMap;
                segment = 1;
            }
            else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SC3000_ASC16L
                  || m_pCartridge->GetType() == Cartridge::CartridgeTypes::SC3000_ASC16LS)
            {
                offset = 0x4000 + (address & (m_pCartridge->GetASC16LRamBytes() - 1));
                map = m_pDisassembledExternalRamMap;
                segment = 1;
            }
            else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SG1000_DAHJEE_TYPE_A)
            {
                offset = BoardRamOffset(address);
                map = m_pDisassembledInternalRamMap;
                segment = 2;
            }
            else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SG1000_DAHJEE_TYPE_B)
            {
                offset = address & 0x1FFF;
                map = m_pDisassembledExternalRamMap;
                segment = 1;
            }
            else
            {
                offset = address & 0xFFFF;
                map = m_pDisassembledExternalRamMap;
                segment = 1;
            }
            break;
        }
        }
    }

}

bool Memory::TranslateToRomOffset(u16 address, u32& romOffset)
{
    stDisassembleRecord** map = NULL;
    int offset = 0, segment = 0, bank = -1;
    ResolveLocation(address, map, offset, segment, bank);
    if (map != m_pDisassembledRomMap || offset < 0)
        return false;
    romOffset = static_cast<u32>(offset);
    return true;
}

bool Memory::AnswersAt(u16 address)
{
    stDisassembleRecord** map = NULL;
    int offset = 0, segment = 0, bank = -1;
    ResolveLocation(address, map, offset, segment, bank);
    if (map == NULL || offset < 0)
        return false;
    if (map == m_pDisassembledRomMap)
        return offset < m_pCartridge->GetROMSize();
    return true;
}

void Memory::ForwardCpuDataRead(u16 address, u8 value)
{
    m_pRomMap->OnCpuDataRead(address, value);
}

void Memory::ForwardCpuPortWrite(u8 port, u8 value)
{
    m_pRomMap->OnCpuPortWrite(port, value);
}
#else
bool Memory::TranslateToRomOffset(u16, u32&)
{
    return false;
}

bool Memory::AnswersAt(u16)
{
    return true;
}

void Memory::ForwardCpuDataRead(u16, u8)
{
}

void Memory::ForwardCpuPortWrite(u8, u8)
{
}
#endif

Memory::stDisassembleRecord* Memory::GetDisassembleRecord(u16 address, bool createIfNotFound)
{
#ifndef GEARSF7000_DISABLE_DISASSEMBLER

    stDisassembleRecord** map = NULL;
    int offset = address;
    int segment = 0;
    int bank = -1;
    ResolveLocation(address, map, offset, segment, bank);

    // Nothing answers at this address on this board (an expansion slot, a
    // cartridge select past the image): no record can exist there.
    if (!IsValidPointer(map))
        return NULL;

    if (!IsValidPointer(map[offset]) && createIfNotFound)
    {
        map[offset] = new Memory::stDisassembleRecord;

        map[offset]->address = address;
        map[offset]->bank = bank;
        map[offset]->name[0] = 0;
        map[offset]->bytes[0] = 0;
        map[offset]->size = 0;
        for (int i = 0; i < 7; i++)
            map[offset]->opcodes[i] = 0;
        map[offset]->jump = false;
        map[offset]->jump_address = 0;
        map[offset]->subroutine = false;
        map[offset]->auto_symbol[0] = 0;
        map[offset]->physical_address = static_cast<u32>(offset);
        map[offset]->execution_count = 0;
        map[offset]->first_execution_tstate = 0;
        map[offset]->last_execution_tstate = 0;

        switch (segment)
        {
            case 0: strcpy(map[offset]->segment, "IPL "); break;
            case 1: strcpy(map[offset]->segment, "RAM "); break;
            case 2: strcpy(map[offset]->segment, "RAM "); break;
            case 3: strcpy(map[offset]->segment, "ROM "); break;
            case 4: strcpy(map[offset]->segment, "ROM "); break; // Bank remains a separate field
            case 5: strcpy(map[offset]->segment, "BIOS"); break; // ColecoVision console ROM
            case 6: strcpy(map[offset]->segment, "SRAM"); break; // Cartridge save RAM
        }
    }

    return map[offset];

#else
    return NULL;
#endif
}

std::vector<Memory::CodeCoverageRecord> Memory::GetCodeCoverage() const
{
    std::vector<CodeCoverageRecord> result;
#ifndef GEARSF7000_DISABLE_DISASSEMBLER
    const auto append = [&](stDisassembleRecord* const* map, std::size_t size)
    {
        if (!IsValidPointer(map))
            return;
        for (std::size_t i = 0; i < size; ++i)
        {
            const stDisassembleRecord* record = map[i];
            if (!IsValidPointer(record) || record->execution_count == 0)
                continue;
            result.push_back({record->address, record->physical_address,
                              record->bank, record->segment, record->name,
                              record->bytes, record->size,
                              record->execution_count,
                              record->first_execution_tstate,
                              record->last_execution_tstate});
        }
    };
    append(m_pDisassembledBiosMap, m_biosSize);
    append(m_pDisassembledInternalRamMap, RamCapacity);
    append(m_pDisassembledExternalRamMap, 0x10000);
    append(m_pDisassembledRomMap, MAX_ROM_SIZE);
    for (const auto& [data, records] : m_additionalRomMaps)
        append(records.data(), records.size());
#endif
    return result;
}

void Memory::ClearCodeCoverage()
{
#ifndef GEARSF7000_DISABLE_DISASSEMBLER
    const auto clear = [](stDisassembleRecord** map, std::size_t size)
    {
        if (!IsValidPointer(map))
            return;
        for (std::size_t i = 0; i < size; ++i)
        {
            stDisassembleRecord* record = map[i];
            if (!IsValidPointer(record))
                continue;
            record->execution_count = 0;
            record->first_execution_tstate = 0;
            record->last_execution_tstate = 0;
        }
    };
    clear(m_pDisassembledBiosMap, m_biosSize);
    clear(m_pDisassembledInternalRamMap, RamCapacity);
    clear(m_pDisassembledExternalRamMap, 0x10000);
    clear(m_pDisassembledRomMap, MAX_ROM_SIZE);
    for (auto& [data, records] : m_additionalRomMaps) clear(records.data(), records.size());
#endif
}
