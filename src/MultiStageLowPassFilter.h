#pragma once

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class MultiStageLowPassFilter {
private:
    // Coefficienti del primo stadio
    float b0_1, b1_1, b2_1, a1_1, a2_1;
    float x1_1, x2_1, y1_1, y2_1; // Stati del primo stadio

    // Coefficienti del secondo stadio
    float b0_2, b1_2, b2_2, a1_2, a2_2;
    float x1_2, x2_2, y1_2, y2_2; // Stati del secondo stadio

public:
    // Costruttore che calcola i coefficienti per entrambi gli stadi
    MultiStageLowPassFilter(float cutoff_frequency, int sampling_rate, float Q = 0.707f) {
        // Calcolo dei coefficienti per il primo stadio
        calculateCoefficients(cutoff_frequency, sampling_rate, Q, b0_1, b1_1, b2_1, a1_1, a2_1);

        // Calcolo dei coefficienti per il secondo stadio
        calculateCoefficients(cutoff_frequency, sampling_rate, Q, b0_2, b1_2, b2_2, a1_2, a2_2);

        // Inizializza gli stati interni
        reset();
    }

    void reset() {
        // Resetta gli stati interni per entrambi gli stadi
        x1_1 = x2_1 = y1_1 = y2_1 = 0.0f;
        x1_2 = x2_2 = y1_2 = y2_2 = 0.0f;
    }

    // Funzione per elaborare un campione
    float process(float input) {
        // Primo stadio
        float stage1_output = b0_1 * input + b1_1 * x1_1 + b2_1 * x2_1 - a1_1 * y1_1 - a2_1 * y2_1;
        x2_1 = x1_1;
        x1_1 = input;
        y2_1 = y1_1;
        y1_1 = stage1_output;

        // Secondo stadio
        float output = b0_2 * stage1_output + b1_2 * x1_2 + b2_2 * x2_2 - a1_2 * y1_2 - a2_2 * y2_2;
        x2_2 = x1_2;
        x1_2 = stage1_output;
        y2_2 = y1_2;
        y1_2 = output;

        return output;
    }

private:
    // Funzione per calcolare i coefficienti del filtro biquad
    void calculateCoefficients(float cutoff_frequency, int sampling_rate, float Q,
        float& b0, float& b1, float& b2, float& a1, float& a2) {
        float omega = 2.0f * M_PI * cutoff_frequency / sampling_rate;
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

        // Normalizzazione rispetto a a0
        b0 /= a0;
        b1 /= a0;
        b2 /= a0;
        a1 /= a0;
        a2 /= a0;
    }
};
