/*
** HFELoader.h — HxC Floppy Emulator v1 (HXCPICFE) loader
**
** Loads an HFE file into DISC_DRIVE::trackMap[][].
** MFM decoding is done at load time only — the NEC765 emulator
** never touches raw MFM bits at runtime.
**
** Supports up to 85 tracks (required for Burglar Bill protection:
** 43 tracks instead of the standard 40).
**
** After a successful load, the caller should save the result as a
** .DS7 file (diskSaveDS7) for fast future loads without MFM decoding.
*/
#ifndef HFELOADER_H
#define HFELOADER_H

#include <stdio.h>
#include "DiskModel.h"

class HFELoader {
public:
    /* Returns true if the file starts with the HXCPICFE magic */
    static bool probe(FILE* f);

    /* Load the HFE file into drv->trackMap[][].
     * Sets drv->sides, drv->tracks, and drive timing constants.
     * Returns true on success. */
    static bool load(FILE* f, DISC_DRIVE* drv);

private:
    /* Deinterleave one HFE track chunk stream into side0/side1 byte arrays.
     * Caller owns the returned arrays (malloc'd). */
    static void deinterleave(FILE* f, long trackByteStart,
                             uint16_t trackLenBytes,
                             uint8_t** side0Out, uint8_t** side1Out,
                             int numSides,
                             uint32_t* side0BytesOut);

    /* Decode the raw MFM byte array of one side into a DiskTrack.
     * Finds IDAM/DAM sync patterns and extracts sector data.
     * rawMFMBytes is the deinterleaved byte array for one side.
     * rawByteCount is the size of that array.
     * The returned DiskTrack owns both the sector data AND the rawMFM pointer. */
    static DiskTrack* decodeMFMTrack(uint8_t* rawMFMBytes,
                                     uint32_t rawByteCount,
                                     uint32_t bitsPerRev,
                                     uint8_t  encoding);
};

#endif /* HFELOADER_H */
