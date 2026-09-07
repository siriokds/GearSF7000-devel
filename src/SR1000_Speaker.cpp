#include "SR1000_Speaker.h"
#include "audio/Blip_Buffer.h"

#include <algorithm>
#include <cmath>

namespace
{
// Monitor gain only. It is applied to the analog signal from the virtual tape
// head, not to the thresholded level read by the PPI.
// A recorder's own speaker is intentionally quieter than the computer's PSG.
// -12 dB from the previous monitor calibration leaves practical headroom for
// the PSG and drive effects (9000 * 0.251188643 = about 2261).
constexpr float TAPE_MONITOR_AMPLITUDE = 2261.0f;
constexpr float DC_CUTOFF_HZ = 60.0f;
constexpr float PI = 3.14159265358979323846f;
constexpr int MAX_SYNTH_ELAPSED_CYCLES = GC_MASTER_CLOCK_NTSC * 2;
// A logical FSK bit is thousands of Z80 cycles long. Events closer than this
// are control chatter, never distinct cassette symbols, so coalesce them.
constexpr int MIN_SYNTH_EVENT_GAP_CYCLES = 64;

float OnePoleCoefficient(float frequencyHz)
{
    return 1.0f - std::exp(-2.0f * PI * frequencyHz / GC_AUDIO_SAMPLE_RATE);
}

double BlipAlignedCyclesPerSample(int clockRate)
{
    // The PSG is the master producer. Match Blip_Buffer's fixed-point clock
    // conversion exactly, otherwise an exact 44.1 kHz cassette clock runs
    // about 22 frames/s ahead and creates 60 Hz phase modulation at mixing.
    constexpr double timeUnit = static_cast<double>(1ULL << BLIP_BUFFER_ACCURACY);
    const double factor = std::floor(
        (static_cast<double>(GC_AUDIO_SAMPLE_RATE) / clockRate) * timeUnit + 0.5);
    return timeUnit / factor;
}
}

SR1000Speaker::SR1000Speaker()
    : m_InputSample(0.0f)
    , m_InputIsSynthesized(false)
    , m_InputFrequencyHz(0.0f)
    , m_SynthPhase(0.0)
    , m_SynthFrequencyHz(0.0f)
    , m_SynthEventCount(0)
    , m_SynthCoalescedEvents(0)
    , m_SynthQueueOverflows(0)
    , m_SynthTimestampRebases(0)
    , m_SynthInvalidInputs(0)
    , m_PreviousInput(0.0f)
    , m_DcEstimate(0.0f)
    , m_DcCoefficient(OnePoleCoefficient(DC_CUTOFF_HZ))
    , m_iCyclesPerSample(BlipAlignedCyclesPerSample(3579545))
    , m_SampleCycleRemainder(0.0)
    , m_iBufferIndex(0)
    , m_ElapsedCycles(0)
    , m_iClockRate(3579545)
    , m_MeterPolarity(0)
    , m_MeterCrossings(0)
    , m_MeterFrames(0)
    , m_MeasuredFrequencyHz(0.0f)
{
    memset(m_Buffer, 0, sizeof(m_Buffer));
}

SR1000Speaker::~SR1000Speaker()
{
}

void SR1000Speaker::Init(int clockRate)
{
    Reset(clockRate);
}

void SR1000Speaker::Reset(int clockRate)
{
    const int safeClockRate = std::clamp(clockRate, 1, GC_MASTER_CLOCK_NTSC * 2);
    const bool clockRateWasClamped = safeClockRate != clockRate;
    m_iClockRate = safeClockRate;
    m_iCyclesPerSample = BlipAlignedCyclesPerSample(m_iClockRate);
    m_InputSample = 0.0f;
    m_InputIsSynthesized = false;
    m_InputFrequencyHz = 0.0f;
    m_SynthPhase = 0.0;
    m_SynthFrequencyHz = 0.0f;
    m_SynthEventCount = 0;
    m_SynthCoalescedEvents = 0;
    m_SynthQueueOverflows = 0;
    m_SynthTimestampRebases = 0;
    m_SynthInvalidInputs = 0;
    if (clockRateWasClamped)
        ++m_SynthInvalidInputs;
    m_PreviousInput = 0.0f;
    m_SampleCycleRemainder = 0.0;
    m_iBufferIndex = 0;
    m_ElapsedCycles = 0;
    m_DcEstimate = 0.0f;
    m_MeterPolarity = 0;
    m_MeterCrossings = 0;
    m_MeterFrames = 0;
    m_MeasuredFrequencyHz = 0.0f;
    memset(m_Buffer, 0, sizeof(m_Buffer));
}

std::string SR1000Speaker::GetInfo()
{
    return "Tape-head monitor";
}

SR1000Speaker::SafetyInfo SR1000Speaker::GetSafetyInfo() const
{
    return { m_SynthEventCount, m_SynthCoalescedEvents,
             m_SynthQueueOverflows, m_SynthTimestampRebases,
             m_SynthInvalidInputs };
}

void SR1000Speaker::WriteSample(float sample, bool synthesized, float frequencyHz)
{
    // Never let a malformed tape/image state turn into a NaN sample or phase.
    // The PPI path is independent; this only makes the monitor fail silent.
    if (!std::isfinite(sample))
    {
        sample = 0.0f;
        ++m_SynthInvalidInputs;
    }
    if (!std::isfinite(frequencyHz) || frequencyHz < 0.0f)
    {
        frequencyHz = 0.0f;
        ++m_SynthInvalidInputs;
    }
    else if (frequencyHz > GC_AUDIO_SAMPLE_RATE * 0.5f)
    {
        frequencyHz = GC_AUDIO_SAMPLE_RATE * 0.5f;
        ++m_SynthInvalidInputs;
    }

    if (sample == m_InputSample && synthesized == m_InputIsSynthesized
        && frequencyHz == m_InputFrequencyHz)
        return;

    if (synthesized)
    {
        // Timestamp the *change*, but do not produce a sample here.  The
        // bounded event queue is consumed at EndFrame by the audio clock.
        const bool frequencyChanged = !m_InputIsSynthesized
            || frequencyHz != m_InputFrequencyHz;
        m_iBufferIndex = 0;
        m_SampleCycleRemainder = 0.0;
        m_InputSample = sample;
        m_InputIsSynthesized = true;
        m_InputFrequencyHz = frequencyHz;
        // The PPI level changes on every FSK half-wave, but the monitor owns
        // its oscillator phase. Re-enqueueing an unchanged frequency would
        // only fill the queue; it would not change the audio.
        if (!frequencyChanged)
            return;

        int timestamp = std::min(m_ElapsedCycles, MAX_SYNTH_ELAPSED_CYCLES);
        if (m_SynthEventCount > 0
            && timestamp < m_SynthEvents[m_SynthEventCount - 1].clockCycles)
        {
            timestamp = m_SynthEvents[m_SynthEventCount - 1].clockCycles;
            ++m_SynthTimestampRebases;
        }
        if (m_SynthEventCount > 0
            && timestamp - m_SynthEvents[m_SynthEventCount - 1].clockCycles
                < MIN_SYNTH_EVENT_GAP_CYCLES)
        {
            m_SynthEvents[m_SynthEventCount - 1].frequencyHz = frequencyHz;
            ++m_SynthCoalescedEvents;
        }
        else if (m_SynthEventCount < static_cast<int>(m_SynthEvents.size()))
        {
            m_SynthEvents[m_SynthEventCount++] = { timestamp, frequencyHz };
        }
        else
        {
            // This cannot occur with FSK (less than 100 edges/frame), but
            // retain the newest state instead of allocating or growing.
            m_SynthEvents.back().frequencyHz = frequencyHz;
            ++m_SynthQueueOverflows;
        }
        return;
    }

    if (m_InputIsSynthesized)
    {
        m_iBufferIndex = 0;
        m_ElapsedCycles = 0;
        m_SampleCycleRemainder = 0.0;
        m_SynthEventCount = 0;
        m_SynthFrequencyHz = 0.0f;
    }

    // Raw WAV tape uses the old, cycle-timed sampling path.
    Sync();
    m_InputSample = sample;
    m_InputIsSynthesized = false;
    m_InputFrequencyHz = frequencyHz;
}

void SR1000Speaker::Tick(unsigned int clockCycles)
{
    // Keep arithmetic bounded even after a debugger pause, a corrupt state or
    // an unexpected caller. The monitor will rebase at EndFrame instead of
    // overflowing its timestamp domain.
    const int elapsedCycles = std::clamp(m_ElapsedCycles, 0, MAX_SYNTH_ELAPSED_CYCLES);
    if (elapsedCycles != m_ElapsedCycles)
    {
        m_ElapsedCycles = elapsedCycles;
        ++m_SynthTimestampRebases;
    }
    if (clockCycles > static_cast<unsigned int>(MAX_SYNTH_ELAPSED_CYCLES - m_ElapsedCycles))
    {
        m_ElapsedCycles = MAX_SYNTH_ELAPSED_CYCLES;
        ++m_SynthTimestampRebases;
    }
    else
    {
        m_ElapsedCycles += static_cast<int>(clockCycles);
    }
}

void SR1000Speaker::Sync()
{
    const double totalCycles = m_SampleCycleRemainder + m_ElapsedCycles;
    const int samplesToGenerate = static_cast<int>(totalCycles / m_iCyclesPerSample);
    m_SampleCycleRemainder = totalCycles - samplesToGenerate * m_iCyclesPerSample;
    m_ElapsedCycles = 0;

    const int availableFrames = (GC_AUDIO_BUFFER_SIZE - m_iBufferIndex) / 2;
    const int framesToWrite = std::min(samplesToGenerate, availableFrames);
    if (framesToWrite != samplesToGenerate)
    {
        Log("SR1000 speaker audio buffer overflow");
    }

    for (int i = 0; i < framesToWrite; ++i)
    {
        // Diagnostic baseline: the virtual recorder speaker receives the
        // head level directly.  For .bit this is a deliberately dry square
        // wave (+/- 1); no interpolation, DC removal or speaker model may
        // modulate its pilot frequency.
        const float monitorSample = std::clamp(
            m_InputSample * TAPE_MONITOR_AMPLITUDE, -32768.0f, 32767.0f);

#if 0
        // Previous monitor path, kept here temporarily for A/B comparison.
        // It is intentionally disabled while validating the bit-tape timing:
        // its stateful average and DC remover are not needed to make a tape
        // data signal audible and can conceal a timing defect with filtering.
        const float input = m_InputSample * TAPE_MONITOR_AMPLITUDE;
        const float tapeBand = 0.50f * (m_PreviousInput + input);
        m_PreviousInput = input;
        m_DcEstimate += m_DcCoefficient * (tapeBand - m_DcEstimate);
        const float dcFree = tapeBand - m_DcEstimate;
#endif

        constexpr float meterThreshold = TAPE_MONITOR_AMPLITUDE * 0.20f;
        int polarity = 0;
        if (monitorSample > meterThreshold)
            polarity = 1;
        else if (monitorSample < -meterThreshold)
            polarity = -1;
        if (polarity != 0)
        {
            if (m_MeterPolarity != 0 && polarity != m_MeterPolarity)
                ++m_MeterCrossings;
            m_MeterPolarity = polarity;
        }
        ++m_MeterFrames;
        if (m_MeterFrames >= GC_AUDIO_SAMPLE_RATE / 4)
        {
            m_MeasuredFrequencyHz = static_cast<float>(m_MeterCrossings)
                * static_cast<float>(GC_AUDIO_SAMPLE_RATE)
                / (2.0f * static_cast<float>(m_MeterFrames));
            m_MeterCrossings = 0;
            m_MeterFrames = 0;
        }

        const s16 output = static_cast<s16>(monitorSample);
        m_Buffer[m_iBufferIndex++] = output;
        m_Buffer[m_iBufferIndex++] = output;
    }
}

int SR1000Speaker::RenderSynthesized(s16* pSampleBuffer, int targetSampleCount,
                                     int frameClockCycles)
{
    const int samplesToWrite = std::max(0,
        std::min(targetSampleCount, GC_AUDIO_BUFFER_SIZE)) & ~1;
    const int framesToWrite = samplesToWrite / 2;
    frameClockCycles = std::clamp(frameClockCycles, 0, MAX_SYNTH_ELAPSED_CYCLES);
    if (!std::isfinite(m_SynthPhase))
    {
        m_SynthPhase = 0.0;
        ++m_SynthInvalidInputs;
    }
    if (!std::isfinite(m_SynthFrequencyHz) || m_SynthFrequencyHz < 0.0f)
    {
        m_SynthFrequencyHz = 0.0f;
        ++m_SynthInvalidInputs;
    }
    int nextEvent = 0;

    for (int frame = 0, sample = 0; frame < framesToWrite; ++frame)
    {
        const int wholeCyclesPerFrame = frameClockCycles / framesToWrite;
        const int remainingCycles = frameClockCycles % framesToWrite;
        const int audioClockPosition = wholeCyclesPerFrame * (frame + 1)
            + (remainingCycles * (frame + 1)) / framesToWrite;
        while (nextEvent < m_SynthEventCount
               && m_SynthEvents[nextEvent].clockCycles <= audioClockPosition)
        {
            m_SynthFrequencyHz = m_SynthEvents[nextEvent++].frequencyHz;
        }

        const s16 output = m_SynthFrequencyHz > 0.0f
            ? (m_SynthPhase < 0.5 ? static_cast<s16>(TAPE_MONITOR_AMPLITUDE)
                                  : static_cast<s16>(-TAPE_MONITOR_AMPLITUDE))
            : 0;
        const double phaseIncrement = std::clamp(
            static_cast<double>(m_SynthFrequencyHz) / GC_AUDIO_SAMPLE_RATE, 0.0, 0.5);
        m_SynthPhase += phaseIncrement;
        if (m_SynthPhase >= 1.0)
            m_SynthPhase -= 1.0;
        m_Buffer[sample++] = output;
        m_Buffer[sample++] = output;

        const int polarity = output > 0 ? 1 : (output < 0 ? -1 : 0);
        if (polarity != 0)
        {
            if (m_MeterPolarity != 0 && polarity != m_MeterPolarity)
                ++m_MeterCrossings;
            m_MeterPolarity = polarity;
        }
        ++m_MeterFrames;
        if (m_MeterFrames >= GC_AUDIO_SAMPLE_RATE / 4)
        {
            m_MeasuredFrequencyHz = static_cast<float>(m_MeterCrossings)
                * static_cast<float>(GC_AUDIO_SAMPLE_RATE)
                / (2.0f * static_cast<float>(m_MeterFrames));
            m_MeterCrossings = 0;
            m_MeterFrames = 0;
        }
    }

    if (IsValidPointer(pSampleBuffer))
        memcpy(pSampleBuffer, m_Buffer, samplesToWrite * sizeof(m_Buffer[0]));
    int retainedEvents = 0;
    for (int event = nextEvent; event < m_SynthEventCount; ++event)
    {
        // Normally every event lies inside this frame.  Retaining the tail is
        // essential when a frame supplies no samples: dropping it would stamp
        // a 50/60 Hz discontinuity onto the tape monitor.
        m_SynthEvents[retainedEvents] = m_SynthEvents[event];
        m_SynthEvents[retainedEvents].clockCycles = std::max(
            0, m_SynthEvents[retainedEvents].clockCycles - frameClockCycles);
        ++retainedEvents;
    }
    m_SynthEventCount = retainedEvents;
    return samplesToWrite;
}

int SR1000Speaker::EndFrame(s16* pSampleBuffer, int targetSampleCount)
{
    // The synthetic .bit monitor is not the PPI input: its audio cadence is
    // supplied by the master mixer, never reconstructed from CPU cycles.
    if (m_InputIsSynthesized && targetSampleCount >= 0)
    {
        m_iBufferIndex = 0;
        const int frameClockCycles = m_ElapsedCycles;
        m_ElapsedCycles = 0;
        m_SampleCycleRemainder = 0.0;
        const int cappedTargetSampleCount = std::clamp(
            targetSampleCount, 0, GC_AUDIO_BUFFER_SIZE) & ~1;
        const int framesToRender = cappedTargetSampleCount / 2;
        const int expectedCycles = framesToRender > 0
            ? std::max(1, static_cast<int>(
                (static_cast<int64_t>(framesToRender) * m_iClockRate)
                / GC_AUDIO_SAMPLE_RATE))
            : 0;
        if (framesToRender == 0 || frameClockCycles > expectedCycles * 8)
        {
            // A debugger pause or a broken caller must not compress seconds of
            // timestamped FSK into one audio block.  Rebase to the newest
            // known state; the PPI/tape timing is intentionally untouched.
            m_SynthEventCount = 0;
            m_SynthFrequencyHz = m_InputFrequencyHz;
            ++m_SynthTimestampRebases;
            if (framesToRender == 0)
                return 0;
            return RenderSynthesized(pSampleBuffer, cappedTargetSampleCount, expectedCycles);
        }
        return RenderSynthesized(pSampleBuffer, cappedTargetSampleCount, frameClockCycles);
    }

    Sync();

    if (IsValidPointer(pSampleBuffer))
        memcpy(pSampleBuffer, m_Buffer, m_iBufferIndex * sizeof(m_Buffer[0]));

    const int sampleCount = m_iBufferIndex;
    m_iBufferIndex = 0;
    return sampleCount;
}

void SR1000Speaker::SaveState(std::ostream& stream)
{
    (void)stream;
}

void SR1000Speaker::LoadState(std::istream& stream)
{
    (void)stream;
}
