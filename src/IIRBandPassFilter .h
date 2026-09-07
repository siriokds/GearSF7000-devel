#pragma once

class IIRBandPassFilter {
private:
    double a[3]; // Coefficienti feed-backward
    double b[3]; // Coefficienti feed-forward
    double x[3] = { 0.0, 0.0, 0.0 }; // Ultimi input
    double y[3] = { 0.0, 0.0, 0.0 }; // Ultimi output

public:
    // Costruttore che accetta coefficienti
    IIRBandPassFilter(const double bCoeff[3], const double aCoeff[3]) {
        for (int i = 0; i < 3; ++i) {
            b[i] = bCoeff[i];
            a[i] = (i == 0) ? 1.0 : aCoeff[i - 1]; // a[0] è sempre 1.0 nei filtri digitali
        }
    }

    // Metodo per elaborare un campione
    double process(double inputSample) {
        // Shift dati
        x[2] = x[1]; x[1] = x[0]; x[0] = inputSample;
        y[2] = y[1]; y[1] = y[0];

        // Formula del filtro
        y[0] = b[0] * x[0] + b[1] * x[1] + b[2] * x[2]
            - a[1] * y[1] - a[2] * y[2];

        return y[0];
    }
};
