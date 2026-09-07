#pragma once

#include <iostream>
#include <vector>

template<typename T>
class RingBuffer {
private:
    std::vector<T> buffer;  // Il buffer interno
    size_t head;            // Indice di scrittura
    size_t tail;            // Indice di lettura
    size_t capacity;        // Capacità massima del buffer
    bool full;              // Stato del buffer (pieno o no)

public:
    // Costruttore
    explicit RingBuffer(size_t size)
        : buffer(size), head(0), tail(0), capacity(size), full(false) {
    }

    // Aggiunge un elemento al buffer (non fa nulla se è pieno)
    void push(const T& item) {
        if (full) {
            return;  // Ignora l'operazione se il buffer è pieno
        }
        buffer[head] = item;
        head = (head + 1) % capacity;  // Avanza il puntatore di scrittura
        full = head == tail;
    }

    // Rimuove e restituisce un elemento dal buffer
    // Restituisce il valore di default passato come parametro se il buffer è vuoto
    T pop(const T& defaultValue) {
        if (empty()) {
            return defaultValue;  // Restituisce il valore di default
        }
        T item = buffer[tail];
        tail = (tail + 1) % capacity;  // Avanza il puntatore di lettura
        full = false;
        return item;
    }

    // Controlla se il buffer è pieno
    bool isFull() const {
        return full;
    }

    // Controlla se il buffer è vuoto
    bool empty() const {
        return (!full && (head == tail));
    }

    // Restituisce la dimensione attuale del buffer
    size_t size() const {
        if (full) {
            return capacity;
        }
        if (head >= tail) {
            return head - tail;
        }
        return capacity + head - tail;
    }

    // Restituisce la capacità del buffer
    size_t getCapacity() const {
        return capacity;
    }
};

//// Esempio di utilizzo
//int main() {
//    RingBuffer<int> rb(3);  // Ring buffer di capacità 3
//
//    // Inserimento di elementi
//    rb.push(10);
//    rb.push(20);
//    rb.push(30);
//
//    // Tentativo di aggiungere un elemento quando il buffer è pieno
//    rb.push(40);  // Ignorato
//
//    // Rimozione di elementi
//    std::cout << "Pop: " << rb.pop(-1) << std::endl;  // 10
//    std::cout << "Pop: " << rb.pop(-1) << std::endl;  // 20
//    std::cout << "Pop: " << rb.pop(-1) << std::endl;  // 30
//
//    // Tentativo di rimuovere un elemento quando il buffer è vuoto
//    std::cout << "Pop: " << rb.pop(-1) << std::endl;  // -1
//
//    return 0;
//}
