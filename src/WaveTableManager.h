#pragma once

//#include <iostream>
//#include <vector>
//#include <cmath>
//#include <map>
//#include <queue>
//
//#define NOMINMAX
//#include <algorithm>
//
//#ifndef M_PI
//#define M_PI 3.14159265358979323846
//#endif
//
//// Precisione per i campioni
//#ifndef SamplePrecision
//#define SamplePrecision float
//#endif
//
//// Enumerazione per il tipo di onda
//enum class WaveType { NONE, SINE, SQUARE, TRIANGLE, SAWTOOTH };
//
//// Classe WavetableManager
//class WavetableManager {
//public:
//    WavetableManager(SamplePrecision fMin, SamplePrecision fMax, int tableSize, int tablesPerOctave, SamplePrecision sampleRate)
//        : fMin(fMin), fMax(fMax), tableSize(tableSize), sampleRate(sampleRate), tablesPerOctave(tablesPerOctave),
//        phase(0.0), currentWaveType(WaveType::NONE), currentFrequency(2400.0) {
//        calculateFrequencies();
//        generateWavetables();
//    }
//
//    // Imposta il tipo di onda e la frequenza, aggiungendo i valori nella coda
//    void setWaveTypeAndFrequency(WaveType waveType, SamplePrecision frequency) {
//        if (frequency < fMin || frequency > fMax) {
//            throw std::out_of_range("Frequency out of range");
//        }
//        parameterQueue.push({ waveType, frequency });
//    }
//
//    // Ottiene il prossimo campione, aggiornando i parametri interni al completamento di un ciclo
//    SamplePrecision getNextSample() {
//        // Genera il campione corrente
//        SamplePrecision value = getSampleWTF(currentWaveType, phase, currentFrequency);
//
//        // Aggiorna la fase
//        phase += currentFrequency / sampleRate;
//        if (phase >= 1.0) {
//            phase -= 1.0;
//
//            // Aggiorna i parametri se ci sono nuovi valori in coda
//            if (!parameterQueue.empty()) {
//                auto [newWaveType, newFrequency] = parameterQueue.front();
//                parameterQueue.pop();
//
//                currentWaveType = newWaveType;
//                currentFrequency = newFrequency;
//            }
//        }
//
//        return value;
//    }
//
//private:
//    // Limiti e parametri globali
//    SamplePrecision fMin, fMax;
//    int tableSize;
//    SamplePrecision sampleRate;
//    int tablesPerOctave;
//
//    // Stato interno
//    SamplePrecision phase;                                  // Fase globale
//    WaveType currentWaveType;                               // Tipo di forma d'onda corrente
//    SamplePrecision currentFrequency;                       // Frequenza corrente
//    std::queue<std::pair<WaveType, SamplePrecision>> parameterQueue; // Coda dei parametri
//
//    // Wavetables
//    std::vector<SamplePrecision> frequencies;
//    std::map<WaveType, std::vector<std::vector<SamplePrecision>>> wavetables;
//
//    void calculateFrequencies() {
//        SamplePrecision ratio = std::pow(2.0, 1.0 / tablesPerOctave);
//        SamplePrecision freq = fMin;
//        while (freq <= fMax) {
//            frequencies.push_back(freq);
//            freq *= ratio;
//        }
//    }
//
//    void generateWavetables() {
//        for (auto waveType : { WaveType::SINE, WaveType::SQUARE, WaveType::TRIANGLE, WaveType::SAWTOOTH }) {
//            std::vector<std::vector<SamplePrecision>> typeTables;
//            for (SamplePrecision freq : frequencies) {
//                typeTables.push_back(generateWavetable(waveType, freq));
//            }
//            wavetables[waveType] = typeTables;
//        }
//    }
//
//    std::vector<SamplePrecision> generateWavetable(WaveType waveType, SamplePrecision freq) {
//        std::vector<SamplePrecision> wavetable(tableSize, 0.0);
//        int maxHarmonics = static_cast<int>(sampleRate / (2 * freq));
//
//        for (int k = 1; k <= maxHarmonics; ++k) {
//            SamplePrecision amplitude = 0.0;
//            if (waveType == WaveType::SINE) {
//                if (k == 1) amplitude = 1.0;
//            }
//            else if (waveType == WaveType::SQUARE) {
//                if (k % 2 == 1) amplitude = 1.0 / k;
//            }
//            else if (waveType == WaveType::TRIANGLE) {
//                if (k % 2 == 1) amplitude = (1.0 / (k * k)) * ((k % 4 == 1) ? 1 : -1);
//            }
//            else if (waveType == WaveType::SAWTOOTH) {
//                amplitude = 1.0 / k * ((k % 2 == 0) ? -1 : 1);
//            }
//
//            for (int n = 0; n < tableSize; ++n) {
//                SamplePrecision t = static_cast<SamplePrecision>(n) / tableSize;
//                wavetable[n] += amplitude * std::sin(2.0 * M_PI * k * t);
//            }
//        }
//
//        SamplePrecision maxAmp = *std::max_element(wavetable.begin(), wavetable.end());
//        for (auto& sample : wavetable) {
//            sample /= maxAmp;
//        }
//
//        return wavetable;
//    }
//
//    inline SamplePrecision getSampleWTF(WaveType waveType, SamplePrecision phase, SamplePrecision freq) {
//
//        if (waveType == WaveType::NONE)
//            return 0;
//
//        size_t lowerIndex = findWavetableIndex(freq);
//        size_t upperIndex = (std::min)(lowerIndex + 1, frequencies.size() - 1);
//        SamplePrecision freqLower = frequencies[lowerIndex];
//        SamplePrecision freqUpper = frequencies[upperIndex];
//        SamplePrecision t = (freq - freqLower) / (freqUpper - freqLower);
//
//        SamplePrecision valueLower = getSplineInterpolatedValue(wavetables[waveType][lowerIndex], phase);
//        SamplePrecision valueUpper = getSplineInterpolatedValue(wavetables[waveType][upperIndex], phase);
//
//        return (1.0 - t) * valueLower + t * valueUpper;
//    }
//
//    inline size_t findWavetableIndex(SamplePrecision freq) const {
//        if (freq <= frequencies.front()) return 0;
//        if (freq >= frequencies.back()) return frequencies.size() - 1;
//
//        for (size_t i = 0; i < frequencies.size() - 1; ++i) {
//            if (freq >= frequencies[i] && freq < frequencies[i + 1]) {
//                return i;
//            }
//        }
//
//        return frequencies.size() - 1;
//    }
//
//    inline SamplePrecision getSplineInterpolatedValue(const std::vector<SamplePrecision>& wavetable, SamplePrecision phase) {
//        SamplePrecision floatIndex = phase * tableSize;
//        int intIndex = static_cast<int>(floatIndex);
//        SamplePrecision frac = floatIndex - intIndex;
//
//        SamplePrecision y0 = wavetable[(intIndex - 1 + tableSize) % tableSize];
//        SamplePrecision y1 = wavetable[intIndex % tableSize];
//        SamplePrecision y2 = wavetable[(intIndex + 1) % tableSize];
//        SamplePrecision y3 = wavetable[(intIndex + 2) % tableSize];
//
//        return cubicSpline(y0, y1, y2, y3, frac);
//    }
//
//    inline static SamplePrecision cubicSpline(SamplePrecision y0, SamplePrecision y1, SamplePrecision y2, SamplePrecision y3, SamplePrecision t) {
//        SamplePrecision a = (-0.5 * y0) + (1.5 * y1) - (1.5 * y2) + (0.5 * y3);
//        SamplePrecision b = y0 - (2.5 * y1) + (2.0 * y2) - (0.5 * y3);
//        SamplePrecision c = (-0.5 * y0) + (0.5 * y2);
//        SamplePrecision d = y1;
//
//        return ((a * t + b) * t + c) * t + d;
//    }
//};


#include <iostream>
#include <vector>
#include <cmath>
#include <map>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Precisione per i campioni
#ifndef SamplePrecision
#define SamplePrecision float
#endif

// Enumerazione per il tipo di onda
enum class WaveType { SINE, SQUARE, TRIANGLE, SAWTOOTH };

#define WT_MIN(a,b) (a < b ? a : b)

// Classe WavetableManager
class WavetableManager {
public:
    WavetableManager(SamplePrecision fMin, SamplePrecision fMax, int tableSize, int tablesPerOctave, SamplePrecision sampleRate)
        : fMin(fMin), fMax(fMax), tableSize(tableSize), sampleRate(sampleRate), tablesPerOctave(tablesPerOctave){
        calculateFrequencies();
        generateWavetables();
    }



    inline SamplePrecision getSampleRTF(WaveType waveType, SamplePrecision phase, SamplePrecision freq) {
            return calculateDirectValue(waveType, phase, freq);
    }

    // Metodo alternativo getSample: indice e numero di sample (dinamico)
    SamplePrecision getSampleRTS(WaveType waveType, SamplePrecision sampleIndex, int totalSamples) {
        SamplePrecision phase = fmod(sampleIndex / totalSamples, 1.0);
        SamplePrecision freq = sampleRate / totalSamples;

        return calculateDirectValue(waveType, phase, freq);
    }



    // Metodo principale getSample: fase e frequenza
    inline SamplePrecision getSampleWTF(WaveType waveType, SamplePrecision phase, SamplePrecision freq) {
        size_t lowerIndex = findWavetableIndex(freq);
        size_t upperIndex = WT_MIN(lowerIndex + 1, frequencies.size() - 1);
        SamplePrecision freqLower = frequencies[lowerIndex];
        SamplePrecision freqUpper = frequencies[upperIndex];
        SamplePrecision t = (freq - freqLower) / (freqUpper - freqLower);

        SamplePrecision valueLower = getSplineInterpolatedValue(wavetables[waveType][lowerIndex], phase);
        SamplePrecision valueUpper = getSplineInterpolatedValue(wavetables[waveType][upperIndex], phase);

        return (1.0 - t) * valueLower + t * valueUpper;
    }

    // Metodo alternativo getSample: indice e numero di sample (dinamico)
    SamplePrecision getSampleWTS(WaveType waveType, SamplePrecision sampleIndex, int totalSamples) {
        SamplePrecision phase = fmod(sampleIndex / totalSamples, 1.0);
        SamplePrecision freq = sampleRate / totalSamples;

        return getSampleWTF(waveType, phase, freq);
    }

private:
    SamplePrecision fMin, fMax;
    int tableSize;
    SamplePrecision sampleRate;
    int tablesPerOctave;

    std::vector<SamplePrecision> frequencies;
    std::map<WaveType, std::vector<std::vector<SamplePrecision>>> wavetables;

    void calculateFrequencies() {
        SamplePrecision ratio = std::pow(2.0, 1.0 / tablesPerOctave);
        SamplePrecision freq = fMin;
        while (freq <= fMax) {
            frequencies.push_back(freq);
            freq *= ratio;
        }
    }

    void generateWavetables() {
        for (auto waveType : { WaveType::SINE, WaveType::SQUARE, WaveType::TRIANGLE, WaveType::SAWTOOTH }) {
            std::vector<std::vector<SamplePrecision>> typeTables;
            for (SamplePrecision freq : frequencies) {
                typeTables.push_back(generateWavetable(waveType, freq));
            }
            wavetables[waveType] = typeTables;
        }
    }

    std::vector<SamplePrecision> generateWavetable(WaveType waveType, SamplePrecision freq) {
        std::vector<SamplePrecision> wavetable(tableSize, 0.0);
        int maxHarmonics = static_cast<int>(sampleRate / (2 * freq));

        for (int k = 1; k <= maxHarmonics; ++k) {
            SamplePrecision amplitude = 0.0;
            if (waveType == WaveType::SINE) {
                if (k == 1) amplitude = 1.0;
            }
            else if (waveType == WaveType::SQUARE) {
                if (k % 2 == 1) amplitude = 1.0 / k;
            }
            else if (waveType == WaveType::TRIANGLE) {
                if (k % 2 == 1) amplitude = (1.0 / (k * k)) * ((k % 4 == 1) ? 1 : -1);
            }
            else if (waveType == WaveType::SAWTOOTH) {
                amplitude = 1.0 / k * ((k % 2 == 0) ? -1 : 1);
            }

            for (int n = 0; n < tableSize; ++n) {
                SamplePrecision t = static_cast<SamplePrecision>(n) / tableSize;
                wavetable[n] += amplitude * std::sin(2.0 * M_PI * k * t);
            }
        }

        SamplePrecision maxAmp = *std::max_element(wavetable.begin(), wavetable.end());
        for (auto& sample : wavetable) {
            sample /= maxAmp;
        }

        return wavetable;
    }

    inline SamplePrecision calculateDirectValue(WaveType waveType, SamplePrecision phase, SamplePrecision freq) {
        SamplePrecision value = 0.0;
        int maxHarmonics = static_cast<int>(sampleRate / (2 * freq));

        if (maxHarmonics > 4) maxHarmonics = 4;

        for (int k = 1; k <= maxHarmonics; k += (waveType == WaveType::SQUARE || waveType == WaveType::TRIANGLE ? 2 : 1)) {
            SamplePrecision amplitude = 0.0;
            if (waveType == WaveType::SINE && k == 1) {
                amplitude = 1.0;
            }
            else if (waveType == WaveType::SQUARE) {
                amplitude = 1.0 / k;
            }
            else if (waveType == WaveType::TRIANGLE) {
                amplitude = (1.0 / (k * k)) * ((k % 4 == 1) ? 1 : -1);
            }
            else if (waveType == WaveType::SAWTOOTH) {
                amplitude = 1.0 / k;
            }

            value += amplitude * std::sin(2.0 * M_PI * k * phase);
        }

        return value;
    }

    inline size_t findWavetableIndex(SamplePrecision freq) const {
        if (freq <= frequencies.front()) return 0;
        if (freq >= frequencies.back()) return frequencies.size() - 1;

        for (size_t i = 0; i < frequencies.size() - 1; ++i) {
            if (freq >= frequencies[i] && freq < frequencies[i + 1]) {
                return i;
            }
        }

        return frequencies.size() - 1;
    }

    inline SamplePrecision getSplineInterpolatedValue(const std::vector<SamplePrecision>& wavetable, SamplePrecision phase) {
        SamplePrecision floatIndex = phase * tableSize;
        int intIndex = static_cast<int>(floatIndex);
        SamplePrecision frac = floatIndex - intIndex;

        SamplePrecision y0 = wavetable[(intIndex - 1 + tableSize) % tableSize];
        SamplePrecision y1 = wavetable[intIndex % tableSize];
        SamplePrecision y2 = wavetable[(intIndex + 1) % tableSize];
        SamplePrecision y3 = wavetable[(intIndex + 2) % tableSize];

        return cubicSpline(y0, y1, y2, y3, frac);
    }

    inline static SamplePrecision cubicSpline(SamplePrecision y0, SamplePrecision y1, SamplePrecision y2, SamplePrecision y3, SamplePrecision t) {
        SamplePrecision a = (-0.5 * y0) + (1.5 * y1) - (1.5 * y2) + (0.5 * y3);
        SamplePrecision b = y0 - (2.5 * y1) + (2.0 * y2) - (0.5 * y3);
        SamplePrecision c = (-0.5 * y0) + (0.5 * y2);
        SamplePrecision d = y1;

        return ((a * t + b) * t + c) * t + d;
    }
};


//int main() {
//    SamplePrecision fMin = 20.0, fMax = 20000.0;
//    int tableSize = 1024;
//    SamplePrecision sampleRate = GC_AUDIO_SAMPLE_RATE;
//    int tablesPerOctave = 4;
//    SamplePrecision directCalculationThreshold = 5000.0;
//
//    WavetableManager manager(fMin, fMax, tableSize, sampleRate, tablesPerOctave, directCalculationThreshold);
//
//    WaveType waveType = WaveType::SINE;
//    int totalSamples = 18;
//
//    for (int i = 0; i < totalSamples; ++i) {
//        std::cout << manager.getSample(waveType, i, totalSamples) << std::endl;
//    }
//
//    return 0;
//}
