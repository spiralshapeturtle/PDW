/*
** telegram.cpp -- Telegram Bot API output sink for PDW  (FIX [Telegram])
**
** Threading model (mirrors webhook.cpp):
**   Main/decoder thread -> TelegramNotify -> g_queue (CRITICAL_SECTION + event)
**   Worker thread       -> dequeues jobs, POSTs to api.telegram.org via WinHTTP
**
** Endpoint:   https://api.telegram.org/bot<token>/sendMessage   (HTTPS, system TLS)
** Payload:    application/json  {"chat_id":N,"text":"...","parse_mode":"HTML",...}
** Targets:    one or more numeric chat_id's (';'-separated) -> one POST per chat_id.
**
** Title emulation: Telegram has no native subject. The configurable title template
** (e.g. "<b>{label}</b>") is prepended as the first line, then a blank line, then body.
**
** Formatting:  parse_mode=HTML. Message body is HTML-escaped (& < >). On a Telegram
**              "can't parse entities" error the same message is re-sent as plain text.
** Limits:      4096 chars/message -> split or truncate (configurable).
** Rate limit:  HTTP 429 -> honour "retry_after" from the response with back-off.
** Migration:   "migrate_to_chat_id" in the response updates the stored chat_id.
**
** Secret handling: the bot token is never written to the log (the request path is
** logged as /bot<redacted>/sendMessage).
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
#include "telegram.h"
#include "logmanager.h"

#pragma comment(lib, "winhttp.lib")

#define TG_HOST          L"api.telegram.org"
#define TG_MAX_TEXT      4096
#define TG_MSG_LEN       (MAX_STR_LEN + 256)
#define TG_QUEUE_SIZE    64
#define MAX_RETRIES      3

// ---------------------------------------------------------------------------
// Configuration snapshot (set at TelegramInit time, protected by g_cs)
// ---------------------------------------------------------------------------

static char  g_szToken    [80]  = "";
static char  g_szChatIds  [512] = "";
static char  g_szTitle    [128] = "";
static char  g_szBody     [256] = "";   // FIX [TgBodyTemplate]: body template, default "{message}"
static int   g_iThreadId        = 0;
static BOOL  g_bSilent          = FALSE;
static BOOL  g_bNoPreview       = FALSE;
static BOOL  g_bSplitLong       = FALSE;
static BOOL  g_bLogToFile       = FALSE;

// ---------------------------------------------------------------------------
// Job queue (ring buffer)
// ---------------------------------------------------------------------------

// FIX [TgGroupBatch]: list capacities for the group-call capcode/label accumulator. Sized like the
// MQTT/webhook feeds (ADDR_LEN 2048, SUBSCRIBERS_LEN 32768) for consistency and headroom; a large test
// alert of ~122 capcodes fits with room to spare. With the "Split" option enabled such a group call is
// delivered in full across several Telegram messages (the per-message API limit is 4096).
// FIX [TgGroupNewline]: labels are joined with newlines so each subscriber appears on its own line.
#define TG_CAPLIST_LEN   2048    // space-separated capcode list (~180 capcodes)
#define TG_LABELLIST_LEN 32768   // newline-separated label list (matches webhook/MQTT SUBSCRIBERS_LEN)

typedef struct {
    char szCapcode[TG_CAPLIST_LEN];   // holds a space-separated capcode list for group calls
    char szMessage[TG_MSG_LEN];
    char szLabel  [TG_LABELLIST_LEN]; // holds a comma-separated label list for group calls
    char szTime   [32];
    char szDate   [32];
    char szMode   [32];
    char szType   [32];
    char szBitrate[32];
} TelegramJob;

static TelegramJob g_queue[TG_QUEUE_SIZE];
static int         g_qHead = 0;
static int         g_qTail = 0;

// FIX [TgGroupBatch]: per-groupbit accumulator so a FLEX group call is sent as ONE message
// listing all (matched) subscriber capcodes/labels, instead of one Telegram per subscriber.
// Mirrors the webhook/MQTT/MySQL group-batching model (WebhookFlushGroup et al.).
#define TG_MAX_GROUPBITS 17
typedef struct {
    BOOL active;
    char szCapcodes[TG_CAPLIST_LEN];
    char szLabels  [TG_LABELLIST_LEN];
    char szMessage [TG_MSG_LEN];
    char szTime    [32];
    char szDate    [32];
    char szMode    [32];
    char szType    [32];
    char szBitrate [32];
} TgGroupAcc;
static TgGroupAcc g_groupAcc[TG_MAX_GROUPBITS];

// Append val to a comma/space-separated list in dst, skipping empties and adjacent duplicates.
static void AppendListItem(char *dst, int dstLen, const char *val, char sep)
{
    if (!val || !val[0]) return;
    int len = (int)strlen(dst);
    // adjacent-duplicate suppression: skip if the list already ends with this exact value
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

static BOOL inline QueueFull(void)  { return ((g_qTail + 1) % TG_QUEUE_SIZE) == g_qHead; }
static BOOL inline QueueEmpty(void) { return g_qHead == g_qTail; }

// ---------------------------------------------------------------------------
// Thread / sync
// ---------------------------------------------------------------------------

static HANDLE          g_hThread  = NULL;
static HANDLE          g_hEvent   = NULL;
// FIX [AtomicRunning]: was `volatile BOOL`. volatile gives neither atomicity nor cross-thread
// memory ordering in the C++ model, yet this flag is written by the GUI thread (Init/Shutdown)
// and read lock-free by the decoder thread (Notify/FlushGroup) and the worker (loop + sleep).
// std::atomic<bool> makes those reads/writes well-defined; assignment from BOOL TRUE/FALSE and
// use in boolean context are unchanged, so no call site needs to change.
static std::atomic<bool> g_bRunning(false);
static CRITICAL_SECTION g_cs;
static BOOL            g_csInit   = FALSE;
static HWND            g_hStatusWnd = NULL;   // protected by g_cs
static unsigned        g_droppedJobs = 0;

static void PostStatus(int status, LPARAM lp)
{
    HWND hWnd;
    EnterCriticalSection(&g_cs);
    hWnd = g_hStatusWnd;
    LeaveCriticalSection(&g_cs);
    if (hWnd) PostMessage(hWnd, WM_TELEGRAM_STATUS, (WPARAM)status, lp);
}

static void WriteLog(const char *fmt, ...)
{
    if (!g_bLogToFile) return;
    char szLine[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(szLine, sizeof(szLine), _TRUNCATE, fmt, ap);
    va_end(ap);
    PDW_TELEGRAMLOG("%s", szLine);
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

// Append src to dst[*p], HTML-escaping & < > and honouring dstLen (no fixed-size temp buffer,
// so it is safe for values as large as a full paging message).
static void AppendHtmlEscaped(char *dst, int *p, int dstLen, const char *src)
{
    for (int i = 0; src && src[i] && *p < dstLen - 7; i++)
    {
        char c = src[i];
        if      (c == '&') { strcpy(dst + *p, "&amp;"); *p += 5; }
        else if (c == '<') { strcpy(dst + *p, "&lt;");  *p += 4; }
        else if (c == '>') { strcpy(dst + *p, "&gt;");  *p += 4; }
        else               { dst[(*p)++] = c; }
    }
}

// HTML-escape & < > into dst (for Telegram parse_mode=HTML body content).
static void HtmlEscape(char *dst, int dstLen, const char *src)
{
    int p = 0;
    AppendHtmlEscaped(dst, &p, dstLen, src);
    dst[p] = '\0';
}

// JSON-escape src into dst[*pos], honouring maxLen. High/control bytes -> \u00XX.
static void JsonEscape(char *dst, int *pos, int maxLen, const char *src)
{
    for (int i = 0; src && src[i] && *pos < maxLen - 8; i++)
    {
        unsigned char c = (unsigned char)src[i];
        if      (c == '"')  { dst[(*pos)++] = '\\'; dst[(*pos)++] = '"';  }
        else if (c == '\\') { dst[(*pos)++] = '\\'; dst[(*pos)++] = '\\'; }
        else if (c == '\r') { dst[(*pos)++] = '\\'; dst[(*pos)++] = 'r';  }
        else if (c == '\n') { dst[(*pos)++] = '\\'; dst[(*pos)++] = 'n';  }
        else if (c == '\t') { dst[(*pos)++] = '\\'; dst[(*pos)++] = 't';  }
        else if (c < 32)
        {
            char esc[8]; sprintf(esc, "\\u%04X", c);
            for (int k = 0; esc[k] && *pos < maxLen - 2; k++) dst[(*pos)++] = esc[k];
        }
        else dst[(*pos)++] = (char)c;   // pass UTF-8 multibyte through unchanged
    }
}

// Resolve {message}/{label}/{capcode}/{time}/{date}/{mode}/{type}/{bitrate} placeholders in a
// template string. Literal template text (incl. <b> tags) is copied verbatim; only the substituted
// *values* are HTML-escaped so they cannot break the HTML parse. Used for both title and body.
static void ResolveTemplate(char *dst, int dstLen, const char *tmpl, const TelegramJob *job)
{
    int p = 0;
    const char *t = tmpl;
    while (*t && p < dstLen - 1)
    {
        // FIX [TgTemplateNewline]: a literal "\n" in the template inserts a real line break, and
        // "\\" inserts a single backslash. Single-line edit controls can't hold a real newline, so
        // this lets a user write e.g. "<b>{message}</b>\n{label}" for title-less, no-blank-line output.
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
                        AppendHtmlEscaped(dst, &p, dstLen, val);
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

// Build the full HTML message text: "<title>\n\n<body>" (title optional) from the given title/body
// templates. An empty body template falls back to the raw (escaped) message so older configs keep
// working. Passing the templates as parameters lets the GUI Test button reuse the exact rendering.
static void BuildMessageTextEx(char *dst, int dstLen, const char *title, const char *body, const TelegramJob *job)
{
    char szTitle[512];
    szTitle[0] = '\0';
    if (title && title[0]) ResolveTemplate(szTitle, sizeof(szTitle), title, job);

    // FIX [TgBuildRace]: this was a function-static buffer, but BuildMessageTextEx runs on BOTH the
    // worker thread (DoSend) and the GUI thread (TelegramTestSend). If the user clicks Test while the
    // worker is mid-send, the two callers raced on the shared static and corrupted each other's body.
    // Allocate per-call instead (it is too large - ~38 KB - for the stack on the decode path).
    const int bodyCap = TG_LABELLIST_LEN + TG_MSG_LEN;
    char *szBody = (char *)malloc(bodyCap);
    if (!szBody) { if (dstLen > 0) dst[0] = '\0'; return; }
    if (body && body[0]) ResolveTemplate(szBody, bodyCap, body, job);
    else                 HtmlEscape(szBody, bodyCap, job->szMessage);

    if (szTitle[0])
        _snprintf(dst, dstLen - 1, "%s\n\n%s", szTitle, szBody);
    else
        _snprintf(dst, dstLen - 1, "%s", szBody);
    dst[dstLen - 1] = '\0';
    free(szBody);
}

// Render using the worker's configured templates (g_szTitle/g_szBody).
static void BuildMessageText(char *dst, int dstLen, const TelegramJob *job)
{
    BuildMessageTextEx(dst, dstLen, g_szTitle, g_szBody, job);
}

// Build the JSON request body for one chat_id. useParse=FALSE drops parse_mode (plain fallback).
static void BuildJson(char *dst, int maxLen, const char *chatId, const char *htmlText, BOOL useParse)
{
    int p = 0;
    const char *pre = "{\"chat_id\":";
    for (int i = 0; pre[i] && p < maxLen - 2; i++) dst[p++] = pre[i];
    for (int i = 0; chatId[i] && p < maxLen - 2; i++) dst[p++] = chatId[i];
    const char *mid = ",\"text\":\"";
    for (int i = 0; mid[i] && p < maxLen - 2; i++) dst[p++] = mid[i];
    JsonEscape(dst, &p, maxLen, htmlText);
    if (p < maxLen - 2) dst[p++] = '"';
    if (useParse)
    {
        const char *pm = ",\"parse_mode\":\"HTML\"";
        for (int i = 0; pm[i] && p < maxLen - 2; i++) dst[p++] = pm[i];
    }
    if (g_bNoPreview)
    {
        const char *np = ",\"disable_web_page_preview\":true";
        for (int i = 0; np[i] && p < maxLen - 2; i++) dst[p++] = np[i];
    }
    if (g_bSilent)
    {
        const char *ds = ",\"disable_notification\":true";
        for (int i = 0; ds[i] && p < maxLen - 2; i++) dst[p++] = ds[i];
    }
    if (g_iThreadId > 0)
    {
        char tt[48]; sprintf(tt, ",\"message_thread_id\":%d", g_iThreadId);
        for (int i = 0; tt[i] && p < maxLen - 2; i++) dst[p++] = tt[i];
    }
    if (p < maxLen - 1) dst[p++] = '}';
    dst[p] = '\0';
}

// ---------------------------------------------------------------------------
// WinHTTP (worker-thread only)
// ---------------------------------------------------------------------------

static HINTERNET g_hSession = NULL;
static HINTERNET g_hConnect = NULL;

static void CloseConnection(void) { if (g_hConnect) { WinHttpCloseHandle(g_hConnect); g_hConnect = NULL; } }
static void CloseSession(void)    { CloseConnection(); if (g_hSession) { WinHttpCloseHandle(g_hSession); g_hSession = NULL; } }

static BOOL EnsureSession(void)
{
    if (g_hSession) return TRUE;
    g_hSession = WinHttpOpen(L"PDW-Telegram/1.0",
                             WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_hSession) return FALSE;
    WinHttpSetTimeouts(g_hSession, 10000, 10000, 10000, 10000);
    DWORD dwProto = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1
                  | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1
                  | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(g_hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &dwProto, sizeof(dwProto));
    return TRUE;
}

static BOOL EnsureConnection(void)
{
    if (g_hConnect) return TRUE;
    if (!EnsureSession()) return FALSE;
    g_hConnect = WinHttpConnect(g_hSession, TG_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    return g_hConnect != NULL;
}

// One HTTPS POST to /bot<token>/<method>. Returns HTTP status (0 = transport error).
// Response body (truncated) copied into respOut for caller parsing.
static int HttpPost(const char *token, const char *method,
                    const char *body, int bodyLen, char *respOut, int respLen)
{
    if (respOut && respLen) respOut[0] = '\0';
    if (!EnsureConnection()) { CloseConnection(); return 0; }

    char szPathA[256];
    _snprintf(szPathA, sizeof(szPathA) - 1, "/bot%s/%s", token, method);
    szPathA[sizeof(szPathA) - 1] = '\0';
    WCHAR wszPath[256];
    MultiByteToWideChar(CP_ACP, 0, szPathA, -1, wszPath, 256);

    HINTERNET hReq = WinHttpOpenRequest(g_hConnect, L"POST", wszPath, NULL,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE);
    if (!hReq) { CloseConnection(); return 0; }

    static const WCHAR *szHeaders = L"Content-Type: application/json\r\nConnection: keep-alive\r\n";
    BOOL bOK = WinHttpSendRequest(hReq, szHeaders, (DWORD)-1L,
                                  (LPVOID)body, (DWORD)bodyLen, (DWORD)bodyLen, 0);
    if (!bOK) { WinHttpCloseHandle(hReq); CloseConnection(); return 0; }

    bOK = WinHttpReceiveResponse(hReq, NULL);
    if (!bOK) { WinHttpCloseHandle(hReq); CloseConnection(); return 0; }

    DWORD dwStatus = 0, dwSize = sizeof(dwStatus);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        NULL, &dwStatus, &dwSize, NULL);

    int rpos = 0;
    DWORD dwRead = 0;
    char buf[1024];
    while (WinHttpReadData(hReq, buf, sizeof(buf), &dwRead) && dwRead > 0)
    {
        if (respOut && rpos < respLen - 1)
        {
            int n = (int)dwRead;
            if (n > respLen - 1 - rpos) n = respLen - 1 - rpos;
            memcpy(respOut + rpos, buf, n);
            rpos += n;
        }
    }
    if (respOut && respLen) respOut[rpos] = '\0';

    WinHttpCloseHandle(hReq);
    return (int)dwStatus;
}

// Extract an integer JSON value: "key":<number>. Returns TRUE if found.
static BOOL JsonFindInt(const char *json, const char *key, LONGLONG *out)
{
    char pat[64];
    _snprintf(pat, sizeof(pat) - 1, "\"%s\":", key);
    pat[sizeof(pat) - 1] = '\0';
    const char *p = strstr(json, pat);
    if (!p) return FALSE;
    p += strlen(pat);
    while (*p == ' ') p++;
    *out = _atoi64(p);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Send one chat_id (with parse_mode fallback + 429 retry_after)
// ---------------------------------------------------------------------------

static const DWORD g_retryDelays[MAX_RETRIES] = { 1000, 2000, 4000 };

// Sleep that bails out early when a shutdown/reconfigure clears g_bRunning, so the
// INFINITE worker-join in TelegramShutdown() stays bounded even on a long 429 retry_after.
static void InterruptibleSleep(DWORD ms)
{
    while (ms > 0 && g_bRunning)
    {
        DWORD step = ms > 200 ? 200 : ms;
        Sleep(step);
        ms -= step;
    }
}

// Replace old chat_id with the migrated one in g_szChatIds and Profile (best effort).
static void ApplyMigration(const char *oldId, LONGLONG newId)
{
    char newStr[32];
    sprintf(newStr, "%lld", newId);
    EnterCriticalSection(&g_cs);
    char *pos = strstr(g_szChatIds, oldId);
    if (pos)
    {
        char tail[512];
        strncpy(tail, pos + strlen(oldId), sizeof(tail) - 1);
        tail[sizeof(tail) - 1] = '\0';
        int head = (int)(pos - g_szChatIds);
        _snprintf(g_szChatIds + head, sizeof(g_szChatIds) - 1 - head, "%s%s", newStr, tail);
        g_szChatIds[sizeof(g_szChatIds) - 1] = '\0';
        // mirror to Profile so it persists on next WriteSettings()
        strncpy(Profile.szTelegramChatIds, g_szChatIds, sizeof(Profile.szTelegramChatIds) - 1);
        Profile.szTelegramChatIds[sizeof(Profile.szTelegramChatIds) - 1] = '\0';
    }
    LeaveCriticalSection(&g_cs);
    WriteLog("MIGRATE chat_id %s -> %lld (updated)", oldId, newId);
}

static void SendToChat(const char *token, const char *chatId, const char *htmlText)
{
    static char jsonBody[6 * TG_MSG_LEN + 1024];
    char resp[2048];

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++)
    {
        if (attempt > 0)
        {
            if (!g_bRunning) return;
            PostStatus(TGS_RETRY, attempt);
        }

        BuildJson(jsonBody, sizeof(jsonBody), chatId, htmlText, TRUE);
        int status = HttpPost(token, "sendMessage", jsonBody, (int)strlen(jsonBody), resp, sizeof(resp));

        if (status >= 200 && status < 300)
        {
            WriteLog("SENT    chat_id=%s -> %d OK", chatId, status);
            PostStatus(TGS_OK, status);
            return;
        }

        // Rate limited: honour retry_after.
        if (status == 429)
        {
            LONGLONG ra = 0;
            DWORD waitMs = g_retryDelays[attempt < MAX_RETRIES ? attempt : MAX_RETRIES - 1];
            if (JsonFindInt(resp, "retry_after", &ra) && ra > 0)
            {
                if (ra > 60) ra = 60;            // cap so shutdown stays bounded
                waitMs = (DWORD)(ra * 1000);
            }
            WriteLog("RATE    chat_id=%s 429 retry_after=%llds", chatId, (LONGLONG)(waitMs / 1000));
            PostStatus(TGS_RETRY, attempt + 1);
            if (!g_bRunning) return;
            InterruptibleSleep(waitMs);
            continue;
        }

        // Parse-entity error -> resend once as plain text (no parse_mode).
        if (status == 400 && strstr(resp, "can't parse entities"))
        {
            WriteLog("PARSE   chat_id=%s HTML rejected, retrying as plain text", chatId);
            BuildJson(jsonBody, sizeof(jsonBody), chatId, htmlText, FALSE);
            status = HttpPost(token, "sendMessage", jsonBody, (int)strlen(jsonBody), resp, sizeof(resp));
            if (status >= 200 && status < 300) { PostStatus(TGS_OK, status); return; }
        }

        // Supergroup migration -> update stored chat_id and retry against the new id.
        if (status == 400 && strstr(resp, "migrate_to_chat_id"))
        {
            LONGLONG newId = 0;
            if (JsonFindInt(resp, "migrate_to_chat_id", &newId))
            {
                ApplyMigration(chatId, newId);
                char newStr[32]; sprintf(newStr, "%lld", newId);
                BuildJson(jsonBody, sizeof(jsonBody), newStr, htmlText, TRUE);
                status = HttpPost(token, "sendMessage", jsonBody, (int)strlen(jsonBody), resp, sizeof(resp));
                if (status >= 200 && status < 300) { PostStatus(TGS_OK, status); return; }
            }
        }

        if (status != 0)
        {
            // 4xx (bot blocked, kicked from group, bad token) — no point retrying.
            WriteLog("ERROR   chat_id=%s -> HTTP %d", chatId, status);
            PostStatus(TGS_ERROR, status);
            return;
        }

        // transport error -> back off and retry
        if (!g_bRunning) return;
        InterruptibleSleep(g_retryDelays[attempt]);
    }
    WriteLog("ERROR   chat_id=%s all retries failed", chatId);
    PostStatus(TGS_ERROR, 0);
}

// Split (or truncate) the HTML text and send each chunk to every configured chat_id.
static void DoSend(const TelegramJob *job)
{
    char szToken[80], szChatIds[512];
    EnterCriticalSection(&g_cs);
    strncpy(szToken,   g_szToken,   sizeof(szToken) - 1);   szToken[sizeof(szToken) - 1]   = '\0';
    strncpy(szChatIds, g_szChatIds, sizeof(szChatIds) - 1); szChatIds[sizeof(szChatIds) - 1] = '\0';
    LeaveCriticalSection(&g_cs);

    if (!szToken[0] || !szChatIds[0]) return;

    static char szText[TG_LABELLIST_LEN + TG_MSG_LEN + 1024];   // title + full label/message body
    BuildMessageText(szText, sizeof(szText), job);

    PostStatus(TGS_SENDING, 0);

    // Build the list of chunks (1 chunk unless >4096 and splitting enabled).
    int total = (int)strlen(szText);
    int chunkStart = 0;
    do
    {
        char chunk[TG_MAX_TEXT + 16];
        int remaining = total - chunkStart;
        int take = remaining;
        if (remaining > TG_MAX_TEXT)
        {
            if (g_bSplitLong) take = TG_MAX_TEXT;
            else { take = TG_MAX_TEXT - 3; }  // truncate
            // FIX [TgSplitBoundary]: never cut inside a UTF-8 multibyte sequence, an HTML tag
            // (<...>) or an escaped entity (&...;). A severed sequence makes the JSON invalid UTF-8
            // (Telegram drops the whole message -> silent loss) or breaks parse_mode=HTML markup.
            // Back the cut off to the last safe boundary; keep at least 1 byte so we always progress.
            {
                int safe = take;
                while (safe > 1)
                {
                    unsigned char c = (unsigned char)szText[chunkStart + safe];
                    if ((c & 0xC0) == 0x80) { safe--; continue; }   // mid UTF-8 continuation byte
                    // don't end while still inside an unfinished '<...>' tag or '&...;' entity
                    int j = chunkStart, openTag = 0, openAmp = 0;
                    for (; j < chunkStart + safe; j++)
                    {
                        char k = szText[j];
                        if      (k == '<') openTag = 1;
                        else if (k == '>') openTag = 0;
                        else if (k == '&') { openAmp = 1; }
                        else if (k == ';') openAmp = 0;
                        else if (openAmp && (k == ' ' || k == '<' || k == '&')) openAmp = 0;
                    }
                    if (openTag || openAmp) { safe--; continue; }
                    break;
                }
                take = safe;
            }
        }
        memcpy(chunk, szText + chunkStart, take);
        chunk[take] = '\0';
        if (remaining > TG_MAX_TEXT && !g_bSplitLong) strcat(chunk, "...");

        // Send this chunk to every chat_id (one POST each).
        char ids[512];
        strncpy(ids, szChatIds, sizeof(ids) - 1); ids[sizeof(ids) - 1] = '\0';
        char *ctx = NULL;
        char *tok = strtok_s(ids, ";, ", &ctx);
        while (tok)
        {
            while (*tok == ' ') tok++;
            if (*tok) SendToChat(szToken, tok, chunk);
            tok = strtok_s(NULL, ";, ", &ctx);
        }

        chunkStart += take;
        if (!g_bSplitLong) break;   // truncate mode: only the first chunk
    } while (chunkStart < total && g_bRunning);
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
            TelegramJob job; BOOL have = FALSE;
            EnterCriticalSection(&g_cs);
            if (!QueueEmpty()) { job = g_queue[g_qHead]; g_qHead = (g_qHead + 1) % TG_QUEUE_SIZE; have = TRUE; }
            LeaveCriticalSection(&g_cs);
            if (!have) break;
            DoSend(&job);
        }
    }
    // flush remaining on shutdown
    for (;;)
    {
        TelegramJob job; BOOL have = FALSE;
        EnterCriticalSection(&g_cs);
        if (!QueueEmpty()) { job = g_queue[g_qHead]; g_qHead = (g_qHead + 1) % TG_QUEUE_SIZE; have = TRUE; }
        LeaveCriticalSection(&g_cs);
        if (!have) break;
        DoSend(&job);
    }
    CloseSession();
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void TelegramInit(void)
{
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = TRUE; }

    TelegramShutdown();

    if (!Profile.telegramEnabled || !Profile.szTelegramToken[0] || !Profile.szTelegramChatIds[0]) return;

    EnterCriticalSection(&g_cs);
    strncpy(g_szToken,   Profile.szTelegramToken,   sizeof(g_szToken) - 1);   g_szToken[sizeof(g_szToken) - 1] = '\0';
    strncpy(g_szChatIds, Profile.szTelegramChatIds, sizeof(g_szChatIds) - 1); g_szChatIds[sizeof(g_szChatIds) - 1] = '\0';
    strncpy(g_szTitle,   Profile.szTelegramTitle,   sizeof(g_szTitle) - 1);   g_szTitle[sizeof(g_szTitle) - 1] = '\0';
    strncpy(g_szBody,    Profile.szTelegramBody,    sizeof(g_szBody) - 1);    g_szBody[sizeof(g_szBody) - 1]   = '\0';   // FIX [TgBodyTemplate]
    g_iThreadId  = Profile.telegramThreadId;
    g_bSilent    = Profile.telegramSilent    ? TRUE : FALSE;
    g_bNoPreview = Profile.telegramNoPreview ? TRUE : FALSE;
    g_bSplitLong = Profile.telegramSplitLong ? TRUE : FALSE;
    g_bLogToFile = Profile.telegramLogToFile ? TRUE : FALSE;
    LeaveCriticalSection(&g_cs);

    g_hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_hEvent) return;

    g_qHead = g_qTail = 0;
    ZeroMemory(g_groupAcc, sizeof(g_groupAcc));   // FIX [TgGroupBatch]
    g_bRunning = TRUE;
    g_hThread  = CreateThread(NULL, 0, WorkerThreadProc, NULL, 0, NULL);
    if (!g_hThread) { g_bRunning = FALSE; CloseHandle(g_hEvent); g_hEvent = NULL; }
}

void TelegramShutdown(void)
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
    // FIX [TgEventRace]: tear down the event and queue under g_cs so a decoder-thread EnqueueJob
    // can't be mid-SetEvent on this handle, nor tear the ring indices, while we close them.
    HANDLE ev = NULL;
    if (g_csInit) EnterCriticalSection(&g_cs);
    ev = g_hEvent; g_hEvent = NULL;
    g_qHead = g_qTail = 0;
    ZeroMemory(g_groupAcc, sizeof(g_groupAcc));   // FIX [TgGroupBatch]
    if (g_csInit) LeaveCriticalSection(&g_cs);
    if (ev) CloseHandle(ev);
}

void TelegramDestroy(void)
{
    TelegramShutdown();
    // FIX [TgCsTeardown]: do NOT DeleteCriticalSection here. TelegramNotify/FlushGroup test the
    // lock-free g_bRunning flag and THEN take g_cs; a decoder thread that passed the test before
    // shutdown cleared the flag could otherwise enter an already-deleted section (UB/crash). This
    // is only ever called at process exit (WM_DESTROY), so leaving g_cs intact and letting the OS
    // reclaim it on termination closes that window with no leak that outlives the process. g_csInit
    // stays TRUE so any late call still hits a valid section.
}

// Enqueue a fully built job (caller holds nothing; takes g_cs internally).
static void EnqueueJob(const TelegramJob *job)
{
    EnterCriticalSection(&g_cs);
    if (QueueFull())
    {
        g_droppedJobs++;
        WriteLog("DROP queue full - message discarded (total dropped=%u)", g_droppedJobs);
    }
    else
    {
        g_queue[g_qTail] = *job;
        g_qTail = (g_qTail + 1) % TG_QUEUE_SIZE;
    }
    // FIX [TgEventRace]: signal WHILE holding the lock. Previously the event was snapshotted under
    // the lock but SetEvent ran after LeaveCriticalSection, so a concurrent TelegramShutdown (GUI
    // thread) could CloseHandle(g_hEvent) in the gap, leaving this SetEvent to hit a closed/recycled
    // handle. Shutdown now also clears the handle under g_cs, closing the window completely.
    if (g_hEvent) SetEvent(g_hEvent);
    LeaveCriticalSection(&g_cs);
}

void TelegramNotify(const char *capcode, const char *message, const char *label,
                    const char *szTime, const char *szDate,
                    const char *szMode, const char *szType, const char *szBitrate,
                    BOOL isGroup, int groupbit)
{
    if (!g_bRunning) return;

    // FIX [TgGroupBatch]: a FLEX group call calls TelegramNotify once per subscriber capcode.
    // Accumulate them per groupbit and emit a single message from TelegramFlushGroup(), so the
    // user receives ONE Telegram listing all subscriber capcodes/labels rather than N copies.
    if (isGroup && groupbit >= 0 && groupbit < TG_MAX_GROUPBITS)
    {
        EnterCriticalSection(&g_cs);
        TgGroupAcc *ga = &g_groupAcc[groupbit];
        if (!ga->active)
        {
            ZeroMemory(ga, sizeof(*ga));
            ga->active = TRUE;
            strncpy(ga->szMessage, message ? message : "", sizeof(ga->szMessage) - 1);
            strncpy(ga->szTime,    szTime  ? szTime  : "", sizeof(ga->szTime)    - 1);
            strncpy(ga->szDate,    szDate  ? szDate  : "", sizeof(ga->szDate)    - 1);
            strncpy(ga->szMode,    szMode  ? szMode  : "", sizeof(ga->szMode)    - 1);
            strncpy(ga->szType,    szType  ? szType  : "", sizeof(ga->szType)    - 1);
            strncpy(ga->szBitrate, szBitrate ? szBitrate : "", sizeof(ga->szBitrate) - 1);
        }
        AppendListItem(ga->szCapcodes, sizeof(ga->szCapcodes), capcode, ' ');
        AppendListItem(ga->szLabels,   sizeof(ga->szLabels),   label,   '\n');	// FIX [TgGroupNewline]: one label per line
        LeaveCriticalSection(&g_cs);
        return;
    }

    TelegramJob job;
    ZeroMemory(&job, sizeof(job));
    strncpy(job.szCapcode, capcode ? capcode : "", sizeof(job.szCapcode) - 1);
    strncpy(job.szMessage, message ? message : "", sizeof(job.szMessage) - 1);
    strncpy(job.szLabel,   label   ? label   : "", sizeof(job.szLabel)   - 1);
    strncpy(job.szTime,    szTime  ? szTime  : "", sizeof(job.szTime)    - 1);
    strncpy(job.szDate,    szDate  ? szDate  : "", sizeof(job.szDate)    - 1);
    strncpy(job.szMode,    szMode  ? szMode  : "", sizeof(job.szMode)    - 1);
    strncpy(job.szType,    szType  ? szType  : "", sizeof(job.szType)    - 1);
    strncpy(job.szBitrate, szBitrate ? szBitrate : "", sizeof(job.szBitrate) - 1);

    EnqueueJob(&job);
}

// FIX [TgGroupBatch]: flush one accumulated group call as a single Telegram message.
void TelegramFlushGroup(int groupbit)
{
    if (!g_bRunning) return;
    if (groupbit < 0 || groupbit >= TG_MAX_GROUPBITS) return;

    TelegramJob job;
    BOOL have = FALSE;
    EnterCriticalSection(&g_cs);
    TgGroupAcc *ga = &g_groupAcc[groupbit];
    if (ga->active)
    {
        ZeroMemory(&job, sizeof(job));
        strncpy(job.szCapcode, ga->szCapcodes, sizeof(job.szCapcode) - 1);
        strncpy(job.szLabel,   ga->szLabels,   sizeof(job.szLabel)   - 1);
        strncpy(job.szMessage, ga->szMessage,  sizeof(job.szMessage) - 1);
        strncpy(job.szTime,    ga->szTime,     sizeof(job.szTime)    - 1);
        strncpy(job.szDate,    ga->szDate,     sizeof(job.szDate)    - 1);
        strncpy(job.szMode,    ga->szMode,     sizeof(job.szMode)    - 1);
        strncpy(job.szType,    ga->szType,     sizeof(job.szType)    - 1);
        strncpy(job.szBitrate, ga->szBitrate,  sizeof(job.szBitrate) - 1);
        ZeroMemory(ga, sizeof(*ga));   // clear slot
        have = TRUE;
    }
    LeaveCriticalSection(&g_cs);

    if (have) EnqueueJob(&job);
}

void TelegramSetStatusWnd(HWND hWnd)
{
    if (!g_csInit) { g_hStatusWnd = hWnd; return; }
    EnterCriticalSection(&g_cs);
    g_hStatusWnd = hWnd;
    LeaveCriticalSection(&g_cs);
}

// ---------------------------------------------------------------------------
// Synchronous helpers for the config dialog (GUI thread, no worker needed)
// ---------------------------------------------------------------------------

// A standalone session for dialog-thread requests (Test / Discover), kept separate
// from the worker connection so it can run while the worker is stopped.
static int DialogPost(const char *token, const char *method,
                      const char *body, int bodyLen, char *respOut, int respLen)
{
    HINTERNET hS = WinHttpOpen(L"PDW-Telegram/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return 0;
    WinHttpSetTimeouts(hS, 10000, 10000, 10000, 10000);
    DWORD dwProto = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(hS, WINHTTP_OPTION_SECURE_PROTOCOLS, &dwProto, sizeof(dwProto));

    HINTERNET hC = WinHttpConnect(hS, TG_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hC) { WinHttpCloseHandle(hS); return 0; }

    char szPathA[256];
    _snprintf(szPathA, sizeof(szPathA) - 1, "/bot%s/%s", token, method);
    szPathA[sizeof(szPathA) - 1] = '\0';
    WCHAR wszPath[256];
    MultiByteToWideChar(CP_ACP, 0, szPathA, -1, wszPath, 256);

    int result = 0;
    HINTERNET hReq = WinHttpOpenRequest(hC, body ? L"POST" : L"GET", wszPath, NULL,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (hReq)
    {
        static const WCHAR *hdr = L"Content-Type: application/json\r\n";
        BOOL bOK = WinHttpSendRequest(hReq, body ? hdr : WINHTTP_NO_ADDITIONAL_HEADERS, body ? (DWORD)-1L : 0,
                                      (LPVOID)body, (DWORD)bodyLen, (DWORD)bodyLen, 0);
        if (bOK && WinHttpReceiveResponse(hReq, NULL))
        {
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
            result = (int)dwStatus;
        }
        WinHttpCloseHandle(hReq);
    }
    WinHttpCloseHandle(hC);
    WinHttpCloseHandle(hS);
    return result;
}

// FIX [TgTestPreview]: fill a job with representative sample values so the Test button previews the
// real Title/Body template layout (incl. bold and one-label-per-line group rendering).
static void FillSampleJob(TelegramJob *job)
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

// FIX [TgTestPreview]: the Test button now renders a sample page through the supplied Title/Body
// templates with parse_mode=HTML, so the recipient sees the exact formatting (bold, line breaks).
BOOL TelegramTestSend(const char *token, const char *chatids, const char *title, const char *body,
                      char *errOut, int errLen)
{
    if (errOut && errLen) errOut[0] = '\0';
    if (!token || !token[0])   { if (errOut) _snprintf(errOut, errLen - 1, "No bot token set"); return FALSE; }
    if (!chatids || !chatids[0]){ if (errOut) _snprintf(errOut, errLen - 1, "No chat_id set");   return FALSE; }

    TelegramJob job;
    FillSampleJob(&job);
    static char text[4096];
    BuildMessageTextEx(text, sizeof(text), title, body, &job);

    char ids[512]; strncpy(ids, chatids, sizeof(ids) - 1); ids[sizeof(ids) - 1] = '\0';
    char resp[2048];
    BOOL anySent = FALSE, anyFail = FALSE;
    char *ctx = NULL;
    char *tok = strtok_s(ids, ";, ", &ctx);
    while (tok)
    {
        while (*tok == ' ') tok++;
        if (*tok)
        {
            static char json[8192];
            int p = 0;
            const char *pre = "{\"chat_id\":";
            for (int i = 0; pre[i] && p < (int)sizeof(json) - 2; i++) json[p++] = pre[i];
            for (int i = 0; tok[i] && p < (int)sizeof(json) - 2; i++) json[p++] = tok[i];
            const char *mid = ",\"text\":\"";
            for (int i = 0; mid[i] && p < (int)sizeof(json) - 2; i++) json[p++] = mid[i];
            JsonEscape(json, &p, sizeof(json), text);
            if (p < (int)sizeof(json) - 2) json[p++] = '"';
            const char *post = ",\"parse_mode\":\"HTML\"}";
            for (int i = 0; post[i] && p < (int)sizeof(json) - 1; i++) json[p++] = post[i];
            json[p] = '\0';

            int st = DialogPost(token, "sendMessage", json, (int)strlen(json), resp, sizeof(resp));
            if (st >= 200 && st < 300) anySent = TRUE;
            else
            {
                // Fall back to plain text if the HTML markup is rejected (mirrors the worker path).
                if (st == 400 && strstr(resp, "can't parse entities"))
                {
                    p = 0;
                    for (int i = 0; pre[i] && p < (int)sizeof(json) - 2; i++) json[p++] = pre[i];
                    for (int i = 0; tok[i] && p < (int)sizeof(json) - 2; i++) json[p++] = tok[i];
                    for (int i = 0; mid[i] && p < (int)sizeof(json) - 2; i++) json[p++] = mid[i];
                    JsonEscape(json, &p, sizeof(json), text);
                    if (p < (int)sizeof(json) - 2) json[p++] = '"';
                    if (p < (int)sizeof(json) - 1) json[p++] = '}';
                    json[p] = '\0';
                    st = DialogPost(token, "sendMessage", json, (int)strlen(json), resp, sizeof(resp));
                }
                if (st >= 200 && st < 300) anySent = TRUE;
                else { anyFail = TRUE; if (errOut && !errOut[0]) _snprintf(errOut, errLen - 1, "chat_id %s failed (HTTP %d)", tok, st); }
            }
        }
        tok = strtok_s(NULL, ";, ", &ctx);
    }
    return anySent && !anyFail;
}

BOOL TelegramDiscoverChatId(const char *token, char *chatOut, int chatLen, char *errOut, int errLen)
{
    if (chatOut && chatLen) chatOut[0] = '\0';
    if (errOut && errLen)   errOut[0]  = '\0';
    if (!token || !token[0]) { if (errOut) _snprintf(errOut, errLen - 1, "No bot token set"); return FALSE; }

    char resp[8192];
    int st = DialogPost(token, "getUpdates", NULL, 0, resp, sizeof(resp));
    if (st < 200 || st >= 300) { if (errOut) _snprintf(errOut, errLen - 1, "getUpdates failed (HTTP %d)", st); return FALSE; }

    // Find the LAST "chat":{"id":<n> ... "first_name"/"title" in the updates.
    const char *p = resp, *lastChat = NULL;
    while ((p = strstr(p, "\"chat\":")) != NULL) { lastChat = p; p += 7; }
    if (!lastChat) { if (errOut) _snprintf(errOut, errLen - 1, "No messages yet - send /start to the bot first"); return FALSE; }

    const char *idp = strstr(lastChat, "\"id\":");
    if (!idp) { if (errOut) _snprintf(errOut, errLen - 1, "Could not parse chat id"); return FALSE; }
    idp += 5;
    LONGLONG id = _atoi64(idp);

    // Try to grab a human-readable name (title for groups, first_name for users).
    char name[128] = "";
    const char *np = strstr(lastChat, "\"title\":\"");
    int npfx = 9;   // strlen("\"title\":\"")
    if (!np) { np = strstr(lastChat, "\"first_name\":\""); npfx = 14; }  // strlen("\"first_name\":\"")
    if (np)
    {
        np += npfx;
        int k = 0;
        while (np[k] && np[k] != '"' && k < (int)sizeof(name) - 1) { name[k] = np[k]; k++; }
        name[k] = '\0';
    }

    if (chatOut) { _snprintf(chatOut, chatLen - 1, "%lld", id); chatOut[chatLen - 1] = '\0'; }
    if (errOut)  { _snprintf(errOut, errLen - 1, name[0] ? "Found: %lld (%s)" : "Found: %lld", id, name); errOut[errLen - 1] = '\0'; }
    return TRUE;
}
