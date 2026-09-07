#include "Tape.h"

#include <algorithm>
#include <cmath>

Tape::Tape() : CyclesElapsed(0), AdjustedCycles(0), TargetPlaybackSpeedPercent(0.0f)
	, EffectivePlaybackSpeedPercent(0.0f), PlaybackCycleRemainder(0.0), Loaded(false), TapeRunning(false)
	, Recording(false), MasterClock(3579545), Motor(nullptr)
{
}


void Tape::Init(int masterClock, TapeMotor* motor)
{
	MasterClock = masterClock;
	Motor = motor;

	Stop();
	Rewind();
	Eject();
}

void Tape::Reset(int masterClock)
{
	MasterClock = masterClock;
	Stop();
	Rewind();
}

void Tape::Tick(int cycles)
{
	AdjustedCycles = 0;
	if (TapeRunning 
		&& Motor->IsMotorOn()
		)
	{
		// A tape deck cannot change capstan speed instantaneously. The target is
		// the UI value; the effective value moves linearly at 40 percent-points
		// per emulated second (0 -> +/-20% takes half a second).
		constexpr float kPlaybackSpeedRampPercentPerSecond = 40.0f;
		const float previousSpeed = EffectivePlaybackSpeedPercent;
		const float maximumDelta = kPlaybackSpeedRampPercentPerSecond
			* static_cast<float>(cycles) / static_cast<float>(MasterClock);
		if (EffectivePlaybackSpeedPercent < TargetPlaybackSpeedPercent)
			EffectivePlaybackSpeedPercent = std::min(
				EffectivePlaybackSpeedPercent + maximumDelta, TargetPlaybackSpeedPercent);
		else if (EffectivePlaybackSpeedPercent > TargetPlaybackSpeedPercent)
			EffectivePlaybackSpeedPercent = std::max(
				EffectivePlaybackSpeedPercent - maximumDelta, TargetPlaybackSpeedPercent);

		// Integrate the linear glide over this CPU step instead of jumping the
		// tape position directly to the new speed.
		const float averageSpeed = (previousSpeed + EffectivePlaybackSpeedPercent) * 0.5f;
		const double adjusted = static_cast<double>(cycles)
			* (1.0 + static_cast<double>(averageSpeed) * 0.01)
			+ PlaybackCycleRemainder;
		AdjustedCycles = static_cast<int>(std::floor(adjusted));
		PlaybackCycleRemainder = adjusted - AdjustedCycles;
		CyclesElapsed += AdjustedCycles;
	}
}

void Tape::SetPlaybackSpeedPercent(float percent)
{
	TargetPlaybackSpeedPercent = std::clamp(percent, -20.0f, 20.0f);
}

bool Tape::LoadTape()
{
	Loaded = true;
	return Loaded;
}

void Tape::Eject()
{
	Loaded = false;
	TapeRunning = false;
	Recording = false;
	CyclesElapsed = 0;
	AdjustedCycles = 0;
	PlaybackCycleRemainder = 0.0;
}

void Tape::Play()
{
	if (Loaded 
		//&& !TapeRunning
		)
	{
		TapeRunning = true;
		CyclesElapsed = 0;
		AdjustedCycles = 0;
		PlaybackCycleRemainder = 0.0;
	}

}


void Tape::Rec()
{
	if (Loaded)
	{
		TapeRunning = true;
		CyclesElapsed = 0;
		AdjustedCycles = 0;
		PlaybackCycleRemainder = 0.0;
		Recording = true;
	}
}




void Tape::Stop()
{
	//if (TapeRunning)
	{
		TapeRunning = false;

		if (Recording == true)
		{
			Recording = false;
		}
	}
}

void Tape::Rewind()
{
	CyclesElapsed = 0;
	AdjustedCycles = 0;
	PlaybackCycleRemainder = 0.0;
}

bool Tape::IsLoaded()
{
	return Loaded;
}

bool Tape::IsPlaying()
{
	return TapeRunning;
}


int Tape::GetSignal()
{
	return 1;
}

float Tape::GetAudioSample()
{
	if (!(TapeRunning && Motor->IsMotorOn()))
		return 0.0f;

	return GetSignal() ? 1.0f : -1.0f;
}

bool Tape::IsAudioSynthesized() const
{
	return true;
}

float Tape::GetAudioFrequency() const
{
	return 0.0f;
}

float Tape::GetPositionPercentage()
{
	return 0;
}

float Tape::GetCounter()
{
	return 0;
}

float Tape::GetCounterMax()
{
	return 0;
}


std::string Tape::GetInfo()
{
	return "";
}
