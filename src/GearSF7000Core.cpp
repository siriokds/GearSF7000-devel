#include "VideoCatalog.h"
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

#include <algorithm>
#include <cstring>
#include <iomanip>
#include "GearSF7000Core.h"
#include "build_info.h"
#include "Memory.h"
#include "cpu/CpuExecution.h"
#include "cpu/clk/CLKZ80Processor.h"
#include "cpu/CpuInterruptRouter.h"
#include "cpu/VdpInterruptAdapter.h"
#include "cpu/CpuInstructionObserver.h"
#include "cpu/CpuStatePersistence.h"
#include "MachineDebugEventProvider.h"
#include "Z80Disassembler.h"
#include "RomMap.h"
#include "Audio.h"
#include "IVideoDevice.h"
#include "IVideoDebugProvider.h"
#include "IVideoCrtSignalProvider.h"
#include "VideoDeviceFactory.h"
#include "SegaKeyboard.h"
#include "Input.h"
#include "SystemBusColeco.h"
#include "SystemBusMSX.h"
#include "SystemBusM5.h"
#if GEARSF7000_ENABLE_SR1000
#include "SR1000.h"
#endif
#if GEARSF7000_ENABLE_SF7000
#include "SF7000.h"
#endif
#include "SC3000SerialPrinterPort.h"
#include "SegaCassettePort.h"
#include "Cartridge.h"
#include "MachineIOPorts.h"
#include "SystemBusSC3000.h"
#include "SystemBusSG1000.h"
#if GEARSF7000_ENABLE_SMS
#include "SystemBusSMS.h"
#endif
#if GEARSF7000_ENABLE_GAMEGEAR
#include "SystemBusGG.h"
#endif
#if GEARSF7000_ENABLE_PICO9918
#include "Pico9918.h"
#endif
#include <stdexcept>
#include <filesystem>

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

namespace
{
const char* MachineDebugKey(MachineType type)
{
    switch (type)
    {
        case MachineType::SG1000: return "event.sg1000";
        case MachineType::SMS: return "event.sms";
        case MachineType::GameGear: return "event.gg";
        case MachineType::SordM5: return "event.m5";
        case MachineType::MSX: return "event.msx";
        case MachineType::Coleco: return "event.coleco";
        default: return "event.sc3000";
    }
}
}

GearSF7000Core::GearSF7000Core()
    : GearSF7000Core(CreateVideoDevice)
{
}

GearSF7000Core::GearSF7000Core(VideoDeviceCreateFunction videoDeviceFactory)
    : m_videoDeviceFactory(videoDeviceFactory != nullptr
          ? videoDeviceFactory : CreateVideoDevice)
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
    InitPointer(m_pColecoInput);
    InitPointer(m_pVdpInterruptLines);
    InitPointer(m_pCpuInstructionObservers);
    InitPointer(m_pDisassembler);
    InitPointer(m_pRomMap);
    InitPointer(m_pAudio);
    m_videoDeviceGeneration = 0;
    InitPointer(m_pKeyboard);
    InitPointer(m_pSR1000);
    InitPointer(m_pSF7000);
    InitPointer(m_pSerialPrinterPort);
    InitPointer(m_pCassettePort);
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
    nec765LogSave("nec765_logsave.log");
#endif

    if (m_pCLKProcessor) m_pCLKProcessor->GetBusHandler().SetSystemBus(nullptr);
    m_systemBus.reset();
    SafeDelete(m_pMachineIOPorts);
    SafeDelete(m_pCartridge);
#if GEARSF7000_ENABLE_SF7000
    SafeDelete(m_pSF7000);
#endif
    SafeDelete(m_pSerialPrinterPort);
    SafeDelete(m_pCassettePort);
#if GEARSF7000_ENABLE_SR1000
    SafeDelete(m_pSR1000);
#endif
    SafeDelete(m_pKeyboard);
    SafeDelete(m_pColecoInput);
    // Destroy the installed device while its bus, memory and interrupt
    // services are still alive. Future video implementations may need those
    // services during teardown even though the current TMS implementation
    // only releases its owned buffers.
    m_pVideo.reset();
    SafeDelete(m_pAudio);
    SafeDelete(m_pVdpInterruptLines);
    SafeDelete(m_pCpuInterruptRouter);
    SafeDelete(m_pCLKProcessor);
#ifndef GEARSF7000_DISABLE_DISASSEMBLER
    SafeDelete(m_pCpuInstructionObservers);
    if (m_pMemory)
        m_pMemory->SetRomMap(nullptr);
    SafeDelete(m_pRomMap);
    SafeDelete(m_pDisassembler);
#endif
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

bool GearSF7000Core::SetMachineType(MachineType type)
{
    if (type != MachineType::SC3000 && type != MachineType::SG1000 &&
        type != MachineType::SMS && type != MachineType::Coleco && type != MachineType::MSX &&
        type != MachineType::SordM5 && type != MachineType::GameGear)
        return false;
    return SetSystemModel(type == MachineType::SG1000
        ? SystemModel::SG1000
        : type == MachineType::SMS ? SystemModel::SMSNTSC
        : type == MachineType::GameGear ? SystemModel::GameGearExport
        : type == MachineType::SordM5 ? SystemModel::SordM5NTSC
        : type == MachineType::MSX ? SystemModel::MSX1NTSC
        : type == MachineType::Coleco ? SystemModel::ColecoVision
        : SystemModel::SC3000);
}

bool GearSF7000Core::ApplyMachineTypeAtReset(MachineType type)
{
    if (type != MachineType::SC3000 && type != MachineType::SG1000 &&
        type != MachineType::SMS && type != MachineType::Coleco && type != MachineType::MSX &&
        type != MachineType::SordM5 && type != MachineType::GameGear)
        return false;
    return ApplySystemModelAtReset(type == MachineType::SG1000
        ? SystemModel::SG1000
        : type == MachineType::SMS ? SystemModel::SMSNTSC
        : type == MachineType::GameGear ? SystemModel::GameGearExport
        : type == MachineType::SordM5 ? SystemModel::SordM5NTSC
        : type == MachineType::MSX ? SystemModel::MSX1NTSC
        : type == MachineType::Coleco ? SystemModel::ColecoVision
        : SystemModel::SC3000);
}

bool GearSF7000Core::SetSystemModel(SystemModel model)
{
#if !GEARSF7000_ENABLE_AY
    if (HasSGM(model) || IsMSXSystem(model)) return false;
#endif
    if (!IsValidSystemModel(model) || !GetDefaultMachineVideoOption(model))
        return false;
    const MachineType motherboard = GetSystemMotherboard(model);
    if (m_systemBus && motherboard != m_machineType)
        return false;
    m_systemModel = model;
    m_machineType = motherboard;
    const auto& options = GetMachineVideoOptions(model);
    const bool compatible = std::any_of(options.begin(), options.end(),
        [this](const auto* option) { return option->code == m_selectedVideoOption; });
    if (m_useDefaultVideoDevice || !compatible)
        SelectDefaultVideoDevice();
    return true;
}

bool GearSF7000Core::SelectVideoDevice(SystemVideoDevice device)
{
    const auto* option = GetVideoCatalog().FindLegacyOption(device);
    return option && SelectVideoOption(option->code);
}

bool GearSF7000Core::SelectVideoOption(const std::string& code)
{
    for (const auto* option : GetMachineVideoOptions(m_systemModel))
        if (option->code == code) {
            m_selectedVideoOption = code;
            m_selectedVideoDevice = option->legacyDevice;
            m_useDefaultVideoDevice = false;
            return true;
        }
    return false;
}

void GearSF7000Core::SelectDefaultVideoDevice()
{
    if (const auto* option = GetDefaultMachineVideoOption(m_systemModel)) {
        m_selectedVideoOption = option->code;
        m_selectedVideoDevice = option->legacyDevice;
        m_useDefaultVideoDevice = true;
    }
}

bool GearSF7000Core::ApplySystemModelAtReset(SystemModel model)
{
#if !GEARSF7000_ENABLE_AY
    if (HasSGM(model) || IsMSXSystem(model)) return false;
#endif
    if (!IsValidSystemModel(model) || !GetDefaultMachineVideoOption(model))
        return false;
    if (!m_systemBus)
        return SetSystemModel(model);

    const MachineType type = GetSystemMotherboard(model);
    if (model == m_systemModel)
        return true;

    // Construct before touching the live connection. unique_ptr assignment
    // then replaces the board in one step while emulation is stopped.
    std::unique_ptr<ISystemBus> replacement;
    if (type != m_machineType ||
        (type == MachineType::SG1000 &&
         HasSK1100(model) != HasSK1100(m_systemModel)) ||
        (type == MachineType::SC3000 &&
         (GetSystemModelDescriptor(model).cpuPsgClockHz !=
             GetSystemModelDescriptor(m_systemModel).cpuPsgClockHz ||
          IsSF7000System(model) != IsSF7000System(m_systemModel))) ||
        (type == MachineType::SMS && model != m_systemModel) ||
        (type == MachineType::GameGear && model != m_systemModel))
    {
        if (type == MachineType::SG1000)
            replacement = std::make_unique<SystemBusSG1000>(
                *m_pMemory, *m_pMachineIOPorts, HasSK1100(model));
#if GEARSF7000_ENABLE_SMS
        else if (type == MachineType::SMS)
            replacement = std::make_unique<SystemBusSMS>(
                *m_pMemory, *m_pMachineIOPorts,
                model == SystemModel::SMSPAL);
#endif
#if GEARSF7000_ENABLE_GAMEGEAR
        else if (type == MachineType::GameGear)
            replacement = std::make_unique<SystemBusGG>(
                *m_pMemory, *m_pMachineIOPorts, *m_pKeyboard, *m_pAudio,
                model == SystemModel::GameGearExport);
#endif
        else if (type == MachineType::SordM5)
            replacement = std::make_unique<SystemBusM5>(SystemBusM5::ProvisionalPsgWaitTStates, &m_debugEvents);
        else if (type == MachineType::MSX)
            replacement = std::make_unique<SystemBusMSX>(*m_pMemory, *m_pCartridge, *m_pAudio, *m_pMachineIOPorts);
        else if (type == MachineType::Coleco)
            replacement = std::make_unique<SystemBusColeco>(
                *m_pMemory, *m_pMachineIOPorts);
        else
            replacement = std::make_unique<SystemBusSC3000>(
                *m_pMemory, *m_pMachineIOPorts,
                GetSystemModelDescriptor(model).cpuPsgClockHz, IsSF7000System(model));
    }

    const MachineType previousType = m_machineType;
    const SystemModel previousModel = m_systemModel;
    const SystemVideoDevice previousVideoDevice = m_selectedVideoDevice;
    const std::string previousVideoOption = m_selectedVideoOption;
    const bool previousDefaultVideoDevice = m_useDefaultVideoDevice;
    const bool previousSF7000 = m_pMemory->IsSF7000Enabled();
    const SGMDevice previousSGM = m_sgm;
    std::unique_ptr<ISystemBus> previous;
    if (replacement)
    {
        previous = std::move(m_systemBus);
        m_systemBus = std::move(replacement);
        m_pCLKProcessor->GetBusHandler().SetSystemBus(m_systemBus.get());
        m_pVdpInterruptLines->SetLineType(m_systemBus->GetVdpLineType());
    ApplyBoardToRomMap();
    }
    m_machineType = type;
    m_systemModel = model;
    RefreshM5Connections();
    const auto& options = GetMachineVideoOptions(model);
    const bool compatible = std::any_of(options.begin(), options.end(),
        [this](const auto* option) { return option->code == m_selectedVideoOption; });
    if (m_useDefaultVideoDevice || !compatible)
        SelectDefaultVideoDevice();
    m_pMemory->EnableSF7000(IsSF7000System(model));
    m_sgm.Configure(HasSGM(model));
    m_pMemory->SetDebugMachineKey(MachineDebugKey(type));
    m_pMemory->SetSMSMotherboard(type == MachineType::SMS || type == MachineType::GameGear);
    m_pMemory->SetBoardRamSize(GetSystemModelDescriptor(model).boardRamSize);
    m_pMemory->SetColecoMotherboard(type == MachineType::Coleco);

    try
    {
        RebuildDebugEventCatalog();
        if (!m_pMemory->ConfigureBiosStorage(m_systemBus->GetBiosSize()))
            throw std::logic_error("System bus requested a non-power-of-two BIOS capacity");
    }
    catch (const std::exception& exception)
    {
        Log("Machine change rejected: %s", exception.what());
        m_machineType = previousType;
        m_systemModel = previousModel;
        m_selectedVideoDevice = previousVideoDevice;
        m_selectedVideoOption = previousVideoOption;
        m_useDefaultVideoDevice = previousDefaultVideoDevice;
        m_pMemory->EnableSF7000(previousSF7000);
        m_sgm = previousSGM;
        if (previous)
        {
            m_systemBus = std::move(previous);
            m_pCLKProcessor->GetBusHandler().SetSystemBus(m_systemBus.get());
            m_pVdpInterruptLines->SetLineType(m_systemBus->GetVdpLineType());
    ApplyBoardToRomMap();
        }
        m_pMemory->SetDebugMachineKey(MachineDebugKey(previousType));
        m_pMemory->SetSMSMotherboard(previousType == MachineType::SMS || previousType == MachineType::GameGear);
        m_pMemory->SetBoardRamSize(GetSystemModelDescriptor(previousModel).boardRamSize);
        m_pMemory->SetColecoMotherboard(previousType == MachineType::Coleco);
        if (previousType == MachineType::SMS || previousType == MachineType::GameGear)
            m_pMachineIOPorts->ConfigureSMS(previousModel == SystemModel::SMSPAL);
        RefreshM5Connections();
        RebuildDebugEventCatalog();
        return false;
    }
    m_pMemory->SetMSXSlots(type == MachineType::MSX ? &static_cast<SystemBusMSX*>(m_systemBus.get())->Slots() : nullptr);
    RefreshM5Connections();
    if (type == MachineType::MSX) static_cast<SystemBusMSX*>(m_systemBus.get())->RebindMemory();
    if (type == MachineType::MSX) static_cast<SystemBusMSX*>(m_systemBus.get())->ConnectPeripherals();
    if (GetSystemFamily(previousModel) != GetSystemFamily(model))
        m_pMemory->LoadBios(""); // Same-sized firmware must not cross boards.
    if (m_sgm.IsPresent() && !previousSGM.IsPresent())
        std::memset(m_pMemory->GetExternalRam(), 0, 0x8000);
    return true;
}

// The ROM map watches the bus: it needs this board's PSG ports and the
// sound routines of its firmware, if any.
void GearSF7000Core::ApplyBoardToRomMap()
{
#if GEARSF7000_ENABLE_DEBUG_TOOLS
    if (!m_pRomMap || !m_systemBus)
        return;
    const PsgPortDecode psg = m_systemBus->GetPsgPorts();
    const u16* entries = nullptr;
    const std::size_t count = m_systemBus->GetFirmwareSoundEntries(&entries);
    m_pRomMap->SetBoardSound(psg.mask, psg.match, entries, count);
    const FirmwareRange firmware = m_systemBus->GetFirmwareRange();
    m_pRomMap->SetFirmwareRange(firmware.start, firmware.end);
#endif
}

bool GearSF7000Core::HasKeyboardPPI() const
{
    return m_machineType == MachineType::SC3000 || HasSK1100(m_systemModel);
}

bool GearSF7000Core::HasKeyboardInterface() const
{
    // The board says what it offers; before Init there is no board yet and
    // the model alone answers.
    return m_systemBus ? m_systemBus->DescribeInput().machineKeyboard
                       : HasKeyboardPPI();
}

bool GearSF7000Core::HasCassetteInterface() const
{
#if GEARSF7000_ENABLE_SR1000
    return HasKeyboardPPI() || m_machineType == MachineType::MSX;
#else
    return false;
#endif
}

bool GearSF7000Core::CanAttachSF7000() const
{
    // The currently modelled SF-7000 compositions use an SC-3000 motherboard.
    // SG-1000 + SK-1100 remains a separate supported composition.
    return m_machineType == MachineType::SC3000;
}

void GearSF7000Core::Init(GC_Color_Format pixelFormat)
{
    Log("--== %s %s by Saverio Russo ==--", GEARSF7000_TITLE, build_info_version());

    m_pixelFormat = pixelFormat;

    m_pCartridge = new Cartridge();
    m_pMemory = new Memory(m_pCartridge, m_debugEvents);
    m_pMemory->SetSMSMotherboard(m_machineType == MachineType::SMS || m_machineType == MachineType::GameGear);
    m_pMemory->SetBoardRamSize(GetSystemModelDescriptor(m_systemModel).boardRamSize);
    m_pMemory->SetColecoMotherboard(m_machineType == MachineType::Coleco);
#ifndef GEARSF7000_DISABLE_DISASSEMBLER
    m_pDisassembler = new Z80Disassembler(m_pMemory);
    m_pCpuInstructionObservers = new CpuInstructionObserverList();
    m_pCpuInstructionObservers->Add(m_pMemory);
    m_pCpuInstructionObservers->Add(m_pDisassembler);
    // After the disassembler, whose record for this PC it reads.
    m_pRomMap = new RomMap(m_pMemory);
    m_pCpuInstructionObservers->Add(m_pRomMap);
    m_pMemory->SetRomMap(m_pRomMap);
    // Asked of whichever video device is installed when a VRAM write needs
    // classifying, so replacing the device needs no rebinding.
    m_pRomMap->SetVramTableSource([this](GC_VramTableSpan* spans, std::size_t capacity) {
        if (!m_pVideo)
            return std::size_t(0);
        const IVideoDebugProvider* provider = m_pVideo->GetDebugProvider();
        return provider ? provider->DescribeVramTables(spans, capacity) : std::size_t(0);
    });
#endif
    // CLK is the sole Z80 implementation. All frontends and debugger commands
    // observe the same cycle-accurate execution timeline.
    m_pCLKProcessor = new CLKZ80Processor(m_pMemory);
    m_pCpuInterruptRouter = new CpuInterruptRouter();
    m_pVdpInterruptLines = new VdpInterruptAdapter();
    m_pVdpInterruptLines->Attach(m_pCpuInterruptRouter);
    m_pAudio = new Audio();
    m_pVideo = m_videoDeviceFactory(m_selectedVideoDevice, m_pMemory,
                                    m_pVdpInterruptLines);
    if (!m_pVideo)
    {
        m_selectedVideoDevice = SystemVideoDevice::TMS9918;
        m_pVideo = m_videoDeviceFactory(m_selectedVideoDevice, m_pMemory,
                                        m_pVdpInterruptLines);
    }
    if (const auto* initial = GetVideoCatalog().FindLegacyOption(m_selectedVideoDevice))
        m_selectedVideoOption = initial->code;
    m_installedVideoOption = m_selectedVideoOption;
    m_installedDefaultVideo = m_useDefaultVideoDevice;
    ++m_videoDeviceGeneration;
    m_pKeyboard = new SegaKeyboard(m_pCpuInterruptRouter, m_pMemory);
    // The spinner of a Coleco controller pulls /INT; the standard controller
    // never does, but the wire is there.
    m_pColecoInput = new Input(m_pCpuInterruptRouter);
    m_pColecoInput->Init();
#if GEARSF7000_ENABLE_SF7000
    m_pSF7000 = new SF7000(m_pMemory, m_pAudio);
#endif
    m_pSerialPrinterPort = new SC3000SerialPrinterPort(m_pMemory);
    m_pCassettePort = new SC3000CassettePort(m_pMemory);
#if GEARSF7000_ENABLE_SR1000
    SR1000DebugConfiguration tapeDebug;
    tapeDebug.key = "event.sc3000.tape";
    tapeDebug.parentKey = "event.sc3000";
    tapeDebug.name = "SR-1000 Tape";
    m_pSR1000 = new SR1000(m_pAudio, m_pMemory, std::move(tapeDebug));
#endif
    m_pMachineIOPorts = new MachineIOPorts(m_pAudio, m_pVideo.get(), m_pKeyboard,
        m_pSR1000, m_pSF7000, m_pSerialPrinterPort, m_pCassettePort,
        m_pCartridge, m_pMemory);
    m_pMachineIOPorts->SetColecoInput(m_pColecoInput);
    const char* machineKey = MachineDebugKey(m_machineType);
    m_pMemory->SetDebugMachineKey(machineKey);
    if (m_machineType == MachineType::SG1000)
        m_systemBus = std::make_unique<SystemBusSG1000>(
            *m_pMemory, *m_pMachineIOPorts, HasSK1100(m_systemModel));
#if GEARSF7000_ENABLE_SMS
    else if (m_machineType == MachineType::SMS)
        m_systemBus = std::make_unique<SystemBusSMS>(
            *m_pMemory, *m_pMachineIOPorts,
            m_systemModel == SystemModel::SMSPAL);
#endif
#if GEARSF7000_ENABLE_GAMEGEAR
    else if (m_machineType == MachineType::GameGear)
        m_systemBus = std::make_unique<SystemBusGG>(
            *m_pMemory, *m_pMachineIOPorts, *m_pKeyboard, *m_pAudio,
            m_systemModel == SystemModel::GameGearExport);
#endif
    else if (m_machineType == MachineType::SordM5)
        m_systemBus = std::make_unique<SystemBusM5>(SystemBusM5::ProvisionalPsgWaitTStates, &m_debugEvents);
    else if (m_machineType == MachineType::MSX)
        m_systemBus = std::make_unique<SystemBusMSX>(*m_pMemory, *m_pCartridge, *m_pAudio, *m_pMachineIOPorts);
    else if (m_machineType == MachineType::Coleco)
        m_systemBus = std::make_unique<SystemBusColeco>(
            *m_pMemory, *m_pMachineIOPorts);
    else
        m_systemBus = std::make_unique<SystemBusSC3000>(
            *m_pMemory, *m_pMachineIOPorts,
            GetSystemModelDescriptor(m_systemModel).cpuPsgClockHz,
            IsSF7000System(m_systemModel));
    m_pVdpInterruptLines->SetLineType(m_systemBus->GetVdpLineType());
    ApplyBoardToRomMap();
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
    if (!m_pMemory->ConfigureBiosStorage(m_systemBus->GetBiosSize()))
        throw std::logic_error("System bus requested a non-power-of-two BIOS capacity");
    m_pMemory->SetMSXSlots(m_machineType == MachineType::MSX ? &static_cast<SystemBusMSX*>(m_systemBus.get())->Slots() : nullptr);
    RefreshM5Connections();
    if (m_machineType == MachineType::MSX) static_cast<SystemBusMSX*>(m_systemBus.get())->RebindMemory();
    m_pMemory->SetSGMDevice(&m_sgm);
    m_sgm.Configure(HasSGM(m_systemModel));
    m_pMemory->EnableSF7000(IsSF7000System(m_systemModel));
    m_pCpuExecution->InitializeExecution();
    m_pAudio->Init();
    m_pAudio->SetPsgVariant(IsSMSSystem(m_systemModel)
        ? Sms_Apu::Variant::SegaIntegrated
        : Sms_Apu::Variant::SN76489AN);
    m_pVideo->Init();
    m_pKeyboard->Init();
#if GEARSF7000_ENABLE_SR1000
    m_pSR1000->Init();
    if (m_machineType == MachineType::MSX) static_cast<SystemBusMSX*>(m_systemBus.get())->ConnectPeripherals();
#endif

#if GEARSF7000_ENABLE_SF7000
    m_pSF7000->Init(m_pAudio);
#endif
    m_pSerialPrinterPort->Reset();
    m_pCassettePort->Reset();

    // Providers may describe concrete subdevices allocated by Init(), such as
    // Audio's SN76489 core. Compose the catalog only after every device exists.
    RebuildDebugEventCatalog();

    // CLK reports elapsed time before each terminal bus operation. Devices
    // are fully initialised at this point, so they can now be advanced at the
    // actual bus timestamp instead of at the end of the whole instruction.
    m_pCLKProcessor->GetBusHandler().SetClockSink(this);

    systemClock.Init(3579545);

#if GEARSF7000_ENABLE_SF7000
    diskInit(0);
    m_pSF7000->GetDrive().Capture(
        m_pSF7000->MotorIsOn(), m_pSF7000->FDCIndexActive());
#endif

    m_pCartridge->Init();

    LoadSF7000Audio();


}

void GearSF7000Core::RebuildDebugEventCatalog()
{
    auto catalog = std::make_unique<DebugEventCatalog>();
    std::string error;
    DebugDeviceDefinition root;
    root.key = "event";
    root.typeKey = "emulator.machine";
    root.name = "GearSF7000";
    if (!catalog->AddDefinition(std::move(root), error))
        throw std::logic_error(error);

    const MachineDebugEventProvider machineProvider(m_machineType);
    const std::string machineKey =
        machineProvider.DescribeDebugDevice().key;
    m_pCLKProcessor->SetDebugParentKey(machineKey);
#if GEARSF7000_ENABLE_AY
    const bool hasAy = (m_machineType == MachineType::SC3000 || m_sgm.IsPresent() || m_machineType == MachineType::MSX) &&
                       m_pAudio->IsAyEnabled();
    m_pAudio->GetAyApu()->SetDebugConfiguration(m_machineType == MachineType::MSX
        ? AyApuDebugConfiguration{"event.msx.ay", "event.msx", "AY-3-8910", 0xA0, true}
        : m_sgm.IsPresent()
        ? AyApuDebugConfiguration{"event.coleco.sgm.ay", "event.coleco.sgm", "SGM AY-3-8910", 0x50}
        : AyApuDebugConfiguration{"event.sc3000.ay", "event.sc3000", "AY Expansion", GetAyPortBase()});
#else
    const bool hasAy = false;
#endif

    if (!catalog->Add(machineProvider, error) ||
        !catalog->Add(*m_pCLKProcessor, error) ||
        !catalog->Add(*m_systemBus, error))
        throw std::logic_error(error);

    if (auto* bus = GetM5Bus()) {
        auto video = m_pVideo->DescribeDebugDevice();
        video.key = "event.m5.vdp"; video.parentKey = machineKey;
        for (auto& space : video.spaces) if (space.id == DebugCommonSpace::IoPorts) {
            space.targetMin = 0x10; space.targetMax = 0x11;
            for (auto& target : space.targets) target.id = 0x10 | (target.id & 1);
        }
        for (auto& preset : video.presets) if (preset.spaceKey == "io_ports") {
            preset.targetStart = 0x10; preset.targetEnd = 0x11;
        }
        auto psg = m_pAudio->GetApu()->DescribeDebugDevice();
        psg.key = "event.m5.psg"; psg.parentKey = machineKey;
        for (auto& space : psg.spaces) if (space.id == DebugCommonSpace::IoPorts) {
            space.targetMin = 0x20; space.targetMax = 0x2F;
        }
        if (!catalog->Add(bus->CTC(), error) || !catalog->Add(bus->Input(), error) ||
            !catalog->AddDefinition(bus->DescribeMapper(), error) ||
            !catalog->AddDefinition(std::move(video), error) || !catalog->AddDefinition(std::move(psg), error))
            throw std::logic_error(error);
    }
    else if (m_machineType == MachineType::MSX)
    {
        auto* bus = static_cast<SystemBusMSX*>(m_systemBus.get());
#if GEARSF7000_ENABLE_SR1000
        if (!catalog->Add(bus->Cassette(), error)) throw std::logic_error(error);
#endif
        auto video = m_pVideo->DescribeDebugDevice();
        video.key = "event.msx.vdp"; video.parentKey = machineKey;
        for (auto& space : video.spaces)
            if (space.id == DebugCommonSpace::IoPorts) {
                space.targetMin = 0x98; space.targetMax = 0x99;
                for (auto& target : space.targets) target.id -= 0x26;
            }
        for (auto& preset : video.presets)
            if (preset.spaceKey == "io_ports") {
                preset.targetStart = 0x98; preset.targetEnd = 0x99;
            }
        auto ppi = bus->PPI().DescribeDebugDevice();
        for (auto& space : ppi.spaces)
            if (space.id == DebugCommonSpace::Registers)
                space.capabilities = DebugDescriptorCapability_EventsPublished;
        if (!catalog->AddDefinition(std::move(video), error) ||
            !catalog->Add(bus->Keyboard(), error) || !catalog->Add(bus->Slots(), error) ||
            !catalog->AddDefinition(MSXAscii16Rom::DescribeDebugDevice(), error) ||
            !catalog->AddDefinition(std::move(ppi), error))
            throw std::logic_error(error);
#if GEARSF7000_ENABLE_AY
        auto ay = m_pAudio->GetAyApu()->DescribeDebugDevice();
        ay.traits &= ~DebugDeviceTrait_External;
        for (auto& space : ay.spaces)
            if (space.id == DebugCommonSpace::Registers)
                space.beforeValueAccesses = DebugEventAccess_Event;
            else if (space.id == DebugCommonSpace::IoPorts) {
                space.targetMax = 0xA2;
                if (space.targets.size() == 4) space.targets.pop_back();
            }
        if (!catalog->AddDefinition(std::move(ay), error) || !catalog->Add(bus->Joysticks(), error))
            throw std::logic_error(error);
#endif
    }
    else if (m_machineType == MachineType::SG1000 ||
        m_machineType == MachineType::SMS ||
        m_machineType == MachineType::GameGear ||
        m_machineType == MachineType::Coleco)
    {
        // Consoles: the VDP and PSG hang directly off the machine node.
        auto video = m_pVideo->DescribeDebugDevice();
        video.key = machineKey + ".vdp";
        video.parentKey = machineKey;
        auto psg = m_pAudio->GetApu()->DescribeDebugDevice();
        psg.key = machineKey + ".psg";
        psg.parentKey = machineKey;
        if (!catalog->AddDefinition(std::move(video), error) ||
            !catalog->AddDefinition(std::move(psg), error))
            throw std::logic_error(error);
#if GEARSF7000_ENABLE_YM2413
        if (m_machineType == MachineType::SMS &&
            m_pAudio->IsYm2413Enabled() &&
            !catalog->Add(*m_pAudio->GetYm2413(), error))
            throw std::logic_error(error);
#endif
        // Whatever is plugged into the two controller ports, each its own device.
        if (m_machineType == MachineType::SMS)
            for (unsigned player = 0; player < 2; ++player)
            {
                ControllerDevice& pad = m_pMachineIOPorts->GetControllerDevice(player);
                pad.SetDebugKeys(machineKey + (player == 0 ? ".pad1" : ".pad2"), machineKey);
                if (!catalog->Add(pad, error))
                    throw std::logic_error(error);
            }
    }
    else
    {
        if (!catalog->Add(*m_pVideo, error) ||
            !catalog->Add(*m_pAudio->GetApu(), error) ||
            !catalog->Add(m_pMachineIOPorts->GetSC3000PPI(), error))
            throw std::logic_error(error);
#if GEARSF7000_ENABLE_SR1000
        if (!catalog->Add(*m_pSR1000, error))
            throw std::logic_error(error);
#endif
#if GEARSF7000_ENABLE_AY
        if (hasAy && !catalog->Add(*m_pAudio->GetAyApu(), error))
            throw std::logic_error(error);
#endif
#if GEARSF7000_ENABLE_SF7000
        if (IsSF7000System(m_systemModel) &&
            (!catalog->Add(*m_pSF7000, error) ||
             !catalog->Add(m_pSF7000->GetPPI(), error) ||
             !catalog->Add(m_pSF7000->GetFDC(), error) ||
             !catalog->Add(m_pSF7000->GetDrive(), error) ||
             !catalog->Add(m_pSF7000->GetUSART(), error)))
            throw std::logic_error(error);
#endif
    }
    if (m_sgm.IsPresent())
    {
        if (!catalog->Add(m_sgm, error)) throw std::logic_error(error);
#if GEARSF7000_ENABLE_AY
        if (hasAy && !catalog->Add(*m_pAudio->GetAyApu(), error)) throw std::logic_error(error);
#endif
    }
    if (m_pAudio->IsOneBitEnabled())
    {
        auto& oneBit = m_pAudio->GetOneBitDevice();
        oneBit.SetDebugParentKey(machineKey);
        if (!catalog->Add(oneBit, error)) throw std::logic_error(error);
    }
    if (!catalog->Add(*m_pMemory, error))
        throw std::logic_error(error);
    if (const Mapper* mapper = m_pMemory->GetMapperDebugEventProvider())
        if (!catalog->Add(*mapper, error))
            throw std::logic_error(error);
    if (m_machineType == MachineType::SC3000)
        if (!catalog->Add(*m_pSerialPrinterPort, error))
            throw std::logic_error(error);
    if (m_machineType == MachineType::SC3000)
        if (!catalog->Add(*m_pKeyboard, error))
            throw std::logic_error(error);
    if (m_machineType == MachineType::SC3000)
        if (!catalog->Add(*m_pCassettePort, error))
            throw std::logic_error(error);
#if GEARSF7000_ENABLE_SF7000
    if (IsSF7000System(m_systemModel))
        if (!catalog->Add(m_pSF7000->GetParallelPrinterPort(), error))
            throw std::logic_error(error);
    if (IsSF7000System(m_systemModel))
        if (!catalog->Add(m_pSF7000->GetMemoryDevice(), error))
            throw std::logic_error(error);
#endif
    if (!catalog->Freeze(error))
        throw std::logic_error(error);

    DebugEventManager* events = GetDebugEventManager();
    if (m_debugEventCatalog)
        events->RebindRegistry(*catalog->GetRegistry());
    else if (!events->SetRegistry(*catalog->GetRegistry()))
        throw std::logic_error("Debug catalog must be bound before event recording");
    m_debugEventCatalog = std::move(catalog);
    m_pMachineIOPorts->BindDebugEvents(events->GetRegistry());
    m_pMemory->BindDebugEventProviders(events->GetRegistry());
    const auto* oneBit = events->GetRegistry().FindDevice((machineKey + ".onebit").c_str());
    m_pAudio->GetOneBitDevice().SetLevelChangeCallback(oneBit
        ? OneBitAudioDevice::LevelChange([this, id = oneBit->id](bool before, bool after) {
            m_pMemory->CheckDeviceStateChange({id, DebugCommonSpace::State, 0}, before, after);
          }) : OneBitAudioDevice::LevelChange{});
    if (auto* bus = GetM5Bus()) {
        if (!bus->BindDebugEvents(events->GetRegistry())) throw std::logic_error("M5 debug catalog binding failed");
    }
    if (m_machineType == MachineType::MSX) static_cast<SystemBusMSX*>(m_systemBus.get())->BindDebugEvents(events->GetRegistry());
    m_pSerialPrinterPort->BindDebugEvents(events->GetRegistry());
    m_pKeyboard->BindDebugEvents(events->GetRegistry());
    m_pCassettePort->BindDebugEvents(events->GetRegistry());
#if GEARSF7000_ENABLE_SF7000
    m_pSF7000->GetDrive().BindDebugEvents(events->GetRegistry());
    m_pSF7000->GetUSART().BindDebugEvents(
        events->GetRegistry(), m_pMemory);
    m_pSF7000->GetParallelPrinterPort().BindDebugEvents(
        events->GetRegistry());
#endif
}

void GearSF7000Core::SetDebugRuntimeActive(bool active)
{
    m_debugEvents.SetRuntimeActive(active);
#ifndef GEARSF7000_DISABLE_DISASSEMBLER
    if (m_pCpuExecution)
        m_pCpuExecution->AttachInstructionObserver(active ? m_pCpuInstructionObservers : nullptr);
    if (m_pRomMap)
        m_pRomMap->SetRuntimeActive(active);
#endif
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
    m_pCLKProcessor->GetBusHandler().SetSystemBus(m_systemBus.get());
#ifndef GEARSF7000_DISABLE_DISASSEMBLER
    m_pCpuExecution->AttachInstructionObserver(
        m_debugEvents.IsRuntimeActive() ? m_pCpuInstructionObservers : nullptr);
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

GC_RunResult GearSF7000Core::RunToVBlank(s16* pSampleBuffer,
    int* pSampleCount, bool step, bool stopOnBreakpoints,
    bool stopOnLineChange)
{
    GC_RunResult result;
    ApplyPendingDebugEventCommands();

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
            if (auto* bus = GetM5Bus()) if (bus->TakeDebugPauseRequest()) m_pCLKProcessor->RequestCpuDebugPause();
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
        // segment as well. GC_RunResult now tells the
        // caller whether this was a real VBlank, so later streaming/audio work
        // can change that policy without inferring it from a bool.
        m_pAudio->EndFrame(pSampleBuffer, pSampleCount);
    }

    return result;
}

void GearSF7000Core::ApplyPendingDebugEventCommands()
{
    m_debugEvents.ApplyPendingRuleCommands();
}

bool GearSF7000Core::TickMachine(std::uint64_t cpuTStates,
                                std::uint64_t videoDeviceClocks,
                                std::uint64_t fdcClocks)
{
    if (cpuTStates == 0 && videoDeviceClocks == 0 && fdcClocks == 0)
        return false;

    if (cpuTStates != 0)
    {
        const unsigned int clocks = static_cast<unsigned int>(cpuTStates);
        systemClock.Tick(clocks);
        m_pAudio->Tick(clocks);
        // The physical SK-1100 matrix changes only on host key/joystick
        // edges. Advance it only while the automatic text injector has timed
        // events.
        if (m_pKeyboard->HasPendingTimedEvents())
            m_pKeyboard->Tick(clocks);
#if GEARSF7000_ENABLE_SR1000
        if (HasCassetteInterface()) GetCassette()->Tick(clocks);
        else m_pAudio->SR1000SpeakerWrite(0.0f, false);
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

    return videoDeviceClocks != 0 && m_pVideo->TickDeviceClocks(
        static_cast<unsigned int>(videoDeviceClocks));
}

void GearSF7000Core::AdvanceZ80HalfCycles(std::uint64_t halfCycles)
{
    // CLK reports objective Z80 half-cycles. Convert the elapsed interval
    // once into each physical oscillator domain, retaining every rational
    // remainder. The VDP can therefore advance independently on PAL hardware
    // without a per-master-clock scheduler loop.
    m_clockDomains.AdvanceZ80HalfCycles(halfCycles);
    const std::uint64_t cpuTStates = m_clockDomains.ConsumeCpuTStates();
    const std::uint64_t videoDeviceClocks =
        m_clockDomains.ConsumeVideoDeviceClocks();
    const std::uint64_t fdcClocks = m_clockDomains.ConsumeFDCClocks();

    m_clkMachineVBlank = TickMachine(
        cpuTStates, videoDeviceClocks, fdcClocks) ||
        m_clkMachineVBlank;
}

bool GearSF7000Core::IsMachineReady() const
{
    // A bare SC-3000 requires a cartridge.  With the external SF-7000 I/O
    // cartridge enabled, its IPL/RAM is a complete bootable machine instead.
    if (m_pMemory->IsSF7000Enabled() || m_machineType == MachineType::MSX || m_machineType == MachineType::Coleco || m_machineType == MachineType::SordM5)
        return m_pMemory->IsBiosLoaded();
    return m_pCartridge->IsReady();
}

void GearSF7000Core::ObserveCurrentInstruction()
{
#ifndef GEARSF7000_DISABLE_DISASSEMBLER
    if (!m_pCpuExecution || !m_pDisassembler)
        return;
    // Decode for the debugger without pretending the instruction executed.
    // Calling the complete observer list here used to contaminate coverage
    // and could also evaluate execute breakpoints merely by inspecting PC.
    m_pDisassembler->Disassemble(m_pCpuExecution->GetProgramCounter());
#endif
}

// An image with an SG16 header says how much RAM it needs at $C000. When the
// console has less than that, the 315-5208 gets the Dahjee B's 8 KB; a manual
// choice of mapper is never touched.
static void ApplySg16HeaderRam(Cartridge* cartridge, const Memory* memory)
{
    if (!cartridge->IsTypeFromSg16Header()
        || cartridge->GetType() != Cartridge::SG1000_SEGA_315_5208)
        return;
    const int needed = cartridge->GetHeaderRamBytes();
    if (needed <= static_cast<int>(memory->GetBoardRamSize()))
        return;
    if (needed > 0x2000)
    {
        Log("SG16 header asks for %d bytes of RAM: no adapter gives that, the console's RAM stays", needed);
        return;
    }
    cartridge->UseDahjeeBRam();
    Log("SG16 header asks for %d bytes of RAM, the console has %u: Dahjee B 8 KB added",
        needed, static_cast<unsigned>(memory->GetBoardRamSize()));
}

namespace
{
// The MSX cartridge type: a manual choice of an MSX mapper as it is; in auto
// mode (no choice) from the image. Anything else, or a size the type cannot
// take, gives CartridgeNotSupported and the reason.
Cartridge::CartridgeTypes MSXCartridgeType(const Cartridge& cart,
    const Cartridge::ForceConfiguration* config, const char** reason)
{
    Cartridge::CartridgeTypes type = config ? config->type : Cartridge::CartridgeNotSupported;
    if (type == Cartridge::CartridgeNotSupported)
        type = Cartridge::GuessMSXType(cart.GetROM(), cart.GetROMSize(), reason);
    else if (type != Cartridge::MSX_LINEAR && type != Cartridge::MSX_ASCII16)
    {
        if (reason) *reason = "not an MSX cartridge mapper";
        return Cartridge::CartridgeNotSupported;
    }
    if (type == Cartridge::CartridgeNotSupported ||
        !Cartridge::IsTypeCompatible(type, cart.GetROMSize(), reason))
        return Cartridge::CartridgeNotSupported;
    return type;
}
}

bool GearSF7000Core::LoadROM(const char* szFilePath, Cartridge::ForceConfiguration* config, bool pauseAtResetVector)
{
    const bool loaded = m_pCartridge->LoadFromFile(szFilePath);
    RefreshM5Connections();
    if (m_machineType == MachineType::MSX) static_cast<SystemBusMSX*>(m_systemBus.get())->RebindMemory();
    if (loaded)
    {
        if (m_machineType == MachineType::SordM5) {
            const char* reason = nullptr;
            if ((config && config->type != Cartridge::CartridgeNotSupported && config->type != Cartridge::SORD_M5_LINEAR) ||
                !Cartridge::IsTypeCompatible(Cartridge::SORD_M5_LINEAR, m_pCartridge->GetROMSize(), &reason)) {
                m_pCartridge->Reset(); RefreshM5Connections(); return false;
            }
            Cartridge::ForceConfiguration chosen;
            if (config) chosen = *config;
            chosen.type = Cartridge::SORD_M5_LINEAR; m_pCartridge->ForceConfig(chosen);
        }
        else if (m_machineType == MachineType::MSX)
        {
            const char* reason = nullptr;
            const Cartridge::CartridgeTypes type = MSXCartridgeType(*m_pCartridge, config, &reason);
            if (type == Cartridge::CartridgeNotSupported)
            {
                Log("MSX cartridge rejected: %s", reason ? reason : "incompatible mapper");
                m_pCartridge->Reset();
                static_cast<SystemBusMSX*>(m_systemBus.get())->RebindMemory();
                return false;
            }
            Cartridge::ForceConfiguration msx;
            if (config) msx = *config;
            msx.type = type;
            m_pCartridge->ForceConfig(msx);
        }
        else if (IsValidPointer(config))
            m_pCartridge->ForceConfig(*config);

        if (!m_pCartridge->IsReady())
        {
            m_pCartridge->Reset();
            return false;
        }

        ApplySg16HeaderRam(m_pCartridge, m_pMemory);
		m_pMemory->SetupMapper();
        Reset(pauseAtResetVector);

        m_pMemory->ResetRomDisassembledMemory();
        ObserveCurrentInstruction();

        return true;
    }
    else
        return false;
}


bool GearSF7000Core::LoadROMNull(Cartridge::ForceConfiguration*, bool pauseAtResetVector)
{
    if (m_pCartridge->LoadFromNull())
    {
        //if (IsValidPointer(config))            m_pCartridge->ForceConfig(*config);

        m_pMemory->SetupMapper();
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
    // Unlike LoadFromFile(), Cartridge::LoadFromBuffer() is also used as the
    // inner step of ZIP loading and therefore cannot clear the cartridge on
    // its own without discarding the outer file name.  At this public core
    // boundary a buffer is a complete replacement image, so give it the same
    // clean lifetime and metadata reset as a file load.  Besides preventing a
    // ROM-buffer leak, this keeps repeated headless/front-end loads from
    // carrying cartridge state into the next machine reset.
    m_pCartridge->Reset();
    const bool loaded = m_pCartridge->LoadFromBuffer(buffer, size);
    RefreshM5Connections();
    if (m_machineType == MachineType::MSX) static_cast<SystemBusMSX*>(m_systemBus.get())->RebindMemory();
    if (loaded)
    {
        if (m_machineType == MachineType::SordM5) {
            const char* reason = nullptr;
            if ((config && config->type != Cartridge::CartridgeNotSupported && config->type != Cartridge::SORD_M5_LINEAR) ||
                !Cartridge::IsTypeCompatible(Cartridge::SORD_M5_LINEAR, m_pCartridge->GetROMSize(), &reason)) {
                m_pCartridge->Reset(); RefreshM5Connections(); return false;
            }
            Cartridge::ForceConfiguration chosen;
            if (config) chosen = *config;
            chosen.type = Cartridge::SORD_M5_LINEAR; m_pCartridge->ForceConfig(chosen);
        }
        else if (m_machineType == MachineType::MSX)
        {
            const char* reason = nullptr;
            const Cartridge::CartridgeTypes type = MSXCartridgeType(*m_pCartridge, config, &reason);
            if (type == Cartridge::CartridgeNotSupported)
            {
                Log("MSX cartridge rejected: %s", reason ? reason : "incompatible mapper");
                m_pCartridge->Reset();
                static_cast<SystemBusMSX*>(m_systemBus.get())->RebindMemory();
                return false;
            }
            Cartridge::ForceConfiguration msx;
            if (config) msx = *config;
            msx.type = type;
            m_pCartridge->ForceConfig(msx);
        }
        else if (IsValidPointer(config))
            m_pCartridge->ForceConfig(*config);

        if (!m_pCartridge->IsReady())
        {
            m_pCartridge->Reset();
            return false;
        }

		ApplySg16HeaderRam(m_pCartridge, m_pMemory);
		m_pMemory->SetupMapper();
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

            for (u32 i = 0; i < m_pMemory->GetBiosSize(); i++)
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
    runtime_info.screen_width = descriptor.visible_area.width;
    runtime_info.screen_height = descriptor.visible_area.height;
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

GC_VideoFrameView GearSF7000Core::GetVideoFrameView()
{
    return m_pVideo ? m_pVideo->GetFrameView(m_pixelFormat)
                    : GC_VideoFrameView{};
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
    return m_pCLKProcessor;
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

RomMap* GearSF7000Core::GetRomMap()
{
    return m_pRomMap;
}

Audio* GearSF7000Core::GetAudio()
{
    return m_pAudio;
}

VideoDeviceHandle GearSF7000Core::AcquireVideoDevice() const
{
    VideoDeviceHandle handle;
    handle.device = m_pVideo.get();
    handle.generation = m_videoDeviceGeneration;
    if (m_pVideo && m_pVideo->GetDeviceDescriptor().key != nullptr)
        handle.type_key = m_pVideo->GetDeviceDescriptor().key;
    return handle;
}

bool GearSF7000Core::IsVideoDeviceHandleCurrent(
    const VideoDeviceHandle& handle) const
{
    if (handle.device == nullptr || handle.device != m_pVideo.get() ||
        handle.generation != m_videoDeviceGeneration || !m_pVideo)
        return false;
    const char* currentKey = m_pVideo->GetDeviceDescriptor().key;
    return currentKey != nullptr && handle.type_key == currentKey;
}

bool GearSF7000Core::InstallSelectedVideoDevice()
{
    const auto* selected = GetVideoCatalog().FindOption(m_selectedVideoOption);
    if (!selected || !selected->available)
        return false;
    if (m_pVideo && m_installedVideoOption == selected->code) {
        m_installedDefaultVideo = m_useDefaultVideoDevice;
        return true;
    }

    std::unique_ptr<IVideoDevice> replacement;
    try {
        replacement = m_videoDeviceFactory == &CreateVideoDevice
            ? CreateVideoOption(*selected, m_pMemory, m_pVdpInterruptLines)
            : m_videoDeviceFactory(selected->legacyDevice, m_pMemory, m_pVdpInterruptLines);
        if (!replacement)
            throw std::runtime_error("video factory returned no device");
        replacement->Init();
        if (m_pVideo)
            replacement->CopyHostConfigurationFrom(*m_pVideo);
    } catch (const std::exception& error) {
        Log("Video selection rejected: %s", error.what());
        if (const auto* active = GetVideoCatalog().FindOption(m_installedVideoOption)) {
            m_selectedVideoOption = active->code;
            m_selectedVideoDevice = active->legacyDevice;
            m_useDefaultVideoDevice = m_installedDefaultVideo;
        }
        return false;
    }

    if (m_pMachineIOPorts)
        m_pMachineIOPorts->SetVideoDevice(replacement.get());
    m_pVideo = std::move(replacement);
    m_installedVideoOption = selected->code;
    m_installedDefaultVideo = m_useDefaultVideoDevice;
    RefreshM5Connections();
    ++m_videoDeviceGeneration;
#ifndef GEARSF7000_DISABLE_DISASSEMBLER
    if (m_pRomMap)
        m_pRomMap->OnVdpLayoutChanged();
#endif
    RebuildDebugEventCatalog();
    return true;
}

u64 GearSF7000Core::GetVideoFrameSerial() const
{
    return m_pVideo ? m_pVideo->GetFrameSerial() : 0;
}

u64 GearSF7000Core::GetVideoDeviceGeneration() const
{
    return m_videoDeviceGeneration;
}

const GC_VideoDeviceDescriptor* GearSF7000Core::GetVideoDeviceDescriptor() const
{
    return m_pVideo ? &m_pVideo->GetDeviceDescriptor() : NULL;
}

std::size_t GearSF7000Core::GetVideoOptionCount() const
{
    return m_pVideo ? m_pVideo->GetOptionCount() : 0;
}

bool GearSF7000Core::DescribeVideoOption(std::size_t index,
    GC_VideoOptionDescriptor& descriptor) const
{
    return m_pVideo && m_pVideo->DescribeOption(index, descriptor);
}

bool GearSF7000Core::SetVideoOption(const char* code, s64 value)
{
    return m_pVideo && m_pVideo->SetOption(code, value);
}

std::size_t GearSF7000Core::GetVideoColorProfileCount() const
{
    return m_pVideo
        ? std::min(m_pVideo->GetColorProfileCount(),
                   GC_VIDEO_MAX_COLOR_PROFILES)
        : 0;
}

bool GearSF7000Core::DescribeVideoColorProfile(std::size_t index,
    GC_VideoColorProfileDescriptor& descriptor) const
{
    return m_pVideo && index < GetVideoColorProfileCount() &&
        m_pVideo->DescribeColorProfile(index, descriptor);
}

bool GearSF7000Core::GetVideoPaletteView(GC_VideoPaletteView& view) const
{
    return m_pVideo && m_pVideo->GetPaletteView(view);
}

bool GearSF7000Core::AttachVideoCrtSignalSink(ICrtSignalSink* sink)
{
    IVideoCrtSignalProvider* provider = m_pVideo
        ? dynamic_cast<IVideoCrtSignalProvider*>(m_pVideo.get())
        : nullptr;
    return provider && provider->AttachCrtSignalSink(sink);
}

ITapeRecorder* GearSF7000Core::GetCassette()
{
#if GEARSF7000_ENABLE_SR1000
    if (m_machineType == MachineType::MSX)
        return &static_cast<SystemBusMSX*>(m_systemBus.get())->Cassette();
    return m_pSR1000;
#else
    return nullptr;
#endif
}

SC3000SerialPrinterPort* GearSF7000Core::GetSerialPrinterPort()
{
    return m_pSerialPrinterPort;
}

SC3000CassettePort* GearSF7000Core::GetCassettePort()
{
    return m_pCassettePort;
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
    if (!GetM5Bus() && m_machineType != MachineType::GameGear)
        m_pKeyboard->PauseKeyPressed();
}

bool GearSF7000Core::QueueKeyboardText(const std::string& text, std::string* error)
{
    if (m_machineType == MachineType::MSX || m_machineType == MachineType::GameGear || GetM5Bus()) {
        if (error) *error = "Text injection is not implemented on this board; use keyboard_key press/release";
        return false;
    }
    return m_pKeyboard->QueueText(text, error);
}

void GearSF7000Core::SetKeyboardTextTiming(int pressPolls, int releasePolls, int newlinePolls)
{
    m_pKeyboard->SetTextTiming(pressPolls, releasePolls, newlinePolls);
}

void GearSF7000Core::ClearKeyboardText()
{
    m_pKeyboard->ClearTextQueue();
}

float GearSF7000Core::GetKeyboardTextProgress() const
{
    const size_t total = m_pKeyboard->GetTextQueueSize();
    if (total == 0)
        return 1.0f;
    return (float)m_pKeyboard->GetTextQueueIndex() / (float)total;
}

void GearSF7000Core::EnableEvents(bool enabled)
{
    enabledEvents = enabled;
}

void GearSF7000Core::SetKeyboardMode(bool enabled)
{
    // Base SG-1000 has no keyboard/PPI. SK-1100 will become an explicit
    // expansion rather than silently changing this motherboard model.
    if (!enabled && GetM5Bus()) ReleaseHostInput();
    if (!enabled) if (auto* keyboard = GetMSXKeyboard()) keyboard->ReleaseHeldKeys();
    m_pKeyboard->SetKeyboardMode(HasKeyboardInterface() && enabled);
}

#if GEARSF7000_ENABLE_SDL_INPUT
void GearSF7000Core::SetEvent(SDL_Event event)
{
    if (enabledEvents)
        m_pKeyboard->SetEvent(event);
}
#endif


namespace
{
// The Mega Drive pad button a host key stands for, or 0.
u8 MegaDriveButton(GC_Keys key)
{
    switch (key)
    {
    case Key_MD_A: return ControllerDevice::ButtonA;
    case Key_MD_Start: return ControllerDevice::ButtonStart;
    case Key_MD_X: return ControllerDevice::ButtonX;
    case Key_MD_Y: return ControllerDevice::ButtonY;
    case Key_MD_Z: return ControllerDevice::ButtonZ;
    case Key_MD_Mode: return ControllerDevice::ButtonMode;
    default: return 0;
    }
}

// The MSX joystick line a host joystick key stands for, or 0.
u8 MSXJoystickLine(GC_Keys key)
{
    switch (key)
    {
    case Key_Up: return MSXJoysticks::Up;
    case Key_Down: return MSXJoysticks::Down;
    case Key_Left: return MSXJoysticks::Left;
    case Key_Right: return MSXJoysticks::Right;
    case Key_Left_Button: return MSXJoysticks::TriggerA;
    case Key_Right_Button: return MSXJoysticks::TriggerB;
    default: return 0;
    }
}
}

void GearSF7000Core::JoystickPressed(GC_Controllers controller, GC_Keys key)
{
    if (key == Key_GG_Start) {
#if GEARSF7000_ENABLE_GAMEGEAR
        if (controller == Controller_1)
            if (auto* bus = dynamic_cast<SystemBusGG*>(m_systemBus.get()))
                bus->SetStartPressed(true);
#endif
        return;
    }
    if (auto* bus = GetM5Bus()) {
        int direction = key == Key_Right ? 0 : key == Key_Up ? 1 : key == Key_Left ? 2 : key == Key_Down ? 3 : -1;
        if (direction >= 0) bus->Input().SetJoystickDirection(M5Input::Source::Controller, unsigned(controller),
            static_cast<M5Input::Direction>(direction), true, bus->CpuClock());
        else if (key == Key_Left_Button || key == Key_Right_Button)
            bus->Input().SetJoystickFire(M5Input::Source::Controller, unsigned(controller),
                key == Key_Left_Button ? 0 : 1, true, bus->CpuClock());
        return;
    }
    if (auto* joysticks = GetMSXJoysticks()) {
        if (const u8 line = MSXJoystickLine(key)) joysticks->Set(unsigned(controller), line, true);
        return;
    }
    // The same host action reaches whichever controller the board has.
    if (m_machineType == MachineType::Coleco)
        m_pColecoInput->KeyPressed(controller, key);
    else if (const u8 button = MegaDriveButton(key))
        m_pMachineIOPorts->GetControllerDevice(controller).SetButton(button, true);
    else
        m_pKeyboard->JoystickPressed(controller, key);
}

void GearSF7000Core::JoystickReleased(GC_Controllers controller, GC_Keys key)
{
    if (key == Key_GG_Start) {
#if GEARSF7000_ENABLE_GAMEGEAR
        if (controller == Controller_1)
            if (auto* bus = dynamic_cast<SystemBusGG*>(m_systemBus.get()))
                bus->SetStartPressed(false);
#endif
        return;
    }
    if (auto* bus = GetM5Bus()) {
        int direction = key == Key_Right ? 0 : key == Key_Up ? 1 : key == Key_Left ? 2 : key == Key_Down ? 3 : -1;
        if (direction >= 0) bus->Input().SetJoystickDirection(M5Input::Source::Controller, unsigned(controller),
            static_cast<M5Input::Direction>(direction), false, bus->CpuClock());
        else if (key == Key_Left_Button || key == Key_Right_Button)
            bus->Input().SetJoystickFire(M5Input::Source::Controller, unsigned(controller),
                key == Key_Left_Button ? 0 : 1, false, bus->CpuClock());
        return;
    }
    if (auto* joysticks = GetMSXJoysticks()) {
        if (const u8 line = MSXJoystickLine(key)) joysticks->Set(unsigned(controller), line, false);
        return;
    }
    if (m_machineType == MachineType::Coleco)
        m_pColecoInput->KeyReleased(controller, key);
    else if (const u8 button = MegaDriveButton(key))
        m_pMachineIOPorts->GetControllerDevice(controller).SetButton(button, false);
    else
        m_pKeyboard->JoystickReleased(controller, key);
}

void GearSF7000Core::ReleaseHostInput()
{
#if GEARSF7000_ENABLE_GAMEGEAR
    if (auto* bus = dynamic_cast<SystemBusGG*>(m_systemBus.get()))
        bus->SetStartPressed(false);
#endif
    if (auto* bus = GetM5Bus()) {
        bus->Input().Release(M5Input::Source::Host, bus->CpuClock());
        bus->Input().Release(M5Input::Source::Synthetic, bus->CpuClock());
        bus->Input().Release(M5Input::Source::Controller, bus->CpuClock());
    }
    if (auto* keyboard = GetMSXKeyboard()) keyboard->ReleaseHeldKeys();
    if (auto* joysticks = GetMSXJoysticks()) joysticks->ReleaseAll();
    m_pKeyboard->ReleaseHeldKeys();
    m_pColecoInput->ReleaseAll();
    for (unsigned player = 0; player < 2; ++player)
        m_pMachineIOPorts->GetControllerDevice(player).SetButton(0xFF, false);
}

bool GearSF7000Core::SetControllerDevice(GC_Controllers controller, SystemControllerDevice type)
{
    if (!IsValidControllerDevice(static_cast<int>(type)))
        return false;
    ControllerDevice& current = m_pMachineIOPorts->GetControllerDevice(controller);
    if (current.GetType() == type)
        return true;
    m_pMachineIOPorts->SetControllerDevice(controller, type);
    // The catalog describes the device that is plugged in.
    RebuildDebugEventCatalog();
    return true;
}

SystemControllerDevice GearSF7000Core::GetControllerDevice(GC_Controllers controller) const
{
    return m_pMachineIOPorts->GetControllerDevice(controller).GetType();
}

void GearSF7000Core::GetInputRows(uint16_t rows[8]) const
{
    m_pKeyboard->GetRows(rows);
}

bool GearSF7000Core::KeyboardKey(const std::string& label, bool pressed)
{
    if (auto* bus = GetM5Bus()) return bus->Input().SetKeyByLabel(M5Input::Source::Synthetic, label, pressed, bus->CpuClock());
    if (auto* keyboard = GetMSXKeyboard()) return keyboard->PressMatrixKeyByLabel(label, pressed);
    return m_pKeyboard->PressMatrixKeyByLabel(label, pressed);
}

SystemBusM5* GearSF7000Core::GetM5Bus() const
{ return m_machineType == MachineType::SordM5 ? static_cast<SystemBusM5*>(m_systemBus.get()) : nullptr; }

void GearSF7000Core::SetM5EM5Installed(bool installed)
{
    if (m_m5EM5Installed == installed) return;
    m_m5EM5Installed = installed;
    if (m_pMemory && GetM5Bus()) RefreshM5Connections();
}

void GearSF7000Core::RefreshM5Connections()
{
    auto* bus = GetM5Bus();
    if (m_pMemory) m_pMemory->SetM5Mapper(bus ? &bus->Mapper() : nullptr);
    if (m_pVdpInterruptLines) m_pVdpInterruptLines->AttachDevice(bus);
    if (!bus) return;
    bus->SetEM5Installed(m_m5EM5Installed);
    bus->AttachVideo(m_pVideo.get()); bus->AttachAudio(m_pAudio);
    bus->AttachCpu(m_pCpuInterruptRouter); bus->AttachMemoryDebug(m_pMemory);
    bus->SetDebugContext([this] { return m_pCLKProcessor->GetCpuDebugSnapshot().programCounter; },
        [this] { m_pCLKProcessor->RequestCpuDebugPause(); });
    const bool cart = m_pCartridge->IsReady() && m_pCartridge->GetType() == Cartridge::SORD_M5_LINEAR &&
        m_pCartridge->GetROMSize() <= 0x5000;
    bus->BindMemory(m_pMemory->GetBiosSize() == 8192 ? std::span<const u8>(m_pMemory->GetBios(),8192) : std::span<const u8>{},
        cart ? std::span<const u8>(m_pCartridge->GetROM(),m_pCartridge->GetROMSize()) : std::span<const u8>{},
        {m_pMemory->GetInternalRam(),4096},
        m_m5EM5Installed ? std::span<u8>(m_pMemory->GetExternalRam(), M5MemoryMapper::EM5RamSize) : std::span<u8>{});
}

MSXKeyboard* GearSF7000Core::GetMSXKeyboard() const
{
    return m_machineType == MachineType::MSX && m_systemBus
        ? &static_cast<SystemBusMSX*>(m_systemBus.get())->Keyboard() : nullptr;
}

MSXJoysticks* GearSF7000Core::GetMSXJoysticks() const
{
    return m_machineType == MachineType::MSX && m_systemBus
        ? &static_cast<SystemBusMSX*>(m_systemBus.get())->Joysticks() : nullptr;
}

void GearSF7000Core::KeyboardMatrixKey(int row, int mask, bool pressed)
{
    if (auto* bus = GetM5Bus()) {
        if (row == 8 && mask == 0x80) bus->Input().SetResetKey(M5Input::Source::Host, pressed, bus->CpuClock());
        else bus->Input().SetMatrixKeyState(M5Input::Source::Host, row, mask, pressed, bus->CpuClock());
    }
    else if (auto* keyboard = GetMSXKeyboard()) keyboard->SetMatrixKeyState(row, mask, pressed);
    else m_pKeyboard->SetMatrixKeyState(row, mask, pressed);
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
    if (m_pCartridge->IsReady() || ((m_machineType == MachineType::MSX || m_machineType == MachineType::Coleco || m_machineType == MachineType::SordM5) && m_pMemory->IsBiosLoaded()))
    {
        Log("GearSF7000 RESET");

        if (IsValidPointer(config))
        {
            auto selected = *config;
            if (m_machineType == MachineType::SordM5) selected.type = Cartridge::SORD_M5_LINEAR;
            if (m_machineType == MachineType::MSX) {
                const Cartridge::CartridgeTypes type = MSXCartridgeType(*m_pCartridge, &selected, nullptr);
                selected.type = type != Cartridge::CartridgeNotSupported ? type : m_pCartridge->GetType();
            }
            m_pCartridge->ForceConfig(selected);
            ApplySg16HeaderRam(m_pCartridge, m_pMemory);
            m_pMemory->SetupMapper();
        }
        Reset(pauseAtResetVector);

        ObserveCurrentInstruction();
    }
}

void GearSF7000Core::EjectCartridge()
{
    Log("GearSF7000 EJECT");

    // Cartridge::Reset() frees the ROM and clears IsReady()/CRC back
    // to their pre-load defaults, same as a fresh Cartridge. Unlike ResetROM,
    // this must run unconditionally: there is no cartridge left to gate on.
    m_pCartridge->Reset();
    m_pMemory->SetupMapper();

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
    // IPL/RAM mapping and physically occupies the cartridge slot.
    ApplySystemModelAtReset(WithSF7000(m_systemModel));
    m_pCartridge->Reset();
    m_pMemory->SetupMapper();
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
    const bool hadOneBit = m_pAudio->IsOneBitEnabled();
    m_pAudio->SetOneBitEnabled(m_systemBus->HasOneBitAudio());
    m_pAudio->Reset(GetVideoFrameDescriptor().region,
                    m_systemBus->GetCpuPsgClockHz());
    if (m_machineType == MachineType::MSX)
        m_pAudio->GetOneBitDevice().RestoreLevel(
            static_cast<SystemBusMSX*>(m_systemBus.get())->KeyboardClickLevel());
    if (hadOneBit != m_pAudio->IsOneBitEnabled()) RebuildDebugEventCatalog();
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

std::string GearSF7000Core::GetSaveStatePath(const char* directoryOrFile, int index) const
{
    if (index < 0)
        return directoryOrFile ? directoryOrFile : "";
    // Like ADAM's computer boot, SF-7000 has firmware without an ordinary
    // cartridge. Keep its slot files beside the IPL (or in the chosen folder).
    const std::filesystem::path content = (m_pMemory->IsSF7000Enabled() || ((m_machineType == MachineType::MSX || m_machineType == MachineType::Coleco || m_machineType == MachineType::SordM5) && !m_pCartridge->IsReady()))
        ? m_pMemory->GetBiosFilePath() : m_pCartridge->GetFilePath();
    if (content.empty())
        return "";
    std::filesystem::path path = directoryOrFile
        ? std::filesystem::path(directoryOrFile) / content.filename() : content;
    path.replace_extension(".state" + std::to_string(index));
    return path.string();
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

    const std::string path = GetSaveStatePath(szPath, index);
    if (path.empty())
    {
        m_LastStateError = "Save state: no content path; provide an explicit file path.";
        return false;
    }
    Log("Save state file: %s", path.c_str());

    ofstream file(path.c_str(), ios::out | ios::binary);
    if (!file)
    {
        Log("Save state failed: could not open %s for writing.",
            path.c_str());
        return false;
    }

    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();

    if (!file)
    {
        Log("Save state failed: the write to %s did not complete.",
            path.c_str());
        return false;
    }

    if (bytesWritten)
        *bytesWritten = bytes.size();

    Log("Save state created [%d bytes]", static_cast<int>(bytes.size()));
    return true;
}

bool GearSF7000Core::SaveState(u8* buffer, size_t& size)
{
    if (!IsMachineReady())
    {
        m_LastStateError = "Machine not ready: required cartridge or firmware is missing.";
        Log("%s", m_LastStateError.c_str());
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
// single device has been handed a byte: media mode, video device and its
// serialization version, cartridge type, mapper, and ROM CRC. Version 2 adds
// video identity; version 1 remains readable for existing snapshots.
constexpr u16 kStateFormatVersion = 4;
constexpr u16 kOldestReadableStateFormatVersion = 1;

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
constexpr u32 kSectionColecoInput = FourCC('C', 'V', 'I', 'N');
constexpr u32 kSectionPPI = FourCC('P', 'P', 'I', ' ');
constexpr u32 kSectionSF7000 = FourCC('S', 'F', '7', '0');
// The machine family (GetSystemFamily), written first so a state from another
// family is refused before any section is applied. A section rather than a header
// field: a build without it skips it, a state without it loads as before.
constexpr u32 kSectionFamily = FourCC('F', 'A', 'M', 'L');
constexpr u32 kSectionFirmware = FourCC('F', 'W', 'R', 'M');
constexpr u32 kSectionMSX = FourCC('M', 'S', 'X', '1');
constexpr u32 kSectionM5 = FourCC('M', '5', 'B', 'S');
constexpr u32 kSectionGameGear = FourCC('G', 'G', 'I', 'O');

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
    if (!IsMachineReady())
    {
        m_LastStateError = StateReason("Machine not ready: required cartridge or firmware is missing.");
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
    // Version 4 records the complete video selection for every machine.
    w.U16(kStateFormatVersion);
    w.U8(sf7000Mode ? kMediaSF7000 : kMediaCartridgeRom);
    w.U8(GetVideoFrameDescriptor().region == Region_PAL ? 1 : 0);
    w.U32(static_cast<u32>(m_pCartridge->GetType()));
    w.U32(m_pMemory->GetMapperId());
    w.U32(m_pCartridge->GetCRC());
    w.U64(m_pVideo->GetFrameSerial());
    w.String(m_pVideo->GetDeviceDescriptor().key);
    w.U16(m_pVideo->GetStateFormatVersion());
    w.U8(m_sgm.IsPresent() ? 1 : 0);
    w.U8(m_sgm.LowerEnabled());
    w.U8(m_sgm.UpperEnabled());
    w.String(m_installedVideoOption);

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

    section(kSectionFamily, 1, [&](std::ostream& out)
            {
                StateWriter family(out);
                family.U8(static_cast<u8>(GetSystemFamily(m_systemModel)));
            });
    if (m_pMemory->IsBiosLoaded())
        section(kSectionFirmware, 1, [&](std::ostream& out)
                {
                    StateWriter firmware(out);
                    firmware.U32(m_pMemory->GetBiosSize());
                    firmware.U32(m_pMemory->GetBiosCRC());
                });
    if (m_machineType == MachineType::MSX)
        section(kSectionMSX, static_cast<SystemBusMSX*>(m_systemBus.get())->StateVersion(), [&](std::ostream& out) { static_cast<SystemBusMSX*>(m_systemBus.get())->SaveState(out); });
    if (auto* bus = GetM5Bus()) section(kSectionM5, 2, [&](std::ostream& out) { bus->SaveState(out); });
#if GEARSF7000_ENABLE_GAMEGEAR
    if (m_machineType == MachineType::GameGear)
        section(kSectionGameGear, 1, [&](std::ostream& out) {
            static_cast<SystemBusGG*>(m_systemBus.get())->SaveState(out);
        });
#endif
    section(kSectionMemory, 2,
            [&](std::ostream& out) { m_pMemory->SaveState(out); });
    section(kSectionCpu, 1, [&](std::ostream& out)
            {
                if (!m_pCpuStatePersistence->SaveCpuState(out))
                    ok = false;
            });
    section(kSectionClocks, 1,
            [&](std::ostream& out) { m_clockDomains.SaveState(out); });
    section(kSectionVideo, m_pVideo->GetStateFormatVersion(),
            [&](std::ostream& out) { m_pVideo->SaveState(out); });
    section(kSectionAudio, 1,
            [&](std::ostream& out) { m_pAudio->SaveState(out); });
    section(kSectionKeyboard, 1,
            [&](std::ostream& out) { m_pKeyboard->SaveState(out); });
    section(kSectionColecoInput, 1,
            [&](std::ostream& out) { m_pColecoInput->SaveState(out); });
    section(kSectionPPI, 4, [&](std::ostream& out)
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

    const std::string path = GetSaveStatePath(szPath, index);
    if (path.empty())
    {
        m_LastStateError = "Load state: no content path; provide an explicit file path.";
        return false;
    }
    Log("Opening save file: %s", path.c_str());

    ifstream file(path.c_str(), ios::in | ios::binary);

    if (file.fail())
    {
        Log("Save state file doesn't exist: %s", path.c_str());
        return false;
    }

    const bool ok = LoadState(file);
    file.close();

    Log(ok ? "Save state loaded" : "Save state was refused");
    return ok;
}

bool GearSF7000Core::LoadState(const u8* buffer, size_t size)
{
    if (IsMachineReady() && (size > 0) && IsValidPointer(buffer))
    {
        Log("Gathering load state data [%d bytes]...", size);

        using namespace std;

        stringstream stream;

        stream.write(reinterpret_cast<const char*> (buffer), size);

        return LoadState(stream);
    }

    m_LastStateError = "Load state: machine not ready or snapshot buffer is empty.";
    Log("%s", m_LastStateError.c_str());

    return false;
}

bool GearSF7000Core::LoadState(std::istream& stream)
{
    if (!IsMachineReady())
    {
        m_LastStateError = StateReason("Machine not ready: required cartridge or firmware is missing.");
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
    if (formatVersion < kOldestReadableStateFormatVersion ||
        formatVersion > kStateFormatVersion)
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
    std::string videoDeviceKey;
    u16 videoStateVersion = 0;
    if (formatVersion >= 2)
    {
        videoDeviceKey = r.String();
        videoStateVersion = r.U16();
    }
    const u8 sgmPresent = formatVersion >= 3 ? r.U8() : 0;
    const u8 sgmLower = formatVersion >= 3 ? r.U8() : 0;
    const u8 sgmUpper = formatVersion >= 3 ? r.U8() : 0;
    const std::string videoOption = formatVersion >= 4 ? r.String() : std::string{};
    if (formatVersion >= 4 && videoOption != m_installedVideoOption) {
        m_LastStateError = StateReason("Load state: video device option %s differs from installed %s.",
            videoOption.c_str(), m_installedVideoOption.c_str());
        return false;
    }
    if (formatVersion < 4) {
        // Legacy states are accepted only by the original registered binding.
        const auto* active = GetVideoCatalog().FindOption(m_installedVideoOption);
        const auto* original = active ? GetVideoCatalog().FindLegacyOption(active->legacyDevice) : nullptr;
        if (!original || original->code != m_installedVideoOption) {
            m_LastStateError = "Load state: legacy snapshot has no compatible core identity.";
            return false;
        }
    }
    if (sgmPresent > 1 || sgmLower > 1 || sgmUpper > 1 ||
        (!sgmPresent && (sgmLower || sgmUpper)) ||
        bool(sgmPresent) != m_sgm.IsPresent())
    {
        m_LastStateError = "Load state: SGM hardware mismatch or invalid control state.";
        return false;
    }

    if (!r.Ok())
    {
        m_LastStateError = StateReason("Load state: header is truncated.");
        Log("%s", m_LastStateError.c_str());
        return false;
    }

    if (formatVersion >= 2)
    {
        const char* currentVideoKey = m_pVideo->GetDeviceDescriptor().key;
        if (currentVideoKey == nullptr || videoDeviceKey != currentVideoKey)
        {
            m_LastStateError = StateReason(
                "Load state: snapshot video device is %s, installed device is %s.",
                videoDeviceKey.c_str(), currentVideoKey ? currentVideoKey : "unknown");
            Log("%s", m_LastStateError.c_str());
            return false;
        }
        if (videoStateVersion != m_pVideo->GetStateFormatVersion())
        {
            m_LastStateError = StateReason(
                "Load state: video state version %u, installed %s expects %u.",
                static_cast<unsigned>(videoStateVersion), currentVideoKey,
                static_cast<unsigned>(m_pVideo->GetStateFormatVersion()));
            Log("%s", m_LastStateError.c_str());
            return false;
        }
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

    const GC_Region activeRegion = GetVideoFrameDescriptor().region;
    if ((region != 0) != (activeRegion == Region_PAL))
    {
        m_LastStateError = StateReason("Load state: snapshot is %s, the machine is %s.",
            region ? "PAL" : "NTSC",
            activeRegion == Region_PAL ? "PAL" : "NTSC");
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

    struct SavedSection { u32 id; u16 version; std::string bytes; };
    std::vector<SavedSection> sections;
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

        const auto payloadStart = stream.tellg();
        if (payloadStart < 0 || static_cast<size_t>(payloadStart) > streamSize - 8 ||
            length > streamSize - 8 - static_cast<size_t>(payloadStart))
        {
            m_LastStateError = "Load state: section exceeds snapshot size.";
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

        sections.push_back({id, version, std::move(bytes)});
    }

    // Validate RAM shape and mapper identity before applying any section.
    unsigned memorySections = 0;
    for (const auto& section : sections) {
        if (section.id != kSectionMemory) continue;
        ++memorySections;
        std::istringstream payload(section.bytes, std::ios::binary);
        if ((section.version != 1 && section.version != 2) || section.bytes.size() < 2 ||
            static_cast<u8>(section.bytes[0]) != section.version || section.bytes[1] != 0 ||
            !m_pMemory->ValidateState(payload) || payload.peek() != std::char_traits<char>::eof()) {
            m_LastStateError = "Load state: invalid RAM section or mapper payload."; return false;
        }
    }
    if (memorySections != 1) {
        m_LastStateError = "Load state: exactly one RAM section is required."; return false;
    }

    if (m_machineType == MachineType::GameGear) {
        unsigned ggSections = 0;
        for (const auto& section : sections)
            if (section.id == kSectionGameGear) {
                ++ggSections;
                if (section.version != 1 || section.bytes.size() != 5) {
                    m_LastStateError = "Load state: invalid Game Gear I/O section.";
                    return false;
                }
            }
        if (ggSections != 1) {
            m_LastStateError = "Load state: exactly one Game Gear I/O section is required.";
            return false;
        }
    }

    if (GetM5Bus()) {
        unsigned boards=0, firmware=0, family=0;
        for (const auto& section : sections) {
            firmware += section.id == kSectionFirmware; family += section.id == kSectionFamily;
            if (section.id != kSectionM5) continue;
            ++boards;
            SystemBusM5 candidate(SystemBusM5::ProvisionalPsgWaitTStates);
            candidate.SetEM5Installed(GetM5Bus()->EM5Installed());
            std::istringstream payload(section.bytes);
            if ((section.version != 1 && section.version != 2) || !candidate.LoadState(payload) ||
                payload.peek() != std::char_traits<char>::eof()) {
                m_LastStateError = "Invalid M5 board state"; return false;
            }
        }
        if (boards != 1 || firmware != 1 || family != 1) {
            m_LastStateError = "M5 state requires board, firmware and family sections"; return false;
        }
    }
    if (m_machineType == MachineType::MSX)
    {
        unsigned msx = 0, firmware = 0, family = 0;
        for (const auto& section : sections)
        {
            msx += section.id == kSectionMSX; firmware += section.id == kSectionFirmware; family += section.id == kSectionFamily;
            if (section.id == kSectionMSX)
            {
                SystemBusMSX candidate(*m_pMemory, *m_pCartridge, *m_pAudio, *m_pMachineIOPorts);
                candidate.CopyOptionalFirmware(*static_cast<SystemBusMSX*>(m_systemBus.get()));
#if GEARSF7000_ENABLE_SR1000
                candidate.SetTapeValidationSource(static_cast<SystemBusMSX*>(m_systemBus.get())->Cassette());
#endif
                std::istringstream bytes(section.bytes);
                if ((section.version < 1 || section.version > 5) || !candidate.LoadState(bytes, section.version) || bytes.peek() != std::char_traits<char>::eof())
                { m_LastStateError = "Invalid MSX board state, different firmware, or missing/different mounted tape"; return false; }
            }
        }
        if (msx != 1 || firmware != 1 || family != 1)
        { m_LastStateError = "MSX state requires board, firmware and family sections"; return false; }
    }
    // Firmware stays external to snapshots. Validate it before any device
    // changes, even if an imported state places its identity section last.
    // Older snapshots lack FWRM and retain their original compatibility rules.
    for (const auto& section : sections)
    {
        if (section.id != kSectionFirmware) continue;
        std::stringstream payload(section.bytes, std::ios::in | std::ios::binary);
        StateReader firmware(payload);
        const u32 size = firmware.U32();
        const u32 crc = firmware.U32();
        if (section.version != 1 || section.bytes.size() != 8 || !firmware.Ok() ||
            !m_pMemory->IsBiosLoaded() || size != m_pMemory->GetBiosSize() ||
            crc != m_pMemory->GetBiosCRC())
        {
            m_LastStateError = "Load state: firmware size/CRC mismatch or firmware missing.";
            return false;
        }
    }

    for (const auto& section : sections)
    {
        const u32 id = section.id;
        const u16 version = section.version;
        const std::string& bytes = section.bytes;
        const u32 length = static_cast<u32>(bytes.size());
        switch (id)
        {
            case kSectionFamily:
            {
                std::stringstream payload(bytes, std::ios::in | std::ios::binary);
                StateReader family(payload);
                const u8 saved = family.U8();
                const SystemFamily current = GetSystemFamily(m_systemModel);
                if (!family.Ok() || saved != static_cast<u8>(current))
                {
                    m_LastStateError = StateReason("Load state: snapshot is from the %s family, "
                        "the machine is %s.",
                        family.Ok() ? GetSystemFamilyName(static_cast<SystemFamily>(saved))
                                    : "unreadable",
                        GetSystemFamilyName(current));
                    Log("%s", m_LastStateError.c_str());
                    return false;
                }
                break;
            }

            case kSectionFirmware: // already validated before applying state
                break;

            case kSectionM5:
                if (!GetM5Bus() || (section.version != 1 && section.version != 2)) return false;
                apply(bytes, [&](std::istream& in) { if (!GetM5Bus()->LoadState(in)) in.setstate(std::ios::failbit); });
                break;
            case kSectionGameGear:
#if GEARSF7000_ENABLE_GAMEGEAR
                if (m_machineType != MachineType::GameGear || section.version != 1) return false;
                apply(bytes, [&](std::istream& in) {
                    if (!static_cast<SystemBusGG*>(m_systemBus.get())->LoadState(in))
                        in.setstate(std::ios::failbit);
                });
                break;
#else
                return false;
#endif
            case kSectionMSX:
                if (m_machineType != MachineType::MSX || (section.version < 1 || section.version > 5)) return false;
                apply(bytes, [&](std::istream& in) {
                    if (!static_cast<SystemBusMSX*>(m_systemBus.get())->LoadState(in, section.version)) in.setstate(std::ios::failbit);
                });
                break;
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
                apply(bytes, [&](std::istream& in) { m_pKeyboard->LoadState(in); });
                break;

            case kSectionColecoInput:
                apply(bytes, [&](std::istream& in) { m_pColecoInput->LoadState(in); });
                break;

            case kSectionPPI:
                if (m_pMachineIOPorts)
                    apply(bytes, [&](std::istream& in)
                          { m_pMachineIOPorts->LoadState(in, version); });
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

    {
        const CpuStateSnapshot cpu = GetCpuStateAccess()->GetCpuStateSnapshot();
        m_pCpuInterruptRouter->SetMaskableInterruptLine(cpu.intLine);
        m_pCpuInterruptRouter->SetNonMaskableInterruptLevel(cpu.nmiLine);
        m_pVdpInterruptLines->SyncLevel(
            m_pVdpInterruptLines->GetLineType() == VdpLineType::VDP_NMI
                ? cpu.nmiLine : cpu.intLine);
    }

    if (auto* bus = GetM5Bus()) {
        m_pVdpInterruptLines->SyncLevel(bus->CTC().GetSnapshot().channels[3].triggerHigh);
        bus->ReconcileInterrupt();
    }
    // PC7 is authoritative, including older MSX states whose audio payload
    // predates the click device. Validate/load candidates never drive audio.
    if (m_machineType == MachineType::MSX) {
        const bool level = static_cast<SystemBusMSX*>(m_systemBus.get())->KeyboardClickLevel();
        if (m_pAudio->GetOneBitDevice().GetLevel() != level)
            m_pAudio->GetOneBitDevice().RestoreLevel(level);
    }
    m_sgm.Restore(sgmLower != 0, sgmUpper != 0);
    RebuildDebugEventCatalog();
    m_LastStateError.clear();
    return true;
}

double GearSF7000Core::GetNativeFrameRate() const
{
    const GC_VideoFrameDescriptor frame = GetVideoFrameDescriptor();
    return frame.refresh_rate.denominator > 0
        ? static_cast<double>(frame.refresh_rate.numerator) /
          static_cast<double>(frame.refresh_rate.denominator)
        : 0.0;
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
    if (!InstallSelectedVideoDevice())
        return;

    // The installed video device owns its native clock, standard and raster.
    const GC_VideoFrameDescriptor frame = m_pVideo->GetFrameDescriptor();
    if (!frame.IsValid())
        return;

    GearSF7000ClockRates rates{
        m_systemBus->GetCpuPsgClockHz(),
        frame.device_clock_hz,
        GearSF7000ClockRate::SF7000FDC};

    // One multiplier, every oscillator. Scaling only some of them would move
    // the domains relative to each other, which is the one thing that would
    // actually change how the machine behaves.
    const double native = static_cast<double>(frame.refresh_rate.numerator) /
        static_cast<double>(frame.refresh_rate.denominator);

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
        rates.videoDeviceHz = static_cast<std::uint32_t>(
            static_cast<double>(rates.videoDeviceHz) * m_ActiveClockScale + 0.5);
        // The FDC runs off the SF-7000's own 8 MHz oscillator, which is not
        // on the machine's board and does not follow it anywhere.
    }

    const int cpuHz = static_cast<int>(rates.cpuHz);

    systemClock.Init(cpuHz);
    m_clockDomains.Configure(rates);
    m_clkMachineVBlank = false;

    RefreshM5Connections();
    m_pMemory->Reset();
    if (auto* bus = GetM5Bus()) bus->Reset();
#if GEARSF7000_ENABLE_GAMEGEAR
    if (m_machineType == MachineType::GameGear)
        static_cast<SystemBusGG*>(m_systemBus.get())->Reset();
#endif
    if (m_machineType == MachineType::MSX) static_cast<SystemBusMSX*>(m_systemBus.get())->Reset();
    m_sgm.Reset();
    m_pVdpInterruptLines->Reset();
    m_pCpuInterruptRouter->SetMaskableInterruptLine(false);
    m_pCpuInterruptRouter->SetNonMaskableInterruptLevel(false);
    m_pCpuExecution->ResetExecution();
    m_pAudio->SetPsgVariant((IsSMSSystem(m_systemModel) || IsGameGearSystem(m_systemModel))
        ? Sms_Apu::Variant::SegaIntegrated
        : Sms_Apu::Variant::SN76489AN);
    m_pAudio->SetPsgStereo(m_machineType == MachineType::GameGear);
#if GEARSF7000_ENABLE_YM2413
    m_pAudio->SetYm2413MachineAvailable(
        IsSMSSystem(m_systemModel));
    m_pAudio->SetYm2413Interface(Audio::FmInterface::ExternalFmUnit);
#endif
#if GEARSF7000_ENABLE_AY
    m_pAudio->SetAyMachine(m_machineType == MachineType::SC3000, m_sgm.IsPresent(), m_machineType == MachineType::MSX);
#endif
    m_pAudio->SetOneBitEnabled(m_systemBus->HasOneBitAudio());
    m_pAudio->Reset(frame.region, rates.cpuHz);
#if GEARSF7000_ENABLE_YM2413
    m_pMachineIOPorts->SetFmPortDevice(
        m_pAudio->IsYm2413Enabled() ? m_pAudio : nullptr);
#endif

    m_pVideo->Reset();
    m_pKeyboard->Reset();
    m_pColecoInput->Reset();
#if GEARSF7000_ENABLE_SR1000
    m_pSR1000->Reset(cpuHz);
#endif
#if GEARSF7000_ENABLE_SF7000
    m_pSF7000->Reset(true, rates.cpuHz, rates.fdcHz);
#endif
    m_pSerialPrinterPort->Reset();
    m_pCassettePort->Reset();
    m_pMachineIOPorts->Reset();

    // Audio::Reset latches optional sound hardware. Recompose at this stopped
    // boundary so Debug Events and the I/O decoder expose the same fitted
    // devices for the whole following run.
    RebuildDebugEventCatalog();

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

    bool loaded = GetCassette()->LoadTape(std::string(fileName));

    return loaded ? 1 : 0;
#else
    (void)fileName; (void)isReadOnly;
    return 0;
#endif
}

void GearSF7000Core::CassetteEject()
{
#if GEARSF7000_ENABLE_SR1000
    if (HasCassetteInterface()) GetCassette()->Eject();
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
    if (HasCassetteInterface()) GetCassette()->Play();
#endif
}

void GearSF7000Core::CassetteStop()
{
#if GEARSF7000_ENABLE_SR1000
    if (HasCassetteInterface()) GetCassette()->Stop();
#endif
}

void GearSF7000Core::CassetteRewind()
{
#if GEARSF7000_ENABLE_SR1000
    if (HasCassetteInterface()) GetCassette()->Rewind();
#endif
}
