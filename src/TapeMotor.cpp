#include "TapeMotor.h"

TapeMotor::TapeMotor() : MotorOn(false)
{
}

void TapeMotor::SetMotor(bool motorOn)
{
	MotorOn = true; // motorOn;
	(void)motorOn; // unused
}

bool TapeMotor::IsMotorOn()
{
	return MotorOn;
}
