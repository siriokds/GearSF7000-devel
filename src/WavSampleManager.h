#ifndef WAVSAMPLEMANAGER_H
#define WAVSAMPLEMANAGER_H

#include "WavSample.h"
#include "WavChannel.h"
#include <vector>

#define WAVSAMPLEMANAGER_SAMPLES_NUM    16

class WavSampleManager {
public:
    int Count;

private:

public:
    WavSample* samples[WAVSAMPLEMANAGER_SAMPLES_NUM];  // Container for loaded samples
    
    WavSampleManager();
    ~WavSampleManager();

    int LoadSample(const char* samplename, const char* filename);
    int LoadSampleFromBuffer(const char* samplename, const unsigned char* wavSampleData, const int wavSampleLength);
    bool SaveSample(int index, const char* filename);
    WavSample* GetSampleByIndex(int index);
    WavSample* GetSampleByName(char* sampleName);
    size_t GetSampleCount() const;

    int LoadSample(WavSample* pSample, const char* samplename, const char* filename);
    //int LoadSample(WavChannel* channel, const char* samplename, const char* filename);
};

#endif // WAVSAMPLEMANAGER_H
