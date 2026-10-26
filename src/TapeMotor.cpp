// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2024-2026  Saverio Russo
#include "TapeMotor.h"

TapeMotor::TapeMotor(Policy policy) : MotorOn(false), m_policy(policy)
{
}

void TapeMotor::SetMotor(bool motorOn)
{
	MotorOn = m_policy == Policy::AlwaysOn || motorOn;
}

bool TapeMotor::IsMotorOn()
{
	return MotorOn;
}
