/*
** mqtt.cpp -- MQTT publisher for PDW
**
** Threading model:
**   Main/decoder thread  -> MqttNotify / MqttFlushGroup  -> g_queue (CRITICAL_SECTION + event)
**   Worker thread        -> dequeues jobs, publishes via Paho MQTT C synchronous API
**
** FLEX group batching:
**   Identical to webhook.cpp: ConvertGroupcall calls MqttNotify multiple times with
**   iConvertingGroupcall > 0, then calls MqttFlushGroup().  MqttNotify accumulates
**   in g_groupAcc[groupbit]; MqttFlushGroup enqueues the batch with bGroupTopic=TRUE.
**
** Paho library:
**   Win32 + x64: paho-mqtt3c-static.lib — statically linked, no DLL required.
**   Both libs must be built with /MD (CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL)
**   to match PDW's runtime; mismatched /MT libs cause LNK4006 CRT duplicates.
**
** SHA1 symbol collision:
**   paho-mqtt3c bundles its own embedded SHA1_Init/Update/Final (used only for
**   WebSocket handshake masking) without paho-prefix. These names collide with
**   OpenSSL's libcrypto_static.lib. We use plain tcp:// (no ws://) so paho never
**   actually calls its SHA1.  pdw_vs2017.vcxproj therefore:
**     1) orders libssl_static + libcrypto_static BEFORE paho-mqtt3c-static so
**        OpenSSL's SHA1 wins under /FORCE:MULTIPLE first-match resolution.
**     2) passes /FORCE:MULTIPLE /IGNORE:4006,4088 to accept the 3 SHA1 duplicates.
**   Switching to ws:// would re-expose paho's never-called SHA1 — at that point
**   rebuild paho with SHA1.c + WebSocket.c removed, or fork with renamed SHA1.
**
** TLS note: this file uses paho-mqtt3c (no TLS). If TLS support is needed,
**   switch to paho-mqtt3cs-static.lib (same /MD requirement) — the MQTTClient
**   API is identical, but the SHA1 collision becomes more serious because
**   paho's TLS path would also reach openssl's crypto.
*/

#ifndef STRICT
#define STRICT 1
#endif

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "..\headers\pdw.h"
#include "..\headers\initapp.h"
#include "mqtt.h"
#include "MQTTClient.h"
#include "logmanager.h"

extern TCHAR szPath[];   // from Initapp.cpp — PDW executable directory

// ---------------------------------------------------------------------------
// Configuration (read from Profile at MqttInit time; worker thread reads g_cs copies)
// ---------------------------------------------------------------------------

static char g_szBroker  [256]  = "";
static char g_szClientId[64]   = "PDW";
static char g_szUser    [64]   = "";
static char g_szPassword[128]  = "";
static char g_szTopic   [256]  = "pdw";
static int  g_iPort            = 1883;
static int  g_iQos             = 0;
static int  g_iRetain          = 0;
static BOOL g_bLogToFile       = FALSE;
static BOOL g_bPadCapcodes     = FALSE;
static BOOL g_bFlatJson        = FALSE;
static int  g_iMqttFields      = 0x7F;
static int  g_iTopicSuffix     = 0;    // 0=base topic only, 1=base/{capcode}

// Field bitmask positions (matches Profile.mqttFields, same layout as webhookFields)
#define MHF_LABEL         (1<<0)
#define MHF_TIME          (1<<1)
#define MHF_DATE          (1<<2)
#define MHF_TIMESTAMP     (1<<3)
#define MHF_MODE          (1<<4)
#define MHF_TYPE          (1<<5)
#define MHF_BITRATE       (1<<6)
#define MHF_LABEL_PERCAP  (1<<7)
#define MHF_LABEL_ARRAY   (1<<8)

// ---------------------------------------------------------------------------
// Job queue  (ring buffer, capacity = MQTT_QUEUE_SIZE)
// ---------------------------------------------------------------------------

#define MQTT_QUEUE_SIZE       64
// FIX [MqttReconnHarden]: 4 publish/connect attempts per message with exponential backoff
// (1/2/4 s between them). Mirrors the webhook feed's taaie retry-profiel so a brief broker
// stall (HA add-on reload/backup) is ridden out instead of dropping the message.
#define MQTT_MAX_ATTEMPTS     4
// FIX [MqttGroupTrunc]: ADDR_LEN 512 -> 2048 en SUBSCRIBERS_LEN 2048 -> 32768 (gelijk aan
// MySQL). Grote groepsoproepen (bv. GROUP15 met ~80 capcodes) overschreden beide buffers:
// de spatie-gescheiden adreslijst kapte af na ~64 capcodes (data-verlies) en de subscribers-
// JSON-array kapte midden in een object af, waarna MqttFlushGroup er blind een ']' achter
// plakte -> ongeldige JSON die Node-RED/HA niet parst. 32768 dekt ~170 lange-label capcodes.
#define MQTT_ADDR_LEN         2048
#define MQTT_MSG_LEN          (MAX_STR_LEN + 64)
#define MQTT_SUBSCRIBERS_LEN  32768

typedef struct {
    char     szAddress     [MQTT_ADDR_LEN];
    char     szMessage     [MQTT_MSG_LEN];
    char     szLabel       [512];
    char     szTime        [32];
    char     szDate        [32];
    char     szMode        [32];
    char     szType        [32];
    char     szBitrate     [32];
    char     szSubscribers [MQTT_SUBSCRIBERS_LEN];
    LONGLONG lTimestamp;
    BOOL     bGroupTopic;   // TRUE → publish to {topic}/group; FALSE → {topic}/{capcode}
} MqttJob;

static MqttJob g_queue[MQTT_QUEUE_SIZE];
static int     g_qHead = 0;
static int     g_qTail = 0;

static BOOL inline QueueFull(void)  { return ((g_qTail + 1) % MQTT_QUEUE_SIZE) == g_qHead; }
static BOOL inline QueueEmpty(void) { return g_qHead == g_qTail; }

// ---------------------------------------------------------------------------
// Per-groupbit accumulator for FLEX group batching
// ---------------------------------------------------------------------------

#define MAX_GROUPBITS 17

typedef struct {
    BOOL     active;
    char     szMessage     [MQTT_MSG_LEN];
    char     szAddresses   [MQTT_ADDR_LEN];
    char     szLabels      [512];
    char     szSubscribers [MQTT_SUBSCRIBERS_LEN];
    char     szTime        [32];
    char     szDate        [32];
    char     szMode        [32];
    char     szType        [32];
    char     szBitrate     [32];
    LONGLONG lTimestamp;
} MqttGroupAcc;

static MqttGroupAcc g_groupAcc[MAX_GROUPBITS];

// ---------------------------------------------------------------------------
// Thread / synchronisation
// ---------------------------------------------------------------------------

static HANDLE          g_hThread  = NULL;
static HANDLE          g_hEvent   = NULL;   // auto-reset, wakes worker
// FIX [MqttVolatile]: volatile so the compiler cannot hoist the read out of the worker
// loop body — main thread writes FALSE on shutdown, worker must see it promptly.
static volatile BOOL   g_bRunning = FALSE;

static CRITICAL_SECTION g_cs;

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
    if (hWnd) PostMessage(hWnd, WM_MQTT_STATUS, (WPARAM)status, lp);
}

// ---------------------------------------------------------------------------
// Log file  (separate critical section)
// ---------------------------------------------------------------------------

static void WriteLog(const char *fmt, ...)
{
    if (!g_bLogToFile) return;

    char szLine[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(szLine, sizeof(szLine), _TRUNCATE, fmt, ap);
    va_end(ap);

    PDW_MQTTLOG("%s", szLine);
}

// ---------------------------------------------------------------------------
// JSON helpers (local copy — identical to webhook.cpp)
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
        // FIX [JSON UTF-8]: escape control chars (<0x20) AND high bytes (>=0x7F) as
        // \u00XX. ASTRID/encrypted messages carry raw non-UTF-8 bytes (e.g. orphan
        // 0xBB); emitting them raw makes the payload invalid UTF-8, so Node-RED's
        // MQTT-in node delivers a byte array instead of a parsed object. Pure-ASCII
        // \u escapes are always valid UTF-8 -> always parsed as JSON. For Latin-1
        // labels \u00XX is the correct code point, so accented text stays intact.
        else if (c < 32 || c >= 0x7F)
        {
            char esc[8];
            sprintf(esc, "\\u%04X", c);
            for (int k = 0; esc[k] && *pos < maxLen - 2; k++) dst[(*pos)++] = esc[k];
        }
        else dst[(*pos)++] = (char)c;
    }
}

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

static void AppendJSONRaw(char *dst, int *pos, int maxLen, const char *key, const char *raw)
{
    const char *pfx = ",\"";
    for (int i = 0; pfx[i] && *pos < maxLen - 2; i++) dst[(*pos)++] = pfx[i];
    for (int i = 0; key[i] && *pos < maxLen - 2; i++) dst[(*pos)++] = key[i];
    if (*pos < maxLen - 2) dst[(*pos)++] = '"';
    if (*pos < maxLen - 2) dst[(*pos)++] = ':';
    for (int i = 0; raw[i] && *pos < maxLen - 2; i++) dst[(*pos)++] = raw[i];
}

static void AppendSubscriberEntry(char *dst, int *pos, int maxLen,
                                   const char *capcode, const char *label, BOOL isFirst)
{
    if (!isFirst && *pos < maxLen - 2) dst[(*pos)++] = ',';
    const char *open = "{\"address\":\"";
    for (int i = 0; open[i] && *pos < maxLen - 2; i++) dst[(*pos)++] = open[i];
    if (g_bPadCapcodes)
    {
        int len = (int)strlen(capcode);
        int nz  = (len < 9) ? (9 - len) : 0;
        for (int z = 0; z < nz && *pos < maxLen - 2; z++) dst[(*pos)++] = '0';
    }
    for (int i = 0; capcode[i] && *pos < maxLen - 2; i++) dst[(*pos)++] = capcode[i];
    const char *mid = "\",\"label\":\"";
    for (int i = 0; mid[i] && *pos < maxLen - 2; i++) dst[(*pos)++] = mid[i];
    AppendEscaped(dst, pos, maxLen, label ? label : "");
    const char *close = "\"}";
    for (int i = 0; close[i] && *pos < maxLen - 2; i++) dst[(*pos)++] = close[i];
}

// PDW-native JSON: {"payload":"...","data":{"new_state":{"state":"...","attributes":{...}}}}
static void BuildJSON(char *dst, int maxLen, const char *address, const MqttJob *job)
{
    int p = 0;
    const char *pre = "{\"payload\":\"";
    for (int i = 0; pre[i] && p < maxLen - 2; i++) dst[p++] = pre[i];
    AppendEscaped(dst, &p, maxLen, job->szMessage);

    const char *mid1 = "\",\"data\":{\"new_state\":{\"state\":\"";
    for (int i = 0; mid1[i] && p < maxLen - 2; i++) dst[p++] = mid1[i];
    AppendEscaped(dst, &p, maxLen, job->szMessage);

    if (g_iMqttFields & MHF_LABEL_ARRAY)
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
        if (g_iMqttFields & (MHF_LABEL | MHF_LABEL_PERCAP)) AppendJSONStr(dst, &p, maxLen, "label", job->szLabel);
    }

    if (g_iMqttFields & MHF_TIME)      AppendJSONStr(dst, &p, maxLen, "time",      job->szTime);
    if (g_iMqttFields & MHF_DATE)      AppendJSONStr(dst, &p, maxLen, "date",      job->szDate);
    if (g_iMqttFields & MHF_TIMESTAMP) AppendJSONNum(dst, &p, maxLen, "timestamp", job->lTimestamp);
    if (g_iMqttFields & MHF_MODE)      AppendJSONStr(dst, &p, maxLen, "mode",      job->szMode);
    if (g_iMqttFields & MHF_TYPE)      AppendJSONStr(dst, &p, maxLen, "type",      job->szType);
    if (g_iMqttFields & MHF_BITRATE)   AppendJSONStr(dst, &p, maxLen, "bitrate",   job->szBitrate);

    const char *suf = "}}}}";
    for (int i = 0; suf[i] && p < maxLen - 2; i++) dst[p++] = suf[i];
    dst[p] = '\0';
}

// Flat / Node-RED JSON: {"message":"...","address":"...",...}
static void BuildJSONFlat(char *dst, int maxLen, const char *address, const MqttJob *job)
{
    int p = 0;
    const char *pre = "{\"message\":\"";
    for (int i = 0; pre[i] && p < maxLen - 2; i++) dst[p++] = pre[i];
    AppendEscaped(dst, &p, maxLen, job->szMessage);

    if (g_iMqttFields & MHF_LABEL_ARRAY)
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
        if (g_iMqttFields & (MHF_LABEL | MHF_LABEL_PERCAP)) AppendJSONStr(dst, &p, maxLen, "label", job->szLabel);
    }

    if (g_iMqttFields & MHF_TIME)      AppendJSONStr(dst, &p, maxLen, "time",      job->szTime);
    if (g_iMqttFields & MHF_DATE)      AppendJSONStr(dst, &p, maxLen, "date",      job->szDate);
    if (g_iMqttFields & MHF_TIMESTAMP) AppendJSONNum(dst, &p, maxLen, "timestamp", job->lTimestamp);
    if (g_iMqttFields & MHF_MODE)      AppendJSONStr(dst, &p, maxLen, "mode",      job->szMode);
    if (g_iMqttFields & MHF_TYPE)      AppendJSONStr(dst, &p, maxLen, "type",      job->szType);
    if (g_iMqttFields & MHF_BITRATE)   AppendJSONStr(dst, &p, maxLen, "bitrate",   job->szBitrate);

    if (p < maxLen - 2) dst[p++] = '}';
    dst[p] = '\0';
}

// Pad space-separated capcodes in src to 9 digits.
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

static void AppendLabel(char *dst, int dstLen, const char *label)
{
    if (!label || !label[0]) return;
    int cur = (int)strlen(dst);
    int lab = (int)strlen(label);
    if (cur >= lab && strcmp(dst + cur - lab, label) == 0) return;
    if (cur == 0)
        strncpy(dst, label, dstLen - 1);
    else if (cur + 2 + lab < dstLen - 1)
    {
        strcat(dst, ", ");
        strcat(dst, label);
    }
}

static LONGLONG GetUnixTimestamp(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart  = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return (LONGLONG)((uli.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

// ---------------------------------------------------------------------------
// Paho MQTT client — worker thread state (no locking needed, single thread)
// ---------------------------------------------------------------------------

static MQTTClient g_mqttClient = NULL;

static void ClientDestroy(void)
{
    if (g_mqttClient)
    {
        MQTTClient_disconnect(g_mqttClient, 2000);
        MQTTClient_destroy(&g_mqttClient);
        g_mqttClient = NULL;
    }
}

static BOOL ClientCreate(void)
{
    if (g_mqttClient) return TRUE;

    char szURI[320];
    _snprintf(szURI, sizeof(szURI) - 1, "tcp://%s:%d", g_szBroker, g_iPort);
    szURI[sizeof(szURI) - 1] = '\0';

    char szId[80];
    strncpy(szId, g_szClientId[0] ? g_szClientId : "PDW", sizeof(szId) - 1);
    szId[sizeof(szId) - 1] = '\0';

    int rc = MQTTClient_create(&g_mqttClient, szURI, szId,
                                MQTTCLIENT_PERSISTENCE_NONE, NULL);
    return (rc == MQTTCLIENT_SUCCESS && g_mqttClient != NULL);
}

static BOOL ClientConnect(void)
{
    if (!g_mqttClient && !ClientCreate()) return FALSE;

    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    // FIX [MqttWarmConn]: keepAlive 60 -> 45 s. Paho's sync client pings every keepAlive
    // interval; 45 s keeps us well under Mosquitto's 1.5x grace (90 s at old 60, now 67 s)
    // and under any NAT idle timeout, and detects a truly dead socket sooner.
    conn_opts.keepAliveInterval = 45;
    conn_opts.cleansession      = 1;
    // FIX [MqttReconnHarden]: connectTimeout 5 -> 10 s, matching the webhook feed's WinHTTP
    // connect timeout (webhook.cpp WinHttpSetTimeouts 10000). A brief broker stall (e.g. an
    // HA add-on reload / backup window) that WinHTTP rides out was exceeding Paho's 5 s and
    // dropping the message on the sparse nighttime heartbeat.
    conn_opts.connectTimeout    = 10;

    if (g_szUser[0])
    {
        conn_opts.username = g_szUser;
        conn_opts.password = g_szPassword;
    }

    int rc = MQTTClient_connect(g_mqttClient, &conn_opts);
    return (rc == MQTTCLIENT_SUCCESS);
}

static BOOL EnsureConnected(void)
{
    if (g_mqttClient && MQTTClient_isConnected(g_mqttClient)) return TRUE;
    return ClientConnect();
}

// FIX [MqttRcText]: translate the common Paho MQTTClient return codes to a short label so the log is
// self-explanatory without a Paho lookup (e.g. rc=-3 reads DISCONNECTED). Unknown codes fall back to
// a generic string; the numeric rc is always printed alongside it.
static const char *MqttRcText(int rc)
{
    switch (rc)
    {
    case  0: return "SUCCESS";
    case -1: return "FAILURE";          // MQTTCLIENT_FAILURE (e.g. socket/timeout)
    case -3: return "DISCONNECTED";     // MQTTCLIENT_DISCONNECTED (half-open / dropped)
    case -4: return "MAX_INFLIGHT";
    case -9: return "BAD_QOS";
    default: return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Send one job  (2 attempts: try → reconnect → retry)
// ---------------------------------------------------------------------------

static void DoSend(const MqttJob *job)
{
    // Apply capcode padding
    char szAddress[MQTT_ADDR_LEN];
    if (g_bPadCapcodes)
        PadAddresses(szAddress, sizeof(szAddress), job->szAddress);
    else
    {
        strncpy(szAddress, job->szAddress, sizeof(szAddress) - 1);
        szAddress[sizeof(szAddress) - 1] = '\0';
    }

    // Build JSON payload.
    // FIX [JSON UTF-8]: \u00XX escaping expands each byte up to 6 chars and the
    // PDW-native format emits the message twice; size for that worst case so an
    // encrypted/binary or long assembled message is never truncated into invalid JSON.
    // FIX [MqttGroupTrunc]: static i.p.v. stack — met SUBSCRIBERS_LEN=32768 zou deze buffer
    // ~108 KB op de stack zijn. DoSend draait uitsluitend op de worker-thread, dus static is
    // veilig en houdt de threadstack klein.
    static char jsonBody[2 * 6 * MQTT_MSG_LEN + 6 * MQTT_ADDR_LEN + MQTT_SUBSCRIBERS_LEN + 1024];
    if (g_bFlatJson)
        BuildJSONFlat(jsonBody, sizeof(jsonBody), szAddress, job);
    else
        BuildJSON(jsonBody, sizeof(jsonBody), szAddress, job);
    int bodyLen = (int)strlen(jsonBody);

    // Build topic
    char szTopic[640];
    if (g_iTopicSuffix == 0)
    {
        // All messages to the base topic — single sensor, overwrites each time
        strncpy(szTopic, g_szTopic, sizeof(szTopic) - 1);
    }
    else if (job->bGroupTopic)
    {
        _snprintf(szTopic, sizeof(szTopic) - 1, "%s/group", g_szTopic);
    }
    else
    {
        _snprintf(szTopic, sizeof(szTopic) - 1, "%s/%s", g_szTopic, szAddress);
    }
    szTopic[sizeof(szTopic) - 1] = '\0';

    PostStatus(MHS_SENDING, 0);

    // FIX [MqttReconnLog]: een mislukte poging gevolgd door een geslaagde retry is GEEN fout
    // maar routine — de broker/NAT/firewall verbreekt de idle TCP-sessie buiten ons om, waarna de
    // eerstvolgende publish een stale socket raakt (rc=-1). De retry reconnect en slaagt vrijwel
    // altijd. We weten zeker dat het om reconnect-en-niet-een-nieuw-bericht gaat omdat alle
    // pogingen binnen DEZELFDE DoSend()-call voor HETZELFDE bericht vallen (een volgend pagingbericht
    // is een aparte DoSend met eigen SENT-regel). De attempt-teller bepaalt de severity:
    //   attempt < laatste mislukt  -> RECONNECT (rustig, verwacht)
    //   laatste attempt mislukt    -> ERROR     (alle pogingen falen = echte storing)
    // FIX [MqttReconnHarden]: 2 -> 3 pogingen met exponentiele backoff 1/2/4 s (was 1 vaste 1 s),
    // gelijk aan de webhook-feed (webhook.cpp g_retryDelays {1000,2000,4000}). De webhook cold-connect
    // per bericht en is toch betrouwbaar juist door dit taaiere retry-profiel; MQTT was krapper
    // afgesteld (2 pogingen, 1 s) en dropte daarom af en toe een bericht dat de webhook wel afleverde.
    static const DWORD s_retryDelays[MQTT_MAX_ATTEMPTS - 1] = { 1000, 2000, 4000 };
    for (int attempt = 0; attempt < MQTT_MAX_ATTEMPTS; attempt++)
    {
        BOOL bLast = (attempt == MQTT_MAX_ATTEMPTS - 1);

        if (attempt > 0)
        {
            // FIX [MqttReconnHarden]: don't burn the retry back-off sleeps once a
            // shutdown/reconfigure is in progress (mirrors webhook.cpp). The first attempt
            // (attempt 0) still runs during the exit-flush so queued messages get one send try.
            if (!g_bRunning) break;
            PostStatus(MHS_RETRY, attempt);
            ClientDestroy();
            Sleep(s_retryDelays[attempt - 1]);
        }

        if (!EnsureConnected())
        {
            if (bLast) WriteLog("ERROR     connect failed: %s:%d", g_szBroker, g_iPort);
            else       WriteLog("RECONNECT connect failed - retrying: %s:%d", g_szBroker, g_iPort);
            continue;
        }

        MQTTClient_deliveryToken token = 0;
        int rc = MQTTClient_publish(g_mqttClient, szTopic,
                                     bodyLen, jsonBody,
                                     g_iQos, g_iRetain, &token);

        if (rc == MQTTCLIENT_SUCCESS)
        {
            // FIX [MqttWaitCheck]: waitForCompletion can return MQTTCLIENT_DISCONNECTED or
            // MQTTCLIENT_FAILURE (timeout) — previously its return was ignored and "SENT"
            // was logged unconditionally, hiding a QoS>0 delivery failure on a flaky broker.
            if (g_iQos > 0)
            {
                int wrc = MQTTClient_waitForCompletion(g_mqttClient, token, 5000);
                if (wrc != MQTTCLIENT_SUCCESS)
                {
                    // FIX [MqttWaitRetry]: a QoS>0 publish() that returns SUCCESS but whose ack never
                    // arrives (waitForCompletion rc=-3 DISCONNECTED / rc=-1 timeout) is the exact
                    // failure observed on the first message after an idle gap: the socket looked alive,
                    // publish() queued locally, but the half-open connection dropped before the PUBACK.
                    // Previously this returned immediately -> the message was lost even with retries
                    // available. Now we drop the stale handle and RETRY on a fresh connection (the QoS
                    // guarantee is at-least-once, so a possible duplicate is acceptable, a miss is not).
                    ClientDestroy();
                    if (bLast)
                    {
                        WriteLog("ERROR     QoS>0 waitForCompletion rc=%d (%s) topic=%s (delivery unconfirmed after retries)", wrc, MqttRcText(wrc), szTopic);
                        break;
                    }
                    WriteLog("RECONNECT QoS>0 waitForCompletion rc=%d (%s) (ack lost) - reconnecting", wrc, MqttRcText(wrc));
                    continue;
                }
            }
            WriteLog("SENT      %s (%d bytes)", szTopic, bodyLen);
            PostStatus(MHS_OK, 0);
            return;
        }

        if (bLast) WriteLog("ERROR     publish failed rc=%d (%s) topic=%s (after reconnect)", rc, MqttRcText(rc), szTopic);
        else       WriteLog("RECONNECT publish rc=%d (%s) (stale connection) - reconnecting", rc, MqttRcText(rc));
    }

    PostStatus(MHS_ERROR, 0);
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

// FIX [MqttWarmConn]: NO proactive idle disconnect. The previous 3-minute idle ClientDestroy()
// forced a COLD reconnect on every sparse nighttime heartbeat (e.g. a 5-min "test call"), and that
// reconnect is the single fragile operation (name resolution, TCP handshake, CONNECT/CONNACK, auth).
// Every other MQTT client on the broker (e.g. a Pi) holds one connection open with keepalive and never
// reconnects during quiet periods, so it rides out brief broker stalls (HA add-on reload/backup) within
// the keepalive grace. PDW was the only client that tore down and cold-reconnected ~288x/day.
//
// FIX [MqttKeepAlive]: keeping the connection open is only half the story. The Paho *synchronous*
// MQTTClient does NOT send keepalive pings on its own - its own header states you must call
// MQTTClient_yield() (or MQTTClient_receive(), or set async callbacks) periodically "to send MQTT
// keepalive pings". PDW is a pure publisher that did none of these, so broker logs showed the PDW
// session being reaped every ~1.5x keepAlive ("exceeded timeout") during any quiet gap, and the next
// message then hit a dead socket (waitForCompletion rc=-3). We now call MQTTClient_yield() about every
// 10 s while connected so PINGREQ actually goes out and the session survives the multi-minute gaps.
// yield() blocks only ~100 ms and stays in synchronous mode, so MQTTClient_waitForCompletion() in
// DoSend keeps working. A genuinely dead socket (real broker restart) is still detected and reconnected
// on the next publish via EnsureConnected, with the hardened 4-attempt/10 s retry profile as backstop.
#define MQTT_YIELD_INTERVAL_MS  10000u

static DWORD WINAPI WorkerThreadProc(LPVOID)
{
    ULONGLONG dwLastYieldMs = 0;

    while (g_bRunning)
    {
        WaitForSingleObject(g_hEvent, 200);

        if (!g_bRunning) break;

        while (TRUE)
        {
            MqttJob job;
            BOOL bHaveJob = FALSE;

            EnterCriticalSection(&g_cs);
            if (!QueueEmpty())
            {
                job = g_queue[g_qHead];
                g_qHead = (g_qHead + 1) % MQTT_QUEUE_SIZE;
                bHaveJob = TRUE;
            }
            LeaveCriticalSection(&g_cs);

            if (!bHaveJob) break;
            DoSend(&job);
        }

        // FIX [MqttKeepAlive]: drive Paho's keepalive so the idle connection is not reaped by the
        // broker. yield() only emits a PINGREQ when one is actually due, so the ~10 s cadence is safe;
        // it blocks ~100 ms, after which we loop and any freshly queued job is picked up immediately.
        if (g_mqttClient && MQTTClient_isConnected(g_mqttClient))
        {
            ULONGLONG now = GetTickCount64();
            if (dwLastYieldMs == 0 || (now - dwLastYieldMs) >= MQTT_YIELD_INTERVAL_MS)
            {
                MQTTClient_yield();
                dwLastYieldMs = now;
            }
        }
    }

    // Flush on exit
    // FIX [FlushLockRace]: dequeue under g_cs, same as the main loop. g_bRunning is already
    // FALSE here, but a producer that passed its unlocked g_bRunning check just before shutdown
    // can still be inside EnterCriticalSection writing g_qTail/g_queue while we drain — reading
    // the ring unlocked was a data race on shared state.
    for (;;)
    {
        MqttJob job;
        BOOL bHaveJob = FALSE;
        EnterCriticalSection(&g_cs);
        if (!QueueEmpty())
        {
            job = g_queue[g_qHead];
            g_qHead = (g_qHead + 1) % MQTT_QUEUE_SIZE;
            bHaveJob = TRUE;
        }
        LeaveCriticalSection(&g_cs);
        if (!bHaveJob) break;
        DoSend(&job);
    }

    ClientDestroy();
    return 0;
}

// ---------------------------------------------------------------------------
// Queue helper (call with g_cs held)
// ---------------------------------------------------------------------------

// FIX [QueueDrop]: tel weggegooide jobs bij volle queue.
static unsigned g_droppedJobs = 0;

static void EnqueueLocked(const MqttJob *job)
{
    if (QueueFull())
    {
        // FIX [QueueDrop]: bij burst > MQTT_QUEUE_SIZE werd de job voorheen stil
        // verworpen, zonder log of teller — dataverlies bleef onzichtbaar.
        g_droppedJobs++;
        WriteLog("DROP queue full — message discarded (total dropped=%u)", g_droppedJobs);
        return;
    }
    g_queue[g_qTail] = *job;
    g_qTail = (g_qTail + 1) % MQTT_QUEUE_SIZE;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static BOOL s_mqttCsInit = FALSE; // FIX [L4]: file-scope so MqttDestroy() can check it

void MqttInit(void)
{
    if (!s_mqttCsInit)
    {
        InitializeCriticalSection(&g_cs);
        s_mqttCsInit = TRUE;
    }

    MqttShutdown();

    if (!Profile.mqttEnabled || !Profile.szMqttBroker[0]) return;

    // Copy config from Profile under lock (not strictly needed before thread start, but clean)
    strncpy(g_szBroker,   Profile.szMqttBroker,   sizeof(g_szBroker)   - 1);
    strncpy(g_szClientId, Profile.szMqttClientId, sizeof(g_szClientId) - 1);
    strncpy(g_szUser,     Profile.szMqttUser,     sizeof(g_szUser)     - 1);
    strncpy(g_szPassword, Profile.szMqttPassword, sizeof(g_szPassword) - 1);
    strncpy(g_szTopic,    Profile.szMqttTopic,    sizeof(g_szTopic)    - 1);
    g_szBroker  [sizeof(g_szBroker)   - 1] = '\0';
    g_szClientId[sizeof(g_szClientId) - 1] = '\0';
    g_szUser    [sizeof(g_szUser)     - 1] = '\0';
    g_szPassword[sizeof(g_szPassword) - 1] = '\0';
    g_szTopic   [sizeof(g_szTopic)    - 1] = '\0';

    if (!g_szTopic[0]) strcpy(g_szTopic, "pdw");

    g_iPort        = Profile.mqttPort  > 0 ? Profile.mqttPort : 1883;
    g_iQos         = Profile.mqttQos  >= 0 && Profile.mqttQos <= 2 ? Profile.mqttQos : 0;
    g_iRetain      = Profile.mqttRetain ? 1 : 0;
    g_bLogToFile   = Profile.mqttLogToFile    ? TRUE : FALSE;
    g_bPadCapcodes = Profile.mqttPadCapcodes  ? TRUE : FALSE;
    g_bFlatJson    = Profile.mqttFlatJson     ? TRUE : FALSE;
    g_iMqttFields  = Profile.mqttFields;
    g_iTopicSuffix = Profile.mqttTopicSuffix;

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

void MqttShutdown(void)
{
    if (!g_bRunning) return;

    g_bRunning = FALSE;
    if (g_hEvent) SetEvent(g_hEvent);

    if (g_hThread)
    {
        // FIX [ShutdownRace]: join to FULL completion before CloseHandle / (later) DeleteCriticalSection.
        // The old 5 s timeout returned while the worker could still be inside a Paho call, after which
        // CloseHandle + DeleteCriticalSection ran under a live thread (crash on exit) — and on a runtime
        // reconfigure MqttInit would start a SECOND worker sharing g_mqttClient/g_cs/g_queue. All Paho
        // calls here are timeout-bounded (connectTimeout=10s, waitForCompletion=5s, disconnect=2s) plus a
        // 200 ms event wait. FIX [MqttReconnHarden]: the multi-attempt back-off does NOT extend shutdown —
        // DoSend's `if (!g_bRunning) break` skips all retries once g_bRunning is FALSE, so during teardown
        // each queued job gets at most ONE ~17 s attempt (10 s connect + 5 s wait + 2 s disconnect); the
        // worker still returns quickly, INFINITE stays safe and correct.
        WaitForSingleObject(g_hThread, INFINITE);
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }

    if (g_hEvent) { CloseHandle(g_hEvent); g_hEvent = NULL; }

    ZeroMemory(g_groupAcc, sizeof(g_groupAcc));
    g_qHead = g_qTail = 0;
}

// FIX [L4]: final teardown — stops thread and releases the CRITICAL_SECTIONs.
// Call from WM_DESTROY instead of MqttShutdown().
void MqttDestroy(void)
{
    MqttShutdown();
    if (s_mqttCsInit)
    {
        DeleteCriticalSection(&g_cs);
        s_mqttCsInit = FALSE;
    }
}

void MqttNotify(const char *capcode, const char *message, const char *label,
                const char *szTime, const char *szDate,
                const char *szMode, const char *szType, const char *szBitrate,
                BOOL isGroup, int groupbit)
{
    if (!g_bRunning) return;

    EnterCriticalSection(&g_cs);

    if (isGroup && groupbit >= 0 && groupbit < MAX_GROUPBITS && !(g_iMqttFields & MHF_LABEL_PERCAP))
    {
        MqttGroupAcc *ga = &g_groupAcc[groupbit];
        if (!ga->active)
        {
            ga->active = TRUE;
            strncpy(ga->szMessage,   message, MQTT_MSG_LEN  - 1);
            ga->szMessage[MQTT_MSG_LEN - 1] = '\0';
            strncpy(ga->szAddresses, capcode, MQTT_ADDR_LEN - 1);
            ga->szAddresses[MQTT_ADDR_LEN - 1] = '\0';
            ga->szLabels[0] = '\0';
            AppendLabel(ga->szLabels, sizeof(ga->szLabels), label);
            strncpy(ga->szTime,    szTime,    sizeof(ga->szTime)    - 1);
            strncpy(ga->szDate,    szDate,    sizeof(ga->szDate)    - 1);
            strncpy(ga->szMode,    szMode,    sizeof(ga->szMode)    - 1);
            strncpy(ga->szType,    szType,    sizeof(ga->szType)    - 1);
            strncpy(ga->szBitrate, szBitrate, sizeof(ga->szBitrate) - 1);
            ga->lTimestamp = GetUnixTimestamp();
            if (g_iMqttFields & MHF_LABEL_ARRAY)
            {
                ga->szSubscribers[0] = '[';
                int sPos = 1;
                AppendSubscriberEntry(ga->szSubscribers, &sPos, MQTT_SUBSCRIBERS_LEN,
                                      capcode, label ? label : "", TRUE);
                ga->szSubscribers[sPos] = '\0';
            }
        }
        else
        {
            int len = (int)strlen(ga->szAddresses);
            if (len + 1 + (int)strlen(capcode) < MQTT_ADDR_LEN - 1)
            {
                ga->szAddresses[len] = ' ';
                strcpy(ga->szAddresses + len + 1, capcode);
            }
            AppendLabel(ga->szLabels, sizeof(ga->szLabels), label);
            if (g_iMqttFields & MHF_LABEL_ARRAY)
            {
                int sPos = (int)strlen(ga->szSubscribers);
                AppendSubscriberEntry(ga->szSubscribers, &sPos, MQTT_SUBSCRIBERS_LEN,
                                      capcode, label ? label : "", FALSE);
                ga->szSubscribers[sPos] = '\0';
            }
        }
        LeaveCriticalSection(&g_cs);
    }
    else
    {
        MqttJob job;
        ZeroMemory(&job, sizeof(job));
        strncpy(job.szAddress, capcode,           sizeof(job.szAddress)  - 1);
        strncpy(job.szMessage, message,           sizeof(job.szMessage)  - 1);
        strncpy(job.szLabel,   label  ? label  : "", sizeof(job.szLabel) - 1);
        strncpy(job.szTime,    szTime ? szTime : "", sizeof(job.szTime)  - 1);
        strncpy(job.szDate,    szDate ? szDate : "", sizeof(job.szDate)  - 1);
        strncpy(job.szMode,    szMode ? szMode : "", sizeof(job.szMode)  - 1);
        strncpy(job.szType,    szType ? szType : "", sizeof(job.szType)  - 1);
        strncpy(job.szBitrate, szBitrate ? szBitrate : "", sizeof(job.szBitrate) - 1);
        job.lTimestamp  = GetUnixTimestamp();
        job.bGroupTopic = FALSE;
        if (g_iMqttFields & MHF_LABEL_ARRAY)
        {
            job.szSubscribers[0] = '[';
            int sPos = 1;
            AppendSubscriberEntry(job.szSubscribers, &sPos, MQTT_SUBSCRIBERS_LEN,
                                  capcode, label ? label : "", TRUE);
            if (sPos < MQTT_SUBSCRIBERS_LEN - 1) { job.szSubscribers[sPos++] = ']'; job.szSubscribers[sPos] = '\0'; }
        }
        EnqueueLocked(&job);
        LeaveCriticalSection(&g_cs);
        SetEvent(g_hEvent);
    }
}

void MqttFlushGroup(int groupbit)
{
    if (!g_bRunning) return;
    if (groupbit < 0 || groupbit >= MAX_GROUPBITS) return;
    if (g_iMqttFields & MHF_LABEL_PERCAP) return;

    EnterCriticalSection(&g_cs);
    MqttGroupAcc *ga = &g_groupAcc[groupbit];
    if (ga->active)
    {
        MqttJob job;
        ZeroMemory(&job, sizeof(job));
        strncpy(job.szAddress, ga->szAddresses, sizeof(job.szAddress) - 1);
        strncpy(job.szMessage, ga->szMessage,   sizeof(job.szMessage) - 1);
        strncpy(job.szLabel,   ga->szLabels,    sizeof(job.szLabel)   - 1);
        strncpy(job.szTime,    ga->szTime,      sizeof(job.szTime)    - 1);
        strncpy(job.szDate,    ga->szDate,      sizeof(job.szDate)    - 1);
        strncpy(job.szMode,    ga->szMode,      sizeof(job.szMode)    - 1);
        strncpy(job.szType,    ga->szType,      sizeof(job.szType)    - 1);
        strncpy(job.szBitrate, ga->szBitrate,   sizeof(job.szBitrate) - 1);
        job.lTimestamp  = ga->lTimestamp;
        job.bGroupTopic = TRUE;
        if (g_iMqttFields & MHF_LABEL_ARRAY)
        {
            strncpy(job.szSubscribers, ga->szSubscribers, MQTT_SUBSCRIBERS_LEN - 2);
            job.szSubscribers[MQTT_SUBSCRIBERS_LEN - 2] = '\0';
            int sLen = (int)strlen(job.szSubscribers);
            if (sLen > 0) { job.szSubscribers[sLen++] = ']'; job.szSubscribers[sLen] = '\0'; }
        }
        ZeroMemory(ga, sizeof(*ga));
        EnqueueLocked(&job);
    }
    LeaveCriticalSection(&g_cs);
    SetEvent(g_hEvent);
}

void MqttSetStatusWnd(HWND hWnd)
{
    // FIX [StatusWndCsGuard]: g_cs may not be initialized yet if the Setup dialog calls this
    // before the first MqttInit(). Entering an uninitialized CRITICAL_SECTION is undefined
    // (crash). Mirror the guarded SqliteSetStatusWnd: store directly until the CS exists.
    if (!s_mqttCsInit) { g_hStatusWnd = hWnd; return; }
    EnterCriticalSection(&g_cs);
    g_hStatusWnd = hWnd;
    LeaveCriticalSection(&g_cs);
}

// FIX [ConnTest]: one-shot connection test for the Setup dialog. Uses a LOCAL MQTTClient so it never
// interferes with the running worker's g_mqttClient. Blocks up to ~5 s (connectTimeout).
BOOL MqttTestConnection(const char *broker, int port, const char *clientId,
                        const char *user, const char *pass, char *szMsg, int msgLen)
{
    if (!szMsg || msgLen < 1) return FALSE;
    szMsg[0] = '\0';

    if (!broker || !broker[0]) {
        _snprintf(szMsg, msgLen - 1, "No broker host entered.");
        szMsg[msgLen - 1] = '\0';
        return FALSE;
    }
    if (port <= 0) port = 1883;

    char szURI[320];
    _snprintf(szURI, sizeof(szURI) - 1, "tcp://%s:%d", broker, port);
    szURI[sizeof(szURI) - 1] = '\0';

    char szId[80];
    strncpy(szId, (clientId && clientId[0]) ? clientId : "PDW-test", sizeof(szId) - 1);
    szId[sizeof(szId) - 1] = '\0';

    MQTTClient cli = NULL;
    int rc = MQTTClient_create(&cli, szURI, szId, MQTTCLIENT_PERSISTENCE_NONE, NULL);
    if (rc != MQTTCLIENT_SUCCESS || !cli) {
        _snprintf(szMsg, msgLen - 1, "Could not create MQTT client (rc=%d).", rc);
        szMsg[msgLen - 1] = '\0';
        if (cli) MQTTClient_destroy(&cli);
        return FALSE;
    }

    MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
    opts.keepAliveInterval = 20;
    opts.cleansession      = 1;
    opts.connectTimeout    = 5;
    if (user && user[0]) {
        opts.username = (char *)user;
        opts.password = (char *)pass;
    }

    rc = MQTTClient_connect(cli, &opts);
    if (rc == MQTTCLIENT_SUCCESS) {
        MQTTClient_disconnect(cli, 1000);
        MQTTClient_destroy(&cli);
        _snprintf(szMsg, msgLen - 1, "Success!\n\nConnected to MQTT broker %s:%d.", broker, port);
        szMsg[msgLen - 1] = '\0';
        return TRUE;
    }

    MQTTClient_destroy(&cli);

    // Map the common Paho / MQTT CONNACK return codes to friendly text.
    const char *hint;
    switch (rc) {
    case 1:  hint = "broker refused the protocol version";            break;
    case 2:  hint = "client ID rejected by the broker";               break;
    case 3:  hint = "broker unavailable";                             break;
    case 4:  hint = "bad username or password";                       break;
    case 5:  hint = "not authorized (check username/password)";       break;
    default: hint = "connection failed (broker down, wrong host/port, or firewall)"; break;
    }
    _snprintf(szMsg, msgLen - 1, "Could not connect to %s:%d.\n\n%s (rc=%d).", broker, port, hint, rc);
    szMsg[msgLen - 1] = '\0';
    return FALSE;
}
