#pragma once

#include <vector>
#include <string>
#include <fstream>
#include <queue>
#include <iostream>

// SPI Mode 0 = CPOL=0, CPHA=0

class SPITapePlayer {
private:
    struct {
        uint8_t last_values[4] = { 0 };  // Ultimi 4 stati
        int position = 0;
        bool detected = false;
    } printer_init_pattern;

    // Pin states
    bool cs_active = false;     // PC6 inverted (0=active)
    bool sclk_last = false;     // PC7 previous state
    bool mosi = false;          // PC5
    bool miso = false;          // PB5 output
    bool ready = true;          // PB6 output (0=ready, 1=busy)

    // SPI state machine
    enum State { IDLE, RECEIVING, SENDING } state = IDLE;
    uint8_t bit_counter = 0;
    uint8_t rx_byte = 0;
    uint8_t tx_byte = 0;

    // Tape data management
    std::vector<uint8_t> tape_data;
    size_t tape_position = 0;
    bool tape_loaded = false;

    // Debug
    bool debug_enabled = false;

public:
    SPITapePlayer() = default;

    int GetBitCounter() const {
        return bit_counter;
    }

    // Load .tsc tape file (header + program consecutive)
    bool loadTape(const std::string& filename) {
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Failed to open tape file: " << filename << std::endl;
            return false;
        }

        tape_data.clear();
        tape_data.assign(std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>());
        file.close();

        tape_position = 0;
        tape_loaded = true;
        ready = true;

        if (debug_enabled) {
            std::cout << "Loaded .tsc tape: " << filename
                << " (" << tape_data.size() << " bytes)" << std::endl;

            // Try to show tape info
            if (tape_data.size() >= 6) {
                std::cout << "Program name: ";
                for (int i = 0; i < 6; i++) {
                    char c = tape_data[i];
                    std::cout << (isprint(c) ? c : '?');
                }
                std::cout << std::endl;
            }
        }
        return true;
    }

    // Reset tape to beginning
    void rewindTape() {
        tape_position = 0;
        ready = true;
        state = IDLE;
        bit_counter = 0;
    }

    // Enable/disable debug output
    void setDebug(bool enable) {
        debug_enabled = enable;
    }

    // Main interface: called when Z80 writes to port C
    void writePortC(uint8_t value) {

        //uint8_t spi_bits = (value & 0xE0) >> 5;  // PC7,PC6,PC5

        //// Salva nello storico pattern
        //printer_init_pattern.last_values[printer_init_pattern.position] = spi_bits;
        //printer_init_pattern.position = (printer_init_pattern.position + 1) % 4;

        //// Pattern init: [000] → [100] → [110] → [000]  
        //// (SCLK,MOSI,CS con CS=0 sempre, quindi bit 0 sempre 0)
        //bool is_init_pattern =
        //    (printer_init_pattern.last_values[0] & 0x6) == 0x0 &&  // 000
        //    (printer_init_pattern.last_values[1] & 0x6) == 0x4 &&  // 100  
        //    (printer_init_pattern.last_values[2] & 0x6) == 0x6 &&  // 110
        //    (printer_init_pattern.last_values[3] & 0x6) == 0x0;    // 000

//        if (is_init_pattern) {
//            // È init stampante - resetta tutto
//            state = IDLE;
//            bit_counter = 0;
//            tape_position = 0;
//            miso = false;
//            printer_init_pattern.detected = true;
//
//#ifdef _DEBUG
//            std::cout << "SPI Reset: " 
//                << " position " << std::dec << tape_position << std::endl;
//#endif
//
//            return;  // Non processare come SPI
//        }

        bool new_cs = !(value & 0x20);    // PC5 inverted (NUOVO MAPPING)
        bool new_sclk = (value & 0x80);   // PC7
        mosi = (value & 0x40);            // PC6 (NUOVO MAPPING)

        // CS edge detection
        if (!cs_active && new_cs) {
            // CS activated (falling edge)
            startTransaction();
        }
        else if (cs_active && !new_cs) {
            // CS deactivated (rising edge)
            endTransaction();
        }

        cs_active = new_cs;

        // SCLK rising edge detection (only when CS active)
        if (cs_active && !sclk_last && new_sclk) {
            handleClockRisingEdge();
        }

        sclk_last = new_sclk;
    }

    // Read port B (returns PB6=ready, PB5=miso)
    uint8_t readPortB() {
        uint8_t result = 0x00;

        if (!ready) result |= 0x40;  // PB6: 1=busy, 0=ready
        if (miso) result |= 0x20;    // PB5: MISO data

        return result;
    }

private:
    void startTransaction() {
        if (!tape_loaded || tape_position >= tape_data.size()) {
            tx_byte = 0x00;  // EOF or no tape
            ready = false;   // Signal not ready
        }
        else {
            tx_byte = tape_data[tape_position];
            ready = true;
        }

        state = RECEIVING;
        bit_counter = 0;
        rx_byte = 0;

//        if (debug_enabled) {
#ifdef _DEBUG
            std::cout << "SPI Start: Will send 0x" << std::hex << (int)tx_byte
                << " at position " << std::dec << tape_position << std::endl;
#endif
//        }
    }

    void handleClockRisingEdge() {
        if (state != RECEIVING) return;

        // Receive MOSI bit (MSB first)
        rx_byte = (rx_byte << 1) | (mosi ? 1 : 0);

        // Send MISO bit (MSB first)
        miso = (tx_byte & 0x80) != 0;
        tx_byte <<= 1;

        bit_counter++;

        if (bit_counter >= 8) {
            // Byte complete
//            if (debug_enabled) {
#ifdef _DEBUG
                std::cout << "SPI Byte complete: RX=0x" << std::hex << (int)rx_byte
                    << " TX=0x" << (int)(tape_data[tape_position] & 0xFF) << std::dec << std::endl;
#endif  
            //}

            // Handle received byte (could be save operation)
            handleReceivedByte(rx_byte);

            state = IDLE;
            bit_counter = 0;
        }
    }

    void endTransaction() {
        // Avanza sempre se eravamo in una transazione valida
        if (tape_position < tape_data.size()) {
            tape_position++;  // Advance to next byte
        }

        state = IDLE;
        bit_counter = 0;
        miso = false;

        // Update ready status
        ready = (tape_position < tape_data.size());

#ifdef _DEBUG
        std::cout << "SPI End: Position now " << tape_position
                << "/" << tape_data.size() << std::endl;
#endif
    }

//    void endTransaction() {
//        if (state == RECEIVING && tape_position < tape_data.size()) {
//            tape_position++;  // Advance to next byte
//        }
//
//        state = IDLE;
//        bit_counter = 0;
//        miso = false;
//
//        // Update ready status
//        ready = (tape_position < tape_data.size());
//
////        if (debug_enabled) {
//#ifdef _DEBUG
//            std::cout << "SPI End: Position now " << tape_position
//                << "/" << tape_data.size() << std::endl;
//#endif
////        }
//    }
//
    void handleReceivedByte(uint8_t byte) {
        // This could be used for save operations
        // For now, just log it
//        if (debug_enabled) {
#ifdef _DEBUG
            std::cout << "Received byte from Z80: 0x" << std::hex << (int)byte << std::dec << std::endl;
#endif
//        }

        // TODO: Implement save functionality
        // tape_data.push_back(byte);
    }

public:
    // Status methods for emulator UI
    bool isReady() const { return ready; }
    bool isTapeLoaded() const { return tape_loaded; }
    size_t getTapePosition() const { return tape_position; }
    size_t getTapeSize() const { return tape_data.size(); }

    // Get tape data for inspection
    const std::vector<uint8_t>& getTapeData() const { return tape_data; }

    // Save current tape data to file
    bool saveTape(const std::string& filename) {
        std::ofstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Failed to save tape file: " << filename << std::endl;
            return false;
        }

        file.write(reinterpret_cast<const char*>(tape_data.data()), tape_data.size());
        file.close();

        if (debug_enabled) {
            std::cout << "Saved tape: " << filename
                << " (" << tape_data.size() << " bytes)" << std::endl;
        }
        return true;
    }
};
