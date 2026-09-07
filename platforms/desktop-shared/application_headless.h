/*
 * GearSF7000 - headless application
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_APPLICATION_HEADLESS_H
#define GEARSF7000_APPLICATION_HEADLESS_H

// The emulator with no window, driven entirely over MCP.
//
// Everything this project is building towards is driven from a client rather
// than from a keyboard: recording, seeking, stepping, reading state. Without a
// window several problems simply stop existing - the machine is no longer
// paced by a compositor, the recorder has no vertical sync to borrow, and two
// identical windows cannot be confused for each other.
//
// Requires an MCP transport: without one there would be no way to talk to it.
int  application_headless_init(const char* rom_file, const char* symbol_file,
                               bool enable_audio);
void application_headless_mainloop(void);
void application_headless_destroy(void);

#endif
