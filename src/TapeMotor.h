// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2024-2026  Saverio Russo
#pragma once

class TapeMotor
{
public:
	enum class Policy { AlwaysOn, Controlled };
	void SetMotor(bool motorOn);
	bool IsMotorOn();

	explicit TapeMotor(Policy policy = Policy::AlwaysOn);
	virtual ~TapeMotor() = default;

protected:
	bool MotorOn;
	Policy m_policy;

};
