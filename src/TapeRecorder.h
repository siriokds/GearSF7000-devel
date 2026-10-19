// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include "ITapeRecorder.h"
#include "Tape.h"
#include "TapeMotor.h"

// Machine-independent transport. Tick receives elapsed CPU T-states; reads
// sample the current signal without advancing it. The owning machine supplies
// its CPU frequency on Reset and routes monitor/debug callbacks as appropriate.
class TapeRecorder : public ITapeRecorder
{
public:
    explicit TapeRecorder(TapeMotor::Policy policy = TapeMotor::Policy::Controlled);
    virtual ~TapeRecorder();
    TapeRecorder(const TapeRecorder&) = delete;
    TapeRecorder& operator=(const TapeRecorder&) = delete;
    void Init() override;
    void Reset(int masterClock) override;
    void Tick(int cycles) override;
    bool LoadTape(std::string filename) override; // WAV/MP3; adapters may extend.
    void Eject() override;
    void Play() override;
    void Rec() override; // Legacy transport flag only; does not record an output file.
    void Stop() override;
    void Rewind() override;
    std::uint8_t GetSignal(bool updateSpeaker = true) const override;
    void SetMotor(bool onoff) override;
    bool IsMotorOn() override;
    bool IsMotorRequested() const override { return MotorRequested; }
    bool IsLoaded() override;
    bool IsPlaying() override;
    float GetPositionPercentage() override;
    float GetCounter() override;
    float GetCounterMax() override;
    void SetTapeSpeedPercent(float percent) override;
    float GetTapeSpeedPercent() const override { return TapeSpeedPercent; }
    float GetEffectiveTapeSpeedPercent() const override;
    std::string GetInfo() override;
    using Monitor = std::function<void(float, bool, float)>;
    using Events = std::function<void(unsigned, std::uint8_t)>;
    void SetMonitor(Monitor monitor) { m_monitor = std::move(monitor); }
    void SetEvents(Events events) { m_events = std::move(events); }
    enum Event { TapeEventLoaded, TapeEventPlaying, TapeEventRewind,
                 TapeEventTargetSpeed, TapeEventMotor };
protected:
    void MountTape(Tape* tape); // Takes ownership of an already loaded tape.
    void PublishEvent(unsigned event, std::uint8_t value) {
        if (m_events) m_events(event, value);
    }
    Tape* Cassette;
    TapeMotor* CassetteMotor;
    int MasterClock;
    float TapeSpeedPercent;
    bool MotorRequested;
    Monitor m_monitor;
    Events m_events;
};
