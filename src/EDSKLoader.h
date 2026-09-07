/*
** EDSKLoader.h — Extended DSK loader (Amstrad/CPC format)
**
** Supports both Standard DSK ("MV - CPC") and Extended DSK
** ("EXTENDED CPC DSK File\r\nDisk-Info\r\n").
**
** Produces a populated DISC_DRIVE::trackMap[][] with synthesized
** bit coordinates (synthesizeSectorPositions called internally).
*/
#ifndef EDSKLOADER_H
#define EDSKLOADER_H

#include <stdio.h>
#include "DiskModel.h"

class EDSKLoader {
public:
    /* Returns true if the file starts with a recognised DSK signature */
    static bool probe(FILE* f);

    /* Load the file into drv->trackMap[][].
     * Sets drv->sides, drv->tracks and nominal geometry fields.
     * Returns true on success. */
    static bool load(FILE* f, DISC_DRIVE* drv);

private:
    /* Standard DSK: all tracks have the same fixed size */
    static bool loadStandard(FILE* f, DISC_DRIVE* drv,
                             int tracks, int sides, uint16_t trackSize);

    /* Extended DSK: variable track sizes from the size table */
    static bool loadExtended(FILE* f, DISC_DRIVE* drv,
                             int tracks, int sides, const uint8_t* sizeTable);

    /* Parse one Track Info Block (at current file position) and the sector
     * data that immediately follows it.  Returns a heap-allocated DiskTrack. */
    static DiskTrack* parseTrackBlock(FILE* f, uint32_t bitsPerRev);
};

#endif /* EDSKLOADER_H */
