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

#ifndef CORE_H
#define	CORE_H

#include <string>
#include "definitions.h"
#include "VideoFrame.h"
#include "Cartridge.h"
#if GEARSF7000_ENABLE_SDL_INPUT
#include <SDL3/SDL.h>
#endif
#include "cpu/clk/GearSF7000ClockDomains.h"

class Memory;
class CLKZ80Processor;
class CpuStateAccess;
class CpuExecution;
class CpuInterruptRouter;
class CpuInstructionObserverList;
class CpuStatePersistence;
class Z80Disassembler;
class Audio;
class Video;
class SK1100;
class SR1000;
class SR1000Speaker;
class SF7000;
class SP400;
class MachineIOPorts;
class WavSampleManager;
class WavPlayer;

// A RunToVBlank call may end at an emulated frame boundary or because the
// debugger requested control. Keep those facts distinct: the legacy output
// flush still happens for every successful run, but callers and future
// streaming/audio code must not mistake a debugger stop for a VBlank.
enum class GC_RunExitReason : u8
{
    NotRun,
    VBlank,
    Step,
    Line,
    Breakpoint,
    SafetyLimit
};

struct GC_RunResult
{
    GC_RunExitReason reason = GC_RunExitReason::NotRun;
    bool reachedVBlank = false;

    bool DidRun() const { return reason != GC_RunExitReason::NotRun; }
    bool BreakpointHit() const { return reason == GC_RunExitReason::Breakpoint; }
};


class GearSF7000Core : private GearSF7000BusClockSink
{

public:
    GearSF7000Core();
    ~GearSF7000Core();
    void Init(GC_Color_Format pixelFormat = GC_PIXEL_RGB888);
    GC_Color_Format GetPixelFormat() const { return m_pixelFormat; }
    GC_RunResult RunToVBlank(u8* pFrameBuffer, s16* pSampleBuffer, int* pSampleCount, bool step = false, bool stopOnBreakpoints = false, bool stopOnLineChange = false);
    bool IsMachineReady() const;
    // pauseAtResetVector: see the comment on Reset() below.
    bool LoadROM(const char* szFilePath, Cartridge::ForceConfiguration* config = NULL, bool pauseAtResetVector = false);
    bool LoadROMNull(Cartridge::ForceConfiguration* config = NULL, bool pauseAtResetVector = false);
    bool LoadROMFromBuffer(const u8* buffer, int size, Cartridge::ForceConfiguration* config = NULL, bool pauseAtResetVector = false);
    void SaveDisassembledROM();
    bool GetRuntimeInfo(GC_RuntimeInfo& runtime_info);
    GC_VideoFrameDescriptor GetVideoFrameDescriptor() const;
    void RenderCurrentFrame(u8* pFrameBuffer);
    
    void EnableEvents(bool enabled);
    void SetKeyboardMode(bool enabled);
#if GEARSF7000_ENABLE_SDL_INPUT
    void SetEvent(SDL_Event event);
#endif
    void PauseKeyPressed();
    bool QueueKeyboardText(const std::string& text, std::string* error = NULL);
    void ClearKeyboardText();
    // Matrix polls per keystroke and the idle hold after Return. Trades margin
    // against speed for the text injector; see SK1100::SetTextTiming.
    void SetKeyboardTextTiming(int pressPolls, int releasePolls, int newlinePolls);
    // Progress of the queued keyboard text as a 0..1 fraction, 1 when idle.
    float GetKeyboardTextProgress() const;
    void JoystickPressed(GC_Controllers controller, GC_Keys key);
    void JoystickReleased(GC_Controllers controller, GC_Keys key);
    // The raw keyboard/joystick matrix, 8 rows of 16 bits. Row 7 is where the
    // two joystick ports live - bits 0-5 controller 1 (up/down/left/right/
    // button1/button2), bits 6-11 controller 2, active low. Everything
    // JoystickPressed() and PressMatrixKeyByLabel() can affect ends up here,
    // which makes it the one place a caller can read back what is currently
    // held down.
    void GetInputRows(uint16_t rows[8]) const;
    // Direct press/release of a single SK-1100 keyboard matrix key that
    // QueueText() can't reach (non-printable, e.g. cursor keys). See
    // SK1100::PressMatrixKeyByLabel for accepted labels.
    bool KeyboardKey(const std::string& label, bool pressed);
    void KeyboardMatrixKey(int row, int mask, bool pressed);
    
    void Pause(bool paused);
    bool IsPaused();
    void ResetROM(Cartridge::ForceConfiguration* config = NULL, bool pauseAtResetVector = false);
    // Clears the cartridge and brings the machine back to the same idle
    // state it is in before anything is ever loaded. Unlike ResetROM, which
    // is a no-op with nothing inserted, this always runs.
    void EjectCartridge();
    void StartSF7000(bool pauseAtResetVector = false);
    void ResetROMPreservingRAM(Cartridge::ForceConfiguration* config = NULL);
    void ResetSound();
    void SaveRam();
    void SaveRam(const char* szPath, bool fullPath = false);
    void LoadRam();
    void LoadRam(const char* szPath, bool fullPath = false);
    bool SaveState(int index);
    // All four report whether the machine was actually written or applied.
    // bytesWritten is filled in on success so a caller can say how large the
    // snapshot was without stat-ing the file back.
    bool SaveState(const char* szPath, int index,
                   size_t* bytesWritten = nullptr);
    bool SaveState(u8* buffer, size_t& size);
    bool SaveState(std::ostream& stream, size_t& size);
    bool LoadState(int index);
    bool LoadState(const char* szPath, int index);
    bool LoadState(const u8* buffer, size_t size);
    bool LoadState(std::istream& stream);
    Memory* GetMemory();
    Cartridge* GetCartridge();
    CpuStateAccess* GetCpuStateAccess();
    const char* GetCpuBackendName() const;
    bool IsCpuStatePersistenceAvailable() const;
    // Version of the snapshot container this build writes and accepts.
    static u16 GetSaveStateFormatVersion();
    // The machine's clock domains, so that anything deriving a rate from them
    // - the frame recorder converting seconds to frames, for one - reads the
    // same numbers the emulation runs on instead of keeping its own copy.
    const GearSF7000ClockDomains& GetClockDomains() const { return m_clockDomains; }

    // Align the machine to the host display.
    //
    // The TMS9918 makes 59.9227 frames a second NTSC, not 60. With the host's
    // vertical sync on there is no other frame limiter while a ROM runs, so
    // the machine is already dragged along at the display's rate - 0.13% fast,
    // and silently. Rather than leave that unsaid, one common multiplier can
    // be applied to *every* oscillator so the machine's own frame rate lands
    // on the display's. Internally nothing changes: the rates keep their
    // ratios, a frame stays 59736 T-states, and every measurement in T-states
    // or dots is untouched. It is a machine whose crystals are all 0.13% high.
    //
    // Not implemented: the machine always runs on its real crystals. The
    // design, and the reason it is deferred - at 120 and 144 Hz the answer is
    // to present a frame more than once, which is a main-loop change and not a
    // multiplier - is in DOCS/16. The accessors below stay because
    // get_sync_settings reports them, and they will keep telling the truth
    // whenever the alignment does arrive.
    //
    // The frame rate this machine's own crystals produce.
    double GetNativeFrameRate() const;

    // Frame rate the machine's crystals should be scaled to reach, or 0 for
    // the real ones. Latched at Reset(), not applied live: the clock rates
    // reach SystemClock, the clock domains, Audio, SR-1000 and the SF-7000
    // through their own Reset() calls, and changing them under a running
    // machine would mean a way to re-rate each of those without disturbing
    // its state. Deliberately not built - see DOCS/16 and D2 in DOCS/18.
    void SetTargetFrameRate(double fps);
    // What it is actually running at, once any alignment is applied.
    double GetEffectiveFrameRate() const;
    // 1.0 when running on the real crystals.
    double GetClockScale() const;
    // Why the last save or load was refused, as a short machine-readable
    // token plus detail. Empty after a success. The MCP layer hands this
    // straight back to the caller: "it failed" is not a useful answer when
    // the cause is a region or a mapper mismatch.
    const char* GetLastStateError() const;
    Z80Disassembler* GetDisassembler();
    Audio* GetAudio();
    Video* GetVideo();
    SR1000* GetCassette();
    SR1000Speaker* GetCassetteSpeaker();
    MachineIOPorts* GetMachineIOPorts();
    SP400* GetSP400();
    SF7000* GetSF7000();

    void DiskShutdown();
    uint8_t DiskChange(int driveId, const char* fileName, bool* isReadOnly);
    void DiskEject(int driveId);
    void DiskWriteProtect(int driveId, bool writeProtected);

    uint8_t CassetteChange(const char* fileName, bool* isReadOnly);
    void CassetteEject();
    void CassetteWriteProtect(bool writeProtected);

    void CassettePlay();
    void CassetteStop();
    void CassetteRewind();


#if GEARSF7000_ENABLE_SF7000
    WavSampleManager* GetWavSampleManager();
    WavPlayer* GetWavPlayer();
#endif

private:
    // pauseAtResetVector runs exactly the reset-completing quantum (see the
    // comment inside Reset()) and pauses with PC already at the reset
    // vector, rather than pausing before that quantum has ever run.
    void Reset(bool pauseAtResetVector = false);
    void RenderFrameBuffer(u8* finalFrameBuffer);
    void LoadSF7000Audio();
    void ConnectCpu();
    void ObserveCurrentInstruction();
    bool TickMachine(std::uint64_t cpuTStates,
                     std::uint64_t vdpMasterClocks,
                     std::uint64_t fdcClocks);
    void AdvanceZ80HalfCycles(std::uint64_t halfCycles) override;

private:
    bool enabledEvents;
    Memory* m_pMemory;
    CLKZ80Processor* m_pCLKProcessor;
    GearSF7000ClockDomains m_clockDomains;
    bool m_clkMachineVBlank;
    CpuExecution* m_pCpuExecution;
    CpuStatePersistence* m_pCpuStatePersistence;
    CpuInterruptRouter* m_pCpuInterruptRouter;
    CpuInstructionObserverList* m_pCpuInstructionObservers;
    Z80Disassembler* m_pDisassembler;
    Audio* m_pAudio;
    Video* m_pVideo;
    SK1100* m_pSK1100;
    SR1000* m_pSR1000;
    SF7000* m_pSF7000;
    SP400* m_pSP400;
    Cartridge* m_pCartridge;
    MachineIOPorts* m_pMachineIOPorts;
    bool m_bPaused;
    GC_Color_Format m_pixelFormat;
    std::string m_LastStateError;
    double m_ActiveClockScale = 1.0;
    double m_RequestedFrameRate = 0.0;

#if GEARSF7000_ENABLE_SF7000
    WavSampleManager* m_pSampleManager;
    WavPlayer* m_pWavPlayer;
#endif
};

#endif	/* CORE_H */
