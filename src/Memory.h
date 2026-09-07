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

#ifndef MEMORY_H
#define	MEMORY_H

#include "definitions.h"
#include "Mapper.h"
#include "Breakpoints.h"
#include "DebugEvents.h"
#include "cpu/CpuDebugContext.h"
#include "cpu/CpuInstructionObserver.h"
#include <vector>

class Cartridge;
class Mapper;

class Memory : public CpuInstructionObserver
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
    Memory(Cartridge* pCartridge);
    ~Memory();
    void SetCpuDebugContext(CpuDebugContext* context);
    void Init();
    void Reset();
    void SetupMapper();
    u8 Read(u16 address);
    void Write(u16 address, u8 value);
    u8* GetRam();
    u8* GetSGMRam();
    u8* GetBios();
    u8 GetRomBank();
    u32 GetRomBankAddress();
    bool HasMapper() const { return m_pMapper != nullptr; }
    // Save-state identity of the installed mapper, or 0 when the cartridge
    // needs none. Recorded in the snapshot header so a state can be rejected
    // or explained before any section is read.
    u32 GetMapperId() const;
    void LoadBios(const char* szFilePath);
    bool IsBiosLoaded();
    void SaveState(std::ostream& stream);
    void LoadState(std::istream& stream);
    void ResetRomDisassembledMemory();
    stDisassembleRecord* GetDisassembleRecord(u16 address, bool createIfNotFound);
    std::vector<CodeCoverageRecord> GetCodeCoverage() const;
    void ClearCodeCoverage();
    stDisassembleRecord** GetDisassembledRomMemoryMap();
    stDisassembleRecord** GetDisassembledRamMemoryMap();
    stDisassembleRecord** GetDisassembledBiosMemoryMap();
    stDisassembleRecord** GetDisassembledSGMRamMemoryMap();
    std::vector<stDisassembleRecord*>* GetBreakpointsCPU();
    std::vector<stMemoryBreakpoint>* GetBreakpointsMem();
    std::vector<stMemoryBreakpoint>* GetBreakpointsVRAM();
    // Arms a one-shot logical-address breakpoint.  It is deliberately armed
    // even when the visible PC already equals address: after a machine reset
    // that value is only the CPU's pending reset destination, and the first
    // real instruction boundary at $0000 still has to be reached.
    bool ArmRunToAddress(u16 address);
    void CancelRunToAddress();
    bool IsRunToAddressActive() const { return m_runToAddressActive; }
    u16 GetRunToAddress() const { return m_runToAddress; }
    void EnableSGMUpper(bool enable);
    void SetIPLRomDisabled(bool disabled);
    // The SF-7000 I/O cartridge overlays IPL/RAM without replacing the
    // cartridge physically inserted in the SC-3000.
    void EnableSF7000(bool enable);
    bool IsSF7000Enabled() const;

    void CheckBreakpoints(u16 address, bool write);
    void CheckBreakpoints(u16 address, bool write, u8 value, bool valueKnown);
    void CheckBreakpoints(u16 address, bool write, u8 beforeValue, bool beforeKnown, u8 afterValue, bool afterKnown);
    void CheckVRAMBreakpoints(u16 address, bool write, u8 value, bool valueKnown);
    void CheckVRAMBreakpoints(u16 address, bool write, u8 beforeValue, bool beforeKnown, u8 afterValue, bool afterKnown,
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
    DebugEventManager* GetDebugEventManager();
    // GUI switches affect only their historical lists. Common DebugEvent/MCP
    // rules are never disabled by these flags.
    void SetLegacyMemoryBreakpointsEnabled(bool enabled) { m_LegacyMemoryBreakpointsEnabled = enabled; }
    void SetLegacyVRAMBreakpointsEnabled(bool enabled) { m_LegacyVRAMBreakpointsEnabled = enabled; }
    void SetLegacyCpuBreakpointsEnabled(bool enabled) { m_LegacyCpuBreakpointsEnabled = enabled; }
private:
    CpuDebugSnapshot GetCpuDebugSnapshot() const;
    void RequestCpuDebugPause();
    void InternalBreakpointsCheck(DebugEventCategory category, u16 address, bool write,
                                  const std::vector<stMemoryBreakpoint>& breakpoints);

private:
    CpuDebugContext* m_cpuDebugContext;
    Cartridge* m_pCartridge;
    Mapper* m_pMapper;
    stDisassembleRecord** m_pDisassembledRomMap;
    stDisassembleRecord** m_pDisassembledRamMap;
    stDisassembleRecord** m_pDisassembledBiosMap;
    stDisassembleRecord** m_pDisassembledSGMRamMap;
    std::vector<stDisassembleRecord*> m_BreakpointsCPU;
    std::vector<stMemoryBreakpoint> m_BreakpointsMem;
    std::vector<stMemoryBreakpoint> m_BreakpointsVRAM;
    DebugEventManager m_DebugEvents;
    bool m_LegacyMemoryBreakpointsEnabled;
    bool m_LegacyVRAMBreakpointsEnabled;
    bool m_LegacyCpuBreakpointsEnabled;
    bool m_runToAddressActive;
    u16 m_runToAddress;
    bool m_bBiosLoaded;
    bool m_bSF7000Enabled;
    bool m_bSGMUpper;
    bool m_bIPLRomDisabled;
    u8* m_pBios;
    u8* m_pRam;
    u8* m_pSGMRam;
    u32 m_RomBankAddress;
    u8 m_RomBank;
};

#include "Memory_inline.h"

#endif	/* MEMORY_H */
