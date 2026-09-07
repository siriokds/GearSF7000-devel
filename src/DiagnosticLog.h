/*
 * GearSF7000 - diagnostic log
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GEARSF7000_DIAGNOSTIC_LOG_H
#define GEARSF7000_DIAGNOSTIC_LOG_H

#include <cstddef>

// A log that exists.
//
// Until now Log() expanded to nothing: it is guarded by DEBUG_GEARSF7000,
// which no build defines. Every diagnostic in the emulator was therefore
// compiled away, which is why a crash left nothing behind to read.
//
// This writes to a file next to the configuration, structured one line per
// event, and bounded: when the file reaches its size limit it is rotated over
// a single previous generation, so the pair can never exceed twice the limit
// however long the emulator runs.
//
// Each line is flushed as it is written. That costs a little, and it is the
// whole point: a process killed by std::terminate or abort() does not get to
// flush anything, so a buffered log would lose exactly the lines that matter.

enum class DiagLevel
{
    Off = 0,
    Error,    // something failed and the caller could not carry on
    Warn,     // something unexpected that was recovered from
    Info,     // lifecycle: media, reset, recorder, MCP transitions
    Debug     // the firehose, including the legacy Log() calls
};

// directory should end with a separator, or be empty for the working
// directory. Safe to call more than once; the previous file is closed.
bool DiagnosticLogOpen(const char* directory, const char* filename);
void DiagnosticLogClose();

// Everything below Info is off by default: a good deal of the existing Log()
// traffic sits in per-frame and per-snapshot paths, and turning it on
// unconditionally would fill the file with sixty lines a second.
void DiagnosticLogSetLevel(DiagLevel level);
DiagLevel DiagnosticLogGetLevel();

// Bytes per file before rotating. Two files are kept.
void DiagnosticLogSetMaxBytes(std::size_t bytes);

const char* DiagnosticLogPath();

void DiagnosticLogWrite(DiagLevel level, const char* category,
                        const char* format, ...);

// The level test happens before the arguments are formatted, so a disabled
// level costs a comparison rather than a vsnprintf.
#define DiagError(category, ...)                                              \
    DiagnosticLogWrite(DiagLevel::Error, category, __VA_ARGS__)
#define DiagWarn(category, ...)                                               \
    DiagnosticLogWrite(DiagLevel::Warn, category, __VA_ARGS__)
#define DiagInfo(category, ...)                                               \
    DiagnosticLogWrite(DiagLevel::Info, category, __VA_ARGS__)
#define DiagDebug(category, ...)                                              \
    do {                                                                      \
        if (DiagnosticLogGetLevel() >= DiagLevel::Debug)                      \
            DiagnosticLogWrite(DiagLevel::Debug, category, __VA_ARGS__);      \
    } while (0)

#endif
