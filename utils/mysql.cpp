/*
** utils/mysql.cpp -- MySQL feed for PDW (zero external DLLs)
**
** Wire protocol: MySQL 5.7 / MariaDB compatible (Protocol v10).
** Authentication: mysql_native_password via Windows CryptAPI (advapi32.dll) SHA1.
** No libmysql, no ODBC, no MariaDB connector.
**
** Configure the MySQL user with (mysql_native_password is REQUIRED — caching_sha2_password,
** the MySQL 8 default, is not supported and is reported with a clear error, not a silent loop):
**   ALTER USER 'user'@'host' IDENTIFIED WITH mysql_native_password BY 'pass';
**
** Auto-provisioning (FIX [MysqlProvision]): the client connects WITHOUT selecting a database, then
** USEs it — and CREATE DATABASE IF NOT EXISTS if that fails — followed by CREATE TABLE IF NOT
** EXISTS. A blank/new user with CREATE rights therefore gets a fully initialised database+table on
** first connect; a least-privilege user on an existing database just USEs it. No schema-version
** migration: changing the Optimized columns later requires a manual ALTER on existing tables.
**
** Threading model:
**   Decoder thread  -> MysqlNotify() -> ring buffer (CRITICAL_SECTION + event)
**   Worker thread   -> dequeues jobs, sends INSERT via MySQL wire protocol
**                      Reconnects with exponential backoff (1 s, 2 s, 4 s … 30 s).
*/

#ifndef STRICT
#define STRICT 1
#endif

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>          /* tcp_keepalive, SIO_KEEPALIVE_VALS */
#include <windows.h>
#include <wincrypt.h>         /* CryptAPI SHA1 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "..\headers\pdw.h"
#include "mysql.h"
#include "logmanager.h"
#include "feedstatus.h"   // FIX [FeedStatus]: Health-panel last-outcome store

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")

extern TCHAR szPath[];          /* PDW exe directory — from Initapp.cpp */

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define MYSQL_QUEUE_SIZE         64
#define MYSQL_RECV_TIMEOUT       10         /* seconds per recv() call */
// FIX [MysqlQueryLen]: was 24576 (24 kB) — far too small once szSubscribers grew to 32 kB. A large
// FLEX group call (~170 capcodes) escapes (quotes doubled, etc.) to ~2x its raw size, so the
// VALUES list alone can approach ~66 kB; the old buffer silently truncated the INSERT mid-literal,
// producing malformed SQL -> server syntax error -> 3 retries -> the group row was dropped. Size for
// the true worst case: escaped subscribers (~66 kB) + escaped message (MAX_STR_LEN*2) + columns +
// keywords, with headroom. The buffer is heap-allocated per INSERT, so 128 kB costs no stack.
#define MYSQL_MAX_QUERY          131072     /* 128 kB — worst-case INSERT incl. full subscribers JSON */
#define MYSQL_SUBSCRIBERS_LEN    32768      /* JSON array of group subscribers */
#define MAX_MYSQL_GROUPBITS      17         /* FLEX groupbits 0-15 + 1 spare */

/* MySQL capability flags used by this client */
#define MY_CAP_LONG_PASSWORD     0x00000001UL
#define MY_CAP_LONG_FLAG         0x00000004UL
#define MY_CAP_CONNECT_WITH_DB   0x00000008UL
#define MY_CAP_PROTOCOL_41       0x00000200UL
#define MY_CAP_TRANSACTIONS      0x00002000UL
#define MY_CAP_SECURE_CONNECTION 0x00008000UL
#define MY_CAP_PLUGIN_AUTH       0x00080000UL

// FIX [MysqlProvision]: deliberately NO MY_CAP_CONNECT_WITH_DB. We connect WITHOUT selecting a
// database so a not-yet-existing database does not fail authentication (error 1049). The target
// database is selected — and created if missing — right after auth in TryConnect(). This is what
// lets a blank/new user be auto-provisioned.
#define MY_CLIENT_CAPS (MY_CAP_LONG_PASSWORD | MY_CAP_LONG_FLAG       | \
                        MY_CAP_PROTOCOL_41   | \
                        MY_CAP_TRANSACTIONS  | MY_CAP_SECURE_CONNECTION | \
                        MY_CAP_PLUGIN_AUTH)

// ---------------------------------------------------------------------------
// Config snapshot (copied from Profile at MysqlInit time)
// ---------------------------------------------------------------------------

static char  g_szHost    [128] = "";
static int   g_iPort           = 3306;
static char  g_szUser    [64]  = "";
static char  g_szPass    [64]  = "";
static char  g_szDatabase[64]  = "";
static char  g_szTable   [64]  = "messages";
static int   g_iFields         = MYF_ALL;
static int   g_iSchema         = MYSQL_SCHEMA_OPTIMIZED;
static BOOL  g_bLinefeed       = FALSE;   // FIX [MysqlUtf8]: snapshot van Profile.Linefeed — 0xBB -> '\n' i.p.v. « »

// ---------------------------------------------------------------------------
// Job queue (ring buffer)
// ---------------------------------------------------------------------------

typedef struct {
    char szCapcode     [16];
    char szMessage     [MAX_STR_LEN + 4];
    char szLabel       [FILTER_LABEL_LEN + 4];
    char szTime        [16];
    char szDate        [16];
    char szMode        [32];
    char szType        [16];
    char szBitrate     [16];
    char szSubscribers [MYSQL_SUBSCRIBERS_LEN]; /* Optimized only — JSON array, empty for non-group */
    int  iMatchType;                            /* 0=none, 1=filtered, 2=monitor-only */
    char szLabelColor  [8];                     /* "#RRGGBB" or "" */
} MysqlJob;

/* Per-groupbit accumulator for FLEX group call subscriber info (main thread only). */
typedef struct {
    BOOL active;
    char szMessage [MAX_STR_LEN + 4];
    char szTime    [16];
    char szDate    [16];
    char szMode    [32];
    char szType    [16];
    char szBitrate [16];
    char szSubscr  [MYSQL_SUBSCRIBERS_LEN]; /* JSON array being built: "[{...},{...}" */
    int  sPos;                               /* write position in szSubscr */
    int  nSubscr;                            /* subscriber count */
    int  iMatchType;                         /* from first subscriber's filter match */
    char szLabelColor[8];                    /* "#RRGGBB" from first subscriber's label */
} MysqlGroupAcc;

static MysqlGroupAcc g_groupAcc[MAX_MYSQL_GROUPBITS];

static MysqlJob  g_queue[MYSQL_QUEUE_SIZE];
static int       g_qHead = 0;
static int       g_qTail = 0;
static unsigned  g_mysqlDropped = 0;   // FIX [MysqlQueueDrop]: rows lost to a full queue

/* FIX [MysqlRequeue]: cap consecutive server-side INSERT rejections of the head row before
   dropping it, so one poison row (bad data / constraint / repeated deadlock) cannot wedge the
   whole feed. Transport failures are NOT counted — those hold the row until the server returns. */
#define MYSQL_MAX_JOB_RETRIES    3

static BOOL inline QueueFull(void)  { return ((g_qTail + 1) % MYSQL_QUEUE_SIZE) == g_qHead; }
static BOOL inline QueueEmpty(void) { return g_qHead == g_qTail; }

// ---------------------------------------------------------------------------
// Thread / synchronisation
// ---------------------------------------------------------------------------

static HANDLE           g_hThread  = NULL;
static HANDLE           g_hEvent   = NULL;
static volatile BOOL    g_bRunning = FALSE;
static CRITICAL_SECTION g_cs;
static BOOL             g_bCsInit  = FALSE;

// ---------------------------------------------------------------------------
// Status window
// ---------------------------------------------------------------------------

static HWND             g_hStatusWnd = NULL;   /* protected by g_cs */

// ---------------------------------------------------------------------------
// Log file
// ---------------------------------------------------------------------------

static BOOL             g_bLogToFile = FALSE;

// ---------------------------------------------------------------------------
// Connection state (worker thread only — no locking needed)
// ---------------------------------------------------------------------------

static SOCKET g_sock     = INVALID_SOCKET;
// FIX [MysqlConnAbort]: in-progress TryConnect() socket, published so MysqlStop can interrupt a
// connect/handshake in flight. g_sock (and the existing [MysqlShutdownFd] snapshot+shutdown in
// MysqlStop) only cover the ESTABLISHED connection used by the main send/receive loop; while
// TryConnect is running the socket lives in a local variable and g_sock is still INVALID_SOCKET, so
// without this the GUI thread's INFINITE join in MysqlStop could sit for the full
// DNS/connect/handshake/auth/provisioning duration (15-65 s against a slow/hung server) with
// nothing to interrupt it.
static volatile SOCKET g_connSock = INVALID_SOCKET;
static BOOL   g_bWsaInit = FALSE;

// ===========================================================================
// Log file  (written via LogManager; date-stamped {YYMMDD}_mysql.log, daily rotation)
// ===========================================================================

static void WriteLog(const char *fmt, ...)
{
    if (!g_bLogToFile) return;

    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);

    PDW_MYSQLLOG("%s", line);
}

// ===========================================================================
// Status window
// ===========================================================================

static void PostStatus(int status)
{
    HWND hWnd;
    FeedStatus_Set(FEED_MYSQL, status);   // FIX [FeedStatus]: pollable last outcome for the Health panel
    EnterCriticalSection(&g_cs);
    hWnd = g_hStatusWnd;
    LeaveCriticalSection(&g_cs);
    if (hWnd) PostMessage(hWnd, WM_MYSQL_STATUS, (WPARAM)status, 0);
}

void MysqlSetStatusWnd(HWND hWnd)
{
    if (!g_bCsInit) { g_hStatusWnd = hWnd; return; }
    EnterCriticalSection(&g_cs);
    g_hStatusWnd = hWnd;
    LeaveCriticalSection(&g_cs);
}

/* Forward declarations — defined after the protocol helpers. */
static void BuildCreateTable(char *out, int outLen);
static int  BuildInsert(char *out, int outLen, const MysqlJob *job);

// ===========================================================================
// Low-level I/O helpers
// ===========================================================================

/* Receive exactly n bytes from s.  Returns bytes received (< n means error/timeout). */
static int RecvAll(SOCKET s, BYTE *buf, int n)
{
    fd_set rdSet;
    timeval tv;
    int got = 0;
    while (got < n) {
        FD_ZERO(&rdSet);
        FD_SET(s, &rdSet);
        tv.tv_sec  = MYSQL_RECV_TIMEOUT;
        tv.tv_usec = 0;
        int r = select((int)s + 1, &rdSet, NULL, NULL, &tv);
        if (r <= 0) return got;
        r = recv(s, (char *)(buf + got), n - got, 0);
        if (r <= 0) return got;
        got += r;
    }
    return got;
}

/* Send exactly n bytes to s.  Returns bytes sent (< n means error). */
static int SendAll(SOCKET s, const BYTE *buf, int n)
{
    int sent = 0;
    while (sent < n) {
        int r = send(s, (const char *)(buf + sent), n - sent, 0);
        if (r <= 0) return sent;
        sent += r;
    }
    return sent;
}

/* Read one MySQL packet (3-byte payload-length LE + 1 seq byte header, then payload). */
static BOOL ReadPacket(SOCKET s, BYTE *buf, int bufCap, int *outLen)
{
    BYTE hdr[4];
    if (RecvAll(s, hdr, 4) != 4) return FALSE;
    int len = (int)hdr[0] | ((int)hdr[1] << 8) | ((int)hdr[2] << 16);
    if (len < 0 || len >= bufCap) return FALSE;
    if (len > 0 && RecvAll(s, buf, len) != len) return FALSE;
    buf[len] = 0;
    *outLen = len;
    return TRUE;
}

/* Write one MySQL packet with the given sequence number. */
static BOOL WritePacket(SOCKET s, const BYTE *payload, int len, int seq)
{
    BYTE hdr[4];
    hdr[0] = (BYTE)( len        & 0xFF);
    hdr[1] = (BYTE)((len >>  8) & 0xFF);
    hdr[2] = (BYTE)((len >> 16) & 0xFF);
    hdr[3] = (BYTE)(seq & 0xFF);
    if (SendAll(s, hdr, 4) != 4) return FALSE;
    if (len > 0 && SendAll(s, payload, len) != len) return FALSE;
    return TRUE;
}

// ===========================================================================
// Cryptography: SHA1 via CryptAPI (advapi32)
// ===========================================================================

static BOOL Sha1(const BYTE *data, int dataLen, BYTE out[20])
{
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    BOOL ok = FALSE;

    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        return FALSE;
    if (!CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) goto done;
    if (!CryptHashData(hHash, data, (DWORD)dataLen, 0)) goto done;
    {
        DWORD hashLen = 20;
        ok = CryptGetHashParam(hHash, HP_HASHVAL, out, &hashLen, 0);
    }
done:
    if (hHash) CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    return ok;
}

/* mysql_native_password: token = SHA1(pass) XOR SHA1(scramble || SHA1(SHA1(pass))) */
static BOOL NativePasswordToken(const char *pass, const BYTE scramble[20], BYTE out[20])
{
    BYTE step1[20], step2[20], step3[20];
    BYTE combined[40];

    if (!Sha1((const BYTE *)pass, (int)strlen(pass), step1)) return FALSE;
    if (!Sha1(step1, 20, step2))                             return FALSE;
    memcpy(combined,      scramble, 20);
    memcpy(combined + 20, step2,    20);
    if (!Sha1(combined, 40, step3)) return FALSE;

    for (int i = 0; i < 20; i++) out[i] = step1[i] ^ step3[i];
    return TRUE;
}

// ===========================================================================
// String helpers
// ===========================================================================

/* FIX [MysqlUtf8]: lengte (1..4) van de geldige UTF-8 reeks die bij p begint, of 0 als p geen
   structureel geldige reeks start. We controleren alleen de structuur (kopbyte + de juiste
   continuation-bytes) — precies wat een utf8mb4-kolom nodig heeft om de bytes te accepteren. */
static int Utf8SeqLen(const unsigned char *p)
{
    unsigned char c = p[0];
    if (c < 0x80) return 1;                   /* ASCII */
    int n;
    if      ((c & 0xE0) == 0xC0) n = 1;       /* 110xxxxx + 1 staart */
    else if ((c & 0xF0) == 0xE0) n = 2;       /* 1110xxxx + 2 staarten */
    else if ((c & 0xF8) == 0xF0) n = 3;       /* 11110xxx + 3 staarten */
    else return 0;                            /* losse staart (0x80-0xBF) of ongeldige kop */
    for (int k = 1; k <= n; k++)
        if ((p[k] & 0xC0) != 0x80) return 0;  /* NUL of niet-staart breekt de reeks af */
    return n + 1;
}

/* FIX [MysqlUtf8]: maak een willekeurige bytestring veilig voor een utf8mb4-kolom. PDW-tekst is een
   mix van ASCII, de 0xBB-linefeedmarker en (bij MOBITEX/encrypted verkeer) ruwe hoge bytes die geen
   geldig UTF-8 vormen. MySQL weigert elke niet-UTF-8 byte met error 1366 en sloopt daarmee de hele
   rij. We herschrijven hier, één keer bij het enqueuen, naar gegarandeerd geldig UTF-8 zodat de
   worker-retry alleen schone data ziet.
     bLinefeed == TRUE  : 0xBB -> '\n'        (gelijk aan de »-als-linefeed weergave)
     bLinefeed == FALSE : 0xBB -> 0xC2 0xBB   (echte UTF-8 « » , net zoals het scherm dan toont)
     byte die geen deel is van geldige UTF-8  -> '?'   (encrypted garbage / losse hoge byte)
     geldige (multibyte) UTF-8                -> ongewijzigd doorgelaten */
static void Utf8SanitizeForMysql(char *dst, int dstLen, const char *src, BOOL bLinefeed)
{
    int j = 0;
    const unsigned char *p = (const unsigned char *)src;
    while (*p) {
        if (*p == 0xBB) {                         /* PDW-linefeedmarker — vóór de UTF-8 regels */
            if (bLinefeed) { if (j > dstLen - 2) break; dst[j++] = '\n'; }
            else           { if (j > dstLen - 3) break; dst[j++] = (char)0xC2; dst[j++] = (char)0xBB; }
            p++;
            continue;
        }
        int n = Utf8SeqLen(p);
        if (n == 0) {                             /* ongeldige / losse byte: garbage */
            if (j > dstLen - 2) break;
            dst[j++] = '?';
            p++;
        } else {                                  /* geldige UTF-8: 1-op-1 doorlaten */
            if (j + n > dstLen - 1) break;
            for (int k = 0; k < n; k++) dst[j++] = (char)p[k];
            p += n;
        }
    }
    dst[j] = '\0';
}

/* Escape a string for use inside a JSON double-quoted value (RFC 7159). */
static void JsonEscapeStr(char *dst, int dstLen, const char *src)
{
    int j = 0;
    const unsigned char *p = (const unsigned char *)src;
    while (*p && j < dstLen - 2) {
        unsigned char c = *p;
        if      (c == '"')  { dst[j++] = '\\'; if (j < dstLen-1) dst[j++] = '"';  p++; }
        else if (c == '\\') { dst[j++] = '\\'; if (j < dstLen-1) dst[j++] = '\\'; p++; }
        else if (c == '\n') { dst[j++] = '\\'; if (j < dstLen-1) dst[j++] = 'n';  p++; }
        else if (c == '\r') { dst[j++] = '\\'; if (j < dstLen-1) dst[j++] = 'r';  p++; }
        else if (c == '\t') { dst[j++] = '\\'; if (j < dstLen-1) dst[j++] = 't';  p++; }
        else if (c < 32)    { p++; /* skip non-printable control chars */ }
        else if (c < 0x80)  { dst[j++] = (char)c; p++; }
        else {
            /* FIX [MysqlUtf8]: alleen geldige UTF-8 in de subscribers-JSON; losse hoge bytes
               (0xBB, encrypted garbage) -> '?' zodat de utf8mb4-kolom de rij niet weigert. */
            int n = Utf8SeqLen(p);
            if (n > 0 && j + n <= dstLen - 2) { for (int k = 0; k < n; k++) dst[j++] = (char)p[k]; p += n; }
            else                              { dst[j++] = '?'; p++; }
        }
    }
    dst[j] = '\0';
}

static void TrimCopy(char *dst, int dstLen, const char *src)
{
    while (*src == ' ') src++;
    const char *end = src + strlen(src);
    while (end > src && *(end - 1) == ' ') end--;
    int len = (int)(end - src);
    if (len > dstLen - 1) len = dstLen - 1;
    if (len > 0) memcpy(dst, src, len);
    dst[len] = '\0';
}

static int MySqlEscape(char *dst, int dstLen, const char *src)
{
    int j = 0;
    for (int i = 0; src[i] && j < dstLen - 2; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
        case '\0': dst[j++] = '\\'; if (j < dstLen-1) dst[j++] = '0';  break;
        // FIX [MysqlAnsiEscape]: escape the single quote by DOUBLING it ('' ) instead of \'.
        // Doubling is valid in every sql_mode — including NO_BACKSLASH_ESCAPES / ANSI — so OTA
        // message/label text can never break out of the quoted literal, whatever the user's
        // MySQL/MariaDB server is set to. In the common default mode the stored value is identical
        // to the old \' form, so this is transparent for existing installs. The double quote needs
        // no escaping inside a single-quoted literal, so it passes through (also ANSI-clean).
        case '\'': dst[j++] = '\''; if (j < dstLen-1) dst[j++] = '\''; break;
        case '\\': dst[j++] = '\\'; if (j < dstLen-1) dst[j++] = '\\'; break;
        case '\n': dst[j++] = '\\'; if (j < dstLen-1) dst[j++] = 'n';  break;
        case '\r': dst[j++] = '\\'; if (j < dstLen-1) dst[j++] = 'r';  break;
        case '\t': dst[j++] = '\\'; if (j < dstLen-1) dst[j++] = 't';  break;
        case 26:   dst[j++] = '\\'; if (j < dstLen-1) dst[j++] = 'Z';  break;
        default:   dst[j++] = (char)c; break;
        }
    }
    dst[j] = '\0';
    return j;
}

/* Convert PDW date "dd-MM-yy" + time "HH:mm:ss" → MySQL DATETIME "YYYY-MM-DD HH:MM:SS". */
static void ConvertDateTime(const char *date, const char *time, char *out, int outLen)
{
    int day = 0, month = 0, year = 0;
    if (sscanf(date, "%d-%d-%d", &day, &month, &year) == 3) {
        if (year >= 0 && year < 100) year += 2000;
        _snprintf(out, outLen - 1, "%04d-%02d-%02d %s",
                  year, month, day, time ? time : "00:00:00");
    } else {
        _snprintf(out, outLen - 1, "0000-00-00 %s", time ? time : "00:00:00");
    }
    out[outLen - 1] = '\0';
}

// ===========================================================================
// MySQL wire protocol
// ===========================================================================

static BOOL ParseHandshake(const BYTE *pkt, int pktLen, BYTE scramble[20])
{
    if (pktLen < 46) return FALSE;
    if (pkt[0] != 10) return FALSE;

    int pos = 1;
    while (pos < pktLen && pkt[pos] != '\0') pos++;
    if (pos >= pktLen) return FALSE;
    pos++;

    if (pos + 4 > pktLen) return FALSE;
    pos += 4;  /* connection_id */

    if (pos + 8 > pktLen) return FALSE;
    memcpy(scramble, pkt + pos, 8);
    pos += 8;

    if (pos >= pktLen) return FALSE;
    pos++;  /* filler */
    if (pos + 2 > pktLen) return FALSE;
    pos += 2;  /* caps lower */
    if (pos >= pktLen) return FALSE;
    pos++;  /* charset */
    if (pos + 2 > pktLen) return FALSE;
    pos += 2;  /* status */
    if (pos + 2 > pktLen) return FALSE;
    pos += 2;  /* caps upper */

    if (pos >= pktLen) return FALSE;
    int pluginDataLen = (int)pkt[pos++];

    pos += 10;
    if (pos > pktLen) return FALSE;

    int part2Len = (pluginDataLen > 8) ? (pluginDataLen - 8) : 13;
    if (part2Len < 1) part2Len = 13;
    if (pos + part2Len > pktLen) part2Len = pktLen - pos;
    if (part2Len < 1) return FALSE;

    int copy2 = (part2Len >= 13) ? 12 : (part2Len - 1);
    if (copy2 < 0)  copy2 = 0;
    if (copy2 > 12) copy2 = 12;
    if (copy2 > 0) memcpy(scramble + 8, pkt + pos, copy2);
    if (copy2 < 12) memset(scramble + 8 + copy2, 0, 12 - copy2);

    return TRUE;
}

static BOOL SendHandshakeResponse(SOCKET s,
                                   const char *user, const char *pass,
                                   const char *db,   const BYTE scramble[20])
{
    BYTE  buf[640];
    int   pos = 0;
    DWORD caps = (DWORD)MY_CLIENT_CAPS;

    buf[pos++] = (BYTE)( caps        & 0xFF);
    buf[pos++] = (BYTE)((caps >>  8) & 0xFF);
    buf[pos++] = (BYTE)((caps >> 16) & 0xFF);
    buf[pos++] = (BYTE)((caps >> 24) & 0xFF);

    buf[pos++] = 0xFF; buf[pos++] = 0xFF; buf[pos++] = 0xFF; buf[pos++] = 0x00;

    buf[pos++] = 33;  /* charset: utf8_general_ci */

    memset(buf + pos, 0, 23); pos += 23;

    {
        int uLen = (int)strlen(user);
        if (uLen > 63) uLen = 63;
        memcpy(buf + pos, user, uLen); pos += uLen;
        buf[pos++] = '\0';
    }

    if (pass && pass[0]) {
        BYTE token[20];
        if (!NativePasswordToken(pass, scramble, token)) return FALSE;
        buf[pos++] = 20;
        memcpy(buf + pos, token, 20); pos += 20;
    } else {
        buf[pos++] = 0;
    }

    /* FIX [MysqlProvision]: only emit the database field when CONNECT_WITH_DB is advertised.
       It is not (see MY_CLIENT_CAPS), so this block is skipped — the server then expects the
       auth-plugin name next, which we write below. Writing an unrequested db field here would
       desynchronise the server's parse of the plugin name. */
    if (MY_CLIENT_CAPS & MY_CAP_CONNECT_WITH_DB) {
        int dbLen = (int)strlen(db);
        if (dbLen > 63) dbLen = 63;
        memcpy(buf + pos, db, dbLen); pos += dbLen;
        buf[pos++] = '\0';
    } else {
        (void)db;
    }

    {
        static const char plugin[] = "mysql_native_password";
        memcpy(buf + pos, plugin, sizeof(plugin));
        pos += (int)sizeof(plugin);
    }

    return WritePacket(s, buf, pos, 1);
}

static BOOL SendQuery(SOCKET s, const char *sql, int sqlLen)
{
    int   pktLen  = 1 + sqlLen;
    BYTE *payload = (BYTE *)malloc(pktLen);
    if (!payload) return FALSE;
    payload[0] = 0x03;
    memcpy(payload + 1, sql, sqlLen);
    BOOL ok = WritePacket(s, payload, pktLen, 0);
    free(payload);
    return ok;
}

/* Read query result.  Returns TRUE on OK/EOF.
   errOut receives a human-readable message on FALSE (may be NULL). */
static BOOL ReadQueryResult(SOCKET s, char *errOut, int errOutLen)
{
    // FIX [MysqlErrPktWedge]: was [512]. ReadPacket rejects (without draining) any packet >= bufCap,
    // and an ERR packet can carry a ~512-byte message (MYSQL_ERRMSG_SIZE) + 9-byte header = ~521
    // bytes. That rejection was reported as "recv timeout/error" (a TRANSPORT failure), which the
    // worker retries by reconnecting forever instead of counting it as a server error and dropping
    // the row after MYSQL_MAX_JOB_RETRIES -> the feed wedged permanently behind one bad row.
    BYTE pkt[1024];
    int  pktLen = 0;

    if (!ReadPacket(s, pkt, sizeof(pkt), &pktLen)) {
        if (errOut) _snprintf(errOut, errOutLen - 1, "recv timeout/error");
        return FALSE;
    }
    if (pktLen < 1) {
        if (errOut) _snprintf(errOut, errOutLen - 1, "empty response");
        return FALSE;
    }

    if (pkt[0] == 0x00 || pkt[0] == 0xFE) return TRUE;

    if (pkt[0] == 0xFF) {
        if (errOut) {
            int eCode = (pktLen >= 3)
                        ? (int)((WORD)pkt[1] | ((WORD)pkt[2] << 8)) : 0;
            const char *msg = (pktLen >= 9 && pkt[3] == '#')
                              ? (const char *)pkt + 9
                              : (const char *)pkt + 3;
            _snprintf(errOut, errOutLen - 1, "MySQL error %d: %.200s", eCode, msg);
            errOut[errOutLen - 1] = '\0';
        }
        return FALSE;
    }

    if (errOut) _snprintf(errOut, errOutLen - 1, "unexpected 0x%02X", pkt[0]);
    return FALSE;
}

// ===========================================================================
// Connect + authenticate
// ===========================================================================

static BOOL EnsureWsa(void)
{
    if (g_bWsaInit) return TRUE;
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return FALSE;
    g_bWsaInit = TRUE;
    return TRUE;
}

static SOCKET TryConnect(void)
{
    if (!EnsureWsa()) return INVALID_SOCKET;

    ADDRINFOA hints, *res = NULL;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char szPort[8];
    _snprintf(szPort, sizeof(szPort) - 1, "%d", g_iPort);
    szPort[sizeof(szPort) - 1] = '\0';

    if (getaddrinfo(g_szHost, szPort, &hints, &res) != 0 || !res) {
        WriteLog("CONNECT FAIL  host=%s port=%d (resolve error)", g_szHost, g_iPort);
        return INVALID_SOCKET;
    }

    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return INVALID_SOCKET; }

    // FIX [MysqlConnAbort]: publish the socket immediately so MysqlStop's shutdown() kick can reach
    // it while we are still connecting/authenticating below.
    g_connSock = s;

    /* FIX [MysqlConnTimeout]/[ShutdownRace]: non-blocking connect with a 5 s ceiling. A blocking
       connect() to an unreachable host parks the worker for the OS default (~20 s), which slowed
       backoff and — worse — made a clean shutdown/reconfigure join wait that long. Bounded connect
       keeps the worker responsive; the handshake/query phase below runs in blocking mode (RecvAll
       enforces its own select() timeout). */
    {
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        int crc = connect(s, res->ai_addr, (int)res->ai_addrlen);
        if (crc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wrSet, exSet;
            FD_ZERO(&wrSet); FD_SET(s, &wrSet);
            FD_ZERO(&exSet); FD_SET(s, &exSet);
            timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
            int sel = select((int)s + 1, NULL, &wrSet, &exSet, &tv);
            if (sel <= 0 || FD_ISSET(s, &exSet)) {
                WriteLog("CONNECT FAIL  host=%s port=%d (connect timeout/refused)", g_szHost, g_iPort);
                closesocket(s); g_connSock = INVALID_SOCKET; freeaddrinfo(res); return INVALID_SOCKET;  // FIX [MysqlConnAbort]
            }
            int soErr = 0, soLen = (int)sizeof(soErr);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soErr, &soLen);
            if (soErr != 0) {
                WriteLog("CONNECT FAIL  host=%s port=%d (connect error %d)", g_szHost, g_iPort, soErr);
                closesocket(s); g_connSock = INVALID_SOCKET; freeaddrinfo(res); return INVALID_SOCKET;  // FIX [MysqlConnAbort]
            }
        } else if (crc == SOCKET_ERROR) {
            WriteLog("CONNECT FAIL  host=%s port=%d (connect error %d)", g_szHost, g_iPort, WSAGetLastError());
            closesocket(s); g_connSock = INVALID_SOCKET; freeaddrinfo(res); return INVALID_SOCKET;  // FIX [MysqlConnAbort]
        }
        nb = 0;
        ioctlsocket(s, FIONBIO, &nb);   /* back to blocking for handshake/query */
    }
    freeaddrinfo(res);

    // FIX [MysqlConnAbort]: phase-boundary check #1 (after the connect select succeeds) -- abort
    // early if a shutdown was requested while we were still connecting, instead of continuing
    // through keepalive setup, handshake, auth and provisioning against a server we're about to
    // disconnect from anyway.
    if (!g_bRunning) { closesocket(s); g_connSock = INVALID_SOCKET; return INVALID_SOCKET; }

    struct tcp_keepalive ka;
    ka.onoff             = 1;
    ka.keepalivetime     = 60000;
    ka.keepaliveinterval = 10000;
    DWORD dummy;
    WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), NULL, 0, &dummy, NULL, NULL);

    /* 1. Read server handshake */
    BYTE pkt[2048];
    int  pktLen = 0;
    if (!ReadPacket(s, pkt, sizeof(pkt), &pktLen)) {
        WriteLog("CONNECT FAIL  host=%s port=%d (no handshake)", g_szHost, g_iPort);
        closesocket(s); g_connSock = INVALID_SOCKET; return INVALID_SOCKET;  // FIX [MysqlConnAbort]
    }

    BYTE scramble[20];
    memset(scramble, 0, sizeof(scramble));
    if (!ParseHandshake(pkt, pktLen, scramble)) {
        WriteLog("CONNECT FAIL  host=%s port=%d (bad handshake)", g_szHost, g_iPort);
        closesocket(s); g_connSock = INVALID_SOCKET; return INVALID_SOCKET;  // FIX [MysqlConnAbort]
    }

    // FIX [MysqlConnAbort]: phase-boundary check #2 (after the server handshake packet is read).
    if (!g_bRunning) { closesocket(s); g_connSock = INVALID_SOCKET; return INVALID_SOCKET; }

    /* 2. Send HandshakeResponse */
    if (!SendHandshakeResponse(s, g_szUser, g_szPass, g_szDatabase, scramble)) {
        closesocket(s); g_connSock = INVALID_SOCKET; return INVALID_SOCKET;  // FIX [MysqlConnAbort]
    }

    /* 3. Read auth result */
    if (!ReadPacket(s, pkt, sizeof(pkt), &pktLen) || pktLen < 1) {
        closesocket(s); g_connSock = INVALID_SOCKET; return INVALID_SOCKET;  // FIX [MysqlConnAbort]
    }
    if (pkt[0] == 0xFF) {
        int eCode = (pktLen >= 3) ? (int)((WORD)pkt[1] | ((WORD)pkt[2] << 8)) : 0;
        WriteLog("AUTH FAIL  host=%s user=%s db=%s (error %d)", g_szHost, g_szUser, g_szDatabase, eCode);
        closesocket(s); g_connSock = INVALID_SOCKET; return INVALID_SOCKET;  // FIX [MysqlConnAbort]
    }
    /* FIX [MysqlAuthSwitch]: 0xFE here is an AuthSwitchRequest — the server wants a different auth
       plugin (e.g. caching_sha2_password, the MySQL 8 default). This client only implements
       mysql_native_password, so we cannot complete the switch. The old code mis-read 0xFE as
       success, then desynced into an endless silent reconnect loop. Fail with a clear, actionable
       message naming the requested plugin and the ALTER USER fix. */
    if (pkt[0] == 0xFE) {
        char plugin[64] = "";
        if (pktLen > 1) {
            int n = pktLen - 1; if (n > 63) n = 63;
            memcpy(plugin, pkt + 1, n); plugin[n] = '\0';   /* plugin name is NUL-terminated */
        }
        WriteLog("AUTH FAIL  host=%s user=%s — server requested auth plugin '%s'; this build only "
                 "supports mysql_native_password. Fix: ALTER USER '%s'@'<host>' IDENTIFIED WITH "
                 "mysql_native_password BY '<password>';",
                 g_szHost, g_szUser, plugin[0] ? plugin : "(unknown)", g_szUser);
        closesocket(s); g_connSock = INVALID_SOCKET; return INVALID_SOCKET;  // FIX [MysqlConnAbort]
    }
    if (pkt[0] != 0x00) {
        closesocket(s); g_connSock = INVALID_SOCKET; return INVALID_SOCKET;  // FIX [MysqlConnAbort]
    }

    // FIX [MysqlConnAbort]: phase-boundary check #3 (after the auth response is read).
    if (!g_bRunning) { closesocket(s); g_connSock = INVALID_SOCKET; return INVALID_SOCKET; }

    /* 4. SET NAMES utf8mb4 */
    {
        static const char setNames[] = "SET NAMES utf8mb4";
        char errMsg[256] = "";
        if (!SendQuery(s, setNames, (int)(sizeof(setNames) - 1)) ||
            !ReadQueryResult(s, errMsg, sizeof(errMsg))) {
            /* Fallback: try utf8 (older servers may not have utf8mb4) */
            static const char setNames3[] = "SET NAMES utf8";
            if (!SendQuery(s, setNames3, (int)(sizeof(setNames3) - 1)) ||
                !ReadQueryResult(s, NULL, 0)) {
                WriteLog("WARN    SET NAMES failed: %s", errMsg);
            }
        }
    }

    // FIX [MysqlConnAbort]: phase-boundary check #4 (between the provisioning queries: SET NAMES
    // above, USE/CREATE DATABASE below).
    if (!g_bRunning) { closesocket(s); g_connSock = INVALID_SOCKET; return INVALID_SOCKET; }

    /* 4b. Select the target database, creating it if missing (we connected without CONNECT_WITH_DB).
       FIX [MysqlProvision]: try USE first — a least-privilege user (INSERT only on an existing DB)
       must not be forced through CREATE DATABASE. Only if USE fails do we attempt to create it,
       which is what auto-provisions a blank/new setup. Both failing is fatal for this connect. */
    {
        char useSql[128];
        _snprintf(useSql, sizeof(useSql) - 1, "USE `%s`", g_szDatabase);
        useSql[sizeof(useSql) - 1] = '\0';

        char errMsg[256] = "";
        if (!SendQuery(s, useSql, (int)strlen(useSql)) ||
            !ReadQueryResult(s, errMsg, sizeof(errMsg))) {
            /* Database likely does not exist yet — create then select it. */
            char createDb[192];
            _snprintf(createDb, sizeof(createDb) - 1,
                      "CREATE DATABASE IF NOT EXISTS `%s` DEFAULT CHARACTER SET utf8mb4", g_szDatabase);
            createDb[sizeof(createDb) - 1] = '\0';

            char errMsg2[256] = "";
            if (!SendQuery(s, createDb, (int)strlen(createDb)) ||
                !ReadQueryResult(s, errMsg2, sizeof(errMsg2))) {
                WriteLog("PROVISION FAIL  CREATE DATABASE `%s`: %s", g_szDatabase, errMsg2);
                closesocket(s); g_connSock = INVALID_SOCKET; return INVALID_SOCKET;  // FIX [MysqlConnAbort]
            }
            WriteLog("PROVISION  database `%s` created", g_szDatabase);

            if (!SendQuery(s, useSql, (int)strlen(useSql)) ||
                !ReadQueryResult(s, errMsg2, sizeof(errMsg2))) {
                WriteLog("PROVISION FAIL  USE `%s` after create: %s", g_szDatabase, errMsg2);
                closesocket(s); g_connSock = INVALID_SOCKET; return INVALID_SOCKET;  // FIX [MysqlConnAbort]
            }
        }
    }

    /* 5. CREATE TABLE IF NOT EXISTS — creates the schema on first connect.
       Schema variant (Classic/Extended/Optimized) determined by g_iSchema.
       If the server doesn't support the schema (e.g. FULLTEXT on old MySQL 5.5),
       logs a warning and continues — user can create the table manually. */
    {
        char createSql[2048];
        BuildCreateTable(createSql, sizeof(createSql));
        char errMsg[256] = "";
        if (!SendQuery(s, createSql, (int)strlen(createSql)) ||
            !ReadQueryResult(s, errMsg, sizeof(errMsg))) {
            WriteLog("WARN    CREATE TABLE `%s` failed: %s", g_szTable, errMsg);
        } else {
            static const char *schemaNames[] = { "Classic", "Extended", "Optimized" };
            int si = (g_iSchema >= 0 && g_iSchema <= 2) ? g_iSchema : 2;
            WriteLog("SCHEMA  table `%s` ready (%s)", g_szTable, schemaNames[si]);
        }
    }

    WriteLog("CONNECT OK   host=%s port=%d db=%s", g_szHost, g_iPort, g_szDatabase);
    // FIX [MysqlConnAbort]: connect sequence done -- clear the in-progress marker. The caller
    // (MysqlWorker) assigns g_sock = TryConnect()'s return value right after this returns; the brief
    // window between here and that assignment is harmless because nothing blocks on socket I/O in
    // it, so there is nothing left for MysqlStop to usefully interrupt during that window anyway.
    g_connSock = INVALID_SOCKET;
    return s;
}

// ===========================================================================
// Schema-specific CREATE TABLE strings (table name substituted at runtime)
// ===========================================================================

static void BuildCreateTable(char *out, int outLen)
{
    switch (g_iSchema) {
    case MYSQL_SCHEMA_CLASSIC:
        _snprintf(out, outLen - 1,
            "CREATE TABLE IF NOT EXISTS `%s` ("
            "`id` INT(11) NOT NULL AUTO_INCREMENT,"
            "`timestamp` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
            "`capcode` VARCHAR(10) NOT NULL DEFAULT '',"
            "`melding` TEXT NOT NULL,"
            "`label` TEXT NOT NULL,"
            "PRIMARY KEY (`id`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
            g_szTable);
        break;
    case MYSQL_SCHEMA_EXTENDED:
        _snprintf(out, outLen - 1,
            "CREATE TABLE IF NOT EXISTS `%s` ("
            "`id` INT(11) NOT NULL AUTO_INCREMENT,"
            "`timestamp` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "`address` VARCHAR(20) NOT NULL DEFAULT '',"
            "`msg_time` VARCHAR(10) NOT NULL DEFAULT '',"
            "`msg_date` VARCHAR(12) NOT NULL DEFAULT '',"
            "`mode` VARCHAR(15) NOT NULL DEFAULT '',"
            "`msg_type` VARCHAR(20) NOT NULL DEFAULT '',"
            "`bitrate` VARCHAR(10) NOT NULL DEFAULT '',"
            "`message` TEXT NOT NULL,"
            "`label` TEXT NOT NULL,"
            "PRIMARY KEY (`id`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
            g_szTable);
        break;
    default: /* MYSQL_SCHEMA_OPTIMIZED */
        _snprintf(out, outLen - 1,
            "CREATE TABLE IF NOT EXISTS `%s` ("
            "`id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
            "`received` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "`address` CHAR(9) NOT NULL DEFAULT '',"
            "`mode` VARCHAR(15) NOT NULL DEFAULT '',"
            "`msg_type` VARCHAR(10) NOT NULL DEFAULT '',"
            "`bitrate` SMALLINT UNSIGNED NOT NULL DEFAULT 0,"
            "`message` TEXT NOT NULL,"
            "`label` VARCHAR(256) NOT NULL DEFAULT '',"
            "`subscribers` TEXT NOT NULL DEFAULT '',"
            "`match_type` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`label_color` VARCHAR(7) NOT NULL DEFAULT '',"
            "PRIMARY KEY (`id`),"
            "INDEX `idx_address`   (`address`),"
            "INDEX `idx_received` (`received`),"
            "INDEX `idx_match`     (`match_type`),"
            "INDEX `idx_label`     (`label`(64)),"
            "FULLTEXT `ft_message` (`message`,`label`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 ROW_FORMAT=DYNAMIC",
            g_szTable);
        break;
    }
    out[outLen - 1] = '\0';
}

// ===========================================================================
// INSERT query builders (one per schema)
// ===========================================================================

/* Classic: capcode (VARCHAR), melding (TEXT), label (TEXT).
   timestamp auto-fills via DEFAULT CURRENT_TIMESTAMP ON UPDATE. */
static int BuildInsertClassic(char *out, int outLen, const MysqlJob *job)
{
    int escMsgSz = MAX_STR_LEN * 2 + 8;
    int escLblSz = FILTER_LABEL_LEN * 2 + 8;
    char *escMsg = (char *)malloc(escMsgSz);
    char *escLbl = (char *)malloc(escLblSz);
    char  escCap[32];
    if (!escMsg || !escLbl) { free(escMsg); free(escLbl); return 0; }

    MySqlEscape(escCap, sizeof(escCap), job->szCapcode);
    MySqlEscape(escMsg, escMsgSz,       job->szMessage);
    MySqlEscape(escLbl, escLblSz,       job->szLabel);

    int n = _snprintf(out, outLen - 1,
        "INSERT INTO `%s` (`capcode`,`melding`,`label`) VALUES ('%s','%s','%s')",
        g_szTable, escCap, escMsg, escLbl);

    free(escMsg); free(escLbl);
    if (n < 0) n = 0;
    out[outLen - 1] = '\0';
    return n;
}

/* Extended: all 8 PDW fields as raw strings (no type conversion).
   timestamp auto-fills via DEFAULT CURRENT_TIMESTAMP. */
static int BuildInsertExtended(char *out, int outLen, const MysqlJob *job)
{
    int escMsgSz = MAX_STR_LEN * 2 + 8;
    int escLblSz = FILTER_LABEL_LEN * 2 + 8;
    char *escMsg = (char *)malloc(escMsgSz);
    char *escLbl = (char *)malloc(escLblSz);
    char  escAddr[32], escTime[32], escDate[32], escMode[64], escType[64], escBit[32];
    if (!escMsg || !escLbl) { free(escMsg); free(escLbl); return 0; }

    MySqlEscape(escAddr, sizeof(escAddr), job->szCapcode);
    MySqlEscape(escTime, sizeof(escTime), job->szTime);
    MySqlEscape(escDate, sizeof(escDate), job->szDate);
    MySqlEscape(escMode, sizeof(escMode), job->szMode);
    MySqlEscape(escType, sizeof(escType), job->szType);
    MySqlEscape(escBit,  sizeof(escBit),  job->szBitrate);
    MySqlEscape(escMsg,  escMsgSz,        job->szMessage);
    MySqlEscape(escLbl,  escLblSz,        job->szLabel);

    int n = _snprintf(out, outLen - 1,
        "INSERT INTO `%s` (`address`,`msg_time`,`msg_date`,`mode`,`msg_type`,`bitrate`,`message`,`label`) "
        "VALUES ('%s','%s','%s','%s','%s','%s','%s','%s')",
        g_szTable, escAddr, escTime, escDate, escMode, escType, escBit, escMsg, escLbl);

    free(escMsg); free(escLbl);
    if (n < 0) n = 0;
    out[outLen - 1] = '\0';
    return n;
}

static int BuildInsertOptimized(char *out, int outLen, const MysqlJob *job)
{
    char szDateTime[32];
    ConvertDateTime(job->szDate, job->szTime, szDateTime, sizeof(szDateTime));

    // FIX [MysqlCapcodeChar]: capcode als string bewaren zodat leading zeros (ASTRID) intact blijven
    char escCap[32];
    MySqlEscape(escCap, sizeof(escCap), job->szCapcode ? job->szCapcode : "");
    int           bitrate = atoi(job->szBitrate);
    int           fields  = g_iFields;

    char colBuf[256];
    int  cPos = 0;

    char *valBuf = (char *)malloc(MYSQL_MAX_QUERY);
    if (!valBuf) return 0;
    int vPos = 0;

    cPos += _snprintf(colBuf + cPos, (int)sizeof(colBuf) - cPos - 1,
                      "`received`, `address`");
    vPos += _snprintf(valBuf + vPos, MYSQL_MAX_QUERY - vPos - 1,
                      "'%s', '%s'", szDateTime, escCap);

    if (fields & MYF_MODE) {
        char esc[64];
        MySqlEscape(esc, sizeof(esc), job->szMode);
        cPos += _snprintf(colBuf + cPos, (int)sizeof(colBuf) - cPos - 1, ", `mode`");
        vPos += _snprintf(valBuf + vPos, MYSQL_MAX_QUERY - vPos - 1, ", '%s'", esc);
    }
    if (fields & MYF_MSG_TYPE) {
        char trimmed[16], esc[32];
        TrimCopy(trimmed, sizeof(trimmed), job->szType);
        MySqlEscape(esc, sizeof(esc), trimmed);
        cPos += _snprintf(colBuf + cPos, (int)sizeof(colBuf) - cPos - 1, ", `msg_type`");
        vPos += _snprintf(valBuf + vPos, MYSQL_MAX_QUERY - vPos - 1, ", '%s'", esc);
    }
    if (fields & MYF_BITRATE) {
        cPos += _snprintf(colBuf + cPos, (int)sizeof(colBuf) - cPos - 1, ", `bitrate`");
        vPos += _snprintf(valBuf + vPos, MYSQL_MAX_QUERY - vPos - 1, ", %d", bitrate);
    }
    if (fields & MYF_MESSAGE) {
        int escSz = MAX_STR_LEN * 2 + 8;
        char *esc = (char *)malloc(escSz);
        if (esc) {
            MySqlEscape(esc, escSz, job->szMessage);
            cPos += _snprintf(colBuf + cPos, (int)sizeof(colBuf) - cPos - 1, ", `message`");
            vPos += _snprintf(valBuf + vPos, MYSQL_MAX_QUERY - vPos - 1, ", '%s'", esc);
            free(esc);
        }
    }
    if (fields & MYF_LABEL) {
        int escSz = FILTER_LABEL_LEN * 2 + 8;
        char *esc = (char *)malloc(escSz);
        if (esc) {
            MySqlEscape(esc, escSz, job->szLabel);
            cPos += _snprintf(colBuf + cPos, (int)sizeof(colBuf) - cPos - 1, ", `label`");
            vPos += _snprintf(valBuf + vPos, MYSQL_MAX_QUERY - vPos - 1, ", '%s'", esc);
            free(esc);
        }
    }
    /* subscribers — omit for non-group messages (DEFAULT '' applies) */
    if (job->szSubscribers[0]) {
        int escSz = MYSQL_SUBSCRIBERS_LEN * 2 + 8;
        char *esc = (char *)malloc(escSz);
        if (esc) {
            MySqlEscape(esc, escSz, job->szSubscribers);
            cPos += _snprintf(colBuf + cPos, (int)sizeof(colBuf) - cPos - 1, ", `subscribers`");
            vPos += _snprintf(valBuf + vPos, MYSQL_MAX_QUERY - vPos - 1, ", '%s'", esc);
            free(esc);
        }
    }

    /* match_type — always written (0 = no filter match) */
    cPos += _snprintf(colBuf + cPos, (int)sizeof(colBuf) - cPos - 1, ", `match_type`");
    vPos += _snprintf(valBuf + vPos, MYSQL_MAX_QUERY - vPos - 1, ", %d", job->iMatchType);

    /* label_color — omit if empty (DEFAULT '' applies) */
    if (job->szLabelColor[0]) {
        char esc[16]; MySqlEscape(esc, sizeof(esc), job->szLabelColor);
        cPos += _snprintf(colBuf + cPos, (int)sizeof(colBuf) - cPos - 1, ", `label_color`");
        vPos += _snprintf(valBuf + vPos, MYSQL_MAX_QUERY - vPos - 1, ", '%s'", esc);
    }

    colBuf[cPos] = '\0';
    valBuf[vPos] = '\0';

    int n = _snprintf(out, outLen - 1, "INSERT INTO `%s` (%s) VALUES (%s)",
                      g_szTable, colBuf, valBuf);
    free(valBuf);
    if (n < 0) n = 0;
    out[outLen - 1] = '\0';
    return n;
}

// ===========================================================================
// FLEX group call — accumulate subscriber info, flush as one row
// ===========================================================================

/* Called from ShowMessage() for each subscriber row in a FLEX group call.
   Captures message/time/etc. from the first subscriber; accumulates capcode+label JSON. */
/* FIX [GroupMatchEscalate]: rang van een match_type voor de groep-rij. De groep moet
   getoond worden zoals PDW het scherm rendert: elke subscriber wordt los gerouteerd, dus
   zit er ook maar EEN filtered lid in de groep dan staat de groep in het filterpaneel.
   Prioriteit: filtered (1) > monitor-only (2) > geen match (0). */
static int MysqlMatchTypeRank(int mt) { return (mt == 1) ? 2 : (mt == 2) ? 1 : 0; }

void MysqlGroupAccumulate(const char *capcode, const char *label,
                          const char *message,
                          const char *szTime, const char *szDate,
                          const char *szMode, const char *szType, const char *szBitrate,
                          int matchType, const char *labelColor,
                          int groupbit)
{
    if (!g_bCsInit) return;
    if (groupbit < 0 || groupbit >= MAX_MYSQL_GROUPBITS) return;

    MysqlGroupAcc *ga = &g_groupAcc[groupbit];

    /* FIX [MysqlSubscrTrunc]: save write position before the comma so we can roll back if
       the entry doesn't fit — otherwise a failed _snprintf leaves a dangling comma and the
       next subscriber adds a second one, producing invalid JSON ("},,]"). */
    int sPosBack = ga->sPos;
    if (!ga->active) {
        /* First subscriber: capture message data and filter state for the group row. */
        ga->active     = TRUE;
        ga->sPos       = 0;
        ga->nSubscr    = 0;
        // FIX [GroupcallIgnoreFeed]: group-ROW match_type stays 0/1/2 (strongest member). An
        // ignored member carries per-subscriber match_type 3 (written to the JSON below), but
        // counts as 0 for the row aggregate so 3 never leaks into the row column.
        ga->iMatchType = (matchType == 3) ? 0 : matchType;
#define GACOPY(dst, src) strncpy(dst, (src) ? (src) : "", sizeof(dst) - 1); dst[sizeof(dst)-1] = '\0'
        // FIX [MysqlUtf8]: groepsbericht-body net zo saneren als MysqlNotify (zelfde UTF-8 garantie).
        Utf8SanitizeForMysql(ga->szMessage, sizeof(ga->szMessage), message ? message : "", g_bLinefeed);
        GACOPY(ga->szTime,       szTime);
        GACOPY(ga->szDate,       szDate);
        GACOPY(ga->szMode,       szMode);
        GACOPY(ga->szType,       szType);
        GACOPY(ga->szBitrate,    szBitrate);
        GACOPY(ga->szLabelColor, labelColor);
#undef GACOPY
        /* Start JSON array */
        ga->szSubscr[ga->sPos++] = '[';
        sPosBack = ga->sPos;  /* '[' stays even if the first entry overflows */
    } else {
        /* Subsequent subscriber: append comma separator (rolled back below if entry overflows). */
        if (ga->sPos < MYSQL_SUBSCRIBERS_LEN - 2)
            ga->szSubscr[ga->sPos++] = ',';
        /* FIX [GroupMatchPerCapcode]: de groep-rij-kolom match_type is enkel voor
           queryability (sterkste match over alle leden, zodat een groep met een
           filtered lid in `WHERE match_type>=1` opduikt). De WEERGAVE per lid wordt
           NIET hierdoor bepaald maar door de per-subscriber match_type in de JSON
           hieronder -- zo toont de website precies wat het PDW-window toont: enkel
           het filtered lid in het filterpaneel, de rest monitor-only. */
        if (MysqlMatchTypeRank(matchType) > MysqlMatchTypeRank(ga->iMatchType))
            ga->iMatchType = matchType;
    }

    /* Append {"address":"0123456","label":"escaped","match_type":N[,"color":"#RRGGBB"]} entry,
       leave room for closing "]" */
    char escLabel[FILTER_LABEL_LEN * 2 + 4];
    JsonEscapeStr(escLabel, sizeof(escLabel), label ? label : "");

    // FIX [MysqlCapcodeChar]: capcode als string zodat leading zeros (ASTRID) intact blijven
    char escCc[32];
    JsonEscapeStr(escCc, sizeof(escCc), capcode ? capcode : "");
    int written;
    /* FIX [WebGroupColor]: kleur per abonnee meeschrijven zodat de website de
       capcode-labels onder een groepsbericht in hun eigen kleur kan tonen.
       FIX [GroupMatchPerCapcode]: per-subscriber match_type meeschrijven (0=geen,
       1=filtered, 2=monitor-only, 3=ignored-in-group [FIX GroupcallIgnoreFeed]) zodat de
       website elk lid in het juiste paneel toont, identiek aan het PDW-window. */
    if (labelColor && labelColor[0]) {
        char escColor[16];
        JsonEscapeStr(escColor, sizeof(escColor), labelColor);
        written = _snprintf(ga->szSubscr + ga->sPos,
                            MYSQL_SUBSCRIBERS_LEN - ga->sPos - 2, /* -2 for final "]" + NUL */
                            "{\"address\":\"%s\",\"label\":\"%s\",\"match_type\":%d,\"color\":\"%s\"}",
                            escCc, escLabel, matchType, escColor);
    } else {
        written = _snprintf(ga->szSubscr + ga->sPos,
                            MYSQL_SUBSCRIBERS_LEN - ga->sPos - 2, /* -2 for final "]" + NUL */
                            "{\"address\":\"%s\",\"label\":\"%s\",\"match_type\":%d}",
                            escCc, escLabel, matchType);
    }
    if (written > 0) { ga->sPos += written; ga->nSubscr++; }
    else             { ga->sPos = sPosBack; } /* roll back comma — entry didn't fit, skip silently */
}

/* Called from ConvertGroupcall() after all subscriber rows have been shown.
   Builds and enqueues the group row (capcode = 2029568+groupbit). */
void MysqlFlushGroup(int groupbit)
{
    if (!g_bRunning || !g_bCsInit) return;
    if (groupbit < 0 || groupbit >= MAX_MYSQL_GROUPBITS) return;

    MysqlGroupAcc *ga = &g_groupAcc[groupbit];
    if (!ga->active) return;

    /* Close the JSON array */
    if (ga->sPos < MYSQL_SUBSCRIBERS_LEN - 1) {
        ga->szSubscr[ga->sPos++] = ']';
        ga->szSubscr[ga->sPos]   = '\0';
    } else {
        ga->szSubscr[MYSQL_SUBSCRIBERS_LEN - 1] = '\0';
    }

    MysqlJob job;
    ZeroMemory(&job, sizeof(job));

    /* Group capcode is always 2029568 + groupbit. */
    _snprintf(job.szCapcode, sizeof(job.szCapcode) - 1, "%07lu", (unsigned long)(2029568 + groupbit));
    job.szCapcode[sizeof(job.szCapcode) - 1] = '\0';

#define JCOPY(dst, src) strncpy(dst, src, sizeof(dst) - 1); dst[sizeof(dst)-1] = '\0'
    JCOPY(job.szMessage,     ga->szMessage);
    JCOPY(job.szTime,        ga->szTime);
    JCOPY(job.szDate,        ga->szDate);
    JCOPY(job.szMode,        ga->szMode);
    JCOPY(job.szType,        ga->szType);
    JCOPY(job.szBitrate,     ga->szBitrate);
    JCOPY(job.szSubscribers, ga->szSubscr);
    JCOPY(job.szLabelColor,  ga->szLabelColor);
#undef JCOPY
    job.iMatchType = ga->iMatchType;

    WriteLog("GROUP    capcode=%s subscribers=%d", job.szCapcode, ga->nSubscr);

    ZeroMemory(ga, sizeof(*ga));  /* clear slot before releasing lock */

    EnterCriticalSection(&g_cs);
    if (!QueueFull()) {
        g_queue[g_qTail] = job;
        g_qTail = (g_qTail + 1) % MYSQL_QUEUE_SIZE;
        LeaveCriticalSection(&g_cs);
        SetEvent(g_hEvent);
    } else {
        // FIX [MysqlQueueDrop]: count + log overflow drops (was a silent loss).
        g_mysqlDropped++;
        LeaveCriticalSection(&g_cs);
        WriteLog("DROP queue full — group row discarded (total dropped=%u)", g_mysqlDropped);
    }
}

/* Dispatch to the right INSERT builder based on g_iSchema. */
static int BuildInsert(char *out, int outLen, const MysqlJob *job)
{
    switch (g_iSchema) {
    case MYSQL_SCHEMA_CLASSIC:  return BuildInsertClassic (out, outLen, job);
    case MYSQL_SCHEMA_EXTENDED: return BuildInsertExtended(out, outLen, job);
    default:                    return BuildInsertOptimized(out, outLen, job);
    }
}

// ===========================================================================
// Worker thread
// ===========================================================================

// FIX [MysqlStopDrain]: log rows the shutdown-flush drain (tail of MysqlWorker) could not deliver.
// Called with the row that just failed already dequeued (g_qHead advanced past it), so the queued
// count still under g_cs is the remainder still waiting; +1 accounts for that already-dequeued row.
static void LogDrainLoss(void)
{
    int nRemaining;
    EnterCriticalSection(&g_cs);
    nRemaining = (g_qTail - g_qHead + MYSQL_QUEUE_SIZE) % MYSQL_QUEUE_SIZE;
    LeaveCriticalSection(&g_cs);
    WriteLog("LOST      shutdown flush aborted - %d row(s) dropped", nRemaining + 1);
}

static DWORD WINAPI MysqlWorker(LPVOID)
{
    DWORD     backoffMs     = 1000;
    ULONGLONG nextConnectMs = 0;
    int       jobAttempts   = 0;    // FIX [MysqlRequeue]: consecutive rejections of the head row

    // FIX [MysqlQueryLen]: one heap buffer for the worker's lifetime, reused for every INSERT.
    // A MYSQL_MAX_QUERY (128 kB) array on the thread stack — per iteration, in two places — risked
    // a stack overflow; a single reused heap buffer is both safe and cheaper than malloc-per-row.
    char *sql = (char *)malloc(MYSQL_MAX_QUERY);
    if (!sql) { FeedStatus_SetDetail(FEED_MYSQL, "out of memory (query buffer)"); PostStatus(MYS_ERROR); return 0; }   // FIX [FeedLastError]

    while (g_bRunning) {
        WaitForSingleObject(g_hEvent, 200);
        if (!g_bRunning) break;

        ULONGLONG now = GetTickCount64();
        if (now < nextConnectMs) continue;

        if (g_sock == INVALID_SOCKET) {
            WriteLog("RECONNECT  host=%s port=%d (backoff was %lu ms)", g_szHost, g_iPort, (unsigned long)backoffMs);
            g_sock = TryConnect();
            if (g_sock == INVALID_SOCKET) {
                nextConnectMs = GetTickCount64() + backoffMs;
                backoffMs     = (backoffMs < 30000) ? backoffMs * 2 : 30000;
                FeedStatus_SetDetail(FEED_MYSQL, "connect failed (%s:%d)", g_szHost, g_iPort);   // FIX [FeedLastError]
                PostStatus(MYS_ERROR);
                continue;
            }
            backoffMs = 1000;
            // FIX [FeedStatusConn]: a successful (re)connect turns the Health-panel dot green
            // immediately instead of leaving the previous connect-failure red (or the startup
            // hollow) standing until the next INSERT, which can be hours away. Deliberately NOT
            // via PostStatus: the config dialog keeps showing per-insert status only.
            FeedStatus_Set(FEED_MYSQL, MYS_OK);
        }

        while (g_bRunning) {
            MysqlJob job;
            BOOL     bHaveJob = FALSE;

            /* FIX [MysqlRequeue]: peek the head row but do NOT advance g_qHead until the INSERT is
               confirmed. On a transport failure the row stays queued and is retried after the
               reconnect instead of being lost (the old code dequeued before sending). The worker
               remains the sole mutator of g_qHead, so the SPSC invariant still holds. */
            EnterCriticalSection(&g_cs);
            if (!QueueEmpty()) {
                job      = g_queue[g_qHead];
                bHaveJob = TRUE;
            }
            LeaveCriticalSection(&g_cs);

            if (!bHaveJob) break;

            int  sqlLen = BuildInsert(sql, MYSQL_MAX_QUERY, &job);
            if (sqlLen <= 0) {
                /* Un-buildable row (e.g. allocation failure) — drop it so we don't spin forever. */
                // FIX [MysqlDropLog]: log the drop -- every other drop path in this file (queue-full,
                // give-up-after-retries, shutdown-flush) already logs; this one dropped silently.
                WriteLog("DROP      unbuildable row (out of memory) capcode=%s", job.szCapcode);
                EnterCriticalSection(&g_cs);
                g_qHead = (g_qHead + 1) % MYSQL_QUEUE_SIZE;
                LeaveCriticalSection(&g_cs);
                jobAttempts = 0;
                continue;
            }

            PostStatus(MYS_SENDING);

            char errMsg[256] = "";
            BOOL ok = SendQuery(g_sock, sql, sqlLen) &&
                      ReadQueryResult(g_sock, errMsg, sizeof(errMsg));
            if (ok) {
                EnterCriticalSection(&g_cs);
                g_qHead = (g_qHead + 1) % MYSQL_QUEUE_SIZE;   /* commit: only now is the row safe */
                LeaveCriticalSection(&g_cs);
                jobAttempts = 0;
                WriteLog("INSERT OK  capcode=%s", job.szCapcode);
                PostStatus(MYS_OK);
            } else {
                WriteLog("INSERT FAIL  capcode=%s  %s", job.szCapcode, errMsg);
                FeedStatus_SetDetail(FEED_MYSQL, "INSERT failed: %.80s", errMsg);   // FIX [FeedLastError]
                PostStatus(MYS_ERROR);

                /* A "MySQL error N" reply means the server is reachable but rejected THIS row
                   (bad data, constraint, deadlock…). Retry it a few times on the same connection,
                   then drop it so one poison row can't block the whole feed. Any other failure is
                   a transport problem: hold the row (head not advanced) and reconnect. */
                if (strncmp(errMsg, "MySQL error", 11) == 0) {
                    if (++jobAttempts >= MYSQL_MAX_JOB_RETRIES) {
                        WriteLog("INSERT GIVE UP  dropping capcode=%s after %d attempts",
                                 job.szCapcode, jobAttempts);
                        EnterCriticalSection(&g_cs);
                        g_qHead = (g_qHead + 1) % MYSQL_QUEUE_SIZE;
                        LeaveCriticalSection(&g_cs);
                        jobAttempts = 0;
                    }
                    continue;   /* keep the connection; retry the row or move to the next */
                }

                closesocket(g_sock);
                g_sock        = INVALID_SOCKET;
                nextConnectMs = GetTickCount64() + backoffMs;
                backoffMs     = (backoffMs < 30000) ? backoffMs * 2 : 30000;
                break;          /* row remains at head — retried after reconnect */
            }
        }
    }

    /* Flush remaining jobs on clean shutdown.
       FIX [MysqlFlushLock]: dequeue under g_cs, mirroring the main loop. g_bRunning is already
       FALSE here, but a producer that passed its unlocked g_bRunning check just before shutdown can
       still be inside EnterCriticalSection writing g_qTail/g_queue while we drain — reading the ring
       unlocked was a data race on shared state. */
    for (;;) {
        MysqlJob job;
        BOOL bHaveJob = FALSE;
        EnterCriticalSection(&g_cs);
        if (!QueueEmpty()) {
            job     = g_queue[g_qHead];
            g_qHead = (g_qHead + 1) % MYSQL_QUEUE_SIZE;
            bHaveJob = TRUE;
        }
        LeaveCriticalSection(&g_cs);
        if (!bHaveJob) break;

        // FIX [MysqlStopDrain]: bound this flush and log what gets lost instead of silently
        // discarding it (the old code just skipped the send when g_sock was already gone, with no
        // log line and no way for the caller/grace-window logic to know rows were lost). Keyed on
        // g_sock validity + SendQuery's result only -- ReadQueryResult's FALSE return also fires for
        // a genuine per-row server rejection (bad data/constraint), which is not proof the
        // connection is dead, so treating it as a drain-abort signal would over-report loss on a
        // single bad row instead of just skipping that one row. SendQuery failing (or the socket
        // already being gone) is the real "this connection cannot deliver anything more" signal.
        if (g_sock == INVALID_SOCKET) {
            LogDrainLoss();
            break;
        }

        int sqlLen = BuildInsert(sql, MYSQL_MAX_QUERY, &job);
        if (sqlLen > 0) {
            if (!SendQuery(g_sock, sql, sqlLen)) {
                LogDrainLoss();
                break;
            }
            ReadQueryResult(g_sock, NULL, 0);  /* result ignored: see comment above */
        }
        else {
            /* FIX [MysqlDropLog]: same OOM-drop logging as the main loop - the drain skipped it silently. */
            WriteLog("DROP      unbuildable row (out of memory) capcode=%s", job.szCapcode);
        }
    }

    free(sql);

    if (g_sock != INVALID_SOCKET) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
    return 0;
}

// ===========================================================================
// Public API
// ===========================================================================

void MysqlInit(void)
{
    if (!g_bCsInit) {
        InitializeCriticalSection(&g_cs);
        g_bCsInit = TRUE;
    }
    MysqlStop();

    if (!Profile.mysql_enabled || !Profile.mysql_host[0] || !Profile.mysql_database[0]) {
        PostStatus(MYS_DISABLED);
        return;
    }

    strncpy(g_szHost,     Profile.mysql_host,     sizeof(g_szHost)     - 1);
    strncpy(g_szUser,     Profile.mysql_user,     sizeof(g_szUser)     - 1);
    strncpy(g_szPass,     Profile.mysql_pass,     sizeof(g_szPass)     - 1);
    strncpy(g_szDatabase, Profile.mysql_database, sizeof(g_szDatabase) - 1);
    strncpy(g_szTable,    Profile.mysql_table[0]  ? Profile.mysql_table
                                                  : "messages",
                          sizeof(g_szTable) - 1);
    g_szHost    [sizeof(g_szHost)     - 1] = '\0';
    g_szUser    [sizeof(g_szUser)     - 1] = '\0';
    g_szPass    [sizeof(g_szPass)     - 1] = '\0';
    g_szDatabase[sizeof(g_szDatabase) - 1] = '\0';
    g_szTable   [sizeof(g_szTable)    - 1] = '\0';

    /* FIX [MysqlIdent]: the database and table names are interpolated raw inside backticks in
       USE/CREATE DATABASE/CREATE TABLE/INSERT. Drop any character that could break out of the
       `...` quoting (backtick) or smuggle a statement (control chars, ';', whitespace). Keeps the
       feed safe even if the INI is hand-edited; normal identifier characters pass through. */
    {
        char *pSrc, *pDst;
        for (pSrc = pDst = g_szDatabase; *pSrc; pSrc++)
            if (*pSrc != '`' && *pSrc != ';' && (unsigned char)*pSrc > ' ') *pDst++ = *pSrc;
        *pDst = '\0';
        for (pSrc = pDst = g_szTable; *pSrc; pSrc++)
            if (*pSrc != '`' && *pSrc != ';' && (unsigned char)*pSrc > ' ') *pDst++ = *pSrc;
        *pDst = '\0';
        if (!g_szTable[0]) strcpy(g_szTable, "messages");
        if (!g_szDatabase[0]) { PostStatus(MYS_DISABLED); return; }   /* nothing safe left to use */
    }
    g_iPort      = (Profile.mysql_port > 0) ? Profile.mysql_port : 3306;
    g_iFields    = Profile.mysql_fields;
    g_iSchema    = Profile.mysql_schema;
    g_bLinefeed  = Profile.Linefeed ? TRUE : FALSE;   // FIX [MysqlUtf8]: spiegelt de GUI »-als-linefeed checkbox
    g_bLogToFile = Profile.mysql_logToFile ? TRUE : FALSE;

    g_hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_hEvent) return;

    g_qHead    = g_qTail = 0;
    ZeroMemory(g_groupAcc, sizeof(g_groupAcc));
    g_bRunning = TRUE;
    g_hThread  = CreateThread(NULL, 0, MysqlWorker, NULL, 0, NULL);
    if (!g_hThread) {
        g_bRunning = FALSE;
        CloseHandle(g_hEvent);
        g_hEvent = NULL;
        PostStatus(MYS_DISABLED);
    } else {
        static const char *schemaNames[] = { "Classic", "Extended", "Optimized" };
        int si = (g_iSchema >= 0 && g_iSchema <= 2) ? g_iSchema : 2;
        WriteLog("START       host=%s port=%d db=%s schema=%s", g_szHost, g_iPort, g_szDatabase, schemaNames[si]);
    }
}

void MysqlNotify(const char *capcode, const char *message, const char *label,
                 const char *szTime,  const char *szDate,
                 const char *szMode,  const char *szType, const char *szBitrate,
                 int matchType, const char *labelColor)
{
    if (!g_bRunning || !g_bCsInit) return;

    EnterCriticalSection(&g_cs);
    if (!QueueFull()) {
        MysqlJob *j = &g_queue[g_qTail];
        ZeroMemory(j, sizeof(*j));
#define SAFE_COPY(dst, src) \
    strncpy((dst), (src) ? (src) : "", sizeof(dst) - 1); \
    (dst)[sizeof(dst) - 1] = '\0'
        SAFE_COPY(j->szCapcode,    capcode);
        // FIX [MysqlUtf8]: message + label naar geldig UTF-8 vóór de queue, zodat de worker-retry
        // nooit een byte herverstuurt die utf8mb4 weigert (was: 3x INSERT FAIL -> rij gedropt).
        Utf8SanitizeForMysql(j->szMessage, sizeof(j->szMessage), message ? message : "", g_bLinefeed);
        Utf8SanitizeForMysql(j->szLabel,   sizeof(j->szLabel),   label   ? label   : "", FALSE);
        SAFE_COPY(j->szTime,       szTime);
        SAFE_COPY(j->szDate,       szDate);
        SAFE_COPY(j->szMode,       szMode);
        SAFE_COPY(j->szType,       szType);
        SAFE_COPY(j->szBitrate,    szBitrate);
        SAFE_COPY(j->szLabelColor, labelColor);
#undef SAFE_COPY
        j->iMatchType = matchType;
        g_qTail = (g_qTail + 1) % MYSQL_QUEUE_SIZE;
        LeaveCriticalSection(&g_cs);
        SetEvent(g_hEvent);
    } else {
        // FIX [MysqlQueueDrop]: count + log overflow drops (was a silent loss, unlike the
        // webhook/MQTT feeds which already track this).
        g_mysqlDropped++;
        LeaveCriticalSection(&g_cs);
        WriteLog("DROP queue full — row discarded (total dropped=%u)", g_mysqlDropped);
    }
}

void MysqlStop(void)
{
    if (!g_bRunning) return;

    WriteLog("STOP        feed uitgeschakeld");
    g_bRunning = FALSE;
    if (g_hEvent) SetEvent(g_hEvent);

    // FIX [MysqlStopDrain]: give the worker a bounded grace window to run its shutdown-flush drain
    // (see the tail of MysqlWorker) over the STILL-INTACT socket before the shutdown() kick below
    // forcibly interrupts it. Without this, that kick killed the live connection before the drain
    // loop ever got a chance to send the still-queued rows, so every stop/reconfigure could silently
    // lose up to MYSQL_QUEUE_SIZE-1 rows with no log line. Healthy server: the worker finishes its
    // main loop and drains the queue over the live socket well within the grace window, so this
    // branch handles the common case. Dead/stalled server: the grace window times out and execution
    // falls through unconditionally to the existing shutdown()+join below -- a second
    // WaitForSingleObject on an already-signaled thread handle returns immediately, so the INFINITE
    // join further down stays correct either way.
    if (g_hThread && WaitForSingleObject(g_hThread, 3000) == WAIT_OBJECT_0) {
        /* worker exited cleanly - shutdown-flush drain delivered (or gave up) over the live socket */
    }

    /* FIX [ShutdownRace]: interrupt a blocking recv()/select() in the worker so it returns at once,
       then join to FULL completion. The old 5 s timeout could return while the worker was still
       mid-transaction, after which CloseHandle + (later) DeleteCriticalSection ran under a live
       thread (crash on exit) — and a runtime reconfigure (MysqlInit→MysqlStop) would then start a
       SECOND worker sharing g_cs/g_queue/g_sock. With the bounded connect above plus this shutdown,
       the worker always returns promptly, so INFINITE is safe and correct.
       FIX [MysqlShutdownFd]: snapshot g_sock once so the check and the shutdown() use the SAME
       descriptor. The worker may closesocket(g_sock) concurrently; re-reading the global could
       let another feed's just-opened socket reuse that fd number between the test and the call,
       shutting down an unrelated connection. shutdown() on an already-closed fd is a harmless
       no-op. (After g_bRunning=FALSE the mysql worker never opens a new socket itself.) */
    SOCKET sShut = g_sock;
    if (sShut != INVALID_SOCKET) shutdown(sShut, SD_BOTH);

    // FIX [MysqlConnAbort]: mirror the [MysqlShutdownFd] snapshot above for the in-progress
    // TryConnect socket -- read g_connSock once so the validity check and the shutdown() call use
    // the same descriptor. TryConnect resets g_connSock to INVALID_SOCKET on every exit path
    // (including success), so a stale non-INVALID value here can only be a socket TryConnect is
    // still actively using; shutdown() on an already-closed fd is a harmless no-op. Only shutdown(),
    // never closesocket() -- TryConnect (and, on success, the worker via g_sock) still owns and
    // closes this socket itself.
    SOCKET sConn = g_connSock;
    if (sConn != INVALID_SOCKET) shutdown(sConn, SD_BOTH);

    if (g_hThread) {
        WaitForSingleObject(g_hThread, INFINITE);
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }
    if (g_hEvent) { CloseHandle(g_hEvent); g_hEvent = NULL; }

    g_qHead = g_qTail = 0;
    ZeroMemory(g_groupAcc, sizeof(g_groupAcc));
    PostStatus(MYS_DISABLED);
}

void MysqlDestroy(void)
{
    MysqlStop();
    if (g_bCsInit) {
        DeleteCriticalSection(&g_cs);
        g_bCsInit = FALSE;
    }
    if (g_bWsaInit) {
        WSACleanup();
        g_bWsaInit = FALSE;
    }
}

// FIX [ConnTest]: one-shot connection test for the Setup dialog. Connects with a LOCAL socket so it
// never touches the running worker's g_sock/globals; authenticates and (if a database name is given)
// checks whether it is accessible. Does NOT create anything. Blocks up to ~5 s.
BOOL MysqlTestConnection(const char *host, int port, const char *user, const char *pass,
                         const char *database, char *szMsg, int msgLen)
{
    if (!szMsg || msgLen < 1) return FALSE;
    szMsg[0] = '\0';

    if (!host || !host[0]) {
        _snprintf(szMsg, msgLen - 1, "No host entered.");
        szMsg[msgLen - 1] = '\0';
        return FALSE;
    }
    if (port <= 0) port = 3306;
    if (!user) user = "";
    if (!pass) pass = "";

    if (!EnsureWsa()) {
        _snprintf(szMsg, msgLen - 1, "Winsock initialisation failed.");
        szMsg[msgLen - 1] = '\0';
        return FALSE;
    }

    ADDRINFOA hints, *res = NULL;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char szPort[8];
    _snprintf(szPort, sizeof(szPort) - 1, "%d", port);
    szPort[sizeof(szPort) - 1] = '\0';

    if (getaddrinfo(host, szPort, &hints, &res) != 0 || !res) {
        _snprintf(szMsg, msgLen - 1, "Cannot resolve host name '%s'.", host);
        szMsg[msgLen - 1] = '\0';
        return FALSE;
    }

    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        _snprintf(szMsg, msgLen - 1, "Could not create socket.");
        szMsg[msgLen - 1] = '\0';
        return FALSE;
    }

    /* Bounded 5 s non-blocking connect (same approach as the worker's TryConnect). */
    BOOL connected = FALSE;
    {
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        int crc = connect(s, res->ai_addr, (int)res->ai_addrlen);
        if (crc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wrSet, exSet;
            FD_ZERO(&wrSet); FD_SET(s, &wrSet);
            FD_ZERO(&exSet); FD_SET(s, &exSet);
            timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
            int sel = select((int)s + 1, NULL, &wrSet, &exSet, &tv);
            if (sel > 0 && !FD_ISSET(s, &exSet)) {
                int soErr = 0, soLen = (int)sizeof(soErr);
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soErr, &soLen);
                connected = (soErr == 0);
            }
        } else {
            connected = (crc != SOCKET_ERROR);
        }
        nb = 0;
        ioctlsocket(s, FIONBIO, &nb);
    }
    freeaddrinfo(res);

    if (!connected) {
        closesocket(s);
        _snprintf(szMsg, msgLen - 1,
                  "Cannot connect to %s:%d.\n\nHost down, wrong port, or blocked by a firewall.", host, port);
        szMsg[msgLen - 1] = '\0';
        return FALSE;
    }

    BYTE pkt[2048];
    int  pktLen = 0;
    if (!ReadPacket(s, pkt, sizeof(pkt), &pktLen)) {
        closesocket(s);
        _snprintf(szMsg, msgLen - 1,
                  "Connected to %s:%d, but no MySQL handshake.\n\nIs this really a MySQL/MariaDB server?", host, port);
        szMsg[msgLen - 1] = '\0';
        return FALSE;
    }

    BYTE scramble[20];
    memset(scramble, 0, sizeof(scramble));
    if (!ParseHandshake(pkt, pktLen, scramble)) {
        closesocket(s);
        _snprintf(szMsg, msgLen - 1, "Bad or unsupported MySQL handshake.");
        szMsg[msgLen - 1] = '\0';
        return FALSE;
    }

    if (!SendHandshakeResponse(s, user, pass, database ? database : "", scramble)) {
        closesocket(s);
        _snprintf(szMsg, msgLen - 1, "Failed to send the authentication packet.");
        szMsg[msgLen - 1] = '\0';
        return FALSE;
    }

    if (!ReadPacket(s, pkt, sizeof(pkt), &pktLen) || pktLen < 1) {
        closesocket(s);
        _snprintf(szMsg, msgLen - 1, "No authentication response from the server.");
        szMsg[msgLen - 1] = '\0';
        return FALSE;
    }

    if (pkt[0] == 0xFF) {
        int eCode = (pktLen >= 3) ? (int)((WORD)pkt[1] | ((WORD)pkt[2] << 8)) : 0;
        const char *m = (pktLen >= 9 && pkt[3] == '#') ? (const char *)pkt + 9 : (const char *)pkt + 3;
        _snprintf(szMsg, msgLen - 1, "Login failed (error %d):\n%.180s", eCode, m);
        szMsg[msgLen - 1] = '\0';
        closesocket(s);
        return FALSE;
    }
    if (pkt[0] == 0xFE) {
        char plugin[64] = "";
        if (pktLen > 1) { int n = pktLen - 1; if (n > 63) n = 63; memcpy(plugin, pkt + 1, n); plugin[n] = '\0'; }
        _snprintf(szMsg, msgLen - 1,
                  "Server wants auth plugin '%s', which this build does not support.\n\n"
                  "Fix on the server:\nALTER USER '%s'@'<host>' IDENTIFIED WITH mysql_native_password BY '<password>';",
                  plugin[0] ? plugin : "(unknown)", user);
        szMsg[msgLen - 1] = '\0';
        closesocket(s);
        return FALSE;
    }
    if (pkt[0] != 0x00) {
        closesocket(s);
        _snprintf(szMsg, msgLen - 1, "Unexpected authentication response (0x%02X).", pkt[0]);
        szMsg[msgLen - 1] = '\0';
        return FALSE;
    }

    /* Auth OK — check database access without creating anything. */
    BOOL dbExists = FALSE;
    if (database && database[0]) {
        char useSql[160];
        _snprintf(useSql, sizeof(useSql) - 1, "USE `%s`", database);
        useSql[sizeof(useSql) - 1] = '\0';
        char err[256] = "";
        if (SendQuery(s, useSql, (int)strlen(useSql)) && ReadQueryResult(s, err, sizeof(err)))
            dbExists = TRUE;
    }

    closesocket(s);

    if (database && database[0]) {
        if (dbExists)
            _snprintf(szMsg, msgLen - 1,
                      "Success!\n\nConnected to %s:%d, logged in, and database '%s' is accessible.",
                      host, port, database);
        else
            _snprintf(szMsg, msgLen - 1,
                      "Connection and login OK.\n\nDatabase '%s' does not exist yet — it will be created "
                      "automatically on the first message (the user needs CREATE rights).",
                      database);
    } else {
        _snprintf(szMsg, msgLen - 1, "Connection and login OK (no database name entered).");
    }
    szMsg[msgLen - 1] = '\0';
    return TRUE;
}
