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

#include "sound_queue.h"
#include <string>
#include <cstdio>
#include <assert.h>
#include "../../src/gearsf7000.h"
#ifdef _WIN32
#include <ShObjIdl.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>

#if GEARSF7000_AUDIO_COREAUDIO
#include <mach/mach_time.h>
#endif

namespace
{
constexpr size_t kBytesPerSample = sizeof(int16_t);
// The emulator produces one video frame at a time.  A macOS compositor hiccup
// can therefore postpone a producer write by 16--33 ms even when the audio
// callback is punctual.  Keep extra *ring* headroom without increasing the
// device callback period itself, so normal callback granularity stays
// unchanged while short UI stalls cannot turn into zero-filled discontinuities.
// Applies to both backends: SDL3's SDL_AUDIO_DEVICE_SAMPLE_FRAMES hint asks
// for the same period CoreAudio is handed explicitly.
constexpr size_t kRingMultiplier = 2;
constexpr auto kProducerWaitLimit = std::chrono::milliseconds(40);

void AtomicMinimum(std::atomic<size_t>& value, size_t candidate)
{
    size_t current = value.load(std::memory_order_relaxed);
    while (candidate < current
           && !value.compare_exchange_weak(current, candidate,
                                           std::memory_order_relaxed))
    {
    }
}

void AtomicMaximum(std::atomic<size_t>& value, size_t candidate)
{
    size_t current = value.load(std::memory_order_relaxed);
    while (candidate > current
           && !value.compare_exchange_weak(current, candidate,
                                           std::memory_order_relaxed))
    {
    }
}

#if GEARSF7000_AUDIO_COREAUDIO
void LogCoreAudioError(const char* operation, OSStatus status)
{
    Log("CoreAudio: %s failed (OSStatus %d)", operation, static_cast<int>(status));
}
#endif
}

// ---------------------------------------------------------------------------
// Shared across both backends: the lock-free SPSC ring, its diagnostics, and
// Write()'s producer-blocks-on-space discipline. Both backends drain this
// ring from a real device callback (AudioQueue on CoreAudio, SDL_AudioStream's
// pull callback on SDL3) rather than a fixed-rate poll, which is what lets
// Write(..., sync=true) pace the emulator off the audio clock instead of a
// wall-clock timer (see DOCS/GEARSF7000_AUDIO_REVIEW.md sec 5) - and, because
// nothing here assumes a fixed relationship between an audio write and a
// video frame presenting, is also what keeps this VRR-friendly.
// ---------------------------------------------------------------------------

void SoundQueue::Write(int16_t* samples, int count, bool sync)
{
    if (!samples || count <= 0 || !m_sound_open.load(std::memory_order_acquire))
        return;

    m_sync_output = sync;
    size_t remaining = static_cast<size_t>(count);
    const int16_t* source = samples;

    const size_t capacity = m_ring.size();
    if (!sync && remaining > capacity)
    {
        source += remaining - capacity;
        remaining = capacity;
    }

    while (remaining > 0)
    {
        const uint64_t write = m_ring_write.load(std::memory_order_relaxed);
        const uint64_t read = m_ring_read.load(std::memory_order_acquire);
        const size_t used = static_cast<size_t>(write - read);
        if (!m_sound_open.load(std::memory_order_acquire)
            || m_stopping.load(std::memory_order_acquire) || capacity == 0)
            return;

        size_t freeSamples = capacity - std::min(used, capacity);
        if (freeSamples == 0)
        {
            // The callback exclusively owns the read counter.  Moving it from
            // here would race with the realtime thread and can splice two
            // unrelated blocks together.  Never block the emulation/UI thread
            // forever: if the device backend is unavailable or stopped,
            // dropping this newest chunk is preferable to freezing the whole
            // computer being emulated.
            if (sync)
            {
                std::unique_lock<std::mutex> lock(m_space_mutex);
                const bool spaceAvailable = m_space_available.wait_for(
                    lock, kProducerWaitLimit, [this] {
                        return m_ring_write.load(std::memory_order_relaxed)
                            - m_ring_read.load(std::memory_order_acquire) < m_ring.size()
                            || !m_sound_open.load(std::memory_order_acquire)
                            || m_stopping.load(std::memory_order_acquire);
                    });
                if (spaceAvailable)
                    continue;
            }
            m_producer_dropped_samples.fetch_add(remaining,
                                                  std::memory_order_relaxed);
            return;
        }

        const size_t start = static_cast<size_t>(write % capacity);
        const size_t contiguous = capacity - start;
        const size_t copied = std::min({remaining, freeSamples, contiguous});
        if (copied == 0)
            continue;

        std::memcpy(m_ring.data() + start, source,
                    copied * sizeof(int16_t));
        m_ring_write.store(write + copied, std::memory_order_release);
        source += copied;
        remaining -= copied;
    }
}

int SoundQueue::GetSampleCount()
{
    const uint64_t write = m_ring_write.load(std::memory_order_acquire);
    const uint64_t read = m_ring_read.load(std::memory_order_acquire);
    return static_cast<int>(std::min<uint64_t>(write - read, m_ring.size()));
}

int16_t* SoundQueue::GetCurrentlyPlaying()
{
    return m_currently_playing.load(std::memory_order_acquire);
}

SoundQueue::Diagnostics SoundQueue::GetDiagnostics() const
{
    Diagnostics result;
    result.callbacks = m_callback_count.load(std::memory_order_relaxed);
    result.underruns = m_underrun_count.load(std::memory_order_relaxed);
    result.zero_filled_samples = m_zero_filled_samples.load(std::memory_order_relaxed);
    result.producer_dropped_samples = m_producer_dropped_samples.load(std::memory_order_relaxed);
    result.variable_size_callbacks = m_variable_size_callbacks.load(std::memory_order_relaxed);
    const size_t minimum = m_min_requested_samples.load(std::memory_order_relaxed);
    result.min_requested_samples = minimum == SIZE_MAX ? 0 : minimum;
    result.max_requested_samples = m_max_requested_samples.load(std::memory_order_relaxed);
    const size_t minimumInterval = m_min_callback_interval_us.load(std::memory_order_relaxed);
    result.min_callback_interval_us = minimumInterval == SIZE_MAX ? 0 : minimumInterval;
    result.max_callback_interval_us = m_max_callback_interval_us.load(std::memory_order_relaxed);
    result.client_sample_rate = m_client_sample_rate;
    result.device_sample_rate = m_device_sample_rate;
    const uint64_t write = m_ring_write.load(std::memory_order_acquire);
    const uint64_t read = m_ring_read.load(std::memory_order_acquire);
    result.queued_samples = static_cast<size_t>(std::min<uint64_t>(
        write - read, m_ring.size()));
    result.ring_capacity_samples = m_ring.size();
    return result;
}

void SoundQueue::ResetDiagnostics()
{
    m_callback_count.store(0, std::memory_order_relaxed);
    m_underrun_count.store(0, std::memory_order_relaxed);
    m_zero_filled_samples.store(0, std::memory_order_relaxed);
    m_producer_dropped_samples.store(0, std::memory_order_relaxed);
    m_variable_size_callbacks.store(0, std::memory_order_relaxed);
    m_min_requested_samples.store(SIZE_MAX, std::memory_order_relaxed);
    m_max_requested_samples.store(0, std::memory_order_relaxed);
    m_last_callback_ticks.store(0, std::memory_order_relaxed);
    m_min_callback_interval_us.store(SIZE_MAX, std::memory_order_relaxed);
    m_max_callback_interval_us.store(0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Backend-specific: construction, teardown, device setup and the fill
// callback each device API calls when it wants more samples.
// ---------------------------------------------------------------------------

#if GEARSF7000_AUDIO_COREAUDIO

SoundQueue::SoundQueue()
    : m_ring_read(0)
    , m_ring_write(0)
    , m_channel_count(0)
    , m_currently_playing(nullptr)
    , m_sound_open(false)
    , m_stopping(false)
    , m_sync_output(true)
    , m_expected_buffer_samples(0)
    , m_callback_count(0)
    , m_underrun_count(0)
    , m_zero_filled_samples(0)
    , m_producer_dropped_samples(0)
    , m_variable_size_callbacks(0)
    , m_min_requested_samples(SIZE_MAX)
    , m_max_requested_samples(0)
    , m_last_callback_ticks(0)
    , m_min_callback_interval_us(SIZE_MAX)
    , m_max_callback_interval_us(0)
    , m_client_sample_rate(0)
    , m_device_sample_rate(0)
    , m_audio_queue(nullptr)
    , m_timebase_numer(1)
    , m_timebase_denom(1)
{
    Log("SoundQueue: native CoreAudio AudioQueue backend");
}

SoundQueue::~SoundQueue()
{
    Stop();
}

bool SoundQueue::Start(int sample_rate, int channel_count,
                       int buffer_size, int buffer_count)
{
    Stop();
    if (sample_rate <= 0 || channel_count <= 0
        || buffer_size <= 0 || buffer_count < 2)
        return false;

    m_channel_count = channel_count;
    const size_t ringSamples = static_cast<size_t>(buffer_size) * buffer_count
        * kRingMultiplier;
    m_ring.assign(ringSamples, 0);
    m_ring_read.store(0, std::memory_order_relaxed);
    // Primed half full with the silence the ring was just cleared to, rather
    // than started empty.
    //
    // The device begins draining the moment it opens, and the producer only
    // ever supplies one frame's worth per frame - production and consumption
    // match by construction, so an empty ring stays empty and underruns
    // forever. Something has to put the first half-buffer in, and the frame
    // scheduler is the wrong thing to do it with: it can only fill by running
    // the machine fast, which means overshooting by however much the control
    // loop lags. Measured over four minutes, that was a buffer reaching 95%
    // full, 1208 samples dropped and five underruns - all of them inside the
    // first ten seconds, and none afterwards.
    //
    // Half a ring of silence is latency the listener never hears, and it
    // starts the drift correction where it is meant to work: near the target,
    // in its gentle regime.
    m_ring_write.store(ringSamples / 2, std::memory_order_relaxed);
    m_currently_playing.store(nullptr, std::memory_order_relaxed);
    m_expected_buffer_samples = static_cast<size_t>(buffer_size);
    m_client_sample_rate = static_cast<uint32_t>(sample_rate);
    m_device_sample_rate = 0;
    m_callback_count.store(0, std::memory_order_relaxed);
    m_underrun_count.store(0, std::memory_order_relaxed);
    m_zero_filled_samples.store(0, std::memory_order_relaxed);
    m_producer_dropped_samples.store(0, std::memory_order_relaxed);
    m_variable_size_callbacks.store(0, std::memory_order_relaxed);
    m_min_requested_samples.store(SIZE_MAX, std::memory_order_relaxed);
    m_max_requested_samples.store(0, std::memory_order_relaxed);
    mach_timebase_info_data_t timebase = {};
    mach_timebase_info(&timebase);
    m_timebase_numer = timebase.numer;
    m_timebase_denom = timebase.denom;
    m_last_callback_ticks.store(0, std::memory_order_relaxed);
    m_min_callback_interval_us.store(SIZE_MAX, std::memory_order_relaxed);
    m_max_callback_interval_us.store(0, std::memory_order_relaxed);
    m_stopping.store(false, std::memory_order_release);

    AudioStreamBasicDescription format = {};
    format.mSampleRate = static_cast<Float64>(sample_rate);
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger
                        | kLinearPCMFormatFlagIsPacked;
    format.mBytesPerPacket = static_cast<UInt32>(channel_count * sizeof(int16_t));
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = static_cast<UInt32>(channel_count * sizeof(int16_t));
    format.mChannelsPerFrame = static_cast<UInt32>(channel_count);
    format.mBitsPerChannel = 16;

    OSStatus status = AudioQueueNewOutput(&format, &CoreAudioCallback, this,
                                           nullptr, nullptr, 0, &m_audio_queue);
    if (status != noErr)
    {
        LogCoreAudioError("AudioQueueNewOutput", status);
        m_audio_queue = nullptr;
        return false;
    }

    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 deviceSize = sizeof(device);
    status = AudioQueueGetProperty(m_audio_queue, kAudioQueueProperty_CurrentDevice,
                                   &device, &deviceSize);
    if (status == noErr && device != kAudioObjectUnknown)
    {
        AudioObjectPropertyAddress rateProperty = {
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        Float64 deviceRate = 0.0;
        UInt32 rateSize = sizeof(deviceRate);
        status = AudioObjectGetPropertyData(device, &rateProperty, 0, nullptr,
                                            &rateSize, &deviceRate);
        if (status == noErr && deviceRate > 0.0)
            m_device_sample_rate = static_cast<uint32_t>(deviceRate + 0.5);
    }

    const UInt32 bytesPerBuffer = static_cast<UInt32>(
        static_cast<size_t>(buffer_size) * sizeof(int16_t));
    m_audio_buffers.assign(static_cast<size_t>(buffer_count), nullptr);
    for (AudioQueueBufferRef& buffer : m_audio_buffers)
    {
        status = AudioQueueAllocateBuffer(m_audio_queue, bytesPerBuffer, &buffer);
        if (status != noErr)
        {
            LogCoreAudioError("AudioQueueAllocateBuffer", status);
            Stop();
            return false;
        }

        std::memset(buffer->mAudioData, 0, buffer->mAudioDataBytesCapacity);
        buffer->mAudioDataByteSize = buffer->mAudioDataBytesCapacity;
        status = AudioQueueEnqueueBuffer(m_audio_queue, buffer, 0, nullptr);
        if (status != noErr)
        {
            LogCoreAudioError("AudioQueueEnqueueBuffer", status);
            Stop();
            return false;
        }
    }

    m_sound_open.store(true, std::memory_order_release);
    status = AudioQueueStart(m_audio_queue, nullptr);
    if (status != noErr)
    {
        LogCoreAudioError("AudioQueueStart", status);
        Stop();
        return false;
    }

    Log("CoreAudio: started %d Hz, %d channels, %d frames/buffer, %d buffers, %zu ring samples",
        sample_rate, channel_count, buffer_size / channel_count, buffer_count,
        m_ring.size());
    return true;
}

void SoundQueue::Stop()
{
    m_stopping.store(true, std::memory_order_release);
    m_sound_open.store(false, std::memory_order_release);
    m_space_available.notify_all();

    if (m_audio_queue)
    {
        // Immediate stop is synchronous: when it returns no callback remains
        // in flight, so the ring and buffers can safely be released.
        AudioQueueStop(m_audio_queue, true);
        AudioQueueDispose(m_audio_queue, true);
        m_audio_queue = nullptr;
    }

    m_audio_buffers.clear();
    m_ring.clear();
    m_ring_read.store(0, std::memory_order_relaxed);
    m_ring_write.store(0, std::memory_order_relaxed);
    m_currently_playing.store(nullptr, std::memory_order_relaxed);
}

void SoundQueue::FillCoreAudioBuffer(AudioQueueBufferRef buffer)
{
    const size_t requested = buffer->mAudioDataBytesCapacity / kBytesPerSample;
    auto* output = static_cast<int16_t*>(buffer->mAudioData);
    size_t copied = 0;
    m_callback_count.fetch_add(1, std::memory_order_relaxed);
    const uint64_t now = mach_continuous_time();
    const uint64_t previous = m_last_callback_ticks.exchange(now, std::memory_order_relaxed);
    if (previous != 0 && m_timebase_denom != 0)
    {
        const uint64_t intervalUs = (now - previous) * m_timebase_numer
            / m_timebase_denom / 1000;
        AtomicMinimum(m_min_callback_interval_us, static_cast<size_t>(intervalUs));
        AtomicMaximum(m_max_callback_interval_us, static_cast<size_t>(intervalUs));
    }
    AtomicMinimum(m_min_requested_samples, requested);
    AtomicMaximum(m_max_requested_samples, requested);
    if (requested != m_expected_buffer_samples)
        m_variable_size_callbacks.fetch_add(1, std::memory_order_relaxed);

    if (m_sound_open.load(std::memory_order_acquire)
        && !m_stopping.load(std::memory_order_acquire) && !m_ring.empty())
    {
        const uint64_t read = m_ring_read.load(std::memory_order_relaxed);
        const uint64_t write = m_ring_write.load(std::memory_order_acquire);
        copied = std::min<size_t>(requested,
            static_cast<size_t>(std::min<uint64_t>(write - read, m_ring.size())));
        const size_t start = static_cast<size_t>(read % m_ring.size());
        const size_t first = std::min(copied, m_ring.size() - start);
        std::memcpy(output, m_ring.data() + start, first * sizeof(int16_t));
        if (copied > first)
        {
            std::memcpy(output + first, m_ring.data(),
                        (copied - first) * sizeof(int16_t));
        }
        m_ring_read.store(read + copied, std::memory_order_release);
    }
    m_currently_playing.store(output, std::memory_order_release);

    if (copied < requested)
    {
        std::memset(output + copied, 0, (requested - copied) * sizeof(int16_t));
        if (m_sound_open.load(std::memory_order_acquire)
            && !m_stopping.load(std::memory_order_acquire))
        {
            m_underrun_count.fetch_add(1, std::memory_order_relaxed);
            m_zero_filled_samples.fetch_add(requested - copied,
                                             std::memory_order_relaxed);
        }
    }

    buffer->mAudioDataByteSize = static_cast<UInt32>(requested * sizeof(int16_t));
    m_space_available.notify_all();
}

void SoundQueue::CoreAudioCallback(void* user_data, AudioQueueRef queue,
                                   AudioQueueBufferRef buffer)
{
    auto* self = static_cast<SoundQueue*>(user_data);
    self->FillCoreAudioBuffer(buffer);
    if (!self->m_stopping.load(std::memory_order_acquire))
    {
        const OSStatus status = AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
        if (status != noErr)
            LogCoreAudioError("AudioQueueEnqueueBuffer callback", status);
    }
}

#else // !GEARSF7000_AUDIO_COREAUDIO - real SDL3 Audio, streaming/device-callback driven

namespace
{
void SdlError(const char* fallback)
{
    const char* sdl_str = SDL_GetError();
    Log("SoundQueue: %s", (sdl_str && *sdl_str) ? sdl_str : fallback);
}
}

bool SoundQueue::IsRunningInWSL()
{
    FILE* file = fopen("/proc/sys/fs/binfmt_misc/WSLInterop", "r");
    if (file)
    {
        fclose(file);
        return true;
    }
    return false;
}

SoundQueue::SoundQueue()
    : m_ring_read(0)
    , m_ring_write(0)
    , m_channel_count(0)
    , m_currently_playing(nullptr)
    , m_sound_open(false)
    , m_stopping(false)
    , m_sync_output(true)
    , m_expected_buffer_samples(0)
    , m_callback_count(0)
    , m_underrun_count(0)
    , m_zero_filled_samples(0)
    , m_producer_dropped_samples(0)
    , m_variable_size_callbacks(0)
    , m_min_requested_samples(SIZE_MAX)
    , m_max_requested_samples(0)
    , m_last_callback_ticks(0)
    , m_min_callback_interval_us(SIZE_MAX)
    , m_max_callback_interval_us(0)
    , m_client_sample_rate(0)
    , m_device_sample_rate(0)
    , m_audio_stream(nullptr)
    , m_audio_device(0)
{
#ifdef _WIN32
    // DirectSound is a Windows-specific SDL backend. Let SDL pick the
    // platform default everywhere else.
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "directsound");
#elif defined(__linux__)
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, IsRunningInWSL() ? "pulseaudio" : "alsa");
#endif

    if (!SDL_Init(SDL_INIT_AUDIO))
        SdlError("Couldn't init AUDIO subsystem");

    Log("SoundQueue: SDL3 Audio backend (%d driver(s) available)", SDL_GetNumAudioDrivers());
    for (int i = 0; i < SDL_GetNumAudioDrivers(); i++)
        Debug("SoundQueue: driver available: %s", SDL_GetAudioDriver(i));

    int device_count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&device_count);
    if (devices)
    {
        Debug("SoundQueue: %d playback device(s)", device_count);
        for (int i = 0; i < device_count; i++)
            Debug("SoundQueue: device: %s", SDL_GetAudioDeviceName(devices[i]));
        SDL_free(devices);
    }

    Log("SoundQueue: %s driver selected", SDL_GetCurrentAudioDriver());

    atexit(SDL_Quit);
}

SoundQueue::~SoundQueue()
{
    Stop();
}

bool SoundQueue::Start(int sample_rate, int channel_count,
                       int buffer_size, int buffer_count)
{
    Stop();
    if (sample_rate <= 0 || channel_count <= 0
        || buffer_size <= 0 || buffer_count < 2)
        return false;

    m_channel_count = channel_count;
    const size_t ringSamples = static_cast<size_t>(buffer_size) * buffer_count
        * kRingMultiplier;
    m_ring.assign(ringSamples, 0);
    m_ring_read.store(0, std::memory_order_relaxed);
    // Primed half full with the silence the ring was just cleared to, rather
    // than started empty.
    //
    // The device begins draining the moment it opens, and the producer only
    // ever supplies one frame's worth per frame - production and consumption
    // match by construction, so an empty ring stays empty and underruns
    // forever. Something has to put the first half-buffer in, and the frame
    // scheduler is the wrong thing to do it with: it can only fill by running
    // the machine fast, which means overshooting by however much the control
    // loop lags. Measured over four minutes, that was a buffer reaching 95%
    // full, 1208 samples dropped and five underruns - all of them inside the
    // first ten seconds, and none afterwards.
    //
    // Half a ring of silence is latency the listener never hears, and it
    // starts the drift correction where it is meant to work: near the target,
    // in its gentle regime.
    m_ring_write.store(ringSamples / 2, std::memory_order_relaxed);
    m_currently_playing.store(nullptr, std::memory_order_relaxed);
    m_expected_buffer_samples = static_cast<size_t>(buffer_size);
    m_client_sample_rate = static_cast<uint32_t>(sample_rate);
    m_device_sample_rate = 0;
    m_callback_count.store(0, std::memory_order_relaxed);
    m_underrun_count.store(0, std::memory_order_relaxed);
    m_zero_filled_samples.store(0, std::memory_order_relaxed);
    m_producer_dropped_samples.store(0, std::memory_order_relaxed);
    m_variable_size_callbacks.store(0, std::memory_order_relaxed);
    m_min_requested_samples.store(SIZE_MAX, std::memory_order_relaxed);
    m_max_requested_samples.store(0, std::memory_order_relaxed);
    m_last_callback_ticks.store(0, std::memory_order_relaxed);
    m_min_callback_interval_us.store(SIZE_MAX, std::memory_order_relaxed);
    m_max_callback_interval_us.store(0, std::memory_order_relaxed);
    m_stopping.store(false, std::memory_order_release);
    // Sized for the common case; FillSDLAudioStream() grows it on demand if a
    // device ever asks for more than this in one pull (rare - see the
    // variable_size_callbacks diagnostic for how often that happens).
    m_scratch.assign(static_cast<size_t>(buffer_size) * 2, 0);

    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16;
    spec.channels = channel_count;
    spec.freq = sample_rate;

    // Ask for the same callback period CoreAudio is handed explicitly via
    // AudioQueueAllocateBuffer(bytesPerBuffer). SDL is free to ignore this on
    // platforms/drivers that don't support a fixed period; the callback
    // handles whatever size actually arrives either way.
    char frames[16];
    std::snprintf(frames, sizeof(frames), "%d", buffer_size / channel_count);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames);

    m_audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                                &spec, &SDLAudioStreamCallback, this);
    if (!m_audio_stream)
    {
        SdlError("Couldn't open SDL3 audio device stream");
        return false;
    }

    m_audio_device = SDL_GetAudioStreamDevice(m_audio_stream);

    SDL_AudioSpec device_spec;
    int device_frames = 0;
    if (m_audio_device != 0
        && SDL_GetAudioDeviceFormat(m_audio_device, &device_spec, &device_frames))
        m_device_sample_rate = static_cast<uint32_t>(device_spec.freq);

    m_sound_open.store(true, std::memory_order_release);

    // A stream opened via SDL_OpenAudioDeviceStream() begins paused so setup
    // can finish first; the pull callback only starts firing after this.
    if (!SDL_ResumeAudioStreamDevice(m_audio_stream))
    {
        SdlError("Couldn't resume SDL3 audio device");
        Stop();
        return false;
    }

    Log("SoundQueue: SDL3 started %d Hz, %d channels, %d frames/buffer, %zu ring samples (device: %u Hz)",
        sample_rate, channel_count, buffer_size / channel_count, m_ring.size(),
        m_device_sample_rate);
    return true;
}

void SoundQueue::Stop()
{
    m_stopping.store(true, std::memory_order_release);
    m_sound_open.store(false, std::memory_order_release);
    m_space_available.notify_all();

    if (m_audio_stream)
    {
        // SDL_LockAudioStream()/UnlockAudioStream() are documented to
        // guarantee the pull callback is not running while the lock is
        // held - taking and releasing it here is the same synchronous-stop
        // guarantee AudioQueueStop(..., true) gives the CoreAudio branch, so
        // the ring can be released safely right after.
        SDL_PauseAudioStreamDevice(m_audio_stream);
        SDL_LockAudioStream(m_audio_stream);
        SDL_UnlockAudioStream(m_audio_stream);
        // Also closes the device that SDL_OpenAudioDeviceStream() opened
        // alongside it.
        SDL_DestroyAudioStream(m_audio_stream);
        m_audio_stream = nullptr;
    }
    m_audio_device = 0;

    m_ring.clear();
    m_ring_read.store(0, std::memory_order_relaxed);
    m_ring_write.store(0, std::memory_order_relaxed);
    m_currently_playing.store(nullptr, std::memory_order_relaxed);
}

void SoundQueue::FillSDLAudioStream(SDL_AudioStream* stream, int additional_bytes)
{
    if (additional_bytes <= 0)
        return;

    const size_t requested = static_cast<size_t>(additional_bytes) / kBytesPerSample;
    if (requested == 0)
        return;
    if (m_scratch.size() < requested)
        m_scratch.resize(requested);
    int16_t* output = m_scratch.data();

    size_t copied = 0;
    m_callback_count.fetch_add(1, std::memory_order_relaxed);
    const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    const uint64_t previous = m_last_callback_ticks.exchange(now, std::memory_order_relaxed);
    if (previous != 0)
    {
        const uint64_t intervalUs = now - previous;
        AtomicMinimum(m_min_callback_interval_us, static_cast<size_t>(intervalUs));
        AtomicMaximum(m_max_callback_interval_us, static_cast<size_t>(intervalUs));
    }
    AtomicMinimum(m_min_requested_samples, requested);
    AtomicMaximum(m_max_requested_samples, requested);
    if (requested != m_expected_buffer_samples)
        m_variable_size_callbacks.fetch_add(1, std::memory_order_relaxed);

    if (m_sound_open.load(std::memory_order_acquire)
        && !m_stopping.load(std::memory_order_acquire) && !m_ring.empty())
    {
        const uint64_t read = m_ring_read.load(std::memory_order_relaxed);
        const uint64_t write = m_ring_write.load(std::memory_order_acquire);
        copied = std::min<size_t>(requested,
            static_cast<size_t>(std::min<uint64_t>(write - read, m_ring.size())));
        const size_t start = static_cast<size_t>(read % m_ring.size());
        const size_t first = std::min(copied, m_ring.size() - start);
        std::memcpy(output, m_ring.data() + start, first * sizeof(int16_t));
        if (copied > first)
        {
            std::memcpy(output + first, m_ring.data(),
                        (copied - first) * sizeof(int16_t));
        }
        m_ring_read.store(read + copied, std::memory_order_release);
    }
    m_currently_playing.store(output, std::memory_order_release);

    if (copied < requested)
    {
        std::memset(output + copied, 0, (requested - copied) * sizeof(int16_t));
        if (m_sound_open.load(std::memory_order_acquire)
            && !m_stopping.load(std::memory_order_acquire))
        {
            m_underrun_count.fetch_add(1, std::memory_order_relaxed);
            m_zero_filled_samples.fetch_add(requested - copied,
                                             std::memory_order_relaxed);
        }
    }

    SDL_PutAudioStreamData(stream, output, static_cast<int>(requested * sizeof(int16_t)));
    m_space_available.notify_all();
}

void SoundQueue::SDLAudioStreamCallback(void* userdata, SDL_AudioStream* stream,
                                        int additional_amount, int total_amount)
{
    (void)total_amount;
    auto* self = static_cast<SoundQueue*>(userdata);
    self->FillSDLAudioStream(stream, additional_amount);
}

#endif
