#include "WavSampleManager.h"
#include "WavChannel.h"
#include "WAVUtils.h"
#include <cstring>
#include <iostream>


// Constructor
WavSampleManager::WavSampleManager() : Count(0)
{
    for (int i = 0; i < WAVSAMPLEMANAGER_SAMPLES_NUM; i++)
        samples[i] = nullptr;
}

// Destructor
WavSampleManager::~WavSampleManager() {
    for (int i = 0; i < WAVSAMPLEMANAGER_SAMPLES_NUM; i++)
    {
        if (samples[i] != nullptr)
        {
            delete samples[i];
            samples[i] = nullptr;
        }
    }

}

// Load a sample from a WAV file
bool WavSampleManager::SaveSample(int index, const char* filename) 
{
    WavSample* wavSample = this->GetSampleByIndex(index);

    if (!wavSample->sampleData || wavSample->sampleSize == 0) {
#ifdef _DEBUG
        std::cerr << "No data to save.\n";
#endif
        return false;
    }

    if (!filename || std::strlen(filename) == 0) {
#ifdef _DEBUG
        std::cerr << "Invalid filename.\n";
#endif
        return false;
    }

    std::ofstream outFile(filename, std::ios::binary);
    if (!outFile) {
#ifdef _DEBUG
        std::cerr << "Failed to open file for writing: " << filename << "\n";
#endif
        return false;
    }

    // Write sample data
    outFile.write(reinterpret_cast<const char*>(wavSample->sampleData), wavSample->sampleSize * sizeof(int16_t));

    if (!outFile) {
#ifdef _DEBUG
        std::cerr << "Error occurred while writing to file.\n";
#endif
        return false;
    }

    outFile.close();
    return true;
}



int WavSampleManager::LoadSampleFromBuffer(const char* samplename, const unsigned char* wavSampleData, const int wavSampleLength)
{
    WAVUtils::WAVStruct wavInfo = {};
    int16_t* audioData = nullptr;

    // Check the WAV file
    int result = WAVUtils::checkWavFromBuffer(wavSampleData, wavSampleLength, &wavInfo);
    if (result != WAVUtils::SUCCESS) {
#ifdef _DEBUG
        printf("checkWav failed with error code: %d\n", result);
#endif
        return result;
    }

    // Allocate memory for the audio data
    audioData = new int16_t[wavInfo.numSamples];

    // Read the audio data, specifying the left channel only
    result = WAVUtils::readWavFromBuffer(wavSampleData, wavSampleLength, &wavInfo, audioData, WAVUtils::BOTH_CHANNELS);
    if (result != WAVUtils::SUCCESS) {
#ifdef _DEBUG
        printf("readWav failed with error code: %d\n", result);
#endif
        delete[] audioData;
        return result;
    }

    if (Count >= WAVSAMPLEMANAGER_SAMPLES_NUM)
    {
        delete[] audioData;
        return WAVUtils::MAX_SAMPLES_REACHED;
    }


    // Add the loaded sample to the manager
    samples[Count] = new WavSample((char*)samplename);
    samples[Count]->sampleRate = wavInfo.sampleRate;
    samples[Count]->sampleSize = wavInfo.numSamples;
    samples[Count]->path = "memwave";
    samples[Count]->sampleData = audioData;
    Count++;

    return WAVUtils::SUCCESS;

}

// Load a sample from a WAV file
int WavSampleManager::LoadSample(const char* samplename, const char* filename) {
    WAVUtils::WAVStruct wavInfo = {};
    int16_t* audioData = nullptr;

    // Check the WAV file
    int result = WAVUtils::checkWav(filename, &wavInfo);
    if (result != WAVUtils::SUCCESS) {
#ifdef _DEBUG
        printf("checkWav failed with error code: %d\n", result);
#endif
        return result;
    }

    // Allocate memory for the audio data
    audioData = new int16_t[wavInfo.numSamples];

    // Read the audio data, specifying the left channel only
    result = WAVUtils::readWav(filename, &wavInfo, audioData, WAVUtils::BOTH_CHANNELS);
    if (result != WAVUtils::SUCCESS) {
#ifdef _DEBUG
        printf("readWav failed with error code: %d\n", result);
#endif
        delete[] audioData;
        return result;
    }

    if (Count >= WAVSAMPLEMANAGER_SAMPLES_NUM)
    {
        delete[] audioData;
        return WAVUtils::MAX_SAMPLES_REACHED;
    }


    // Add the loaded sample to the manager
    samples[Count] = new WavSample((char*)samplename);
    samples[Count]->sampleRate = wavInfo.sampleRate;
    samples[Count]->sampleSize = wavInfo.numSamples;
    samples[Count]->path = filename;
    samples[Count]->sampleData = audioData;
    Count++;

    return WAVUtils::SUCCESS;
}

// Get a sample from the list by index
WavSample* WavSampleManager::GetSampleByIndex(int index) 
{
    if (index < 0 || index >= WAVSAMPLEMANAGER_SAMPLES_NUM) {
        return nullptr;
    }
    return samples[index];
}

// Get a sample from the list by index
WavSample* WavSampleManager::GetSampleByName(char* sampleName) 
{
    for (int i = 0; i < WAVSAMPLEMANAGER_SAMPLES_NUM; i++)
    {
        if (samples[i] != nullptr && samples[i]->name == sampleName)
        {
            return samples[i];
        }
    }

    return nullptr;
}


// Get the number of loaded samples
size_t WavSampleManager::GetSampleCount() const {
    return Count;
}



// Load a sample from a WAV file
int WavSampleManager::LoadSample(WavSample * pSample, const char* samplename, const char* filename) 
{
    WAVUtils::WAVStruct wavInfo = {};
    int16_t* audioData = nullptr;

    // Check the WAV file
    int result = WAVUtils::checkWav(filename, &wavInfo);
    if (result != WAVUtils::SUCCESS) {
#ifdef _DEBUG
        printf("checkWav failed with error code: %d\n", result);
#endif
        return result;
    }

    // Allocate memory for the audio data
    audioData = new int16_t[wavInfo.numSamples];

    // Read the audio data, specifying the left channel only
    result = WAVUtils::readWav(filename, &wavInfo, audioData, WAVUtils::BOTH_CHANNELS);
    if (result != WAVUtils::SUCCESS) {
#ifdef _DEBUG
        printf("readWav failed with error code: %d\n", result);
#endif
        delete[] audioData;
        return result;
    }

    if (Count >= WAVSAMPLEMANAGER_SAMPLES_NUM)
    {
        delete[] audioData;
        return WAVUtils::MAX_SAMPLES_REACHED;
    }


    // Add the loaded sample to the manager
    pSample->sampleRate = wavInfo.sampleRate;
    pSample->sampleSize = wavInfo.numSamples;
    if (pSample->sampleData != nullptr) delete[] pSample->sampleData;
    pSample->sampleData = audioData;
    pSample->name = samplename;
    pSample->path = filename;

    return WAVUtils::SUCCESS;
}


// Load a sample from a WAV file
//int WavSampleManager::LoadSample(WavChannel* channel, const char* samplename, const char* filename)
//{
//    WAVUtils::WAVStruct wavInfo = {};
//    int16_t* audioData = nullptr;
//
//    // Check the WAV file
//    int result = WAVUtils::checkWav(filename, &wavInfo);
//    if (result != WAVUtils::SUCCESS) {
//#ifdef _DEBUG
//        printf("checkWav failed with error code: %d\n", result);
//#endif
//        return result;
//    }
//
//    // Allocate memory for the audio data
//    audioData = new int16_t[wavInfo.numSamples];
//
//    // Read the audio data, specifying the left channel only
//    result = WAVUtils::readWav(filename, &wavInfo, audioData, WAVUtils::BOTH_CHANNELS);
//    if (result != WAVUtils::SUCCESS) {
//#ifdef _DEBUG
//        printf("readWav failed with error code: %d\n", result);
//#endif
//        delete[] audioData;
//        return result;
//    }
//
//    if (Count >= WAVSAMPLEMANAGER_SAMPLES_NUM)
//    {
//        delete[] audioData;
//        return WAVUtils::MAX_SAMPLES_REACHED;
//    }
//
//
//    // Add the loaded sample to the manager
//    channel->sampleRate = wavInfo.sampleRate;
//    channel->sampleSize = wavInfo.numSamples;
//    
//    if (channel->sampleData != nullptr) delete[] channel->sampleData;
//    channel->sampleData = new int16_t[wavInfo.numSamples * sizeof(int16_t)];
//    std::memcpy(channel->sampleData, audioData, wavInfo.numSamples * sizeof(int16_t));
//
//    channel->sampleName = samplename;
//    channel->samplePath = filename;
//
//    delete[] audioData;
//
//    return WAVUtils::SUCCESS;
//}