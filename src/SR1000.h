#pragma once
#include "IDebugEventProvider.h"
#include "TapeRecorder.h"

class Audio;
class Memory;
struct SR1000DebugConfiguration {
    std::string key;
    std::string parentKey;
    std::string name;
};

// Sega adapter: source/bit formats, legacy motor policy, monitor and debug route.
class SR1000 : public TapeRecorder, public IDebugEventProvider
{
public:
    SR1000(Audio* audio, Memory* memory = nullptr);
    SR1000(Audio* audio, Memory* memory, SR1000DebugConfiguration debug);
    DebugDeviceDefinition DescribeDebugDevice() const override;
    bool LoadTape(std::string filename) override;
    bool LoadBitTape(std::string filename) { return LoadTape(std::move(filename)); }
private:
    SR1000DebugConfiguration m_debug;
};
