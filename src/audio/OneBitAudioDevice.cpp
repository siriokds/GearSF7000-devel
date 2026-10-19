// SPDX-License-Identifier: GPL-3.0-or-later
#include "OneBitAudioDevice.h"
#include "../SaveStateStream.h"
#include <stdexcept>

void OneBitAudioDevice::Reset(bool enabled, u32 sourceClockHz, u32 sampleRate)
{
    if (!sourceClockHz || sourceClockHz > 100000000 || sampleRate < 8000 || sampleRate > 192000)
        throw std::invalid_argument("Invalid one-bit audio timebase");
    if (enabled && m_buffer.sample_rate() != sampleRate)
        if (const char* error = m_buffer.set_sample_rate(sampleRate))
            throw std::runtime_error(error);
    m_buffer.clear();
    if (enabled) m_buffer.clock_rate(sourceClockHz);
    m_synth.volume(0.10); // conservative nominal gain; board analogue scaling can follow
    m_sourceClock = sourceClockHz;
    m_enabled = enabled;
    m_level = false;
    m_lastWrite = 0;
}

bool OneBitAudioDevice::ValidTime(u64 cycles) const
{
    // Bound the write before handing a timestamp to Blip's unchecked offset.
    // 100 ms is well beyond an emulated frame, inside its 250 ms allocation.
    return cycles >= m_lastWrite && cycles <= m_sourceClock / 10;
}
void OneBitAudioDevice::Emit(u32 cycles, int delta)
{
    m_synth.offset(static_cast<blip_time_t>(cycles), delta, &m_buffer);
    m_buffer.set_modified();
}
bool OneBitAudioDevice::WriteLevel(u64 cycles, bool high)
{
    if (!m_enabled) return true; // disconnected: no latch, event or waveform
    if (!ValidTime(cycles)) return false;
    m_lastWrite = static_cast<u32>(cycles);
    if (high == m_level) return true;
    const bool before = m_level;
    m_level = high;
    Emit(m_lastWrite, high ? 1 : -1);
    if (m_change) m_change(before, high);
    return true;
}
int OneBitAudioDevice::EndFrame(u64 cycles, s16* samples, int capacity)
{
    if (!m_enabled) return 0;
    if (!ValidTime(cycles) || !samples || capacity < 2)
        throw std::invalid_argument("Invalid one-bit audio frame");
    const auto required = m_buffer.samples_avail() + m_buffer.count_samples(static_cast<blip_time_t>(cycles));
    if (required > capacity / 2)
        throw std::invalid_argument("One-bit audio output buffer too small");
    m_buffer.end_frame(static_cast<blip_time_t>(cycles));
    const int count = static_cast<int>(m_buffer.read_samples(samples, capacity / 2));
    for (int i = count - 1; i >= 0; --i)
        samples[2 * i] = samples[2 * i + 1] = samples[i];
    m_lastWrite = 0;
    return count * 2;
}
void OneBitAudioDevice::RestoreLevel(bool high)
{
    m_buffer.clear();
    m_level = m_enabled && high;
    m_lastWrite = 0;
    if (m_level) Emit(0, 1);
}
void OneBitAudioDevice::SaveState(std::ostream& stream) const
{
    StateWriter writer(stream);
    writer.U16(1); writer.Bool(m_level); writer.U32(m_lastWrite);
}
bool OneBitAudioDevice::LoadState(std::istream& stream)
{
    StateReader reader(stream);
    const auto version = reader.U16();
    const auto level = reader.U8();
    const auto time = reader.U32();
    if (!reader.Ok() || version != 1 || level > 1 || time > m_sourceClock / 10 ||
        (!m_enabled && (level || time))) return false;
    // Hardware presence/timebase belong to the owner, not to the snapshot.
    // Like the engine's other devices, pending host audio is discarded.
    m_buffer.clear();
    m_level = level != 0;
    m_lastWrite = time;
    if (m_enabled && m_level) Emit(0, 1);
    return true;
}
DebugDeviceDefinition OneBitAudioDevice::DescribeDebugDevice() const
{
    DebugDeviceDefinition device;
    device.key = m_parent + ".onebit";
    device.parentKey = m_parent;
    device.typeKey = "audio.one_bit_dac";
    device.name = "1-bit audio output";
    DebugSpaceDefinition state;
    state.id = DebugCommonSpace::State;
    state.defaultBitWidth = 1;
    state.key = "state"; state.name = "Output level";
    state.supportedAccesses = DebugEventAccess_Event;
    state.beforeValueAccesses = state.afterValueAccesses = DebugEventAccess_Event;
    state.targets.push_back({0, "Level", 1});
    device.spaces.push_back(std::move(state));
    DebugRulePresetDefinition preset;
    preset.key = "level"; preset.name = "1-bit output transitions";
    preset.spaceKey = "state"; preset.accessMask = DebugEventAccess_Event;
    device.presets.push_back(std::move(preset));
    return device;
}
