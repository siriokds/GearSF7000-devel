#include "SP400.h"
#include "SPITapePlayer.h"


SP400::SP400()
{
	Reset(false);
}

SP400::~SP400()
{
}

void SP400::Init()
{
	Reset(false);
}

void SP400::Reset(bool running)
{
	FAULT = false;
	BUSY = false;

	DATA = false;
	nRESET = false;
	nFEED = false;

}

uint8_t	SP400::ReadPortB() 
{ 
	uint8_t val = 0xC0;

	val = tap.readPortB();

	BUSY = (val & 0x40) != 0; // PB6
	FAULT = (val & 0x20) != 0; // PB5


	return (BUSY ? 0x40 : 0x00) | (FAULT ? 0x20 : 0x00); 
}

int		SP400::WritePortC(uint8_t val) 
{ 
	bool t_nFEED = (val & 0x80) != 0; // PC7
	bool t_nRESET = (val & 0x40) != 0; // PC6
	bool t_DATA = (val & 0x20) != 0; // PC5

	//if (t_nFEED == nFEED && t_nRESET == nRESET && t_DATA == DATA)
	//	return 0; // No change



//#ifdef _DEBUG
//	printf("*** [SP-400]: Write to Port C\n");
//
//	printf("\t  /FEED  (SCLK) => %d\n", val & 0x80);
//	printf("\t  /RESET (MOSI) => %d\n", val & 0x40);
//	printf("\t  DATA   (CS)   => %d\n", val & 0x20);
//	printf("\t  BIT COUNTER:  => %d\n", tap.GetBitCounter());
//#endif



	nFEED = (val & 0x80) != 0; // PC7
	nRESET = (val & 0x40) != 0; // PC6
	DATA = (val & 0x20) != 0; // PC5



	tap.writePortC(val);

	return 0; 
}
