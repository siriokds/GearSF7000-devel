#include "SegaBasicWavTape.h"
#include "definitions.h"
//#include "SegaBasicTapeFormat.h"
#include <cmath>
#include <filesystem>

// Single translation unit for this vendored decoder - see
// third_party/dr_libs/README.md. Nothing else in the codebase includes
// dr_mp3.h.
#define DR_MP3_IMPLEMENTATION
#include "../third_party/dr_libs/dr_mp3.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const double CAPSTAN_SPEED = 1.875;  // in inches per second
const double MICROSECONDS_PER_BYTE = 833.33333333333333333;  // time per byte in microseconds

SegaBasicWavTape::SegaBasicWavTape() : Tape(), FilePath("")
	, CyclesPerPosition(2984), Position(0), EndPosition(0), ActualSymbol(0)
	, PositionCyclesCounter(0), ActualValue(0), audioData(0), audioDataIndex(0) 
{
}


SegaBasicWavTape::~SegaBasicWavTape()
{
	if (audioData)
	{
		delete[] audioData;
		audioData = 0;
	}


}

// Calcola i cicli di clock per un dato tempo e frequenza
inline int CalculateCyclesFromMicros(double clockFrequency, double timeMicroseconds) {
	return static_cast<int>(std::round(clockFrequency * (timeMicroseconds / 1000000.0)));
}


void SegaBasicWavTape::Init(int masterClock, TapeMotor* motor)
{
	Tape::Init(masterClock, motor);

	FilePath = "";

	if (audioData)
	{
		delete[] audioData;
		audioData = 0;
	}
	audioDataIndex = 0;

	if (wavInfo.sampleRate > 0)
		CyclesPerPosition = MasterClock / static_cast<float>(wavInfo.sampleRate);
	else
		CyclesPerPosition = MasterClock / static_cast<float>(GC_AUDIO_SAMPLE_RATE);

	//CyclesPerPosition = CalculateCyclesFromMicros(MasterClock, MICROSECONDS_PER_BYTE);

	CyclePositions[0] = 0;
	CyclePositions[1] = (CyclesPerPosition / 2) - 4;
	CyclePositions[2] = ((CyclesPerPosition * 3) / 4) - 4;
	CyclePositions[3] = CyclesPerPosition - 4;

	Position = 0;
	PositionCyclesCounter = 0;
}

void SegaBasicWavTape::Reset(int masterClock)
{
	Tape::Reset(masterClock);

	if (wavInfo.sampleRate > 0)
		CyclesPerPosition = MasterClock / static_cast<float>(wavInfo.sampleRate);
	else
		CyclesPerPosition = MasterClock / static_cast<float>(GC_AUDIO_SAMPLE_RATE);

	audioDataIndex = 0;
	Position = 0;
	PositionCyclesCounter = 0;
}



int SegaBasicWavTape::GetSignal()
{
	if (!(TapeRunning
		&& Motor->IsMotorOn()
		))
		return 1;

	return ActualValue;
}

float SegaBasicWavTape::GetAudioSample()
{
	if (!(TapeRunning && Motor->IsMotorOn()))
		return 0.0f;

	return static_cast<float>(GetSample()) / 32768.0f;
}

bool SegaBasicWavTape::IsAudioSynthesized() const
{
	return false;
}

bool SegaBasicWavTape::Eof()
{
	return Position > EndPosition;
}

int16_t SegaBasicWavTape::GetSample()
{
	if (!audioData || Eof()) return 0;

	return audioData[Position];
}


void SegaBasicWavTape::Tick(int cycles)
{
	Tape::Tick(cycles);
	const int tapeCycles = AdjustedCycles;

	if (TapeRunning && !Eof() && Motor->IsMotorOn())
	{
		PositionCyclesCounter += tapeCycles;
		if (PositionCyclesCounter >= CyclesPerPosition)
		{
			PositionCyclesCounter -= CyclesPerPosition;

			if (Eof())
			{
				//Position = EndPosition;
				Stop();
			}
			else
			{
				//if (TapeRunning && Motor->IsMotorOn())
				{
					ActualSymbol = GetSample();
					Position++;
				}
			}


		}
		
		ActualValue = 1;
		if (TapeRunning && Motor->IsMotorOn())
		{
			if (ActualSymbol < 8000.0f)
			{
				ActualValue = 0;
			}
		}

	}



}



bool SegaBasicWavTape::LoadBas(std::string fullPath)
{
	//FilePath = fullPath;
	//Position = 0;
	//PositionCyclesCounter = 0;


	//std::filesystem::path basFileFullPath = fullPath;

	//std::string programName = basFileFullPath.stem().string();

	//long fileSize = std::filesystem::file_size(basFileFullPath);
	//if (fileSize == 0) return false;

	//bool loaded = false;
	//PositionCyclesCounter = 0;
	//Position = 0;
	//EndPosition = 0;

	//BasicProgram* basicProgram = new BasicProgram();
	//if (basicProgram)
	//{
	//	loaded = basicProgram->LoadFromProgram(programName, basFileFullPath);
	//	if (loaded)
	//	{
	//		loaded = false;

	//		FileStream = std::tmpfile();
	//		if (FileStream)
	//		{
	//			long len = basicProgram->SaveToAsciiBit(FileStream);

	//			if (len > 0)
	//			{
	//				EndPosition = len - 1;
	//				loaded = true;
	//			}
	//		}
	//	}
	//	delete basicProgram;
	//}

	//this->Loaded = loaded;

	//if (this->Loaded)
	//{
	//	return Tape::LoadTape();
	//}
	//else
	//{
	//	EndPosition = 0;
	//	Tape::Eject();
	//	return false;
	//}

	return false;
}


bool SegaBasicWavTape::LoadTape(std::string filePath)
{
	FilePath = filePath;
	Position = 0;
	PositionCyclesCounter = 0;

	std::error_code fileSizeError;
	long fileSize = static_cast<long>(std::filesystem::file_size(filePath, fileSizeError));
	if (fileSizeError || fileSize == 0) return false;


	PositionCyclesCounter = 0;
	Position = 0;
	EndPosition = 0;


	if (WAVUtils::checkWav(FilePath.c_str(), &wavInfo) != WAVUtils::SUCCESS) return false;

	if (audioData)
	{
		delete[] audioData;
		audioData = 0;
	}


	audioData = new int16_t[wavInfo.numSamples];

	int result = WAVUtils::readWav(FilePath.c_str(), &wavInfo, audioData, WAVUtils::BOTH_CHANNELS);
	if (result != WAVUtils::SUCCESS) {
#ifdef _DEBUG
		printf("[SegaBasicWavTape::LoadTape] failed with error code: %d\n", result);
#endif
		if (audioData)
		{
			delete[] audioData;
			audioData = 0;
		}

		Tape::Eject();
		return false;
	}

	if (wavInfo.sampleRate > 0)
		CyclesPerPosition = MasterClock / static_cast<float>(wavInfo.sampleRate);
	else
		CyclesPerPosition = MasterClock / static_cast<float>(GC_AUDIO_SAMPLE_RATE);
	
	EndPosition = wavInfo.numSamples - 1;

	return Tape::LoadTape();
}

bool SegaBasicWavTape::LoadMp3(std::string filePath)
{
	FilePath = filePath;
	Position = 0;
	PositionCyclesCounter = 0;
	EndPosition = 0;

	std::error_code fileSizeError;
	long fileSize = static_cast<long>(std::filesystem::file_size(filePath, fileSizeError));
	if (fileSizeError || fileSize == 0) return false;

	drmp3_config config;
	config.channels = 0;   // 0 = keep the stream's own channel count
	config.sampleRate = 0; // 0 = keep the stream's own sample rate
	drmp3_uint64 frameCount = 0;
	drmp3_int16* pcm = drmp3_open_file_and_read_pcm_frames_s16(
		FilePath.c_str(), &config, &frameCount, NULL);
	if (!pcm || frameCount == 0)
	{
#ifdef _DEBUG
		printf("[SegaBasicWavTape::LoadMp3] dr_mp3 failed to decode '%s'\n", FilePath.c_str());
#endif
		if (pcm) drmp3_free(pcm, NULL);
		Tape::Eject();
		return false;
	}

	if (audioData)
	{
		delete[] audioData;
		audioData = 0;
	}

	// Cassette audio is a single logical signal; downmix to mono the same
	// way real tape hardware only ever had one channel, regardless of how
	// many channels this particular MP3 rip happens to carry.
	audioData = new int16_t[static_cast<size_t>(frameCount)];
	const drmp3_uint32 channels = config.channels > 0 ? config.channels : 1;
	for (drmp3_uint64 frame = 0; frame < frameCount; frame++)
	{
		int32_t sum = 0;
		for (drmp3_uint32 ch = 0; ch < channels; ch++)
			sum += pcm[frame * channels + ch];
		audioData[frame] = static_cast<int16_t>(sum / static_cast<int32_t>(channels));
	}
	drmp3_free(pcm, NULL);

	wavInfo = WAVUtils::WAVStruct{};
	wavInfo.channels = 1;
	wavInfo.sampleRate = static_cast<int32_t>(config.sampleRate);
	wavInfo.bitDepth = 16;
	wavInfo.numSamples = static_cast<int>(frameCount);

	if (wavInfo.sampleRate > 0)
		CyclesPerPosition = MasterClock / static_cast<float>(wavInfo.sampleRate);
	else
		CyclesPerPosition = MasterClock / static_cast<float>(GC_AUDIO_SAMPLE_RATE);

	EndPosition = wavInfo.numSamples - 1;

	return Tape::LoadTape();
}

void SegaBasicWavTape::Eject()
{

	FilePath = "";
	Position = 0;
	PositionCyclesCounter = 0;

	Tape::Eject();
}

void SegaBasicWavTape::Play()
{
	Tape::Play();
	//Position = 0;
	PositionCyclesCounter = 0;

	//
}

void SegaBasicWavTape::Rec()
{
	Tape::Rec();
	PositionCyclesCounter = 0;
	//
}

void SegaBasicWavTape::Stop()
{
	Tape::Stop();
	PositionCyclesCounter = 0;
	ActualSymbol = 0;
	ActualValue = 1;
}

void SegaBasicWavTape::Rewind()
{
	Tape::Rewind();

	Position = 0;
	PositionCyclesCounter = 0;
}

float SegaBasicWavTape::GetPositionPercentage()
{
	if (EndPosition <= 0) return 0;
	return ((float)Position * 100.0f) / ((float)EndPosition + 1);
}



double SegaBasicWavTape::CalculateRevolutions(int position, double tapeLengthInInches) {
	
	double sampleRate = (wavInfo.sampleRate
		? static_cast<double>(wavInfo.sampleRate)
		: static_cast<double>(GC_AUDIO_SAMPLE_RATE));
	double sampleDuration = 1000000.0f / sampleRate;

	// Total time of the tape in microseconds
	double totalTimeInMicroseconds = position * sampleDuration;

	// Convert the total time into seconds
	double totalTimeInSeconds = totalTimeInMicroseconds / 1e6;

	// Calculate the number of revolutions (revolutions = distance / circumference)
	double tapeCircumference = tapeLengthInInches * M_PI;  // circumference of the reel
	double revolutions = totalTimeInSeconds * CAPSTAN_SPEED / tapeCircumference;

	return revolutions;
}



float SegaBasicWavTape::GetCounter()
{
	return CalculateRevolutions(Position, 60) * 100;
}


float SegaBasicWavTape::GetCounterMax()
{
	return CalculateRevolutions(EndPosition, 60) * 100;
}

std::string SegaBasicWavTape::GetInfo()
{
	float revs = CalculateRevolutions(Position, 60) * 100;

	//double perc = static_cast<double>(PositionCyclesCounter) / CyclesPerPosition;
	//int offset = static_cast<int>(perc * 4.0) & 3; // Ridotto a intervallo [0,3]

	return "C: " + std::to_string(revs);
}
