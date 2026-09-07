#pragma once

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class SimpleLowPassFilter {
private:
    float alpha;          // Coefficiente del filtro
    float prev_output;    // Uscita precedente (y(t-1))

public:
    // Costruttore che calcola il coefficiente alpha
    SimpleLowPassFilter(float cutoff_frequency, int sampling_rate) {
        // Calcola il coefficiente alpha
        float T = 1.0f / static_cast<float>(sampling_rate);  // Intervallo di campionamento
        alpha = T / (T + (1.0f / (2 * M_PI * cutoff_frequency)));
        prev_output = 0.0f; // Inizializza l'uscita precedente
    }

    void reset()
    {
        prev_output = 0.0f;
    }

    // Funzione per elaborare un nuovo campione
    float process(float input) {
        // Formula del filtro RC: y(t) = alpha * x(t) + (1 - alpha) * y(t-1)
        float output = alpha * input + (1.0f - alpha) * prev_output;
        prev_output = output;  // Memorizza l'uscita per il prossimo ciclo
        return output;
    }
};

