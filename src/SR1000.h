#pragma once

#include <string>
#include "Tape.h"
#include "TapeMotor.h"
#include "SegaBasicBitTape.h"
#include "SegaBasicWavTape.h"
#include "Audio.h"

class Memory;

class SR1000
{
private:
	Tape* Cassette;
	TapeMotor *CassetteMotor;

	int MasterClock;
	float TapeSpeedPercent;
	bool MotorRequested;

	Audio* m_pAudio;
	Memory* m_pDebugMemory;


public:
	void Init();
	void Reset(int masterClock);
	void Tick(int cycles);

	bool LoadBitTape(std::string filename);
	void Eject();

	void Play();
	void Rec();
	void Stop();
	void Rewind();

	uint8_t GetSignal(bool updateSpeaker = true);
	void SetMotor(bool onoff);
	bool IsMotorOn();
	bool IsMotorRequested() const { return MotorRequested; }

	bool IsLoaded();
	bool IsPlaying();

	float GetPositionPercentage();

	float GetCounter();
	float GetCounterMax();
	void SetTapeSpeedPercent(float percent);
	float GetTapeSpeedPercent() const { return TapeSpeedPercent; }
	float GetEffectiveTapeSpeedPercent() const;
	std::string GetInfo();

	SR1000(Audio* pAudio, Memory* pDebugMemory = nullptr);
	~SR1000();
};
