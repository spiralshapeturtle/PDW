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
#define TS_TXSTOP_DEBOUNCE_MS  2500u       /* FIX [TelnetServer]: suppress false EOT TX_STOP */
#define TS_SELECT_TIMEOUT_SEC  1

/* RS232 / AUDIO watchdog.
** FIX [RS232Flap]: aligned with rs232.cpp RS232_STALL_MS (5000) + RS232_WARMUP_MS
** (6000) so a Moxa worker-reconnect cycle no longer fires <RS232:0> while the
** lower layer is still recovering normally. Natural FLEX inter-burst silences
** are also covered. Decoded message arrival is independent of this threshold —
** decoder consumes from rs232_linedata[] ring, not from this heartbeat. */
#define TS_RS232_TIMEOUT_MS       10000u   /* no data → <RS232:0> (was 2000u, p2kflex parity) */
#define TS_RS232_STARTUP_MS       10000u   /* no data at startup → <RS232:0> (was 2000u) */
#define TS_AUDIO_WINDOW_MS         1000u   /* bit-transition counting window */
#define TS_AUDIO_SILENCE_THRESHOLD   10    /* min transitions/window = audio present */
#define TS_AUDIO_SILENCE_WINDOWS      4    /* consecutive silent windows → <AUDIO:0> */

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
    int         replayLineCount;               /* incremented per replayed line, reported in event */
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
static BOOL             g_tsTxStopPending    = FALSE;   /* FIX [TelnetServer]: TX_STOP debounce */
static ULONGLONG        g_tsTxStopRequestMs  = 0;

/* RS232 / AUDIO state machine (active only in RS232/slicer input mode).
** All fields are protected by g_tsCs. */
static int              g_tsRS232Enabled     = 0;        /* 1 while rs232 mode active */
static int              g_tsRS232State       = -1;       /* -1=unset, 0=lost, 1=active */
static BOOL             g_tsRS232Initialized = FALSE;    /* first data seen or startup expired */
static int              g_tsRS232SilentTicks = 0;        /* FIX [RS232Flap]: 2-of-2 hysteresis counter */
static ULONGLONG        g_tsRS232EnabledMs   = 0;        /* tick when Enable(1) was called */
static ULONGLONG        g_tsRS232LastHbMs    = 0;        /* tick of last bytes received */
static int              g_tsAudioState       = -1;       /* -1=unset, 0=silent, 1=active */
static int              g_tsAudioSilWin      = 0;        /* consecutive silent windows */
static BOOL             g_tsAudioWarmup      = TRUE;     /* discard first window (p2kflex) */
static uint32_t         g_tsAudioTransitions = 0;        /* bit-transitions this window */
static BYTE             g_tsAudioLastByte    = 0;        /* previous byte for XOR diff */
static ULONGLONG        g_tsAudioWinMs       = 0;        /* window start tick (0 = unset) */

/* Config snapshot taken at Init time */
static char             g_tsBind[64]         = "0.0.0.0";
static int              g_tsPort             = 8024;
static int              g_tsMaxClients       = TS_MAX_CLIENTS;
static int              g_tsWdSec            = 20;
static int              g_tsBufferTimeSec    = 60;
static BOOL             g_tsLogToFile        = FALSE;
static BOOL             g_tsEnabled          = FALSE;   /* current running state */

/* Status window for live updates */
static HWND             g_tsStatusWnd        = NULL;

/* Forward declarations — keep new event-helpers above TsLog() definition without
** moving the original code block. */
static void TsLog(const char *fmt, ...);

/* Log file — own critical section to avoid lock-inversion against g_tsCs */
static CRITICAL_SECTION g_tsLogCs;
static BOOL             g_tsLogCsInit        = FALSE;

/* In-memory event ring for the Ctrl-N "Recent activity" listbox. Drained via
** TelnetServerGetEvents() with a periodic timer in the dialog. Sized so a
** moderately busy session (~1 event every few seconds during reconnects)
** keeps history for ~5-10 minutes. */
#define TS_EVENT_RING_SIZE  64
static TsEvent g_tsEvents[TS_EVENT_RING_SIZE];
static int     g_tsEventsHead = 0;     /* next write slot */
static int     g_tsEventsCount = 0;

static void TsEventPush_Locked(const char *text)
{
    if (!text) return;
    TsEvent *e = &g_tsEvents[g_tsEventsHead];
    e->ts_ms = (long long)GetTickCount64();
    strncpy_s(e->text, sizeof(e->text), text, _TRUNCATE);
    g_tsEventsHead = (g_tsEventsHead + 1) % TS_EVENT_RING_SIZE;
    if (g_tsEventsCount < TS_EVENT_RING_SIZE) g_tsEventsCount++;
}

/* Format-and-log helper. Writes to the disk log (if enabled) AND to the
** in-memory event ring. Caller may or may not hold g_tsCs; both paths are
** independently protected. */
static void TsLogEvent(const char *fmt, ...)
{
    char line[128];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);

    /* Disk log */
    TsLog("%s", line);

    /* In-memory ring (own critical section to avoid lock-order issues) */
    if (g_tsCsInit) {
        EnterCriticalSection(&g_tsCs);
        TsEventPush_Locked(line);
        LeaveCriticalSection(&g_tsCs);
    }
}

/* Wire-log: every wire-line that goes out can optionally be appended to a
** disk file in CS FlexDecoder format. Separate from g_tsLogToFile (which is
** for lifecycle events). Uses g_tsLogCs to serialize. */
/* FIX [WireLogRotate]: maandafkortingen voor de niet-numerieke datumvorm,
** identiek aan debuglog.cpp zodat beide PDW-logs dezelfde naamgeving delen. */
static const char *kWireMonthAbbr[12] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

static void TsWriteWireLog(const char *line, int len)
{
    if (!Profile.telnetServerWireLog || !g_tsLogCsInit) return;
    if (!line || len <= 0) return;
    if (len >= 4 && memcmp(line, "<WD>", 4) == 0) return; // FIX [WireLogWD]: <WD> niet naar disk, wel over de lijn

    SYSTEMTIME st; GetLocalTime(&st);

    // FIX [WireLogRotate]: dagelijks geroteerde bestandsnaam met de standaard PDW-
    // datumnotatie (zelfde als de live debug log, debuglog.cpp). Voorheen schreef dit
    // naar één oneindig groeiend pdw_flexdecoder.log; nu één bestand per dag zodat een
    // vergelijk-script per dag op bestandsnaam kan koppelen aan de p2kflexDecoder-logs.
    const char *root = (Profile.LogfilePath[0]) ? Profile.LogfilePath : szPath;
    char path[MAX_PATH];
    if (Profile.MonthNumber) {
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%02d%02d%02d_telnet_traffic.log",
                    root, st.wYear % 100, st.wMonth, st.wDay);
    } else {
        const char *mon = (st.wMonth >= 1 && st.wMonth <= 12) ? kWireMonthAbbr[st.wMonth - 1] : "???";
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%02d%s%02d_telnet_traffic.log",
                    root, st.wYear % 100, mon, st.wDay);
    }

    EnterCriticalSection(&g_tsLogCs);
    FILE *fp = NULL;
    if (fopen_s(&fp, path, "a") == 0 && fp) {
        fprintf(fp, "%04d-%02d-%02d %02d:%02d:%02d.%03d  ",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        /* line already ends with \r; strip and append \n for log readability. */
        int writeLen = len;
        while (writeLen > 0 && (line[writeLen-1] == '\r' || line[writeLen-1] == '\n')) writeLen--;
        fwrite(line, 1, writeLen, fp);
        fputc('\n', fp);
        fclose(fp);
    }
    LeaveCriticalSection(&g_tsLogCs);
}

/* ---------------------------------------------------------------------------
** Helpers
** ---------------------------------------------------------------------------*/

static ULONGLONG NowMs(void) { return GetTickCount64(); }

static void TsLog(const char *fmt, ...)
{
    if (!g_tsLogToFile || !g_tsLogCsInit) return;

    char path[MAX_PATH];
    SYSTEMTIME st; GetLocalTime(&st);
    if (Profile.LogfilePath[0]) {
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%02d%02d%02d_telnet_server.log",
                    Profile.LogfilePath, st.wYear % 100, st.wMonth, st.wDay);
    } else {
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%02d%02d%02d_telnet_server.log",
                    szPath, st.wYear % 100, st.wMonth, st.wDay);
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
    // FIX [TelnetStatusLock]: g_tsStatusWnd en g_tsClientCount worden elders onder g_tsCs
    // geschreven; lees ze hier ook onder de lock voor een consistente snapshot. Windows
    // CRITICAL_SECTION is re-entrant, dus dit is veilig ook als de caller de lock al houdt.
    HWND h;
    int  count;
    if (g_tsCsInit) EnterCriticalSection(&g_tsCs);
    h     = g_tsStatusWnd;
    count = g_tsClientCount;
    if (g_tsCsInit) LeaveCriticalSection(&g_tsCs);
    if (h) PostMessage(h, WM_TELNET_STATUS, (WPARAM)state, (LPARAM)count);
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

    /* 3. optional disk log of every emitted wire-line, in CS FlexDecoder format. */
    TsWriteWireLog(line, len);
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
        // FIX [TelnetMode7]: lees mode[7] alleen als mode[6] niet de NUL is — bij een kale
        // "POCSAG" (zonder "-N") wees mode[7] voorbij de string-terminator.
        if (mode[6] && mode[7] >= '0' && mode[7] <= '9') func = mode[7] - '0';

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
** RS232 / AUDIO state machine — helpers (all called under g_tsCs)
** ---------------------------------------------------------------------------*/

/* Portable 8-bit popcount — counts set bits in b. Used like __popcnt in
** p2kflexDecoder to measure bit transitions between consecutive bytes. */
static int BitCount8(BYTE b)
{
    b = b - ((b >> 1) & 0x55u);
    b = (BYTE)((b & 0x33u) + ((b >> 2) & 0x33u));
    return (int)((b + (b >> 4)) & 0x0Fu);
}

/* FIX [TsShutdownRs232]: bij graceful app-shutdown onderdrukken we <RS232:0>
** zodat de remote slave niet in exponential backoff gaat. De TCP-close van
** TelnetServerDestroy() signaleert de remote al dat de sessie eindigt; <RS232:1>
** bij (her)start blijft ongemoeid. Reconfig (COM-poort wijzigen) zet deze vlag
** NIET en blijft dus wel <RS232:0>/<RS232:1> emitten. */
static volatile BOOL g_tsShuttingDown = FALSE;
void TelnetServerBeginShutdown(void) { g_tsShuttingDown = TRUE; }

static void EmitRS232_Locked(int active)
{
    if (active == g_tsRS232State) return;
    if (!active && g_tsShuttingDown) return;   // FIX [TsShutdownRs232]: geen <RS232:0> bij afsluiten
    g_tsRS232State = active;
    TsLogEvent(active ? "<RS232:1> RS232 data active" : "<RS232:0> RS232 data lost");
    EmitMarker(active ? "<RS232:1>" : "<RS232:0>");
}

static void EmitAudio_Locked(int active)
{
    if (active == g_tsAudioState) return;
    g_tsAudioState = active;
    TsLogEvent(active ? "<AUDIO:1> Audio present" : "<AUDIO:0> Audio lost");
    EmitMarker(active ? "<AUDIO:1>" : "<AUDIO:0>");
}

/* Called from worker loop every ~1 s. Mirrors p2kflexDecoder CheckRS232Watchdog(). */
static void CheckRS232Watchdog_Locked(ULONGLONG now)
{
    if (!g_tsRS232Enabled) return;

    if (!g_tsRS232Initialized) {
        /* Startup window: if no data arrives within TS_RS232_STARTUP_MS, declare lost */
        if ((now - g_tsRS232EnabledMs) > TS_RS232_STARTUP_MS) {
            g_tsRS232Initialized = TRUE;
            EmitRS232_Locked(0);
        }
        return;
    }

    /* FIX [RS232Flap]: 2-of-2 hysteresis on the way down. Threshold is 10 s
    ** (above); worker ticks ~1 Hz so 2 confirmations gives a ~11 s floor for
    ** 1→0, exceeding rs232.cpp's RS232_STALL_MS (5 s) + RS232_WARMUP_MS (6 s)
    ** budget and natural FLEX inter-burst silences. Any byte arrival resets
    ** the counter via the else-branch on the next tick (LastHbMs gets bumped
    ** in BytesReceived/SlicerActivity → delta drops back under threshold).
    ** The 0→1 transition stays instantaneous in the producer paths. */
    if (g_tsRS232State == 1 && (now - g_tsRS232LastHbMs) > TS_RS232_TIMEOUT_MS) {
        if (++g_tsRS232SilentTicks >= 2) {
            EmitRS232_Locked(0);
            g_tsRS232SilentTicks = 0;
        }
    } else {
        g_tsRS232SilentTicks = 0;
    }
}

/* Called from worker loop every ~1 s. Mirrors p2kflexDecoder AUDIO window logic. */
static void CheckAudioWindow_Locked(ULONGLONG now)
{
    if (!g_tsRS232Enabled || g_tsAudioWinMs == 0) return;
    if ((now - g_tsAudioWinMs) < TS_AUDIO_WINDOW_MS) return;

    BOOL audioPresent = (g_tsAudioTransitions >= (uint32_t)TS_AUDIO_SILENCE_THRESHOLD);

    if (g_tsAudioWarmup) {
        /* Discard first window — only initialize state, no emit (p2kflex behavior) */
        g_tsAudioState  = audioPresent ? 1 : 0;
        g_tsAudioSilWin = 0;
        g_tsAudioWarmup = FALSE;
    } else if (audioPresent) {
        g_tsAudioSilWin = 0;
        EmitAudio_Locked(1);
    } else {
        if (++g_tsAudioSilWin >= TS_AUDIO_SILENCE_WINDOWS) {
            EmitAudio_Locked(0);
        }
    }

    g_tsAudioTransitions = 0;
    g_tsAudioWinMs       = now;
}

/* ---------------------------------------------------------------------------
** RS232 / AUDIO public API
** ---------------------------------------------------------------------------*/

void TelnetServerRS232Enable(int active)
{
    if (!g_tsCsInit) return;
    EnterCriticalSection(&g_tsCs);

    if (active) {
        g_tsRS232Enabled     = 1;
        /* FIX [TsStartupGating]: behoud een hangende 'lost'(0) over een disable/enable-
        ** cyclus (bv. COM-poort reconfig) zodat de reconnect <RS232:1> als recovery
        ** stuurt. Een verse eerste start staat op -1 (global init) en blijft dus stil. */
        if (g_tsRS232State != 0) g_tsRS232State = -1;
        g_tsRS232Initialized = FALSE;
        g_tsRS232SilentTicks = 0;   /* FIX [RS232Flap] */
        g_tsRS232EnabledMs   = NowMs();
        g_tsRS232LastHbMs    = NowMs();
        g_tsAudioState       = -1;
        g_tsAudioSilWin      = 0;
        g_tsAudioWarmup      = TRUE;
        g_tsAudioTransitions = 0;
        g_tsAudioLastByte    = 0;
        g_tsAudioWinMs       = 0;
    } else {
        /* Explicit disconnect: emit <RS232:0> immediately if link was up */
        if (g_tsRS232Enabled && g_tsRS232State == 1) {
            EmitRS232_Locked(0);
        }
        g_tsRS232Enabled     = 0;
        /* FIX [TsStartupGating]: zojuist geëmit <RS232:0> (state=0) bewaren i.p.v. -1,
        ** zodat de volgende Enable(1)+data <RS232:1> (recovery) stuurt. Een nooit-
        ** opgekomen link blijft -1 → stil bij verse start. */
        if (g_tsRS232State != 0) g_tsRS232State = -1;
        g_tsRS232Initialized = FALSE;
        g_tsRS232SilentTicks = 0;   /* FIX [RS232Flap] */
        g_tsAudioState       = -1;
        g_tsAudioSilWin      = 0;
        g_tsAudioWarmup      = TRUE;
        g_tsAudioTransitions = 0;
        g_tsAudioLastByte    = 0;
        g_tsAudioWinMs       = 0;
    }

    LeaveCriticalSection(&g_tsCs);
}

/* FIX [RS232Flap]: refresh the watchdog clock on a successful worker reconnect.
** Called from rs232_worker_reopen() success-path. Resets the silence timer (and
** hysteresis counter) so the byte-gap that preceded the reconnect does not count
** toward TS_RS232_TIMEOUT_MS — mirrors the g_connectTickMs/g_lastDataTickMs reset
** rs232.cpp does for its own stall watchdog. Deliberately does NOT touch state:
** <RS232:1> stays gated on real byte arrival via TelnetServerRS232BytesReceived. */
void TelnetServerRS232Heartbeat(void)
{
    if (!Profile.telnetServerEnabled || !g_tsCsInit || !g_tsRS232Enabled) return;

    EnterCriticalSection(&g_tsCs);
    g_tsRS232LastHbMs    = NowMs();
    g_tsRS232SilentTicks = 0;
    LeaveCriticalSection(&g_tsCs);
}

/* Called from RxThread (rs232_read / slicer_read) for every batch of bytes.
** Thread-safe: acquires g_tsCs for the brief state update. */
void TelnetServerRS232BytesReceived(const BYTE *data, int len)
{
    if (!Profile.telnetServerEnabled || !g_tsCsInit || !g_tsRS232Enabled) return;
    if (!data || len <= 0) return;

    EnterCriticalSection(&g_tsCs);

    ULONGLONG now = NowMs();
    g_tsRS232LastHbMs    = now;
    g_tsRS232Initialized = TRUE;

    /* FIX [TsStartupGating]: spiegel p2kflexDecoder exact. Daar emit de EERSTE data
    ** bij startup GEEN <RS232:1> — RS232.cpp:333-335 zet enkel g_rs232StateInitialized
    ** = true (stille init). <RS232:1> komt uitsluitend uit de recovery-tak
    ** (if g_RS232_waslost, RS232.cpp:305-312). Wij scheiden daarom unset(-1) van
    ** lost(0): lost->present = echt herstel -> emit; unset->present = stille
    ** startup-init, geen emit. EmitRS232_Locked blijft idempotent. */
    if (g_tsRS232State == 0) {
        EmitRS232_Locked(1);        /* genuine recovery after loss */
    } else if (g_tsRS232State == -1) {
        g_tsRS232State = 1;         /* first data at startup: silent init (p2kflex parity) */
    }

    /* Accumulate bit transitions for AUDIO window (p2kflex RS232_ACTIVITY) */
    if (g_tsAudioWinMs == 0) g_tsAudioWinMs = now;
    for (int i = 0; i < len; i++) {
        BYTE diff = data[i] ^ g_tsAudioLastByte;
        g_tsAudioTransitions += (uint32_t)BitCount8(diff);
        g_tsAudioLastByte     = data[i];
    }

    LeaveCriticalSection(&g_tsCs);
}

/* Called from slicer_read (bOrgcomPortRS232=FALSE) for every batch of decoded
** line-data bytes.  The PDW slicer driver emits one decoded-bit byte (0x00 or
** 0x10) per input bit, so consecutive-byte XOR yields at most 1 set-bit vs up
** to 8 for raw 19200 baud RS232 framing bytes.  The p2kflexDecoder threshold
** (10 transitions/window) was calibrated for raw RS232 — it is never met by
** slicer line-data even during active FLEX decode.
** Fix: treat byte ARRIVAL as audio activity.  Any slicer data means the COM
** link is delivering decoded bits → audio present.  RS232 loss (no data for
** 2 s) still fires <RS232:0>, and that naturally precedes any <AUDIO:0>. */
void TelnetServerSlicerActivity(int nBytes)
{
    if (!Profile.telnetServerEnabled || !g_tsCsInit || !g_tsRS232Enabled) return;
    if (nBytes <= 0) return;

    EnterCriticalSection(&g_tsCs);

    ULONGLONG now = NowMs();
    g_tsRS232LastHbMs    = now;
    g_tsRS232Initialized = TRUE;

    /* FIX [TsStartupGating]: zelfde gating als BytesReceived — startup-init is stil,
    ** alleen lost(0)->present emit <RS232:1> (p2kflex recovery-semantiek). */
    if (g_tsRS232State == 0) {
        EmitRS232_Locked(1);        /* genuine recovery after loss */
    } else if (g_tsRS232State == -1) {
        g_tsRS232State = 1;         /* first data at startup: silent init (p2kflex parity) */
    }

    /* Guarantee the audio-present threshold is met for this window. */
    if (g_tsAudioWinMs == 0) g_tsAudioWinMs = now;
    if (g_tsAudioTransitions < (uint32_t)TS_AUDIO_SILENCE_THRESHOLD)
        g_tsAudioTransitions  = (uint32_t)TS_AUDIO_SILENCE_THRESHOLD;

    LeaveCriticalSection(&g_tsCs);
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
    g_tsTxStopPending = FALSE;   /* FIX [TelnetServer]: cancel pending debounce — false EOT */
    EmitTxStart_Locked();
    LeaveCriticalSection(&g_tsCs);
}

void TelnetServerNotifyTxStop(void)
{
    if (!Profile.telnetServerEnabled || !g_tsCsInit) return;
    EnterCriticalSection(&g_tsCs);
    /* FIX [TelnetServer]: debounced TX_STOP.
    ** PDW's FLEX decoder fires display_showmo(MODE_IDLE) on a false EOT pattern at every
    ** block boundary (~111ms before the next real sync), producing spurious TX_STOP/TX_START
    ** cycles that split a single burst into fragments. The p2kflexMonitor then cannot match
    ** INSTRs to ALPHAs across those fragments ("Missed Instructions!").
    ** Solution: set a 1500ms pending window; if TX_START arrives first the stop is cancelled
    ** and the burst continues uninterrupted. Real signal loss always lasts >2s. */
    if (g_tsTxState == TX_ACTIVE && !g_tsTxStopPending) {
        g_tsTxStopPending   = TRUE;
        g_tsTxStopRequestMs = NowMs();
    }
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

int TelnetServerGetClients(TsClientInfo *out, int maxCount)
{
    if (!out || maxCount <= 0 || !g_tsCsInit) return 0;
    int written = 0;

    EnterCriticalSection(&g_tsCs);
    for (int i = 0; i < TS_MAX_CLIENTS && written < maxCount; i++) {
        const TsClient *c = &g_tsClients[i];
        if (!c->used) continue;
        TsClientInfo *o = &out[written];
        o->used         = 1;
        o->disconnected = c->disconnected ? 1 : 0;
        strncpy_s(o->ip, sizeof(o->ip), inet_ntoa(c->addr.sin_addr), _TRUNCATE);
        o->port         = ntohs(c->addr.sin_port);
        strncpy_s(o->name, sizeof(o->name), c->clientName, _TRUNCATE);
        /* hasName means we received CLIENT:..., but role is only known if ROLE:... too.
        ** We track role as -1 unset because the C struct uses a 3-state convention. */
        if (!c->hasName && !c->disconnected) {
            o->role = -1;
        } else {
            /* TsClient has only isMaster (BOOL); we don't currently distinguish
            ** "role explicitly set to SLAVE" from "role never received". Treat
            ** isMaster=TRUE as MASTER, FALSE as SLAVE-or-default. */
            o->role = c->isMaster ? 1 : 0;
        }
        written++;
    }
    LeaveCriticalSection(&g_tsCs);
    return written;
}

int TelnetServerGetEvents(TsEvent *out, int maxCount)
{
    if (!out || maxCount <= 0 || !g_tsCsInit) return 0;
    int written = 0;

    EnterCriticalSection(&g_tsCs);
    /* Copy newest-first. Newest is at (head-1) mod N; walk backwards. */
    int idx = (g_tsEventsHead - 1 + TS_EVENT_RING_SIZE) % TS_EVENT_RING_SIZE;
    int avail = g_tsEventsCount;
    while (written < maxCount && avail > 0) {
        out[written] = g_tsEvents[idx];
        written++;
        idx = (idx - 1 + TS_EVENT_RING_SIZE) % TS_EVENT_RING_SIZE;
        avail--;
    }
    LeaveCriticalSection(&g_tsCs);
    return written;
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

/* Push the current RS232/AUDIO state to one client slot immediately after
** connect.  Mirrors p2kflexDecoder's on-connect status push so the monitor
** application never has to wait for the next state-change event.
** Caller holds g_tsCs. */
static void SendRS232AudioState_Locked(int idx)
{
    if (!g_tsRS232Enabled) return;

    /* Only send RS232 status once the state has been determined (not unset=-1). */
    if (g_tsRS232State >= 0) {
        const char *rs = g_tsRS232State ? "<RS232:1>\r" : "<RS232:0>\r";
        SendToClient(idx, rs, (int)strlen(rs));
    }

    /* Send AUDIO status once warmup is complete and state is known. */
    if (!g_tsAudioWarmup && g_tsAudioState >= 0) {
        const char *au = g_tsAudioState ? "<AUDIO:1>\r" : "<AUDIO:0>\r";
        SendToClient(idx, au, (int)strlen(au));
    }
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
        /* Logged via TsEvent (not TsLog) so it shows up in the Ctrl-N activity list. */
        char ev[128];
        _snprintf_s(ev, sizeof(ev), _TRUNCATE, "Reconnect %s:%d (slot %d, name='%s')",
                    inet_ntoa(peer.sin_addr), ntohs(peer.sin_port), idx, c->clientName);
        TsEventPush_Locked(ev);
        TsLog("%s", ev);
        SendIacGreeting(idx);
        SendRS232AudioState_Locked(idx);   /* push current RS232/AUDIO status immediately */
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

    char ev[128];
    _snprintf_s(ev, sizeof(ev), _TRUNCATE, "Accept %s:%d (slot %d)",
                inet_ntoa(peer.sin_addr), ntohs(peer.sin_port), idx);
    TsEventPush_Locked(ev);
    TsLog("%s", ev);
    SendIacGreeting(idx);
    SendRS232AudioState_Locked(idx);       /* push current RS232/AUDIO status immediately */

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
        char ev[128];
        _snprintf_s(ev, sizeof(ev), _TRUNCATE, "Slot %d identifies as '%s'", idx, c->clientName);
        TsEventPush_Locked(ev);
        TsLog("%s", ev);
    }
    else if (strncmp(line, "ROLE:", 5) == 0) {
        const char *role = line + 5;
        if      (strcmp(role, "MASTER") == 0) c->isMaster = TRUE;
        else if (strcmp(role, "SLAVE")  == 0) c->isMaster = FALSE;
        char ev[128];
        _snprintf_s(ev, sizeof(ev), _TRUNCATE, "Slot %d (%s) role='%s'",
                    idx, c->clientName[0] ? c->clientName : "?", role);
        TsEventPush_Locked(ev);
        TsLog("%s", ev);
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
        char ev[128];
        _snprintf_s(ev, sizeof(ev), _TRUNCATE, "Slot %d (%s) peer closed",
                    idx, c->clientName[0] ? c->clientName : "?");
        TsEventPush_Locked(ev);
        TsLog("%s", ev);
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
            char ev[128];
            _snprintf_s(ev, sizeof(ev), _TRUNCATE,
                        "GC slot %d (name='%s')", i, c->clientName);
            TsEventPush_Locked(ev);
            TsLog("%s", ev);
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
            c->replayCursor    = idx;
            c->replayLineCount = 0;
        }

        /* Emit up to 16 entries per tick */
        int budget = 16;
        while (budget-- > 0) {
            if (c->replayCursor == g_tsBacklogHead) {
                /* done */
                static const char ftr[] = "<BUFFER_STOP>\r";
                SendToClient(i, ftr, (int)sizeof(ftr) - 1);
                char ev[128];
                _snprintf_s(ev, sizeof(ev), _TRUNCATE,
                            "Replay to slot %d (%s) — %d lines",
                            i, c->clientName[0] ? c->clientName : "?", c->replayLineCount);
                TsEventPush_Locked(ev);
                TsLog("%s", ev);
                c->reconnectReplay = FALSE;
                c->replayCursor    = -1;
                break;
            }
            const TsBacklogEntry *e = &g_tsBacklog[c->replayCursor];
            if (e->len > 0) {
                SendToClient(i, e->line, e->len);
                c->replayLineCount++;
            }
            c->replayCursor = (c->replayCursor + 1) % TS_BACKLOG_LINES;
        }
    }
}

/* Worker loop: select() on listen + all client sockets with 1 s timeout.
** Each iteration we also check timers (auto TX_STOP, WD heartbeat, GC, replay). */
static DWORD WINAPI TelnetWorker(LPVOID /*arg*/)
{
    TsLogEvent("Worker started, listening on %s:%d", g_tsBind, g_tsPort);

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

        /* FIX [TelnetServer]: flush debounced TX_STOP after 1500ms window */
        if (g_tsTxStopPending && (now - g_tsTxStopRequestMs) >= TS_TXSTOP_DEBOUNCE_MS) {
            EmitTxStop_Locked();
            g_tsTxStopPending = FALSE;
        }

        /* Auto TX_STOP after ~3 s idle */
        if (g_tsTxState == TX_ACTIVE && (now - g_tsLastNotifyTickMs) > TS_AUTO_TXSTOP_MS) {
            EmitTxStop_Locked();
            g_tsTxStopPending = FALSE;
        }

        /* Watchdog <WD> heartbeat */
        if (g_tsClientCount > 0 && g_tsWdSec > 0
            && (now - g_tsLastSendTickMs) > (ULONGLONG)g_tsWdSec * 1000) {
            EmitMarker("<WD>");
        }

        GarbageCollectSlots();
        FlushReplay();

        /* RS232 / AUDIO watchdogs — identical timing to p2kflexDecoder */
        CheckRS232Watchdog_Locked(now);
        CheckAudioWindow_Locked(now);

        LeaveCriticalSection(&g_tsCs);
    }

    TsLogEvent("Worker stopping");
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

    /* Compute new config values from Profile */
    BOOL newEnabled = Profile.telnetServerEnabled ? TRUE : FALSE;
    char newBind[64];
    strncpy_s(newBind, sizeof(newBind), Profile.szTelnetServerBind, _TRUNCATE);
    if (newBind[0] == '\0') strcpy_s(newBind, sizeof(newBind), "0.0.0.0");
    int newPort       = Profile.telnetServerPort       > 0 ? Profile.telnetServerPort       : 8024;
    int newMaxClients = Profile.telnetServerMaxClients > 0 ? Profile.telnetServerMaxClients : TS_MAX_CLIENTS;
    if (newMaxClients > TS_MAX_CLIENTS) newMaxClients = TS_MAX_CLIENTS;
    int  newWdSec     = Profile.telnetServerWdSec      > 0 ? Profile.telnetServerWdSec      : 20;
    int  newBufTime   = Profile.telnetServerBufferTime > 0 ? Profile.telnetServerBufferTime : 60;
    BOOL newLogToFile = Profile.telnetServerLogToFile  ? TRUE : FALSE;

    /* If only runtime parameters changed (not port/bind/enabled), update in place
    ** without disconnecting clients. A full restart is only needed when the listen
    ** socket itself must change — i.e. enabled flipped, port changed, or bind IP
    ** changed. Every other setting (watchdog, buffer time, max clients, log) takes
    ** effect on the next worker tick without touching existing connections. */
    BOOL needRestart = !g_tsEnabled                          /* first time */
                    || (newEnabled != g_tsEnabled)
                    || (newPort    != g_tsPort)
                    || (strcmp(newBind, g_tsBind) != 0);

    if (!needRestart) {
        /* Hot-update runtime parameters only — clients stay connected. */
        EnterCriticalSection(&g_tsCs);
        g_tsMaxClients    = newMaxClients;
        g_tsWdSec         = newWdSec;
        g_tsBufferTimeSec = newBufTime;
        g_tsLogToFile     = newLogToFile;
        LeaveCriticalSection(&g_tsCs);
        TsLogEvent("Config updated (no restart needed — clients kept)");
        return;
    }

    /* Full restart required. */
    TelnetServerShutdown();
    g_tsEnabled = FALSE;

    if (!newEnabled) {
        PostStatus(TSS_DISABLED);
        return;
    }

    /* Snapshot new config */
    EnterCriticalSection(&g_tsCs);
    strncpy_s(g_tsBind, sizeof(g_tsBind), newBind, _TRUNCATE);
    g_tsPort          = newPort;
    g_tsMaxClients    = newMaxClients;
    g_tsWdSec         = newWdSec;
    g_tsBufferTimeSec = newBufTime;
    g_tsLogToFile     = newLogToFile;

    g_tsBacklogHead      = 0;
    g_tsBacklogCount     = 0;
    g_tsLastSendTickMs   = NowMs();
    g_tsLastNotifyTickMs = NowMs();
    g_tsTxState          = TX_IDLE;
    LeaveCriticalSection(&g_tsCs);

    if (!OpenListenSocket()) {
        PostStatus(TSS_ERROR);
        return;
    }

    g_tsRun    = TRUE;
    g_tsEnabled = TRUE;
    g_tsThread = CreateThread(NULL, 0, TelnetWorker, NULL, 0, NULL);
    if (!g_tsThread) {
        g_tsRun    = FALSE;
        g_tsEnabled = FALSE;
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
            // FIX [TelnetJoin]: join to FULL completion before CloseHandle and (later, in
            // TelnetServerDestroy) DeleteCriticalSection — same hardening as the mqtt/webhook/mysql
            // workers. A 5 s timeout could expire while the worker was still live, after which those
            // teardown steps ran under a running thread (crash on exit). The worker select()s on a
            // 1 s timeout, all sockets are non-blocking, and we just closed the listen socket to
            // unblock select(), so it returns within ~1 s — INFINITE is safe and correct.
            WaitForSingleObject(g_tsThread, INFINITE);
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
