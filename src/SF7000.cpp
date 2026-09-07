#include "SF7000.h"
#include "NEC765.h"
#include "Memory.h"
#include <cstring>
#include <limits>
#include <sstream>
#include "SaveStateStream.h"

namespace
{
constexpr std::uint16_t kSF7000StateVersion = 1;
}

SF7000::SF7000(Memory* pMemory, Audio* pAudio)
{
	m_pMemory = pMemory;

	fdc = nec765Create();
	nec765SetAudio(fdc, pAudio);

	Reset(false);

	m_usart.SetSpeed(300);                // SF-7000 DSW1 default (ponticello fisico, non cambia al reset)
	// USART_SetBackendTcpServer(9001);   // Bridge TCP seriale: disabilitato temporaneamente
}

SF7000::~SF7000()
{
	if (fdc)
		nec765Destroy(fdc);
}

void SF7000::Init(Audio* pAudio)
{
	m_pAudio = pAudio;
	Reset(false);
}

enum SF7000FDCEventType
{
	SF7000FDCEventCommand = 0x00,
	SF7000FDCEventPhase   = 0x01,
	SF7000FDCEventIRQ     = 0x02,
	SF7000FDCEventTrack   = 0x03
};

void SF7000::CaptureFDCDebugState()
{
	nec765GetDebugInfo(fdc, &m_lastFDCDebugInfo);
	m_hasLastFDCDebugInfo = true;
}

void SF7000::PublishFDCChanges()
{
	NEC765DebugInfo current = {};
	nec765GetDebugInfo(fdc, &current);
	if (!m_hasLastFDCDebugInfo)
	{
		m_lastFDCDebugInfo = current;
		m_hasLastFDCDebugInfo = true;
		return;
	}

	Memory* memory = m_pMemory;
	if (current.commandCode != m_lastFDCDebugInfo.commandCode)
		memory->CheckFDCEvent(SF7000FDCEventCommand,
			m_lastFDCDebugInfo.commandCode, current.commandCode);
	if (current.phase != m_lastFDCDebugInfo.phase)
		memory->CheckFDCEvent(SF7000FDCEventPhase,
			m_lastFDCDebugInfo.phase, current.phase);
	if (current.interrupt != m_lastFDCDebugInfo.interrupt)
		memory->CheckFDCEvent(SF7000FDCEventIRQ,
			m_lastFDCDebugInfo.interrupt, current.interrupt);
	if (current.currentTrack != m_lastFDCDebugInfo.currentTrack)
		memory->CheckFDCEvent(SF7000FDCEventTrack,
			m_lastFDCDebugInfo.currentTrack, current.currentTrack);

	m_lastFDCDebugInfo = current;
}

bool SF7000::FDC_RotationActive() const
{
	return DiskMotorOn && nec765IsDiskPresent(fdc) &&
		nec765IsDiskEnabled(fdc);
}

void SF7000::FDC_Synchronize()
{
	if (m_fdcPendingClocks == 0)
		return;

	nec765Tick(fdc, m_fdcPendingClocks, FDC_RotationActive());
	m_fdcPendingClocks = 0;
	PublishFDCChanges();
}



void SF7000::SetMotorOn(bool motorOn)
{
	// SetMotorOn is also called directly by media eject. Flush the old motor
	// interval here as well as in SetPortE6 so neither entry point loses it.
	FDC_Synchronize();

	if (DiskMotorOn == motorOn)
		return;

	// This is the emulated spindle state, not the state of its optional WAV.
	// Keeping the two separate makes FDC/index timing correct with audio disabled
	// or when the motor sound asset is unavailable.
	DiskMotorOn = motorOn;

	if (m_pAudio == nullptr || m_pAudio->GetWavPlayer() == nullptr)
		return;

	WavChannel* channel = m_pAudio->GetWavPlayer()->GetChannelByName((char*)"DISC_MOTOR");
	if (channel == nullptr)
		return;

	if (motorOn)
	{
		channel->SetupChannel(WavChannel::PlayMode::LOOP, 1.0f);
		channel->PlayChannel();
	}
	else
	{
		channel->StopChannelSlow(150);
	}
}




void SF7000::SetPortE6(uint8_t value)
{
	// Account for all time under the old motor/reset signals before changing
	// their electrical state.
	FDC_Synchronize();

	const uint8_t oldPortE6 = Port_E6;
	bool oldMotor = (Port_E6 & 0x02) == 0;
	const bool oldFdcReset = (Port_E6 & 0x08) != 0;

	Port_E6 = value;
	if (oldPortE6 != Port_E6)
	{
		// Final PPI2 Port-C state after direct or BSR writes. The generic I/O
		// watchpoint still records the raw CPU OUT separately.
		m_pMemory->CheckPPIStateChange(0x00E6, oldPortE6, Port_E6);
	}

	bool motorOn = (Port_E6 & 0x02) == 0;
	const bool fdcReset = (Port_E6 & 0x08) != 0;

#ifdef _DEBUG
	printf("[SF-7000] DISK_MOTOR %d, OLDREG_MOTOR %d, NEWREG_MOTOR: %d, DiskPresent: %d, DiskEnabled: %d\n", 
		DiskMotorOn, oldMotor, motorOn, nec765IsDiskPresent(fdc), nec765IsDiskEnabled(fdc));
#endif


	if (!nec765IsDiskPresent(fdc))
	{
		motorOn = false;
	}

	//if (motorOn && (!nec765IsDiskPresent(fdc) || !nec765IsDiskEnabled(fdc))) {

	//	Port_E6 = (Port_E6 & 0xFD) | (oldMotor & 0x02);

	//	return;
	//}

	SetMotorOn(motorOn);

	// PC3 controls the FDC reset line.  Only its assertion resets the
	// controller; an unrelated PPI write must never teleport the head.
	if (!oldFdcReset && fdcReset)
	{
		nec765Reset(fdc);
		CaptureFDCDebugState();
	}
}

bool SF7000::MotorIsOn()
{
	return (Port_E6 & 0x02) == 0;
}

void SF7000::Reset(bool running, uint32_t clockRate,
	uint32_t fdcClockRate)
{
	cpuClockHz = clockRate ? clockRate : GC_MASTER_CLOCK_NTSC;
	fdcClockHz = fdcClockRate ? fdcClockRate : 8'000'000;
	diskRotationClocks =
		(static_cast<uint64_t>(fdcClockHz) * 60u) / SF7_RPM;
	// Preserve the existing approximately 4.032 ms INDEX aperture.
	indexPulseClocks = (diskRotationClocks * 2016u) / 100000u;
	clocksIndex = 0;
	m_fdcPendingClocks = 0;
	m_usartPendingClocks = 0;
	m_usartReqPending.store(false, std::memory_order_relaxed);

	//FDC765_Init();
	nec765SetClockRate(fdc, fdcClockHz);
	nec765ResetTimebase(fdc);
	nec765Reset(fdc);
	CaptureFDCDebugState();

	Port_E4 = 0x04;
	Port_E5 = 0x00;

	if (running)
	{
		SetPortE6(0x02);
	}
	else
	{
		Port_E6 = 0x02;
		DiskMotorOn = false;
	}

	Port_E7 = 0x00;
	Port_E8 = 0x00;
	Port_E9 = 0x00;

	m_usart.Reset(static_cast<int>(cpuClockHz));
}



int SF7000::FDC765_IsIndex(int drive)
{
	(void)drive;
	return (Port_E4 & 4) ? 1 : 0;
}

void SF7000::USART_ApplyRequest()
{
	UsartRequest req;
	{
		std::lock_guard<std::mutex> lk(m_usartReqMtx);
		req = m_usartReq;
		m_usartReq.type = UsartRequest::Type::None;
	}
	m_usartReqPending.store(false, std::memory_order_relaxed);

	switch (req.type) {
	case UsartRequest::Type::SetSpeed:
		m_usart.SetSpeed(req.baudRate); break;
	case UsartRequest::Type::BackendNone:
		m_usart.SetBackendNone(); break;
	case UsartRequest::Type::BackendLogFile:
		m_usart.SetBackendLogFile(req.logPath, req.logHex); break;
	case UsartRequest::Type::BackendTcpServer:
		m_usart.SetBackendTcpServer(req.port); break;
	case UsartRequest::Type::BackendTcpClient:
		m_usart.SetBackendTcpClient(req.host, req.port); break;
	default: break;
	}
}

void SF7000::USART_Synchronize()
{
	if (m_usartPendingClocks == 0)
		return;
	m_usart.Tick(m_usartPendingClocks);
	m_usartPendingClocks = 0;
}

void SF7000::TickCpu(uint64_t clockCycles)
{
	if (clockCycles == 0)
		return;

	if (m_usartReqPending.load(std::memory_order_relaxed))
	{
		USART_Synchronize();
		USART_ApplyRequest();
	}

	const uint64_t nextUsartEvent = m_usart.ClocksToNextEvent();
	if (m_usart.HasTimedWork())
	{
		if (m_usartPendingClocks >
			std::numeric_limits<uint64_t>::max() - clockCycles)
			USART_Synchronize();
		m_usartPendingClocks += clockCycles;
		if (nextUsartEvent != UINT64_MAX &&
			m_usartPendingClocks >= nextUsartEvent)
			USART_Synchronize();
	}
	else
	{
		m_usartPendingClocks = 0;
	}
}

void SF7000::TickFDC(uint64_t clockCycles)
{
	if (clockCycles == 0)
		return;

	const bool rotationActive = FDC_RotationActive();
	const uint64_t nextFdcEvent =
		nec765ClocksToNextEvent(fdc, rotationActive);
	const bool fdcNeedsTime = rotationActive || nextFdcEvent != UINT64_MAX;
	if (fdcNeedsTime)
	{
		if (m_fdcPendingClocks >
			std::numeric_limits<uint64_t>::max() - clockCycles)
		{
			FDC_Synchronize();
		}
		m_fdcPendingClocks += clockCycles;
	}
	else
	{
		// Idle wall time is not a controller event. Dropping it prevents an
		// inactive SF-7000 from building an arbitrarily large deferred delta.
		m_fdcPendingClocks = 0;
	}

	// This is the existing SF-7000 spindle/index simulator. DiskMotorOn is the
	// effective drive state after SetPortE6 has dealt with media/audio state.
	const bool oldIndex = (Port_E4 & 0x04) != 0;
	if (DiskMotorOn)
	{
		clocksIndex += clockCycles;
		if (diskRotationClocks != 0)
			clocksIndex %= diskRotationClocks;
	
		if (nec765IsDiskPresent(fdc))
		{
			if (clocksIndex < indexPulseClocks)
			{
				this->Port_E4 |= 4;
			}
			else
			{
				this->Port_E4 &= 0xFB;
			}
		}
		else
		{
			this->Port_E4 &= 0xFB;
		}
	}
	else
	{
		// PA2 (INDEX) has no pulses while the spindle is stationary.
		this->Port_E4 &= 0xFB;
	}

	// FDC work is delivered only at a meaningful deadline: a seek step,
	// sector/format completion, or an INDEX edge. Ordinary instruction-sized
	// blocks remain accumulated and are synchronized on the next FDC access.
	const bool indexChanged = oldIndex != ((Port_E4 & 0x04) != 0);
	if (indexChanged ||
		(nextFdcEvent != UINT64_MAX && m_fdcPendingClocks >= nextFdcEvent))
	{
		FDC_Synchronize();
	}
}


void SF7000::FDC765_Data_Write(uint8_t value)
{
	FDC_Synchronize();
	nec765Write(fdc, value);
	PublishFDCChanges();
}

uint8_t SF7000::FDC765_Data_Read(void)
{
	FDC_Synchronize();
	const uint8_t value = nec765Read(fdc);
	PublishFDCChanges();
	return value;
}

uint8_t SF7000::FDC765_Status_Read(void)
{
	FDC_Synchronize();
	return nec765ReadStatus(fdc);
}



void SF7000::FDC765_DiskChanged(int drive)
{
	FDC_Synchronize();
	// Establish a deterministic common index phase for the spindle and the
	// FDC raw-bit clock when media is inserted.
	clocksIndex = 0;
	nec765DiskChanged(fdc, drive);

}


void SF7000::FDC765_DiskEject(int drive)
{
	FDC_Synchronize();
	SetMotorOn(false);

	nec765DiskChanged(fdc, drive);

}

void	SF7000::FDC765_SetReadOnly(int drive, bool readonly)
{
	nec765SetReadOnly(fdc, readonly);
}

void SF7000::GetFDCDebugInfo(NEC765DebugInfo* info) const
{
	nec765GetDebugInfo(fdc, info);
}

void SF7000::GetDiskDebugInfo(DiskDebugInfo* info) const
{
	diskGetDebugInfo(0, info);
}

uint32_t SF7000::GetFDCRotationBits() const
{
	// GUI and MCP inspection must never advance the emulated controller.
	return nec765GetProjectedRotationBits(
		fdc, m_fdcPendingClocks, FDC_RotationActive());
}

uint32_t SF7000::GetFDCBitsPerRevolution() const
{
	return nec765GetBitsPerRevolution(fdc);
}

uint32_t SF7000::GetFDCClockHz() const
{
	return fdcClockHz;
}

float SF7000::GetDiskRotationFraction() const
{
	if (!DiskMotorOn || !nec765IsDiskPresent(fdc) || diskRotationClocks == 0)
		return 0.0f;

	return static_cast<float>(clocksIndex) / static_cast<float>(diskRotationClocks);
}

bool SF7000::FDCIndexActive() const
{
	return (Port_E4 & 0x04) != 0;
}



//===== PPI ============
//======================
//E4: FDC/Printer control
// PA0 = FDC INT : INT signal from input from FDC
// PA1 = BUSY from Centronics printer (Not used)
// PA2 = Pin 17 (INDEX) of the FDC
uint8_t	SF7000::PPI_ReadPortA()
{
	FDC_Synchronize();
	//if (nec765GetIndex(fdc))
	//{
	//	this->Port_E4 ^= 4;	// PA2 (Index)
	//}

		//if (--DiskRotationTime <= 0)
		//{
		//	DiskRotationTime = FDC_ROTATIONTIME;
		//	this->Port_E4 ^= 4;	// PA2 (Index)
		//	clocksIndex = 0;
		//}
		//else
		//{
		//	this->Port_E4 = this->Port_E4;
		//}
		
	//if (!nec765IsDiskPresent(fdc))
	//{
	//	this->Port_E4 &= 0xFB;
	//}

	return this->Port_E4 | nec765GetInt(fdc);// | FDC765_GetIntSignal();
}

/* Printer data output (parallel) */
// Data outputs to Centronics printer
uint8_t	SF7000::PPI_ReadPortB()
{
	return this->Port_E5;
}


/* FDC/Printer control */
//-----------------------
//PC0 = /INUSE signal to FDD
//PC1 = /MOTOR ON signal to FDD
//PC2 = TC signal to FDD
//PC3 = RESET signal to FDC
//PC4 = N.C.
//PC5 = N.C.
//PC6 = /RON SEL:Switching between IPL ROM and RAM
//PC7 = /STROBE to Centronics printer
uint8_t	SF7000::PPI_ReadPortC()
{
	return this->Port_E6;
}

/* Control Register */
uint8_t	SF7000::PPI_ReadControl()
{
	return this->Port_E7;
}

bool SF7000::IPL_Enabled() { return ((this->Port_E6 & 0x40) == 0) ? true : false; }

/* E6: FDC/Printer control */
int		SF7000::PPI_WritePortC(uint8_t val)
{
	int retval = 0;

	SetPortE6(val);

#ifdef _DEBUG
	printf("*** [SF-7000]: Write to Port C\n");

	printf("\t  /INUSE    => %d\n", val & 1);
	printf("\t  /MOTOR ON => %d\n", val & 2);
	printf("\t   TC       => %d\n", val & 4);
	printf("\t   RESET    => %d\n", val & 8);
	printf("\t  /RONSEL   => %d\n", val & 64);
	printf("\t  /STROBE   => %d\n\n", val & 128);
#endif

	return retval;
}


/* E7: FDC/Printer control */
int		SF7000::PPI_WriteControl(uint8_t val)
{
	int retval = 0;

	const uint8_t oldPortE7 = Port_E7;
	Port_E7 = val;
	if (oldPortE7 != Port_E7)
		m_pMemory->CheckPPIStateChange(0x00E7, oldPortE7, Port_E7);
	if (!(val & 0x80))
	{
		uint8_t bitVal = val & 1;
		uint8_t bitIndex = (val >> 1) & 7;
		uint8_t mask = 1 << bitIndex;

		if (bitVal)		SetPortE6(Port_E6 | mask);
		else			SetPortE6(Port_E6 & ~mask);



		int a = 0;

		switch (bitIndex)
		{
			case 0:			// FDD /INUSE
#ifdef _DEBUG
				printf("FDD: /INUSE => %d\n", bitVal);
#endif
				break;

		case 1:				// FDD /MOTORON
				a = 0;
#ifdef _DEBUG
				printf("FDD: /MOTOR ON => %d\n", bitVal);
#endif
				break;



			/*

			TC = Terminal Count

			This signal indicates to FDC765 that data transfer is complete.
			If DMA operational mode is selected for command execution, TC will be qualified by DACK , but not in the programmed I/O execution.
			In PC AT or Special mode, qualification by DACK requires the Operations mode, qualification by DACK requires the operations
			resister signal DMAEN to be logically true.

			Note also that in PC AT mode, TC will be qualified by DACK, whether in DMA or non-DMA host operation. programmed I/O in PC
			AT mode will cause an abnormal termination error at the completion of a command.
			*/

		case 2:			// FDD TC
#ifdef _DEBUG

			if (bitVal) //printf("FDD: TC\n\n");
			//				printf("\nFDD: TC => %d\t", bitVal);
							//if (!bitVal) printf("\n");
#endif
			{
				retval = 1;
				//FDC765_SetTCSignal(bitVal);
				nec765SetTCSignal(fdc, bitVal);
			}
			break;

		case 3:			// FDD RESET (handled on the PC3 edge by SetPortE6)
#ifdef _DEBUG
			printf("FDD: RESET => %d\n", bitVal);
#endif

			break;
		case 6:			// /RONSEL
#ifdef _DEBUG
			printf("/RONSEL => %d\n", bitVal);
#endif
			break;
	
		case 7:			// /STROBE
#ifdef _DEBUG
			printf("/STROBE => %d\n", bitVal);
#endif
			break;



		}
	}
	else
	{
#ifdef _DEBUG
		printf("\n*** [SF-7000 PPI]: Control => $%02X\n", val);
#endif
	}

	return retval;
}







//===== USART (8251A) ==
//======================

uint8_t	SF7000::USART_ReadData()
{
	USART_Synchronize();
	uint8_t val = m_usart.ReadData();
	Port_E8 = val;
#ifdef _DEBUG
	printf("\n*** [USART]: READ DATA: $%02X\n", val);
#endif
	return val;
}

uint8_t	SF7000::USART_ReadCommand()
{
	USART_Synchronize();
	uint8_t val = m_usart.ReadStatus();
	Port_E9 = val;
#ifdef _DEBUG
	printf("\n*** [USART]: READ STATUS: $%02X\n", val);
#endif
	return val;
}

void	SF7000::USART_WriteData(uint8_t val)
{
	USART_Synchronize();
#ifdef _DEBUG
	printf("\n*** [USART]: WRITE DATA: $%02X\n", val);
#endif
	Port_E8 = val;
	m_usart.WriteData(val);
}

void	SF7000::USART_WriteCommand(uint8_t val)
{
	USART_Synchronize();
#ifdef _DEBUG
	printf("\n*** [USART]: WRITE CONTROL: $%02X\n", val);
#endif
	Port_E9 = val;
	m_usart.WriteControl(val);
}

// --- Backend / speed wrappers -------------------------------------------

void	SF7000::USART_SetSpeed(uint32_t baudRate)          { USART_Synchronize(); m_usart.SetSpeed(baudRate); }
void	SF7000::USART_SetBackendNone()                     { m_usart.SetBackendNone(); }
void	SF7000::USART_SetBackendLogFile(const char* path, bool hexMode) { m_usart.SetBackendLogFile(path, hexMode); }
void	SF7000::USART_SetBackendTcpServer(uint16_t port)   { m_usart.SetBackendTcpServer(port); }
void	SF7000::USART_SetBackendTcpClient(const char* host, uint16_t port) { m_usart.SetBackendTcpClient(host, port); }

// --- Thread-safe Request variants (GUI thread) ---------------------------

void SF7000::USART_RequestSpeed(uint32_t baudRate) {
	{ std::lock_guard<std::mutex> lk(m_usartReqMtx);
	  m_usartReq.type = UsartRequest::Type::SetSpeed;
	  m_usartReq.baudRate = baudRate; }
	m_usartReqPending.store(true, std::memory_order_relaxed);
}
void SF7000::USART_RequestBackendNone() {
	{ std::lock_guard<std::mutex> lk(m_usartReqMtx);
	  m_usartReq.type = UsartRequest::Type::BackendNone; }
	m_usartReqPending.store(true, std::memory_order_relaxed);
}
void SF7000::USART_RequestBackendLogFile(const char* path, bool hexMode) {
	{ std::lock_guard<std::mutex> lk(m_usartReqMtx);
	  m_usartReq.type   = UsartRequest::Type::BackendLogFile;
	  m_usartReq.logHex = hexMode;
	  std::strncpy(m_usartReq.logPath, path, sizeof(m_usartReq.logPath) - 1);
	  m_usartReq.logPath[sizeof(m_usartReq.logPath) - 1] = '\0'; }
	m_usartReqPending.store(true, std::memory_order_relaxed);
}
void SF7000::USART_RequestBackendTcpServer(uint16_t port) {
	{ std::lock_guard<std::mutex> lk(m_usartReqMtx);
	  m_usartReq.type = UsartRequest::Type::BackendTcpServer;
	  m_usartReq.port = port; }
	m_usartReqPending.store(true, std::memory_order_relaxed);
}
void SF7000::USART_RequestBackendTcpClient(const char* host, uint16_t port) {
	{ std::lock_guard<std::mutex> lk(m_usartReqMtx);
	  m_usartReq.type = UsartRequest::Type::BackendTcpClient;
	  m_usartReq.port = port;
	  std::strncpy(m_usartReq.host, host, sizeof(m_usartReq.host) - 1);
	  m_usartReq.host[sizeof(m_usartReq.host) - 1] = '\0'; }
	m_usartReqPending.store(true, std::memory_order_relaxed);
}

void	SF7000::USART_PushRxByte(uint8_t b)                { m_usart.PushRxByte(b); }
bool	SF7000::USART_HasTxData() const                    { return m_usart.HasTxData(); }
uint8_t	SF7000::USART_PopTxByte()                          { return m_usart.PopTxByte(); }
void	SF7000::USART_SetTxCallback(std::function<void(uint8_t)> cb) { m_usart.SetTxCallback(cb); }





void SF7000::SaveState(std::ostream& stream)
{
    StateWriter w(stream);

    w.U16(kSF7000StateVersion);

    // 8255 #2. Port E6 bit 6 is the IPL overlay, so these four bytes also
    // decide what the CPU sees in the bottom of the address space.
    w.U8(Port_E4);
    w.U8(Port_E5);
    w.U8(Port_E6);
    w.U8(Port_E7);
    w.U8(Port_E8);
    w.U8(Port_E9);

    // Spindle. The motor takes time to reach speed and the index pulse is
    // derived from an absolute clock, so both counters travel together.
    w.Bool(DiskMotorOn);
    w.U64(clocksIndex);
    w.U64(diskRotationClocks);
    w.U64(indexPulseClocks);
    w.U32(cpuClockHz);
    w.U32(fdcClockHz);
    w.U64(m_fdcPendingClocks);
    w.U64(m_usartPendingClocks);

    // Controller. Length-prefixed so a build or a state without the FDC can
    // step over it cleanly.
    {
        std::stringstream payload(std::ios::in | std::ios::out |
                                  std::ios::binary);
        if (fdc)
            nec765SaveState(fdc, payload);
        const std::string bytes = payload.str();
        w.U32(static_cast<std::uint32_t>(bytes.size()));
        w.Bytes(bytes.data(), bytes.size());
    }

    USART_Synchronize();
    m_usart.SaveState(stream);
}

bool SF7000::LoadState(std::istream& stream)
{
    StateReader r(stream);

    if (r.U16() != kSF7000StateVersion)
        return false;

    const uint8_t e4 = r.U8();
    const uint8_t e5 = r.U8();
    const uint8_t e6 = r.U8();
    const uint8_t e7 = r.U8();
    const uint8_t e8 = r.U8();
    const uint8_t e9 = r.U8();

    const bool motorOn = r.Bool();
    const uint64_t index = r.U64();
    const uint64_t rotation = r.U64();
    const uint64_t indexPulse = r.U64();
    const uint32_t cpuHz = r.U32();
    const uint32_t fdcHz = r.U32();
    const uint64_t fdcPending = r.U64();
    const uint64_t usartPending = r.U64();

    const std::uint32_t fdcBytes = r.U32();
    if (!r.Ok())
        return false;

    std::string fdcPayload;
    if (fdcBytes > 0)
    {
        fdcPayload.resize(fdcBytes);
        r.Bytes(&fdcPayload[0], fdcBytes);
        if (!r.Ok())
            return false;
    }

    Port_E4 = e4;
    Port_E5 = e5;
    Port_E6 = e6;
    Port_E7 = e7;
    Port_E8 = e8;
    Port_E9 = e9;

    DiskMotorOn = motorOn;
    clocksIndex = index;
    diskRotationClocks = rotation;
    indexPulseClocks = indexPulse;
    cpuClockHz = cpuHz;
    fdcClockHz = fdcHz;
    m_fdcPendingClocks = fdcPending;
    m_usartPendingClocks = usartPending;

    if (fdc && !fdcPayload.empty())
    {
        std::stringstream payload(fdcPayload,
                                  std::ios::in | std::ios::binary);
        if (!nec765LoadState(fdc, payload))
            return false;
    }

    m_usart.LoadState(stream);
    return true;
}
