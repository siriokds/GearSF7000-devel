#include "WavChannel.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr float kAttackMilliseconds = 2.0f;
}

WavChannel::WavChannel() : sampleData(nullptr)
{
    Init(GC_AUDIO_SAMPLE_RATE);
}

// Destructor for WavChannel
WavChannel::~WavChannel() 
{
    DeleteSampleData();
}

void WavChannel::DeleteSampleData()
{
    if (sampleData != nullptr)
        delete[] sampleData;

    sampleData = nullptr;
}

void WavChannel::Init(int sampleRate)
{
    Index = -1;
    //m_pWavSample = nullptr;

    DeleteSampleData();
    //sampleData = nullptr;       // Data for the sample (mono)

    this->sampleRate = sampleRate;
    sampleSize = 0;             // Total number of samples
    sampleName = "";
    samplePath = "";

    Reset(GC_AUDIO_SAMPLE_RATE);
}


void WavChannel::SetChannelSample(int index, WavSample* pWavSample)
{
    Index = index;

    DeleteSampleData();
    //if (sampleData != nullptr) delete [] sampleData;

    const size_t sampleCount = pWavSample->sampleSize;
    sampleData = new int16_t[sampleCount];
    std::memcpy(sampleData, pWavSample->sampleData, sampleCount * sizeof(int16_t));

    sampleRate = pWavSample->sampleRate;
    sampleSize = pWavSample->sampleSize;

    sampleName = pWavSample->name;
    samplePath = pWavSample->path;


    //m_pWavSample = pWavSample;
}


// Reset all mixer variables while keeping the loaded samples
void WavChannel::Reset(int masterSampleRate)
{
    m_MasterSampleRate = masterSampleRate;

    m_Volume = 0.0f;
    m_TargetVolume = 0.0f;
    m_VolumeStep = 0.0f;
    m_PitchDecrement = 0.0f;
    m_BasePositionIncr = 1.0f;
    m_ChannelPositionIncr = 1.0f;
    m_Playing = false;
    m_PlayMode = PlayMode::ONESHOT;
    m_ChannelPosition = 0;
    m_LoopStart = m_ChannelPosition;
    m_LoopEnd = 0;
    m_LoopRep = 1;
}


//WavSample* WavChannel::GetWavSample()
//{
//    return m_pWavSample;
//}

// Set a sample on a specific channel
void WavChannel::SetupChannel(PlayMode mode, float volume) 
{
    m_Volume = 0.0f;
    m_TargetVolume = volume;
    m_VolumeStep = 0.0f;

    m_ChannelPosition = 0;
    m_BasePositionIncr = (sampleRate > 0 && m_MasterSampleRate > 0)
        ? static_cast<float>(sampleRate) / static_cast<float>(m_MasterSampleRate)
        : 1.0f;
    m_ChannelPositionIncr = m_BasePositionIncr;
    m_PitchDecrement = 0;

    m_PlayMode = mode;
    m_LoopStart = m_ChannelPosition;
    m_LoopEnd = sampleSize - 1;
    m_LoopRep = 1;

    m_PlayMode_r = m_PlayMode;
    m_LoopStart_r = m_LoopStart;
    m_LoopEnd_r = m_LoopEnd;

    //float sampleRate = static_cast<float>(sampleRate);
    m_Playing = false;
}

// Set a sample on a specific channel
void WavChannel::SetupChannelMs(float loopStartMs, float loopEndMs, int loopRep, float volume)
{
    m_PlayMode = PlayMode::REPS;
    m_Volume = volume;

    const float freq = GC_AUDIO_SAMPLE_RATE / 1000.0f;

    SetupChannel(loopStartMs * freq, loopEndMs * freq, loopRep, volume);

}

// Set a sample on a specific channel
void WavChannel::SetupChannel(int loopStart, int loopEnd, int loopRep, float volume)
{
    m_Volume = 0.0f;
    m_TargetVolume = volume;
    m_VolumeStep = 0.0f;
    m_ChannelPosition = loopStart;
    m_BasePositionIncr = (sampleRate > 0 && m_MasterSampleRate > 0)
        ? static_cast<float>(sampleRate) / static_cast<float>(m_MasterSampleRate)
        : 1.0f;
    m_ChannelPositionIncr = m_BasePositionIncr;
    m_PitchDecrement = 0;

    m_PlayMode = PlayMode::REPS;
    m_LoopStart = m_ChannelPosition;
    m_LoopEnd = loopEnd;
    m_LoopRep = loopRep;

    m_PlayMode_r = m_PlayMode;
    m_LoopStart_r = m_LoopStart;
    m_LoopEnd_r = m_LoopEnd;


    //float sampleRate = static_cast<float>(sampleRate);
    m_Playing = false;
}

// Start playback of the sample on the specified channel
void WavChannel::PlayChannel() 
{
    if (sampleData == nullptr) return;

    m_ChannelPosition = m_LoopStart;
    m_ChannelPositionIncr = m_BasePositionIncr;
    m_PitchDecrement = 0.0f;
    m_Volume = 0.0f;
    const int attackSamples = std::max(1, static_cast<int>(
        std::lround(m_MasterSampleRate * kAttackMilliseconds / 1000.0f)));
    m_VolumeStep = m_TargetVolume / static_cast<float>(attackSamples);
    m_Playing = true;
}

// Stop a specific channel with fade-out
void WavChannel::StopChannel() 
{
    m_Playing = false;
    m_ChannelPosition = m_LoopStart;
    m_Volume = 0.0f;
    m_VolumeStep = 0.0f;
    m_PitchDecrement = 0.0f;
}

void WavChannel::StopChannelSlow(int durationMs)
{
    if (!m_Playing) return;  // Se il canale non sta suonando, non fare nulla
    if (m_Volume <= 0.0f) {
        StopChannel();
        return;
    }

    const float freq = GC_AUDIO_SAMPLE_RATE / 1000.0f;  // Conversione da millisecondi a campioni
    const int totalSteps = std::max(1, static_cast<int>(durationMs * freq));
    m_TargetVolume = 0.0f;
    m_VolumeStep = -m_Volume / static_cast<float>(totalSteps);

    // Il pitch-down è intenzionale: simula l'inerzia del motore del drive.
    // Manteniamo il rapporto storico (circa -25% in 150 ms) ma lo rendiamo
    // indipendente dalla rampa di gain.
    m_PitchDecrement = (m_ChannelPositionIncr * 0.25f) / static_cast<float>(totalSteps);

}

// Check if a specific channel is currently playing
bool WavChannel::IsChannelPlaying() const {
    return m_Playing;
}





float WavChannel::GetNextSample()
{
    if (!m_Playing || sampleData == nullptr || sampleSize == 0) return 0.0f;

    const int index = static_cast<int>(m_ChannelPosition);
    const float fraction = m_ChannelPosition - static_cast<float>(index);
    const auto sampleAt = [this](int position) -> float {
        if (position >= 0 && position < static_cast<int>(sampleSize))
            return static_cast<float>(sampleData[position]);
        return 0.0f;
    };

    int nextIndex = index + 1;
    if ((m_PlayMode_r == PlayMode::LOOP || m_PlayMode_r == PlayMode::REPS)
        && nextIndex > m_LoopEnd_r)
    {
        nextIndex = m_LoopStart_r;
    }

    // La posizione resta frazionaria perché il rate WAV può differire dal
    // rate del mixer. L'interpolazione lineare elimina gli scalini del reader.
    const float source = sampleAt(index)
        + (sampleAt(nextIndex) - sampleAt(index)) * fraction;
    const float sample = source * m_Volume;

    m_ChannelPosition += m_ChannelPositionIncr;
    if (m_ChannelPosition > m_LoopEnd_r)
    {
        if (m_PlayMode_r == PlayMode::ONESHOT) {
            m_Playing = false;
            m_ChannelPosition = static_cast<float>(m_LoopEnd_r);
        } else if (m_PlayMode_r == PlayMode::LOOP) {
            const float loopLength = static_cast<float>(m_LoopEnd_r - m_LoopStart_r + 1);
            m_ChannelPosition = static_cast<float>(m_LoopStart_r)
                + std::fmod(m_ChannelPosition - static_cast<float>(m_LoopStart_r), loopLength);
        } else if (m_PlayMode_r == PlayMode::REPS) {
            if (--m_LoopRep < 2)
                m_PlayMode_r = PlayMode::ONESHOT;
            m_ChannelPosition = static_cast<float>(m_LoopStart_r);
        }
    }

    m_Volume += m_VolumeStep;
    if (m_VolumeStep > 0.0f && m_Volume >= m_TargetVolume) {
        m_Volume = m_TargetVolume;
        m_VolumeStep = 0.0f;
    } else if (m_VolumeStep < 0.0f && m_Volume <= 0.0f) {
        m_Volume = 0.0f;
        m_VolumeStep = 0.0f;
        m_Playing = false;
        m_ChannelPosition = static_cast<float>(m_LoopStart);
    }

    if (m_PitchDecrement > 0.0f) {
        m_ChannelPositionIncr -= m_PitchDecrement;
        if (m_ChannelPositionIncr <= 0.0f) {
            m_ChannelPositionIncr = m_BasePositionIncr;
            m_PitchDecrement = 0.0f;
            m_Playing = false;
            m_ChannelPosition = static_cast<float>(m_LoopStart);
        }
    }

    return m_Playing ? sample : 0.0f;
}
