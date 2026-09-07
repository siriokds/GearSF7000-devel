/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026 Saverio Russo

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 *
 */

// This file is deliberately recompiled by every build, so __DATE__ and
// __TIME__ are the moment the binary was made rather than the moment this
// file last changed. Keep it small and dependency-free: everything it
// includes is recompiled as often as it is.

#include <cstdio>
#include "build_info.h"

#ifndef EMULATOR_BUILD
#define EMULATOR_BUILD "undefined"
#endif

namespace
{

// __DATE__ is "Aug 31 2026" with the day space-padded, which sorts badly and
// reads as American. Rewritten once, on first use, into 31/08/2026.
const char* const kMonths[12] =
    { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

char g_timestamp[32] = { 0 };
char g_full[96] = { 0 };

void build_timestamp_once(void)
{
    if (g_timestamp[0] != 0)
        return;

    const char* date = __DATE__;   // "Aug 31 2026" / "Aug  1 2026"
    const char* time = __TIME__;   // "12:34:56"

    int month = 0;
    for (int i = 0; i < 12; ++i)
    {
        if (date[0] == kMonths[i][0] && date[1] == kMonths[i][1] &&
            date[2] == kMonths[i][2])
        {
            month = i + 1;
            break;
        }
    }

    const int day = (date[4] == ' ' ? 0 : (date[4] - '0') * 10) + (date[5] - '0');
    const int year = (date[7] - '0') * 1000 + (date[8] - '0') * 100 +
                     (date[9] - '0') * 10 + (date[10] - '0');

    // A month of 0 means the compiler handed us something other than the
    // documented __DATE__ format. Say so rather than printing 00/.
    if (month == 0)
        snprintf(g_timestamp, sizeof(g_timestamp), "%s %s", date, time);
    else
        snprintf(g_timestamp, sizeof(g_timestamp), "%02d/%02d/%04d %s",
                 day, month, year, time);
}

} // namespace

const char* build_info_version(void)
{
    return EMULATOR_BUILD;
}

const char* build_info_timestamp(void)
{
    build_timestamp_once();
    return g_timestamp;
}

const char* build_info_full(void)
{
    if (g_full[0] == 0)
    {
        build_timestamp_once();
        snprintf(g_full, sizeof(g_full), "%s (%s)", EMULATOR_BUILD, g_timestamp);
    }
    return g_full;
}
