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

#ifndef BUILD_INFO_H
#define BUILD_INFO_H

// Which build is this, answered at run time rather than by a macro pasted
// into whichever files happened to be recompiled.
//
// EMULATOR_BUILD is fixed into a translation unit when that unit is compiled.
// Change one source, rebuild, and every file that was already up to date keeps
// the string from whenever it was last compiled - so the binary can report a
// commit it does not contain. It happened: a binary built after a commit still
// announced the commit before it, because the file holding the macro had not
// needed rebuilding.
//
// Worse, "abc1234-dirty" is the same string for every build made from the same
// commit with uncommitted work, which during a working session is all of them.
// Five different binaries, one name.
//
// Both are fixed by putting the answer in one file that is recompiled on every
// build (see the force target in Makefile.common) and asking it at run time.
// The timestamp is what actually distinguishes two builds; the commit says
// where they started from.

// Commit as `git describe` saw it when this build was made, e.g. "d199dad-dirty".
const char* build_info_version(void);

// When this binary was compiled: "31/08/2026 12:34:56". This is the value that
// tells two builds of the same dirty commit apart, and tells a stale .app from
// a fresh one.
const char* build_info_timestamp(void);

// Both together, for a title bar or a one-line banner:
// "d199dad-dirty (31/08/2026 12:34:56)".
const char* build_info_full(void);

#endif /* BUILD_INFO_H */
