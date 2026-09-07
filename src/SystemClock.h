#pragma once

#include <stdint.h>
#include <algorithm>
#include <iostream>
#include <iomanip> // Per manipolare la precisione dell'output
#include <vector>

class UpdatableBase
{
public:
    virtual void update() = 0;
    virtual ~UpdatableBase() = default;
};

class UpdatableBaseList
{
private:
    std::vector<UpdatableBase*> items; // vettore statico di puntatori a UpdatableBase

public:
    void add(UpdatableBase* updatable)
    {
        items.push_back(updatable);
    }

    void remove(UpdatableBase* updatable)
    {
        auto it = std::find(items.begin(), items.end(), updatable);
        if (it != items.end())
        {
            items.erase(it);
        }
    }

    void update()
    {
        for (auto& updatable : items)
        {
            updatable->update();
        }
    }
};



class SystemClock
{
public:
    uint64_t m_CyclesElapsed;   // Tempo corrente in cicli

    uint64_t totalMicroseconds;   // Tempo corrente in micros
    float partialMicroseconds;

    float m_MicrosPerCycle;
    int m_ClockRate;
    UpdatableBaseList Timers;

    // Inizializza il timer con la frequenza di clock specificata
    void Init(int clockRate)
    {
        Reset(clockRate);
    }

    // Resetta il timer con una nuova frequenza di clock
    void Reset(int clockRate) {
        if (clockRate <= 0) {
#ifdef _DEBUG
            std::cerr << "Error: clockRate must be positive.\n";
#endif
            return;
        }
        m_CyclesElapsed = 0;
        totalMicroseconds = 0;
        partialMicroseconds = 0;

        m_ClockRate = clockRate;
        m_MicrosPerCycle = static_cast<float>((1000000.0 / static_cast<double>(m_ClockRate)));
    }

    // Aggiorna il timer (simula il passaggio del tempo in base al clock rate)
    void Tick(int cycles)
    {
        m_CyclesElapsed += cycles;

        partialMicroseconds += static_cast<float>(cycles) * m_MicrosPerCycle;
        if (partialMicroseconds >= 1.0f) {
            uint32_t integerPart = static_cast<uint32_t>(partialMicroseconds);
            totalMicroseconds += integerPart;
            partialMicroseconds -= integerPart; // Mantieni solo la parte decimale
        }

        Timers.update();
    }

    // Aggiorna il timer (simula il passaggio del tempo in base al clock rate)
    uint64_t CyclesElapsed()
    {
        return m_CyclesElapsed;
    }

    // Restituisce il tempo trascorso in microsecondi
    uint64_t micros() {
        if (m_ClockRate == 0) return 0; // Prevenire divisioni per zero
        return totalMicroseconds; // (m_CyclesElapsed * 1000000) / m_ClockRate;
    }

    // Restituisce il tempo trascorso in millisecondi
    uint64_t millis() {
        return micros() / 1000;
        //if (m_ClockRate == 0) return 0; // Prevenire divisioni per zero
        //return (m_CyclesElapsed * 1000) / m_ClockRate;
    }

    // Restituisce il tempo trascorso in secondi
    uint64_t seconds() {
        //if (m_ClockRate == 0) return 0.0; // Prevenire divisioni per zero
        //return static_cast<double>(m_CyclesElapsed) / m_ClockRate;
        return millis() / 1000;
    }



};

static SystemClock systemClock;

static int millis()
{
    return (int)systemClock.millis();
}

static int micros()
{
    return (int)systemClock.micros();
}



class TimerClassBase
{
public:
    // Metodo virtuale che deve essere implementato nelle classi derivate
    virtual void interruptHandler() = 0;
};


class Timer : UpdatableBase
{
private:
    uint64_t _startTimeMicros;
    uint64_t _lastTimeMicros;
    bool running;

    TimerClassBase* timerInstance;
    void (TimerClassBase::* interruptHandler)(); // Puntatore a metodo

public:
    Timer() : _startTimeMicros(0), _lastTimeMicros(0), timerInstance(0), interruptHandler(0), running(false) {}

    inline void update()
    {
        if (!running) return;

        _lastTimeMicros = micros();

        if (_lastTimeMicros >= _startTimeMicros)
        {
            stop();
            trigger();
        }
    }

    inline void initialize(unsigned long microseconds = 1000000) {
        setPeriod(microseconds);
    }

    inline void setPeriod(unsigned long microseconds) {
        _lastTimeMicros = micros();
        _startTimeMicros = _lastTimeMicros + microseconds;
        resume();
    }

    inline void start() {
        stop();
        _lastTimeMicros = micros();
        //FTM1_CNT = 0;
        resume();
    }

    inline void stop() {
        running = false;
        //FTM1_SC = FTM1_SC & (FTM_SC_TOIE | FTM_SC_CPWMS | FTM_SC_PS(7));
    }

    inline void restart() {
        start();
    }
    inline void resume() {
        running = true;
        //FTM1_SC = (FTM1_SC & (FTM_SC_TOIE | FTM_SC_PS(7))) | FTM_SC_CPWMS | FTM_SC_CLKS(1);
    }

    inline uint64_t time() {
        return _lastTimeMicros - _startTimeMicros;
        //FTM1_SC = (FTM1_SC & (FTM_SC_TOIE | FTM_SC_PS(7))) | FTM_SC_CPWMS | FTM_SC_CLKS(1);
    }

    inline bool isRunning() {
        return running;
        //FTM1_SC = (FTM1_SC & (FTM_SC_TOIE | FTM_SC_PS(7))) | FTM_SC_CPWMS | FTM_SC_CLKS(1);
    }


    inline void trigger() {
        // Simula l'invocazione dell'interrupt
        if (interruptHandler) {
            (timerInstance->*interruptHandler)();
        }
    }

    void setTimerClassInstance(TimerClassBase* instance) {
        timerInstance = instance;
    }

    // Memorizza un puntatore a metodo
    void attachInterrupt(void (TimerClassBase::* func)()) {
        interruptHandler = func;
    }

};
