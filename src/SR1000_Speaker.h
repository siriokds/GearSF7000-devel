#ifndef SR1000_SPEAKER_H
#define	SR1000_SPEAKER_H

#include "definitions.h"
#include <string.h>
#include <array>


class SR1000Speaker
{
public:
    struct SafetyInfo
    {
        int pendingEvents = 0;
        unsigned int coalescedEvents = 0;
        unsigned int queueOverflows = 0;
        unsigned int timestampRebases = 0;
        unsigned int invalidInputs = 0;
    };

    SR1000Speaker();
    ~SR1000Speaker();
    void Init(int clockRate);
    void Reset(int clockRate);
    void WriteSample(float sample, bool synthesized, float frequencyHz = 0.0f);
    void Tick(unsigned int clockCycles);
    int EndFrame(s16* pSampleBuffer, int targetSampleCount = -1);
    void SaveState(std::ostream& stream);
    void LoadState(std::istream& stream);
    std::string GetInfo();
    float GetMeasuredFrequencyHz() const { return m_MeasuredFrequencyHz; }
    SafetyInfo GetSafetyInfo() const;
private:
    struct SynthEvent
    {
        int clockCycles;
        float frequencyHz;
    };

    void Sync();
    int RenderSynthesized(s16* pSampleBuffer, int targetSampleCount,
                          int frameClockCycles);
private:
    float m_InputSample;
    bool m_InputIsSynthesized;
    float m_InputFrequencyHz;
    double m_SynthPhase;
    float m_SynthFrequencyHz;
    std::array<SynthEvent, 512> m_SynthEvents;
    int m_SynthEventCount;
    unsigned int m_SynthCoalescedEvents;
    unsigned int m_SynthQueueOverflows;
    unsigned int m_SynthTimestampRebases;
    unsigned int m_SynthInvalidInputs;
    float m_PreviousInput;
    float m_DcEstimate;
    float m_DcCoefficient;
    double m_iCyclesPerSample;
    double m_SampleCycleRemainder;
    s16 m_Buffer[GC_AUDIO_BUFFER_SIZE];
    int m_iBufferIndex;
    int m_ElapsedCycles;
    int m_iClockRate;
    int m_MeterPolarity;
    int m_MeterCrossings;
    int m_MeterFrames;
    float m_MeasuredFrequencyHz;

};


#endif	/* SR1000_SPEAKER_H */
