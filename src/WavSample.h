#ifndef WAVSAMPLE_H
#define WAVSAMPLE_H

#include <string>
#include <iostream>
#include <cstdint>

class WavSample {
public:
    int16_t* sampleData;  // Data for the sample (mono)
    int sampleRate;       // Sample rate of the sample
    size_t sampleSize;    // Total number of samples
    std::string name;
    std::string path;

    WavSample(char *samplename);
    ~WavSample();

    // Prevent copying and assignment
    WavSample(const WavSample&) = delete;
    WavSample& operator=(const WavSample&) = delete;

    // Move constructor
    WavSample(WavSample&& other) noexcept;
    WavSample& operator=(WavSample&& other) noexcept;
};

#endif // WAVSAMPLE_H
