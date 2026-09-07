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
#include <sstream>
#include "SaveStateStream.h"

namespace
{
constexpr std::uint16_t kMemoryStateVersion = 1;
}
#include "Cartridge.h"
#include "ASC16LMapper.h"



Memory::Memory(Cartridge* pCartridge)
{
    m_pCartridge = pCartridge;
    m_pMapper = NULL; // Inizializzazione di sicurezza
    m_LegacyMemoryBreakpointsEnabled = true;
    m_LegacyVRAMBreakpointsEnabled = true;
    m_LegacyCpuBreakpointsEnabled = true;
    InitPointer(m_cpuDebugContext);
    InitPointer(m_pDisassembledRomMap);
    InitPointer(m_pDisassembledRamMap);
    InitPointer(m_pDisassembledBiosMap);
    InitPointer(m_pDisassembledSGMRamMap);
    m_runToAddressActive = false;
    m_runToAddress = 0;
    InitPointer(m_pBios);
    InitPointer(m_pRam);
    InitPointer(m_pSGMRam);
    m_bBiosLoaded = false;
    m_bSF7000Enabled = false;
    m_bSGMUpper = false;
    m_bIPLRomDisabled = false;
    m_RomBankAddress = 0;
    m_RomBank = 0;
}



Memory::~Memory()
{
    SafeDeleteArray(m_pBios);
    SafeDeleteArray(m_pRam);
    SafeDeleteArray(m_pSGMRam);
    SafeDelete(m_pMapper);

    if (IsValidPointer(m_pDisassembledRomMap))
    {
        for (int i = 0; i < MAX_ROM_SIZE; i++)
        {
            SafeDelete(m_pDisassembledRomMap[i]);
        }
        SafeDeleteArray(m_pDisassembledRomMap);
    }

    if (IsValidPointer(m_pDisassembledRamMap))
    {
        for (int i = 0; i < MAX_SRAM_SIZE; i++)
        {
            SafeDelete(m_pDisassembledRamMap[i]);
        }
        SafeDeleteArray(m_pDisassembledRamMap);
    }

    if (IsValidPointer(m_pDisassembledBiosMap))
    {
        for (int i = 0; i < 0x2000; i++)
        {
            SafeDelete(m_pDisassembledBiosMap[i]);
        }
        SafeDeleteArray(m_pDisassembledBiosMap);
    }

    if (IsValidPointer(m_pDisassembledSGMRamMap))
    {
        for (int i = 0; i < 0x10000; i++)
        {
            SafeDelete(m_pDisassembledSGMRamMap[i]);
        }
        SafeDeleteArray(m_pDisassembledSGMRamMap);
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
    m_pRam = new u8[MAX_SRAM_SIZE];
    m_pBios = new u8[0x2000];
    m_pSGMRam = new u8[0x10000];

#ifndef GEARSF7000_DISABLE_DISASSEMBLER
    m_pDisassembledRomMap = new stDisassembleRecord*[MAX_ROM_SIZE];
    for (int i = 0; i < MAX_ROM_SIZE; i++)
    {
        InitPointer(m_pDisassembledRomMap[i]);
    }

    m_pDisassembledRamMap = new stDisassembleRecord*[MAX_SRAM_SIZE];
    for (int i = 0; i < MAX_SRAM_SIZE; i++)
    {
        InitPointer(m_pDisassembledRamMap[i]);
    }

    m_pDisassembledBiosMap = new stDisassembleRecord*[0x2000];
    for (int i = 0; i < 0x2000; i++)
    {
        InitPointer(m_pDisassembledBiosMap[i]);
    }

    m_pDisassembledSGMRamMap = new stDisassembleRecord*[0x10000];
    for (int i = 0; i < 0x10000; i++)
    {
        InitPointer(m_pDisassembledSGMRamMap[i]);
    }
#endif

    m_BreakpointsCPU.clear();
    m_BreakpointsMem.clear();
    m_BreakpointsVRAM.clear();

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
    m_bIPLRomDisabled = false;
    m_RomBank = 0;
    m_RomBankAddress = 0x0000;

    for (int i = 0; i < MAX_SRAM_SIZE; i++)
    {
        m_pRam[i] = rand() % 256;
    }

    for (int i = 0; i < 0x10000; i++)
    {
        //m_pSGMRam[i] = rand() % 256;
        m_pSGMRam[i] = 0;
    }

    // Flat map: the image is poured into the 64 KB and everything is RAM from
    // there on, exactly as on the SF-7000 with its IPL switched off. The copy
    // lives here rather than in SetupMapper() because SetupMapper() runs first
    // and Reset() then zeroes m_pSGMRam, which would wipe a freshly written
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
                m_pSGMRam[i] = pRom[i];
            Log("Flat 64K: loaded %d KB of image into RAM", copySize / 1024);
        }
    }

    //if (m_pCartridge->IsPAL())
    //    m_pBios[0x69] = 0x32;
    //else
    //    m_pBios[0x69] = 0x3C;
}

u32 Memory::GetMapperId() const
{
    return IsValidPointer(m_pMapper) ? m_pMapper->GetMapperId() : 0u;
}

void Memory::SaveState(std::ostream& stream)
{
    StateWriter w(stream);

    w.U16(kMemoryStateVersion);

    w.Bytes(m_pRam, MAX_SRAM_SIZE);
    w.Bytes(m_pSGMRam, 0x10000);
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

void Memory::LoadState(std::istream& stream)
{
    StateReader r(stream);

    if (r.U16() != kMemoryStateVersion)
        return;

    r.Bytes(m_pRam, MAX_SRAM_SIZE);
    r.Bytes(m_pSGMRam, 0x10000);
    m_bSGMUpper = r.Bool();
    m_bIPLRomDisabled = r.Bool();

    m_bBiosLoaded = r.Bool();
    m_bSF7000Enabled = r.Bool();

    m_RomBankAddress = r.U32();
    m_RomBank = r.U8();

    const bool hasMapper = r.Bool();
    if (!hasMapper)
        return;

    const std::uint32_t mapperId = r.U32();
    const std::uint16_t mapperVersion = r.U16();
    const std::uint32_t payloadSize = r.U32();
    if (!r.Ok())
        return;

    const bool matches = IsValidPointer(m_pMapper) &&
                         m_pMapper->GetMapperId() == mapperId &&
                         m_pMapper->GetStateVersion() == mapperVersion;

    if (!matches)
    {
        // Wrong mapper for this state. Stepping over the payload keeps the
        // rest of the stream readable; applying it would put a bank register
        // somewhere it does not belong.
        Log("Save state carries mapper 0x%08X v%u, which is not the one "
            "installed. Its %u bytes were skipped.",
            mapperId, static_cast<unsigned>(mapperVersion), payloadSize);
        r.Skip(payloadSize);
        return;
    }

    std::string bytes;
    bytes.resize(payloadSize);
    if (payloadSize > 0)
        r.Bytes(&bytes[0], payloadSize);
    if (!r.Ok())
        return;

    std::stringstream payload(bytes, std::ios::in | std::ios::binary);
    m_pMapper->LoadState(payload);
}

std::vector<Memory::stDisassembleRecord*>* Memory::GetBreakpointsCPU()
{
    return &m_BreakpointsCPU;
}

std::vector<Memory::stMemoryBreakpoint>* Memory::GetBreakpointsMem()
{
    return &m_BreakpointsMem;
}

std::vector<Memory::stMemoryBreakpoint>* Memory::GetBreakpointsVRAM()
{
    return &m_BreakpointsVRAM;
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

void Memory::LoadBios(const char* szFilePath)
{
    using namespace std;

    m_bBiosLoaded = false;

    ifstream file(szFilePath, ios::in | ios::binary | ios::ate);

    if (file.is_open())
    {
        int size = static_cast<int> (file.tellg());

        if (size != 0x2000)
        {
            Log("Incorrect BIOS size %d: %s", size, szFilePath);
            return;
        }

        file.seekg(0, ios::beg);
        file.read(reinterpret_cast<char*>(m_pBios), size);
        file.close();

        m_bBiosLoaded = true;

        Log("BIOS %s loaded (%d bytes)", szFilePath, size);
    }
    else
    {
        Log("There was a problem opening the file %s", szFilePath);
    }
}

u8* Memory::GetRam()
{
    return m_pRam;
}

u8* Memory::GetSGMRam()
{
    return m_pSGMRam;
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

bool Memory::IsBiosLoaded()
{
    return m_bBiosLoaded;
}

void Memory::EnableSGMUpper(bool enable)
{
    m_bSGMUpper = enable;
}

void Memory::SetIPLRomDisabled(bool disabled)
{
    m_bIPLRomDisabled = disabled;
}

void Memory::EnableSF7000(bool enable)
{
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
    default:
        m_pMapper = NULL; // new StandardMapper(m_pCartridge);
        break;
    }

    if (IsValidPointer(m_pMapper))
    {
        m_pMapper->Reset();
    }
}

// In Memory.h (privato)
void Memory::InternalBreakpointsCheck(DebugEventCategory category, u16 address, bool write,
                                      const std::vector<stMemoryBreakpoint>& breakpoints)
{
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
    const CpuDebugSnapshot state = GetCpuDebugSnapshot();
    const bool pause = m_DebugEvents.Evaluate(DebugEventCategory::CpuMemory,
        write ? DebugEventAccess_Write : DebugEventAccess_Read,
        address, beforeValue, beforeKnown, afterValue, afterKnown, state.programCounter, state.elapsedTStates);
    if (m_LegacyMemoryBreakpointsEnabled)
        InternalBreakpointsCheck(DebugEventCategory::CpuMemory, address, write, m_BreakpointsMem);
    if (pause)
        RequestCpuDebugPause();
}

void Memory::CheckVRAMBreakpoints(u16 address, bool write, u8 value, bool valueKnown)
{
    CheckVRAMBreakpoints(address, write, 0, false, value, valueKnown);
}

void Memory::CheckVRAMBreakpoints(u16 address, bool write, u8 beforeValue, bool beforeKnown, u8 afterValue, bool afterKnown,
                                  const DebugEventRasterContext* raster)
{
    const CpuDebugSnapshot state = GetCpuDebugSnapshot();
    const bool pause = m_DebugEvents.Evaluate(DebugEventCategory::Vram,
        write ? DebugEventAccess_Write : DebugEventAccess_Read,
        address & 0x3FFF, beforeValue, beforeKnown, afterValue, afterKnown,
        state.programCounter, state.elapsedTStates, raster);
    if (m_LegacyVRAMBreakpointsEnabled)
        InternalBreakpointsCheck(DebugEventCategory::Vram, address & 0x3FFF, write, m_BreakpointsVRAM);
    if (pause)
        RequestCpuDebugPause();
}

void Memory::CheckVDPRegisterWrite(u8 reg, u8 beforeValue, u8 afterValue)
{
    const CpuDebugSnapshot state = GetCpuDebugSnapshot();
    const bool pause = m_DebugEvents.Evaluate(DebugEventCategory::VdpRegister,
        // The TMS9918 caller currently emits only R0-R7.  Keep the common
        // hook 8-bit clean so future devices such as Pico9918 can publish
        // their complete register number instead of aliasing it modulo 8.
        DebugEventAccess_Write, reg, beforeValue, true, afterValue, true,
        state.programCounter, state.elapsedTStates);
    if (pause)
        RequestCpuDebugPause();
}

void Memory::CheckVDPStateChange(u16 stateTarget, u64 beforeValue, u64 afterValue,
                                 DebugEventSource source,
                                 const DebugEventRasterContext* raster)
{
    const CpuDebugSnapshot state = GetCpuDebugSnapshot();
    const bool pause = m_DebugEvents.EvaluateProbe(GetGearVdpStateProbe(stateTarget),
        DebugEventAccess_Event, beforeValue, true, afterValue, true,
        source, DebugEventPhase::AfterCommitDeviceBoundary,
        state.programCounter, state.elapsedTStates, raster);
    if (pause)
        RequestCpuDebugPause();
}

void Memory::CheckVideoTimingEvent(u16 scanline, u8 eventFlags)
{
    const CpuDebugSnapshot state = GetCpuDebugSnapshot();
    const bool pause = m_DebugEvents.Evaluate(DebugEventCategory::VideoTiming,
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
    const bool pause = m_DebugEvents.Evaluate(DebugEventCategory::Audio,
        DebugEventAccess_Event, reg, value, true,
        state.programCounter, state.elapsedTStates);
    if (pause)
        RequestCpuDebugPause();
}

void Memory::CheckAyEvent(u16 reg, u8 value)
{
    const CpuDebugSnapshot state = GetCpuDebugSnapshot();
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

DebugEventManager* Memory::GetDebugEventManager()
{
    return &m_DebugEvents;
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

    if (IsValidPointer(m_pDisassembledRomMap))
    {
        for (int i = 0; i < MAX_ROM_SIZE; i++)
        {
            SafeDelete(m_pDisassembledRomMap[i]);
        }
    }

    if (IsValidPointer(m_pDisassembledRamMap))
    {
        for (int i = 0; i < MAX_SRAM_SIZE; i++)
        {
            SafeDelete(m_pDisassembledRamMap[i]);
        }
    }

    if (IsValidPointer(m_pDisassembledBiosMap))
    {
        for (int i = 0; i < 0x2000; i++)
        {
            SafeDelete(m_pDisassembledBiosMap[i]);
        }
    }

    if (IsValidPointer(m_pDisassembledSGMRamMap))
    {
        for (int i = 0; i < 0x10000; i++)
        {
            SafeDelete(m_pDisassembledSGMRamMap[i]);
        }
    }

    #endif
}

Memory::stDisassembleRecord* Memory::GetDisassembleRecord(u16 address, bool createIfNotFound)
{
#ifndef GEARSF7000_DISABLE_DISASSEMBLER

    stDisassembleRecord** map = NULL;
    int offset = address;
    int segment = 0;
    int bank = -1;

    if (m_bSF7000Enabled)
    {
        if (address < 0x4000)
        {
            offset = address & 0x1FFF;
            map = m_bIPLRomDisabled ? m_pDisassembledSGMRamMap : m_pDisassembledBiosMap;
            segment = m_bIPLRomDisabled ? 1 : 0;
        }
        else
        {
            offset = address;
            map = m_pDisassembledSGMRamMap;
            segment = 1;
        }
    }
    else
    {
        switch (address & 0xE000)
        {
        case 0x0000:
        case 0x2000:
        case 0x4000:
        case 0x6000:
        {
            offset = address;
            map = m_pDisassembledRomMap;
            segment = 3; // ROM standard
            break;
        }
        case 0x8000:
        case 0xA000:
        {
            map = m_pDisassembledRomMap;
            // Se il mapper ASC16L è attivo, calcoliamo l'offset reale e il banco
            if (IsValidPointer(m_pMapper) && m_pCartridge->GetType() == Cartridge::SC3000_ASC16L)
            {
                offset = m_pMapper->GetRomBankAddress() + (address & 0x3FFF);
                bank = m_pMapper->GetRomBank();
                segment = 4; // MROM (Visualizza il banco nel debugger)
            }
            else
            {
                offset = address;
                segment = 3; // ROM standard
            }
            break;
        }
        default:
        {
            if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SG1000_1K)
            {
                offset = address & 0x3FF;
                map = m_pDisassembledRamMap;
                segment = 2;
            }
            else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SC3000_2K)
            {
                offset = address & 0x7FF;
                map = m_pDisassembledRamMap;
                segment = 2;
            }
            else if (m_pCartridge->GetType() == Cartridge::CartridgeTypes::SG1000_16K)
            {
                offset = address & 0x3FFF;
                map = m_pDisassembledSGMRamMap;
                segment = 1;
            }
            else
            {
                offset = address & 0xFFFF;
                map = m_pDisassembledSGMRamMap;
                segment = 1;
            }
            break;
        }
        }
    }

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
            case 2: strcpy(map[offset]->segment, "SRA "); break;
            case 3: strcpy(map[offset]->segment, "ROM "); break;
            case 4: strcpy(map[offset]->segment, "MAP "); break; // Mapper ROM
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
    const auto append = [&](stDisassembleRecord** map, std::size_t size)
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
    append(m_pDisassembledBiosMap, 0x2000);
    append(m_pDisassembledRamMap, MAX_SRAM_SIZE);
    append(m_pDisassembledSGMRamMap, 0x10000);
    append(m_pDisassembledRomMap, MAX_ROM_SIZE);
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
    clear(m_pDisassembledBiosMap, 0x2000);
    clear(m_pDisassembledRamMap, MAX_SRAM_SIZE);
    clear(m_pDisassembledSGMRamMap, 0x10000);
    clear(m_pDisassembledRomMap, MAX_ROM_SIZE);
#endif
}
