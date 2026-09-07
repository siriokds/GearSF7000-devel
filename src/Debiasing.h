#pragma once

#include <iostream>
#include <queue>
#include <cmath>

class Debiasing {
private:
    float samplingFrequency; // Frequenza di campionamento in Hz
    float debiasTime;        // Tempo di debiasing in microsecondi
    size_t windowSize;        // Dimensione della finestra per la media mobile
    std::queue<float> buffer; // Buffer per i campioni
    float sum;               // Somma dei valori nel buffer

    // Calcola la dimensione della finestra basandosi su samplingFrequency e debiasTime
    void updateWindowSize() {
        windowSize = static_cast<size_t>(std::ceil((samplingFrequency * debiasTime) / 1e6));
        while (buffer.size() > windowSize) {
            sum -= buffer.front();
            buffer.pop();
        }
    }

public:
    // Costruttore
    Debiasing(float initDebiasTime, float initbias, float samplingFrequency)
        : samplingFrequency(samplingFrequency),
        debiasTime(initDebiasTime),
        sum(initbias) {
        updateWindowSize();
    }

    // Cambia la frequenza di campionamento
    void setSamplingFrequency(float newSamplingFrequency) {
        samplingFrequency = newSamplingFrequency;
        updateWindowSize();
    }

    // Cambia il tempo necessario per il debiasing (in microsecondi)
    void setDebiasTime(float newDebiasTime) {
        debiasTime = newDebiasTime;
        updateWindowSize();
    }

    void reset() {
        std::queue<float>().swap(buffer);
        sum = 0.0f;
    }

    // Processo di debiasing: aggiunge il valore al buffer e calcola l'uscita
    float process(float input) {
        buffer.push(input);
        sum += input;

        if (buffer.size() > windowSize) {
            sum -= buffer.front();
            buffer.pop();
        }

        // Ritorna il valore debiasato (input - media)
        return input - (sum / buffer.size());
    }

    // Ottieni la frequenza di campionamento
    float getSamplingFrequency() const {
        return samplingFrequency;
    }

    // Ottieni il tempo di debiasing
    float getDebiasTime() const {
        return debiasTime;
    }
};

//// Esempio di utilizzo
//int main() {
//    Debiasing debias(48000.0, 100000.0); // Frequenza di campionamento: 48000 Hz, tempo di debiasing: 100 ms
//
//    float inputValues[] = { 1.0, 2.0, 3.0, 4.0, 5.0 };
//
//    std::cout << "Processed values:" << std::endl;
//    for (float value : inputValues) {
//        float output = debias.process(value);
//        std::cout << "Input: " << value << ", Output: " << output << std::endl;
//    }
//
//    return 0;
//}
