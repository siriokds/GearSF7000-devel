/*
 * GearSF7000 - diagnostic log
 * Copyright (C) 2026 Saverio Russo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DiagnosticLog.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

namespace
{

std::mutex g_mutex;
FILE* g_file = nullptr;
std::string g_path;
std::string g_previousPath;
std::size_t g_written = 0;
std::size_t g_maxBytes = 4u * 1024u * 1024u;
DiagLevel g_level = DiagLevel::Info;

const char* LevelName(DiagLevel level)
{
    switch (level)
    {
        case DiagLevel::Error: return "ERROR";
        case DiagLevel::Warn:  return "WARN ";
        case DiagLevel::Info:  return "INFO ";
        case DiagLevel::Debug: return "DEBUG";
        default:               return "?????";
    }
}

// Local time with milliseconds. Local rather than UTC on purpose: this is read
// next to a wall clock while something is going wrong, not correlated across
// machines.
void FormatTimestamp(char* out, std::size_t size)
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto seconds = time_point_cast<std::chrono::seconds>(now);
    const auto millis =
        duration_cast<milliseconds>(now - seconds).count();

    const std::time_t raw = system_clock::to_time_t(now);
    std::tm broken{};
#if defined(_WIN32)
    localtime_s(&broken, &raw);
#else
    localtime_r(&raw, &broken);
#endif

    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &broken);
    std::snprintf(out, size, "%s.%03d", stamp, static_cast<int>(millis));
}

void WriteSessionHeader()
{
    if (!g_file)
        return;

    char stamp[64];
    FormatTimestamp(stamp, sizeof(stamp));
    std::fprintf(g_file,
                 "=== GearSF7000 log opened %s ===\n"
                 "=== rotates at %zu bytes, one previous generation kept ===\n",
                 stamp, g_maxBytes);
    std::fflush(g_file);
}

// One generation back, then start again. Two files, so the pair is bounded by
// twice the limit no matter how long a session lasts.
void RotateIfNeeded()
{
    if (!g_file || g_written < g_maxBytes)
        return;

    std::fclose(g_file);
    g_file = nullptr;

    std::remove(g_previousPath.c_str());
    std::rename(g_path.c_str(), g_previousPath.c_str());

    g_file = std::fopen(g_path.c_str(), "w");
    g_written = 0;
    WriteSessionHeader();
}

}

bool DiagnosticLogOpen(const char* directory, const char* filename)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_file)
    {
        std::fclose(g_file);
        g_file = nullptr;
    }

    g_path = (directory ? directory : "");
    g_path += (filename ? filename : "gearsf7000.log");
    g_previousPath = g_path + ".1";

    // Appended, not truncated: a crash on startup would otherwise erase the
    // evidence of the run before it.
    g_file = std::fopen(g_path.c_str(), "a");
    if (!g_file)
    {
        g_path.clear();
        return false;
    }

    std::fseek(g_file, 0, SEEK_END);
    const long size = std::ftell(g_file);
    g_written = size > 0 ? static_cast<std::size_t>(size) : 0;

    WriteSessionHeader();
    return true;
}

void DiagnosticLogClose()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file)
    {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void DiagnosticLogSetLevel(DiagLevel level) { g_level = level; }
DiagLevel DiagnosticLogGetLevel() { return g_level; }

void DiagnosticLogSetMaxBytes(std::size_t bytes)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_maxBytes = bytes < 64u * 1024u ? 64u * 1024u : bytes;
}

const char* DiagnosticLogPath()
{
    return g_path.c_str();
}

void DiagnosticLogWrite(DiagLevel level, const char* category,
                        const char* format, ...)
{
    if (level == DiagLevel::Off || level > g_level)
        return;

    char message[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format ? format : "", args);
    va_end(args);

    char stamp[64];
    FormatTimestamp(stamp, sizeof(stamp));

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file)
        return;

    const int written = std::fprintf(g_file, "%s %s [%-8s] %s\n", stamp,
                                     LevelName(level),
                                     category ? category : "-", message);
    if (written > 0)
        g_written += static_cast<std::size_t>(written);

    // Flushed per line on purpose: the lines worth having are the ones written
    // immediately before the process dies, and a dying process does not flush.
    std::fflush(g_file);

    RotateIfNeeded();
}
