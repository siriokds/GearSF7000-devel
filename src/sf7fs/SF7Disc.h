/*
** SF7Disc.h — Sega Disk BASIC filesystem for SF-7000 disc images
**
** Directory and FAT logic for flat .sf7 images, derived from the sfdisc
** command-line tool (https://github.com/siriokds/sfdisc).
**
** The whole image is held in memory: an SF-7000 disc is 163840 bytes, so
** there is nothing to gain from streaming it, and a plain buffer makes every
** operation testable and keeps host file access at the edges. Load once,
** operate, save when the caller asks.
**
** No function here prints anything. Every failure is reported as a FATResult;
** sf7FATErrorText() turns one into a message for the UI.
*/
#ifndef SF7DISC_H
#define SF7DISC_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

/* =========================================================
 * Geometry — SF-7000: 40 tracks, 1 side, 16 sectors, 256 B
 * ========================================================= */
#define DISC_SECTOR_SIZE_BYTES      256
#define DISC_CLUSTER_NUM_SECTORS    (4)
#define DISC_CLUSTER_SIZE_BYTES     (DISC_SECTOR_SIZE_BYTES*DISC_CLUSTER_NUM_SECTORS)
#define DISC_TRACKS                 40
#define DISC_SECTORS_PER_TRACK      16
#define DISC_IMAGE_SIZE             (DISC_TRACKS * DISC_SECTORS_PER_TRACK * DISC_SECTOR_SIZE_BYTES)
#define DISC_CLUSTERS_NUM           (DISC_IMAGE_SIZE/DISC_CLUSTER_SIZE_BYTES)

/* Sega Disc Format */
#define BOOT_SECTOR_TRACK           0
#define BOOT_SECTOR_SECTOR          1

/* Directory: track 20, sectors 1..12 — 16 entries per sector, 192 total */
#define DIRECTORY_TRACK             20
#define DIRECTORY_START_SECTOR      1
#define DIRECTORY_END_SECTOR        12
#define DIRECTORY_ENTRY_COUNT       192

/* FAT: track 20, sector 13 */
#define FAT_TRACK                   20
#define FAT_SECTOR                  13

#define FAT_FREE_CLUSTER            0xFF
#define FAT_RESERVED_CLUSTER        0xFE
#define FAT_LAST_CLUSTER_CODE       0xC0
#define FAT_LAST_CLUSTER_CODE_MIN   (FAT_LAST_CLUSTER_CODE + 1)                         /* 0xC1 */
#define FAT_LAST_CLUSTER_CODE_MAX   (FAT_LAST_CLUSTER_CODE + DISC_CLUSTER_NUM_SECTORS)  /* 0xC4 */

#define SYS_RESERVED_SECTORS_START  2
#define SYS_RESERVED_SECTORS_END    16
#define SYS_HEADER                  "SYS:"
#define SYS_LABEL_OFFSET            4
#define SYS_LABEL_SIZE              28

/* Directory entry attribute byte.
 *
 * Disk BASIC only ever looks at the type in the low bits and at read-only.
 * SC-DOS 2.0 reuses two of the spare bits for HIDDEN and SYSTEM, and BASIC
 * ignores them: a $E2 file lists as *COM exactly like $02, and free space is
 * unaffected. The filesystem is otherwise identical between the two, so the
 * extension costs nothing to support and reading it is the whole difference.
 *
 * The type is three bits, not seven: masking with $7F was enough only while
 * bits 5 and 6 were always clear. */
#define SF7_ATTR_TYPE_MASK          0x07
#define SF7_ATTR_TYPE_BASIC         0x00   /* tokenised BASIC */
#define SF7_ATTR_TYPE_ASCII         0x01
#define SF7_ATTR_TYPE_HEX           0x02   /* binary / machine code */
#define SF7_ATTR_READONLY           0x80
#define SF7_ATTR_HIDDEN             0x40   /* SC-DOS extension */
#define SF7_ATTR_SYSTEM             0x20   /* SC-DOS extension */

/* $FF in the first-cluster byte means no cluster is allocated. Cluster 0 is a
 * real data cluster -- files written by Disk BASIC commonly start there -- so
 * zero cannot be the sentinel. */
#define SF7_CLUSTER_NONE            0xFF

#define MAX_FILENAME_LENGTH         8
#define MAX_EXTENSION_LENGTH        3

/* =========================================================
 * On-disc structures
 *
 * These are copied byte for byte out of the image, so their layout is part
 * of the disc format and not a choice of the compiler: without pack, an ABI
 * with different alignment would insert padding and shift the directory.
 * ========================================================= */
#pragma pack(push, 1)
struct DirectoryItem {
    char filename[12];      /* 8-character name + '.' + 3-character extension */
    uint8_t initialCluster; /* Starting cluster */
    uint8_t attribute;      /* File attribute */
    uint16_t unused;        /* Not used, always zero */
};

struct DiscHeader {
    char signature[4];      /* "SYS:" on a system disc */
    char discname[27];
    uint8_t unused;
};
#pragma pack(pop)

static_assert(sizeof(DirectoryItem) == 16, "DirectoryItem deve essere 16 byte esatti");
static_assert(sizeof(DiscHeader) == 32, "DiscHeader deve essere 32 byte esatti");

enum class FATResult {
    SUCCESS,
    ERROR_IMAGE_FILE,
    ERROR_IMAGE_SIZE,
    ERROR_NOT_DISK_BASIC,
    ERROR_NO_IMAGE_LOADED,
    ERROR_FILE_NOT_FOUND2,
    ERROR_READING_FILE,
    ERROR_WRITING_FILE,
    ERROR_ENCOUNTERED_INVALID_CLUSTER,
    ERROR_CHAIN_LOOP_DETECTED,
    ERROR_ENCOUNTERED_RESERVED_CLUSTER,
    ERROR_NOT_ENOUGH_SPACE,
    ERROR_NO_FREE_DIRECTORY_SLOT,
    ERROR_IMPORTING_FILE,
    ERROR_FILE_ALREADY_EXISTS,
    ERROR_DELETING_FILE
};

/* =========================================================
 * Pure helpers — no I/O, no state
 * ========================================================= */
std::string sf7FormatMSDOSFilename(const std::string& filename);
std::string sf7NormalizeFilename(const char* rawFilename);
bool        sf7IsDirItemEmpty(const DirectoryItem& dirItem);

/* Byte offset of a sector. Track is 0-based, sector is 1-based.
 * Returns -1 when either is out of range. */
int         sf7CalculateOffset(int track, int sector);
int         sf7CalculateClusterOffset(int cluster);

std::string sf7FATErrorText(FATResult result);

/* "BAS", "ASC", "HEX", or "?" for a type this format does not define. */
const char* sf7AttributeTypeText(uint8_t attribute);

/* The flags as "R/O+HID+SYS", empty when none are set. */
std::string sf7AttributeFlagsText(uint8_t attribute);

/* Case-insensitive glob over a normalised 8.3 name: '*' spans any run, '?'
 * one character. An empty pattern matches everything, so a filter box that
 * has not been typed in yet shows the whole directory. */
bool sf7MatchWildcard(const std::string& filename, const std::string& pattern);

/* =========================================================
 * FAT logic — operates on the cluster map alone, never on the image
 * ========================================================= */
FATResult sf7GetClusterChainFromFAT(const std::map<int, uint8_t>& fatEntries,
                                    int startCluster,
                                    std::vector<uint8_t>& outputClusterChain,
                                    int* sizeBytes);

FATResult sf7FindFreeClusters(const std::map<int, uint8_t>& fatEntries,
                              int fileSizeBytes,
                              std::vector<uint8_t>& freeClusters);

int sf7CountFreeClusters(const std::map<int, uint8_t>& fatEntries);

/* =========================================================
 * SF7Image — one mounted disc image
 * ========================================================= */
class SF7Image {
public:
    /* ---- Host file access, the only I/O in this library ---- */

    /* Loads any image of the right size, whatever it turns out to hold. The
     * tool is an analyser: refusing to open a CP/M or SegaDOS disc would hide
     * exactly the thing worth looking at. Ask validate() for a verdict and
     * show it; do not gate on it. */
    FATResult load(const std::string& path);
    FATResult save(const std::string& path);

    /* Adopt an image already in memory; it must be DISC_IMAGE_SIZE bytes. */
    FATResult adopt(const std::vector<uint8_t>& bytes);

    /* A verdict, not a gate: SUCCESS when the image carries a Sega Disk BASIC
     * filesystem, ERROR_NOT_DISK_BASIC otherwise. Checks the FAT holds only
     * legal values and that the occupied directory entries carry printable
     * 8.3 names. An empty formatted disc is valid. Callers are free to read a
     * disc that fails this -- CP/M keeps Z80 code where the FAT belongs, and
     * seeing that is the point.
     *
     * The directory reader never filters: entries filled with $FE are
     * reserved padding but are listed like everything else, matching sfdisc,
     * because the aim is the real content of the disc rather than what Disk
     * BASIC would choose to show. */
    FATResult validate() const;

    bool isLoaded() const { return image.size() == DISC_IMAGE_SIZE; }

    /* Set by every operation that changes the image, cleared by save(). */
    bool isDirty() const { return dirty; }
    void clearDirty()    { dirty = false; }

    const std::string& path() const { return loadedPath; }
    const std::vector<uint8_t>& bytes() const { return image; }

    /* ---- Reading ---- */
    FATResult readHeader(DiscHeader* out) const;

    /* Volume label of a "SYS:" disc, empty when the image is not one. */
    std::string discLabel() const;

    FATResult readDirectory(std::vector<DirectoryItem>* dirList,
                            bool includeEmptyEntries = false) const;

    FATResult readFAT(std::map<int, uint8_t>* fatEntries) const;

    /* Directory index of a file, or -1. Name is matched 8.3, case-insensitive. */
    int findFile(const std::string& filename, DirectoryItem* dirItem = nullptr) const;

    /* Size in bytes, rounded up to the sector: the format records a sector
     * count, not an exact length. */
    FATResult fileSize(const DirectoryItem& dirItem, int* sizeBytes) const;

    FATResult exportFile(const std::string& filename, std::vector<uint8_t>& out) const;

    /* ---- Writing ---- */

    /* Fails with ERROR_FILE_ALREADY_EXISTS rather than overwriting; the
     * caller decides whether to delete first. */
    FATResult importFile(const std::string& filename, const std::vector<uint8_t>& data);

    FATResult deleteFile(const std::string& filename);

private:
    FATResult writeFAT(const std::map<int, uint8_t>& fatEntries);
    int  findFreeDirectorySlot() const;
    void readEntry(int index, DirectoryItem* out) const;
    void writeEntry(int index, const DirectoryItem& item);

    std::vector<uint8_t> image;
    std::string loadedPath;
    bool dirty = false;
};

/* Convenience wrappers for reading and writing a host file as raw bytes. */
FATResult sf7ReadHostFile(const std::string& path, std::vector<uint8_t>& out);
FATResult sf7WriteHostFile(const std::string& path, const std::vector<uint8_t>& data);

#endif /* SF7DISC_H */
