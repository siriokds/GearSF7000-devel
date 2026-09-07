// SEGA SC-3000 Keyboard Routines

#ifndef _SF7000_H_
#define _SF7000_H_

#include <stdint.h>
#include <SDL3/SDL.h>
#include "definitions.h"

#include "NEC765.h"
#include "Disk.h"
#include "Audio.h"
#include "COM8251A.h"
//#include "Input.h"

class Memory;

class	SF7000
{
private:

	Memory* m_pMemory;
	NEC765* fdc;

	Audio* m_pAudio;
	COM8251A m_usart;

	// USART pending request (thread-safe GUI → emulator handoff)
	struct UsartRequest {
		enum class Type { None, SetSpeed, BackendNone,
		                  BackendLogFile, BackendTcpServer, BackendTcpClient };
		Type     type     = Type::None;
		uint32_t baudRate = 9600;
		uint16_t port     = 9001;
		char     host[64] = {};
		char     logPath[260] = {};
		bool     logHex   = false;
	};
	std::mutex        m_usartReqMtx;
	UsartRequest      m_usartReq;
	std::atomic<bool> m_usartReqPending;  // dirty flag — set by Request*, cleared by ApplyRequest()
	NEC765DebugInfo m_lastFDCDebugInfo{};
	bool m_hasLastFDCDebugInfo = false;
	uint64_t m_fdcPendingClocks = 0;
	uint64_t m_usartPendingClocks = 0;

	void USART_ApplyRequest();   // called by TickCpu() only when pending
	void USART_Synchronize();
	bool FDC_RotationActive() const;
	void FDC_Synchronize();
	void SetPortE6(uint8_t value);
	void CaptureFDCDebugState();
	void PublishFDCChanges();

public:
	uint8_t	Port_E4, Port_E5, Port_E6, Port_E7;
	uint8_t	Port_E8, Port_E9;

	bool	DiskMotorOn;
	uint64_t clocksIndex;
	uint64_t diskRotationClocks;
	uint64_t indexPulseClocks;
	uint32_t cpuClockHz;
	uint32_t fdcClockHz;

	SF7000(Memory* pMemory, Audio* pAudio);
	~SF7000();
	void	Init(Audio* pAudio);
	void	Reset(bool running = true,
			  uint32_t clockRate = GC_MASTER_CLOCK_NTSC,
			  uint32_t fdcClockRate = 8'000'000);

	void	TickCpu(uint64_t clockCycles);
	void	TickFDC(uint64_t clockCycles);
	bool	MotorIsOn();
	void	SetMotorOn(bool motorOn);
	//void	Update(void);

	//--[ NEC FDC765 ]----------------------------------------------------
	void    FDC765_Data_Write(uint8_t value);
	uint8_t FDC765_Data_Read(void);
	uint8_t FDC765_Status_Read(void);

	void	FDC765_DiskChanged(int drive);
	void	FDC765_DiskEject(int drive);
	int		FDC765_IsIndex(int drive);
	void	FDC765_SetReadOnly(int drive, bool readonly);
	void	GetFDCDebugInfo(NEC765DebugInfo* info) const;
	void	GetDiskDebugInfo(DiskDebugInfo* info) const;
	uint32_t GetFDCRotationBits() const;
	uint32_t GetFDCBitsPerRevolution() const;
	uint32_t GetFDCClockHz() const;
	float	GetDiskRotationFraction() const;
	bool	FDCIndexActive() const;

	//--[ P.P.I. 8255A ]--------------------------------------------------
	uint8_t	PPI_ReadPortA();				/* E4: FDC/Printer control */
	uint8_t	PPI_ReadPortB();				/* E5: Printer data output (parallel) */
	uint8_t	PPI_ReadPortC();				/* E6: FDC/Printer control */
	uint8_t	PPI_ReadControl();				/* E7: Control Register */

	int		PPI_WritePortC(uint8_t val);		/* E6: FDC/Printer control */
	int		PPI_WriteControl(uint8_t val);	/* E7: Control Register */

	bool	IPL_Enabled();

	//--[ USART (8251A) ]------------------------------------------------
	uint8_t	USART_ReadData();				/* E8: Data Register  */
	uint8_t	USART_ReadCommand();			/* E9: Status Register */
	void	USART_WriteData(uint8_t v);		/* E8: Data Register  */
	void	USART_WriteCommand(uint8_t v);	/* E9: Control Register */

	// Direct calls — use when emulator is paused (emulator thread only)
	void	USART_SetSpeed(uint32_t baudRate);
	void	USART_SetBackendNone();
	void	USART_SetBackendLogFile(const char* path, bool hexMode = false);
	void	USART_SetBackendTcpServer(uint16_t port);
	void	USART_SetBackendTcpClient(const char* host, uint16_t port);

	// Request calls — thread-safe, applied at next TickCpu() (GUI thread safe)
	void	USART_RequestSpeed(uint32_t baudRate);
	void	USART_RequestBackendNone();
	void	USART_RequestBackendLogFile(const char* path, bool hexMode = false);
	void	USART_RequestBackendTcpServer(uint16_t port);
	void	USART_RequestBackendTcpClient(const char* host, uint16_t port);

	// Inject / poll bytes (thread-safe)
	void	USART_PushRxByte(uint8_t b);
	bool	USART_HasTxData() const;
	uint8_t	USART_PopTxByte();

	// TX callback (called from TickCpu() when a byte is dispatched)
	void	USART_SetTxCallback(std::function<void(uint8_t)> cb);

	// Accessors (GUI read-only)
	uint32_t USART_GetBaudRate()   const  { return m_usart.GetBaudRate(); }
	uint32_t USART_GetCpuClockHz() const  { return m_usart.GetCpuClockHz(); }



	// PPI2 (including the IPL overlay bit), the spindle counters, the NEC765
	// and the USART. Only written when the machine is actually in SF-7000
	// mode: in plain cartridge mode these bytes describe nothing, and the
	// controller's 4 KB sector buffer is the single heaviest thing in a
	// snapshot.
	bool	LoadState(std::istream& stream);
	void	SaveState(std::ostream& stream);
};


#endif /* _SF7000_H_ */
