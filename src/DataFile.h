#pragma once

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

class DataFile 
{
private:
    std::iostream* stream; // Puntatore a un generico stream
    std::string tempFileName; // Nome del file temporaneo
    std::string FileName;

public:
    enum FileOpenMode
    {
        O_RDONLY,

    };

    // Costruttore
    DataFile() : stream(nullptr) {}

    // Distruttore
    ~DataFile() {
        close(); // Chiudiamo il flusso quando l'oggetto viene distrutto
    }


    std::ios_base::openmode GetOpenMode(int mode)
    {
        switch (mode)
        {
            case O_RDONLY:
                return std::ios::in | std::ios::binary;
        }

        return std::ios::in;
    }

    void setFilename(std::string filename)
    {
        FileName = filename;
    }


    void getName(char* output, int max)
    {
        if (!output || max < 1) return;

        int len = FileName.length();
        if (len > max) len = max;

        for (int i = 0; i < len; i++)
            output[i] = FileName[i];
    }

    bool open(int mode)
    {
        return openFile(FileName, GetOpenMode(mode));
    }


        // Costruttore per file
    bool openFile(const std::string& fileName, std::ios::openmode mode) 
    {
        if (stream != nullptr) {
            close(); // Se un flusso è già aperto, lo chiudiamo
        }

        stream = new std::fstream(fileName, mode);
        if (!stream || !stream->good()) {
            delete stream; // Se non è riuscito ad aprire il file, lo distruggiamo
            stream = nullptr;
            return false;
        }
        return true;
    }

    // Funzione per generare un nome unico per il file temporaneo
    std::string generateTempFileName() {
        std::string baseName = "tempfile_";
        std::string timestamp = std::to_string(std::time(nullptr));
        return baseName + timestamp + ".tmp";
    }

    bool openFile() {
        // Genera un nome univoco per il file temporaneo
        std::string fileName = generateTempFileName();
        tempFileName = fileName;

        stream = new std::fstream(fileName, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        return stream && stream->good();
    }

    // Costruttore per buffer in memoria
    void openBuffer() {
        if (stream != nullptr) {
            close(); // Se un flusso è già aperto, lo chiudiamo
        }

        stream = new std::stringstream();
    }


    int read(uint8_t* buffer, size_t bytes) {
        if (stream && stream->good()) {

            stream->read((char*)buffer, bytes);
        }
        return bytes;
    }

    int read(char* buffer, size_t bytes) {
        if (stream && stream->good()) {

            stream->read(buffer, bytes);
        }
        return bytes;
    }


    // Scrittura
    bool write(const std::string& data) {
        if (stream && stream->good()) {
            *stream << data;
            return true;
        }
        return false;
    }

    // Lettura
    std::string read(size_t bytes) {
        if (stream && stream->good()) {
            std::string buffer(bytes, '\0');
            stream->read(&buffer[0], bytes);
            buffer.resize(stream->gcount()); // Ridimensiona per i byte effettivamente letti
            return buffer;
        }
        return "";
    }

    // Controllo EOF
    bool eof() const {
        return stream && stream->eof();
    }

    // Chiudi file o buffer
    void close() {
        if (stream != nullptr) {
            if (auto fileStream = dynamic_cast<std::fstream*>(stream)) {
                fileStream->close(); // Chiudiamo il file
            }
            delete stream; // Liberiamo la memoria
            stream = nullptr;
        }
    }

    // Restituisce la dimensione del file (solo se è un file stream)
    std::streamoff fileSize() {
        if (auto fileStream = dynamic_cast<std::fstream*>(stream)) {
            // Salviamo la posizione corrente
            std::streampos currentPos = fileStream->tellg();

            // Spostiamo il puntatore alla fine del file
            fileStream->seekg(0, std::ios::end);

            // Otteniamo la dimensione del file
            std::streamoff size = fileStream->tellg();

            // Ripristiniamo la posizione originale del puntatore
            fileStream->seekg(currentPos);

            return size;
        }
        return -1; // Restituisce -1 se non è un file stream
    }

    // Seek: dalla posizione iniziale del file
    bool seekSet(long filepos) {
        if (auto fileStream = dynamic_cast<std::fstream*>(stream)) {
            fileStream->seekg(filepos, std::ios::beg);
            return fileStream->good();
        }
        return false;
    }

    // Seek: dalla fine del file
    bool seekEnd(long filepos) {
        if (auto fileStream = dynamic_cast<std::fstream*>(stream)) {
            fileStream->seekg(filepos, std::ios::end);
            return fileStream->good();
        }
        return false;
    }

    // Seek: dalla posizione corrente del file
    bool seekCurr(long filepos) {
        if (auto fileStream = dynamic_cast<std::fstream*>(stream)) {
            fileStream->seekg(filepos, std::ios::cur);
            return fileStream->good();
        }
        return false;
    }

    // Funzioni di lettura e scrittura con filePos
    bool readByte(unsigned long pos, unsigned char* data) {
        if (seekSet(pos)) {
            return readByte(data); // Chiama la versione senza filePos
        }
        return false;
    }

    bool readByte(unsigned char* data) {
        if (stream && stream->good()) {
            *stream >> *data;
            return true;
        }
        return false;
    }

    bool readWord(unsigned long pos, unsigned short* data) {
        if (seekSet(pos)) {
            return readWord(data); // Chiama la versione senza filePos
        }
        return false;
    }

    bool readWord(unsigned short* data) {
        if (stream && stream->good()) {
            unsigned char lowByte, highByte;
            *stream >> lowByte;
            *stream >> highByte;
            *data = lowByte | (highByte << 8);
            return true;
        }
        return false;
    }

    bool readLong(unsigned long pos, unsigned long* data) {
        if (seekSet(pos)) {
            return readLong(data); // Chiama la versione senza filePos
        }
        return false;
    }

    bool readLong(unsigned long* data) {
        if (stream && stream->good()) {
            unsigned char byte1, byte2, byte3, byte4;
            *stream >> byte1;
            *stream >> byte2;
            *stream >> byte3;
            *stream >> byte4;
            *data = byte1 | (byte2 << 8) | (byte3 << 16) | (byte4 << 24);
            return true;
        }
        return false;
    }

    bool readDword(unsigned long pos, unsigned long* data) {
        if (seekSet(pos)) {
            return readDword(data); // Chiama la versione senza filePos
        }
        return false;
    }

    bool readDword(unsigned long* data) {
        if (stream && stream->good()) {
            unsigned char byte1, byte2, byte3, byte4;
            *stream >> byte1;
            *stream >> byte2;
            *stream >> byte3;
            *stream >> byte4;
            *data = byte1 | (byte2 << 8) | (byte3 << 16) | (byte4 << 24);
            return true;
        }
        return false;
    }

    // Funzioni di scrittura con filePos
    bool writeByte(unsigned long pos, unsigned char data) {
        if (seekSet(pos)) {
            return writeByte(data); // Chiama la versione senza filePos
        }
        return false;
    }

    bool writeByte(unsigned char data) {
        if (stream && stream->good()) {
            *stream << data;
            return true;
        }
        return false;
    }

    bool writeWord(unsigned long pos, unsigned short data) {
        if (seekSet(pos)) {
            return writeWord(data); // Chiama la versione senza filePos
        }
        return false;
    }

    bool writeWord(unsigned short data) {
        if (stream && stream->good()) {
            *stream << static_cast<char>(data & 0xFF);
            *stream << static_cast<char>((data >> 8) & 0xFF);
            return true;
        }
        return false;
    }

    bool writeLong(unsigned long pos, unsigned long data) {
        if (seekSet(pos)) {
            return writeLong(data); // Chiama la versione senza filePos
        }
        return false;
    }

    bool writeLong(unsigned long data) {
        if (stream && stream->good()) {
            *stream << static_cast<char>(data & 0xFF);
            *stream << static_cast<char>((data >> 8) & 0xFF);
            *stream << static_cast<char>((data >> 16) & 0xFF);
            return true;
        }
        return false;
    }

    bool writeDword(unsigned long pos, unsigned long data) {
        if (seekSet(pos)) {
            return writeDword(data); // Chiama la versione senza filePos
        }
        return false;
    }

    bool writeDword(unsigned long data) {
        if (stream && stream->good()) {
            *stream << static_cast<char>(data & 0xFF);
            *stream << static_cast<char>((data >> 8) & 0xFF);
            *stream << static_cast<char>((data >> 16) & 0xFF);
            *stream << static_cast<char>((data >> 24) & 0xFF);
            return true;
        }
        return false;
    }

    bool saveAs(const std::string& filename) {
        if (stream == nullptr) {
            return false; // Se lo stream non è stato aperto, restituiamo false
        }

        // Se lo stream è un file (fstream)
        if (auto fileStream = dynamic_cast<std::fstream*>(stream)) {
            // Salviamo il contenuto del file nello stream in un nuovo file binario
            std::ofstream outFile(filename, std::ios::out | std::ios::binary);
            if (!outFile) {
                return false; // Se non riusciamo ad aprire il file di output, restituiamo false
            }

            // Ripristiniamo la posizione del puntatore del file di input
            std::streampos currentPos = fileStream->tellg();
            fileStream->seekg(0, std::ios::beg);

            // Copiamo i dati dal file di input al file di output in modalità binaria
            outFile << fileStream->rdbuf();

            // Ripristiniamo la posizione del file di input
            fileStream->seekg(currentPos);
            return true;
        }
        // Se lo stream è un buffer in memoria (stringstream)
        else if (auto memStream = dynamic_cast<std::stringstream*>(stream)) {
            std::ofstream outFile(filename, std::ios::out | std::ios::binary);
            if (!outFile) {
                return false; // Se non riusciamo ad aprire il file di output, restituiamo false
            }

            // Salviamo il contenuto del buffer in memoria nel file binario
            outFile.write(memStream->str().c_str(), memStream->str().size());
            return true;
        }

        return false; // Se non è né un fstream né un stringstream, restituiamo false
    }

};

//int main() {
//    DataManager manager;
//
//    // Esempio di scrittura e lettura
//    if (manager.openFile("test.bin", std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc)) {
//        // Scrittura di dati
//        manager.writeByte(10, 'A');
//        manager.writeWord(20, 0x1234);
//        manager.writeLong(30, 0x12345678);
//        manager.writeDword(40, 0x12345678);
//
//        // Lettura dei dati
//        unsigned char byteData;
//        unsigned short wordData;
//        unsigned long longData, dwordData;
//
//        manager.readByte(10, byteData);
//        manager.readWord(20, wordData);
//        manager.readLong(30, longData);
//        manager.readDword(40, dwordData);
//
//        // Visualizza i risultati
//        std::cout << "Byte: " << byteData << std::endl;
//        std::cout << "Word: " << wordData << std::endl;
//        std::cout << "Long: " << longData << std::endl;
//        std::cout << "Dword: " << dwordData << std::endl;
//
//        manager.close();
//    }
//
//    return 0;
//}
