#pragma once

#pragma once

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class AdvancedLowPassFilter {
private:
    // Coefficienti del filtro biquad
    float a1, a2, b0, b1, b2;
    // Stati interni
    float x1, x2; // Campioni di input passati
    float y1, y2; // Campioni di output passati

public:
    // Costruttore che calcola i coefficienti del filtro
    AdvancedLowPassFilter(float cutoff_frequency, int sampling_rate, float Q = 0.707f) {
        float omega = 2.0f * M_PI * cutoff_frequency / sampling_rate; // Frequenza normalizzata
        float alpha = sin(omega) / (2.0f * Q);

        float cos_omega = cos(omega);
        float a0 = 1.0f + alpha;

        // Coefficienti del numeratore (b0, b1, b2)
        b0 = (1.0f - cos_omega) / 2.0f;
        b1 = 1.0f - cos_omega;
        b2 = (1.0f - cos_omega) / 2.0f;

        // Coefficienti del denominatore (a1, a2)
        a1 = -2.0f * cos_omega;
        a2 = 1.0f - alpha;

        // Normalizza i coefficienti rispetto a a0
        b0 /= a0;
        b1 /= a0;
        b2 /= a0;
        a1 /= a0;
        a2 /= a0;

        // Inizializza gli stati interni
        x1 = x2 = 0.0f;
        y1 = y2 = 0.0f;
    }

    void reset() {
        x1 = x2 = 0.0f;
        y1 = y2 = 0.0f;
    }

    // Funzione per elaborare un nuovo campione
    float process(float input) {
        // Formula del filtro biquad
        float output = b0 * input + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;

        // Aggiorna gli stati interni
        x2 = x1;
        x1 = input;
        y2 = y1;
        y1 = output;

        return output;
    }
};
