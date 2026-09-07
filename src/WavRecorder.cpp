#include "WavRecorder.h"

WavRecorder::WavRecorder() : m_DataSize(0), m_SampleRate(48000) {}

WavRecorder::~WavRecorder() {
    if (m_File.is_open()) Stop();
}

bool WavRecorder::Start(const std::string& filename, int sampleRate) {
    m_File.open(filename, std::ios::binary);
    if (!m_File.is_open()) return false;

    m_SampleRate = sampleRate;
    m_DataSize = 0;

    // Lasciamo spazio per l'header (44 byte) che scriveremo alla fine
    char dummyHeader[44] = { 0 };
    m_File.write(dummyHeader, 44);

    return true;
}

void WavRecorder::WriteSample(int16_t sample) {
    if (m_File.is_open()) {
        m_File.write(reinterpret_cast<const char*>(&sample), sizeof(int16_t));
        m_DataSize += sizeof(int16_t);
    }
}

void WavRecorder::Stop() {
    if (!m_File.is_open()) return;

    // Torna all'inizio per scrivere l'header definitivo
    m_File.seekp(0, std::ios::beg);
    WriteWavHeader();

    m_File.close();
}

void WavRecorder::WriteWavHeader() {
    uint32_t totalFileSize = 36 + m_DataSize;
    uint32_t byteRate = m_SampleRate * 1 * 2; // rate * mono * 16bit/8
    uint16_t blockAlign = 2; // mono * 16bit/8
    uint16_t bitDepth = 16;

    m_File.write("RIFF", 4);
    m_File.write(reinterpret_cast<const char*>(&totalFileSize), 4);
    m_File.write("WAVE", 4);
    m_File.write("fmt ", 4);

    uint32_t fmtSize = 16;
    uint16_t audioFormat = 1; // PCM
    uint16_t channels = 1;    // Mono

    m_File.write(reinterpret_cast<const char*>(&fmtSize), 4);
    m_File.write(reinterpret_cast<const char*>(&audioFormat), 2);
    m_File.write(reinterpret_cast<const char*>(&channels), 2);
    m_File.write(reinterpret_cast<const char*>(&m_SampleRate), 4);
    m_File.write(reinterpret_cast<const char*>(&byteRate), 4);
    m_File.write(reinterpret_cast<const char*>(&blockAlign), 2);
    m_File.write(reinterpret_cast<const char*>(&bitDepth), 2);

    m_File.write("data", 4);
    m_File.write(reinterpret_cast<const char*>(&m_DataSize), 4);
}