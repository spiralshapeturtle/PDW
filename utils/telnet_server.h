/*
** telnet_server.h -- Telnet server for PDW
**
** Listens on a TCP port (default 8024) and pushes decoded paging messages to all
** connected clients in the p2kflexDecoder wire-format:
**
**     <TX_START>\r
**     CC/FFF -ALPHA- 1234567 message text\r
**     ...
**     <TX_STOP><RXQ:NN[+|-]>\r
**     <WD>\r                                  (heartbeat after silence)
**     <BUFFER_START>\r ... <BUFFER_STOP>\r    (backlog replay after reconnect)
**
** Compatible with existing CS FlexDecoder clients — same protocol & framing.
*/
#ifndef PDW_TELNET_SERVER_H
#define PDW_TELNET_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Status notification — posted to the dialog window if registered.
** wParam = TSS_* state, lParam = current client count.
*/
#define WM_TELNET_STATUS    (WM_USER + 60)

#define TSS_DISABLED   0
#define TSS_LISTENING  1
#define TSS_ERROR      2

/* Lifecycle — main-thread only.
** Init reads Profile.telnetServer* settings. No-op if disabled.
** Shutdown is safe to call repeatedly. Destroy releases CRITICAL_SECTIONs.
*/
void TelnetServerInit(void);
void TelnetServerShutdown(void);
void TelnetServerDestroy(void);

/* Called from ShowMessage() on the main thread, after Current_MSG[] is filled
** and after webhook/MQTT have been notified. Reads Current_MSG[] + globals
** (iCurrentCycle/iCurrentFrame, dRX_Quality) and pushes one wire-line to all
** clients. Emits <TX_START> first if needed. */
void TelnetServerNotifyMessage(void);

/* TX-burst boundary signals.
** TxStart is idempotent — only the first call after IDLE emits a wire-frame.
** TxStop is also idempotent.
** If TxStop is not called explicitly, the worker thread fires it automatically
** after ~3 s of message inactivity (matches p2kflex auto-stop heuristic). */
void TelnetServerNotifyTxStart(void);
void TelnetServerNotifyTxStop(void);

/* Emit one Short Instruction line directly — bypasses ShowMessage() because PDW
** with Profile.convert_si=1 (default) routes SIs through AddAssignment() and
** never calls ShowMessage() for them. Format matches p2kflexDecoder:
**     CC/FFF -INSTR- <subscriber> <group> <assignedFrame>\r
** Reads iCurrentCycle / iCurrentFrame globals. Called from Flex.cpp. */
void TelnetServerNotifyInstr(long subscriberCapcode, int groupCapcode, int assignedFrame);

/* Emit one FLEX frame-info line. body becomes the trailing free-form text:
**     CC/FFF -FRAME- 0000000 <body>\r
** Examples: body = "Empty frame", "FlexTIME: 27-05-2026 08:30:18".
** Reads iCurrentCycle / iCurrentFrame globals. Called from Flex.cpp. */
void TelnetServerNotifyFrame(const char *body);

/* RS232 / AUDIO monitoring — only active in RS232/slicer input mode.
**
** TelnetServerRS232Enable(1): call from rs232_connect on success. Starts the
**   watchdog; if no bytes arrive within 2 s emits <RS232:0>.
** TelnetServerRS232Enable(0): call from rs232_disconnect. Emits <RS232:0>
**   immediately if the link was active, then stops the watchdog.
**
** TelnetServerRS232BytesReceived: call from rs232_read (bOrgcomPortRS232=TRUE,
**   Rene's raw 19200 baud converter). Counts bit transitions in raw RS232
**   bytes — identical to p2kflexDecoder RS232.cpp RS232_ACTIVITY logic.
**
** TelnetServerSlicerActivity: call from slicer_read (bOrgcomPortRS232=FALSE).
**   The PDW slicer driver outputs decoded-bit bytes (0x00/0x10 per bit) not
**   raw RS232 frames, so bit-transition counting yields near-zero regardless
**   of actual audio presence. Instead: any arriving bytes = audio present. */
void TelnetServerRS232Enable(int active);
void TelnetServerRS232BytesReceived(const BYTE *data, int len);
void TelnetServerSlicerActivity(int nBytes);

/* TelnetServerRS232Heartbeat: call from rs232_worker_reopen() on successful
** reconnect. Resets the watchdog silence timer so the pre-reconnect byte-gap
** does not trigger a spurious <RS232:0>. Does not change link state. */
void TelnetServerRS232Heartbeat(void);

/* Hook for the dialog: registers/clears the HWND that receives WM_TELNET_STATUS. */
void TelnetServerSetStatusWnd(HWND hWnd);

/* Current client count — for status display. */
int  TelnetServerClientCount(void);

/* Snapshot of one connected slot for the GUI's connected-clients grid. */
typedef struct {
    int  used;             /* 0/1 — slot is in use (incl. disconnected-but-buffered) */
    int  disconnected;     /* 1 if peer closed but slot kept for replay window */
    char ip[24];           /* "a.b.c.d" or empty */
    int  port;
    char name[64];         /* from CLIENT:..., empty if not identified */
    int  role;             /* -1=unset, 0=SLAVE, 1=MASTER */
} TsClientInfo;

/* Fill out[] with current slot snapshots. Returns count written (<= maxCount).
** Thread-safe — takes the internal critical section. */
int  TelnetServerGetClients(TsClientInfo *out, int maxCount);

/* Pull recent events for the GUI activity-list. Returns count written,
** newest first. Each event is one human-readable line. */
typedef struct {
    long long ts_ms;       /* monotonic tick from GetTickCount64() */
    char text[128];
} TsEvent;

int  TelnetServerGetEvents(TsEvent *out, int maxCount);

#ifdef __cplusplus
}
#endif
#endif /* PDW_TELNET_SERVER_H */
