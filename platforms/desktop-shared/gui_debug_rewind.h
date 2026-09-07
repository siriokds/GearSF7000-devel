/*
 * GearSF7000 - frame recorder window
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_GUI_DEBUG_REWIND_H
#define GEARSF7000_GUI_DEBUG_REWIND_H

void gui_debug_window_rewind(void);

// One frame back and one frame forward along the recording, for the keyboard
// shortcuts. Both leave the machine paused on the frame they land on.
bool gui_debug_rewind_step_back(void);
bool gui_debug_rewind_step_forward(void);
bool gui_debug_rewind_resume_from_here(void);

// Discard the recorded timeline without changing whether capture is enabled.
// Shared by the Recorder window and its Debug menu.
void gui_debug_rewind_reset_data(void);

#endif
