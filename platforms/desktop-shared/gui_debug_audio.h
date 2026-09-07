/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026 Saverio Russo
 *
 * PSG (SN76489) and AY-3-8910/YM2149 debug panels. Split out of gui_debug.cpp
 * once the PSG panel grew a mute control alongside its existing disable, a
 * table layout, a collapsible diagnostics section and a per-channel
 * waveform - the same trajectory gui_debug_mem_import.cpp and
 * gui_debug_rewind.cpp already went through.
 */

#ifndef GUI_DEBUG_AUDIO_H
#define GUI_DEBUG_AUDIO_H

void gui_debug_psg_window(void);
void gui_debug_ay_window(void);

#endif /* GUI_DEBUG_AUDIO_H */
