/*
 * Geargrafx - PC Engine / TurboGrafx Emulator
 * Copyright (C) 2024  Saverio Russo

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

#ifndef SOUND_QUEUE_H
#define SOUND_QUEUE_H

#include <stdint.h>
#include <assert.h>

// CoreAudio is the tuned, measured macOS default (see
// DOCS/COREAUDIO_RINGBUFFER.md and DOCS/GEARSF7000_AUDIO_REVIEW.md): kept
// exactly as-is, selectable, not deleted. The SDL3 branch below is a real
// SDL3 Audio implementation - the previous #else branch called SDL2-only API
// (SDL_OpenAudio, SDL_SemWait, AUDIO_S16SYS...) that SDL3 doesn't have, so on
// macOS it never actually compiled; the review already flagged it as
// something to replace, not patch. Force it on macOS with
// -DGEARSF7000_AUDIO_COREAUDIO=0 to A/B it against CoreAudio (the tape
// speaker in particular was noisy under old SDL2) before deciding what a
// Windows/Linux build should ship with - both branches use the same
// streaming, device-callback-driven ring design, so what you measure here is
// what a PC build gets too.
#ifndef GEARSF7000_AUDIO_COREAUDIO
#if defined(__APPLE__)
#define GEARSF7000_AUDIO_COREAUDIO 1
#else
#define GEARSF7000_AUDIO_COREAUDIO 0
#endif
#endif

#if GEARSF7000_AUDIO_COREAUDIO
#include <AudioToolbox/AudioToolbox.h>
#else
#include <SDL3/SDL.h>
#endif
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

class SoundQueue
{
public:
    struct Diagnostics
    {
        uint64_t callbacks = 0;
        uint64_t underruns = 0;
        uint64_t zero_filled_samples = 0;
        uint64_t producer_dropped_samples = 0;
        uint64_t variable_size_callbacks = 0;
        uint64_t min_callback_interval_us = 0;
        uint64_t max_callback_interval_us = 0;
        uint64_t client_sample_rate = 0;
        uint64_t device_sample_rate = 0;
        size_t min_requested_samples = 0;
        size_t max_requested_samples = 0;
        size_t queued_samples = 0;
        size_t ring_capacity_samples = 0;
    };

    SoundQueue();
    ~SoundQueue();
    bool Start(int sample_rate, int channel_count, int buffer_size = 2048, int buffer_count = 3);
    void Stop();
    void Write(int16_t* samples, int count, bool sync);
    int GetSampleCount();
    int16_t* GetCurrentlyPlaying();
    Diagnostics GetDiagnostics() const;
    void ResetDiagnostics();

private:
    // Both backends share this ring/diagnostics shape on purpose: a lock-free
    // SPSC ring drained by a real device callback (AudioQueue on CoreAudio,
    // SDL_AudioStream's pull callback on SDL3), the same producer-blocks-on-
    // space discipline that lets Write(..., sync=true) pace the emulator off
    // the audio clock (see DOCS/GEARSF7000_AUDIO_REVIEW.md sec 5 - this is
    // also what keeps the design VRR-friendly: nothing here assumes a fixed
    // relationship between audio writes and video frame presentation), and
    // the same diagnostics fields so gui_debug's audio panel doesn't need to
    // know which backend is active.
    std::vector<int16_t> m_ring;
    std::atomic<uint64_t> m_ring_read;
    std::atomic<uint64_t> m_ring_write;
    int m_channel_count;
    std::atomic<int16_t*> m_currently_playing;
    std::condition_variable m_space_available;
    std::mutex m_space_mutex;
    std::atomic<bool> m_sound_open;
    std::atomic<bool> m_stopping;
    bool m_sync_output;
    size_t m_expected_buffer_samples;
    std::atomic<uint64_t> m_callback_count;
    std::atomic<uint64_t> m_underrun_count;
    std::atomic<uint64_t> m_zero_filled_samples;
    std::atomic<uint64_t> m_producer_dropped_samples;
    std::atomic<uint64_t> m_variable_size_callbacks;
    std::atomic<size_t> m_min_requested_samples;
    std::atomic<size_t> m_max_requested_samples;
    std::atomic<uint64_t> m_last_callback_ticks;
    std::atomic<size_t> m_min_callback_interval_us;
    std::atomic<size_t> m_max_callback_interval_us;
    uint32_t m_client_sample_rate;
    uint32_t m_device_sample_rate;

#if GEARSF7000_AUDIO_COREAUDIO
    AudioQueueRef m_audio_queue;
    std::vector<AudioQueueBufferRef> m_audio_buffers;
    uint32_t m_timebase_numer;
    uint32_t m_timebase_denom;

    void FillCoreAudioBuffer(AudioQueueBufferRef buffer);
    static void CoreAudioCallback(void* user_data, AudioQueueRef queue,
                                  AudioQueueBufferRef buffer);
#else
    SDL_AudioStream* m_audio_stream;
    SDL_AudioDeviceID m_audio_device;
    std::vector<int16_t> m_scratch;
    bool IsRunningInWSL();

    void FillSDLAudioStream(SDL_AudioStream* stream, int additional_bytes);
    static void SDLAudioStreamCallback(void* userdata, SDL_AudioStream* stream,
                                       int additional_amount, int total_amount);
#endif
};

#endif /* SOUND_QUEUE_H */
