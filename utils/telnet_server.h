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

/* Hook for the dialog: registers/clears the HWND that receives WM_TELNET_STATUS. */
void TelnetServerSetStatusWnd(HWND hWnd);

/* Current client count — for status display. */
int  TelnetServerClientCount(void);

#ifdef __cplusplus
}
#endif
#endif /* PDW_TELNET_SERVER_H */
