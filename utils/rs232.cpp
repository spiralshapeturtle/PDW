#include <windows.h>
#include <winbase.h>
#include "..\headers\slicer.h"
#include "..\utils\debug.h"
#include "..\utils\debuglog.h"
#include "..\utils\ostype.h"
#include "..\headers\pdw.h"
#include "rs232.h"
#include "telnet_server.h"

#define SLICER_BUFSIZE 10000

// Cross-thread state. Accessed from both the main thread (rs232_connect /
// rs232_disconnect) and the RxThread worker — declared volatile so the
// compiler doesn't hoist reads out of RxThread's loop.
volatile HANDLE m_hRxThread = INVALID_HANDLE_VALUE;
ULONG WINAPI RxThread(LPVOID pCl);
volatile BOOL   m_bConnectedToComport = FALSE;
volatile HANDLE m_ComPortHandle = INVALID_HANDLE_VALUE;
DWORD           m_dwThreadId = 0;
volatile BOOL   bKeepThreadAlive;

// Serializes open/close transitions on m_ComPortHandle. The main thread can
// call rs232_disconnect while RxThread is mid-rs232_worker_reopen — without
// this, the two paths can double-close (one CloseHandle racing the other) or
// leak (worker writes a fresh handle the main thread won't see and won't
// close). ReadFile/WriteFile do NOT take this lock: Windows handles their
// own kernel-side refcounting, so a ReadFile against a just-closed handle
// returns an error instead of crashing.
static CRITICAL_SECTION g_handleCs;
static BOOL             g_handleCsInited = FALSE;
static inline void rs232_ensure_handle_cs(void) {
	// Called from the main thread inside rs232_connect before RxThread starts,
	// so single-threaded entry is guaranteed on first init.
	if (!g_handleCsInited) {
		InitializeCriticalSection(&g_handleCs);
		g_handleCsInited = TRUE;
	}
}

// FIX [L2]: release the CRITICAL_SECTION on final shutdown (called from WM_DESTROY).
void rs232_cleanup(void)
{
	if (g_handleCsInited) {
		DeleteCriticalSection(&g_handleCs);
		g_handleCsInited = FALSE;
	}
}
double  nTiming ;	// was int
BOOL    bOrgcomPortRS232 ;
BOOL    bSlicerDriver = FALSE;

WORD  rs232_freqdata[SLICER_BUFSIZE] ;
BYTE  rs232_linedata[SLICER_BUFSIZE] ;
// Producer: rs232_read / slicer_read on RxThread.
// Consumer: decode.cpp via the cpstn pointer wired up in PDW.cpp (slicer_out.cpstn).
// Declared volatile so the compiler can't hoist reads in either side's polling loops,
// and so the store ordering established by _WriteBarrier() in the writers is honored
// at the language level. On x86/x64 TSO this is essentially a compiler hint; the fence
// is required on weakly-ordered architectures.
volatile DWORD rs232_cpstn;

// Overrun telemetry. We do not have direct visibility into decode.cpp's consumer
// position (pd_i), so we approximate: count wraps and log when the wrap interval
// suggests the consumer is falling behind. At 19200 baud the buffer holds ~0.5 s of
// samples; a wrap interval under 200 ms means the consumer is more than 2 s behind
// real-time and is dropping data.
static volatile ULONGLONG g_lastWrapTickMs = 0;
static volatile DWORD g_overrunWarnCount = 0;

BYTE  byRS232Data[SLICER_BUFSIZE * (sizeof(WORD) + sizeof(BYTE))] ;

// Worker self-reconnect state: when the Moxa NPort (or any virtual COM
// redirector) silently drops its TCP tunnel, ReadFile keeps returning
// success with 0 bytes. The watchdog in RxThread detects this and tears
// down + reopens the handle without involving the main thread.
static int            g_comPortNumber  = 0;
static volatile ULONGLONG g_lastDataTickMs = 0;
// g_connectTickMs is set on every rs232_connect() and rs232_worker_reopen()
// so the stall watchdog can suppress itself during the Moxa TCP warmup period.
static volatile ULONGLONG g_connectTickMs  = 0;
#define RS232_STALL_MS              5000u
#define RS232_RECONNECT_BACKOFF_MS  2000u
// Moxa NPort needs 1-4 s after CreateFile to start delivering data.
// Don't fire the stall watchdog until this much time has elapsed since
// the last connect/reconnect.
#define RS232_WARMUP_MS             6000u
// When ReadFile reports the device is physically gone (USB-COM unplugged,
// Moxa box powered off) there is no point hammering CreateFile every 50 ms.
// Back off to this interval until the device shows up again.
#define RS232_DEVICE_GONE_MS        30000u

// Last GetLastError() value reported by rs232_read / slicer_read. RxThread
// inspects this to decide whether the disconnection is "transient" (just
// reopen) or "device removed" (long backoff).
static volatile DWORD g_lastReadError = 0;

// FIX [RxWedgeDiag]: observe-only diagnostic to pinpoint a producer freeze.
// RxThread/slicer_read/rs232_read stamp g_rxPhase at each stage; a separate
// low-rate thread (RxDiagThread) logs the current phase + how long it has been
// held + data-age. If the producer wedges, the log makes the cause conclusive:
//   phase stuck on READFILE  -> blocked in ReadFile (Moxa TCP tunnel wedged)
//   phase stuck on TSNOTIFY  -> blocked on g_tsCs (telnet wire-log disk write)
//   phase stuck on REOPEN    -> blocked in rs232_worker_reopen
// Everything here is gated behind Profile.bDebugLog and never touches the
// decode/ring path; it only reads volatiles already maintained by the worker.
enum { RXP_SLEEP = 0, RXP_READFILE = 1, RXP_PARSE = 2, RXP_TSNOTIFY = 3, RXP_REOPEN = 4 };
static volatile LONG      g_rxPhase      = RXP_SLEEP;
static volatile ULONGLONG g_rxPhaseMs    = 0;   // tick when phase last changed
static volatile DWORD     g_rxLoops      = 0;   // RxThread loop iteration counter
static volatile HANDLE    g_rxDiagThread = INVALID_HANDLE_VALUE;
static volatile BOOL      g_rxDiagRun    = FALSE;

static inline void RxSetPhase(LONG p) { g_rxPhase = p; g_rxPhaseMs = GetTickCount64(); }

static DWORD WINAPI RxDiagThread(LPVOID)
{
	static const char *kPhase[] = {
		"SLEEP", "READFILE", "PARSE", "TSNOTIFY(g_tsCs+wirelog)", "REOPEN"
	};
	DWORD lastLoops = (DWORD)-1;
	while (g_rxDiagRun) {
		// ~2 s cadence, but poll the stop flag every 100 ms so disconnect is snappy.
		for (int i = 0; i < 20 && g_rxDiagRun; i++) Sleep(100);
		if (!g_rxDiagRun) break;
		if (!Profile.bDebugLog) { lastLoops = (DWORD)-1; continue; }

		ULONGLONG now = GetTickCount64();
		LONG ph = g_rxPhase;
		const char *name = (ph >= RXP_SLEEP && ph <= RXP_REOPEN) ? kPhase[ph] : "?";
		DWORD loops = g_rxLoops;
		// "frozen" = the worker has not advanced its loop counter since last tick.
		const char *flag = (loops == lastLoops) ? "  <-- NO PROGRESS" : "";
		lastLoops = loops;

		DebugLog("[RxDiag] phase=%s heldMs=%llu dataAgeMs=%llu loops=%lu err=%lu connAgeMs=%llu%s",
			name, now - g_rxPhaseMs, now - g_lastDataTickMs,
			(unsigned long)loops, (unsigned long)g_lastReadError,
			now - g_connectTickMs, flag);
	}
	return 0;
}

// NOTE [AssertNoOp]: 'assert' is DELIBERATELY redefined to a non-aborting, log-only check. This is
// intentional for a long-running unattended daemon: a failed assertion logs to the debug channel
// (DebugView) but must NEVER call abort()/terminate() and kill the process. Standard <assert.h>
// semantics do NOT apply here — do not assume a failing assert stops execution.
#define assert(a)		if(!(a))  { OUTPUTDEBUGMSG(("SIMULATE ASSERT in file %s at %d\n", __FILE__, __LINE__ )); }

// Apply our standard DCB settings and read timeouts to m_ComPortHandle.
// Factored out of rs232_connect so the worker-thread self-reconnect path
// can use it too. Always sets timeouts (previously only set when in raw
// RS232 mode — slicer mode could block indefinitely on ReadFile).
// FIX [ComPortReopenLock]: operate on a caller-supplied handle (was the global m_ComPortHandle)
// so the worker-reconnect path can configure a freshly-opened port BEFORE publishing it into
// m_ComPortHandle under the lock — keeping every blocking-prone serial call out of g_handleCs.
static int rs232_apply_dcb_and_timeouts(HANDLE h)
{
	DCB m_comDCB = {};
	COMMTIMEOUTS ComTimeOuts = {};

	if (h == INVALID_HANDLE_VALUE) return RS232_NO_DUT;

	if (!GetCommState(h, &m_comDCB)) {
		DebugLog("[rs232_apply_dcb] GetCommState failed: %08lX", GetLastError());
		return RS232_NO_DUT;
	}
	m_comDCB.BaudRate    = bOrgcomPortRS232 ? CBR_19200 : (nOSType == OS_WIN2000) ? CBR_SLICER_2K : CBR_SLICER_XP;
	m_comDCB.ByteSize    = 8;
	m_comDCB.Parity      = NOPARITY;
	m_comDCB.StopBits    = ONESTOPBIT;
	m_comDCB.fBinary     = TRUE;
	m_comDCB.fParity     = FALSE;
	m_comDCB.fDtrControl = DTR_CONTROL_DISABLE;
	m_comDCB.fRtsControl = bOrgcomPortRS232 ? RTS_CONTROL_DISABLE : RTS_CONTROL_ENABLE;
	// Explicitly disable every flow-control mechanism. Without this, GetCommState
	// returns driver defaults (some FTDI / USB-COM drivers default fOutxCtsFlow=TRUE)
	// which then make ReadFile block indefinitely waiting for CTS. fAbortOnError must
	// be FALSE so a single framing error doesn't lock the port into a fault state.
	m_comDCB.fOutxCtsFlow    = FALSE;
	m_comDCB.fOutxDsrFlow    = FALSE;
	m_comDCB.fDsrSensitivity = FALSE;
	m_comDCB.fInX            = FALSE;
	m_comDCB.fOutX           = FALSE;
	m_comDCB.fErrorChar      = FALSE;
	m_comDCB.fNull           = FALSE;
	m_comDCB.fAbortOnError   = FALSE;
	if (!SetCommState(h, &m_comDCB)) {
		DebugLog("[rs232_apply_dcb] SetCommState failed: %08lX", GetLastError());
		return RS232_NO_DUT;
	}
	if (!SetCommMask(h, bOrgcomPortRS232 ? 0 : EV_CTS | EV_DSR | EV_RLSD)) {
		DebugLog("[rs232_apply_dcb] SetCommMask failed: %08lX", GetLastError());
		return RS232_NO_DUT;
	}
	if (!PurgeComm(h, PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR)) {
		DebugLog("[rs232_apply_dcb] PurgeComm failed: %08lX", GetLastError());
		return RS232_NO_DUT;
	}
	ComTimeOuts.ReadIntervalTimeout        = MAXDWORD;
	ComTimeOuts.ReadTotalTimeoutMultiplier = MAXDWORD;
	ComTimeOuts.ReadTotalTimeoutConstant   = 100;	// 100 ms — same value used previously for raw RS232
	if (!SetCommTimeouts(h, &ComTimeOuts)) {
		DebugLog("[rs232_apply_dcb] SetCommTimeouts failed: %08lX", GetLastError());
		return RS232_NO_DUT;
	}
	return RS232_SUCCESS;
}

// Worker-thread reconnect helper. Closes the current handle, reopens
// using the stashed port number, and reapplies DCB + timeouts. The main
// thread must NOT touch m_ComPortHandle while RxThread is alive — the
// shutdown path waits for the worker to exit before closing.
static int rs232_worker_reopen(void)
{
	char pcComPort[32];

	// FIX [ComPortReopenLock]: close the stale handle under the lock (CloseHandle is fast and
	// non-blocking), then drop the lock BEFORE the blocking CreateFile. Previously the whole
	// close+open sequence ran inside g_handleCs; CreateFile on a network-redirected COM port
	// (Moxa NPort) can block for a long time, so if rs232_disconnect's 2 s TerminateThread
	// fallback fired while we were stuck in CreateFile, the worker was killed while still owning
	// g_handleCs. A Windows CRITICAL_SECTION has no abandoned-owner recovery, so the next
	// rs232_connect()'s EnterCriticalSection deadlocked the UI thread permanently. With the open
	// outside the lock, a TerminateThread during CreateFile leaves no lock held.
	EnterCriticalSection(&g_handleCs);
	if (!bKeepThreadAlive) {
		LeaveCriticalSection(&g_handleCs);
		return RS232_NO_CONNECTION;
	}
	if (m_ComPortHandle != INVALID_HANDLE_VALUE) {
		// Must close first: the port is held exclusively (share-mode 0), so a second open of the
		// same port would fail with ERROR_ACCESS_DENIED while this handle is still alive.
		CloseHandle(m_ComPortHandle);
		m_ComPortHandle = INVALID_HANDLE_VALUE;
	}
	int portNum = g_comPortNumber;
	LeaveCriticalSection(&g_handleCs);

	if (portNum > 9)
		_snprintf_s(pcComPort, sizeof(pcComPort), _TRUNCATE, R"(\\.\COM%d)", portNum);
	else
		_snprintf_s(pcComPort, sizeof(pcComPort), _TRUNCATE, "COM%d", portNum);

	// FIX [ComPortExclusive]: match the main-thread open (RW + share 0) so the reconnect re-claims
	// exclusive ownership and stays un-hijackable. Opened OUTSIDE g_handleCs (see above) and
	// configured on the local handle, so the lock is only re-taken to publish the result.
	HANDLE hNew = CreateFile(pcComPort, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
	if (hNew == INVALID_HANDLE_VALUE) {
		DebugLog("[rs232_worker_reopen] CreateFile(%s) failed: %08lX", pcComPort, GetLastError());
		return RS232_NO_DUT;
	}
	int rc = rs232_apply_dcb_and_timeouts(hNew);
	if (rc != RS232_SUCCESS) {
		CloseHandle(hNew);
		return rc;
	}

	// Publish the freshly-opened, fully-configured handle under the lock.
	EnterCriticalSection(&g_handleCs);
	if (!bKeepThreadAlive) {
		// Shutdown was requested while we were opening — discard the fresh handle.
		LeaveCriticalSection(&g_handleCs);
		CloseHandle(hNew);
		return RS232_NO_CONNECTION;
	}
	m_ComPortHandle = hNew;
	// Reset both timers so the stall watchdog skips the Moxa TCP warmup.
	g_connectTickMs  = GetTickCount64();
	g_lastDataTickMs = g_connectTickMs;
	LeaveCriticalSection(&g_handleCs);
	// FIX [RS232Flap]: keep the telnet watchdog clock in sync with our own
	// connect/data timer reset above, so the pre-reconnect silence gap does not
	// count toward <RS232:0>. Called outside g_handleCs (takes its own lock).
	TelnetServerRS232Heartbeat();
	DebugLog("[rs232_worker_reopen] reconnected on %s", pcComPort);
	return RS232_SUCCESS;
}

int rs232_connect(const SLICER_IN_STR *pInSlicer, SLICER_OUT_STR *pOutSlicer)
{
	extern double ct1600;
	int rc = RS232_NO_DUT;
	char pcComPort[32] = "COM1:";
	COMMPROP ComProp = {} ;

	// Lazily allocate the handle-mutation lock. Safe to do here since this
	// function only runs on the main thread before any worker is spawned.
	rs232_ensure_handle_cs();

	// This as user can switch to slicer/rs232 without ports are close/opened (yet)
	bOrgcomPortRS232 = Profile.comPortRS232 ;
	g_comPortNumber  = pInSlicer->com_port;	// stash for worker self-reconnect
	if(pInSlicer->com_port > 9) {
		_snprintf_s(pcComPort, sizeof(pcComPort), _TRUNCATE, R"(\\.\COM%d)", pInSlicer->com_port) ;
	}
	else {
		_snprintf_s(pcComPort, sizeof(pcComPort), _TRUNCATE, "COM%d", pInSlicer->com_port) ;
	}

	switch (Profile.comPortRS232)
	{
		case 1:
			nTiming = 500;
			break ;
		case 2:
		default:
			nTiming = ct1600;
			break ;
		case 3:
			nTiming = 1.0/((float) 8000 * 839.22e-9);
			break ;
	}

	OUTPUTDEBUGMSG((("calling: rs232_connect(%s)\n"), pcComPort));
	static DWORD g_dwConnectCallCount = 0;
	++g_dwConnectCallCount;
	DebugLog("[rs232_connect] PID=%lu call#=%lu comPortRS232=%d nTiming=%.1f ct1600=%.1f port=%s",
		(unsigned long)GetCurrentProcessId(), (unsigned long)g_dwConnectCallCount,
		Profile.comPortRS232, nTiming, ct1600, pcComPort);

	pOutSlicer->freqdata = rs232_freqdata ;
	pOutSlicer->linedata = rs232_linedata ;
	// FIX [L3]: SLICER_OUT_STR.cpstn is nu volatile unsigned long* — cast niet meer nodig.
	pOutSlicer->cpstn	 = &rs232_cpstn ;
	pOutSlicer->bufsize  = SLICER_BUFSIZE ;

	if (m_bConnectedToComport)
	{
		rc = rs232_disconnect();
		assert (rc >= 0);
		if (rc < 0) {
			return rc;
		}
	}

	/********************************************************************************************
	* Seek contact with the serial.sys driver. Configure it for overlapped operation, this is   *
	* done so the receiving thread (later on in this code) can be terminated by the main thread *
	********************************************************************************************/
	// Lock the handle slot for the open + probe + configure sequence. No
	// worker thread is alive yet on the initial connect path, but the
	// disconnect-then-reconnect path inside this same function may run
	// concurrently with a still-draining worker from a previous session.
	EnterCriticalSection(&g_handleCs);
	// FIX [ComPortExclusive]: open GENERIC_READ|GENERIC_WRITE with share-mode 0
	// (was GENERIC_READ only). PDW never writes, but a virtual COM redirector
	// (Moxa NPort) only enforces exclusive ownership for a read-WRITE open; a
	// read-only open is treated as shareable, letting a second process (PDW or
	// any other app) open the same port and split the byte stream so neither
	// decodes. Matching p2kflexDecoder (GENERIC_READ|GENERIC_WRITE, share 0) makes
	// the running instance hold the port firmly: any second opener fails at
	// CreateFile (ERROR_ACCESS_DENIED) instead of hijacking it.
	m_ComPortHandle = CreateFile(pcComPort, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
	if(m_ComPortHandle == INVALID_HANDLE_VALUE)
	{
	    OUTPUTDEBUGMSG((("ERROR: CreateFile() %08lX!\n"), GetLastError()));
		LeaveCriticalSection(&g_handleCs);
		return RS232_NO_DUT;
	}


	//	We must check if this is a Slicer Driver
	if(!GetCommProperties(m_ComPortHandle, &ComProp)) {
	    OUTPUTDEBUGMSG((("ERROR: GetCommProperties() %08lX!\n"), GetLastError()));
		CloseHandle(m_ComPortHandle);
		m_ComPortHandle = INVALID_HANDLE_VALUE;
		LeaveCriticalSection(&g_handleCs);
		return RS232_NO_DUT;
	}

	if(ComProp.dwProvSpec1 == 0x48576877 && ComProp.dwProvSpec2 == 0x68774857)
	{
		bSlicerDriver = TRUE ;
	}
	else
	{
		bSlicerDriver = FALSE ;
	}
	if(!bOrgcomPortRS232)
	{
		if(!bSlicerDriver)
		{
	        OUTPUTDEBUGMSG((("ERROR:ComProp.dwProvSpec1 != 0x48576877 || ComProp.dwProvSpec2 != 0x68774857!\n")));
		    CloseHandle(m_ComPortHandle);
		    m_ComPortHandle = INVALID_HANDLE_VALUE;
		    LeaveCriticalSection(&g_handleCs);
		    MessageBox(NULL, "Please install the Slicer driver from the install package!", "Slicer Driver Not Installed", MB_OK | MB_ICONEXCLAMATION) ;
		    return RS232_NO_DUT;
		}
	}
	// Apply DCB + read timeouts (shared with worker self-reconnect path).
	rc = rs232_apply_dcb_and_timeouts(m_ComPortHandle);
	if (rc != RS232_SUCCESS) {
		CloseHandle(m_ComPortHandle);
		m_ComPortHandle = INVALID_HANDLE_VALUE;
		LeaveCriticalSection(&g_handleCs);
		return rc;
	}
	m_bConnectedToComport= TRUE;
	g_connectTickMs  = GetTickCount64();
	g_lastDataTickMs = g_connectTickMs;
	LeaveCriticalSection(&g_handleCs);

	/************************************************************************************
	* Next step : fire up a thread that takes care of placing received data in a buffer *
	************************************************************************************/

	bKeepThreadAlive = TRUE;
	m_hRxThread = CreateThread(NULL, 0, RxThread, (LPVOID) NULL, CREATE_SUSPENDED, &m_dwThreadId) ;
	ResumeThread(m_hRxThread);

	/* FIX [RxWedgeDiag]: start observe-only producer-phase diagnostic thread. */
	if (g_rxDiagThread == INVALID_HANDLE_VALUE || g_rxDiagThread == NULL) {
		g_rxDiagRun    = TRUE;
		g_rxDiagThread = CreateThread(NULL, 0, RxDiagThread, NULL, 0, NULL);
	}

	/* FIX [TelnetRS232]: start RS232/AUDIO watchdog */
	TelnetServerRS232Enable(1);
	return(RS232_SUCCESS) ;
}

int rs232_disconnect()
{
	int rc;
	COMMTIMEOUTS ComTimeOuts = { 0 } ;

	OUTPUTDEBUGMSG(("calling: rs232_disconnect()\n"));
	if (!m_bConnectedToComport) {
		// return when already connected
		return RS232_NO_CONNECTION;
	}
	/********************************************************************
	* Terminate the Rx thread cleanly. Signal exit, cancel any pending  *
	* synchronous ReadFile so the worker wakes immediately, then wait   *
	* for natural termination. TerminateThread is only used as a last   *
	* resort if the worker doesn't exit within 2 s — calling it while   *
	* the thread holds kernel locks (e.g. inside the COM redirector)    *
	* can corrupt driver state and was the suspected source of post-    *
	* reconnect instability on Moxa NPort virtual COM ports.            *
	********************************************************************/
	OUTPUTDEBUGMSG(("main thread : signaling RxThread to exit\n"));
	bKeepThreadAlive = FALSE;

	HANDLE hThread = m_hRxThread;
	if (hThread != INVALID_HANDLE_VALUE && hThread != NULL) {
		// Wake any in-flight ReadFile on the worker. Safe to ignore the
		// return value: if no I/O is pending the call is a no-op.
		CancelSynchronousIo(hThread);

		DWORD waitResult = WaitForSingleObject(hThread, 2000);
		if (waitResult != WAIT_OBJECT_0) {
			OUTPUTDEBUGMSG(("main thread : RxThread did not exit, terminating\n"));
			DebugLog("[rs232_disconnect] RxThread did not exit in 2s, force-terminating");
			TerminateThread(hThread, (DWORD)-1);
			WaitForSingleObject(hThread, 500);
		}
		CloseHandle(hThread);
		m_hRxThread = INVALID_HANDLE_VALUE;
	}

	OUTPUTDEBUGMSG(("main thread : closing COM handle\n"));

	// We already waited for the worker to exit above, so normally there is
	// no contender for this lock. The exception is the TerminateThread
	// fallback path: if the worker was killed mid-rs232_worker_reopen it may
	// have left the critical section owned by a now-dead thread, which would
	// deadlock us here. Use TryEnterCriticalSection with a bounded retry so
	// shutdown can still progress (we'd rather leak a handle than hang).
	BOOL gotLock = FALSE;
	if (g_handleCsInited) {
		for (int i = 0; i < 20; i++) {
			if (TryEnterCriticalSection(&g_handleCs)) { gotLock = TRUE; break; }
			Sleep(50);
		}
		if (!gotLock)
			OUTPUTDEBUGMSG(("rs232_disconnect: handle lock contended (dead worker?); proceeding unlocked\n"));
	}
	if (m_ComPortHandle != INVALID_HANDLE_VALUE) {
		if (!SetCommTimeouts(m_ComPortHandle, &ComTimeOuts)) {
			OUTPUTDEBUGMSG((("ERROR: SetCommTimeouts() %08lX!\n"), GetLastError()));
		}
		if (!SetCommMask(m_ComPortHandle, 0)) {
			OUTPUTDEBUGMSG((("ERROR: SetCommMask() %08lX!\n"), GetLastError()));
		}
		rc = CloseHandle(m_ComPortHandle);
		if (!rc) {
			OUTPUTDEBUGMSG(("main thread : error closing handle!\n"));
		}
		m_ComPortHandle = INVALID_HANDLE_VALUE;
	}
	m_bConnectedToComport = FALSE;
	if (gotLock) LeaveCriticalSection(&g_handleCs);

	/* FIX [RxWedgeDiag]: stop observe-only diagnostic thread. */
	if (g_rxDiagThread != INVALID_HANDLE_VALUE && g_rxDiagThread != NULL) {
		g_rxDiagRun = FALSE;
		WaitForSingleObject(g_rxDiagThread, 1000);
		CloseHandle(g_rxDiagThread);
		g_rxDiagThread = INVALID_HANDLE_VALUE;
	}

	/* FIX [TelnetRS232]: stop RS232/AUDIO watchdog, emit <RS232:0> if was active */
	TelnetServerRS232Enable(0);
	return(RS232_SUCCESS) ;
}

/***********************************************************
* This worker thread will place received data in a mailbox *
***********************************************************/
DWORD WINAPI RxThread(LPVOID pCl)
{
	g_lastDataTickMs = GetTickCount64();

	do
	{
		g_rxLoops++;	// FIX [RxWedgeDiag]: liveness counter (read by RxDiagThread)

		int bytesRead = 0;
		if (bOrgcomPortRS232) {
			bytesRead = rs232_read();
		}
		else {
			bytesRead = slicer_read();
		}
		RxSetPhase(RXP_SLEEP);	// FIX [RxWedgeDiag]: back in the loop body

		// Classify the most recent read result so the watchdog can pick a
		// matching backoff. ERROR_OPERATION_ABORTED is the normal exit path
		// when rs232_disconnect calls CancelSynchronousIo on us — break out
		// immediately, no reconnect attempt.
		DWORD readErr = g_lastReadError;
		if (readErr == ERROR_OPERATION_ABORTED && !bKeepThreadAlive) {
			break;
		}
		BOOL deviceGone = (readErr == ERROR_DEVICE_REMOVED
		                || readErr == ERROR_FILE_NOT_FOUND
		                || readErr == ERROR_INVALID_HANDLE
		                || readErr == ERROR_ACCESS_DENIED);

		// Watchdog: virtual COM redirectors (e.g. Moxa NPort) silently
		// return ReadFile=TRUE with dwRead=0 after their TCP tunnel
		// drops, so the worker keeps "succeeding" without any data
		// flowing. After RS232_STALL_MS of zero-byte reads, tear down
		// the handle and reopen it from this thread — no main-thread
		// involvement, no message-loop dependency.
		ULONGLONG now = GetTickCount64();
		if (bytesRead > 0) {
			g_lastDataTickMs = now;
		}
		else if ((now - g_lastDataTickMs) > RS232_STALL_MS &&
		         (now - g_connectTickMs)  > RS232_WARMUP_MS) {
			DebugLog("[RxThread] no data for %llums on COM%d (err=%lu) - reconnecting",
				now - g_lastDataTickMs, g_comPortNumber, (unsigned long)readErr);
			RxSetPhase(RXP_REOPEN);	// FIX [RxWedgeDiag]
			if (rs232_worker_reopen() != RS232_SUCCESS) {
				// Reopen failed. Distinguish "device physically gone"
				// (long backoff, don't burn CPU) from "transient hiccup"
				// (short backoff so we recover quickly on Moxa).
				g_connectTickMs = GetTickCount64();
				Sleep(deviceGone ? RS232_DEVICE_GONE_MS : RS232_RECONNECT_BACKOFF_MS);
			}
		}

		// Only rate-limit when there was no data. With bytes flowing, the
		// next ReadFile blocks passively (no CPU) until the next batch or
		// the 100 ms timeout, so dropping the extra Sleep here cuts decode
		// latency without raising CPU. When idle, keep the 50 ms cap so
		// the stall watchdog can't fire more than ~20 times per second on
		// a removed device.
		if (bytesRead <= 0) Sleep(50);
	}
	while (bKeepThreadAlive);

	OUTPUTDEBUGMSG(("RxThread : terminating...\n"));
	ExitThread(0L);
	return 0;
}


int rs232_read(void)
{
	DWORD	dwRead = 0;
	int     bit ;
	BYTE    byData[256] ;

	//	OUTPUTDEBUGMSG((("rs232_read() \n")));
	if(m_ComPortHandle == INVALID_HANDLE_VALUE)
	{
		OUTPUTDEBUGMSG((("rs232_read : COMport not open!\n")));
		return(0) ;
	}

	RxSetPhase(RXP_READFILE);	// FIX [RxWedgeDiag]: stuck here == ReadFile wedged
	BOOL bRead = ReadFile(m_ComPortHandle, byData, sizeof(byData), &dwRead, 0);
	RxSetPhase(RXP_PARSE);		// FIX [RxWedgeDiag]: ReadFile returned
	if(!bRead)
	{
		DWORD err = GetLastError();
		g_lastReadError = err;
		OUTPUTDEBUGMSG((("rs232_read : Error in reading 0x%0x!\n"), err));
		// PurgeComm on a removed device just generates another error; skip
		// it for the gone-cases so we don't spam the log on each retry.
		if (m_ComPortHandle != INVALID_HANDLE_VALUE
		    && err != ERROR_DEVICE_REMOVED && err != ERROR_FILE_NOT_FOUND
		    && err != ERROR_INVALID_HANDLE && err != ERROR_ACCESS_DENIED
		    && err != ERROR_OPERATION_ABORTED)
		{
			PurgeComm(m_ComPortHandle, PURGE_RXCLEAR);
		}
		return(0);
	}
	g_lastReadError = 0;
	for (DWORD i = 0; i < dwRead; i++) {
		for (int j = 7; j >= 0; j--)
		{
			if (Profile.fourlevel) {
				j--;
				bit = (byData[i] >> j) & 3;
			}
			else {
				bit = (byData[i] >> j) & 1;
			}
			// Stage writes into a local index, commit data BEFORE publishing the
			// new index. The barrier prevents the compiler from reordering the
			// index store ahead of the data stores; on weakly-ordered CPUs the
			// matching read-barrier on the consumer side would close the loop.
			DWORD idx = rs232_cpstn;
			rs232_linedata[idx] = (BYTE)(bit << 4);
			rs232_freqdata[idx] = (WORD)nTiming;
			_WriteBarrier();
			DWORD next = idx + 1;
			if (next >= SLICER_BUFSIZE) {
				next = 0;
				ULONGLONG now = GetTickCount64();
				ULONGLONG lastWrap = g_lastWrapTickMs;
				if (lastWrap != 0 && (now - lastWrap) < 200u) {
					if ((++g_overrunWarnCount & 0x3F) == 1)
						DebugLog("[rs232] RX ring wrapped %llums after previous wrap - consumer likely behind (count=%u)",
							now - lastWrap, (unsigned)g_overrunWarnCount);
				}
				g_lastWrapTickMs = now;
			}
			rs232_cpstn = next;
		}
	}
	/* FIX [TelnetRS232]: notify RS232/AUDIO watchdog with raw bytes */
	if (dwRead > 0) {
		RxSetPhase(RXP_TSNOTIFY);	// FIX [RxWedgeDiag]: stuck here == blocked on g_tsCs (telnet wire-log)
		TelnetServerRS232BytesReceived(byData, (int)dwRead);
	}
	return (int)dwRead;
}


int slicer_read(void)
{
	DWORD dwRead = 0, i, num;
	WORD	*freq ;
	BYTE	*line ;

	if(m_ComPortHandle == INVALID_HANDLE_VALUE)
	{
		OUTPUTDEBUGMSG((("slicer_read : COMport not open!\n")));
		return(0);
	}
	RxSetPhase(RXP_READFILE);	// FIX [RxWedgeDiag]: stuck here == ReadFile wedged
	BOOL bRead = ReadFile(m_ComPortHandle, byRS232Data, sizeof(byRS232Data), &dwRead, 0);
	RxSetPhase(RXP_PARSE);		// FIX [RxWedgeDiag]: ReadFile returned
	if(!bRead)
	{
		DWORD err = GetLastError();
		g_lastReadError = err;
		OUTPUTDEBUGMSG((("slicer_read : Error in reading 0x%0x!\n"), err));
		if (m_ComPortHandle != INVALID_HANDLE_VALUE
		    && err != ERROR_DEVICE_REMOVED && err != ERROR_FILE_NOT_FOUND
		    && err != ERROR_INVALID_HANDLE && err != ERROR_ACCESS_DENIED
		    && err != ERROR_OPERATION_ABORTED)
		{
			PurgeComm(m_ComPortHandle, PURGE_RXCLEAR);
		}
		return(0);
	}
	g_lastReadError = 0;

	num = dwRead / (sizeof(WORD) + sizeof(BYTE));
	line = byRS232Data;
	freq = (WORD *)(byRS232Data + num * sizeof(BYTE));
	for (i = 0; i < num; i++) {
		DWORD idx = rs232_cpstn;
		rs232_linedata[idx] = *line++;
		rs232_freqdata[idx] = *freq++;
		_WriteBarrier();
		DWORD next = idx + 1;
		if (next >= SLICER_BUFSIZE) {
			next = 0;
			ULONGLONG now = GetTickCount64();
			ULONGLONG lastWrap = g_lastWrapTickMs;
			if (lastWrap != 0 && (now - lastWrap) < 200u) {
				if ((++g_overrunWarnCount & 0x3F) == 1)
					DebugLog("[slicer] RX ring wrapped %llums after previous wrap - consumer likely behind (count=%u)",
						now - lastWrap, (unsigned)g_overrunWarnCount);
			}
			g_lastWrapTickMs = now;
		}
		rs232_cpstn = next;
	}
	/* FIX [TelnetRS232]: notify RS232/AUDIO watchdog; use SlicerActivity
	** (not BytesReceived) — decoded line-data bytes have too few XOR
	** transitions to reach the p2kflexDecoder audio threshold.
	** Use dwRead (raw bytes) not num (complete 3-byte samples): partial reads
	** (dwRead=1 or 2) still prove the COM link is alive even if num rounds to 0. */
	if (dwRead > 0) {
		RxSetPhase(RXP_TSNOTIFY);	// FIX [RxWedgeDiag]: stuck here == blocked on g_tsCs (telnet wire-log)
		TelnetServerSlicerActivity((int)dwRead);
	}
	return (int)dwRead;
}


#define _COMPORT_1		0
#define _COMPORT_2		1
#define _COMPORT_3		2
#define _COMPORT_4		3

// Variables to put into pdw.ini
int nComPort2 =  _COMPORT_1;
HANDLE m_ComPortHandle2 = INVALID_HANDLE_VALUE;
BOOL m_bConnectedToComport2 = FALSE;

int OpenComPort(void)
{
	int rc = RS232_NO_DUT;
	char pcComPort[32] = "COM1:";
	DCB m_comDCB = {} ;

	OUTPUTDEBUGMSG((("calling: OpenComPort()\n")));

	// FIX [SerieelIO]: pcComPort[3]='1'+nComPort2 werkte alleen voor COM1-COM9 en
	// genereerde ongeldige strings (bijv. "COM::") voor nComPort2>=9 zonder \\.\-prefix.
	if (nComPort2 + 1 > 9)
		_snprintf_s(pcComPort, sizeof(pcComPort), _TRUNCATE, R"(\\.\COM%d)", nComPort2 + 1);
	else
		_snprintf_s(pcComPort, sizeof(pcComPort), _TRUNCATE, "COM%d", nComPort2 + 1);

	if (m_bConnectedToComport2) {
		rc = CloseComPort();
		assert (rc >= 0);
		if (rc < 0) return rc;
	}

	assert (m_ComPortHandle2 == INVALID_HANDLE_VALUE);
	m_ComPortHandle2 = CreateFile(pcComPort,GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
	if(m_ComPortHandle2 == INVALID_HANDLE_VALUE)
	{
	    OUTPUTDEBUGMSG((("ERROR: CreateFile() %08lX!\n"), GetLastError()));
		return RS232_NO_DUT;
	}

	//	We got a connection with the serial.sys driver
	if(!GetCommState(m_ComPortHandle2,&m_comDCB))
	{
	    OUTPUTDEBUGMSG((("ERROR: GetCommState() %08lX!\n"), GetLastError()));
		CloseHandle(m_ComPortHandle2);
		return RS232_NO_DUT;
	}

	// Our specific part of the connection 
	// All the = Zero and by that default
	m_comDCB.BaudRate			= CBR_19200 ;
	m_comDCB.ByteSize			= 8 ;
	m_comDCB.Parity				= NOPARITY ;
	m_comDCB.StopBits			= ONESTOPBIT;
	m_comDCB.fBinary			= TRUE;
	m_comDCB.fParity			= FALSE;
	m_comDCB.fDtrControl        = DTR_CONTROL_ENABLE ;
	m_comDCB.fRtsControl        = RTS_CONTROL_ENABLE ;

	if(!SetCommState(m_ComPortHandle2,&m_comDCB)) {
	    OUTPUTDEBUGMSG((("ERROR: GetCommState() %08lX!\n"), GetLastError()));
		CloseHandle(m_ComPortHandle2);
		return RS232_NO_DUT;
	}

	/* Purge buffers */
	if(!PurgeComm(m_ComPortHandle2,PURGE_TXABORT|PURGE_RXABORT|PURGE_TXCLEAR|PURGE_RXCLEAR)) {
	    OUTPUTDEBUGMSG((("ERROR: PurgeComm() %08lX!\n"), GetLastError()));
		CloseHandle(m_ComPortHandle2);
		return RS232_NO_DUT;
	}

	// FIX [SerieelIO]: SetCommTimeouts ontbrak voor de secundaire poort — ReadFile kon
	// dan onbeperkt blokkeren bij afwezigheid van data. Zelfde waarden als primaire poort.
	{
		COMMTIMEOUTS ct = {};
		ct.ReadIntervalTimeout        = MAXDWORD;
		ct.ReadTotalTimeoutMultiplier = MAXDWORD;
		ct.ReadTotalTimeoutConstant   = 100;
		if (!SetCommTimeouts(m_ComPortHandle2, &ct)) {
			OUTPUTDEBUGMSG((("ERROR: SetCommTimeouts() %08lX!\n"), GetLastError()));
			CloseHandle(m_ComPortHandle2);
			return RS232_NO_DUT;
		}
	}

	m_bConnectedToComport2 = TRUE;

	return RS232_SUCCESS;
}

char chStartChar = 10 ;
char chEndChar = 4 ;

int WriteComPort(char *szLine)
{
	char szTemp[1024] ;
	int len = 0 ;
	DWORD dwWrite = 0;

	OUTPUTDEBUGMSG(((">>> WriteComPort()\n")));

	if (m_ComPortHandle2 == INVALID_HANDLE_VALUE) {
		OUTPUTDEBUGMSG(("WriteComPort : port not open\n"));
		return RS232_NO_CONNECTION;
	}

	if(chStartChar) {
		szTemp[len++] = chStartChar ;
	}
	// Reserve room for the optional trailing framing byte plus the null
	// terminator _snprintf_s appends. _TRUNCATE silently caps over-length
	// input — preferable to wsprintf's ill-documented 1024-byte ceiling.
	int reserved = (chEndChar ? 1 : 0) + 1;
	int avail    = (int)sizeof(szTemp) - len - reserved;
	if (avail < 0) avail = 0;
	int payload = _snprintf_s(szTemp + len, avail + 1, _TRUNCATE, "%s", szLine);
	if (payload < 0) payload = avail;	// truncation reported as -1
	len += payload;
	if(chEndChar) {
		szTemp[len++] = chEndChar ;
		szTemp[len] = 0;
	}

	if(!WriteFile(m_ComPortHandle2, szTemp, len, &dwWrite, 0)) {
		OUTPUTDEBUGMSG((("WriteComPort : Error in Writing 0x%0x!\n"), GetLastError()));
		OUTPUTDEBUGMSG((("<<< WriteComPort()\n")));
		return RS232_NO_DUT;
	}
	OUTPUTDEBUGMSG((("<<< WriteComPort()\n")));
	// Treat a short write as a failure so the caller knows the command was
	// only partially transmitted. WriteFile in non-overlapped mode is allowed
	// to short-write when the TX buffer fills; callers can choose to retry.
	return (dwWrite == (DWORD)len) ? RS232_SUCCESS : RS232_NO_DUT;
}


int CloseComPort(void)
{
	int rc; 

	OUTPUTDEBUGMSG((("CloseComPort()\n")));
	rc = CloseHandle(m_ComPortHandle2);
	if (!rc) {
		OUTPUTDEBUGMSG(("CloseComPort: Error closing handle!\n"));
		rc = RS232_UNKNOWN;
	}
	else {
		OUTPUTDEBUGMSG(("CloseComPort: comport closed.\n"));
		m_ComPortHandle2 = INVALID_HANDLE_VALUE;
		m_bConnectedToComport2 = FALSE;
	}
	return(0) ;
}

// 52 entries: supports up to 51 found ports + terminating 0.
// The loop scans COM1..COM49 — Moxa NPort adapters can create many virtual ports.
int nComPortsArr[52] ;

int *FindComPorts(void)
{
	DWORD error ;
	int nNumFound = 0 ;
	char szPort[32] ;
	HANDLE hCom ;
	BOOL bFound ;
	for(int i=1; i<50; i++) {
		if(nNumFound >= 51) break ;		// leave room for terminating 0
		if(i > 9) {
			wsprintf(szPort, "\\\\.\\COM%d", i) ;
		}
		else {
			wsprintf(szPort, "COM%d", i) ;
		}

		bFound = FALSE ;
		hCom = CreateFile(szPort, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
		if(hCom == INVALID_HANDLE_VALUE) {
			error = GetLastError() ;
			if(error != ERROR_FILE_NOT_FOUND)
				bFound = TRUE ;			// port exists but access denied / in use
		}
		else {
			bFound = TRUE ;
			CloseHandle(hCom) ;
		}
		if(bFound) {
			OUTPUTDEBUGMSG((("COM%d: Found\n"), i));
			nComPortsArr[nNumFound++] = i ;
		}
		else {
			OUTPUTDEBUGMSG((("COM%d: Not Found\n"), i));
		}
	}
	nComPortsArr[nNumFound] = 0 ;
	return(nComPortsArr) ;
}


int GetRs232DriverType(void)
{
	return(m_bConnectedToComport ? (bSlicerDriver ?  DRIVER_TYPE_SLICER : DRIVER_TYPE_RS232) : DRIVER_TYPE_NOT_LOADED) ;
}
