// logmanager.cpp — Central log manager for PDW.
//
// Design:
//   • Direct mode (bufEnabled=false): Write/WriteRaw open+write+close per line.
//     Identical to the old per-subsystem behaviour; no extra latency.
//   • Buffered mode (bufEnabled=true): lines are pushed onto a ring buffer.
//     A background worker thread drains the ring every flushMs milliseconds
//     (or immediately when signaled at >50% full).  One fopen/fclose per
//     unique file per flush batch — dramatically fewer disk writes.
//
// Timestamp format (system logs):  YYYY-MM-DD HH:MM:SS
//   Consistent across all subsystems.  No weekday abbreviation, no ms.
//   Filenames keep the existing {YYMMDD} or {YYMMMDD} prefix controlled by
//   Profile.MonthNumber — no behaviour change visible to long-running users.

#ifndef STRICT
#define STRICT 1
#endif

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "logmanager.h"

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
LogManager& LogManager::Get()
{
    static LogManager s_instance;
    return s_instance;
}

LogManager::LogManager()
    : m_buf(nullptr), m_slots(0), m_head(0), m_tail(0), m_count(0)
    , m_drain(nullptr)
    , m_enableMask(0), m_monthNumber(1)
    , m_bufEnabled(false), m_flushMs(2000)
    , m_hEvent(nullptr), m_hThread(nullptr)
    , m_stop(false), m_initialized(false)
{
    m_path[0] = '\0';
    InitializeCriticalSection(&m_cs);
}

LogManager::~LogManager()
{
    Shutdown();
    DeleteCriticalSection(&m_cs);
}

// ---------------------------------------------------------------------------
// Init / Reconfigure / Shutdown
// ---------------------------------------------------------------------------
void LogManager::Init(const char* path, uint32_t enableMask, int monthNumber,
                      bool bufEnabled, DWORD flushMs, int bufSlots)
{
    if (m_initialized) return;

    strncpy_s(m_path, sizeof(m_path), path ? path : "", _TRUNCATE);
    m_enableMask  = enableMask;
    m_monthNumber = monthNumber;
    m_bufEnabled  = bufEnabled;
    m_flushMs     = flushMs ? flushMs : 2000;

    if (bufEnabled && bufSlots > 0) {
        m_slots  = bufSlots;
        m_buf    = new Entry[m_slots];
        m_drain  = new Entry[m_slots];
        m_head   = m_tail = m_count = 0;

        m_stop   = false;
        m_hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr); // auto-reset
        m_hThread = CreateThread(nullptr, 0, WorkerProc, this, 0, nullptr);
    }

    m_initialized = true;
}

void LogManager::Reconfigure(const char* path, uint32_t enableMask, int monthNumber,
                              bool bufEnabled, DWORD flushMs, int bufSlots)
{
    // Flush current buffer before changing settings.
    Flush();

    // Stop existing worker if any.
    if (m_hThread) {
        m_stop = true;
        if (m_hEvent) SetEvent(m_hEvent);
        // FIX [LogJoinRace]: join to FULL completion before freeing m_buf/m_drain below.
        // The old 5 s timeout could expire while the worker was still inside DrainAll/
        // WriteEntries (fopen/fwrite/fclose can block for minutes on a stalled disk or a
        // disconnected network log path), after which delete[] m_buf/m_drain freed memory
        // the worker was still reading/writing -> heap corruption. The worker has no
        // unbounded wait other than disk I/O, so INFINITE is safe and correct (same
        // hardening as the mqtt/webhook/telnet workers).
        WaitForSingleObject(m_hThread, INFINITE);
        CloseHandle(m_hThread); m_hThread = nullptr;
        if (m_hEvent) { CloseHandle(m_hEvent); m_hEvent = nullptr; }
    }
    // Null m_buf under m_cs BEFORE deleting so concurrent Emit() calls see nullptr
    // under the lock and fall back to direct-write rather than accessing freed memory.
    Entry* oldBuf   = nullptr;
    Entry* oldDrain = nullptr;
    EnterCriticalSection(&m_cs);
    oldBuf   = m_buf;   m_buf   = nullptr;
    oldDrain = m_drain; m_drain = nullptr;
    m_slots  = m_head = m_tail = m_count = 0;
    m_bufEnabled = false;
    LeaveCriticalSection(&m_cs);
    delete[] oldBuf;
    delete[] oldDrain;

    // Apply new settings.
    strncpy_s(m_path, sizeof(m_path), path ? path : "", _TRUNCATE);
    m_enableMask  = enableMask;
    m_monthNumber = monthNumber;
    m_bufEnabled  = bufEnabled;
    m_flushMs     = flushMs ? flushMs : 2000;
    m_stop        = false;

    if (bufEnabled && bufSlots > 0) {
        m_slots  = bufSlots;
        m_buf    = new Entry[m_slots];
        m_drain  = new Entry[m_slots];
        m_head   = m_tail = m_count = 0;

        m_hEvent  = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        m_hThread = CreateThread(nullptr, 0, WorkerProc, this, 0, nullptr);
    }
}

void LogManager::Shutdown()
{
    if (!m_initialized) return;

    if (m_hThread) {
        m_stop = true;
        if (m_hEvent) SetEvent(m_hEvent);
        // FIX [LogJoinRace]: join to FULL completion before freeing the ring buffers below.
        // A 5 s timeout could return with the worker still live inside DrainAll/WriteEntries
        // on a stalled disk, after which delete[] m_buf/m_drain ran under a running thread
        // (use-after-free / heap corruption on exit). INFINITE matches the other feed workers.
        WaitForSingleObject(m_hThread, INFINITE);
        CloseHandle(m_hThread); m_hThread = nullptr;
    }
    if (m_hEvent) { CloseHandle(m_hEvent); m_hEvent = nullptr; }

    // Null m_buf under m_cs before deleting — same pattern as Reconfigure.
    // Any decoder thread that sneaks past the m_initialized check will find
    // m_buf==nullptr under the lock and fall back to a direct write, not freed memory.
    Entry* oldBuf   = nullptr;
    Entry* oldDrain = nullptr;
    int    finalCount = 0;
    EnterCriticalSection(&m_cs);
    // Snapshot remaining ring entries into m_drain for the final flush below.
    if (m_drain && m_buf && m_count > 0) {
        finalCount = m_count;
        int head = m_head;
        for (int i = 0; i < finalCount; i++)
            m_drain[i] = m_buf[(head + i) % m_slots];
    }
    oldBuf   = m_buf;   m_buf   = nullptr;
    oldDrain = m_drain; m_drain = nullptr;
    m_slots  = m_head = m_tail = m_count = 0;
    m_bufEnabled  = false;
    m_initialized = false;  // last — decoder threads now see both null buf and false initialized
    LeaveCriticalSection(&m_cs);

    // Flush snapshot to disk outside the lock.
    if (oldDrain && finalCount > 0)
        WriteEntries(oldDrain, finalCount);

    delete[] oldBuf;
    delete[] oldDrain;
}

// ---------------------------------------------------------------------------
// Public write API
// ---------------------------------------------------------------------------
void LogManager::Write(LogCat cat, const char* fmt, ...)
{
    if (!m_initialized)            return;
    if (!(m_enableMask & cat))     return;
    if (!fmt)                      return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[LM_LINE_MAX];
    int  pos = _snprintf_s(line, sizeof(line), _TRUNCATE,
                           "%04d-%02d-%02d %02d:%02d:%02d.%03d ",
                           st.wYear, st.wMonth, st.wDay,
                           st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (pos < 0) pos = 0;

    va_list ap;
    va_start(ap, fmt);
    int written = _vsnprintf_s(line + pos, sizeof(line) - pos, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (written < 0) written = 0;
    pos += written;

    // Ensure newline.
    if (pos < (int)sizeof(line) - 1 && (pos == 0 || line[pos-1] != '\n'))
        line[pos++] = '\n';
    line[pos] = '\0';

    char path[MAX_PATH];
    BuildPath(path, sizeof(path), cat, st);

    Emit(path, line, pos);
}

void LogManager::WriteRaw(LogCat cat, const char* line, int len)
{
    if (!m_initialized)        return;
    if (!(m_enableMask & cat)) return;
    if (!line)                 return;

    if (len < 0) len = (int)strlen(line);
    if (len == 0) return;
    if (len >= LM_LINE_MAX) len = LM_LINE_MAX - 1;

    SYSTEMTIME st;
    GetLocalTime(&st);

    char path[MAX_PATH];
    BuildPath(path, sizeof(path), cat, st);

    Emit(path, line, len);
}

void LogManager::WriteLineTo(const char* path, const char* line, int len)
{
    if (!m_initialized || !path || !line) return;
    if (len < 0) len = (int)strlen(line);
    if (len == 0) return;
    if (len >= LM_LINE_MAX) len = LM_LINE_MAX - 1;
    Emit(path, line, len);
}

void LogManager::SetEnabled(LogCat cat, bool on)
{
    EnterCriticalSection(&m_cs);
    if (on) m_enableMask |=  (uint32_t)cat;
    else    m_enableMask &= ~(uint32_t)cat;
    LeaveCriticalSection(&m_cs);
}

bool LogManager::IsEnabled(LogCat cat) const
{
    return (m_enableMask & (uint32_t)cat) != 0;
}

void LogManager::Flush()
{
    if (!m_bufEnabled || !m_buf) return;
    // Signal the worker, then wait briefly for it to drain.
    if (m_hEvent) SetEvent(m_hEvent);
    Sleep(50);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Build the full file path for a category.
void LogManager::BuildPath(char* out, int outSize, LogCat cat, const SYSTEMTIME& st) const
{
    const char* root = m_path[0] ? m_path : ".";

    // Suffix determines filename (kept identical to existing per-subsystem names).
    const char* suffix = nullptr;
    bool        noDate = false;   // unused — all logs now use date prefix

    switch (cat) {
    case LC_DEBUG:   suffix = "_pdw_debug.log";         break;
    case LC_TELNET:  suffix = "_telnet_server.log";     break;
    case LC_WIRE:    suffix = "_telnet_traffic.log";    break;
    case LC_MQTT:    suffix = "_mqtt.log";              break;
    case LC_WEBHOOK: suffix = "_webhook.log";           break;
    case LC_MYSQL:   suffix = "_mysql.log";             break;
    case LC_SQLITE:  suffix = "_sqlite.log";            break;
    case LC_SMTP:    suffix = "_mail.log";              break;
    default:         suffix = "_pdw.log";               break;
    }

    static const char* kMon[12] = {
        "JAN","FEB","MAR","APR","MAY","JUN",
        "JUL","AUG","SEP","OCT","NOV","DEC"
    };

    if (m_monthNumber) {
        // Numeric month: 260603_pdw_debug.log
        _snprintf_s(out, outSize, _TRUNCATE, "%s\\%02d%02d%02d%s",
                    root, st.wYear % 100, st.wMonth, st.wDay, suffix);
    } else {
        // Abbreviated month: 26JUN03_pdw_debug.log
        const char* mon = (st.wMonth >= 1 && st.wMonth <= 12)
                          ? kMon[st.wMonth - 1] : "???";
        _snprintf_s(out, outSize, _TRUNCATE, "%s\\%02d%s%02d%s",
                    root, st.wYear % 100, mon, st.wDay, suffix);
    }
}

// Push one entry into the ring buffer (buffered mode) or write directly.
void LogManager::Emit(const char* path, const char* line, int len)
{
    if (!m_bufEnabled || !m_buf) {
        // Direct write — same behaviour as old fopen/fprintf/fclose per line.
        FILE* f = fopen(path, "a");
        if (f) { fwrite(line, 1, len, f); fclose(f); }
        return;
    }

    EnterCriticalSection(&m_cs);

    // Re-check m_buf under the lock: Shutdown/Reconfigure may have nulled it
    // between the unlocked check above and this point.
    if (!m_buf || m_slots == 0) {
        LeaveCriticalSection(&m_cs);
        FILE* f = fopen(path, "a");
        if (f) { fwrite(line, 1, len, f); fclose(f); }
        return;
    }

    if (m_count >= m_slots) {
        // Ring buffer full — drop the oldest entry (oldest = head).
        m_head  = (m_head + 1) % m_slots;
        m_count--;
    }

    Entry& e = m_buf[m_tail];
    strncpy_s(e.path, sizeof(e.path), path, _TRUNCATE);
    int copy = (len < LM_LINE_MAX - 1) ? len : LM_LINE_MAX - 1;
    memcpy(e.line, line, copy);
    e.line[copy] = '\0';
    e.lineLen    = copy;
    m_tail  = (m_tail + 1) % m_slots;
    m_count++;

    bool halfFull = (m_count >= m_slots / 2);

    LeaveCriticalSection(&m_cs);

    // Wake worker immediately when buffer is getting full.
    if (halfFull && m_hEvent) SetEvent(m_hEvent);
}

// Drain all buffered entries.  Called only from the worker thread.
void LogManager::DrainAll()
{
    EnterCriticalSection(&m_cs);
    int count = m_count;
    int head  = m_head;
    // Snapshot entries into drain buffer while holding the lock.
    for (int i = 0; i < count; i++)
        m_drain[i] = m_buf[(head + i) % m_slots];
    m_head = m_tail = m_count = 0;
    LeaveCriticalSection(&m_cs);

    if (count > 0) WriteEntries(m_drain, count);
}

// Write a batch of entries to disk.
// Groups consecutive entries with the same path to minimise open/close calls.
void LogManager::WriteEntries(Entry* entries, int count)
{
    int i = 0;
    while (i < count) {
        const char* p = entries[i].path;

        FILE* f = fopen(p, "a");
        if (f) {
            // Write all consecutive entries going to the same file.
            int j = i;
            while (j < count && strcmp(entries[j].path, p) == 0) {
                fwrite(entries[j].line, 1, entries[j].lineLen, f);
                j++;
            }
            fclose(f);
            i = j;
        } else {
            i++; // skip unwritable entry
        }
    }
}


// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------
DWORD WINAPI LogManager::WorkerProc(LPVOID param)
{
    reinterpret_cast<LogManager*>(param)->WorkerLoop();
    return 0;
}

void LogManager::WorkerLoop()
{
    while (!m_stop) {
        // Wait up to flushMs for a signal, then drain regardless.
        WaitForSingleObject(m_hEvent, m_flushMs);
        DrainAll();
    }
    // Final drain after stop flag is set.
    DrainAll();
}
