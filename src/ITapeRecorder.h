// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#pragma once
#include <cstdint>
#include <string>

// Common engine-facing transport contract. Rec currently only changes the
// legacy transport flag; actual capture and save-state support are separate work.
class ITapeRecorder {
public:
    virtual ~ITapeRecorder() = default;
    virtual void Init() = 0;
    virtual void Reset(int masterClock) = 0;
    virtual void Tick(int cycles) = 0;
    virtual bool LoadTape(std::string filename) = 0;
    virtual void Eject() = 0;
    virtual void Play() = 0;
    virtual void Rec() = 0;
    virtual void Stop() = 0;
    virtual void Rewind() = 0;
    virtual std::uint8_t GetSignal(bool updateSpeaker = true) const = 0;
    virtual void SetMotor(bool onoff) = 0;
    virtual bool IsMotorOn() = 0;
    virtual bool IsMotorRequested() const = 0;
    virtual bool IsLoaded() = 0;
    virtual bool IsPlaying() = 0;
    virtual float GetPositionPercentage() = 0;
    virtual float GetCounter() = 0;
    virtual float GetCounterMax() = 0;
    virtual void SetTapeSpeedPercent(float percent) = 0;
    virtual float GetTapeSpeedPercent() const = 0;
    virtual float GetEffectiveTapeSpeedPercent() const = 0;
    virtual std::string GetInfo() = 0;
};
