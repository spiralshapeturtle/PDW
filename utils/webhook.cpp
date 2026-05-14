/*
** webhook.cpp -- HTTP/HTTPS webhook sender for PDW
**
** Threading model:
**   Main/decoder thread  -> WebhookNotify / WebhookFlushGroup  -> g_queue (CRITICAL_SECTION + event)
**   Worker thread        -> dequeues jobs, sends via WinHTTP with 3-attempt exponential backoff
**
** FLEX group batching:
**   ConvertGroupcall calls ShowMessage multiple times (once per subscriber capcode) with
**   iConvertingGroupcall > 0, then resets it to 0 and calls WebhookFlushGroup().
**   WebhookNotify accumulates capcodes in g_groupAcc[groupbit]; FlushGroup queues the batch.
**
** WinHTTP is used for both HTTP and HTTPS.  The HINTERNET connection handle is kept alive
** across requests (TCP keep-alive).  Self-signed cert option sets SECURITY_FLAG_IGNORE_*.
*/

#ifndef STRICT
#define STRICT 1
#endif

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>

#include "..\headers\pdw.h"
#include "..\headers\initapp.h"
#include "webhook.h"

extern TCHAR szPath[];          // from Initapp.cpp — PDW executable directory

#pragma comment(lib, "winhttp.lib")

// ---------------------------------------------------------------------------
// Configuration (read from Profile at WebhookInit time)
// ---------------------------------------------------------------------------

static char  g_szURL[WEBHOOK_URL_LEN]  = "";
static BOOL  g_bTrustSelfSigned        = FALSE;
static BOOL  g_bLogToFile              = FALSE;
static BOOL  g_bPadCapcodes            = FALSE;
static BOOL  g_bPagermonFormat         = FALSE;
static int   g_iWebhookFields          = 0x7F;  // all 7 optional fields on by default

// Field bitmask positions (matches Profile.webhookFields)
#define WHF_LABEL      (1<<0)   // label – CSV mode: groups batched, labels comma-joined
#define WHF_TIME       (1<<1)
#define WHF_DATE       (1<<2)
#define WHF_TIMESTAMP  (1<<3)
#define WHF_MODE       (1<<4)
#define WHF_TYPE       (1<<5)
#define WHF_BITRATE    (1<<6)
#define WHF_LABEL_PERCAP  (1<<7) // label – per-capcode mode: groups sent individually
#define WHF_LABEL_ARRAY   (1<<8) // label – subscribers array: groups as [{address,label},...]

// ---------------------------------------------------------------------------
// Job queue  (ring buffer, capacity = WEBHOOK_QUEUE_SIZE)
// ---------------------------------------------------------------------------

#define WEBHOOK_QUEUE_SIZE       64
#define WEBHOOK_ADDR_LEN         512
#define WEBHOOK_MSG_LEN          (MAX_STR_LEN + 64)
#define WEBHOOK_SUBSCRIBERS_LEN  2048   // pre-built JSON array for WHF_LABEL_ARRAY

typedef struct {
    char     szAddress     [WEBHOOK_ADDR_LEN];
    char     szMessage     [WEBHOOK_MSG_LEN];
    char     szLabel       [512];
    char     szTime        [32];
    char     szDate        [32];
    char     szMode        [32];
    char     szType        [32];
    char     szBitrate     [32];
    char     szSubscribers [WEBHOOK_SUBSCRIBERS_LEN]; // WHF_LABEL_ARRAY only
    LONGLONG lTimestamp;
} WebhookJob;

static WebhookJob g_queue[WEBHOOK_QUEUE_SIZE];
static int        g_qHead = 0;
static int        g_qTail = 0;

static BOOL inline QueueFull(void)  { return ((g_qTail + 1) % WEBHOOK_QUEUE_SIZE) == g_qHead; }
static BOOL inline QueueEmpty(void) { return g_qHead == g_qTail; }

// ---------------------------------------------------------------------------
// Per-groupbit accumulator for FLEX group batching
// ---------------------------------------------------------------------------

#define MAX_GROUPBITS 17

typedef struct {
    BOOL     active;
    char     szMessage     [WEBHOOK_MSG_LEN];
    char     szAddresses   [WEBHOOK_ADDR_LEN];
    char     szLabels      [512];
    char     szSubscribers [WEBHOOK_SUBSCRIBERS_LEN]; // WHF_LABEL_ARRAY only
    char     szTime        [32];
    char     szDate        [32];
    char     szMode        [32];
    char     szType        [32];
    char     szBitrate     [32];
    LONGLONG lTimestamp;
} GroupAcc;

static GroupAcc g_groupAcc[MAX_GROUPBITS];

// ---------------------------------------------------------------------------
// Thread / synchronisation
// ---------------------------------------------------------------------------

static HANDLE g_hThread  = NULL;
static HANDLE g_hEvent   = NULL;   // auto-reset, signals worker
static BOOL   g_bRunning = FALSE;

static CRITICAL_SECTION g_cs;      // protects queue, groupAcc, g_hStatusWnd

// ---------------------------------------------------------------------------
// Status notification
// ---------------------------------------------------------------------------

static HWND g_hStatusWnd = NULL;   // protected by g_cs

static void PostStatus(int status, LPARAM lp)
{
    HWND hWnd;
    EnterCriticalSection(&g_cs);
    hWnd = g_hStatusWnd;
    LeaveCriticalSection(&g_cs);
    if (hWnd) PostMessage(hWnd, WM_WEBHOOK_STATUS, (WPARAM)status, lp);
}

// ---------------------------------------------------------------------------
// Log file  (separate critical section)
// ---------------------------------------------------------------------------

static CRITICAL_SECTION g_logCs;
static BOOL g_logCsInit = FALSE;

static void WriteLog(const char *fmt, ...)
{
    if (!g_bLogToFile) return;

    char szLine[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(szLine, sizeof(szLine) - 1, fmt, ap);
    szLine[sizeof(szLine) - 1] = '\0';
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char szTimestamp[32];
    sprintf(szTimestamp, "%04d-%02d-%02d %02d:%02d:%02d ",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    const char *logDir = (Profile.LogfilePath[0]) ? Profile.LogfilePath : (const char *)szPath;
    char szFile[MAX_PATH];
    sprintf(szFile, "%s\\pdw_webhook.log", logDir);

    EnterCriticalSection(&g_logCs);
    FILE *fp = fopen(szFile, "a");
    if (fp)
    {
        fprintf(fp, "%s%s\n", szTimestamp, szLine);
        fclose(fp);
    }
    LeaveCriticalSection(&g_logCs);
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

static void AppendEscaped(char *dst, int *pos, int maxLen, const char *src)
{
    for (int i = 0; src[i] != '\0' && *pos < maxLen - 8; i++)
    {
        unsigned char c = (unsigned char)src[i];
        if      (c == '"')  { dst[(*pos)++] = '\\'; dst[(*pos)++] = '"';  }
        else if (c == '\\') { dst[(*pos)++] = '\\'; dst[(*pos)++] = '\\'; }
        else if (c == '\r') { dst[(*pos)++] = '\\'; dst[(*pos)++] = 'r';  }
        else if (c == '\n') { dst[(*pos)++] = '\\'; dst[(*pos)++] = 'n';  }
        else if (c == '\t') { dst[(*pos)++] = '\\'; dst[(*pos)++] = 't';  }
        else if (c < 32)
        {
            char esc[8];
            sprintf(esc, "\\u%04X", c);
            for (int k = 0; esc[k] && *pos < maxLen - 2; k++) dst[(*pos)++] = esc[k];
        }
        else dst[(*pos)++] = (char)c;
    }
}

// Append ,"key":"escaped-val" to dst.
static void AppendJSONStr(char *dst, int *pos, int maxLen, const char *key, const char *val)
{
    const char *pfx = ",\"";
    for (int i = 0; pfx[i] && *pos < maxLen - 2; i++) dst[(*pos)++] = pfx[i];
    for (int i = 0; key[i] && *pos < maxLen - 2; i++) dst[(*pos)++] = key[i];
    const char *mid = "\":\"";
    for (int i = 0; mid[i] && *pos < maxLen - 2; i++) dst[(*pos)++] = mid[i];
    AppendEscaped(dst, pos, maxLen, val);
    if (*pos < maxLen - 2) dst[(*pos)++] = '"';
}

// Append ,"key":number to dst.
static void AppendJSONNum(char *dst, int *pos, int maxLen, const char *key, LONGLONG val)
{
    const char *pfx = ",\"";
    for (int i = 0; pfx[i] && *pos < maxLen - 2; i++) dst[(*pos)++] = pfx[i];
    for (int i = 0; key[i] && *pos < maxLen - 2; i++) dst[(*pos)++] = key[i];
    const char *sep = "\":";
    for (int i = 0; sep[i] && *pos < maxLen - 2; i++) dst[(*pos)++] = sep[i];
    char num[24];
    sprintf(num, "%lld", val);
    for (int i = 0; num[i] && *pos < maxLen - 2; i++) dst[(*pos)++] = num[i];
}

// Append ,"key":raw_json_value  (raw is already valid JSON — no quoting).
static void AppendJSONRaw(char *dst, int *pos, int maxLen, const char *key, const char *raw)
{
    const char *pfx = ",\"";
    for (int i = 0; pfx[i] && *pos < maxLen-2; i++) dst[(*pos)++] = pfx[i];
    for (int i = 0; key[i] && *pos < maxLen-2; i++) dst[(*pos)++] = key[i];
    if (*pos < maxLen-2) dst[(*pos)++] = '"';
    if (*pos < maxLen-2) dst[(*pos)++] = ':';
    for (int i = 0; raw[i] && *pos < maxLen-2; i++) dst[(*pos)++] = raw[i];
}

// Append one {"address":"...","label":"..."} entry into a subscribers array buffer.
// isFirst=TRUE omits the leading comma.  Applies capcode padding when g_bPadCapcodes.
static void AppendSubscriberEntry(char *dst, int *pos, int maxLen,
                                   const char *capcode, const char *label, BOOL isFirst)
{
    if (!isFirst && *pos < maxLen-2) dst[(*pos)++] = ',';

    const char *open = "{\"address\":\"";
    for (int i = 0; open[i] && *pos < maxLen-2; i++) dst[(*pos)++] = open[i];

    if (g_bPadCapcodes)
    {
        int len = (int)strlen(capcode);
        int nz  = (len < 9) ? (9 - len) : 0;
        for (int z = 0; z < nz && *pos < maxLen-2; z++) dst[(*pos)++] = '0';
    }
    for (int i = 0; capcode[i] && *pos < maxLen-2; i++) dst[(*pos)++] = capcode[i];

    const char *mid = "\",\"label\":\"";
    for (int i = 0; mid[i] && *pos < maxLen-2; i++) dst[(*pos)++] = mid[i];
    AppendEscaped(dst, pos, maxLen, label ? label : "");

    const char *close = "\"}";
    for (int i = 0; close[i] && *pos < maxLen-2; i++) dst[(*pos)++] = close[i];
}

// PDW-native format: {"payload":"MSG","data":{"new_state":{"state":"MSG","attributes":{...}}}}
static void BuildJSON(char *dst, int maxLen, const char *address, const WebhookJob *job)
{
    int p = 0;
    const char *pre = "{\"payload\":\"";
    for (int i = 0; pre[i] && p < maxLen - 2; i++) dst[p++] = pre[i];
    AppendEscaped(dst, &p, maxLen, job->szMessage);

    const char *mid1 = "\",\"data\":{\"new_state\":{\"state\":\"";
    for (int i = 0; mid1[i] && p < maxLen - 2; i++) dst[p++] = mid1[i];
    AppendEscaped(dst, &p, maxLen, job->szMessage);

    if (g_iWebhookFields & WHF_LABEL_ARRAY)
    {
        const char *mid2a = "\",\"attributes\":{\"subscribers\":";
        for (int i = 0; mid2a[i] && p < maxLen - 2; i++) dst[p++] = mid2a[i];
        for (int i = 0; job->szSubscribers[i] && p < maxLen - 2; i++) dst[p++] = job->szSubscribers[i];
    }
    else
    {
        const char *mid2b = "\",\"attributes\":{\"address\":\"";
        for (int i = 0; mid2b[i] && p < maxLen - 2; i++) dst[p++] = mid2b[i];
        AppendEscaped(dst, &p, maxLen, address);
        if (p < maxLen - 2) dst[p++] = '"';
        if (g_iWebhookFields & (WHF_LABEL | WHF_LABEL_PERCAP)) AppendJSONStr(dst, &p, maxLen, "label", job->szLabel);
    }

    if (g_iWebhookFields & WHF_TIME)      AppendJSONStr(dst, &p, maxLen, "time",      job->szTime);
    if (g_iWebhookFields & WHF_DATE)      AppendJSONStr(dst, &p, maxLen, "date",      job->szDate);
    if (g_iWebhookFields & WHF_TIMESTAMP) AppendJSONNum(dst, &p, maxLen, "timestamp", job->lTimestamp);
    if (g_iWebhookFields & WHF_MODE)      AppendJSONStr(dst, &p, maxLen, "mode",      job->szMode);
    if (g_iWebhookFields & WHF_TYPE)      AppendJSONStr(dst, &p, maxLen, "type",      job->szType);
    if (g_iWebhookFields & WHF_BITRATE)   AppendJSONStr(dst, &p, maxLen, "bitrate",   job->szBitrate);

    const char *suf = "}}}}";
    for (int i = 0; suf[i] && p < maxLen - 2; i++) dst[p++] = suf[i];
    dst[p] = '\0';
}

// Flat / Node-RED format: {"message":"...","address":"...",...}
// No outer wrapper key — HA webhook node maps POST body directly to msg.payload.
static void BuildJSONFlat(char *dst, int maxLen, const char *address, const WebhookJob *job)
{
    int p = 0;
    const char *pre = "{\"message\":\"";
    for (int i = 0; pre[i] && p < maxLen - 2; i++) dst[p++] = pre[i];
    AppendEscaped(dst, &p, maxLen, job->szMessage);

    if (g_iWebhookFields & WHF_LABEL_ARRAY)
    {
        if (p < maxLen - 2) dst[p++] = '"';
        AppendJSONRaw(dst, &p, maxLen, "subscribers", job->szSubscribers);
    }
    else
    {
        const char *mid = "\",\"address\":\"";
        for (int i = 0; mid[i] && p < maxLen - 2; i++) dst[p++] = mid[i];
        AppendEscaped(dst, &p, maxLen, address);
        if (p < maxLen - 2) dst[p++] = '"';
        if (g_iWebhookFields & (WHF_LABEL | WHF_LABEL_PERCAP)) AppendJSONStr(dst, &p, maxLen, "label", job->szLabel);
    }

    if (g_iWebhookFields & WHF_TIME)      AppendJSONStr(dst, &p, maxLen, "time",      job->szTime);
    if (g_iWebhookFields & WHF_DATE)      AppendJSONStr(dst, &p, maxLen, "date",      job->szDate);
    if (g_iWebhookFields & WHF_TIMESTAMP) AppendJSONNum(dst, &p, maxLen, "timestamp", job->lTimestamp);
    if (g_iWebhookFields & WHF_MODE)      AppendJSONStr(dst, &p, maxLen, "mode",      job->szMode);
    if (g_iWebhookFields & WHF_TYPE)      AppendJSONStr(dst, &p, maxLen, "type",      job->szType);
    if (g_iWebhookFields & WHF_BITRATE)   AppendJSONStr(dst, &p, maxLen, "bitrate",   job->szBitrate);

    if (p < maxLen - 2) dst[p++] = '}';
    dst[p] = '\0';
}

// Pad each space-separated token in src to 9 digits with leading zeros.
static void PadAddresses(char *dst, int dstLen, const char *src)
{
    int pos = 0;
    BOOL first = TRUE;
    while (*src && pos < dstLen - 1)
    {
        while (*src == ' ') src++;
        if (!*src) break;

        char token[32];
        int  tLen = 0;
        while (*src && *src != ' ' && tLen < 31) token[tLen++] = *src++;
        token[tLen] = '\0';

        if (!first && pos < dstLen - 1) dst[pos++] = ' ';
        first = FALSE;

        int pad = (tLen < 9) ? (9 - tLen) : 0;
        for (int i = 0; i < pad && pos < dstLen - 1; i++) dst[pos++] = '0';
        for (int i = 0; i < tLen && pos < dstLen - 1; i++) dst[pos++] = token[i];
    }
    dst[pos] = '\0';
}

// Append label to a comma-separated label accumulator; skip if identical to last.
static void AppendLabel(char *dst, int dstLen, const char *label)
{
    if (!label || !label[0]) return;
    int cur = (int)strlen(dst);
    int lab = (int)strlen(label);
    // Already ends with this label?
    if (cur >= lab && strcmp(dst + cur - lab, label) == 0) return;
    if (cur == 0)
        strncpy(dst, label, dstLen - 1);
    else if (cur + 2 + lab < dstLen - 1)
    {
        strcat(dst, ", ");
        strcat(dst, label);
    }
}

// Unix epoch seconds via Windows FILETIME.
static LONGLONG GetUnixTimestamp(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart  = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    // 100-ns intervals since 1601-01-01; subtract offset to 1970-01-01
    return (LONGLONG)((uli.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

// ---------------------------------------------------------------------------
// URL parsing  (WinHTTP-only, via WinHttpCrackUrl)
// ---------------------------------------------------------------------------

typedef struct {
    WCHAR szScheme  [16];
    WCHAR szHost    [512];
    WCHAR szPath    [1024];
    int   nPort;
    BOOL  bHttps;
} ParsedURL;

static BOOL ParseURL(const char *url, ParsedURL *out)
{
    WCHAR wszURL[WEBHOOK_URL_LEN];
    if (!MultiByteToWideChar(CP_ACP, 0, url, -1, wszURL, WEBHOOK_URL_LEN)) return FALSE;

    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize     = sizeof(uc);
    uc.lpszScheme       = out->szScheme;
    uc.dwSchemeLength   = sizeof(out->szScheme)  / sizeof(WCHAR);
    uc.lpszHostName     = out->szHost;
    uc.dwHostNameLength = sizeof(out->szHost)    / sizeof(WCHAR);
    uc.lpszUrlPath      = out->szPath;
    uc.dwUrlPathLength  = sizeof(out->szPath)    / sizeof(WCHAR);

    if (!WinHttpCrackUrl(wszURL, 0, 0, &uc)) return FALSE;

    out->nPort  = uc.nPort;
    out->bHttps = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return TRUE;
}

// ---------------------------------------------------------------------------
// WinHTTP persistent connection state (worker thread only — no locking needed)
// ---------------------------------------------------------------------------

static HINTERNET g_hSession  = NULL;
static HINTERNET g_hConnect  = NULL;
static WCHAR     g_wConnHost [512] = L"";
static int       g_nConnPort       = 0;
static BOOL      g_bConnHttps      = FALSE;

static void CloseConnection(void)
{
    if (g_hConnect) { WinHttpCloseHandle(g_hConnect); g_hConnect = NULL; }
}

static void CloseSession(void)
{
    CloseConnection();
    if (g_hSession) { WinHttpCloseHandle(g_hSession); g_hSession = NULL; }
}

static BOOL EnsureSession(void)
{
    if (g_hSession) return TRUE;
    g_hSession = WinHttpOpen(L"PDW-Webhook/1.0",
                             WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS,
                             0);
    if (!g_hSession) return FALSE;

    // Enable TLS 1.0 through 1.3 so self-hosted servers with older configs work.
    DWORD dwProto = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1
                  | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1
                  | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(g_hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &dwProto, sizeof(dwProto));

    return TRUE;
}

static BOOL EnsureConnection(const ParsedURL *pu)
{
    if (g_hConnect &&
        wcscmp(g_wConnHost, pu->szHost) == 0 &&
        g_nConnPort  == pu->nPort &&
        g_bConnHttps == pu->bHttps)
    {
        return TRUE;   // reuse existing connection
    }

    CloseConnection();
    if (!EnsureSession()) return FALSE;

    g_hConnect = WinHttpConnect(g_hSession, pu->szHost, (INTERNET_PORT)pu->nPort, 0);
    if (!g_hConnect) return FALSE;

    wcscpy(g_wConnHost, pu->szHost);
    g_nConnPort  = pu->nPort;
    g_bConnHttps = pu->bHttps;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Single HTTP POST attempt — returns HTTP status code, or 0 on transport error
// ---------------------------------------------------------------------------

static int TrySend(const ParsedURL *pu, const char *body, int bodyLen)
{
    if (!EnsureConnection(pu))
    {
        CloseConnection();
        return 0;
    }

    DWORD dwFlags = pu->bHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(g_hConnect,
                                        L"POST",
                                        pu->szPath,
                                        NULL,
                                        WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        dwFlags);
    if (!hReq) { CloseConnection(); return 0; }

    // Self-signed certificate support
    if (pu->bHttps && g_bTrustSelfSigned)
    {
        DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA
                       | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
                       | SECURITY_FLAG_IGNORE_CERT_CN_INVALID
                       | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
    }

    // Keep-alive header
    static const WCHAR *szHeaders = L"Content-Type: application/json\r\nConnection: keep-alive\r\n";

    BOOL bOK = WinHttpSendRequest(hReq, szHeaders, (DWORD)-1L,
                                  (LPVOID)body, (DWORD)bodyLen, (DWORD)bodyLen, 0);

    if (!bOK) { WinHttpCloseHandle(hReq); CloseConnection(); return 0; }

    bOK = WinHttpReceiveResponse(hReq, NULL);
    if (!bOK) { WinHttpCloseHandle(hReq); CloseConnection(); return 0; }

    DWORD dwStatus = 0;
    DWORD dwSize   = sizeof(dwStatus);
    WinHttpQueryHeaders(hReq,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        NULL, &dwStatus, &dwSize, NULL);

    // Drain the response body so keep-alive works
    DWORD dwRead = 0;
    char drain[256];
    while (WinHttpReadData(hReq, drain, sizeof(drain), &dwRead) && dwRead > 0) {}

    WinHttpCloseHandle(hReq);
    return (int)dwStatus;
}

// ---------------------------------------------------------------------------
// Send one job with retries
// ---------------------------------------------------------------------------

#define MAX_RETRIES     3

static const DWORD g_retryDelays[MAX_RETRIES] = { 1000, 2000, 4000 };

static void DoSend(const WebhookJob *job)
{
    // Apply optional capcode padding
    char szAddress[WEBHOOK_ADDR_LEN];
    if (g_bPadCapcodes)
        PadAddresses(szAddress, sizeof(szAddress), job->szAddress);
    else
    {
        strncpy(szAddress, job->szAddress, sizeof(szAddress) - 1);
        szAddress[sizeof(szAddress) - 1] = '\0';
    }

    // Build JSON in the configured format
    char jsonBody[WEBHOOK_MSG_LEN + WEBHOOK_ADDR_LEN + 2048];
    if (g_bPagermonFormat)
        BuildJSONFlat(jsonBody, sizeof(jsonBody), szAddress, job);
    else
        BuildJSON(jsonBody, sizeof(jsonBody), szAddress, job);
    int bodyLen = (int)strlen(jsonBody);

    // Snapshot URL + settings (may change while running, take copy at send time)
    char szURL[WEBHOOK_URL_LEN];
    EnterCriticalSection(&g_cs);
    strncpy(szURL, g_szURL, sizeof(szURL) - 1);
    szURL[sizeof(szURL) - 1] = '\0';
    LeaveCriticalSection(&g_cs);

    ParsedURL pu;
    if (!ParseURL(szURL, &pu))
    {
        WriteLog("ERROR   URL parse failed: %s", szURL);
        PostStatus(WHS_ERROR, 0);
        return;
    }

    PostStatus(WHS_SENDING, 0);

    int httpStatus = 0;
    for (int attempt = 0; attempt < MAX_RETRIES; attempt++)
    {
        if (attempt > 0)
        {
            PostStatus(WHS_RETRY, attempt);
            WriteLog("RETRY   %d/%d", attempt, MAX_RETRIES);
            Sleep(g_retryDelays[attempt - 1]);
        }

        httpStatus = TrySend(&pu, jsonBody, bodyLen);

        if (httpStatus >= 200 && httpStatus < 300)
        {
            char szURLshort[80];
            strncpy(szURLshort, szURL, 79); szURLshort[79] = '\0';
            WriteLog("SENT    POST %s -> %d OK", szURLshort, httpStatus);
            PostStatus(WHS_OK, httpStatus);
            return;
        }

        if (httpStatus != 0)
        {
            // HTTP error (e.g. 4xx/5xx) — server reachable but rejected; no point retrying
            char szURLshort[80];
            strncpy(szURLshort, szURL, 79); szURLshort[79] = '\0';
            WriteLog("ERROR   POST %s -> HTTP %d", szURLshort, httpStatus);
            PostStatus(WHS_ERROR, httpStatus);
            return;
        }
        // httpStatus == 0: transport error, retry
    }

    WriteLog("ERROR   all retries failed (transport error)");
    PostStatus(WHS_ERROR, 0);
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

static DWORD WINAPI WorkerThreadProc(LPVOID)
{
    while (g_bRunning)
    {
        DWORD dwWait = WaitForSingleObject(g_hEvent, 200);
        (void)dwWait;

        if (!g_bRunning) break;

        // Drain the queue
        while (TRUE)
        {
            WebhookJob job;
            BOOL bHaveJob = FALSE;

            EnterCriticalSection(&g_cs);
            if (!QueueEmpty())
            {
                job = g_queue[g_qHead];
                g_qHead = (g_qHead + 1) % WEBHOOK_QUEUE_SIZE;
                bHaveJob = TRUE;
            }
            LeaveCriticalSection(&g_cs);

            if (!bHaveJob) break;
            DoSend(&job);
        }
    }

    // Flush on exit: process any remaining jobs
    while (!QueueEmpty())
    {
        WebhookJob job = g_queue[g_qHead];
        g_qHead = (g_qHead + 1) % WEBHOOK_QUEUE_SIZE;
        DoSend(&job);
    }

    CloseSession();
    return 0;
}

// ---------------------------------------------------------------------------
// Queue helpers (call with g_cs held)
// ---------------------------------------------------------------------------

static void EnqueueLocked(const WebhookJob *job)
{
    if (QueueFull()) return;
    g_queue[g_qTail] = *job;
    g_qTail = (g_qTail + 1) % WEBHOOK_QUEUE_SIZE;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void WebhookInit(void)
{
    // Always initialize synchronisation objects first (WebhookSetStatusWnd
    // uses g_cs even when the thread is not running, e.g. from the dialog).
    static BOOL s_csInit = FALSE;
    if (!s_csInit)
    {
        InitializeCriticalSection(&g_cs);
        InitializeCriticalSection(&g_logCs);
        g_logCsInit = TRUE;
        s_csInit = TRUE;
    }

    WebhookShutdown();   // stop existing thread cleanly first

    if (!Profile.webhookEnabled || !Profile.szWebhookURL[0]) return;

    // Copy config from Profile
    strncpy(g_szURL, Profile.szWebhookURL, sizeof(g_szURL) - 1);
    g_szURL[sizeof(g_szURL) - 1] = '\0';
    g_bTrustSelfSigned = Profile.webhookTrustSelfSigned ? TRUE : FALSE;
    g_bLogToFile       = Profile.webhookLogToFile       ? TRUE : FALSE;
    g_bPadCapcodes     = Profile.webhookPadCapcodes     ? TRUE : FALSE;
    g_bPagermonFormat  = Profile.webhookPagermonFormat  ? TRUE : FALSE;
    g_iWebhookFields   = Profile.webhookFields;

    g_hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_hEvent) return;

    g_qHead = g_qTail = 0;
    ZeroMemory(g_groupAcc, sizeof(g_groupAcc));

    g_bRunning = TRUE;
    g_hThread  = CreateThread(NULL, 0, WorkerThreadProc, NULL, 0, NULL);
    if (!g_hThread)
    {
        g_bRunning = FALSE;
        CloseHandle(g_hEvent);
        g_hEvent = NULL;
    }
}

void WebhookShutdown(void)
{
    if (!g_bRunning) return;

    g_bRunning = FALSE;
    if (g_hEvent) SetEvent(g_hEvent);

    if (g_hThread)
    {
        WaitForSingleObject(g_hThread, 5000);
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }

    if (g_hEvent) { CloseHandle(g_hEvent); g_hEvent = NULL; }

    ZeroMemory(g_groupAcc, sizeof(g_groupAcc));
    g_qHead = g_qTail = 0;
}

void WebhookNotify(const char *capcode, const char *message, const char *label,
                   const char *szTime, const char *szDate,
                   const char *szMode, const char *szType, const char *szBitrate,
                   BOOL isGroup, int groupbit)
{
    if (!g_bRunning) return;

    EnterCriticalSection(&g_cs);

    if (isGroup && groupbit >= 0 && groupbit < MAX_GROUPBITS && !(g_iWebhookFields & WHF_LABEL_PERCAP))
    {
        GroupAcc *ga = &g_groupAcc[groupbit];
        if (!ga->active)
        {
            ga->active = TRUE;
            strncpy(ga->szMessage,   message, WEBHOOK_MSG_LEN  - 1);
            ga->szMessage[WEBHOOK_MSG_LEN - 1] = '\0';
            strncpy(ga->szAddresses, capcode, WEBHOOK_ADDR_LEN - 1);
            ga->szAddresses[WEBHOOK_ADDR_LEN - 1] = '\0';
            ga->szLabels[0] = '\0';
            AppendLabel(ga->szLabels, sizeof(ga->szLabels), label);
            strncpy(ga->szTime,    szTime,    sizeof(ga->szTime)    - 1);
            strncpy(ga->szDate,    szDate,    sizeof(ga->szDate)    - 1);
            strncpy(ga->szMode,    szMode,    sizeof(ga->szMode)    - 1);
            strncpy(ga->szType,    szType,    sizeof(ga->szType)    - 1);
            strncpy(ga->szBitrate, szBitrate, sizeof(ga->szBitrate) - 1);
            ga->lTimestamp = GetUnixTimestamp();
            if (g_iWebhookFields & WHF_LABEL_ARRAY)
            {
                ga->szSubscribers[0] = '[';
                int sPos = 1;
                AppendSubscriberEntry(ga->szSubscribers, &sPos, WEBHOOK_SUBSCRIBERS_LEN,
                                      capcode, label ? label : "", TRUE);
                ga->szSubscribers[sPos] = '\0';
            }
        }
        else
        {
            // Append capcode
            int len = (int)strlen(ga->szAddresses);
            if (len + 1 + (int)strlen(capcode) < WEBHOOK_ADDR_LEN - 1)
            {
                ga->szAddresses[len] = ' ';
                strcpy(ga->szAddresses + len + 1, capcode);
            }
            // Accumulate label (comma-separated, deduplication of adjacent identical labels)
            AppendLabel(ga->szLabels, sizeof(ga->szLabels), label);
            if (g_iWebhookFields & WHF_LABEL_ARRAY)
            {
                int sPos = (int)strlen(ga->szSubscribers);
                AppendSubscriberEntry(ga->szSubscribers, &sPos, WEBHOOK_SUBSCRIBERS_LEN,
                                      capcode, label ? label : "", FALSE);
                ga->szSubscribers[sPos] = '\0';
            }
        }
        LeaveCriticalSection(&g_cs);
    }
    else
    {
        WebhookJob job;
        ZeroMemory(&job, sizeof(job));
        strncpy(job.szAddress, capcode,  sizeof(job.szAddress)  - 1);
        strncpy(job.szMessage, message,  sizeof(job.szMessage)  - 1);
        strncpy(job.szLabel,   label  ? label  : "", sizeof(job.szLabel)   - 1);
        strncpy(job.szTime,    szTime ? szTime : "", sizeof(job.szTime)    - 1);
        strncpy(job.szDate,    szDate ? szDate : "", sizeof(job.szDate)    - 1);
        strncpy(job.szMode,    szMode ? szMode : "", sizeof(job.szMode)    - 1);
        strncpy(job.szType,    szType ? szType : "", sizeof(job.szType)    - 1);
        strncpy(job.szBitrate, szBitrate ? szBitrate : "", sizeof(job.szBitrate) - 1);
        job.lTimestamp = GetUnixTimestamp();
        if (g_iWebhookFields & WHF_LABEL_ARRAY)
        {
            job.szSubscribers[0] = '[';
            int sPos = 1;
            AppendSubscriberEntry(job.szSubscribers, &sPos, WEBHOOK_SUBSCRIBERS_LEN,
                                  capcode, label ? label : "", TRUE);
            if (sPos < WEBHOOK_SUBSCRIBERS_LEN - 1) { job.szSubscribers[sPos++] = ']'; job.szSubscribers[sPos] = '\0'; }
        }
        EnqueueLocked(&job);
        LeaveCriticalSection(&g_cs);
        SetEvent(g_hEvent);
    }
}

void WebhookFlushGroup(int groupbit)
{
    if (!g_bRunning) return;
    if (groupbit < 0 || groupbit >= MAX_GROUPBITS) return;
    if (g_iWebhookFields & WHF_LABEL_PERCAP) return;  // groups already sent individually in WebhookNotify

    EnterCriticalSection(&g_cs);
    GroupAcc *ga = &g_groupAcc[groupbit];
    if (ga->active)
    {
        WebhookJob job;
        ZeroMemory(&job, sizeof(job));
        strncpy(job.szAddress, ga->szAddresses, sizeof(job.szAddress)  - 1);
        strncpy(job.szMessage, ga->szMessage,   sizeof(job.szMessage)  - 1);
        strncpy(job.szLabel,   ga->szLabels,    sizeof(job.szLabel)    - 1);
        strncpy(job.szTime,    ga->szTime,      sizeof(job.szTime)     - 1);
        strncpy(job.szDate,    ga->szDate,      sizeof(job.szDate)     - 1);
        strncpy(job.szMode,    ga->szMode,      sizeof(job.szMode)     - 1);
        strncpy(job.szType,    ga->szType,      sizeof(job.szType)     - 1);
        strncpy(job.szBitrate, ga->szBitrate,   sizeof(job.szBitrate)  - 1);
        job.lTimestamp = ga->lTimestamp;
        if (g_iWebhookFields & WHF_LABEL_ARRAY)
        {
            strncpy(job.szSubscribers, ga->szSubscribers, WEBHOOK_SUBSCRIBERS_LEN - 2);
            job.szSubscribers[WEBHOOK_SUBSCRIBERS_LEN - 2] = '\0';
            int sLen = (int)strlen(job.szSubscribers);
            if (sLen > 0) { job.szSubscribers[sLen++] = ']'; job.szSubscribers[sLen] = '\0'; }
        }
        ZeroMemory(ga, sizeof(*ga));   // clear slot
        EnqueueLocked(&job);
    }
    LeaveCriticalSection(&g_cs);
    SetEvent(g_hEvent);
}

void WebhookSetStatusWnd(HWND hWnd)
{
    EnterCriticalSection(&g_cs);
    g_hStatusWnd = hWnd;
    LeaveCriticalSection(&g_cs);
}
