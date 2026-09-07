#ifndef WAVUTILS_H
#define WAVUTILS_H

#include <cstdint>
#include <fstream>

class WAVUtils {
public:
    // Enum to represent error codes
    enum ErrorCode {
        SUCCESS = 0,              // Operation successful
        FILE_NOT_FOUND,           // File could not be opened
        UNSUPPORTED_BIT_DEPTH,    // Unsupported bit depth (e.g., not 16-bit PCM)
        READ_ERROR,               // Failed to read the expected amount of data
        UNEXPECTED_END_OF_FILE,   // Reached EOF before completing the read
        MAX_SAMPLES_REACHED       // Reached EOS
    };

    // Enum to specify which channel to read
    enum ChannelMode {
        BOTH_CHANNELS = 0,        // Read both channels (default)
        LEFT_CHANNEL_ONLY         // Read only the left channel
    };

    // Struct to store WAV header information
    struct WAVStruct {
        int32_t chunkID;          // Chunk ID (e.g., "RIFF")
        int32_t fileSize;         // File size in bytes
        int32_t riffType;         // RIFF type (e.g., "WAVE")
        int32_t fmtID;            // Format chunk ID (e.g., "fmt ")
        int32_t fmtSize;          // Size of the format chunk
        int16_t fmtCode;          // Audio format code (1 = PCM)
        int16_t channels;         // Number of channels (1 = mono, 2 = stereo)
        int32_t sampleRate;       // Sampling rate (e.g., 48000 Hz)
        int32_t byteRate;         // Byte rate (bytes per second)
        int16_t fmtBlockAlign;    // Block alignment (bytes per sample frame)
        int16_t bitDepth;         // Bits per sample (e.g., 16 for 16-bit audio)
        int32_t dataID;           // Data chunk ID (e.g., "data")
        int32_t dataSize;         // Size of the data chunk in bytes
        int numSamples;           // Number of samples per channel
    };

    // Function to read the WAV header and populate the WAVStruct
    static int checkWav(const char* filename, WAVStruct* wavInfo);
    static int checkWavFromBuffer(const unsigned char* sourceData, const int sourceDataLength, WAVStruct* wavInfo);

    // Function to read audio data
    static int readWav(const char* filename, const WAVStruct* wavInfo, int16_t* audioData, ChannelMode channelMode = BOTH_CHANNELS);
    static int readWavFromBuffer(const unsigned char* sourceData, const int sourceDataLength, const WAVStruct* wavInfo, int16_t* audioData, ChannelMode channelMode = BOTH_CHANNELS);

private:
    static int32_t readInt32(std::ifstream& file); // Helper to read 32-bit integers
    static int16_t readInt16(std::ifstream& file); // Helper to read 16-bit integers

    static int16_t readInt16m(const unsigned char* data, int* pos);
    static int32_t readInt32m(const unsigned char* data, int* pos);

};

#endif // WAVUTILS_H
