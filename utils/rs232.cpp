#include <windows.h>
#include <winbase.h>
#include "..\headers\slicer.h"
#include "..\utils\debug.h"
#include "..\utils\debuglog.h"
#include "..\utils\ostype.h"
#include "..\headers\pdw.h"
#include "rs232.h"

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
double  nTiming ;	// was int
BOOL    bOrgcomPortRS232 ;
BOOL    bSlicerDriver = FALSE;

WORD  rs232_freqdata[SLICER_BUFSIZE] ;
BYTE  rs232_linedata[SLICER_BUFSIZE] ;
DWORD rs232_cpstn;

BYTE  byRS232Data[SLICER_BUFSIZE * (sizeof(WORD) + sizeof(BYTE))] ;

// Worker self-reconnect state: when the Moxa NPort (or any virtual COM
// redirector) silently drops its TCP tunnel, ReadFile keeps returning
// success with 0 bytes. The watchdog in RxThread detects this and tears
// down + reopens the handle without involving the main thread.
static int            g_comPortNumber  = 0;
static volatile DWORD g_lastDataTickMs = 0;
// g_connectTickMs is set on every rs232_connect() and rs232_worker_reopen()
// so the stall watchdog can suppress itself during the Moxa TCP warmup period.
static volatile DWORD g_connectTickMs  = 0;
#define RS232_STALL_MS              5000u
#define RS232_RECONNECT_BACKOFF_MS  2000u
// Moxa NPort needs 1-4 s after CreateFile to start delivering data.
// Don't fire the stall watchdog until this much time has elapsed since
// the last connect/reconnect.
#define RS232_WARMUP_MS             6000u

#define assert(a)		if(!(a))  { OUTPUTDEBUGMSG(("SIMULATE ASSERT in file %s at %d\n", __FILE__, __LINE__ )); }

// Apply our standard DCB settings and read timeouts to m_ComPortHandle.
// Factored out of rs232_connect so the worker-thread self-reconnect path
// can use it too. Always sets timeouts (previously only set when in raw
// RS232 mode — slicer mode could block indefinitely on ReadFile).
static int rs232_apply_dcb_and_timeouts(void)
{
	DCB m_comDCB = {};
	COMMTIMEOUTS ComTimeOuts = {};

	if (m_ComPortHandle == INVALID_HANDLE_VALUE) return RS232_NO_DUT;

	if (!GetCommState(m_ComPortHandle, &m_comDCB)) {
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
	if (!SetCommState(m_ComPortHandle, &m_comDCB)) {
		DebugLog("[rs232_apply_dcb] SetCommState failed: %08lX", GetLastError());
		return RS232_NO_DUT;
	}
	if (!SetCommMask(m_ComPortHandle, bOrgcomPortRS232 ? 0 : EV_CTS | EV_DSR | EV_RLSD)) {
		DebugLog("[rs232_apply_dcb] SetCommMask failed: %08lX", GetLastError());
		return RS232_NO_DUT;
	}
	if (!PurgeComm(m_ComPortHandle, PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR)) {
		DebugLog("[rs232_apply_dcb] PurgeComm failed: %08lX", GetLastError());
		return RS232_NO_DUT;
	}
	ComTimeOuts.ReadIntervalTimeout        = MAXDWORD;
	ComTimeOuts.ReadTotalTimeoutMultiplier = MAXDWORD;
	ComTimeOuts.ReadTotalTimeoutConstant   = 100;	// 100 ms — same value used previously for raw RS232
	if (!SetCommTimeouts(m_ComPortHandle, &ComTimeOuts)) {
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

	if (m_ComPortHandle != INVALID_HANDLE_VALUE) {
		CloseHandle(m_ComPortHandle);
		m_ComPortHandle = INVALID_HANDLE_VALUE;
	}
	if (g_comPortNumber > 9)
		_snprintf(pcComPort, sizeof(pcComPort) - 1, R"(\\.\COM%d)", g_comPortNumber);
	else
		_snprintf(pcComPort, sizeof(pcComPort) - 1, "COM%d", g_comPortNumber);
	pcComPort[sizeof(pcComPort) - 1] = '\0';

	m_ComPortHandle = CreateFile(pcComPort, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
	if (m_ComPortHandle == INVALID_HANDLE_VALUE) {
		DebugLog("[rs232_worker_reopen] CreateFile(%s) failed: %08lX", pcComPort, GetLastError());
		return RS232_NO_DUT;
	}
	int rc = rs232_apply_dcb_and_timeouts();
	if (rc != RS232_SUCCESS) {
		CloseHandle(m_ComPortHandle);
		m_ComPortHandle = INVALID_HANDLE_VALUE;
		return rc;
	}
	// Reset both timers so the stall watchdog skips the Moxa TCP warmup.
	g_connectTickMs  = GetTickCount();
	g_lastDataTickMs = g_connectTickMs;
	DebugLog("[rs232_worker_reopen] reconnected on %s", pcComPort);
	return RS232_SUCCESS;
}

int rs232_connect(const SLICER_IN_STR *pInSlicer, SLICER_OUT_STR *pOutSlicer)
{
	extern double ct1600;
	int rc = RS232_NO_DUT;
	char pcComPort[32] = "COM1:";
	COMMPROP ComProp = {} ;

	// This as user can switch to slicer/rs232 without ports are close/opened (yet)
	bOrgcomPortRS232 = Profile.comPortRS232 ;
	g_comPortNumber  = pInSlicer->com_port;	// stash for worker self-reconnect
	if(pInSlicer->com_port > 9) {
		_snprintf(pcComPort, sizeof(pcComPort) - 1, R"(\\.\COM%d)", pInSlicer->com_port) ;
		pcComPort[sizeof(pcComPort) - 1] = '\0';
	}
	else {
		_snprintf(pcComPort, sizeof(pcComPort) - 1, "COM%d", pInSlicer->com_port) ;
		pcComPort[sizeof(pcComPort) - 1] = '\0';
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
	m_ComPortHandle = CreateFile(pcComPort,GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
	if(m_ComPortHandle == INVALID_HANDLE_VALUE)
	{
	    OUTPUTDEBUGMSG((("ERROR: CreateFile() %08lX!\n"), GetLastError()));
		return RS232_NO_DUT;
	}


	//	We must check if this is a Slicer Driver
	if(!GetCommProperties(m_ComPortHandle, &ComProp)) {
	    OUTPUTDEBUGMSG((("ERROR: GetCommProperties() %08lX!\n"), GetLastError()));
		CloseHandle(m_ComPortHandle);
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
		    MessageBox(NULL, "Please install the Slicer driver from the install package!", "Slicer Driver Not Installed", MB_OK | MB_ICONEXCLAMATION) ;
		    return RS232_NO_DUT;
		}
	}
	// Apply DCB + read timeouts (shared with worker self-reconnect path).
	rc = rs232_apply_dcb_and_timeouts();
	if (rc != RS232_SUCCESS) {
		CloseHandle(m_ComPortHandle);
		m_ComPortHandle = INVALID_HANDLE_VALUE;
		return rc;
	}
	m_bConnectedToComport= TRUE;
	g_connectTickMs  = GetTickCount();
	g_lastDataTickMs = g_connectTickMs;

	/************************************************************************************
	* Next step : fire up a thread that takes care of placing received data in a buffer *
	************************************************************************************/

	bKeepThreadAlive = TRUE;
	m_hRxThread = CreateThread(NULL, 0, RxThread, (LPVOID) NULL, CREATE_SUSPENDED, &m_dwThreadId) ;
	ResumeThread(m_hRxThread);

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
	return(RS232_SUCCESS) ;
}

/***********************************************************
* This worker thread will place received data in a mailbox *
***********************************************************/
DWORD WINAPI RxThread(LPVOID pCl)
{
	g_lastDataTickMs = GetTickCount();

	do
	{
		int bytesRead = 0;
		if (bOrgcomPortRS232) {
			bytesRead = rs232_read();
		}
		else {
			bytesRead = slicer_read();
		}

		// Watchdog: virtual COM redirectors (e.g. Moxa NPort) silently
		// return ReadFile=TRUE with dwRead=0 after their TCP tunnel
		// drops, so the worker keeps "succeeding" without any data
		// flowing. After RS232_STALL_MS of zero-byte reads, tear down
		// the handle and reopen it from this thread — no main-thread
		// involvement, no message-loop dependency.
		DWORD now = GetTickCount();
		if (bytesRead > 0) {
			g_lastDataTickMs = now;
		}
		else if ((now - g_lastDataTickMs) > RS232_STALL_MS &&
		         (now - g_connectTickMs)  > RS232_WARMUP_MS) {
			DebugLog("[RxThread] no data for %ums on COM%d - reconnecting",
				now - g_lastDataTickMs, g_comPortNumber);
			if (rs232_worker_reopen() != RS232_SUCCESS) {
				// Reopen failed (port gone, permission denied, etc).
				// Back off so we don't hammer CreateFile.
				g_connectTickMs = GetTickCount();
				Sleep(RS232_RECONNECT_BACKOFF_MS);
			}
		}

		Sleep(50);
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

	if(!ReadFile(m_ComPortHandle, byData, sizeof(byData), &dwRead, 0))
	{
		OUTPUTDEBUGMSG((("rs232_read : Error in reading 0x%0x!\n"), GetLastError()));
		if (m_ComPortHandle != INVALID_HANDLE_VALUE)
			PurgeComm(m_ComPortHandle, PURGE_RXCLEAR);
		return(0);
	}
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
			rs232_linedata[rs232_cpstn] = bit << 4;
			rs232_freqdata[rs232_cpstn++] = (WORD)nTiming;
			if (rs232_cpstn >= SLICER_BUFSIZE) {
				rs232_cpstn = 0;
			}
		}
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
	if(!ReadFile(m_ComPortHandle, byRS232Data, sizeof(byRS232Data), &dwRead, 0))
	{
		OUTPUTDEBUGMSG((("slicer_read : Error in reading 0x%0x!\n"), GetLastError()));
		if (m_ComPortHandle != INVALID_HANDLE_VALUE)
			PurgeComm(m_ComPortHandle, PURGE_RXCLEAR);
		return(0);
	}

	num = dwRead / (sizeof(WORD) + sizeof(BYTE));
	line = byRS232Data;
	freq = (WORD *)(byRS232Data + num * sizeof(BYTE));
	for (i = 0; i < num; i++) {
		rs232_linedata[rs232_cpstn] = *line++;
		rs232_freqdata[rs232_cpstn++] = *freq++;
		if (rs232_cpstn >= SLICER_BUFSIZE) {
			rs232_cpstn = 0;
		}
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

	pcComPort[3] = '1' + nComPort2 ;

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

	m_bConnectedToComport2 = TRUE;

	return RS232_SUCCESS;
}

char chStartChar = 10 ;
char chEndChar = 4 ;

int WriteComPort(char *szLine)
{
	char szTemp[1024] ;
	int len = 0 ;
	DWORD dwWrite  ;

	OUTPUTDEBUGMSG(((">>> WriteComPort()\n")));

	if(chStartChar) {
		szTemp[len++] = chStartChar ;
	}
	len += wsprintf(szTemp + len, "%s", szLine) ;
	if(chEndChar) {
		szTemp[len++] = chEndChar ;
		szTemp[len] = 0;
	}

	if(!WriteFile(m_ComPortHandle2, szTemp, len, &dwWrite, 0)) {
		OUTPUTDEBUGMSG((("WriteComPort : Error in Writing 0x%0x!\n"), GetLastError()));
	}
	OUTPUTDEBUGMSG((("<<< WriteComPort()\n")));
	return (0) ;
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
