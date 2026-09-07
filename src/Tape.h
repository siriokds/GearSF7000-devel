#pragma once

#include <memory>
#include <string>

#include "TapeMotor.h"
//#include "Storage.h"


class Tape 
{
	public:
		virtual void Init(int masterClock, TapeMotor* motor);
		virtual void Reset(int masterClock);
		virtual void Tick(int cycles);

		bool LoadTape();
		virtual void Eject();

		virtual void Play();
		virtual void Rec();

		virtual void Stop();
		virtual void Rewind();

		virtual int GetSignal();
		// Audio monitored by the cassette recorder's speaker.  This is kept
		// separate from GetSignal(), which is the digital level seen by PPI B7.
		virtual float GetAudioSample();
		virtual bool IsAudioSynthesized() const;
		virtual float GetAudioFrequency() const;
		virtual float GetPositionPercentage();
		virtual float GetCounter();
		virtual float GetCounterMax();

		virtual std::string GetInfo();

		virtual bool IsLoaded();
		virtual bool IsPlaying();
		void SetPlaybackSpeedPercent(float percent);
		float GetPlaybackSpeedPercent() const { return TargetPlaybackSpeedPercent; }
		float GetEffectivePlaybackSpeedPercent() const { return EffectivePlaybackSpeedPercent; }
		float GetPlaybackSpeedMultiplier() const { return 1.0f + EffectivePlaybackSpeedPercent * 0.01f; }

		Tape();
		virtual ~Tape() = default;

	protected:
		int MasterClock;
		bool Loaded;
		bool TapeRunning;
		bool Recording;
		long CyclesElapsed;
		int AdjustedCycles;
		float TargetPlaybackSpeedPercent;
		float EffectivePlaybackSpeedPercent;
		double PlaybackCycleRemainder;

		TapeMotor *Motor;

	private:

};
