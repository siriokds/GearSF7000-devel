// SPDX-License-Identifier: GPL-3.0-or-later
#include "TapeRecorder.h"
#include "SegaBasicWavTape.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>

TapeRecorder::TapeRecorder(TapeMotor::Policy policy)
    : Cassette(nullptr), CassetteMotor(new TapeMotor(policy)), MasterClock(3579545),
      TapeSpeedPercent(0), MotorRequested(false) {}

TapeRecorder::~TapeRecorder()
{
	if (Cassette)
	{
		delete Cassette;
		Cassette = 0;
	}

	delete CassetteMotor;
}

void TapeRecorder::Init()
{
	// The motor is allocated once by the constructor.

	if (Cassette)
	{
		Cassette->Reset(MasterClock);
	}
}

void TapeRecorder::Reset(int masterClock)
{
	MasterClock = masterClock;
	MotorRequested = false;
	CassetteMotor->SetMotor(false);


	if (Cassette)
	{
		Cassette->Reset(masterClock);
	}
}

void TapeRecorder::Tick(int cycles)
{
	if (Cassette)
	{
		const bool wasPlaying = Cassette->IsPlaying();
		Cassette->Tick(cycles);
		if (wasPlaying && !Cassette->IsPlaying()) PublishEvent(TapeEventPlaying, 0);
		// The recorder speaker monitors the tape-head audio, independently of
		// the digital level sampled by the machine.
		if (m_monitor) m_monitor(Cassette->GetAudioSample(),
			Cassette->IsAudioSynthesized(), Cassette->GetAudioFrequency());
	}
	else
	{
		if (m_monitor) m_monitor(0.0f, false, 0.0f);
	}
}



static std::string getExtension(const std::string& filename) {
	size_t pos = filename.rfind('.');
	if (pos != std::string::npos) {
		std::string extension = filename.substr(pos);
		std::transform(extension.begin(), extension.end(), extension.begin(),
			[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
		return extension;
	}
	return ""; // Nessuna estensione trovata
}


bool TapeRecorder::LoadTape(std::string filename)
{
    if (filename.empty()) { Eject(); return true; }
    const auto extension = getExtension(filename);
    if (extension != ".wav" && extension != ".mp3") return false;
    auto tape = std::make_unique<SegaBasicWavTape>();
    tape->Init(MasterClock, CassetteMotor);
    const bool loaded = extension == ".wav" ? tape->LoadTape(filename) : tape->LoadMp3(filename);
    if (!loaded) return false;
    MountTape(tape.release());
    return true;
}

void TapeRecorder::MountTape(Tape* tape)
{
    Eject();
    Cassette = tape;
    Cassette->SetPlaybackSpeedPercent(TapeSpeedPercent);
    PublishEvent(TapeEventLoaded, 1);
}

void TapeRecorder::Eject()
{
	if (Cassette)
	{
		Cassette->Eject();
		delete Cassette;
		Cassette = 0;
		PublishEvent(TapeEventLoaded, 0);
	}

}

void TapeRecorder::Play()
{
	if (Cassette && !Cassette->IsPlaying())
	{
		Cassette->Play();
		PublishEvent(TapeEventPlaying, 1);
	}
}


void TapeRecorder::Rec()
{
	if (Cassette)
	{
		Cassette->Rec();
	}
}

void TapeRecorder::Stop()
{
	if (Cassette && Cassette->IsPlaying())
	{
		Cassette->Stop();
		PublishEvent(TapeEventPlaying, 0);
	}
}

void TapeRecorder::Rewind()
{
	if (Cassette)
	{
		Cassette->Rewind();
		PublishEvent(TapeEventRewind, 1);
	}
}




uint8_t TapeRecorder::GetSignal(bool updateSpeaker) const
{
	(void)updateSpeaker;
	uint8_t signal = 1;

	if (Cassette)
	{
		signal = Cassette->GetSignal();
	}

	return signal;
}

void TapeRecorder::SetMotor(bool onoff)
{
	if (CassetteMotor)
	{
		CassetteMotor->SetMotor(onoff);
		if (MotorRequested != onoff)
			PublishEvent(TapeEventMotor, onoff ? 1 : 0);
		MotorRequested = onoff;
	}
}


bool TapeRecorder::IsPlaying()
{
	if (Cassette)
	{
		return Cassette->IsPlaying();
	}

	return false;
}

bool TapeRecorder::IsLoaded()
{
	if (Cassette)
	{
		return Cassette->IsLoaded();
	}

	return false;
}

bool TapeRecorder::IsMotorOn()
{
	return CassetteMotor ? CassetteMotor->IsMotorOn() : false;
}


float TapeRecorder::GetPositionPercentage()
{
	if (Cassette)
	{
		return Cassette->GetPositionPercentage();
	}

	return 0;
}

float TapeRecorder::GetCounter()
{
	if (Cassette)
	{
		return Cassette->GetCounter();
	}

	return 0;
}

float TapeRecorder::GetCounterMax()
{
	if (Cassette)
	{
		return Cassette->GetCounterMax();
	}

	return 0;
}

void TapeRecorder::SetTapeSpeedPercent(float percent)
{
	const float requested = std::clamp(percent, -20.0f, 20.0f);
	if (TapeSpeedPercent == requested)
		return;
	TapeSpeedPercent = requested;
	if (Cassette)
		Cassette->SetPlaybackSpeedPercent(TapeSpeedPercent);
	PublishEvent(TapeEventTargetSpeed,
			static_cast<std::uint8_t>(std::lround(TapeSpeedPercent) + 20));
}

float TapeRecorder::GetEffectiveTapeSpeedPercent() const
{
	return Cassette ? Cassette->GetEffectivePlaybackSpeedPercent() : TapeSpeedPercent;
}

std::string TapeRecorder::GetInfo()
{
	if (Cassette)
	{
		return Cassette->GetInfo();
	}

	return "";
}
