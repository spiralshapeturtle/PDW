/*
** pushover.cpp -- Pushover (pushover.net) output sink for PDW  (FIX [Pushover])
**
** Threading model (mirrors telegram.cpp / webhook.cpp):
**   Main/decoder thread -> PushoverNotify -> g_queue (CRITICAL_SECTION + event)
**   Worker thread       -> dequeues jobs, POSTs to api.pushover.net via WinHTTP
**
** Endpoint:  https://api.pushover.net/1/messages.json   (HTTPS, system TLS)
** Payload:   application/x-www-form-urlencoded
**            token=<app>&user=<key>&message=...&title=...&priority=N[&sound=..][&html=1][&device=..]
**
** Title:     maps 1:1 onto the SMTP subject; configurable template with the same
**            {label}/{capcode}/{time}/{date}/{mode}/{type}/{bitrate} placeholders as Telegram.
** Limits:    message <= 1024 chars, title <= 250 chars -> truncated.
** Priority:  -2..1 only. Emergency priority 2 (retry/expire + receipt polling) is intentionally
**            NOT offered in this phase.
** Rate:      HTTP 429 -> back-off retry. 4xx (bad token/user) -> no retry.
**
** Secret handling: the app token and user/group key are never written to the log.
*/

#ifndef STRICT
#define STRICT 1
#endif

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <atomic>      // FIX [AtomicRunning]: std::atomic<bool> for the cross-thread run flag

#include "..\headers\pdw.h"
#include "..\headers\initapp.h"
#include "pushover.h"
#include "logmanager.h"

#pragma comment(lib, "winhttp.lib")

#define PO_HOST          L"api.pushover.net"
#define PO_PATH          L"/1/messages.json"
#define PO_MAX_MSG       1024
#define PO_MAX_TITLE     250
#define PO_MSG_LEN       (MAX_STR_LEN + 256)
#define PO_QUEUE_SIZE    64
#define MAX_RETRIES      3

// ---------------------------------------------------------------------------
// Config snapshot (set at PushoverInit, protected by g_cs)
// ---------------------------------------------------------------------------

static char g_szAppToken[64] = "";
static char g_szUserKey [64] = "";
static char g_szTitle   [128] = "";
static char g_szBody    [256] = "";   // FIX [PoBodyTemplate]: body template, default "{message}"
static char g_szSound   [32] = "";
static char g_szDevice  [64] = "";
static int  g_iPriority      = 0;
static BOOL g_bHtml          = FALSE;
static BOOL g_bLogToFile     = FALSE;

// ---------------------------------------------------------------------------
// Job queue
// ---------------------------------------------------------------------------

// FIX [PoGroupBatch]: list capacities for the group-call accumulator. The capcode list matches the
// MQTT/webhook feeds (2048) so ~122 capcodes fit; the label list sits just above the Pushover message
// cap (1024), so the API limit - not this buffer - decides where a long list is cut.
// FIX [PoGroupNewline]: labels are joined with newlines so each subscriber appears on its own line.
#define PO_CAPLIST_LEN   2048   // space-separated capcode list (~180 capcodes)
#define PO_LABELLIST_LEN 1100   // newline-separated label list (just over the 1024 message cap)

typedef struct {
    char szCapcode[PO_CAPLIST_LEN];   // holds a space-separated capcode list for group calls
    char szMessage[PO_MSG_LEN];
    char szLabel  [PO_LABELLIST_LEN]; // holds a comma-separated label list for group calls
    char szTime   [32];
    char szDate   [32];
    char szMode   [32];
    char szType   [32];
    char szBitrate[32];
    int  jobPriority;       // FIX [PushoverPerFilter]: -9=use global, -2..1=override
    char jobSound[32];      // FIX [PushoverPerFilter]: ""=use global
} PushoverJob;

static PushoverJob g_queue[PO_QUEUE_SIZE];
static int         g_qHead = 0;
static int         g_qTail = 0;

// FIX [PoGroupBatch]: per-groupbit accumulator so a FLEX group call is sent as ONE Pushover
// notification listing all (matched) subscriber capcodes/labels. Mirrors the webhook/MySQL model.
#define PO_MAX_GROUPBITS 17
typedef struct {
    BOOL active;
    char szCapcodes[PO_CAPLIST_LEN];
    char szLabels  [PO_LABELLIST_LEN];
    char szMessage [PO_MSG_LEN];
    char szTime    [32];
    char szDate    [32];
    char szMode    [32];
    char szType    [32];
    char szBitrate [32];
    int  accPriority;       // FIX [GroupcallPrioMax]: highest priority across members (override or global); -9=none yet
    char accSound[32];      // FIX [GroupcallPerFilter]: aggregated per-filter sound (first non-empty wins)
} PoGroupAcc;
static PoGroupAcc g_groupAcc[PO_MAX_GROUPBITS];

// Append val to a comma/space-separated list in dst, skipping empties and adjacent duplicates.
static void AppendListItem(char *dst, int dstLen, const char *val, char sep)
{
    if (!val || !val[0]) return;
    int len = (int)strlen(dst);
    int vlen = (int)strlen(val);
    if (len >= vlen)
    {
        const char *tail = dst + len - vlen;
        if (!strcmp(tail, val) && (tail == dst || tail[-1] == sep || tail[-1] == ' '))
            return;
    }
    if (len > 0 && len < dstLen - 2) { dst[len++] = sep; if (sep == ',') dst[len++] = ' '; }
    _snprintf(dst + len, dstLen - 1 - len, "%s", val);
    dst[dstLen - 1] = '\0';
}

static BOOL inline QueueFull(void)  { return ((g_qTail + 1) % PO_QUEUE_SIZE) == g_qHead; }
static BOOL inline QueueEmpty(void) { return g_qHead == g_qTail; }

// ---------------------------------------------------------------------------
// Thread / sync
// ---------------------------------------------------------------------------

static HANDLE           g_hThread  = NULL;
static HANDLE           g_hEvent   = NULL;
// FIX [AtomicRunning]: was `volatile BOOL`. volatile gives neither atomicity nor cross-thread
// memory ordering in the C++ model, yet this flag is written by the GUI thread (Init/Shutdown)
// and read lock-free by the decoder thread (Notify/FlushGroup) and the worker (loop + sleep).
// std::atomic<bool> makes those reads/writes well-defined; assignment from BOOL TRUE/FALSE and
// use in boolean context are unchanged, so no call site needs to change.
static std::atomic<bool> g_bRunning(false);
static CRITICAL_SECTION g_cs;
static BOOL             g_csInit   = FALSE;
static HWND             g_hStatusWnd = NULL;
static unsigned         g_droppedJobs = 0;

static void PostStatus(int status, LPARAM lp)
{
    HWND hWnd;
    EnterCriticalSection(&g_cs);
    hWnd = g_hStatusWnd;
    LeaveCriticalSection(&g_cs);
    if (hWnd) PostMessage(hWnd, WM_PUSHOVER_STATUS, (WPARAM)status, lp);
}

static void WriteLog(const char *fmt, ...)
{
    if (!g_bLogToFile) return;
    char szLine[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(szLine, sizeof(szLine), _TRUNCATE, fmt, ap);
    va_end(ap);
    PDW_PUSHOVERLOG("%s", szLine);
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

// application/x-www-form-urlencoded percent-encoding (RFC 3986 unreserved kept).
static void UrlEncode(char *dst, int *pos, int maxLen, const char *src)
{
    static const char *hex = "0123456789ABCDEF";
    for (int i = 0; src && src[i] && *pos < maxLen - 4; i++)
    {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
        {
            dst[(*pos)++] = (char)c;
        }
        else
        {
            dst[(*pos)++] = '%';
            dst[(*pos)++] = hex[(c >> 4) & 0xF];
            dst[(*pos)++] = hex[c & 0xF];
        }
    }
}

// Append "key=urlencoded(value)" (with leading '&' when not first) to dst.
static void AppendField(char *dst, int *pos, int maxLen, BOOL *first, const char *key, const char *val)
{
    if (!*first && *pos < maxLen - 1) dst[(*pos)++] = '&';
    *first = FALSE;
    for (int i = 0; key[i] && *pos < maxLen - 1; i++) dst[(*pos)++] = key[i];
    if (*pos < maxLen - 1) dst[(*pos)++] = '=';
    UrlEncode(dst, pos, maxLen, val);
}

// Resolve {message}/{label}/{capcode}/{time}/{date}/{mode}/{type}/{bitrate} placeholders in a
// template. Values are copied verbatim (URL-encoding happens later in AppendField). Used for both
// the title and the body, so the two can be swapped freely.
static void ResolveTemplate(char *dst, int dstLen, const char *tmpl, const PushoverJob *job)
{
    int p = 0;
    const char *t = tmpl;
    while (*t && p < dstLen - 1)
    {
        // FIX [PoTemplateNewline]: literal "\n" -> real line break, "\\" -> single backslash.
        if (*t == '\\' && t[1] == 'n') { dst[p++] = '\n'; t += 2; continue; }
        if (*t == '\\' && t[1] == '\\') { dst[p++] = '\\'; t += 2; continue; }
        if (*t == '{')
        {
            const char *end = strchr(t, '}');
            if (end)
            {
                char key[24]; int kl = (int)(end - t - 1);
                if (kl > 0 && kl < (int)sizeof(key))
                {
                    memcpy(key, t + 1, kl); key[kl] = '\0';
                    const char *val = NULL;
                    if      (!_stricmp(key, "message")) val = job->szMessage;
                    else if (!_stricmp(key, "label"))   val = job->szLabel;
                    else if (!_stricmp(key, "capcode")) val = job->szCapcode;
                    else if (!_stricmp(key, "time"))    val = job->szTime;
                    else if (!_stricmp(key, "date"))    val = job->szDate;
                    else if (!_stricmp(key, "mode"))    val = job->szMode;
                    else if (!_stricmp(key, "type"))    val = job->szType;
                    else if (!_stricmp(key, "bitrate")) val = job->szBitrate;
                    if (val)
                    {
                        for (int k = 0; val[k] && p < dstLen - 1; k++) dst[p++] = val[k];
                        t = end + 1;
                        continue;
                    }
                }
            }
        }
        dst[p++] = *t++;
    }
    dst[p] = '\0';
}

// FIX [PoHtmlNewlineSpace]: do NOT convert newlines to <br> in html mode. Empirically Pushover
// already turns a bare "\n" into a line break in BOTH plain and html mode (nl2br on its side). The
// earlier conversion (whether "<br>\n" or "\n<br>") therefore stacked our own <br> on top of
// Pushover's own break, producing a stray leading space / blank line per capcode/label line in the
// app while the preview (HTML tags stripped) kept a single clean newline - hence the mismatch the
// user saw. The correct behaviour is to leave the newlines untouched: "\n" renders as one clean line
// break in plain mode, the app, AND the preview, while html=1 still styles <b>/<i>/etc. So no html-
// specific newline transform is needed at all.

// ---------------------------------------------------------------------------
// WinHTTP (worker-thread only)
// ---------------------------------------------------------------------------

static HINTERNET g_hSession = NULL;
static HINTERNET g_hConnect = NULL;

static void CloseConnection(void) { if (g_hConnect) { WinHttpCloseHandle(g_hConnect); g_hConnect = NULL; } }
static void CloseSession(void)    { CloseConnection(); if (g_hSession) { WinHttpCloseHandle(g_hSession); g_hSession = NULL; } }

static BOOL EnsureConnection(void)
{
    if (g_hConnect) return TRUE;
    if (!g_hSession)
    {
        g_hSession = WinHttpOpen(L"PDW-Pushover/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!g_hSession) return FALSE;
        WinHttpSetTimeouts(g_hSession, 10000, 10000, 10000, 10000);
        DWORD dwProto = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(g_hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &dwProto, sizeof(dwProto));
    }
    g_hConnect = WinHttpConnect(g_hSession, PO_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    return g_hConnect != NULL;
}

// POST form body to /1/messages.json. Returns HTTP status (0 = transport error). Body copied to respOut.
static int HttpPostForm(HINTERNET hConn, const char *body, int bodyLen, char *respOut, int respLen)
{
    if (respOut && respLen) respOut[0] = '\0';

    HINTERNET hReq = WinHttpOpenRequest(hConn, L"POST", PO_PATH, NULL,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hReq) return 0;

    static const WCHAR *szHeaders = L"Content-Type: application/x-www-form-urlencoded\r\nConnection: keep-alive\r\n";
    BOOL bOK = WinHttpSendRequest(hReq, szHeaders, (DWORD)-1L, (LPVOID)body, (DWORD)bodyLen, (DWORD)bodyLen, 0);
    if (!bOK) { WinHttpCloseHandle(hReq); return 0; }

    bOK = WinHttpReceiveResponse(hReq, NULL);
    if (!bOK) { WinHttpCloseHandle(hReq); return 0; }

    DWORD dwStatus = 0, dwSize = sizeof(dwStatus);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &dwStatus, &dwSize, NULL);

    int rpos = 0; DWORD dwRead = 0; char buf[1024];
    while (WinHttpReadData(hReq, buf, sizeof(buf), &dwRead) && dwRead > 0)
    {
        if (respOut && rpos < respLen - 1)
        {
            int n = (int)dwRead; if (n > respLen - 1 - rpos) n = respLen - 1 - rpos;
            memcpy(respOut + rpos, buf, n); rpos += n;
        }
    }
    if (respOut && respLen) respOut[rpos] = '\0';

    WinHttpCloseHandle(hReq);
    return (int)dwStatus;
}

// ---------------------------------------------------------------------------
// Send one job (with 429 back-off)
// ---------------------------------------------------------------------------

static const DWORD g_retryDelays[MAX_RETRIES] = { 1000, 2000, 4000 };

// Sleep that bails out early when g_bRunning is cleared, keeping the worker-join bounded.
static void InterruptibleSleep(DWORD ms)
{
    while (ms > 0 && g_bRunning)
    {
        DWORD step = ms > 200 ? 200 : ms;
        Sleep(step);
        ms -= step;
    }
}

// FIX [FlushBounded]: returns TRUE while the transport is usable (delivered, or a fast
// definitive HTTP error - server alive), FALSE on a transport-level failure or when a
// shutdown interrupts the retry/backoff. The exit-flush uses this to stop flushing
// against a dead endpoint instead of burning the full timeout budget per queued job.
static BOOL DoSend(const PushoverJob *job)
{
    char appTok[64], userKey[64], sound[32], device[64];
    EnterCriticalSection(&g_cs);
    strncpy(appTok,  g_szAppToken, sizeof(appTok) - 1);  appTok[sizeof(appTok) - 1]   = '\0';
    strncpy(userKey, g_szUserKey,  sizeof(userKey) - 1); userKey[sizeof(userKey) - 1] = '\0';
    strncpy(sound,   g_szSound,    sizeof(sound) - 1);   sound[sizeof(sound) - 1]     = '\0';
    strncpy(device,  g_szDevice,   sizeof(device) - 1);  device[sizeof(device) - 1]   = '\0';
    int  prio = g_iPriority;
    BOOL html = g_bHtml;
    LeaveCriticalSection(&g_cs);
    // FIX [PushoverPerFilter]: apply per-job overrides when not sentinel
    if (job->jobPriority >= -2 && job->jobPriority <= 1) prio = job->jobPriority;
    if (job->jobSound[0]) { strncpy(sound, job->jobSound, sizeof(sound) - 1); sound[sizeof(sound) - 1] = '\0'; }

    if (!appTok[0] || !userKey[0]) return TRUE;   // FIX [FlushBounded]: no-op, not a transport failure

    char title[512];
    title[0] = '\0';
    if (g_szTitle[0]) ResolveTemplate(title, sizeof(title), g_szTitle, job);
    if ((int)strlen(title) > PO_MAX_TITLE) title[PO_MAX_TITLE] = '\0';   // Pushover title cap

    // FIX [PoBodyTemplate]: body comes from its own template (default "{message}"); an empty
    // template falls back to the raw page text so the API never gets an empty 'message' field.
    char message[PO_MSG_LEN];
    if (g_szBody[0]) ResolveTemplate(message, sizeof(message), g_szBody, job);
    else { strncpy(message, job->szMessage, sizeof(message) - 1); message[sizeof(message) - 1] = '\0'; }
    if (!message[0]) { strncpy(message, job->szMessage, sizeof(message) - 1); message[sizeof(message) - 1] = '\0'; }
    // FIX [PoHtmlNewlineSpace]: no html newline conversion - Pushover honours "\n" as a line break in
    // both plain and html mode, so the text is sent verbatim and html=1 only adds <b>/<i> styling.
    if ((int)strlen(message) > PO_MAX_MSG) message[PO_MAX_MSG] = '\0';   // Pushover message cap

    static char body[8 * PO_MSG_LEN];
    int p = 0; BOOL first = TRUE;
    AppendField(body, &p, sizeof(body), &first, "token",   appTok);
    AppendField(body, &p, sizeof(body), &first, "user",    userKey);
    AppendField(body, &p, sizeof(body), &first, "message", message);
    if (title[0]) AppendField(body, &p, sizeof(body), &first, "title", title);
    {
        char pr[8]; sprintf(pr, "%d", prio);
        AppendField(body, &p, sizeof(body), &first, "priority", pr);
    }
    if (sound[0])  AppendField(body, &p, sizeof(body), &first, "sound",  sound);
    if (device[0]) AppendField(body, &p, sizeof(body), &first, "device", device);
    if (html)      AppendField(body, &p, sizeof(body), &first, "html",   "1");
    body[p] = '\0';

    PostStatus(PUS_SENDING, 0);

    char resp[2048];
    for (int attempt = 0; attempt < MAX_RETRIES; attempt++)
    {
        if (attempt > 0)
        {
            if (!g_bRunning)
            {
                // FIX [ShutdownLost]: this message is dropped here - say so in the log.
                WriteLog("LOST    capcode=%s shutdown during retry - message dropped", job->szCapcode);
                return FALSE;   // FIX [FlushBounded]: transport failed + shutdown in progress
            }
            PostStatus(PUS_RETRY, attempt);
            InterruptibleSleep(g_retryDelays[attempt - 1]);
        }

        // FIX [PushoverFirstConnectRetry]: EnsureConnection on EVERY attempt. The old pre-loop
        // early-return dropped the message on the first transient connect failure (a network blip
        // or resolver hiccup), unlike telegram/webhook which retry. A failed connect now falls
        // through to the retry loop and, after all attempts, the "all retries failed" path below.
        if (!EnsureConnection()) { CloseConnection(); continue; }

        int status = HttpPostForm(g_hConnect, body, p, resp, sizeof(resp));

        if (status >= 200 && status < 300)
        {
            WriteLog("SENT    capcode=%s -> %d OK", job->szCapcode, status);
            PostStatus(PUS_OK, status);
            return TRUE;
        }
        if (status == 429)
        {
            WriteLog("RATE    429 rate limited, backing off");
            CloseConnection();
            continue;
        }
        if (status != 0)
        {
            // 4xx (bad token/user/message) — server reachable but rejected; do not retry.
            WriteLog("ERROR   capcode=%s -> HTTP %d", job->szCapcode, status);
            PostStatus(PUS_ERROR, status);
            return TRUE;   // FIX [FlushBounded]: endpoint alive (fast definitive answer)
        }
        // transport error -> retry
        CloseConnection();
    }
    WriteLog("ERROR   capcode=%s all retries failed", job->szCapcode);
    PostStatus(PUS_ERROR, 0);
    return FALSE;   // FIX [FlushBounded]
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

static DWORD WINAPI WorkerThreadProc(LPVOID)
{
    while (g_bRunning)
    {
        WaitForSingleObject(g_hEvent, 200);
        if (!g_bRunning) break;
        for (;;)
        {
            PushoverJob job; BOOL have = FALSE;
            EnterCriticalSection(&g_cs);
            if (!QueueEmpty()) { job = g_queue[g_qHead]; g_qHead = (g_qHead + 1) % PO_QUEUE_SIZE; have = TRUE; }
            LeaveCriticalSection(&g_cs);
            if (!have) break;
            DoSend(&job);
        }
    }
    for (;;)
    {
        PushoverJob job; BOOL have = FALSE;
        EnterCriticalSection(&g_cs);
        if (!QueueEmpty()) { job = g_queue[g_qHead]; g_qHead = (g_qHead + 1) % PO_QUEUE_SIZE; have = TRUE; }
        LeaveCriticalSection(&g_cs);
        if (!have) break;
        // FIX [FlushBounded]: bound the exit flush - on the first transport failure, log
        // what remains as LOST and stop, instead of burning the full timeout budget per
        // queued job inside PushoverShutdown's INFINITE join on the GUI thread.
        if (!DoSend(&job))
        {
            int nLost;
            EnterCriticalSection(&g_cs);
            nLost = (g_qTail - g_qHead + PO_QUEUE_SIZE) % PO_QUEUE_SIZE;
            LeaveCriticalSection(&g_cs);
            WriteLog("LOST    shutdown flush aborted - %d message(s) dropped (transport failed)", nLost + 1);
            break;
        }
    }
    CloseSession();
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void PushoverInit(void)
{
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = TRUE; }

    PushoverShutdown();

    if (!Profile.pushoverEnabled || !Profile.szPushoverAppToken[0] || !Profile.szPushoverUserKey[0]) return;

    EnterCriticalSection(&g_cs);
    strncpy(g_szAppToken, Profile.szPushoverAppToken, sizeof(g_szAppToken) - 1); g_szAppToken[sizeof(g_szAppToken) - 1] = '\0';
    strncpy(g_szUserKey,  Profile.szPushoverUserKey,  sizeof(g_szUserKey) - 1);  g_szUserKey[sizeof(g_szUserKey) - 1]   = '\0';
    strncpy(g_szTitle,    Profile.szPushoverTitle,    sizeof(g_szTitle) - 1);    g_szTitle[sizeof(g_szTitle) - 1]       = '\0';
    strncpy(g_szBody,     Profile.szPushoverBody,     sizeof(g_szBody) - 1);     g_szBody[sizeof(g_szBody) - 1]         = '\0';   // FIX [PoBodyTemplate]
    strncpy(g_szSound,    Profile.szPushoverSound,    sizeof(g_szSound) - 1);    g_szSound[sizeof(g_szSound) - 1]       = '\0';
    strncpy(g_szDevice,   Profile.szPushoverDevice,   sizeof(g_szDevice) - 1);   g_szDevice[sizeof(g_szDevice) - 1]     = '\0';
    g_iPriority  = Profile.pushoverPriority;
    if (g_iPriority < -2) g_iPriority = -2;
    if (g_iPriority >  1) g_iPriority =  1;   // emergency priority 2 not offered in this phase
    g_bHtml      = Profile.pushoverHtml      ? TRUE : FALSE;
    g_bLogToFile = Profile.pushoverLogToFile ? TRUE : FALSE;
    LeaveCriticalSection(&g_cs);

    g_hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_hEvent) return;

    g_qHead = g_qTail = 0;
    ZeroMemory(g_groupAcc, sizeof(g_groupAcc));   // FIX [PoGroupBatch]
    g_bRunning = TRUE;
    g_hThread  = CreateThread(NULL, 0, WorkerThreadProc, NULL, 0, NULL);
    if (!g_hThread) { g_bRunning = FALSE; CloseHandle(g_hEvent); g_hEvent = NULL; }
}

void PushoverShutdown(void)
{
    if (!g_bRunning) return;
    g_bRunning = FALSE;
    if (g_hEvent) SetEvent(g_hEvent);
    if (g_hThread)
    {
        WaitForSingleObject(g_hThread, INFINITE);
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }
    // FIX [PoEventRace]: tear down the event and queue under g_cs so a decoder-thread EnqueueJob
    // can't be mid-SetEvent on this handle, nor tear the ring indices, while we close them.
    HANDLE ev = NULL;
    if (g_csInit) EnterCriticalSection(&g_cs);
    ev = g_hEvent; g_hEvent = NULL;
    g_qHead = g_qTail = 0;
    ZeroMemory(g_groupAcc, sizeof(g_groupAcc));   // FIX [PoGroupBatch]
    if (g_csInit) LeaveCriticalSection(&g_cs);
    if (ev) CloseHandle(ev);
}

void PushoverDestroy(void)
{
    PushoverShutdown();
    // FIX [PoCsTeardown]: do NOT DeleteCriticalSection here. PushoverNotify/FlushGroup test the
    // lock-free g_bRunning flag and THEN take g_cs; a decoder thread that passed the test before
    // shutdown cleared the flag could otherwise enter an already-deleted section (UB/crash). This
    // is only ever called at process exit (WM_DESTROY), so leaving g_cs intact and letting the OS
    // reclaim it on termination closes that window with no leak that outlives the process. g_csInit
    // stays TRUE so any late call still hits a valid section.
}

static void EnqueueJob(const PushoverJob *job)
{
    BOOL     bDropped = FALSE;
    unsigned nDropped = 0;
    EnterCriticalSection(&g_cs);
    if (QueueFull())
    {
        g_droppedJobs++;
        bDropped = TRUE;
        nDropped = g_droppedJobs;
    }
    else
    {
        g_queue[g_qTail] = *job;
        g_qTail = (g_qTail + 1) % PO_QUEUE_SIZE;
    }
    // FIX [PoEventRace]: signal WHILE holding the lock so a concurrent PushoverShutdown (GUI thread)
    // can't CloseHandle(g_hEvent) in the gap between LeaveCriticalSection and SetEvent. Shutdown now
    // also clears the handle under g_cs.
    if (g_hEvent) SetEvent(g_hEvent);
    LeaveCriticalSection(&g_cs);
    if (bDropped)
    {
        // FIX [QueueDropVisible]: log OUTSIDE g_cs (LogManager direct mode does synchronous
        // disk I/O and this runs on the GUI thread) and ALWAYS post the error status - with
        // LogToFile off the drop was previously completely invisible to the operator.
        WriteLog("DROP queue full - message discarded (total dropped=%u)", nDropped);
        PostStatus(PUS_ERROR, 0);
    }
}

void PushoverNotify(const char *capcode, const char *message, const char *label,
                    const char *szTime, const char *szDate,
                    const char *szMode, const char *szType, const char *szBitrate,
                    BOOL isGroup, int groupbit,
                    int jobPriority, const char *jobSound)  // FIX [PushoverPerFilter]: -9/""=use global
{
    if (!g_bRunning) return;

    // FIX [PoGroupBatch]: accumulate group-call subscribers per groupbit; one notification is
    // emitted from PushoverFlushGroup() instead of one per subscriber capcode.
    // FIX [GroupcallPerFilter]: aggregate each matched subscriber's per-filter priority/sound so a
    // monitor capcode that only appears inside a group call still drives the notification's priority
    // and sound. First non-empty sound wins.
    // FIX [GroupcallPrioMax]: the HIGHEST priority in the group must be heard on the phone, even when
    // other members have a lower or no override. Every subscriber therefore contributes either its
    // own override OR the global priority (when it defers, jobPriority==-9), and the most-urgent of
    // all those contributions wins. This stops a single low-priority member from dragging the whole
    // group below what a defer-to-global member would otherwise have triggered.
    if (isGroup && groupbit >= 0 && groupbit < PO_MAX_GROUPBITS)
    {
        EnterCriticalSection(&g_cs);
        PoGroupAcc *ga = &g_groupAcc[groupbit];
        if (!ga->active)
        {
            ZeroMemory(ga, sizeof(*ga));
            ga->active = TRUE;
            ga->accPriority = -9;   // FIX [GroupcallPerFilter]: -9 = no contribution folded in yet
            strncpy(ga->szMessage, message ? message : "", sizeof(ga->szMessage) - 1);
            strncpy(ga->szTime,    szTime  ? szTime  : "", sizeof(ga->szTime)    - 1);
            strncpy(ga->szDate,    szDate  ? szDate  : "", sizeof(ga->szDate)    - 1);
            strncpy(ga->szMode,    szMode  ? szMode  : "", sizeof(ga->szMode)    - 1);
            strncpy(ga->szType,    szType  ? szType  : "", sizeof(ga->szType)    - 1);
            strncpy(ga->szBitrate, szBitrate ? szBitrate : "", sizeof(ga->szBitrate) - 1);
        }
        AppendListItem(ga->szCapcodes, sizeof(ga->szCapcodes), capcode, ' ');
        AppendListItem(ga->szLabels,   sizeof(ga->szLabels),   label,   '\n');	// FIX [PoGroupNewline]: one label per line
        // FIX [GroupcallPrioMax]: fold this subscriber's contribution into the group aggregate. A
        // member with an explicit override (-2..1) contributes that value; a member that defers to
        // global (jobPriority==-9) contributes the global priority g_iPriority (already clamped to
        // -2..1 in PushoverInit). The most-urgent contribution wins, so the highest priority in the
        // group is always the one that fires the phone.
        {
            int effPrio = (jobPriority >= -2 && jobPriority <= 1) ? jobPriority : g_iPriority;
            if (ga->accPriority < -2 || effPrio > ga->accPriority) ga->accPriority = effPrio; // highest wins
        }
        if (jobSound && jobSound[0] && !ga->accSound[0])
        {
            strncpy(ga->accSound, jobSound, sizeof(ga->accSound) - 1);
            ga->accSound[sizeof(ga->accSound) - 1] = '\0';
        }
        LeaveCriticalSection(&g_cs);
        return;
    }

    PushoverJob job;
    ZeroMemory(&job, sizeof(job));
    strncpy(job.szCapcode, capcode ? capcode : "", sizeof(job.szCapcode) - 1);
    strncpy(job.szMessage, message ? message : "", sizeof(job.szMessage) - 1);
    strncpy(job.szLabel,   label   ? label   : "", sizeof(job.szLabel)   - 1);
    strncpy(job.szTime,    szTime  ? szTime  : "", sizeof(job.szTime)    - 1);
    strncpy(job.szDate,    szDate  ? szDate  : "", sizeof(job.szDate)    - 1);
    strncpy(job.szMode,    szMode  ? szMode  : "", sizeof(job.szMode)    - 1);
    strncpy(job.szType,    szType  ? szType  : "", sizeof(job.szType)    - 1);
    strncpy(job.szBitrate, szBitrate ? szBitrate : "", sizeof(job.szBitrate) - 1);
    job.jobPriority = jobPriority;   // FIX [PushoverPerFilter]
    if (jobSound) { strncpy(job.jobSound, jobSound, sizeof(job.jobSound) - 1); job.jobSound[sizeof(job.jobSound) - 1] = '\0'; }

    EnqueueJob(&job);
}

// FIX [PoGroupBatch]: flush one accumulated group call as a single Pushover notification.
void PushoverFlushGroup(int groupbit)
{
    if (!g_bRunning) return;
    if (groupbit < 0 || groupbit >= PO_MAX_GROUPBITS) return;

    PushoverJob job;
    BOOL have = FALSE;
    EnterCriticalSection(&g_cs);
    PoGroupAcc *ga = &g_groupAcc[groupbit];
    if (ga->active)
    {
        ZeroMemory(&job, sizeof(job));
        // FIX [GroupcallPrioMax]: apply the aggregated priority. accPriority is the most-urgent of all
        // members' contributions (each member's override, or the global priority when it deferred), so
        // it is a concrete -2..1 value once any subscriber was folded in. accSound: "" = use global.
        job.jobPriority = ga->accPriority;
        if (ga->accSound[0]) { strncpy(job.jobSound, ga->accSound, sizeof(job.jobSound) - 1); job.jobSound[sizeof(job.jobSound) - 1] = '\0'; }
        strncpy(job.szCapcode, ga->szCapcodes, sizeof(job.szCapcode) - 1);
        strncpy(job.szLabel,   ga->szLabels,   sizeof(job.szLabel)   - 1);
        strncpy(job.szMessage, ga->szMessage,  sizeof(job.szMessage) - 1);
        strncpy(job.szTime,    ga->szTime,     sizeof(job.szTime)    - 1);
        strncpy(job.szDate,    ga->szDate,     sizeof(job.szDate)    - 1);
        strncpy(job.szMode,    ga->szMode,     sizeof(job.szMode)    - 1);
        strncpy(job.szType,    ga->szType,     sizeof(job.szType)    - 1);
        strncpy(job.szBitrate, ga->szBitrate,  sizeof(job.szBitrate) - 1);
        ZeroMemory(ga, sizeof(*ga));
        have = TRUE;
    }
    LeaveCriticalSection(&g_cs);

    if (have) EnqueueJob(&job);
}

void PushoverSetStatusWnd(HWND hWnd)
{
    if (!g_csInit) { g_hStatusWnd = hWnd; return; }
    EnterCriticalSection(&g_cs);
    g_hStatusWnd = hWnd;
    LeaveCriticalSection(&g_cs);
}

// ---------------------------------------------------------------------------
// Synchronous test send for the config dialog (GUI thread)
// ---------------------------------------------------------------------------

// FIX [PoTestPreview]: fill a job with representative sample values for the Test-button preview.
static void FillSampleJob(PushoverJob *job)
{
    ZeroMemory(job, sizeof(*job));
    SYSTEMTIME st; GetLocalTime(&st);
    _snprintf(job->szTime, sizeof(job->szTime) - 1, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    _snprintf(job->szDate, sizeof(job->szDate) - 1, "%02d-%02d-%04d", st.wDay, st.wMonth, st.wYear);
    strcpy(job->szCapcode, "1234567 1234568 1234569");
    strcpy(job->szMessage, "PDW test page - sample alert text");
    strcpy(job->szLabel,   "Test Service Alpha\nTest Service Bravo\nTest Service Charlie");
    strcpy(job->szMode,    "FLEX");
    strcpy(job->szType,    "ALPHA");
    strcpy(job->szBitrate, "1600");
}

// FIX [PoTestPreview]: the Test button renders a sample page through the supplied Title/Body templates
// (and html flag), so the test previews the real formatting just like Telegram.
BOOL PushoverTestSend(const char *appToken, const char *userKey, const char *title, const char *body_tmpl,
                      BOOL html, int priority, const char *sound, char *errOut, int errLen)  // FIX [PushoverPerFilter]
{
    if (errOut && errLen) errOut[0] = '\0';
    if (!appToken || !appToken[0]) { if (errOut) _snprintf(errOut, errLen - 1, "No app token set"); return FALSE; }
    if (!userKey  || !userKey[0])  { if (errOut) _snprintf(errOut, errLen - 1, "No user/group key set"); return FALSE; }

    PushoverJob job;
    FillSampleJob(&job);

    char titleBuf[512];
    titleBuf[0] = '\0';
    if (title && title[0]) ResolveTemplate(titleBuf, sizeof(titleBuf), title, &job);
    if ((int)strlen(titleBuf) > PO_MAX_TITLE) titleBuf[PO_MAX_TITLE] = '\0';

    char msgBuf[PO_MSG_LEN];
    if (body_tmpl && body_tmpl[0]) ResolveTemplate(msgBuf, sizeof(msgBuf), body_tmpl, &job);
    else { strncpy(msgBuf, job.szMessage, sizeof(msgBuf) - 1); msgBuf[sizeof(msgBuf) - 1] = '\0'; }
    if (!msgBuf[0]) { strncpy(msgBuf, job.szMessage, sizeof(msgBuf) - 1); msgBuf[sizeof(msgBuf) - 1] = '\0'; }
    // FIX [PoHtmlNewlineSpace]: no html newline conversion (Pushover honours "\n" in html mode too).
    if ((int)strlen(msgBuf) > PO_MAX_MSG) msgBuf[PO_MAX_MSG] = '\0';

    HINTERNET hS = WinHttpOpen(L"PDW-Pushover/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) { if (errOut) _snprintf(errOut, errLen - 1, "WinHTTP init failed"); return FALSE; }
    WinHttpSetTimeouts(hS, 10000, 10000, 10000, 10000);
    DWORD dwProto = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(hS, WINHTTP_OPTION_SECURE_PROTOCOLS, &dwProto, sizeof(dwProto));

    HINTERNET hC = WinHttpConnect(hS, PO_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hC) { WinHttpCloseHandle(hS); if (errOut) _snprintf(errOut, errLen - 1, "Connect failed"); return FALSE; }

    static char body[8 * PO_MSG_LEN]; int p = 0; BOOL first = TRUE;
    AppendField(body, &p, sizeof(body), &first, "token",   appToken);
    AppendField(body, &p, sizeof(body), &first, "user",    userKey);
    AppendField(body, &p, sizeof(body), &first, "message", msgBuf);
    if (titleBuf[0]) AppendField(body, &p, sizeof(body), &first, "title", titleBuf);
    // FIX [PushoverPerFilter]: include priority/sound so the Test send reflects the configured values
    {
        char pr[8]; sprintf(pr, "%d", priority);
        AppendField(body, &p, sizeof(body), &first, "priority", pr);
    }
    if (sound && sound[0]) AppendField(body, &p, sizeof(body), &first, "sound", sound);
    if (html)        AppendField(body, &p, sizeof(body), &first, "html",  "1");
    body[p] = '\0';

    char resp[2048];
    int st = HttpPostForm(hC, body, p, resp, sizeof(resp));

    WinHttpCloseHandle(hC);
    WinHttpCloseHandle(hS);

    if (st >= 200 && st < 300) return TRUE;
    if (errOut) _snprintf(errOut, errLen - 1, "Failed (HTTP %d)", st);
    return FALSE;
}
