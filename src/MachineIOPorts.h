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

#ifndef MACHINEIOPORTS_H
#define	MACHINEIOPORTS_H


#include "IOPorts.h"
#include "IVideoPortDevice.h"
#include "IFmPortDevice.h"
#include "Intel8255.h"
#include "ControllerDevice.h"

#include <cstdint>
#include <vector>
#include <functional>
#include <cmath>
#include <iostream>
#include <istream>
#include <memory>
#include <ostream>
#include "SegaBasicDeserializer.h"
#include <stdio.h>

class Audio;
class SegaKeyboard;
class Input;
class SF7000;
class SC3000SerialPrinterPort;
class SegaCassettePort;
class SR1000;
class Cartridge;
class Memory;
class MachineIOPorts : public IOPorts, private IIntel8255Pins
{
public:
    MachineIOPorts(Audio* pAudio, IVideoPortDevice* pVideo, SegaKeyboard* pKeyboard, SR1000* pSR1000, SF7000* pSF7000, SC3000SerialPrinterPort* pSerialPrinterPort, SegaCassettePort* pCassettePort, Cartridge* pCartridge, Memory* pMemory);
    ~MachineIOPorts();
    void Reset() override;
    u8 In(uint64_t tstates, u8 port) override;
    void Out(uint64_t tstates, u8 port, u8 value) override;
    unsigned int GetWaitStates(u8 port, bool write) const override
    {
        // The SN76489AN READY line stalls the Z80 after a write.  This is the
        // existing GearSF7000 timing (16 extra T-states), now exposed as a
        // property of the bus rather than injected into the CPU directly.
        return write && port >= 0x40 && port < 0x80 ? 16u : 0u;
    }
    unsigned int GetWaitStatesAt(uint64_t tstates, u8 port,
                                 bool write) const override;
    DebugProbe GetDebugProbe(u8 port) const override;
    void BindDebugEvents(const DebugDeviceRegistry& registry);
    IVideoPortDevice* GetVideoDevice() const { return m_pVideo; }
    void SetVideoDevice(IVideoPortDevice* video) { m_pVideo = video; }
    const Intel8255& GetSC3000PPI() const { return *m_sc3000Ppi; }

    // SG-1000 motherboard routing. These deliberately bypass the onboard
    // SC-3000 PPI and all SF-7000 registers. A future SK-1100 composition
    // will add its own responder to the $C0-$FF block.
    u8 SG1000In(uint64_t tstates, u8 port, bool sk1100Attached = false);
    void SG1000Out(uint64_t tstates, u8 port, u8 value,
                   bool sk1100Attached = false);
    unsigned int SG1000GetWaitStates(u8 port, bool write) const;
    DebugProbe GetSG1000DebugProbe(u8 port,
                                   bool sk1100Attached = false) const;

    // ColecoVision motherboard routing (schematic, Processor sheet: a
    // 74LS138 on A5-A7 with /IORQ and /WR, so every block repeats every 32
    // ports). $80 write: keypad mode; $A0: VDP, A0 selects data/control;
    // $C0 write: joystick mode; $E0: controllers in, SN76489 out.
    void SetColecoInput(Input* input) { m_pColecoInput = input; }
    u8 ColecoIn(uint64_t tstates, u8 port);
    void ColecoOut(uint64_t tstates, u8 port, u8 value);
    unsigned int ColecoGetWaitStates(u8 port, bool write) const;
    DebugProbe GetColecoDebugProbe(u8 port) const;

    // Master System motherboard routing. Exact $F0-$F2 FM decoding is
    // checked before the otherwise mirrored controller block.
    void ConfigureSMS(bool pal);
    // What is plugged into each Master System controller port (player 0 is port A):
    // the SMS pad unless the user picks a Mega Drive pad. A user setting, like a
    // controller in the socket - not part of a machine's model.
    void SetControllerDevice(unsigned player, SystemControllerDevice type);
    ControllerDevice& GetControllerDevice(unsigned player) { return *m_controllers[player & 1]; }
    const ControllerDevice& GetControllerDevice(unsigned player) const { return *m_controllers[player & 1]; }
    u8 SMSIn(uint64_t tstates, u8 port);
    void SMSOut(uint64_t tstates, u8 port, u8 value);
    unsigned int SMSGetWaitStates(u8 port, bool write) const;
    DebugProbe GetSMSDebugProbe(u8 port) const;
    void SetFmPortDevice(IFmPortDevice* device,
                         FmPortMap ports = FmPortMap{})
    {
        m_fmDevice = device;
        m_fmPorts = ports;
    }
    u8 GetSMSMemoryControl() const { return m_smsMemoryControl; }
    u8 GetSMSIoControl() const { return m_smsIoControl; }
    
    uint8_t SC3000_PPI_GetPortA();
    
    uint8_t SC3000_PPI_GetPortB(uint64_t tstates = 0);

    void SC3000_PPI_SetPortC(uint64_t tstates, uint8_t val,
                             uint8_t drivenMask = 0xFF);
    uint8_t SC3000_PPI_SamplePortC();
    uint8_t SC3000_PPI_GetPortC(uint64_t tstates);

    // The 8255's four registers plus the latched cassette input level. This
    // is the whole of the port decoder's own state: everything else it owns
    // is a pointer to another device, which saves itself.
    void SaveState(std::ostream& stream) const;
    void LoadState(std::istream& stream, std::uint16_t version = 3);

    void SC3000_PPI_GetPorts(uint8_t* ports)
    {
        if (!ports) return;
		
        ports[0] = SC3000_PPI_PortA;
        ports[1] = SC3000_PPI_PortB;
        ports[2] = SC3000_PPI_PortC;
		ports[3] = SC3000_PPI_PortCTRL;
    }

private:
    Intel8255PinSample ReadPortA(std::uint64_t timestamp) override;
    Intel8255PinSample ReadPortB(std::uint64_t timestamp) override;
    Intel8255PinSample ReadPortC(std::uint64_t timestamp) override;
    Intel8255PinSample PeekPortA(std::uint64_t timestamp) const override;
    Intel8255PinSample PeekPortB(std::uint64_t timestamp) const override;
    Intel8255PinSample PeekPortC(std::uint64_t timestamp) const override;
    void DrivePortA(std::uint8_t value, std::uint8_t drivenMask,
                    std::uint64_t timestamp) override;
    void DrivePortB(std::uint8_t value, std::uint8_t drivenMask,
                    std::uint64_t timestamp) override;
    void DrivePortC(std::uint8_t value, std::uint8_t drivenMask,
                    std::uint64_t timestamp) override;

    Audio* m_pAudio;
    IVideoPortDevice* m_pVideo;
    SegaKeyboard* m_pKeyboard;
    SR1000* m_pSR1000;
    SF7000* m_pSF7000;
    SC3000SerialPrinterPort* m_pSerialPrinterPort;
    SegaCassettePort* m_pCassettePort;
    Cartridge* m_pCartridge;
    Memory* m_pMemory;
    uint8_t SC3000_PPI_PortA = 0;
    uint8_t SC3000_PPI_PortB = 0;
    uint8_t SC3000_PPI_PortC = 0;
    uint8_t SC3000_PPI_PortCTRL = 0x9B;
    std::unique_ptr<Intel8255> m_sc3000Ppi;
    DebugDeviceId m_scBusDebugDevice = 0;
    DebugDeviceId m_scVdpDebugDevice = 0;
    DebugDeviceId m_scPpiDebugDevice = 0;
    DebugDeviceId m_scAyDebugDevice = 0;
    DebugDeviceId m_sgmDebugDevice = 0;
    DebugDeviceId m_sgmAyDebugDevice = 0;
    DebugDeviceId m_sfFdcDebugDevice = 0;
    DebugDeviceId m_sfPpiDebugDevice = 0;
    DebugDeviceId m_sfUsartDebugDevice = 0;
    DebugDeviceId m_sgBusDebugDevice = 0;
    DebugDeviceId m_sgVdpDebugDevice = 0;
    DebugDeviceId m_smsBusDebugDevice = 0;
    DebugDeviceId m_colecoBusDebugDevice = 0;
    DebugDeviceId m_colecoVdpDebugDevice = 0;
    Input* m_pColecoInput = nullptr;
    DebugDeviceId m_smsVdpDebugDevice = 0;
    DebugDeviceId m_smsPsgDebugDevice = 0;
    DebugDeviceId m_smsFmDebugDevice = 0;

    IFmPortDevice* m_fmDevice = nullptr;
    FmPortMap m_fmPorts{};
    bool m_smsPal = false;
    std::unique_ptr<ControllerDevice> m_controllers[2];
    // The SMS pad lines of each player as the joystick row holds them: player 1 in
    // bits 0-5 of $DC; player 2's up and down in bits 6-7 of $DC, the rest in bits
    // 0-3 of $DD.
    u8 SmsPadLines(unsigned player) const;
    void UpdateControllerTh(uint64_t tstates);
    uint8_t m_smsMemoryControl = 0xFF;
    uint8_t m_smsIoControl = 0xFF;

    uint8_t SR1000_CassetteSignal = 0;

    SegaBasicDeserializer *basicDeserializer;
    void onByteReceived(uint8_t byte);
    bool SmsThLevel(unsigned player) const;
    bool SmsTrLevel(unsigned player) const;
    u8 ReadFmPort(u8 port) const;
    void WriteFmPort(uint64_t tstates, u8 port, u8 value);

#ifdef DEBUG_PORT
    FILE* fp_debug;
#endif
};

#include "Audio.h"
#include "SegaKeyboard.h"
#if GEARSF7000_ENABLE_SR1000
#include "SR1000.h"
#endif
#if GEARSF7000_ENABLE_SF7000
#include "SF7000.h"
#endif
#include "SC3000SerialPrinterPort.h"
#include "SegaCassettePort.h"
#include "Cartridge.h"
#include "Memory.h"
#ifdef DEBUG_PORT
static inline bool IsSC3000_DEBUGport(u8 port)
{
    return ((port >= 0x00) && (port < 0x40));
    //return ((port & 0xC0) == 0x40);
}
#endif


static inline bool IsSC3000_PSGport(u8 port)
{
    return ((port >= 0x40) && (port < 0x80));
    //return ((port & 0xC0) == 0x40);
}

#if GEARSF7000_ENABLE_AY
// AY-3-8910 / YM2149 on an SGM-style expansion.
//
// Address choice. On a real SC-3000 bus the only block where no machine IC is
// selected is $20-$3F: A7=0, A6=0, A5=1. Everything else is taken - PSG at
// A7=0/A6=1, VDP at A7=1/A6=0, PPI wherever A5=0, and $E0-$FF for the SF-7000.
// So neither the ColecoVision SGM's $50-$52 (lands on the PSG chip select) nor
// the MSX's $A0-$A2 (lands on the VDP) can be used verbatim here.
//
// $20 keeps the MSX register layout with only bit 7 cleared: $A0/$A1/$A2
// becomes $20/$21/$22, so porting MSX code is a mechanical substitution -
// which matters, since that is what this emulator is for.
//
// A card should decode the whole $20-$3F block - one three-input gate - and
// subdivide internally with A4-A2, leaving seven more slots of four ports for
// a second sound chip and mapper registers without touching the main decode.
// Narrowing the decode to $20-$23 would need more gates for less room.
//
// The base is a variable so the address can be moved without rebuilding,
// because the board it has to match does not exist yet. It must be `inline`
// and not `static`: two translation units include this header, and a static
// would give each its own copy, so setting it from the UI would change one
// and leave the emulator reading the other.
inline u8 g_ay_port_base = 0x20;

inline void SetAyPortBase(u8 base) { g_ay_port_base = base & 0xFC; }
inline u8 GetAyPortBase() { return g_ay_port_base; }

inline bool IsSC3000_AYport(u8 port)
{
    return (port & 0xFC) == g_ay_port_base;
}
#endif


static inline bool IsSC3000_PPIport(u8 port)
{
    return ((port >= 0xDC) && (port < 0xE0));
    //return ((port >= 0xC0) && (port < 0xE0));
    //return ((port & 0x20) == 0);
}

static inline bool IsSC3000_VDPport(u8 port)
{
    //return ((port >= 0x80) && (port < 0xC0));
    return ((port & 0xC0) == 0x80);
}

static inline bool IsSF7000_port(u8 port)
{
    //return ((port >= 0xE0) && (port < 0xF0));
    return ((port & 0xE0) == 0xE0);
}



inline void MachineIOPorts::SC3000_PPI_SetPortC(uint64_t tstates,
                                                uint8_t value,
                                                uint8_t drivenMask)
{
    const uint8_t oldValue = SC3000_PPI_PortC;
    SC3000_PPI_PortC = value;

#ifdef _DEBUG
        //printf("\n[SC-3000 PPI C] Write Cassette Signal %d\n", value & 0x10 ? 1 : 0);
#endif
#if GEARSF7000_ENABLE_SR1000
    if (basicDeserializer)
    {
        bool signal = value & 0x10 ? 1 : 0;
        basicDeserializer->Set(tstates, signal);
        //m_pAudio->SR1000SpeakerWrite(signal ? 1 : 0);
    }

    m_pSR1000->SetMotor((value & 0x08) != 0);       // PC3 Cassette Motor (not connected on the real machine but used on basic)
#endif
	m_pKeyboard->SetPortC(value);
	m_pSerialPrinterPort->DrivePortC(value, drivenMask, tstates);
    m_pCassettePort->DrivePortC(value, drivenMask);

    // This also covers the BSR path: publish the resulting PC latch only
    // when it really changed, rather than the raw OUT (available as I/O).
    if (oldValue != value)
        m_pMemory->CheckPPIStateChange(0xDE, oldValue, value);

#ifdef _DEBUG
    //printf("*** [SC-3000 PPI]: Write to Port C\n");

    //printf("\t  DIN-5 /RESET    => %d\n", (value >> 6) & 1);
    //printf("\t  DIN-5 /FEED     => %d\n", (value >> 7) & 1);
#endif
}

inline uint8_t MachineIOPorts::SC3000_PPI_SamplePortC()
{
    //SC3000_PPI_PortC
    return SC3000_PPI_PortC = (

        // (m_pKeyboard->GetPortC() & 0xF7)

		      (m_pKeyboard->GetPortC() & 0x07)             //                   -   PC2, PC1, PC0
#if GEARSF7000_ENABLE_SR1000
            | (m_pSR1000->IsMotorOn() ? 0x10 : 0x00)    //               PC4
#endif
        ); // PC3 Cassette Motor (not connected on the real machine but used on basic)
}

inline uint8_t MachineIOPorts::SC3000_PPI_GetPortC(uint64_t tstates)
{
    return m_sc3000Ppi->ReadRegister(2, tstates);
}



inline u8 MachineIOPorts::In(uint64_t tstates, u8 port)
{
    if (IsSC3000_VDPport(port))
    {
        // Reads from even addresses return the VDP data port contents
        // Reads from odd address return the VDP status flags
        if ((port & 0x01) == 0x00)
            return m_pVideo->GetDataPort(tstates);
        else
            return m_pVideo->GetStatusFlags();
    }

#if GEARSF7000_ENABLE_AY
    if (m_pAudio->IsAyEnabled() && IsSC3000_AYport(port))
    {
        // Only +2 reads back; the rest of the window is open bus, as it would
        // be on a card that decodes the block but drives no data there.
        if ((port & 0x03) == 0x02)
            return m_pAudio->AyReadData();
        return 0xFF;
    }
#endif

    if (IsSC3000_PPIport(port))
    {
        //// Reads from even addresses return the I/O port A/B register
        //// Reads from odd address return the I/O port B/misc. register
        //if ((port & 0x01) == 0x00)
        //    return m_pInput->GetPortDC();
        //else
        //    return ((m_pInput->GetPortDD() & 0x3F) | (m_Port3F & 0xC0));

        return m_sc3000Ppi->ReadRegister(port & 3, tstates);
    }
    
    
#if GEARSF7000_ENABLE_SF7000
    if (IsSF7000_port(port))
    {
        switch (port)
        {
            case 0xE0:
            {
                return m_pSF7000->FDC765_Status_Read();
                //return 0xFF;
    //                temp = sf7.FDC_ReadStatus();

                    //if (shadowStatus != temp)
                    //{
                    //    //const int time = cpu.IMasterCycled;
                    //    const int time = (int)((float)cpu.IMasterCycled / 3.58f);
                    //    printf("SF-7000: Read Status from 0xE0: %16d, %04X %02X\n", time, cpu.PC, temp);
                    //}

                    //shadowStatus = temp;
            }
            //            break;

            case 0xE1:
                return m_pSF7000->FDC765_Data_Read();
                //return 0xFF;

            case 0xE4:
            case 0xE5:
            case 0xE6:
            case 0xE7:
            {
                return m_pSF7000->PPI_ReadRegister(port & 3, tstates);
            }

            case 0xE8: return m_pSF7000->USART_ReadData();  	/* USART Data */
            case 0xE9: return m_pSF7000->USART_ReadCommand(); 	/* USART Command */
        }
    }
    else
    {
        Log("--> ** Attempting to read from port $%X", port);
        return 0xFF;
    }
#else
    // No SF-7000 fitted: the whole $E0-$FF block is open bus, same as any
    // other unclaimed range - nothing decodes it, so it always reads 0xFF.
    if (IsSF7000_port(port))
        return 0xFF;
    else
        Log("--> ** Attempting to read from port $%X", port);
#endif

    return 0xFF;
}



inline void MachineIOPorts::Out(uint64_t tstates, u8 port, u8 value)
{
#ifdef DEBUG_PORT
    if (IsSC3000_DEBUGport(port))
    {
        //SC3000_DEBUGout()
        //putchar(value);

        if (fp_debug)
            putc(value, fp_debug);
    }
#endif

    // Cartridge-side I/O decoding is independent of the motherboard
    // devices below. Survivors boards decode every port in $E0-$FF.
    m_pMemory->WriteMapperPort(port, value);

    if (IsSC3000_PSGport(port))                             // bit 7 = 0, bit 6 = 1
    {
        // Writes to any address go to the SN76489AN PSG
        m_pAudio->WriteAudioRegister(value);
        const Sms_Apu::WriteInfo& write = m_pAudio->GetApu()->GetLastWriteInfo();
        // The target is decoded but the value stays the exact bus byte: tone
        // periods are 10-bit, while the generic event payload is deliberately
        // 8-bit. The full resulting PSG state is available from get_psg_status.
        m_pMemory->CheckAudioEvent(write.reg, write.rawData);
        //outi          16 + 16 = 32
        //otir          21 + 16 = 37
        //out($7F), a   11 + 16 = 27
        //out(c), a     12 + 16 = 28
    }
    
    
    if (IsSC3000_VDPport(port))                             // bit 7 = 1, bit 6 = 0
    {
        // Writes to even addresses go to the VDP data port.
        // Writes to odd addresses go to the VDP control port.
        if ((port & 0x01) == 0x00)
            m_pVideo->WriteData(tstates, value);
        else
            m_pVideo->WriteControl(tstates, value);
    }

#if GEARSF7000_ENABLE_AY
    if (m_pAudio->IsAyEnabled() && IsSC3000_AYport(port))
    {
        // MSX register layout with bit 7 cleared: +0 latch, +1 data write,
        // +2 data read. Unlike the PSG this takes no wait states - the
        // AY-3-8910 has no READY line, so a card too slow to catch a write
        // drops it silently instead of stalling the Z80. See GetWaitStates().
        switch (port & 0x03)
        {
            case 0x00:
                m_pAudio->AySelectRegister(value);
                break;
            case 0x01:
                // Publish before the write so the event carries the register
                // the value is about to land in, not one already overwritten.
                // Only data writes are reported: the latch on its own says
                // nothing about what the software is doing to the chip.
                m_pMemory->CheckAyEvent(m_pAudio->GetAySelectedRegister(), value);
                m_pAudio->AyWriteData(value);
                break;
            default:
                break;      // +2 is read-only, +3 unused
        }
    }
#endif
    
    
    if (IsSC3000_PPIport(port))
    {
        // The CPU bus records the attempt even for input-configured
        // or ignored ports. A derived Port-C latch transition remains a
        // separate device event.
        const Intel8255Snapshot before = m_sc3000Ppi->GetSnapshot();
        m_sc3000Ppi->WriteRegister(port & 3, value, tstates);
        const Intel8255Snapshot after = m_sc3000Ppi->GetSnapshot();
        SC3000_PPI_PortA = after.outputLatch[0];
        SC3000_PPI_PortB = after.outputLatch[1];
        SC3000_PPI_PortCTRL = after.control;
        if ((port & 3) == 3 && before.control != after.control)
            m_pMemory->CheckPPIStateChange(0xDF, before.control, after.control);
    }


#if GEARSF7000_ENABLE_SF7000
    if (IsSF7000_port(port))
    {
        //Log("--> ** Attempting to write to SF-7000 at port $%X", port);
        //=== SF-7000 =========================

        switch (port)
        {
            case 0xE1:
            {
                m_pSF7000->FDC765_Data_Write(value);
            }
            break;

            case 0xE4:
            case 0xE5:
            case 0xE6:
            case 0xE7:
                m_pSF7000->PPI_WriteRegister(port & 3, value, tstates);
                break;

            case 0xE8:	m_pSF7000->USART_WriteData(value); break;
            case 0xE9:	m_pSF7000->USART_WriteCommand(value); break;
        }

    }
    else
    {
        //Log("--> ** Attempting to write to port $%X", port);
    }
#else
    // No SF-7000 fitted: writes into $E0-$FF land nowhere, same as any other
    // unclaimed range - silently dropped.
#endif
}
#endif	/* MACHINEIOPORTS_H */
