#pragma once

#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>

#include "WAVUtils.h"

#include "Tape.h"
#include "TapeMotor.h"

class SegaBasicWavTape : public Tape
{

protected:
	std::string FilePath;

	WAVUtils::WAVStruct wavInfo;
	int16_t* audioData;
	int audioDataIndex;

	int CyclePositions[4];
	float CyclesPerPosition;
	int Position;
	int EndPosition;
	float PositionCyclesCounter;
	int16_t ActualSymbol;
	uint8_t ActualValue;

	double CalculateRevolutions(int position, double tapeLengthInInches);
public:
	SegaBasicWavTape();
	~SegaBasicWavTape() override;

	void Init(int masterClock, TapeMotor* motor) override;
	void Reset(int masterClock) override;
	void Tick(int cycles) override;

	void Play() override;
	void Rec() override;
	void Stop() override;
	void Rewind() override;
	void Eject() override;
	int GetSignal() override;
	float GetAudioSample() override;
	bool IsAudioSynthesized() const override;
	float GetPositionPercentage() override;
	float GetCounter() override;
	float GetCounterMax() override;
	std::string GetInfo() override;

	bool Eof();
	int16_t GetSample();
	bool LoadTape(std::string filePath);
	bool LoadBas(std::string filePath);
	// Decodes filePath (MP3, any source channel count/rate) with dr_mp3 into
	// the same mono int16_t PCM shape LoadTape() produces from a WAV file, so
	// every signal-decode routine below works unmodified either way.
	bool LoadMp3(std::string filePath);



};
