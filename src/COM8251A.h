// ===========================================================================
// COM8251A.h  —  Intel 8251A USART emulator for SF-7000
// ===========================================================================
//
// Hardware (SF-7000):
//   Port 0xE8  DATA  —  read: RX byte,  write: TX byte
//   Port 0xE9  CTRL  —  read: status,   write: mode/command word
//
//   Status register bits (read from 0xE9):
//     bit 0  TxRDY    transmitter ready (accepts next byte)
//     bit 1  RxRDY    received byte available
//     bit 2  TxEMPTY  transmitter idle
//     bit 3  PE       parity error
//     bit 4  OE       overrun error
//     bit 5  FE       framing error
//     bit 6  BRKDET   break detected  (not emulated)
//     bit 7  DSR      DSR input pin
//
//   SC-DOS init sequence (8N1 default):
//     OUT (E9), 0x00     sync strobe #1
//     OUT (E9), 0x00     sync strobe #2
//     OUT (E9), 0x40     CMD_RESET → state = EXPECT_MODE
//     OUT (E9), 0x4E     MODE: async×16, 8 data, no parity, 1 stop
//     OUT (E9), 0x37     COMMAND: TxEN+RxEN+DTR+RTS+ER
//
//   Baud rates via SetSpeed() — maps to SF-7000 DSW1 hardware jumper:
//     9600  4800  2400  1200  600  300
//
// Usage:
//   COM8251A usart;
//   usart.Init(3579545);          // CPU clock in Hz (SF-7000 default)
//
//   // Backend (optional, call any time):
//   usart.SetBackendLogFile("serial.log");       // append TX to file
//   usart.SetBackendTcpServer(9600);             // listen on TCP port
//   usart.SetBackendTcpClient("127.0.0.1",9600); // connect to host
//   usart.SetSpeed(9600);                        // baud rate
//
//   // Emulator loop:
//   usart.Tick(executedClockCycles);
//
//   // Z80 port handlers:
//   val  = usart.ReadData();       // IN  A,(0xE8)
//   stat = usart.ReadStatus();     // IN  A,(0xE9)
//   usart.WriteData(val);          // OUT (0xE8),A
//   usart.WriteControl(val);       // OUT (0xE9),A
//
// Thread safety:
//   The TCP backends use a dedicated I/O thread.  All queue accesses
//   between the emulator thread and I/O thread are mutex-protected.
//   SetBackend*(), SetSpeed(), Init(), Reset() are NOT thread-safe —
//   call them only from the emulator (main) thread before the I/O
//   thread is active, or stop the backend first.
// ===========================================================================

#pragma once
#ifndef _COM8251A_H_
#define _COM8251A_H_

#include <cstdint>
#include <functional>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <iosfwd>


class COM8251A
{
public:
    // DSW1 external clock frequencies (Hz) at each jumper position.
    // With x16 mode: baud = DSW1_CLK[pos] / 16.
    // [1]=9600 [2]=4800 [3]=2400 [4]=1200 [5]=600 [6]=300
    static const uint32_t DSW1_CLK[7];

    COM8251A();
    ~COM8251A();

    // -----------------------------------------------------------------------
    // Init — initialise the chip and set the CPU clock rate.
    // clockRate: Z80 CPU clock in Hz (SF-7000 default: 3579545).
    // Call once before the emulator loop.  May be called again to change
    // the clock rate; the emulator state is preserved (no hardware reset).
    // -----------------------------------------------------------------------
    void Init(int clockRate = 3579545);

    // -----------------------------------------------------------------------
    // Reset — hardware reset of the 8251A.
    // clockRate: same as Init(); pass -1 to keep the current rate.
    // Clears all 8251A state and error flags.  Does NOT close the backend.
    // -----------------------------------------------------------------------
    void Reset(int clockRate = -1);

    // -----------------------------------------------------------------------
    // Tick — advance emulation.  Call once per CPU instruction batch.
    // executedClockCycles: number of Z80 CPU cycles since last Tick().
    // -----------------------------------------------------------------------
    void Tick(uint64_t executedClockCycles);
    bool HasTimedWork() const;
    uint64_t ClocksToNextEvent() const;

    // -----------------------------------------------------------------------
    // Z80 I/O — wire to SF7000 port E8/E9 handlers.
    // -----------------------------------------------------------------------
    uint8_t ReadData();              // IN  A,(0xE8)
    uint8_t ReadStatus();            // IN  A,(0xE9)
    void    WriteData(uint8_t v);    // OUT (0xE8),A — send a byte
    void    WriteControl(uint8_t v); // OUT (0xE9),A — mode or command word

    // -----------------------------------------------------------------------
    // SetSpeed — select baud rate (maps to SF-7000 DSW1 hardware jumper).
    // Supported values: 300, 600, 1200, 2400, 4800, 9600.
    // Any other value is rounded to the nearest supported rate.
    // Takes effect immediately; does not reset the 8251A state.
    // -----------------------------------------------------------------------
    void SetSpeed(uint32_t baudRate);

    // -----------------------------------------------------------------------
    // Backends — select where TX bytes go and where RX bytes come from.
    // Only one backend can be active at a time; switching closes the old one.
    // -----------------------------------------------------------------------

    // No backend (default): TX bytes are queued internally (poll via HasTxData).
    void SetBackendNone();

    // Log file: TX bytes appended to a file (text or hex dump).
    //   hexMode = false  →  raw bytes written as-is
    //   hexMode = true   →  "XX " per byte (hex dump)
    void SetBackendLogFile(const char* path, bool hexMode = false);

    // TCP server: listen on the given port, accept one client.
    // The I/O thread handles accept/recv/send transparently.
    void SetBackendTcpServer(uint16_t port);

    // TCP client: connect to host:port.
    // Retries the connection automatically if it drops.
    void SetBackendTcpClient(const char* host, uint16_t port);

    // -----------------------------------------------------------------------
    // Inject a byte for the Z80 to receive (used by non-TCP backends or tests).
    // Thread-safe.
    // -----------------------------------------------------------------------
    void    PushRxByte(uint8_t b);

    // -----------------------------------------------------------------------
    // Poll TX bytes sent by the Z80 (when backend is None).
    // Thread-safe.
    // -----------------------------------------------------------------------
    bool    HasTxData() const;
    uint8_t PopTxByte();

    // -----------------------------------------------------------------------
    // Optional TX callback invoked from Tick() when a byte is dispatched.
    // Used instead of (or in addition to) the backend.
    // -----------------------------------------------------------------------
    void    SetTxCallback(std::function<void(uint8_t)> cb);

    // -----------------------------------------------------------------------
    // DSR input pin state (status bit 7).  Default: true.
    // -----------------------------------------------------------------------
    void    SetDSR(bool dsr);

    // -----------------------------------------------------------------------
    // Accessors (debug / GUI)
    // -----------------------------------------------------------------------
    uint32_t GetBaudRate()     const;
    uint32_t GetCpuClockHz()   const;
    uint8_t  GetModeWord()     const;
    uint8_t  GetCommandWord()  const;
    bool     IsTxEnabled()     const;
    bool     IsRxEnabled()     const;
    bool     HasParityError()  const;
    bool     HasOverrunError() const;
    bool     HasFramingError() const;

    void SaveState(std::ostream& stream);
    void LoadState(std::istream& stream);

private:
    // -----------------------------------------------------------------------
    // 8251A init-state machine
    // -----------------------------------------------------------------------
    enum class InitState : uint8_t { EXPECT_MODE, EXPECT_COMMAND };
    InitState m_initState;

    // -----------------------------------------------------------------------
    // Decoded configuration
    // -----------------------------------------------------------------------
    uint8_t  m_modeWord;
    uint8_t  m_commandWord;
    uint8_t  m_dataBits;       // 5, 6, 7, or 8
    uint8_t  m_baudFactor;     // 1, 16, or 64
    uint8_t  m_bitsPerFrame;   // total bits per frame (start+data+parity+stop)
    bool     m_parityEnable;
    bool     m_parityEven;

    // -----------------------------------------------------------------------
    // Command state
    // -----------------------------------------------------------------------
    bool m_txEnabled;
    bool m_rxEnabled;
    bool m_dtr;
    bool m_rts;

    // -----------------------------------------------------------------------
    // Error flags / input signals
    // -----------------------------------------------------------------------
    bool m_pe, m_oe, m_fe;
    bool m_dsr;

    // -----------------------------------------------------------------------
    // TX state
    // -----------------------------------------------------------------------
    bool     m_txReady;         // TxRDY: data register empty
    bool     m_txEmpty;         // TxEMPTY: shift register also empty
    bool     m_txPending;       // a byte is being shifted out
    uint8_t  m_txShiftByte;     // byte in shift register
    int32_t  m_txClocksLeft;    // countdown for current TX frame

    // -----------------------------------------------------------------------
    // RX state
    // -----------------------------------------------------------------------
    bool    m_rxReady;          // RxRDY: unread byte in data register
    uint8_t m_rxData;           // RX data register

    // -----------------------------------------------------------------------
    // Timing
    // -----------------------------------------------------------------------
    uint32_t m_cpuClockHz;      // Z80 CPU clock (set by Init/Reset)
    uint32_t m_baudRate;        // current baud rate
    uint32_t m_clocksPerFrame;  // CPU clocks per serial frame
    uint64_t m_rxPollAccum;     // always normalized below one RX frame

    // -----------------------------------------------------------------------
    // Internal queues (thread-safe via m_queueMtx)
    // -----------------------------------------------------------------------
    mutable std::mutex      m_queueMtx;
    std::queue<uint8_t>     m_rxQueue;   // backend → Z80
    std::queue<uint8_t>     m_txQueue;   // Z80 → backend (when backend=None)
    std::atomic<uint64_t>   m_rxQueuedCount;

    // -----------------------------------------------------------------------
    // TX callback
    // -----------------------------------------------------------------------
    std::function<void(uint8_t)> m_txCallback;

    // -----------------------------------------------------------------------
    // Backend
    // -----------------------------------------------------------------------
    enum class Backend { None, LogFile, TcpServer, TcpClient };
    Backend  m_backend;

    // Log file
    void*    m_logFile;         // FILE* (void* to avoid stdio.h in header)
    bool     m_logHexMode;

    // TCP (Winsock2 types stored as uintptr_t to avoid including winsock2.h)
    uintptr_t         m_listenSocket;   // server listen socket
    uintptr_t         m_clientSocket;   // connected client / client-mode socket
    uint16_t          m_tcpPort;
    char              m_tcpHost[64];

    std::thread       m_ioThread;
    std::atomic<bool> m_ioRunning;

    // -----------------------------------------------------------------------
    // Private helpers
    // -----------------------------------------------------------------------
    void ParseModeWord(uint8_t v);
    void ParseCommandWord(uint8_t v);
    void UpdateTiming();
    uint8_t BuildStatus() const;

    void DispatchTxByte(uint8_t b);     // called from Tick() when TX frame ends

    void StopBackend();                  // close current backend cleanly

    void LogFileWrite(uint8_t b);
    void TcpServerThreadFunc();
    void TcpClientThreadFunc();
    void TcpSocketWrite(uint8_t b);     // send one byte to connected socket

    // Thread-safe queue helpers
    void  EnqueueRx(uint8_t b);
    bool  DequeueRx(uint8_t& b);
    void  EnqueueTx(uint8_t b);
    bool  DequeueTx(uint8_t& b);
};

#endif // _COM8251A_H_
