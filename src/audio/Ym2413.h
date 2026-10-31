// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#ifndef GEARSF7000_YM2413_H
#define GEARSF7000_YM2413_H

#include "../IDebugEventProvider.h"
#include "../definitions.h"
#include "../IFmPortDevice.h"
#include "emu2413.h"

#include <array>
#include <memory>

// Host-independent YM2413 (OPLL). Port decoding and sample scheduling live
// outside this class so the same chip can later be fitted to another bus.
class Ym2413 final : public IDebugEventProvider
{
public:
    Ym2413();
    ~Ym2413();

    bool Init(u32 clockHz, u32 sampleRate);
    void Reset();
    void SelectRegister(u8 value);
    FmAccessTiming ObserveAccess(u64 chipClock, bool data);
    u64 GetTimingViolationCount() const { return m_timingViolations; }
    void WriteData(u8 value);
    bool StageData(u8 value);
    void ApplyRegister(u8 reg, u8 value);
    void Generate(int sampleCount, s16* stereoOut);
    void SetChannelMuted(int channel, bool muted);
    bool IsChannelMuted(int channel) const;

    // Debug channel IDs match OPLL mask bits: tones 0..8, HH/CYM/TOM/SD/BD 9..13.
    void SetChannelEnabled(int channel, bool enabled);
    bool IsChannelEnabled(int channel) const;
    void EnableDebug(bool enabled);
    void BeginDebugFrame();
    bool IsDebugEnabled() const { return !!m_debug; }
    const s16* GetDebugSamples(int channel) const;
    int GetDebugSampleCount() const;
    static const char* GetChannelName(int channel);

    u8 GetSelectedRegister() const { return m_selectedRegister; }
    u8 ReadRegister(int index) const;
    u32 GetClockHz() const { return m_clockHz; }
    const char* GetInstrumentName(int instrument) const;
    DebugDeviceDefinition DescribeDebugDevice() const override;

private:
    void ApplyMuteMask();
    bool FilterWrite(u8 reg, u8& value) const;
    struct DebugCapture
    {
        std::array<std::array<s16, 4096>, 14> samples{};
        int count = 0;
    };
    std::unique_ptr<DebugCapture> m_debug;
    std::array<bool, 14> m_channelEnabled{};

    bool m_haveAccess = false;
    u64 m_lastAccess = 0;
    u32 m_accessWait = 0;
    u64 m_timingViolations = 0;
    OPLL* m_opll = nullptr;
    u32 m_clockHz = 0;
    u32 m_sampleRate = 0;
    u8 m_selectedRegister = 0;
    std::array<u8, 0x40> m_registers{};
    std::array<bool, 14> m_channelMuted{};
};

#endif
