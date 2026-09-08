#include "TapeMotor.h"

TapeMotor::TapeMotor() : MotorOn(false)
{
}

void TapeMotor::SetMotor(bool)
{
	MotorOn = true; // motorOn;
}

bool TapeMotor::IsMotorOn()
{
	return MotorOn;
}
