/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2021  Ignacio Sanchez
 * Copyright (C) 2024-2026  Saverio Russo
 *
 * Derived from Gearcoleco by Ignacio Sanchez. His code is free software: you
 * can redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * The changes and additions by Saverio Russo are free software: you can
 * redistribute them and/or modify them under the terms of the GNU Affero
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 * Section 13 of both licences permits the combination.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License and the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of both licences along with this
 * program.  If not, see http://www.gnu.org/licenses/
 *
 * SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later
 */

#ifndef MEMORY_H
#define	MEMORY_H

#include "definitions.h"
#include "MSXSlotMapper.h"
#include "Mapper.h"
#include "SGMDevice.h"
#include "Breakpoints.h"
#include "DebugEvents.h"
#include "cpu/CpuDebugContext.h"
#include "cpu/CpuInstructionObserver.h"
#include "IDebugEventProvider.h"
#include <vector>
#include <map>

class Cartridge;
class Mapper;
class RomMap;

class Memory : public CpuInstructionObserver, public IDebugEventProvider
{
public:
    struct stDisassembleRecord
    {
        u16 address;
        char segment[5];
        // `name` intentionally retains GearSystem's inline syntax markers
        // ({n}=mnemonic, {o}=operand, {e}=relative extra).  The GUI consumes
        // them to colour semantic parts of an instruction.
        char name[96];
        char bytes[32];
        int size;
        int bank;
        u8 opcodes[7];
        bool jump;
        u16 jump_address;
        bool subroutine;
        char auto_symbol[32];
        // Execution coverage is deliberately separate from the mere presence
        // of a disassembly record: opening a disassembly view may decode code,
        // but only a real CPU instruction boundary increments these fields.
        u32 physical_address;
        u64 execution_count;
        u64 first_execution_tstate;
        u64 last_execution_tstate;
    };

    struct CodeCoverageRecord
    {
        u16 logical_address;
        u32 physical_address;
        int bank;
        std::string segment;
        std::string instruction;
        std::string bytes;
        int size;
        u64 execution_count;
        u64 first_execution_tstate;
        u64 last_execution_tstate;
    };

    struct stMemoryBreakpoint
    {
        u16 address1;
        u16 address2;
        bool read;
        bool write;
        bool range;
    };


public:
    Memory(Cartridge* pCartridge, DebugEventManager& debugEvents);
    ~Memory();
    void SetCpuDebugContext(CpuDebugContext* context);
    void Init();
    void Reset();
    void SetM5Mapper(class M5MemoryMapper* mapper) { m_m5Mapper = mapper; }
    void SetMSXSlots(MSXSlotMapper* slots) { m_msxSlots = slots; }
    MSXSlotMapper* GetMSXSlots() const { return m_msxSlots; }
    void SetColecoMotherboard(bool enabled) { m_colecoMotherboard = enabled; }
    void SetSMSMotherboard(bool enabled) { m_smsMotherboard = enabled; }
    // The motherboard's decoded work RAM (SystemModelDescriptor::boardRamSize):
    // what answers at the board's RAM window when the cartridge does not.
    void SetBoardRamSize(u32 size) { m_boardRamSize = size; }
    u32 GetBoardRamSize() const { return m_boardRamSize; }
    // Offset of a CPU address in the board's RAM: it repeats every
    // boardRamSize bytes across whatever window the board decodes.
    u16 BoardRamOffset(u16 address) const
    { return static_cast<u16>(address & (m_boardRamSize - 1)); }
    bool UsesSMSWorkRam() const;
    const Cartridge& GetCartridge() const { return *m_pCartridge; }
    void SetupMapper();
    DebugDeviceDefinition DescribeDebugDevice() const override;
    void SetDebugMachineKey(const std::string& key);
    const Mapper* GetMapperDebugEventProvider() const { return m_pMapper; }
    void BindDebugEventProviders(const DebugDeviceRegistry& registry);
    u8 Read(u16 address);
    // Inspection follows the current mapping without publishing CPU accesses.
    u8 Peek(u16 address);
    void Write(u16 address, u8 value);
    static constexpr u32 RamCapacity = 0x10000;
    static constexpr u32 LegacyInternalRamSize = 0x800;
    u8* GetInternalRam();
    u8* GetExternalRam();
    u8* GetSMSCartridgeRam();
    u8* GetBios();
    const std::string& GetBiosFilePath() const { return m_biosFilePath; }
    u32 GetBiosCRC() const;
    // Zero releases storage. Reject non-powers of two without changing state.
    // Equal sizes preserve the loaded firmware; call LoadBios for a new board.
    bool ConfigureBiosStorage(u32 size);
    u32 GetBiosSize() const { return m_biosSize; }
    // Valid only when GetBiosSize() != 0; mapping decides whether to mirror.
    u32 GetBiosMask() const { return m_biosSize ? m_biosSize - 1 : 0; }
    u8 GetRomBank();
    u32 GetRomBankAddress();
    void WriteMapperPort(u8 port, u8 value);
    bool HasMapper() const { return m_pMapper != nullptr; }
    // Save-state identity of the installed mapper, or 0 when the cartridge
    // needs none. Recorded in the snapshot header so a state can be rejected
    // or explained before any section is read.
    u32 GetMapperId() const;
    void LoadBios(const char* szFilePath);
    bool LoadBiosFromBuffer(const u8* data, u32 size, const std::string& identity);
    bool IsBiosLoaded() const;
    void SaveState(std::ostream& stream);
    void LoadState(std::istream& stream);
    bool ValidateState(std::istream& stream) const;
    void ResetRomDisassembledMemory();
    stDisassembleRecord* GetDisassembleRecord(u16 address, bool createIfNotFound);
    // The cartridge ROM offset a CPU address reads right now, through the
    // active mapper; false when it is not cartridge ROM (BIOS, RAM, SRAM).
    bool TranslateToRomOffset(u16 address, u32& romOffset);
    // Something answers at this address on this board right now: ROM that
    // exists, RAM, BIOS. False for a hole (an empty slot, past the end of the
    // cartridge). What tells code from data that only decodes as code.
    bool AnswersAt(u16 address);

    // ROM map feed (src/RomMap.h). The bus calls these for data reads - not
    // opcode fetches - and for port writes; VRAM writes and VDP register
    // writes reach it from CheckVRAMAccess/CheckVDPRegisterWrite. With no map
    // attached each is one null test.
    void SetRomMap(RomMap* romMap) { m_pRomMap = romMap; }
    void NotifyCpuDataRead(u16 address, u8 value)
    {
        if (m_pRomMap)
            ForwardCpuDataRead(address, value);
    }
    void NotifyCpuPortWrite(u8 port, u8 value)
    {
        if (m_pRomMap)
            ForwardCpuPortWrite(port, value);
    }
    std::vector<CodeCoverageRecord> GetCodeCoverage() const;
    void ClearCodeCoverage();
    stDisassembleRecord** GetDisassembledRomMemoryMap();
    stDisassembleRecord** GetDisassembledInternalRamMemoryMap();
    stDisassembleRecord** GetDisassembledBiosMemoryMap();
    stDisassembleRecord** GetDisassembledExternalRamMemoryMap();
    std::vector<stDisassembleRecord*>* GetBreakpointsCPU();
    std::vector<stMemoryBreakpoint>* GetBreakpointsMem();
    // Arms a one-shot logical-address breakpoint.  It is deliberately armed
    // even when the visible PC already equals address: after a machine reset
    // that value is only the CPU's pending reset destination, and the first
    // real instruction boundary at $0000 still has to be reached.
    bool ArmRunToAddress(u16 address);
    void CancelRunToAddress();
    bool IsRunToAddressActive() const { return m_runToAddressActive; }
    u16 GetRunToAddress() const { return m_runToAddress; }
    void EnableSGMUpper(bool enable);
    void SetSGMDevice(class SGMDevice* device) { m_sgm = device; }
    SGMDevice* GetSGMDevice() const { return m_sgm; }
    void SetIPLRomDisabled(bool disabled);
    // The SF-7000 I/O cartridge overlays IPL/RAM without replacing the
    // cartridge physically inserted in the SC-3000.
    void EnableSF7000(bool enable);
    bool IsSF7000Enabled() const;

    void CheckBreakpoints(u16 address, bool write);
    void CheckBreakpoints(u16 address, bool write, u8 value, bool valueKnown);
    void CheckBreakpoints(u16 address, bool write, u8 beforeValue, bool beforeKnown, u8 afterValue, bool afterKnown);
    void CheckVRAMAccess(u16 address, bool write, u8 value, bool valueKnown);
    void CheckVRAMAccess(u16 address, bool write, u8 beforeValue, bool beforeKnown, u8 afterValue, bool afterKnown,
        const DebugEventRasterContext* raster = NULL);
    // Publishes one decoded CPU I/O transaction using the active CPU debug
    // context for PC, timestamp and pause delivery. Both Z80 cores use this
    // path so port breakpoints do not depend on a concrete CPU class.
    void CheckIOPortAccess(const DebugProbe& probe, u16 rawPort, u8 access,
                           u8 value, bool valueKnown = true);
    void OnCpuInstructionBoundary(u16 nextProgramCounter,
                                  u64 elapsedTStates) override;
    // Device-neutral video-register event. TMS9918 uses R0-R7 today; an
    // alternative VDP can publish any 8-bit register index through it.
    void CheckVDPRegisterWrite(u8 reg, u8 beforeValue, u8 afterValue);
    void CheckVDPStateChange(u16 stateTarget, u64 beforeValue, u64 afterValue,
        DebugEventSource source,
        const DebugEventRasterContext* raster = NULL);
    // Video timing target is the scanline; value is a VideoTimingEvent flag.
    void CheckVideoTimingEvent(u16 scanline, u8 eventFlags);
    // Decoded PPI latch transition caused by a port or BSR/control write.
    // Raw CPU port attempts are published once by CheckIOPortAccess().
    void CheckPPIStateChange(u16 port, u8 beforeValue, u8 afterValue);
    // Decoded uPD765 transition; target identifies the FDC state field.
    void CheckFDCEvent(u16 eventType, u8 beforeValue, u8 afterValue);
    // Decoded audio-chip register transition; target identifies the register.
    void CheckAudioEvent(u16 reg, u8 value);
    void CheckAyEvent(u16 reg, u8 value);
    // Decoded recorder/cassette transport transition.
    void CheckTapeEvent(u16 eventType, u8 value);
    // Native component signal/state transition. The provider supplies its
    // runtime probe after the catalog is frozen; no legacy category alias is
    // produced for newly detached devices.
    void CheckDeviceStateChange(const DebugProbe& probe,
                                u64 beforeValue, u64 afterValue);
    // Native logical register or endpoint access. The System Bus separately
    // owns the raw CPU transaction; this records what the selected component
    // accepted without adding component-specific hooks to Memory.
    void CheckDeviceAccess(const DebugProbe& probe, u8 access,
                           u64 beforeValue, bool beforeKnown,
                           u64 afterValue, bool afterKnown);
    // GUI switches affect only their historical lists. Common DebugEvent/MCP
    // rules are never disabled by these flags.
    void SetLegacyMemoryBreakpointsEnabled(bool enabled) { m_LegacyMemoryBreakpointsEnabled = enabled; }
    void SetLegacyCpuBreakpointsEnabled(bool enabled) { m_LegacyCpuBreakpointsEnabled = enabled; }
private:
    u8 ReadMapped(u16 address, bool publish);
    // Composition state, derived from the core's saved machine identity.
    class M5MemoryMapper* m_m5Mapper = nullptr;
    MSXSlotMapper* m_msxSlots = nullptr;
    bool m_smsMotherboard = false;
    u32 m_boardRamSize = 0x400;
    bool m_colecoMotherboard = false;
    void ForwardCpuDataRead(u16 address, u8 value);
    void ForwardCpuPortWrite(u8 port, u8 value);
#ifndef GEARSF7000_DISABLE_DISASSEMBLER
    void ResolveLocation(u16 address, stDisassembleRecord**& map,
                         int& offset, int& segment, int& bank);
#endif
    RomMap* m_pRomMap = nullptr;

private:
    void WriteMapperMemory(u16 address, u8 value);
    void PublishMapperBankChange(u8 beforeBank, u8 afterBank);
    u8 ReadMegaCart(u16 address, bool publish);
    CpuDebugSnapshot GetCpuDebugSnapshot() const;
    void RequestCpuDebugPause();
    void InternalBreakpointsCheck(DebugEventCategory category, u16 address, bool write,
                                  const std::vector<stMemoryBreakpoint>& breakpoints);

private:
    CpuDebugContext* m_cpuDebugContext;
    Cartridge* m_pCartridge;
    Mapper* m_pMapper;
    std::map<const u8*, std::vector<stDisassembleRecord*>> m_additionalRomMaps;
    stDisassembleRecord** m_pDisassembledRomMap;
    stDisassembleRecord** m_pDisassembledInternalRamMap;
    stDisassembleRecord** m_pDisassembledBiosMap;
    stDisassembleRecord** m_pDisassembledExternalRamMap;
    std::vector<stDisassembleRecord*> m_BreakpointsCPU;
    std::vector<stMemoryBreakpoint> m_BreakpointsMem;
    DebugEventManager& m_DebugEvents;
    DebugDeviceId m_debugMemoryDevice;
    DebugDeviceId m_debugMapperDevice;
    DebugDeviceId m_debugSF7000MemoryDevice;
    DebugDeviceId m_debugVideoDevice;
    DebugDeviceId m_debugPsgDevice;
    std::string m_debugMachineKey = "event.sc3000";
    bool m_LegacyMemoryBreakpointsEnabled;
    bool m_LegacyCpuBreakpointsEnabled;
    bool m_runToAddressActive;
    u16 m_runToAddress;
    bool m_bBiosLoaded;
    bool m_bSF7000Enabled;
    // Legacy SGM latch retained in the savestate layout; not decoded by Coleco yet.
    bool m_bSGMUpper;
    SGMDevice* m_sgm = nullptr;
    bool m_bIPLRomDisabled;
    u8* m_pBios;
    u32 m_biosSize = 0;
    std::string m_biosFilePath;
    // Stable 64 KiB allocations. SystemBus declares installed regions;
    // mappers decode CPU windows/mirrors. Capacity never implies presence.
    u8* m_pInternalRam;
    u8* m_pExternalRam;
    u32 m_RomBankAddress;
    u8 m_RomBank;
};

#include "M5MemoryMapper.h"
#include "Memory_inline.h"

#endif	/* MEMORY_H */
