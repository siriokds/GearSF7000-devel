#ifndef WAVCHANNEL_H
#define WAVCHANNEL_H

#include "WavSampleManager.h"

#include "definitions.h"



class WavChannel 
{
public:
    // Enum to specify play mode
    enum class PlayMode { ONESHOT, LOOP, REPS };
    int Index;

    int16_t* sampleData;  // Data for the sample (mono)
    int sampleRate;       // Sample rate of the sample
    size_t sampleSize;    // Total number of samples
    std::string sampleName;
    std::string samplePath;

private:
    //WavSampleManager* m_SampleManager;  // A pointer to the sample manager

    int m_MasterSampleRate;                           // Master sample rate
    float m_Volume;           // Volume levels for each channel
    bool m_Playing;              // Playback state of each channel
    float m_ChannelPosition;    // Current positions in the sample for each channel (use float)
    float m_ChannelPositionIncr;    // Current positions in the sample for each channel (use float)

    PlayMode m_PlayMode;         // Playback mode of each channel (ONESHOT/LOOP)
    int m_LoopStart;
    int m_LoopEnd;
    int m_LoopRep;

    PlayMode m_PlayMode_r;         // Playback mode of each channel (ONESHOT/LOOP)
    int m_LoopStart_r;
    int m_LoopEnd_r;

    float m_TargetVolume;
    float m_VolumeStep;
    float m_PitchDecrement;
    float m_BasePositionIncr;


    //WavSample* m_pWavSample;

    void DeleteSampleData();

public:
    WavChannel();
    ~WavChannel();

    void SetChannelSample(int index, WavSample* pWavSample);

    void Init(int sampleRate);
    void Reset(int sampleRate);
    //WavSample* GetWavSample();
    
    float GetNextSample();
    void SetupChannel(PlayMode mode, float volume);
    void SetupChannel(int loopStart, int loopEnd, int loopRep, float volume);
    void SetupChannelMs(float loopStartMs, float loopEndMs, int loopRep, float volume);
    void PlayChannel();
    void StopChannel();
    void StopChannelSlow(int millis);
    bool IsChannelPlaying() const;

};

#endif // WAVCHANNEL_H
