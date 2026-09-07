// ===========================================================================
// COM8251A.cpp  —  Intel 8251A USART emulator for SF-7000
// ===========================================================================
//
// Winsock uses different names and lifetime setup from POSIX sockets. Keep
// the serial backend on their common subset so it also builds on macOS/Linux.
#ifdef _WIN32
#  pragma comment(lib, "Ws2_32.lib")
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <cerrno>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
using SOCKET = int;
static constexpr SOCKET INVALID_SOCKET = -1;
static constexpr int SOCKET_ERROR = -1;
static inline int closesocket(SOCKET socket) { return close(socket); }
static inline int WSAGetLastError() { return errno; }
#  define WSAStartup(...) 0
#  define WSACleanup() 0
#  define MAKEWORD(a, b) 0
using WSADATA = int;
#endif

#include "COM8251A.h"

#include <cstdio>
#include <cstring>
#include <cassert>
#include <algorithm>
#include <ostream>
#include <istream>
#include <chrono>

#ifdef _DEBUG
#  define DBG(fmt, ...) printf("[COM8251A] " fmt "\n", ##__VA_ARGS__)
#else
#  define DBG(fmt, ...)
#endif

// ---------------------------------------------------------------------------
// DSW1 table: external CLK fed to 8251A at each jumper position.
// Crystal 4.9152 MHz → IC36 74LS293 → IC38 4040 → DSW1 tap.
// With x16 baud factor:  baud = DSW1_CLK[pos] / 16.
// ---------------------------------------------------------------------------
const uint32_t COM8251A::DSW1_CLK[7] = {
    0,        // [0] unused
    153600,   // [1]  9600 baud at x16
     76800,   // [2]  4800 baud at x16
     38400,   // [3]  2400 baud at x16
     19200,   // [4]  1200 baud at x16
      9600,   // [5]   600 baud at x16
      4800,   // [6]   300 baud at x16
};

static const uintptr_t INVALID_SOCK = (uintptr_t)INVALID_SOCKET;

// ===========================================================================
// Constructor / Destructor
// ===========================================================================

COM8251A::COM8251A()
    : m_initState(InitState::EXPECT_MODE)
    , m_modeWord(0), m_commandWord(0)
    , m_dataBits(8), m_baudFactor(16), m_bitsPerFrame(10)
    , m_parityEnable(false), m_parityEven(false)
    , m_txEnabled(false), m_rxEnabled(false)
    , m_dtr(false), m_rts(false)
    , m_pe(false), m_oe(false), m_fe(false)
    , m_dsr(true)
    , m_txReady(false), m_txEmpty(true)
    , m_txPending(false), m_txShiftByte(0), m_txClocksLeft(0)
    , m_rxReady(false), m_rxData(0xFF)
    , m_cpuClockHz(3579545)
    , m_baudRate(9600), m_clocksPerFrame(3729), m_rxPollAccum(0)
    , m_rxQueuedCount(0)
    , m_backend(Backend::None)
    , m_logFile(nullptr), m_logHexMode(false)
    , m_listenSocket(INVALID_SOCK), m_clientSocket(INVALID_SOCK)
    , m_tcpPort(0)
    , m_ioRunning(false)
{
    m_tcpHost[0] = '\0';

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

COM8251A::~COM8251A()
{
    StopBackend();
    WSACleanup();
}

// ===========================================================================
// Init / Reset
// ===========================================================================

void COM8251A::Init(int clockRate)
{
    if (clockRate > 0)
        m_cpuClockHz = (uint32_t)clockRate;

    // Hardware state reset (keeps backend)
    m_initState    = InitState::EXPECT_MODE;
    m_modeWord     = 0;  m_commandWord   = 0;
    m_dataBits     = 8;  m_baudFactor    = 16;
    m_bitsPerFrame = 10;
    m_parityEnable = false; m_parityEven = false;
    m_txEnabled    = false; m_rxEnabled  = false;
    m_dtr          = false; m_rts        = false;
    m_pe = m_oe = m_fe = false;
    m_txReady      = false; m_txEmpty    = true;
    m_txPending    = false; m_txClocksLeft = 0;
    m_rxReady      = false; m_rxData     = 0xFF;
    m_rxPollAccum  = 0;

    { std::lock_guard<std::mutex> lk(m_queueMtx);
      while (!m_rxQueue.empty()) m_rxQueue.pop();
      while (!m_txQueue.empty()) m_txQueue.pop();
      m_rxQueuedCount.store(0, std::memory_order_relaxed); }

    UpdateTiming();

    DBG("Init: CPU=%u Hz, baud=%u, frame=%u clocks", m_cpuClockHz, m_baudRate, m_clocksPerFrame);
}

void COM8251A::Reset(int clockRate)
{
    if (clockRate >= 0)
        m_cpuClockHz = (uint32_t)clockRate;

    Init(-1);   // re-init keeping current clock rate

    DBG("Reset");
}

// ===========================================================================
// Timing
// ===========================================================================

void COM8251A::UpdateTiming()
{
    const uint32_t oldClocksPerFrame = m_clocksPerFrame;
    // Baud rate = current baud (set via SetSpeed, or derived from mode/DSW1)
    uint32_t baud = m_baudRate > 0 ? m_baudRate : 9600;

    // CPU clocks per serial frame
    if (baud > 0)
        m_clocksPerFrame = (uint32_t)((uint64_t)m_cpuClockHz * m_bitsPerFrame / baud);
    else
        m_clocksPerFrame = (uint32_t)((uint64_t)m_cpuClockHz * m_bitsPerFrame / 9600);

    if (m_clocksPerFrame == 0)
        m_clocksPerFrame = 1;

    // A live DSW1/mode change alters the duration of the remaining fraction,
    // not the fraction already completed. Init/reset have zeroed both values,
    // while a runtime change is rebased without an audible/visible jump.
    if (oldClocksPerFrame != 0 && oldClocksPerFrame != m_clocksPerFrame)
    {
        m_rxPollAccum =
            (m_rxPollAccum * m_clocksPerFrame) / oldClocksPerFrame;
        if (m_txPending && m_txClocksLeft > 0)
        {
            const uint64_t scaled =
                static_cast<uint64_t>(m_txClocksLeft) * m_clocksPerFrame;
            m_txClocksLeft = static_cast<int32_t>(
                std::min<uint64_t>(INT32_MAX,
                    scaled / oldClocksPerFrame +
                    ((scaled % oldClocksPerFrame) != 0 ? 1u : 0u)));
        }
    }
}

void COM8251A::SetSpeed(uint32_t baud)
{
    // Round to nearest supported DSW1 rate
    static const uint32_t rates[] = { 300, 600, 1200, 2400, 4800, 9600, 0 };
    uint32_t best = 9600;
    uint32_t bestDiff = UINT32_MAX;
    for (int i = 0; rates[i]; i++) {
        uint32_t d = (baud > rates[i]) ? (baud - rates[i]) : (rates[i] - baud);
        if (d < bestDiff) { bestDiff = d; best = rates[i]; }
    }
    m_baudRate = best;
    UpdateTiming();
    DBG("SetSpeed: requested %u → actual %u baud (%u clocks/frame)", baud, m_baudRate, m_clocksPerFrame);
}

// ===========================================================================
// 8251A mode/command parsing
// ===========================================================================

void COM8251A::ParseModeWord(uint8_t v)
{
    m_modeWord = v;

    // bits 1:0 — baud rate factor
    switch (v & 0x03) {
        case 0x01: m_baudFactor =  1; break;
        case 0x02: m_baudFactor = 16; break;
        case 0x03: m_baudFactor = 64; break;
        default:   m_baudFactor = 16; break;  // 0x00 = sync; treat as x16
    }

    // bits 3:2 — character length: 00=5, 01=6, 10=7, 11=8
    m_dataBits = 5 + ((v >> 2) & 0x03);

    // bit 4 — parity enable
    m_parityEnable = (v & 0x10) != 0;

    // bit 5 — even parity
    m_parityEven = (v & 0x20) != 0;

    // bits 7:6 — stop bits (01=1, 10=1.5, 11=2)
    uint8_t stopX2 = 0;
    switch ((v >> 6) & 0x03) {
        case 0x01: stopX2 = 2; break;
        case 0x02: stopX2 = 3; break;
        case 0x03: stopX2 = 4; break;
        default:   stopX2 = 2; break;
    }
    uint8_t stopBits = (stopX2 + 1) / 2;  // round 1.5→2 for timing
    m_bitsPerFrame = 1 + m_dataBits + (m_parityEnable ? 1 : 0) + stopBits;

    UpdateTiming();

    DBG("ParseMode: %02X  bits=%d  par=%s  stop=%s  x%d  baud=%u",
        v, m_dataBits,
        m_parityEnable ? (m_parityEven ? "E" : "O") : "N",
        (stopX2 == 2) ? "1" : (stopX2 == 3) ? "1.5" : "2",
        m_baudFactor, m_baudRate);
}

void COM8251A::ParseCommandWord(uint8_t v)
{
    m_commandWord = v;
    m_txEnabled   = (v & 0x01) != 0;
    m_dtr         = (v & 0x02) != 0;
    m_rxEnabled   = (v & 0x04) != 0;
    // bit 3: SBRK (send break) — not emulated
    if (v & 0x10) m_pe = m_oe = m_fe = false;  // ER: error reset
    m_rts         = (v & 0x20) != 0;
    // bit 6: IR handled in WriteControl
    // bit 7: EH (hunt, sync only) — not emulated

    // TxRDY follows TxEN
    if (m_txEnabled && !m_txPending && m_txClocksLeft == 0)
        m_txReady = true;
    if (!m_txEnabled)
        m_txReady = false;

    DBG("ParseCmd: %02X  TxEN=%d  RxEN=%d  DTR=%d  RTS=%d  ER=%d",
        v, m_txEnabled, m_rxEnabled, m_dtr, m_rts, (v & 0x10) ? 1 : 0);
}

// ===========================================================================
// 8251A status register
// ===========================================================================

uint8_t COM8251A::BuildStatus() const
{
    uint8_t s = 0;
    if (m_txReady) s |= 0x01;
    if (m_rxReady) s |= 0x02;
    if (m_txEmpty) s |= 0x04;
    if (m_pe)      s |= 0x08;
    if (m_oe)      s |= 0x10;
    if (m_fe)      s |= 0x20;
    // bit 6: BRKDET not emulated
    if (m_dsr)     s |= 0x80;
    return s;
}

// ===========================================================================
// Z80 I/O handlers
// ===========================================================================

uint8_t COM8251A::ReadData()
{
    uint8_t val = m_rxData;
    m_rxReady   = false;
    DBG("RX data: $%02X  ('%c')", val, (val >= 0x20 && val < 0x7F) ? (char)val : '.');
    return val;
}

uint8_t COM8251A::ReadStatus()
{
    return BuildStatus();
}

void COM8251A::WriteData(uint8_t v)
{
    if (!m_txEnabled) {
        DBG("TX ignored (TxEN=0): $%02X", v);
        return;
    }

    // Load into shift register, start countdown
    m_txShiftByte  = v;
    m_txPending    = true;
    m_txReady      = false;   // data register busy
    m_txEmpty      = false;
    m_txClocksLeft = (int32_t)m_clocksPerFrame;

    DBG("TX start: $%02X  ('%c')  [%u baud, %u clocks]",
        v, (v >= 0x20 && v < 0x7F) ? (char)v : '.', m_baudRate, m_clocksPerFrame);
}

void COM8251A::WriteControl(uint8_t v)
{
    if (m_initState == InitState::EXPECT_COMMAND && (v & 0x40)) {
        // IR bit in a command word — internal reset → expect MODE next
        DBG("CMD_RESET (IR bit set)");
        m_initState    = InitState::EXPECT_MODE;
        m_txEnabled    = false;
        m_rxEnabled    = false;
        m_txReady      = false;
        m_txEmpty      = true;
        m_txPending    = false;
        m_txClocksLeft = 0;
        m_rxPollAccum  = 0;
        m_rxReady      = false;
        m_pe = m_oe = m_fe = false;
        return;
    }

    if (m_initState == InitState::EXPECT_MODE) {
        ParseModeWord(v);
        m_initState = InitState::EXPECT_COMMAND;
    } else {
        ParseCommandWord(v);
    }
}

// ===========================================================================
// Tick — advance emulation by executedClockCycles CPU clocks
// ===========================================================================

bool COM8251A::HasTimedWork() const
{
    return (m_txPending && m_txClocksLeft > 0) ||
        (m_rxEnabled &&
         m_rxQueuedCount.load(std::memory_order_relaxed) != 0);
}

uint64_t COM8251A::ClocksToNextEvent() const
{
    uint64_t result = UINT64_MAX;
    if (m_txPending && m_txClocksLeft > 0)
        result = static_cast<uint64_t>(m_txClocksLeft);

    if (m_rxEnabled &&
        m_rxQueuedCount.load(std::memory_order_relaxed) != 0)
    {
        const uint64_t rx = m_rxPollAccum < m_clocksPerFrame
            ? m_clocksPerFrame - m_rxPollAccum : 0;
        result = std::min(result, rx);
    }
    return result;
}

void COM8251A::Tick(uint64_t executedClockCycles)
{
    // ---- TX countdown ----
    if (m_txPending && m_txClocksLeft > 0) {
        if (executedClockCycles >= static_cast<uint64_t>(m_txClocksLeft)) {
            m_txClocksLeft = 0;
            // Frame complete: dispatch byte to backend
            DispatchTxByte(m_txShiftByte);
            m_txPending = false;
            m_txReady   = m_txEnabled;
            m_txEmpty   = true;
            DBG("TX complete: $%02X", m_txShiftByte);
        }
        else {
            m_txClocksLeft -= static_cast<int32_t>(executedClockCycles);
        }
    }

    // ---- RX poll from backend queue ----
    // Poll at most once per baud period to simulate realistic receive timing.
    if (!m_rxEnabled ||
        m_rxQueuedCount.load(std::memory_order_relaxed) == 0)
    {
        m_rxPollAccum = 0;
        return;
    }

    // SF7000 normally delivers exactly to the next deadline. The loop also
    // makes direct/coarse callers deterministic and correctly raises OE when
    // several complete frames pass before the CPU reads the previous byte.
    uint64_t completeFrames = executedClockCycles / m_clocksPerFrame;
    uint64_t partialClocks = executedClockCycles % m_clocksPerFrame;
    partialClocks += m_rxPollAccum;
    completeFrames += partialClocks / m_clocksPerFrame;
    partialClocks %= m_clocksPerFrame;
    while (completeFrames != 0) {
        uint8_t b;
        if (!DequeueRx(b))
            break;
        --completeFrames;
        if (m_rxReady)
            m_oe = true;    // overrun: previous byte not read
        m_rxData  = b;
        m_rxReady = true;
        DBG("RX ready: $%02X  ('%c')", b, (b >= 0x20 && b < 0x7F) ? (char)b : '.');
    }

    m_rxPollAccum =
        m_rxQueuedCount.load(std::memory_order_relaxed) != 0
            ? partialClocks : 0;
}

// ===========================================================================
// TX dispatch
// ===========================================================================

void COM8251A::DispatchTxByte(uint8_t b)
{
    // User callback
    if (m_txCallback)
        m_txCallback(b);

    // Backend
    switch (m_backend) {
        case Backend::LogFile:
            LogFileWrite(b);
            break;
        case Backend::TcpServer:
        case Backend::TcpClient:
            TcpSocketWrite(b);
            break;
        case Backend::None:
        default:
            EnqueueTx(b);   // store for polling via PopTxByte()
            break;
    }
}

// ===========================================================================
// PushRxByte / HasTxData / PopTxByte
// ===========================================================================

void COM8251A::PushRxByte(uint8_t b)
{
    EnqueueRx(b);
}

bool COM8251A::HasTxData() const
{
    std::lock_guard<std::mutex> lk(m_queueMtx);
    return !m_txQueue.empty();
}

uint8_t COM8251A::PopTxByte()
{
    std::lock_guard<std::mutex> lk(m_queueMtx);
    if (m_txQueue.empty()) return 0xFF;
    uint8_t b = m_txQueue.front();
    m_txQueue.pop();
    return b;
}

void COM8251A::SetTxCallback(std::function<void(uint8_t)> cb)
{
    m_txCallback = cb;
}

void COM8251A::SetDSR(bool dsr)
{
    m_dsr = dsr;
}

// ===========================================================================
// Queue helpers (thread-safe)
// ===========================================================================

void COM8251A::EnqueueRx(uint8_t b)
{
    std::lock_guard<std::mutex> lk(m_queueMtx);
    m_rxQueue.push(b);
    m_rxQueuedCount.fetch_add(1, std::memory_order_relaxed);
}

bool COM8251A::DequeueRx(uint8_t& b)
{
    std::lock_guard<std::mutex> lk(m_queueMtx);
    if (m_rxQueue.empty()) return false;
    b = m_rxQueue.front();
    m_rxQueue.pop();
    m_rxQueuedCount.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

void COM8251A::EnqueueTx(uint8_t b)
{
    std::lock_guard<std::mutex> lk(m_queueMtx);
    m_txQueue.push(b);
}

bool COM8251A::DequeueTx(uint8_t& b)
{
    std::lock_guard<std::mutex> lk(m_queueMtx);
    if (m_txQueue.empty()) return false;
    b = m_txQueue.front();
    m_txQueue.pop();
    return true;
}

// ===========================================================================
// Backend management
// ===========================================================================

void COM8251A::StopBackend()
{
    // Signal I/O thread to stop and join
    if (m_ioRunning.load()) {
        m_ioRunning.store(false);
        // Close sockets to unblock blocking calls
        if (m_clientSocket != INVALID_SOCK) {
            closesocket((SOCKET)m_clientSocket);
            m_clientSocket = INVALID_SOCK;
        }
        if (m_listenSocket != INVALID_SOCK) {
            closesocket((SOCKET)m_listenSocket);
            m_listenSocket = INVALID_SOCK;
        }
        if (m_ioThread.joinable())
            m_ioThread.join();
    }

    // Close log file
    if (m_logFile) {
        fclose((FILE*)m_logFile);
        m_logFile = nullptr;
    }

    m_backend = Backend::None;
}

void COM8251A::SetBackendNone()
{
    StopBackend();
    DBG("Backend: None");
}

// ---------------------------------------------------------------------------
// Log file backend
// ---------------------------------------------------------------------------

void COM8251A::SetBackendLogFile(const char* path, bool hexMode)
{
    StopBackend();
    FILE* f = fopen(path, "ab");
    if (!f) {
        DBG("SetBackendLogFile: cannot open '%s'", path);
        return;
    }
    m_logFile    = f;
    m_logHexMode = hexMode;
    m_backend    = Backend::LogFile;
    DBG("Backend: LogFile '%s' (%s)", path, hexMode ? "hex" : "raw");
}

void COM8251A::LogFileWrite(uint8_t b)
{
    if (!m_logFile) return;
    FILE* f = (FILE*)m_logFile;
    if (m_logHexMode)
        fprintf(f, "%02X ", b);
    else
        fwrite(&b, 1, 1, f);
    fflush(f);
}

// ---------------------------------------------------------------------------
// TCP server backend
// ---------------------------------------------------------------------------

void COM8251A::SetBackendTcpServer(uint16_t port)
{
    StopBackend();
    m_tcpPort = port;
    m_backend = Backend::TcpServer;

    // Create and bind listening socket
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) {
        DBG("TcpServer: socket() failed: %d", WSAGetLastError());
        m_backend = Backend::None;
        return;
    }
    int opt = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);
    if (bind(ls, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(ls, 1) == SOCKET_ERROR) {
        DBG("TcpServer: bind/listen failed: %d", WSAGetLastError());
        closesocket(ls);
        m_backend = Backend::None;
        return;
    }
    m_listenSocket = (uintptr_t)ls;
    m_clientSocket = INVALID_SOCK;

    m_ioRunning.store(true);
    m_ioThread = std::thread(&COM8251A::TcpServerThreadFunc, this);
    DBG("Backend: TcpServer port %u", port);
}

void COM8251A::TcpServerThreadFunc()
{
    while (m_ioRunning.load())
    {
        // Wait for a client connection
        if (m_clientSocket == INVALID_SOCK) {
            SOCKET ls = (SOCKET)m_listenSocket;
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(ls, &fds);
            timeval tv = { 0, 200000 };   // 200 ms timeout
#ifdef _WIN32
            int r = select(0, &fds, nullptr, nullptr, &tv);
#else
            int r = select(ls + 1, &fds, nullptr, nullptr, &tv);
#endif
            if (r <= 0) continue;

            SOCKET cs = accept(ls, nullptr, nullptr);
            if (cs == INVALID_SOCKET) continue;

            // Disable Nagle for low-latency serial feel
            int flag = 1;
            setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
            m_clientSocket = (uintptr_t)cs;
            DBG("TcpServer: client connected");
        }

        // Service connected client
        SOCKET cs = (SOCKET)m_clientSocket;

        // Try to receive one byte
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(cs, &rfds);
        timeval tv = { 0, 1000 };   // 1 ms: don't hog CPU
#ifdef _WIN32
        if (select(0, &rfds, nullptr, nullptr, &tv) > 0) {
#else
        if (select(cs + 1, &rfds, nullptr, nullptr, &tv) > 0) {
#endif
            uint8_t b;
            int n = recv(cs, (char*)&b, 1, 0);
            if (n == 1) {
                EnqueueRx(b);
            } else if (n <= 0) {
                // Client disconnected
                DBG("TcpServer: client disconnected");
                closesocket(cs);
                m_clientSocket = INVALID_SOCK;
                continue;
            }
        }
    }

    // Cleanup on exit
    if (m_clientSocket != INVALID_SOCK) {
        closesocket((SOCKET)m_clientSocket);
        m_clientSocket = INVALID_SOCK;
    }
    if (m_listenSocket != INVALID_SOCK) {
        closesocket((SOCKET)m_listenSocket);
        m_listenSocket = INVALID_SOCK;
    }
}

// ---------------------------------------------------------------------------
// TCP client backend
// ---------------------------------------------------------------------------

void COM8251A::SetBackendTcpClient(const char* host, uint16_t port)
{
    StopBackend();
    std::strncpy(m_tcpHost, host, sizeof(m_tcpHost) - 1);
    m_tcpHost[sizeof(m_tcpHost) - 1] = '\0';
    m_tcpPort      = port;
    m_clientSocket = INVALID_SOCK;
    m_backend      = Backend::TcpClient;

    m_ioRunning.store(true);
    m_ioThread = std::thread(&COM8251A::TcpClientThreadFunc, this);
    DBG("Backend: TcpClient %s:%u", host, port);
}

void COM8251A::TcpClientThreadFunc()
{
    while (m_ioRunning.load())
    {
        // Connect (or reconnect) if not connected
        if (m_clientSocket == INVALID_SOCK) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            addrinfo hints = {}, *res = nullptr;
            hints.ai_family   = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            char portStr[8];
            snprintf(portStr, sizeof(portStr), "%u", m_tcpPort);
            if (getaddrinfo(m_tcpHost, portStr, &hints, &res) != 0 || !res) {
                closesocket(s);
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
            int cr = connect(s, res->ai_addr, (int)res->ai_addrlen);
            freeaddrinfo(res);
            if (cr == SOCKET_ERROR) {
                closesocket(s);
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
            int flag = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
            m_clientSocket = (uintptr_t)s;
            DBG("TcpClient: connected to %s:%u", m_tcpHost, m_tcpPort);
        }

        // Service socket
        SOCKET cs = (SOCKET)m_clientSocket;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(cs, &rfds);
        timeval tv = { 0, 1000 };
#ifdef _WIN32
        if (select(0, &rfds, nullptr, nullptr, &tv) > 0) {
#else
        if (select(cs + 1, &rfds, nullptr, nullptr, &tv) > 0) {
#endif
            uint8_t b;
            int n = recv(cs, (char*)&b, 1, 0);
            if (n == 1) {
                EnqueueRx(b);
            } else if (n <= 0) {
                DBG("TcpClient: disconnected, will retry");
                closesocket(cs);
                m_clientSocket = INVALID_SOCK;
            }
        }
    }

    if (m_clientSocket != INVALID_SOCK) {
        closesocket((SOCKET)m_clientSocket);
        m_clientSocket = INVALID_SOCK;
    }
}

// ---------------------------------------------------------------------------
// TCP send (called from Tick() — emulator thread)
// ---------------------------------------------------------------------------

void COM8251A::TcpSocketWrite(uint8_t b)
{
    // Note: send() from the emulator thread while I/O thread calls recv().
    // This is safe for non-overlapping send/recv on the same socket (Winsock2).
    if (m_clientSocket == INVALID_SOCK) return;
    send((SOCKET)m_clientSocket, (const char*)&b, 1, 0);
}

// ===========================================================================
// Accessors
// ===========================================================================

uint32_t COM8251A::GetBaudRate()     const { return m_baudRate; }
uint32_t COM8251A::GetCpuClockHz()  const { return m_cpuClockHz; }
uint8_t  COM8251A::GetModeWord()    const { return m_modeWord; }
uint8_t  COM8251A::GetCommandWord() const { return m_commandWord; }
bool     COM8251A::IsTxEnabled()    const { return m_txEnabled; }
bool     COM8251A::IsRxEnabled()    const { return m_rxEnabled; }
bool     COM8251A::HasParityError() const { return m_pe; }
bool     COM8251A::HasOverrunError()const { return m_oe; }
bool     COM8251A::HasFramingError()const { return m_fe; }

// ===========================================================================
// SaveState / LoadState
// ===========================================================================

void COM8251A::SaveState(std::ostream& stream)
{
    auto w8  = [&](uint8_t  v){ stream.write(reinterpret_cast<const char*>(&v), 1); };
    auto w32 = [&](uint32_t v){ stream.write(reinterpret_cast<const char*>(&v), 4); };
    auto wi32= [&](int32_t  v){ stream.write(reinterpret_cast<const char*>(&v), 4); };
    auto wb  = [&](bool     v){ w8(v ? 1u : 0u); };

    w8 (static_cast<uint8_t>(m_initState));
    w8 (m_modeWord);    w8 (m_commandWord);
    w8 (m_dataBits);    w8 (m_baudFactor);   w8 (m_bitsPerFrame);
    wb (m_parityEnable); wb (m_parityEven);
    wb (m_txEnabled);  wb (m_rxEnabled);  wb (m_dtr);   wb (m_rts);
    wb (m_pe);  wb (m_oe);  wb (m_fe);  wb (m_dsr);
    wb (m_txReady);   wb (m_txEmpty);
    wb (m_txPending); w8 (m_txShiftByte); wi32(m_txClocksLeft);
    wb (m_rxReady);   w8 (m_rxData);
    w32(m_cpuClockHz); w32(m_baudRate); w32(m_clocksPerFrame);

    // RX queue snapshot (items not yet delivered to Z80)
    std::lock_guard<std::mutex> lk(m_queueMtx);
    uint16_t rxSz = (uint16_t)m_rxQueue.size();
    stream.write(reinterpret_cast<const char*>(&rxSz), 2);
    std::queue<uint8_t> tmp = m_rxQueue;
    while (!tmp.empty()) { w8(tmp.front()); tmp.pop(); }

    // TX queue snapshot (bytes pending for external polling)
    uint16_t txSz = (uint16_t)m_txQueue.size();
    stream.write(reinterpret_cast<const char*>(&txSz), 2);
    tmp = m_txQueue;
    while (!tmp.empty()) { w8(tmp.front()); tmp.pop(); }
}

void COM8251A::LoadState(std::istream& stream)
{
    auto r8  = [&]() -> uint8_t  { uint8_t  v; stream.read(reinterpret_cast<char*>(&v), 1); return v; };
    auto r32 = [&]() -> uint32_t { uint32_t v; stream.read(reinterpret_cast<char*>(&v), 4); return v; };
    auto ri32= [&]() -> int32_t  { int32_t  v; stream.read(reinterpret_cast<char*>(&v), 4); return v; };
    auto rb  = [&]() -> bool     { return r8() != 0; };

    m_initState    = static_cast<InitState>(r8());
    m_modeWord     = r8();  m_commandWord  = r8();
    m_dataBits     = r8();  m_baudFactor   = r8();  m_bitsPerFrame = r8();
    m_parityEnable = rb();  m_parityEven   = rb();
    m_txEnabled    = rb();  m_rxEnabled    = rb();  m_dtr = rb();  m_rts = rb();
    m_pe = rb();  m_oe = rb();  m_fe = rb();  m_dsr = rb();
    m_txReady      = rb();  m_txEmpty      = rb();
    m_txPending    = rb();  m_txShiftByte  = r8();  m_txClocksLeft = ri32();
    m_rxReady      = rb();  m_rxData       = r8();
    m_cpuClockHz   = r32(); m_baudRate     = r32(); m_clocksPerFrame = r32();

    std::lock_guard<std::mutex> lk(m_queueMtx);
    while (!m_rxQueue.empty()) m_rxQueue.pop();
    uint16_t rxSz; stream.read(reinterpret_cast<char*>(&rxSz), 2);
    for (uint16_t i = 0; i < rxSz; i++) m_rxQueue.push(r8());
    m_rxQueuedCount.store(rxSz, std::memory_order_relaxed);
    // The historical state format did not serialize fractional RX progress.
    m_rxPollAccum = 0;

    while (!m_txQueue.empty()) m_txQueue.pop();
    uint16_t txSz; stream.read(reinterpret_cast<char*>(&txSz), 2);
    for (uint16_t i = 0; i < txSz; i++) m_txQueue.push(r8());
}
