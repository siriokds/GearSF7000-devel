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

#include <cstdint>
#include <vector>
#include <functional>
#include <cmath>
#include <iostream>
#include <istream>
#include <ostream>
#include "SegaBasicDeserializer.h"
#include <stdio.h>

class Audio;
class Video;
class SK1100;
class SF7000;
class SP400;
class SR1000;
class Cartridge;
class Memory;
class MachineIOPorts : public IOPorts
{
public:
    MachineIOPorts(Audio* pAudio, Video* pVideo, SK1100* pSK1100, SR1000* pSR1000, SF7000* pSF7000, SP400* pSP400, Cartridge* pCartridge, Memory* pMemory);
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
    
    inline uint8_t SC3000_PPI_GetPortA();
    
    void SC3000_PPI_SetPortB(uint8_t val);
    inline uint8_t SC3000_PPI_GetPortB();

    void SC3000_PPI_SetPortC(uint64_t tstates, uint8_t val);
    uint8_t SC3000_PPI_SamplePortC();
    uint8_t SC3000_PPI_GetPortC(uint64_t tstates);

    // The 8255's four registers plus the latched cassette input level. This
    // is the whole of the port decoder's own state: everything else it owns
    // is a pointer to another device, which saves itself.
    void SaveState(std::ostream& stream) const;
    void LoadState(std::istream& stream);

    void SC3000_PPI_GetPorts(uint8_t* ports)
    {
        if (!ports) return;
		
        ports[0] = SC3000_PPI_PortA;
        ports[1] = SC3000_PPI_PortB;
        ports[2] = SC3000_PPI_PortC;
		ports[3] = SC3000_PPI_PortCTRL;
    }

private:
    Audio* m_pAudio;
    Video* m_pVideo;
    SK1100* m_pSK1100;
    SR1000* m_pSR1000;
    SF7000* m_pSF7000;
    SP400* m_pSP400;
    Cartridge* m_pCartridge;
    Memory* m_pMemory;
    uint8_t SC3000_PPI_PortA;
    uint8_t SC3000_PPI_PortB;
    uint8_t SC3000_PPI_PortC;
    uint8_t SC3000_PPI_PortCTRL;

    uint8_t SR1000_CassetteSignal;

    SegaBasicDeserializer *basicDeserializer;
    void onByteReceived(uint8_t byte);

#ifdef DEBUG_PORT
    FILE* fp_debug;
#endif
};

#include "Video.h"
#include "Audio.h"
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



inline uint8_t MachineIOPorts::SC3000_PPI_GetPortA()
{
    const uint8_t value = m_pSK1100->GetPortA();
    SC3000_PPI_PortA = value;
    return value;
}


inline uint8_t MachineIOPorts::SC3000_PPI_GetPortB()
{
#if GEARSF7000_ENABLE_SR1000
    uint8_t sr1000Signal = m_pSR1000->GetSignal();

#ifdef _DEBUG
    if (SR1000_CassetteSignal != sr1000Signal)
    {
        printf("\n[SC-3000 PPI B] Read Cassette Signal %d\n", sr1000Signal);
    }

#endif

    SR1000_CassetteSignal = sr1000Signal;
#endif

    uint8_t value =
#if GEARSF7000_ENABLE_SR1000
          (SR1000_CassetteSignal != 0 ? 0x80 : 0x00)
#else
          0x00 // no cassette fitted: line reads idle
#endif
#if GEARSF7000_ENABLE_SP400
        | (m_pSP400->ReadPortB()  )
#endif
        | (m_pSK1100->GetPortB() & 0x1F) ;



#ifdef _DEBUG
    //printf("\n[SC-3000 PPI B] Read SP400 FAULT Signal %d\n", (value >> 5) & 1);
    //printf("\n[SC-3000 PPI B] Read SP400 BUSY  Signal %d\n", (value >> 6) & 1);
#endif


    SC3000_PPI_PortB = value;
    return value;
}

inline void MachineIOPorts::SC3000_PPI_SetPortB(uint8_t value)
{
//    SC3000_PPI_PortB = value;
//
//#ifdef _DEBUG
//    if (SR1000_CassetteSignal != sr1000Signal)
//    {
//        printf("\n[SC-3000 PPI B] Write Cassette Signal %d\n", value & 0x80 ? 1 : 0);
//    }
//#endif
//
//    //m_pSR1000->SetMotor((value & 0x08) != 0);       // PC3 Cassette Motor (not connected on the real machine but used on basic)
//    //m_pSK1100->SetPortC(value);

}


inline void MachineIOPorts::SC3000_PPI_SetPortC(uint64_t tstates, uint8_t value)
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
	m_pSK1100->SetPortC(value);
#if GEARSF7000_ENABLE_SP400
	m_pSP400->WritePortC(value & 0xE0); // PC5, PC6, PC7
#endif

    // This also covers the BSR path: publish the resulting PC latch only
    // when it really changed, rather than the raw OUT (available as I/O).
    if (oldValue != value)
        m_pMemory->CheckPPIStateChange(0xDE, oldValue, value);

#ifdef _DEBUG
    //printf("*** [SC-3000 PPI]: Write to Port C\n");

    //printf("\t  SP-400 /RESET   => %d\n", (value >> 6) & 1);
    //printf("\t  SP-400 /FEED    => %d\n", (value >> 7) & 1);
#endif
}

inline uint8_t MachineIOPorts::SC3000_PPI_SamplePortC()
{
    //SC3000_PPI_PortC
    return SC3000_PPI_PortC = (

        // (m_pSK1100->GetPortC() & 0xF7)

		      (m_pSK1100->GetPortC() & 0x07)             //                   -   PC2, PC1, PC0
#if GEARSF7000_ENABLE_SR1000
            | (m_pSR1000->IsMotorOn() ? 0x10 : 0x00)    //               PC4
#endif
#if GEARSF7000_ENABLE_SP400
		    | (m_pSP400->GetPortC() & 0xE0)             // PC7, PC6, PC5
#endif

        ); // PC3 Cassette Motor (not connected on the real machine but used on basic)
}

inline uint8_t MachineIOPorts::SC3000_PPI_GetPortC(uint64_t tstates)
{
    (void)tstates;
    const uint8_t value = SC3000_PPI_SamplePortC();
    return value;
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
    if (IsSC3000_AYport(port))
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

        switch (port)
        {
            case 0xDC:								/* D2 U2 B1 A1 R1 L1 D1 U1 */
            {
                return SC3000_PPI_GetPortA();
            }
            case 0xDD:								/* D2 U2 B1 A1 R1 L1 D1 U1 */
            {

                return SC3000_PPI_GetPortB();
            }
            case 0xDE:
            {
                return SC3000_PPI_GetPortC(tstates);
            }
            case 0xDF:
            {
                // The 8255 control register is write-only.  Preserve the
                // existing open-bus result while making the attempted read
                // visible to the centralized CPU bus publisher.
                const uint8_t value = 0xFF;
                return value;
            }

        }
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
                uint8_t value = 0xFF;
                if (port == 0xE4) value = m_pSF7000->PPI_ReadPortA();
                else if (port == 0xE5) value = m_pSF7000->PPI_ReadPortB();
                else if (port == 0xE6) value = m_pSF7000->PPI_ReadPortC();
                else value = m_pSF7000->PPI_ReadControl();
                return value;
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
    if (IsSC3000_AYport(port))
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
        switch (port & 3)
        {
            case 0x00:  // 0xDC
                break;

            case 0x01:  // 0xDD
                //SC3000_PPI_SetPortB(value);
                break;

            case 0x02:  // 0xDE
                SC3000_PPI_SetPortC(tstates, value);
                break;

            case 0x03:  // 0xDF
                {
                    // Control-mode and BSR writes are meaningful independently
                    // from the PC transition they may subsequently cause.
                    bool bsrMode = ((value & 0x80) == 0);


                    if (bsrMode)
                    {
                        uint8_t bitVal = value & 1;
                        uint8_t bitIndex = (value >> 1) & 7;
                        uint8_t mask = 1 << bitIndex;


                        const uint8_t portC = SC3000_PPI_SamplePortC();
                        if (bitVal)		SC3000_PPI_SetPortC(tstates, portC | mask);
                        else			SC3000_PPI_SetPortC(tstates, portC & ~mask);
                    }
                    else
                    {
						const uint8_t oldControl = SC3000_PPI_PortCTRL;
						SC3000_PPI_PortCTRL = value;
                        if (oldControl != SC3000_PPI_PortCTRL)
                            m_pMemory->CheckPPIStateChange(0xDF, oldControl, SC3000_PPI_PortCTRL);
                        SC3000_PPI_PortA = 0;
                        SC3000_PPI_PortB = 0;
                        //SC3000_PPI_SetPortB(0x00);
                        // Let SetPortC observe the previous latch before the
                        // mode-set command clears it; pre-clearing here hid
                        // the transition from both hardware and debugger.
                        SC3000_PPI_SetPortC(tstates, 0x00);
#ifdef _DEBUG
                        printf("[SC3000 PPI] Init $%02X\n", value);

#endif
                    }

                }
                break;
        }
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

            case 0xE6:
                m_pSF7000->PPI_WritePortC(value);
                //CPU_ForceNMI = sf7.PPI_WritePortC(val);

                m_pMemory->SetIPLRomDisabled(!m_pSF7000->IPL_Enabled());

                break;

            case 0xE7:
                m_pSF7000->PPI_WriteControl(value); //SC3000_MapperIPL();
                m_pMemory->SetIPLRomDisabled(!m_pSF7000->IPL_Enabled());
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
