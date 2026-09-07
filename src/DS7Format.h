/*
** DS7Format.h — GearSF7000 native Disk Sector Model format (.ds7)
**
** Binary format that serializes the in-memory Physical Track Model
** (DiskTrack / DiskSector) for fast loading without MFM decoding.
**
** When loading an HFE, EDSK, or flat SF7 image for the first time,
** call DS7Format::save() to create a sidecar .ds7 file.
** Subsequent sessions load from the .ds7 directly.
**
** --- File layout ---
**
** File Header (32 bytes):
**   [0..7]  Magic: "GEARSF7\0"
**   [8]     Version: 1
**   [9]     Tracks   (uint8)
**   [10]    Sides    (uint8)
**   [11]    Encoding (0=MFM, 2=FM)
**   [12-13] Bitrate kbps (uint16 LE)
**   [14-15] RPM (uint16 LE)
**   [16-31] Reserved (0x00)
**
** Track Table (tracks × sides × 8 bytes):
**   Order: (side=0,track=0), (side=0,track=1), ..., (side=1,track=0), ...
**   Per entry:
**   [0] present (1=yes, 0=absent)
**   [1] sectorCount
**   [2] gap3
**   [3] gap4a
**   [4] gap1
**   [5] gap2
**   [6] fillerByte
**   [7] reserved
**
** Track Data (for each present track, same traversal order):
**   Sector headers: sectorCount × 12 bytes
**     [0]   C
**     [1]   H
**     [2]   R
**     [3]   N
**     [4]   ST1
**     [5]   ST2
**     [6]   damType
**     [7]   reserved
**     [8-9] dataLen (uint16 LE)
**     [10-11] reserved
**   Sector data: dataLen bytes (no padding between sectors)
**
** On load: synthesizeSectorPositions() is called automatically.
** Raw MFM bitstream is NOT stored — not needed for emulation.
*/
#ifndef DS7FORMAT_H
#define DS7FORMAT_H

#include <stdio.h>
#include "DiskModel.h"

class DS7Format {
public:
    /* Returns true if the file starts with the GEARSF7 magic */
    static bool probe(FILE* f);

    /* Load a .ds7 file into drv->trackMap[][].
     * Sets drv->sides, drv->tracks, and drive constants.
     * Returns true on success. */
    static bool load(FILE* f, DISC_DRIVE* drv);

    /* Save the current in-memory model to a .ds7 file.
     * Returns true on success. */
    static bool save(const DISC_DRIVE* drv, const char* fileName);

    /* Convenience: derive the .ds7 filename by replacing the extension
     * of sourceFileName.  Result written to outBuf (outBufLen bytes).
     * Returns true if the result fits in the buffer. */
    static bool makeDS7Name(const char* sourceFileName,
                            char* outBuf, int outBufLen);
};

#endif /* DS7FORMAT_H */
