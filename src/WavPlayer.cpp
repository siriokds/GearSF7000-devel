#include "WavPlayer.h"
#include "audio/Blip_Buffer.h"
#include <cmath>
#include <cstring>

namespace
{
double BlipAlignedCyclesPerSample(int clockRate)
{
    constexpr double timeUnit = static_cast<double>(1ULL << BLIP_BUFFER_ACCURACY);
    const double factor = std::floor(
        (static_cast<double>(GC_AUDIO_SAMPLE_RATE) / clockRate) * timeUnit + 0.5);
    return timeUnit / factor;
}
}


// Constructor for WavPlayer
WavPlayer::WavPlayer() : Count(0)
{
    Init(3579545);

}

// Destructor for WavPlayer
//WavPlayer::~WavPlayer() 
//{
//}


void WavPlayer::Init(int masterClock)
{
    for (int i = 0; i < WAVPLAYER_CHANS_NUM; i++)
        Channels[i].Init(GC_AUDIO_SAMPLE_RATE);

    memset(m_Buffer, 0, sizeof(m_Buffer));
    Reset(masterClock);
}

void WavPlayer::Reset(int masterClock)
{
    m_MasterSampleRate = masterClock;
    m_ElapsedCycles = 0;
    m_iBufferIndex = 0;
    m_iCyclesPerSample = BlipAlignedCyclesPerSample(m_MasterSampleRate);
    memset(m_Buffer, 0, sizeof(m_Buffer));

    for (int i = 0; i < WAVPLAYER_CHANS_NUM; i++)
        Channels[i].Reset(GC_AUDIO_SAMPLE_RATE);
}



bool WavPlayer::SetChannel(int index, WavSample* pSample)
{
    if (index < 0 || index >= WAVPLAYER_CHANS_NUM) return false;
    if (pSample == nullptr) return false;

    Channels[index].SetChannelSample(index, pSample);

    Count++;

    return true;
}


// Get a sample from the list by index
WavChannel* WavPlayer::GetChannelByName(char* sampleName)
{
    for (int i = 0; i < Count; i++)
    {
        //WavSample* wavSample = Channels[i].GetWavSample();

        if (Channels[i].sampleData != nullptr && Channels[i].sampleName == sampleName)
        {
            return &Channels[i];
        }
    }

    return nullptr;
}

void WavPlayer::Tick(int elapsedCycles)
{
    m_ElapsedCycles += static_cast<double>(elapsedCycles);
}


void WavPlayer::Sync(void)
{
    // Calcola il numero di campioni da elaborare in base ai cicli accumulati
    int samplesToProcess = static_cast<int>(m_ElapsedCycles / m_iCyclesPerSample);
    m_ElapsedCycles -= samplesToProcess * m_iCyclesPerSample; // Rimuovi i cicli elaborati

    for (int i = 0; i < samplesToProcess; i++) 
    {
        float sample = 0;

        for (int chn = 0; chn < WAVPLAYER_CHANS_NUM; chn++)
        {
            sample += Channels[chn].GetNextSample();
        }

        // Scrive il campione nel buffer
        m_Buffer[m_iBufferIndex++] = sample;
        m_Buffer[m_iBufferIndex++] = sample;

        if (m_iBufferIndex >= GC_AUDIO_BUFFER_SIZE) {
            m_iBufferIndex = 0;
        }
    }
}

int WavPlayer::EndFrame(float* pSampleBuffer)
{
    Sync();

    int ret = 0;

    if (IsValidPointer(pSampleBuffer))
    {
        ret = m_iBufferIndex;

        for (int i = 0; i < m_iBufferIndex; i++)
        {
            pSampleBuffer[i] = m_Buffer[i];
        }
    }

    m_iBufferIndex = 0;

    return ret;

}
