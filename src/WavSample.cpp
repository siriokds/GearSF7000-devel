#include "WavSample.h"
#include <cstring>

// Constructor
WavSample::WavSample(char *samplename) : sampleData(nullptr), sampleRate(0), sampleSize(0), name(samplename) {}

// Destructor
WavSample::~WavSample() {
    if (sampleData) {
        delete[] sampleData;
        sampleData = nullptr;
    }
}

// Move constructor
WavSample::WavSample(WavSample&& other) noexcept
    : sampleData(other.sampleData), sampleRate(other.sampleRate), sampleSize(other.sampleSize), name(other.name) {
    other.sampleData = nullptr;
    other.sampleRate = 0;
    other.sampleSize = 0;
    other.name.clear();
    other.path.clear();
}

// Move assignment operator
WavSample& WavSample::operator=(WavSample&& other) noexcept {
    if (this != &other) {
        delete[] sampleData;

        sampleData = other.sampleData;
        sampleRate = other.sampleRate;
        sampleSize = other.sampleSize;
        name = other.name;
        path = other.path;

        other.sampleData = nullptr;
        other.sampleRate = 0;
        other.sampleSize = 0;
        other.name.clear();
        other.path.clear();
    }
    return *this;
}

