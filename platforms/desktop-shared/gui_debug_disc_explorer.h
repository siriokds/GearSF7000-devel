/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026 Saverio Russo
 *
 * Disc Explorer: a window onto the Sega Disk BASIC filesystem of an .sf7
 * image, built on src/sf7fs. It is a GUI for the sfdisc command-line tool
 * and behaves like it -- the same directory listing, the same FAT map, the
 * same export/import/delete -- so anything sfdisc shows, this shows.
 *
 * It is an analyser, not a file manager: it opens the file itself rather
 * than reading the disc mounted in the emulated drive, it never hides an
 * entry, and it opens images that are not Disk BASIC volumes at all so you
 * can see what they hold instead. The mounted disc is offered only as a
 * shortcut that fills in the path.
 *
 * Writes stay in memory until Save is pressed, so nothing touches the file
 * on disc by accident -- including the image the emulator may have open.
 */

#ifndef GUI_DEBUG_DISC_EXPLORER_H
#define GUI_DEBUG_DISC_EXPLORER_H

void gui_debug_disc_explorer_window(void);

#endif /* GUI_DEBUG_DISC_EXPLORER_H */
