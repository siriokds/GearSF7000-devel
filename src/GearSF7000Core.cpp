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

#include <iomanip>
#include "GearSF7000Core.h"
#include "build_info.h"
#include "Memory.h"
#include "cpu/CpuExecution.h"
#include "cpu/clk/CLKZ80Processor.h"
#include "cpu/CpuInterruptRouter.h"
#include "cpu/CpuInstructionObserver.h"
#include "cpu/CpuStatePersistence.h"
#include "Z80Disassembler.h"
#include "Audio.h"
#include "Video.h"
#include "sk1100.h"
#if GEARSF7000_ENABLE_SR1000
#include "SR1000.h"
#endif
#if GEARSF7000_ENABLE_SF7000
#include "SF7000.h"
#endif
#if GEARSF7000_ENABLE_SP400
#include "SP400.h"
#endif
#include "Cartridge.h"
#include "MachineIOPorts.h"
#include "no_bios.h"

#if GEARSF7000_ENABLE_SF7000
#include "Disk.h"
#include "NEC765.h"
#endif
#if GEARSF7000_ENABLE_SF7000
#include "WavSampleManager.h"
#include "WavPlayer.h"
#endif

#include "SystemClock.h"

#if GEARSF7000_ENABLE_SF7000
#include "SF-7000_DiscMotor.h"
#include "SF-7000_DiscTrack2.h"
#endif
#include <cstdarg>
#include <sstream>
#include "SaveStateStream.h"

GearSF7000Core::GearSF7000Core()
{
#if GEARSF7000_ENABLE_SF7000
    InitPointer(m_pSampleManager);
    InitPointer(m_pWavPlayer);
#endif

    InitPointer(m_pMemory);
    InitPointer(m_pCLKProcessor);
    m_clkMachineVBlank = false;
    InitPointer(m_pCpuExecution);
    InitPointer(m_pCpuStatePersistence);
    InitPointer(m_pCpuInterruptRouter);
    InitPointer(m_pCpuInstructionObservers);
    InitPointer(m_pDisassembler);
    InitPointer(m_pAudio);
    InitPointer(m_pVideo);
    InitPointer(m_pSK1100);
    InitPointer(m_pSR1000);
    InitPointer(m_pSF7000);
    InitPointer(m_pSP400);
    InitPointer(m_pCartridge);
    InitPointer(m_pMachineIOPorts);

    m_bPaused = true;
    m_pixelFormat = GC_PIXEL_RGB888;


}

GearSF7000Core::~GearSF7000Core()
{
    m_bPaused = true;

#if GEARSF7000_ENABLE_SF7000
    diskEject(0);
    //DiskShutdown();
#endif

    SafeDelete(m_pMachineIOPorts);
    SafeDelete(m_pCartridge);
#if GEARSF7000_ENABLE_SF7000
    SafeDelete(m_pSF7000);
#endif
#if GEARSF7000_ENABLE_SP400
    SafeDelete(m_pSP400);
#endif
#if GEARSF7000_ENABLE_SR1000
    SafeDelete(m_pSR1000);
#endif
    SafeDelete(m_pSK1100);
    SafeDelete(m_pVideo);
    SafeDelete(m_pAudio);
    SafeDelete(m_pCpuInterruptRouter);
    SafeDelete(m_pCLKProcessor);
    SafeDelete(m_pCpuInstructionObservers);
    SafeDelete(m_pDisassembler);
    SafeDelete(m_pMemory);

#if GEARSF7000_ENABLE_SF7000
    SafeDelete(m_pSampleManager);
#endif
}

void GearSF7000Core::LoadSF7000Audio()
{
#if GEARSF7000_ENABLE_SF7000
    WavSampleManager m_pSampleManager;

    m_pSampleManager.LoadSampleFromBuffer("DISC_MOTOR", SF7000_DiscMotor_data, SF7000_DiscMotor_size);
    m_pSampleManager.LoadSampleFromBuffer("DISC_TRACK", SF7000_Track2_data, SF7000_Track2_size);

    //m_pSampleManager.LoadSample("DISC_MOTOR", "d:\\data\\SF-7000_DiscMotor.wav");
    //m_pSampleManager.LoadSample("DISC_TRACK", "d:\\data\\SF-7000_DiscTrack2.wav");

    //m_pSampleManager.LoadSample("DISC_LOAD", "d:\\data\\SF-7000_DiscLoad.wav");
    //m_pSampleManager.LoadSample("DISC_EJECT", "d:\\data\\SF-7000_DiscEject.wav");



    m_pWavPlayer = m_pAudio->GetWavPlayer();

    WavSample* wavSampleMotor = m_pSampleManager.GetSampleByName((char*)"DISC_MOTOR");
    WavSample* wavSampleTrack = m_pSampleManager.GetSampleByName((char*)"DISC_TRACK");

    m_pWavPlayer->SetChannel(0, wavSampleMotor);
    m_pWavPlayer->SetChannel(1, wavSampleTrack);
#endif
}

void GearSF7000Core::Init(GC_Color_Format pixelFormat)
{
    Log("--== %s %s by Saverio Russo ==--", GEARSF7000_TITLE, build_info_version());

    m_pixelFormat = pixelFormat;

    m_pCartridge = new Cartridge();
    m_pMemory = new Memory(m_pCartridge);
#ifndef GEARSF7000_DISABLE_DISASSEMBLER
    m_pDisassembler = new Z80Disassembler(m_pMemory);
    m_pCpuInstructionObservers = new CpuInstructionObserverList();
    m_pCpuInstructionObservers->Add(m_pMemory);
    m_pCpuInstructionObservers->Add(m_pDisassembler);
#endif
    // CLK is the sole Z80 implementation. All frontends and debugger commands
    // observe the same cycle-accurate execution timeline.
    m_pCLKProcessor = new CLKZ80Processor(m_pMemory);
    m_pCpuInterruptRouter = new CpuInterruptRouter();
    m_pAudio = new Audio();
    m_pVideo = new Video(m_pMemory, m_pCpuInterruptRouter);
    m_pSK1100 = new SK1100(m_pCpuInterruptRouter);
#if GEARSF7000_ENABLE_SF7000
    m_pSF7000 = new SF7000(m_pMemory, m_pAudio);
#endif
#if GEARSF7000_ENABLE_SP400
    m_pSP400 = new SP400();
#endif
#if GEARSF7000_ENABLE_SR1000
    m_pSR1000 = new SR1000(m_pAudio, m_pMemory);
#endif
    m_pMachineIOPorts = new MachineIOPorts(m_pAudio, m_pVideo, m_pSK1100, m_pSR1000, m_pSF7000, m_pSP400, m_pCartridge, m_pMemory);
    ConnectCpu();
    Log("CPU backend: CLK (save states available, format version %u)",
        static_cast<unsigned>(GetSaveStateFormatVersion()));


    //WavSample* sampleMotor = m_pSampleManager->GetSampleByName((char*)"DISC_MOTOR");
    //WavChannel* wavChannelMotor = new WavChannel(sampleMotor);
    //m_pWavPlayer->AddChannel(wavChannelMotor);

    //WavSample* sampleTrack = m_pSampleManager->GetSampleByName((char*)"DISC_TRACK");
    //WavChannel* wavChannelTrack = new WavChannel(sampleTrack);
    //m_pWavPlayer->AddChannel(wavChannelTrack);


    //m_pSampleManager->SaveSample(0, "d:\\data\\SF-7000_DiscMotor2.raw");

    m_pMemory->Init();
    m_pCpuExecution->InitializeExecution();
    m_pAudio->Init();
    m_pVideo->Init();
    m_pSK1100->Init();
#if GEARSF7000_ENABLE_SR1000
    m_pSR1000->Init();
#endif

#if GEARSF7000_ENABLE_SF7000
    m_pSF7000->Init(m_pAudio);
#endif
#if GEARSF7000_ENABLE_SP400
    m_pSP400->Init();
#endif

    // CLK reports elapsed time before each terminal bus operation. Devices
    // are fully initialised at this point, so they can now be advanced at the
    // actual bus timestamp instead of at the end of the whole instruction.
    m_pCLKProcessor->GetBusHandler().SetClockSink(this);

    systemClock.Init(3579545);

#if GEARSF7000_ENABLE_SF7000
    diskInit(0);
#endif

    m_pCartridge->Init();

    LoadSF7000Audio();


}

void GearSF7000Core::ConnectCpu()
{
    if (!m_pCLKProcessor || !m_pMemory || !m_pCpuInterruptRouter)
        return;

    m_pCpuExecution = m_pCLKProcessor;
    // The CLK backend serializes its own micro-op scheduler, so save states
    // are available and can be taken part-way through an instruction.
    m_pCpuStatePersistence = m_pCLKProcessor;
    m_pMemory->SetCpuDebugContext(m_pCpuExecution);
    m_pCpuInterruptRouter->Attach(m_pCpuExecution);
    m_pCpuExecution->AttachIOPorts(m_pMachineIOPorts);
#ifndef GEARSF7000_DISABLE_DISASSEMBLER
    m_pCpuExecution->AttachInstructionObserver(m_pCpuInstructionObservers);
#endif
}


#if GEARSF7000_ENABLE_SF7000
WavSampleManager* GearSF7000Core::GetWavSampleManager()
{
    return m_pSampleManager;
}

WavPlayer* GearSF7000Core::GetWavPlayer()
{
    return m_pWavPlayer;
}
#endif

GC_RunResult GearSF7000Core::RunToVBlank(u8* pFrameBuffer, s16* pSampleBuffer, int* pSampleCount, bool step, bool stopOnBreakpoints, bool stopOnLineChange)
{
    GC_RunResult result;

    if (!m_bPaused && IsMachineReady())
    {
        bool stop = false;
        int totalClocks = 0;
        const int startLine = stopOnLineChange ? m_pVideo->GetRenderLine() : -1;
        while (!stop)
        {
#ifdef PERFORMANCE
            const unsigned int budget = 75;
#else
            const unsigned int budget = 1;
#endif
            const unsigned int clockCycles = static_cast<unsigned int>(
                step ? m_pCpuExecution->ExecuteInstruction()
                     : m_pCpuExecution->ExecuteForTStates(budget));
            // The CLK bus sink has already advanced the machine, including
            // time consumed by deterministic wait states. Keep instruction
            // granularity for frame exit while preserving bus-level order.
            result.reachedVBlank = m_clkMachineVBlank;
            m_clkMachineVBlank = false;
            if (result.reachedVBlank)
            {
                result.reason = GC_RunExitReason::VBlank;
                stop = true;
            }

            totalClocks += clockCycles;

#ifndef GEARSF7000_DISABLE_DISASSEMBLER
            if ((step || (stopOnBreakpoints && m_pCpuExecution->HasBreakpointHit())) && !m_pCpuExecution->IsDuringInputOperation())
            {
                stop = true;
                if (m_pCpuExecution->HasBreakpointHit())
                    result.reason = GC_RunExitReason::Breakpoint;
                else
                    result.reason = GC_RunExitReason::Step;
            }
            else if (stopOnLineChange && !m_pCpuExecution->IsDuringInputOperation() &&
                     m_pVideo->GetRenderLine() != startLine)
            {
                stop = true;
                result.reason = GC_RunExitReason::Line;
            }
#endif

            if (totalClocks > 702240)
            {
                if (!stop)
                    result.reason = GC_RunExitReason::SafetyLimit;
                stop = true;
            }
        }

        // Preserved legacy policy: a debugger stop flushes the partial audio
        // segment and current framebuffer as well. GC_RunResult now tells the
        // caller whether this was a real VBlank, so later streaming/audio work
        // can change that policy without inferring it from a bool.
        m_pAudio->EndFrame(pSampleBuffer, pSampleCount);
        RenderFrameBuffer(pFrameBuffer);
    }

    return result;
}

bool GearSF7000Core::TickMachine(std::uint64_t cpuTStates,
                                std::uint64_t vdpMasterClocks,
                                std::uint64_t fdcClocks)
{
    if (cpuTStates == 0 && vdpMasterClocks == 0 && fdcClocks == 0)
        return false;

    if (cpuTStates != 0)
    {
        const unsigned int clocks = static_cast<unsigned int>(cpuTStates);
        systemClock.Tick(clocks);
        m_pAudio->Tick(clocks);
        // The physical SK-1100 matrix changes only on host key/joystick
        // edges. Advance it only while the automatic text injector has timed
        // events.
        if (m_pSK1100->HasPendingTimedEvents())
            m_pSK1100->Tick(clocks);
#if GEARSF7000_ENABLE_SR1000
        m_pSR1000->Tick(clocks);
#endif
#if GEARSF7000_ENABLE_SF7000
        if (m_pMemory->IsSF7000Enabled())
            m_pSF7000->TickCpu(cpuTStates);
#endif
    }

#if GEARSF7000_ENABLE_SF7000
    if (fdcClocks != 0 && m_pMemory->IsSF7000Enabled())
        m_pSF7000->TickFDC(fdcClocks);
#endif

    return vdpMasterClocks != 0 && m_pVideo->TickMasterClocks(
        static_cast<unsigned int>(vdpMasterClocks));
}

void GearSF7000Core::AdvanceZ80HalfCycles(std::uint64_t halfCycles)
{
    // CLK reports objective Z80 half-cycles. Convert the elapsed interval
    // once into each physical oscillator domain, retaining every rational
    // remainder. The VDP can therefore advance independently on PAL hardware
    // without a per-master-clock scheduler loop.
    m_clockDomains.AdvanceZ80HalfCycles(halfCycles);
    const std::uint64_t cpuTStates = m_clockDomains.ConsumeCpuTStates();
    const std::uint64_t vdpMasterClocks =
        m_clockDomains.ConsumeVDPMasterClocks();
    const std::uint64_t fdcClocks = m_clockDomains.ConsumeFDCClocks();

    m_clkMachineVBlank = TickMachine(
        cpuTStates, vdpMasterClocks, fdcClocks) ||
        m_clkMachineVBlank;
}

bool GearSF7000Core::IsMachineReady() const
{
    // A bare SC-3000 requires a cartridge.  With the external SF-7000 I/O
    // cartridge enabled, its IPL/RAM is a complete bootable machine instead.
    return m_pCartridge->IsReady() ||
           (m_pMemory->IsSF7000Enabled() && m_pMemory->IsBiosLoaded());
}

void GearSF7000Core::ObserveCurrentInstruction()
{
#ifndef GEARSF7000_DISABLE_DISASSEMBLER
    if (!m_pCpuExecution || !m_pDisassembler)
        return;
    const CpuStateSnapshot state = m_pCpuExecution->GetCpuStateSnapshot();
    // Decode for the debugger without pretending the instruction executed.
    // Calling the complete observer list here used to contaminate coverage
    // and could also evaluate execute breakpoints merely by inspecting PC.
    m_pDisassembler->Disassemble(state.pc);
#endif
}

bool GearSF7000Core::LoadROM(const char* szFilePath, Cartridge::ForceConfiguration* config, bool pauseAtResetVector)
{
    if (m_pCartridge->LoadFromFile(szFilePath))
    {
        if (IsValidPointer(config))
            m_pCartridge->ForceConfig(*config);

		m_pMemory->SetupMapper();
        Reset(pauseAtResetVector);

        m_pMemory->ResetRomDisassembledMemory();
        ObserveCurrentInstruction();

        return true;
    }
    else
        return false;
}


bool GearSF7000Core::LoadROMNull(Cartridge::ForceConfiguration* config, bool pauseAtResetVector)
{
    (void)config; // unused
    if (m_pCartridge->LoadFromNull())
    {
        //if (IsValidPointer(config))            m_pCartridge->ForceConfig(*config);

        Reset(pauseAtResetVector);

        m_pMemory->ResetRomDisassembledMemory();
        ObserveCurrentInstruction();

        return true;
    }
    else
        return false;
}

bool GearSF7000Core::LoadROMFromBuffer(const u8* buffer, int size, Cartridge::ForceConfiguration* config, bool pauseAtResetVector)
{
    if (m_pCartridge->LoadFromBuffer(buffer, size))
    {
        if (IsValidPointer(config))
            m_pCartridge->ForceConfig(*config);

        Reset(pauseAtResetVector);

        m_pMemory->ResetRomDisassembledMemory();
        ObserveCurrentInstruction();

        return true;
    }
    else
        return false;
}

void GearSF7000Core::SaveDisassembledROM()
{
    Memory::stDisassembleRecord** biosMap = m_pMemory->GetDisassembledBiosMemoryMap();
    Memory::stDisassembleRecord** romMap = m_pMemory->GetDisassembledRomMemoryMap();

    if (m_pCartridge->IsReady() && (strlen(m_pCartridge->GetFilePath()) > 0) && IsValidPointer(romMap))
    {
        using namespace std;

        char path[512];

        strcpy(path, m_pCartridge->GetFilePath());
        strcat(path, ".dis");

        Log("Saving Disassembled ROM %s...", path);

        ofstream myfile(path, ios::out | ios::trunc);

        if (myfile.is_open())
        {
            #define PAD_ADDR(digits) std::uppercase << std::hex << std::setw(digits) << std::setfill('0')
            #define PAD_MEM(chars) std::setw(chars) << std::setfill(' ')

            for (int i = 0; i < 0x2000; i++)
            {
                if (IsValidPointer(biosMap[i]) && (biosMap[i]->name[0] != 0))
                {
                    myfile << "BIOS $" << PAD_ADDR(4) << i << "   " << PAD_MEM(12) << biosMap[i]->bytes << "  " << biosMap[i]->name << "\n";
                }
            }

            for (int i = 0; i < MAX_ROM_SIZE; i++)
            {
                if (IsValidPointer(romMap[i]) && (romMap[i]->name[0] != 0))
                {
                    myfile << "ROM  $" << PAD_ADDR(4) << i + 0x8000 << "   " << PAD_MEM(12) << romMap[i]->bytes << "  " << romMap[i]->name << "\n";
                }
            }

            myfile.close();
        }

        Log("Disassembled ROM Saved");
    }
}

bool GearSF7000Core::GetRuntimeInfo(GC_RuntimeInfo& runtime_info)
{
    const GC_VideoFrameDescriptor descriptor = GetVideoFrameDescriptor();
    runtime_info.screen_width = descriptor.frame_width;
    runtime_info.screen_height = descriptor.frame_height;
    runtime_info.region = descriptor.region;

    if (IsMachineReady())
    {
        return true;
    }

    return false;
}

GC_VideoFrameDescriptor GearSF7000Core::GetVideoFrameDescriptor() const
{
    return m_pVideo ? m_pVideo->GetFrameDescriptor()
                    : GC_VideoFrameDescriptor{};
}

void GearSF7000Core::RenderCurrentFrame(u8* pFrameBuffer)
{
    if (pFrameBuffer != NULL)
        RenderFrameBuffer(pFrameBuffer);
}

Memory* GearSF7000Core::GetMemory()
{
    return m_pMemory;
}

Cartridge* GearSF7000Core::GetCartridge()
{
    return m_pCartridge;
}

CpuStateAccess* GearSF7000Core::GetCpuStateAccess()
{
    return m_pCpuExecution;
}

const char* GearSF7000Core::GetCpuBackendName() const
{
    return "CLK";
}

bool GearSF7000Core::IsCpuStatePersistenceAvailable() const
{
    return m_pCpuStatePersistence != nullptr;
}

Z80Disassembler* GearSF7000Core::GetDisassembler()
{
    return m_pDisassembler;
}

Audio* GearSF7000Core::GetAudio()
{
    return m_pAudio;
}

Video* GearSF7000Core::GetVideo()
{
    return m_pVideo;
}

SR1000* GearSF7000Core::GetCassette()
{
    return m_pSR1000;
}

SP400* GearSF7000Core::GetSP400()
{
    return m_pSP400;
}

SF7000* GearSF7000Core::GetSF7000()
{
    return m_pSF7000;
}
MachineIOPorts* GearSF7000Core::GetMachineIOPorts()
{
    return m_pMachineIOPorts;
}


SR1000Speaker* GearSF7000Core::GetCassetteSpeaker()
{
#if GEARSF7000_ENABLE_SR1000
    return m_pAudio->GetSR1000Speaker();
#else
    return nullptr;
#endif
}

void GearSF7000Core::PauseKeyPressed()
{
    m_pSK1100->PauseKeyPressed();
}

bool GearSF7000Core::QueueKeyboardText(const std::string& text, std::string* error)
{
    return m_pSK1100->QueueText(text, error);
}

void GearSF7000Core::SetKeyboardTextTiming(int pressPolls, int releasePolls, int newlinePolls)
{
    m_pSK1100->SetTextTiming(pressPolls, releasePolls, newlinePolls);
}

void GearSF7000Core::ClearKeyboardText()
{
    m_pSK1100->ClearTextQueue();
}

float GearSF7000Core::GetKeyboardTextProgress() const
{
    const size_t total = m_pSK1100->GetTextQueueSize();
    if (total == 0)
        return 1.0f;
    return (float)m_pSK1100->GetTextQueueIndex() / (float)total;
}

void GearSF7000Core::EnableEvents(bool enabled)
{
    enabledEvents = enabled;
}

void GearSF7000Core::SetKeyboardMode(bool enabled)
{
    m_pSK1100->SetKeyboardMode(enabled);
}

#if GEARSF7000_ENABLE_SDL_INPUT
void GearSF7000Core::SetEvent(SDL_Event event)
{
    if (enabledEvents)
        m_pSK1100->SetEvent(event);
}
#endif


void GearSF7000Core::JoystickPressed(GC_Controllers controller, GC_Keys key)
{
    m_pSK1100->JoystickPressed(controller, key);
}

void GearSF7000Core::JoystickReleased(GC_Controllers controller, GC_Keys key)
{
    m_pSK1100->JoystickReleased(controller, key);
}

void GearSF7000Core::GetInputRows(uint16_t rows[8]) const
{
    m_pSK1100->GetRows(rows);
}

bool GearSF7000Core::KeyboardKey(const std::string& label, bool pressed)
{
    return m_pSK1100->PressMatrixKeyByLabel(label, pressed);
}

void GearSF7000Core::KeyboardMatrixKey(int row, int mask, bool pressed)
{
    m_pSK1100->SetMatrixKeyState(row, mask, pressed);
}


void GearSF7000Core::Pause(bool paused)
{
    if (paused)
    {
        Log("GearSF7000 PAUSED");
    }
    else
    {
        Log("GearSF7000 RESUMED");
    }
    m_bPaused = paused;
}

bool GearSF7000Core::IsPaused()
{
    return m_bPaused;
}

void GearSF7000Core::ResetROM(Cartridge::ForceConfiguration* config, bool pauseAtResetVector)
{
    if (m_pCartridge->IsReady())
    {
        Log("GearSF7000 RESET");

        if (IsValidPointer(config))
            m_pCartridge->ForceConfig(*config);

        Reset(pauseAtResetVector);

        ObserveCurrentInstruction();
    }
}

void GearSF7000Core::EjectCartridge()
{
    Log("GearSF7000 EJECT");

    // Cartridge::Reset() frees the ROM and clears IsReady()/IsPAL()/CRC back
    // to their pre-load defaults, same as a fresh Cartridge. Unlike ResetROM,
    // this must run unconditionally: there is no cartridge left to gate on.
    m_pCartridge->Reset();

    Reset();

    // Same cleanup LoadROM does after a fresh load, so stale disassembly
    // records (and any breakpoints set against the outgoing ROM's addresses)
    // do not linger and read as though the old cartridge were still there.
    m_pMemory->ResetRomDisassembledMemory();

    // Deliberately no ObserveCurrentInstruction() here. ResetROM only ever
    // calls it inside its own IsReady() guard, because the disassembler's
    // memory read for the current PC assumes a cartridge is mapped; calling
    // it right after ejecting, with no ROM behind PC 0, crashed with a
    // SIGSEGV at the PC value itself rather than a real address.
}

void GearSF7000Core::StartSF7000(bool pauseAtResetVector)
{
    // The SF-7000 is an external Control Station. Its I/O cartridge supplies
    // IPL/RAM mapping but does not eject the SC-3000 cartridge.
    m_pMemory->EnableSF7000(true);
    Reset(pauseAtResetVector);
    ObserveCurrentInstruction();
}

void GearSF7000Core::ResetROMPreservingRAM(Cartridge::ForceConfiguration* config)
{
    // TODO

    ResetROM(config);
}

void GearSF7000Core::ResetSound()
{
    m_pAudio->Reset(m_pCartridge->IsPAL());
}

void GearSF7000Core::SaveRam()
{
    SaveRam(NULL);
}

void GearSF7000Core::SaveRam(const char*, bool)
{
    // TODO
}

void GearSF7000Core::LoadRam()
{
    LoadRam(NULL);
}

void GearSF7000Core::LoadRam(const char*, bool)
{
    // TODO
}

bool GearSF7000Core::SaveState(int index)
{
    Log("Creating save state %d...", index);

    const bool ok = SaveState(NULL, index);

    Log(ok ? "Save state %d created" : "Save state %d failed", index);
    return ok;
}

bool GearSF7000Core::SaveState(const char* szPath, int index,
                               size_t* bytesWritten)
{
    Log("Creating save state...");

    using namespace std;

    if (bytesWritten)
        *bytesWritten = 0;

    // Serialized once, into memory, and only then committed to disk. The
    // previous shape built the state twice - once to measure it, once to
    // write it - and reported success without looking at either result,
    // which is how a zero-byte file could be announced as saved.
    stringstream state(ios::in | ios::out | ios::binary);
    size_t size = 0;
    if (!SaveState(state, size))
    {
        Log("Save state failed: the machine could not be serialized.");
        return false;
    }

    const string bytes = state.str();
    if (bytes.size() != size)
    {
        Log("Save state failed: expected %d bytes, produced %d.",
            static_cast<int>(size), static_cast<int>(bytes.size()));
        return false;
    }

    string path = "";

    if (IsValidPointer(szPath))
    {
        path += szPath;
        path += "/";
        path += m_pCartridge->GetFileName();
    }
    else
    {
        path = m_pCartridge->GetFilePath();
    }

    string::size_type i = path.rfind('.', path.length());

    if (i != string::npos) {
        path.replace(i + 1, 3, "state");
    }

    std::stringstream sstm;

    if (index < 0)
        sstm << szPath;
    else
        sstm << path << index;

    Log("Save state file: %s", sstm.str().c_str());

    ofstream file(sstm.str().c_str(), ios::out | ios::binary);
    if (!file)
    {
        Log("Save state failed: could not open %s for writing.",
            sstm.str().c_str());
        return false;
    }

    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();

    if (!file)
    {
        Log("Save state failed: the write to %s did not complete.",
            sstm.str().c_str());
        return false;
    }

    if (bytesWritten)
        *bytesWritten = bytes.size();

    Log("Save state created [%d bytes]", static_cast<int>(bytes.size()));
    return true;
}

bool GearSF7000Core::SaveState(u8* buffer, size_t& size)
{
    if (!m_pCartridge->IsReady())
    {
        Log("Invalid rom.");
        return false;
    }

    using namespace std;

    stringstream stream(ios::in | ios::out | ios::binary);

    // A failed serialization is a failure whether or not a destination
    // buffer was supplied. The previous shape returned true for any valid
    // pointer, which turned an aborted save into a reported success.
    if (!SaveState(stream, size))
        return false;

    if (!IsValidPointer(buffer))
        return true;

    const string bytes = stream.str();
    if (bytes.size() < size)
    {
        Log("Save state failed: produced %d bytes, expected at least %d.",
            static_cast<int>(bytes.size()), static_cast<int>(size));
        return false;
    }

    Log("Saving state to buffer [%d bytes]...", static_cast<int>(size));
    memcpy(buffer, bytes.data(), size);
    return true;
}

namespace
{
// Snapshot container.
//
// The body is a list of independently sized sections rather than one fixed
// field order. Two things follow from that. A build that does not know a
// section can step over exactly its bytes instead of losing sync with
// everything after it, so a device can be added later without invalidating
// what already works. And a section that does not apply to how the machine is
// currently wired is simply not written: in plain cartridge mode the SF-7000
// block is absent, which is worth about 4 KB per frame of sector buffer that
// would otherwise be recorded for nothing.
//
// The header carries enough identity to refuse a mismatch up front, before a
// single device has been handed a byte: media mode, region, cartridge type,
// the recognised mapper, and the ROM's CRC.
constexpr u16 kStateFormatVersion = 1;

constexpr u32 FourCC(char a, char b, char c, char d)
{
    return static_cast<u32>(static_cast<unsigned char>(a)) |
           (static_cast<u32>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<u32>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<u32>(static_cast<unsigned char>(d)) << 24);
}

constexpr u32 kSectionEnd = 0;
constexpr u32 kSectionMemory = FourCC('M', 'E', 'M', ' ');
constexpr u32 kSectionCpu = FourCC('C', 'P', 'U', ' ');
constexpr u32 kSectionClocks = FourCC('C', 'L', 'K', 'D');
constexpr u32 kSectionVideo = FourCC('V', 'D', 'P', ' ');
constexpr u32 kSectionAudio = FourCC('P', 'S', 'G', ' ');
constexpr u32 kSectionKeyboard = FourCC('K', 'E', 'Y', 'B');
constexpr u32 kSectionPPI = FourCC('P', 'P', 'I', ' ');
constexpr u32 kSectionSF7000 = FourCC('S', 'F', '7', '0');

enum : u8
{
    kMediaCartridgeRom = 0,
    kMediaSF7000 = 1
};
}

const char* GearSF7000Core::GetLastStateError() const
{
    return m_LastStateError.c_str();
}

namespace
{
// Formats a refusal into the caller-visible reason. The leading token is
// stable so a client can branch on it; the rest is for a human reading a log.
std::string StateReason(const char* format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return std::string(buffer);
}
}

u16 GearSF7000Core::GetSaveStateFormatVersion()
{
    return kStateFormatVersion;
}

bool GearSF7000Core::SaveState(std::ostream& stream, size_t& size)
{
    if (!m_pCartridge->IsReady())
    {
        m_LastStateError = StateReason("Invalid rom.");
        Log("%s", m_LastStateError.c_str());
        return false;
    }

    if (!m_pCpuStatePersistence)
    {
        m_LastStateError = StateReason("Save states are not supported by the active CPU backend.");
        Log("%s", m_LastStateError.c_str());
        return false;
    }

    const bool sf7000Mode = m_pMemory->IsSF7000Enabled();

    StateWriter w(stream);

    w.U32(GC_SAVESTATE_MAGIC);
    w.U16(kStateFormatVersion);
    w.U8(sf7000Mode ? kMediaSF7000 : kMediaCartridgeRom);
    w.U8(m_pVideo->IsPAL() ? 1 : 0);
    w.U32(static_cast<u32>(m_pCartridge->GetType()));
    w.U32(m_pMemory->GetMapperId());
    w.U32(m_pCartridge->GetCRC());
    w.U64(m_pVideo->GetFrameRenderDiagnostics().frameSerial);

    bool ok = true;
    const auto section = [&](u32 id, u16 version, auto&& write)
    {
        if (!ok)
            return;

        std::stringstream payload(std::ios::in | std::ios::out |
                                  std::ios::binary);
        write(payload);
        if (!payload)
        {
            ok = false;
            return;
        }

        const std::string bytes = payload.str();
        w.U32(id);
        w.U16(version);
        w.U32(static_cast<u32>(bytes.size()));
        w.Bytes(bytes.data(), bytes.size());
    };

    section(kSectionMemory, 1,
            [&](std::ostream& out) { m_pMemory->SaveState(out); });
    section(kSectionCpu, 1, [&](std::ostream& out)
            {
                if (!m_pCpuStatePersistence->SaveCpuState(out))
                    ok = false;
            });
    section(kSectionClocks, 1,
            [&](std::ostream& out) { m_clockDomains.SaveState(out); });
    section(kSectionVideo, 1,
            [&](std::ostream& out) { m_pVideo->SaveState(out); });
    section(kSectionAudio, 1,
            [&](std::ostream& out) { m_pAudio->SaveState(out); });
    section(kSectionKeyboard, 1,
            [&](std::ostream& out) { m_pSK1100->SaveState(out); });
    section(kSectionPPI, 1, [&](std::ostream& out)
            {
                if (m_pMachineIOPorts)
                    m_pMachineIOPorts->SaveState(out);
            });

    // Only in SF-7000 mode. In cartridge mode there is no disk, no second
    // PPI and no controller worth describing, and the section's absence is
    // what tells a loader that.
#if GEARSF7000_ENABLE_SF7000
    if (sf7000Mode && m_pSF7000)
        section(kSectionSF7000, 1,
                [&](std::ostream& out) { m_pSF7000->SaveState(out); });
#endif

    if (!ok)
    {
        m_LastStateError = StateReason("Save state aborted: a device refused to serialize.");
        Log("%s", m_LastStateError.c_str());
        return false;
    }

    w.U32(kSectionEnd);

    size = static_cast<size_t>(stream.tellp()) + (sizeof(u32) * 2);

    w.U32(GC_SAVESTATE_MAGIC);
    w.U32(static_cast<u32>(size));

    if (!w.Ok())
    {
        m_LastStateError = StateReason("Save state aborted: the stream went bad while writing.");
        Log("%s", m_LastStateError.c_str());
        return false;
    }

    m_LastStateError.clear();
    Log("Save state size: %d", static_cast<int>(size));
    return true;
}

bool GearSF7000Core::LoadState(int index)
{
    Log("Loading save state %d...", index);

    const bool ok = LoadState(NULL, index);

    Log(ok ? "State %d file loaded" : "State %d could not be loaded", index);
    return ok;
}

bool GearSF7000Core::LoadState(const char* szPath, int index)
{
    Log("Loading save state...");

    using namespace std;

    string sav_path = "";

    if (IsValidPointer(szPath))
    {
        sav_path += szPath;
        sav_path += "/";
        sav_path += m_pCartridge->GetFileName();
    }
    else
    {
        sav_path = m_pCartridge->GetFilePath();
    }

    string::size_type i = sav_path.rfind('.', sav_path.length());

    if (i != string::npos) {
        sav_path.replace(i + 1, 3, "state");
    }

    std::stringstream sstm;

    if (index < 0)
        sstm << szPath;
    else
        sstm << sav_path << index;

    Log("Opening save file: %s", sstm.str().c_str());

    ifstream file(sstm.str().c_str(), ios::in | ios::binary);

    if (file.fail())
    {
        Log("Save state file doesn't exist: %s", sstm.str().c_str());
        return false;
    }

    const bool ok = LoadState(file);
    file.close();

    Log(ok ? "Save state loaded" : "Save state was refused");
    return ok;
}

bool GearSF7000Core::LoadState(const u8* buffer, size_t size)
{
    if (m_pCartridge->IsReady() && (size > 0) && IsValidPointer(buffer))
    {
        Log("Gathering load state data [%d bytes]...", size);

        using namespace std;

        stringstream stream;

        stream.write(reinterpret_cast<const char*> (buffer), size);

        return LoadState(stream);
    }

    Log("Invalid rom or memory.");

    return false;
}

bool GearSF7000Core::LoadState(std::istream& stream)
{
    if (!m_pCartridge->IsReady())
    {
        m_LastStateError = StateReason("Invalid rom");
        Log("%s", m_LastStateError.c_str());
        return false;
    }

    if (!m_pCpuStatePersistence)
    {
        m_LastStateError = StateReason("Save states are not supported by the active CPU backend.");
        Log("%s", m_LastStateError.c_str());
        return false;
    }

    using namespace std;

    stream.seekg(0, ios::end);
    const size_t streamSize = static_cast<size_t>(stream.tellg());
    if (streamSize <= sizeof(u32) * 2)
    {
        m_LastStateError = StateReason("Load state: stream is too short to be a snapshot (%d bytes).",
            static_cast<int>(streamSize));
        Log("%s", m_LastStateError.c_str());
        return false;
    }

    // Trailer first: it is the cheapest way to tell a truncated file from a
    // complete one before anything is applied.
    stream.seekg(static_cast<std::streamoff>(streamSize - (2 * sizeof(u32))),
                 ios::beg);
    {
        StateReader trailer(stream);
        const u32 trailerMagic = trailer.U32();
        const u32 trailerSize = trailer.U32();
        if (!trailer.Ok() || trailerMagic != GC_SAVESTATE_MAGIC ||
            trailerSize != static_cast<u32>(streamSize))
        {
            m_LastStateError = StateReason("Load state: trailer says magic 0x%08X size %u, stream is %d "
                "bytes. Refusing.",
                trailerMagic, trailerSize, static_cast<int>(streamSize));
            Log("%s", m_LastStateError.c_str());
            return false;
        }
    }

    stream.seekg(0, ios::beg);
    StateReader r(stream);

    if (r.U32() != GC_SAVESTATE_MAGIC)
    {
        m_LastStateError = StateReason("Load state: not a GearSF7000 snapshot.");
        Log("%s", m_LastStateError.c_str());
        return false;
    }

    const u16 formatVersion = r.U16();
    if (formatVersion != kStateFormatVersion)
    {
        m_LastStateError = StateReason("Load state: format version %u, this build writes %u.",
            static_cast<unsigned>(formatVersion),
            static_cast<unsigned>(kStateFormatVersion));
        Log("%s", m_LastStateError.c_str());
        return false;
    }

    const u8 mediaMode = r.U8();
    const u8 region = r.U8();
    const u32 cartridgeType = r.U32();
    const u32 mapperId = r.U32();
    const u32 romCrc = r.U32();
    r.U64(); // frame serial, informational

    if (!r.Ok())
    {
        m_LastStateError = StateReason("Load state: header is truncated.");
        Log("%s", m_LastStateError.c_str());
        return false;
    }

    // Every mismatch below is refused with its own reason rather than being
    // patched over. Applying a snapshot to a machine wired differently would
    // succeed quietly and then diverge somewhere far away from here.
    const u8 currentMedia =
        m_pMemory->IsSF7000Enabled() ? kMediaSF7000 : kMediaCartridgeRom;
    if (mediaMode != currentMedia)
    {
        m_LastStateError = StateReason("Load state: snapshot is %s, the machine is %s.",
            mediaMode == kMediaSF7000 ? "SF-7000" : "cartridge ROM",
            currentMedia == kMediaSF7000 ? "SF-7000" : "cartridge ROM");
        Log("%s", m_LastStateError.c_str());
        return false;
    }

    if ((region != 0) != m_pVideo->IsPAL())
    {
        m_LastStateError = StateReason("Load state: snapshot is %s, the machine is %s.",
            region ? "PAL" : "NTSC", m_pVideo->IsPAL() ? "PAL" : "NTSC");
        Log("%s", m_LastStateError.c_str());
        return false;
    }

    if (cartridgeType != static_cast<u32>(m_pCartridge->GetType()))
    {
        m_LastStateError = StateReason("Load state: snapshot cartridge type %u, loaded cartridge is %u.",
            cartridgeType, static_cast<u32>(m_pCartridge->GetType()));
        Log("%s", m_LastStateError.c_str());
        return false;
    }

    if (mapperId != m_pMemory->GetMapperId())
    {
        m_LastStateError = StateReason("Load state: snapshot mapper 0x%08X, installed mapper 0x%08X.",
            mapperId, m_pMemory->GetMapperId());
        Log("%s", m_LastStateError.c_str());
        return false;
    }

    if (romCrc != m_pCartridge->GetCRC())
    {
        m_LastStateError = StateReason("Load state: snapshot ROM CRC 0x%08X, loaded ROM is 0x%08X.",
            romCrc, m_pCartridge->GetCRC());
        Log("%s", m_LastStateError.c_str());
        return false;
    }

    bool ok = true;
    const auto apply = [&](const std::string& bytes, auto&& read)
    {
        std::stringstream payload(bytes, std::ios::in | std::ios::binary);
        read(payload);
    };

    for (;;)
    {
        const u32 id = r.U32();
        if (!r.Ok())
        {
            m_LastStateError = StateReason("Load state: section list ended without a terminator.");
            Log("%s", m_LastStateError.c_str());
            return false;
        }
        if (id == kSectionEnd)
            break;

        const u16 version = r.U16();
        const u32 length = r.U32();
        if (!r.Ok())
        {
            m_LastStateError = StateReason("Load state: section header is truncated.");
            Log("%s", m_LastStateError.c_str());
            return false;
        }

        std::string bytes;
        if (length > 0)
        {
            bytes.resize(length);
            r.Bytes(&bytes[0], length);
            if (!r.Ok())
            {
                m_LastStateError = StateReason("Load state: section 0x%08X claims %u bytes that are not "
                    "there.", id, length);
                Log("%s", m_LastStateError.c_str());
                return false;
            }
        }

        switch (id)
        {
            case kSectionMemory:
                apply(bytes, [&](std::istream& in) { m_pMemory->LoadState(in); });
                break;

            case kSectionCpu:
                apply(bytes, [&](std::istream& in)
                      {
                          if (!m_pCpuStatePersistence->LoadCpuState(in))
                              ok = false;
                      });
                break;

            case kSectionClocks:
                apply(bytes, [&](std::istream& in)
                      {
                          if (!m_clockDomains.LoadState(in))
                              ok = false;
                      });
                break;

            case kSectionVideo:
                apply(bytes, [&](std::istream& in) { m_pVideo->LoadState(in); });
                break;

            case kSectionAudio:
                apply(bytes, [&](std::istream& in) { m_pAudio->LoadState(in); });
                break;

            case kSectionKeyboard:
                apply(bytes, [&](std::istream& in) { m_pSK1100->LoadState(in); });
                break;

            case kSectionPPI:
                if (m_pMachineIOPorts)
                    apply(bytes, [&](std::istream& in)
                          { m_pMachineIOPorts->LoadState(in); });
                break;

#if GEARSF7000_ENABLE_SF7000
            case kSectionSF7000:
                if (m_pSF7000)
                    apply(bytes, [&](std::istream& in)
                          {
                              if (!m_pSF7000->LoadState(in))
                                  ok = false;
                          });
                break;
#endif

            default:
                // Written by a build that knows a device this one does not.
                // The length is exactly why it is recorded.
                m_LastStateError = StateReason("Load state: skipping unknown section 0x%08X v%u (%u "
                    "bytes).", id, static_cast<unsigned>(version), length);
                break;
        }

        if (!ok)
        {
            Log("Load state: section 0x%08X was rejected by its device.", id);
                Log("%s", m_LastStateError.c_str());
            return false;
        }
    }

    m_pCpuInterruptRouter->SetMaskableInterruptLine(
        m_pCpuExecution->GetCpuStateSnapshot().intLine);

    m_LastStateError.clear();
    return true;
}

namespace
{
// Master clocks in one raster line: 342 dots at two master clocks each.
constexpr double kMasterClocksPerLine = 684.0;

}

double GearSF7000Core::GetNativeFrameRate() const
{
    const bool pal = m_pVideo ? m_pVideo->IsPAL() : false;
    const GearSF7000ClockRates rates = pal ? GearSF7000ClockRate::SC3000PAL
                                           : GearSF7000ClockRate::SC3000NTSC;
    const double lines = pal ? GC_LINES_PER_FRAME_PAL : GC_LINES_PER_FRAME_NTSC;
    return static_cast<double>(rates.vdpMasterHz) /
           (kMasterClocksPerLine * lines);
}

void GearSF7000Core::SetTargetFrameRate(double fps)
{
    m_RequestedFrameRate = (fps > 1.0) ? fps : 0.0;
}

double GearSF7000Core::GetClockScale() const
{
    return m_ActiveClockScale;
}

double GearSF7000Core::GetEffectiveFrameRate() const
{
    return GetNativeFrameRate() * m_ActiveClockScale;
}

void GearSF7000Core::Reset(bool pauseAtResetVector)
{
    const bool pal = m_pCartridge->IsPAL();

    GearSF7000ClockRates rates = pal ? GearSF7000ClockRate::SC3000PAL
                                     : GearSF7000ClockRate::SC3000NTSC;

    // One multiplier, every oscillator. Scaling only some of them would move
    // the domains relative to each other, which is the one thing that would
    // actually change how the machine behaves.
    const double lines = pal ? GC_LINES_PER_FRAME_PAL : GC_LINES_PER_FRAME_NTSC;
    const double native =
        static_cast<double>(rates.vdpMasterHz) / (kMasterClocksPerLine * lines);

    // Aligned to the host display when asked, otherwise the real crystals.
    //
    // Locking the machine to the vertical blank (see scheduler.cpp) makes it
    // produce one frame per refresh, so on a 60 Hz display an NTSC machine
    // runs 60 frames a wall second while its clocks still say 59.9227. That
    // works - the audio resampler absorbs the 0.13% - but it leaves the
    // machine's own idea of a second permanently 0.13% short. Scaling every
    // oscillator by the same factor removes the discrepancy instead of
    // compensating it: a second of emulated time becomes a second of real
    // time, and 48000 samples come out of a frame that produces exactly
    // 48000/60 of them.
    //
    // Every domain or none. Scaling some would move them relative to each
    // other, which is the one thing that would really change how the machine
    // behaves; scaling all of them leaves every ratio - T-states per line,
    // master clocks per dot, cycles per frame - exactly as it was.
    m_ActiveClockScale = 1.0;
    if (m_RequestedFrameRate > 1.0 && native > 1.0)
    {
        const double scale = m_RequestedFrameRate / native;
        // A refusal rather than a silent stretch: anything beyond a couple of
        // percent is not a display alignment, it is a mistake, and running
        // the machine at it would be worse than ignoring the request.
        if (scale > 0.95 && scale < 1.05)
            m_ActiveClockScale = scale;
    }

    if (m_ActiveClockScale != 1.0)
    {
        rates.cpuHz = static_cast<std::uint32_t>(
            static_cast<double>(rates.cpuHz) * m_ActiveClockScale + 0.5);
        rates.vdpMasterHz = static_cast<std::uint32_t>(
            static_cast<double>(rates.vdpMasterHz) * m_ActiveClockScale + 0.5);
        // The FDC runs off the SF-7000's own 8 MHz oscillator, which is not
        // on the machine's board and does not follow it anywhere.
    }

    const int cpuHz = static_cast<int>(rates.cpuHz);

    systemClock.Init(cpuHz);
    m_clockDomains.Configure(rates);
    m_clkMachineVBlank = false;

    m_pMemory->Reset();
    m_pCpuInterruptRouter->SetMaskableInterruptLine(false);
    m_pCpuExecution->ResetExecution();
    m_pAudio->Reset(pal, rates.cpuHz);

    m_pVideo->Reset(pal);
    m_pSK1100->Reset();
#if GEARSF7000_ENABLE_SR1000
    m_pSR1000->Reset(cpuHz);
#endif
#if GEARSF7000_ENABLE_SF7000
    m_pSF7000->Reset(true, rates.cpuHz, rates.fdcHz);
#endif
#if GEARSF7000_ENABLE_SP400
    m_pSP400->Reset();
#endif
    m_pMachineIOPorts->Reset();

    if (pauseAtResetVector)
    {
        // ResetExecution() only requests a power-on reset; the CLK core
        // defers actually applying it (clearing PC to the reset vector, and
        // whatever else a real Z80 reset sequence takes) to the next time it
        // is asked to run at all - CLKZ80Processor::RunToNextInstructionBoundary
        // special-cases this as a single boundary so $0000 becomes observable
        // before its instruction executes. Pausing the machine outright,
        // as the plain reset below does, means that quantum never runs: PC
        // stays at whatever it held from before this reset, not 0000. One
        // ExecuteInstruction() call consumes exactly that quantum - the reset
        // completing, not the real first opcode - then the machine stops.
        m_pCpuExecution->ExecuteInstruction();
        m_bPaused = true;
    }
    else
    {
        m_bPaused = false;
    }
}

void GearSF7000Core::RenderFrameBuffer(u8* finalFrameBuffer)
{
    if (m_pVideo->IsFullRasterDebugEnabled())
    {
        // Already packed RGB888 at the full 342-dot raster - the debug
        // buffer bypasses the palette-index path entirely, it is not
        // content indexed against m_pVideo->GetFrameBuffer().
        int width = 0;
        int height = 0;
        const u8* debugBuffer = m_pVideo->GetFullRasterDebugBuffer(width, height);
        if (debugBuffer != NULL)
            memcpy(finalFrameBuffer, debugBuffer,
                static_cast<size_t>(width) * height * 3);
        return;
    }

    const GC_VideoFrameDescriptor descriptor = GetVideoFrameDescriptor();
    const int size = descriptor.frame_width * descriptor.frame_height;
    // A loaded IPL was historically enough for the desktop frontend to show
    // the ordinary VDP framebuffer while waiting for the user to start the
    // SF-7000. Reduced targets have no IPL, so their loaded cartridge still
    // needs IsMachineReady(). Do not make the old NO BIOS artwork reappear in
    // the full application merely because the SF-7000 has not been enabled.
    const bool hasDisplayableMachine =
        IsMachineReady() || m_pMemory->IsBiosLoaded();
    u16* srcBuffer = hasDisplayableMachine
        ? m_pVideo->GetFrameBuffer()
        : kNoBiosImage;

    switch (m_pixelFormat)
    {
        case GC_PIXEL_RGB555:
        case GC_PIXEL_BGR555:
        case GC_PIXEL_RGB565:
        case GC_PIXEL_BGR565:
        {
            m_pVideo->Render16bit(srcBuffer, finalFrameBuffer, m_pixelFormat, size, true);
            break;
        }
        case GC_PIXEL_RGB888:
        case GC_PIXEL_BGR888:
        {
            m_pVideo->Render24bit(srcBuffer, finalFrameBuffer, m_pixelFormat, size, true);
            break;
        }
    }
}

void GearSF7000Core::DiskShutdown()
{
#if GEARSF7000_ENABLE_SF7000
    for (int i = 0; i < MAXDRIVES; i++)
        diskChange(i, NULL);   // triggera l'auto-save dirty + free memoria
#endif
}

uint8_t GearSF7000Core::DiskChange(int driveId, const char* fileName, bool* isReadOnly)
{
#if GEARSF7000_ENABLE_SF7000
    uint8_t ret = diskChange(driveId, fileName);
//    nec765LogClear();
    nec765LogDiskEject(driveId);
    nec765LogDiskLoad(driveId, fileName);

    if (ret)
    {
        //diskEnable(driveId, 1);

        *isReadOnly = diskIsReadOnly(driveId);

        m_pSF7000->FDC765_DiskChanged(driveId);
    }

    return ret;
#else
    (void)driveId; (void)fileName; (void)isReadOnly;
    return 0;
#endif
}

void GearSF7000Core::DiskEject(int driveId)
{
#if GEARSF7000_ENABLE_SF7000
    diskEject(driveId);
    //nec765LogClear();
    nec765LogDiskEject(driveId);

    //diskEnable(driveId, 0);
#else
    (void)driveId;
#endif
}

void GearSF7000Core::DiskWriteProtect(int driveId, bool writeProtected)
{
#if GEARSF7000_ENABLE_SF7000
    m_pSF7000->FDC765_SetReadOnly(driveId, writeProtected);
    diskReadOnly(driveId, writeProtected ? 1 : 0);
#else
    (void)driveId; (void)writeProtected;
#endif
}



uint8_t GearSF7000Core::CassetteChange(const char* fileName, bool* isReadOnly)
{
#if GEARSF7000_ENABLE_SR1000
    *isReadOnly = true;

    bool loaded = m_pSR1000->LoadBitTape(std::string(fileName));

    return loaded ? 1 : 0;
#else
    (void)fileName; (void)isReadOnly;
    return 0;
#endif
}

void GearSF7000Core::CassetteEject()
{
#if GEARSF7000_ENABLE_SR1000
    m_pSR1000->Eject();
#endif
}

void GearSF7000Core::CassetteWriteProtect(bool writeProtected)
{
    (void)writeProtected;
    //m_pSF7000->FDC765_SetReadOnly(driveId, writeProtected);
    //diskReadOnly(driveId, writeProtected ? 1 : 0);
}

void GearSF7000Core::CassettePlay()
{
#if GEARSF7000_ENABLE_SR1000
    m_pSR1000->Play();
#endif
}

void GearSF7000Core::CassetteStop()
{
#if GEARSF7000_ENABLE_SR1000
    m_pSR1000->Stop();
#endif
}

void GearSF7000Core::CassetteRewind()
{
#if GEARSF7000_ENABLE_SR1000
    m_pSR1000->Rewind();
#endif
}
