/*
 * GearSF7000 - ROM map: what each byte of the cartridge ROM turned out to be
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef GEARSF7000_ROM_MAP_H
#define GEARSF7000_ROM_MAP_H

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "definitions.h"
#include "VideoDebugDescriptor.h"
#include "cpu/CpuInstructionObserver.h"

class Memory;

// One byte of state per byte of the physical cartridge ROM. A freshly
// attached ROM is 100% Unknown; bytes leave Unknown only because something
// was observed while the machine ran - never because they were looked at.
// DOCS/PROGRAM_ANALYSIS_AUDIT.md section 12.
//
// Phase A: executed bytes are Code. Phase B: bytes read as data are Data,
// and a byte the CPU copies into VRAM takes the type of the table it lands
// in, as the video device reports its layout at that moment (NAMETAB,
// PATGEN, COLTAB, SAT, SPRPAT), so background and sprite graphics are told
// apart.
//
// Off, it holds no memory and every hook returns at once. The map is not
// part of the machine: a reset or a save state leaves it alone, only loading
// a different ROM or Reset() clears it.
class RomMap final : public CpuInstructionObserver
{
public:
    // Stored in the low nibble of each byte; values are part of the file
    // format, so new kinds are appended, never renumbered.
    enum Kind : u8
    {
        Kind_Unknown = 0,
        Kind_Code = 1,
        Kind_Data = 2,      // read as data; where it went is not known
        Kind_GfxBg = 3,     // to PATGEN or COLTAB
        Kind_GfxSpr = 4,    // to SPRPAT
        Kind_Gfx = 5,       // to a pattern table shared by background and sprites
        Kind_Screen = 6,    // to NAMETAB
        Kind_Sat = 7,       // to the sprite attribute table
        Kind_Vram = 8,      // to VRAM, outside any table the device describes
        Kind_Sound = 9,     // read by a routine that writes the PSG (inferred)
        Kind_Fill = 10,     // padding: $00 or $FF all the way to the end of the ROM
        Kind_CodeCandidate = 11, // CODE?: the never-taken side of an executed branch
        Kind_Table = 12,    // a table of addresses, recognised statically
        Kind_Count
    };

    static constexpr u8 KindMask = 0x0F;
    // First byte of an executed instruction, as opposed to its operands.
    static constexpr u8 InstructionStart = 0x80;
    // For Kind_GfxBg: the byte went to the colour table, not the patterns.
    // For Kind_CodeCandidate: a never-called routine, not a never-taken branch.
    static constexpr u8 ColorTableFlag = 0x40;
    static constexpr u8 OrphanFlag = 0x40;
    // Any kind: inferred by a static rule, not seen happening - shown as
    // "DATA?", like CODE? for code. Observing the byte clears it. ($10 is
    // kept free for the dead-code proof.)
    static constexpr u8 CandidateFlag = 0x20;
    static constexpr u16 NoDestination = 0xFFFF;

    struct Range
    {
        u32 start;
        u32 end; // inclusive
        u8 kind;
        bool candidate = false; // CandidateFlag on its bytes
    };

    // Fills the device's current VRAM tables; returns how many.
    using VramTableSource = std::function<std::size_t(GC_VramTableSpan*, std::size_t)>;

    explicit RomMap(Memory* memory);

    void SetVramTableSource(VramTableSource source);

    // Binds the map to a ROM. Clears everything when the ROM differs from the
    // one currently attached; allocates only while enabled.
    void Attach(u32 romSize, u32 crc);
    void Detach();
    bool IsAttached() const { return m_romSize != 0; }

    // Enabling allocates an all-Unknown map for the attached ROM; disabling
    // frees it. Persisting across that is the caller's job (Save/Load).
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return m_enabled; }
    bool IsCollecting() const { return m_runtimeActive && m_enabled && !m_bytes.empty(); }
    void SetRuntimeActive(bool active);

    // Back to 100% Unknown, sessions to zero, as for a ROM never seen.
    void Reset();

    // The classifications made by looking rather than running, both padding:
    // the run of identical $00 or $FF bytes that ends the ROM (at least
    // kMinFill long), in $0000-$00FF the $00/$FF bytes that end right
    // before an RST or NMI vector, aligning it, and a run of $00/$FF ending
    // exactly at a 256-byte page that starts with something known (a table
    // aligned for speed). Only Unknown bytes change; anything observed wins.
    void MarkFill(const u8* rom, u32 romSize);
    static constexpr u32 kMinFill = 16;

    // Static candidates, shown apart from what was observed: from every
    // executed conditional branch (JR/JP/CALL/RET cc, DJNZ), the side never
    // taken that lands in Unknown is decoded until an unconditional end
    // (JP, JR, RET, RETI, RETN, JP (HL)) or known code, and becomes CODE?.
    // The "else" that never happens. Also CODE?: never-called routines - an
    // Unknown gap right after an executed JP/JR/RET that decodes cleanly into
    // one or more routines, filling the gap to its end (or falling into
    // known code) and branching to code. Recomputed from scratch each call;
    // executing a candidate later makes it CODE like anything else.
    // Assumes the ROM is seen at its own offsets (no mapper), as on the
    // SG-1000 games this targets first.
    void MarkBranchCandidates(const u8* rom, u32 romSize);

    // Static tables, recomputed from scratch each call and set only on Unknown
    // bytes, so anything observed later wins. General Z80 rules: pointer tables
    // (3+ words to routine entries - targets of executed CALL/JP/RST - or data
    // blocks of 8+ bytes). Sega standard sound driver (SG-1000/SC-3000/SMS
    // games): the tone table by its equal-tempered shape, the song table by
    // entries pointing to plausible song headers, and the shared volume-curve
    // table with its curves. Each rule validates its own shape, so on a game
    // without that driver it simply finds nothing. Last, table gaps: a run of
    // up to kMaxTableGap Unknown bytes with data read by the same routine on
    // both sides is an entry that routine never needed (an index value that
    // never came up, a field past an end marker); it needs the readers, so
    // maps from before format 3 get it as their neighbours are read again.
    void MarkStaticTables(const u8* rom, u32 romSize);
    // Evidence codes kept in the destination field of statically marked bytes.
    enum StaticReason : u16
    {
        Static_Base = 0xF000,
        Static_PointerTable = 0xF001,
        Static_ToneTable = 0xF002,
        Static_SongTable = 0xF003,
        Static_SongHeader = 0xF004,
        Static_CurveTable = 0xF005,
        Static_VolumeCurve = 0xF006,
        Static_TableGap = 0xF007,
        Static_JumpTable = 0xF008
    };
    // Jump tables found by Deep Search (JumpTables, rommap/jumptables.txt):
    // MarkStaticTables marks their Unknown bytes TABLE, redone from scratch
    // with the other static rules at every save. ROM offsets and byte counts.
    void SetJumpTables(const std::vector<std::pair<u32, u32>>& tables) { m_jumpTables = tables; }
    // Code Deep Search deduced by following the flow out of known code
    // (rommap/deduced.txt): MarkBranchCandidates makes its Unknown bytes
    // CODE?, before its own rules, redone at every save.
    struct DeducedBlock
    {
        u32 start;          // ROM offsets, end exclusive
        u32 end;
        u32 from;           // what leads there
        int score;          // 0-100, how much it looks like the game's code
        std::string reason;
    };
    void SetDeducedCode(std::vector<DeducedBlock> blocks);
    // The deduced block holding an offset, or null.
    const DeducedBlock* DeducedAt(u32 offset) const;
    // What Deep Search's data step made of bytes nobody read
    // (rommap/datahints.txt): "data? table at 2652, 2-byte entries ...".
    // Carried for the listing's headers, never a kind: a suggestion.
    struct DataHint
    {
        u32 start;          // ROM offsets, end exclusive
        u32 end;
        std::string text;
    };
    void SetDataHints(std::vector<DataHint> hints);
    const DataHint* DataHintAt(u32 offset) const;
    // Longest run of Unknown bytes the table-gap rule fills (MarkStaticTables).
    static constexpr u32 kMaxTableGap = 8;
    // The routine that read a byte, or the instruction when it was read
    // outside any call: what "read by the same code" compares.
    u16 GetReaderKey(u32 offset) const
    {
        const u16 routine = GetReaderRoutine(offset);
        return routine != NoDestination ? routine : GetReader(offset);
    }

    u32 GetRomSize() const { return m_romSize; }
    u32 GetCrc() const { return m_crc; }
    u32 GetCount(Kind kind) const { return kind < Kind_Count ? m_counts[kind] : 0; }
    u32 GetSessions() const { return m_sessions; }
    void SetSessions(u32 sessions) { m_sessions = sessions; }
    // Bumped on every change of classification: a viewer rebuilds its ranges
    // only when this moves.
    u64 GetGeneration() const { return m_generation; }
    const std::vector<u8>& GetBytes() const { return m_bytes; }
    // First VRAM address a byte was copied to - or, for Kind_Sound, the
    // entry of the sound routine that read it - or NoDestination.
    u16 GetDestination(u32 offset) const
    {
        return offset < m_destinations.size() ? m_destinations[offset] : NoDestination;
    }
    // Who first read a byte as data: the instruction, and the routine it was
    // in (the innermost call frame, or NoDestination outside any). What makes
    // a block that nobody touched readable is who touches its neighbours.
    u16 GetReader(u32 offset) const
    {
        return offset < m_readers.size() ? m_readers[offset] : NoDestination;
    }
    u16 GetReaderRoutine(u32 offset) const
    {
        return offset < m_readerRoutines.size() ? m_readerRoutines[offset] : NoDestination;
    }

    // Consecutive bytes of the same kind (and candidate or not), in address
    // order, covering the ROM.
    std::vector<Range> BuildRanges() const;
    static const char* KindName(u8 kind);
    // The kind as a range shows it: "DATA", or "DATA?" for a candidate.
    static std::string RangeName(const Range& range);
    // Why a range has its kind: "executed", "read as data", "-> PATGEN $0000".
    std::string DescribeEvidence(const Range& range) const;

    // Binary file: refuses a file written for a different ROM (CRC or size).
    // Reads version 1 (no destinations) and 2 (no readers) files as well.
    bool Save(const std::string& path) const;
    bool Load(const std::string& path, std::string* error = nullptr);

    void OnCpuInstructionBoundary(u16 nextProgramCounter,
                                  u64 elapsedTStates) override;

    // Marks an instruction executed at a physical ROM offset. The hook above
    // uses it; so does enabling the map mid-run, which takes over what the
    // execution coverage already saw since the ROM was loaded.
    void MarkExecuted(u32 physicalAddress, u32 size);

    // Phase B feed, from Memory: data reads (never opcode fetches), port
    // writes, VRAM writes as committed by the video device, and changes to
    // the VDP registers that move the tables.
    void OnCpuDataRead(u16 address, u8 value);
    void OnCpuPortWrite(u8 port, u8 value);
    // The board in use: where its PSG is, and the sound routines its
    // firmware provides. Kept across Attach/Load, applied to every map.
    void SetBoardSound(u8 psgMask, u8 psgMatch, const u16* entries, std::size_t count);

    // Firmware usage: what the cartridge takes from the board's firmware.
    // Entry points are every control transfer from outside the firmware
    // into it (CALL, JP, RST) and the interrupt vectors the hardware jumps
    // to; data is every firmware byte read as data. Kept with the map in
    // the project (firmware.txt) so an export can include just those parts.
    struct FirmwareEntry
    {
        u16 address;
        u32 count;
        bool interrupt;
    };
    void SetFirmwareRange(u16 start, u16 end);
    bool HasFirmware() const { return m_firmwareStart != m_firmwareEnd; }
    std::vector<FirmwareEntry> GetFirmwareEntries() const;
    std::vector<std::pair<u16, u16>> GetFirmwareReadRanges() const;  // [start, end)
    u32 GetFirmwareReadBytes() const;
    bool SaveFirmwareUsage(const std::string& path) const;
    bool LoadFirmwareUsage(const std::string& path, std::string* error);
    // Jump table entries seen in use: a JP (HL)/(IX)/(IY) landing on the
    // word the code had just read from the ROM - two bytes side by side, a
    // few instructions before. Proof, as an executed byte is: the entry at
    // `entry` is an address, and the jump at `site` goes through it. Kept
    // in the project (jumps_seen.txt). ROM offsets.
    struct ObservedJump
    {
        u32 site;
        u32 entry;
        u32 count;
    };
    const std::vector<ObservedJump>& GetObservedJumps() const { return m_observedJumps; }
    bool SaveObservedJumps(const std::string& path) const;
    bool LoadObservedJumps(const std::string& path, std::string* error = nullptr);
    // Pointers seen in use: a word read from the ROM - two bytes side by
    // side, by one instruction or by two different ones (not a block copy
    // walking through) - and then, within a few frames, a read at the
    // address it holds, in the ROM or in RAM. Proof that the word is an
    // absolute address, what a relocation must rewrite (Saverio). Kept in
    // the project (pointers_seen.txt). ROM offsets; the target as the CPU
    // saw it.
    struct ObservedPointer
    {
        u32 entry;
        u16 target;
        u32 count;
    };
    const std::vector<ObservedPointer>& GetObservedPointers() const { return m_observedPointers; }
    bool SaveObservedPointers(const std::string& path) const;
    bool LoadObservedPointers(const std::string& path, std::string* error = nullptr);
    void OnVramWrite(u16 vramAddress, u8 value);
    void OnVdpLayoutChanged() { m_layoutDirty = true; }

private:
    void Allocate();
    void Recount();
    void Classify(u32 offset, u8 kind);
    u8 KindForVram(u16 vramAddress, bool& colorTable);
    void TrackCallStack(u16 nextProgramCounter);
    void RememberInstruction(u16 pc, const u8* opcodes, int size, u16 jumpAddress);

    Memory* m_memory;
    bool m_enabled = false;
    bool m_runtimeActive = true;
    u32 m_romSize = 0;
    u32 m_crc = 0;
    u32 m_sessions = 0;
    u64 m_generation = 0;
    u32 m_counts[Kind_Count] = {};
    std::vector<u8> m_bytes;
    std::vector<u16> m_destinations;
    std::vector<u16> m_readers;
    std::vector<u16> m_readerRoutines;
    u8 m_fillValue = 0xFF;

    // The instruction being executed, so its own operand fetches are not
    // mistaken for data reads.
    u16 m_instructionPc = 0;
    u16 m_instructionSize = 0;
    u64 m_instructionIndex = 0;

    // Last byte read from ROM as data, and when.
    bool m_lastReadValid = false;
    u32 m_lastReadOffset = 0;
    u8 m_lastReadValue = 0;
    u64 m_lastReadInstruction = 0;

    // Port writes waiting for the VDP to commit them to VRAM: the TMS takes
    // a byte in the next access slot, which can come after the next
    // instruction has already read its own byte from ROM. So the source is
    // fixed at the OUT and handed to the VRAM write in order.
    struct PendingWrite
    {
        u8 value;
        bool fromRom;
        u32 romOffset;
    };
    static constexpr int kPendingCapacity = 8;
    PendingWrite m_pending[kPendingCapacity] = {};
    int m_pendingHead = 0;
    int m_pendingCount = 0;

    // Shadow call stack, rebuilt from instruction boundaries: a CALL or RST
    // whose next PC is its target opens a frame, an unrequested jump to an
    // interrupt vector opens one, a taken RET closes one. Each frame knows
    // whether it runs sound code, which a data read then inherits.
    struct Frame
    {
        u16 entry;
        bool interrupt;
        bool sound;
    };
    static constexpr int kMaxFrames = 64;
    Frame m_frames[kMaxFrames] = {};
    int m_frameCount = 0;
    // Routines seen writing the PSG, and their callers: the entries whose
    // later invocations make their reads SOUND. 64K bits.
    std::vector<u8> m_soundEntries;
    u8 m_psgMask = 0xC0;
    u8 m_psgMatch = 0x40;
    std::vector<u16> m_firmwareSoundEntries;
    u16 m_firmwareStart = 0;
    u16 m_firmwareEnd = 0;
    std::vector<FirmwareEntry> m_firmwareEntries;   // sorted by address
    std::vector<u8> m_firmwareReads;                // one bit per firmware byte
    void TrackFirmware(u16 next);
    void TrackIndirectJump(u16 next);
    std::vector<ObservedJump> m_observedJumps;      // sorted by site, then entry
    std::vector<ObservedPointer> m_observedPointers;// sorted by entry
    // The words read from the ROM lately, for the pointers seen in use.
    struct RecentWord
    {
        bool valid;
        u16 value;
        u32 entry;
        u64 instruction;
    };
    static constexpr int kRecentWords = 16;
    RecentWord m_recentWords[kRecentWords] = {};
    int m_recentNext = 0;
    void TrackPointer(u16 address, u32 readOffset, bool inRom);
    std::vector<std::pair<u32, u32>> m_jumpTables;  // Deep Search's tables: offset, bytes
    std::vector<DeducedBlock> m_deduced;            // sorted by start
    std::vector<DataHint> m_dataHints;              // sorted by start
    // The last two ROM bytes read as data, for the word a jump went through.
    struct Read
    {
        bool valid;
        u32 offset;
        u8 value;
        u64 instruction;
        u16 pc = 0;
    };
    Read m_reads[2] = {};
    void ClearFirmwareUsage();
    // The instruction that just ran, to tell what the transition was.
    bool m_prevValid = false;
    u16 m_prevPc = 0;
    u16 m_prevSize = 0;
    u16 m_prevTarget = 0;
    u8 m_prevFlow = 0; // 0 other, 1 call, 2 unconditional ret, 3 conditional ret,
                       // 4 jp (hl)/(ix)/(iy)

    VramTableSource m_vramTableSource;
    bool m_layoutDirty = true;
    static constexpr int kMaxTables = 8;
    GC_VramTableSpan m_tables[kMaxTables];
    std::size_t m_tableCount = 0;
};

#endif
