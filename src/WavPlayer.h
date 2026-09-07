#ifndef WAVPLAYER_H
#define WAVPLAYER_H

#include "WavSampleManager.h"
#include "WavChannel.h"
#include <vector>

#include "definitions.h"

#define WAVPLAYER_CHANS_NUM     4     // Number of channels



class WavPlayer {
public:
    WavChannel Channels[WAVPLAYER_CHANS_NUM];     // Container for loaded samples
    int Count;
private:
    float m_Buffer[GC_AUDIO_BUFFER_SIZE];         // Internal audio buffer
    double m_ElapsedCycles;                       // Elapsed clock cycles plus fractional remainder
    int m_MasterSampleRate;                       // Master sample rate
    double m_iCyclesPerSample;                    // Blip-aligned output sample step
    int m_iBufferIndex;                                // Current index in the buffer
public:
    WavPlayer();
    //~WavPlayer();

    void Init(int masterClock);
    void Reset(int masterClock);
    void Tick(int elapsedClocks);
    void Sync(void);
    int  EndFrame(float* pSampleBuffer);

    bool SetChannel(int index, WavSample* pSample);

    WavChannel* GetChannelByName(char* sampleName);

};

#endif // WAVPLAYER_H
