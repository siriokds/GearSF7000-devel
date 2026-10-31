/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2021  Ignacio Sanchez
 * Copyright (C) 2024-2026  Saverio Russo
 *
 * Derived from Gearcoleco by Ignacio Sanchez. His code is free software: you
 * can redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * The changes and additions by Saverio Russo are free software: you can
 * redistribute them and/or modify them under the terms of the GNU Affero
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 * Section 13 of both licences permits the combination.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License and the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of both licences along with this
 * program.  If not, see http://www.gnu.org/licenses/
 *
 * SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later
 */

#ifndef INPUT_H
#define	INPUT_H

#include "definitions.h"

class CpuInterruptLines;

class Input
{
public:
    enum InputSegments
    {
        SegmentKeypadRightButtons,
        SegmentJoystickLeftButtons
    };

public:
    Input(CpuInterruptLines* interruptLines);
    void Init();
    void Reset();
    // Everything the host holds, released (a return to live play after a load).
    void ReleaseAll();
    void KeyPressed(GC_Controllers controller, GC_Keys key);
    void KeyReleased(GC_Controllers controller, GC_Keys key);
    void Spinner1(int movement);
    void Spinner2(int movement);
    void SaveState(std::ostream& stream);
    void LoadState(std::istream& stream);
    void SetInputSegment(InputSegments segment);
    u8 ReadInput(u8 port);

private:
    CpuInterruptLines* m_interruptLines;
    u8 m_Gamepad[2];
    u8 m_Keypad[2];
    InputSegments m_Segment;
    int m_iSpinnerRel[2];
};

#endif	/* INPUT_H */
