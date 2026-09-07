#pragma once
#include <vector>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FILTERSAMPLE    double


class Butterworth2ndOrderBandPassFilter {
private:
    std::vector<FILTERSAMPLE> a; // Coefficienti del denominatore (feedback)
    std::vector<FILTERSAMPLE> b; // Coefficienti del numeratore (feedforward)
    std::vector<FILTERSAMPLE> x; // Campioni precedenti di ingresso
    std::vector<FILTERSAMPLE> y; // Campioni precedenti di uscita

public:
    // Costruttore: calcola i coefficienti per il filtro passa banda
    Butterworth2ndOrderBandPassFilter(FILTERSAMPLE low_cutoff_frequency, FILTERSAMPLE high_cutoff_frequency, int sampling_rate) {
        FILTERSAMPLE T = 1.0f / sampling_rate; // Periodo di campionamento
        FILTERSAMPLE omega_low = 2 * M_PI * low_cutoff_frequency / sampling_rate;
        FILTERSAMPLE omega_high = 2 * M_PI * high_cutoff_frequency / sampling_rate;
        FILTERSAMPLE omega_center = (omega_low + omega_high) / 2.0f;
        FILTERSAMPLE bandwidth = omega_high - omega_low;

        // Parametri per il design bilineare
        FILTERSAMPLE Q = omega_center / bandwidth; // Fattore di qualità
        FILTERSAMPLE alpha = sin(omega_center) / (2.0f * Q);

        // Coefficienti del filtro IIR (Butterworth di secondo ordine)
        b = {
            alpha, 0.0f, -alpha
        }; // Numeratore
        a = {
            1.0f + alpha,
            -2.0f * cos(omega_center),
            1.0f - alpha
        }; // Denominatore

        // Normalizzazione dei coefficienti
        for (FILTERSAMPLE& coeff : b) {
            coeff /= a[0];
        }
        for (FILTERSAMPLE& coeff : a) {
            coeff /= a[0];
        }

        // Inizializzazione dei campioni precedenti
        x = std::vector<FILTERSAMPLE>(3, 0.0f); // Tre campioni per il passa banda
        y = std::vector<FILTERSAMPLE>(3, 0.0f);
    }

    void reset() {
        std::fill(x.begin(), x.end(), 0.0f);
        std::fill(y.begin(), y.end(), 0.0f);
    }

    // Elabora un nuovo campione
    FILTERSAMPLE process(FILTERSAMPLE input) 
    {

        // Shift dei campioni precedenti
        x[2] = x[1];
        x[1] = x[0];
        x[0] = input;

        y[2] = y[1];
        y[1] = y[0];

        // Applicazione dell'equazione del filtro
        y[0] = b[0] * x[0] + b[1] * x[1] + b[2] * x[2]
            - a[1] * y[1] - a[2] * y[2];

        return y[0];
    }

    FILTERSAMPLE process(FILTERSAMPLE input, bool mute_output) {
        // Shift dei campioni
        x[2] = x[1];
        x[1] = x[0];
        x[0] = input;

        y[2] = y[1];
        y[1] = y[0];

        // Filtro passa banda
        y[0] = b[0] * x[0] + b[1] * x[1] + b[2] * x[2]
            - a[1] * y[1] - a[2] * y[2];

        // Se mute_output è true, forza l'output a zero ma preserva lo stato interno
        return mute_output ? 0.0f : y[0];
    }
};
