/*
** utils/sqlite_feed.cpp -- SQLite output feed for PDW (zero external DLLs)
**
** Engine: SQLite amalgamation (utils/sqlite/sqlite3.c) compiled in statically. No DLL,
** no install, no runtime dependency on the target machine.
**
** Schema: MySQL "Optimized" 1-to-1 (see utils/mysql.cpp BuildInsertOptimized). address is TEXT
** so ASTRID leading zeros are preserved (FIX [MysqlCapcodeChar]). No FULLTEXT -- the website
** searches with LIKE; plain B-tree indexes on address/received/match_type/label are sufficient.
**
** Safety: all values go through a PREPARED statement with bound parameters -- no
** string-escaping needed (unlike MySQL which interpolates values into SQL text).
**
** Threading model (identical to the MySQL feed):
**   Decoder-thread  -> SqliteNotify() -> ring buffer (CRITICAL_SECTION + event)
**   Worker-thread   -> dequeues jobs, binds + steps the prepared INSERT (batched per transaction).
**                      Owns the sqlite3* connection; no lock needed on the db itself.
**
** Best-practice defaults (overridable in the GUI):
**   journal_mode=WAL, synchronous=NORMAL, busy_timeout=5000, auto_vacuum=INCREMENTAL.
**
** NVMe write reduction (Profile.sqlite_lowWrite): synchronous=OFF + commit interval ~15 s +
**   higher wal_autocheckpoint. Far fewer write/fsync operations, but on crash/power loss the
**   last uncommitted batch may be lost. Intentional trade-off for users who want to spare SSD wear.
*/

#ifndef STRICT
#define STRICT 1
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#include "..\headers\pdw.h"
#include "sqlite_feed.h"
#include "sqlite\sqlite3.h"
#include "logmanager.h"

extern TCHAR szPath[];          /* PDW exe-directory — uit Initapp.cpp */

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define SQLITE_QUEUE_SIZE        64
#define SQLITE_SUBSCRIBERS_LEN   32768      /* JSON-array van groep-abonnees */
#define MAX_SQLITE_GROUPBITS     17         /* FLEX groupbits 0-15 + 1 spare */
#define SQLITE_OPEN_RETRY_MS     5000       /* heropen-interval na een open/IO-fout */
#define SQLITE_MAINT_INTERVAL_MS 3600000UL  /* purge/size maintenance: once per hour */
#define SQLITE_LOWWRITE_COMMIT_MS 15000     /* commit interval in NVMe low-write mode */

// ---------------------------------------------------------------------------
// Config snapshot (gekopieerd uit Profile bij SqliteInit)
// ---------------------------------------------------------------------------

static char  g_szPath  [MAX_PATH] = "";
static char  g_szTable [64]       = "messages";
static int   g_iFields            = SQF_ALL;
static BOOL  g_bLinefeed          = FALSE;   // 0xBB -> '\n' instead of guillemet, mirrors Profile.Linefeed
static BOOL  g_bLogToFile         = FALSE;
static BOOL  g_bLowWrite          = FALSE;   // NVMe-write-reductie (dataloss-risico)
static BOOL  g_bPurgeEnabled      = FALSE;
static int   g_iPurgeDays         = 30;
static int   g_iMaxSizeMB         = 0;       // 0 = uit

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
    char szSubscribers [SQLITE_SUBSCRIBERS_LEN]; /* JSON array, empty for non-group messages */
    int  iMatchType;                             /* 0=none, 1=filtered, 2=monitor-only */
    char szLabelColor  [8];                      /* "#RRGGBB" of "" */
} SqliteJob;

/* Per-groupbit accumulator for FLEX group calls (main/decoder-thread only). */
typedef struct {
    BOOL active;
    char szMessage [MAX_STR_LEN + 4];
    char szTime    [16];
    char szDate    [16];
    char szMode    [32];
    char szType    [16];
    char szBitrate [16];
    char szSubscr  [SQLITE_SUBSCRIBERS_LEN];
    int  sPos;
    int  nSubscr;
    int  iMatchType;
    char szLabelColor[8];
} SqliteGroupAcc;

static SqliteGroupAcc g_groupAcc[MAX_SQLITE_GROUPBITS];

static SqliteJob g_queue[SQLITE_QUEUE_SIZE];
static int       g_qHead = 0;
static int       g_qTail = 0;
static unsigned  g_dropped = 0;

static BOOL inline QueueFull(void)  { return ((g_qTail + 1) % SQLITE_QUEUE_SIZE) == g_qHead; }
static BOOL inline QueueEmpty(void) { return g_qHead == g_qTail; }

// ---------------------------------------------------------------------------
// Thread / synchronisatie
// ---------------------------------------------------------------------------

static HANDLE           g_hThread  = NULL;
static HANDLE           g_hEvent   = NULL;
static volatile BOOL    g_bRunning = FALSE;
static CRITICAL_SECTION g_cs;
static BOOL             g_bCsInit  = FALSE;

static HWND             g_hStatusWnd = NULL;   /* beschermd door g_cs */

// ---------------------------------------------------------------------------
// Database (worker-thread only -- no lock needed)
// ---------------------------------------------------------------------------

static sqlite3      *g_db     = NULL;
static sqlite3_stmt *g_insert = NULL;
static BOOL          g_bTxnOpen = FALSE;

// ---------------------------------------------------------------------------
// Logbestand
// ---------------------------------------------------------------------------

// Log via LogManager (daily rotation, write buffering, uniform timestamp).
static void WriteLog(const char *fmt, ...)
{
    if (!g_bLogToFile) return;

    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);

    PDW_SQLITELOG("%s", line);
}

// ---------------------------------------------------------------------------
// Status-venster
// ---------------------------------------------------------------------------

static void PostStatus(int status)
{
    HWND hWnd;
    EnterCriticalSection(&g_cs);
    hWnd = g_hStatusWnd;
    LeaveCriticalSection(&g_cs);
    if (hWnd) PostMessage(hWnd, WM_SQLITE_STATUS, (WPARAM)status, 0);
}

void SqliteSetStatusWnd(HWND hWnd)
{
    if (!g_bCsInit) { g_hStatusWnd = hWnd; return; }
    EnterCriticalSection(&g_cs);
    g_hStatusWnd = hWnd;
    LeaveCriticalSection(&g_cs);
}

// ===========================================================================
// Tekst-helpers (overgenomen uit utils/mysql.cpp — identiek gedrag)
// ===========================================================================

static int Utf8SeqLen(const unsigned char *p)
{
    unsigned char c = p[0];
    if (c < 0x80) return 1;
    int n;
    if      ((c & 0xE0) == 0xC0) n = 1;
    else if ((c & 0xF0) == 0xE0) n = 2;
    else if ((c & 0xF8) == 0xF0) n = 3;
    else return 0;
    for (int k = 1; k <= n; k++)
        if ((p[k] & 0xC0) != 0x80) return 0;
    return n + 1;
}

/* Sanitize an arbitrary byte string to valid UTF-8 (PDW text is not clean UTF-8). */
static void Utf8Sanitize(char *dst, int dstLen, const char *src, BOOL bLinefeed)
{
    int j = 0;
    const unsigned char *p = (const unsigned char *)src;
    while (*p) {
        if (*p == 0xBB) {                         /* PDW-linefeedmarker */
            if (bLinefeed) { if (j > dstLen - 2) break; dst[j++] = '\n'; }
            else           { if (j > dstLen - 3) break; dst[j++] = (char)0xC2; dst[j++] = (char)0xBB; }
            p++;
            continue;
        }
        int n = Utf8SeqLen(p);
        if (n == 0) {
            if (j > dstLen - 2) break;
            dst[j++] = '?';
            p++;
        } else {
            if (j + n > dstLen - 1) break;
            for (int k = 0; k < n; k++) dst[j++] = (char)p[k];
            p += n;
        }
    }
    dst[j] = '\0';
}

/* Escape a string for use in a JSON double-quoted value (RFC 7159) -- used for the subscribers column. */
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
        else if (c < 32)    { p++; }
        else if (c < 0x80)  { dst[j++] = (char)c; p++; }
        else {
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

/* PDW datum "dd-MM-yy" + tijd "HH:mm:ss" -> "YYYY-MM-DD HH:MM:SS" (sorteerbaar als tekst). */
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

/* Maak een identifier (tabelnaam) veilig binnen "..."-quoting: gooi ", control chars en spaties weg. */
static void SanitizeIdent(char *s)
{
    char *pSrc, *pDst;
    for (pSrc = pDst = s; *pSrc; pSrc++)
        if (*pSrc != '"' && *pSrc != ';' && (unsigned char)*pSrc > ' ') *pDst++ = *pSrc;
    *pDst = '\0';
}

// ===========================================================================
// Database-laag (worker-thread)
// ===========================================================================

/* Maak het Optimized-schema (idempotent). Vult err op fout. Geeft SQLITE_OK terug. */
static int CreateSchema(sqlite3 *db, const char *table, char *err, int errLen)
{
    char sql[2048];
    _snprintf(sql, sizeof(sql) - 1,
        "CREATE TABLE IF NOT EXISTS \"%s\" ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " received TEXT NOT NULL DEFAULT '',"      /* 'YYYY-MM-DD HH:MM:SS' */
        " address TEXT NOT NULL DEFAULT '',"         /* CHAR(9)-equivalent: voorloopnullen intact */
        " mode TEXT NOT NULL DEFAULT '',"
        " msg_type TEXT NOT NULL DEFAULT '',"
        " bitrate INTEGER NOT NULL DEFAULT 0,"
        " message TEXT NOT NULL DEFAULT '',"
        " label TEXT NOT NULL DEFAULT '',"
        " subscribers TEXT NOT NULL DEFAULT '',"
        " match_type INTEGER NOT NULL DEFAULT 0,"
        " label_color TEXT NOT NULL DEFAULT '');"
        "CREATE INDEX IF NOT EXISTS \"idx_%s_address\"   ON \"%s\"(address);"
        "CREATE INDEX IF NOT EXISTS \"idx_%s_received\" ON \"%s\"(received);"
        "CREATE INDEX IF NOT EXISTS \"idx_%s_match\"     ON \"%s\"(match_type);"
        "CREATE INDEX IF NOT EXISTS \"idx_%s_label\"     ON \"%s\"(label);",
        table, table, table, table, table, table, table, table, table);
    sql[sizeof(sql) - 1] = '\0';

    char *e = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &e);
    if (rc != SQLITE_OK && err) {
        _snprintf(err, errLen - 1, "%s", e ? e : sqlite3_errmsg(db));
        err[errLen - 1] = '\0';
    }
    if (e) sqlite3_free(e);
    return rc;
}

/* Pas de pragma's toe op basis van de write-modus. db-laag, worker-thread. */
static void ApplyPragmas(sqlite3 *db, BOOL lowWrite)
{
    sqlite3_busy_timeout(db, 5000);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA temp_store=MEMORY;", NULL, NULL, NULL);
    if (lowWrite) {
        // NVMe write reduction: no fsync per commit, fewer/larger checkpoints.
        sqlite3_exec(db, "PRAGMA synchronous=OFF;",        NULL, NULL, NULL);
        sqlite3_exec(db, "PRAGMA wal_autocheckpoint=10000;", NULL, NULL, NULL);
    } else {
        // Durable best-practice: NORMAL is safe with WAL (only vulnerable for the last txn
        // on OS/power failure, not on app crash).
        sqlite3_exec(db, "PRAGMA synchronous=NORMAL;",     NULL, NULL, NULL);
        sqlite3_exec(db, "PRAGMA wal_autocheckpoint=1000;",  NULL, NULL, NULL);
    }
}

/* Prepare the INSERT statement for all 11 columns (fixed statement text). */
static int PrepareInsert(sqlite3 *db, const char *table, sqlite3_stmt **stmt)
{
    char sql[512];
    _snprintf(sql, sizeof(sql) - 1,
        "INSERT INTO \"%s\""
        "(received,address,mode,msg_type,bitrate,message,label,subscribers,match_type,label_color)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);",
        table);
    sql[sizeof(sql) - 1] = '\0';
    return sqlite3_prepare_v2(db, sql, -1, stmt, NULL);
}

static void TxnBegin(void)
{
    if (g_db && !g_bTxnOpen) {
        if (sqlite3_exec(g_db, "BEGIN;", NULL, NULL, NULL) == SQLITE_OK) g_bTxnOpen = TRUE;
    }
}

static void TxnCommit(void)
{
    if (g_db && g_bTxnOpen) {
        sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL);
        g_bTxnOpen = FALSE;
    }
}

/* Bind one job and execute the INSERT. The field bitmask controls whether the real value or an
   empty string is bound (received + address always; subscribers/match_type/label_color always --
   same as the MySQL Optimized builder). Returns SQLITE_DONE on success. */
static int InsertJob(const SqliteJob *job)
{
    if (!g_insert) return SQLITE_MISUSE;

    char szDateTime[32];
    ConvertDateTime(job->szDate, job->szTime, szDateTime, sizeof(szDateTime));

    char trimmedType[16];
    TrimCopy(trimmedType, sizeof(trimmedType), job->szType);

    int bitrate = atoi(job->szBitrate);

    sqlite3_bind_text(g_insert, 1, szDateTime,    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(g_insert, 2, job->szCapcode, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(g_insert, 3, (g_iFields & SQF_MODE)     ? job->szMode  : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(g_insert, 4, (g_iFields & SQF_MSG_TYPE) ? trimmedType  : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (g_insert, 5, (g_iFields & SQF_BITRATE)  ? bitrate : 0);
    sqlite3_bind_text(g_insert, 6, (g_iFields & SQF_MESSAGE)  ? job->szMessage : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(g_insert, 7, (g_iFields & SQF_LABEL)    ? job->szLabel   : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(g_insert, 8, job->szSubscribers, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (g_insert, 9, job->iMatchType);
    sqlite3_bind_text(g_insert, 10, job->szLabelColor, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(g_insert);
    sqlite3_reset(g_insert);
    sqlite3_clear_bindings(g_insert);
    return rc;
}

/* Eén-int query (bv. PRAGMA page_count). Geeft -1 bij fout. */
static sqlite3_int64 QueryInt(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_int64 v = -1;
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* Onderhoud: leeftijd-purge en/of size-cap. Draait buiten een open transactie. */
static void RunMaintenance(void)
{
    if (!g_db) return;
    BOOL didDelete = FALSE;

    if (g_bPurgeEnabled && g_iPurgeDays > 0) {
        char sql[256], mod[32];
        _snprintf(mod, sizeof(mod) - 1, "-%d days", g_iPurgeDays);
        _snprintf(sql, sizeof(sql) - 1,
                  "DELETE FROM \"%s\" WHERE received < datetime('now','localtime','%s');",
                  g_szTable, mod);
        if (sqlite3_exec(g_db, sql, NULL, NULL, NULL) == SQLITE_OK) {
            int n = sqlite3_changes(g_db);
            if (n > 0) { didDelete = TRUE; WriteLog("PURGE age   rows=%d  older_than=%d days", n, g_iPurgeDays); }
        }
    }

    if (g_iMaxSizeMB > 0) {
        sqlite3_int64 pageCount = QueryInt(g_db, "PRAGMA page_count;");
        sqlite3_int64 pageSize  = QueryInt(g_db, "PRAGMA page_size;");
        if (pageCount > 0 && pageSize > 0) {
            sqlite3_int64 limit = (sqlite3_int64)g_iMaxSizeMB * 1024 * 1024;
            int guard = 0;
            while ((pageCount * pageSize) > limit && guard++ < 1000) {
                char sql[256];
                _snprintf(sql, sizeof(sql) - 1,
                    "DELETE FROM \"%s\" WHERE id IN (SELECT id FROM \"%s\" ORDER BY id LIMIT 1000);",
                    g_szTable, g_szTable);
                if (sqlite3_exec(g_db, sql, NULL, NULL, NULL) != SQLITE_OK) break;
                if (sqlite3_changes(g_db) == 0) break;   /* table empty -- nothing left to delete */
                didDelete = TRUE;
                sqlite3_exec(g_db, "PRAGMA incremental_vacuum;", NULL, NULL, NULL);
                pageCount = QueryInt(g_db, "PRAGMA page_count;");
            }
            if (didDelete) WriteLog("PURGE size  max_mb=%d", g_iMaxSizeMB);
        }
    }

    if (didDelete) {
        sqlite3_exec(g_db, "PRAGMA incremental_vacuum;", NULL, NULL, NULL);
        sqlite3_exec(g_db, "PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, NULL);
    }
}

/* Open (or create) the db file + schema + prepared statement. Returns TRUE on success. */
static BOOL OpenDb(void)
{
    char err[256] = "";
    int rc = sqlite3_open_v2(g_szPath, &g_db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        WriteLog("OPEN FAIL   path=%s  %s", g_szPath, g_db ? sqlite3_errmsg(g_db) : "out of memory");
        if (g_db) { sqlite3_close(g_db); g_db = NULL; }
        return FALSE;
    }
    // auto_vacuum must be set before table creation on a NEW db (on existing db: no-op without VACUUM).
    sqlite3_exec(g_db, "PRAGMA auto_vacuum=INCREMENTAL;", NULL, NULL, NULL);
    ApplyPragmas(g_db, g_bLowWrite);

    if (CreateSchema(g_db, g_szTable, err, sizeof(err)) != SQLITE_OK) {
        WriteLog("SCHEMA FAIL  table=%s  %s", g_szTable, err);
        sqlite3_close(g_db); g_db = NULL;
        return FALSE;
    }
    if (PrepareInsert(g_db, g_szTable, &g_insert) != SQLITE_OK) {
        WriteLog("PREPARE FAIL  table=%s  %s", g_szTable, sqlite3_errmsg(g_db));
        sqlite3_close(g_db); g_db = NULL;
        return FALSE;
    }
    WriteLog("OPEN OK     path=%s  table=%s  lowWrite=%d", g_szPath, g_szTable, g_bLowWrite ? 1 : 0);
    return TRUE;
}

static void CloseDb(void)
{
    TxnCommit();
    if (g_insert) { sqlite3_finalize(g_insert); g_insert = NULL; }
    if (g_db) {
        sqlite3_exec(g_db, "PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, NULL);
        sqlite3_close(g_db);
        g_db = NULL;
    }
    g_bTxnOpen = FALSE;
}

// ===========================================================================
// Worker-thread
// ===========================================================================

static DWORD WINAPI SqliteWorker(LPVOID)
{
    ULONGLONG nextOpenMs  = 0;
    ULONGLONG lastMaintMs = GetTickCount64();
    ULONGLONG lastCommitMs = GetTickCount64();
    BOOL      bErrorPosted = FALSE;

    while (g_bRunning) {
        WaitForSingleObject(g_hEvent, 200);
        if (!g_bRunning) break;

        ULONGLONG now = GetTickCount64();

        if (!g_db) {
            if (now < nextOpenMs) continue;
            if (!OpenDb()) {
                nextOpenMs = now + SQLITE_OPEN_RETRY_MS;
                if (!bErrorPosted) { PostStatus(SQS_ERROR); bErrorPosted = TRUE; }
                continue;
            }
            bErrorPosted = FALSE;
            lastMaintMs  = 0;   /* meteen onderhoud draaien na (her)open */
        }

        /* Drain de queue. */
        BOOL didWork = FALSE;
        for (;;) {
            SqliteJob job;
            EnterCriticalSection(&g_cs);
            BOOL have = (g_qHead != g_qTail);
            if (have) { job = g_queue[g_qHead]; g_qHead = (g_qHead + 1) % SQLITE_QUEUE_SIZE; }
            LeaveCriticalSection(&g_cs);
            if (!have) break;

            TxnBegin();
            int rc = InsertJob(&job);
            if (rc != SQLITE_DONE) {
                // Write error: row is lost (unlike MySQL we don't reconnect a socket;
                // a local I/O error is usually fatal for this db handle). Log + reopen.
                WriteLog("INSERT FAIL  capcode=%s  rc=%d  %s", job.szCapcode, rc, sqlite3_errmsg(g_db));
                TxnCommit();
                CloseDb();
                nextOpenMs = GetTickCount64() + SQLITE_OPEN_RETRY_MS;
                PostStatus(SQS_ERROR);
                bErrorPosted = TRUE;
                break;
            }
            WriteLog("INSERT OK  capcode=%s", job.szCapcode);
            didWork = TRUE;
        }

        if (!g_db) continue;   /* heropen afgedwongen door een insert-fout */

        /* Commit cadence: durable = immediately, NVMe low-write mode = every ~15 s. */
        if (g_bTxnOpen) {
            ULONGLONG commitEvery = g_bLowWrite ? SQLITE_LOWWRITE_COMMIT_MS : 0;
            if (now - lastCommitMs >= commitEvery) {
                TxnCommit();
                lastCommitMs = now;
            }
        }
        if (didWork) PostStatus(SQS_OK);

        /* Maintenance (purge/size) once per hour, outside any open transaction. */
        if (now - lastMaintMs >= SQLITE_MAINT_INTERVAL_MS) {
            TxnCommit();
            RunMaintenance();
            lastMaintMs = now;
        }
    }

    CloseDb();
    return 0;
}

// ===========================================================================
// Publieke API
// ===========================================================================

void SqliteInit(void)
{
    if (!g_bCsInit)    { InitializeCriticalSection(&g_cs);    g_bCsInit    = TRUE; }

    SqliteStop();

    if (!Profile.sqlite_enabled) {
        PostStatus(SQS_DISABLED);
        return;
    }

    /* Path: empty -> <exedir>\pdw.db */
    if (Profile.sqlite_path[0]) {
        strncpy(g_szPath, Profile.sqlite_path, sizeof(g_szPath) - 1);
    } else {
        _snprintf(g_szPath, sizeof(g_szPath) - 1, "%s\\pdw.db", (const char *)szPath);
    }
    g_szPath[sizeof(g_szPath) - 1] = '\0';

    strncpy(g_szTable, Profile.sqlite_table[0] ? Profile.sqlite_table : "messages",
            sizeof(g_szTable) - 1);
    g_szTable[sizeof(g_szTable) - 1] = '\0';
    SanitizeIdent(g_szTable);
    if (!g_szTable[0]) strcpy(g_szTable, "messages");

    g_iFields       = Profile.sqlite_fields;
    g_bLinefeed     = Profile.Linefeed ? TRUE : FALSE;
    g_bLogToFile    = Profile.sqlite_logToFile ? TRUE : FALSE;
    g_bLowWrite     = Profile.sqlite_lowWrite ? TRUE : FALSE;
    g_bPurgeEnabled = Profile.sqlite_purgeEnabled ? TRUE : FALSE;
    g_iPurgeDays    = (Profile.sqlite_purgeDays > 0) ? Profile.sqlite_purgeDays : 30;
    g_iMaxSizeMB    = (Profile.sqlite_maxSizeMB > 0) ? Profile.sqlite_maxSizeMB : 0;

    g_hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_hEvent) return;

    g_qHead = g_qTail = 0;
    ZeroMemory(g_groupAcc, sizeof(g_groupAcc));

    g_bRunning = TRUE;
    g_hThread  = CreateThread(NULL, 0, SqliteWorker, NULL, 0, NULL);
    if (!g_hThread) {
        g_bRunning = FALSE;
        CloseHandle(g_hEvent);
        g_hEvent = NULL;
        PostStatus(SQS_DISABLED);
    } else {
        WriteLog("START       path=%s  table=%s", g_szPath, g_szTable);
    }
}

void SqliteNotify(const char *capcode, const char *message, const char *label,
                  const char *szTime,  const char *szDate,
                  const char *szMode,  const char *szType, const char *szBitrate,
                  int matchType, const char *labelColor)
{
    if (!g_bRunning || !g_bCsInit) return;

    EnterCriticalSection(&g_cs);
    if (!QueueFull()) {
        SqliteJob *j = &g_queue[g_qTail];
        ZeroMemory(j, sizeof(*j));
#define SAFE_COPY(dst, src) \
    strncpy((dst), (src) ? (src) : "", sizeof(dst) - 1); \
    (dst)[sizeof(dst) - 1] = '\0'
        SAFE_COPY(j->szCapcode, capcode);
        Utf8Sanitize(j->szMessage, sizeof(j->szMessage), message ? message : "", g_bLinefeed);
        Utf8Sanitize(j->szLabel,   sizeof(j->szLabel),   label   ? label   : "", FALSE);
        SAFE_COPY(j->szTime,       szTime);
        SAFE_COPY(j->szDate,       szDate);
        SAFE_COPY(j->szMode,       szMode);
        SAFE_COPY(j->szType,       szType);
        SAFE_COPY(j->szBitrate,    szBitrate);
        SAFE_COPY(j->szLabelColor, labelColor);
#undef SAFE_COPY
        j->iMatchType = matchType;
        g_qTail = (g_qTail + 1) % SQLITE_QUEUE_SIZE;
        LeaveCriticalSection(&g_cs);
        SetEvent(g_hEvent);
    } else {
        g_dropped++;
        LeaveCriticalSection(&g_cs);
        WriteLog("DROP  queue full  total dropped=%u", g_dropped);
    }
}

void SqliteGroupAccumulate(const char *capcode, const char *label,
                           const char *message,
                           const char *szTime, const char *szDate,
                           const char *szMode, const char *szType, const char *szBitrate,
                           int matchType, const char *labelColor,
                           int groupbit)
{
    if (!g_bCsInit) return;
    if (groupbit < 0 || groupbit >= MAX_SQLITE_GROUPBITS) return;

    SqliteGroupAcc *ga = &g_groupAcc[groupbit];

    int sPosBack = ga->sPos;
    if (!ga->active) {
        ga->active     = TRUE;
        ga->sPos       = 0;
        ga->nSubscr    = 0;
        ga->iMatchType = matchType;
#define GACOPY(dst, src) strncpy(dst, (src) ? (src) : "", sizeof(dst) - 1); dst[sizeof(dst)-1] = '\0'
        Utf8Sanitize(ga->szMessage, sizeof(ga->szMessage), message ? message : "", g_bLinefeed);
        GACOPY(ga->szTime,       szTime);
        GACOPY(ga->szDate,       szDate);
        GACOPY(ga->szMode,       szMode);
        GACOPY(ga->szType,       szType);
        GACOPY(ga->szBitrate,    szBitrate);
        GACOPY(ga->szLabelColor, labelColor);
#undef GACOPY
        ga->szSubscr[ga->sPos++] = '[';
        sPosBack = ga->sPos;
    } else {
        if (ga->sPos < SQLITE_SUBSCRIBERS_LEN - 2)
            ga->szSubscr[ga->sPos++] = ',';
    }

    char escLabel[FILTER_LABEL_LEN * 2 + 4];
    JsonEscapeStr(escLabel, sizeof(escLabel), label ? label : "");

    char escCc[32];
    JsonEscapeStr(escCc, sizeof(escCc), capcode ? capcode : "");
    int written;
    if (labelColor && labelColor[0]) {
        char escColor[16];
        JsonEscapeStr(escColor, sizeof(escColor), labelColor);
        written = _snprintf(ga->szSubscr + ga->sPos,
                            SQLITE_SUBSCRIBERS_LEN - ga->sPos - 2,
                            "{\"address\":\"%s\",\"label\":\"%s\",\"color\":\"%s\"}",
                            escCc, escLabel, escColor);
    } else {
        written = _snprintf(ga->szSubscr + ga->sPos,
                            SQLITE_SUBSCRIBERS_LEN - ga->sPos - 2,
                            "{\"address\":\"%s\",\"label\":\"%s\"}",
                            escCc, escLabel);
    }
    if (written > 0) { ga->sPos += written; ga->nSubscr++; }
    else             { ga->sPos = sPosBack; }
}

void SqliteFlushGroup(int groupbit)
{
    if (!g_bRunning || !g_bCsInit) return;
    if (groupbit < 0 || groupbit >= MAX_SQLITE_GROUPBITS) return;

    SqliteGroupAcc *ga = &g_groupAcc[groupbit];
    if (!ga->active) return;

    if (ga->sPos < SQLITE_SUBSCRIBERS_LEN - 1) {
        ga->szSubscr[ga->sPos++] = ']';
        ga->szSubscr[ga->sPos]   = '\0';
    } else {
        ga->szSubscr[SQLITE_SUBSCRIBERS_LEN - 1] = '\0';
    }

    SqliteJob job;
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

    ZeroMemory(ga, sizeof(*ga));

    EnterCriticalSection(&g_cs);
    if (!QueueFull()) {
        g_queue[g_qTail] = job;
        g_qTail = (g_qTail + 1) % SQLITE_QUEUE_SIZE;
        LeaveCriticalSection(&g_cs);
        SetEvent(g_hEvent);
    } else {
        g_dropped++;
        LeaveCriticalSection(&g_cs);
        WriteLog("DROP  queue full (group row)  total dropped=%u", g_dropped);
    }
}

void SqliteStop(void)
{
    if (!g_bRunning) return;

    WriteLog("STOP        feed uitgeschakeld");
    g_bRunning = FALSE;
    if (g_hEvent) SetEvent(g_hEvent);

    if (g_hThread) {
        WaitForSingleObject(g_hThread, INFINITE);
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }
    if (g_hEvent) { CloseHandle(g_hEvent); g_hEvent = NULL; }

    g_qHead = g_qTail = 0;
    ZeroMemory(g_groupAcc, sizeof(g_groupAcc));
    PostStatus(SQS_DISABLED);
}

void SqliteDestroy(void)
{
    SqliteStop();
    if (g_bCsInit)   { DeleteCriticalSection(&g_cs);    g_bCsInit   = FALSE; }
}

static void ApplyPragmas(sqlite3 *db, BOOL lowWrite); /* forward — gedefinieerd in de worker-sectie */

// FIX [ConnTest]: synchronous test for the Setup dialog. Opens (or creates) the db file with its
// OWN connection, creates/checks the schema and closes. Never touches the worker globals.
BOOL SqliteTestConnection(const char *path, const char *table, char *szMsg, int msgLen)
{
    if (!szMsg || msgLen < 1) return FALSE;
    szMsg[0] = '\0';

    char szP[MAX_PATH];
    if (path && path[0]) {
        strncpy(szP, path, sizeof(szP) - 1);
    } else {
        _snprintf(szP, sizeof(szP) - 1, "%s\\pdw.db", (const char *)szPath);
    }
    szP[sizeof(szP) - 1] = '\0';

    char szT[64];
    strncpy(szT, (table && table[0]) ? table : "messages", sizeof(szT) - 1);
    szT[sizeof(szT) - 1] = '\0';
    SanitizeIdent(szT);
    if (!szT[0]) strcpy(szT, "messages");

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(szP, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        _snprintf(szMsg, msgLen - 1, "Cannot open database:\n%s\n\n%s",
                  szP, db ? sqlite3_errmsg(db) : "(out of memory)");
        szMsg[msgLen - 1] = '\0';
        if (db) sqlite3_close(db);
        return FALSE;
    }

    // Apply best-practice pragmas so the test-created db already has the right settings.
    ApplyPragmas(db, FALSE);

    char err[256] = "";
    rc = CreateSchema(db, szT, err, sizeof(err));
    if (rc != SQLITE_OK) {
        _snprintf(szMsg, msgLen - 1, "Failed to create schema in table '%s':\n%s", szT, err);
        szMsg[msgLen - 1] = '\0';
        sqlite3_close(db);
        return FALSE;
    }

    char cntSql[128];
    _snprintf(cntSql, sizeof(cntSql) - 1, "SELECT COUNT(*) FROM \"%s\";", szT);
    sqlite3_int64 rows = QueryInt(db, cntSql);
    sqlite3_close(db);

    _snprintf(szMsg, msgLen - 1,
              "OK - database ready.\n\nFile: %s\nTable: %s (%lld rows)\nSQLite %s",
              szP, szT, (long long)(rows < 0 ? 0 : rows), sqlite3_libversion());
    szMsg[msgLen - 1] = '\0';
    return TRUE;
}
