/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2024-2026  Saverio Russo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "MachineIOPorts.h"
#include "Input.h"
#include "SaveStateStream.h"

MachineIOPorts::MachineIOPorts(Audio* pAudio, IVideoPortDevice* pVideo, SegaKeyboard* pKeyboard, SR1000* pSR1000, SF7000* pSF7000, SC3000SerialPrinterPort* pSerialPrinterPort, SegaCassettePort* pCassettePort, Cartridge* pCartridge, Memory* pMemory)
{
    m_pAudio = pAudio;
    m_pVideo = pVideo;
    m_pKeyboard = pKeyboard;
    m_pSR1000 = pSR1000;
    m_pSF7000 = pSF7000;
    m_pSerialPrinterPort = pSerialPrinterPort;
    m_pCassettePort = pCassettePort;
    m_pCartridge = pCartridge;
    m_pMemory = pMemory;
    basicDeserializer = new SegaBasicDeserializer(3580000, [this](uint8_t byte) { this->onByteReceived(byte); });
    Intel8255DebugConfiguration ppiDebug;
    ppiDebug.key = "event.sc3000.ppi";
    ppiDebug.parentKey = "event.sc3000";
    ppiDebug.name = "SC-3000 PPI";
    ppiDebug.ioPortBase = 0xDC;
    ppiDebug.hasIoPorts = true;
    m_sc3000Ppi = std::make_unique<Intel8255>(
        static_cast<IIntel8255Pins&>(*this), std::move(ppiDebug));
    for (unsigned player = 0; player < 2; ++player)
        SetControllerDevice(player, SystemControllerDevice::SmsPad);

#ifdef DEBUG_PORT
    fp_debug = fopen("d:\\data\\debug.txt", "wt");
#endif
}

MachineIOPorts::~MachineIOPorts()
{
    if (basicDeserializer)
    {
        delete basicDeserializer;
        basicDeserializer = 0;
    }

#ifdef DEBUG_PORT
    if (fp_debug)
    {
        fclose(fp_debug);
        fp_debug = 0;
    }
#endif
}

unsigned int MachineIOPorts::GetWaitStatesAt(
    uint64_t tstates, u8 port, bool write) const
{
    (void)tstates;

    // The TMS9918/9929 cannot stall the Z80: it has no READY/WAIT output pin
    // at all, and on the SC-3000 the CPU /WAIT line (IC1 pin 24, pulled up by
    // R5 4.7K) is driven only by the SN76489AN READY output (IC4 pin 4).
    // Verified on SC-3000_PAL_Schematics.pdf, MAIN P.C.B. CIRCUIT (PAL).
    //
    // The 2 us port delay and the 0..5.95 us access-window wait of the data
    // manual (Table 2-2 / programmer's guide Appendix B) are therefore
    // *requirements on the program*, not delays the chip imposes. A program
    // that ignores them does not get slowed down: the VDP address/data latch
    // can be overwritten and the pending byte may never reach VRAM. That data
    // loss belongs in the VDP sequencer, and it can only ever be reproduced if
    // the CPU is left free to run too fast - hence no wait here. The sequencer
    // still has to enforce minimum port latency and CPU-owned VRAM slots before
    // this behaviour can be considered cycle-accurate.
    //
    return GetWaitStates(port, write);
}

DebugProbe MachineIOPorts::GetDebugProbe(u8 port) const
{
    // PSG writes stay on the System Bus, as on every board: the SN76489
    // publishes its logical registers, the port attempt belongs to the bus
    // (see kSn76489DebugSpaces).
    DebugProbe probe{m_scBusDebugDevice, DebugCommonSpace::IoPorts, port};
    if ((port & 0xC0) == 0x80 && m_scVdpDebugDevice != 0)
    {
        probe.device = m_scVdpDebugDevice;
        probe.target = (port & 1) == 0 ? 0xBE : 0xBF;
    }
#if GEARSF7000_ENABLE_AY
    else if (m_pAudio->IsAyEnabled() && IsSC3000_AYport(port) &&
             m_scAyDebugDevice != 0)
        probe.device = m_scAyDebugDevice;
#endif
    else if (port >= 0xDC && port <= 0xDF && m_scPpiDebugDevice != 0)
        probe.device = m_scPpiDebugDevice;
    else if ((port == 0xE0 || port == 0xE1) && m_sfFdcDebugDevice != 0)
        probe.device = m_sfFdcDebugDevice;
    else if (port >= 0xE4 && port <= 0xE7 && m_sfPpiDebugDevice != 0)
        probe.device = m_sfPpiDebugDevice;
    else if ((port == 0xE8 || port == 0xE9) && m_sfUsartDebugDevice != 0)
        probe.device = m_sfUsartDebugDevice;
    return probe;
}

void MachineIOPorts::BindDebugEvents(const DebugDeviceRegistry& registry)
{
    const auto find = [&registry](const char* key) -> DebugDeviceId {
        const auto* device = registry.FindDevice(key);
        return device != NULL ? device->id : 0;
    };
    m_scBusDebugDevice = find("event.sc3000.bus");
    m_scVdpDebugDevice = find("event.sc3000.vdp");
    m_scPpiDebugDevice = find("event.sc3000.ppi");
    m_scAyDebugDevice = find("event.sc3000.ay");
    m_sgmDebugDevice = find("event.coleco.sgm");
    for (auto& controller : m_controllers)
        controller->BindDebugEvents(registry);
    m_sgmAyDebugDevice = find("event.coleco.sgm.ay");
    m_sfFdcDebugDevice = find("event.sc3000.sf7000.fdc");
    m_sfPpiDebugDevice = find("event.sc3000.sf7000.ppi2");
    m_sfUsartDebugDevice = find("event.sc3000.sf7000.usart");
    m_sgBusDebugDevice = find("event.sg1000.bus");
    m_sgVdpDebugDevice = find("event.sg1000.vdp");
    m_smsBusDebugDevice = find("event.sms.bus");
    if (m_smsBusDebugDevice == 0) m_smsBusDebugDevice = find("event.gg.bus");
    m_colecoBusDebugDevice = find("event.coleco.bus");
    m_colecoVdpDebugDevice = find("event.coleco.vdp");
    m_smsVdpDebugDevice = find("event.sms.vdp");
    if (m_smsVdpDebugDevice == 0) m_smsVdpDebugDevice = find("event.gg.vdp");
    m_smsPsgDebugDevice = find("event.sms.psg");
    if (m_smsPsgDebugDevice == 0) m_smsPsgDebugDevice = find("event.gg.psg");
    m_smsFmDebugDevice = find("event.sms.fm");
}

void MachineIOPorts::ConfigureSMS(bool pal)
{
    m_smsPal = pal;
    if (m_pVideo)
        m_pVideo->ConfigureSmsRaster(pal);
}

void MachineIOPorts::SetControllerDevice(unsigned player, SystemControllerDevice type)
{
    player &= 1;
    if (m_controllers[player] && m_controllers[player]->GetType() == type)
        return;
    // Buttons the host holds stay held across the swap.
    const u8 held = m_controllers[player] ? m_controllers[player]->GetButtons() : 0;
    m_controllers[player] = ControllerDevice::Create(type);
    m_controllers[player]->SetMemory(m_pMemory);
    m_controllers[player]->SetButton(held, true);
    m_controllers[player]->Reset();
    m_controllers[player]->OnTh(SmsThLevel(player), 0);
}

u8 MachineIOPorts::SmsPadLines(unsigned player) const
{
    const unsigned row = m_pKeyboard->GetJoystickPortA() |
        (static_cast<unsigned>(m_pKeyboard->GetJoystickPortB()) << 8);
    return static_cast<u8>((row >> (player ? 6 : 0)) & 0x3F);
}

void MachineIOPorts::UpdateControllerTh(uint64_t tstates)
{
    for (unsigned player = 0; player < 2; ++player)
        m_controllers[player]->OnTh(SmsThLevel(player), tstates);
}

bool MachineIOPorts::SmsThLevel(unsigned player) const
{
    const u8 outputBit = player == 0 ? 0x02 : 0x08;
    const u8 directionBit = player == 0 ? 0x20 : 0x80;
    // Direction=1 releases the pin; the console pull-up reads it high.
    return (m_smsIoControl & directionBit) != 0 ||
           (m_smsIoControl & outputBit) != 0;
}

bool MachineIOPorts::SmsTrLevel(unsigned player) const
{
    const u8 outputBit = player == 0 ? 0x01 : 0x04;
    const u8 directionBit = player == 0 ? 0x10 : 0x40;
    return (m_smsIoControl & directionBit) != 0 ||
           (m_smsIoControl & outputBit) != 0;
}

u8 MachineIOPorts::ReadFmPort(u8 port) const
{
    if (!m_fmPorts.Contains(port) || !m_fmDevice)
        return 0xFF;
    return port == m_fmPorts.control ? m_fmDevice->ReadControl() : 0xFF;
}

void MachineIOPorts::WriteFmPort(uint64_t tstates, u8 port, u8 value)
{
    if (!m_fmPorts.Contains(port) || !m_fmDevice)
        return;
    if (port != m_fmPorts.control)
    {
        // Supported SMS profiles clock CPU and YM2413 identically. F2 belongs
        // to the board mixer, so it does not restart the YM2413 write interval.
        const auto timing = m_fmDevice->ObserveAccess(tstates, port == m_fmPorts.data);
        if (timing.violation && m_smsFmDebugDevice != 0)
        {
            const u8 reg = port == m_fmPorts.select ? (value & 0x3f)
                                                   : m_fmDevice->GetSelectedRegister();
            const u64 packed = timing.required | (u64(port) << 8) |
                (u64(reg) << 16) | (u64(value) << 24);
            m_pMemory->CheckDeviceAccess(
                {m_smsFmDebugDevice, DebugCommonSpace::State, timing.required == 12 ? 0u : 1u},
                DebugEventAccess_Event, timing.elapsed, true, packed, true);
        }
    }
    if (port == m_fmPorts.select)
        m_fmDevice->SelectRegister(value);
    else if (port == m_fmPorts.data)
    {
        const u8 reg = m_fmDevice->GetSelectedRegister();
        m_fmDevice->WriteData(value);
        if (m_smsFmDebugDevice != 0)
            m_pMemory->CheckDeviceAccess(
                {m_smsFmDebugDevice, DebugCommonSpace::Registers, reg},
                DebugEventAccess_Event, 0, false, value, true);
    }
    else
        m_fmDevice->WriteControl(value);
}

u8 MachineIOPorts::SMSIn(uint64_t tstates, u8 port)
{
    if (m_fmPorts.Contains(port))
        return ReadFmPort(port);

    switch (port & 0xC0)
    {
        case 0x40:
            return (port & 1) == 0
                ? m_pVideo->ReadSmsVCounter(tstates)
                : m_pVideo->ReadSmsHCounter(tstates);
        case 0x80:
            return (port & 1) == 0
                ? m_pVideo->GetDataPort(tstates)
                : m_pVideo->GetStatusFlags();
        case 0xC0:
        {
            // Each port's six lines come from what is plugged into it; with the
            // SMS pad they are the joystick row's, as before.
            const u8 padA = m_controllers[0]->Read(SmsPadLines(0), SmsThLevel(0), tstates);
            const u8 padB = m_controllers[1]->Read(SmsPadLines(1), SmsThLevel(1), tstates);
            if ((port & 1) == 0)
            {
                u8 value = static_cast<u8>(padA | ((padB & 0x03) << 6));
                if ((m_smsIoControl & 0x10) == 0)
                    value = static_cast<u8>((value & ~0x20u) |
                        (SmsTrLevel(0) ? 0x20u : 0u));
                return value;
            }
            u8 value = static_cast<u8>((m_pKeyboard->GetJoystickPortB() & 0xF0) |
                ((padB >> 2) & 0x0F));
            if ((m_smsIoControl & 0x40) == 0)
                value = static_cast<u8>((value & ~0x08u) |
                    (SmsTrLevel(1) ? 0x08u : 0u));
            if ((m_smsIoControl & 0x20) == 0)
                value = static_cast<u8>((value & ~0x40u) |
                    (SmsThLevel(0) ? 0x40u : 0u));
            if ((m_smsIoControl & 0x80) == 0)
                value = static_cast<u8>((value & ~0x80u) |
                    (SmsThLevel(1) ? 0x80u : 0u));
            return value;
        }
        default:
            return 0xFF;
    }
}

void MachineIOPorts::SMSOut(uint64_t tstates, u8 port, u8 value)
{
    if (m_fmPorts.Contains(port))
    {
        WriteFmPort(tstates, port, value);
        return;
    }

    switch (port & 0xC0)
    {
        case 0x00:
            if ((port & 1) == 0)
                m_smsMemoryControl = value;
            else
            {
                const bool oldThA = SmsThLevel(0);
                const bool oldThB = SmsThLevel(1);
                m_smsIoControl = value;
                if ((!oldThA && SmsThLevel(0)) ||
                    (!oldThB && SmsThLevel(1)))
                    m_pVideo->LatchSmsHCounter(tstates);
                UpdateControllerTh(tstates);
            }
            break;
        case 0x40:
        {
            m_pAudio->WriteAudioRegister(value);
            const Sms_Apu::WriteInfo& write =
                m_pAudio->GetApu()->GetLastWriteInfo();
            m_pMemory->CheckAudioEvent(write.reg, write.rawData);
            break;
        }
        case 0x80:
            if ((port & 1) == 0)
                m_pVideo->WriteData(tstates, value);
            else
                m_pVideo->WriteControl(tstates, value);
            break;
        default:
            break;
    }
}

unsigned int MachineIOPorts::SMSGetWaitStates(u8, bool) const
{
    // The SMS integrates the PSG with the VDP; there is no external
    // SN76489 READY pin adding the SG/SC 16-T-state write stall.
    return 0;
}

DebugProbe MachineIOPorts::GetSMSDebugProbe(u8 port) const
{
    DebugProbe probe{m_smsBusDebugDevice, DebugCommonSpace::IoPorts, port};
    if (m_fmDevice && m_fmPorts.Contains(port) && m_smsFmDebugDevice != 0)
    {
        probe.device = m_smsFmDebugDevice;
        return probe;
    }
    // $40-$7F is direction-dependent (counter reads, PSG writes), while the
    // probe contract has no access direction. Keep its raw transaction on
    // the system bus; accepted PSG writes already publish their own event.
    if ((port & 0xC0) == 0x80 && m_smsVdpDebugDevice != 0)
    {
        probe.device = m_smsVdpDebugDevice;
        probe.target = (port & 1) == 0 ? 0xBE : 0xBF;
    }
    return probe;
}

unsigned int MachineIOPorts::SG1000GetWaitStates(u8 port, bool write) const
{
    // The SG-1000 uses the same SN76489 READY connection as the SC-3000.
    return write && (port & 0xC0) == 0x40 ? 16u : 0u;
}

DebugProbe MachineIOPorts::GetSG1000DebugProbe(u8 port,
                                               bool sk1100Attached) const
{
    // PSG writes stay on the System Bus (see GetDebugProbe).
    DebugProbe probe{m_sgBusDebugDevice, DebugCommonSpace::IoPorts, port};
    if ((port & 0xC0) == 0x80 && m_sgVdpDebugDevice != 0)
    {
        probe.device = m_sgVdpDebugDevice;
        probe.target = (port & 1) == 0 ? 0xBE : 0xBF;
    }
    else if (sk1100Attached && (port & 0x20) == 0 &&
             m_scPpiDebugDevice != 0)
    {
        probe.device = m_scPpiDebugDevice;
        probe.target = static_cast<u8>(0xDC | (port & 3));
    }
    return probe;
}

u8 MachineIOPorts::ColecoIn(uint64_t tstates, u8 port)
{
#if GEARSF7000_ENABLE_AY
    if (port == 0x52 && m_pMemory->GetSGMDevice() && m_pMemory->GetSGMDevice()->IsPresent())
        return m_pAudio->AyReadData();
#endif
    switch (port & 0xE0)
    {
        case 0xA0:
            return (port & 1) == 0
                ? m_pVideo->GetDataPort(tstates)
                : m_pVideo->GetStatusFlags();
        case 0xE0:
            // A1 picks the controller; what the byte means depends on the
            // mode last selected through $80/$C0.
            return m_pColecoInput ? m_pColecoInput->ReadInput(port) : 0xFF;
        default:
            return 0xFF;
    }
}

void MachineIOPorts::ColecoOut(uint64_t tstates, u8 port, u8 value)
{
    SGMDevice* sgm = m_pMemory->GetSGMDevice();
    if (sgm && sgm->IsPresent())
    {
        const bool before = port == 0x53 ? sgm->UpperEnabled() : sgm->LowerEnabled();
        if (sgm->WriteControl(port, value))
        {
            const bool after = port == 0x53 ? sgm->UpperEnabled() : sgm->LowerEnabled();
            if (m_sgmDebugDevice && before != after)
                m_pMemory->CheckDeviceStateChange({m_sgmDebugDevice, DebugCommonSpace::State,
                    static_cast<u32>(port == 0x53 ? 1 : 0)}, before, after);
            return;
        }
#if GEARSF7000_ENABLE_AY
        if (port == 0x50) { m_pAudio->AySelectRegister(value); return; }
        if (port == 0x51)
        {
            m_pAudio->AyWriteData(value);
            m_pMemory->CheckAyEvent(m_pAudio->GetAySelectedRegister(), value);
            return;
        }
#endif
    }
    m_pMemory->WriteMapperPort(port, value);

    switch (port & 0xE0)
    {
        case 0x80:
            if (m_pColecoInput)
                m_pColecoInput->SetInputSegment(Input::SegmentKeypadRightButtons);
            break;
        case 0xA0:
            if ((port & 1) == 0)
                m_pVideo->WriteData(tstates, value);
            else
                m_pVideo->WriteControl(tstates, value);
            break;
        case 0xC0:
            if (m_pColecoInput)
                m_pColecoInput->SetInputSegment(Input::SegmentJoystickLeftButtons);
            break;
        case 0xE0:
        {
            m_pAudio->WriteAudioRegister(value);
            const Sms_Apu::WriteInfo& write =
                m_pAudio->GetApu()->GetLastWriteInfo();
            m_pMemory->CheckAudioEvent(write.reg, write.rawData);
            break;
        }
        default:
            break;
    }
}

unsigned int MachineIOPorts::ColecoGetWaitStates(u8 port, bool write) const
{
    // SN76489AN READY to /WAIT (schematic, Sound sheet). Same figure as the
    // SG-1000 until the stall is measured on hardware; then both change.
    return write && (port & 0xE0) == 0xE0 ? 16u : 0u;
}

DebugProbe MachineIOPorts::GetColecoDebugProbe(u8 port) const
{
    if (port >= 0x50 && port <= 0x52 && m_sgmAyDebugDevice)
        return {m_sgmAyDebugDevice, DebugCommonSpace::IoPorts, port};
    // $E0-$FF is direction-dependent (controller reads, PSG writes) and the
    // probe carries no direction, so it stays on the System Bus with its raw
    // port, as the SMS keeps $40-$7F; the PSG publishes its registers.
    DebugProbe probe{m_colecoBusDebugDevice, DebugCommonSpace::IoPorts, port};
    if ((port & 0xE0) == 0xA0 && m_colecoVdpDebugDevice != 0)
    {
        probe.device = m_colecoVdpDebugDevice;
        probe.target = (port & 1) == 0 ? 0xBE : 0xBF;
    }
    return probe;
}

u8 MachineIOPorts::SG1000In(uint64_t tstates, u8 port, bool sk1100Attached)
{
    switch (port & 0xC0)
    {
        case 0x80:
            return (port & 1) == 0
                ? m_pVideo->GetDataPort(tstates)
                : m_pVideo->GetStatusFlags();
        case 0xC0:
            if (sk1100Attached && (port & 0x20) == 0)
                return m_sc3000Ppi->ReadRegister(port & 3, tstates);
            // A0 selects one of the two bytes produced by the onboard
            // joystick multiplexers. A1-A5 are not decoded.
            return (port & 1) == 0
                ? m_pKeyboard->GetJoystickPortA()
                : m_pKeyboard->GetJoystickPortB();
        default:
            return 0xFF;
    }
}

void MachineIOPorts::SG1000Out(uint64_t tstates, u8 port, u8 value,
                               bool sk1100Attached)
{
    // A cartridge is a separate bus responder and may decode I/O even when
    // no motherboard device owns this block.
    m_pMemory->WriteMapperPort(port, value);

    switch (port & 0xC0)
    {
        case 0x40:
        {
            m_pAudio->WriteAudioRegister(value);
            const Sms_Apu::WriteInfo& write =
                m_pAudio->GetApu()->GetLastWriteInfo();
            m_pMemory->CheckAudioEvent(write.reg, write.rawData);
            break;
        }
        case 0x80:
            if ((port & 1) == 0)
                m_pVideo->WriteData(tstates, value);
            else
                m_pVideo->WriteControl(tstates, value);
            break;
        case 0xC0:
            if (sk1100Attached && (port & 0x20) == 0)
            {
                const Intel8255Snapshot before = m_sc3000Ppi->GetSnapshot();
                m_sc3000Ppi->WriteRegister(port & 3, value, tstates);
                const Intel8255Snapshot after = m_sc3000Ppi->GetSnapshot();
                SC3000_PPI_PortA = after.outputLatch[0];
                SC3000_PPI_PortB = after.outputLatch[1];
                SC3000_PPI_PortC = after.outputLatch[2];
                SC3000_PPI_PortCTRL = after.control;
                if ((port & 3) == 3 && before.control != after.control)
                    m_pMemory->CheckPPIStateChange(
                        static_cast<u16>(0xDC | (port & 3)),
                        before.control, after.control);
            }
            break;
        default:
            break;
    }
}

void MachineIOPorts::Reset()
{
    if (basicDeserializer)
        basicDeserializer->Reset(3580000);

    SC3000_PPI_PortA = 0x00;
    SC3000_PPI_PortB = 0x00;
    SC3000_PPI_PortC = 0x00;
    SC3000_PPI_PortCTRL = 0x9B;
    if (m_sc3000Ppi)
        m_sc3000Ppi->Reset();
    m_smsMemoryControl = 0xFF;
    m_smsIoControl = 0xFF;
    for (auto& controller : m_controllers)
        controller->Reset();
    if (m_pVideo)
        m_pVideo->ConfigureSmsRaster(m_smsPal);
}

void MachineIOPorts::onByteReceived(uint8_t) {

}

void MachineIOPorts::SaveState(std::ostream& stream) const
{
    StateWriter w(stream);
    m_sc3000Ppi->SaveState(stream);
    w.U8(SR1000_CassetteSignal);
    w.Bool(m_smsPal);
    w.U8(m_smsMemoryControl);
    w.U8(m_smsIoControl);
    // Version 4: each controller port's device and its state. The device type is
    // the user's setting; a state from another type is read and set aside.
    for (const auto& controller : m_controllers)
    {
        w.U8(static_cast<u8>(controller->GetType()));
        controller->SaveState(w);
    }
}

void MachineIOPorts::LoadState(std::istream& stream, std::uint16_t version)
{
    if (version == 1)
    {
        StateReader legacy(stream);
        const uint8_t portA = legacy.U8();
        const uint8_t portB = legacy.U8();
        const uint8_t portC = legacy.U8();
        const uint8_t control = legacy.U8();
        const uint8_t cassette = legacy.U8();
        if (!legacy.Ok()) return;
        m_sc3000Ppi->Reset();
        if (control & 0x80) m_sc3000Ppi->WriteRegister(3, control);
        m_sc3000Ppi->WriteRegister(0, portA);
        m_sc3000Ppi->WriteRegister(1, portB);
        m_sc3000Ppi->WriteRegister(2, portC);
        SC3000_PPI_PortA = portA;
        SC3000_PPI_PortB = portB;
        SC3000_PPI_PortC = portC;
        SC3000_PPI_PortCTRL = control;
        SR1000_CassetteSignal = cassette;
        return;
    }
    if (version < 2 || version > 4)
        return;
    if (!m_sc3000Ppi->LoadState(stream))
        return;
    StateReader r(stream);
    SR1000_CassetteSignal = r.U8();
    if (version >= 3)
    {
        m_smsPal = r.Bool();
        m_smsMemoryControl = r.U8();
        m_smsIoControl = r.U8();
    }
    if (version >= 4)
    {
        for (auto& controller : m_controllers)
        {
            const int type = r.U8();
            if (!IsValidControllerDevice(type))
                return;
            if (static_cast<SystemControllerDevice>(type) == controller->GetType())
                controller->LoadState(r);
            else
                ControllerDevice::Create(static_cast<SystemControllerDevice>(type))->LoadState(r);
        }
    }
    if (!r.Ok()) return;
    const Intel8255Snapshot state = m_sc3000Ppi->GetSnapshot();
    SC3000_PPI_PortA = state.outputLatch[0];
    SC3000_PPI_PortB = state.outputLatch[1];
    SC3000_PPI_PortC = state.outputLatch[2];
    SC3000_PPI_PortCTRL = state.control;
    if (m_pVideo)
        m_pVideo->ConfigureSmsRaster(m_smsPal);
}

uint8_t MachineIOPorts::SC3000_PPI_GetPortA()
{
    const uint8_t value = m_pKeyboard->GetPortA();
    SC3000_PPI_PortA = value;
    return value;
}

uint8_t MachineIOPorts::SC3000_PPI_GetPortB(uint64_t tstates)
{
#if GEARSF7000_ENABLE_SR1000
    SR1000_CassetteSignal = m_pSR1000->GetSignal();
#endif
    m_pCassettePort->ObserveTapeInput(SR1000_CassetteSignal != 0);
    const Intel8255PinSample printer =
        m_pSerialPrinterPort->ReadPortB(tstates);
    const uint8_t value =
#if GEARSF7000_ENABLE_SR1000
        (SR1000_CassetteSignal != 0 ? 0x80 : 0x00)
#else
        0x00
#endif
        | printer.value
        | (m_pKeyboard->GetPortB() & 0x1F);
    SC3000_PPI_PortB = value;
    return value;
}

Intel8255PinSample MachineIOPorts::ReadPortA(std::uint64_t)
{
    return {SC3000_PPI_GetPortA(), 0xFF};
}

Intel8255PinSample MachineIOPorts::ReadPortB(std::uint64_t timestamp)
{
    return {SC3000_PPI_GetPortB(timestamp), 0xFF};
}

Intel8255PinSample MachineIOPorts::ReadPortC(std::uint64_t)
{
    return {SC3000_PPI_SamplePortC(), 0xFF};
}

Intel8255PinSample MachineIOPorts::PeekPortA(std::uint64_t) const
{
    return {m_pKeyboard->GetPortA(), 0xFF};
}

Intel8255PinSample MachineIOPorts::PeekPortB(std::uint64_t) const
{
    const uint8_t value =
#if GEARSF7000_ENABLE_SR1000
        (m_pSR1000->GetSignal() != 0 ? 0x80 : 0x00)
#else
        0x00
#endif
        | m_pSerialPrinterPort->PeekPortB().value
        | (m_pKeyboard->GetPortB() & 0x1F);
    return {value, 0xFF};
}

Intel8255PinSample MachineIOPorts::PeekPortC(std::uint64_t) const
{
    uint8_t value = m_pKeyboard->GetPortC() & 0x07;
#if GEARSF7000_ENABLE_SR1000
    value |= m_pSR1000->IsMotorOn() ? 0x10 : 0x00;
#endif
    return {value, 0xFF};
}

void MachineIOPorts::DrivePortA(std::uint8_t value, std::uint8_t mask,
                                std::uint64_t)
{
    SC3000_PPI_PortA = static_cast<uint8_t>(
        (SC3000_PPI_PortA & ~mask) | (value & mask));
}

void MachineIOPorts::DrivePortB(std::uint8_t value, std::uint8_t mask,
                                std::uint64_t)
{
    SC3000_PPI_PortB = static_cast<uint8_t>(
        (SC3000_PPI_PortB & ~mask) | (value & mask));
}

void MachineIOPorts::DrivePortC(std::uint8_t value, std::uint8_t mask,
                                std::uint64_t timestamp)
{
    if (mask == 0)
    {
        // A mode-set can release every Port C pin without changing the
        // output latch. Propagate that electrical state to the connectors.
        m_pSerialPrinterPort->DrivePortC(value, 0, timestamp);
        m_pCassettePort->DrivePortC(value, 0);
        return;
    }
    const uint8_t resolved = static_cast<uint8_t>(
        (SC3000_PPI_PortC & ~mask) | (value & mask));
    SC3000_PPI_SetPortC(timestamp, resolved, mask);
}
