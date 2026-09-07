/*
 * GearSF7000 - VGM export
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Adapted from GearSF7000's VgmRecorder (Copyright (C) 2021 Ignacio Sanchez).
 * See VgmRecorder.h for why this one was ported instead of SC3K-System's.
 */

#include "VgmRecorder.h"
#include "DiagnosticLog.h"
#include <cstring>

VgmRecorder::VgmRecorder()
{
    m_bRecording = false;
    m_PendingWait = 0;
    m_TotalSamples = 0;
    m_ClockRate = 0;
    m_bPAL = false;
    m_bPSGUsed = false;
    m_bAY8910Used = false;
}

VgmRecorder::~VgmRecorder()
{
    if (m_bRecording)
        Stop();
}

void VgmRecorder::Start(const char* file_path, int clock_rate, bool is_pal)
{
    if (m_bRecording)
        return;

    m_FilePath = file_path;
    m_ClockRate = clock_rate;
    m_bPAL = is_pal;
    m_bRecording = true;
    m_PendingWait = 0;
    m_TotalSamples = 0;
    m_bPSGUsed = false;
    m_bAY8910Used = false;
    m_CommandBuffer.clear();
}

const std::string& VgmRecorder::Stop()
{
    static const std::string empty;
    if (!m_bRecording)
        return empty;

    FlushPendingWait();

    // End of sound data command.
    WriteCommand(0x66);

    std::ofstream file(m_FilePath.c_str(), std::ios::binary);
    if (file.is_open())
    {
        u8 header[256];
        memset(header, 0, 256);

        header[0x00] = 0x56; header[0x01] = 0x67;
        header[0x02] = 0x6d; header[0x03] = 0x20;   // "Vgm "

        const u32 eof_offset = (256 + static_cast<u32>(m_CommandBuffer.size())) - 4;
        header[0x04] = (eof_offset >> 0) & 0xFF;
        header[0x05] = (eof_offset >> 8) & 0xFF;
        header[0x06] = (eof_offset >> 16) & 0xFF;
        header[0x07] = (eof_offset >> 24) & 0xFF;

        header[0x08] = 0x70; header[0x09] = 0x01;   // version 1.70
        header[0x0A] = 0x00; header[0x0B] = 0x00;

        if (m_bPSGUsed)
        {
            const u32 psg_clock = static_cast<u32>(m_ClockRate);
            header[0x0C] = (psg_clock >> 0) & 0xFF;
            header[0x0D] = (psg_clock >> 8) & 0xFF;
            header[0x0E] = (psg_clock >> 16) & 0xFF;
            header[0x0F] = (psg_clock >> 24) & 0xFF;
        }

        // GD3 offset left at 0: no track/author tag written.

        header[0x18] = (m_TotalSamples >> 0) & 0xFF;
        header[0x19] = (m_TotalSamples >> 8) & 0xFF;
        header[0x1A] = (m_TotalSamples >> 16) & 0xFF;
        header[0x1B] = (m_TotalSamples >> 24) & 0xFF;

        // Loop offset/samples left at 0: no loop point.

        const u32 rate = m_bPAL ? 50 : 60;
        header[0x24] = (rate >> 0) & 0xFF;
        header[0x25] = (rate >> 8) & 0xFF;
        header[0x26] = (rate >> 16) & 0xFF;
        header[0x27] = (rate >> 24) & 0xFF;

        if (m_bPSGUsed)
        {
            header[0x28] = 0x09; header[0x29] = 0x00; // feedback (SMS/CV/SC-3000)
            header[0x2A] = 0x10;                      // shift register width 16
            header[0x2B] = 0x00;                      // no GG stereo
        }

        // VGM data offset, relative from 0x34. Data starts at 0x100.
        header[0x34] = 0xCC; header[0x35] = 0x00;
        header[0x36] = 0x00; header[0x37] = 0x00;

        if (m_bAY8910Used)
        {
            const u32 ay_clock = static_cast<u32>(m_ClockRate);
            header[0x74] = (ay_clock >> 0) & 0xFF;
            header[0x75] = (ay_clock >> 8) & 0xFF;
            header[0x76] = (ay_clock >> 16) & 0xFF;
            header[0x77] = (ay_clock >> 24) & 0xFF;
            header[0x78] = 0x00;   // AY-3-8910
            header[0x79] = 0x01;   // legacy output
        }

        file.write(reinterpret_cast<const char*>(header), 256);
        if (!m_CommandBuffer.empty())
            file.write(reinterpret_cast<const char*>(&m_CommandBuffer[0]),
                       static_cast<std::streamsize>(m_CommandBuffer.size()));
        file.close();
    }
    else
    {
        DiagError("audio", "VGM: could not open %s for writing",
                  m_FilePath.c_str());
    }

    m_bRecording = false;
    m_CommandBuffer.clear();
    return m_FilePath;
}

void VgmRecorder::WritePSG(u8 data)
{
    if (!m_bRecording)
        return;
    FlushPendingWait();
    m_bPSGUsed = true;
    WriteCommand(0x50, data);   // PSG (SN76489) write
}

void VgmRecorder::WriteAY8910(u8 reg, u8 data)
{
    if (!m_bRecording)
        return;
    FlushPendingWait();
    m_bAY8910Used = true;
    WriteCommand(0xA0, reg, data);   // AY8910 register write
}

void VgmRecorder::UpdateTiming(int elapsed_samples)
{
    if (!m_bRecording)
        return;
    m_PendingWait += elapsed_samples;
    m_TotalSamples += elapsed_samples;
}

void VgmRecorder::WriteCommand(u8 command)
{
    m_CommandBuffer.push_back(command);
}

void VgmRecorder::WriteCommand(u8 command, u8 data)
{
    m_CommandBuffer.push_back(command);
    m_CommandBuffer.push_back(data);
}

void VgmRecorder::WriteCommand(u8 command, u8 data1, u8 data2)
{
    m_CommandBuffer.push_back(command);
    m_CommandBuffer.push_back(data1);
    m_CommandBuffer.push_back(data2);
}

void VgmRecorder::WriteWait(int samples)
{
    if (samples <= 0)
        return;

    while (samples > 0)
    {
        if (samples == 735)
        {
            WriteCommand(0x62);   // 1/60 s
            samples -= 735;
        }
        else if (samples == 882)
        {
            WriteCommand(0x63);   // 1/50 s
            samples -= 882;
        }
        else if (samples <= 16)
        {
            WriteCommand(static_cast<u8>(0x70 + (samples - 1)));
            samples = 0;
        }
        else if (samples <= 65535)
        {
            WriteCommand(0x61);
            m_CommandBuffer.push_back(static_cast<u8>(samples & 0xFF));
            m_CommandBuffer.push_back(static_cast<u8>((samples >> 8) & 0xFF));
            samples = 0;
        }
        else
        {
            WriteCommand(0x61);
            m_CommandBuffer.push_back(0xFF);
            m_CommandBuffer.push_back(0xFF);
            samples -= 65535;
        }
    }
}

void VgmRecorder::FlushPendingWait()
{
    if (m_PendingWait > 0)
    {
        WriteWait(m_PendingWait);
        m_PendingWait = 0;
    }
}
