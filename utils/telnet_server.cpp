/*
** telnet_server.cpp -- Telnet server for PDW
**
** Threading model:
**   Main/decoder thread  -> TelnetServerNotifyMessage / NotifyTxStart / NotifyTxStop
**                           (fan-out send() to all clients, append to backlog)
**   Worker thread        -> listen+accept, recv (CLIENT:/ROLE:), watchdog & auto-TX_STOP
**                           timers, cleanup disconnected slots, flush backlog to
**                           reconnected clients
**
** All shared state is protected by g_tsCs. send() runs under the lock; client
** sockets are non-blocking so send() returns immediately even on slow peers.
**
** Wire-format mirrors p2kflexDecoder (Server.cpp / Decode.cpp / Flex.cpp in
** the p2kflexDecoder source). See telnet_server.h for the framing.
*/

#ifndef STRICT
#define STRICT 1
#endif

#define _WINSOCK_DEPRECATED_NO_WARNINGS    /* inet_ntoa() in logs is fine here */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "..\headers\pdw.h"
#include "..\headers\misc.h"
#include "..\headers\initapp.h"
#include "telnet_server.h"
#include "rxq.h"

#pragma comment(lib, "ws2_32.lib")

extern TCHAR szPath[];               /* PDW exe directory — for log file fallback */
extern int   iCurrentCycle;          /* current FLEX cycle */
extern int   iCurrentFrame;          /* current FLEX frame */
extern double dRX_Quality;           /* computed in Misc.cpp CountBiterrors() */

/* ---------------------------------------------------------------------------
** Constants
** ---------------------------------------------------------------------------*/

#define TS_MAX_CLIENTS         25
#define TS_LINE_MAX            1100        /* max wire-line incl. \r and NUL */
#define TS_BACKLOG_LINES       256
#define TS_AUTO_TXSTOP_MS      3000u       /* idle threshold for auto TX_STOP */
#define TS_SELECT_TIMEOUT_SEC  1

/* ---------------------------------------------------------------------------
** Per-client slot
** ---------------------------------------------------------------------------*/

typedef struct {
    SOCKET      sock;
    SOCKADDR_IN addr;
    BOOL        used;
    BOOL        iacSent;
    int         sendErrCount;
    char        clientName[64];                /* from CLIENT:... */
    BOOL        hasName;
    BOOL        isMaster;
    BOOL        disconnected;                  /* socket closed but slot kept for replay */
    ULONGLONG   disconnectTickMs;
    BOOL        reconnectReplay;               /* emit <BUFFER_START> on next tick */
    int         replayCursor;                  /* index in g_tsBacklog when replaying */
    /* per-client recv buffer for line-based CLIENT:/ROLE: parsing */
    char        rxbuf[256];
    int         rxlen;
} TsClient;

/* ---------------------------------------------------------------------------
** Backlog ring buffer — one shared ring; clients that reconnect replay all
** entries newer than their disconnectTickMs.
** ---------------------------------------------------------------------------*/

typedef struct {
    char       line[TS_LINE_MAX];              /* already \r-terminated */
    int        len;                            /* incl. \r */
    ULONGLONG  ts;
} TsBacklogEntry;

/* ---------------------------------------------------------------------------
** Module state (file-static, all under g_tsCs)
** ---------------------------------------------------------------------------*/

static CRITICAL_SECTION g_tsCs;
static BOOL             g_tsCsInit       = FALSE;

static SOCKET           g_tsListenSock   = INVALID_SOCKET;
static HANDLE           g_tsThread       = NULL;
static volatile BOOL    g_tsRun          = FALSE;
static BOOL             g_tsWsaStarted   = FALSE;

static TsClient         g_tsClients[TS_MAX_CLIENTS];
static int              g_tsClientCount  = 0;

static TsBacklogEntry   g_tsBacklog[TS_BACKLOG_LINES];
static int              g_tsBacklogHead  = 0;            /* write position */
static int              g_tsBacklogCount = 0;            /* live entries */

static ULONGLONG        g_tsLastSendTickMs   = 0;        /* for WD heartbeat */
static ULONGLONG        g_tsLastNotifyTickMs = 0;        /* for auto TX_STOP */

static enum { TX_IDLE = 0, TX_ACTIVE = 1 } g_tsTxState = TX_IDLE;

static int              g_tsLastRxqEmitted   = 100;      /* last RXQ in TX_STOP */

/* Config snapshot taken at Init time */
static char             g_tsBind[64]         = "0.0.0.0";
static int              g_tsPort             = 8024;
static int              g_tsMaxClients       = TS_MAX_CLIENTS;
static int              g_tsWdSec            = 20;
static int              g_tsBufferTimeSec    = 60;
static BOOL             g_tsLogToFile        = FALSE;

/* Status window for live updates */
static HWND             g_tsStatusWnd        = NULL;

/* Log file — own critical section to avoid lock-inversion against g_tsCs */
static CRITICAL_SECTION g_tsLogCs;
static BOOL             g_tsLogCsInit        = FALSE;

/* ---------------------------------------------------------------------------
** Helpers
** ---------------------------------------------------------------------------*/

static ULONGLONG NowMs(void) { return GetTickCount64(); }

static void TsLog(const char *fmt, ...)
{
    if (!g_tsLogToFile || !g_tsLogCsInit) return;

    char path[MAX_PATH];
    if (Profile.LogfilePath[0]) {
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\pdw_telnet_server.log", Profile.LogfilePath);
    } else {
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\pdw_telnet_server.log", szPath);
    }

    EnterCriticalSection(&g_tsLogCs);
    FILE *fp = NULL;
    if (fopen_s(&fp, path, "a") == 0 && fp) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(fp, "%04d-%02d-%02d %02d:%02d:%02d.%03d  ",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap; va_start(ap, fmt);
        vfprintf(fp, fmt, ap);
        va_end(ap);
        fputc('\n', fp);
        fclose(fp);
    }
    LeaveCriticalSection(&g_tsLogCs);
}

static void PostStatus(int state)
{
    HWND h = g_tsStatusWnd;
    if (h) PostMessage(h, WM_TELNET_STATUS, (WPARAM)state, (LPARAM)g_tsClientCount);
}

/* ---------------------------------------------------------------------------
** Send one buffer to one client. Returns TRUE on full send, FALSE on error.
** Called only under g_tsCs. */
static BOOL SendToClient(int idx, const char *data, int len)
{
    if (idx < 0 || idx >= TS_MAX_CLIENTS) return FALSE;
    TsClient *c = &g_tsClients[idx];
    if (!c->used || c->disconnected || c->sock == INVALID_SOCKET) return FALSE;

    int sent = 0;
    while (sent < len) {
        int n = send(c->sock, data + sent, len - sent, 0);
        if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            /* Non-blocking socket: WOULDBLOCK means the kernel buffer is full.
            ** That's effectively a slow/stuck client — drop the partial line and
            ** count an error. Three strikes -> disconnect. */
            c->sendErrCount++;
            TsLog("send() to %s:%d failed err=%d (count=%d)",
                  inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port),
                  err, c->sendErrCount);
            if (c->sendErrCount > 2) {
                closesocket(c->sock);
                c->sock = INVALID_SOCKET;
                c->disconnected = TRUE;
                c->disconnectTickMs = NowMs();
                if (g_tsClientCount > 0) g_tsClientCount--;
            }
            return FALSE;
        }
        sent += n;
    }
    c->sendErrCount = 0;
    return TRUE;
}

/* Fan-out one wire-line to every connected client. Also appends to backlog so
** clients that disconnect within g_tsBufferTimeSec can replay missed lines. */
static void FanOutLine(const char *line, int len)
{
    g_tsLastSendTickMs = NowMs();

    /* 1. send to every active client */
    int i;
    for (i = 0; i < TS_MAX_CLIENTS; i++) {
        if (g_tsClients[i].used && !g_tsClients[i].disconnected) {
            SendToClient(i, line, len);
        }
    }

    /* 2. append to backlog (drops oldest if full) */
    if (len > 0 && len < TS_LINE_MAX) {
        TsBacklogEntry *e = &g_tsBacklog[g_tsBacklogHead];
        memcpy(e->line, line, len);
        e->line[len] = '\0';
        e->len = len;
        e->ts  = NowMs();
        g_tsBacklogHead = (g_tsBacklogHead + 1) % TS_BACKLOG_LINES;
        if (g_tsBacklogCount < TS_BACKLOG_LINES) g_tsBacklogCount++;
    }
}

/* Emit a literal marker (e.g. "<TX_START>"), append "\r" to match p2kflex. */
static void EmitMarker(const char *marker)
{
    char buf[TS_LINE_MAX];
    int  n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s\r", marker);
    if (n > 0) FanOutLine(buf, n);
}

/* ---------------------------------------------------------------------------
** TX-burst state machine
** ---------------------------------------------------------------------------*/

static void EmitTxStart_Locked(void)
{
    if (g_tsTxState == TX_ACTIVE) return;
    g_tsTxState = TX_ACTIVE;
    EmitMarker("<TX_START>");
    g_tsLastNotifyTickMs = NowMs();
}

static void EmitTxStop_Locked(void)
{
    if (g_tsTxState == TX_IDLE) return;
    g_tsTxState = TX_IDLE;

    /* RXQ from the p2kflex-compatible parallel track (utils/rxq.cpp), NOT
    ** PDW's dRX_Quality. The two algorithms diverge significantly on the
    ** same input signal; p2kflexMonitor's master/slave selection logic
    ** assumes p2kflexDecoder semantics, so we must mirror that path. */
    int rxq = (int)Rxq_GetEMA();
    if (rxq < 0)   rxq = 0;
    if (rxq > 100) rxq = 100;
    char trend = Rxq_GetTrend();         /* '+', '-' or 0 */
    g_tsLastRxqEmitted = rxq;

    char buf[TS_LINE_MAX];
    int  n;
    if (trend) n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "<TX_STOP><RXQ:%d%c>\r", rxq, trend);
    else       n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "<TX_STOP><RXQ:%d>\r",   rxq);
    if (n > 0) FanOutLine(buf, n);
}

/* ---------------------------------------------------------------------------
** Format Current_MSG[] into a wire-line. Returns the byte count (incl. \r).
** ---------------------------------------------------------------------------*/

/* Map PDW vtype[] strings (Flex.cpp:131) to p2kflex wire-tags */
static const char *MapFlexTag(const char *type)
{
    if (!type) return "-ALPHA-";
    /* vtype[] entries are 7 chars wide, padded with spaces. Compare on
    ** the meaningful substring (trimmed). */
    if (strstr(type, "ALPHA"))   return "-ALPHA-";
    if (strstr(type, "StNUM"))   return "-StNUM-";
    if (strstr(type, "SfNUM"))   return "-SfNUM-";
    if (strstr(type, "NuNUM"))   return "-NuNUM-";
    if (strstr(type, "SH/TONE")) return "SH/TONE";
    if (strstr(type, "BINARY"))  return "BINARY-";
    if (strstr(type, "INSTR"))   return "-INSTR-";
    if (strstr(type, "GROUP"))   return "-ALPHA-";   /* ConvertGroupcall path */
    if (strstr(type, "SECURE"))  return "SECURE-";
    return "-ALPHA-";
}

/* Map PDW POCSAG MSG_TYPE (set in Pocsag.cpp show_addr): " ALPHA " / "NUMERIC" / "TONE ONLY" */
static const char *MapPocsagTag(const char *type)
{
    if (!type) return "-ALPHA-";
    if (strstr(type, "TONE"))    return "SH/TONE";
    if (strstr(type, "NUMERIC")) return "-NuNUM-";
    if (strstr(type, "ALPHA"))   return "-ALPHA-";
    return "-ALPHA-";
}

static int BuildLineFromCurrentMsg(char *out, int outsz)
{
    const char *mode    = Current_MSG[MSG_MODE];
    const char *type    = Current_MSG[MSG_TYPE];
    const char *capcode = Current_MSG[MSG_CAPCODE];
    const char *body    = Current_MSG[MSG_MESSAGE];

    if (!mode || !mode[0]) return 0;

    int n = 0;

    if (strncmp(mode, "FLEX", 4) == 0 || strncmp(mode, "REFLEX", 6) == 0) {
        /* FLEX: "CC/FFF -ALPHA- 1234567 message"
        ** '/' becomes '\' if uncorrectable biterrors — proxy: messageitems_colors[6]
        ** equals COLOR_BITERRORS. Best-effort; not all decode paths set this. */
        char sep = '/';
        if (messageitems_colors[6] == COLOR_BITERRORS) sep = '\\';

        const char *tag = MapFlexTag(type);
        n = _snprintf_s(out, outsz, _TRUNCATE, "%02d%c%03d %s %s %s\r",
                        iCurrentCycle, sep, iCurrentFrame, tag,
                        capcode ? capcode : "0000000",
                        body ? body : "");
    }
    else if (strncmp(mode, "POCSAG", 6) == 0) {
        /* POCSAG: "-ALPHA- 1234567-N message" — function from "POCSAG-N". */
        int func = 0;
        if (mode[7] >= '0' && mode[7] <= '9') func = mode[7] - '0';

        const char *tag = MapPocsagTag(type);
        if (strstr(type ? type : "", "TONE")) {
            n = _snprintf_s(out, outsz, _TRUNCATE, "%s %s-%d TONE ONLY\r",
                            tag, capcode ? capcode : "0000000", func);
        } else {
            n = _snprintf_s(out, outsz, _TRUNCATE, "%s %s-%d %s\r",
                            tag, capcode ? capcode : "0000000", func,
                            body ? body : "");
        }
    }
    else {
        /* ACARS / MOBITEX / ERMES — not in p2kflex protocol scope; skip. */
        return 0;
    }

    if (n < 0) n = 0;
    return n;
}

/* ---------------------------------------------------------------------------
** Public API — main thread
** ---------------------------------------------------------------------------*/

void TelnetServerNotifyMessage(void)
{
    if (!Profile.telnetServerEnabled || !g_tsCsInit) return;

    EnterCriticalSection(&g_tsCs);

    /* Lazy TX_START — if we got a message without an explicit sync event, the
    ** state machine still emits a coherent <TX_START>...<TX_STOP> burst. */
    if (g_tsTxState == TX_IDLE) EmitTxStart_Locked();

    char buf[TS_LINE_MAX];
    int  n = BuildLineFromCurrentMsg(buf, sizeof(buf));
    if (n > 0) FanOutLine(buf, n);

    g_tsLastNotifyTickMs = NowMs();
    LeaveCriticalSection(&g_tsCs);
}

void TelnetServerNotifyTxStart(void)
{
    if (!Profile.telnetServerEnabled || !g_tsCsInit) return;
    EnterCriticalSection(&g_tsCs);
    EmitTxStart_Locked();
    LeaveCriticalSection(&g_tsCs);
}

void TelnetServerNotifyTxStop(void)
{
    if (!Profile.telnetServerEnabled || !g_tsCsInit) return;
    EnterCriticalSection(&g_tsCs);
    EmitTxStop_Locked();
    LeaveCriticalSection(&g_tsCs);
}

void TelnetServerNotifyInstr(long subscriberCapcode, int groupCapcode, int assignedFrame)
{
    if (!Profile.telnetServerEnabled || !g_tsCsInit) return;

    EnterCriticalSection(&g_tsCs);
    if (g_tsTxState == TX_IDLE) EmitTxStart_Locked();

    char buf[TS_LINE_MAX];
    int  n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                         "%02d/%03d -INSTR- %07ld %07d %03d\r",
                         iCurrentCycle, iCurrentFrame,
                         subscriberCapcode, groupCapcode, assignedFrame);
    if (n > 0) FanOutLine(buf, n);

    g_tsLastNotifyTickMs = NowMs();
    LeaveCriticalSection(&g_tsCs);
}

void TelnetServerNotifyFrame(const char *body)
{
    if (!Profile.telnetServerEnabled || !g_tsCsInit) return;
    if (!body) body = "";

    EnterCriticalSection(&g_tsCs);
    if (g_tsTxState == TX_IDLE) EmitTxStart_Locked();

    char buf[TS_LINE_MAX];
    int  n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                         "%02d/%03d -FRAME- 0000000 %s\r",
                         iCurrentCycle, iCurrentFrame, body);
    if (n > 0) FanOutLine(buf, n);

    g_tsLastNotifyTickMs = NowMs();
    LeaveCriticalSection(&g_tsCs);
}

void TelnetServerSetStatusWnd(HWND hWnd)
{
    if (!g_tsCsInit) { g_tsStatusWnd = hWnd; return; }
    EnterCriticalSection(&g_tsCs);
    g_tsStatusWnd = hWnd;
    LeaveCriticalSection(&g_tsCs);
}

int TelnetServerClientCount(void)
{
    if (!g_tsCsInit) return 0;
    EnterCriticalSection(&g_tsCs);
    int n = g_tsClientCount;
    LeaveCriticalSection(&g_tsCs);
    return n;
}

/* ---------------------------------------------------------------------------
** Worker-thread routines
** ---------------------------------------------------------------------------*/

/* Search for a disconnected slot with the same IP — used for replay-on-reconnect.
** Caller holds g_tsCs. Returns slot index or -1. */
static int FindDisconnectedSlotByIp(const SOCKADDR_IN *peer)
{
    int i;
    ULONGLONG now = NowMs();
    for (i = 0; i < TS_MAX_CLIENTS; i++) {
        TsClient *c = &g_tsClients[i];
        if (!c->used || !c->disconnected) continue;
        if ((now - c->disconnectTickMs) > (ULONGLONG)g_tsBufferTimeSec * 1000) continue;
        if (c->addr.sin_addr.s_addr == peer->sin_addr.s_addr) return i;
    }
    return -1;
}

static int FindFreeSlot(void)
{
    int i;
    for (i = 0; i < TS_MAX_CLIENTS; i++) if (!g_tsClients[i].used) return i;
    return -1;
}

/* IAC negotiation — same 6 bytes as p2kflexDecoder sends on accept. */
static void SendIacGreeting(int idx)
{
    static const unsigned char iac[] = {
        0xFF, 0xFB, 0x01,    /* IAC WILL ECHO */
        0xFF, 0xFB, 0x03     /* IAC WILL SUPPRESS-GO-AHEAD */
    };
    SendToClient(idx, (const char *)iac, (int)sizeof(iac));
    g_tsClients[idx].iacSent = TRUE;
}

/* Called when select() flags the listening socket — accept one client. */
static void AcceptOneClient(void)
{
    SOCKADDR_IN peer; int peerLen = (int)sizeof(peer);
    SOCKET s = accept(g_tsListenSock, (sockaddr *)&peer, &peerLen);
    if (s == INVALID_SOCKET) return;

    /* Make the new socket non-blocking — keeps fan-out send() from stalling. */
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);

    EnterCriticalSection(&g_tsCs);

    /* Refuse over max-clients */
    int active = 0;
    for (int i = 0; i < TS_MAX_CLIENTS; i++)
        if (g_tsClients[i].used && !g_tsClients[i].disconnected) active++;
    if (active >= g_tsMaxClients) {
        LeaveCriticalSection(&g_tsCs);
        closesocket(s);
        TsLog("Reject %s:%d — max clients (%d) reached",
              inet_ntoa(peer.sin_addr), ntohs(peer.sin_port), g_tsMaxClients);
        return;
    }

    /* Reconnect detection — re-use disconnected slot from same IP */
    int idx = FindDisconnectedSlotByIp(&peer);
    if (idx >= 0) {
        TsClient *c = &g_tsClients[idx];
        c->sock            = s;
        c->addr            = peer;
        c->disconnected    = FALSE;
        c->iacSent         = FALSE;
        c->sendErrCount    = 0;
        c->reconnectReplay = TRUE;
        c->replayCursor    = -1;       /* set on first replay tick */
        c->rxlen           = 0;
        g_tsClientCount++;
        TsLog("Reconnect %s:%d (slot %d, name='%s')",
              inet_ntoa(peer.sin_addr), ntohs(peer.sin_port), idx, c->clientName);
        SendIacGreeting(idx);
        LeaveCriticalSection(&g_tsCs);
        PostStatus(TSS_LISTENING);
        return;
    }

    /* Brand-new client */
    idx = FindFreeSlot();
    if (idx < 0) {
        LeaveCriticalSection(&g_tsCs);
        closesocket(s);
        TsLog("No free slot for %s:%d", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
        return;
    }

    TsClient *c = &g_tsClients[idx];
    memset(c, 0, sizeof(*c));
    c->sock = s;
    c->addr = peer;
    c->used = TRUE;
    g_tsClientCount++;

    TsLog("Accept  %s:%d (slot %d)",
          inet_ntoa(peer.sin_addr), ntohs(peer.sin_port), idx);
    SendIacGreeting(idx);

    LeaveCriticalSection(&g_tsCs);
    PostStatus(TSS_LISTENING);
}

/* Process one line of received input (CLIENT:... or ROLE:...). Caller holds g_tsCs. */
static void ProcessClientLine(int idx, const char *line)
{
    if (!line) return;
    TsClient *c = &g_tsClients[idx];

    if (strncmp(line, "CLIENT:", 7) == 0) {
        strncpy_s(c->clientName, sizeof(c->clientName), line + 7, _TRUNCATE);
        c->hasName = TRUE;
        TsLog("Slot %d identifies as '%s'", idx, c->clientName);
    }
    else if (strncmp(line, "ROLE:", 5) == 0) {
        const char *role = line + 5;
        if      (strcmp(role, "MASTER") == 0) c->isMaster = TRUE;
        else if (strcmp(role, "SLAVE")  == 0) c->isMaster = FALSE;
        TsLog("Slot %d role='%s'", idx, role);
    }
    /* Unknown lines are silently dropped — same as p2kflex behavior. */
}

/* Read available bytes from one client; parse CLIENT:/ROLE: lines.
** IAC byte sequences (FF xx [yy]) in the stream are skipped over.
** Caller holds g_tsCs. */
static void RecvFromOne(int idx)
{
    TsClient *c = &g_tsClients[idx];
    if (!c->used || c->disconnected) return;

    char tmp[256];
    int n = recv(c->sock, tmp, (int)sizeof(tmp), 0);
    if (n == 0) {
        /* Orderly close from peer */
        TsLog("Slot %d peer closed", idx);
        closesocket(c->sock);
        c->sock = INVALID_SOCKET;
        c->disconnected = TRUE;
        c->disconnectTickMs = NowMs();
        if (g_tsClientCount > 0) g_tsClientCount--;
        return;
    }
    if (n == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return;
        TsLog("Slot %d recv err=%d", idx, err);
        closesocket(c->sock);
        c->sock = INVALID_SOCKET;
        c->disconnected = TRUE;
        c->disconnectTickMs = NowMs();
        if (g_tsClientCount > 0) g_tsClientCount--;
        return;
    }

    /* Filter IAC sequences and append to per-client line buffer */
    for (int i = 0; i < n; i++) {
        unsigned char b = (unsigned char)tmp[i];
        if (b == 0xFF) {
            /* IAC ... skip next 1-2 bytes (command + optional option) */
            if (i + 1 < n) {
                unsigned char cmd = (unsigned char)tmp[i + 1];
                if (cmd >= 251 && cmd <= 254) { i += 2; continue; }   /* WILL/WONT/DO/DONT */
                i += 1; continue;
            }
            continue;
        }
        if (b == '\r') continue;                     /* normalize CRLF */
        if (b == '\n') {
            c->rxbuf[c->rxlen] = '\0';
            if (c->rxlen > 0) ProcessClientLine(idx, c->rxbuf);
            c->rxlen = 0;
            continue;
        }
        if (c->rxlen < (int)sizeof(c->rxbuf) - 1) c->rxbuf[c->rxlen++] = (char)b;
    }
}

/* Reclaim slots whose disconnect window has expired. Caller holds g_tsCs. */
static void GarbageCollectSlots(void)
{
    ULONGLONG now = NowMs();
    for (int i = 0; i < TS_MAX_CLIENTS; i++) {
        TsClient *c = &g_tsClients[i];
        if (c->used && c->disconnected &&
            (now - c->disconnectTickMs) > (ULONGLONG)g_tsBufferTimeSec * 1000) {
            TsLog("GC slot %d (name='%s')", i, c->clientName);
            memset(c, 0, sizeof(*c));
            c->sock = INVALID_SOCKET;
        }
    }
}

/* For each slot flagged reconnectReplay, dribble out backlog entries newer than
** its disconnect time, framed by <BUFFER_START> ... <BUFFER_STOP>.
** Done a few per tick so a huge backlog doesn't lock the worker. Caller holds g_tsCs. */
static void FlushReplay(void)
{
    for (int i = 0; i < TS_MAX_CLIENTS; i++) {
        TsClient *c = &g_tsClients[i];
        if (!c->used || c->disconnected || !c->reconnectReplay) continue;

        /* First tick: emit <BUFFER_START> and locate the oldest qualifying entry */
        if (c->replayCursor < 0) {
            static const char hdr[] = "<BUFFER_START>\r";
            SendToClient(i, hdr, (int)sizeof(hdr) - 1);

            /* Walk backlog oldest-to-newest. Oldest entry index =
            ** (head - count + N) mod N. Skip entries older than disconnect time. */
            int n   = g_tsBacklogCount;
            int idx = (g_tsBacklogHead - n + TS_BACKLOG_LINES) % TS_BACKLOG_LINES;
            while (n > 0 && g_tsBacklog[idx].ts < c->disconnectTickMs) {
                idx = (idx + 1) % TS_BACKLOG_LINES;
                n--;
            }
            c->replayCursor = idx;
        }

        /* Emit up to 16 entries per tick */
        int budget = 16;
        while (budget-- > 0) {
            if (c->replayCursor == g_tsBacklogHead) {
                /* done */
                static const char ftr[] = "<BUFFER_STOP>\r";
                SendToClient(i, ftr, (int)sizeof(ftr) - 1);
                c->reconnectReplay = FALSE;
                c->replayCursor    = -1;
                break;
            }
            const TsBacklogEntry *e = &g_tsBacklog[c->replayCursor];
            if (e->len > 0) SendToClient(i, e->line, e->len);
            c->replayCursor = (c->replayCursor + 1) % TS_BACKLOG_LINES;
        }
    }
}

/* Worker loop: select() on listen + all client sockets with 1 s timeout.
** Each iteration we also check timers (auto TX_STOP, WD heartbeat, GC, replay). */
static DWORD WINAPI TelnetWorker(LPVOID /*arg*/)
{
    TsLog("Worker started, listening on %s:%d", g_tsBind, g_tsPort);

    while (g_tsRun) {
        fd_set rd;
        FD_ZERO(&rd);
        int maxfd = 0;
        SOCKET listenCopy = g_tsListenSock;
        if (listenCopy != INVALID_SOCKET) {
            FD_SET(listenCopy, &rd);
            if ((int)listenCopy > maxfd) maxfd = (int)listenCopy;
        }

        EnterCriticalSection(&g_tsCs);
        for (int i = 0; i < TS_MAX_CLIENTS; i++) {
            if (g_tsClients[i].used && !g_tsClients[i].disconnected
                && g_tsClients[i].sock != INVALID_SOCKET) {
                FD_SET(g_tsClients[i].sock, &rd);
                if ((int)g_tsClients[i].sock > maxfd) maxfd = (int)g_tsClients[i].sock;
            }
        }
        LeaveCriticalSection(&g_tsCs);

        timeval tv;
        tv.tv_sec  = TS_SELECT_TIMEOUT_SEC;
        tv.tv_usec = 0;
        int r = select(maxfd + 1, &rd, NULL, NULL, &tv);

        if (!g_tsRun) break;

        if (r > 0) {
            if (listenCopy != INVALID_SOCKET && FD_ISSET(listenCopy, &rd)) {
                AcceptOneClient();
            }
            EnterCriticalSection(&g_tsCs);
            for (int i = 0; i < TS_MAX_CLIENTS; i++) {
                if (g_tsClients[i].used && !g_tsClients[i].disconnected
                    && g_tsClients[i].sock != INVALID_SOCKET
                    && FD_ISSET(g_tsClients[i].sock, &rd)) {
                    RecvFromOne(i);
                }
            }
            LeaveCriticalSection(&g_tsCs);
        }

        /* Timers and housekeeping */
        EnterCriticalSection(&g_tsCs);
        ULONGLONG now = NowMs();

        /* Auto TX_STOP after ~3 s idle */
        if (g_tsTxState == TX_ACTIVE && (now - g_tsLastNotifyTickMs) > TS_AUTO_TXSTOP_MS) {
            EmitTxStop_Locked();
        }

        /* Watchdog <WD> heartbeat */
        if (g_tsClientCount > 0 && g_tsWdSec > 0
            && (now - g_tsLastSendTickMs) > (ULONGLONG)g_tsWdSec * 1000) {
            EmitMarker("<WD>");
        }

        GarbageCollectSlots();
        FlushReplay();
        LeaveCriticalSection(&g_tsCs);
    }

    TsLog("Worker stopping");
    return 0;
}

/* ---------------------------------------------------------------------------
** Lifecycle
** ---------------------------------------------------------------------------*/

static BOOL EnsureWsaStartup(void)
{
    if (g_tsWsaStarted) return TRUE;
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return FALSE;
    g_tsWsaStarted = TRUE;
    return TRUE;
}

static BOOL OpenListenSocket(void)
{
    if (!EnsureWsaStartup()) return FALSE;

    g_tsListenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_tsListenSock == INVALID_SOCKET) return FALSE;

    /* SO_REUSEADDR avoids "address already in use" after a quick restart */
    int yes = 1;
    setsockopt(g_tsListenSock, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));

    SOCKADDR_IN a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons((u_short)g_tsPort);

    if (strcmp(g_tsBind, "0.0.0.0") == 0 || g_tsBind[0] == '\0') {
        a.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        a.sin_addr.s_addr = inet_addr(g_tsBind);
        if (a.sin_addr.s_addr == INADDR_NONE) {
            TsLog("Bad bind address '%s', falling back to 0.0.0.0", g_tsBind);
            a.sin_addr.s_addr = htonl(INADDR_ANY);
        }
    }

    if (bind(g_tsListenSock, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        TsLog("bind() failed err=%d on %s:%d", WSAGetLastError(), g_tsBind, g_tsPort);
        closesocket(g_tsListenSock);
        g_tsListenSock = INVALID_SOCKET;
        return FALSE;
    }

    if (listen(g_tsListenSock, TS_MAX_CLIENTS) == SOCKET_ERROR) {
        TsLog("listen() failed err=%d", WSAGetLastError());
        closesocket(g_tsListenSock);
        g_tsListenSock = INVALID_SOCKET;
        return FALSE;
    }

    /* Non-blocking — select() will gate accept() */
    u_long nb = 1;
    ioctlsocket(g_tsListenSock, FIONBIO, &nb);
    return TRUE;
}

void TelnetServerInit(void)
{
    if (!g_tsCsInit) {
        InitializeCriticalSection(&g_tsCs);
        InitializeCriticalSection(&g_tsLogCs);
        g_tsLogCsInit = TRUE;
        g_tsCsInit    = TRUE;
        for (int i = 0; i < TS_MAX_CLIENTS; i++) {
            g_tsClients[i].sock = INVALID_SOCKET;
        }
    }

    TelnetServerShutdown();    /* clean restart if already running */

    if (!Profile.telnetServerEnabled) {
        PostStatus(TSS_DISABLED);
        return;
    }

    /* Snapshot config from Profile under our own lock — once at startup */
    EnterCriticalSection(&g_tsCs);
    strncpy_s(g_tsBind, sizeof(g_tsBind), Profile.szTelnetServerBind, _TRUNCATE);
    if (g_tsBind[0] == '\0') strcpy_s(g_tsBind, sizeof(g_tsBind), "0.0.0.0");
    g_tsPort          = Profile.telnetServerPort     > 0    ? Profile.telnetServerPort     : 8024;
    g_tsMaxClients    = Profile.telnetServerMaxClients > 0  ? Profile.telnetServerMaxClients : TS_MAX_CLIENTS;
    if (g_tsMaxClients > TS_MAX_CLIENTS) g_tsMaxClients = TS_MAX_CLIENTS;
    g_tsWdSec         = Profile.telnetServerWdSec    > 0    ? Profile.telnetServerWdSec    : 20;
    g_tsBufferTimeSec = Profile.telnetServerBufferTime > 0  ? Profile.telnetServerBufferTime : 60;
    g_tsLogToFile     = Profile.telnetServerLogToFile ? TRUE : FALSE;

    g_tsBacklogHead     = 0;
    g_tsBacklogCount    = 0;
    g_tsLastSendTickMs   = NowMs();
    g_tsLastNotifyTickMs = NowMs();
    g_tsTxState          = TX_IDLE;
    LeaveCriticalSection(&g_tsCs);

    if (!OpenListenSocket()) {
        PostStatus(TSS_ERROR);
        return;
    }

    g_tsRun    = TRUE;
    g_tsThread = CreateThread(NULL, 0, TelnetWorker, NULL, 0, NULL);
    if (!g_tsThread) {
        g_tsRun = FALSE;
        closesocket(g_tsListenSock);
        g_tsListenSock = INVALID_SOCKET;
        PostStatus(TSS_ERROR);
        return;
    }
    PostStatus(TSS_LISTENING);
}

void TelnetServerShutdown(void)
{
    if (!g_tsCsInit) return;

    if (g_tsRun) {
        g_tsRun = FALSE;
        /* Closing the listen socket unblocks select(). */
        if (g_tsListenSock != INVALID_SOCKET) {
            closesocket(g_tsListenSock);
            g_tsListenSock = INVALID_SOCKET;
        }
        if (g_tsThread) {
            WaitForSingleObject(g_tsThread, 5000);
            CloseHandle(g_tsThread);
            g_tsThread = NULL;
        }
    } else if (g_tsListenSock != INVALID_SOCKET) {
        closesocket(g_tsListenSock);
        g_tsListenSock = INVALID_SOCKET;
    }

    /* Close all client sockets */
    EnterCriticalSection(&g_tsCs);
    for (int i = 0; i < TS_MAX_CLIENTS; i++) {
        TsClient *c = &g_tsClients[i];
        if (c->used && c->sock != INVALID_SOCKET) closesocket(c->sock);
        memset(c, 0, sizeof(*c));
        c->sock = INVALID_SOCKET;
    }
    g_tsClientCount = 0;
    LeaveCriticalSection(&g_tsCs);

    PostStatus(TSS_DISABLED);
}

void TelnetServerDestroy(void)
{
    TelnetServerShutdown();

    if (g_tsCsInit) {
        DeleteCriticalSection(&g_tsCs);
        g_tsCsInit = FALSE;
    }
    if (g_tsLogCsInit) {
        DeleteCriticalSection(&g_tsLogCs);
        g_tsLogCsInit = FALSE;
    }
    if (g_tsWsaStarted) {
        WSACleanup();
        g_tsWsaStarted = FALSE;
    }
}
