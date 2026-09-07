#include <cstdint>
#include <vector>
#include <functional>
#include <cmath>
#include <iostream>

#define IDEAL_PULSE_DURATION (833.3333/4)
#define TOLERANCE_PERCENT 0.16 // 16% di tolleranza


#define PULSE_833_DURATION_MIN ((IDEAL_PULSE_DURATION) * 4 * (1.0 - TOLERANCE_PERCENT))
#define PULSE_833_DURATION_MAX ((IDEAL_PULSE_DURATION) * 4 * (1.0 + TOLERANCE_PERCENT))



class SegaBasicDeserializer {
private:
    struct Pulse {
        bool value;       // Alto (true) o basso (false)
        double duration;  // Durata in microsecondi
    };

    enum State {
        WaitForPilotSignal,
        DetectPilot,
        Decode,
        Pause
    };

    State currentState;
    uint64_t lastTStates;
    double masterClock;
    double microsecondsPerTState;

    std::vector<bool> bitBuffer;
    std::function<void(uint8_t)> onByteReceived;

    int pulseCount;
    bool lastValue;
    int continuousOnes;

    std::vector<Pulse> pulseSequence; // Registro a scorrimento per impulsi

    double elapsedTimeUs(uint64_t currentTStates) {
        return (currentTStates - lastTStates) * microsecondsPerTState;
    }

    void resetToValidState() {
        pulseCount = 0;
        lastValue = false;
        continuousOnes = 0;
        pulseSequence.clear();
        currentState = WaitForPilotSignal;
#ifdef _DEBUG
        //std::cout << "\n";
#endif
    }

    void addPulse(bool value, double durationUs) {
        if (pulseSequence.size() == 4) {
            pulseSequence.erase(pulseSequence.begin()); // Rimuovi il più vecchio
        }
        pulseSequence.push_back({ value, durationUs }); // Aggiungi il nuovo impulso
    }

    void CheckSequence() {
        if (pulseCount > 4) {
#ifdef _DEBUG
            std::cout << "[CASSETTE BASIC] Resetting to a valid state.\n";
#endif
            resetToValidState(); // Troppi impulsi senza una sequenza valida
            return;
        }
        // Verifica la sequenza per identificare correttamente il bit
        if (pulseCount == 4) { // Possibile bit 1 (2400 Hz, [alto-basso-alto-basso])
            if (pulseSequence.size() == 4 &&
                pulseSequence[0].value == true && pulseSequence[1].value == false &&
                pulseSequence[2].value == true && pulseSequence[3].value == false) {

                double totalDuration = pulseSequence[0].duration + pulseSequence[1].duration +
                    pulseSequence[2].duration + pulseSequence[3].duration;

                if (totalDuration >= IDEAL_PULSE_DURATION * 4 * (1.0 - TOLERANCE_PERCENT) &&
                    totalDuration <= IDEAL_PULSE_DURATION * 4 * (1.0 + TOLERANCE_PERCENT)) {
                    pulseCount = 0;
                    pulseSequence.clear();
                    handleBit(true); // Bit 1
                }
                else {
#ifdef _DEBUG
//                    std::cout << "Invalid total duration for bit 1: " << totalDuration << " us\n";
#endif
#ifdef _DEBUG
//                    std::cout << "Resetting to a valid state.\n";
#endif

                    //resetToValidState();
                }
            }
        }
        else if (pulseCount == 2) { // Possibile bit 0 (1200 Hz, [alto-basso])
            if (pulseSequence.size() == 2 &&
                pulseSequence[0].value == true && pulseSequence[1].value == false) {

                double totalDuration = pulseSequence[0].duration + pulseSequence[1].duration;

                if (totalDuration >= IDEAL_PULSE_DURATION * 4 * (1.0 - TOLERANCE_PERCENT) &&
                    totalDuration <= IDEAL_PULSE_DURATION * 4 * (1.0 + TOLERANCE_PERCENT)) {
                    pulseCount = 0;
                    pulseSequence.clear();
                    handleBit(false); // Bit 0
                }
                else {
#ifdef _DEBUG
//                    std::cout << "Invalid total duration for bit 0: " << totalDuration << " us\n";
#endif
#ifdef _DEBUG
//                    std::cout << "Resetting to a valid state.\n";
#endif

                    //resetToValidState();
                }
            }
        }
    }

    void processPulse(double pulseDurationUs, bool value) {
        // Calcola i limiti con la tolleranza definita
        double lowerBound = IDEAL_PULSE_DURATION * (1.0 - TOLERANCE_PERCENT);
        double upperBound = IDEAL_PULSE_DURATION * 4 * (1.0 + TOLERANCE_PERCENT);

#ifdef _DEBUG
        //std::cout << "Pulse duration: " << pulseDurationUs << " us - Signal: " << value << "\n";
#endif


        // Controlla se il tempo trascorso è fuori dai limiti ragionevoli
        if (pulseDurationUs < lowerBound || pulseDurationUs > upperBound) {
#ifdef _DEBUG
            if (pulseDurationUs > upperBound) {
                std::cout << "[CASSETTE BASIC] Invalid pulse duration: " << pulseDurationUs << " us\n";
            }
#endif
            resetToValidState();
            return;
        }
        else
        {
        }

        // Rileva cambio di stato e registra il nuovo impulso
        if (value != lastValue) {
            lastValue = value;
            pulseCount++;
            addPulse(value, pulseDurationUs);
        }

        CheckSequence();
    }


    void handleBit(bool bit) {
        // Gestione degli stati
        switch (currentState) 
        {
            case WaitForPilotSignal:
                if (bit) {
                    continuousOnes++;
                    if (continuousOnes > 3600) {
                        resetToValidState();
        #ifdef _DEBUG
                        std::cout << "\n[CASSETTE BASIC] Pilot signal exceeded maximum limit of 3600 bits. Resetting state.\n";
        #endif
                            return;
                    }
                    if (continuousOnes >= 256) {
                        currentState = DetectPilot;
        #ifdef _DEBUG
                        std::cout << "\n[CASSETTE BASIC] PILOT Signal Detected.\n";
        #endif
                    }
                }
                else {
                    continuousOnes = 0;
                }
                break;

            case DetectPilot:
                if (!bit) {
                    currentState = Decode;
        #ifdef _DEBUG
                    std::cout << "[CASSETTE BASIC] Pilot length: " << continuousOnes << "\n";
                    std::cout << "[CASSETTE BASIC] Decoding Data...\n";
        #endif
                    continuousOnes = 0;

                    processBit(bit);
                }
                else
                {
                    continuousOnes++;
                }
                break;

            case Decode:
                processBit(bit);
                break;

            case Pause:
                // Ignora impulsi durante lo stato di pausa
                break;
        }
    }

    void processBit(bool value) {
        bitBuffer.push_back(value);

        // Se abbiamo raccolto 11 bit (start + 8 data + 2 stop), elaboriamo il byte
        if (bitBuffer.size() == 11) {
            if (bitBuffer[0] == false && bitBuffer[9] == true && bitBuffer[10] == true) {
                // Ricostruiamo il byte dai bit di dati
                uint8_t dataByte = 0;
                for (int i = 0; i < 8; ++i) {
                    dataByte |= (bitBuffer[1 + i] << i);
                }

                // Chiamiamo il callback per il byte ricevuto
                if (onByteReceived) {
                    onByteReceived(dataByte);
                }

#ifdef _DEBUG
                printf("%02X ", dataByte);
                //std::cout << std::setw(4) << std::hex << (int)dataByte << " ";
#endif
            }
            else {
    #ifdef _DEBUG
                std::cout << "\n[CASSETTE BASIC] Invalid frame detected!\n";
    #endif
            }

            // Svuotiamo il buffer
            bitBuffer.clear();
        }
    }

public:
    SegaBasicDeserializer(double masterClockHz, std::function<void(uint8_t)> callback)
        : currentState(WaitForPilotSignal), lastTStates(0), masterClock(masterClockHz), onByteReceived(callback), pulseCount(0), lastValue(false), continuousOnes(0) {
        microsecondsPerTState = 1e6 / masterClock;
        bitBuffer.reserve(11);
    }

    void Init(double masterClockHz) {
        Reset(masterClockHz);
    }

    void Reset(double masterClockHz) {
        masterClock = masterClockHz;
        microsecondsPerTState = 1e6 / masterClock;
        lastTStates = 0;
        pulseCount = 0;
        lastValue = false;
        resetToValidState();
    }

    void Set(uint64_t tstates, bool value) {
        double elapsedUs = elapsedTimeUs(tstates);
        lastTStates = tstates;

        if (elapsedUs < 0 || elapsedUs > 2000.0) { // Valori arbitrari per evitare anomalie
            resetToValidState();
            return;
        }

        processPulse(elapsedUs, value);
    }
};

//// Esempio di utilizzo
//void onByteReceived(uint8_t byte) {
//#ifdef _DEBUG
//    std::cout << "Byte ricevuto: " << std::hex << (int)byte << std::endl;
//#endif
//}
//
//int main() {
//    SegaBasicDeserializer deserializer(3579545, onByteReceived);
//
//    // Simulazione di impulsi
//    deserializer.Set(0, true);   // Impulso iniziale
//    deserializer.Set(2986, false); // Bit start (833.3 us)
//    deserializer.Set(5973, true);  // Bit 0 (833.3 us)
//    deserializer.Set(8960, false); // Bit 1 (833.3 us)
//    deserializer.Set(11947, true); // Bit 2 (833.3 us)
//    deserializer.Set(14934, false); // ...
//
//    return 0;
//}
