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

#include "MachineIOPorts.h"
#include "SaveStateStream.h"

MachineIOPorts::MachineIOPorts(Audio* pAudio, Video* pVideo, SK1100* pSK1100, SR1000* pSR1000, SF7000* pSF7000, SP400* pSP400, Cartridge* pCartridge, Memory* pMemory)
{
    m_pAudio = pAudio;
    m_pVideo = pVideo;
    m_pSK1100 = pSK1100;
    m_pSR1000 = pSR1000;
    m_pSF7000 = pSF7000;
    m_pSP400 = pSP400;
    m_pCartridge = pCartridge;
    m_pMemory = pMemory;
    basicDeserializer = new SegaBasicDeserializer(3580000, [this](uint8_t byte) { this->onByteReceived(byte); });

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
    return DecodeGearIoPort(port);
}

void MachineIOPorts::Reset()
{
    if (basicDeserializer)
        basicDeserializer->Reset(3580000);

	SC3000_PPI_PortA = 0x00;
	SC3000_PPI_PortB = 0x00;
	SC3000_PPI_PortC = 0x00;
	SC3000_PPI_PortCTRL = 0x00;
}

void MachineIOPorts::onByteReceived(uint8_t) {

}

void MachineIOPorts::SaveState(std::ostream& stream) const
{
    StateWriter w(stream);
    w.U8(SC3000_PPI_PortA);
    w.U8(SC3000_PPI_PortB);
    w.U8(SC3000_PPI_PortC);
    w.U8(SC3000_PPI_PortCTRL);
    w.U8(SR1000_CassetteSignal);
}

void MachineIOPorts::LoadState(std::istream& stream)
{
    StateReader r(stream);
    const uint8_t portA = r.U8();
    const uint8_t portB = r.U8();
    const uint8_t portC = r.U8();
    const uint8_t control = r.U8();
    const uint8_t cassette = r.U8();
    if (!r.Ok())
        return;

    SC3000_PPI_PortA = portA;
    SC3000_PPI_PortB = portB;
    SC3000_PPI_PortC = portC;
    SC3000_PPI_PortCTRL = control;
    SR1000_CassetteSignal = cassette;
}
