#include "SR1000.h"
#include "Memory.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace
{
enum SR1000TapeEventType
{
	SR1000TapeEventLoaded = 0x00,
	SR1000TapeEventPlaying = 0x01,
	SR1000TapeEventRewind = 0x02,
	SR1000TapeEventTargetSpeed = 0x03,
	SR1000TapeEventMotor = 0x04
};
}

SR1000::SR1000(Audio* pAudio, Memory* pDebugMemory) : Cassette(0), MasterClock(3579545)
	, TapeSpeedPercent(0.0f), MotorRequested(false), CassetteMotor(0)
{
	m_pAudio = pAudio;
	m_pDebugMemory = pDebugMemory;
}

SR1000::~SR1000()
{
	if (Cassette)
	{
		delete Cassette;
		Cassette = 0;
	}

	delete CassetteMotor;
}

void SR1000::Init()
{
	CassetteMotor = new TapeMotor();

	if (Cassette)
	{
		Cassette->Reset(MasterClock);
	}
}

void SR1000::Reset(int masterClock)
{
	MasterClock = masterClock;
	MotorRequested = false;
	CassetteMotor->SetMotor(false);
	

	if (Cassette)
	{
		Cassette->Reset(masterClock);
	}
}

void SR1000::Tick(int cycles)
{
	if (Cassette)
	{
		Cassette->Tick(cycles);
		// The recorder speaker monitors the tape-head audio, independently of
		// the binary level that the PPI reads on B7.
		m_pAudio->SR1000SpeakerWrite(Cassette->GetAudioSample(),
			Cassette->IsAudioSynthesized(), Cassette->GetAudioFrequency());
	}
	else
	{
		m_pAudio->SR1000SpeakerWrite(0.0f, false);
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


bool SR1000::LoadBitTape(std::string filename)
{
	if (filename == "") {
		Eject();
		return true;
	}

	std::string extension = getExtension(filename);
	// The restoration archive uses .basic while older tools use .bas. Both
	// contain the same source program and are converted to the canonical,
	// two-block SC-3000 BASIC stream at load time.
	if (extension != ".bit" && extension != ".bas" && extension != ".basic"
		&& extension != ".wav" && extension != ".mp3") return false;

	bool loaded = false;
	Tape* loadedTape = nullptr;

	if (extension == ".bit")
	{
		SegaBasicBitTape* bitTape = new SegaBasicBitTape();
		if (bitTape)
		{
			bitTape->Init(MasterClock, this->CassetteMotor);
			loaded = bitTape->LoadTape(filename);
			loadedTape = bitTape;
		}
	}
	else if (extension == ".bas" || extension == ".basic")
	{
		SegaBasicBitTape* bitTape = new SegaBasicBitTape();
		if (bitTape)
		{
			bitTape->Init(MasterClock, this->CassetteMotor);
			loaded = bitTape->LoadBas(filename);
			loadedTape = bitTape;
		}
	}
	else if (extension == ".wav")
	{
		SegaBasicWavTape* wavTape = new SegaBasicWavTape();
		if (wavTape)
		{
			wavTape->Init(MasterClock, this->CassetteMotor);
			loaded = wavTape->LoadTape(filename);
			loadedTape = wavTape;
		}
	}
	else if (extension == ".mp3")
	{
		SegaBasicWavTape* wavTape = new SegaBasicWavTape();
		if (wavTape)
		{
			wavTape->Init(MasterClock, this->CassetteMotor);
			loaded = wavTape->LoadMp3(filename);
			loadedTape = wavTape;
		}
	}

	if (!loaded)
	{
		delete loadedTape;
		return false;
	}

	// Replace the currently mounted tape only after the new tape has loaded.
	// This also keeps the old tape intact when opening a bad/unsupported file.
	Eject();
	Cassette = loadedTape;
	Cassette->SetPlaybackSpeedPercent(TapeSpeedPercent);
	if (m_pDebugMemory)
		m_pDebugMemory->CheckTapeEvent(SR1000TapeEventLoaded, 1);
	return true;

}


void SR1000::Eject() 
{
	if (Cassette)
	{
		Cassette->Eject();
		delete Cassette;
		Cassette = 0;
		if (m_pDebugMemory)
			m_pDebugMemory->CheckTapeEvent(SR1000TapeEventLoaded, 0);
	}

}

void SR1000::Play()
{
	if (Cassette && !Cassette->IsPlaying())
	{
		Cassette->Play();
		if (m_pDebugMemory)
			m_pDebugMemory->CheckTapeEvent(SR1000TapeEventPlaying, 1);
	}
}


void SR1000::Rec()
{
	if (Cassette)
	{
		Cassette->Rec();
	}
}

void SR1000::Stop()
{
	if (Cassette && Cassette->IsPlaying())
	{
		Cassette->Stop();
		if (m_pDebugMemory)
			m_pDebugMemory->CheckTapeEvent(SR1000TapeEventPlaying, 0);
	}
}

void SR1000::Rewind()
{
	if (Cassette)
	{
		Cassette->Rewind();
		if (m_pDebugMemory)
			m_pDebugMemory->CheckTapeEvent(SR1000TapeEventRewind, 1);
	}
}




uint8_t SR1000::GetSignal(bool updateSpeaker)
{
	(void)updateSpeaker;
	uint8_t signal = 1;

	if (Cassette)
	{
		signal = Cassette->GetSignal();
	}

	return signal;
}

void SR1000::SetMotor(bool onoff)
{
	if (CassetteMotor)
	{
		CassetteMotor->SetMotor(onoff);
		if (MotorRequested != onoff && m_pDebugMemory)
			m_pDebugMemory->CheckTapeEvent(SR1000TapeEventMotor, onoff ? 1 : 0);
		MotorRequested = onoff;
	}
}


bool SR1000::IsPlaying()
{
	if (Cassette)
	{
		return Cassette->IsPlaying();
	}

	return false;
}

bool SR1000::IsLoaded()
{
	if (Cassette)
	{
		return Cassette->IsLoaded();
	}

	return false;
}

bool SR1000::IsMotorOn()
{
	return CassetteMotor ? CassetteMotor->IsMotorOn() : false;
}


float SR1000::GetPositionPercentage()
{
	if (Cassette)
	{
		return Cassette->GetPositionPercentage();
	}

	return 0;
}

float SR1000::GetCounter()
{
	if (Cassette)
	{
		return Cassette->GetCounter();
	}

	return 0;
}

float SR1000::GetCounterMax()
{
	if (Cassette)
	{
		return Cassette->GetCounterMax();
	}

	return 0;
}

void SR1000::SetTapeSpeedPercent(float percent)
{
	const float requested = std::clamp(percent, -20.0f, 20.0f);
	if (TapeSpeedPercent == requested)
		return;
	TapeSpeedPercent = requested;
	if (Cassette)
		Cassette->SetPlaybackSpeedPercent(TapeSpeedPercent);
	if (m_pDebugMemory)
		m_pDebugMemory->CheckTapeEvent(SR1000TapeEventTargetSpeed,
			static_cast<u8>(std::lround(TapeSpeedPercent) + 20));
}

float SR1000::GetEffectiveTapeSpeedPercent() const
{
	return Cassette ? Cassette->GetEffectivePlaybackSpeedPercent() : TapeSpeedPercent;
}

std::string SR1000::GetInfo()
{
	if (Cassette)
	{
		return Cassette->GetInfo();
	}

	return "";
}
