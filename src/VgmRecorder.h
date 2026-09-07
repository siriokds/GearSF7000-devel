/*
 * GearSF7000 - VGM export
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Adapted from GearSF7000's VgmRecorder (Copyright (C) 2021 Ignacio Sanchez),
 * itself the reason this was ported here instead of SC3K-System's older
 * vgm.cpp: VGM 1.70, a std::vector<u8> command buffer instead of a raw
 * FILE*, sample-accurate timing rather than one record per host frame, and -
 * the deciding point - PSG and AY8910 already handled as two distinct chips
 * in the same file. That is exactly the SC-3000 case: SN76489 always,
 * AY-3-8910/YM2149 on the optional expansion.
 */

#ifndef VGM_RECORDER_H
#define VGM_RECORDER_H

#include "definitions.h"
#include <vector>
#include <string>
#include <fstream>

class VgmRecorder
{
public:
    VgmRecorder();
    ~VgmRecorder();

    void Start(const char* file_path, int clock_rate, bool is_pal);
    // Returns the path actually written, so a caller (the auto-stop on
    // rewind seek, in particular) can report where the file ended up without
    // keeping its own copy of the path.
    const std::string& Stop();
    bool IsRecording() const { return m_bRecording; }

    void WritePSG(u8 data);
    void WriteAY8910(u8 reg, u8 data);
    void UpdateTiming(int elapsed_samples);

private:
    void WriteCommand(u8 command);
    void WriteCommand(u8 command, u8 data);
    void WriteCommand(u8 command, u8 data1, u8 data2);
    void WriteWait(int samples);
    void FlushPendingWait();

private:
    bool m_bRecording;
    std::string m_FilePath;
    std::vector<u8> m_CommandBuffer;
    int m_PendingWait;
    int m_TotalSamples;
    int m_ClockRate;
    bool m_bPAL;
    bool m_bPSGUsed;
    bool m_bAY8910Used;
};

#endif /* VGM_RECORDER_H */
