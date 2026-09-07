#pragma once

#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>

#include "Tape.h"
#include "TapeMotor.h"

class SegaBasicBitTape : public Tape
{

protected:
	std::string FilePath;
	FILE* FileStream;

	double CyclesPerPosition;
	int Position;
	int EndPosition;
	double PositionCyclesCounter;
	uint8_t ActualSymbol;
	uint8_t ActualValue;

	double CalculateRevolutions(int position, double tapeLengthInInches);
public:
	SegaBasicBitTape();
	~SegaBasicBitTape() override;

	void Init(int masterClock, TapeMotor* motor) override;
	void Reset(int masterClock) override;
	void Tick(int cycles) override;

	void Play() override;
	void Rec() override;
	void Stop() override;
	void Rewind() override;
	void Eject() override;
	int GetSignal() override;
	float GetAudioFrequency() const override;
	float GetPositionPercentage() override;
	float GetCounter() override;
	float GetCounterMax() override;
	std::string GetInfo() override;

	bool LoadTape(std::string filePath);
	bool LoadBas(std::string filePath);

	

};
