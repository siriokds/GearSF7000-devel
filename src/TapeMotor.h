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
