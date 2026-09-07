/*
** SF7Disc.cpp — Sega Disk BASIC filesystem for SF-7000 disc images
** See SF7Disc.h for the contract.
*/
#include "SF7Disc.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <unordered_set>

/* =========================================================
 * Pure helpers
 * ========================================================= */

int sf7CalculateOffset(int track, int sector) {
    if (track < 0 || track >= DISC_TRACKS) {
        return -1;
    }
    if (sector < 1 || sector > DISC_SECTORS_PER_TRACK) {
        return -1;
    }
    return (track * DISC_SECTORS_PER_TRACK + (sector - 1)) * DISC_SECTOR_SIZE_BYTES;
}

int sf7CalculateClusterOffset(int cluster) {
    return cluster * DISC_CLUSTER_SIZE_BYTES;
}

std::string sf7FormatMSDOSFilename(const std::string& filename) {
    std::string namePart;
    std::string extPart;
    size_t dotPos = filename.find_last_of('.');

    if (dotPos != std::string::npos) {
        namePart = filename.substr(0, dotPos);
        extPart = filename.substr(dotPos + 1);
    }
    else {
        namePart = filename;
    }

    if (namePart.length() > MAX_FILENAME_LENGTH) {
        namePart = namePart.substr(0, MAX_FILENAME_LENGTH);
    }
    else {
        namePart.append(MAX_FILENAME_LENGTH - namePart.length(), ' ');
    }

    if (extPart.length() > MAX_EXTENSION_LENGTH) {
        extPart = extPart.substr(0, MAX_EXTENSION_LENGTH);
    }
    else {
        extPart.append(MAX_EXTENSION_LENGTH - extPart.length(), ' ');
    }

    for (char& c : namePart) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (char& c : extPart)  c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    return namePart + "." + extPart;
}

std::string sf7NormalizeFilename(const char* rawFilename) {
    std::string name(rawFilename, MAX_FILENAME_LENGTH);
    std::string ext(rawFilename + MAX_FILENAME_LENGTH + 1, MAX_EXTENSION_LENGTH);

    name.erase(name.find_last_not_of(' ') + 1);
    ext.erase(ext.find_last_not_of(' ') + 1);

    std::string fullName = name;
    if (!ext.empty()) {
        fullName += "." + ext;
    }

    std::transform(fullName.begin(), fullName.end(), fullName.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return fullName;
}

bool sf7IsDirItemEmpty(const DirectoryItem& dirItem) {
    /* A free slot is not necessarily all zero: real images leave the '.'
     * separator in place at index 8, and other tools pad name and extension
     * with spaces. Treat a slot as free when neither field holds a character. */
    const int nameEnd = MAX_FILENAME_LENGTH;                 /* filename[0..7]  */
    const int extStart = MAX_FILENAME_LENGTH + 1;            /* filename[9..11] */
    const int filenameSize = MAX_FILENAME_LENGTH + 1 + MAX_EXTENSION_LENGTH;

    for (int j = 0; j < filenameSize; ++j) {
        if (j >= nameEnd && j < extStart) {
            continue;                                        /* the separator */
        }
        const unsigned char c = static_cast<unsigned char>(dirItem.filename[j]);
        if (c != 0x00 && c != ' ') {
            return false;
        }
    }

    return true;
}

std::string sf7FATErrorText(FATResult result) {
    switch (result) {
    case FATResult::SUCCESS:                            return "";
    case FATResult::ERROR_IMAGE_FILE:                   return "Failed to read disk image.";
    case FATResult::ERROR_IMAGE_SIZE:                   return "Not an SF-7000 disc image (wrong size).";
    case FATResult::ERROR_NOT_DISK_BASIC:               return "Not a Sega Disk BASIC disc (no usable directory or FAT).";
    case FATResult::ERROR_NO_IMAGE_LOADED:              return "No disc image loaded.";
    case FATResult::ERROR_FILE_NOT_FOUND2:              return "File not found.";
    case FATResult::ERROR_READING_FILE:                 return "Can't read the file.";
    case FATResult::ERROR_WRITING_FILE:                 return "Can't write the file.";
    case FATResult::ERROR_ENCOUNTERED_INVALID_CLUSTER:  return "Invalid cluster encountered.";
    case FATResult::ERROR_CHAIN_LOOP_DETECTED:          return "Loop detected in cluster chain.";
    case FATResult::ERROR_ENCOUNTERED_RESERVED_CLUSTER: return "Reserved or free cluster encountered.";
    case FATResult::ERROR_NOT_ENOUGH_SPACE:             return "Not enough free space on disk to allocate the file.";
    case FATResult::ERROR_NO_FREE_DIRECTORY_SLOT:       return "No free directory slot available.";
    case FATResult::ERROR_IMPORTING_FILE:               return "Can't insert the file.";
    case FATResult::ERROR_FILE_ALREADY_EXISTS:          return "File already exists.";
    case FATResult::ERROR_DELETING_FILE:                return "Can't delete the file.";
    }
    return "Unknown error.";
}

bool sf7MatchWildcard(const std::string& filename, const std::string& pattern) {
    if (pattern.empty()) return true;

    /* Iterative glob rather than <regex>: the same behaviour for '*' and '?'
     * without pulling the regex engine into every translation unit. On a '*'
     * remember where to resume, and backtrack there if the rest fails. */
    size_t f = 0, p = 0;
    size_t starP = std::string::npos, starF = 0;

    auto fold = [](char ch) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    };

    while (f < filename.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || fold(pattern[p]) == fold(filename[f]))) {
            ++f; ++p;
        }
        else if (p < pattern.size() && pattern[p] == '*') {
            starP = p++;
            starF = f;
        }
        else if (starP != std::string::npos) {
            p = starP + 1;
            f = ++starF;
        }
        else {
            return false;
        }
    }

    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

const char* sf7AttributeTypeText(uint8_t attribute) {
    switch (attribute & SF7_ATTR_TYPE_MASK) {
    case SF7_ATTR_TYPE_BASIC: return "BAS";
    case SF7_ATTR_TYPE_ASCII: return "ASC";
    case SF7_ATTR_TYPE_HEX:   return "HEX";
    default:                  return "?";
    }
}

std::string sf7AttributeFlagsText(uint8_t attribute) {
    std::string out;
    if (attribute & SF7_ATTR_READONLY) out += "R/O";
    if (attribute & SF7_ATTR_HIDDEN)   { if (!out.empty()) out += "+"; out += "HID"; }
    if (attribute & SF7_ATTR_SYSTEM)   { if (!out.empty()) out += "+"; out += "SYS"; }
    return out;
}

/* =========================================================
 * FAT logic — the cluster map only
 * ========================================================= */

FATResult sf7GetClusterChainFromFAT(const std::map<int, uint8_t>& fatEntries,
                                    int startCluster,
                                    std::vector<uint8_t>& outputClusterChain,
                                    int* sizeBytes)
{
    std::unordered_set<int> visitedClusters;
    int currentCluster = startCluster;
    outputClusterChain.clear();

    if (sizeBytes) *sizeBytes = 0;

    while (true) {
        if (currentCluster < 0 || currentCluster >= DISC_CLUSTERS_NUM ||
            fatEntries.find(currentCluster) == fatEntries.end()) {
            return FATResult::ERROR_ENCOUNTERED_INVALID_CLUSTER;
        }

        if (visitedClusters.find(currentCluster) != visitedClusters.end()) {
            return FATResult::ERROR_CHAIN_LOOP_DETECTED;
        }
        visitedClusters.insert(currentCluster);

        uint8_t value = fatEntries.at(currentCluster);
        outputClusterChain.push_back(static_cast<uint8_t>(currentCluster));

        /* $C1-$C4 ends the chain and carries the sector count of this cluster. */
        if (value >= FAT_LAST_CLUSTER_CODE_MIN && value <= FAT_LAST_CLUSTER_CODE_MAX) {
            if (sizeBytes) *sizeBytes += (value & 15) * DISC_SECTOR_SIZE_BYTES;
            return FATResult::SUCCESS;
        }

        if (value == FAT_RESERVED_CLUSTER || value == FAT_FREE_CLUSTER) {
            return FATResult::ERROR_ENCOUNTERED_RESERVED_CLUSTER;
        }

        if (sizeBytes) *sizeBytes += DISC_CLUSTER_SIZE_BYTES;
        currentCluster = value;
    }
}

FATResult sf7FindFreeClusters(const std::map<int, uint8_t>& fatEntries,
                              int fileSizeBytes,
                              std::vector<uint8_t>& freeClusters)
{
    int requiredClusters = (fileSizeBytes + DISC_CLUSTER_SIZE_BYTES - 1) / DISC_CLUSTER_SIZE_BYTES;
    if (requiredClusters < 1) requiredClusters = 1;

    freeClusters.clear();

    for (const auto& [cluster, value] : fatEntries) {
        if (value == FAT_FREE_CLUSTER) {
            freeClusters.push_back(static_cast<uint8_t>(cluster));
            if (freeClusters.size() == static_cast<size_t>(requiredClusters)) {
                return FATResult::SUCCESS;
            }
        }
    }

    freeClusters.clear();
    return FATResult::ERROR_NOT_ENOUGH_SPACE;
}

int sf7CountFreeClusters(const std::map<int, uint8_t>& fatEntries) {
    int count = 0;
    for (const auto& [cluster, value] : fatEntries) {
        (void)cluster;
        if (value == FAT_FREE_CLUSTER) ++count;
    }
    return count;
}

/* =========================================================
 * Host file access
 * ========================================================= */

FATResult sf7ReadHostFile(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return FATResult::ERROR_READING_FILE;

    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return FATResult::ERROR_READING_FILE; }
    long size = std::ftell(f);
    if (size < 0) { std::fclose(f); return FATResult::ERROR_READING_FILE; }
    std::rewind(f);

    out.resize(static_cast<size_t>(size));
    if (size > 0 && std::fread(out.data(), 1, out.size(), f) != out.size()) {
        std::fclose(f);
        out.clear();
        return FATResult::ERROR_READING_FILE;
    }

    std::fclose(f);
    return FATResult::SUCCESS;
}

FATResult sf7WriteHostFile(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return FATResult::ERROR_WRITING_FILE;

    if (!data.empty() && std::fwrite(data.data(), 1, data.size(), f) != data.size()) {
        std::fclose(f);
        return FATResult::ERROR_WRITING_FILE;
    }

    if (std::fclose(f) != 0) return FATResult::ERROR_WRITING_FILE;
    return FATResult::SUCCESS;
}

/* =========================================================
 * SF7Image — lifecycle
 * ========================================================= */

FATResult SF7Image::adopt(const std::vector<uint8_t>& bytes) {
    if (bytes.size() != DISC_IMAGE_SIZE) {
        return FATResult::ERROR_IMAGE_SIZE;
    }
    image = bytes;
    dirty = false;
    return FATResult::SUCCESS;
}

/* A directory entry whose name and extension are printable, ignoring the
 * separator at index 8 and trailing NULs. */
static bool sf7EntryNameIsPrintable(const DirectoryItem& item) {
    for (int i = 0; i < 12; ++i) {
        if (i == MAX_FILENAME_LENGTH) continue;
        const unsigned char c = static_cast<unsigned char>(item.filename[i]);
        if (c != 0x00 && (c < 0x20 || c > 0x7E)) return false;
    }
    return true;
}

FATResult SF7Image::validate() const {
    if (!isLoaded()) return FATResult::ERROR_NO_IMAGE_LOADED;

    const int fatOffset = sf7CalculateOffset(FAT_TRACK, FAT_SECTOR);
    if (fatOffset < 0) return FATResult::ERROR_NOT_DISK_BASIC;

    /* Every FAT byte is a next-cluster number, an end marker, reserved or
     * free. A CP/M or SegaDOS image has Z80 code here and fails at once. */
    for (int cluster = 0; cluster < DISC_CLUSTERS_NUM; ++cluster) {
        const uint8_t v = image[fatOffset + cluster];
        const bool legal = (v < DISC_CLUSTERS_NUM)
                        || (v >= FAT_LAST_CLUSTER_CODE_MIN && v <= FAT_LAST_CLUSTER_CODE_MAX)
                        || (v == FAT_RESERVED_CLUSTER)
                        || (v == FAT_FREE_CLUSTER);
        if (!legal) return FATResult::ERROR_NOT_DISK_BASIC;
    }

    /* A legal-looking FAT is not enough on its own: a raw data disc can be
     * filled with a byte that happens to be a valid cluster number. Require
     * the occupied directory entries to look like filenames too. */
    int occupied = 0;
    int printable = 0;
    DirectoryItem entry;
    for (int i = 0; i < DIRECTORY_ENTRY_COUNT; ++i) {
        readEntry(i, &entry);
        if (sf7IsDirItemEmpty(entry)) continue;
        ++occupied;
        if (sf7EntryNameIsPrintable(entry)) ++printable;
    }

    /* An empty formatted disc has no occupied entries and is perfectly valid. */
    if (occupied > 0 && printable * 2 < occupied) {
        return FATResult::ERROR_NOT_DISK_BASIC;
    }

    return FATResult::SUCCESS;
}

FATResult SF7Image::load(const std::string& path) {
    std::vector<uint8_t> raw;
    FATResult res = sf7ReadHostFile(path, raw);
    if (res != FATResult::SUCCESS) {
        return FATResult::ERROR_IMAGE_FILE;
    }

    res = adopt(raw);
    if (res != FATResult::SUCCESS) {
        return res;
    }

    loadedPath = path;
    return FATResult::SUCCESS;
}

FATResult SF7Image::save(const std::string& path) {
    if (!isLoaded()) return FATResult::ERROR_NO_IMAGE_LOADED;

    FATResult res = sf7WriteHostFile(path, image);
    if (res != FATResult::SUCCESS) return res;

    loadedPath = path;
    dirty = false;
    return FATResult::SUCCESS;
}

/* =========================================================
 * SF7Image — directory access
 * ========================================================= */

void SF7Image::readEntry(int index, DirectoryItem* out) const {
    const int base = sf7CalculateOffset(DIRECTORY_TRACK, DIRECTORY_START_SECTOR);
    std::memcpy(out, image.data() + base + index * sizeof(DirectoryItem), sizeof(DirectoryItem));
}

void SF7Image::writeEntry(int index, const DirectoryItem& item) {
    const int base = sf7CalculateOffset(DIRECTORY_TRACK, DIRECTORY_START_SECTOR);
    std::memcpy(image.data() + base + index * sizeof(DirectoryItem), &item, sizeof(DirectoryItem));
    dirty = true;
}

int SF7Image::findFreeDirectorySlot() const {
    DirectoryItem entry;
    for (int i = 0; i < DIRECTORY_ENTRY_COUNT; ++i) {
        readEntry(i, &entry);
        if (sf7IsDirItemEmpty(entry)) return i;
    }
    return -1;
}

FATResult SF7Image::readHeader(DiscHeader* out) const {
    if (!isLoaded()) return FATResult::ERROR_NO_IMAGE_LOADED;

    const int offset = sf7CalculateOffset(BOOT_SECTOR_TRACK, BOOT_SECTOR_SECTOR);
    if (offset < 0) return FATResult::ERROR_IMAGE_FILE;

    std::memcpy(out, image.data() + offset, sizeof(DiscHeader));
    return FATResult::SUCCESS;
}

std::string SF7Image::discLabel() const {
    DiscHeader header;
    if (readHeader(&header) != FATResult::SUCCESS) return "";

    if (std::memcmp(header.signature, SYS_HEADER, 4) != 0) {
        return "";
    }

    /* Signature plus the name field, and no further: the byte after it belongs
     * to 'unused'. Taking SYS_LABEL_SIZE (28) instead of sizeof(discname) (27)
     * pulled that byte in, which on a disc ending in '.' showed a second dot
     * that sfdisc does not print. The label is padded, not terminated, so trim
     * rather than trusting a NUL. */
    const char* start = reinterpret_cast<const char*>(&header);
    std::string label(start, SYS_LABEL_OFFSET + sizeof(header.discname));

    size_t end = label.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) return "";
    label.erase(end + 1);

    /* Stop at the first control character, which marks unwritten padding. */
    for (size_t i = 0; i < label.size(); ++i) {
        if (static_cast<unsigned char>(label[i]) < 0x20) {
            label.erase(i);
            break;
        }
    }

    return label;
}

FATResult SF7Image::readDirectory(std::vector<DirectoryItem>* dirList,
                                  bool includeEmptyEntries) const
{
    if (!isLoaded()) return FATResult::ERROR_NO_IMAGE_LOADED;

    dirList->clear();

    DirectoryItem entry;
    for (int i = 0; i < DIRECTORY_ENTRY_COUNT; ++i) {
        readEntry(i, &entry);
        if (includeEmptyEntries || !sf7IsDirItemEmpty(entry)) {
            dirList->push_back(entry);
        }
    }

    return FATResult::SUCCESS;
}

FATResult SF7Image::readFAT(std::map<int, uint8_t>* fatEntries) const {
    if (!isLoaded()) return FATResult::ERROR_NO_IMAGE_LOADED;

    const int offset = sf7CalculateOffset(FAT_TRACK, FAT_SECTOR);
    if (offset < 0) return FATResult::ERROR_IMAGE_FILE;

    fatEntries->clear();
    for (int cluster = 0; cluster < DISC_CLUSTERS_NUM; ++cluster) {
        (*fatEntries)[cluster] = image[offset + cluster];
    }

    return FATResult::SUCCESS;
}

FATResult SF7Image::writeFAT(const std::map<int, uint8_t>& fatEntries) {
    const int offset = sf7CalculateOffset(FAT_TRACK, FAT_SECTOR);
    if (offset < 0) return FATResult::ERROR_IMAGE_FILE;

    for (int cluster = 0; cluster < DISC_CLUSTERS_NUM; ++cluster) {
        auto it = fatEntries.find(cluster);
        if (it != fatEntries.end()) {
            image[offset + cluster] = it->second;
        }
    }

    dirty = true;
    return FATResult::SUCCESS;
}

int SF7Image::findFile(const std::string& filename, DirectoryItem* dirItem) const {
    if (!isLoaded()) return -1;

    const std::string searchFilename =
        sf7NormalizeFilename(sf7FormatMSDOSFilename(filename).c_str());

    DirectoryItem entry;
    for (int i = 0; i < DIRECTORY_ENTRY_COUNT; ++i) {
        readEntry(i, &entry);
        if (sf7IsDirItemEmpty(entry)) continue;

        if (sf7NormalizeFilename(entry.filename) == searchFilename) {
            if (dirItem) *dirItem = entry;
            return i;
        }
    }

    return -1;
}

FATResult SF7Image::fileSize(const DirectoryItem& dirItem, int* sizeBytes) const {
    if (!isLoaded()) return FATResult::ERROR_NO_IMAGE_LOADED;

    std::map<int, uint8_t> fatEntries;
    FATResult res = readFAT(&fatEntries);
    if (res != FATResult::SUCCESS) return res;

    std::vector<uint8_t> chain;
    return sf7GetClusterChainFromFAT(fatEntries, dirItem.initialCluster, chain, sizeBytes);
}

/* =========================================================
 * SF7Image — export
 * ========================================================= */

FATResult SF7Image::exportFile(const std::string& filename, std::vector<uint8_t>& out) const {
    if (!isLoaded()) return FATResult::ERROR_NO_IMAGE_LOADED;

    out.clear();

    DirectoryItem dirItem;
    if (findFile(filename, &dirItem) < 0) {
        return FATResult::ERROR_FILE_NOT_FOUND2;
    }

    std::map<int, uint8_t> fatEntries;
    FATResult res = readFAT(&fatEntries);
    if (res != FATResult::SUCCESS) return res;

    /* Validate the whole chain before reading a single byte. */
    std::vector<uint8_t> chain;
    int sizeBytes = 0;
    res = sf7GetClusterChainFromFAT(fatEntries, dirItem.initialCluster, chain, &sizeBytes);
    if (res != FATResult::SUCCESS) return res;

    out.reserve(static_cast<size_t>(sizeBytes));

    for (size_t i = 0; i < chain.size(); ++i) {
        const int cluster = chain[i];
        const uint8_t next = fatEntries.at(cluster);

        /* The terminator is a value in $C1-$C4 and its low nibble is the
         * number of sectors used in this last cluster. Masking with 0xC0
         * would also match the $FE and $FF markers and read past the
         * cluster. */
        int bytesToRead = DISC_CLUSTER_SIZE_BYTES;
        if (next >= FAT_LAST_CLUSTER_CODE_MIN && next <= FAT_LAST_CLUSTER_CODE_MAX) {
            bytesToRead = (next & 15) * DISC_SECTOR_SIZE_BYTES;
            if (bytesToRead > DISC_CLUSTER_SIZE_BYTES) bytesToRead = DISC_CLUSTER_SIZE_BYTES;
        }

        const int offset = sf7CalculateClusterOffset(cluster);
        if (offset < 0 || offset + bytesToRead > static_cast<int>(image.size())) {
            return FATResult::ERROR_ENCOUNTERED_INVALID_CLUSTER;
        }

        out.insert(out.end(), image.begin() + offset, image.begin() + offset + bytesToRead);
    }

    return FATResult::SUCCESS;
}

/* =========================================================
 * SF7Image — import and delete
 * ========================================================= */

FATResult SF7Image::importFile(const std::string& filename, const std::vector<uint8_t>& data) {
    if (!isLoaded()) return FATResult::ERROR_NO_IMAGE_LOADED;

    if (findFile(filename) >= 0) {
        return FATResult::ERROR_FILE_ALREADY_EXISTS;
    }

    const int dirIndex = findFreeDirectorySlot();
    if (dirIndex < 0) {
        return FATResult::ERROR_NO_FREE_DIRECTORY_SLOT;
    }

    std::map<int, uint8_t> fatEntries;
    FATResult res = readFAT(&fatEntries);
    if (res != FATResult::SUCCESS) return res;

    std::vector<uint8_t> freeClusters;
    res = sf7FindFreeClusters(fatEntries, static_cast<int>(data.size()), freeClusters);
    if (res != FATResult::SUCCESS) return res;

    int remainingBytes = static_cast<int>(data.size());
    int dataOffset = 0;

    for (size_t i = 0; i < freeClusters.size(); ++i) {
        const int cluster = freeClusters[i];
        const int offset = sf7CalculateClusterOffset(cluster);
        if (offset < 0 || offset + DISC_CLUSTER_SIZE_BYTES > static_cast<int>(image.size())) {
            return FATResult::ERROR_ENCOUNTERED_INVALID_CLUSTER;
        }

        const int bytesToWrite = std::min(remainingBytes, DISC_CLUSTER_SIZE_BYTES);

        /* Zero the tail so the unused part of the last cluster does not leak
         * whatever the previous file left behind. */
        std::memset(image.data() + offset, 0, DISC_CLUSTER_SIZE_BYTES);
        if (bytesToWrite > 0) {
            std::memcpy(image.data() + offset, data.data() + dataOffset,
                        static_cast<size_t>(bytesToWrite));
        }

        remainingBytes -= bytesToWrite;
        dataOffset += bytesToWrite;

        if (i < freeClusters.size() - 1) {
            fatEntries[cluster] = freeClusters[i + 1];
        }
        else {
            /* Count the sectors from what went into THIS cluster. Deriving it
             * from the remaining byte count after the subtraction above makes
             * it always zero, which used to write $C1 every time and truncate
             * every file to one sector on the way back out. */
            int sectorsOccupied = (bytesToWrite + DISC_SECTOR_SIZE_BYTES - 1) / DISC_SECTOR_SIZE_BYTES;
            if (sectorsOccupied < 1) sectorsOccupied = 1;
            if (sectorsOccupied > DISC_CLUSTER_NUM_SECTORS) sectorsOccupied = DISC_CLUSTER_NUM_SECTORS;
            fatEntries[cluster] = static_cast<uint8_t>(FAT_LAST_CLUSTER_CODE + sectorsOccupied);
        }
    }

    res = writeFAT(fatEntries);
    if (res != FATResult::SUCCESS) return res;

    DirectoryItem item;
    std::memset(&item, 0, sizeof(item));
    const std::string fname = sf7FormatMSDOSFilename(filename);
    std::memcpy(item.filename, fname.c_str(), sizeof(item.filename));
    item.initialCluster = freeClusters[0];
    item.attribute = 0;
    item.unused = 0;

    writeEntry(dirIndex, item);

    return FATResult::SUCCESS;
}

FATResult SF7Image::deleteFile(const std::string& filename) {
    if (!isLoaded()) return FATResult::ERROR_NO_IMAGE_LOADED;

    DirectoryItem dirItem;
    const int dirEntryIndex = findFile(filename, &dirItem);
    if (dirEntryIndex < 0) {
        return FATResult::ERROR_FILE_NOT_FOUND2;
    }

    std::map<int, uint8_t> fatEntries;
    FATResult res = readFAT(&fatEntries);
    if (res != FATResult::SUCCESS) return res;

    /* Walk the chain before touching anything: a corrupt chain must leave
     * the image as it was rather than half-freed. */
    std::vector<uint8_t> chain;
    res = sf7GetClusterChainFromFAT(fatEntries, dirItem.initialCluster, chain, nullptr);
    if (res != FATResult::SUCCESS) return res;

    for (uint8_t cluster : chain) {
        fatEntries[cluster] = FAT_FREE_CLUSTER;
    }

    res = writeFAT(fatEntries);
    if (res != FATResult::SUCCESS) return res;

    DirectoryItem empty;
    std::memset(&empty, 0, sizeof(empty));
    writeEntry(dirEntryIndex, empty);

    return FATResult::SUCCESS;
}
