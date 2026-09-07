#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string>
#include <filesystem>


struct BasicProgramHeader
{
public:
	uint8_t KeyCode;
	uint8_t Filename[16];
	uint8_t ProgramLength_Hi;
	uint8_t ProgramLength_Lo;
	uint8_t CRC;
	uint8_t Filler[2];
};

struct BasicProgramBody
{
public:
	uint8_t KeyCode;
	uint8_t* ProgramData;
	uint8_t CRC;
	uint8_t Filler[2];
};



uint8_t CalcBasicHeaderCRC(BasicProgramHeader* header)
{
	uint8_t sum = 0;

	for (int i = 0; i < sizeof(header->Filename); i++)
		sum += header->Filename[i];

	sum += header->ProgramLength_Hi;
	sum += header->ProgramLength_Lo;

	return 256 - sum;
}

uint8_t CalcBasicProgramCRC(BasicProgramBody* body, int length)
{
	uint8_t sum = 0;

	if (body->ProgramData != 0)
	{
		for (int i = 0; i < length; i++)
			sum += body->ProgramData[i];
	}

	return 256 - sum;
}



#define BITWRITER_SILENCE	0x20
#define BITWRITER_ZERO		0x30
#define BITWRITER_ONE		0x31

// SC-3000 BASIC Level III V1.1 cassette format: each FSK bit lasts
// 833.3 microseconds, so the inter-block no-recording interval is 1200 bits.
#define BITWRITER_INTER_BLOCK_SILENCE_BITS 1200
// The ROM reader accepts a leader after 256 pilot oscillations.  A shorter
// generated .bit remains faithful to the waveform while avoiding a needless
// three-second wait; existing files may of course contain longer leaders.
#define BITWRITER_LEADER_BITS 256

class BitWriter
{
public:


	void Write(uint8_t byte, FILE *fp)
	{
		int mask = 0x01;

		uint8_t bit = BITWRITER_ZERO;			
		fwrite(&bit, sizeof(uint8_t), 1, fp);	// Start Bit (0)

		for (int i = 0; i < 8; i++)				// D0, D1, D2, D3, D4, D5, D6, D7
		{
			uint8_t bit = byte & mask ? BITWRITER_ONE : BITWRITER_ZERO;

			fwrite(&bit, sizeof(uint8_t), 1, fp);

			mask <<= 1;
		}

		bit = BITWRITER_ONE;					
		fwrite(&bit, sizeof(uint8_t), 1, fp);	// Stop Bit (1)
		fwrite(&bit, sizeof(uint8_t), 1, fp);	// Stop Bit (1)
	}

	void Write(uint8_t* bytePtr, int len, FILE* fp)
	{
		for (int i = 0; i < len; i++)
		{
			Write(bytePtr[i], fp);
		}
	}

	void Fill(char ch, int len, FILE* fp)
	{
		for (int i = 0; i < len; i++)
			fwrite(&ch, sizeof(uint8_t), 1, fp);
	}

};


class BasicProgram
{
private: 
	BasicProgramHeader header;
	BasicProgramBody body;

public:
	BasicProgram() 
	{ 
		header.KeyCode = 0x16; 
		for (int i = 0; i < sizeof(header.Filename); i++) header.Filename[i] = ' ';
		header.ProgramLength_Hi = 0;
		header.ProgramLength_Lo = 0;
		header.CRC = 0xFF;
		header.Filler[0] = header.Filler[1] = 0;

		body.KeyCode = 0x17;
		body.ProgramData = 0;
		body.CRC = 0xFF;
		body.Filler[0] = body.Filler[1] = 0;
	}

	~BasicProgram()
	{
		if (body.ProgramData != 0)
			delete[] body.ProgramData;
	}

	bool LoadFromProgram(std::string programName, std::filesystem::path filePath)
	{
		FILE* fp = fopen(filePath.string().c_str(), "rb");
		if (fp)
		{
			size_t len = LoadFromProgram(programName, fp);
			fclose(fp);

			return len > 0;
		}

		return false;
	}

	size_t LoadFromProgram(std::string programName, FILE* fp)
	{
		size_t obtained = 0;

		int nameLength = programName.length();
		if (nameLength > 16) nameLength = 16;
		
		for (int i = 0; i < 16; i++)
			header.Filename[i] = ' ';

		for (int i = 0; i < nameLength; i++)
			header.Filename[i] = programName[i];

		fseek(fp, 0, 2);
		long fileLength = ftell(fp);
		fseek(fp, 0, 0);
		if (fileLength > 65535) fileLength = 65535;

		header.ProgramLength_Lo = (uint8_t)(fileLength & 255);
		header.ProgramLength_Hi = (uint8_t)((fileLength >> 8) & 255);
		header.CRC = CalcBasicHeaderCRC(&header);

		if (body.ProgramData)
			delete[] body.ProgramData;

		body.ProgramData = new uint8_t[fileLength];

		obtained = fread(body.ProgramData, sizeof(uint8_t), fileLength, fp);
		fseek(fp, 0, 0);

		if (obtained == fileLength)
		{
			body.CRC = CalcBasicProgramCRC(&body, fileLength);
		}
		else
		{
			body.CRC = 0;
		}
		

		return obtained;
	}

	long SaveToAsciiBit(FILE* fp)
	{
		BitWriter bitwriter;

		bitwriter.Fill(BITWRITER_SILENCE, BITWRITER_INTER_BLOCK_SILENCE_BITS, fp);

		bitwriter.Fill(BITWRITER_ONE, BITWRITER_LEADER_BITS, fp);
		bitwriter.Write(header.KeyCode, fp);
		bitwriter.Write(header.Filename, 16, fp);
		bitwriter.Write(header.ProgramLength_Hi, fp);
		bitwriter.Write(header.ProgramLength_Lo, fp);
		bitwriter.Write(header.CRC, fp);
		bitwriter.Write(header.Filler, 2, fp);

		bitwriter.Fill(BITWRITER_SILENCE, 1200, fp);

		int programLength = ((int)header.ProgramLength_Hi << 8) | (int)(header.ProgramLength_Lo);

		bitwriter.Fill(BITWRITER_ONE, BITWRITER_LEADER_BITS, fp);
		bitwriter.Write(body.KeyCode, fp);
		bitwriter.Write(body.ProgramData, programLength, fp);
		bitwriter.Write(body.CRC, fp);
		bitwriter.Write(body.Filler, 2, fp);

		fseek(fp, 0, 2);
		long len = ftell(fp);
		fseek(fp, 0, 0);

		return len;
	}
};
