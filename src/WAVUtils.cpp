#include "WAVUtils.h"
#include <cstdio>   // For printf
#include <cstring>  // For memcpy

// Reads the WAV header and populates the WAVStruct
int WAVUtils::checkWav(const char* filename, WAVStruct* wavInfo) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
#ifdef _DEBUG
        printf("Error in checkWav: Failed to open file '%s'\n", filename);
#endif
        return FILE_NOT_FOUND;
    }

    // Read chunk 0
    wavInfo->chunkID = readInt32(file);
    wavInfo->fileSize = readInt32(file);
    wavInfo->riffType = readInt32(file);

    // Read chunk 1
    wavInfo->fmtID = readInt32(file);
    wavInfo->fmtSize = readInt32(file);

    wavInfo->fmtCode = readInt16(file);
    wavInfo->channels = readInt16(file);
    wavInfo->sampleRate = readInt32(file);
    wavInfo->byteRate = readInt32(file);
    wavInfo->fmtBlockAlign = readInt16(file);
    wavInfo->bitDepth = readInt16(file);

    if (wavInfo->fmtSize == 18) {
        int16_t fmtExtraSize = readInt16(file);
        file.ignore(fmtExtraSize); // Skip any extra bytes
    }

    // Read chunk 2
    wavInfo->dataID = readInt32(file);
    wavInfo->dataSize = readInt32(file);

    // Calculate the number of samples
    int bytesPerSample = wavInfo->bitDepth / 8;
    if (wavInfo->bitDepth != 16) {
#ifdef _DEBUG
        printf("Error in checkWav: Unsupported bit depth %d (only 16-bit supported)\n", wavInfo->bitDepth);
#endif
        return UNSUPPORTED_BIT_DEPTH;
    }

    wavInfo->numSamples = wavInfo->dataSize / (bytesPerSample * wavInfo->channels);
    return SUCCESS;
}







// Reads the WAV header and populates the WAVStruct
int WAVUtils::checkWavFromBuffer(const unsigned char* sourceData, const int sourceDataLength, WAVStruct* wavInfo) {
    if (sourceData == 0 || sourceDataLength < 1) {
#ifdef _DEBUG
        printf("Error in checkWav: Failed to open data\n");
#endif
        return FILE_NOT_FOUND;
    }

    int pos = 0;

    // Read chunk 0
    wavInfo->chunkID = readInt32m(sourceData, &pos);
    wavInfo->fileSize = readInt32m(sourceData, &pos);
    wavInfo->riffType = readInt32m(sourceData, &pos);

    // Read chunk 1
    wavInfo->fmtID = readInt32m(sourceData, &pos);
    wavInfo->fmtSize = readInt32m(sourceData, &pos);

    wavInfo->fmtCode = readInt16m(sourceData, &pos);
    wavInfo->channels = readInt16m(sourceData, &pos);
    wavInfo->sampleRate = readInt32m(sourceData, &pos);
    wavInfo->byteRate = readInt32m(sourceData, &pos);
    wavInfo->fmtBlockAlign = readInt16m(sourceData, &pos);
    wavInfo->bitDepth = readInt16m(sourceData, &pos);

    if (wavInfo->fmtSize == 18) {
        int16_t fmtExtraSize = readInt16m(sourceData, &pos);
        pos += fmtExtraSize;
        //file.ignore(fmtExtraSize); // Skip any extra bytes
    }

    // Read chunk 2
    wavInfo->dataID = readInt32m(sourceData, &pos);
    wavInfo->dataSize = readInt32m(sourceData, &pos);

    // Calculate the number of samples
    int bytesPerSample = wavInfo->bitDepth / 8;
    if (wavInfo->bitDepth != 16) {
#ifdef _DEBUG
        printf("Error in checkWav: Unsupported bit depth %d (only 16-bit supported)\n", wavInfo->bitDepth);
#endif
        return UNSUPPORTED_BIT_DEPTH;
    }

    wavInfo->numSamples = wavInfo->dataSize / (bytesPerSample * wavInfo->channels);
    return SUCCESS;
}



// Reads the audio data from the file
int WAVUtils::readWav(const char* filename, const WAVStruct* wavInfo, int16_t* audioData, ChannelMode channelMode) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
#ifdef _DEBUG
        printf("Error in readWav: Failed to open file '%s'\n", filename);
#endif
        return FILE_NOT_FOUND;
    }

    // Skip the header to reach the data section
    int headerSize = 44; // Typical size of a WAV header
    file.seekg(headerSize, std::ios::beg);

    if (wavInfo->channels == 1 || channelMode == BOTH_CHANNELS) 
    {
        // Read all data (mono or interleaved stereo)
        file.read(reinterpret_cast<char*>(audioData), wavInfo->dataSize);
        if (file.gcount() != wavInfo->dataSize) {
#ifdef _DEBUG
            printf("Error in readWav: Failed to read all audio data\n");
#endif
            return READ_ERROR;
        }
    }
    else if (channelMode == LEFT_CHANNEL_ONLY) {
        // Read only the left channel if the file is stereo
        int bytesPerSample = wavInfo->bitDepth / 8;
        int frameSize = bytesPerSample * wavInfo->channels; // Frame size in bytes
        int16_t* buffer = new int16_t[wavInfo->channels];   // Temporary buffer for a frame

        for (int i = 0; i < wavInfo->numSamples; ++i) {
            file.read(reinterpret_cast<char*>(buffer), frameSize); // Read a full frame
            if (file.gcount() != frameSize) {
#ifdef _DEBUG
                printf("Error in readWav: Unexpected end of file\n");
#endif
                delete[] buffer;
                return UNEXPECTED_END_OF_FILE;
            }
            audioData[i] = buffer[0]; // Extract the left channel
        }

        delete[] buffer;
    }

    return SUCCESS;
}




// Reads the audio data from the file
int WAVUtils::readWavFromBuffer(const unsigned char* sourceData, const int sourceDataLength, const WAVStruct* wavInfo, int16_t* audioData, ChannelMode channelMode) {
    if (sourceData == 0 || sourceDataLength < 1) {
#ifdef _DEBUG
        printf("Error in readWav: Failed to open file data\n");
#endif
        return FILE_NOT_FOUND;
    }

    // Skip the header to reach the data section
    int headerSize = 44; // Typical size of a WAV header

    int pos = headerSize;

    if (wavInfo->channels == 1 || channelMode == BOTH_CHANNELS)
    {
        // Read all data (mono or interleaved stereo)
        memcpy(audioData, sourceData + pos, wavInfo->dataSize);
    }
    else if (channelMode == LEFT_CHANNEL_ONLY) {
        // Read only the left channel if the file is stereo
        int bytesPerSample = wavInfo->bitDepth / 8;
        int frameSize = bytesPerSample * wavInfo->channels; // Frame size in bytes
        int16_t* buffer = new int16_t[wavInfo->channels];   // Temporary buffer for a frame

        for (int i = 0; i < wavInfo->numSamples; ++i) {

            memcpy(buffer, sourceData + pos, frameSize); // Read a full frame
            pos += frameSize;

            audioData[i] = buffer[0]; // Extract the left channel
        }

        delete[] buffer;
    }

    return SUCCESS;
}


// Helper function to read a 32-bit integer
int32_t WAVUtils::readInt32(std::ifstream& file) {
    int32_t value;
    file.read(reinterpret_cast<char*>(&value), sizeof(value));
    return value;
}

// Helper function to read a 16-bit integer
int16_t WAVUtils::readInt16(std::ifstream& file) {
    int16_t value;
    file.read(reinterpret_cast<char*>(&value), sizeof(value));
    return value;
}



// Helper function to read a 16-bit integer
int16_t WAVUtils::readInt16m(const unsigned char* data, int* pos) {
    int16_t value;

    value = (static_cast<int16_t>(data[*pos + 0]) & 255) | (static_cast<int16_t>(data[*pos + 1]) << 8);
    *pos += 2;

    return value;
}



// Helper function to read a 32-bit integer
int32_t WAVUtils::readInt32m(const unsigned char* data, int *pos) {
    int32_t value;

    value = (readInt16m(data, pos) & 65535) | (readInt16m(data, pos) << 16);
    //*pos += 4;

    return value;
}


