#pragma once

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class SimpleBandPassFilter {
private:
    float low_alpha;       // Coefficiente del filtro passa basso
    float high_alpha;      // Coefficiente del filtro passa alto
    float prev_low_output; // Uscita precedente del filtro passa basso
    float prev_high_output; // Uscita precedente del filtro passa alto
    float prev_input;      // Ingresso precedente

public:
    // Costruttore che calcola i coefficienti per il filtro passa banda
    SimpleBandPassFilter(float low_cutoff_frequency, float high_cutoff_frequency, int sampling_rate) {
        float T = 1.0f / static_cast<float>(sampling_rate);  // Intervallo di campionamento

        // Calcola il coefficiente per il filtro passa basso
        low_alpha = T / (T + (1.0f / (2 * M_PI * low_cutoff_frequency)));

        // Calcola il coefficiente per il filtro passa alto
        high_alpha = (1.0f / (2 * M_PI * high_cutoff_frequency)) / (T + (1.0f / (2 * M_PI * high_cutoff_frequency)));

        // Inizializzazione degli stati precedenti
        prev_input = 0.0f;
        prev_low_output = 0.0f;
        prev_high_output = 0.0f;
    }

    void reset() {
        prev_input = 0.0f;
        prev_low_output = 0.0f;
        prev_high_output = 0.0f;
    }

    // Funzione per elaborare un nuovo campione
    float process(float input) {
        // Filtro passa basso (filtro inferiore)
        float low_output = low_alpha * input + (1.0f - low_alpha) * prev_low_output;
        prev_low_output = low_output;

        // Filtro passa alto (filtro superiore)
        float high_output = high_alpha * (input - prev_input) + prev_high_output;
        prev_input = input;
        prev_high_output = high_output;

        // Il filtro passa banda è la differenza tra i due filtri
        return high_output - low_output;
    }
};
