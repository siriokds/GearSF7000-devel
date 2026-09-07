#ifndef _SP400_H_
#define _SP400_H_

#include <stdint.h>
#include <SDL3/SDL.h>
#include "definitions.h"

#include "SPITapePlayer.h"
//#include "Audio.h"
//#include "Input.h"


//	/FEED  PC7
//	/RESET PC6
//	DATA   PC5

//	BUSY   PB6
//	FAULT  PB5


class	SP400
{
private:

	SPITapePlayer tap;

	void SetPortE6(uint8_t value);

public:
	bool FAULT;
	bool BUSY;

	bool DATA;
	bool nRESET;
	bool nFEED;

	SP400();
	~SP400();
	void	Init();
	void	Reset(bool running = true);

	bool	MotorIsOn();
	void	SetMotorOn(bool motorOn);
	//void	Update(void);

	//--[ from P.P.I. 8255A ]--------------------------------------------------
	uint8_t	ReadPortB();
	uint8_t GetPortC() { return (nFEED ? 0x80 : 0x00) | (nRESET ? 0x40 : 0x00) | (DATA ? 0x20 : 0x00); } // PC7, PC6, PC5
	int		WritePortC(uint8_t val);

	void	LoadState(std::istream& stream);
	void	SaveState(std::ostream& stream);
};


#endif /* _SP400_H_ */
