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


#include "Audio.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include "SaveStateStream.h"

namespace
{
// v3 adds a length-prefixed YM2413 section. v2 remains readable: each Audio
// payload is already bounded by the top-level save-state section table.
constexpr std::uint16_t kAudioStateVersion = 3;
}

namespace
{
constexpr float MIX_FULL_SCALE = 32767.0f;
constexpr float MIX_SOFT_LIMIT_START = MIX_FULL_SCALE * 0.95f;
// Approximate the attenuation of the SC-3000 CN1 A/V output network. Keep
// this on the PSG bus so cassette and mechanical drive audio are unaffected.
constexpr float PSG_BUS_GAIN = 0.75f;
// Recorded mechanical effects are presentation audio, not an emulated audio
// bus. Their source WAV peaks at -0.7 dBFS, so it needs mixing headroom.
constexpr float DRIVE_BUS_GAIN = 0.40f;
// Sms_Apu::volume() scales by 0.85 / (osc_count * 64 * 2). Removing the unused
// tape oscillator took osc_count from 5 to 4, which on its own would have made
// the PSG 5/4 louder (+1.9 dB) and unbalanced it against the cassette and drive
// buses, whose levels were tuned by ear against the old loudness. Compensate
// exactly: 0.6 * 4/5 = 0.48 reproduces the previous output sample for sample.
// Change this only to deliberately re-voice the PSG, never as a side effect.
constexpr double APU_VOLUME = 0.48;
#if GEARSF7000_ENABLE_YM2413
// Unity: FM and PSG at the same perceived loudness. Measured with the BS.1770
// meter on a jukebox ROM that plays the same pieces on both chips: at the
// earlier 2.0 (+6 dB) the FM arrangements read a median 6 LU louder than their
// PSG counterparts, which is exactly that boost. The limiter never engaged at
// either gain, so this moves every FM loudness reading by -6.0 LU and nothing
// else. Keep this at the bus boundary; the chip core remains bit-for-bit
// upstream emu2413 and VGM output remains unscaled register data.
constexpr float YM2413_BUS_GAIN = 1.0f;
#endif
#if GEARSF7000_ENABLE_AY
// An SGM-style expansion halves the 3.58 MHz CPU clock to feed the AY, which
// is also how MSX wires it. Ay_Apu's inherited renderer measures time at
// twice the physical AY clock: its period factor is a half-wave, while the
// documented AY divider describes a complete wave. The shared CPU/Blip clock
// is already exactly that doubled 1.79 MHz clock, so it needs no further
// scaling. Applying the hardware divider inside Ay_Apu as well made every
// tone, noise period and envelope one octave/timescale too low.
constexpr int AY_HARDWARE_CLOCK_DIVIDER = 2;
constexpr int AY_TIMEBASE_DIVIDER = 1;
// Calibrated on the complete rendered output so the same three-note chord at
// maximum fixed volume has equal RMS on the AY and on the SN76489's three tone
// channels. The cores normalise their private amplitude tables differently,
// so equal numeric volume arguments are not equal output levels: 1.0 made the
// three-channel AY mix 2.30 times (+7.2 dB) louder. On physical hardware the
// final ratio is set by the expansion board's mixing resistors; this is the
// neutral emulator default until a particular board is modelled.
constexpr double AY_VOLUME = 0.435;
#endif

float SoftLimit(float sample)
{
    const float magnitude = std::fabs(sample);
    if (magnitude <= MIX_SOFT_LIMIT_START)
        return sample;

    // Unity gain below the knee; above it, asymptotically approach full scale
    // rather than producing the discontinuity of an integer hard clip.
    const float knee = MIX_FULL_SCALE - MIX_SOFT_LIMIT_START;
    const float limited = MIX_SOFT_LIMIT_START
        + knee * (1.0f - std::exp(-(magnitude - MIX_SOFT_LIMIT_START) / knee));
    return std::copysign(limited, sample);
}

template<typename Sample>
void AppendAudioCarry(Sample* carry, int& carryCount,
                      const Sample* produced, int producedCount,
                      const char* busName)
{
    const int available = GC_AUDIO_BUFFER_SIZE - carryCount;
    const int copyCount = std::min(producedCount, available);
    if (copyCount > 0)
    {
        std::memcpy(carry + carryCount, produced,
                    static_cast<size_t>(copyCount) * sizeof(Sample));
        carryCount += copyCount;
    }

    if (copyCount != producedCount)
        Log("Audio %s carry overflow: dropped %d samples", busName,
            producedCount - copyCount);
}

template<typename Sample>
void ConsumeAudioCarry(Sample* carry, int& carryCount, int consumedCount)
{
    const int consumed = std::min(carryCount, consumedCount);
    carryCount -= consumed;
    if (carryCount > 0)
    {
        std::memmove(carry, carry + consumed,
                     static_cast<size_t>(carryCount) * sizeof(Sample));
    }
}
}



Audio::Audio()
{
    m_ElapsedCycles = 0;
    m_iSampleRate = GC_AUDIO_SAMPLE_RATE;
    m_iPsgClockRate = GC_MASTER_CLOCK_NTSC;
    for (int i = 0; i < 4; i++)
        m_PsgDebugSampleCount[i] = 0;
#if GEARSF7000_ENABLE_AY
    for (int i = 0; i < 3; i++)
        m_AyDebugSampleCount[i] = 0;
#endif
    InitPointer(m_pApu);
    InitPointer(m_pBuffer);
    InitPointer(m_pSampleBuffer);
#if GEARSF7000_ENABLE_YM2413
    InitPointer(m_pYm2413);
#endif
    m_bPAL = false;
#if GEARSF7000_ENABLE_SR1000
    m_iSR1000SpeakerCarryCount = 0;
#endif
#if GEARSF7000_ENABLE_SF7000
    m_iWavPlayerCarryCount = 0;
#endif

#if GEARSF7000_ENABLE_AY
    InitPointer(m_pAyApu);
    m_bAyEnabled = false;
    m_bAyEnabledPending = false;
    m_AyChip = AyChip::AY_3_8910;
    m_AyChipPending = AyChip::AY_3_8910;
    m_AySelectedRegister = 0;
#endif

#if GEARSF7000_ENABLE_SR1000
    InitPointer(m_pSR1000Speaker);
#endif

#if GEARSF7000_ENABLE_SF7000
    //InitPointer(m_pWavPlayer);
    m_pWavPlayer.Init(3579545);
#endif

    m_bMute = false;

#ifdef WRITE_AUDIO_TO_FILE 
    if (fp = fopen("d:\\data\\audio.raw", "rb"))
    {
        fclose(fp);
        remove("d:\\data\\audio.raw");
    }

    fp = fopen("d:\\data\\audio.raw", "wb");
#endif
}

Audio::~Audio()
{
    SafeDelete(m_pApu);
    SafeDelete(m_pBuffer);
    SafeDeleteArray(m_pSampleBuffer);
#if GEARSF7000_ENABLE_YM2413
    SafeDelete(m_pYm2413);
#endif

#if GEARSF7000_ENABLE_AY
    SafeDelete(m_pAyApu);
#endif

#if GEARSF7000_ENABLE_SR1000
    SafeDelete(m_pSR1000Speaker);
#endif

    //SafeDeleteArray(m_pWavPlayer);

#ifdef WRITE_AUDIO_TO_FILE 
    fclose(fp);
#endif
}

void Audio::Init()
{
    m_pSampleBuffer = new blip_sample_t[GC_AUDIO_BUFFER_SIZE];

    m_pApu = new Sms_Apu();
    m_pBuffer = new Stereo_Buffer();

    m_pBuffer->clock_rate(m_bPAL ? GC_MASTER_CLOCK_PAL : GC_MASTER_CLOCK_NTSC);
    m_pBuffer->set_sample_rate(m_iSampleRate);

    //m_pApu->treble_eq(-15.0);
    //m_pBuffer->bass_freq(100);

    m_pApu->output(m_pBuffer->center(), m_pBuffer->left(), m_pBuffer->right());
    m_pApu->volume(APU_VOLUME);

#if GEARSF7000_ENABLE_YM2413
    m_Ym2413Writes.reserve(2048);
    m_pYm2413 = new Ym2413();
    m_pYm2413->Init(GC_MASTER_CLOCK_NTSC, m_iSampleRate);
    std::memset(m_Ym2413Buffer, 0, sizeof(m_Ym2413Buffer));
#endif

#if GEARSF7000_ENABLE_AY
    // Always constructed, even when the expansion is switched off, so that
    // turning it on at runtime never allocates. Attaching to center() puts it
    // in the same Blip time domain as the PSG, which is required (see the
    // member comment in Audio.h) and free: Blip sums the two chips' deltas.
    m_pAyApu = new Ay_Apu();
    // Deliberately not attached here: Reset() connects it only when the card
    // is actually fitted. A silent AY is not a silent output - ay_amp_table[0]
    // is 62 of 255, the DC bias a channel carries at volume zero - so leaving
    // an absent card wired to the buffer injected a 186-unit step that
    // Blip's DC blocker then removed as an audible click at startup.
    m_pAyApu->set_timebase_divider(AY_TIMEBASE_DIVIDER);
    m_pAyApu->volume(AY_VOLUME);
    m_pAyApu->reset();
#endif

#if GEARSF7000_ENABLE_SF7000
    //m_pWavPlayer = new WavPlayer();
    memset(m_pWavPlayerBuffer, 0, sizeof(m_pWavPlayerBuffer));
    memset(m_pWavPlayerCarry, 0, sizeof(m_pWavPlayerCarry));
    m_iWavPlayerCarryCount = 0;
    m_pWavPlayer.Init(m_bPAL ? GC_MASTER_CLOCK_PAL : GC_MASTER_CLOCK_NTSC);
#endif

#if GEARSF7000_ENABLE_SR1000
    memset(m_pSR1000SpeakerBuffer, 0, sizeof(m_pSR1000SpeakerBuffer));
    memset(m_pSR1000SpeakerCarry, 0, sizeof(m_pSR1000SpeakerCarry));
    m_iSR1000SpeakerCarryCount = 0;
    m_pSR1000Speaker = new SR1000Speaker();
    m_pSR1000Speaker->Init(m_bPAL ? GC_MASTER_CLOCK_PAL : GC_MASTER_CLOCK_NTSC);
#endif
    ResetPeakInfo();

}

void Audio::Reset(GC_Region region, u32 clockRate)
{
    m_bPAL = region == Region_PAL;
    const u32 cpuClock =
        clockRate != 0 ? clockRate
                       : static_cast<u32>(m_bPAL ? GC_MASTER_CLOCK_PAL
                                                 : GC_MASTER_CLOCK_NTSC);

    m_pApu->SetVariant(m_PsgVariantPending);
    m_pApu->SetGgStereoEnabled(m_PsgStereoPending);
    m_pApu->reset();
    m_pApu->volume(APU_VOLUME);
    m_pBuffer->clear();
    m_pBuffer->clock_rate(cpuClock);
    m_iPsgClockRate = cpuClock;
    m_oneBit.Reset(m_oneBitEnabledPending, cpuClock, m_iSampleRate);
    m_oneBitCarryCount = 0;

#if GEARSF7000_ENABLE_YM2413
    m_bYm2413Enabled = m_bYm2413EnabledPending &&
                       m_bYm2413MachineAvailable;
    m_Ym2413Interface = m_Ym2413InterfacePending;
    m_pYm2413->Init(cpuClock, m_iSampleRate);
    m_Ym2413Control = 0;
    m_Ym2413FrameStartControl = 0;
    m_Ym2413Writes.clear();
    m_Ym2413CsyncCounter = 0;
    std::memset(m_Ym2413Buffer, 0, sizeof(m_Ym2413Buffer));
#endif

#if GEARSF7000_ENABLE_AY
    // Latch the pending expansion configuration here and nowhere else. This is
    // the point that corresponds to plugging the card in or swapping the chip,
    // so software never sees either change mid-run.
    m_bAyEnabled = m_ayOnboard || m_aySGM || (m_aySC3000 && m_bAyEnabledPending);
    m_AyChip = (m_aySGM || m_ayOnboard) ? AyChip::AY_3_8910 : m_AyChipPending;
    // Connect the chip only when the card is fitted. Ay_Apu skips any
    // oscillator whose output is NULL, so an absent card contributes nothing.
    m_pAyApu->output(m_bAyEnabled ? m_pBuffer->center() : NULL);
    m_pAyApu->chip_type(m_AyChip == AyChip::YM2149
        ? Ay_Apu::Chip_Type::ym2149
        : Ay_Apu::Chip_Type::ay_3_8910);
    m_pAyApu->set_timebase_divider(AY_TIMEBASE_DIVIDER);
    m_pAyApu->volume(AY_VOLUME);
    m_pAyApu->reset();
    m_AySelectedRegister = 0;
#endif

    // The chip reset above remains a faithful hardware reset. Debug channel
    // isolation is a host-side listening/capture policy and is layered back
    // on afterwards for this application run only.
    ApplyDebugChannelFilters();

#if GEARSF7000_ENABLE_SR1000
    m_pSR1000Speaker->Reset(cpuClock);

    memset(m_pSR1000SpeakerBuffer, 0, sizeof(m_pSR1000SpeakerBuffer));
    memset(m_pSR1000SpeakerCarry, 0, sizeof(m_pSR1000SpeakerCarry));
    m_iSR1000SpeakerCarryCount = 0;
#endif

#if GEARSF7000_ENABLE_SF7000
    m_pWavPlayer.Reset(cpuClock);
    memset(m_pWavPlayerBuffer, 0, sizeof(m_pWavPlayerBuffer));
    memset(m_pWavPlayerCarry, 0, sizeof(m_pWavPlayerCarry));
    m_iWavPlayerCarryCount = 0;
#endif

    m_ElapsedCycles = 0;
    ResetPeakInfo();
}

void Audio::ApplyDebugChannelFilters()
{
    for (int channel = 0; channel < 4; ++channel)
    {
        m_pApu->EnableChannel(channel, m_PsgChannelEnabled[channel]);
        m_pApu->MuteChannel(channel, m_PsgChannelMuted[channel]);
    }

#if GEARSF7000_ENABLE_AY
    for (int channel = 0; channel < 3; ++channel)
    {
        m_pAyApu->EnableChannel(channel, m_AyChannelEnabled[channel]);
        m_pAyApu->MuteChannel(channel, m_AyChannelMuted[channel]);
    }
#endif
}

void Audio::SetPsgChannelEnabled(int channel, bool enabled)
{
    if (channel < 0 || channel >= 4)
        return;
    m_PsgChannelEnabled[channel] = enabled;
    m_pApu->EnableChannel(channel, enabled);
}

void Audio::SetPsgChannelMuted(int channel, bool muted)
{
    if (channel < 0 || channel >= 4)
        return;
    m_PsgChannelMuted[channel] = muted;
    m_pApu->MuteChannel(channel, muted);
}

bool Audio::IsPsgChannelEnabled(int channel) const
{
    return channel >= 0 && channel < 4 ? m_PsgChannelEnabled[channel] : false;
}

bool Audio::IsPsgChannelMuted(int channel) const
{
    return channel >= 0 && channel < 4 ? m_PsgChannelMuted[channel] : false;
}

#if GEARSF7000_ENABLE_AY
void Audio::SetAyChannelEnabled(int channel, bool enabled)
{
    if (channel < 0 || channel >= 3)
        return;
    m_AyChannelEnabled[channel] = enabled;
    m_pAyApu->EnableChannel(channel, enabled);
}

void Audio::SetAyChannelMuted(int channel, bool muted)
{
    if (channel < 0 || channel >= 3)
        return;
    m_AyChannelMuted[channel] = muted;
    m_pAyApu->MuteChannel(channel, muted);
}

bool Audio::IsAyChannelEnabled(int channel) const
{
    return channel >= 0 && channel < 3 ? m_AyChannelEnabled[channel] : false;
}

bool Audio::IsAyChannelMuted(int channel) const
{
    return channel >= 0 && channel < 3 ? m_AyChannelMuted[channel] : false;
}
#endif

void Audio::SetClockTrim(double ratio)
{
    // Clamped well inside anything audible. Wider excursions are not this
    // knob's job: a buffer far from target is refilled by the scheduler
    // running catch-up frames, or was primed at startup, and reaching for a
    // large trim here would only shift the pitch to fix a level problem.
    const double clamped = ratio < -0.01 ? -0.01 : (ratio > 0.01 ? 0.01 : ratio);
    const long trimmed =
        static_cast<long>(static_cast<double>(m_iPsgClockRate) * (1.0 + clamped));
    if (trimmed > 0)
        m_pBuffer->clock_rate(trimmed);
}

void Audio::EnablePsgDebug(bool enable)
{
    // Guarded, not called unconditionally every frame the panel is open:
    // init_debug_buffers() clears all four Blip_Buffers, which would throw
    // away a partial frame's samples on every single call if this ran
    // without checking first.
    if (enable && !m_pApu->is_debug_enabled())
        m_pApu->init_debug_buffers(m_iSampleRate, m_iPsgClockRate);
    else if (!enable && m_pApu->is_debug_enabled())
        m_pApu->disable_debug_buffers();
}



void Audio::Mute(bool mute)
{
    m_bMute = mute;
}

#if GEARSF7000_ENABLE_AY
int Audio::GetAyClockHz() const
{
    return m_iPsgClockRate / AY_HARDWARE_CLOCK_DIVIDER;
}

void Audio::EnableAyDebug(bool enable)
{
    // Same guard as EnablePsgDebug: init_debug_buffers() clears all three
    // Blip_Buffers, so calling it unconditionally every frame the panel is
    // open would throw away a partial frame's samples each time.
    if (enable && !m_pAyApu->is_debug_enabled())
        m_pAyApu->init_debug_buffers(m_iSampleRate, m_iPsgClockRate);
    else if (!enable && m_pAyApu->is_debug_enabled())
        m_pAyApu->disable_debug_buffers();
}
#endif

#if GEARSF7000_ENABLE_YM2413
bool Audio::IsPsgOutputEnabled() const
{
    if (!m_bYm2413Enabled)
        return true;
    return m_Ym2413Interface == FmInterface::ExternalFmUnit
        ? (m_Ym2413Control & 0x01) == 0
        // Japanese SMS truth table: 00=PSG, 01=FM, 10=mute,
        // 11=PSG+FM. PSG therefore sounds when the two control bits match.
        : ((m_Ym2413Control ^ (m_Ym2413Control >> 1)) & 0x01) == 0;
}

bool Audio::IsFmOutputEnabled() const
{
    return m_bYm2413Enabled && (m_Ym2413Control & 0x01) != 0;
}

void Audio::SelectRegister(u8 value)
{
    if (m_bYm2413Enabled)
        m_pYm2413->SelectRegister(value);
}

void Audio::WriteData(u8 value)
{
    if (!m_bYm2413Enabled)
        return;
    if (!m_pYm2413->StageData(value))
        return;
    value = m_pYm2413->ReadRegister(m_pYm2413->GetSelectedRegister());
    m_Ym2413Writes.push_back({
        m_ElapsedCycles, m_pYm2413->GetSelectedRegister(), value });
#if GEARSF7000_ENABLE_RECORDER
    if (m_bVgmRecordingEnabled)
        m_VgmRecorder.WriteYM2413(m_pYm2413->GetSelectedRegister(), value);
#endif
}

void Audio::WriteControl(u8 value)
{
    if (!m_bYm2413Enabled)
        return;
    const u8 masked = m_Ym2413Interface == FmInterface::ExternalFmUnit
        ? static_cast<u8>(value & 0x01) : static_cast<u8>(value & 0x03);
    m_Ym2413Writes.push_back({ m_ElapsedCycles, 0xFF, masked });
    m_Ym2413Control = masked;
}

u8 Audio::ReadControl() const
{
    if (!m_bYm2413Enabled)
        return 0xFF;
    const u16 counter = static_cast<u16>(
        (m_Ym2413CsyncCounter + m_ElapsedCycles / 228) & 0x0FFF);
    const u8 counterBits = static_cast<u8>(
        ((counter >> 4) & 0x80) | ((counter >> 1) & 0x40) |
        ((counter << 2) & 0x20));
    return static_cast<u8>(counterBits | m_Ym2413Control);
}

void Audio::SetYm2413ChannelMuted(int channel, bool muted)
{
    if (m_pYm2413)
        m_pYm2413->SetChannelMuted(channel, muted);
}

bool Audio::IsYm2413ChannelMuted(int channel) const
{
    return m_pYm2413 && m_pYm2413->IsChannelMuted(channel);
}

void Audio::EnableLoudnessMeter(bool enable)
{
    if (enable && !m_bLoudnessEnabled)
        ResetLoudnessMeter();
    m_bLoudnessEnabled = enable;
}

void Audio::ResetLoudnessMeter()
{
    m_FmLoudness.Reset(m_iSampleRate);
    m_OutputLoudness.Reset(m_iSampleRate);
    m_LoudnessPreLimiterPeak = 0.0f;
    m_LoudnessLimitedSamples = 0;
}
#endif

void Audio::EndFrame(s16* pSampleBuffer, int* pSampleCount)
{
    m_pApu->end_frame(m_ElapsedCycles);
    if (m_pApu->is_debug_enabled())
    {
        for (int i = 0; i < 4; i++)
        {
            m_PsgDebugSampleCount[i] =
                m_pApu->read_debug_samples(i, m_PsgDebugSamples[i], 4096);
        }
    }
#if GEARSF7000_ENABLE_AY
    // Same timestamp as the PSG: both chips write into the same Blip_Buffer,
    // so their frames must close together or the buffer's notion of "now"
    // disagrees between them. Run it even when the expansion is disabled - it
    // is then silent, but keeping its clock aligned means enabling it at the
    // next reset does not start from a stale time.
    m_pAyApu->end_frame(m_ElapsedCycles);
    if (m_pAyApu->is_debug_enabled())
    {
        for (int i = 0; i < 3; i++)
        {
            m_AyDebugSampleCount[i] =
                m_pAyApu->read_debug_samples(i, m_AyDebugSamples[i], 4096);
        }
    }
#endif
#if GEARSF7000_ENABLE_YM2413
    // Use the same fractional clock-to-sample phase as the PSG buffer.
    // Scaling by this frame's rounded sample count shifts writes depending
    // on frame boundaries, particularly during short frames / stepping.
    for (FmWriteEvent& event : m_Ym2413Writes)
        event.sample = static_cast<int>(m_pBuffer->center()->count_samples(
            static_cast<blip_time_t>(event.cycle)));
#endif
    m_pBuffer->end_frame(m_ElapsedCycles);

    int count = static_cast<int>(m_pBuffer->read_samples(m_pSampleBuffer, GC_AUDIO_BUFFER_SIZE));

#if GEARSF7000_ENABLE_YM2413
    std::memset(m_Ym2413Buffer, 0, sizeof(m_Ym2413Buffer));
    const int fmPairs = count / 2;
    int fmRendered = 0;
    m_pYm2413->BeginDebugFrame();
    if (m_bYm2413Enabled && m_ElapsedCycles != 0)
    {
        for (const FmWriteEvent& event : m_Ym2413Writes)
        {
            if (event.reg == 0xFF)
                continue;
            const int target = std::clamp(event.sample, 0, fmPairs);
            m_pYm2413->Generate(target - fmRendered,
                m_Ym2413Buffer + fmRendered * 2);
            fmRendered = target;
            m_pYm2413->ApplyRegister(event.reg, event.value);
        }
        m_pYm2413->Generate(fmPairs - fmRendered,
            m_Ym2413Buffer + fmRendered * 2);
    }
#endif

    // count is stereo-interleaved (left+right per sample), and VGM timing is
    // per output sample, so halve it - the same convention GearSF7000's own
    // VgmRecorder hookup uses for the same buffer type.
#if GEARSF7000_ENABLE_RECORDER
    if (m_bVgmRecordingEnabled)
        m_VgmRecorder.UpdateTiming(count / 2);
#endif

    // The producers use the same nominal rate, but Blip_Buffer has its own
    // filter latency and each producer rounds fractional samples differently.
    // Queue non-PSG samples instead of dropping their per-frame remainder.
#if GEARSF7000_ENABLE_SF7000
    memset(m_pWavPlayerBuffer, 0, sizeof(m_pWavPlayerBuffer));
    const int driveCount = m_pWavPlayer.EndFrame(m_pWavPlayerBuffer);
    AppendAudioCarry(m_pWavPlayerCarry, m_iWavPlayerCarryCount,
                     m_pWavPlayerBuffer, driveCount, "drive");
#else
    const int driveCount = 0;
#endif

#if GEARSF7000_ENABLE_SR1000
    memset(m_pSR1000SpeakerBuffer, 0, sizeof(m_pSR1000SpeakerBuffer));
    const int cassetteCount = m_pSR1000Speaker->EndFrame(m_pSR1000SpeakerBuffer, count);
    AppendAudioCarry(m_pSR1000SpeakerCarry, m_iSR1000SpeakerCarryCount,
                     m_pSR1000SpeakerBuffer, cassetteCount, "cassette");
#else
    const int cassetteCount = 0;
#endif

    const int oneBitCount = m_oneBit.EndFrame(m_ElapsedCycles, m_oneBitBuffer, GC_AUDIO_BUFFER_SIZE);
    AppendAudioCarry(m_oneBitCarry, m_oneBitCarryCount, m_oneBitBuffer, oneBitCount, "one-bit");
    m_PeakInfo.psgFrameSamples = count;
    m_PeakInfo.driveFrameSamples = driveCount;
    m_PeakInfo.cassetteFrameSamples = cassetteCount;
#if GEARSF7000_ENABLE_SR1000
    m_PeakInfo.cassetteFrequencyHz = m_pSR1000Speaker->GetMeasuredFrequencyHz();
    const SR1000Speaker::SafetyInfo tapeSafety = m_pSR1000Speaker->GetSafetyInfo();
    m_PeakInfo.cassettePendingEvents = tapeSafety.pendingEvents;
    m_PeakInfo.cassetteCoalescedEvents = tapeSafety.coalescedEvents;
    m_PeakInfo.cassetteQueueOverflows = tapeSafety.queueOverflows;
    m_PeakInfo.cassetteTimestampRebases = tapeSafety.timestampRebases;
    m_PeakInfo.cassetteInvalidInputs = tapeSafety.invalidInputs;
#endif


    if (IsValidPointer(pSampleBuffer) && IsValidPointer(pSampleCount) )
    {
        *pSampleCount = count;

        u8 fmControl =
#if GEARSF7000_ENABLE_YM2413
            m_Ym2413FrameStartControl;
        size_t fmControlEvent = 0;
        // Latched once per frame so the per-sample cost when off is one
        // predictable branch. Frames arrive interleaved L,R: hold the left
        // sample until its right partner comes in.
        const bool measureLoudness = m_bLoudnessEnabled;
        float loudnessFmLeft = 0.0f;
        float loudnessOutLeft = 0.0f;
#else
            0;
#endif
        for (int i=0; i<count; i++)
        {
            // Il mute azzera l'uscita e salta il mix, che costa. Ma se si sta
            // registrando il mix serve lo stesso: registrare senza ascoltare e`
            // proprio il caso d'uso, e in headless il mute e` sempre acceso.
            if (m_bMute
#if GEARSF7000_ENABLE_RECORDER
                && !m_bIsRecording
#endif
            )
            {
                pSampleBuffer[i] = 0;
            }
            else
            {
#if GEARSF7000_ENABLE_YM2413
                const int pair = i / 2;
                while (fmControlEvent < m_Ym2413Writes.size())
                {
                    const FmWriteEvent& event = m_Ym2413Writes[fmControlEvent];
                    const int target = std::clamp(event.sample, 0, fmPairs);
                    if (target > pair)
                        break;
                    if (event.reg == 0xFF)
                        fmControl = event.value;
                    ++fmControlEvent;
                }
                const bool psgEnabled = !m_bYm2413Enabled ||
                    (m_Ym2413Interface == FmInterface::ExternalFmUnit
                        ? (fmControl & 0x01) == 0
                        : ((fmControl ^ (fmControl >> 1)) & 0x01) == 0);
                const bool fmEnabled = m_bYm2413Enabled &&
                    (fmControl & 0x01) != 0;
                const float psg = psgEnabled
                    ? static_cast<float>(m_pSampleBuffer[i]) * PSG_BUS_GAIN
                    : 0.0f;
                const float fm = fmEnabled
                    ? static_cast<float>(m_Ym2413Buffer[i]) * YM2413_BUS_GAIN
                    : 0.0f;
#else
                const float psg = static_cast<float>(m_pSampleBuffer[i]) * PSG_BUS_GAIN;
                const float fm = 0.0f;
#endif
#if GEARSF7000_ENABLE_SF7000
                const float drive = (i < m_iWavPlayerCarryCount
                    ? m_pWavPlayerCarry[i] : 0.0f) * DRIVE_BUS_GAIN;
#else
                const float drive = 0.0f;
#endif
#if GEARSF7000_ENABLE_SR1000
                const float cassette = i < m_iSR1000SpeakerCarryCount
                    ? static_cast<float>(m_pSR1000SpeakerCarry[i]) : 0.0f;
#else
                const float cassette = 0.0f;
#endif
                const float oneBit = i < m_oneBitCarryCount ? static_cast<float>(m_oneBitCarry[i]) : 0.0f;
                const float mixed = psg + fm + drive + cassette + oneBit;
                m_PeakInfo.oneBit = std::max(m_PeakInfo.oneBit, std::fabs(oneBit));
                const float limited = SoftLimit(mixed);

                m_PeakInfo.psg = std::max(m_PeakInfo.psg, std::fabs(psg));
#if GEARSF7000_ENABLE_YM2413
                m_PeakInfo.fm = std::max(m_PeakInfo.fm, std::fabs(fm));
                if (measureLoudness)
                {
                    const float fmNorm = fm / 32768.0f;
                    const float outNorm = limited / 32768.0f;
                    m_LoudnessPreLimiterPeak = std::max(
                        m_LoudnessPreLimiterPeak, std::fabs(mixed));
                    if (std::fabs(mixed) > MIX_SOFT_LIMIT_START)
                        ++m_LoudnessLimitedSamples;
                    if (i & 1)
                    {
                        m_FmLoudness.Process(loudnessFmLeft, fmNorm);
                        m_OutputLoudness.Process(loudnessOutLeft, outNorm);
                    }
                    else
                    {
                        loudnessFmLeft = fmNorm;
                        loudnessOutLeft = outNorm;
                    }
                }
#endif
                m_PeakInfo.drive = std::max(m_PeakInfo.drive, std::fabs(drive));
                m_PeakInfo.cassette = std::max(m_PeakInfo.cassette, std::fabs(cassette));
                m_PeakInfo.preLimiter = std::max(m_PeakInfo.preLimiter, std::fabs(mixed));
                m_PeakInfo.output = std::max(m_PeakInfo.output, std::fabs(limited));
                if (std::fabs(mixed) > MIX_SOFT_LIMIT_START)
                    ++m_PeakInfo.softLimitSamples;

                const s16 mixedOut = static_cast<s16>(std::clamp(
                    limited, -32768.0f, MIX_FULL_SCALE));
#if GEARSF7000_ENABLE_RECORDER
                // Il file prende il mix finale, quello che si sentirebbe: dopo
                // i guadagni di bus e dopo il limiter, e anche se e` mutato.
                if (m_bIsRecording)
                    m_MasterRecorder.WriteSample(mixedOut);
#endif
                pSampleBuffer[i] = m_bMute ? 0 : mixedOut;
            }
                //+ m_pSGMBuffer[i]
                //+ m_pWavPlayerBuffer[i];
        }

        ConsumeAudioCarry(m_oneBitCarry, m_oneBitCarryCount, count);
#if GEARSF7000_ENABLE_SF7000
        ConsumeAudioCarry(m_pWavPlayerCarry, m_iWavPlayerCarryCount, count);
        m_PeakInfo.driveQueuedSamples = m_iWavPlayerCarryCount;
#endif
#if GEARSF7000_ENABLE_SR1000
        ConsumeAudioCarry(m_pSR1000SpeakerCarry, m_iSR1000SpeakerCarryCount, count);
        m_PeakInfo.cassetteQueuedSamples = m_iSR1000SpeakerCarryCount;
#endif

#ifdef WRITE_AUDIO_TO_FILE 
        fwrite(pSampleBuffer, sizeof(s16), *pSampleCount, fp);
#endif
    }
    //if (IsValidPointer(pSampleBuffer) && IsValidPointer(pSampleCount)) {
    //    *pSampleCount = count;
    //    for (int i = 0; i < count; i++) {
    //        // Qui avviene il mix dei componenti
    //        pSampleBuffer[i] = m_pSampleBuffer[i] + m_pWavPlayerBuffer[i] + m_pSR1000SpeakerBuffer[i];

    //        // CATTURA MASTER
    //        if (m_bIsRecording) {
    //            m_MasterRecorder.WriteSample(pSampleBuffer[i]);
    //        }
    //    }
    //}

    m_ElapsedCycles = 0;
#if GEARSF7000_ENABLE_YM2413
    m_Ym2413FrameStartControl = m_Ym2413Control;
    m_Ym2413Writes.clear();
    m_Ym2413CsyncCounter = static_cast<u16>(
        (m_Ym2413CsyncCounter + (m_bPAL ? 313 : 262)) & 0x0FFF);
#endif


}

#define mstosamples(ms) ((GC_AUDIO_SAMPLE_RATE*ms)/1000.0f)

#if GEARSF7000_ENABLE_SF7000
void Audio::PlayDriveTrack(uint8_t oldtrack, uint8_t newtrack)
{
    if (oldtrack == newtrack) return;
    // Ottieni il canale audio associato

//    int diff = newtrack > oldtrack ? newtrack - oldtrack : (newtrack < oldtrack ? oldtrack - newtrack : newtrack);

    //if (deltaTracks == 1)
    //{
        WavChannel* channel = GetWavPlayer()->GetChannelByName((char*)"DISC_TRACK");
        if (channel != nullptr)
        {
            channel->StopChannel();
            channel->SetupChannel(WavChannel::PlayMode::ONESHOT, 1.0f);
            channel->PlayChannel();
        }
    //}
    //else
    //{
    //    WavChannel* channel = GetWavPlayer()->GetChannelByName((char*)"DISC_TRACK");
    //    if (channel != nullptr)
    //    {
    //        channel->StopChannel();
    //        //channel->SetupChannel(100, 100+921, deltaTracks, 1.0f);

    //        float offsetMs = 0;// 2.267f;
    //        //float trackDurationMs = 6.907f + 14.0f;
    //        float trackDurationMs = 6.9f + 14.0f;

    //        channel->SetupChannelMs(offsetMs, offsetMs + trackDurationMs, deltaTracks, 1.0f);
    //        channel->PlayChannel();
    //    }
    //}
}
#endif

void Audio::SaveState(std::ostream& stream)
{
    StateWriter w(stream);

    w.U16(m_oneBit.IsEnabled() ? 4 : kAudioStateVersion);

    w.U64(m_ElapsedCycles);
    w.Bool(m_bPAL);

    // SN76489: registers, oscillator phase and the noise shifter.
    m_pApu->SaveState(stream);

    // The AY used to be left out on purpose, because saving its registers
    // would have pinned down a format while the expansion was still being
    // designed. Sections now carry their own version, which is the condition
    // that decision was waiting on, so the chip is saved like any other.
    //
    // It is length-prefixed because the expansion can be compiled out: a
    // build without it has no Ay_Apu to hand the bytes to, and needs to know
    // how many to step over.
#if GEARSF7000_ENABLE_AY
    {
        std::stringstream ay(std::ios::in | std::ios::out | std::ios::binary);
        StateWriter aw(ay);
        aw.Bool(m_bAyEnabled);
        aw.Bool(m_bAyEnabledPending);
        aw.U8(static_cast<std::uint8_t>(m_AyChip));
        aw.U8(static_cast<std::uint8_t>(m_AyChipPending));
        aw.U8(m_AySelectedRegister);
        m_pAyApu->SaveState(ay);

        const std::string bytes = ay.str();
        w.U32(static_cast<std::uint32_t>(bytes.size()));
        w.Bytes(bytes.data(), bytes.size());
    }
#else
    w.U32(0);
#endif

#if GEARSF7000_ENABLE_YM2413
    {
        std::stringstream fm(std::ios::in | std::ios::out | std::ios::binary);
        StateWriter fw(fm);
        fw.Bool(m_bYm2413Enabled);
        fw.Bool(m_bYm2413EnabledPending);
        fw.U8(static_cast<std::uint8_t>(m_Ym2413Interface));
        fw.U8(static_cast<std::uint8_t>(m_Ym2413InterfacePending));
        fw.U8(m_Ym2413Control);
        fw.U8(m_Ym2413FrameStartControl);
        fw.U16(m_Ym2413CsyncCounter);
        fw.U8(m_pYm2413->GetSelectedRegister());
        for (int reg = 0; reg < 0x40; ++reg)
            fw.U8(m_pYm2413->ReadRegister(reg));

        const std::string bytes = fm.str();
        w.U32(static_cast<std::uint32_t>(bytes.size()));
        w.Bytes(bytes.data(), bytes.size());
    }
#else
    w.U32(0);
#endif

#if GEARSF7000_ENABLE_SR1000
    {
        std::stringstream cassette(std::ios::in | std::ios::out | std::ios::binary);
        m_pSR1000Speaker->SaveState(cassette);

        const std::string bytes = cassette.str();
        w.U32(static_cast<std::uint32_t>(bytes.size()));
        w.Bytes(bytes.data(), bytes.size());
    }
#else
    w.U32(0);
#endif

    if (m_oneBit.IsEnabled()) m_oneBit.SaveState(stream);

    // Deliberately absent: m_pSampleBuffer, the SR1000 and WavPlayer carry
    // buffers, the peak meter and the WAV recorders. Those hold samples on
    // their way out to the host, not anything the machine can read back, and
    // together they are 48 KB per snapshot - the single largest thing in a
    // frame, and the one that compresses worst.
}

void Audio::LoadState(std::istream& stream)
{
    StateReader r(stream);

    const std::uint16_t version = r.U16();
    if ((version != 2 && version != kAudioStateVersion && version != 4) ||
        (version == 4 && !m_oneBit.IsEnabled()))
    { stream.setstate(std::ios::failbit); return; }

    m_ElapsedCycles = r.U64();
    m_bPAL = r.Bool();
    if (!r.Ok())
        return;

    m_pApu->LoadState(stream);

    const std::uint32_t ayBytes = r.U32();
    if (!r.Ok())
        return;

    if (ayBytes > 0)
    {
#if GEARSF7000_ENABLE_AY
        std::string bytes;
        bytes.resize(ayBytes);
        r.Bytes(&bytes[0], ayBytes);
        if (!r.Ok())
            return;

        std::stringstream ay(bytes, std::ios::in | std::ios::binary);
        StateReader ar(ay);
        m_bAyEnabled = ar.Bool();
        m_bAyEnabledPending = ar.Bool();
        m_AyChip = static_cast<AyChip>(ar.U8());
        m_AyChipPending = static_cast<AyChip>(ar.U8());
        m_AySelectedRegister = ar.U8();
        if (ar.Ok() && m_pAyApu->LoadState(ay))
        {
            // States written before the AY pitch fix stored the erroneous
            // extra /2 timebase scale. Normalise both new and old states to
            // the wiring selected by this build; the setter also rescales
            // pending oscillator/noise/envelope intervals coherently.
            m_pAyApu->set_timebase_divider(AY_TIMEBASE_DIVIDER);
        }
#else
        // Built without the expansion. The byte count is exactly why it is
        // written: step over the chip and carry on.
        r.Skip(ayBytes);
#endif
    }

    if (version >= 3)
    {
        const std::uint32_t fmBytes = r.U32();
        if (!r.Ok())
            return;
        if (fmBytes > 0)
        {
#if GEARSF7000_ENABLE_YM2413
            std::string bytes(fmBytes, '\0');
            r.Bytes(&bytes[0], fmBytes);
            if (!r.Ok())
                return;

            std::stringstream fm(bytes, std::ios::in | std::ios::binary);
            StateReader fr(fm);
            m_bYm2413Enabled = fr.Bool();
            m_bYm2413EnabledPending = fr.Bool();
            m_Ym2413Interface = static_cast<FmInterface>(fr.U8());
            m_Ym2413InterfacePending = static_cast<FmInterface>(fr.U8());
            m_Ym2413Control = fr.U8();
            m_Ym2413FrameStartControl = fr.U8();
            m_Ym2413CsyncCounter = fr.U16();
            const u8 selected = fr.U8();
            std::array<u8, 0x40> registers{};
            for (u8& value : registers)
                value = fr.U8();
            if (!fr.Ok())
                return;

            // emu2413 has no stable public snapshot ABI. Replaying the full
            // register image restores all software-visible chip state and
            // starts envelope/phase generation from a deterministic point.
            // This can create a sub-frame discontinuity after loading, but
            // avoids serialising internal pointers and rate-converter data.
            m_pYm2413->Reset();
            for (int reg = 0; reg < 0x40; ++reg)
                m_pYm2413->ApplyRegister(static_cast<u8>(reg), registers[reg]);
            m_pYm2413->SelectRegister(selected);
            m_Ym2413Writes.clear();
            std::memset(m_Ym2413Buffer, 0, sizeof(m_Ym2413Buffer));
#else
            r.Skip(fmBytes);
#endif
        }
    }

    const std::uint32_t cassetteBytes = r.U32();
    if (!r.Ok())
        return;

    if (cassetteBytes > 0)
    {
#if GEARSF7000_ENABLE_SR1000
        std::string bytes;
        bytes.resize(cassetteBytes);
        r.Bytes(&bytes[0], cassetteBytes);
        if (!r.Ok())
            return;

        std::stringstream cassette(bytes, std::ios::in | std::ios::binary);
        m_pSR1000Speaker->LoadState(cassette);
#else
        // Built without the recorder. The byte count is exactly why it is
        // written: step over it and carry on.
        r.Skip(cassetteBytes);
#endif
    }

    m_oneBit.Reset(m_oneBit.IsEnabled(), m_iPsgClockRate, m_iSampleRate);
    m_oneBitCarryCount = 0;
    if (version == 4 && !m_oneBit.LoadState(stream))
    { stream.setstate(std::ios::failbit); return; }

    // Loading restores emulated chip registers and phases, not the host-side
    // channel selection the user is currently using to listen or capture.
    ApplyDebugChannelFilters();

    // Output-stage buffers hold samples produced before the load. They are
    // dropped rather than replayed; at worst that costs one frame of audio.
    m_pBuffer->clear();
#if GEARSF7000_ENABLE_SR1000
    memset(m_pSR1000SpeakerBuffer, 0, sizeof(m_pSR1000SpeakerBuffer));
    memset(m_pSR1000SpeakerCarry, 0, sizeof(m_pSR1000SpeakerCarry));
    m_iSR1000SpeakerCarryCount = 0;
#endif
#if GEARSF7000_ENABLE_SF7000
    memset(m_pWavPlayerBuffer, 0, sizeof(m_pWavPlayerBuffer));
    memset(m_pWavPlayerCarry, 0, sizeof(m_pWavPlayerCarry));
    m_iWavPlayerCarryCount = 0;
#endif
}

// In Audio.cpp

#if GEARSF7000_ENABLE_RECORDER
void Audio::StartRecording(const std::string& basePath) {
    m_sRecordingPath = basePath;

    // Il mix finale, stereo come l'uscita. I quattro file per canale restano
    // spenti: il PSG passa per Blip_Buffer gia` sommato, e per confrontare i
    // canali uno per uno c'e` il VGM, che dice anche da quale registro arriva.
    m_bIsRecording = m_MasterRecorder.Start(basePath, m_iSampleRate, 2);
}

void Audio::StopRecording() {
    m_bIsRecording = false;
    m_MasterRecorder.Stop();
}

bool Audio::StartVgmRecording(const char* file_path, int clockRate, bool isPAL)
{
    if (m_bVgmRecordingEnabled)
        return false;

    m_VgmRecorder.Start(file_path, clockRate, isPAL);
    m_bVgmRecordingEnabled = m_VgmRecorder.IsRecording();
    return m_bVgmRecordingEnabled;
}

std::string Audio::StopVgmRecording()
{
    if (!m_bVgmRecordingEnabled)
        return std::string();

    // Cleared first: Stop() below writes the file, and chip write hooks
    // check m_bVgmRecordingEnabled before touching m_VgmRecorder, so nothing
    // can append to the buffer while it is being flushed to disk.
    m_bVgmRecordingEnabled = false;
    return m_VgmRecorder.Stop();
}
#endif
