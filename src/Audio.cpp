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
// v2: the SR-1000 cassette speaker block gained a byte-count prefix, same as
// the AY block already had, so a build without GEARSF7000_ENABLE_SR1000 can
// step over it instead of desyncing the rest of the stream.
constexpr std::uint16_t kAudioStateVersion = 2;
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
#if GEARSF7000_ENABLE_AY
// An SGM-style expansion halves the 3.58 MHz CPU clock to feed the AY, which
// is also how MSX wires it. See Ay_Apu::set_clock_divider().
constexpr int AY_CLOCK_DIVIDER = 2;
// Calibrated so one AY channel at full volume matches one PSG channel at full
// volume, measured on rendered output rather than derived: the two chips
// normalise their own scales differently (Sms_Apu divides by osc_count*128*2,
// Ay_Apu by osc_count*amp_range), so the numbers are not comparable on paper.
// Equal loudness is only a sensible default - on real hardware the ratio is
// set by the mixing resistors on the expansion board, so this is the knob to
// turn once that board exists.
constexpr double AY_VOLUME = 1.0;
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
    m_pAyApu->set_clock_divider(AY_CLOCK_DIVIDER);
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

void Audio::Reset(bool bPAL, u32 clockRate)
{
    m_bPAL = bPAL;
    const u32 cpuClock =
        clockRate != 0 ? clockRate
                       : static_cast<u32>(m_bPAL ? GC_MASTER_CLOCK_PAL
                                                 : GC_MASTER_CLOCK_NTSC);

    m_pApu->reset();
    m_pApu->volume(APU_VOLUME);
    m_pBuffer->clear();
    m_pBuffer->clock_rate(cpuClock);
    m_iPsgClockRate = cpuClock;

#if GEARSF7000_ENABLE_AY
    // Latch the pending expansion configuration here and nowhere else. This is
    // the point that corresponds to plugging the card in or swapping the chip,
    // so software never sees either change mid-run.
    m_bAyEnabled = m_bAyEnabledPending;
    m_AyChip = m_AyChipPending;
    // Connect the chip only when the card is fitted. Ay_Apu skips any
    // oscillator whose output is NULL, so an absent card contributes nothing
    // at all - not even the resting DC bias its amplitude table carries at
    // volume zero, which is otherwise heard as a click at startup.
    m_pAyApu->output(m_bAyEnabled ? m_pBuffer->center() : NULL);
    m_pAyApu->chip_type(m_AyChip == AyChip::YM2149
        ? Ay_Apu::Chip_Type::ym2149
        : Ay_Apu::Chip_Type::ay_3_8910);
    m_pAyApu->set_clock_divider(AY_CLOCK_DIVIDER);
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
    return (m_bPAL ? GC_MASTER_CLOCK_PAL : GC_MASTER_CLOCK_NTSC) / AY_CLOCK_DIVIDER;
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
    m_pBuffer->end_frame(m_ElapsedCycles);

    int count = static_cast<int>(m_pBuffer->read_samples(m_pSampleBuffer, GC_AUDIO_BUFFER_SIZE));

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

        for (int i=0; i<count; i++)
        {
            if (m_bMute)
            {
                pSampleBuffer[i] = 0;
            }
            else
            {
                const float psg = static_cast<float>(m_pSampleBuffer[i]) * PSG_BUS_GAIN;
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
                const float mixed = psg + drive + cassette;
                const float limited = SoftLimit(mixed);

                m_PeakInfo.psg = std::max(m_PeakInfo.psg, std::fabs(psg));
                m_PeakInfo.drive = std::max(m_PeakInfo.drive, std::fabs(drive));
                m_PeakInfo.cassette = std::max(m_PeakInfo.cassette, std::fabs(cassette));
                m_PeakInfo.preLimiter = std::max(m_PeakInfo.preLimiter, std::fabs(mixed));
                m_PeakInfo.output = std::max(m_PeakInfo.output, std::fabs(limited));
                if (std::fabs(mixed) > MIX_SOFT_LIMIT_START)
                    ++m_PeakInfo.softLimitSamples;

                pSampleBuffer[i] = static_cast<s16>(std::clamp(
                    limited, -32768.0f, MIX_FULL_SCALE));
            }
                //+ m_pSGMBuffer[i]
                //+ m_pWavPlayerBuffer[i];
        }

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


}

#define mstosamples(ms) ((GC_AUDIO_SAMPLE_RATE*ms)/1000.0f)

#if GEARSF7000_ENABLE_SF7000
void Audio::PlayDriveTrack(uint8_t oldtrack, uint8_t newtrack)
{
    if (oldtrack == newtrack) return;
    // Ottieni il canale audio associato

    int deltaTracks = std::abs(newtrack - oldtrack);
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

    w.U16(kAudioStateVersion);

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

    // Deliberately absent: m_pSampleBuffer, the SR1000 and WavPlayer carry
    // buffers, the peak meter and the WAV recorders. Those hold samples on
    // their way out to the host, not anything the machine can read back, and
    // together they are 48 KB per snapshot - the single largest thing in a
    // frame, and the one that compresses worst.
}

void Audio::LoadState(std::istream& stream)
{
    StateReader r(stream);

    if (r.U16() != kAudioStateVersion)
        return;

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
        if (ar.Ok())
            m_pAyApu->LoadState(ay);
#else
        // Built without the expansion. The byte count is exactly why it is
        // written: step over the chip and carry on.
        r.Skip(ayBytes);
#endif
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

    // Inizia la registrazione dei 4 canali
    for (int i = 0; i < 4; i++) {
        std::string name = basePath + "_ch" + std::to_string(i) + ".wav";
        m_ChRecorders[i].Start(name, m_iSampleRate);
    }

    // Inizia la registrazione del Master (quello che l'utente sente)
    m_MasterRecorder.Start(basePath + "_master.wav", m_iSampleRate);

    m_bIsRecording = true;
}

void Audio::StopRecording() {
    m_bIsRecording = false;

    for (int i = 0; i < 4; i++) {
        m_ChRecorders[i].Stop();
    }
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

    // Cleared first: Stop() below writes the file, and WritePSG/WriteAY8910
    // check m_bVgmRecordingEnabled before touching m_VgmRecorder, so nothing
    // can append to the buffer while it is being flushed to disk.
    m_bVgmRecordingEnabled = false;
    return m_VgmRecorder.Stop();
}
#endif
