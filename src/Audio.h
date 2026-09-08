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

#ifndef AUDIO_H
#define	AUDIO_H

#include "definitions.h"
#include "audio/Multi_Buffer.h"
#include "audio/Sms_Apu.h"
#if GEARSF7000_ENABLE_SR1000
#include "SR1000_Speaker.h"
#endif
#if GEARSF7000_ENABLE_SF7000
#include "WavPlayer.h"
#endif
#if GEARSF7000_ENABLE_RECORDER
#include "WavRecorder.h"
#include "VgmRecorder.h"
#endif

// Optional AY-3-8910 / YM2149 on an SGM-style expansion. Desktop only: the
// libretro Makefile neither compiles Ay_Apu.cpp nor defines this, and an
// undefined macro evaluates to 0 in #if, same as GEARSF7000_ENABLE_MCP.
//
// This changes sizeof(Audio), so it must be defined identically for every
// translation unit that sees this header - set it in the build, never per file.
#if GEARSF7000_ENABLE_AY
#include "audio/Ay_Apu.h"
#endif


class Audio
{
public:
    struct PeakInfo
    {
        float psg = 0.0f;
        float drive = 0.0f;
        float cassette = 0.0f;
        float preLimiter = 0.0f;
        float output = 0.0f;
        unsigned int softLimitSamples = 0;
        int psgFrameSamples = 0;
        int driveFrameSamples = 0;
        int cassetteFrameSamples = 0;
        int driveQueuedSamples = 0;
        int cassetteQueuedSamples = 0;
        float cassetteFrequencyHz = 0.0f;
        int cassettePendingEvents = 0;
        unsigned int cassetteCoalescedEvents = 0;
        unsigned int cassetteQueueOverflows = 0;
        unsigned int cassetteTimestampRebases = 0;
        unsigned int cassetteInvalidInputs = 0;
    };

    Audio();
    ~Audio();
    void Init();
    // clockRate is the CPU clock the machine is actually running at, which is
    // not the regional constant when the machine has been aligned to the host
    // display. Zero keeps the constant.
    void Reset(bool bPAL, u32 clockRate = 0);
    void Mute(bool mute);
    void WriteAudioRegister(u8 value);
#if GEARSF7000_ENABLE_SR1000
    void SR1000SpeakerWrite(float sample, bool synthesized, float frequencyHz = 0.0f);
#endif

#if GEARSF7000_ENABLE_AY
    enum class AyChip { AY_3_8910, YM2149 };

    // Expansion presence and chip choice behave like hardware, not like a
    // filter: software that has already probed the chip must not see it appear
    // or vanish underneath it, and Ay_Apu::chip_type() re-derives the envelope
    // table without fixing up amplitudes already in flight. Both are therefore
    // latched at Reset(), not applied live. Audio owns no policy about when
    // that happens - the caller decides.
    void SetAyEnabled(bool enabled) { m_bAyEnabledPending = enabled; }
    void SetAyChip(AyChip chip) { m_AyChipPending = chip; }
    bool IsAyEnabled() const { return m_bAyEnabled; }
    AyChip GetAyChip() const { return m_AyChip; }

    // Bus side. Address latch, data write, data read - the AY-3-8910 register
    // interface as the MSX exposes it, minus the chip's own I/O ports which no
    // expansion of this kind uses.
    void AySelectRegister(u8 reg);
    void AyWriteData(u8 value);
    u8 AyReadData() const;

    // For the debugger and the MCP adapter. Unlike AyReadData() these do not
    // disturb or depend on the latched register index.
    Ay_Apu* GetAyApu() { return m_pAyApu; }
    u8 GetAySelectedRegister() const { return m_AySelectedRegister; }
    // Effective chip clock, i.e. the machine clock after the expansion's
    // divider. Defined in Audio.cpp, where that divider lives.
    int GetAyClockHz() const;

    // Debug waveform tap for the AY panel, matching EnablePsgDebug exactly -
    // opt-in, cheap when off, drained once per emulated audio frame inside
    // EndFrame rather than once per GUI draw call (fast forward can run
    // several emulated frames per rendered one; draining on draw would
    // silently skip the extras, corrupting the waveform - same reasoning as
    // the PSG tap).
    //
    // clock_rate passed to Ay_Apu::init_debug_buffers is m_iPsgClockRate, the
    // CPU clock - NOT GetAyClockHz(). The AY has no Blip time domain of its
    // own (see Ay_Apu.h's own comment on set_clock_divider): every write()
    // call and every synth_.offset() inside it counts time in the same CPU-
    // clock blip_time_t the PSG uses, because both chips share m_pBuffer.
    // A debug Blip_Buffer has to agree with that or the waveform comes out
    // at the wrong pitch.
    void EnableAyDebug(bool enable);
    bool IsAyDebugEnabled() const { return m_pAyApu->is_debug_enabled(); }
    const blip_sample_t* GetAyDebugSamples(int channel) const
    {
        return (channel >= 0 && channel < 3) ? m_AyDebugSamples[channel] : NULL;
    }
    long GetAyDebugSampleCount(int channel) const
    {
        return (channel >= 0 && channel < 3) ? m_AyDebugSampleCount[channel] : 0;
    }
#endif

    void Tick(unsigned int clockCycles);
    void EndFrame(s16* pSampleBuffer, int* pSampleCount);
    void SaveState(std::ostream& stream);
    void LoadState(std::istream& stream);

#if GEARSF7000_ENABLE_SF7000
    void PlayDriveTrack(uint8_t oldtrack, uint8_t newtrack);
#endif

    Sms_Apu* GetApu()
    {
        return m_pApu;
    }

    // Runtime-only debugger filters. They deliberately survive machine
    // reset and state loading, but are not serialized or written to config:
    // closing the application restores the normal all-on defaults.
    void SetPsgChannelEnabled(int channel, bool enabled);
    void SetPsgChannelMuted(int channel, bool muted);
    bool IsPsgChannelEnabled(int channel) const;
    bool IsPsgChannelMuted(int channel) const;
#if GEARSF7000_ENABLE_AY
    void SetAyChannelEnabled(int channel, bool enabled);
    void SetAyChannelMuted(int channel, bool muted);
    bool IsAyChannelEnabled(int channel) const;
    bool IsAyChannelMuted(int channel) const;
#endif

    // Trims the audio resampler, not the emulation.
    //
    // The device's crystal is not the emulated one - measured here, about 200
    // ppm apart - so producing exactly 48000 samples for every emulated
    // second slowly fills or empties the queue until it clicks. Something has
    // to absorb that difference. Bending the machine's frame rate absorbs it
    // in the wrong place: this is a tool built to be believed about timing,
    // and 59.9227 has to mean 59.9227 against a real clock, not "whatever
    // keeps the sound card happy".
    //
    // So the difference goes into the resampler instead. Blip_Buffer converts
    // emulated cycles to samples through one factor; nudging the clock rate
    // it is told about changes how many samples an emulated second produces,
    // and nothing else. The machine keeps its own frame rate exactly, and the
    // pitch moves by the same fraction as the crystal error - a fiftieth of a
    // cent at 200 ppm, which no one can hear.
    void SetClockTrim(double ratio);

    // Debug waveform tap for the PSG panel - opt-in, cheap when off, only
    // enabled while that window is actually open. clock_rate here has to
    // match m_pBuffer's own, which Reset() latches into m_iPsgClockRate,
    // otherwise the debug waveform's pitch and the real audio's pitch
    // silently disagree.
    //
    // Drained once per emulated audio frame, inside EndFrame - not once per
    // GUI draw call. Fast-forward can run several emulated frames per
    // rendered one; draining on draw would silently skip whatever those
    // extra frames wrote, corrupting the waveform. The panel only peeks at
    // the latest snapshot.
    void EnablePsgDebug(bool enable);
    bool IsPsgDebugEnabled() const { return m_pApu->is_debug_enabled(); }
    const blip_sample_t* GetPsgDebugSamples(int channel) const
    {
        return (channel >= 0 && channel < 4) ? m_PsgDebugSamples[channel] : NULL;
    }
    long GetPsgDebugSampleCount(int channel) const
    {
        return (channel >= 0 && channel < 4) ? m_PsgDebugSampleCount[channel] : 0;
    }


#if GEARSF7000_ENABLE_SF7000
    WavPlayer* GetWavPlayer()
    {
        return &m_pWavPlayer;
    }
#endif

#if GEARSF7000_ENABLE_SR1000
    SR1000Speaker* GetSR1000Speaker()
    {
        return m_pSR1000Speaker;
    }
#endif

    const PeakInfo& GetPeakInfo() const { return m_PeakInfo; }
    void ResetPeakInfo() { m_PeakInfo = {}; }

#if GEARSF7000_ENABLE_RECORDER
    void StartRecording(const std::string& basePath);
    void StopRecording();
    bool IsRecording() const { return m_bIsRecording; }

    // VGM export - the register-write log a real chip's I/O port sees, not a
    // sample recording. clockRate is the machine's nominal clock for the
    // region (GC_MASTER_CLOCK_NTSC/PAL), never the live one after clock trim
    // or alignment: a VGM player replays writes on its own clock, so the file
    // has to state the crystal a real machine would use, not whatever this
    // session happened to be running at.
    bool StartVgmRecording(const char* file_path, int clockRate, bool isPAL);
    // Returns the path written, or empty if nothing was recording.
    std::string StopVgmRecording();
    bool IsVgmRecording() const { return m_bVgmRecordingEnabled; }
#endif

private:
    void ApplyDebugChannelFilters();

    Sms_Apu* m_pApu;
    Stereo_Buffer* m_pBuffer;
    bool m_PsgChannelEnabled[4] = { true, true, true, true };
    bool m_PsgChannelMuted[4] = { false, false, false, false };

#if GEARSF7000_ENABLE_AY
    // Shares m_pBuffer with the PSG. Stereo_Buffer holds exactly three fixed
    // Blip_Buffers, so a second chip cannot have a buffer of its own - it
    // attaches to the same one and Blip sums the deltas for free. That also
    // means both chips must live in the same time domain; see
    // Ay_Apu::set_clock_divider().
    Ay_Apu* m_pAyApu;
    bool m_bAyEnabled;
    bool m_bAyEnabledPending;
    AyChip m_AyChip;
    AyChip m_AyChipPending;
    u8 m_AySelectedRegister;
    bool m_AyChannelEnabled[3] = { true, true, true };
    bool m_AyChannelMuted[3] = { false, false, false };
#endif

#if GEARSF7000_ENABLE_SR1000
    SR1000Speaker* m_pSR1000Speaker;
    s16 m_pSR1000SpeakerBuffer[GC_AUDIO_BUFFER_SIZE];
    s16 m_pSR1000SpeakerCarry[GC_AUDIO_BUFFER_SIZE];
    int m_iSR1000SpeakerCarryCount;
#endif

#if GEARSF7000_ENABLE_SF7000
    WavPlayer m_pWavPlayer;
    float m_pWavPlayerBuffer[GC_AUDIO_BUFFER_SIZE];
    float m_pWavPlayerCarry[GC_AUDIO_BUFFER_SIZE];
    int m_iWavPlayerCarryCount;
#endif


    u64 m_ElapsedCycles;
    int m_iSampleRate;
    u32 m_iPsgClockRate;
    // 4096 comfortably covers a PAL frame at any sample rate this build
    // supports (see GC_AUDIO_SAMPLE_RATE) - one emulated frame's worth of
    // debug samples, refreshed in EndFrame, not a rolling history.
    blip_sample_t m_PsgDebugSamples[4][4096];
    long m_PsgDebugSampleCount[4];
#if GEARSF7000_ENABLE_AY
    blip_sample_t m_AyDebugSamples[3][4096];
    long m_AyDebugSampleCount[3];
#endif
    blip_sample_t* m_pSampleBuffer;
    bool m_bPAL;
    PeakInfo m_PeakInfo;
    //s16* m_pSGMBuffer;
    bool m_bMute;

#if GEARSF7000_ENABLE_RECORDER
    WavRecorder m_ChRecorders[4];    // Recorder per i 4 canali separati
    WavRecorder m_MasterRecorder;   // Recorder per l'audio finale mixato
    bool m_bIsRecording = false;    // Variabile di stato
    std::string m_sRecordingPath;   // Percorso base

    VgmRecorder m_VgmRecorder;
    bool m_bVgmRecordingEnabled = false;
#endif

#ifdef WRITE_AUDIO_TO_FILE 
    FILE* fp;
#endif
};

inline void Audio::Tick(unsigned int clockCycles)
{
    m_ElapsedCycles += clockCycles;

#if GEARSF7000_ENABLE_SF7000
    m_pWavPlayer.Tick(clockCycles);
#endif
#if GEARSF7000_ENABLE_SR1000
    m_pSR1000Speaker->Tick(clockCycles);
#endif
    // Ay_Apu needs no Tick: like Sms_Apu it is driven entirely by timestamped
    // writes and end_frame(), so it catches up on demand.
}

inline void Audio::WriteAudioRegister(u8 value)
{
    m_pApu->write_data((blip_time_t)m_ElapsedCycles, value);
#if GEARSF7000_ENABLE_RECORDER
    if (m_bVgmRecordingEnabled)
        m_VgmRecorder.WritePSG(value);
#endif
}

#if GEARSF7000_ENABLE_AY
inline void Audio::AySelectRegister(u8 reg)
{
    m_AySelectedRegister = reg & 0x0F;
}

inline void Audio::AyWriteData(u8 value)
{
    // MachineIOPorts calls this on every write to the port range,
    // whether or not the card is fitted - the AY has no READY line, so
    // software can write into empty air and the call site cannot tell.
    // Recording has to follow what actually reached a chip, the same gate
    // as the line above: an unfitted card silently drops the write on real
    // hardware, and a VGM file claiming an AY-3-8910 was present when it
    // never was would be a false record of the session, not a faithful one.
    if (m_bAyEnabled)
    {
        m_pAyApu->write((blip_time_t)m_ElapsedCycles, m_AySelectedRegister, value);
#if GEARSF7000_ENABLE_RECORDER
        if (m_bVgmRecordingEnabled)
            m_VgmRecorder.WriteAY8910(m_AySelectedRegister, value);
#endif
    }
}

inline u8 Audio::AyReadData() const
{
    // Nothing on the bus when the expansion is absent: the SC-3000 has no
    // pull-ups that would produce anything else, so an absent card reads as
    // open bus like every other unclaimed port in MachineIOPorts::In().
    if (!m_bAyEnabled)
        return 0xFF;
    return static_cast<u8>(m_pAyApu->read(m_AySelectedRegister));
}
#endif

//inline void Audio::SGMWrite(u8 value)
//{
//    m_pAY8910->WriteRegister(value);
//}
//
//inline u8 Audio::SGMRead()
//{
//    return m_pAY8910->ReadRegister();
//}
//
//inline void Audio::SGMRegister(u8 reg)
//{
//    m_pAY8910->SelectRegister(reg);
//}

#if GEARSF7000_ENABLE_SR1000
inline void Audio::SR1000SpeakerWrite(float sample, bool synthesized, float frequencyHz)
{
    m_pSR1000Speaker->WriteSample(sample, synthesized, frequencyHz);
}
#endif

#endif	/* AUDIO_H */
