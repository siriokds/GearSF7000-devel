// SPDX-License-Identifier: GPL-3.0-or-later
#include "../definitions.h"
#if GEARSF7000_ENABLE_YM2413
#include "Ym2413.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <utility>

namespace
{
constexpr int kOutputIndex[14] = {0,1,2,3,4,5,6,7,8,10,13,12,11,9};
u8 CanonicalRegister(u8 reg)
{
    reg &= 0x3f;
    return reg >= 0x10 && (reg & 15) >= 9 ? reg - 9 : reg;
}
bool FormatFmTiming(const DebugEvent& event, DebugEventPresentation& out)
{
    if (event.space != DebugCommonSpace::State || event.targetV2 > 1 ||
        !event.beforeKnown || !event.afterKnown) return false;
    const u64 elapsed = event.beforeValueV2;
    const u64 packed = event.afterValueV2;
    const u64 required = packed & 0xff;
    const u64 port = (packed >> 8) & 0xff;
    const u64 reg = (packed >> 16) & 0xff;
    const u64 value = (packed >> 24) & 0xff;
    std::snprintf(out.typeLabel, sizeof(out.typeLabel), "FM ACCESS TOO FAST");
    std::snprintf(out.details, sizeof(out.details),
        "Port $%02X, register $%02X, value $%02X\n"
        "Elapsed %llu / required %llu YM2413 clocks after %s write\n"
        "Diagnostic only: write not delayed or discarded",
        unsigned(port), unsigned(reg), unsigned(value),
        static_cast<unsigned long long>(elapsed), static_cast<unsigned long long>(required),
        event.targetV2 == 0 ? "address" : "data");
    out.semanticKey = "fm_access_timing_violation";
    out.attributes[out.attributeCount++] = {"timing", "elapsed_clocks", DebugPresentationValueKind::Unsigned, elapsed, nullptr};
    out.attributes[out.attributeCount++] = {"timing", "required_clocks", DebugPresentationValueKind::Unsigned, required, nullptr};
    out.attributes[out.attributeCount++] = {"access", "port", DebugPresentationValueKind::Unsigned, port, nullptr};
    out.attributes[out.attributeCount++] = {"access", "register", DebugPresentationValueKind::Unsigned, reg, nullptr};
    out.attributes[out.attributeCount++] = {"access", "value", DebugPresentationValueKind::Unsigned, value, nullptr};
    return true;
}
const char* const kInstrumentNames[16] = {
    "User", "Violin", "Guitar", "Piano", "Flute", "Clarinet",
    "Oboe", "Trumpet", "Organ", "Horn", "Synthesizer",
    "Harpsichord", "Vibraphone", "Synth Bass", "Acoustic Bass",
    "Electric Guitar"
};
}

Ym2413::Ym2413() { m_channelEnabled.fill(true); }

Ym2413::~Ym2413()
{
    if (m_opll)
        OPLL_delete(m_opll);
}

bool Ym2413::Init(u32 clockHz, u32 sampleRate)
{
    if (m_opll)
        OPLL_delete(m_opll);
    m_clockHz = clockHz;
    m_sampleRate = sampleRate;
    m_opll = OPLL_new(clockHz, sampleRate);
    if (!m_opll)
        return false;
    OPLL_setChipType(m_opll, OPLL_2413_TONE);
    OPLL_resetPatch(m_opll, OPLL_2413_TONE);
    Reset();
    return true;
}

void Ym2413::Reset()
{
    m_haveAccess = false;
    m_timingViolations = 0;
    BeginDebugFrame();
    m_selectedRegister = 0;
    m_registers.fill(0);
    if (m_opll)
    {
        OPLL_reset(m_opll);
        OPLL_resetPatch(m_opll, OPLL_2413_TONE);
        ApplyMuteMask();
    }
}

FmAccessTiming Ym2413::ObserveAccess(u64 chipClock, bool data)
{
    FmAccessTiming result;
    // Reset/load may move CPU time backwards; never report unsigned underflow.
    if (m_haveAccess && chipClock >= m_lastAccess)
    {
        result.elapsed = chipClock - m_lastAccess;
        result.required = m_accessWait;
        result.violation = result.elapsed < result.required;
        if (result.violation) ++m_timingViolations;
    }
    m_lastAccess = chipClock;
    m_accessWait = data ? 84 : 12;
    m_haveAccess = true;
    return result;
}

void Ym2413::SelectRegister(u8 value)
{
    m_selectedRegister = value & 0x3F;
}

void Ym2413::WriteData(u8 value)
{
    if (StageData(value))
        ApplyRegister(m_selectedRegister, ReadRegister(m_selectedRegister));
}

bool Ym2413::StageData(u8 value)
{
    const u8 reg = CanonicalRegister(m_selectedRegister);
    if (!FilterWrite(reg, value))
        return false;
    m_registers[reg] = value;
    return true;
}

bool Ym2413::FilterWrite(u8 reg, u8& value) const
{
    const u8 old = m_registers[reg];
    if (reg == 0x0e)
    {
        for (int ch = 9; ch < 14; ++ch)
        {
            const u8 bit = 1 << (ch - 9);
            if (!m_channelEnabled[ch])
                value = (value & ~bit) | (old & bit);
        }
    }
    else if (reg >= 0x10)
    {
        const int ch = reg & 15;
        if ((m_registers[0x0e] & 0x20) && ch >= 6)
        {
            // Rhythm shares pitch/phase generators. Preserve individual volume
            // nibbles, but accept common pitch while either paired voice is enabled.
            const int low = ch == 6 ? 13 : ch == 7 ? 12 : 10;
            const int high = ch == 6 ? 13 : ch == 7 ? 9 : 11;
            if (reg >= 0x30)
            {
                if (!m_channelEnabled[low]) value = (value & 0xf0) | (old & 15);
                if (!m_channelEnabled[high]) value = (value & 15) | (old & 0xf0);
            }
            else if (!m_channelEnabled[low] && !m_channelEnabled[high])
                return false;
        }
        else if (!m_channelEnabled[ch])
            return false;
    }
    // User patch, rhythm mode and test registers remain global chip controls.
    return true;
}

void Ym2413::ApplyRegister(u8 reg, u8 value)
{
    reg = CanonicalRegister(reg);
    m_registers[reg] = value;
    if (m_opll)
    {
        if (reg == 0x0e && m_opll->rhythm_mode != ((value >> 5) & 1))
        {
            // emu2413 only updates the active group but sums all cached outputs.
            // Drop the group being disconnected at the timestamped mode write;
            // key-off with rhythm still enabled must retain its release tail.
            const int first = (value & 0x20) ? 6 : 9;
            const int end = (value & 0x20) ? 9 : 14;
            std::fill(m_opll->ch_out + first, m_opll->ch_out + end, 0);
        }
        OPLL_writeReg(m_opll, reg, value);
    }
}

void Ym2413::Generate(int sampleCount, s16* stereoOut)
{
    if (!stereoOut || sampleCount <= 0)
        return;
    for (int i = 0; i < sampleCount; ++i)
    {
        const s16 sample = m_opll ? OPLL_calc(m_opll) : 0;
        stereoOut[i * 2] = sample;
        stereoOut[i * 2 + 1] = sample;
        if (m_debug && m_debug->count < 4096)
        {
            for (int ch = 0; ch < 14; ++ch)
            {
                const bool active = m_opll && (ch < 6 ||
                    (ch < 9 ? !m_opll->rhythm_mode : m_opll->rhythm_mode));
                m_debug->samples[ch][m_debug->count] = active
                    ? m_opll->ch_out[kOutputIndex[ch]] : 0;
            }
            ++m_debug->count;
        }
    }
}

void Ym2413::SetChannelMuted(int channel, bool muted)
{
    if (channel < 0 || channel >= static_cast<int>(m_channelMuted.size()))
        return;
    m_channelMuted[channel] = muted;
    ApplyMuteMask();
}

bool Ym2413::IsChannelMuted(int channel) const
{
    return channel >= 0 && channel < static_cast<int>(m_channelMuted.size())
        ? m_channelMuted[channel] : false;
}

void Ym2413::ApplyMuteMask()
{
    if (!m_opll)
        return;
    u32 mask = 0;
    for (int i = 0; i < static_cast<int>(m_channelMuted.size()); ++i)
        if (m_channelMuted[i] || !m_channelEnabled[i])
        {
            mask |= 1u << i;
            // Clear the listening tap immediately, including while paused.
            m_opll->ch_out[kOutputIndex[i]] = 0;
        }
    OPLL_setMask(m_opll, mask);
}

u8 Ym2413::ReadRegister(int index) const
{
    return index >= 0 && index < static_cast<int>(m_registers.size())
        ? m_registers[CanonicalRegister(static_cast<u8>(index))] : 0;
}

void Ym2413::SetChannelEnabled(int channel, bool enabled)
{
    if (channel < 0 || channel >= 14) return;
    m_channelEnabled[channel] = enabled;
    ApplyMuteMask();
}

bool Ym2413::IsChannelEnabled(int channel) const
{
    return channel >= 0 && channel < 14 && m_channelEnabled[channel];
}

void Ym2413::EnableDebug(bool enabled)
{
    if (enabled && !m_debug) m_debug = std::make_unique<DebugCapture>();
    else if (!enabled) m_debug.reset();
}

void Ym2413::BeginDebugFrame()
{
    if (m_debug) m_debug->count = 0;
}

const s16* Ym2413::GetDebugSamples(int channel) const
{
    return m_debug && channel >= 0 && channel < 14
        ? m_debug->samples[channel].data() : nullptr;
}

int Ym2413::GetDebugSampleCount() const
{
    return m_debug ? m_debug->count : 0;
}

const char* Ym2413::GetChannelName(int channel)
{
    static const char* names[14] = {"FM 1", "FM 2", "FM 3", "FM 4", "FM 5",
        "FM 6", "FM 7", "FM 8", "FM 9", "Hi-hat", "Cymbal", "Tom", "Snare", "Bass drum"};
    return channel >= 0 && channel < 14 ? names[channel] : "Unknown";
}

const char* Ym2413::GetInstrumentName(int instrument) const
{
    return kInstrumentNames[instrument & 0x0F];
}

DebugDeviceDefinition Ym2413::DescribeDebugDevice() const
{
    DebugDeviceDefinition device;
    device.key = "event.sms.fm";
    device.parentKey = "event.sms";
    device.typeKey = "yamaha.ym2413";
    device.name = "YM2413 FM Sound Unit / Interface";
    device.traits = DebugDeviceTrait_External;
    device.formatter = &FormatFmTiming;

    DebugSpaceDefinition ports;
    ports.id = DebugCommonSpace::IoPorts;
    ports.key = "io_ports";
    ports.name = "I/O Ports";
    ports.targetMin = 0xF0;
    ports.targetMax = 0xF2;
    ports.supportedAccesses = DebugEventAccess_Read | DebugEventAccess_Write;
    ports.afterValueAccesses = ports.supportedAccesses;

    DebugSpaceDefinition registers;
    registers.id = DebugCommonSpace::Registers;
    registers.key = "registers";
    registers.name = "Registers";
    registers.targetMax = 0x3F;
    registers.supportedAccesses = DebugEventAccess_Event;
    registers.afterValueAccesses = DebugEventAccess_Event;
    registers.defaultBitWidth = 8;
    registers.capabilities = DebugDescriptorCapability_EventsPublished |
                             DebugDescriptorCapability_SnapshotReadable;

    DebugSpaceDefinition timing;
    timing.id = DebugCommonSpace::State;
    timing.key = "state";
    timing.name = "Access Timing";
    timing.targetMax = 1;
    timing.supportedAccesses = timing.beforeValueAccesses = timing.afterValueAccesses = DebugEventAccess_Event;
    timing.defaultBitWidth = 64;
    timing.targets = {{0, "Too fast after address write", 64}, {1, "Too fast after data write", 64}};
    device.spaces.push_back(std::move(timing));
    DebugRulePresetDefinition preset;
    preset.key = "fm_access_timing";
    preset.name = "FM writes too fast";
    preset.spaceKey = "state";
    preset.targetEnd = 1;
    preset.accessMask = DebugEventAccess_Event;
    device.presets.push_back(std::move(preset));

    device.spaces.push_back(std::move(ports));
    device.spaces.push_back(std::move(registers));
    return device;
}

#endif /* GEARSF7000_ENABLE_YM2413 */
