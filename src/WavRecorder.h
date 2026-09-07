#ifndef WAVRECORDER_H
#define WAVRECORDER_H

#include <fstream>
#include <string>
#include <cstdint>
#include "WAVUtils.h"

class WavRecorder {
public:
    WavRecorder();
    ~WavRecorder();

    // Inizia la registrazione
    bool Start(const std::string& filename, int sampleRate = 48000);

    // Aggiunge un singolo campione (Mono, 16-bit)
    void WriteSample(int16_t sample);

    // Chiude il file e scrive l'header finale
    void Stop();

    bool IsRecording() const { return m_File.is_open(); }

private:
    std::ofstream m_File;
    uint32_t m_DataSize; // Dimensione totale dei campioni in byte
    int m_SampleRate;

    void WriteWavHeader();
};

#endif