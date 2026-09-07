#pragma once

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class SimpleHighPassFilter {
private:
    float alpha;          // Coefficiente del filtro
    float prev_input;     // Ingresso precedente (x(t-1))
    float prev_output;    // Uscita precedente (y(t-1))

public:
    // Costruttore che calcola il coefficiente alpha
    SimpleHighPassFilter(float cutoff_frequency, int sampling_rate) {
        // Calcola il coefficiente alpha
        float T = 1.0f / static_cast<float>(sampling_rate);  // Intervallo di campionamento
        alpha = (1.0f / (2 * M_PI * cutoff_frequency)) / (T + (1.0f / (2 * M_PI * cutoff_frequency)));
        prev_input = 0.0f;  // Inizializza l'ingresso precedente
        prev_output = 0.0f; // Inizializza l'uscita precedente
    }

    void reset() {
        prev_input = 0.0f;
        prev_output = 0.0f;
    }

    // Funzione per elaborare un nuovo campione
    float process(float input) {
        // Formula del filtro passa alto: y(t) = alpha * (y(t-1) + x(t) - x(t-1))
        float output = alpha * (prev_output + input - prev_input);
        prev_input = input;  // Memorizza l'ingresso per il prossimo ciclo
        prev_output = output;  // Memorizza l'uscita per il prossimo ciclo
        return output;
    }
};

