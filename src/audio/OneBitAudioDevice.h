// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#ifndef ONE_BIT_AUDIO_DEVICE_H
#define ONE_BIT_AUDIO_DEVICE_H
#include "Blip_Buffer.h"
#include "../IDebugEventProvider.h"
#include <functional>
#include <iosfwd>
#include <utility>

// A two-level output pin, with no oscillator. Times are CPU/source-clock
// cycles relative to the current audio frame. The owner supplies the timebase.
// Owns its filter/buffer: disconnecting it cannot disturb another sound source.
class OneBitAudioDevice final : public IDebugEventProvider
{
public:
    using LevelChange = std::function<void(bool before, bool after)>;
    void Reset(bool enabled, u32 sourceClockHz, u32 sampleRate);
    bool IsEnabled() const { return m_enabled; }
    bool GetLevel() const { return m_level; }
    bool WriteLevel(u64 cycles, bool high);
    // Stereo interleaved samples, same signal on both sides; returns sample count.
    int EndFrame(u64 cycles, s16* samples, int capacity);
    // Restore an output pin after external board state is loaded. Discards
    // host filter history and publishes no synthetic transition event.
    void RestoreLevel(bool high);
    void SaveState(std::ostream& stream) const;
    bool LoadState(std::istream& stream);
    void SetDebugParentKey(const std::string& parent) { m_parent = parent; }
    void SetLevelChangeCallback(LevelChange callback) { m_change = std::move(callback); }
    DebugDeviceDefinition DescribeDebugDevice() const override;
private:
    bool ValidTime(u64 cycles) const;
    void Emit(u32 cycles, int delta);
    Blip_Buffer m_buffer;
    Blip_Synth<blip_good_quality, 1> m_synth;
    bool m_enabled = false;
    bool m_level = false;
    u32 m_sourceClock = 0;
    u32 m_lastWrite = 0;
    std::string m_parent = "event";
    LevelChange m_change;
};
#endif
