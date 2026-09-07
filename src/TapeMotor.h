#pragma once

class TapeMotor
{
public:
	void SetMotor(bool motorOn);
	bool IsMotorOn();

	TapeMotor();
	virtual ~TapeMotor() = default;

protected:
	bool MotorOn;

};
