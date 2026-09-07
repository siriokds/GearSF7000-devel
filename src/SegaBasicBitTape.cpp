#include "SegaBasicBitTape.h"
#include "SegaBasicTapeFormat.h"
#include "definitions.h"
#include <cmath>
#include <filesystem>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const double CAPSTAN_SPEED = 1.875;  // in inches per second
const double MICROSECONDS_PER_BYTE = 833.3;// 3333333333333333;  // time per byte in microseconds
// Sega BASIC cassette FSK: a zero is one 1200 Hz cycle, while a one
// is two 2400 Hz cycles in the same bit period.  The pilot is a long run
// of ones, therefore it must be 2400 Hz.
const double TAPE_BIT_PERIOD_HZ = 1200.0;

SegaBasicBitTape::SegaBasicBitTape() : Tape(), FilePath(""), FileStream(0)
	, CyclesPerPosition(2984), Position(0), EndPosition(0), ActualSymbol(0x20), PositionCyclesCounter(0)
	, ActualValue(0) {
}


SegaBasicBitTape::~SegaBasicBitTape()
{
	if (FileStream)
	{
		fclose(FileStream);
		FileStream = 0;
	}
}

void SegaBasicBitTape::Init(int masterClock, TapeMotor* motor)
{
	Tape::Init(masterClock, motor);

	FilePath = "";

	// Keep the fractional remainder between symbols.  Rounding every bit to
	// 2983 master cycles accumulates phase error and releases it in audible
	// clusters.  Keeping the remainder distributes unavoidable output-sample
	// rounding (for example 9/10 samples at 44.1 kHz) instead of batching it.
	CyclesPerPosition = static_cast<double>(MasterClock) / TAPE_BIT_PERIOD_HZ;

	Position = 0;
	PositionCyclesCounter = 0;
}

void SegaBasicBitTape::Reset(int masterClock)
{
	Tape::Reset(masterClock);

	CyclesPerPosition = static_cast<double>(MasterClock) / TAPE_BIT_PERIOD_HZ;
	Position = 0;
	PositionCyclesCounter = 0;
}



int SegaBasicBitTape::GetSignal()
{
	if (!(TapeRunning 
		&& Motor->IsMotorOn()
		))
		return 1;

	return ActualValue;

	//const int BitOne[4] = { 1, 0, 1, 0 };
	//const int BitZero[4] = { 1, 1, 0, 0 };

	//double perc = static_cast<double>(PositionCyclesCounter) / CyclesPerPosition;
	//int offset = static_cast<int>(perc * 4.0) & 3; // Ridotto a intervallo [0,3]

	//switch (ActualSymbol)
	//{
	//	case 0x31: // Bit 1
	//		return BitOne[offset];
	//	case 0x30: // Bit 0
	//		return BitZero[offset];
	//	default:
	//		return 1;
	//}
}

float SegaBasicBitTape::GetAudioFrequency() const
{
	if (!(TapeRunning && Motor->IsMotorOn()))
		return 0.0f;
	// The tape speed glides continuously, but emitting a new timestamped audio
	// event for every CPU cycle would needlessly fill the bounded synth queue.
	// One-hertz steps are inaudible in a 1200/2400 Hz FSK carrier and still
	// leave ample event headroom at +/-20% transport speed.
	const auto speedAdjustedFrequency = [this](float baseFrequency) {
		return std::round(baseFrequency * GetPlaybackSpeedMultiplier());
	};
	if (ActualSymbol == 0x31)
		return speedAdjustedFrequency(2400.0f);
	if (ActualSymbol == 0x30)
		return speedAdjustedFrequency(1200.0f);
	return 0.0f;
}


void SegaBasicBitTape::Tick(int cycles)
{
	Tape::Tick(cycles);
	const int tapeCycles = AdjustedCycles;

	if (TapeRunning && FileStream && Motor->IsMotorOn())
	{
		PositionCyclesCounter += tapeCycles;
		while (PositionCyclesCounter >= CyclesPerPosition && TapeRunning)
		{
			PositionCyclesCounter -= CyclesPerPosition;

			if (Position > EndPosition)
			{
				Stop();
				break;
			}
			else
			{
				const int symbol = fgetc(FileStream);
				if (symbol == EOF)
				{
					Stop();
					break;
				}
				ActualSymbol = static_cast<uint8_t>(symbol);
				Position++;
			}
		}

		ActualValue = 1;

		if (TapeRunning && Motor->IsMotorOn())
		{
			// Divide every 833.3 us symbol into four exact integer phases.
			// A one is H,L,H,L (two 2400 Hz cycles); a zero is H,H,L,L
			// (one 1200 Hz cycle).  The old 0.99 cutoff inserted a short high
			// glitch once per symbol and created a spurious 1200 Hz component
			// underneath a run of pilot ones.
			const int phase = static_cast<int>((PositionCyclesCounter * 4.0) / CyclesPerPosition);

			switch (ActualSymbol)
			{
				case 0x31: ActualValue = (phase == 0 || phase == 2) ? 1 : 0; break;
				case 0x30: ActualValue = phase < 2 ? 1 : 0; break;
				default:   ActualValue = 1; break; // 0x20: high, then DC removal
			}
		}
	}
}



bool SegaBasicBitTape::LoadBas(std::string fullPath)
{
	FilePath = fullPath;
	Position = 0;
	PositionCyclesCounter = 0;


	std::filesystem::path basFileFullPath = fullPath;

	std::string programName = basFileFullPath.stem().string();

	std::error_code fileSizeError;
	long fileSize = static_cast<long>(std::filesystem::file_size(basFileFullPath, fileSizeError));
	if (fileSizeError || fileSize == 0) return false;

	bool loaded = false;
	PositionCyclesCounter = 0;
	Position = 0;
	EndPosition = 0;

	BasicProgram *basicProgram = new BasicProgram();
	if (basicProgram)
	{
		loaded = basicProgram->LoadFromProgram(programName, basFileFullPath);
		if (loaded)
		{
			loaded = false;

			FileStream = std::tmpfile();
			if (FileStream)
			{
				long len = basicProgram->SaveToAsciiBit(FileStream);

				if (len > 0)
				{
					EndPosition = len - 1;
					loaded = true;
				}
			}
		}
		delete basicProgram;
	}

	this->Loaded = loaded;

	if (this->Loaded)
	{
		return Tape::LoadTape();
	}
	else
	{
		EndPosition = 0;
		Tape::Eject();
		return false;
	}
}


bool SegaBasicBitTape::LoadTape(std::string filePath)
{
	FilePath = filePath;
	Position = 0;
	PositionCyclesCounter = 0;

	std::error_code fileSizeError;
	long fileSize = static_cast<long>(std::filesystem::file_size(filePath, fileSizeError));
	if (fileSizeError || fileSize == 0) return false;


	PositionCyclesCounter = 0;
	Position = 0;
	EndPosition = fileSize - 1;

	//

	FileStream = fopen(FilePath.c_str(), "rb");

	if (FileStream)
	{


		return Tape::LoadTape();
	}
	else
	{
		EndPosition = 0;
		Tape::Eject();
		return false;
	}
}

void SegaBasicBitTape::Eject()
{
	if (FileStream != 0)
	{
		fclose(FileStream);
		FileStream = 0;
	}

	FilePath = "";
	Position = 0;
	PositionCyclesCounter = 0;

	Tape::Eject();
}

void SegaBasicBitTape::Play()
{
	Tape::Play();
	PositionCyclesCounter = 0;

	// Prime the first symbol at t=0.  Previously the monitor emitted a high
	// level for a whole bit period before reading byte zero from the file.
	if (TapeRunning && FileStream && Position == 0 && Position <= EndPosition)
	{
		const int symbol = fgetc(FileStream);
		if (symbol == EOF)
		{
			Stop();
		}
		else
		{
			ActualSymbol = static_cast<uint8_t>(symbol);
			Position = 1;
			ActualValue = 1;
		}
	}
}

void SegaBasicBitTape::Rec()
{
	Tape::Rec();
	PositionCyclesCounter = 0;
	//
}

void SegaBasicBitTape::Stop()
{
	Tape::Stop();
	PositionCyclesCounter = 0;
	ActualSymbol = 0x20;
	ActualValue = 1;
}

void SegaBasicBitTape::Rewind()
{
	Tape::Rewind();

	if (FileStream)
	{
		fseek(FileStream, 0, 0);
	}

	Position = 0;
	PositionCyclesCounter = 0;
	ActualSymbol = 0x20;
	ActualValue = 1;
}

float SegaBasicBitTape::GetPositionPercentage()
{
	if (EndPosition <= 0) return 0;
	return ((float)Position * 100.0f) / ((float)EndPosition + 1);
}



double SegaBasicBitTape::CalculateRevolutions(int position, double tapeLengthInInches) {
	// Total time of the tape in microseconds
	double totalTimeInMicroseconds = position * MICROSECONDS_PER_BYTE;

	// Convert the total time into seconds
	double totalTimeInSeconds = totalTimeInMicroseconds / 1e6;

	// Calculate the number of revolutions (revolutions = distance / circumference)
	double tapeCircumference = tapeLengthInInches * M_PI;  // circumference of the reel
	double revolutions = totalTimeInSeconds * CAPSTAN_SPEED / tapeCircumference;

	return revolutions;
}



float SegaBasicBitTape::GetCounter() 
{
	return CalculateRevolutions(Position, 60) * 100;
}


float SegaBasicBitTape::GetCounterMax()
{
	return CalculateRevolutions(EndPosition, 60) * 100;
}

std::string SegaBasicBitTape::GetInfo()
{
	double perc = static_cast<double>(PositionCyclesCounter) / CyclesPerPosition;
	int offset = static_cast<int>(perc * 4.0) & 3; // Ridotto a intervallo [0,3]

	return "Off: " + std::to_string(offset) + " - Perc: " + std::to_string(perc);
}
